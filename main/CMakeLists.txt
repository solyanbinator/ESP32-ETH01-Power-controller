idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    EMBED_FILES "web/index.html"
    PRIV_REQUIRES nvs_flash esp_eth esp_netif esp_event esp_timer
                  esp_http_server esp_http_client esp_driver_gpio lwip
                  app_update esp_app_format bootloader_support mbedtls
)
