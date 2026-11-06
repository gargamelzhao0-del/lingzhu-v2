#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool mounted;
    uint64_t capacity_bytes;
    uint32_t sector_size;
    uint32_t max_frequency_khz;
} nuwax_sd_card_info_t;

/* Canonical CID/capacity identity, internal only; never sent or logged. */
typedef struct { uint8_t bytes[40]; } nuwax_sd_card_identity_t;
bool nuwax_sd_card_get_identity(nuwax_sd_card_identity_t *identity);

esp_err_t nuwax_sd_card_mount(nuwax_sd_card_info_t *info);
esp_err_t nuwax_sd_card_read_write_selftest(void);
/* Only call after every VFS FILE user has quiesced. */
esp_err_t nuwax_sd_card_unmount(void);
/* Task-context API. Calls are serialized; no hot-plug ISR API is provided. */
bool nuwax_sd_card_is_mounted(void);
/* Nonblocking last completed mount result, not proof of current readiness
 * or physical presence. Retained through health probes and ordered recovery. */
bool nuwax_sd_card_media_detected(void);
/* Last reported driver/I/O fault, without taking the SD lock. ESP_OK means
 * no current driver fault; full catalog readiness is checked separately. */
esp_err_t nuwax_sd_card_media_error(void);
/* Read-only task-context diagnostic; does not issue a card command. */
void nuwax_sd_card_log_clock(void);
/* Performs one filesystem health probe using the SD driver's command
 * timeouts. The
 * caller must serialize it against archive FILE users; a failed probe latches
 * the media unhealthy so the ordered recovery path can remount it. */
esp_err_t nuwax_sd_card_probe(void);
/* Read-only directory admission. Caller serializes against archive FILE users.
 * Missing paths may be allowed before mkdir; existing paths are always checked. */
esp_err_t nuwax_sd_card_validate_directory(const char *path, bool allow_missing);
/* Optional evidence cannot fence core audio merely because its own directory
 * is damaged. Same structural validation, no admission-state mutation. */
esp_err_t nuwax_sd_card_validate_evidence_directory(const char *path, bool allow_missing);
/* Latches a runtime media/filesystem failure without unmounting underneath
 * active FILE users. The storage owner must later call the ordered recovery
 * API only after all FILE, writer, and replay storage users have quiesced.
 * A MIC owner may keep bounded RAM frames while its storage path is paused. */
void nuwax_sd_card_mark_io_fault(esp_err_t result);
/* A high-speed validation failure must be retried after ordered recovery at
 * 20 MHz before classifying an archive as corrupt. Does not unmount here. */
bool nuwax_sd_card_fallback_after_read_error(esp_err_t result);
/* Ordered task-context recovery for a card removed and reinserted at runtime.
 * The caller must first prove that no VFS FILE or SD writer user remains.
 * A missing card is reported as an error and never formats the medium. */
esp_err_t nuwax_sd_card_recover(uint32_t timeout_ms);
/* Ordered storage-owner scope, same task begin/end. Applies remaining time
 * to native SDMMC commands, including mount and later catalog I/O. Existing
 * reader/writer leases must have quiesced before beginning recovery. */
esp_err_t nuwax_sd_card_io_deadline_begin(int64_t deadline_us);
void nuwax_sd_card_io_deadline_end(void);
