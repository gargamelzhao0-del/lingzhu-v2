/* RTOS/media effects only are mocked; the classifier is extracted verbatim. */
#include "sd_audio_file_fault.h"
static sd_audio_file_fault_t s_card_file_fault[2];
static atomic_uint s_media_generation = 1U;
static atomic_bool s_media_ready = true, s_replay_media_fault, s_card_failed;
static atomic_bool s_card_partial;
static sd_audio_damage_window_t s_card_damage __attribute__((unused));
static sd_audio_session_t s_card_session;
static bool fault_media_healthy = true, fault_active_capture, fault_stop_requested;
static bool nuwax_sd_card_is_mounted(void) { return fault_media_healthy; }
static bool nuwax_sd_session_active(void) { return fault_active_capture; }
static void nuwax_sd_card_mark_io_fault(esp_err_t err) { assert(err!=ESP_OK);fault_media_healthy=false; }
static void notify_writer_task(void) {}
static void nuwax_offline_audio_supervisor_wake(void) {}
static esp_err_t nuwax_offline_audio_request_stop(bool complete) { assert(!complete);fault_stop_requested=true;return ESP_OK; }
#ifndef SD_FILE_FAULT_STATE_ONLY
#include "sd_file_fault_production_host.inc"
#endif
