#ifndef TPCIE_MODEM_H
#define TPCIE_MODEM_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// Permite que o FreeRTOS escolha a CPU (No Affinity)
#define TPCIE_CORE_ANY  tskNO_AFFINITY

// Estrutura de configuração (Parâmetros Opcionais)
typedef struct {
    const char* apn;       // APN da operadora
    int task_core_id;      // CPU 0, CPU 1 ou TPCIE_CORE_ANY
    uint32_t task_stack;   // Tamanho da pilha (recomendado: 4096)
} tpcie_modem_config_t;

// Macro com valores padrão da engenharia
#define TPCIE_MODEM_DEFAULT_CONFIG() { \
    .apn = "zap.vivo.com.br", \
    .task_core_id = TPCIE_CORE_ANY, \
    .task_stack = 4096 \
}

// ---------------------------------------------------------
// API DO COMPONENTE
// ---------------------------------------------------------

/**
 * @brief Inicializa as estruturas e sobe a Task Gerenciadora oculta.
 * @param config Ponteiro para a configuração (use TPCIE_MODEM_DEFAULT_CONFIG).
 */
esp_err_t tpcie_modem_init(const tpcie_modem_config_t *config);

/**
 * @brief Inicia a rotina de boot físico do modem (Pinos 25 e 4).
 * Retorna imediatamente. O modem levará ~10s em background para ligar.
 */
esp_err_t tpcie_modem_power_on(void);

/**
 * @brief Solicita à Task que ligue o hardware e conecte à operadora.
 * Retorna imediatamente (não bloqueia). Use tpcie_wait_for_network() para esperar.
 */
esp_err_t tpcie_modem_connect(void);

/**
 * @brief Desmonta a interface PPP e corta a energia do slot PCIe.
 */
esp_err_t tpcie_modem_power_off(void);

/**
 * @brief Função bloqueante para aguardar o IP da operadora.
 * @param timeout_ticks Tempo máximo de espera (ex: pdMS_TO_TICKS(60000)).
 * @return true se conectou, false se deu timeout.
 */
bool tpcie_wait_for_network(uint32_t timeout_ticks);

/**
 * @brief Obtém a qualidade do sinal (CSQ) do modem.
 * @return Valor de 0 a 31 (CSQ) ou valor negativo em caso de erro/indisponível.
 */
int tpcie_modem_get_rssi(void);

/**
 * @brief Retorna instantaneamente se a placa tem um IP válido agora.
 */
bool tpcie_is_connected(void);

#endif // TPCIE_MODEM_H
