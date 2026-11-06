"""Check initial/reconnect hello and prevent mode-triggered reauthentication."""
from pathlib import Path
import json
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')

# There must be exactly one production call, in the new-connection path.
# Do not retain a one-shot boot flag: a fresh connection still needs hello.
callers = []
for p in (root/'main').rglob('*.c'):
    if p.name.endswith('_host_check.c') or p.name == 'session_protocol.c':
        continue
    text = p.read_text(encoding='utf-8-sig')
    callers.extend([p.relative_to(root).as_posix()] * len(re.findall(r'\bnuwax_session_send_hello\s*\(', text)))
assert callers == ['main/nuwax_ai_partner_main.c'], callers
assert 'hello_refresh' not in source and 'mode_hello' not in source
assert not (root/'main/hello_refresh.h').exists()
assert 'nuwax_session_send_hello_until_if_epoch' not in (root/'main/session_protocol.h').read_text(encoding='utf-8-sig')
start = source.index('        const bool hello_has_sd_card = nuwax_sd_card_media_detected();')
end = source.index('        ESP_ERROR_CHECK_WITHOUT_ABORT(hello_err);', start)
block = source[start:end]
assert source.count('nuwax_session_send_hello(') == 1
assert source.index('reset_application_keepalive(&ws);') < start
assert 'if (hello_err != ESP_OK)' in source[end:]
assert 'cloud hello send failed; closing session' in source[end:]

fixture = r'''
#define ESP_LOGI(...) ((void)0)
static bool detected;
static unsigned probes;
static bool nuwax_sd_card_media_detected(void) { ++probes; return detected; }
static void check_connection_hello(void) {
    nuwax_ws_client_t ws = {0};
    struct {char device_secret[96];} cloud_config = {"test-secret"};
    /* Execute the exact owner-side send block for boot and subsequent
     * connections, with changing SD detection results. */
    const bool cards[] = {true, false, true, true};
    for (unsigned i=0; i<sizeof(cards)/sizeof(cards[0]); ++i) {
        detected=cards[i];
        const unsigned before=sends, before_probes=probes;
''' + block + r'''
        assert(hello_err==ESP_OK && sends==before+1 && probes==before_probes+1);
        check_wire(cards[i]);
    }
    puts("PASS: actual boot/reconnect block sends once per connection and rereads SD status");
}
'''

with tempfile.TemporaryDirectory(prefix='hello-connection-') as d:
    out = Path(d)
    (out/'hello-connection-check.inc').write_text(fixture, encoding='utf-8')
    command = ['gcc','-std=c11','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',
               '-I'+str(out),'-I'+str(root/'main/session_protocol_host_stubs'),
               '-I'+str(root/'main'),'-I'+str(root/'managed_components/espressif__cjson/cJSON'),
               str(root/'main/session_protocol.c'),str(root/'main/hello_connection_host_check.c'),
               str(root/'managed_components/espressif__cjson/cJSON/cJSON.c'),
               '-Wl,--gc-sections','-o',str(out/'check.exe')]
    subprocess.run(command, check=True)
    subprocess.run([str(out/'check.exe')], check=True)

doc = (root/'docs/SD会议监控场次上传协议.md').read_text(encoding='utf-8-sig')
assert '同一连接只发送一次 hello' in doc
assert '开启会议／监控不额外发送 hello' in doc
assert not re.search(r'https?://|<a\b', doc)
for target in re.findall(r'\]\(([^)]+)\)', doc):
    # Related local documentation is allowed; the hello description and
    # complete wire examples must still be present in this document itself.
    assert (root/'docs'/target).is_file(), target
for example in re.findall(r'```json\s*\n(.*?)```', doc, re.S): json.loads(example)
print('PASS: single production caller, no mode refresh queue/API, standalone protocol with valid JSON examples')
