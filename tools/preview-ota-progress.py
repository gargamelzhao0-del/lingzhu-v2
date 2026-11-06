"""Render the production OTA logo scene and verify the expression underneath."""
from pathlib import Path
import importlib.util, os, struct, subprocess
from PIL import Image

root=Path(__file__).resolve().parents[1]
out=root/'tools/ui-speaking-capture/conversation-v1'
spec=importlib.util.spec_from_file_location('pack_verify',root/'tools/verify-emotion-pack.py')
verify=importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)
blob=(root/'assets/emotion/emotion-assets.bin').read_bytes()
header=verify.HEADER.unpack_from(blob)
entry=verify.ENTRY.unpack_from(blob,header[7]+3*verify.ENTRY.size)
assert entry[0]==4  # Existing sleeping expression; do not redraw its features.
segment=verify.SEGMENT.unpack_from(blob,header[8]+entry[5]*verify.SEGMENT.size)
frames=verify.decode_v4_segment(blob[segment[0]:segment[0]+segment[1]],segment[4])['frames']
palette=struct.unpack_from('<64H',blob,entry[8])
frame=frames[0]
out.mkdir(parents=True,exist_ok=True)
(out/'ota-sleeping.rgb565').write_bytes(struct.pack('<129600H',*(palette[p] for p in frame)))
env=os.environ.copy()
env['PATH']='C:/Users/Administrator/scoop/apps/gcc/current/bin;'+env.get('PATH','')
subprocess.run([str(out/'nuwax_speaking_capture.exe')],cwd=out,env=env,check=True)
for value in (0,62,100):
    im=Image.open(out/f'ota-{value}.ppm')
    im.save(out/f'ota-{value}.png')
Image.open(out/'ota-62.ppm').resize((720,720),Image.Resampling.NEAREST).save(out/'ota-preview.png')
print(out/'ota-preview.png')
