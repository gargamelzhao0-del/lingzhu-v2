"""Guarded SDMMC wait-error fix and optional bounded command observers.

Generate a build-local SDK copy. Never edit the installed SDK, change the
normal command sequence, or infer R1 errors without a command-specific rule.
"""
import argparse
import hashlib
import json
from pathlib import Path

SDK_SHA256 = '3526c0966bb6d6d5665c3f1ad66af5387fa114fed1fb169f18696f909846b53a'


def patch(data):
    if hashlib.sha256(data).hexdigest() != SDK_SHA256:
        raise ValueError('Unreviewed sdmmc_cmd.c; review before applying SD status fix')
    text = data.decode('utf-8').replace('\r\n', '\n')
    at = '#include "esp_private/sdmmc_common.h"'
    assert text.count(at) == 1
    text = text.replace(at, at + '\n#include "sd_probe_trace.h"\n#include "sdkconfig.h"\n'
        '#if CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE\n#include "sd_command_history.h"\n#endif', 1)
    before = '''        switch (sdmmc_wait_for_idle(card, status)) {
            case ESP_OK:
                break;
            case ESP_ERR_TIMEOUT:
                ESP_LOGE(TAG, "%s: sdmmc_wait_for_idle timeout", __func__);
                return ESP_ERR_TIMEOUT;
            default:
                return err;
        }'''
    assert text.count(before) == 2
    after = before.replace('switch (sdmmc_wait_for_idle(card, status))',
        'const esp_err_t wait_error = sdmmc_wait_for_idle(card, status);\n        switch (wait_error)')
    after = after.replace('default:\n                return err;', 'default:\n                return wait_error;')
    text = text.replace(before, after)
    before = '    esp_err_t err = (*card->host.do_transaction)(slot, cmd);'
    assert text.count(before) == 1
    text = text.replace(before, before + '''
    /* No I/O, allocation or logging outside an explicit scratch scope. */
    if (nuwax_sd_probe_trace_active()) {
        const esp_err_t probe_error = err != ESP_OK ? err : cmd->error;
        nuwax_sd_probe_trace_command(card, cmd->opcode, cmd->arg,
            probe_error == ESP_OK ? cmd->response[0] : 0U,
            probe_error, cmd->data, cmd->datalen);
    }
#if CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE
    if (nuwax_sd_command_history_observing()) {
        const esp_err_t command_error = err != ESP_OK ? err : cmd->error;
        nuwax_sd_command_history_command(card, cmd->opcode, cmd->arg,
            command_error == ESP_OK ? cmd->response[0] : 0U,
            command_error, cmd->data, cmd->datalen);
    }
#endif''', 1)
    return text.encode('utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        raise ValueError('SDK input must not be the generated output')
    data = args.input.read_bytes()
    generated = patch(data)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_bytes() != generated:
        args.output.write_bytes(generated)
    args.output.with_suffix('.json').write_text(json.dumps(dict(revision=206,
        inputSha256=hashlib.sha256(data).hexdigest(),
        outputSha256=hashlib.sha256(generated).hexdigest(),
        waitErrorSites=2, commandOrderUnchanged=True), indent=2) + '\n', encoding='utf-8')
    print('SDMMC wait errors preserved; build-local copy, SDK unchanged')


if __name__ == '__main__':
    main()
