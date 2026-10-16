#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_NVS_NOT_FOUND 4
#define ESP_ERR_NVS_INVALID_LENGTH 5
#define ESP_ERR_INVALID_SIZE 6
#define ESP_ERR_TIMEOUT 0x107
const char *esp_err_to_name(esp_err_t);
