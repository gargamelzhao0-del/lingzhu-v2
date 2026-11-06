"""Run the actual PowerShell contracts on isolated, deliberately broken sources."""
from pathlib import Path
import json
import re
import shutil
import subprocess

root=Path(__file__).resolve().parents[1]
script=root/'tools/run-provisioning-host-checks.ps1'
text=script.read_text(encoding='utf-8-sig')
files=set(re.findall(r'Join-Path \$projectRoot "([^"]+)"',text))
files.update(['sdkconfig.defaults','sdkconfig.product-ota.defaults','sdkconfig.barge-final'])
files={p for p in files if (root/p).is_file()}
files.update(str(p.relative_to(root)) for p in (root/'main/nuwax_provision_new').glob('*.h'))
files.update(['main/nuwax_provision_new/nuwax_provision_policy_host_check.c',
              'main/nuwax_provision_new/nuwax_provision_qr_payload_host_check.c'])
out=root/'build-host-checks/w01/provisioning-contracts';out.mkdir(parents=True,exist_ok=True)
shell=shutil.which('pwsh') or shutil.which('powershell');assert shell
app='main/nuwax_ai_partner_main.c'
variants={
 'good':None,
 'missing-rollback':(app,'rollback_wifi_credentials_guarded();','ESP_OK;','runtime-04'),
 'lost-isolation':(app,'if (!rollback_failed) {','if (true) {','runtime-08'),
 'missing-audio-restore':(app,'restore_audio_after_runtime_provisioning();','ESP_OK;','runtime-04'),
 'missing-buffer-return':('main/nuwax_lvgl_port.c','release_draw_buffer_pair(previous1, previous2, previous_split);','(void)previous1;','runtime-26'),
}
report={}
for name,change in variants.items():
    fixture=out/name;fixture.mkdir(exist_ok=True)
    for file in files:
        target=fixture/file;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(root/file,target)
    if change:
        file,before,after,_=change;p=fixture/file;s=p.read_text(encoding='utf-8-sig')
        assert before in s,(name,before)
        p.write_text(s.replace(before,after),encoding='utf-8')
    result=subprocess.run([shell,'-NoProfile','-ExecutionPolicy','Bypass','-File',str(script),
                           '-SourceRoot',str(fixture)],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
    (out/(name+'.txt')).write_text(result.stdout+result.stderr,encoding='utf-8')
    if change:
        contracts=json.loads((fixture/'build-host-checks/provisioning/runtime-contracts.json').read_text(encoding='utf-8-sig'))
        assert result.returncode!=0 and any(c['id']==change[3] and c['failed'] for c in contracts),(name,result.stdout,result.stderr)
    else:
        assert result.returncode==0,(result.stdout,result.stderr)
    report[name]=dict(exit_code=result.returncode,expected_failure=bool(change))
    print('PASS: provisioning source fixture '+name)
(out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
