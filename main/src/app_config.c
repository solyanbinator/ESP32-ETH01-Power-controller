#include "app_config.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

// Ключи NVS совпадают с прошлой версией прошивки, сохранённые настройки подхватятся.
#define NVS_NAMESPACE   "servercfg"
#define KEY_IP          "ip"
#define KEY_MAC         "mac"
#define KEY_PING_PATH   "pingPath"
#define KEY_OFF_PATH    "off"

#define MSG_IP      "Нужен IPv4-адрес хоста, например 192.168.1.200"
#define MSG_MAC     "Нужен MAC сетевой карты, например AA:BB:CC:DD:EE:FF"
#define MSG_PATH    "Путь начинается с /, допустимы A-Z a-z 0-9 - . _ ~ / ? = &"
#define MSG_NO_PATH "Путь обязателен"

static const app_config_t DEFAULTS = {
    .ip = "192.168.1.200",
    .port = 8080,
    .mac = "AA:BB:CC:DD:EE:FF",
    .wol_port = 9,
    .ping_path = "/ping",
    .poweroff_path = "/poweroff",
    .mains_lost_s = 600,
    .mains_restore_s = 60,
    .ping_interval_s = 10,
    .off_wait_s = 60,
    .on_wait_s = 60,
    .attempts = 3,
    .outputs_enabled = 0,
    .io32_off_s = 60,
    .io32_on_s = 60,
    .io33_off_s = 60,
    .io33_on_s = 60,
};

// Числовые поля: все uint16_t, различаются только ключами и допустимым диапазоном.
// Диапазоны продублированы в main/web/index.html.
typedef struct {
    const char *key;        // ключ в API и JSON
    const char *nvs_key;    // не длиннее 15 символов
    size_t offset;
    uint16_t min;
    uint16_t max;
    const char *error;
} num_field_t;

#define NUM_FIELD(field, nvs, lo, hi, msg) {#field, nvs, offsetof(app_config_t, field), lo, hi, msg}

static const num_field_t NUM_FIELDS[] = {
    NUM_FIELD(port, "port", 1, 65535, "Порт: целое число от 1 до 65535"),
    NUM_FIELD(wol_port, "wolport", 1, 65535, "Порт: целое число от 1 до 65535"),
    NUM_FIELD(mains_lost_s, "mainsLost", 1, 36000, "Целое число секунд от 1 до 36000"),
    NUM_FIELD(mains_restore_s, "mainsRestore", 1, 36000, "Целое число секунд от 1 до 36000"),
    NUM_FIELD(ping_interval_s, "pingInterval", 2, 3600, "Целое число секунд от 2 до 3600"),
    NUM_FIELD(off_wait_s, "offWait", 5, 3600, "Целое число секунд от 5 до 3600"),
    NUM_FIELD(on_wait_s, "onWait", 5, 3600, "Целое число секунд от 5 до 3600"),
    NUM_FIELD(attempts, "attempts", 1, 10, "Целое число от 1 до 10"),
    NUM_FIELD(outputs_enabled, "outEnabled", 0, 1, "0 — выключено, 1 — включено"),
    NUM_FIELD(io32_off_s, "io32Off", 0, 36000, "Целое число секунд от 0 до 36000"),
    NUM_FIELD(io32_on_s, "io32On", 0, 36000, "Целое число секунд от 0 до 36000"),
    NUM_FIELD(io33_off_s, "io33Off", 0, 36000, "Целое число секунд от 0 до 36000"),
    NUM_FIELD(io33_on_s, "io33On", 0, 36000, "Целое число секунд от 0 до 36000"),
};

#define NUM_FIELD_COUNT (sizeof(NUM_FIELDS) / sizeof(NUM_FIELDS[0]))

static app_config_t s_config;
static SemaphoreHandle_t s_lock;

static uint16_t *num_ptr(app_config_t *cfg, const num_field_t *f)
{
    return (uint16_t *)((char *)cfg + f->offset);
}

static uint16_t num_value(const app_config_t *cfg, const num_field_t *f)
{
    return *(const uint16_t *)((const char *)cfg + f->offset);
}

// Строгий IPv4: ровно 4 десятичных октета без ведущих нулей.
// inet_aton из старой версии пропускал "1.2.3", "0x7f.1", "010.0.0.1" (восьмеричное) и мусор после пробела.
bool app_config_valid_ip(const char *ip)
{
    if (!ip) {
        return false;
    }
    unsigned first = 0;
    const char *p = ip;
    for (int i = 0; i < 4; i++) {
        const char *start = p;
        unsigned value = 0;
        while (isdigit((unsigned char)*p) && p - start < 3) {
            value = value * 10 + (unsigned)(*p++ - '0');
        }
        int digits = p - start;
        if (digits == 0 || isdigit((unsigned char)*p) || value > 255 || (digits > 1 && *start == '0')) {
            return false;
        }
        if (i == 0) {
            first = value;
        }
        if (i < 3 && *p++ != '.') {
            return false;
        }
    }
    // Адрес хоста: не 0.x.x.x, не loopback, не multicast/broadcast
    return *p == '\0' && first != 0 && first != 127 && first < 224;
}

// Строгое десятичное число в диапазоне. atoi из старой версии принимал "80abc" и переполнялся.
static bool parse_number(const char *str, uint16_t min, uint16_t max, uint16_t *out)
{
    if (!str || *str == '\0') {
        return false;
    }
    uint32_t value = 0;
    for (const char *p = str; *p; p++) {
        if (!isdigit((unsigned char)*p) || p - str >= 5) {
            return false;
        }
        value = value * 10 + (uint32_t)(*p - '0');
    }
    if (value < min || value > max) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

// AA:BB:CC:DD:EE:FF, AA-BB-CC-DD-EE-FF или AABBCCDDEEFF. Только unicast и не нулевой.
bool app_config_parse_mac(const char *str, uint8_t out[6])
{
    if (!str) {
        return false;
    }
    size_t len = strlen(str);
    char sep = 0;
    if (len == 17) {
        sep = str[2];
        if (sep != ':' && sep != '-') {
            return false;
        }
    } else if (len != 12) {
        return false;
    }
    const char *p = str;
    uint8_t any = 0;
    for (int i = 0; i < 6; i++) {
        if (i > 0 && sep && *p++ != sep) {
            return false;
        }
        int hi = hex_value(p[0]);
        int lo = hi < 0 ? -1 : hex_value(p[1]);
        if (lo < 0) {
            return false;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
        any |= out[i];
        p += 2;
    }
    return any != 0 && (out[0] & 0x01) == 0;
}

// Путь HTTP-запроса: начинается с '/', без пробелов, кавычек, '#', '%' и управляющих символов.
bool app_config_valid_path(const char *path, bool allow_empty)
{
    if (!path) {
        return false;
    }
    if (path[0] == '\0') {
        return allow_empty;
    }
    if (path[0] != '/' || strlen(path) >= APP_CONFIG_PATH_MAX) {
        return false;
    }
    for (const char *p = path; *p; p++) {
        if (!isalnum((unsigned char)*p) && !strchr("/-._~?=&", *p)) {
            return false;
        }
    }
    return true;
}

static const char *set_path(char *dst, const char *value, bool allow_empty)
{
    if (!app_config_valid_path(value, allow_empty)) {
        return value[0] ? MSG_PATH : MSG_NO_PATH;
    }
    strlcpy(dst, value, APP_CONFIG_PATH_MAX);
    return NULL;
}

const char *app_config_set_field(app_config_t *cfg, const char *key, const char *value)
{
    if (!key || !value) {
        return "Пустой параметр";
    }
    for (size_t i = 0; i < NUM_FIELD_COUNT; i++) {
        const num_field_t *f = &NUM_FIELDS[i];
        if (strcmp(key, f->key) == 0) {
            return parse_number(value, f->min, f->max, num_ptr(cfg, f)) ? NULL : f->error;
        }
    }
    if (strcmp(key, "ip") == 0) {
        if (!app_config_valid_ip(value)) {
            return MSG_IP;
        }
        strlcpy(cfg->ip, value, sizeof(cfg->ip));
    } else if (strcmp(key, "mac") == 0) {
        uint8_t b[6];
        if (!app_config_parse_mac(value, b)) {
            return MSG_MAC;
        }
        snprintf(cfg->mac, sizeof(cfg->mac), "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
    } else if (strcmp(key, "ping_path") == 0) {
        return set_path(cfg->ping_path, value, true);
    } else if (strcmp(key, "poweroff_path") == 0) {
        return set_path(cfg->poweroff_path, value, false);
    } else {
        return "Неизвестный параметр";
    }
    return NULL;
}

// Значения из NVS тоже проходят валидацию: битые записи заменяются значениями по умолчанию.
static void load_str(nvs_handle_t h, const char *nvs_key, const char *field)
{
    char buf[APP_CONFIG_PATH_MAX];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, nvs_key, buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK || app_config_set_field(&s_config, field, buf) != NULL) {
        ESP_LOGW(TAG, "Invalid NVS value '%s', using default", nvs_key);
    }
}

static void load_number(nvs_handle_t h, const num_field_t *f)
{
    uint16_t value;
    esp_err_t err = nvs_get_u16(h, f->nvs_key, &value);
    if (err == ESP_OK && value >= f->min && value <= f->max) {
        *num_ptr(&s_config, f) = value;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Invalid NVS value '%s', using default", f->nvs_key);
    }
}

esp_err_t app_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_config = DEFAULTS;
    nvs_handle_t h;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        load_str(h, KEY_IP, "ip");
        load_str(h, KEY_MAC, "mac");
        load_str(h, KEY_PING_PATH, "ping_path");
        load_str(h, KEY_OFF_PATH, "poweroff_path");
        for (size_t i = 0; i < NUM_FIELD_COUNT; i++) {
            load_number(h, &NUM_FIELDS[i]);
        }
        nvs_close(h);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_open: %s, using defaults", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Server %s:%u, MAC %s, WOL port %u, ping '%s', poweroff '%s'",
             s_config.ip, s_config.port, s_config.mac, s_config.wol_port,
             s_config.ping_path, s_config.poweroff_path);
    ESP_LOGI(TAG, "Timings: mains lost %us, restore %us, ping %us, off wait %us, on wait %us, attempts %u",
             s_config.mains_lost_s, s_config.mains_restore_s, s_config.ping_interval_s,
             s_config.off_wait_s, s_config.on_wait_s, s_config.attempts);
    ESP_LOGI(TAG, "Outputs %s: IO32 off %us / on %us, IO33 off %us / on %us",
             s_config.outputs_enabled ? "enabled" : "disabled", s_config.io32_off_s, s_config.io32_on_s,
             s_config.io33_off_s, s_config.io33_on_s);
    return ESP_OK;
}

void app_config_get(app_config_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_config;
    xSemaphoreGive(s_lock);
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Пишем только изменённые поля: при автосохранении меняется одно поле за раз.
    esp_err_t err = ESP_OK;
    if (err == ESP_OK && strcmp(cfg->ip, s_config.ip) != 0) {
        err = nvs_set_str(h, KEY_IP, cfg->ip);
    }
    if (err == ESP_OK && strcmp(cfg->mac, s_config.mac) != 0) {
        err = nvs_set_str(h, KEY_MAC, cfg->mac);
    }
    if (err == ESP_OK && strcmp(cfg->ping_path, s_config.ping_path) != 0) {
        err = nvs_set_str(h, KEY_PING_PATH, cfg->ping_path);
    }
    if (err == ESP_OK && strcmp(cfg->poweroff_path, s_config.poweroff_path) != 0) {
        err = nvs_set_str(h, KEY_OFF_PATH, cfg->poweroff_path);
    }
    for (size_t i = 0; i < NUM_FIELD_COUNT && err == ESP_OK; i++) {
        const num_field_t *f = &NUM_FIELDS[i];
        if (num_value(cfg, f) != num_value(&s_config, f)) {
            err = nvs_set_u16(h, f->nvs_key, num_value(cfg, f));
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        s_config = *cfg;
    }
    xSemaphoreGive(s_lock);

    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(err));
    }
    return err;
}
