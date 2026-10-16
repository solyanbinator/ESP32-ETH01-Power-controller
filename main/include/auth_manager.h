#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define AUTH_PASSWORD_MIN   6
#define AUTH_PASSWORD_MAX   64      // байт UTF-8
#define AUTH_TOKEN_LEN      32      // hex-символов в идентификаторе сессии

typedef enum {
    AUTH_OK,
    AUTH_WRONG,
    AUTH_LOCKED,    // слишком много неверных попыток подряд
} auth_result_t;

// Загружает пароль веб-интерфейса из NVS. Перемычка IO2–GND при запуске сбрасывает пароль:
// при следующем входе веб-интерфейс попросит задать новый.
esp_err_t auth_init(void);

bool auth_password_is_set(void);
bool auth_was_reset(void);      // пароль сброшен перемычкой при этом запуске

// NULL — пароль подходит, иначе текст ошибки для пользователя
const char *auth_validate_password(const char *password);

// Первичная установка пароля. ESP_ERR_INVALID_STATE — пароль уже задан.
esp_err_t auth_set_password(const char *password);

// Проверка пароля с защитой от подбора; retry_after_s — через сколько можно повторить при AUTH_LOCKED
auth_result_t auth_check_password(const char *password, uint32_t *retry_after_s);

// Сессии хранятся в RAM: после перезагрузки нужно войти заново
void auth_session_create(char token[AUTH_TOKEN_LEN + 1]);
bool auth_session_valid(const char *token);
void auth_session_drop(const char *token);
