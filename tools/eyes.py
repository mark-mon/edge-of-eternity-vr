#!/usr/bin/env python3
# eyes.py <dir> : for each eoevr_dump_<id>_{left,right,hud}.bmp set -> stats + <dir>/sbs_<id>.png preview
import sys, os, glob, re
import numpy as np
from PIL import Image
d = sys.argv[1]
ids = sorted({re.search(r'eoevr_dump_(\d+)_', f).group(1) for f in glob.glob(os.path.join(d, 'eoevr_dump_*'))})
for i in ids:
    def load(k):
        p = os.path.join(d, f'eoevr_dump_{i}_{k}.bmp')
        return np.asarray(Image.open(p).convert('RGBA')).astype(np.float32) if os.path.exists(p) else None
    L, R, H = load('left'), load('right'), load('hud')
    print(f'== dump {i}')
    for n, a in (('left', L), ('right', R), ('hud', H)):
        if a is not None:
            print(f'  {n}: {a.shape[1]}x{a.shape[0]} mean rgb {a[...,:3].mean():.1f} std {a[...,:3].std():.1f} alpha mean {a[...,3].mean():.1f} nonblack {(a[...,:3].max(-1)>8).mean()*100:.1f}%')
    if L is not None and R is not None and L.shape == R.shape:
        g = lambda a: a[..., :3].mean(-1)
        gl, gr = g(L), g(R)
        print(f'  |L-R| mean {np.abs(gl-gr).mean():.2f}  identical {np.array_equal(L, R)}')
        # disparity: best horizontal shift of right vs left (central band)
        h, w = gl.shape
        band = slice(h//3, 2*h//3); best = None
        for s in range(-60, 61, 2):
            a = gl[band, 100+max(0,s):w-100+min(0,s)]; b = gr[band, 100+max(0,-s):w-100+min(0,-s)]
            e = np.abs(a-b).mean()
            if best is None or e < best[1]: best = (s, e)
        print(f'  best shift {best[0]} px (err {best[1]:.2f})')
    ims = [Image.fromarray(a[..., :3].astype(np.uint8)) for a in (L, R) if a is not None]
    if H is not None:
        hh = Image.fromarray(H.astype(np.uint8)); bg = Image.new('RGBA', hh.size, (255, 0, 255, 255)); bg.alpha_composite(hh); ims.append(bg.convert('RGB'))
    if ims:
        th = 600; ims = [im.resize((int(im.width*th/im.height), th)) for im in ims]
        out = Image.new('RGB', (sum(im.width for im in ims) + 10*len(ims), th), (40, 40, 40)); x = 0
        for im in ims: out.paste(im, (x, 0)); x += im.width + 10
        out.save(os.path.join(d, f'sbs_{i}.png')); print('  preview', os.path.join(d, f'sbs_{i}.png'))
