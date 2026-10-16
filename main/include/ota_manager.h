#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char version[32];
    char build_date[16];
    char build_time[16];
    char partition[17];     // слот, из которого работает прошивка (ota_0 / ota_1)
    bool pending_verify;    // новая прошивка ещё не подтвердила работоспособность
} ota_info_t;

void ota_manager_init(void);
void ota_manager_get_info(ota_info_t *out);

// Новая прошивка после обновления подтверждает работоспособность сама: проработала 15 с,
// получила IP и веб-сервер отвечает. Не подтвердилась — при перезагрузке загрузчик
// вернёт предыдущую версию.
void ota_manager_start_validation(void);

// Записанный образ: версия и время сборки (по ним страница после перезагрузки понимает,
// запустилась ли новая прошивка или загрузчик вернул прежнюю)
typedef struct {
    char version[32];
    char build[32];         // "Oct 16 2026 10:00:00"
} ota_image_t;

// Приём образа (build/esp32_eth01_controller.bin) по частям. При ошибке error — текст для
// пользователя, обновление прерывается.
esp_err_t ota_manager_begin(size_t image_size, const char **error);
esp_err_t ota_manager_write(const void *data, size_t len, const char **error);
esp_err_t ota_manager_end(ota_image_t *image, const char **error);
void ota_manager_abort(void);

// Перезагрузка в новую прошивку через секунду, чтобы ответ успел уйти браузеру
void ota_manager_restart_later(void);
