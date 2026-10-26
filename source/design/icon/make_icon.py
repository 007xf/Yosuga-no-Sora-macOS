"""Builds AppIcon.icns for the merged app: Sora over Yosuga's title sky, macOS icon grid."""
import os, subprocess, sys
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.abspath(__file__))
sky = Image.open(sys.argv[1]).convert('RGBA')       # title background FRM_0611 (800x600)
sora = Image.open(sys.argv[2]).convert('RGBA')      # CA01_01L standing sprite
S = 1024
BODY = 824                                          # Apple icon grid: 824 px body, 100 px margins
body = Image.new('RGBA', (BODY, BODY))
# Sky: crop the upper clouds and fill the square.
k = sky.width / 800                                 # works for the original and an HD (2x) sky
crop = sky.crop((round(80 * k), 0, round(560 * k), round(480 * k))).resize((BODY, BODY), Image.LANCZOS)
body.alpha_composite(crop)
# Soft light at the bottom, like the title screen's haze.
haze = Image.new('RGBA', (BODY, BODY), (255, 255, 255, 0))
d = ImageDraw.Draw(haze)
for i in range(260):
    d.line([(0, BODY - i), (BODY, BODY - i)], fill=(255, 255, 255, int(150 * (1 - i / 260) ** 2)))
body.alpha_composite(haze)
# Sora: head and shoulders, centred, a little below the middle.
bbox = sora.getbbox()
f = sora.width / 956                                # the original sprite is 956 px wide
head = sora.crop((bbox[0] + round(40 * f), bbox[1], bbox[2] - round(40 * f), bbox[1] + round(560 * f)))
scale = BODY * 0.98 / head.width
head = head.resize((round(head.width * scale), round(head.height * scale)), Image.LANCZOS)
shadow = Image.new('RGBA', head.size, (20, 60, 110, 0))
shadow.putalpha(head.getchannel('A').point(lambda a: a // 3).filter(ImageFilter.GaussianBlur(10)))
x = (BODY - head.width) // 2
y = BODY - head.height + 40
body.alpha_composite(shadow, (x + 6, y + 10))
body.alpha_composite(head, (x, y))
# Rounded-square mask (continuous-corner approximation) and a hairline rim.
mask = Image.new('L', (BODY * 4, BODY * 4), 0)
ImageDraw.Draw(mask).rounded_rectangle((0, 0, BODY * 4 - 1, BODY * 4 - 1), radius=int(BODY * 4 * 0.2237), fill=255)
mask = mask.resize((BODY, BODY), Image.LANCZOS)
body.putalpha(Image.composite(body.getchannel('A'), Image.new('L', (BODY, BODY), 0), mask))
rim = Image.new('RGBA', (BODY * 4, BODY * 4), (0, 0, 0, 0))
ImageDraw.Draw(rim).rounded_rectangle((4, 4, BODY * 4 - 5, BODY * 4 - 5), radius=int(BODY * 4 * 0.2237),
                                      outline=(255, 255, 255, 90), width=8)
body.alpha_composite(rim.resize((BODY, BODY), Image.LANCZOS))
icon = Image.new('RGBA', (S, S), (0, 0, 0, 0))
drop = Image.new('RGBA', (S, S), (0, 0, 0, 0))
drop.paste((0, 0, 0, 90), (100, 112, 100 + BODY, 112 + BODY), mask)
icon.alpha_composite(drop.filter(ImageFilter.GaussianBlur(14)))
icon.alpha_composite(body, (100, 100))
icon.save(os.path.join(ROOT, 'AppIcon_1024.png'))
iconset = os.path.join(ROOT, 'AppIcon.iconset')
os.makedirs(iconset, exist_ok=True)
for size in (16, 32, 128, 256, 512):
    icon.resize((size, size), Image.LANCZOS).save(f'{iconset}/icon_{size}x{size}.png')
    icon.resize((size * 2, size * 2), Image.LANCZOS).save(f'{iconset}/icon_{size}x{size}@2x.png')
subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', os.path.join(ROOT, 'AppIcon.icns')], check=True)
print(os.path.join(ROOT, 'AppIcon.icns'))
