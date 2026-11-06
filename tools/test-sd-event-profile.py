"""Guard the exact SDK instrumentation and reject unknown upstream changes."""
import importlib.util
from pathlib import Path
import hashlib
import argparse

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--idf', default='C:/esp/v6.0/esp-idf')
a = p.parse_args()
spec = importlib.util.spec_from_file_location('profile', Path(__file__).with_name('prepare-sd-event-profile.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
root = Path(a.idf) / 'components/esp_driver_sdmmc'
generated = {}
for name, digest in m.HASHES.items():
    original = (root/name).read_bytes()
    generated[name] = m.instrument(name, original).decode()
    assert hashlib.sha256((root/name).read_bytes()).hexdigest() == digest
    try:
        m.instrument(name, original + b'\n')
        raise AssertionError('Unknown source accepted')
    except ValueError:
        pass
host = generated['src/sd_host_sdmmc.c']
trans = generated['src/sd_trans_sdmmc.c']
assert host.count('event.nuwax_queued_us = (uint32_t)esp_timer_get_time();') == 1
assert 'xQueueSendFromISR(ctlr->event_queue, &event, &higher_priority_task_awoken);' in host
assert 'need_yield |= higher_priority_task_awoken == pdTRUE;' in host
assert 'portYIELD_FROM_ISR();' in host
assert trans.count('nuwax_sd_event_profile_record(') == 1
assert 'xQueueReceive(slot->ctlr->event_queue, out_event, tick_count);' in trans
assert generated['include/esp_private/sd_host_private.h'].count('uint32_t nuwax_queued_us;') == 1
print('PASS: all three pinned SDK sources, changed-source rejection, unchanged originals, unchanged queue/yield/timeout behavior')
