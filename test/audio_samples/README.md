# Nuwax AI Partner 7.0 Audio Sample Library

This directory is the fixed 50/50/30 validation library for smart interrupt.
Large audio files may live in Git LFS or external storage, but every sample must
have one JSONL entry in `manifest.jsonl`.

## Targets

| Category | Development target | Final target | Expected behavior |
| --- | ---: | ---: | --- |
| `interrupt` | 10 | 50 | Near-end speech during playback should become a record candidate. Gray cancel samples may send `cancel_turn` only when explicitly requested and `p6_cancel_candidate=1`. |
| `non_interrupt` | 10 | 50 | Playback-only, silence, or device self-playback must not create `p6_cancel_candidate` and must not cancel. Broad record candidates may be logged for analysis. |
| `noise_background` | 5 | 30 | Background speech/noise not directed at the device should not cancel the active turn. |

## Directory Layout

```text
audio_samples/
  interrupt/           # near-end user speech intended to interrupt
  non_interrupt/       # silence/playback-only/non-command speech
  noise_background/    # TV/music/ambient/other people not targeting device
  edge/                # optional edge cases, not counted toward 50/50/30 unless category says so
  manifest.jsonl       # one JSON object per sample
  manifest.schema.json # schema for each manifest line
```

## Sample ID Rules

- `INT-0001` to `INT-9999`: `interrupt`
- `NON-0001` to `NON-9999`: `non_interrupt`
- `NOI-0001` to `NOI-9999`: `noise_background`
- `EDG-0001` to `EDG-9999`: optional edge cases

## Required Capture Protocol

- Board: CyberVoc V1.2.
- Firmware route: P3-0S software reference, `aec=false`, L3/L4 default off.
- Audio: 16 kHz mono WAV when a raw audio artifact is available.
- Distances: 20 cm, 50 cm, and 100-200 cm should all be represented.
- Playback volume baseline: 20 percent, unless the sample explicitly tests volume sensitivity.
- Each run must keep the related runtime log path.

## Validation

Run the current development gate:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\check-audio-sample-manifest.ps1
```

Run the final 50/50/30 gate:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\check-audio-sample-manifest.ps1 -Gate final
```

## Registering a Sample

Do not edit `manifest.jsonl` by hand for normal collection. Use:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\add-audio-sample-manifest-entry.ps1 `
  -Category interrupt `
  -Scenario nearend_interrupt_word `
  -DistanceCm 20 `
  -Ambient quiet `
  -VolumePercent 20 `
  -RuntimeLog runtime-logs\YOUR_RUNTIME_LOG.log `
  -LabelStatus labeled `
  -ExpectedCandidate true `
  -ExpectedCancelCandidate true `
  -ExpectedCancel true
```

For playback-only or other no-cancel samples:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\add-audio-sample-manifest-entry.ps1 `
  -Category non_interrupt `
  -Scenario playback_only `
  -DistanceCm 0 `
  -Ambient quiet `
  -VolumePercent 20 `
  -RuntimeLog runtime-logs\YOUR_RUNTIME_LOG.log `
  -LabelStatus labeled `
  -ExpectedCandidate false `
  -ExpectedCancelCandidate false `
  -ExpectedCancel false
```

For P6 gray cancel boundary samples, `expected_candidate` may be `true` even
for `non_interrupt` when the broad record candidate is intentionally observed.
The safety requirement is `expected_cancel_candidate=false` and
`expected_cancel=false`.

The registration script auto-generates the next sample id, fills `audio_path`,
and extracts observed fields from the runtime log when possible:

- `observed_candidate`
- `observed_cancel_candidate`
- `observed_cancel`
- `pass`
- `metrics.erle_db`
- `metrics.active_ref_ratio`
- `metrics.candidate_to_cancel_ms`
- `metrics.process_min_internal`

## First Collection Batch

Collect in small batches and run the validator after every batch.

| Batch | Category | Count | Scenarios | Distances |
| --- | --- | ---: | --- | --- |
| B1 | `interrupt` | 10 | `nearend_interrupt_word`, `nearend_question_change`, `nearend_negation` | 20 cm, 50 cm |
| B2 | `non_interrupt` | 10 | `playback_only` | 0 cm |
| B3 | `noise_background` | 10 | `background_speech`, `tv_music`, `ambient_noise` | 100-200 cm |
| B4 | `interrupt` | 15 | `nearend_long_sentence`, `edge_low_voice`, `edge_far_field` | 50 cm, 100-200 cm |
| B5 | `non_interrupt` | 20 | `playback_only`, `tv_music`, `ambient_noise` | 0 cm, 100-200 cm |
| B6 | remaining | 65 | Fill weak scenarios and failed/rejected gaps | Mixed |

Current development gate:

- `interrupt >= 10`
- `non_interrupt >= 10`
- `noise_background >= 5`
- every counted sample has `label_status="labeled"`
- every counted sample has expected candidate/cancel fields

Final release gate:

- `interrupt >= 50`
- `non_interrupt >= 50`
- `noise_background >= 30`
- every counted sample has `label_status="labeled"`
- every counted sample has expected candidate/cancel fields

## Current Status

As of 2026-06-30 14:35, the reduced development library has been registered
from the P6 gray cancel `-4.0dB` closure, B1/B2 collection, and B3
noise/background collection:

- `interrupt`: 10/10 labeled for development, 10/50 for final
- `non_interrupt`: 10/10 labeled for development, 10/50 for final
- `noise_background`: 5/5 labeled for development, 5/30 for final

The 10/10/5 development gate is PASS. The final 50/50/30 gate is still not
ready. `NOI-0001` is a rejected placeholder and is not counted; `NOI-0002` to
`NOI-0006` are the counted `noise_background` samples.
