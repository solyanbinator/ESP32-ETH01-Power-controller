#include "web_server.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "app_defs.h"
#include "auth_manager.h"
#include "bootloader_random.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "ethernet_manager.h"
#include "ota_manager.h"
#include "output_manager.h"
#include "power_manager.h"
#include "server_client.h"

// Сокеты: LWIP_MAX_SOCKETS = 12, httpd занимает max_open_sockets + 3 служебных.
// Остальные — задаче server_client (HTTP-запрос к серверу и UDP для WOL) и самопроверке
// после обновления. Раньше httpd забирал все сокеты, а без LRU висящие keep-alive
// соединения браузера блокировали приём новых — страница переставала отвечать.
#define HTTPD_MAX_SOCKETS   5
#define HTTPD_STACK_SIZE    6144
#define HTTPD_MAX_HANDLERS  12
#define CONFIG_BODY_MAX     512
#define JSON_BUF_SIZE       2048
#define OTA_CHUNK_SIZE      4096

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;
static char s_boot_id[9];   // новый при каждом запуске: страница по нему видит перезагрузку

typedef struct {
    char *buf;
    size_t size;
    size_t len;
} json_buf_t;

__attribute__((format(printf, 2, 3)))
static void json_printf(json_buf_t *b, const char *fmt, ...)
{
    if (b->len >= b->size) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(b->buf + b->len, b->size - b->len, fmt, args);
    va_end(args);
    b->len = n < 0 ? b->size : b->len + (size_t)n;
}

// Строки настроек прошли валидацию и не содержат символов, требующих экранирования в JSON.
static void json_config(json_buf_t *b, const app_config_t *c)
{
    json_printf(b, "\"config\":{\"ip\":\"%s\",\"port\":%u,\"mac\":\"%s\",\"wol_port\":%u,"
                "\"ping_path\":\"%s\",\"poweroff_path\":\"%s\","
                "\"mains_lost_s\":%u,\"mains_restore_s\":%u,\"ping_interval_s\":%u,"
                "\"off_wait_s\":%u,\"on_wait_s\":%u,\"attempts\":%u,"
                "\"outputs_enabled\":%u,\"io32_off_s\":%u,\"io32_on_s\":%u,\"io33_off_s\":%u,\"io33_on_s\":%u}",
                c->ip, c->port, c->mac, c->wol_port, c->ping_path, c->poweroff_path,
                c->mains_lost_s, c->mains_restore_s, c->ping_interval_s,
                c->off_wait_s, c->on_wait_s, c->attempts,
                c->outputs_enabled, c->io32_off_s, c->io32_on_s, c->io33_off_s, c->io33_on_s);
}

static const char *json_bool(bool value)
{
    return value ? "true" : "false";
}

// Версия и время сборки из загруженного образа: в JSON попадают только безопасные символы
static void sanitize(char *s)
{
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || strchr(".-_+~ :", *s))) {
            *s = '?';
        }
    }
}

static long long ago_s(int64_t at_ms, int64_t now)
{
    return at_ms ? (now - at_ms) / 1000 : -1;
}

static uint32_t ceil_s(uint32_t ms)
{
    return (ms + 999) / 1000;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_json_buf(httpd_req_t *req, const json_buf_t *b)
{
    if (b->len >= b->size) {
        ESP_LOGE(TAG, "JSON buffer overflow");
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"Ответ не поместился в буфер\"}");
    }
    return send_json(req, "200 OK", b->buf);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *error)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", error);
    return send_json(req, status, buf);
}

// ------------------------------------------------------------------ вход

static bool session_token(httpd_req_t *req, char token[AUTH_TOKEN_LEN + 1])
{
    size_t len = AUTH_TOKEN_LEN + 1;
    return httpd_req_get_cookie_val(req, "sid", token, &len) == ESP_OK;
}

static bool authorized(httpd_req_t *req)
{
    char token[AUTH_TOKEN_LEN + 1];
    return session_token(req, token) && auth_session_valid(token);
}

// 401: веб-интерфейс по полю auth показывает форму входа или установки пароля
static esp_err_t send_unauthorized(httpd_req_t *req)
{
    return send_json(req, "401 Unauthorized", auth_password_is_set()
                     ? "{\"ok\":false,\"auth\":\"login\",\"error\":\"Требуется вход\"}"
                     : "{\"ok\":false,\"auth\":\"setup\",\"error\":\"Задайте пароль веб-интерфейса\"}");
}

static esp_err_t send_auth_failure(httpd_req_t *req, auth_result_t result, uint32_t retry_after_s, const char *status)
{
    if (result == AUTH_LOCKED) {
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"Слишком много неверных попыток, повторите через %lu с\","
                 "\"retry_after_s\":%lu}", (unsigned long)retry_after_s, (unsigned long)retry_after_s);
        return send_json(req, "429 Too Many Requests", buf);
    }
    return send_error(req, status, "Неверный пароль");
}

static esp_err_t start_session(httpd_req_t *req)
{
    char token[AUTH_TOKEN_LEN + 1];
    auth_session_create(token);
    char cookie[96];
    snprintf(cookie, sizeof(cookie), "sid=%s; Path=/; HttpOnly; SameSite=Strict", token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return send_json(req, "200 OK", "{\"ok\":true}");
}

// ------------------------------------------------------------------ формы

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Декодирует application/x-www-form-urlencoded на месте.
// Старая версия не декодировала %XX: браузер присылает "AA%3ABB..." и "%2Fping",
// поэтому валидация MAC и путей не проходила никогда и настройки не сохранялись.
static bool url_decode(char *s)
{
    char *out = s;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            int hi = hex_digit(p[1]);
            int lo = hi < 0 ? -1 : hex_digit(p[2]);
            if (lo < 0) {
                return false;
            }
            c = (char)(hi << 4 | lo);
            p += 2;
        }
        if ((unsigned char)c < 0x20 || c == 0x7f) {
            return false;
        }
        *out++ = c;
    }
    *out = '\0';
    return true;
}

static bool read_body(httpd_req_t *req, char *buf, size_t len)
{
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        got += (size_t)r;
    }
    buf[len] = '\0';
    return true;
}

// Тело формы в body. false — ответ уже отправлен (или соединение нужно закрыть), *result — что вернуть
static bool read_form(httpd_req_t *req, char body[CONFIG_BODY_MAX + 1], esp_err_t *result)
{
    if (req->content_len == 0 || req->content_len > CONFIG_BODY_MAX) {
        *result = send_error(req, "400 Bad Request", "Некорректный размер запроса");
        return false;
    }
    if (!read_body(req, body, req->content_len)) {
        *result = ESP_FAIL;
        return false;
    }
    return true;
}

// Значение поля формы, декодируется на месте; NULL — поля нет
static const char *form_field(char *body, const char *key)
{
    char *save_ptr = NULL;
    for (char *pair = strtok_r(body, "&", &save_ptr); pair; pair = strtok_r(NULL, "&", &save_ptr)) {
        char *value = strchr(pair, '=');
        if (!value) {
            continue;
        }
        *value++ = '\0';
        if (url_decode(pair) && strcmp(pair, key) == 0) {
            return url_decode(value) ? value : NULL;
        }
    }
    return NULL;
}

// ------------------------------------------------------------------ обработчики

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

// GET /api/auth, без входа: нужно ли задать пароль, выполнен ли вход, какая прошивка работает.
// boot меняется при каждом запуске: увидев другое значение, страница перезагружается целиком.
static esp_err_t auth_state_handler(httpd_req_t *req)
{
    ota_info_t fw;
    ota_manager_get_info(&fw);
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"setup_required\":%s,\"authenticated\":%s,\"reset_by_jumper\":%s,"
             "\"version\":\"%s\",\"build\":\"%s %s\",\"boot\":\"%s\"}",
             json_bool(!auth_password_is_set()), json_bool(authorized(req)), json_bool(auth_was_reset()),
             fw.version, fw.build_date, fw.build_time, s_boot_id);
    return send_json(req, "200 OK", buf);
}

// POST /api/setup, password=...: первая установка пароля или после сброса перемычкой IO2
static esp_err_t setup_handler(httpd_req_t *req)
{
    char body[CONFIG_BODY_MAX + 1];
    esp_err_t result;
    if (!read_form(req, body, &result)) {
        return result;
    }
    if (auth_password_is_set()) {
        return send_error(req, "409 Conflict", "Пароль уже задан");
    }
    const char *password = form_field(body, "password");
    const char *error = auth_validate_password(password);
    if (error) {
        return send_error(req, "400 Bad Request", error);
    }
    esp_err_t err = auth_set_password(password);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "Пароль уже задан");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "Не удалось сохранить пароль");
    }
    return start_session(req);
}

// POST /api/login, password=...
static esp_err_t login_handler(httpd_req_t *req)
{
    char body[CONFIG_BODY_MAX + 1];
    esp_err_t result;
    if (!read_form(req, body, &result)) {
        return result;
    }
    if (!auth_password_is_set()) {
        return send_unauthorized(req);
    }
    uint32_t retry_after_s = 0;
    auth_result_t auth = auth_check_password(form_field(body, "password"), &retry_after_s);
    if (auth != AUTH_OK) {
        return send_auth_failure(req, auth, retry_after_s, "401 Unauthorized");
    }
    return start_session(req);
}

static esp_err_t logout_handler(httpd_req_t *req)
{
    char token[AUTH_TOKEN_LEN + 1];
    if (session_token(req, token)) {
        auth_session_drop(token);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    return send_json(req, "200 OK", "{\"ok\":true}");
}

// Состояние отдаётся из кэша: обработчик не обращается к серверу и отвечает мгновенно.
static esp_err_t status_handler(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_unauthorized(req);
    }
    app_config_t cfg;
    eth_status_t eth;
    server_status_t srv;
    power_status_t pwr;
    output_status_t out;
    ota_info_t fw;
    app_config_get(&cfg);
    ethernet_manager_get_status(&eth);
    server_client_get_status(&srv);
    power_manager_get_status(&pwr);
    output_manager_get_status(&out);
    ota_manager_get_info(&fw);
    int64_t now = esp_timer_get_time() / 1000;

    char buf[JSON_BUF_SIZE];
    json_buf_t b = {buf, sizeof(buf), 0};
    json_printf(&b, "{\"uptime_s\":%lld,", (long long)(now / 1000));
    json_printf(&b, "\"firmware\":{\"version\":\"%s\",\"date\":\"%s %s\",\"partition\":\"%s\",\"pending_verify\":%s},",
                fw.version, fw.build_date, fw.build_time, fw.partition, json_bool(fw.pending_verify));
    json_printf(&b, "\"eth\":{\"link\":%s,\"has_ip\":%s,\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\",\"mac\":\"%s\"},",
                json_bool(eth.link_up), json_bool(eth.has_ip), eth.ip, eth.netmask, eth.gateway, eth.mac);
    json_printf(&b, "\"server\":{\"state\":\"%s\",\"detail\":\"%s\",\"checked_ago_s\":%lld,"
                "\"icmp\":{\"ok\":%s,\"rtt_ms\":%lu},\"http\":{\"enabled\":%s,\"ok\":%s,\"detail\":\"%s\"}},",
                server_state_name(srv.state), srv.state_detail, ago_s(srv.checked_at_ms, now),
                json_bool(srv.icmp_ok), (unsigned long)srv.icmp_rtt_ms,
                json_bool(srv.http_enabled), json_bool(srv.http_ok), srv.http_detail);
    json_printf(&b, "\"command\":{\"name\":\"%s\",\"ok\":%s,\"detail\":\"%s\",\"ago_s\":%lld,"
                "\"busy\":%s,\"pending\":\"%s\"},",
                server_cmd_name(srv.cmd), json_bool(srv.cmd_ok), srv.cmd_detail,
                ago_s(srv.cmd_at_ms, now), json_bool(srv.cmd_busy), server_cmd_name(srv.pending_cmd));
    json_printf(&b, "\"inputs\":{\"power_failure\":%s,\"button\":%s},",
                json_bool(pwr.power_failure), json_bool(pwr.button_pressed));
    json_printf(&b, "\"auto\":{\"mains\":\"%s\",\"confirm_left_s\":%lu,\"hold_left_s\":%lu,"
                "\"waiting_shutdown\":%s,\"scenario\":\"%s\",\"attempts\":%d,"
                "\"max_attempts\":%d,\"next_step_s\":%lu,"
                "\"last\":{\"scenario\":\"%s\",\"result\":\"%s\",\"attempts\":%d,\"ago_s\":%lld}},",
                mains_state_name(pwr.mains), (unsigned long)ceil_s(pwr.confirm_left_ms),
                (unsigned long)ceil_s(pwr.hold_left_ms), json_bool(pwr.waiting_shutdown),
                scenario_name(pwr.scenario), pwr.attempts, pwr.max_attempts,
                (unsigned long)ceil_s(pwr.next_step_ms), scenario_name(pwr.last_scenario),
                scenario_result_name(pwr.last_result), pwr.last_attempts, ago_s(pwr.last_at_ms, now));
    json_printf(&b, "\"outputs\":{\"enabled\":%s,\"io32\":{\"high\":%s,\"change_in_s\":%lu},"
                "\"io33\":{\"high\":%s,\"change_in_s\":%lu}},",
                json_bool(out.enabled), json_bool(out.high[0]), (unsigned long)ceil_s(out.change_in_ms[0]),
                json_bool(out.high[1]), (unsigned long)ceil_s(out.change_in_ms[1]));
    json_config(&b, &cfg);
    json_printf(&b, "}");
    return send_json_buf(req, &b);
}

// POST /api/config, тело: ключ=значение[&ключ=значение...].
// Изменения применяются, только если валидны все поля.
static esp_err_t config_handler(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_unauthorized(req);
    }
    char body[CONFIG_BODY_MAX + 1];
    esp_err_t result;
    if (!read_form(req, body, &result)) {
        return result;
    }

    app_config_t old_cfg;
    app_config_get(&old_cfg);
    app_config_t cfg = old_cfg;
    char *save_ptr = NULL;
    for (char *pair = strtok_r(body, "&", &save_ptr); pair; pair = strtok_r(NULL, "&", &save_ptr)) {
        char *value = strchr(pair, '=');
        if (!value) {
            return send_error(req, "400 Bad Request", "Некорректный формат запроса");
        }
        *value++ = '\0';
        if (!url_decode(pair) || !url_decode(value)) {
            return send_error(req, "400 Bad Request", "Некорректная кодировка запроса");
        }
        const char *error = app_config_set_field(&cfg, pair, value);
        if (error) {
            return send_error(req, "400 Bad Request", error);
        }
    }

    if (app_config_save(&cfg) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "Ошибка записи во flash");
    }
    // Состояние, полученное для старого адреса или пути, больше не актуально
    if (strcmp(cfg.ip, old_cfg.ip) != 0 || cfg.port != old_cfg.port || strcmp(cfg.ping_path, old_cfg.ping_path) != 0) {
        server_client_recheck();
    }

    char buf[JSON_BUF_SIZE];
    json_buf_t b = {buf, sizeof(buf), 0};
    json_printf(&b, "{\"ok\":true,");
    json_config(&b, &cfg);
    json_printf(&b, "}");
    return send_json_buf(req, &b);
}

// POST /api/ota: тело — build/esp32_eth01_controller.bin, пароль ещё раз в заголовке
// X-Password (URL-кодирование). После записи и проверки образа — перезагрузка.
static esp_err_t ota_handler(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_unauthorized(req);
    }
    char password[3 * AUTH_PASSWORD_MAX + 1];
    if (httpd_req_get_hdr_value_str(req, "X-Password", password, sizeof(password)) != ESP_OK || !url_decode(password)) {
        return send_error(req, "403 Forbidden", "Для обновления введите пароль");
    }
    uint32_t retry_after_s = 0;
    auth_result_t auth = auth_check_password(password, &retry_after_s);
    if (auth != AUTH_OK) {
        return send_auth_failure(req, auth, retry_after_s, "403 Forbidden");
    }

    // Перезагрузка не должна прервать сценарий или временно включить выключенные выходы
    power_status_t pwr;
    power_manager_get_status(&pwr);
    if (pwr.scenario != SCENARIO_NONE) {
        return send_error(req, "409 Conflict", "Идёт сценарий автоматики, обновите после его завершения");
    }
    output_status_t out;
    output_manager_get_status(&out);
    if (out.enabled && (!out.high[0] || !out.high[1])) {
        return send_error(req, "409 Conflict", "Выходы IO32/IO33 выключены");
    }

    const char *error = NULL;
    if (ota_manager_begin(req->content_len, &error) != ESP_OK) {
        return send_error(req, "400 Bad Request", error);
    }
    static char chunk[OTA_CHUNK_SIZE];  // обработчики httpd выполняются в одной задаче
    size_t left = req->content_len;
    int timeouts = 0;
    while (left > 0) {
        int r = httpd_req_recv(req, chunk, left < sizeof(chunk) ? left : sizeof(chunk));
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (r <= 0) {
            ota_manager_abort();
            ESP_LOGW(TAG, "Firmware upload interrupted");
            return ESP_FAIL;
        }
        timeouts = 0;
        if (ota_manager_write(chunk, (size_t)r, &error) != ESP_OK) {
            ota_manager_abort();
            return send_error(req, "400 Bad Request", error);
        }
        left -= (size_t)r;
    }
    ota_image_t image;
    if (ota_manager_end(&image, &error) != ESP_OK) {
        return send_error(req, "400 Bad Request", error);
    }
    sanitize(image.version);
    sanitize(image.build);
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"version\":\"%s\",\"build\":\"%s\"}", image.version, image.build);
    esp_err_t err = send_json(req, "200 OK", buf);
    ota_manager_restart_later();
    return err;
}

esp_err_t web_server_start(void)
{
    // Без радиомодуля ГСЧ даёт настоящую случайность только с источником энтропии загрузчика
    bootloader_random_enable();
    snprintf(s_boot_id, sizeof(s_boot_id), "%08lx", (unsigned long)esp_random());
    bootloader_random_disable();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEB_PORT;
    cfg.stack_size = HTTPD_STACK_SIZE;
    cfg.max_open_sockets = HTTPD_MAX_SOCKETS;
    cfg.max_uri_handlers = HTTPD_MAX_HANDLERS;
    cfg.lru_purge_enable = true;  // закрывать самое старое соединение вместо отказа новым

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd start");

    // Без входа доступны только страница и /api/auth, /api/setup, /api/login, /api/logout
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/api/auth", .method = HTTP_GET, .handler = auth_state_handler},
        {.uri = "/api/setup", .method = HTTP_POST, .handler = setup_handler},
        {.uri = "/api/login", .method = HTTP_POST, .handler = login_handler},
        {.uri = "/api/logout", .method = HTTP_POST, .handler = logout_handler},
        {.uri = "/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_handler},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG, "register %s", routes[i].uri);
    }

    ESP_LOGI(TAG, "HTTP server listening on port %d", WEB_PORT);
    return ESP_OK;
}
