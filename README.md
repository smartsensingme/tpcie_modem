# Gateway 4G LTE com LilyGO T-PCIE (SIM7600SA) via PPPoS

## 🚀 Visão Geral

Este firmware foi desenvolvido para operação de nível industrial na placa **LilyGO T-PCIE** com o modem **SIM7600SA**. O código utiliza o componente `esp_modem` do **ESP-IDF v6.0**, integrando a conexão celular diretamente à pilha TCP/IP (LwIP) do ESP32 através do protocolo PPPoS.

---

## ⚙️ 1. Configuração do Hardware (menuconfig)

Acesse o editor de configurações (`idf.py menuconfig`) e aplique os seguintes parâmetros na seção **Example Configuration**:

* **UART TXD Pin Number:** `27`
* **UART RXD Pin Number:** `26`
  * *Nota: A LilyGO utiliza mapeamento cruzado. O ESP32 transmite pelo 27 e recebe pelo 26.*
* **Modem Model:** `SIM7600`
* **Set MODEM APN:** Insira o APN da operadora (ex: `zap.vivo.com.br`).
* **Flow Control:** `No control flow`.
* **Send SMS:** `Desmarcado` (focar na estabilidade dos dados).

---

## 🛠️ 2. Lógica de Inicialização (app_main.c)

Para garantir que o modem responda e conecte, o código implementa as seguintes etapas críticas:

### A. Definições de Energia

```c
#define SIM7600_POWER_PIN 25 // Chave Geral da Fonte
#define SIM7600_PWRKEY     4 // Botão de Boot do Modem
```

### B. Sequência de Boot do Hardware

1. Liga o **Pino 25** (nível ALTO) para alimentar o slot PCIe.

2. Aguarda 1 segundo para estabilização da tensão.

3. Envia um pulso de 500ms no **Pino 4 (PWRKEY)** para iniciar o SO do modem.

4. Aguarda 10 segundos para o carregamento completo do firmware do SIM7600.

```c
// =======================================================
    // 1. ROTINA DE BOOT DO HARDWARE LILYGO T-PCIE
    // =======================================================
    ESP_LOGI("MODEM", "Ligando a chave geral de energia da placa (Pin 25)...");
    
    // Liga a energia física do slot PCIe
    gpio_reset_pin(SIM7600_POWER_PIN);
    gpio_set_direction(SIM7600_POWER_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(SIM7600_POWER_PIN, 1); 
    
    vTaskDelay(pdMS_TO_TICKS(1000)); // Dá 1 segundo para a tensão estabilizar

    ESP_LOGI("MODEM", "Enviando pulso no PWRKEY (Pin 4)...");
    
    // O pulso exato da LilyGO: HIGH por 500ms, depois LOW
    gpio_reset_pin(SIM7600_PWRKEY);
    gpio_set_direction(SIM7600_PWRKEY, GPIO_MODE_OUTPUT);
    gpio_set_level(SIM7600_PWRKEY, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(SIM7600_PWRKEY, 0);
    
    ESP_LOGI("MODEM", "Aguardando o SO do modem carregar (10s)...");
    vTaskDelay(pdMS_TO_TICKS(10000));
```

### C. Sincronização de Baud Rate (Autobaud)

O modem inicia em modo de detecção automática. O código executa um loop de comandos `AT` (até 15 tentativas) até receber o primeiro "OK", sincronizando a velocidade de comunicação em 115200 bps.

```c
// =======================================================
    // 2. SINCRONIZAÇÃO DE AUTOBAUD
    // =======================================================
    ESP_LOGI(TAG, "Sincronizando baud rate (Autobaud) com o modem...");
    bool sync_ok = false;
    for (int i = 0; i < 15; i++) {
        if (esp_modem_sync(dce) == ESP_OK) {
            ESP_LOGI(TAG, "Modem respondeu OK! Baud rate sincronizado.");
            sync_ok = true;
            break;
        }
        ESP_LOGW(TAG, "Aguardando modem responder AT... (tentativa %d de 15)", i+1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    if (!sync_ok) {
        ESP_LOGE(TAG, "Falha critica: O modem está mudo. Verifique os pinos TX e RX no menuconfig!");
        return;
    }
    // =======================================================
```

### D. Registro na Rede Celular

Antes de iniciar a conexão de dados, o firmware monitora a qualidade do sinal (`AT+CSQ`). Ele aguarda em loop até que o valor de RSSI seja diferente de `99` (sinal desconhecido), garantindo que o modem encontrou a torre da operadora.

```c
// =======================================================
    // 3. ESPERA O REGISTRO NA REDE CELULAR
    // =======================================================
    ESP_LOGI(TAG, "Aguardando o modem registrar na torre da operadora...");
    int rssi = 99, ber = 99;
    bool sinal_ok = false;
    
    for (int i = 0; i < 30; i++) {
        err = esp_modem_get_signal_quality(dce, &rssi, &ber);
        if (err == ESP_OK && rssi != 99 && rssi > 0) {
            ESP_LOGI(TAG, "Sucesso! Sinal conectado: rssi=%d, ber=%d", rssi, ber);
            sinal_ok = true;
            break;
        }
        ESP_LOGW(TAG, "Procurando torre... (rssi=%d) - Tentativa %d de 30", rssi, i+1);
        vTaskDelay(pdMS_TO_TICKS(2000)); // Espera 2 segundos entre as tentativas
    }

    if (!sinal_ok) {
        ESP_LOGE(TAG, "Falha critica: O modem não encontrou a rede celular após 1 minuto.");
        ESP_LOGE(TAG, "Verifique a antena, o chip SIM e se há sinal na sua região.");
        return;
    }
    // =======================================================
```

### E. Conexão Persistente

Diferente dos exemplos padrão, este código remove a rotina de encerramento de rede. Após a obtenção do IP, o programa entra em um loop `while(1)`, mantendo a interface PPP ativa para o tráfego contínuo de dados (MQTT/HTTP).

```c
if (ping_ret_val != 0) {
        ESP_LOGE(TAG, "Ping command failed with return value: %d", ping_ret_val);
    }
    CHECK_USB_DISCONNECTION(event_group);

    // =======================================================
    // 4. LOOP PRINCIPAL DA APLICAÇÃO (TELEMETRIA)
    // =======================================================
    ESP_LOGI(TAG, "Internet 4G estabelecida. Entrando no loop principal da aplicação...");
    
    while (1) {
        // No futuro, você vai colocar o seu código MQTT ou HTTP aqui:
        // ler_inversor_solar();
        // publicar_mqtt();
        
        // Mantém a task viva sem travar o processador (delay de 10 segundos)
        vTaskDelay(pdMS_TO_TICKS(10000)); 
    } 
```

---

## 📊 3. Diagnóstico e Sinais

- **IP Recebido:** Verifique se o log exibe um IP real da operadora (ex: `100.x.x.x`).

- **Ping Test:** O sucesso é confirmado com a mensagem `Ping command finished with return value: 0`.

- **RSSI 99:** Indica falta de sinal, antena desconectada ou problema no chip SIM.

- **Timeout (Erro -1):** Geralmente associado a pinos TX/RX invertidos ou falta de alimentação no pino 25.
