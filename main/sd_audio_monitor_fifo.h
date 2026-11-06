#pragma once
#include "sd_audio_catalog.h"

/* Storage lock required. An empty result with waiting=true keeps the queue
 * pending; it is never permission to send session_end or skip to another
 * monitor. Meeting scheduling remains independent. */
esp_err_t sd_monitor_fifo_next(const uint8_t owner[32], sd_audio_session_t *session,
    sd_audio_segment_t *segment, bool *found, bool *file, bool *waiting);
void sd_monitor_fifo_reset(void);
/* Bounded metadata-only end gate; NOT_FINISHED means resume verification. */
esp_err_t sd_monitor_fifo_end_ready(sd_audio_session_t *session, bool *ready);
esp_err_t sd_monitor_fifo_finish(sd_audio_session_t *session);

/* Independent meeting lane, identical file/ACK ordering. Sealed files are
 * eligible while capture continues; session_end still requires ended=true
 * and a durable ACK/missing proof for every reserved index. */
esp_err_t sd_meeting_fifo_next(const uint8_t owner[32], sd_audio_session_t *session,
    sd_audio_segment_t *segment, bool *found, bool *file, bool *waiting);
esp_err_t sd_meeting_fifo_end_ready(sd_audio_session_t *session, bool *ready);
esp_err_t sd_meeting_fifo_finish(sd_audio_session_t *session);
