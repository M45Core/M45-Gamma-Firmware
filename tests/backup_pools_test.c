#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m45_backup_pools.h"
#include "nvs.h"

static char *stored, *pending;
static size_t stored_bytes, pending_bytes;
static bool storage_full, commit_failed;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) { size_t n = len < size - 1 ? len : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return len;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "m45") == 0); (void)mode; *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    (void)handle; assert(strcmp(key, "backup_pools") == 0);
    if (!stored) return ESP_ERR_NVS_NOT_FOUND;
    if (out) { assert(*size >= stored_bytes); memcpy(out, stored, stored_bytes); }
    *size = stored_bytes; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    (void)handle; assert(strcmp(key, "backup_pools") == 0);
    if (storage_full) return ESP_FAIL;
    pending = malloc(size); assert(pending); memcpy(pending, data, size); pending_bytes = size;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (commit_failed) return ESP_FAIL;
    free(stored); stored = pending; stored_bytes = pending_bytes; pending = NULL;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; free(pending); pending = NULL; }

static esp_err_t save_text(const char *text)
{
    cJSON *json = cJSON_Parse(text);
    esp_err_t err = m45_backup_pools_save(json);
    cJSON_Delete(json); return err;
}

int main(void)
{
    assert(!m45_backup_pools_parse_json("[[[[]]]]"));
    assert(!m45_backup_pools_parse_json("[] trailing data"));
    cJSON *shallow = m45_backup_pools_parse_json("[{\"host\":\"braces{inside}string\",\"port\":1}]");
    assert(shallow); cJSON_Delete(shallow);
    assert(m45_backup_pools_load() == ESP_OK);
    assert(m45_backup_pools_count() == 0);
    assert(m45_backup_pools_next_index(0) == 0);
    assert(save_text("[{\"host\":\"first.example\",\"port\":3333},{\"host\":\"second.example\",\"port\":443,\"tls\":true}]") == ESP_OK);
    m45_backup_pool_t pool;
    assert(m45_backup_pool_get(0, &pool) && !pool.tls && pool.port == 3333);
    assert(m45_backup_pool_get(1, &pool) && pool.tls && pool.port == 443);
    assert(!m45_backup_pool_get(2, &pool));
    m45_backup_pool_cache_ip("second.example", "192.0.2.2");
    assert(m45_backup_pool_get(1, &pool) && strcmp(pool.ip, "192.0.2.2") == 0);
    m45_backup_pool_cache_ip("missing.example", "192.0.2.3");
    assert(m45_backup_pools_next_index(0) == 1);
    assert(m45_backup_pools_next_index(1) == 2);
    assert(m45_backup_pools_next_index(2) == 0);
    assert(m45_backup_pools_next_index(100) == 0);
    const char *invalid[] = {"{}", "[{}]", "[{\"host\":\"\",\"port\":1}]",
        "[{\"host\":\"bad host\",\"port\":1}]", "[{\"host\":\"stratum://pool\",\"port\":1}]",
        "[{\"host\":\"pool\",\"port\":0}]", "[{\"host\":\"pool\",\"port\":65536}]",
        "[{\"host\":\"pool\",\"port\":1.5}]", "[{\"host\":\"pool\",\"port\":1,\"tls\":1}]"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        assert(save_text(invalid[i]) == ESP_ERR_INVALID_ARG);
        assert(m45_backup_pools_count() == 2);
    }
    storage_full = true;
    assert(save_text("[]") != ESP_OK);
    storage_full = false; commit_failed = true;
    assert(save_text("[]") != ESP_OK);
    commit_failed = false;
    assert(m45_backup_pools_count() == 2);
    m45_backup_pools_clear_runtime();
    assert(m45_backup_pools_load() == ESP_OK && m45_backup_pools_count() == 2);
    assert(m45_backup_pool_get(0, &pool) && strcmp(pool.host, "first.example") == 0);
    assert(m45_backup_pool_get(1, &pool) && pool.ip[0] == '\0');
    /* More than a string-sized NVS entry and far more than the weighted pool count. */
    cJSON *many = cJSON_CreateArray();
    for (unsigned i = 0; i < 100; ++i) {
        char host[64]; snprintf(host, sizeof(host), "backup-%u.example", i);
        cJSON *item = cJSON_CreateObject(); cJSON_AddStringToObject(item, "host", host);
        cJSON_AddNumberToObject(item, "port", 3333); cJSON_AddBoolToObject(item, "tls", false);
        cJSON_AddItemToArray(many, item);
    }
    assert(m45_backup_pools_save(many) == ESP_OK && stored_bytes > 4096);
    cJSON_Delete(many); m45_backup_pools_clear_runtime();
    assert(m45_backup_pools_load() == ESP_OK && m45_backup_pools_count() == 100);
    assert(m45_backup_pool_get(99, &pool) && strcmp(pool.host, "backup-99.example") == 0);
    cJSON *exported = m45_backup_pools_json(); assert(cJSON_GetArraySize(exported) == 100); cJSON_Delete(exported);
    many = cJSON_CreateArray();
    for (unsigned i = 0; i < 1000; ++i) {
        cJSON *item = cJSON_CreateObject(); cJSON_AddStringToObject(item, "host", "large-backup.example");
        cJSON_AddNumberToObject(item, "port", 3333); cJSON_AddItemToArray(many, item);
    }
    assert(m45_backup_pools_save(many) == ESP_ERR_INVALID_SIZE);
    assert(m45_backup_pools_count() == 100);
    cJSON_Delete(many);
    assert(save_text("[]") == ESP_OK);
    m45_backup_pools_clear_runtime(); assert(m45_backup_pools_load() == ESP_OK && m45_backup_pools_count() == 0);
    free(stored); puts("backup pool persistence, validation, failure retention, and rotation passed");
    return 0;
}
