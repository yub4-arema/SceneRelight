"""3 値評価（もっと欲しい／程よい／やりすぎ）用の画像を作る（初期値のまま。第 2 回・第 3 回で使用）。

  作業ディレクトリに bg2/<slug>_<1-3>.jpg と fg/<name>.png を置いて実行する。
    python3 gen3.py [出力先（既定 site3）]
  <出力先>/img/<id>.jpg（処理後）と <id>_n.jpg（素）、<出力先>/tiles.json を書き出す。
"""
import json
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import reference as R  # noqa: E402

TW, TH = 720, 405
OUT = sys.argv[1] if len(sys.argv) > 1 else 'site3'
os.makedirs(f'{OUT}/img', exist_ok=True)
SC = [('river', 'summer_river', '川'), ('shrine', 'shrine', '神社'), ('street', 'street_in_summer', '夏の街'),
      ('roof', 'school_rooftop', '屋上'), ('urban', 'urban_street', '都市')]
TN = {1: '昼', 2: '夕方', 3: '夜'}
OTHERS = ['josei06', 'josei12', 'josei17']
tiles = []


def save(a, path):
    Image.fromarray((np.clip(a, 0, 1) * 255).astype(np.uint8)).resize((TW, TH), Image.LANCZOS).save(path, quality=84)


i = 0
for key, slug, jp in SC:
    for n in (1, 2, 3):
        bg = Image.open(f'bg2/{slug}_{n}.jpg').convert('RGB')
        for fg, x, h in (('josei07', 0.3, 0.66), (OTHERS[i % 3], 0.64, 0.85)):
            tid = f'{key}{n}_{fg}'
            naive, harm = R.run(bg, Image.open(f'fg/{fg}.png'), x, 1.0, h, {}, finish_on=False)
            save(harm, f'{OUT}/img/{tid}.jpg')
            save(naive, f'{OUT}/img/{tid}_n.jpg')
            tiles.append(dict(id=tid, bg=jp, time=TN[n], fg=fg))
        i += 1
json.dump(tiles, open(f'{OUT}/tiles.json', 'w'), ensure_ascii=False)
print(len(tiles))
