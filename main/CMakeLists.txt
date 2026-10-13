idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    EMBED_FILES "web/index.html"
    PRIV_REQUIRES nvs_flash esp_eth esp_netif esp_event esp_timer
                  esp_http_server esp_http_client esp_driver_gpio lwip
)

# Тестовая сборка: входы IO35/IO36 и ICMP-проверка подменяются через POST /api/test
# (используется server/test_scenarios.py). Собирается в отдельную папку, чтобы не
# перепутать с рабочей прошивкой:
#   idf.py -B build_test -DAPP_TEST_HOOKS=ON build
if(APP_TEST_HOOKS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_TEST_HOOKS=1)
endif()
