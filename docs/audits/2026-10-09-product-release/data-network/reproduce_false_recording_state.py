"""Read-only production-source audit: a retained lease publishes ACTIVE without capture."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "main/nuwax_ai_partner_main.c").is_file())
OUT = Path(__file__).resolve().parent
MAIN = ROOT / "main/nuwax_ai_partner_main.c"
HARNESS = ROOT / "main/audio_mode_ui_lifecycle_host_check.c"
SPOOL = ROOT / "main/offline_audio_spool.c"

source = MAIN.read_text(encoding="utf-8-sig")
start = source.index("static void sync_exclusive_audio_mode_ui_state(void) {")
end = source.index("static void sync_pending_semantic_emotion(", start)
(OUT / "audio_mode_ui_host.inc").write_text(source[start:end], encoding="utf-8")

harness = HARNESS.read_text(encoding="utf-8-sig").split("int main(void) {")[0]
harness += r'''
int main(void) {
    s_meeting_mode=s_monitoring_mode=0;
    capture=meeting_resume=monitoring_resume=false;
    lease=true; old_kind=0;
    sync_exclusive_audio_mode_ui_state();
    assert(last_meeting==NUWAX_MEETING_STATE_ACTIVE);
    printf("CONFIRMED: no capture/no controller, retained meeting lease renders ACTIVE=%d\n",last_meeting);
    old_kind=1;
    sync_exclusive_audio_mode_ui_state();
    assert(last_monitoring==NUWAX_MONITORING_STATE_ACTIVE);
    printf("CONFIRMED: no capture/no controller, retained monitoring lease renders ACTIVE=%d\n",last_monitoring);
    lease=false;
    sync_exclusive_audio_mode_ui_state();
    assert(last_meeting==0 && last_monitoring==0);
    puts("CONTROL: clearing the lease renders both modes DISABLED");
}
'''
(OUT / "false_recording_state.c").write_text(harness, encoding="utf-8")
command = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
           "-I" + str(ROOT / "main/audio_modes/meeting/host_stubs"),
           "-I" + str(OUT), str(OUT / "false_recording_state.c"),
           "-o", str(OUT / "false_recording_state.exe")]
build = subprocess.run(command, capture_output=True, text=True)
build.check_returncode()
run = subprocess.run([str(OUT / "false_recording_state.exe")], capture_output=True, text=True)
(OUT / "false-recording-state.log").write_text(build.stdout + build.stderr + run.stdout + run.stderr, encoding="utf-8")
manifest = {
    "tested_at_utc": datetime.now(timezone.utc).isoformat(),
    "evidence_level": "production function extracted and executed with host stubs; not a hardware or cloud test",
    "condition": "both mode controllers disabled; no capture; no pending handoff; retained offline mode lease",
    "source_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in (MAIN, HARNESS, SPOOL)},
    "compile_command": command,
    "build_exit_code": build.returncode,
    "run_exit_code": run.returncode,
    "production_source_modified": False,
}
(OUT / "false-recording-state-evidence.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(run.stdout, end="")
run.check_returncode()
