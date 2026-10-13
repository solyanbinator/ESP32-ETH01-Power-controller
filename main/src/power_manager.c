#include "power_manager.h"

#include "app_config.h"
#include "app_defs.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "output_manager.h"
#include "server_client.h"

static const char *TAG = "power";

// Автомат работает только в главном цикле; другие задачи читают снимок s_public.
static power_status_t s_state;
static power_status_t s_public;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool s_raw_failure;          // последний прочитанный уровень IO35
static int64_t s_raw_since_ms;      // с какого момента уровень не менялся

static bool s_button_raw;           // последний прочитанный уровень IO36
static int64_t s_button_raw_since_ms;
static bool s_button_fired;         // по текущему нажатию сценарий уже запущен

static int64_t s_step_started_ms;   // когда начался выполняемый шаг
static int64_t s_last_send_ms;      // когда сценарий отправил последнюю команду
// Серверу отправлен Power OFF, но OFFLINE после этого ещё не наблюдался: он может
// ещё выключаться, и онлайн-состояние в шаге «включение» нельзя считать результатом.
static bool s_shutdown_pending;

#ifdef APP_TEST_HOOKS
static volatile int s_test_power_failure = -1;
static volatile int s_test_button = -1;

void power_manager_test_get(power_test_inputs_t *out)
{
    out->power_failure = s_test_power_failure;
    out->button = s_test_button;
}

void power_manager_test_set(const power_test_inputs_t *in)
{
    s_test_power_failure = in->power_failure;
    s_test_button = in->button;
    ESP_LOGW(TAG, "Test inputs: power_failure=%d button=%d", in->power_failure, in->button);
}
#endif

const char *mains_state_name(mains_state_t mains)
{
    switch (mains) {
    case MAINS_PRESENT:
        return "present";
    case MAINS_LOST:
        return "lost";
    default:
        return "unknown";
    }
}

const char *scenario_name(scenario_t scenario)
{
    switch (scenario) {
    case SCENARIO_POWER_ON:
        return "power_on";
    case SCENARIO_POWER_OFF:
        return "power_off";
    case SCENARIO_MANUAL_OFF:
        return "manual_off";
    case SCENARIO_MANUAL_ON:
        return "manual_on";
    default:
        return "none";
    }
}

const char *scenario_result_name(scenario_result_t result)
{
    switch (result) {
    case SCENARIO_RESULT_DONE:
        return "done";
    case SCENARIO_RESULT_FAILED:
        return "failed";
    case SCENARIO_RESULT_ABORTED:
        return "aborted";
    case SCENARIO_RESULT_TIMEOUT:
        return "timeout";
    default:
        return "none";
    }
}

bool scenario_is_power_on(scenario_t scenario)
{
    return scenario == SCENARIO_POWER_ON || scenario == SCENARIO_MANUAL_ON;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool read_power_failure(void)
{
#ifdef APP_TEST_HOOKS
    if (s_test_power_failure >= 0) {
        return s_test_power_failure;
    }
#endif
    return gpio_get_level(PIN_POWER_SENSE) == POWER_FAILURE_LEVEL;
}

static bool read_button(void)
{
#ifdef APP_TEST_HOOKS
    if (s_test_button >= 0) {
        return s_test_button;
    }
#endif
    return gpio_get_level(PIN_MANUAL_BUTTON) == BUTTON_ACTIVE_LEVEL;
}

void power_manager_init(void)
{
    // IO35 и IO36 — только входы, без внутренних подтяжек: уровень задаёт внешняя схема.
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_POWER_SENSE) | BIT64(PIN_MANUAL_BUTTON),
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    int64_t now = now_ms();
    // Состояние питания при старте неизвестно: через mains_lost_s / mains_restore_s стабильного
    // уровня оно подтвердится и запустит соответствующий сценарий (как после любого изменения).
    s_raw_failure = read_power_failure();
    s_raw_since_ms = now;
    s_state.mains = MAINS_UNKNOWN;

    // Кнопка, нажатая при старте, сценарий не запускает: нужно отпустить и нажать снова.
    s_button_raw = read_button();
    s_button_raw_since_ms = now;
    s_state.button_pressed = s_button_raw;
    s_button_fired = true;
}

static void finish_scenario(scenario_result_t result, int64_t now)
{
    ESP_LOGI(TAG, "Scenario %s: %s, commands sent: %d",
             scenario_name(s_state.scenario), scenario_result_name(result), s_state.attempts);
    s_state.last_scenario = s_state.scenario;
    s_state.last_result = result;
    s_state.last_attempts = s_state.attempts;
    s_state.last_at_ms = now;
    s_state.scenario = SCENARIO_NONE;
    s_state.attempts = 0;
    s_state.next_step_ms = 0;
    s_state.waiting_shutdown = false;
}

static void start_scenario(scenario_t scenario, int64_t now)
{
    if (s_state.scenario != SCENARIO_NONE) {
        finish_scenario(SCENARIO_RESULT_ABORTED, now);
    }
    s_state.scenario = scenario;
    s_state.attempts = 0;
    s_step_started_ms = now;
    ESP_LOGI(TAG, "Scenario %s started", scenario_name(scenario));
}

// Шаг сценария, одинаковый для автоматики и кнопки: команда до cfg->attempts раз,
// после каждой — ожидание целевого состояния (off_wait_s / on_wait_s). Как только сервер
// перешёл в целевое состояние, повторы прекращаются.
static void run_scenario(const app_config_t *cfg, int64_t now)
{
    scenario_t scenario = s_state.scenario;
    bool power_on = scenario_is_power_on(scenario);
    server_state_t target = power_on ? SERVER_STATE_ONLINE : SERVER_STATE_OFFLINE;
    int64_t wait_ms = (int64_t)(power_on ? cfg->on_wait_s : cfg->off_wait_s) * 1000;
    server_state_t state = server_client_get_state();
    int64_t since_send = now - s_last_send_ms;

    s_state.next_step_ms = 0;
    s_state.waiting_shutdown = false;
    if (state == SERVER_STATE_OFFLINE) {
        s_shutdown_pending = false;
    }
    // Шаг «включение» сразу после Power OFF (ручной перезапуск, вернувшееся питание):
    // сервер, который ещё отвечает, может доделывать выключение. Ждём on_wait_s: пропадёт —
    // отправляем WOL, останется онлайн — значит не выключался, шаг выполнен.
    if (power_on && s_shutdown_pending && state == SERVER_STATE_ONLINE) {
        int64_t waited = now - s_step_started_ms;
        if (waited < wait_ms) {
            s_state.waiting_shutdown = true;
            s_state.next_step_ms = (uint32_t)(wait_ms - waited);
            return;
        }
        s_shutdown_pending = false;
    }
    if (state == target) {
        finish_scenario(SCENARIO_RESULT_DONE, now);
        if (scenario == SCENARIO_MANUAL_OFF) {
            start_scenario(SCENARIO_MANUAL_ON, now);
        }
        return;
    }
    if (state == SERVER_STATE_UNKNOWN) {
        return;  // нет сети или идёт проверка: решаем только по известному состоянию
    }
    if (s_state.attempts > 0 && since_send < wait_ms) {
        s_state.next_step_ms = (uint32_t)(wait_ms - since_send);
        return;
    }
    if (s_state.attempts >= cfg->attempts) {
        if (scenario == SCENARIO_MANUAL_OFF) {
            finish_scenario(SCENARIO_RESULT_TIMEOUT, now);
            start_scenario(SCENARIO_MANUAL_ON, now);
        } else {
            finish_scenario(SCENARIO_RESULT_FAILED, now);
        }
        return;
    }
    if (server_client_request(power_on ? SERVER_CMD_WOL : SERVER_CMD_POWEROFF)) {
        s_state.attempts++;
        s_last_send_ms = now;
        if (!power_on) {
            s_shutdown_pending = true;
        }
        s_state.next_step_ms = (uint32_t)wait_ms;
        ESP_LOGI(TAG, "Scenario %s: command %d/%u sent", scenario_name(scenario), s_state.attempts, cfg->attempts);
    }
}

// Новый уровень IO35 принимается, только если продержался mains_lost_s (пропадание)
// или mains_restore_s (появление)
static void update_mains(const app_config_t *cfg, bool failure, int64_t now)
{
    if (failure != s_raw_failure) {
        s_raw_failure = failure;
        s_raw_since_ms = now;
    }
    mains_state_t level = failure ? MAINS_LOST : MAINS_PRESENT;
    int64_t confirm_ms = (int64_t)(failure ? cfg->mains_lost_s : cfg->mains_restore_s) * 1000;
    int64_t stable_ms = now - s_raw_since_ms;
    s_state.confirm_left_ms = 0;
    if (level == s_state.mains) {
        return;
    }
    if (stable_ms >= confirm_ms) {
        ESP_LOGI(TAG, "Mains %s confirmed", mains_state_name(level));
        s_state.mains = level;
        start_scenario(level == MAINS_PRESENT ? SCENARIO_POWER_ON : SCENARIO_POWER_OFF, now);
    } else {
        s_state.confirm_left_ms = (uint32_t)(confirm_ms - stable_ms);
    }
}

// Кнопка IO36: удержание BUTTON_HOLD_MS запускает ручной перезапуск, один раз на нажатие.
static void update_button(int64_t now)
{
    bool raw = read_button();
    if (raw != s_button_raw) {
        s_button_raw = raw;
        s_button_raw_since_ms = now;
    }
    if (raw != s_state.button_pressed && now - s_button_raw_since_ms >= BUTTON_DEBOUNCE_MS) {
        s_state.button_pressed = raw;
        if (raw) {
            s_button_fired = false;
        }
    }

    s_state.hold_left_ms = 0;
    if (!s_state.button_pressed || s_button_fired) {
        return;
    }
    int64_t held = now - s_button_raw_since_ms;
    if (held >= BUTTON_HOLD_MS) {
        s_button_fired = true;
        ESP_LOGI(TAG, "Button held for %d ms", BUTTON_HOLD_MS);
        start_scenario(SCENARIO_MANUAL_OFF, now);
    } else {
        s_state.hold_left_ms = (uint32_t)(BUTTON_HOLD_MS - held);
    }
}

void power_manager_update(void)
{
    // Настройки читаются каждый проход: изменения из веб-интерфейса применяются сразу
    app_config_t cfg;
    app_config_get(&cfg);
    int64_t now = now_ms();
    bool failure = read_power_failure();

    update_mains(&cfg, failure, now);
    output_manager_update(&cfg, failure, now - s_raw_since_ms);
    update_button(now);

    if (s_state.scenario != SCENARIO_NONE) {
        run_scenario(&cfg, now);
    }

    s_state.power_failure = failure;
    s_state.max_attempts = cfg.attempts;
    taskENTER_CRITICAL(&s_mux);
    s_public = s_state;
    taskEXIT_CRITICAL(&s_mux);
}

void power_manager_get_status(power_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_public;
    taskEXIT_CRITICAL(&s_mux);
}
