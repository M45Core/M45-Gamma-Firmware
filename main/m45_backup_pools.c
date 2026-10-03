#include "m45_backup_pools.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static m45_backup_pool_t *g_pools;
static size_t g_count;

cJSON *m45_backup_pools_parse_json(const char *text)
{
    /* Expected topology is array -> object -> primitive. Bound recursion before
       calling cJSON so a large malformed upload cannot exhaust the HTTP stack. */
    if (!text) return NULL;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (const char *p = text; *p; ++p) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (*p == '\\') escaped = true;
            else if (*p == '"') quoted = false;
        } else if (*p == '"') quoted = true;
        else if (*p == '[' || *p == '{') {
            if (++depth > 2) return NULL;
        } else if (*p == ']' || *p == '}') {
            if (!depth) return NULL;
            --depth;
        }
    }
    return cJSON_ParseWithOpts(text, NULL, true);
}

static esp_err_t parse_pools(const cJSON *array, m45_backup_pool_t **pools,
                             size_t *count)
{
    if (!cJSON_IsArray(array)) return ESP_ERR_INVALID_ARG;
    *count = (size_t)cJSON_GetArraySize(array);
    *pools = *count ? calloc(*count, sizeof(**pools)) : NULL;
    if (*count && !*pools) return ESP_ERR_NO_MEM;
    size_t i = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        const cJSON *host = cJSON_GetObjectItemCaseSensitive(item, "host");
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(item, "port");
        const cJSON *tls = cJSON_GetObjectItemCaseSensitive(item, "tls");
        if (!cJSON_IsString(host) || !host->valuestring[0] ||
            strlen(host->valuestring) > M45_POOL_HOST_MAX || !cJSON_IsNumber(port) ||
            !isfinite(port->valuedouble) || port->valuedouble < 1 ||
            port->valuedouble > 65535 || floor(port->valuedouble) != port->valuedouble ||
            (tls && !cJSON_IsBool(tls))) goto invalid;
        /* Hosts are addresses, not URLs or strings containing whitespace. */
        for (const unsigned char *p = (const unsigned char *)host->valuestring; *p; ++p) {
            if (*p <= 32 || *p >= 127 || *p == '/' || *p == '\\' || *p == '"') goto invalid;
        }
        strlcpy((*pools)[i].host, host->valuestring, sizeof((*pools)[i].host));
        (*pools)[i].port = (uint16_t)port->valuedouble;
        (*pools)[i].tls = cJSON_IsTrue(tls);
        ++i;
    }
    return ESP_OK;
invalid:
    free(*pools);
    *pools = NULL;
    return ESP_ERR_INVALID_ARG;
}

static void replace_pools(m45_backup_pool_t *pools, size_t count)
{
    pthread_mutex_lock(&g_lock);
    free(g_pools);
    g_pools = pools;
    g_count = count;
    pthread_mutex_unlock(&g_lock);
}

void m45_backup_pools_clear_runtime(void) { replace_pools(NULL, 0); }

esp_err_t m45_backup_pools_load(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("m45", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) { replace_pools(NULL, 0); return ESP_OK; }
    if (err != ESP_OK) return err;
    size_t bytes = 0;
    err = nvs_get_blob(nvs, "backup_pools", NULL, &bytes);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        replace_pools(NULL, 0);
        return ESP_OK;
    }
    if (err != ESP_OK || bytes == 0 || bytes > M45_BACKUP_JSON_MAX_BYTES) {
        nvs_close(nvs);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    char *text = malloc(bytes);
    if (!text) { nvs_close(nvs); return ESP_ERR_NO_MEM; }
    err = nvs_get_blob(nvs, "backup_pools", text, &bytes);
    nvs_close(nvs);
    cJSON *json = err == ESP_OK && text[bytes - 1] == '\0'
                      ? m45_backup_pools_parse_json(text) : NULL;
    free(text);
    m45_backup_pool_t *pools = NULL;
    size_t count = 0;
    if (err == ESP_OK) err = parse_pools(json, &pools, &count);
    cJSON_Delete(json);
    if (err == ESP_OK) replace_pools(pools, count);
    return err;
}

esp_err_t m45_backup_pools_save(const cJSON *array)
{
    m45_backup_pool_t *pools = NULL;
    size_t count = 0;
    esp_err_t err = parse_pools(array, &pools, &count);
    if (err != ESP_OK) return err;
    char *text = cJSON_PrintUnformatted(array);
    if (!text) { free(pools); return ESP_ERR_NO_MEM; }
    if (strlen(text) + 1 > M45_BACKUP_JSON_MAX_BYTES) {
        cJSON_free(text); free(pools); return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t nvs;
    err = nvs_open("m45", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "backup_pools", text, strlen(text) + 1);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    cJSON_free(text);
    if (err == ESP_OK) replace_pools(pools, count);
    else free(pools);
    return err;
}

size_t m45_backup_pools_count(void)
{
    pthread_mutex_lock(&g_lock);
    size_t count = g_count;
    pthread_mutex_unlock(&g_lock);
    return count;
}

size_t m45_backup_pools_next_index(size_t current)
{
    const size_t count = m45_backup_pools_count();
    return current >= count ? 0 : current + 1;
}

bool m45_backup_pool_get(size_t index, m45_backup_pool_t *pool)
{
    pthread_mutex_lock(&g_lock);
    bool found = index < g_count;
    if (found) *pool = g_pools[index];
    pthread_mutex_unlock(&g_lock);
    return found;
}

void m45_backup_pool_cache_ip(const char *host, const char *ip)
{
    if (!host || !ip || strlen(ip) > M45_POOL_IP_MAX) return;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_count; ++i) {
        if (strcmp(g_pools[i].host, host) == 0) {
            strlcpy(g_pools[i].ip, ip, sizeof(g_pools[i].ip));
        }
    }
    pthread_mutex_unlock(&g_lock);
}

cJSON *m45_backup_pools_json(void)
{
    cJSON *array = cJSON_CreateArray();
    if (!array) return NULL;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddStringToObject(item, "host", g_pools[i].host) ||
            !cJSON_AddNumberToObject(item, "port", g_pools[i].port) ||
            !cJSON_AddBoolToObject(item, "tls", g_pools[i].tls) ||
            !cJSON_AddItemToArray(array, item)) {
            cJSON_Delete(item); cJSON_Delete(array); array = NULL; break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return array;
}
