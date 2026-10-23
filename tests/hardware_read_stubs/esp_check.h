#pragma once
#define ESP_RETURN_ON_ERROR(call, ...) do { esp_err_t err_ = (call); if (err_ != ESP_OK) return err_; } while (0)
#define ESP_RETURN_ON_FALSE(condition, err, ...) do { if (!(condition)) return (err); } while (0)
