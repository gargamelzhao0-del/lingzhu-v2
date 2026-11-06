"""Check that the new scheduling hooks are reached in the linked ESP image."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--elf',type=Path,required=True)
p.add_argument('--expected-version',default='7.0.148');args=p.parse_args()
tool=Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
symbols=subprocess.check_output([str(tool/'xtensa-esp32s3-elf-nm.exe'),str(args.elf)],text=True)
checks={
 'app_shell_lvgl_pre_handler':['nuwax_face_service_decode'],
 'nuwax_face_service_decode':['nuwax_emotion_animation_player_service_decode'],
 'replay_send_windowed':['nuwax_replay_ui_handoff','nuwax_replay_ui_help_snapshot','nuwax_replay_schedule_snapshot','nuwax_replay_schedule_init','nuwax_replay_schedule_yield'],
 'replay_prefetch_task':['nuwax_replay_schedule_snapshot'],
 'nuwax_replay_ui_handoff':['xEventGroupWaitBits','nuwax_replay_ui_help_snapshot','esp_timer_get_time'],
 'nuwax_replay_ui_set_help_state':['xEventGroupSetBits'],
 'replay_read_validate':['nuwax_replay_dma_loan_begin','nuwax_replay_dma_loan_end'],
 'nuwax_replay_schedule_init':['esp_register_freertos_idle_hook_for_cpu'],
 'nuwax_replay_schedule_yield':['xEventGroupWaitBits','vTaskDelay'],
 'replay_idle_hook':['xEventGroupSetBits'],
}
ui_owner = 'lvgl_maybe_rebalance_internal_buffers' if any(
    line.split()[-1].startswith('lvgl_maybe_rebalance_internal_buffers')
    for line in symbols.splitlines() if line.split()) else 'lvgl_task'
checks[ui_owner] = ['nuwax_replay_dma_loan_bytes']
for caller,targets in checks.items():
 names=[line.split()[-1] for line in symbols.splitlines() if line.split() and
        (line.split()[-1]==caller or line.split()[-1].startswith(caller+'.') or line.split()[-1].startswith(caller+'$'))]
 assert names,caller
 asm='\n'.join(subprocess.check_output([str(tool/'xtensa-esp32s3-elf-objdump.exe'),'-d','--disassemble='+n,str(args.elf)],text=True) for n in names)
 for target in targets:assert '<'+target+'>' in asm,(caller,target)
binary=args.elf.with_suffix('.bin').read_bytes()
assert binary[48:80].split(b'\0')[0]==args.expected_version.encode()
assert b'SD scheduler revision=147 help_priority=5 reader_handoff=0 sender_handoff=event' in binary
assert b'cooperate_order=shared_wait help_lead_us=25000' in binary and b'ui_cancelled=%lu ui_pending=%lu' in binary
assert b'help_pending=original_deadline' in binary
assert b'fairness_wait=idle_event' in binary and b'offline replay fairness idle_wakes=' in binary
assert b'decode schedule revision=135' in binary
assert b'LVGL compact double buffer restored split_lease=1' in binary
assert 'nuwax_replay_ui_reader_handoff' not in symbols
out=Path(__file__).resolve().parents[1]/'build-host-checks/replay-ui-schedule'
(out/'linked.json').write_text(json.dumps({'version':args.expected_version,'hardwareValidated':False,
    'elfSha256':hashlib.sha256(args.elf.read_bytes()).hexdigest(),'calls':checks},indent=2),encoding='utf-8')
print('PASS final ELF: UI owner -> decode service; sender handoff and both budget observers; DMA loan lifetime; IDLE0 registration; version '+args.expected_version)
