#include "bitmain_asic.h"

#include <math.h>
#include <stdbool.h>

#include "asic_crc.h"
#include "asic_serial.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BITMAIN_RESPONSE_PREAMBLE 0xaa55u
#define BITMAIN_CHIP_ID_TIMEOUT_MS 1000
#define BITMAIN_CHIP_ID_RETRY_WINDOW_MS 5000
#define BITMAIN_CHIP_ID_RETRY_DELAY_MS 100
#define BITMAIN_WORK_TIMEOUT_MS 10000
#define BITMAIN_MAX_CHIP_ID_RESPONSE_BYTES 16

static const char *TAG = "bitmain_asic";

unsigned char _reverse_bits(unsigned char num)
{
    unsigned char reversed = 0;
    for (int i = 0; i < 8; ++i) {
        reversed <<= 1;
        reversed |= num & 1u;
        num >>= 1;
    }
    return reversed;
}

int _largest_power_of_two(int num)
{
    int power = 0;
    while (num > 1) {
        num >>= 1;
        ++power;
    }
    return 1 << power;
}

int _next_power_of_two(int num)
{
    if (num <= 1) {
        return 1;
    }

    int power = 1;
    while (power < num) {
        power <<= 1;
    }
    return power;
}

static bool validate_preamble(const uint8_t *buffer)
{
    const uint16_t received_preamble = ((uint16_t)buffer[0] << 8) | buffer[1];
    return received_preamble == BITMAIN_RESPONSE_PREAMBLE;
}

int count_asic_chips(uint16_t asic_count, uint16_t chip_id, int chip_id_response_length,
                      void (*request_read)(void *),
                      void (*read_event)(void *, bool), void *context)
{
    if (chip_id_response_length <= 0 ||
        chip_id_response_length > BITMAIN_MAX_CHIP_ID_RESPONSE_BYTES) {
        ESP_LOGE(TAG, "invalid CHIP_ID response length %d", chip_id_response_length);
        return 0;
    }

    uint8_t buffer[BITMAIN_MAX_CHIP_ID_RESPONSE_BYTES] = {0};
    int chip_counter = 0;
    bool retrying = false;
    const int64_t started_us = esp_timer_get_time();
    const int64_t timeout_us = (int64_t)BITMAIN_CHIP_ID_RETRY_WINDOW_MS * 1000;
    while (true) {
        const int64_t elapsed_us = esp_timer_get_time() - started_us;
        if (elapsed_us >= timeout_us) {
            break;
        }
        // Only repeat the chip-ID read command. Reset, power, baud and the
        // remaining initialization sequence are unchanged during this window.
        if (chip_counter == 0 && request_read != NULL) {
            request_read(context);
        }
        const int64_t remaining_ms = (timeout_us - elapsed_us + 999) / 1000;
        const uint16_t read_ms = remaining_ms < BITMAIN_CHIP_ID_TIMEOUT_MS
                                     ? (uint16_t)remaining_ms : BITMAIN_CHIP_ID_TIMEOUT_MS;
        const int received = SERIAL_rx(buffer, (uint16_t)chip_id_response_length, read_ms);
        bool valid = received == chip_id_response_length;
        if (valid) {
            const uint16_t received_chip_id = ((uint16_t)buffer[2] << 8) | buffer[3];
            valid = validate_preamble(buffer) && received_chip_id == chip_id &&
                    crc5(buffer + 2, (size_t)received - 2U) == 0;
        }
        if (valid) {
            ESP_LOGI(TAG, "chip %d detected: CORE_NUM=0x%02x ADDR=0x%02x", chip_counter,
                     buffer[4], buffer[5]);
            ++chip_counter;
            if (asic_count > 0 && chip_counter >= asic_count) {
                break;
            }
            continue;
        }
        if (!retrying) {
            ESP_LOGW(TAG, "CHIP_ID read failed or invalid (%d bytes); retrying reads up to %ums",
                     received, BITMAIN_CHIP_ID_RETRY_WINDOW_MS);
            if (read_event != NULL) {
                read_event(context, false);
            }
            retrying = true;
        }
        // Do not re-query a partially discovered chain and count its first
        // chip twice. Gamma602 has one chip, so its failures retry below.
        if (chip_counter > 0) {
            break;
        }
        const int64_t delay_remaining_us = timeout_us - (esp_timer_get_time() - started_us);
        if (delay_remaining_us <= 0) {
            break;
        }
        const int64_t delay_ms = (delay_remaining_us + 999) / 1000;
        vTaskDelay(pdMS_TO_TICKS(delay_ms < BITMAIN_CHIP_ID_RETRY_DELAY_MS
                                   ? (unsigned)delay_ms : BITMAIN_CHIP_ID_RETRY_DELAY_MS));
    }

    if (chip_counter != asic_count) {
        ESP_LOGW(TAG, "%d chip(s) detected on chain, expected %u", chip_counter, asic_count);
    } else if (retrying && read_event != NULL) {
        read_event(context, true);
    }
    return chip_counter;
}

esp_err_t receive_work(uint8_t *buffer, int buffer_size, uint64_t *out_timestamp_us)
{
    if (buffer == NULL || buffer_size <= 0 || buffer_size > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const int received = SERIAL_rx(buffer, (uint16_t)buffer_size, BITMAIN_WORK_TIMEOUT_MS);
    if (out_timestamp_us != NULL) {
        *out_timestamp_us = esp_timer_get_time();
    }

    if (received < 0) {
        ESP_LOGE(TAG, "UART error in serial RX");
        return ESP_FAIL;
    }

    if (received == 0) {
        ESP_LOGD(TAG, "UART timeout in serial RX");
        return ESP_FAIL;
    }

    if (received != buffer_size) {
        ESP_LOGE(TAG, "invalid response length %d", received);
        ESP_LOG_BUFFER_HEX(TAG, buffer, received);
        SERIAL_clear_buffer();
        return ESP_FAIL;
    }

    if (!validate_preamble(buffer)) {
        const uint16_t received_preamble = ((uint16_t)buffer[0] << 8) | buffer[1];
        ESP_LOGE(TAG, "preamble mismatch: got 0x%04x, expected 0x%04x", received_preamble,
                 BITMAIN_RESPONSE_PREAMBLE);
        ESP_LOG_BUFFER_HEX(TAG, buffer, received);
        SERIAL_clear_buffer();
        return ESP_FAIL;
    }

    if (crc5(buffer + 2, (size_t)buffer_size - 2U) != 0) {
        ESP_LOGE(TAG, "response checksum failed");
        ESP_LOG_BUFFER_HEX(TAG, buffer, received);
        SERIAL_clear_buffer();
        return ESP_FAIL;
    }

    return ESP_OK;
}

void get_difficulty_mask(double difficulty, uint8_t *job_difficulty_mask)
{
    if (job_difficulty_mask == NULL) {
        return;
    }

    uint32_t mask = UINT32_MAX;
    if (isfinite(difficulty) && difficulty > 0.0 && ceil(difficulty) < NONCE_SPACE) {
        uint32_t diff_int = (uint32_t)ceil(difficulty);
        int power = 0;
        while (diff_int > 1) {
            diff_int >>= 1;
            ++power;
        }
        mask = (1u << power) - 1u;
    }

    job_difficulty_mask[0] = 0x00;
    job_difficulty_mask[1] = 0x14;
    job_difficulty_mask[2] = _reverse_bits((mask >> 24) & 0xff);
    job_difficulty_mask[3] = _reverse_bits((mask >> 16) & 0xff);
    job_difficulty_mask[4] = _reverse_bits((mask >> 8) & 0xff);
    job_difficulty_mask[5] = _reverse_bits(mask & 0xff);
}

double calculate_bm_timeout_ms(float frequency_mhz, size_t asic_count, size_t small_cores,
                               size_t cores, size_t version_size, float timeout_percent,
                               double default_time_ms)
{
    if (asic_count == 0) {
        return default_time_ms;
    }

    const int cores_up = _next_power_of_two((int)cores);
    const int small_cores_up = _next_power_of_two((int)small_cores);
    const int asic_count_up = _next_power_of_two((int)asic_count);
    if (small_cores_up < cores_up || frequency_mhz <= 0.0f) {
        return default_time_ms;
    }

    const double midstates = (double)small_cores_up / (double)cores_up;
    const double serial_versions = (double)version_size / midstates;
    const double serial_nonces = NONCE_SPACE / (double)cores_up / (double)asic_count_up;
    const double fullspace_timeout_ms =
        serial_versions * serial_nonces / ((double)frequency_mhz * 1000.0);
    if (!(fullspace_timeout_ms > 0.0)) {
        return default_time_ms;
    }

    return (double)timeout_percent * fullspace_timeout_ms;
}
