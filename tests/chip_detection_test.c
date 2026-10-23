#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ESP_LOGE(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#define ESP_LOG_BUFFER_HEX(tag, ...) ((void)(tag))

// Compile the real parser; only UART, time, and CRC-device responses are faked.
#include "../main/bitmain_asic.c"

enum response_mode { BAD_PREAMBLE, BAD_ID, BAD_CRC, NOISE_THEN_CHIP, VALID, SILENT };
static enum response_mode mode;
static unsigned reads, queries, first_events, recovered_events;
static int64_t now_us, start_us;
void vTaskDelay(TickType_t ticks) { now_us += (int64_t)ticks * 1000; }
static void request_read(void *context) { assert(context == &reads); queries++; }
static void read_event(void *context, bool recovered) { assert(context == &reads); if (recovered) recovered_events++; else first_events++; }
static int detect(unsigned expected, unsigned length) { return count_asic_chips(expected, 0x1370, length, request_read, read_event, &reads); }

int64_t esp_timer_get_time(void) { return now_us; }
void SERIAL_clear_buffer(void) {}
uint8_t crc5(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return mode == BAD_CRC ? 1 : 0;
}

int16_t SERIAL_rx(uint8_t *buffer, uint16_t size, uint16_t timeout_ms)
{
    assert(size == 11);
    assert(now_us - start_us < 5000000);
    assert(timeout_ms <= 1000);
    assert(timeout_ms <= (5000000 - (now_us - start_us) + 999) / 1000);
    assert(++reads <= 16); // Old malformed-packet loop would never finish.
    if (mode == SILENT) {
        now_us += (int64_t)timeout_ms * 1000;
        return 0;
    }
    now_us += timeout_ms < 250 ? (int64_t)timeout_ms * 1000 : 250000;
    memset(buffer, 0, size);
    buffer[0] = 0xaa;
    buffer[1] = 0x55;
    buffer[2] = 0x13;
    buffer[3] = 0x70;
    if (mode == BAD_PREAMBLE || (mode == NOISE_THEN_CHIP && reads < 3)) {
        buffer[0] = 0;
    } else if (mode == BAD_ID) {
        buffer[3] = 0x68;
    }
    return (int16_t)size;
}

static void reset(enum response_mode selected)
{
    mode = selected;
    reads = queries = first_events = recovered_events = 0;
    now_us = start_us = INT64_C(500000000000);
}

int main(void)
{
    const enum response_mode noise[] = {BAD_PREAMBLE, BAD_ID, BAD_CRC};
    for (unsigned i = 0; i < sizeof(noise) / sizeof(noise[0]); ++i) {
        reset(noise[i]);
        assert(detect(1, 11) == 0);
        assert(reads > 4 && queries == reads && now_us - start_us == 5000000);
        assert(first_events == 1 && recovered_events == 0);
    }
    reset(NOISE_THEN_CHIP);
    assert(detect(1, 11) == 1);
    assert(reads == 3 && queries == 3 && now_us - start_us == 950000);
    assert(first_events == 1 && recovered_events == 1);
    reset(VALID);
    assert(detect(2, 11) == 2);
    assert(reads == 2 && queries == 1 && first_events == 0);
    reset(SILENT);
    assert(detect(1, 11) == 0);
    assert(reads == 5 && queries == 5 && now_us - start_us == 5000000);
    assert(first_events == 1 && recovered_events == 0);
    reset(VALID);
    assert(detect(1, 0) == 0);
    assert(detect(1, 17) == 0);
    assert(reads == 0);
    puts("ASIC five-second read-only discovery and noisy UART retry tests passed.");
    return 0;
}
