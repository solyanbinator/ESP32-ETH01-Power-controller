#include "web_server.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "app_defs.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ethernet_manager.h"
#include "output_manager.h"
#include "power_manager.h"
#include "server_client.h"

// Сокеты: LWIP_MAX_SOCKETS = 10, httpd занимает max_open_sockets + 3 служебных.
// Оставляем 2 сокета задаче server_client (HTTP-запрос к серверу и UDP для WOL).
// Раньше httpd забирал все 10 (7 + 3), а без LRU висящие keep-alive соединения
// браузера блокировали приём новых — страница переставала отвечать.
#define HTTPD_MAX_SOCKETS   5
#define HTTPD_STACK_SIZE    6144
#define CONFIG_BODY_MAX     512
#define JSON_BUF_SIZE       2048

#ifdef APP_TEST_HOOKS
#define TEST_HOOKS true
#else
#define TEST_HOOKS false
#endif

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;

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

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

// Состояние отдаётся из кэша: обработчик не обращается к серверу и отвечает мгновенно.
static esp_err_t status_handler(httpd_req_t *req)
{
    app_config_t cfg;
    eth_status_t eth;
    server_status_t srv;
    power_status_t pwr;
    output_status_t out;
    app_config_get(&cfg);
    ethernet_manager_get_status(&eth);
    server_client_get_status(&srv);
    power_manager_get_status(&pwr);
    output_manager_get_status(&out);
    int64_t now = esp_timer_get_time() / 1000;

    char buf[JSON_BUF_SIZE];
    json_buf_t b = {buf, sizeof(buf), 0};
    json_printf(&b, "{\"uptime_s\":%lld,\"test_hooks\":%s,", (long long)(now / 1000), json_bool(TEST_HOOKS));
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

static esp_err_t send_config_error(httpd_req_t *req, const char *status, const char *error)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", error);
    return send_json(req, status, buf);
}

// POST /api/config, тело: ключ=значение[&ключ=значение...].
// Изменения применяются, только если валидны все поля.
static esp_err_t config_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > CONFIG_BODY_MAX) {
        return send_config_error(req, "400 Bad Request", "Некорректный размер запроса");
    }
    char body[CONFIG_BODY_MAX + 1];
    if (!read_body(req, body, req->content_len)) {
        return ESP_FAIL;  // соединение закрывается
    }

    app_config_t old_cfg;
    app_config_get(&old_cfg);
    app_config_t cfg = old_cfg;
    char *save_ptr = NULL;
    for (char *pair = strtok_r(body, "&", &save_ptr); pair; pair = strtok_r(NULL, "&", &save_ptr)) {
        char *value = strchr(pair, '=');
        if (!value) {
            return send_config_error(req, "400 Bad Request", "Некорректный формат запроса");
        }
        *value++ = '\0';
        if (!url_decode(pair) || !url_decode(value)) {
            return send_config_error(req, "400 Bad Request", "Некорректная кодировка запроса");
        }
        const char *error = app_config_set_field(&cfg, pair, value);
        if (error) {
            return send_config_error(req, "400 Bad Request", error);
        }
    }

    if (app_config_save(&cfg) != ESP_OK) {
        return send_config_error(req, "500 Internal Server Error", "Ошибка записи во flash");
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

#ifdef APP_TEST_HOOKS
// -1 — читать GPIO, 0/1 — принудительный уровень
static bool parse_override(const char *value, int *out)
{
    if (strcmp(value, "auto") == 0) {
        *out = -1;
    } else if (strcmp(value, "0") == 0 || strcmp(value, "1") == 0) {
        *out = value[0] - '0';
    } else {
        return false;
    }
    return true;
}

// POST /api/test, только в тестовой сборке: power_failure=auto|0|1, button=auto|0|1, icmp=auto|off
static esp_err_t test_handler(httpd_req_t *req)
{
    char body[CONFIG_BODY_MAX + 1] = "";
    if (req->content_len > CONFIG_BODY_MAX) {
        return send_config_error(req, "400 Bad Request", "Некорректный размер запроса");
    }
    if (req->content_len > 0 && !read_body(req, body, req->content_len)) {
        return ESP_FAIL;
    }

    power_test_inputs_t inputs;
    power_manager_test_get(&inputs);
    bool icmp = server_client_test_icmp_enabled();
    char *save_ptr = NULL;
    for (char *pair = strtok_r(body, "&", &save_ptr); pair; pair = strtok_r(NULL, "&", &save_ptr)) {
        char *value = strchr(pair, '=');
        if (!value) {
            return send_config_error(req, "400 Bad Request", "Некорректный формат запроса");
        }
        *value++ = '\0';
        bool ok = url_decode(pair) && url_decode(value);
        if (ok && strcmp(pair, "power_failure") == 0) {
            ok = parse_override(value, &inputs.power_failure);
        } else if (ok && strcmp(pair, "button") == 0) {
            ok = parse_override(value, &inputs.button);
        } else if (ok && strcmp(pair, "icmp") == 0) {
            ok = strcmp(value, "auto") == 0 || strcmp(value, "off") == 0;
            icmp = strcmp(value, "auto") == 0;
        } else {
            ok = false;
        }
        if (!ok) {
            return send_config_error(req, "400 Bad Request", "Ожидается power_failure|button=auto|0|1, icmp=auto|off");
        }
    }
    power_manager_test_set(&inputs);
    server_client_test_set_icmp(icmp);

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"power_failure\":%d,\"button\":%d,\"icmp\":\"%s\"}",
             inputs.power_failure, inputs.button, icmp ? "auto" : "off");
    return send_json(req, "200 OK", buf);
}
#endif

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEB_PORT;
    cfg.stack_size = HTTPD_STACK_SIZE;
    cfg.max_open_sockets = HTTPD_MAX_SOCKETS;
    cfg.lru_purge_enable = true;  // закрывать самое старое соединение вместо отказа новым

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd start");

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_handler},
#ifdef APP_TEST_HOOKS
        {.uri = "/api/test", .method = HTTP_POST, .handler = test_handler},
#endif
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG, "register %s", routes[i].uri);
    }

    ESP_LOGI(TAG, "HTTP server listening on port %d", WEB_PORT);
    return ESP_OK;
}
