#include "tpcie_modem.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_modem_api.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include <string.h>

static const char *TAG = "TPCIE_HAL";

// ========================================================
// HARDWARE DEFINES (Constantes da LilyGO T-PCIE)
// ========================================================
#define SIM7600_POWER_PIN  25
#define SIM7600_PWRKEY     4
#define TPCIE_UART_TX      27
#define TPCIE_UART_RX      26

// ========================================================
// EVENTOS INTERNOS (Máquina de Estados)
// ========================================================
static EventGroupHandle_t s_modem_events;
#define EVENT_CMD_CONNECT   BIT0
#define EVENT_CMD_POWEROFF  BIT1
#define EVENT_NET_CONNECTED BIT2
#define EVENT_CMD_POWERON   BIT3

// Variáveis de Estado Globais do Componente
static tpcie_modem_config_t s_config;
static esp_modem_dce_t *s_dce = NULL;
static esp_netif_t *s_esp_netif = NULL;
static bool s_is_powered_on = false;

// ========================================================
// HANDLERS DO LwIP (Pilha TCP/IP)
// ========================================================
static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Conexão 4G Estabelecida! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        // Força o LwIP a usar o modem como rota principal de internet
        esp_netif_set_default_netif(s_esp_netif);
        xEventGroupSetBits(s_modem_events, EVENT_NET_CONNECTED);
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "Sinal PPP perdido ou desconectado.");
        xEventGroupClearBits(s_modem_events, EVENT_NET_CONNECTED);
    }
}

// ========================================================
// ROTINAS DE HARDWARE PRIVADAS
// ========================================================
static void hw_power_on_sequence(void) {
    if (s_is_powered_on) return; // Evita ligar duas vezes
    
    ESP_LOGI(TAG, "Executando Boot Fisico (Pin 25 e 4)...");
    gpio_reset_pin(SIM7600_POWER_PIN);
    gpio_set_direction(SIM7600_POWER_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(SIM7600_POWER_PIN, 1); 
    vTaskDelay(pdMS_TO_TICKS(1000));

    gpio_reset_pin(SIM7600_PWRKEY);
    gpio_set_direction(SIM7600_PWRKEY, GPIO_MODE_OUTPUT);
    gpio_set_level(SIM7600_PWRKEY, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(SIM7600_PWRKEY, 0);
    
    ESP_LOGI(TAG, "Aguardando SO do modem (10s)...");
    vTaskDelay(pdMS_TO_TICKS(10000));
    s_is_powered_on = true;
}

static void hw_power_off_sequence(void) {
    ESP_LOGI(TAG, "Cortando energia da interface PCIe...");
    gpio_set_level(SIM7600_POWER_PIN, 0); // Corta energia total
    s_is_powered_on = false;
    xEventGroupClearBits(s_modem_events, EVENT_NET_CONNECTED);
}

// ========================================================
// A TASK GERENCIADORA (O "Cérebro" rodando em background)
// ========================================================
static void modem_manager_task(void *arg) {
    ESP_LOGI(TAG, "Gerenciador iniciado. Aguardando comandos...");

    while (1) {
        // Dorme sem gastar CPU até receber um comando
        EventBits_t bits = xEventGroupWaitBits(s_modem_events, EVENT_CMD_CONNECT | EVENT_CMD_POWEROFF | EVENT_CMD_POWERON, 
            pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & EVENT_CMD_POWERON) {
            ESP_LOGI(TAG, "[CMD] Solicitacao explicita de Power ON...");
            hw_power_on_sequence();
        } else if (bits & EVENT_CMD_CONNECT) {
            ESP_LOGI(TAG, "[CMD] Iniciando processo de conexão...");
            hw_power_on_sequence();

            // Configurações do esp_modem com pinos cravados
            esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
            dte_config.uart_config.tx_io_num = TPCIE_UART_TX;
            dte_config.uart_config.rx_io_num = TPCIE_UART_RX;
            dte_config.uart_config.rts_io_num = -1;
            dte_config.uart_config.cts_io_num = -1;
            dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
            dte_config.dte_buffer_size = 2048;
            dte_config.task_priority = 18;
            dte_config.uart_config.rx_buffer_size = 16384;
            dte_config.uart_config.tx_buffer_size = 2048;
            dte_config.uart_config.event_queue_size = 60;

            esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(s_config.apn);
            esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
            
            if (s_esp_netif == NULL) s_esp_netif = esp_netif_new(&netif_ppp_config);
            if (s_dce == NULL) s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config, s_esp_netif);
            if (s_dce == NULL) {
                ESP_LOGE(TAG, "Falha ao criar DCE.");
                continue;
            }

            // Sincronização Autobaud
            ESP_LOGI(TAG, "Sincronizando Autobaud...");
            bool sync_ok = false;
            for (int i = 0; i < 15; i++) {
                if (esp_modem_sync(s_dce) == ESP_OK) { sync_ok = true; break; }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            if (!sync_ok) { ESP_LOGE(TAG, "Erro fatal de Autobaud."); continue; }

            // Loop de Registro Celular
            ESP_LOGI(TAG, "Aguardando sinal celular...");
            int rssi = 99, ber = 99;
            for (int i = 0; i < 30; i++) {
                if (esp_modem_get_signal_quality(s_dce, &rssi, &ber) == ESP_OK && rssi != 99 && rssi > 0) break;
                vTaskDelay(pdMS_TO_TICKS(2000));
            }

            // Ativa CMUX para manter PPP (dados) e AT (comandos) simultaneamente no mesmo UART
            ESP_LOGI(TAG, "Ativando CMUX + abrindo tunel PPP...");
            esp_err_t mode_err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX);
            if (mode_err != ESP_OK) {
                ESP_LOGE(TAG, "Falha ao ativar CMUX (%s). Tentando modo DATA.", esp_err_to_name(mode_err));
                mode_err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
                if (mode_err != ESP_OK) {
                    ESP_LOGE(TAG, "Falha ao entrar em modo DATA (%s).", esp_err_to_name(mode_err));
                    continue;
                }
            }
            // O IP virá pelo evento do LwIP (on_ip_event)
        } 
        
        else if (bits & EVENT_CMD_POWEROFF) {
            ESP_LOGI(TAG, "[CMD] Desligando sistema...");
            if (s_dce) {
                esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
                esp_modem_destroy(s_dce);
                s_dce = NULL;
            }
            hw_power_off_sequence();
        }
    }
}

// ========================================================
// FUNÇÕES PÚBLICAS DA API
// ========================================================
esp_err_t tpcie_modem_init(const tpcie_modem_config_t *config) {
    if (s_modem_events != NULL) return ESP_OK; // Já iniciado
    
    s_config = *config; // Salva config localmente
    s_modem_events = xEventGroupCreate();

    // Registra os listeners da pilha TCP/IP
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL);
    if (err != ESP_OK) return err;

    // Cria a task oculta, respeitando a escolha de CPU do usuário
    xTaskCreatePinnedToCore(modem_manager_task, "modem_task", s_config.task_stack, NULL, 5, NULL, s_config.task_core_id);
    
    return ESP_OK;
}

esp_err_t tpcie_modem_power_on(void) {
    if (s_modem_events == NULL) return ESP_ERR_INVALID_STATE;
    xEventGroupSetBits(s_modem_events, EVENT_CMD_POWERON); // Avisa a task para ligar!
    return ESP_OK;
}

esp_err_t tpcie_modem_connect(void) {
    if (s_modem_events == NULL) return ESP_ERR_INVALID_STATE;
    xEventGroupSetBits(s_modem_events, EVENT_CMD_CONNECT); // Avisa a task!
    return ESP_OK;
}

esp_err_t tpcie_modem_power_off(void) {
    if (s_modem_events == NULL) return ESP_ERR_INVALID_STATE;
    xEventGroupSetBits(s_modem_events, EVENT_CMD_POWEROFF); // Avisa a task!
    return ESP_OK;
}

bool tpcie_wait_for_network(uint32_t timeout_ticks) {
    if (s_modem_events == NULL) return false;
    EventBits_t bits = xEventGroupWaitBits(s_modem_events, EVENT_NET_CONNECTED, pdFALSE, pdFALSE, timeout_ticks);
    return (bits & EVENT_NET_CONNECTED) != 0;
}

int tpcie_modem_get_rssi(void) {
    if (s_dce == NULL) return -1;

    int rssi = 0;
    int ber = 0;

    // Tente forçar um comando AT direto via DCE com um timeout longo (3 segundos)
    // Isso ignora a abstração de alto nível e vai direto ao ponto.
    esp_err_t err = esp_modem_get_signal_quality(s_dce, &rssi, &ber);

    if (err != ESP_OK) {
        // Se der erro, vamos dar um "reset" lógico na interface de comando
        ESP_LOGD("MODEM", "Aguardando canal AT liberar...");
        return -2; 
    }

    if (rssi == 99) return -3;

    return rssi;
}

bool tpcie_is_connected(void) {
    if (s_modem_events == NULL) return false;
    return (xEventGroupGetBits(s_modem_events) & EVENT_NET_CONNECTED) != 0;
}
