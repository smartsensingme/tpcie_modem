#include "tpcie_modem.h"
#include "wifi_softap_nat.h"
#include "esp_log.h"
#include "esp_netif.h"     
#include "esp_event.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"

static vprintf_like_t s_prev_vprintf = NULL;

static void print_heap_snapshot(const char *reason)
{
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    uint32_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    uint32_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    uint32_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    esp_rom_printf("\nHEAP_SNAPSHOT reason=%s free=%u min_free=%u free_8bit=%u largest_8bit=%u free_dma=%u largest_dma=%u\n",
                   reason ? reason : "unknown",
                   free_heap, min_free_heap, free_8bit, largest_8bit, free_dma, largest_dma);
}

static int log_vprintf_with_heap(const char *fmt, va_list args)
{
    int ret = 0;
    if (s_prev_vprintf) ret = s_prev_vprintf(fmt, args);

    if (fmt && strstr(fmt, "pppos_input_tcpip failed") != NULL) {
        print_heap_snapshot("pppos_input_tcpip_failed");
    }
    return ret;
}

void app_main(void)
{
    // =======================================================
    // INICIALIZAÇÃO DO HARDWARE BASE E MEMÓRIA
    // =======================================================
    // Inicia a partição NVS (Necessário para o Wi-Fi salvar calibrações)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // =======================================================
    // INICIALIZAÇÃO GLOBAL DO SISTEMA (Motor de Rede)
    // =======================================================
    s_prev_vprintf = esp_log_set_vprintf(&log_vprintf_with_heap);
    ESP_LOGI("CFG", "LWIP mbox cfg: tcpip=%d tcp=%d udp=%d tcpip_prio=%d",
             CONFIG_LWIP_TCPIP_RECVMBOX_SIZE, CONFIG_LWIP_TCP_RECVMBOX_SIZE, CONFIG_LWIP_UDP_RECVMBOX_SIZE,
             CONFIG_LWIP_TCPIP_TASK_PRIO);
    esp_netif_init();
    esp_event_loop_create_default();

    // 1. Configura e inicia a rede local Wi-Fi com NAT
    wifi_softap_config_t wifi_cfg = WIFI_SOFTAP_DEFAULT_CONFIG();
    wifi_cfg.ssid = "Solar_Gateway_01";
    wifi_cfg.password = "energia123";
    // wifi_cfg.gateway_ip = "10.0.0.1"; // Descomente se quiser mudar o IP padrão!
    
    wifi_softap_nat_init(&wifi_cfg);

    // 1. Configura usando os padrões (APN Vivo, Qualquer Core)
    tpcie_modem_config_t config = TPCIE_MODEM_DEFAULT_CONFIG();
    tpcie_modem_init(&config);

    ESP_LOGI("MAIN", "Solicitando Power ON...");
    tpcie_modem_power_on(); // Liga o modem (Pinos 25 e 4) - Não bloqueia, leva ~10s para ligar

    // 2. Manda conectar (A task de fundo assume o controle e faz os delays físicos)
    ESP_LOGI("MAIN", "Solicitando conexao 4G...");
    tpcie_modem_connect();

    // 3. Espera o IP bloqueando apenas essa task principal (Máximo 120 segundos)
    if (tpcie_wait_for_network(pdMS_TO_TICKS(120000))) {
        ESP_LOGI("MAIN", "Pronto! O sistema está online. Iniciando telemetria...");
        
        while(1) {
            if (tpcie_is_connected()) {
                int csq = tpcie_modem_get_rssi();

                if (csq == -2) {
                    ESP_LOGW("STATUS", "Sinal: Modem ocupado processando dados...");
                } else if (csq < 0) {
                    ESP_LOGW("STATUS", "Sinal: Indisponivel no momento");
                } else {
                    // Cálculo de dBm para termos uma noção real da potência
                    int dbm = (csq * 2) - 113;
                    
                    const char* nivel;
                    if (csq <= 9)       nivel = "CRÍTICO (Causando erro -1)";
                    else if (csq <= 14) nivel = "MARGINAL (Lento)";
                    else if (csq <= 19) nivel = "BOM";
                    else                nivel = "EXCELENTE";

                    ESP_LOGI("GATEWAY", "Sinal: %d CSQ (%d dBm) -> %s", csq, dbm, nivel);
                }
            } else {
                ESP_LOGW("GATEWAY", "Aguardando modem estabelecer conexão...");
            }

            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    } else {
        ESP_LOGE("MAIN", "Falha ao conectar. Desligando modem para economizar bateria.");
        tpcie_modem_power_off();
    }
}
