idf_component_register(SRCS "tpcie_modem.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_modem esp_netif esp_driver_gpio)