#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

#define OUTPUT_COUNT 2  // IO32, IO33

typedef struct {
    bool enabled;                           // управление по питанию включено (outputs_enabled)
    bool high[OUTPUT_COUNT];                // уровень на выводе, прочитанный обратно
    uint32_t change_in_ms[OUTPUT_COUNT];    // до переключения, 0 — переключение не ожидается
} output_status_t;

// Выходы IO32/IO33 сразу в HIGH («включено»).
void output_manager_init(void);

// Вызывается из главного цикла: power_failure — уровень IO35, stable_ms — сколько он держится.
// Питание пропало и держится ioXX_off_s — выход в LOW, есть и держится ioXX_on_s — в HIGH.
// Когда управление выключено, выходы всегда HIGH.
void output_manager_update(const app_config_t *cfg, bool power_failure, int64_t stable_ms);

void output_manager_get_status(output_status_t *out);
