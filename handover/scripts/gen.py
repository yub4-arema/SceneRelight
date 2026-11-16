import sys, os, json
sys.path.insert(0, '/home/user/obs-harmonizer/tools')
import numpy as np
from PIL import Image
import reference as R

TW, TH = 576, 324
os.makedirs('site/img', exist_ok=True)

SC = [('river','summer_river','川'), ('shrine','shrine','神社'), ('street','street_in_summer','夏の街'), ('roof','school_rooftop','屋上'), ('urban','urban_street','都市')]
TN = {1: '昼', 2: '夕方', 3: '夜'}
BG = {}
for key, slug, jp in SC:
    for n in (1, 2, 3):
        BG[f'{key}{n}'] = (f'{jp}・{TN[n]}', Image.open(f'bg2/{slug}_{n}.jpg').convert('RGB'))

Z = dict(strength=1.0, tint=0, exposure=0, contrast=0, shadow_tint=0, shading=0, rim=0)
PRESETS = [
  ('P01', '標準（今の初期値）', {}),
  ('P02', '弱め 全体0.5', dict(strength=0.5)),
  ('P03', '弱め 全体0.3', dict(strength=0.3)),
  ('P04', '色だけ 0.7', dict(Z, tint=0.7)),
  ('P05', '明るさだけ 0.35', dict(Z, exposure=0.35)),
  ('P06', 'コントラストだけ 0.4', dict(Z, contrast=0.4)),
  ('P07', '暗部の色だけ 0.35', dict(Z, shadow_tint=0.35)),
  ('P08', '自動陰影だけ 1.0', dict(Z, shading=1.0)),
  ('P09', '縁の光だけ 0.6', dict(Z, rim=0.6)),
  ('P10', '髪対策A 明0.15 色0.5 陰0.7 縁0.4', dict(exposure=0.15, tint=0.5, shading=0.7, rim=0.4)),
  ('P11', '髪対策B 明0 色0.5 コン0.2 陰0.6 縁0.3 暗0.25', dict(exposure=0, tint=0.5, contrast=0.2, shading=0.6, rim=0.3, shadow_tint=0.25)),
  ('P12', '髪対策C 明0 色0.5 コン0 陰1.0 縁0.3 暗0.3', dict(exposure=0, tint=0.5, contrast=0, shading=1.0, rim=0.3, shadow_tint=0.3)),
  ('P13', '全部強め 色1 明0.7 コン0.7 暗0.7 陰1.6 縁1.2', dict(tint=1.0, exposure=0.7, contrast=0.7, shadow_tint=0.7, shading=1.6, rim=1.2)),
]
PM = {p[0]: p for p in PRESETS}
tiles = []

def render(bgk, fg, x, h, prm, naive=False):
    n, hm = R.run(BG[bgk][1], Image.open(f'fg/{fg}.png'), x, 1.0, h, prm, finish_on=False)
    a = n if naive else hm
    return Image.fromarray((np.clip(a, 0, 1) * 255).astype(np.uint8)).resize((TW, TH), Image.LANCZOS)

def add(tid, section, bgk, fg, pid, x, h, naive=False, note=''):
    prm = PM[pid][2] if pid else {}
    im = render(bgk, fg, x, h, prm, naive)
    im.save(f'site/img/{tid}.jpg', quality=80)
    tiles.append(dict(id=tid, section=section, bg=BG[bgk][0], bgk=bgk, fg=fg,
        preset=('素' if naive else pid), label=('素（合成しただけ）' if naive else PM[pid][1]),
        pos=f'x={x} 高さ={h}' + (' ' + note if note else '')))

# 1) josei07 × 川（昼・夕方・夜）× 全13設定
for b in ['river1', 'river2', 'river3']:
    add(f'1_{b}_N', '1', b, 'josei07', None, 0.3, 0.66, naive=True)
    for pid, _, _ in PRESETS:
        add(f'1_{b}_{pid}', '1', b, 'josei07', pid, 0.3, 0.66)
# 2) josei07 × 残り12背景 × 5設定（+素）
for b in BG:
    if b.startswith('river'):
        continue
    add(f'2_{b}_N', '2', b, 'josei07', None, 0.3, 0.66, naive=True)
    for pid in ['P01', 'P02', 'P10', 'P12', 'P13']:
        add(f'2_{b}_{pid}', '2', b, 'josei07', pid, 0.3, 0.66)
# 3) 他のキャラ × 6背景 × 3設定
for fg in ['josei12', 'josei17', 'josei06']:
    for b in ['river1', 'river2', 'shrine3', 'street2', 'urban3', 'roof2']:
        for pid in ['P01', 'P10', 'P12']:
            add(f'3_{fg}_{b}_{pid}', '3', b, fg, pid, 0.5, 0.8)
# 4) 配置（josei07・川の昼・標準と髪対策C）
pl = [('左小', 0.25, 0.6), ('左大', 0.25, 0.9), ('中央', 0.5, 0.8), ('右・明るい側', 0.75, 0.8), ('右端大', 0.85, 1.0), ('右小', 0.72, 0.5)]
for n, x, h in pl:
    for pid in ['P01', 'P12']:
        add(f'4_{n}_{pid}', '4', 'river1', 'josei07', pid, x, h, note=f'配置:{n}')
json.dump(tiles, open('site/tiles.json', 'w'), ensure_ascii=False)
print(len(tiles))
