"""Narrow source-preservation normalization for reviewed SD admission gates."""
import re
import hashlib
import json
from pathlib import Path


def restore_reviewed_r218(text, name):
    """Retain older source fences outside precisely reviewed R218 changes.

    The R218 fault tests exercise the enabled implementation. This inverse
    patch only lets older baselines continue checking the unchanged code;
    full before/after digests reject any unreviewed surrounding change.
    """
    entry=json.loads((Path(__file__).parent/'fixtures/sd-r218-reviewed-source.json').read_text())[name]
    digest=hashlib.sha256(text.encode()).hexdigest()
    if digest==entry['before_sha256']:return text
    assert digest==entry['after_sha256'], 'Reviewed R218 source changed; rerun its behavioral checks before updating the fence: '+name
    lines=text.splitlines(keepends=True)
    for hunk in reversed(entry['hunks']):
        at,end=hunk['after_start'],hunk['after_end']
        assert lines[at:end]==hunk['after']
        lines[at:end]=hunk['before']
    result=''.join(lines)
    assert hashlib.sha256(result.encode()).hexdigest()==entry['before_sha256']
    return result

def catalog_without_directory_guard(text):
    text=restore_reviewed_r218(text,'main/sd_audio_catalog.c')
    include='''#if CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
#include "board/sd_card_driver.h"
#endif
'''
    assert text.count(include)==1, 'Expected the one reviewed board guard include'
    text=text.replace(include,'',1)
    # Match the exact enabled boundary as well as retaining the disabled path.
    # No catalog record, read, ACK, manifest or deletion code is excluded.
    enabled='''    esp_err_t err = nuwax_sd_card_validate_directory(SD_AUDIO_CATALOG_ROOT, true);
    if (err != ESP_OK) return err;
    if (mkdir(SD_AUDIO_CATALOG_ROOT, 0775) != 0 &&
        !(errno == EEXIST && stat(SD_AUDIO_CATALOG_ROOT, &st) == 0 && S_ISDIR(st.st_mode))) return ESP_FAIL;
    return nuwax_sd_card_validate_directory(SD_AUDIO_CATALOG_ROOT, false);'''
    pattern=r'\n#if defined\(ESP_PLATFORM\) && CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE\n(.*?)\n#else\n(.*?)\n#endif\n'
    matches=list(re.finditer(pattern,text,re.S))
    assert len(matches)==1 and matches[0][1]==enabled, 'Directory admission changed; review enabled tests before updating this boundary'
    return re.sub(pattern,lambda m:'\n'+m[2]+'\n',text,count=1,flags=re.S)


def _restore_exact_r216(text, edits):
    for reviewed, previous in edits:
        assert text.count(reviewed)==1, 'Reviewed R216 boundary changed; run its behavioral tests before updating this fence'
        text=text.replace(reviewed,previous,1)
    return text


def append_without_r216(text):
    # Only the reviewed offset/prefix or fallback dispatch tokens are normalized.
    text = _restore_exact_r216(text, [(
        '        s_writer_fault_stage = "seal_prefix_ram"; s_writer_fault_errno = 0; return ESP_ERR_INVALID_CRC;\n',
        '        writer_note_fault("seal_prefix_ram", 0); return ESP_ERR_INVALID_CRC;\n'), (
        '''        const esp_err_t sealed = commit_segment_locked(!atomic_load_explicit(&s.degraded, memory_order_acquire));
        if (sealed != ESP_OK) return sealed;
        const esp_err_t opened = open_segment_locked();
        if (opened != ESP_OK) return opened;
''', '''        if (commit_segment_locked(!atomic_load_explicit(&s.degraded, memory_order_acquire)) != ESP_OK ||
            open_segment_locked() != ESP_OK) {
            return ESP_FAIL;
        }
''')])
    return _restore_exact_r216(text, [('    /* last_good_offset is the logical end accepted by fwrite. verify_offset\n     * advances only after sync and independent readback. Picolibc ftell flushes\n     * its write buffer, so calling it for each 20 ms record defeats batching. */\n    const long record_start = s.last_good_offset;\n    if (record_start < (long)sizeof(archive_header_t) ||\n        record_bytes > (uint64_t)LONG_MAX - (uint64_t)record_start) {\n        writer_note_fault("record_offset", 0); return ESP_ERR_INVALID_SIZE;\n    }\n', '    const long record_start = ftell(s.file);\n'), ('#if CONFIG_NUWAX_SD_SESSION_ENABLE\n    if (card_recording_locked() && !sd_audio_seal_head_record(s.seal_head, item->data,\n            (uint32_t)item->bytes, s.capture_generation, s.segment_index, crc32_update)) {\n        s.writer_frame_uncertain = true;\n        writer_note_fault("seal_prefix_ram", 0); return ESP_ERR_INVALID_CRC;\n    }\n#endif\n    s.last_good_offset = record_start + (long)record_bytes;\n', '    s.last_good_offset = ftell(s.file);\n')])


def dma_without_r217(text):
    text=restore_reviewed_r218(text,'main/board/sd_dma_bulk.c')
    # Only the two reviewed new recovery blocks are removed. Their exact
    # digests and native/diagnostic-enabled behavioral tests fence this change;
    # the entire healthy fast path still compares with the original baseline.
    ranges = [
        ('static atomic_uintptr_t s_recovery_reader;',
         'nuwax_sd_bulk_stats_t nuwax_sd_bulk_stats(void)',
         'fb542d9eff2b9bd616ab0f150455c8c7ead5bf169c6d62b65f53cdb4deb4e9a8'),
        ('    const bool recovery_owner =',
         '    if (!card || !buffer || !count || !esp_ptr_external_ram(buffer))',
         '2c89433181c3087afaaa85d504719005954dfc082f7fd056a6fd96869db77a1d')]
    for first, last, digest in ranges:
        assert text.count(first) == 1 and text.count(last) == 1
        at, end = text.index(first), text.index(last)
        assert at < end and hashlib.sha256(text[at:end].encode()).hexdigest() == digest
        text = text[:at] + text[end:]
    return text


def file_sender_without_r216(text):
    # Only the reviewed offset/prefix or fallback dispatch tokens are normalized.
    return _restore_exact_r216(text, [('            ESP_LOGI(TAG, "SD formal prefetch reserve unavailable; single-pass bounded sender revision=216");\n            err = replay_send_shared_bounded_file(ws, epoch, file, segment_id, total_bytes,\n                offset, &header, &footer);\n', '            ESP_LOGW(TAG, "SD formal prefetch reserve unavailable; cancellable bounded sender retained revision=185 tls_retry=socket_ready");\n        } else {\n            if (err != ESP_OK) return err;\n            offset = total_bytes;\n            shared_streamed = true;\n'), ('        if (err != ESP_OK) return err;\n        offset = total_bytes;\n        shared_streamed = true;\n', '')])


def shared_without_r216(text):
    marker='/* A single small buffer is also a complete, validating streaming path.'
    assert text.count(marker)==1
    at=text.index(marker)
    fn=text.index('static esp_err_t replay_send_shared_bounded_file(',at)
    end=text.index('{',fn)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    assert text[end:].strip()=='#endif', 'Unreviewed trailing code after bounded sender'
    text=text[:at].rstrip()+'\n#endif\n'
    assert text.count('*capacity >= 4096U')==1
    return text.replace('*capacity >= 4096U','*capacity >= 8192U',1)
