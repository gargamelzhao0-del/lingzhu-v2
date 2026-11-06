"""Exercise production contact cancellation; no LVGL calls from touch task."""
from pathlib import Path
import subprocess, hashlib, json, re

root = Path(__file__).resolve().parents[1]
source = root / 'main/conversation/touch_gesture.c'
text = source.read_text(encoding='utf-8')
out = root / 'build-host-checks/drawer-contact'
out.mkdir(parents=True, exist_ok=True)

def function(signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        if text[end] == '{': depth += 1
        elif text[end] == '}': depth -= 1
        end += 1
    return text[start:end]

struct = re.search(r'typedef struct \{.*?\} touch_session_t;', text, re.S).group()
parts = [function(s) for s in ['static void reset_touch_session(', 'static void record_touch_motion(',
    'static void begin_touch_session(',
    'static void update_drawer_contact(', 'static bool drawer_tap_allowed(']]
assert 'update_drawer_contact(&session, &point);' in text
assert 'if (!drawer_tap_allowed(&session, event)) continue;' in text
assert 'nuwax_drawer_contact(' not in text  # Touch task uses the mailbox API only.
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "control_center_input_policy.h"
typedef struct { uint16_t x,y; uint32_t contact_id,sample_ms; } nuwax_touch_point_t;
typedef enum { NUWAX_APP_SHELL_EVENT_TAP, SWIPE } nuwax_app_shell_event_t;
static int calls;
static bool down;
static uint16_t px,py;
static int64_t esp_timer_get_time(void) { return 1000; }
static void nuwax_app_shell_touch_begin(void) {}
static void nuwax_app_shell_drawer_contact(bool d,uint16_t x,uint16_t y) {
    calls++;down=d;px=x;py=y;
}
'''
test = r'''
int main(void) {
    touch_session_t s;
    nuwax_touch_point_t start={.x=110,.y=210},jitter={.x=116,.y=207},away={.x=129,.y=210};
    begin_touch_session(&s,&start);s.drawer_touch_started=true;
    update_drawer_contact(&s,&jitter);
    assert(down && px==110 && py==210 && drawer_tap_allowed(&s,NUWAX_APP_SHELL_EVENT_TAP));
    update_drawer_contact(&s,&away);assert(!down);
    update_drawer_contact(&s,&start);assert(!down);
    assert(!drawer_tap_allowed(&s,NUWAX_APP_SHELL_EVENT_TAP));
    assert(drawer_tap_allowed(&s,SWIPE));
    begin_touch_session(&s,&start);s.drawer_touch_started=true;
    update_drawer_contact(&s,&start);assert(down && drawer_tap_allowed(&s,NUWAX_APP_SHELL_EVENT_TAP));
    begin_touch_session(&s,&start);int before=calls;
    update_drawer_contact(&s,&away);assert(calls==before);
    assert(drawer_tap_allowed(&s,NUWAX_APP_SHELL_EVENT_TAP));
    puts("PASS: production contact anchor, jitter, slide-out cancellation, no re-arm, new contact and home-touch isolation");
}
'''
c = out / 'production-contact.c'
c.write_text(prefix + struct + '\n'.join(parts) + test, encoding='utf-8')
exe = out / 'contact.exe'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'main/nuwax_ui_v28'),str(c),'-o',str(exe)], check=True)
subprocess.run([str(exe)], check=True)
(out/'report.json').write_text(json.dumps(dict(sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(),passed=True),indent=2))
