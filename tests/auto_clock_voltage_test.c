#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "m45_config.h"
#include "m45_auto_clock_voltage.h"
#include "tps546.h"
#define CONFIG_M45_BITAXE_ASIC_FREQUENCY_MHZ 525
#define CONFIG_M45_BITAXE_ASIC_VOLTAGE_MV 1150
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
typedef unsigned TickType_t;
typedef struct {
    struct {float actual_frequency;} POWER_MANAGEMENT_MODULE;
    struct {bool hardware_fault;} SYSTEM_MODULE;
} GlobalState;
static TickType_t tick, g_auto_clock_next_tick;
static uint8_t g_auto_clock_up_dwell_ticks, g_auto_clock_down_dwell_ticks;
static uint16_t g_commanded_voltage_mv;
static m45_config_t config;
static m45_auto_clock_voltage_state_t g_auto_clock_voltage;
static struct {bool input_voltage_limited; char hold_reason[96];} g_auto_clock_status;
static bool startup_ready;
static unsigned calls;
static size_t strlcpy(char *dest, const char *src, size_t size) {
    snprintf(dest, size, "%s", src); return strlen(src);
}
const m45_config_t *m45_config_get(void) {return &config;}
static TickType_t xTaskGetTickCount(void) {return tick;}
static int64_t esp_timer_get_time(void) {return (int64_t)tick * 1000;}
static bool auto_clock_startup_gate_ready(const char **reason) {return startup_ready;}
static void bitaxe_fan_force_max_if_allowed(GlobalState *s, const char *r) {}
static esp_err_t auto_clock_apply_preset(GlobalState *s, const m45_config_t *c,
                                        uint16_t f, uint16_t b, uint16_t v) {
    if (g_auto_clock_voltage.limited) {
        assert(f <= s->POWER_MANAGEMENT_MODULE.actual_frequency);
        assert(v <= g_commanded_voltage_mv);
    }
    s->POWER_MANAGEMENT_MODULE.actual_frequency = f;
    g_commanded_voltage_mv = v;
    calls++;
    return ESP_OK;
}
/* CONTROLLER_UNDER_TEST */
static GlobalState state;
static TPS546_StatusSnapshot averaged, raw;
static void reset(unsigned frequency, unsigned millivolts) {
    config = (m45_config_t){.overclock_enabled=true, .auto_clock_enabled=true,
        .fan_override_enabled=true, .auto_clock_target_temp_c=62, .auto_clock_max_watts=40,
        .safety_asic_voltage_min_mv=700, .safety_asic_voltage_max_mv=1400,
        .safety_asic_temp_max_c=69, .safety_tps546_temp_expected_max_c=90,
        .safety_tps546_temp_max_c=110, .safety_iout_warn_deciamps=290,
        .safety_iout_fault_deciamps=330, .safety_input_voltage_min_mv=4800,
        .safety_input_voltage_expected_min_mv=4950};
    state = (GlobalState){.POWER_MANAGEMENT_MODULE.actual_frequency=frequency};
    averaged = (TPS546_StatusSnapshot){.read_vout=millivolts/1000.0f, .read_iout=20,
        .read_vin=5.15f, .read_temp1=70};
    raw = averaged;
    g_commanded_voltage_mv = millivolts;
    g_auto_clock_voltage = (m45_auto_clock_voltage_state_t){0};
    memset(&g_auto_clock_status, 0, sizeof(g_auto_clock_status));
    g_auto_clock_next_tick = 0;
    g_auto_clock_up_dwell_ticks = g_auto_clock_down_dwell_ticks = 0;
    tick = 100;
    calls = 0;
    startup_ready = true;
}
static void step(unsigned now, float temp) {
    tick = now;
    averaged.read_vout = g_commanded_voltage_mv / 1000.0f;
    assert(apply_auto_clock_control(&state, temp, temp, &averaged, &raw) == ESP_OK);
}
static void guard_tests(void) {
    m45_auto_clock_voltage_state_t s = {0};
    m45_auto_clock_voltage_result_t r;
    r = m45_auto_clock_voltage_update(&s, 5.15f, 5.15f, 4.8f, 4.95f, 0);
    assert(!r.limited && fabsf(r.guard_v - 5.0f) < 0.001f);
    r = m45_auto_clock_voltage_update(&s, 5.0f, 5.15f, 4.8f, 4.95f, 100);
    assert(r.limited && r.backoff);
    r = m45_auto_clock_voltage_update(&s, 4.99f, 5.15f, 4.8f, 4.95f, 350);
    assert(r.limited && !r.backoff);
    r = m45_auto_clock_voltage_update(&s, 5.15f, 5.0f, 4.8f, 4.95f, 1100);
    assert(r.limited && r.backoff);
    r = m45_auto_clock_voltage_update(&s, 5.05f, 5.05f, 4.8f, 4.95f, 2000);
    assert(r.limited && !r.backoff);
    r = m45_auto_clock_voltage_update(&s, 5.05f, 5.05f, 4.8f, 4.95f, 6999);
    assert(r.limited);
    r = m45_auto_clock_voltage_update(&s, 5.04f, 5.15f, 4.8f, 4.95f, 7000);
    assert(r.limited && !s.recovering);
    m45_auto_clock_voltage_update(&s, 5.05f, 5.05f, 4.8f, 4.95f, 8000);
    r = m45_auto_clock_voltage_update(&s, 5.05f, 5.05f, 4.8f, 4.95f, 13000);
    assert(!r.limited);
    r = m45_auto_clock_voltage_update(&s, 5.19f, 5.30f, 5.0f, 4.95f, 14000);
    assert(r.limited && fabsf(r.guard_v - 5.2f) < 0.001f);
    r = m45_auto_clock_voltage_update(&s, 5.29f, 5.30f, 4.8f, 5.3f, 15000);
    assert(r.limited && fabsf(r.guard_v - 5.3f) < 0.001f);
    const float invalid[] = {NAN, INFINITY, 0, -1};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        r = m45_auto_clock_voltage_update(&s, invalid[i], 5.15f, 4.8f, 4.95f, 16000);
        assert(!r.valid && r.limited && !r.backoff);
        r = m45_auto_clock_voltage_update(&s, 5.15f, invalid[i], 4.8f, 4.95f, 16000);
        assert(!r.valid && r.limited && !r.backoff);
    }
}
int main(void) {
    guard_tests();
    reset(850, 1150);
    for (unsigned t = 100; t <= 10100; t += 5000) step(t, 55);
    assert(calls > 0 && state.POWER_MANAGEMENT_MODULE.actual_frequency > 850);
    assert(!g_auto_clock_status.input_voltage_limited);

    reset(850, 1150);
    g_auto_clock_next_tick = 5100;
    raw.read_vin = 4.99f; // Fresh sag must bypass averaging and the five-second tick.
    step(100, 55);
    assert(state.POWER_MANAGEMENT_MODULE.actual_frequency == 775 && calls == 1);
    assert(g_auto_clock_status.input_voltage_limited);
    assert(strstr(g_auto_clock_status.hold_reason, "guard 5.000V"));
    step(350, 55);
    assert(calls == 1);
    step(1100, 55);
    assert(state.POWER_MANAGEMENT_MODULE.actual_frequency == 700 && calls == 2);
    raw.read_vin = averaged.read_vin = 5.02f;
    for (unsigned t = 2100; t < 20000; t += 1000) step(t, 55);
    assert(state.POWER_MANAGEMENT_MODULE.actual_frequency <= 700);
    assert(g_auto_clock_status.input_voltage_limited);
    raw.read_vin = averaged.read_vin = 5.05f;
    step(20100, 55);
    step(25099, 55);
    assert(g_auto_clock_status.input_voltage_limited);
    step(25100, 55);
    assert(!g_auto_clock_status.input_voltage_limited);
    assert(!strstr(g_auto_clock_status.hold_reason, "TPS VIN"));

    reset(600, 1065);
    raw.read_vin = 4.99f;
    step(100, 55);
    assert(state.POWER_MANAGEMENT_MODULE.actual_frequency == 500);
    assert(g_commanded_voltage_mv <= 1065); // Skip the 525 MHz / 1150 mV bump.

    reset(850, 1150);
    raw.read_vin = 4.99f;
    startup_ready = false;
    step(100, 62);
    assert(calls == 1 && g_auto_clock_status.input_voltage_limited);
    assert(strstr(g_auto_clock_status.hold_reason, "TPS VIN"));

    reset(850, 1150);
    config.safety_input_voltage_min_mv = 5000;
    raw.read_vin = averaged.read_vin = 5.19f;
    step(100, 55);
    assert(calls == 1 && strstr(g_auto_clock_status.hold_reason, "guard 5.200V"));

    reset(850, 1150);
    raw.read_vin = NAN;
    step(100, 55);
    assert(calls == 0 && g_auto_clock_status.input_voltage_limited);
    assert(strstr(g_auto_clock_status.hold_reason, "unavailable"));
    config.auto_clock_enabled = false;
    step(350, 55);
    assert(calls == 0 && !g_auto_clock_status.input_voltage_limited);

    reset(50, 870);
    raw.read_vin = 4.99f;
    step(100, 55);
    assert(state.POWER_MANAGEMENT_MODULE.actual_frequency == 50 && calls == 0);
    puts("Auto-clock voltage guard/controller tests passed");
}
