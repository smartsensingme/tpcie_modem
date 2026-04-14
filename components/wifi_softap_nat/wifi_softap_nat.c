#include "wifi_softap_nat.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "lwip/lwip_napt.h"
#include <string.h>

static const char *TAG = "WIFI_NAT";
static esp_netif_t *s_ap_netif = NULL;
static bool s_is_initialized = false;

// ========================================================
// HANDLER PRIVADO DE EVENTOS
// ========================================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Dispositivo Conectado! MAC: " MACSTR ", AID=%d", MAC2STR(event->mac), event->aid);
    } 
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGW(TAG, "Dispositivo Desconectado! MAC: " MACSTR ", AID=%d", MAC2STR(event->mac), event->aid);
    }
}

// ========================================================
// FUNÇÕES PÚBLICAS DA API
// ========================================================
esp_err_t wifi_softap_nat_init(const wifi_softap_config_t *config) {
    if (s_is_initialized) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Iniciando Roteador Wi-Fi...");

    // =======================================================
    // ARMADURA DEFENSIVA: Auto-Inicialização de Subsistemas
    // =======================================================
    esp_netif_init();
    esp_err_t evt_err = esp_event_loop_create_default();
    if (evt_err != ESP_OK && evt_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Falha critica: Nao foi possivel iniciar o loop de eventos.");
        return evt_err;
    }

    // =======================================================
    // 1. CONFIGURAÇÃO DA INTERFACE E IP
    // =======================================================
    if (s_ap_netif == NULL) s_ap_netif = esp_netif_create_default_wifi_ap();
    
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(s_ap_netif, &ip_info);
    
    // Converte as strings de IP para o formato do ESP32
    ip_info.ip.addr = esp_ip4addr_aton(config->gateway_ip);
    ip_info.gw.addr = esp_ip4addr_aton(config->gateway_ip);
    ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0"); // Sintaxe corrigida!
    
    esp_netif_dhcps_stop(s_ap_netif); // Para o DHCP para aplicar as mudanças
    esp_netif_set_ip_info(s_ap_netif, &ip_info);

    // =======================================================
    // 2. A MÁGICA DO DNS EXTERNO (Google: 8.8.8.8)
    // =======================================================
    // Avisa ao DHCP para enviar a opção de DNS aos clientes
    uint8_t opt_val = 1; 
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &opt_val, sizeof(opt_val));
    
    // Configura qual será esse DNS
    esp_netif_dns_info_t dns_info;
    dns_info.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    
    // Religa o DHCP com o novo IP e o novo DNS
    esp_netif_dhcps_start(s_ap_netif); 

    // =======================================================
    // 3. CONFIGURAÇÃO DA ANTENA E RÁDIO
    // =======================================================
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(config->ssid),
            .channel = config->channel,
            .max_connection = 10,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };
    strcpy((char*)wifi_config.ap.ssid, config->ssid);
    strcpy((char*)wifi_config.ap.password, config->password);

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    // =======================================================
    // 4. ATIVAÇÃO DO ROTEAMENTO (A Ponte para o Modem)
    // =======================================================
    esp_err_t nat_err = esp_netif_napt_enable(s_ap_netif);
    if (nat_err != ESP_OK) {
        ESP_LOGE(TAG, "FALHA CRÍTICA NO NAT (%s). Volte no menuconfig e ative 'IP Forwarding' e 'NAT'!", esp_err_to_name(nat_err));
    } else {
        ESP_LOGI(TAG, "Roteador NAT Ativado com Sucesso!");
    }

    s_is_initialized = true;
    ESP_LOGI(TAG, "Rede '%s' no ar! IP Gateway: %s", config->ssid, config->gateway_ip);
    return ESP_OK;
}

esp_err_t wifi_softap_nat_power_off(void) {
    if (!s_is_initialized) return ESP_ERR_INVALID_STATE;
    
    ESP_LOGI(TAG, "Desligando Roteador Wi-Fi...");
    
    esp_netif_napt_disable(s_ap_netif);
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    
    s_is_initialized = false;
    return ESP_OK;
}

uint8_t wifi_softap_get_station_count(void) {
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return sta_list.num;
    }
    return 0;
}