#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cloud_time.h"
#include "sd_audio_time.h"
#define SD_AUDIO_SEGMENT_ATTEMPTED 2U
#define SD_AUDIO_SEGMENT_RETIRING 2U
#define SD_AUDIO_SEGMENT_RETIRED 3U
/* RETIRED is also a terminal local discard: its original wire number remains
 * reserved, it is never an upload ACK and archive repair must not revive it. */
/* Permanent protocol conflicts retain the entire scene for manual recovery. */
esp_err_t sd_audio_catalog_block(const char *id);

/* All calls are serialized by the spool storage lock. No network operations
 * are permitted while that lock is held. Two CRC slots survive torn updates. */
typedef struct {
    uint32_t generation;
    uint32_t magic;
    char session_id[48];
    uint8_t owner[32];
    uint64_t start_ms;
    uint64_t ended_ms;
    uint32_t boot_id;
    uint32_t segment_count;
    uint32_t first_pending;
    uint8_t mode;
    uint8_t ended;
    uint8_t complete;
    uint8_t done;
    uint32_t crc;
} sd_audio_session_t;

typedef struct {
    uint32_t generation;
    uint32_t magic;
    char path[128];
    uint64_t start_ms;
    uint64_t end_ms;
    uint32_t total_bytes;
    uint32_t payload_crc;
    uint32_t index;
    uint8_t sealed;
    uint8_t acknowledged;
    uint8_t realtime;
    uint8_t missing;
    uint32_t crc;
} sd_audio_segment_t;

/* Optional CRC-protected sidecars leave existing archive/manifest layouts intact.
 * Caller holds storage lock. Unknown/missing time never changes audio validity. */
esp_err_t sd_audio_catalog_clock_save(uint32_t boot_id, const uint8_t owner[32],
    const nuwax_cloud_time_anchor_t *anchor);
esp_err_t sd_audio_catalog_session_time(const sd_audio_session_t *session,
    uint32_t current_boot_id, const nuwax_cloud_time_anchor_t *anchor,
    bool stopped, uint64_t stopped_ms, sd_audio_session_time_t *out);

esp_err_t sd_audio_catalog_init(void);
/* Quiescent storage only. Uses exclusive scratch files, never scene identities.
 * Verifies actual stdio dual-slot updates, rename and a cross-sector index. */
esp_err_t sd_audio_catalog_qualify_storage(void);
/* Storage lock required; checks durable identity without modifying old files. */
esp_err_t sd_audio_catalog_check_new(const char *id);
esp_err_t sd_audio_catalog_create(sd_audio_session_t *session);
esp_err_t sd_audio_catalog_load(const char *id, sd_audio_session_t *session);
esp_err_t sd_audio_catalog_save(sd_audio_session_t *session);
esp_err_t sd_audio_catalog_read(const char *id, uint32_t index, sd_audio_segment_t *segment);
esp_err_t sd_audio_catalog_write(const char *id, sd_audio_segment_t *segment);
/* Durable retry marker only: does not change eligibility/order. Preserves
 * replay picks/cursors, but invalidates single-use selection/ACK proof. */
esp_err_t sd_audio_catalog_mark_attempted(const char *id, uint32_t index);
esp_err_t sd_audio_catalog_reserve(sd_audio_session_t *session, sd_audio_segment_t *segment);
esp_err_t sd_audio_catalog_ack(const char *id, uint32_t index);
/* Storage lock required. Reuses one unchanged CRC-checked selection, otherwise
 * loads normally. Caller still checks current owner and archive identity. */
esp_err_t sd_audio_catalog_bind(const char *id, uint32_t index,
    sd_audio_session_t *session, sd_audio_segment_t *segment, bool *reused);
/* Storage lock + replay file lease required. Caller first validates the full
 * immutable audio and fences the same physical card. Reconcile only this path;
 * never replace another owner/capture, durable ACK or conflicting sidecar. */
esp_err_t sd_audio_catalog_reconcile_file(const sd_audio_session_t *expected,
    const sd_audio_segment_t *expected_part, bool *changed);
/* Only after durable ACK AND successful file removal; volatile hint only. */
void sd_audio_catalog_note_deleted(const char *id, uint32_t index);
esp_err_t sd_audio_catalog_mark_missing(const char *id, uint32_t index);
/* Known-file loss must never replace unreadable metadata with an empty hole.
 * Storage lock held. Checks immutable identity and preserves its original CRC. */
esp_err_t sd_audio_catalog_mark_missing_expected(const sd_audio_session_t *session,
                                               const sd_audio_segment_t *segment);
/* Only after independent verification + ordered media recovery confirms loss.
 * Persist a terminal gap before unlink; preserve identity/count/other indexes.
 * A failed best-effort unlink cannot put this file back in the upload queue. */
esp_err_t sd_audio_catalog_discard_expected(const sd_audio_session_t *session,
                                          const sd_audio_segment_t *segment);
/* Enable bounded page indexes before the first file of a new monitor scene.
 * Existing .index scenes remain readable and reclaimable without migration. */
esp_err_t sd_audio_catalog_enable_retention(const char *id);
/* Same storage lock as selection and reclamation. One replay worker holds a
 * file lease from selection until its final local cleanup/error handling. */
esp_err_t sd_audio_catalog_pin(const char *id, uint32_t index);
void sd_audio_catalog_unpin(void);
bool sd_audio_catalog_replay_pinned(void);
typedef uint64_t sd_audio_lease_token_t;
typedef enum {
    SD_AUDIO_LEASE_REPLAY = 1,
    SD_AUDIO_LEASE_VERIFY = 2,
} sd_audio_lease_owner_t;
/* One reader at a time. The selected immutable tuple is captured without I/O;
 * callers fence the physical card through media_generation before publishing.
 * A failed acquire never modifies another owner's lease. All calls need the
 * storage lock. Legacy pin/unpin cannot release these token-owned leases. */
esp_err_t sd_audio_catalog_lease_acquire(const sd_audio_session_t *session,
    const sd_audio_segment_t *segment, sd_audio_lease_owner_t owner,
    uint32_t media_generation, uint32_t capture_generation, sd_audio_lease_token_t *token);
bool sd_audio_catalog_lease_matches(sd_audio_lease_token_t token,
    sd_audio_lease_owner_t owner, uint32_t media_generation);
esp_err_t sd_audio_catalog_lease_release(sd_audio_lease_token_t token,
                                       sd_audio_lease_owner_t owner);
/* Offline-only bounded inventory. Optional verification hints are scoped to
 * this boot nonce + media generation; they never authorize ACK/deletion/end.
 * NOT_FOUND means directory exhaustion without a metadata error; unreadable
 * scenes do not hide later healthy files or count as a successful full scan.
 * Storage lock required, reset only after the reader releases its lease. */
void sd_audio_catalog_verify_reset(void);
esp_err_t sd_audio_catalog_verify_next(const uint8_t owner[32], uint64_t nonce,
    uint32_t media_generation, sd_audio_session_t *session, sd_audio_segment_t *segment);
esp_err_t sd_audio_catalog_verify_checkpoint(const sd_audio_session_t *session,
    uint64_t nonce, uint32_t media_generation, uint32_t next_index, sd_audio_lease_token_t token);
/* Delete at most one oldest eligible monitoring file owned by this binding.
 * Missing metadata is durable before unlink. Meetings, foreign/blocked scenes,
 * unsealed files and the replay lease are protected. NOT_FINISHED asks the
 * caller to release the lock/yield during bounded cursor maintenance. */
esp_err_t sd_audio_catalog_reclaim(const uint8_t owner[32], uint64_t *removed_bytes,
                                 char removed_path[128]);
/* Data-first policy. Never retire unconfirmed audio; previously persisted
 * RETIRING records from older firmware still recover through the old path. */
esp_err_t sd_audio_catalog_reclaim_confirmed(const uint8_t owner[32], uint64_t *removed_bytes,
                                           char removed_path[128]);
void sd_audio_catalog_replay_reset(void);
/* Defer exactly this ID for the current pass (not a boot-relative time range).
 * A full bounded set rotates the NEXT pass after this scene and returns NO_MEM;
 * caller ends/backoffs without deleting data. Media initialization resets rotation. */
esp_err_t sd_audio_catalog_defer(const sd_audio_session_t *session);
/* Recovery runs only with recording/replay stopped, after archive .part recovery. */
esp_err_t sd_audio_catalog_recover(uint32_t boot_id);
/* Same physical card, binding and capture generation must be fenced by the
 * caller. Quiescent storage only. Rebuilds only an unreadable active manifest;
 * existing file ACKs, owners and valid completed scenes remain authoritative. */
esp_err_t sd_audio_catalog_prepare_active(const sd_audio_session_t *checkpoint);
/* Quiesce all FILE/replay users first. Active recovery preserves this scene's
 * recording intent; the storage owner retains its MIC and bounded RAM queue. */
esp_err_t sd_audio_catalog_recover_active(uint32_t boot_id, const char *active_id);
esp_err_t sd_audio_catalog_prepare_remount(void);
/* Storage lock required. Background index repair uses bounded, read-only
 * directory slices; no FILE or DMA loan survives a return. Set a native
 * absolute monotonic deadline for the current selection, then clear to zero.
 * Cancel/remount must reset before reporting the replay owner quiescent. */
void sd_audio_catalog_repair_set_deadline(int64_t until_us);
esp_err_t sd_audio_catalog_repair_reset(void);
/* Only after durable file ACK and successful audio unlink. Local sidecar only. */
esp_err_t sd_audio_catalog_drop_backup(const char *archive_path);
/* After recovery, distinguish an actual pending session from an empty or
 * fully acknowledged catalog without creating an unnecessary connection. */
esp_err_t sd_audio_catalog_has_pending(bool *pending);
/* Diagnostic counters under the same storage lock; unsigned deltas wrap. */
typedef struct {
    uint32_t manifest_reads;
    uint32_t completed_hits;
    uint32_t owner_skips;
    uint32_t repaired_sessions;
    char last_owner_blocked[48];
} sd_audio_catalog_stats_t;
void sd_audio_catalog_stats(sd_audio_catalog_stats_t *stats);
/* Atomic, I/O-free eligibility hint. Cannot authorize upload or deletion. */
uint32_t sd_audio_catalog_live_revision(void);
/* Bounded tail scan for live work; historical cursor advances over durable ACKs.
 * found_session may have no file: all data committed, session_end still due.
 * NOT_FINISHED means cursor maintenance remains: unlock/yield and call again. */
esp_err_t sd_audio_catalog_next(const uint8_t owner[32], uint32_t boot_id,
    uint64_t now_ms, uint64_t live_since_ms, const char *active_id, bool realtime_only,
    int preferred_mode, sd_audio_session_t *session, sd_audio_segment_t *segment,
    bool *found_session, bool *found_file);
bool sd_audio_segment_realtime(const sd_audio_session_t *session,
    const sd_audio_segment_t *segment, uint32_t boot_id, uint64_t now_ms, uint64_t live_since_ms);
