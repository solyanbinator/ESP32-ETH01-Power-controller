#pragma once

// Настройки, задаваемые при сборке: распиновка, полярность сигналов, сетевые таймауты.
// Настройки, редактируемые через веб-интерфейс (сервер и задержки сценариев), — в app_config.h.

// --- GPIO ---
#define PIN_POWER_SENSE         35  // наличие сети в квартире
#define PIN_MANUAL_BUTTON       36  // кнопка ручного сценария

#define PIN_OUTPUT_1            32  // выходы, управляемые по питанию (output_manager)
#define PIN_OUTPUT_2            33

#define PIN_LED_RED             12  // strapping-пин (MTDI): схема светодиода не должна тянуть его вверх при старте
#define PIN_LED_GREEN           14
#define PIN_LED_BLUE            15

// --- Полярность сигналов ---
#define POWER_FAILURE_LEVEL     1   // HIGH — питание квартиры отключено, LOW — питание есть
#define BUTTON_ACTIVE_LEVEL     1
#define LED_ACTIVE_LEVEL        0   // светодиоды горят при LOW
#define OUTPUT_ON_LEVEL         1   // IO32/IO33: HIGH — включено, LOW — выключено

// --- Ethernet PHY (WT32-ETH01, LAN8720) ---
#define ETH_PHY_ADDR            1
#define ETH_MDC_GPIO            23
#define ETH_MDIO_GPIO           18
#define ETH_PHY_POWER_GPIO      16  // включает генератор 50 МГц, используется как линия сброса PHY
#define ETH_HOSTNAME            "esp32-eth01"

// --- Проверка сервера и WOL (интервал проверки задаётся в веб-интерфейсе) ---
#define ICMP_ATTEMPTS           3       // эхо-запросов за проверку, достаточно одного ответа
#define ICMP_TIMEOUT_MS         1000
#define HTTP_TIMEOUT_MS         3000
#define WOL_REPEAT              3       // magic packet по UDP может потеряться
#define WOL_REPEAT_DELAY_MS     1000

// --- Светодиоды ---
#define LED_BLINK_PERIOD_MS     1000    // нет сети: 500 мс горит / 500 мс нет
#define LED_BLINK_ON_MS         500
#define LED_PULSE_PERIOD_MS     1000    // идёт сценарий: 250 мс горит / 750 мс нет
#define LED_PULSE_ON_MS         250

// --- Кнопка IO36 ---
#define BUTTON_DEBOUNCE_MS      50
#define BUTTON_HOLD_MS          4000    // удержание для запуска ручного перезапуска

// --- Веб-интерфейс ESP32 ---
#define WEB_PORT                80
