#include "server_client.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "app_defs.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ethernet_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/sockets.h"

#define WORKER_STACK_SIZE   6144
#define WORKER_PRIORITY     5
#define QUEUE_LENGTH        4
#define NO_NETWORK_RETRY_MS 1000
#define ICMP_ECHO_ID        0xE532
#define ICMP_PAYLOAD_SIZE   32

static const char *TAG = "server";

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static server_status_t s_status;
static int s_pending;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

const char *server_state_name(server_state_t state)
{
    switch (state) {
    case SERVER_STATE_ONLINE:
        return "online";
    case SERVER_STATE_OFFLINE:
        return "offline";
    default:
        return "unknown";
    }
}

const char *server_cmd_name(server_cmd_t cmd)
{
    switch (cmd) {
    case SERVER_CMD_WOL:
        return "wol";
    case SERVER_CMD_POWEROFF:
        return "poweroff";
    default:
        return "none";
    }
}

static void describe_error(esp_err_t err, char *detail, size_t size)
{
    if (err == ESP_ERR_HTTP_CONNECT) {
        snprintf(detail, size, "HTTP: нет соединения");
    } else if (err == ESP_ERR_HTTP_FETCH_HEADER || err == ESP_ERR_HTTP_EAGAIN) {
        snprintf(detail, size, "HTTP: нет ответа");
    } else {
        snprintf(detail, size, "HTTP: %s", esp_err_to_name(err));
    }
}

// Блокирующий HTTP GET, вызывается только из рабочей задачи.
static bool http_get(const app_config_t *cfg, const char *path, char *detail, size_t size)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%u%s", cfg->ip, cfg->port, path);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        snprintf(detail, size, "HTTP: нет памяти");
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        describe_error(err, detail, size);
        ESP_LOGD(TAG, "GET %s: %s", url, esp_err_to_name(err));
        return false;
    }
    snprintf(detail, size, "HTTP %d", status);
    ESP_LOGD(TAG, "GET %s: HTTP %d", url, status);
    return status >= 200 && status < 400;
}

// ICMP echo через raw-сокет: до ICMP_ATTEMPTS запросов, успех по первому ответу.
static bool icmp_ping(const char *ip, uint32_t *rtt_ms)
{
    struct sockaddr_in dst = {.sin_family = AF_INET};
    if (inet_aton(ip, &dst.sin_addr) == 0) {
        return false;
    }
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        ESP_LOGW(TAG, "ICMP socket: errno %d", errno);
        return false;
    }
    struct timeval tv = {.tv_sec = ICMP_TIMEOUT_MS / 1000, .tv_usec = (ICMP_TIMEOUT_MS % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    static uint16_t s_seqno;
    bool ok = false;
    for (int attempt = 0; attempt < ICMP_ATTEMPTS && !ok; attempt++) {
        struct {
            struct icmp_echo_hdr hdr;
            uint8_t payload[ICMP_PAYLOAD_SIZE];
        } req;
        memset(&req, 0, sizeof(req));
        ICMPH_TYPE_SET(&req.hdr, ICMP_ECHO);
        req.hdr.id = htons(ICMP_ECHO_ID);
        req.hdr.seqno = htons(++s_seqno);
        memset(req.payload, 'E', sizeof(req.payload));
        req.hdr.chksum = inet_chksum(&req, sizeof(req));

        int64_t sent_at = esp_timer_get_time();
        if (sendto(sock, &req, sizeof(req), 0, (struct sockaddr *)&dst, sizeof(dst)) != sizeof(req)) {
            continue;
        }
        // Сокет получает все входящие ICMP-пакеты: ждём ответ с нашими id и seqno
        while (esp_timer_get_time() - sent_at < ICMP_TIMEOUT_MS * 1000LL) {
            uint8_t buf[64 + sizeof(req)];
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
            if (len < 0) {
                break;  // таймаут
            }
            const struct ip_hdr *iph = (const struct ip_hdr *)buf;
            size_t hdr_len = IPH_HL_BYTES(iph);
            if (from.sin_addr.s_addr != dst.sin_addr.s_addr || len < hdr_len + sizeof(struct icmp_echo_hdr)) {
                continue;
            }
            const struct icmp_echo_hdr *rep = (const struct icmp_echo_hdr *)(buf + hdr_len);
            if (ICMPH_TYPE(rep) == ICMP_ER && rep->id == req.hdr.id && rep->seqno == req.hdr.seqno) {
                *rtt_ms = (uint32_t)((esp_timer_get_time() - sent_at) / 1000);
                ok = true;
                break;
            }
        }
    }
    close(sock);
    return ok;
}

static bool send_wol(const app_config_t *cfg, char *detail, size_t size)
{
    uint8_t mac[6];
    if (!app_config_parse_mac(cfg->mac, mac)) {
        snprintf(detail, size, "некорректный MAC");
        return false;
    }
    // Magic packet: 6 x 0xFF и 16 повторов MAC
    uint8_t packet[102];
    memset(packet, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(&packet[6 + i * 6], mac, sizeof(mac));
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        snprintf(detail, size, "socket: errno %d", errno);
        return false;
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(cfg->wol_port),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    int sent = 0;
    int send_errno = 0;
    for (int i = 0; i < WOL_REPEAT; i++) {
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(WOL_REPEAT_DELAY_MS));
        }
        if (sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&dst, sizeof(dst)) == sizeof(packet)) {
            sent++;
        } else {
            send_errno = errno;
        }
    }
    close(sock);

    if (sent == 0) {
        snprintf(detail, size, "sendto: errno %d", send_errno);
        return false;
    }
    snprintf(detail, size, "magic packet x%d на %s, UDP %u", sent, cfg->mac, cfg->wol_port);
    return true;
}

typedef struct {
    server_state_t state;
    char detail[SERVER_DETAIL_MAX];
    bool icmp_ok;
    uint32_t icmp_rtt_ms;
    bool http_enabled;
    bool http_ok;
    char http_detail[40];
} check_result_t;

// ICMP может быть закрыт файрволом (Windows по умолчанию), а HTTP-агент может быть
// не запущен при работающей ОС, поэтому сервер онлайн, если ответила любая из проверок.
static void check_server(const app_config_t *cfg, check_result_t *r)
{
    memset(r, 0, sizeof(*r));
    if (!ethernet_manager_is_ready()) {
        r->state = SERVER_STATE_UNKNOWN;
        snprintf(r->detail, sizeof(r->detail), "нет сети");
        return;
    }

    r->icmp_ok = icmp_ping(cfg->ip, &r->icmp_rtt_ms);
    r->http_enabled = cfg->ping_path[0] != '\0';
    if (r->http_enabled) {
        r->http_ok = http_get(cfg, cfg->ping_path, r->http_detail, sizeof(r->http_detail));
    } else {
        snprintf(r->http_detail, sizeof(r->http_detail), "HTTP выкл.");
    }
    r->state = (r->icmp_ok || r->http_ok) ? SERVER_STATE_ONLINE : SERVER_STATE_OFFLINE;

    char icmp[32];
    if (r->icmp_ok) {
        snprintf(icmp, sizeof(icmp), "ICMP %lu мс", (unsigned long)r->icmp_rtt_ms);
    } else {
        snprintf(icmp, sizeof(icmp), "ICMP нет ответа");
    }
    snprintf(r->detail, sizeof(r->detail), "%s · %s", icmp, r->http_detail);
}

static void set_state(const check_result_t *r)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    server_state_t prev = s_status.state;
    s_status.state = r->state;
    strlcpy(s_status.state_detail, r->detail, sizeof(s_status.state_detail));
    s_status.checked_at_ms = now_ms();
    s_status.icmp_ok = r->icmp_ok;
    s_status.icmp_rtt_ms = r->icmp_rtt_ms;
    s_status.http_enabled = r->http_enabled;
    s_status.http_ok = r->http_ok;
    strlcpy(s_status.http_detail, r->http_detail, sizeof(s_status.http_detail));
    xSemaphoreGive(s_lock);

    if (r->state != prev) {
        ESP_LOGI(TAG, "Server %s -> %s (%s)", server_state_name(prev), server_state_name(r->state), r->detail);
    }
}

static void finish_command(server_cmd_t cmd, bool ok, const char *detail)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pending > 0) {
        s_pending--;
    }
    s_status.cmd = cmd;
    s_status.cmd_ok = ok;
    s_status.cmd_busy = s_pending > 0;
    strlcpy(s_status.cmd_detail, detail, sizeof(s_status.cmd_detail));
    s_status.cmd_at_ms = now_ms();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Command %s: %s (%s)", server_cmd_name(cmd), ok ? "ok" : "failed", detail);
}

static void worker_task(void *arg)
{
    TickType_t last_check = 0;
    bool checked = false;  // первая проверка сразу после старта

    for (;;) {
        app_config_t cfg;
        app_config_get(&cfg);
        uint32_t interval_ms = ethernet_manager_is_ready() ? cfg.ping_interval_s * 1000U : NO_NETWORK_RETRY_MS;
        TickType_t interval = pdMS_TO_TICKS(interval_ms);
        TickType_t elapsed = xTaskGetTickCount() - last_check;
        bool due = !checked || elapsed >= interval;
        // Ждём не дольше секунды, чтобы новый интервал из веб-интерфейса применился сразу
        TickType_t wait = due ? 0 : interval - elapsed;
        if (wait > pdMS_TO_TICKS(1000)) {
            wait = pdMS_TO_TICKS(1000);
        }

        // SERVER_CMD_NONE — плановая (по таймауту) или внеочередная проверка
        server_cmd_t cmd = SERVER_CMD_NONE;
        if (xQueueReceive(s_queue, &cmd, wait) != pdTRUE && !due) {
            continue;
        }
        app_config_get(&cfg);  // настройки могли измениться за время ожидания

        if (cmd == SERVER_CMD_NONE) {
            check_result_t result;
            check_server(&cfg, &result);
            set_state(&result);
            last_check = xTaskGetTickCount();
            checked = true;
            continue;
        }

        char detail[SERVER_DETAIL_MAX];
        bool ok = false;
        if (!ethernet_manager_is_ready()) {
            snprintf(detail, sizeof(detail), "нет сети");
        } else if (cmd == SERVER_CMD_WOL) {
            ok = send_wol(&cfg, detail, sizeof(detail));
        } else if (cmd == SERVER_CMD_POWEROFF) {
            ok = http_get(&cfg, cfg.poweroff_path, detail, sizeof(detail));
        }
        finish_command(cmd, ok, detail);
    }
}

esp_err_t server_client_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_LENGTH, sizeof(server_cmd_t));
    if (!s_lock || !s_queue) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_status.state_detail, sizeof(s_status.state_detail), "проверка...");
    if (xTaskCreate(worker_task, "server_client", WORKER_STACK_SIZE, NULL, WORKER_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool server_client_request(server_cmd_t cmd)
{
    if (!s_queue || cmd == SERVER_CMD_NONE) {
        return false;
    }
    // Счётчик увеличиваем до постановки в очередь, иначе задача может завершить команду раньше
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pending++;
    s_status.cmd_busy = true;
    xSemaphoreGive(s_lock);

    if (xQueueSend(s_queue, &cmd, 0) == pdTRUE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.pending_cmd = cmd;
        xSemaphoreGive(s_lock);
        return true;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pending--;
    s_status.cmd_busy = s_pending > 0;
    xSemaphoreGive(s_lock);
    return false;
}

void server_client_recheck(void)
{
    if (!s_queue) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = SERVER_STATE_UNKNOWN;
    snprintf(s_status.state_detail, sizeof(s_status.state_detail), "проверка...");
    s_status.checked_at_ms = 0;
    xSemaphoreGive(s_lock);

    server_cmd_t cmd = SERVER_CMD_NONE;
    xQueueSend(s_queue, &cmd, 0);
}

server_state_t server_client_get_state(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    server_state_t state = s_status.state;
    xSemaphoreGive(s_lock);
    return state;
}

void server_client_get_status(server_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}
