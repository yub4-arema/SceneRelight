import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import numpy as np
from PIL import Image, ImageDraw
import importlib, reference as R
W = sys.argv[1]; out = sys.argv[2]
fgn = sys.argv[3]; bgs = sys.argv[4].split(','); x=float(sys.argv[5]); h=float(sys.argv[6])
variants = [('naive',None),('legacy',dict(legacy=True)),('v3',{})]
if len(sys.argv)>7:
    import json; variants += [(k,v) for k,v in json.loads(sys.argv[7]).items()]
rows=[]
for b in bgs:
    bg = Image.open(f'{W}/bg2/{b}.jpg'); fg = Image.open(f'{W}/fg/{fgn}.png')
    cells=[]
    for name,prm in variants:
        n,hm = R.run(bg, fg, x, 1.0, h, prm or {}, finish_on=False)
        a = n if prm is None else hm
        im = Image.fromarray((np.clip(a,0,1)*255).astype(np.uint8))
        ImageDraw.Draw(im).text((8,8), f'{b} {name}', fill=(255,255,0))
        cells.append(np.asarray(im))
    rows.append(np.concatenate(cells,1))
Image.fromarray(np.concatenate(rows,0)).save(out, quality=88)
