#include "m45_fault_log.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_timer.h"
#include "nvs.h"

#define FAULT_NAMESPACE "m45_faults"
#define FAULT_KEY "history"
#define FAULT_MAGIC 0x4d343546U
#define VALID_WALL_TIME_EPOCH 1704067200ULL

typedef struct {
    uint32_t magic;
    uint32_t next_id;
    uint8_t count;
    uint8_t reserved[3];
    m45_fault_entry_t entries[M45_FAULT_LOG_CAPACITY];
} fault_store_t;

typedef struct {
    uint32_t magic;
    uint32_t next_id;
    uint8_t count;
    uint8_t reserved[3];
} fault_metadata_t;

#define FAULT_METADATA_MAGIC (FAULT_MAGIC + 1U)
_Static_assert(M45_FAULT_LOG_CAPACITY <= 8, "fault slot key buffer capacity");

static pthread_mutex_t g_fault_lock = PTHREAD_MUTEX_INITIALIZER;
static fault_store_t g_faults;
static uint32_t g_last_recorded_id_this_boot;

static void fault_store_reset(void)
{
    memset(&g_faults, 0, sizeof(g_faults));
    g_faults.magic = FAULT_MAGIC;
    g_faults.next_id = 1;
}

static esp_err_t fault_store_save_locked(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(FAULT_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    // Separate, length-sized records avoid rewriting an 8 KiB blob in the
    // board's 24 KiB NVS partition. Only changed slots need a flash write.
    m45_fault_entry_t existing;
    for (size_t i = 0; err == ESP_OK && i < g_faults.count; ++i) {
        char key[] = "fault0";
        key[5] += (char)i;
        const size_t bytes = offsetof(m45_fault_entry_t, message) +
                             strlen(g_faults.entries[i].message) + 1;
        size_t old_bytes = sizeof(existing);
        const esp_err_t read_err = nvs_get_blob(nvs, key, &existing, &old_bytes);
        if (read_err == ESP_OK && old_bytes == bytes &&
            memcmp(&existing, &g_faults.entries[i], bytes) == 0) {
            continue;
        }
        err = nvs_set_blob(nvs, key, &g_faults.entries[i], bytes);
    }
    for (size_t i = g_faults.count; err == ESP_OK && i < M45_FAULT_LOG_CAPACITY; ++i) {
        char key[] = "fault0";
        key[5] += (char)i;
        const esp_err_t erase_err = nvs_erase_key(nvs, key);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = erase_err;
        }
    }
    const fault_metadata_t metadata = {
        .magic = FAULT_METADATA_MAGIC, .next_id = g_faults.next_id,
        .count = g_faults.count,
    };
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, FAULT_KEY, &metadata, sizeof(metadata));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t m45_fault_log_init(void)
{
    pthread_mutex_lock(&g_fault_lock);
    fault_store_reset();
    g_last_recorded_id_this_boot = 0;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(FAULT_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        pthread_mutex_unlock(&g_fault_lock);
        return ESP_OK;
    }
    if (err == ESP_OK) {
        size_t size = 0;
        err = nvs_get_blob(nvs, FAULT_KEY, NULL, &size);
        if (err == ESP_OK && size == sizeof(fault_metadata_t)) {
            fault_metadata_t metadata;
            err = nvs_get_blob(nvs, FAULT_KEY, &metadata, &size);
            if (err == ESP_OK && metadata.magic == FAULT_METADATA_MAGIC &&
                metadata.count <= M45_FAULT_LOG_CAPACITY && metadata.next_id != 0) {
                g_faults.count = metadata.count;
                g_faults.next_id = metadata.next_id;
                for (size_t i = 0; err == ESP_OK && i < metadata.count; ++i) {
                    char key[] = "fault0";
                    key[5] += (char)i;
                    size_t bytes = sizeof(g_faults.entries[i]);
                    err = nvs_get_blob(nvs, key, &g_faults.entries[i], &bytes);
                    if (err == ESP_ERR_NVS_NOT_FOUND) {
                        err = ESP_ERR_INVALID_SIZE;
                    }
                    if (err == ESP_OK &&
                        (bytes <= offsetof(m45_fault_entry_t, message) ||
                         bytes > offsetof(m45_fault_entry_t, message) + M45_FAULT_MESSAGE_MAX + 1 ||
                         ((char *)&g_faults.entries[i])[bytes - 1] != '\0')) {
                        err = ESP_ERR_INVALID_SIZE;
                    }
                }
                size = sizeof(g_faults);
            }
        } else if (err == ESP_OK && size == sizeof(g_faults)) {
            err = nvs_get_blob(nvs, FAULT_KEY, &g_faults, &size);
        } else if (err == ESP_OK) {
            // Import v0.0.11/v0.0.12 history without losing dates or IDs.
            typedef struct {
                uint32_t id;
                uint64_t epoch_seconds;
                uint32_t uptime_seconds;
                char message[96];
            } legacy_entry_t;
            typedef struct {
                uint32_t magic;
                uint32_t next_id;
                uint8_t count;
                uint8_t reserved[3];
                legacy_entry_t entries[M45_FAULT_LOG_CAPACITY];
            } legacy_store_t;
            legacy_store_t *legacy = size == sizeof(legacy_store_t)
                                         ? malloc(sizeof(*legacy)) : NULL;
            if (legacy != NULL) {
                err = nvs_get_blob(nvs, FAULT_KEY, legacy, &size);
                if (err == ESP_OK && legacy->magic == FAULT_MAGIC &&
                    legacy->count <= M45_FAULT_LOG_CAPACITY && legacy->next_id != 0) {
                    g_faults.count = legacy->count;
                    g_faults.next_id = legacy->next_id;
                    for (size_t i = 0; i < legacy->count; ++i) {
                        g_faults.entries[i].id = legacy->entries[i].id;
                        g_faults.entries[i].epoch_seconds = legacy->entries[i].epoch_seconds;
                        g_faults.entries[i].uptime_seconds = legacy->entries[i].uptime_seconds;
                        legacy->entries[i].message[95] = '\0';
                        strlcpy(g_faults.entries[i].message, legacy->entries[i].message,
                                sizeof(g_faults.entries[i].message));
                    }
                    size = sizeof(g_faults);
                }
                free(legacy);
            }
        }
        nvs_close(nvs);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err != ESP_OK || size != sizeof(g_faults) || g_faults.magic != FAULT_MAGIC ||
            g_faults.count > M45_FAULT_LOG_CAPACITY || g_faults.next_id == 0) {
            fault_store_reset();
            err = ESP_OK;
        }
    }
    for (size_t i = 0; i < g_faults.count; ++i) {
        g_faults.entries[i].message[M45_FAULT_MESSAGE_MAX] = '\0';
    }
    pthread_mutex_unlock(&g_fault_lock);
    return err;
}

esp_err_t m45_fault_log_record(const char *message)
{
    if (message == NULL || message[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&g_fault_lock);
    const uint32_t uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    if (g_faults.count > 0) {
        const m45_fault_entry_t *latest = &g_faults.entries[g_faults.count - 1U];
        if (latest->id == g_last_recorded_id_this_boot &&
            strcmp(latest->message, message) == 0 &&
            uptime_seconds >= latest->uptime_seconds &&
            uptime_seconds - latest->uptime_seconds <= 5U) {
            pthread_mutex_unlock(&g_fault_lock);
            return ESP_OK;
        }
    }
    if (g_faults.count == M45_FAULT_LOG_CAPACITY) {
        memmove(&g_faults.entries[0], &g_faults.entries[1],
                sizeof(g_faults.entries[0]) * (M45_FAULT_LOG_CAPACITY - 1));
        --g_faults.count;
    }
    m45_fault_entry_t *entry = &g_faults.entries[g_faults.count++];
    memset(entry, 0, sizeof(*entry));
    entry->id = g_faults.next_id++;
    if (g_faults.next_id == 0) {
        g_faults.next_id = 1;
    }
    const time_t now = time(NULL);
    entry->epoch_seconds = now >= (time_t)VALID_WALL_TIME_EPOCH ? (uint64_t)now : 0;
    entry->uptime_seconds = uptime_seconds;
    strlcpy(entry->message, message, sizeof(entry->message));
    const esp_err_t err = fault_store_save_locked();
    g_last_recorded_id_this_boot = err == ESP_OK ? entry->id : 0;
    pthread_mutex_unlock(&g_fault_lock);
    return err;
}

size_t m45_fault_log_snapshot(m45_fault_entry_t *entries, size_t capacity)
{
    if (entries == NULL || capacity == 0) {
        return 0;
    }
    pthread_mutex_lock(&g_fault_lock);
    const size_t count = g_faults.count < capacity ? g_faults.count : capacity;
    for (size_t i = 0; i < count; ++i) {
        entries[i] = g_faults.entries[g_faults.count - 1U - i];
    }
    pthread_mutex_unlock(&g_fault_lock);
    return count;
}

esp_err_t m45_fault_log_remove(uint32_t id)
{
    pthread_mutex_lock(&g_fault_lock);
    size_t index = g_faults.count;
    for (size_t i = 0; i < g_faults.count; ++i) {
        if (g_faults.entries[i].id == id) {
            index = i;
            break;
        }
    }
    if (index == g_faults.count) {
        pthread_mutex_unlock(&g_fault_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (index + 1U < g_faults.count) {
        memmove(&g_faults.entries[index], &g_faults.entries[index + 1U],
                sizeof(g_faults.entries[0]) * (g_faults.count - index - 1U));
    }
    memset(&g_faults.entries[g_faults.count - 1U], 0, sizeof(g_faults.entries[0]));
    --g_faults.count;
    const esp_err_t err = fault_store_save_locked();
    pthread_mutex_unlock(&g_fault_lock);
    return err;
}

esp_err_t m45_fault_log_clear(void)
{
    pthread_mutex_lock(&g_fault_lock);
    const uint32_t next_id = g_faults.next_id;
    fault_store_reset();
    g_faults.next_id = next_id == 0 ? 1 : next_id;
    const esp_err_t err = fault_store_save_locked();
    pthread_mutex_unlock(&g_fault_lock);
    return err;
}
