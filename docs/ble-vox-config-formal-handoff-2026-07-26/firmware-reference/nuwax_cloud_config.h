#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NUWAX_CLOUD_DEVICE_ID_MAX 40
#define NUWAX_CLOUD_URL_MAX 192
#define NUWAX_CLOUD_DEVICE_SECRET_MAX 96

typedef struct {
    uint32_t version;
    char device_id[NUWAX_CLOUD_DEVICE_ID_MAX];
    char cloud_url[NUWAX_CLOUD_URL_MAX];
    char device_secret[NUWAX_CLOUD_DEVICE_SECRET_MAX];
} nuwax_cloud_config_t;

/* Initializes the dynamic cloud configuration store. Safe to call repeatedly. */
esp_err_t nuwax_cloud_config_init(void);

/* Validates and atomically replaces the active URL/device-secret pair. */
esp_err_t nuwax_cloud_config_set(const nuwax_cloud_config_t *config);

/* No compiled fallback URL or secret exists. */
esp_err_t nuwax_cloud_config_get(nuwax_cloud_config_t *config);

bool nuwax_cloud_config_has_config(void);

#ifdef __cplusplus
}
#endif
