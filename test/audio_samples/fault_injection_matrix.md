# P8 Fault Injection Matrix

This matrix is the next validation track after P3-0S fast path 2h soak.
Run these tests only with the current P3-0S route unless a row explicitly says
otherwise. Do not set `aec=true`; do not enable DOA.

| ID | Fault | Setup | Expected result | Evidence |
| --- | --- | --- | --- | --- |
| F1 | Gateway restart during idle | Device online, no playback, restart cloud gateway | Device reconnects, `/api/status` shows one device, no crash | status before/after, serial log |
| F2 | Gateway restart during playback | Trigger TTS playback, restart cloud gateway | Playback stops or completes cleanly; device recovers WS without WDT | serial log, gateway log |
| F3 | WS unreachable retry | Stop gateway for 2-3 reconnect intervals | Device reports cloud unreachable/recovery, no infinite tight retry | serial log |
| F4 | Wi-Fi fallback | Make primary SSID unreachable if test environment allows | Device falls back to configured secondary SSID and persists last working slot | status JSON, serial log |
| F5 | Playback-only no-cancel | Run playback-only P6/P3 smoke in quiet room | p6_cancel_candidate=0, cancel_sent=0; broad candidate may be logged as observation | runtime log |
| F6 | Near-end gray cancel | Explicit gray cancel test with user speech | p6_cancel_candidate=1, cancel_sent=1 only in explicit gray flow | runtime log, cloud ack |
| F7 | Repeated cancel | Run 5-10 explicit gray cancel samples | No stale audio replay, no queue growth, no crash | runtime logs |
| F8 | Low battery / high volume | Battery below normal operating threshold, volume sensitivity check | Clear degradation or warning; no brownout loop | battery/status log |
| F9 | ASR/TTS provider timeout | Cloud-side timeout/error simulation | Device returns recoverable state; no stuck speaking/listening | gateway log, serial log |
| F10 | Long idle after tests | 30 min idle after P6/P3 run | No heap/DMA trend down, no reconnect storm | serial log |

Minimum P8 development gate: F1, F3, F5, F6, and F10 must pass before broader user sampling.

## Status 2026-06-29

| ID | Status | Evidence |
| --- | --- | --- |
| F1 | Passed | Valid rerun `runtime-logs/nuwax7-p8-p8-f1-gateway-restart-idle-norst-r2-20260629-143958.log`; final `device_count=1`, `fatal_count=0`, `reconnect_line_count=2`, `result=pass`. First run `...idle-after-f10-20260629-142922.log` is not counted because the old runner accepted a transient reconnect while final status had `device_count=0`. |
| F3 | Passed | `runtime-logs/nuwax7-p8-p8-f3-ws-unreachable-retry-norst-r1-20260629-144343.log`; gateway down 90s, final `device_count=1`, `fatal_count=0`, `reconnect_line_count=8`, `result=pass`. |
| F5 | Passed | `runtime-logs/nuwax7-p8-f5-playbackonly-nocancel-cancelgate2-r1-20260629-102358.log`, `...r2-20260629-102515.log`, `...r3-20260629-102553.log`; `p6_cancel_candidate=0`, `cancel_sent=0`. |
| F6 | Passed | `runtime-logs/nuwax7-p8-f6-nearend-graycancel-cancelgate2-user-r2-short-20260629-134329.log`; `p6_cancel_candidate=1`, `cancel_sent=1`, `cancel_turn_ack ok=true`. |
| F10 | Passed | `runtime-logs/nuwax7-p8-p8-f10-idle-after-f6-30min-20260629-135659.log`; 30 min idle, before/after `device_count=1`, `fatal_count=0`, `reconnect_line_count=0`, `result=pass`. |

Current minimum P8 development gate status: 5/5 passed. This only closes the minimum fault-injection gate; the 50/50/30 real sample gate remains NOT READY.
