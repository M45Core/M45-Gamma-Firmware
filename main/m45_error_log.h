#pragma once

#include <stddef.h>
#include <stdint.h>

#define M45_ERROR_LOG_CAPACITY 64
#define M45_ERROR_MESSAGE_SIZE 2048

typedef struct {
    uint64_t epoch_seconds;
    uint32_t uptime_seconds;
    char message[M45_ERROR_MESSAGE_SIZE];
} m45_error_entry_t;

// RAM-only history: never changes persistent hardware fault history.
void m45_error_log_append(const char *message, int formatted_length);
size_t m45_error_log_snapshot(m45_error_entry_t *entries, size_t capacity,
                              uint32_t *dropped, uint32_t *truncated);
