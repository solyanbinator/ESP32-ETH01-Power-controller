#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_CONFIG_PATH_MAX 64

// Настройки, редактируемые через веб-интерфейс, хранятся в NVS.
// Все строки проходят валидацию и содержат только безопасные для URL/JSON/HTML символы.
typedef struct {
    // Сервер
    char ip[16];                                // IPv4 сервера
    uint16_t port;                              // HTTP-порт сервера
    char mac[18];                               // MAC для WOL, всегда в виде AA:BB:CC:DD:EE:FF
    uint16_t wol_port;                          // UDP-порт WOL
    char ping_path[APP_CONFIG_PATH_MAX];        // HTTP-путь проверки; пустой — только ICMP
    char poweroff_path[APP_CONFIG_PATH_MAX];    // HTTP-путь команды выключения

    // Задержки сценариев, секунды. Шаги «выключение» и «включение» с одними и теми же
    // параметрами используют и автоматика (IO35), и ручной перезапуск кнопкой (IO36).
    uint16_t mains_lost_s;      // IO35 HIGH столько секунд подряд — питание пропало, сценарий выключения
    uint16_t mains_restore_s;   // IO35 LOW столько секунд подряд — питание появилось, сценарий включения
    uint16_t ping_interval_s;   // интервал проверки доступности сервера
    uint16_t off_wait_s;        // после Power OFF: ожидание OFFLINE до повтора команды
    uint16_t on_wait_s;         // после WOL: ожидание ONLINE до повтора команды
    uint16_t attempts;          // сколько раз отправлять команду в одном шаге

    // Выходы IO32/IO33, секунды от смены уровня IO35. Выключено управление — выходы всегда HIGH.
    uint16_t outputs_enabled;   // 0/1
    uint16_t io32_off_s;        // питание пропало и держится столько — IO32 в LOW
    uint16_t io32_on_s;         // питание есть и держится столько — IO32 в HIGH
    uint16_t io33_off_s;
    uint16_t io33_on_s;
} app_config_t;

esp_err_t app_config_init(void);

// Потокобезопасная копия текущих настроек.
void app_config_get(app_config_t *out);

// Проверяет значение и записывает его в cfg. Ключи совпадают с именами полей app_config_t.
// Возвращает NULL при успехе или текст ошибки для пользователя.
const char *app_config_set_field(app_config_t *cfg, const char *key, const char *value);

// Делает cfg текущими настройками и сохраняет изменённые поля в NVS.
esp_err_t app_config_save(const app_config_t *cfg);

// Валидаторы. Те же правила продублированы в main/web/index.html.
bool app_config_valid_ip(const char *ip);
bool app_config_parse_mac(const char *str, uint8_t out[6]);
bool app_config_valid_path(const char *path, bool allow_empty);
