#include "m45_error_log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static m45_error_entry_t *g_entries;
static size_t g_count;
static size_t g_next;
static uint32_t g_dropped;
static uint32_t g_truncated;

void m45_error_log_append(const char *message, int formatted_length)
{
    if (message == NULL || message[0] == '\0') {
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (g_entries == NULL) {
        g_entries = calloc(M45_ERROR_LOG_CAPACITY, sizeof(*g_entries));
    }
    if (g_entries == NULL) {
        ++g_dropped;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    m45_error_entry_t *entry = &g_entries[g_next];
    const time_t now = time(NULL);
    entry->epoch_seconds = now >= (time_t)1704067200 ? (uint64_t)now : 0;
    entry->uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    // Strip terminal ANSI colors while preserving the tag and log text.
    size_t out = 0;
    for (size_t i = 0; message[i] && out + 1 < sizeof(entry->message); ++i) {
        if (message[i] == '\033' && message[i + 1] == '[') {
            i += 2;
            while (message[i] && !((unsigned char)message[i] >= 0x40 &&
                                    (unsigned char)message[i] <= 0x7e)) ++i;
            if (!message[i]) break;
        } else {
            entry->message[out++] = message[i];
        }
    }
    entry->message[out] = '\0';
    if (formatted_length > (int)strlen(message) ||
        formatted_length >= M45_ERROR_MESSAGE_SIZE) {
        ++g_truncated;
        const char marker[] = " [message truncated]";
        size_t at = out > sizeof(marker) ? out - sizeof(marker) : 0;
        memcpy(entry->message + at, marker, sizeof(marker));
    }
    g_next = (g_next + 1) % M45_ERROR_LOG_CAPACITY;
    if (g_count < M45_ERROR_LOG_CAPACITY) ++g_count;
    else ++g_dropped;
    pthread_mutex_unlock(&g_lock);
}

size_t m45_error_log_snapshot(m45_error_entry_t *entries, size_t capacity,
                              uint32_t *dropped, uint32_t *truncated)
{
    pthread_mutex_lock(&g_lock);
    const size_t count = entries == NULL ? 0 : (g_count < capacity ? g_count : capacity);
    for (size_t i = 0; i < count; ++i) {
        entries[i] = g_entries[(g_next + M45_ERROR_LOG_CAPACITY - 1 - i) % M45_ERROR_LOG_CAPACITY];
    }
    if (dropped != NULL) *dropped = g_dropped;
    if (truncated != NULL) *truncated = g_truncated;
    pthread_mutex_unlock(&g_lock);
    return count;
}
