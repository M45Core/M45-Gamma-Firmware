#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "m45_error_log.h"
#include "m45_fault_log.h"
#include "m45_log_buffer.h"
#include "tps546.h"

static int64_t boot_us = 32000000;
static time_t wall_time = 0;
static unsigned char stored[16384];
static size_t stored_size;
static unsigned char slot_data[8][2048];
static size_t slot_sizes[8];
static vprintf_like_t log_hook;
int64_t esp_timer_get_time(void) { return boot_us; }
time_t time(time_t *out) { if (out) *out = wall_time; return wall_time; }
const char *esp_err_to_name(esp_err_t err) { return err == ESP_ERR_TIMEOUT ? "ESP_ERR_TIMEOUT" : "ESP_FAIL"; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer) { (void)args; *timer=(void *)1; return ESP_OK; }
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) { (void)t; (void)us; return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t t) { (void)t; return ESP_OK; }
static int sink(const char *fmt, va_list args) { return vsnprintf(NULL, 0, fmt, args); }
vprintf_like_t esp_log_set_vprintf(vprintf_like_t hook) { log_hook=hook; return sink; }
static void emit(const char *fmt, ...) { va_list args; va_start(args,fmt); log_hook(fmt,args); va_end(args); }
size_t strlcpy(char *dst,const char *src,size_t cap) { size_t n=strlen(src); if(cap){size_t len=n<cap-1?n:cap-1;memcpy(dst,src,len);dst[len]=0;}return n; }
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h) { (void)name;(void)mode;*h=1;return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size) {
    (void)h; int slot=strncmp(key,"fault",5)==0?key[5]-'0':-1;
    size_t bytes=slot<0?stored_size:slot_sizes[slot];
    const void *data=slot<0?(void *)stored:(void *)slot_data[slot];
    if(!bytes)return ESP_ERR_NVS_NOT_FOUND;
    if(!out){*size=bytes;return ESP_OK;}
    if(*size<bytes)return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out,data,bytes);*size=bytes;return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t size) {
    (void)h;int slot=strncmp(key,"fault",5)==0?key[5]-'0':-1;
    if(slot<0){assert(size<=sizeof(stored));memcpy(stored,data,size);stored_size=size;}
    else{assert(slot<8 && size<=sizeof(slot_data[slot]));memcpy(slot_data[slot],data,size);slot_sizes[slot]=size;}
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key) {
    (void)h;assert(strncmp(key,"fault",5)==0);int slot=key[5]-'0';assert(slot>=0 && slot<8);
    if(!slot_sizes[slot])return ESP_ERR_NVS_NOT_FOUND;
    slot_sizes[slot]=0;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h;return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

static void snapshot_tests(void)
{
    char text[1024];
    TPS546_StatusSnapshot s={.valid=TPS546_SNAPSHOT_ALL_VALID,
        .status_word=TPS546_STATUS_IOUT_OC|TPS546_STATUS_PGOOD,
        .st_iout=TPS546_STATUS_IOUT_OCF,.read_vin=4.91f,.read_vout=1.15f,
        .read_iout=33.4f,.read_temp1=88,.vout_command=1.16f};
    TPS546_format_snapshot(&s,text,sizeof(text));
    assert(strstr(text,"IOUT overcurrent"));assert(strstr(text,"power-good lost"));
    assert(strstr(text,"IOUT=33.400A"));assert(strstr(text,"VIN=4.910V"));assert(strstr(text,"IOUT_STATUS=0x80"));
    s.valid=(1U<<TPS546_FIELD_STATUS);s.read_error=ESP_ERR_TIMEOUT;s.failed_command=0x7a;
    TPS546_format_snapshot(&s,text,sizeof(text));
    assert(strstr(text,"ESP_ERR_TIMEOUT (0x107) reading STATUS_VOUT (0x7a)"));
    assert(strstr(text,"VIN=unavailable"));assert(!strstr(text,"VIN=4.910V"));
    s.valid=TPS546_SNAPSHOT_ALL_VALID;s.read_error=ESP_OK;s.status_word=0xffff;
    s.st_vout=s.st_input=s.st_iout=s.st_temp=s.st_cml=s.st_mfr=s.st_other=0xff;
    TPS546_format_snapshot(&s,text,sizeof(text)-128);
    assert(strstr(text,"command=1.160V")); // Even simultaneous flags retain measurements.
    char tiny[4]={'x','x','x','x'};TPS546_format_snapshot(&s,tiny,sizeof(tiny));assert(tiny[3]==0);
    TPS546_format_snapshot(NULL,text,sizeof(text));assert(strstr(text,"unavailable"));
}

static void persistence_tests(void)
{
    typedef struct {uint32_t id;uint64_t epoch_seconds;uint32_t uptime_seconds;char message[96];} old_entry;
    struct {uint32_t magic,next_id;uint8_t count,reserved[3];old_entry entries[8];} old={.magic=0x4d343546,.next_id=8,.count=1};
    old.entries[0]=(old_entry){.id=7,.epoch_seconds=1786801200,.uptime_seconds=12,.message="old TPS fault"};
    memcpy(stored,&old,sizeof(old));stored_size=sizeof(old);
    assert(m45_fault_log_init()==ESP_OK);
    m45_fault_entry_t entries[8];assert(m45_fault_log_snapshot(entries,8)==1);
    assert(entries[0].id==7 && entries[0].epoch_seconds==1786801200 && entries[0].uptime_seconds==12);
    assert(!strcmp(entries[0].message,"old TPS fault"));
    char long_text[1024];memset(long_text,'x',sizeof(long_text));long_text[1023]=0;
    assert(m45_fault_log_record(long_text)==ESP_OK);
    assert(m45_fault_log_record(long_text)==ESP_OK); // Deduplicate the startup wrapper.
    assert(m45_fault_log_snapshot(entries,8)==2);assert(strlen(entries[0].message)==1023);
    assert(entries[0].epoch_seconds==0 && entries[0].uptime_seconds==32);
    wall_time=1786801500;boot_us+=6000000;
    assert(m45_fault_log_record("ASIC temperature 70.0C >= maximum 69.0C")==ESP_OK);
    assert(m45_fault_log_init()==ESP_OK); // Simulated restart: full text and dates survive.
    assert(m45_fault_log_snapshot(entries,8)==3);assert(entries[0].epoch_seconds==1786801500);
    assert(entries[0].uptime_seconds==38);assert(strlen(entries[1].message)==1023);
    // The same failure at the same uptime in a new boot is a separate event.
    assert(m45_fault_log_record("ASIC temperature 70.0C >= maximum 69.0C")==ESP_OK);
    assert(m45_fault_log_snapshot(entries,8)==4);
    assert(entries[0].id != entries[1].id);
    assert(m45_fault_log_remove(entries[1].id)==ESP_OK);
    assert(m45_fault_log_clear()==ESP_OK);assert(m45_fault_log_snapshot(entries,8)==0);
    for(int i=0;i<12;i++){boot_us+=6000000;long_text[0]=(char)('a'+i);assert(m45_fault_log_record(long_text)==ESP_OK);}
    assert(stored_size < 32); // Metadata never rewrites a full large history blob.
    size_t stored_total=stored_size;for(int i=0;i<8;i++)stored_total+=slot_sizes[i];
    assert(stored_total<9000); // Worst-case retained records fit comfortably in 24 KiB NVS.
    assert(m45_fault_log_init()==ESP_OK);assert(m45_fault_log_snapshot(entries,8)==8);
    assert(entries[0].message[0]=='l' && entries[7].message[0]=='e');
    assert(m45_fault_log_clear()==ESP_OK);assert(m45_fault_log_init()==ESP_OK);
    assert(m45_fault_log_snapshot(entries,8)==0);
}

static void error_capture_tests(void)
{
    boot_us = 38000000;
    m45_log_buffer_init();
    m45_error_entry_t *entries=calloc(M45_ERROR_LOG_CAPACITY,sizeof(*entries));assert(entries);
    uint32_t dropped,truncated;
    emit("I (1) test: normal log\n");emit("W (2) test: warning\n");
    emit("\033[0;31mE (3) tps546: timeout %d\033[0m\n",7);
    assert(m45_error_log_snapshot(entries,64,&dropped,&truncated)==1);
    assert(strstr(entries[0].message,"timeout 7"));assert(!strchr(entries[0].message,'\033'));
    assert(entries[0].epoch_seconds==(uint64_t)wall_time && entries[0].uptime_seconds==38);
    assert(!dropped && !truncated);
    char live[128];assert(m45_log_buffer_copy_since(0,live,sizeof(live),NULL,NULL)==0);
    m45_log_buffer_keep_active(5000);emit("E (4) test: page open\n");
    assert(m45_log_buffer_copy_since(0,live,sizeof(live),NULL,NULL)>0);
    boot_us+=6000000;wall_time=0;emit("E (5) test: page closed again\n");
    assert(m45_error_log_snapshot(entries,64,&dropped,&truncated)==3);
    assert(entries[0].epoch_seconds==0 && entries[0].uptime_seconds==44);
    char detailed[1200];memset(detailed,'b',sizeof(detailed)-1);detailed[1199]=0;
    emit("E (6) test: %s end-of-details",detailed);
    assert(m45_error_log_snapshot(entries,64,&dropped,&truncated)==4);
    assert(strstr(entries[0].message,"end-of-details"));assert(truncated==0);
    char oversized[3000];memset(oversized,'a',sizeof(oversized)-1);oversized[2999]=0;
    emit("E (6) test: %s",oversized);
    assert(m45_error_log_snapshot(entries,64,&dropped,&truncated)==5);assert(truncated==1);
    assert(strstr(entries[0].message,"[message truncated]"));
    for(int i=0;i<70;i++)emit("E (%d) test: entry %d\n",i,i);
    assert(m45_error_log_snapshot(entries,64,&dropped,&truncated)==64);assert(dropped==11);
    assert(strstr(entries[0].message,"entry 69"));assert(strstr(entries[63].message,"entry 6"));
    free(entries);
}
int main(void){snapshot_tests();persistence_tests();error_capture_tests();puts("Fault diagnostics, persistence/migration, timestamps, and always-on error capture passed.");}
