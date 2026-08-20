#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "conversation/wake_word_config.h"
#include "esp_err.h"

typedef struct {
    bool enabled;
    bool initialized;
    uint32_t detections;
    uint32_t weak_rejections;
    uint32_t partial_reads;
    uint32_t read_timeouts;
    uint32_t input_gaps;
    uint32_t max_feed_gap_ms;
    char active_model[64];
    char active_command[64];
    uint8_t active_variant_count;
    char last_wake_word[64];
    char backend[16];
    int feed_chunksize;
    int sample_rate;
} nuwax_wake_word_status_t;

typedef enum {
    NUWAX_WAKE_MODE_OFF = 0,
    NUWAX_WAKE_MODE_SLEEP,
    NUWAX_WAKE_MODE_PLAYBACK_INTERRUPT,
} nuwax_wake_mode_t;

typedef enum {
    NUWAX_WAKE_PLAYBACK_OWNER_CONVERSATION = 0,
    NUWAX_WAKE_PLAYBACK_OWNER_NOTIFICATION,
} nuwax_wake_playback_owner_t;

typedef struct {
    nuwax_wake_mode_t mode;
    uint32_t mode_token;
    uint32_t ws_epoch;
    uint32_t speaker_generation;
    nuwax_wake_playback_owner_t playback_owner;
    char turn_id[48];
    char notify_id[48];
    char playback_id[48];
} nuwax_wake_arm_context_t;

typedef struct {
    nuwax_wake_arm_context_t arm;
    char wake_word[64];
    uint32_t probability_q1000;
    uint32_t detected_ms;
    int64_t detected_us;
    uint32_t reference_generation;
    uint32_t reference_correlation_q15;
    uint32_t mic1_avg_abs;
    uint32_t mic1_peak;
    uint32_t mic2_avg_abs;
    uint32_t mic2_peak;
    uint32_t diff_avg_abs;
    uint32_t diff_peak;
    bool reference_available;
    bool reference_active;
    bool echo_likely;
    bool nearfield_safe;
    bool probe_requested;
    bool action_eligible;
} nuwax_wake_word_result_t;

typedef void (*nuwax_wake_word_callback_t)(
    const nuwax_wake_word_result_t *result,
    void *user_ctx);

#define NUWAX_WAKE_WORD_STOP_TIMEOUT_MS 5000U

esp_err_t nuwax_wake_word_detector_init(nuwax_wake_word_callback_t callback, void *user_ctx);
esp_err_t nuwax_wake_word_detector_start(void);
esp_err_t nuwax_wake_word_detector_start_with_context(
    const nuwax_wake_arm_context_t *context);
/* Non-blocking. The worker releases the microphone before publishing exit. */
void nuwax_wake_word_detector_request_stop(void);
/* Synchronously reaps the suspended worker when called outside its callback. */
void nuwax_wake_word_detector_stop(void);
/* Bounded variant for resource-owner transitions. A timeout means another
 * microphone owner must not start on the current runtime. */
esp_err_t nuwax_wake_word_detector_stop_with_timeout(uint32_t timeout_ms);
/* False means the owner has deleted the task and its static storage is reusable. */
bool nuwax_wake_word_detector_is_running(void);
/* True only after the worker has acquired the shared microphone and is ready
 * to consume PCM.  A created FreeRTOS task is not sufficient because the
 * conversation capture path owns the same I2S resource. */
bool nuwax_wake_word_detector_is_ready(void);
void nuwax_wake_word_detector_deinit(void);
/* Update the MultiNet command safely. persist=true stores the command in NVS
 * and makes it the command used after reboot. The old command is restored if
 * model parsing, persistence, or detector restart fails. */
esp_err_t nuwax_wake_word_detector_set_command(const char *command,
                                               bool persist);
/* Applies exactly the normalized variants in config. All variants share one
 * MultiNet command id, so callbacks and conversation state remain unchanged. */
esp_err_t nuwax_wake_word_detector_set_config(
    const nuwax_wake_word_config_t *config,
    bool persist);
/* Restores the firmware default command set and removes only the wake-word
 * NVS override. Other provisioning and device settings remain untouched. */
esp_err_t nuwax_wake_word_detector_reset_to_default(void);
void nuwax_wake_word_detector_get_status(nuwax_wake_word_status_t *out_status);
