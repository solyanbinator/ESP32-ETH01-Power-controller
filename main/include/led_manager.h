#pragma once

typedef enum {
    LED_OFF,
    LED_ON,
    LED_BLINK,      // равномерное мигание, LED_BLINK_ON_MS из LED_BLINK_PERIOD_MS
    LED_PULSE,      // короткие вспышки, LED_PULSE_ON_MS из LED_PULSE_PERIOD_MS
} led_mode_t;

void led_manager_init(void);
void led_manager_set(led_mode_t red, led_mode_t green, led_mode_t blue);

// Вызывается из главного цикла, отрабатывает мигание без задержек.
void led_manager_update(void);
