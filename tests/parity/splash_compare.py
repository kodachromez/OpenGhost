#!/usr/bin/env python3
"""Pixel diff of stepped splash captures (splash-reference.mjs vs openghost-cpp --splash-at).

usage: splash_compare.py <dir> [--sheet]
Full-frame RGB metrics per scene time (threshold 0, no masks, no alignment),
plus the same metrics inside and outside a box around the Ghost and word, so
background (mist) and foreground drift are reported separately, never hidden.
"""
import json, sys
from pathlib import Path
import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from metrics import measure

folder = Path(sys.argv[1])
rows = []
for ref in sorted(folder.glob('reference-*.png'), key=lambda p: int(p.stem.split('-')[1])):
    t = int(ref.stem.split('-')[1])
    nat = folder / f'native-{t}.png'
    if not nat.exists():
        continue
    a = np.asarray(Image.open(ref).convert('RGB'))
    b = np.asarray(Image.open(nat).convert('RGB'))
    if a.shape != b.shape:
        raise SystemExit(f'size mismatch at {t}: {a.shape} vs {b.shape}')
    d = np.abs(a.astype(np.int16) - b.astype(np.int16)).astype(np.uint8)
    Image.fromarray(np.minimum(d.astype(np.uint16) * 8, 255).astype(np.uint8)).save(folder / f'diff-{t}.x8.png')
    # Foreground: pixels where either image differs strongly from its own
    # 9-px median-free local background are the Ghost/word; report a box
    # around both sides' bright (> 200) pixels separately.
    bright = (a.max(2) > 200) | (b.max(2) > 200)
    ys, xs = np.nonzero(bright)
    fg = np.zeros(a.shape[:2], bool)
    if len(xs):
        # The Ghost and word only; motes are single bright dots, so take the
        # densest bright region: the bounding box of bright pixels with many
        # bright neighbours.
        dense = np.zeros_like(bright)
        k = 6
        cs = np.pad(bright.astype(np.int32), ((1, 0), (1, 0))).cumsum(0).cumsum(1)
        h, w = bright.shape
        y0 = np.clip(np.arange(h) - k, 0, h); y1 = np.clip(np.arange(h) + k + 1, 0, h)
        x0 = np.clip(np.arange(w) - k, 0, w); x1 = np.clip(np.arange(w) + k + 1, 0, w)
        count = cs[y1][:, x1] - cs[y0][:, x1] - cs[y1][:, x0] + cs[y0][:, x0]
        dense = bright & (count > 60)
        ys, xs = np.nonzero(dense)
        if len(xs):
            pad = 24
            fg[max(0, ys.min() - pad):ys.max() + pad, max(0, xs.min() - pad):xs.max() + pad] = True
    row = dict(t=t, full=measure(a, b))
    if fg.any():
        row['foreground'] = measure(a[fg][None], b[fg][None])
        row['background'] = measure(a[~fg][None], b[~fg][None])
    rows.append(row)
    f = row['full']
    print(f"{t:5d} ms  changed {f['changed_percent']:6.2f}%  >8 {f['over8_percent']:6.3f}%  MAE {f['mae']:.3f}  max {f['max_channel']:3d}"
          + (f"  | bg MAE {row['background']['mae']:.3f} >8 {row['background']['over8_percent']:.3f}%  fg MAE {row['foreground']['mae']:.3f}" if 'background' in row else ''))
(folder / 'compare.json').write_text(json.dumps(rows, indent=2) + '\n')
