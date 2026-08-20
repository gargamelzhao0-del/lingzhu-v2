param(
    [string]$S40FarLog,
    [string]$S42FarLog,
    [string]$S42HumanLog,
    [string]$S43FarLog,
    [string]$S44HumanLog,
    [string]$S45FarLog,
    [string]$S46HumanLog,
    [string]$S47FarLog
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent $projectRoot
if (-not $S40FarLog) {
    $S40FarLog = Join-Path $workspaceRoot 'runtime-logs\nuwax7-serial-20260714-124644.log'
}
if (-not $S42FarLog) {
    $S42FarLog = Join-Path $workspaceRoot 'runtime-logs\nuwax7-serial-20260714-141645.log'
}
if (-not $S42HumanLog) {
    $S42HumanLog = Join-Path $workspaceRoot 'runtime-logs\nuwax7-serial-20260714-141912.log'
}
if (-not $S43FarLog) {
    $S43FarLog = Join-Path $workspaceRoot 'runtime-logs\nuwax7-serial-20260714-163200.log'
}
if (-not $S44HumanLog) {
    $S44HumanLog = Join-Path $workspaceRoot 'runtime-logs\nuwax7-serial-20260714-163919.log'
}
if (-not $S45FarLog) {
    $S45FarLog = Join-Path $workspaceRoot 'runtime-logs\nuwax7-serial-20260714-170706.log'
}
if (-not $S46HumanLog) {
    $S46HumanLog = Join-Path $workspaceRoot 'runtime-logs\nuwax7-serial-20260714-172012.log'
}
if (-not $S47FarLog) {
    $S47FarLog = Join-Path $workspaceRoot 'runtime-logs\nuwax7-serial-20260714-173456.log'
}

$CandidateMaxGapFrames = 48
$ProvisionalHoldFrames = 8
$ConfirmedHangoverFrames = 256
$FastHoldFrames = $CandidateMaxGapFrames + 1

function Read-ProtectionFixture {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Replay log not found: $Path"
    }

    $events = @{}
    $playbackFrames = 0
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match 'playback human strong seq=') {
            $fields = @{}
            foreach ($match in [regex]::Matches($line, '(?<key>[A-Za-z0-9_]+)=(?<value>-?\d+)')) {
                $fields[$match.Groups['key'].Value] = [int64]$match.Groups['value'].Value
            }
            if ($fields.ContainsKey('seq')) {
                $events[[int]$fields['seq']] = $fields
            }
        }
        if ($line -match 'I2S microphone stream done .*aec_playback_frames=(\d+)') {
            $playbackFrames = [int]$Matches[1]
        }
    }

    if ($playbackFrames -le 0) {
        throw "aec_playback_frames was not found in: $Path"
    }
    return [pscustomobject]@{
        Path = $Path
        Events = $events
        PlaybackFrames = $playbackFrames
    }
}

function Get-Field {
    param(
        [hashtable]$Fields,
        [Parameter(Mandatory)][string]$Name
    )
    if ($null -ne $Fields -and $Fields.ContainsKey($Name)) {
        return [int64]$Fields[$Name]
    }
    return [int64]0
}

function Test-LowLevelAnchor {
    param([hashtable]$Fields)

    if ($null -eq $Fields) {
        return $false
    }
    $mic1Peak = Get-Field $Fields 'mic1_peak'
    $diffPeak = Get-Field $Fields 'diff_peak'
    return (Get-Field $Fields 'seed_evidence') -eq 1 -and
        (Get-Field $Fields 'aec') -eq 1 -and
        (Get-Field $Fields 'mic1_avg') -ge 650 -and
        $mic1Peak -ge 2300 -and
        (Get-Field $Fields 'diff_avg') -ge 800 -and
        $diffPeak -ge 3000 -and
        (Get-Field $Fields 'diff_energy_q15') -ge 28000 -and
        (Get-Field $Fields 'balance_q15') -ge 23000 -and
        (Get-Field $Fields 'balance_q15') -le 30000 -and
        (Get-Field $Fields 'ref_corr_q15') -le 3000 -and
        ([int64]$diffPeak * 32768) -ge ([int64]$mic1Peak * 37683)
}

function Test-VoiceAnchor {
    param(
        [hashtable]$Fields,
        [int]$PriorCandidateCount
    )

    if ($null -eq $Fields) {
        return $false
    }
    $mic1Peak = Get-Field $Fields 'mic1_peak'
    $diffPeak = Get-Field $Fields 'diff_peak'
    return (Get-Field $Fields 'seed_evidence') -eq 1 -and
        (Get-Field $Fields 'voice') -eq 1 -and
        (Get-Field $Fields 'aec') -eq 1 -and
        $PriorCandidateCount -ge 2 -and
        (Get-Field $Fields 'mic1_avg') -ge 1400 -and
        $mic1Peak -ge 6000 -and
        (Get-Field $Fields 'diff_avg') -ge 650 -and
        $diffPeak -ge 2500 -and
        (Get-Field $Fields 'ref_corr_q15') -le 2000 -and
        ([int64]$diffPeak * 32768) -ge ([int64]$mic1Peak * 15000)
}

function Invoke-ProtectionReplay {
    param(
        [Parameter(Mandatory)]$Fixture,
        [Parameter(Mandatory)][ValidateSet('old', 'new')][string]$RefreshPolicy
    )

    $provisionalHoldFramesForPolicy = if ($RefreshPolicy -eq 'old') { 48 } else { $ProvisionalHoldFrames }
    $fixtureLastSeedEvidence = -1
    foreach ($entry in $Fixture.Events.GetEnumerator()) {
        if ((Get-Field ([hashtable]$entry.Value) 'seed_evidence') -eq 1 -and
            [int]$entry.Key -gt $fixtureLastSeedEvidence) {
            $fixtureLastSeedEvidence = [int]$entry.Key
        }
    }

    $candidateCount = 0
    $candidateGap = 0
    [int64]$candidateAvgSum = 0
    [int64]$candidatePeakSum = 0
    $candidateAnchorSeen = $false
    $seedSeen = $false
    $seedRemaining = 0
    $fastRemaining = 0
    $rescueMix = 0

    $anchors = 0
    $confirmEvents = 0
    $refreshEvents = 0
    $provisionalFrames = 0
    $fastFrames = 0
    $confirmedFrames = 0
    $mix25Frames = 0
    $mix75Frames = 0
    $firstConfirmed = -1
    $lastSeedEvidence = -1
    $confirmedGapFrames = 0

    for ($seq = 0; $seq -lt $Fixture.PlaybackFrames; $seq++) {
        $fields = $null
        if ($Fixture.Events.ContainsKey($seq)) {
            $fields = [hashtable]$Fixture.Events[$seq]
        }
        $seedEvidence = (Get-Field $fields 'seed_evidence') -eq 1
        $legacyAnchor = (Get-Field $fields 'spatial_anchor') -eq 1
        $newLowLevelAnchor =
            $RefreshPolicy -eq 'new' -and (Test-LowLevelAnchor $fields)
        $newVoiceAnchor =
            $RefreshPolicy -eq 'new' -and
            (Test-VoiceAnchor $fields $candidateCount)
        $spatialAnchor = $legacyAnchor -or $newLowLevelAnchor -or $newVoiceAnchor
        if ($spatialAnchor) {
            $anchors++
        }
        if ($seedEvidence) {
            $lastSeedEvidence = $seq
            $candidateCount++
            $candidateGap = 0
            $candidateAvgSum += Get-Field $fields 'mic1_avg'
            $candidatePeakSum += Get-Field $fields 'mic1_peak'
            if ($spatialAnchor) {
                $candidateAnchorSeen = $true
            }
        } elseif ($candidateCount -gt 0 -and $candidateGap -lt $CandidateMaxGapFrames) {
            $candidateGap++
        } else {
            $candidateCount = 0
            $candidateGap = 0
            $candidateAvgSum = 0
            $candidatePeakSum = 0
            $candidateAnchorSeen = $false
        }

        $windowEnergy = $candidateCount -gt 0 -and
            $candidateAvgSum -ge (500 * $candidateCount) -and
            $candidatePeakSum -ge (1800 * $candidateCount)
        $currentHumanEvidence = $seedEvidence -and
            $candidateCount -ge 2 -and
            $windowEnergy -and
            $candidateAnchorSeen
        $provisional = $candidateCount -ge 1 -and
            $candidateGap -le $provisionalHoldFramesForPolicy -and
            $candidateAvgSum -ge (450 * $candidateCount) -and
            $candidatePeakSum -ge (1400 * $candidateCount)
        $fastEvidence = $seedEvidence -and
            (Get-Field $fields 'mic1_avg') -ge 650 -and
            (Get-Field $fields 'mic1_peak') -ge 2300 -and
            $spatialAnchor
        if ($fastEvidence) {
            $fastRemaining = $FastHoldFrames
        } elseif ($fastRemaining -gt 0) {
            $fastRemaining--
        }

        $refresh = $seedSeen -and $seedEvidence -and -not $currentHumanEvidence
        if ($RefreshPolicy -eq 'old') {
            $refresh = $refresh -and ((Get-Field $fields 'voice') -eq 1)
        }
        if ($currentHumanEvidence -or $refresh) {
            $seedSeen = $true
            $seedRemaining = $ConfirmedHangoverFrames
            if ($currentHumanEvidence) {
                $confirmEvents++
            } else {
                $refreshEvents++
            }
        } elseif ($seedRemaining -gt 0) {
            $seedRemaining--
        }

        $confirmed = $seedSeen -and $seedRemaining -gt 0
        $fast = -not $confirmed -and $fastRemaining -gt 0
        $provisionalProtect = $provisional -and -not $fast -and -not $confirmed
        if ($confirmed) {
            $confirmedFrames++
            if ($firstConfirmed -lt 0) {
                $firstConfirmed = $seq
            }
        } elseif ($firstConfirmed -ge 0 -and $seq -le $fixtureLastSeedEvidence) {
            $confirmedGapFrames++
        }
        if ($fast) { $fastFrames++ }
        if ($provisionalProtect) { $provisionalFrames++ }

        $targetMix = if ($confirmed -or $fast) { 24576 } elseif ($provisionalProtect) { 8192 } else { 0 }
        if ($rescueMix -lt $targetMix) {
            $rescueMix += [Math]::Min(8192, $targetMix - $rescueMix)
        } elseif ($rescueMix -gt $targetMix) {
            $rescueMix -= [Math]::Min(2048, $rescueMix - $targetMix)
        }
        if ($rescueMix -ge 8192) { $mix25Frames++ }
        if ($rescueMix -ge 24576) { $mix75Frames++ }
    }

    return [pscustomobject]@{
        Policy = $RefreshPolicy
        Frames = $Fixture.PlaybackFrames
        Anchors = $anchors
        ConfirmEvents = $confirmEvents
        RefreshEvents = $refreshEvents
        ProvisionalFrames = $provisionalFrames
        FastFrames = $fastFrames
        ConfirmedFrames = $confirmedFrames
        Mix25Frames = $mix25Frames
        Mix75Frames = $mix75Frames
        FirstConfirmed = $firstConfirmed
        LastSeedEvidence = $lastSeedEvidence
        ConfirmedGapFrames = $confirmedGapFrames
    }
}

$cases = @(
    [pscustomobject]@{ Name = 's40_far'; Kind = 'far'; Fixture = Read-ProtectionFixture $S40FarLog },
    [pscustomobject]@{ Name = 's42_retry_far'; Kind = 'far'; Fixture = Read-ProtectionFixture $S42FarLog },
    [pscustomobject]@{ Name = 's43_far'; Kind = 'far'; Fixture = Read-ProtectionFixture $S43FarLog },
    [pscustomobject]@{ Name = 's45_far'; Kind = 'far'; Fixture = Read-ProtectionFixture $S45FarLog },
    [pscustomobject]@{ Name = 's47_far'; Kind = 'far'; Fixture = Read-ProtectionFixture $S47FarLog },
    [pscustomobject]@{ Name = 's42_retry2_human'; Kind = 'human'; Fixture = Read-ProtectionFixture $S42HumanLog },
    [pscustomobject]@{ Name = 's44_human'; Kind = 'human'; Fixture = Read-ProtectionFixture $S44HumanLog },
    [pscustomobject]@{ Name = 's46_current_human'; Kind = 'current_human'; Fixture = Read-ProtectionFixture $S46HumanLog }
)

$rows = @()
$failed = $false
foreach ($case in $cases) {
    $old = Invoke-ProtectionReplay $case.Fixture 'old'
    $new = Invoke-ProtectionReplay $case.Fixture 'new'
    $rows += [pscustomobject]@{
        Case = $case.Name
        Policy = 'old'
        Frames = $old.Frames
        Anchors = $old.Anchors
        Provisional = $old.ProvisionalFrames
        Fast = $old.FastFrames
        Confirmed = $old.ConfirmedFrames
        Mix75 = $old.Mix75Frames
        ConfirmedGaps = $old.ConfirmedGapFrames
    }
    $rows += [pscustomobject]@{
        Case = $case.Name
        Policy = 'new'
        Frames = $new.Frames
        Anchors = $new.Anchors
        Provisional = $new.ProvisionalFrames
        Fast = $new.FastFrames
        Confirmed = $new.ConfirmedFrames
        Mix75 = $new.Mix75Frames
        ConfirmedGaps = $new.ConfirmedGapFrames
    }

    if ($case.Kind -eq 'far') {
        if ($new.Anchors -ne 0 -or $new.ConfirmedFrames -ne 0 -or
            $new.ProvisionalFrames -gt [Math]::Ceiling($new.Frames * 0.25)) {
            Write-Error "Far-only protection regression: $($case.Name)"
            $failed = $true
        }
    } elseif ($case.Kind -eq 'human') {
        if ($new.Anchors -lt 1 -or $new.FirstConfirmed -lt 0 -or
            $new.ConfirmedGapFrames -ne 0 -or
            $new.ConfirmedFrames -le ($old.ConfirmedFrames * 2)) {
            Write-Error "Human continuity regression: $($case.Name)"
            $failed = $true
        }
    } else {
        if ($new.Anchors -lt 1 -or $new.FirstConfirmed -lt 0 -or
            $new.ConfirmedGapFrames -ne 0 -or
            $new.ConfirmedFrames -le [Math]::Floor($new.Frames * 0.75)) {
            Write-Error "Current-firmware human continuity regression: $($case.Name)"
            $failed = $true
        }
    }
}

$rows | Format-Table -AutoSize
if ($failed) {
    exit 1
}

$liveAecSource = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'main\conversation\live_aec.c')
$mainSource = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'main\nuwax_ai_partner_main.c')
$softRefSource = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'main\p3_software_ref.c')
$sdkconfigText = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'sdkconfig')
$firInvariants = @(
    [pscustomobject]@{
        Name = 'provisional-hard-freeze'
        Passed = $liveAecSource -match 'hard_human_protection\s*=\s*aec->provisional_human_protect\s*\|\|\s*aec->fast_human_protect\s*\|\|\s*aec->confirmed_human_protect\s*\|\|\s*aec->nearend_preserve_hint'
    },
    [pscustomobject]@{
        Name = 'held-out-validation-gated-ready'
        Passed = $liveAecSource -match 'validation_raw_q15\s*<=' -and
                 $liveAecSource -match 'validation_scalar_q15\s*<=' -and
                 $liveAecSource -match 'validation_corr_q15\s*>=' -and
                 $liveAecSource -match 'coefficient_shape_valid;[\s\S]*?if\s*\(validation_passed\)\s*\{[\s\S]*?human_rescue_fir_ready\s*=\s*true'
    },
    [pscustomobject]@{
        Name = 'ready-model-frozen'
        Passed = $liveAecSource -match 'allow_adaptation\s*=\s*strict_far_only\s*&&[\s\S]*?!aec->human_rescue_fir_validating\s*&&[\s\S]*?!aec->human_rescue_fir_ready'
    },
    [pscustomobject]@{
        Name = 'scalar-fallback'
        Passed = $liveAecSource -match 'build_scalar_human_rescue_source\(aec, scalar_ref, sample_count\);'
    },
    [pscustomobject]@{
        Name = 'stored-preroll-source'
        Passed = $mainSource -match 'frame->human_rescue_source\[sample\]' -and
                 $mainSource -match 'NUWAX_LIVE_AEC_RESCUE_SOURCE_SCALAR' -and
                 $mainSource -match 'untrusted_source_frames' -and
                 $mainSource -match 'runtime_cleaner->prev_highpass = repair_cleaner.prev_highpass'
    },
    [pscustomobject]@{
        Name = 'independent-validation'
        Passed = $liveAecSource -match '#define\s+NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_VALIDATION_CHUNKS\s+32U' -and
                 $liveAecSource -match 'validation_passed' -and
                 $liveAecSource -match 'human_rescue_fir_ready_bad_windows'
    },
    [pscustomobject]@{
        Name = 'windowed-ready-health'
        Passed = $liveAecSource -match 'NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_READY_HEALTH_CHUNKS' -and
                 $liveAecSource -match 'human_rescue_fir_ready_health_raw_energy\s*\+=' -and
                 $liveAecSource -match 'human_rescue_fir_ready_health_residual_energy\s*\+=' -and
                 $liveAecSource -match 'ready_health_block\s*=\s*aec->human_rescue_fir_ready\s*&&\s*enough_training_excitation' -and
                 $liveAecSource -match 'ready_raw_q15\s*>\s*NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_READY_MAX_RAW_Q15' -and
                 $liveAecSource -match 'hinted_nearend\s*&&[\s\S]*?ready_raw_q15\s*>\s*NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_READY_HINT_RAW_Q15'
    },
    [pscustomobject]@{
        Name = 'ready-health-nearend-protection'
        Passed = $liveAecSource -match 'NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_NEAREND_HOLD_CHUNKS' -and
                 $liveAecSource -match 'NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_PENDING_MIX_Q15' -and
                 $liveAecSource -match 'action=nearend_hold' -and
                 $liveAecSource -match 'human_rescue_fir_nearend_hold_chunks\s*=\s*NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_NEAREND_HOLD_CHUNKS' -and
                 $liveAecSource -notmatch 'ready lost[\s\S]{0,500}?reset_human_rescue_fir_session'
    },
    [pscustomobject]@{
        Name = 'validation-failure-shape-policy'
        Passed = $liveAecSource -match 'coefficient_shape_valid\s*\?\s*"continue_training"\s*:\s*"clear_retrain"' -and
                 $liveAecSource -match 'if\s*\(coefficient_shape_valid\)\s*\{\s*reset_human_rescue_fir_training_health\(aec\);\s*\}\s*else\s*\{\s*reset_human_rescue_fir_session\(aec\);'
    },
    [pscustomobject]@{
        Name = 'coefficient-shape-validation'
        Passed = $liveAecSource -match 'human_rescue_fir_coefficient_shape_valid' -and
                 $liveAecSource -match 'NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_EDGE_ENERGY_Q15' -and
                 $liveAecSource -match 'validation_corr_q15\s*>=[\s\S]*?coefficient_shape_valid'
    },
    [pscustomobject]@{
        Name = 'split-fixed-reference'
        Passed = $mainSource -match 'nuwax_p3_software_ref_read_delayed_pair_continuous' -and
                 $mainSource -match 'nuwax_live_aec_process_with_refs_and_dual_mic' -and
                 $liveAecSource -match 'prepare_human_rescue_source\(aec,[\s\S]*?aec->ref_chunk,[\s\S]*?fir_ref_chunk'
    },
    [pscustomobject]@{
        Name = 'monotonic-reference-cursor'
        Passed = $softRefSource -match 'reader->source_end_index\s*\+=\s*sample_count' -and
                 $softRefSource -match 'reader->generation\s*!=\s*s_ref\.generation' -and
                 $softRefSource -match 'first_target_end\s*=\s*source_end\s*-\s*first_delay_samples' -and
                  $softRefSource -match 'second_target_end\s*=\s*source_end\s*-\s*second_delay_samples'
    },
    [pscustomobject]@{
        Name = 'write-gated-reference-reanchor'
        Passed = $softRefSource -match 's_ref\.write_index\s*>\s*reader->reanchor_wait_write_index' -and
                 $softRefSource -match 'reader->reanchor_wait_for_write\s*=\s*true' -and
                 $softRefSource -match 'reader->reanchor_wait_write_index\s*=\s*s_ref\.write_index' -and
                 $softRefSource -match 'reader->blocked_reanchor_reads\+\+'
    },
    [pscustomobject]@{
        Name = 'reference-generation-model-reset'
        Passed = $mainSource -match 'frame_software_ref_generation\s*!=\s*live_aec_software_ref_generation' -and
                 $mainSource -match 'nuwax_live_aec_reset_reference_epoch\(&live_aec\)' -and
                 $liveAecSource -match 'human_rescue_fir_reference_epoch_resets\+\+'
    },
    [pscustomobject]@{
        Name = 'reference-reanchor-preserves-model'
        Passed = $mainSource -match 'nuwax_live_aec_reanchor_reference\(&live_aec\)' -and
                 $mainSource -match 'model_preserved\s*\?\s*"preserved"\s*:\s*"retrain"' -and
                 $liveAecSource -match 'human_rescue_fir_reference_reanchors\+\+' -and
                 $liveAecSource -match 'preserve_ready_model\s*=\s*aec->human_rescue_fir_ready' -and
                 $liveAecSource -match 'human_rescue_fir_reference_retrain_reanchors\+\+' -and
                 $liveAecSource -match 'bool\s+nuwax_live_aec_reanchor_reference[\s\S]*?reset_human_rescue_fir_validation' -and
                 $liveAecSource -match 'bool\s+nuwax_live_aec_reanchor_reference[\s\S]*?reset_human_rescue_fir_ready_health_window'
    },
    [pscustomobject]@{
        Name = 'strict-gated-independent-validation'
        Passed = $liveAecSource -match 'validation_block\s*=\s*aec->human_rescue_fir_validating\s*&&\s*strict_far_only' -and
                 $liveAecSource -match 'if\s*\(strict_far_only\s*&&[\s\S]*?!aec->human_rescue_fir_ready\s*&&\s*!aec->human_rescue_fir_validating'
    },
    [pscustomobject]@{
        Name = 'synchronous-raw-reference-diagnostic'
        Passed = $mainSource -match 'raw_fir_ref_pair' -and
                 $mainSource -match 's_audio_raw_fir_ref_pair\[i\s*\*\s*2U\]\s*=\s*s_audio_samples\[source\]' -and
                 $mainSource -match 's_audio_raw_fir_ref_pair\[i\s*\*\s*2U\s*\+\s*1U\]\s*=\s*s_audio_fir_ref_samples\[source\]'
    },
    [pscustomobject]@{
        Name = 'warmup-gated-far-only-training'
        Passed = $liveAecSource -match 'human_rescue_fir_warmup_samples\s*>=' -and
                 $liveAecSource -match 'strict_far_only\s*=\s*reference_warmup_complete\s*&&[\s\S]*?!freeze_coefficients\s*&&\s*enough_training_excitation' -and
                 $liveAecSource -notmatch 'strict_far_only\s*=[\s\S]{0,300}?aligned_ref_metrics\.corr_q15'
    },
    [pscustomobject]@{
        Name = 'fixed-delay-preserved'
        Passed = $sdkconfigText -match '(?m)^CONFIG_NUWAX_LIVE_AEC_REF_DELAY_SAMPLES=840\r?$'
    },
    [pscustomobject]@{
        Name = 'fir-runtime-config'
        Passed = $sdkconfigText -match '(?m)^CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_ENABLE=y\r?$' -and
                 $sdkconfigText -match '(?m)^CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_PROVISIONAL_MIX_Q15=24576\r?$' -and
                 $sdkconfigText -match '(?m)^CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TAPS=256\r?$' -and
                 $sdkconfigText -match '(?m)^CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_REF_LAG_SAMPLES=376\r?$' -and
                 $sdkconfigText -match '(?m)^CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_SEED_TAP=120\r?$' -and
                 $sdkconfigText -match '(?m)^CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MIN_TRAINING_SAMPLES=8192\r?$' -and
                 $sdkconfigText -match '(?m)^CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_WARMUP_SAMPLES=32000\r?$' -and
                 $sdkconfigText -match '(?m)^CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_RESIDUAL_RAW_Q15=6144\r?$' -and
                 $sdkconfigText -match '(?m)^CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_RESIDUAL_SCALAR_Q15=8192\r?$'
    }
)
$firInvariants | Format-Table -AutoSize
$failedFirInvariant = $firInvariants | Where-Object { -not $_.Passed }
if ($failedFirInvariant) {
    Write-Error "FIR rescue source invariant regression: $($failedFirInvariant.Name -join ', ')"
    exit 1
}
Write-Host '[AEC HUMAN REPLAY] PASS'
