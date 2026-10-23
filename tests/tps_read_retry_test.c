#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "tps546.h"
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_STATE 0x103
#define M45_FAULT_MESSAGE_MAX 1023
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGW(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
static const char *TAG = "test";
typedef struct {
    struct { bool hardware_fault; } SYSTEM_MODULE;
    struct { float core_voltage; } POWER_MANAGEMENT_MODULE;
} GlobalState;
typedef struct { uint16_t safety_asic_voltage_min_mv, safety_asic_voltage_max_mv; } m45_config_t;
typedef struct {
    float input_voltage_min_v, input_voltage_max_v, asic_voltage_min_v, asic_voltage_max_v;
    float iout_warn_a, iout_fault_a, tps546_temp_max_c;
} bitaxe_gamma602_safety_limits_t;
static const m45_config_t config = {700, 1400};
static bool g_tps546_ready = true, g_regulator_enabled = true;
static uint16_t g_commanded_voltage_mv;
static unsigned read_calls, write_calls, records, delay_calls, remembered, stored;
static unsigned read_duration_ms;
static int failures_left;
static int64_t now_us;
static esp_err_t write_result;
static TPS546_StatusSnapshot sample;
static char last_reason[1024], hardware_status[64];
static const m45_config_t *m45_config_get(void) { return &config; }
static void set_hw_status(const char *s) { snprintf(hardware_status, sizeof(hardware_status), "%s", s); }
const char *esp_err_to_name(esp_err_t e) { return e == ESP_ERR_TIMEOUT ? "ESP_ERR_TIMEOUT" : "ESP_FAIL"; }
static int64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelay(unsigned ticks) {
    assert(g_regulator_enabled); // Retrying a read must not turn off output.
    assert(!write_calls || write_calls == 1); // A runtime tune writes once.
    delay_calls++;
    now_us += (int64_t)ticks * 1000;
}
static void remember_fault_power_sample(const TPS546_StatusSnapshot *s) { (void)s; remembered++; }
static void store_power_snapshot(const TPS546_StatusSnapshot *s) { (void)s; stored++; }
static void format_fault_context(GlobalState *s, const char *reason, char *dst, size_t size) {
    (void)s;
    char copy[1024]; snprintf(copy, sizeof(copy), "%s", reason);
    snprintf(dst, size, "%.850s; actual 700MHz, ASIC 55C", copy);
}
static esp_err_t m45_fault_log_record(const char *message) {
    records++; snprintf(last_reason, sizeof(last_reason), "%s", message); return ESP_OK;
}
static void bitaxe_gamma602_safety_limits(bitaxe_gamma602_safety_limits_t *l) {
    *l = (bitaxe_gamma602_safety_limits_t){4.8f, 5.5f, .7f, 1.4f, 30, 33, 110};
}
static esp_err_t fail_regulator_safety(GlobalState *s, const TPS546_StatusSnapshot *snapshot, const char *fmt, ...) {
    (void)snapshot; va_list args; va_start(args, fmt);
    vsnprintf(last_reason, sizeof(last_reason), fmt, args); va_end(args);
    s->SYSTEM_MODULE.hardware_fault = true; g_regulator_enabled = false; return ESP_FAIL;
}
static esp_err_t shutdown_and_return(GlobalState *s, const char *reason, esp_err_t err) {
    (void)s; snprintf(last_reason, sizeof(last_reason), "%s", reason); g_regulator_enabled = false; return err;
}
esp_err_t TPS546_snapshot_status(TPS546_StatusSnapshot *out) {
    read_calls++; now_us += (int64_t)read_duration_ms * 1000;
    *out = sample;
    if (failures_left != 0) {
        if (failures_left > 0) failures_left--;
        return out->read_error;
    }
    *out = (TPS546_StatusSnapshot){.valid=TPS546_SNAPSHOT_ALL_VALID,.operation=OPERATION_ON,
        .read_vin=5.1f,.read_vout=1.15f,.read_iout=20,.read_temp1=70};
    return ESP_OK;
}
esp_err_t TPS546_set_vout(float volts) { (void)volts; write_calls++; return write_result; }
static esp_err_t wait_for_regulator_after_enable(GlobalState *s, float volts);
/* POWER_VALIDATION_UNDER_TEST */
static esp_err_t wait_for_regulator_after_enable(GlobalState *s, float volts) {
    (void)volts; TPS546_StatusSnapshot out; return capture_power_snapshot(s, &out, true);
}
static void reset(void) {
    g_regulator_enabled = true; write_calls=read_calls=records=delay_calls=remembered=stored=0;
    failures_left=0; now_us=0; read_duration_ms=0; write_result=ESP_OK;
    sample=(TPS546_StatusSnapshot){.read_error=ESP_ERR_TIMEOUT,.failed_command=0x79};
    hardware_status[0]=last_reason[0]='\0';
}
static void expect_safety(TPS546_StatusSnapshot unsafe, bool startup, const char *reason) {
    reset(); sample=unsafe; failures_left=-1; GlobalState state={0}; TPS546_StatusSnapshot out;
    assert(capture_power_snapshot(&state,&out,startup)!=ESP_OK);
    assert(state.SYSTEM_MODULE.hardware_fault && !g_regulator_enabled);
    assert(read_calls==1 && delay_calls==0 && write_calls==0 && records==0);
    assert(strstr(last_reason,reason));
}
int main(void) {
    GlobalState state={0}; TPS546_StatusSnapshot out;
    reset(); failures_left=1;
    assert(capture_power_snapshot(&state,&out,false)==ESP_OK);
    assert(read_calls==2 && delay_calls==1 && records==2 && stored==1 && remembered==1);
    assert(g_regulator_enabled && !state.SYSTEM_MODULE.hardware_fault && write_calls==0);
    assert(strstr(last_reason,"recovered") && strstr(last_reason,"actual 700MHz"));
    reset(); failures_left=-1;
    assert(capture_power_snapshot(&state,&out,false)!=ESP_OK);
    assert(now_us==5000000 && read_calls==50 && records==1 && write_calls==0);
    assert(state.SYSTEM_MODULE.hardware_fault && strstr(last_reason,"5000ms"));
    state=(GlobalState){0}; reset(); failures_left=-1; read_duration_ms=1500;
    assert(capture_power_snapshot(&state,&out,true)!=ESP_OK);
    assert(now_us==6300000 && read_calls==4); // Already-started I2C call can finish after deadline.
    TPS546_StatusSnapshot unsafe={.read_error=ESP_ERR_TIMEOUT,.valid=1U<<TPS546_FIELD_STATUS,
        .status_word=TPS546_STATUS_IOUT_OC};
    expect_safety(unsafe,false,"status fault"); expect_safety(unsafe,true,"status fault");
    unsafe=(TPS546_StatusSnapshot){.read_error=ESP_ERR_TIMEOUT,.valid=1U<<TPS546_FIELD_INPUT_STATUS | 1U<<TPS546_FIELD_STATUS,
        .status_word=TPS546_STATUS_INPUT,.st_input=TPS546_STATUS_VIN_OVF};
    expect_safety(unsafe,false,"VIN overvoltage");
    unsafe=(TPS546_StatusSnapshot){.read_error=ESP_ERR_TIMEOUT,.valid=1U<<TPS546_FIELD_VIN,.read_vin=4.7f};
    expect_safety(unsafe,false,"VIN 4.700V <"); expect_safety(unsafe,true,"VIN 4.700V <");
    unsafe.read_vin=5.5f; expect_safety(unsafe,false,"VIN 5.500V >=");
    unsafe=(TPS546_StatusSnapshot){.read_error=ESP_ERR_TIMEOUT,.valid=1U<<TPS546_FIELD_VOUT,.read_vout=1.4f};
    expect_safety(unsafe,false,"VOUT 1.400V >="); expect_safety(unsafe,true,"VOUT 1.400V >=");
    unsafe=(TPS546_StatusSnapshot){.read_error=ESP_ERR_TIMEOUT,.valid=1U<<TPS546_FIELD_IOUT,.read_iout=33};
    expect_safety(unsafe,false,"IOUT 33.000A >=");
    unsafe=(TPS546_StatusSnapshot){.read_error=ESP_ERR_TIMEOUT,.valid=1U<<TPS546_FIELD_TEMP,.read_temp1=110};
    expect_safety(unsafe,false,"temperature 110C");
    // Unread fields can contain garbage but never become measurements.
    reset(); state=(GlobalState){0}; failures_left=1; sample=unsafe; sample.valid=0;
    assert(capture_power_snapshot(&state,&out,false)==ESP_OK && !state.SYSTEM_MODULE.hardware_fault);
    reset(); sample.read_error=ESP_ERR_INVALID_ARG; failures_left=-1;
    assert(capture_power_snapshot(&state,&out,false)!=ESP_OK && read_calls==1 && delay_calls==0);
    // Runtime voltage sampling retries the read without repeating the write or restarting ASIC.
    reset(); state=(GlobalState){0}; failures_left=1;
    assert(bitaxe_gamma602_set_voltage_mv_for_config_unlocked(&state,1200,&config)==ESP_OK);
    assert(write_calls==1 && read_calls==2 && g_commanded_voltage_mv==1200 && g_regulator_enabled);
    reset(); write_result=ESP_ERR_TIMEOUT;
    assert(bitaxe_gamma602_set_voltage_mv_for_config_unlocked(&state,1200,&config)==ESP_ERR_TIMEOUT);
    assert(write_calls==1 && read_calls==0 && !g_regulator_enabled);
    reset(); write_result=ESP_ERR_INVALID_STATE;
    assert(bitaxe_gamma602_set_voltage_mv_for_config_unlocked(&state,1200,&config)==ESP_FAIL);
    assert(state.SYSTEM_MODULE.hardware_fault && read_calls==1 && write_calls==1);
    puts("TPS read-only grace, immediate safety faults, and single-write voltage tuning tests passed.");
}
