#include "output_manager.h"

#include "app_defs.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "outputs";

static const gpio_num_t PINS[OUTPUT_COUNT] = {PIN_OUTPUT_1, PIN_OUTPUT_2};

static bool s_on[OUTPUT_COUNT];
static output_status_t s_public;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void set_output(int i, bool on)
{
    if (s_on[i] == on) {
        return;
    }
    s_on[i] = on;
    gpio_set_level(PINS[i], on ? OUTPUT_ON_LEVEL : !OUTPUT_ON_LEVEL);
    ESP_LOGI(TAG, "IO%d -> %s", PINS[i], on ? "on" : "off");
}

void output_manager_init(void)
{
    // Сначала уровень «включено», потом выход — без провала при старте.
    // Режим вход+выход, чтобы в статусе показывать фактический уровень на выводе.
    for (int i = 0; i < OUTPUT_COUNT; i++) {
        s_on[i] = true;
        gpio_set_level(PINS[i], OUTPUT_ON_LEVEL);
    }
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_OUTPUT_1) | BIT64(PIN_OUTPUT_2),
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

void output_manager_update(const app_config_t *cfg, bool power_failure, int64_t stable_ms)
{
    const uint16_t off_s[OUTPUT_COUNT] = {cfg->io32_off_s, cfg->io33_off_s};
    const uint16_t on_s[OUTPUT_COUNT] = {cfg->io32_on_s, cfg->io33_on_s};
    output_status_t status = {.enabled = cfg->outputs_enabled != 0};

    for (int i = 0; i < OUTPUT_COUNT; i++) {
        if (!status.enabled) {
            set_output(i, true);
        } else {
            bool target = !power_failure;
            int64_t delay_ms = (int64_t)(power_failure ? off_s[i] : on_s[i]) * 1000;
            if (s_on[i] != target) {
                if (stable_ms >= delay_ms) {
                    set_output(i, target);
                } else {
                    status.change_in_ms[i] = (uint32_t)(delay_ms - stable_ms);
                }
            }
        }
        status.high[i] = gpio_get_level(PINS[i]) != 0;
    }

    taskENTER_CRITICAL(&s_mux);
    s_public = status;
    taskEXIT_CRITICAL(&s_mux);
}

void output_manager_get_status(output_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_public;
    taskEXIT_CRITICAL(&s_mux);
}
