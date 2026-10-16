#include "app_config.h"
#include "auth_manager.h"
#include "esp_log.h"
#include "ethernet_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_manager.h"
#include "ota_manager.h"
#include "output_manager.h"
#include "power_manager.h"
#include "server_client.h"
#include "web_server.h"

static const char *TAG = "main";

// Индикация (полная логика светодиодов — этап 5):
// кнопка IO36 удерживается, сценарий ещё не запущен — горит только синий;
// идёт сценарий включения — вспышки зелёного, выключения — вспышки красного (250/750 мс);
// иначе красный мигает — нет сети, красный горит — сервер недоступен, зелёный — сервер онлайн.
static void update_indication(void)
{
    power_status_t power;
    power_manager_get_status(&power);
    if (power.hold_left_ms > 0) {
        led_manager_set(LED_OFF, LED_OFF, LED_ON);
    } else if (power.scenario != SCENARIO_NONE) {
        bool power_on = scenario_is_power_on(power.scenario);
        led_manager_set(power_on ? LED_OFF : LED_PULSE, power_on ? LED_PULSE : LED_OFF, LED_OFF);
    } else if (!ethernet_manager_is_ready()) {
        led_manager_set(LED_BLINK, LED_OFF, LED_OFF);
    } else if (server_client_get_state() == SERVER_STATE_ONLINE) {
        led_manager_set(LED_OFF, LED_ON, LED_OFF);
    } else {
        led_manager_set(LED_ON, LED_OFF, LED_OFF);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(auth_init());
    ota_manager_init();
    led_manager_init();
    output_manager_init();
    power_manager_init();
    ESP_ERROR_CHECK(ethernet_manager_start());
    ESP_ERROR_CHECK(server_client_start());
    ESP_ERROR_CHECK(web_server_start());
    ota_manager_start_validation();
    ESP_LOGI(TAG, "ESP32 ETH01 controller started");

    // Главный цикл ничего не ждёт по сети: запросы к серверу выполняет задача server_client.
    while (true) {
        power_manager_update();
        update_indication();
        led_manager_update();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
