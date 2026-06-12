idf_component_register(SRCS "main.c"
                            "wifi_provisioning.c"
                            "certificate_manager.c"
                            "mtls_client_certs.c"
                            "internet_verification.c"
                            "mqtt_handler.c"
                            "prov_token_validate.c"
                            "ota_signing.c"
                            "ota_handler.c"
                            "http_mtls_client.c"
                            "ed25519/verify.c"
                            "ed25519/sha512.c"
                            "ed25519/fe.c"
                            "ed25519/ge.c"
                            "ed25519/sc.c"
                    PRIV_REQUIRES esp_wifi
                                  nvs_flash
                                  esp_http_server
                                  esp_http_client
                                  esp-tls
                                  esp_netif
                                  esp_event
                                  mqtt
                                  mbedtls
                                  espressif__cjson
                                  app_update
                                  esp_timer
                    INCLUDE_DIRS "." "ed25519")

target_compile_definitions(${COMPONENT_LIB} PRIVATE FIRMWARE_VERSION=\"${CONFIG_FIRMWARE_VERSION}\")

if(CONFIG_USE_EMBEDDED_MTLS_CERTS)
    target_add_binary_data(${COMPONENT_LIB}
        "mtls_client/certs/primary/client.crt" TEXT)
    target_add_binary_data(${COMPONENT_LIB}
        "mtls_client/certs/primary/client.key" TEXT)
    target_add_binary_data(${COMPONENT_LIB}
        "mtls_client/certs/ca_root.pem" TEXT)
endif()
