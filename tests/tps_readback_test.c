#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_LOGE(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#include "../main/tps546.c"
static int failures;
static unsigned reads, writes, first_events, recovered_events;
static uint8_t failed_command, operation;
static int64_t now_us;
const char *esp_err_to_name(esp_err_t e) { (void)e; return "ESP_ERR_TIMEOUT"; }
int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(TickType_t ticks) { now_us += (int64_t)ticks * 1000; }
static void read_event(void *context, const char *message) {
    assert(context == &reads);
    if (strstr(message, "recovered")) recovered_events++; else first_events++;
}
esp_err_t i2c_bitaxe_add_device(uint8_t addr, i2c_master_dev_handle_t *h, const char *tag) {
    (void)addr; (void)tag; *h=&reads; return ESP_OK;
}
esp_err_t i2c_bitaxe_register_read(i2c_master_dev_handle_t h, uint8_t command, uint8_t *data, size_t len) {
    (void)h; reads++;
    if (command==failed_command && failures!=0) { if (failures>0) failures--; return ESP_ERR_TIMEOUT; }
    memset(data,0,len);
    if (command==PMBUS_OPERATION) data[0]=operation;
    if (command==PMBUS_VOUT_MODE) data[0]=0x17;
    if (command==PMBUS_IC_DEVICE_ID) { data[0]=6; memcpy(data+1,DEVICE_ID_TPS546D24A,6); }
    return ESP_OK;
}
esp_err_t i2c_bitaxe_register_write_byte(i2c_master_dev_handle_t h,uint8_t command,uint8_t value) {
    (void)h;(void)command;(void)value;writes++;return ESP_OK;
}
esp_err_t i2c_bitaxe_register_write_word(i2c_master_dev_handle_t h,uint8_t command,uint16_t value) {
    (void)h;(void)command;(void)value;writes++;return ESP_OK;
}
esp_err_t i2c_bitaxe_register_write_addr(i2c_master_dev_handle_t h,uint8_t command) {
    (void)h;(void)command;writes++;return ESP_OK;
}
void TPS546_format_snapshot(const TPS546_StatusSnapshot *s,char *dst,size_t size) { (void)s; if(size)dst[0]=0; }
static void reset(uint8_t command,int count) {
    failed_command=command;failures=count;reads=writes=first_events=recovered_events=0;now_us=0;operation=OPERATION_ON;
    s_config=(TPS546_CONFIG){.TPS546_INIT_VOUT_MIN=.7f,.TPS546_INIT_VOUT_MAX=1.4f};
    s_vout_mode_valid=true;s_vout_exponent=-9;
    TPS546_set_read_event_callback(read_event,&reads);
}
int main(void) {
    reset(PMBUS_OPERATION,2);
    assert(TPS546_set_vout(1.15f)==ESP_OK);
    assert(reads==3 && writes==2 && first_events==1 && recovered_events==1 && now_us==200000);
    reset(PMBUS_OPERATION,-1);
    assert(TPS546_set_vout(1.15f)==ESP_ERR_TIMEOUT);
    assert(reads==50 && writes==2 && now_us==5000000 && first_events==1 && recovered_events==0);
    reset(PMBUS_OPERATION,0);operation=OPERATION_OFF;
    assert(TPS546_set_vout(1.15f)==ESP_ERR_INVALID_STATE);
    assert(reads==1 && writes==2 && now_us==0 && first_events==0);
    reset(PMBUS_VOUT_MODE,1);
    assert(read_vout_mode()==ESP_OK && reads==2 && writes==0 && first_events==1 && recovered_events==1);
    reset(PMBUS_IC_DEVICE_ID,1);uint8_t id[6];
    assert(read_device_id(id)==ESP_OK && reads==2 && writes==0 && first_events==1 && recovered_events==1);
    reset(PMBUS_IC_DEVICE_ID,-1);
    assert(read_device_id(id)==ESP_ERR_TIMEOUT && reads==50 && writes==0 && now_us==5000000);
    puts("TPS readback/startup retries preserve single writes, deadlines, and diagnostic events.");
}
