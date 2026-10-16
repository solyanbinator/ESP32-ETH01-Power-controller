#include "ota_manager.h"

#include <stdio.h>
#include <string.h>
#include "app_defs.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "ethernet_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define VALIDATION_UPTIME_MS    15000
#define RESTART_DELAY_US        (1000 * 1000)

// Начало образа приложения: заголовок, первый сегмент и описание приложения (версия, проект)
#define HEAD_SIZE (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static const char *TAG = "ota";

static bool s_pending_verify;
static const esp_partition_t *s_target;
static esp_ota_handle_t s_handle;
static bool s_started;      // esp_ota_begin выполнен
static size_t s_expected;
static size_t s_received;
static uint8_t s_head[HEAD_SIZE];
static esp_app_desc_t s_new_desc;

void ota_manager_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        ESP_LOGW(TAG, "New firmware in %s waits for confirmation", running->label);
    }
    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware %s (%s %s) from %s", desc->version, desc->date, desc->time, running->label);
}

void ota_manager_get_info(ota_info_t *out)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    strlcpy(out->version, desc->version, sizeof(out->version));
    strlcpy(out->build_date, desc->date, sizeof(out->build_date));
    strlcpy(out->build_time, desc->time, sizeof(out->build_time));
    strlcpy(out->partition, esp_ota_get_running_partition()->label, sizeof(out->partition));
    out->pending_verify = s_pending_verify;
}

// Веб-сервер отвечает на запрос к самому себе
static bool web_server_alive(void)
{
    char url[48];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/api/auth", WEB_PORT);
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 2000};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return false;
    }
    bool ok = esp_http_client_perform(client) == ESP_OK && esp_http_client_get_status_code(client) == 200;
    esp_http_client_cleanup(client);
    return ok;
}

static void validation_task(void *arg)
{
    while (esp_timer_get_time() / 1000 < VALIDATION_UPTIME_MS || !ethernet_manager_is_ready() || !web_server_alive()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_pending_verify = false;
        ESP_LOGI(TAG, "New firmware confirmed, rollback cancelled");
    }
    vTaskDelete(NULL);
}

void ota_manager_start_validation(void)
{
    if (s_pending_verify) {
        xTaskCreate(validation_task, "ota_validate", 4096, NULL, 3, NULL);
    }
}

void ota_manager_abort(void)
{
    if (s_started) {
        esp_ota_abort(s_handle);
    }
    s_started = false;
    s_target = NULL;
}

esp_err_t ota_manager_begin(size_t image_size, const char **error)
{
    ota_manager_abort();
    s_target = esp_ota_get_next_update_partition(NULL);
    if (!s_target) {
        *error = "Нет слота для обновления: прошейте плату по USB с новой таблицей разделов";
        return ESP_ERR_NOT_FOUND;
    }
    if (image_size < HEAD_SIZE) {
        *error = "Файл не похож на прошивку";
        return ESP_ERR_INVALID_SIZE;
    }
    if (image_size > s_target->size) {
        *error = "Файл больше слота прошивки";
        return ESP_ERR_INVALID_SIZE;
    }
    s_expected = image_size;
    s_received = 0;
    ESP_LOGI(TAG, "Update to %s, %u bytes", s_target->label, (unsigned)image_size);
    return ESP_OK;
}

// По началу образа: это приложение для ESP32 и для этого же проекта
static esp_err_t check_head(const char **error)
{
    const esp_image_header_t *image = (const esp_image_header_t *)s_head;
    memcpy(&s_new_desc, s_head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(s_new_desc));
    if (image->magic != ESP_IMAGE_HEADER_MAGIC || s_new_desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        *error = "Файл не похож на прошивку: нужен build/esp32_eth01_controller.bin";
        return ESP_ERR_INVALID_ARG;
    }
    if (image->chip_id != ESP_CHIP_ID_ESP32) {
        *error = "Прошивка собрана для другого чипа";
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(s_new_desc.project_name, esp_app_get_description()->project_name, sizeof(s_new_desc.project_name)) != 0) {
        *error = "Прошивка от другого проекта";
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        *error = "Не удалось начать запись во flash";
        return err;
    }
    s_started = true;
    err = esp_ota_write(s_handle, s_head, sizeof(s_head));
    if (err != ESP_OK) {
        *error = "Ошибка записи во flash";
    }
    return err;
}

esp_err_t ota_manager_write(const void *data, size_t len, const char **error)
{
    if (!s_target || s_received + len > s_expected) {
        *error = "Получено больше данных, чем объявлено";
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *p = data;
    // Начало образа копится в буфере, пока не станет возможной проверка заголовка
    if (s_received < HEAD_SIZE) {
        size_t take = HEAD_SIZE - s_received < len ? HEAD_SIZE - s_received : len;
        memcpy(s_head + s_received, p, take);
        s_received += take;
        p += take;
        len -= take;
        if (s_received == HEAD_SIZE) {
            esp_err_t err = check_head(error);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    if (len == 0) {
        return ESP_OK;
    }
    esp_err_t err = esp_ota_write(s_handle, p, len);
    if (err != ESP_OK) {
        *error = "Ошибка записи во flash";
        return err;
    }
    s_received += len;
    return ESP_OK;
}

esp_err_t ota_manager_end(ota_image_t *image, const char **error)
{
    if (!s_started || s_received != s_expected) {
        *error = "Файл получен не полностью";
        ota_manager_abort();
        return ESP_ERR_INVALID_SIZE;
    }
    s_started = false;
    esp_err_t err = esp_ota_end(s_handle);  // проверяет контрольную сумму и SHA-256 образа
    if (err != ESP_OK) {
        *error = err == ESP_ERR_OTA_VALIDATE_FAILED ? "Образ повреждён: не сходится контрольная сумма"
                                                    : "Не удалось завершить запись";
        s_target = NULL;
        return err;
    }
    err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        *error = "Не удалось переключиться на новую прошивку";
        s_target = NULL;
        return err;
    }
    strlcpy(image->version, s_new_desc.version, sizeof(image->version));
    snprintf(image->build, sizeof(image->build), "%.16s %.16s", s_new_desc.date, s_new_desc.time);
    ESP_LOGI(TAG, "Firmware %s written to %s", s_new_desc.version, s_target->label);
    s_target = NULL;
    return ESP_OK;
}

static void restart_cb(void *arg)
{
    ESP_LOGW(TAG, "Restarting into new firmware");
    esp_restart();
}

void ota_manager_restart_later(void)
{
    const esp_timer_create_args_t args = {.callback = restart_cb, .name = "ota_restart"};
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, RESTART_DELAY_US);
    } else {
        esp_restart();
    }
}
