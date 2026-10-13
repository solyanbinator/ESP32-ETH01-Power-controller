#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SERVER_STATE_UNKNOWN,
    SERVER_STATE_ONLINE,
    SERVER_STATE_OFFLINE,
} server_state_t;

// Команды отправляют сценарии power_manager
typedef enum {
    SERVER_CMD_NONE,
    SERVER_CMD_WOL,         // Power ON
    SERVER_CMD_POWEROFF,    // Power OFF
} server_cmd_t;

#define SERVER_DETAIL_MAX 80

// Доступность сервера определяется двумя независимыми проверками:
// ICMP echo (работает ли ОС) и HTTP GET ping_path (работает ли агент, принимающий poweroff).
// ONLINE — ответила хотя бы одна, OFFLINE — не ответила ни одна,
// UNKNOWN — проверить нельзя (нет сети) или проверки ещё не было.
typedef struct {
    server_state_t state;
    char state_detail[SERVER_DETAIL_MAX];   // сводка: "ICMP 2 мс · HTTP 200", "нет сети"...
    int64_t checked_at_ms;                  // время последней проверки (uptime), 0 — ещё не было
    bool icmp_ok;
    uint32_t icmp_rtt_ms;
    bool http_enabled;                      // задан путь HTTP-проверки
    bool http_ok;
    char http_detail[40];                   // "HTTP 200", "HTTP: нет соединения"...

    // Результат отправки команды хранится отдельно от доступности сервера:
    // успешно отправленный WOL не означает, что сервер уже включился.
    server_cmd_t cmd;                       // последняя выполненная команда
    bool cmd_ok;
    char cmd_detail[SERVER_DETAIL_MAX];
    int64_t cmd_at_ms;
    bool cmd_busy;                          // есть команды в очереди или в работе
    server_cmd_t pending_cmd;               // последняя поставленная в очередь (актуальна при cmd_busy)
} server_status_t;

// Все сетевые обращения к серверу выполняет отдельная задача: проверка каждые
// ping_interval_s (настройки) и команды из очереди. Веб-сервер и главный цикл никогда не блокируются.
esp_err_t server_client_start(void);

// Ставит команду в очередь, не блокирует. false — очередь переполнена.
bool server_client_request(server_cmd_t cmd);

// Внеочередная проверка доступности (после смены адреса или пути), состояние сбрасывается в UNKNOWN.
void server_client_recheck(void);

server_state_t server_client_get_state(void);
void server_client_get_status(server_status_t *out);

#ifdef APP_TEST_HOOKS
// Тестовая сборка: не выполнять ICMP-проверку (ноутбук с эмулятором сам отвечает на ping).
void server_client_test_set_icmp(bool enabled);
bool server_client_test_icmp_enabled(void);
#endif

const char *server_state_name(server_state_t state);
const char *server_cmd_name(server_cmd_t cmd);