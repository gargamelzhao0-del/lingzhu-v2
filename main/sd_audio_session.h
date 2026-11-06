#pragma once
#include "sdkconfig.h"
#include "offline_audio_spool.h"
#include "sd_audio_identity.h"

#if CONFIG_NUWAX_SD_SESSION_ENABLE
bool nuwax_sd_session_available(void);
/* Select local capture even while a known card is busy/recovering/full.
 * Start still checks readiness and the controller bounds the wait. */
bool nuwax_sd_session_prefers_local(void);
/* A confirmed I/O/qualification fault rejects a new start immediately.
 * ESP_OK permits the existing readiness/busy wait; it does not admit capture. */
esp_err_t nuwax_sd_session_start_fault(void);
bool nuwax_sd_session_active(void);
bool nuwax_sd_session_monitoring(void);
bool nuwax_sd_session_complete(void);
/* Capture, writer and queued tails have released their resources. Uploads
 * may continue; audio completeness is deliberately a separate result. */
bool nuwax_sd_session_stopped(void);
/* Owner-task start barrier. Pauses replay asynchronously before a new MIC
 * owner is published. Release on ACTIVE or cancellation; leases also expire. */
esp_err_t nuwax_sd_session_prepare_start(nuwax_offline_audio_mode_t mode, int64_t deadline_us);
void nuwax_sd_session_release_start(nuwax_offline_audio_mode_t mode);
void nuwax_sd_session_network(bool online);
/* Owner-task admission before stopping another mode. Read-only, serialized
 * with the SD writer; does not reserve, rename or create a scene. */
esp_err_t nuwax_sd_session_check_new_identity(const char *session_id);
/* In/out: adopt a nonempty valid cloud ID; generate only for an empty local
 * start. Existing/unfinished manifests must never be reused for new audio. */
esp_err_t nuwax_sd_session_start(nuwax_offline_audio_mode_t mode, char session_id[48]);
/* Cloud commands use a nonblocking catalog preflight. The storage supervisor
 * does the I/O; NOT_FINISHED means poll again without stopping the old owner. */
esp_err_t nuwax_sd_session_poll_identity(nuwax_offline_audio_mode_t mode,
    const char *session_id, int64_t deadline_us);
esp_err_t nuwax_sd_session_start_until(nuwax_offline_audio_mode_t mode,
    char session_id[48], int64_t deadline_us);
/* Nonblocking; final metadata is sealed by the storage owner. */
void nuwax_sd_session_stop(bool complete);
#else
static inline bool nuwax_sd_session_available(void) { return false; }
static inline bool nuwax_sd_session_prefers_local(void) { return false; }
static inline esp_err_t nuwax_sd_session_start_fault(void) { return ESP_OK; }
static inline bool nuwax_sd_session_active(void) { return false; }
static inline bool nuwax_sd_session_monitoring(void) { return false; }
static inline void nuwax_sd_session_network(bool online) { (void)online; }
#endif
