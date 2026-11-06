"""Generate guarded ESP-IDF 6.0 SD event timing copies; never edit the SDK."""
import argparse
import hashlib
import json
from pathlib import Path

HASHES = {
    'src/sd_host_sdmmc.c': '8fb6ae837c9b90e861e15c5a261db4b280506811cf26ceb8836ce26b38321432',
    'src/sd_trans_sdmmc.c': 'fe02408164a826eeeacb463d38730741b2a3675cb1fd9afcc11cabe264914a2c',
    'include/esp_private/sd_host_private.h': 'ea42c67083ef6721da65d9a486cfdc9c1e3a69a002a4b01945afb6626b87f248',
}
def once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('SD profile insertion point changed')
    return text.replace(before, after, 1)

def instrument(name, data):
    if hashlib.sha256(data).hexdigest() != HASHES[name]:
        raise ValueError('Unreviewed SD SDK source: ' + name)
    s = data.decode('utf-8').replace('\r\n', '\n')
    if name.endswith('.h'):
        s = once(s, '} sd_host_sdmmc_event_t;',
                 '    uint32_t nuwax_queued_us; ///< diagnostic enqueue timestamp\n} sd_host_sdmmc_event_t;')
    elif name.endswith('sd_host_sdmmc.c'):
        s = once(s, '        xQueueSendFromISR(ctlr->event_queue, &event, &higher_priority_task_awoken);',
                 '        event.nuwax_queued_us = (uint32_t)esp_timer_get_time();\n'
                 '        xQueueSendFromISR(ctlr->event_queue, &event, &higher_priority_task_awoken);')
    else:
        s = once(s, '#include <stdbool.h>',
                 '#include <stdbool.h>\n#include "esp_timer.h"\n#include "sd_event_profile.h"')
        s = once(s, '    int ret = xQueueReceive(slot->ctlr->event_queue, out_event, tick_count);',
                 '    const bool already_queued = uxQueueMessagesWaiting(slot->ctlr->event_queue) != 0;\n'
                 '    const uint32_t wait_started = (uint32_t)esp_timer_get_time();\n'
                 '    int ret = xQueueReceive(slot->ctlr->event_queue, out_event, tick_count);\n'
                 '    if (ret != pdFALSE) {\n'
                 '        const uint32_t received = (uint32_t)esp_timer_get_time();\n'
                 '        nuwax_sd_event_profile_record(out_event->nuwax_queued_us, received,\n'
                 '                                     received - wait_started, already_queued);\n'
                 '    }')
    return s.encode('utf-8')

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--component', required=True); p.add_argument('--output', required=True)
    a = p.parse_args(); source = Path(a.component); target = Path(a.output)
    if source.resolve() == target.resolve(): raise ValueError('Distinct build directory required')
    # Validate every source before publishing any generated file.
    files = {name: instrument(name, (source/name).read_bytes()) for name in HASHES}
    for name, data in files.items():
        dest = target/name; dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists() or dest.read_bytes() != data: dest.write_bytes(data)
    report = {'revision': 'sd-event-lag-v1', 'originalSha256': HASHES,
              'generatedSha256': {n: hashlib.sha256(b).hexdigest() for n,b in files.items()}}
    (target/'profile-report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('SD event profiling copies verified; SDK unchanged')
if __name__ == '__main__': main()
