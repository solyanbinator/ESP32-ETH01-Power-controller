#include "led_manager.h"

#include <stdbool.h>
#include <stdint.h>
#include "app_defs.h"
#include "driver/gpio.h"
#include "esp_timer.h"

static const gpio_num_t PINS[3] = {PIN_LED_RED, PIN_LED_GREEN, PIN_LED_BLUE};

static led_mode_t s_modes[3] = {LED_ON, LED_OFF, LED_OFF};  // при старте горит красный

void led_manager_init(void)
{
    // Сначала задаём уровень «выключено», потом включаем выход — без вспышки при старте.
    for (int i = 0; i < 3; i++) {
        gpio_set_level(PINS[i], !LED_ACTIVE_LEVEL);
    }
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_LED_RED) | BIT64(PIN_LED_GREEN) | BIT64(PIN_LED_BLUE),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    led_manager_update();
}

void led_manager_set(led_mode_t red, led_mode_t green, led_mode_t blue)
{
    s_modes[0] = red;
    s_modes[1] = green;
    s_modes[2] = blue;
}

// Фаза мигания считается от общего времени, поэтому все светодиоды мигают синхронно.
static bool is_lit(led_mode_t mode, int64_t now_ms)
{
    switch (mode) {
    case LED_ON:
        return true;
    case LED_BLINK:
        return now_ms % LED_BLINK_PERIOD_MS < LED_BLINK_ON_MS;
    case LED_PULSE:
        return now_ms % LED_PULSE_PERIOD_MS < LED_PULSE_ON_MS;
    default:
        return false;
    }
}

void led_manager_update(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    for (int i = 0; i < 3; i++) {
        gpio_set_level(PINS[i], is_lit(s_modes[i], now_ms) ? LED_ACTIVE_LEVEL : !LED_ACTIVE_LEVEL);
    }
}
