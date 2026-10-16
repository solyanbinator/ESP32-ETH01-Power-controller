#include "auth_manager.h"

#include <stdio.h>
#include <string.h>
#include "app_defs.h"
#include "bootloader_random.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "nvs.h"

#define NVS_NAMESPACE       "auth"
#define SALT_LEN            16
#define HASH_LEN            32
#define PBKDF2_ITERATIONS   4096
#define SESSION_SLOTS       4
#define SESSION_IDLE_MS     (60LL * 60 * 1000)   // вход действует 1 час с последнего запроса
#define MAX_FAILED_LOGINS   5
#define LOCK_MS             30000

static const char *TAG = "auth";

static SemaphoreHandle_t s_lock;
static bool s_has_password;
static bool s_reset;
static uint8_t s_salt[SALT_LEN];
static uint8_t s_hash[HASH_LEN];
static int s_failed;
static int64_t s_locked_until_ms;

static struct {
    char token[AUTH_TOKEN_LEN + 1];
    int64_t used_ms;
} s_sessions[SESSION_SLOTS];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Без радиомодуля аппаратный ГСЧ ESP32 даёт настоящую случайность только с включённым
// источником энтропии загрузчика: включаем его на время генерации соли и токенов.
static void random_bytes(void *buf, size_t len)
{
    bootloader_random_enable();
    esp_fill_random(buf, len);
    bootloader_random_disable();
}

static bool equal_ct(const void *a, const void *b, size_t len)
{
    const uint8_t *x = a, *y = b;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= x[i] ^ y[i];
    }
    return diff == 0;
}

static bool derive(const char *password, const uint8_t salt[SALT_LEN], uint8_t out[HASH_LEN])
{
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)password, strlen(password),
                                         salt, SALT_LEN, PBKDF2_ITERATIONS, HASH_LEN, out) == 0;
}

// Перемычка IO2–GND: вывод подтянут к питанию внутренней подтяжкой, перемычка даёт LOW
static bool reset_jumper_installed(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_PASSWORD_RESET),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    vTaskDelay(pdMS_TO_TICKS(10));
    for (int i = 0; i < 5; i++) {
        if (gpio_get_level(PIN_PASSWORD_RESET) != 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

esp_err_t auth_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t h;
    if (reset_jumper_installed()) {
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        s_reset = true;
        ESP_LOGW(TAG, "Web password reset by IO2 jumper, remove the jumper");
    }

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t salt_len = SALT_LEN;
        size_t hash_len = HASH_LEN;
        s_has_password = nvs_get_blob(h, "salt", s_salt, &salt_len) == ESP_OK && salt_len == SALT_LEN &&
                         nvs_get_blob(h, "hash", s_hash, &hash_len) == ESP_OK && hash_len == HASH_LEN;
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Web password %s", s_has_password ? "set" : "not set, setup required");
    return ESP_OK;
}

bool auth_password_is_set(void)
{
    return s_has_password;
}

bool auth_was_reset(void)
{
    return s_reset;
}

const char *auth_validate_password(const char *password)
{
    size_t len = password ? strlen(password) : 0;
    if (len < AUTH_PASSWORD_MIN) {
        return "Пароль не короче 6 символов";
    }
    if (len > AUTH_PASSWORD_MAX) {
        return "Пароль слишком длинный";
    }
    return NULL;
}

esp_err_t auth_set_password(const char *password)
{
    if (auth_validate_password(password)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t salt[SALT_LEN];
    uint8_t hash[HASH_LEN];
    random_bytes(salt, sizeof(salt));
    if (!derive(password, salt, hash)) {
        return ESP_FAIL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = s_has_password ? ESP_ERR_INVALID_STATE : ESP_OK;
    nvs_handle_t h;
    if (err == ESP_OK) {
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "salt", salt, sizeof(salt));
        if (err == ESP_OK) {
            err = nvs_set_blob(h, "hash", hash, sizeof(hash));
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        memcpy(s_salt, salt, sizeof(salt));
        memcpy(s_hash, hash, sizeof(hash));
        s_has_password = true;
        ESP_LOGI(TAG, "Web password set");
    }
    xSemaphoreGive(s_lock);
    return err;
}

auth_result_t auth_check_password(const char *password, uint32_t *retry_after_s)
{
    int64_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t locked_ms = s_locked_until_ms - now;
    xSemaphoreGive(s_lock);
    if (locked_ms > 0) {
        *retry_after_s = (uint32_t)((locked_ms + 999) / 1000);
        return AUTH_LOCKED;
    }

    uint8_t hash[HASH_LEN];
    bool ok = s_has_password && password && derive(password, s_salt, hash) && equal_ct(hash, s_hash, HASH_LEN);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ok) {
        s_failed = 0;
    } else if (++s_failed >= MAX_FAILED_LOGINS) {
        s_failed = 0;
        s_locked_until_ms = now + LOCK_MS;
        ESP_LOGW(TAG, "Too many wrong passwords, locked for %d s", LOCK_MS / 1000);
    }
    xSemaphoreGive(s_lock);
    return ok ? AUTH_OK : AUTH_WRONG;
}

void auth_session_create(char token[AUTH_TOKEN_LEN + 1])
{
    uint8_t raw[AUTH_TOKEN_LEN / 2];
    random_bytes(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++) {
        snprintf(&token[i * 2], 3, "%02x", raw[i]);
    }

    int64_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Свободный или самый давно использованный слот
    int slot = 0;
    for (int i = 0; i < SESSION_SLOTS; i++) {
        if (s_sessions[i].token[0] == '\0' || now - s_sessions[i].used_ms >= SESSION_IDLE_MS) {
            slot = i;
            break;
        }
        if (s_sessions[i].used_ms < s_sessions[slot].used_ms) {
            slot = i;
        }
    }
    memcpy(s_sessions[slot].token, token, AUTH_TOKEN_LEN + 1);
    s_sessions[slot].used_ms = now;
    xSemaphoreGive(s_lock);
}

bool auth_session_valid(const char *token)
{
    if (!token || strlen(token) != AUTH_TOKEN_LEN) {
        return false;
    }
    int64_t now = now_ms();
    bool valid = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SESSION_SLOTS; i++) {
        if (s_sessions[i].token[0] != '\0' && now - s_sessions[i].used_ms < SESSION_IDLE_MS &&
            equal_ct(s_sessions[i].token, token, AUTH_TOKEN_LEN)) {
            s_sessions[i].used_ms = now;
            valid = true;
        }
    }
    xSemaphoreGive(s_lock);
    return valid;
}

void auth_session_drop(const char *token)
{
    if (!token || strlen(token) != AUTH_TOKEN_LEN) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SESSION_SLOTS; i++) {
        if (equal_ct(s_sessions[i].token, token, AUTH_TOKEN_LEN)) {
            s_sessions[i].token[0] = '\0';
        }
    }
    xSemaphoreGive(s_lock);
}
