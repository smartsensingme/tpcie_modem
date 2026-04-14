#ifndef WIFI_SOFTAP_NAT_H
#define WIFI_SOFTAP_NAT_H

#include "esp_err.h"

// Struct opcional para configurações avançadas 
typedef struct {
    const char* ssid;
    const char* password;
    uint8_t channel;
    const char* gateway_ip; // Ex: "192.168.4.1"
} wifi_softap_config_t;

// Macro com valores padrão
#define WIFI_SOFTAP_DEFAULT_CONFIG() { \
    .ssid = "Gateway_Solar_1", \
    .password = "energia123", \
    .channel = 6, \
    .gateway_ip = "192.168.4.1" \
}

/**
 * @brief Inicia o Roteador Wi-Fi com NAT ativado.
 */
esp_err_t wifi_softap_nat_init(const wifi_softap_config_t *config);

/**
 * @brief Desliga o rádio Wi-Fi, o servidor DHCP e o NAT (Economiza ~100mA).
 */
esp_err_t wifi_softap_nat_power_off(void);

/**
 * @brief Retorna o número de dispositivos (S3) atualmente conectados no Gateway.
 */
uint8_t wifi_softap_get_station_count(void);

#endif // WIFI_SOFTAP_NAT_H