idf_component_register(SRCS "main.c"
                            "wifi_provisioning.c"
                            "certificate_manager.c"
                            "internet_verification.c"
                            "mqtt_handler.c"
                    PRIV_REQUIRES esp_wifi 
                                  nvs_flash 
                                  esp_http_server 
                                  esp_http_client
                                  esp-tls
                                  esp_netif
                                  esp_event
                                  mqtt
                    INCLUDE_DIRS ".")
