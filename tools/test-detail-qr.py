"""Decode native LVGL captures. Requires Pillow and zxing-cpp (host only)."""
from pathlib import Path
import argparse
import json
from PIL import Image
import zxingcpp

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--captures', type=Path, required=True)
args = parser.parse_args()
expected = 'https://nuwax.com/lz.html?n=PROV_A1B2C3&u=NX-A1B2-C3D4-E5F6&p=Nuwax7-Dev-PoP'
qr = Image.open(args.captures / 'qr.png').convert('RGB')
results = zxingcpp.read_barcodes(qr)
assert len(results) == 1 and results[0].text == expected
# Rendering must retain an unrounded, square white border on all four sides.
for x in range(70, 290):
    assert qr.getpixel((x, 76)) == qr.getpixel((x, 295)) == (255, 255, 255)
for y in range(76, 296):
    assert qr.getpixel((70, y)) == qr.getpixel((289, y)) == (255, 255, 255)
assert not zxingcpp.read_barcodes(Image.open(args.captures / 'qr-unavailable.png'))
(args.captures / 'qr-report.json').write_text(json.dumps(dict(
    nativeResolution='360x360', decodedPayload=expected, squareWhiteBorder=True,
    unavailableHidesOldPayload=True, hardwareValidated=False), indent=2))
print('PASS: native QR decodes to provisioning URL; border intact; unavailable page hides stale QR')
