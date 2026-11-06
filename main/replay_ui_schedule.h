#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Process lifetime observer; no task, timer, allocation or wait in the hook. */
void nuwax_replay_schedule_init(void);
bool nuwax_replay_schedule_snapshot(uint32_t *idle_epoch, uint32_t *ui_epoch);
/* Pinned sender, outside transport locks: yield until this core's IDLE progress or
 * one tick. True means observed idle progress, not a hardware speed claim. */
bool nuwax_replay_schedule_yield(void);
typedef enum {
    NUWAX_REPLAY_UI_HELP_NONE = 0,
    NUWAX_REPLAY_UI_HELP_REQUESTED,
    NUWAX_REPLAY_UI_HELP_READY,
    NUWAX_REPLAY_UI_HELP_EXPIRED,
    NUWAX_REPLAY_UI_HELP_CANCELLED,
} nuwax_replay_ui_help_state_t;
/* Only the UI owner publishes grants/outcomes. Expiry/cancel never means a
 * decoded frame is ready. A snapshot is diagnostic, not fairness credit. */
/* until_us is the low 32 bits of the owner's original esp_timer deadline,
 * required only for a new REQUESTED grant. Republishing cannot renew it. */
void nuwax_replay_ui_set_help_state(nuwax_replay_ui_help_state_t state, uint32_t until_us);
nuwax_replay_ui_help_state_t nuwax_replay_ui_help_snapshot(uint32_t *generation, uint32_t *until_us);
void nuwax_replay_ui_frame_done(void);
/* Priority-5 sender only, outside transport locks. At most one tick per call;
 * pending work remains eligible on a later batch within the ORIGINAL deadline.
 * Only an outcome/deadline consumes the generation, not a partial wait. Tick
 * quantization/preemption can delay return; no new wait starts after expiry.
 * Sender stays at 5, reader at 6. Never use UI completion as IDLE credit. */
bool nuwax_replay_ui_handoff(uint32_t *served_generation);

/* Serialized replay reader only. Track the short-lived internal DMA buffer,
 * never PSRAM or a retained allocation. UI reads a bounded, coherent hint;
 * this does not reserve memory or authorize any allocation. */
void nuwax_replay_dma_loan_begin(uint32_t bytes, uint32_t now_us);
void nuwax_replay_dma_loan_end(void);
uint32_t nuwax_replay_dma_loan_bytes(uint32_t now_us);
