
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "conversation/speaker_jitter_policy.h"
#include "conversation/notification_playback.h"
static nuwax_notification_transaction_t tx;

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_INVALID_RESPONSE 2
#define pdTRUE 1
#define NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION 1
#define NUWAX_SPEAKER_PLAYBACK_OWNER_NONE 0
#define NUWAX_AUDIO_FLAG_END 2
#define NUWAX_SPEAKER_JITTER_STARTUP_BASE_MS 800U
#define NUWAX_SPEAKER_JITTER_RESUME_MIN_MS 640U
#define NUWAX_SPEAKER_JITTER_RESUME_MAX_MS 960U
#define NUWAX_SPEAKER_JITTER_RESUME_GAP_CUSHION_MS 160U
#define NUWAX_SPEAKER_JITTER_POLL_MS 20U
#define NUWAX_SPEAKER_REBUFFER_STALL_WARN_MS 5000U
#define NUWAX_SPEAKER_REBUFFER_STALL_TIMEOUT_MS 15000U
#define NUWAX_SPEAKER_STREAM_STALL_WARN_MS 12000U
#define NUWAX_SPEAKER_STREAM_STALL_TIMEOUT_MS 60000U
typedef int esp_err_t;
typedef struct {uint32_t connection_epoch,generation,sample_rate;} speaker_playback_packet_t;
typedef struct {uint32_t connection_epoch,generation;} speaker_generation_ref_t;
static struct {
 struct {bool active,end_accepted;uint32_t generation,next_sequence,pcm_frames;uint64_t pcm_bytes;} stream;
 bool stream_protocol_faulted,drop_until_end,playback_active,jitter_rebuffering;
 bool playback_session_active,start_gate_pending,start_gate_authorized;
 bool stop_fault_active,fade_cancel_active,runtime_reset_in_progress,downlink_admission_allowed;
 bool jitter_resume_fade_pending;
 uint32_t submitted_connection_epoch,submitted_generation,playback_connection_epoch,playback_generation;
 uint32_t protocol_fault_pending_generation,protocol_fault_pending_connection_epoch;
 uint32_t protocol_fault_notified_generation,protocol_fault_notified_connection_epoch;
 uint32_t stream_protocol_faults,stream_stall_faults;
 uint32_t jitter_startup_target_ms,jitter_resume_target_ms,jitter_rebuffer_started_ms;
 uint32_t jitter_rebuffer_entry_pcm_frames,jitter_rebuffer_events,ingress_max_gap_ms;
 uint32_t jitter_rebuffer_wait_ms,jitter_rebuffer_max_wait_ms,jitter_tail_wait_events,jitter_tail_wait_ms;
 uint32_t jitter_empty_started_ms,completion_claim_generation,cancel_pending_generation;
 uint32_t output_stop_owner_generation,io_claim_generation,maintenance_claim_generation;
 uint32_t metadata_expected_frames,metadata_expected_bytes,fade_cancel_deadline_ms;
 int active_owner;
 int64_t first_pcm_rx_us,startup_watermark_ready_us,playback_begin_rx_us;
 int64_t start_gate_authorized_us,first_i2s_write_us;
 char fade_cancel_source[48],active_turn_id[48],active_playback_id[48],active_notify_id[48];
} s_speaker_ctx;
static uint64_t now_us;
static uint32_t start_ms,records,samples,notices,cancels,polls,mode;
static bool locked,fade;
static uint32_t monotonic_ms(void){return (uint32_t)(now_us/1000);}
static int64_t esp_timer_get_time(void){return (int64_t)now_us;}
static void speaker_lock(void){assert(!locked);locked=true;}
static void speaker_unlock(void){assert(locked);locked=false;}
static uint32_t speaker_packet_ring_count(void){return records;}
static uint32_t speaker_packet_ring_samples(void){return samples;}
static void speaker_notify_playback_fault(uint32_t e,uint32_t g,int result){
 assert(!locked&&e==2&&g==9&&result!=0);notices++;
}
static bool speaker_restore_probe_duck_if_due(uint32_t t){return false;}
static uint32_t speaker_positive_delay_ticks(uint32_t ms){return ms;}
static bool speaker_fade_cancel_due(uint32_t t,char *s,size_t n,uint32_t *g){
 if(!fade)return false;*g=9;return true;
}
static bool speaker_cancel_now_if_current_epoch(uint32_t e,uint32_t g,const char *reason){
 assert(!locked);
 if(e!=s_speaker_ctx.submitted_connection_epoch||g!=s_speaker_ctx.submitted_generation)return false;
 cancels++;s_speaker_ctx.stream.active=false;s_speaker_ctx.drop_until_end=true;return true;
}
static void speaker_clear_end_seen_locked(void){assert(locked);}
static void speaker_reset_stream_runtime_locked(bool clear){
 assert(locked);memset(&s_speaker_ctx.stream,0,sizeof(s_speaker_ctx.stream));
}
static size_t strlcpy(char *d,const char *s,size_t n){size_t l=strlen(s);if(n){snprintf(d,n,"%s",s);}return l;}
static bool nuwax_speaker_stream_bind_expected(void *s,uint32_t g,uint32_t f,uint32_t b){return true;}
static void ulTaskNotifyTake(int clear,uint32_t ms){
 assert(!locked);assert(++polls<20000);now_us+=(uint64_t)ms*1000;
 uint32_t age=monotonic_ms()-start_ms;
 if(mode==1 && age%1000==0 && age<=4000){records++;samples+=320;s_speaker_ctx.stream.pcm_frames++;}
 if(mode==2 && age>=5000)s_speaker_ctx.stream.end_accepted=true;
 if(mode==3 && age>=5000){records=39;samples=39*320;}
 if(mode==4 && age>=1000)s_speaker_ctx.submitted_generation=10;
 if(mode==5 && age>=1000)s_speaker_ctx.submitted_connection_epoch=3;
 if(mode==6 && age>=1000)fade=true;
 if(mode==7 && age>=1000)s_speaker_ctx.drop_until_end=true;
 if(mode==8 && age>=5000)s_speaker_ctx.stream_protocol_faulted=true;
 if(mode==9 && age>=6000)s_speaker_ctx.stream.end_accepted=true;
 if(mode==10){
  if(age%9000==0){
   records++;samples+=320;s_speaker_ctx.stream.pcm_frames++;
   assert(nuwax_notification_refresh_stream_deadline(&tx,1,2,monotonic_ms(),10000));
  }
  assert(!nuwax_notification_deadline_expired(&tx,monotonic_ms()));
 }

}
static bool speaker_generation_source_matches_locked(uint32_t connection_epoch,
                                                     uint32_t generation) {
    return generation != 0U &&
           s_speaker_ctx.submitted_generation == generation &&
           s_speaker_ctx.submitted_connection_epoch == connection_epoch;
}
typedef struct {
    bool notify;
    uint32_t expected_sequence;
    uint32_t received_frames;
    uint64_t received_bytes;
} speaker_fault_notice_t;
static speaker_fault_notice_t speaker_claim_protocol_fault_locked(
    uint32_t connection_epoch, uint32_t generation, bool stream_stall) {
    speaker_fault_notice_t notice = {0};
    if (speaker_generation_source_matches_locked(connection_epoch, generation) &&
        s_speaker_ctx.stream.active &&
        s_speaker_ctx.stream.generation == generation) {
        s_speaker_ctx.stream_protocol_faulted = true;
        s_speaker_ctx.protocol_fault_pending_generation = generation;
        s_speaker_ctx.protocol_fault_pending_connection_epoch = connection_epoch;
        notice.expected_sequence = s_speaker_ctx.stream.next_sequence;
        notice.received_frames = s_speaker_ctx.stream.pcm_frames;
        notice.received_bytes = s_speaker_ctx.stream.pcm_bytes;
        if (s_speaker_ctx.protocol_fault_notified_generation != generation ||
            s_speaker_ctx.protocol_fault_notified_connection_epoch != connection_epoch) {
            s_speaker_ctx.protocol_fault_notified_generation = generation;
            s_speaker_ctx.protocol_fault_notified_connection_epoch = connection_epoch;
            s_speaker_ctx.stream_protocol_faults++;
            if (stream_stall) {
                s_speaker_ctx.stream_stall_faults++;
            }
            notice.notify = true;
        }
    }
    return notice;
}
static void speaker_publish_protocol_fault(uint32_t connection_epoch,
                                           uint32_t generation,
                                           esp_err_t result,
                                           const char *reason,
                                           speaker_fault_notice_t notice) {
    if (!notice.notify) {
        return;
    }
    ESP_LOGE(TAG,
             "speaker downlink stream fault generation=%lu result=%s reason=%s expected_seq=%lu received_frames=%lu received_bytes=%llu",
             (unsigned long)generation, esp_err_to_name(result),
             reason ? reason : "-", (unsigned long)notice.expected_sequence,
             (unsigned long)notice.received_frames,
             (unsigned long long)notice.received_bytes);
    speaker_notify_playback_fault(connection_epoch, generation, result);
}
static void speaker_schedule_protocol_fault(uint32_t connection_epoch,
                                            uint32_t generation,
                                            esp_err_t result,
                                            const char *reason,
                                            bool stream_stall) {
    speaker_lock();
    const speaker_fault_notice_t notice = speaker_claim_protocol_fault_locked(
        connection_epoch, generation, stream_stall);
    speaker_unlock();
    speaker_publish_protocol_fault(connection_epoch, generation, result, reason, notice);
}
static bool speaker_process_pending_protocol_fault(void) {
    speaker_generation_ref_t fault = {0};
    speaker_lock();
    fault.generation = s_speaker_ctx.protocol_fault_pending_generation;
    fault.connection_epoch =
        s_speaker_ctx.protocol_fault_pending_connection_epoch;
    s_speaker_ctx.protocol_fault_pending_generation = 0U;
    s_speaker_ctx.protocol_fault_pending_connection_epoch = 0U;
    speaker_unlock();
    if (fault.generation == 0U) {
        return false;
    }
    if (!speaker_cancel_now_if_current_epoch(
            fault.connection_epoch,
            fault.generation,
            "downlink_protocol_fault")) {
        ESP_LOGE(TAG,
                 "speaker downlink protocol-fault stop incomplete generation=%lu",
                  (unsigned long)fault.generation);
    }
    return true;
}
void nuwax_speaker_playback_note_playback_begin(const char *turn_id,
                                                const char *playback_id,
                                                uint32_t frames,
                                                uint32_t bytes) {
    const char *turn = turn_id ? turn_id : "";
    const char *playback = playback_id ? playback_id : "";
    uint32_t generation = 0U;
    uint32_t connection_epoch = 0U;
    bool totals_valid = true;
    const int64_t playback_begin_rx_us = esp_timer_get_time();
    speaker_lock();
    const bool same_playback =
        s_speaker_ctx.active_owner ==
            NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION &&
        turn[0] != '\0' &&
        s_speaker_ctx.active_turn_id[0] != '\0' &&
        strcmp(turn, s_speaker_ctx.active_turn_id) == 0 &&
        playback[0] != '\0' &&
        s_speaker_ctx.active_playback_id[0] != '\0' &&
        strcmp(playback, s_speaker_ctx.active_playback_id) == 0;
    if (s_speaker_ctx.fade_cancel_active &&
        playback[0] != '\0' &&
        s_speaker_ctx.active_playback_id[0] != '\0' &&
        strcmp(playback, s_speaker_ctx.active_playback_id) != 0) {
        s_speaker_ctx.fade_cancel_active = false;
        s_speaker_ctx.fade_cancel_deadline_ms = 0;
        s_speaker_ctx.fade_cancel_source[0] = '\0';
    }
    if (!same_playback) {
        s_speaker_ctx.metadata_expected_frames = frames;
        s_speaker_ctx.metadata_expected_bytes = bytes;
        s_speaker_ctx.playback_begin_rx_us = playback_begin_rx_us;
    } else {
        if (frames != 0U &&
            s_speaker_ctx.metadata_expected_frames != 0U &&
            frames != s_speaker_ctx.metadata_expected_frames) {
            totals_valid = false;
        } else if (frames != 0U) {
            s_speaker_ctx.metadata_expected_frames = frames;
        }
        if (bytes != 0U &&
            s_speaker_ctx.metadata_expected_bytes != 0U &&
            bytes != s_speaker_ctx.metadata_expected_bytes) {
            totals_valid = false;
        } else if (bytes != 0U) {
            s_speaker_ctx.metadata_expected_bytes = bytes;
        }
    }
    s_speaker_ctx.active_owner =
        NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION;
    strlcpy(s_speaker_ctx.active_turn_id, turn, sizeof(s_speaker_ctx.active_turn_id));
    s_speaker_ctx.active_notify_id[0] = '\0';
    strlcpy(s_speaker_ctx.active_playback_id, playback, sizeof(s_speaker_ctx.active_playback_id));
    s_speaker_ctx.playback_session_active = true;
    const bool live_stream_generation =
        s_speaker_ctx.stream.active &&
        !s_speaker_ctx.stream.end_accepted &&
        (s_speaker_ctx.start_gate_pending ||
         s_speaker_ctx.start_gate_authorized ||
         s_speaker_ctx.playback_active) &&
        s_speaker_ctx.stream.generation ==
            s_speaker_ctx.submitted_generation;
    if (live_stream_generation) {
        generation = s_speaker_ctx.stream.generation;
        connection_epoch = s_speaker_ctx.submitted_connection_epoch;
        totals_valid = totals_valid && nuwax_speaker_stream_bind_expected(
                                           &s_speaker_ctx.stream,
                                           generation,
                                           s_speaker_ctx.metadata_expected_frames,
                                           s_speaker_ctx.metadata_expected_bytes);
    }
    speaker_unlock();

    ESP_LOGI(TAG,
             "speaker playback context begin turn=%s playback=%s frames=%lu bytes=%lu generation=%lu totals_valid=%d",
             turn[0] ? turn : "-",
             playback[0] ? playback : "-",
             (unsigned long)frames,
             (unsigned long)bytes,
             (unsigned long)generation,
             totals_valid ? 1 : 0);
    if (!totals_valid && generation != 0U) {
        speaker_schedule_protocol_fault(connection_epoch,
                                        generation,
                                        ESP_ERR_INVALID_RESPONSE,
                                        "playback_metadata_totals",
                                        false);
    }
}
static bool speaker_abort_metadata_only_session(
    const char *turn_id,
    const char *playback_id,
    const char *reason,
    uint32_t timeout_ms) {
    const char *turn = turn_id ? turn_id : "";
    const char *playback = playback_id ? playback_id : "";
    bool cleared = false;
    int64_t playback_begin_rx_us = 0;
    uint32_t stale_generation = 0U;

    speaker_lock();
    const bool ids_match =
        s_speaker_ctx.active_owner ==
            NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION &&
        turn[0] != '\0' && playback[0] != '\0' &&
        strcmp(turn, s_speaker_ctx.active_turn_id) == 0 &&
        strcmp(playback, s_speaker_ctx.active_playback_id) == 0;
    const bool metadata_only =
        s_speaker_ctx.playback_session_active &&
        !s_speaker_ctx.playback_active &&
        !s_speaker_ctx.start_gate_pending &&
        !s_speaker_ctx.start_gate_authorized &&
        !s_speaker_ctx.stream.active &&
        !s_speaker_ctx.stop_fault_active &&
        !s_speaker_ctx.fade_cancel_active &&
        !s_speaker_ctx.runtime_reset_in_progress &&
        s_speaker_ctx.io_claim_generation == 0U &&
        s_speaker_ctx.maintenance_claim_generation == 0U &&
        s_speaker_ctx.completion_claim_generation == 0U &&
        s_speaker_ctx.cancel_pending_generation == 0U &&
        s_speaker_ctx.output_stop_owner_generation == 0U &&
        speaker_packet_ring_count() == 0U;
    const bool expired = timeout_ms == 0U ||
        (s_speaker_ctx.playback_begin_rx_us > 0 &&
         esp_timer_get_time() - s_speaker_ctx.playback_begin_rx_us >=
             (int64_t)timeout_ms * 1000LL);
    if (ids_match && metadata_only && expired) {
        if (timeout_ms != 0U) {
            /* Fence in-flight RX before retiring metadata. The main owner will
             * also close its admission and retire the turn in the same pass. */
            s_speaker_ctx.downlink_admission_allowed = false;
        }
        playback_begin_rx_us = s_speaker_ctx.playback_begin_rx_us;
        stale_generation = s_speaker_ctx.submitted_generation;
        s_speaker_ctx.playback_session_active = false;
        s_speaker_ctx.active_owner = NUWAX_SPEAKER_PLAYBACK_OWNER_NONE;
        s_speaker_ctx.active_turn_id[0] = '\0';
        s_speaker_ctx.active_notify_id[0] = '\0';
        s_speaker_ctx.active_playback_id[0] = '\0';
        s_speaker_ctx.playback_begin_rx_us = 0;
        s_speaker_ctx.first_pcm_rx_us = 0;
        s_speaker_ctx.start_gate_authorized_us = 0;
        s_speaker_ctx.startup_watermark_ready_us = 0;
        s_speaker_ctx.first_i2s_write_us = 0;
        s_speaker_ctx.metadata_expected_frames = 0U;
        s_speaker_ctx.metadata_expected_bytes = 0U;
        speaker_clear_end_seen_locked();
        speaker_reset_stream_runtime_locked(true);
        cleared = true;
    }
    speaker_unlock();

    if (cleared) {
        ESP_LOGW(TAG,
                 "speaker metadata-only playback aborted turn=%s playback=%s stale_generation=%lu age_ms=%lld reason=%s",
                 turn,
                 playback,
                 (unsigned long)stale_generation,
                 playback_begin_rx_us > 0
                     ? (long long)((esp_timer_get_time() -
                                    playback_begin_rx_us) /
                                   1000LL)
                     : -1LL,
                 reason ? reason : "-");
    }
    return cleared;
}
bool nuwax_speaker_playback_abort_metadata_only_session(
    const char *turn_id, const char *playback_id, const char *reason) {
    return speaker_abort_metadata_only_session(turn_id, playback_id, reason, 0U);
}
bool nuwax_speaker_playback_expire_metadata_only_session(
    const char *turn_id, const char *playback_id, uint32_t timeout_ms) {
    return timeout_ms != 0U && speaker_abort_metadata_only_session(
        turn_id, playback_id, "first_pcm_timeout", timeout_ms);
}
static bool speaker_wait_for_jitter_watermark(
    const speaker_playback_packet_t *packet,
    bool playback_started,
    uint32_t held_samples,
    uint32_t *waited_ms_out) {
    if (!packet || packet->generation == 0U) {
        return false;
    }

    const uint32_t wait_started_ms = monotonic_ms();
    uint32_t last_ingress_progress_ms = wait_started_ms;
    uint32_t last_ingress_frames = 0U;
    bool recoverable_stall_warned = false;
    bool rebuffer_stall_warned = false;
    while (true) {
        const uint32_t now_ms = monotonic_ms();
        bool generation_current = false;
        bool end_accepted = false;
        bool rebuffering = false;
        bool ready = false;
        uint32_t queued_records = 0U;
        uint32_t queued_samples = 0U;
        uint32_t ingress_frames = 0U;
        uint32_t expected_sequence = 0U;
        bool forced_resume = false;
        bool startup_expired = false;
        speaker_fault_notice_t startup_fault = {0};
        uint32_t startup_target_ms =
            NUWAX_SPEAKER_JITTER_STARTUP_BASE_MS;
        uint32_t resume_target_ms = NUWAX_SPEAKER_JITTER_RESUME_MIN_MS;
        const uint32_t waited_from_entry_ms = now_ms - wait_started_ms;
        speaker_lock();
        generation_current =
            speaker_generation_source_matches_locked(packet->connection_epoch,
                                                     packet->generation) &&
            !s_speaker_ctx.drop_until_end &&
            !s_speaker_ctx.stream_protocol_faulted &&
            s_speaker_ctx.stream.active &&
            s_speaker_ctx.stream.generation == packet->generation &&
            (!playback_started ||
             (s_speaker_ctx.playback_active &&
              s_speaker_ctx.playback_connection_epoch ==
                  packet->connection_epoch &&
              s_speaker_ctx.playback_generation == packet->generation));
        end_accepted = generation_current &&
                       s_speaker_ctx.stream.end_accepted;
        rebuffering = generation_current && s_speaker_ctx.jitter_rebuffering;
        ingress_frames = s_speaker_ctx.stream.pcm_frames;
        expected_sequence = s_speaker_ctx.stream.next_sequence;
        startup_target_ms = s_speaker_ctx.jitter_startup_target_ms;
        resume_target_ms = s_speaker_ctx.jitter_resume_target_ms;
        queued_records = speaker_packet_ring_count() +
                         (held_samples > 0U ? 1U : 0U);
        queued_samples = speaker_packet_ring_samples() + held_samples;
        ready = generation_current && nuwax_speaker_jitter_ready_ms(
                                         playback_started,
                                         end_accepted,
                                         queued_records,
                                         queued_samples,
                                         packet->sample_rate,
                                         startup_target_ms,
                                         resume_target_ms);
        forced_resume = generation_current &&
                        nuwax_speaker_jitter_force_resume(
                            playback_started,
                            rebuffering,
                            waited_from_entry_ms,
                            queued_records,
                            queued_samples,
                            NUWAX_SPEAKER_JITTER_FORCE_RESUME_WAIT_MS);
        if (forced_resume) {
            /* Keep generation/order validation intact, but let the consumer
             * resume with the available PCM. Waiting indefinitely for a full
             * watermark would fill the ingress ring and block WebSocket RX. */
            ready = true;
        }
        if (ready && playback_started &&
            s_speaker_ctx.jitter_rebuffering) {
            const uint32_t rebuffer_waited_ms =
                now_ms - s_speaker_ctx.jitter_rebuffer_started_ms;
            const bool midstream =
                nuwax_speaker_jitter_rebuffer_is_midstream(
                    s_speaker_ctx.jitter_rebuffer_entry_pcm_frames,
                    ingress_frames);
            if (midstream) {
                s_speaker_ctx.jitter_rebuffer_events++;
                s_speaker_ctx.jitter_resume_target_ms =
                    nuwax_speaker_jitter_resume_target_with_gap(
                        s_speaker_ctx.jitter_rebuffer_events,
                        NUWAX_SPEAKER_JITTER_RESUME_MIN_MS,
                        NUWAX_SPEAKER_JITTER_RESUME_MAX_MS,
                        s_speaker_ctx.ingress_max_gap_ms,
                        NUWAX_SPEAKER_JITTER_RESUME_GAP_CUSHION_MS);
                s_speaker_ctx.jitter_rebuffer_wait_ms += rebuffer_waited_ms;
                if (rebuffer_waited_ms >
                    s_speaker_ctx.jitter_rebuffer_max_wait_ms) {
                    s_speaker_ctx.jitter_rebuffer_max_wait_ms =
                        rebuffer_waited_ms;
                }
                s_speaker_ctx.jitter_resume_fade_pending = true;
            } else {
                s_speaker_ctx.jitter_tail_wait_events++;
                s_speaker_ctx.jitter_tail_wait_ms += rebuffer_waited_ms;
            }
            s_speaker_ctx.jitter_rebuffering = false;
            s_speaker_ctx.jitter_rebuffer_started_ms = 0U;
            s_speaker_ctx.jitter_rebuffer_entry_pcm_frames = 0U;
            s_speaker_ctx.jitter_empty_started_ms = 0U;
        }
        if (ready && !playback_started) {
            s_speaker_ctx.startup_watermark_ready_us = esp_timer_get_time();
        }
        /* An absolute startup budget cannot be renewed by sparse PCM. Ready
         * (including a short response with END) wins at the boundary. Bind and
         * fault under this lock so concurrent END/PCM cannot slip between the
         * readiness decision and cancellation. Physical stop uses the existing
         * generation/epoch-bound terminal path on the next worker pass. */
        const int64_t first_pcm_us = s_speaker_ctx.first_pcm_rx_us;
        const uint32_t startup_age_ms = first_pcm_us > 0
            ? (uint32_t)((esp_timer_get_time() - first_pcm_us) / 1000LL)
            : waited_from_entry_ms;
        startup_expired = generation_current &&
            s_speaker_ctx.active_owner == NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION &&
            nuwax_speaker_jitter_startup_expired(playback_started, ready,
                                                startup_age_ms);
        if (startup_expired) {
            startup_fault = speaker_claim_protocol_fault_locked(
                packet->connection_epoch, packet->generation, true);
        }
        speaker_unlock();
        if (startup_expired) {
            speaker_publish_protocol_fault(packet->connection_epoch,
                                           packet->generation, ESP_ERR_TIMEOUT,
                                           "startup_buffer_timeout", startup_fault);
            return false;
        }

        if (ingress_frames != last_ingress_frames) {
            last_ingress_frames = ingress_frames;
            last_ingress_progress_ms = now_ms;
        }

        if (!generation_current) {
            return false;
        }
        if (ready) {
            if (waited_ms_out) {
                *waited_ms_out = waited_from_entry_ms;
            }
            ESP_LOGI(TAG,
                     "speaker jitter watermark ready generation=%lu phase=%s queued_records=%lu queued_samples=%lu queued_ms=%lu target_ms=%u end_accepted=%d forced_resume=%d waited_ms=%lu",
                     (unsigned long)packet->generation,
                     playback_started ? "resume" : "startup",
                     (unsigned long)queued_records,
                     (unsigned long)queued_samples,
                     (unsigned long)nuwax_speaker_jitter_audio_ms(
                         queued_samples, packet->sample_rate),
                     (unsigned)(playback_started
                                    ? resume_target_ms
                                    : startup_target_ms),
                     end_accepted ? 1 : 0,
                     forced_resume ? 1 : 0,
                     (unsigned long)waited_from_entry_ms);
            return true;
        }
        if (speaker_process_pending_protocol_fault()) {
            return false;
        }
        (void)speaker_restore_probe_duck_if_due(now_ms);
        char fade_source[sizeof(s_speaker_ctx.fade_cancel_source)] = {0};
        uint32_t fade_generation = 0U;
        if (speaker_fade_cancel_due(now_ms,
                                    fade_source,
                                    sizeof(fade_source),
                                    &fade_generation)) {
            (void)speaker_cancel_now_if_current_epoch(
                packet->connection_epoch,
                fade_generation,
                fade_source[0] ? fade_source : "fade_deadline");
            return false;
        }
        const uint32_t stalled_ms = now_ms - last_ingress_progress_ms;
        if (playback_started && rebuffering && !end_accepted &&
            stalled_ms >= NUWAX_SPEAKER_REBUFFER_STALL_WARN_MS &&
            !rebuffer_stall_warned) {
            rebuffer_stall_warned = true;
            recoverable_stall_warned = true;
            ESP_LOGW(TAG,
                     "speaker jitter rebuffer still waiting for cloud PCM generation=%lu stalled_ms=%lu hard_timeout_ms=%u expected_seq=%lu",
                     (unsigned long)packet->generation,
                     (unsigned long)stalled_ms,
                     (unsigned)NUWAX_SPEAKER_REBUFFER_STALL_TIMEOUT_MS,
                     (unsigned long)expected_sequence);
        }
        if (playback_started && rebuffering && !end_accepted &&
            stalled_ms >= NUWAX_SPEAKER_REBUFFER_STALL_TIMEOUT_MS) {
            ESP_LOGE(TAG,
                     "speaker jitter rebuffer timed out generation=%lu stalled_ms=%lu timeout_ms=%u; aborting generation while retaining WebSocket",
                     (unsigned long)packet->generation,
                     (unsigned long)stalled_ms,
                     (unsigned)NUWAX_SPEAKER_REBUFFER_STALL_TIMEOUT_MS);
            speaker_schedule_protocol_fault(packet->connection_epoch,
                                            packet->generation,
                                            ESP_ERR_TIMEOUT,
                                            "jitter_rebuffer_timeout",
                                            true);
            return false;
        }
        const nuwax_speaker_stall_action_t stall_action =
            nuwax_speaker_stall_action(
                stalled_ms,
                NUWAX_SPEAKER_STREAM_STALL_WARN_MS,
                NUWAX_SPEAKER_STREAM_STALL_TIMEOUT_MS,
                recoverable_stall_warned);
        if (stall_action == NUWAX_SPEAKER_STALL_WARN) {
            recoverable_stall_warned = true;
            ESP_LOGW(TAG,
                     "speaker downlink PCM paused; keeping generation recoverable generation=%lu stalled_ms=%lu hard_timeout_ms=%u expected_seq=%lu",
                     (unsigned long)packet->generation,
                     (unsigned long)stalled_ms,
                     (unsigned)NUWAX_SPEAKER_STREAM_STALL_TIMEOUT_MS,
                     (unsigned long)expected_sequence);
        } else if (stall_action == NUWAX_SPEAKER_STALL_ABORT) {
            speaker_schedule_protocol_fault(packet->connection_epoch,
                                            packet->generation,
                                            ESP_ERR_TIMEOUT,
                                            "pcm_progress_hard_stall",
                                            true);
            return false;
        }
        (void)ulTaskNotifyTake(
            pdTRUE,
            speaker_positive_delay_ticks(NUWAX_SPEAKER_JITTER_POLL_MS));
    }
}
static void reset(void){
 memset(&s_speaker_ctx,0,sizeof(s_speaker_ctx));
 now_us=1000000;start_ms=monotonic_ms();records=17;samples=17*320;
 notices=cancels=polls=mode=0;fade=locked=false;
 s_speaker_ctx.submitted_generation=9;s_speaker_ctx.submitted_connection_epoch=2;
 s_speaker_ctx.active_owner=NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION;
 s_speaker_ctx.stream.active=true;s_speaker_ctx.stream.generation=9;
 s_speaker_ctx.stream.pcm_frames=18;s_speaker_ctx.stream.pcm_bytes=18*640;
 s_speaker_ctx.first_pcm_rx_us=now_us;
 s_speaker_ctx.jitter_startup_target_ms=800;s_speaker_ctx.jitter_resume_target_ms=640;
}
static bool wait_start(void){
 speaker_playback_packet_t p={.connection_epoch=2,.generation=9,.sample_rate=16000};
 uint32_t waited=0;return speaker_wait_for_jitter_watermark(&p,false,320,&waited);
}
static void metadata(void){
 reset();records=samples=0;memset(&s_speaker_ctx.stream,0,sizeof(s_speaker_ctx.stream));
 s_speaker_ctx.first_pcm_rx_us=0;s_speaker_ctx.drop_until_end=true;
 s_speaker_ctx.downlink_admission_allowed=true;
 nuwax_speaker_playback_note_playback_begin("turn","pb",0,0);
}

int main(void){
 reset();mode=10;s_speaker_ctx.active_owner=2;
 tx=(nuwax_notification_transaction_t){.active=true,.token=1,.ws_epoch=2,
       .state=NUWAX_NOTIFICATION_STATE_STREAMING,.deadline_ms=monotonic_ms()+10000};
 assert(wait_start());assert(notices==0);
 assert(monotonic_ms()-start_ms==198000);
 printf("notification startup_wait_ms=%u faults=%u transaction_timeout=0\n",monotonic_ms()-start_ms,notices);
}
