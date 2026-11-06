"""W01 regression entry; persist step results and stop on any failed check."""
from pathlib import Path
import json
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/w01';out.mkdir(parents=True,exist_ok=True)
results=[]
for name in ['test-longrun-gate.py','test-provisioning-buffer-lifecycle.py','test-provisioning-contracts.py']:
    result=subprocess.run([sys.executable,str(root/'tools'/name)],capture_output=True,
                          encoding='utf-8',errors='replace',timeout=180)
    log=out/(name+'.txt');log.write_text(result.stdout+result.stderr,encoding='utf-8')
    results.append(dict(check=name,status='PASS' if result.returncode==0 else 'FAIL',
                        exit_code=result.returncode,log=str(log)))
    (out/'checks.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    print(f"{results[-1]['status']}: {name}",flush=True)
    if result.returncode:
        print(result.stdout+result.stderr)
        raise SystemExit(1)
print('W01 audit tooling checks: PASS (host tests only)')
