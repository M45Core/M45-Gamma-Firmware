#pragma once

#include "cJSON.h"
#include "m45_config.h"

/* Byte budget, not a pool-count limit; NVS may run out before this budget. */
#define M45_BACKUP_JSON_MAX_BYTES 32768U

typedef struct {
    char host[M45_POOL_HOST_MAX + 1];
    char ip[M45_POOL_IP_MAX + 1]; /* Runtime DNS cache; never stored in JSON. */
    uint16_t port;
    bool tls;
} m45_backup_pool_t;

esp_err_t m45_backup_pools_load(void);
void m45_backup_pools_clear_runtime(void);
esp_err_t m45_backup_pools_save(const cJSON *array);
cJSON *m45_backup_pools_parse_json(const char *text);
cJSON *m45_backup_pools_json(void);
size_t m45_backup_pools_count(void);
size_t m45_backup_pools_next_index(size_t current);
bool m45_backup_pool_get(size_t index, m45_backup_pool_t *pool);
void m45_backup_pool_cache_ip(const char *host, const char *ip);
