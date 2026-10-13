#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MAINS_UNKNOWN,          // после старта, пока уровень IO35 не подтверждён
    MAINS_PRESENT,          // IO35 LOW — питание квартиры есть
    MAINS_LOST,             // IO35 HIGH — питание квартиры отключено
} mains_state_t;

// Каждый сценарий — шаг «выключение» (poweroff, ожидание OFFLINE) или «включение» (WOL, ожидание ONLINE).
// Шаг отправляет команду до attempts раз, после каждой ждёт off_wait_s / on_wait_s (app_config_t).
// Параметры шагов общие для автоматики и ручного перезапуска.
typedef enum {
    SCENARIO_NONE,
    SCENARIO_POWER_ON,      // питание появилось: шаг «включение»
    SCENARIO_POWER_OFF,     // питание пропало: шаг «выключение»
    SCENARIO_MANUAL_OFF,    // кнопка IO36, шаг 1: «выключение», затем всегда шаг 2
    SCENARIO_MANUAL_ON,     // кнопка IO36, шаг 2: «включение»
} scenario_t;

typedef enum {
    SCENARIO_RESULT_NONE,
    SCENARIO_RESULT_DONE,       // сервер перешёл в целевое состояние
    SCENARIO_RESULT_FAILED,     // все попытки отправлены, сервер не перешёл в целевое состояние
    SCENARIO_RESULT_ABORTED,    // запущен другой сценарий (изменилось питание или нажата кнопка)
    SCENARIO_RESULT_TIMEOUT,    // MANUAL_OFF: сервер не стал недоступен, переход к шагу 2 (WOL)
} scenario_result_t;

typedef struct {
    bool power_failure;             // IO35 сейчас
    bool button_pressed;            // IO36 с антидребезгом
    uint32_t hold_left_ms;          // кнопка удерживается: до запуска ручного сценария, иначе 0
    mains_state_t mains;            // подтверждённое состояние питания
    uint32_t confirm_left_ms;       // до подтверждения нового уровня IO35, 0 — подтверждать нечего

    scenario_t scenario;            // выполняемый сценарий
    int attempts;                   // отправлено команд в выполняемом сценарии
    int max_attempts;               // attempts из настроек
    uint32_t next_step_ms;          // до повтора команды или до завершения шага
    bool waiting_shutdown;          // шаг «включение» ждёт, пока сервер доделает выключение

    scenario_t last_scenario;       // последний завершённый сценарий
    scenario_result_t last_result;
    int last_attempts;
    int64_t last_at_ms;             // время завершения (uptime), 0 — сценариев не было
} power_status_t;

void power_manager_init(void);

// Вызывается из главного цикла: подтверждение уровня IO35, кнопка IO36 и сценарии.
void power_manager_update(void);

// Сценарий включает сервер (зелёная индикация) или выключает (красная).
bool scenario_is_power_on(scenario_t scenario);

// Потокобезопасный снимок состояния.
void power_manager_get_status(power_status_t *out);

#ifdef APP_TEST_HOOKS
// Тестовая сборка: подмена входов через HTTP. -1 — читать GPIO, 0/1 — принудительный уровень.
typedef struct {
    int power_failure;
    int button;
} power_test_inputs_t;

void power_manager_test_get(power_test_inputs_t *out);
void power_manager_test_set(const power_test_inputs_t *in);
#endif

const char *mains_state_name(mains_state_t mains);
const char *scenario_name(scenario_t scenario);
const char *scenario_result_name(scenario_result_t result);
