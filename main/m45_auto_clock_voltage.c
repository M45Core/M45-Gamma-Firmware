#include "m45_auto_clock_voltage.h"

#include <math.h>

m45_auto_clock_voltage_result_t m45_auto_clock_voltage_update(
    m45_auto_clock_voltage_state_t *state, float raw_vin, float average_vin,
    float stop_min_v, float watch_min_v, uint64_t now_ms)
{
    m45_auto_clock_voltage_result_t result = {
        .valid = isfinite(raw_vin) && raw_vin > 0.0f &&
                 isfinite(average_vin) && average_vin > 0.0f,
        .guard_v = fmaxf(stop_min_v + M45_AUTO_CLOCK_VIN_BUFFER_V, watch_min_v),
    };
    result.resume_v = result.guard_v + M45_AUTO_CLOCK_VIN_HYSTERESIS_V;
    result.vin = result.valid ? fminf(raw_vin, average_vin) : 0.0f;
    if (!result.valid) {
        state->limited = true;
        state->recovering = false;
    } else if (result.vin <= result.guard_v) {
        state->limited = true;
        state->recovering = false;
        if (now_ms >= state->next_backoff_ms) {
            result.backoff = true;
            state->next_backoff_ms = now_ms + M45_AUTO_CLOCK_VIN_BACKOFF_MS;
        }
    } else if (state->limited) {
        if (result.vin >= result.resume_v) {
            if (!state->recovering) {
                state->recovering = true;
                state->recovery_since_ms = now_ms;
            }
            if (now_ms - state->recovery_since_ms >= M45_AUTO_CLOCK_VIN_RECOVERY_MS) {
                state->limited = false;
                state->recovering = false;
            }
        } else {
            state->recovering = false;
        }
    }
    result.limited = state->limited;
    return result;
}
