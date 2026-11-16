"""第 4 回の評価画像：背景と立ち絵の種類を増やした組（初期値のまま）。

  作業ディレクトリに bg2/<slug>_<n>.jpg と fg/<name>.png を置いて実行する。
    python3 gen4.py [出力先（既定 site5）]
  追加した背景（みんちりえ background-c）: vending_machine, shopping_street, station_platform, back_alley,
  single_room2, casual_restaurant, school_hallway, park_in_autumn, summer_beach, shooting_stall
  追加した立ち絵（わたおきば）: dansei03, dansei12, dansei16, josei01, josei03, josei10, josei20
"""
import json
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import reference as R  # noqa: E402

TW, TH = 720, 405
OUT = sys.argv[1] if len(sys.argv) > 1 else 'site5'
os.makedirs(f'{OUT}/img', exist_ok=True)

# (背景ファイル, 表示名, 時間帯, 立ち絵, x, 高さ)
L, R_ = (0.3, 0.7), (0.66, 0.85)
SPEC = [
    ('back_alley_3', '路地', '夜', 'josei01', *L), ('back_alley_3', '路地', '夜', 'dansei16', *R_),
    ('park_in_autumn_3', '秋の公園', '夜', 'josei03', *L), ('park_in_autumn_3', '秋の公園', '夜', 'dansei03', *R_),
    ('shopping_street_3', '商店街', '夜', 'josei20', *L), ('shopping_street_3', '商店街', '夜', 'josei10', *R_),
    ('station_platform_3', '駅のホーム', '夜', 'dansei12', *L), ('station_platform_3', '駅のホーム', '夜', 'josei01', *R_),
    ('vending_machine_3', '自販機', '夜', 'josei10', *L), ('vending_machine_3', '自販機', '夜', 'josei03', *R_),
    ('shooting_stall_3', '屋台', '夜', 'dansei16', *L), ('shooting_stall_3', '屋台', '夜', 'josei20', *R_),
    ('single_room2_4', '部屋', '夜', 'josei01', *R_), ('school_hallway_4', '廊下', '夜', 'dansei03', *L),
    ('casual_restaurant_3', '飲食店', '夜', 'josei03', *R_), ('summer_beach_3', '砂浜', '夜', 'josei20', *L),
    ('back_alley_2', '路地', '夕方', 'josei10', *R_), ('park_in_autumn_2', '秋の公園', '夕方', 'josei01', *L),
    ('single_room2_2', '部屋', '夕方', 'josei20', *L), ('station_platform_2', '駅のホーム', '夕方', 'dansei16', *R_),
    ('summer_beach_2', '砂浜', '夕方', 'josei03', *R_), ('school_hallway_2', '廊下', '夕方', 'dansei12', *L),
    ('vending_machine_2', '自販機', '夕方', 'dansei03', *R_),
    ('park_in_autumn_1', '秋の公園', '昼', 'josei06', *R_), ('park_in_autumn_1', '秋の公園', '昼', 'josei20', *L),
    ('casual_restaurant_1', '飲食店', '昼', 'josei01', *L), ('school_hallway_1', '廊下', '昼', 'josei06', *R_),
    ('school_hallway_1', '廊下', '昼', 'josei10', *L), ('shooting_stall_1', '屋台', '昼', 'dansei16', *R_),
    ('single_room2_1', '部屋', '昼', 'josei06', *L), ('single_room2_1', '部屋', '昼', 'josei03', *R_),
    ('summer_beach_1', '砂浜', '昼', 'josei06', *R_), ('summer_beach_1', '砂浜', '昼', 'dansei03', *L),
    ('station_platform_1', '駅のホーム', '昼', 'josei10', *R_), ('shopping_street_1', '商店街', '昼', 'dansei12', *L),
    ('shrine_1', '神社', '昼', 'josei06', *R_),
]
tiles = []


def save(a, path):
    Image.fromarray((np.clip(a, 0, 1) * 255).astype(np.uint8)).resize((TW, TH), Image.LANCZOS).save(path, quality=84)


for bgf, jp, tm, fg, x, h in SPEC:
    tid = f'{bgf}_{fg}'
    naive, harm = R.run(Image.open(f'bg2/{bgf}.jpg').convert('RGB'), Image.open(f'fg/{fg}.png'), x, 1.0, h, {}, finish_on=False)
    save(harm, f'{OUT}/img/{tid}.jpg')
    save(naive, f'{OUT}/img/{tid}_n.jpg')
    tiles.append(dict(id=tid, bg=jp, time=tm, fg=fg))
json.dump(tiles, open(f'{OUT}/tiles.json', 'w'), ensure_ascii=False)
print(len(tiles))
