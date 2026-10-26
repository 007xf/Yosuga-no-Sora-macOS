"""Automatic review of an upscaled asset against its original.

usage: python3 tools/hd/check_hd.py <id> [--file hd/inbox/<id>.png]

Checks (thresholds depend on the asset's "fidelity" in tools/hd/manifest.json):
  size     HD is exactly scale x the original (aspect within 0.5%; use fit_hd.py first)
  format   8-bit RGBA (or RGB for opaque assets)
  alpha    transparency kept where the original is transparent (IoU of alpha masks)
  colour   mean colour difference after scaling the HD back down
  detail   structural similarity (SSIM) of the downscaled HD vs the original
Writes hd/review/<id>_compare.png (original | HD | HD as the game will show it | difference)
and hd/review/<id>.json. Exit code 0 = PASS, 1 = FAIL.
"""
import json, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageOps

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
LIMITS = {  # min alpha IoU, max mean colour diff (0..255), min SSIM
    'strict': (0.97, 10.0, 0.90),
    'moderate': (None, 18.0, 0.80),
    'redraw': (0.80, None, None),
}


def load_manifest():
    with open(os.path.join(ROOT, 'tools/hd/manifest.json'), encoding='utf-8') as f:
        return {a['id']: a for a in json.load(f)['assets']}


def ssim(a, b):
    """SSIM of two grayscale float images (0..255), 8x8 box windows."""
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    def box(x, k=8):
        s = np.cumsum(np.cumsum(np.pad(x, ((1, 0), (1, 0))), 0), 1)
        return (s[k:, k:] - s[:-k, k:] - s[k:, :-k] + s[:-k, :-k]) / (k * k)
    if min(a.shape) < 8:
        return 1.0 - float(np.mean(np.abs(a - b))) / 255
    ma, mb = box(a), box(b)
    va, vb, cov = box(a * a) - ma ** 2, box(b * b) - mb ** 2, box(a * b) - ma * mb
    m = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma ** 2 + mb ** 2 + c1) * (va + vb + c2))
    return float(m.mean())


def check(asset, path):
    report = {'id': asset['id'], 'file': os.path.relpath(path, ROOT), 'problems': []}
    original = Image.open(os.path.join(ROOT, asset['source'])).convert('RGBA')
    raw = Image.open(path)
    report['mode'] = raw.mode
    if raw.mode not in ('RGBA', 'RGB') or getattr(raw, 'bits', 8) != 8:
        report['problems'].append(f'format: {raw.mode}; save as 8-bit RGBA PNG')
    hd = raw.convert('RGBA')
    want = (original.width * asset['scale'], original.height * asset['scale'])
    report['size'] = list(hd.size)
    report['expected_size'] = list(want)
    if hd.size != want:
        aspect = (hd.width / hd.height) / (original.width / original.height)
        report['problems'].append(
            f'size: {hd.size} != {want} (aspect ratio off by {abs(aspect - 1) * 100:.2f}%); '
            'run tools/hd/fit_hd.py' if abs(aspect - 1) <= 0.005 else
            f'size: {hd.size} != {want} and the aspect ratio differs by {abs(aspect - 1) * 100:.1f}% - the picture was re-framed; regenerate')
    small = hd.resize(original.size, Image.LANCZOS)
    o = np.asarray(original, dtype=np.float64)
    s = np.asarray(small, dtype=np.float64)
    iou_min, diff_max, ssim_min = LIMITS[asset['fidelity']]
    if asset['alpha']:
        oa, sa = o[..., 3] > 127, s[..., 3] > 127
        union = np.logical_or(oa, sa).sum()
        iou = float(np.logical_and(oa, sa).sum() / union) if union else 1.0
        report['alpha_iou'] = round(iou, 4)
        if not (np.asarray(hd)[..., 3] < 255).any():
            report['problems'].append('alpha: the HD image has no transparency (background was filled); run tools/hd/fix_alpha.py')
        elif iou_min is not None and iou < iou_min:
            report['problems'].append(f'alpha: mask IoU {iou:.3f} < {iou_min} (shape changed or halo/background left)')
    weight = (o[..., 3] / 255) if asset['alpha'] else np.ones(o.shape[:2])
    if weight.sum() > 0:
        diff = float((np.abs(o[..., :3] - s[..., :3]).mean(-1) * weight).sum() / weight.sum())
        report['colour_diff'] = round(diff, 2)
        if diff_max is not None and diff > diff_max:
            report['problems'].append(f'colour: mean difference {diff:.1f} > {diff_max} (tint/brightness changed; try fix_alpha.py --match-colour)')
    gray = lambda x: x[..., :3] @ np.array([0.299, 0.587, 0.114]) * (x[..., 3] / 255 if asset['alpha'] else 1)
    score = ssim(gray(o), gray(s))
    report['ssim'] = round(score, 4)
    if ssim_min is not None and score < ssim_min:
        report['problems'].append(f'detail: SSIM {score:.3f} < {ssim_min} (content changed: faces, letters or lines differ)')
    report['result'] = 'PASS' if not report['problems'] else 'FAIL'
    sheet(asset, original, hd, small, os.path.join(ROOT, 'hd/review', asset['id'] + '_compare.png'))
    with open(os.path.join(ROOT, 'hd/review', asset['id'] + '.json'), 'w', encoding='utf-8') as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
    return report


def sheet(asset, original, hd, small, out):
    """original (nearest) | HD | downscaled HD (what the engine draws) | difference x4."""
    w, h = hd.size
    if w > 1200:
        h = round(h * 1200 / w); w = 1200
    checker = Image.new('RGBA', (w, h), (200, 200, 200, 255))
    d = ImageDraw.Draw(checker)
    for y in range(0, h, 16):
        for x in range(0, w, 16):
            if (x // 16 + y // 16) % 2: d.rectangle((x, y, x + 15, y + 15), fill=(240, 240, 240, 255))
    def panel(im, resample):
        p = checker.copy(); p.alpha_composite(im.resize((w, h), resample)); return p
    diff = Image.fromarray(np.clip(np.abs(np.asarray(original, np.int16)[..., :3] - np.asarray(small, np.int16)[..., :3]) * 4, 0, 255).astype(np.uint8))
    panels = [panel(original, Image.NEAREST), panel(hd, Image.LANCZOS), panel(small, Image.NEAREST),
              diff.resize((w, h), Image.NEAREST).convert('RGBA')]
    labels = ['原图 original', 'HD 高清版', 'HD 缩回原尺寸（游戏里看到的）', '差异 ×4']
    out_im = Image.new('RGBA', (w * 2 + 10, (h + 24) * 2 + 10), (40, 40, 40, 255))
    for i, (p, label) in enumerate(zip(panels, labels)):
        x, y = (i % 2) * (w + 10), (i // 2) * (h + 34)
        out_im.paste(p, (x, y + 24))
        ImageDraw.Draw(out_im).text((x + 4, y + 4), f'{asset["id"]}: {label}', fill=(255, 255, 255, 255))
    out_im.convert('RGB').save(out)


if __name__ == '__main__':
    assets = load_manifest()
    asset = assets.get(sys.argv[1]) if len(sys.argv) > 1 else None
    if not asset:
        sys.exit('usage: check_hd.py <id> [--file path]; ids: ' + ', '.join(assets))
    path = sys.argv[sys.argv.index('--file') + 1] if '--file' in sys.argv else os.path.join(ROOT, 'hd/inbox', asset['id'] + '.png')
    if not os.path.exists(path):
        sys.exit(f'missing {path}')
    report = check(asset, path)
    print(json.dumps(report, ensure_ascii=False, indent=1))
    sys.exit(0 if report['result'] == 'PASS' else 1)
