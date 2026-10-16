#pragma once

#include <stdbool.h>
#include <stdint.h>

#define M45_AUTO_CLOCK_VIN_BUFFER_V 0.20f
#define M45_AUTO_CLOCK_VIN_HYSTERESIS_V 0.05f
#define M45_AUTO_CLOCK_VIN_RECOVERY_MS 5000U
#define M45_AUTO_CLOCK_VIN_BACKOFF_MS 1000U

typedef struct {
    bool limited;
    bool recovering;
    uint64_t recovery_since_ms;
    uint64_t next_backoff_ms;
} m45_auto_clock_voltage_state_t;

typedef struct {
    bool valid;
    bool limited;
    bool backoff;
    float vin;
    float guard_v;
    float resume_v;
} m45_auto_clock_voltage_result_t;

m45_auto_clock_voltage_result_t m45_auto_clock_voltage_update(
    m45_auto_clock_voltage_state_t *state, float raw_vin, float average_vin,
    float stop_min_v, float watch_min_v, uint64_t now_ms);
