idf_component_register(SRCS "main.c" "dht22.c" "oled.c"
                        INCLUDE_DIRS "."
                        REQUIRES esp_adc esp_timer hal esp_rom
                                 freertos esp_driver_gpio driver
                                 esp_wifi esp_event nvs_flash
                                 esp_http_client)