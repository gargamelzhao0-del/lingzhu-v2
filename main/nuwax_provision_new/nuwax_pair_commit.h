#pragma once

#include "esp_wifi.h"
#include "nuwax_cloud_config.h"

/* All mutations run on the provisioning owner. The status queries are atomic
 * because UI cancellation can run on another task. No wire-format changes. */
esp_err_t nuwax_pair_commit_load(void);
bool nuwax_pair_commit_pending(void);
bool nuwax_pair_commit_requires_app(void);
bool nuwax_pair_commit_blocks_rollback(void);
/* Preserve an ambiguous rollback record without boot-looping or trusting it. */
esp_err_t nuwax_pair_commit_require_app(void);
esp_err_t nuwax_pair_commit_begin(const wifi_config_t *wifi,
                                  const nuwax_cloud_config_t *cloud);
esp_err_t nuwax_pair_commit_complete(
    esp_err_t (*mark_verified)(const wifi_config_t *),
    bool (*marker_matches)(const wifi_config_t *));
