# Gateway 4G LTE com LilyGO T-PCIE (SIM7600SA) via PPPoS + Wi-Fi NAT

## 🚀 Visão Geral

Este firmware foi desenvolvido para operação de nível industrial na placa **LilyGO T-PCIE** com o modem **SIM7600SA**. O código utiliza o componente `esp_modem` do **ESP-IDF v6.0**, integrando a conexão celular diretamente à pilha TCP/IP (LwIP) do ESP32 através do protocolo PPPoS, e roteando a internet via **Wi-Fi SoftAP (NAT)**.

Diferente de implementações básicas, este projeto utiliza o protocolo **CMUX (Multiplexador)** para manter o canal de dados (PPP) e o canal de comandos (AT) ativos simultaneamente, permitindo a leitura em tempo real da qualidade do sinal celular sem interromper a conexão de internet.

---

## 🏗️ 1. Arquitetura e Soluções Críticas

Durante o desenvolvimento e testes de carga (múltiplos dispositivos conectados ao Wi-Fi trafegando dados), alguns gargalos do ESP-IDF foram identificados e resolvidos:

### A. Protocolo CMUX e Buffers da UART
Para permitir o envio de comandos `AT+CSQ` (Sinal) enquanto a internet (PPPoS) está ativa na mesma UART física, ativamos o `ESP_MODEM_MODE_CMUX`. 
No entanto, o tráfego intenso gerava o erro `uart_terminal: Ring Buffer Full` e derrubava a máquina de estados do CMUX.
**Solução:** Os buffers do DTE (Data Terminal Equipment) foram drasticamente aumentados no `tpcie_modem.c`:
- `rx_buffer_size`: 16384 bytes
- `tx_buffer_size`: 2048 bytes
- `dte_buffer_size`: 2048 bytes
- `event_queue_size`: 60
- `task_priority`: 18

### B. Saturação do Mailbox do LwIP (Erro: `pppos_input_tcpip failed with -1`)
Sob carga de tráfego NAT (roteamento Wi-Fi), a fila de mensagens padrão do LwIP (`tcpip_mbox`) saturava rapidamente. O ESP-IDF v6.0 limita o tamanho dessa fila a 64 posições quando o `LWIP_WND_SCALE` não está ativo, o que não é suficiente para o volume de pacotes gerados.
**Solução:** Ativamos o **LwIP Core Locking** no `sdkconfig.defaults`. Isso faz com que a entrada de pacotes ignore a fila (mailbox) e injete os dados diretamente na pilha TCP/IP usando um *Mutex*, eliminando o gargalo de memória e as quedas de pacote:
```properties
CONFIG_LWIP_TCPIP_CORE_LOCKING=y
CONFIG_LWIP_TCPIP_CORE_LOCKING_INPUT=y
CONFIG_LWIP_TCPIP_TASK_PRIO=23
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1=y
```

---

## 🛠️ 2. Componente `tpcie_modem` (Gerenciador Assíncrono)

Para não bloquear o processador, a inicialização e reconexão do modem rodam em uma Task dedicada e orientada a eventos (`modem_manager_task`). O fluxo inclui:

1. **Boot Físico:** Acionamento dos pinos `25` (Energia da PCIe) e `4` (PWRKEY).
2. **Sincronização (Autobaud):** Loop enviando `AT` até o modem responder `OK` a 115200 bps.
3. **Registro de Rede:** Aguarda o comando `AT+CSQ` retornar um RSSI válido (diferente de 99).
4. **Ativação do CMUX:** Habilita os canais virtuais para separar Comandos e Dados.
5. **Conexão PPPoS:** O LwIP assume a interface, obtém o IP da operadora e emite o evento `IP_EVENT_PPP_GOT_IP`.

```c
// Exemplo de uso no app_main.c:
tpcie_modem_config_t modem_cfg = {
    .apn = "zap.vivo.com.br",
    .task_stack = 8192,
    .task_core_id = 1
};
tpcie_modem_init(&modem_cfg);
tpcie_modem_connect();
```

---

## 📡 3. Wi-Fi NAT e Telemetria

O sistema levanta um Ponto de Acesso (SoftAP) Wi-Fi e configura o NAPT (Network Address and Port Translation) interno do ESP32 para rotear o tráfego dos clientes conectados (ex: celulares, notebooks) diretamente para a interface PPP (4G).

Enquanto isso, a função principal (`app_main`) entra em um loop infinito de telemetria, consultando a saúde do sinal a cada 5 segundos usando a função segura `tpcie_modem_get_rssi()`, sem causar interferência nos dados em trânsito.

---

## 📊 4. Diagnóstico de Falhas Comuns

- **IP Recebido:** Verifique se o log exibe um IP real da operadora (ex: `100.x.x.x`) logo após "Conexão 4G Estabelecida".
- **`pppos_input_tcpip failed with -1`:** Se reaparecer, certifique-se de que o `sdkconfig.defaults` foi aplicado corretamente (`idf.py fullclean` e reconstrução podem ser necessários).
- **RSSI 99 / Sinal Indisponível:** Indica falta de sinal, antena desconectada ou problema no chip SIM.
- **CMUX Indisponível:** Se o log exibir *CMUX indisponivel nesta versao. Entrando em modo DATA*, significa que a inicialização falhou e a telemetria do sinal retornará erro (`-2`), pois a UART estará bloqueada apenas para os dados PPP.
- **Falha de Autobaud:** Geralmente associado a pinos TX/RX invertidos ou falta de alimentação no pino 25.
