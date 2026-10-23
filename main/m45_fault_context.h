#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Space reserved by the formatter so a long cause cannot displace telemetry.
#define M45_FAULT_CONTEXT_BYTES 512
#define M45_FAULT_REASON_MIN_BYTES 256

typedef struct {
    bool power_valid;
    bool asic_temp_valid;
    bool operating_settings_valid;
    uint64_t power_sample_ms;
    uint64_t asic_temp_sample_ms;
    uint64_t operating_settings_sample_ms;
    float vin_v;
    float vout_v;
    float iout_a;
    float vr_temp_c;
    float asic_temp_c;
    float actual_frequency_mhz;
    float requested_frequency_mhz;
    uint16_t requested_voltage_mv;
    uint16_t commanded_voltage_mv;
} m45_fault_context_t;

// Failed/non-finite measurements leave the last complete sample and age intact.
void m45_fault_context_remember_power(m45_fault_context_t *context, uint64_t now_ms,
                                     float vin_v, float vout_v, float iout_a,
                                     float vr_temp_c);
void m45_fault_context_remember_asic_temp(m45_fault_context_t *context,
                                         uint64_t now_ms, float temp_c);
void m45_fault_context_remember_settings(m45_fault_context_t *context, uint64_t now_ms,
                                        float actual_mhz, float requested_mhz,
                                        uint16_t requested_mv, uint16_t commanded_mv);

// reason and dst may be the same buffer. Truncation is explicit, and a full-size
// fault buffer always reserves room for every context field.
void m45_fault_context_format(const m45_fault_context_t *context, uint64_t now_ms,
                              const char *reason, char *dst, size_t size);
