#pragma once

#include <stdbool.h>

void nuwax_diagnostics_log_heap(const char *tag);
void nuwax_diagnostics_log_heap_label(const char *tag, const char *label);
void nuwax_diagnostics_log_current_task_stack(const char *tag, const char *label);

/*
 * P3-0: structured AEC ref health log line (one-liner for script parsing).
 * Fields: status, score, tone_amp, quiet_rms, corr, delay, clip_ratio, slot0_rms, passed.
 */
void nuwax_diagnostics_log_aec_ref_health(const char *tag,
                                           int status,
                                           float score,
                                           float tone_amp,
                                           float quiet_rms,
                                           float corr,
                                           int delay_samples,
                                           float clip_ratio,
                                           int slot0_rms,
                                           bool passed);
