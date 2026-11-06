#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <setjmp.h>
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef struct { struct { uint8_t ssid[32], password[64]; } sta; } wifi_config_t;
typedef struct { uint8_t ssid[33]; } wifi_ap_record_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
#define ESP_ERR_WIFI_STATE 0x3006
#define ESP_ERR_WIFI_CONN 0x3007
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define WIFI_IF_STA 0
#define WIFI_STORAGE_FLASH 1
#define PROVISION_NVS_FLASH_GUARD_TIMEOUT_MS 100
#define CONFIG_NUWAX_PROVISION_RUNTIME_IDLE_TIMEOUT_MS 600000
#define ROLLBACK_CONNECT_TIMEOUT_MS 10000
#define NUWAX_PROVISION_OUTCOME_SUCCESS 1
#define NUWAX_PROVISION_OUTCOME_OLD_WIFI_RECOVERED 9
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr, ...) do {int e_=(expr);if(e_!=ESP_OK)return e_;}while(0)
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(t) (t)
int nvs_open(const char *,int,nvs_handle_t *);
void nvs_close(nvs_handle_t);
int nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
int nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
int nvs_erase_key(nvs_handle_t,const char *);
int nvs_commit(nvs_handle_t);
int esp_wifi_get_config(int,wifi_config_t *);
int esp_wifi_set_config(int,const wifi_config_t *);
int esp_wifi_set_storage(int);
int nuwax_wifi_import_driver_credentials(void);
bool nuwax_wifi_saved_credentials_match(const char *,const char *);
uint32_t esp_crc32_le(uint32_t,const uint8_t *,uint32_t);
uint32_t esp_random(void);
