#!/usr/bin/env python3
"""obs-harmonizer の参照実装（numpy）。

シェーダー（data/*.effect）と同じ段取り・同じ式で計算する。
見た目の調整と、シェーダーを変えたときの確認に使う。

使い方:
  python3 tools/reference.py --bg 背景.jpg --fg 立ち絵.png --out out.png \
      [--x 0.62 --y 1.0 --h 0.95] [--finish] [--compare]

--x/--y は前景の下端中央のキャンバス上の位置（0..1）、--h は前景の高さ（キャンバス比）。
"""
import argparse
import math

import numpy as np
from PIL import Image

# ---------------------------------------------------------------- 共通


def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(c):
    c = np.clip(c, 0.0, None)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


LUMA = np.array([0.2126, 0.7152, 0.0722], np.float32)


def luma(c):
    return c[..., :3] @ LUMA


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def sample_bilinear(img, u, v):
    """img: HxWxC, u,v: 0..1 の配列。シェーダーの clamp + linear サンプラー相当。"""
    h, w = img.shape[:2]
    x = np.clip(u * w - 0.5, 0, w - 1)
    y = np.clip(v * h - 0.5, 0, h - 1)
    x0 = np.floor(x).astype(int)
    y0 = np.floor(y).astype(int)
    x1 = np.minimum(x0 + 1, w - 1)
    y1 = np.minimum(y0 + 1, h - 1)
    fx = (x - x0)[..., None]
    fy = (y - y0)[..., None]
    a = img[y0, x0] * (1 - fx) + img[y0, x1] * fx
    b = img[y1, x0] * (1 - fx) + img[y1, x1] * fx
    return a * (1 - fy) + b * fy


# ---------------------------------------------------------------- 前処理パス
# OBS 側:  背景ソースを 256x144 に描く → 32x18 のブロック平均（rgb + 知覚輝度の2乗平均）
#          → 統計 2x1（平均・ハイライト色） → 32x18 の環境光マップをぼかす
#          それぞれ時間方向に指数平滑（ピンポン）

SMALL_W, SMALL_H = 256, 144
GRID_W, GRID_H = 32, 18


def bg_blocks(bg_lin):
    """256x144 に縮小 → 8x8 ブロック平均。rgb=平均色(線形), a=知覚輝度^2 の平均。"""
    img = Image.fromarray((lin_to_srgb(bg_lin) * 255).astype(np.uint8)).resize(
        (SMALL_W, SMALL_H), Image.BILINEAR)
    s = srgb_to_lin(np.asarray(img).astype(np.float32) / 255)
    lp = np.power(np.maximum(luma(s), 0), 1 / 2.2)
    bw, bh = SMALL_W // GRID_W, SMALL_H // GRID_H
    rgb = s.reshape(GRID_H, bh, GRID_W, bw, 3).mean((1, 3))
    lp2 = (lp ** 2).reshape(GRID_H, bh, GRID_W, bw).mean((1, 3))
    lp1 = lp.reshape(GRID_H, bh, GRID_W, bw).mean((1, 3))
    return np.concatenate([rgb, lp1[..., None], lp2[..., None]], -1)  # 5ch（シェーダーでは rgb+lp2 と lp を分ける）


def bg_stats(blocks):
    rgb = blocks[..., :3].reshape(-1, 3)
    lp1 = blocks[..., 3].reshape(-1)
    lp2 = blocks[..., 4].reshape(-1)
    mean_rgb = rgb.mean(0)
    mean_lp = lp1.mean()
    std_lp = math.sqrt(max(lp2.mean() - mean_lp ** 2, 1e-6))
    w = np.power(lp1, 4) + 1e-6
    hi_rgb = (rgb * w[:, None]).sum(0) / w.sum()
    lo_w = np.power(1 - lp1, 4) + 1e-6
    lo_rgb = (rgb * lo_w[:, None]).sum(0) / lo_w.sum()
    return dict(mean_rgb=mean_rgb, mean_lp=mean_lp, std_lp=std_lp, hi_rgb=hi_rgb, lo_rgb=lo_rgb)


def fg_stats(fg):
    """前景を 64x64 に縮小して alpha 加重の知覚輝度の平均・標準偏差。"""
    img = Image.fromarray((fg * 255).astype(np.uint8), 'RGBA').resize((64, 64), Image.BILINEAR)
    f = np.asarray(img).astype(np.float32) / 255
    a = f[..., 3]
    lp = np.power(np.maximum(luma(srgb_to_lin(f[..., :3])), 0), 1 / 2.2)
    wsum = a.sum() + 1e-6
    m = (lp * a).sum() / wsum
    v = (lp * lp * a).sum() / wsum - m * m
    return dict(mean_lp=m, std_lp=math.sqrt(max(v, 1e-6)))


def blur_grid(g, radius):
    """32x18 上の分離ガウス。radius はセル単位。"""
    if radius <= 0:
        return g
    sigma = radius / 2.0
    r = int(math.ceil(radius))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    out = g
    for ax in (0, 1):
        pad = [(0, 0)] * g.ndim
        pad[ax] = (r, r)
        p = np.pad(out, pad, mode='edge')
        acc = np.zeros_like(out)
        for i, kv in enumerate(k):
            sl = [slice(None)] * g.ndim
            sl[ax] = slice(i, i + out.shape[ax])
            acc += kv * p[tuple(sl)]
        out = acc
    return out


# ---------------------------------------------------------------- Harmonizer 本体

DEFAULTS = dict(
    strength=1.0,         # 全体の効き
    tint=0.7,             # 背景の光の色を前景に乗せる
    exposure=0.35,        # 明るさの追従度
    contrast=0.4,         # コントラストの追従度
    shadow_tint=0.35,     # 暗部を背景の暗い色に寄せる
    shading=1.0,          # 自動陰影（背景の明暗の分布を前景に掛ける）
    shading_spread=4.0,   # 自動陰影のぼかし（セル）
    rim=0.6,              # 縁の光
    rim_width=4.0,        # 縁の光の幅（前景ピクセル）
    rim_dir=0.85,          # 縁の光の方向性（0=全周, 1=光源側だけ）
    # 手置きの光と影（2枠）。kind: 0=なし 1=直線 2=円
    light1=dict(kind=0, x=0.5, y=0.5, angle=0.0, size=0.5, soft=0.5, amount=0.0, color=None),
    light2=dict(kind=0, x=0.5, y=0.5, angle=0.0, size=0.5, soft=0.5, amount=0.0, color=None),
)


def light_mask(p, u, v, aspect):
    """手置きライトのマスク 0..1。u,v は前景の UV。"""
    if p['kind'] == 0 or p['amount'] == 0:
        return None
    dx = (u - p['x']) * aspect
    dy = v - p['y']
    if p['kind'] == 1:
        a = math.radians(p['angle'])
        d = dx * math.cos(a) + dy * math.sin(a)  # 境界 (x,y) から angle 方向への距離
        soft = max(p['soft'] * 0.5, 1e-3)
        return 1 - smoothstep(-soft, soft, d)    # angle と反対側が 1
    r = np.sqrt(dx * dx + dy * dy)
    rad = max(p['size'] * 0.5, 1e-3)
    soft = max(p['soft'], 1e-3)
    return 1 - smoothstep(rad * (1 - soft), rad, r)


def harmonize_legacy(fg, fg_to_bg, env, bst, fst, prm, fg_px_w, fg_px_h):
    """旧方式（比較用に残す）。fg: HxWx4 straight alpha sRGB 0..1。fg_to_bg(u,v) -> 背景UV。env: 32x18x3 線形。"""
    h, w = fg.shape[:2]
    v, u = np.mgrid[0:h, 0:w].astype(np.float32)
    u = (u + 0.5) / w
    v = (v + 0.5) / h
    bu, bv = fg_to_bg(u, v)

    src = fg[..., :3]
    a = fg[..., 3]
    lin = srgb_to_lin(src)

    # 1) 明るさとコントラスト（知覚空間、輝度統計の転写）
    s = 1 + (np.clip(bst['std_lp'] / max(fst['std_lp'], 1e-3), 0.75, 1.35) - 1) * prm['contrast']
    target_m = fst['mean_lp'] + (bst['mean_lp'] - fst['mean_lp']) * prm['exposure']
    # 黒は黒のまま：g' = 目標平均 * (g / 前景平均)^s
    g = lin_to_srgb(lin)
    mf = max(fst['mean_lp'], 1e-3)
    g = target_m * np.power(np.maximum(g, 0) / mf, s)
    lin = srgb_to_lin(np.clip(g, 0, 1.5))

    # 2) 光の色（ハイライト色の色度で白をそめる）。暗部は背景の暗い色へ。
    def chroma(c, limit=0.45):
        # 色度（輝度 1 に正規化した色）。彩度の強い背景で破綻しないよう 1 からのずれを limit までに抑える
        c = c / max(float(c @ LUMA), 1e-4)
        dev = float(np.abs(c - 1).max())
        return 1 + (c - 1) * min(1.0, limit / max(dev, 1e-4))
    illum = chroma(bst['hi_rgb'] * 0.6 + bst['mean_rgb'] * 0.4)
    lin = lin * (1 + (illum - 1) * prm['tint'])
    L = luma(lin)[..., None]
    shadow_col = chroma(bst['lo_rgb']) * L
    lin = lin + (shadow_col - lin) * prm['shadow_tint'] * np.power(1 - np.clip(L, 0, 1), 3)

    # 3) 自動陰影：背景の局所的な明るさ / 背景平均
    e = sample_bilinear(env, bu, bv)
    eL = np.maximum(e @ LUMA, 1e-4)[..., None]
    mL = max(float(bst['mean_rgb'] @ LUMA), 1e-4)
    rel = np.clip(eL / mL, 0.25, 2.5)
    shade = np.power(rel, prm['shading'] * 0.8) * (1 + (e / eL - 1) * 0.5 * prm['shading'])
    lin = lin * shade

    # 4) 縁の光（alpha の勾配で縁と法線を出し、背景の明るさで光らせる）
    rw = prm['rim_width']
    acc = np.zeros_like(a)
    gx = np.zeros_like(a)
    gy = np.zeros_like(a)
    for k in range(12):
        t = 2 * math.pi * k / 12
        ox, oy = math.cos(t) * rw / fg_px_w, math.sin(t) * rw / fg_px_h
        an = sample_bilinear(a[..., None], u + ox, v + oy)[..., 0]
        acc += an
        gx += an * math.cos(t)
        gy += an * math.sin(t)
    acc /= 12
    edge = a * np.clip(1 - acc, 0, 1) * 2
    nx, ny = -gx, -gy  # 外向き法線
    nl = np.sqrt(nx * nx + ny * ny) + 1e-5
    nx, ny = nx / nl, ny / nl
    # 光源方向 = 環境光マップの明るさ勾配
    du = 1.5 / GRID_W
    dv = 1.5 / GRID_H
    lx = (sample_bilinear(env, bu + du, bv) - sample_bilinear(env, bu - du, bv)) @ LUMA
    ly = (sample_bilinear(env, bu, bv + dv) - sample_bilinear(env, bu, bv - dv)) @ LUMA
    ll = np.sqrt(lx * lx + ly * ly) + 1e-5
    facing = np.clip(nx * lx / ll + ny * ly / ll, -1, 1)
    dirw = 1 + (np.clip(facing * 0.5 + 0.5, 0, 1) * 2 - 1) * prm['rim_dir']
    dirw = np.clip(dirw, 0, 2)
    # 背景が平均より明るい所ほど強く光る
    rim_col = e / eL * np.clip(eL * 1.5, 0, 1) * smoothstep(0.7, 1.6, rel) * 1.5
    rim_amt = (edge * prm['rim'] * dirw)[..., None]
    lin = 1 - (1 - lin) * (1 - np.clip(rim_col * rim_amt, 0, 1))  # screen

    # 5) 手置きの光と影
    aspect = fg_px_w / fg_px_h
    for key in ('light1', 'light2'):
        p = prm[key]
        m = light_mask(p, u, v, aspect)
        if m is None:
            continue
        m = m[..., None]
        if p['amount'] < 0:
            col = np.array(p['color']) if p['color'] is not None else np.clip(chroma(bst['lo_rgb']) * 0.35, 0, 1)
            lin = lin * (1 + (col - 1) * m * -p['amount'])
        else:
            col = np.array(p['color']) if p['color'] is not None else np.clip(bst['hi_rgb'] / max(bst['hi_rgb'].max(), 1e-4), 0, 1)
            lin = lin * (1 + col * m * p['amount'] * 2.0) + col * m * p['amount'] * 0.04

    out = lin_to_srgb(lin)
    out = src + (out - src) * prm['strength']
    return np.concatenate([np.clip(out, 0, 1), a[..., None]], -1)


# ---------------------------------------------------------------- 新方式（v2）
# 旧方式の問題（実際の評価で分かったこと）:
#  - 明るさ: 前景の平均に対する倍率をそのまま掛けていた。黒い服が平均を下げるので、
#    明るい昼の背景で髪が白飛びした。 → 輝度だけを、黒と白を固定したべき乗カーブで動かす
#  - 自動陰影: 背後の背景の「色」までそのまま乗せていた。川や木の色が前景に写り込み、
#    手前にいるはずの前景が背景の一部のように見えた。 → 局所は明暗だけ、色は広い範囲の平均だけ
#  - 光の色・縁の光: 彩度の上限が高く、青空や川に引っ張られた。 → 上限を下げる
CHROMA_LIMIT = 0.28   # 光の色・暗部の色の彩度の上限
LOCAL_REL = (0.6, 1.6)  # 背後の明るさの比の範囲（旧: 0.25〜2.5）
EXPO_P = (0.8, 1.25)   # 明るさ追従のべき指数の範囲（1 で変化なし）


def chroma_v(c, limit):
    c = c / max(float(c @ LUMA), 1e-4)
    dev = float(np.abs(c - 1).max())
    return 1 + (c - 1) * min(1.0, limit / max(dev, 1e-4))


def harmonize(fg, fg_to_bg, env, bst, fst, prm, fg_px_w, fg_px_h):
    if prm.get('legacy'):
        return harmonize_legacy(fg, fg_to_bg, env, bst, fst, prm, fg_px_w, fg_px_h)
    h, w = fg.shape[:2]
    v, u = np.mgrid[0:h, 0:w].astype(np.float32)
    u = (u + 0.5) / w
    v = (v + 0.5) / h
    bu, bv = fg_to_bg(u, v)
    src = fg[..., :3]
    a = fg[..., 3]
    lin = srgb_to_lin(src)

    # 1) 明るさとコントラスト：輝度だけを動かす（色相は保つ）。0 は 0、1 は 1 のまま
    Y = np.maximum(luma(lin), 1e-5)
    lp = np.power(Y, 1 / 2.2)
    fm = float(np.clip(fst['mean_lp'], 0.03, 0.97))
    target = fm + (bst['mean_lp'] - fm) * prm['exposure']
    p = math.log(float(np.clip(target, 0.03, 0.97))) / math.log(fm)
    p = float(np.clip(p, *EXPO_P))
    lp = np.power(lp, p)
    sc = 1 + (np.clip(bst['std_lp'] / max(fst['std_lp'], 1e-3), 0.75, 1.35) - 1) * prm['contrast']
    pivot = fm ** p
    lp2 = pivot * np.power(np.maximum(lp, 1e-4) / pivot, sc)
    keep = smoothstep(0.7, 1.0, lp)                       # 白は崩さない
    lp = lp2 * (1 - keep) + lp * keep
    gain = np.clip(np.power(np.clip(lp, 0, 1), 2.2) / Y, 0, 4.0)
    lin = lin * gain[..., None]

    # 2) 光の色（広い範囲の平均が中心）と暗部の色
    illum = chroma_v(bst['hi_rgb'] * 0.5 + bst['mean_rgb'] * 0.5, CHROMA_LIMIT)
    lin = lin * (1 + (illum - 1) * prm['tint'])
    L = luma(lin)[..., None]
    shadow_col = chroma_v(bst['lo_rgb'], CHROMA_LIMIT) * L
    lin = lin + (shadow_col - lin) * prm['shadow_tint'] * np.power(1 - np.clip(L, 0, 1), 3)

    # 3) 自動陰影：局所は「明暗」だけ。色は上下の環境光（空側と地面側）から、広く・弱く
    e = sample_bilinear(env, bu, bv)
    eL = np.maximum(e @ LUMA, 1e-4)[..., None]
    mL = max(float(bst['mean_rgb'] @ LUMA), 1e-4)
    rel = np.clip(eL / mL, *LOCAL_REL)
    sh = prm['shading']
    lin = lin * np.power(rel, sh * 0.6)
    top = env[:GRID_H // 3].reshape(-1, 3).mean(0)
    bot = env[-GRID_H // 3:].reshape(-1, 3).mean(0)
    wv = smoothstep(0.1, 0.9, v)[..., None]               # 前景の上端 0 → 下端 1（前景自身の上下）
    amb = chroma_v(top, 0.2) * (1 - wv) + chroma_v(bot, 0.2) * wv
    lin = lin * (1 + (amb - 1) * 0.6 * min(sh, 1.5))
    lv = np.clip(((top @ LUMA) * (1 - wv) + (bot @ LUMA) * wv) / mL, 0.8, 1.25)
    lin = lin * np.power(lv, sh * 0.3)

    # 4) 縁の光（色は彩度を抑える）
    am = np.pad(a, 1)
    acc = np.zeros_like(a)
    gx = np.zeros_like(a)
    gy = np.zeros_like(a)
    for i in range(12):
        t = 2 * math.pi * i / 12
        dx, dy = math.cos(t) * prm['rim_width'], math.sin(t) * prm['rim_width']
        an = sample_bilinear(a[..., None], u + dx / w, v + dy / h)[..., 0]
        acc += an
        gx += an * math.cos(t)
        gy += an * math.sin(t)
    acc /= 12
    edge = a * np.clip(1 - acc, 0, 1) * 2
    nx, ny = -gx, -gy
    nl = np.sqrt(nx * nx + ny * ny) + 1e-5
    nx, ny = nx / nl, ny / nl
    du = 1.5 / GRID_W
    dv = 1.5 / GRID_H
    lx = (sample_bilinear(env, bu + du, bv) - sample_bilinear(env, bu - du, bv)) @ LUMA
    ly = (sample_bilinear(env, bu, bv + dv) - sample_bilinear(env, bu, bv - dv)) @ LUMA
    ll = np.sqrt(lx * lx + ly * ly) + 1e-5
    facing = np.clip(nx * lx / ll + ny * ly / ll, -1, 1)
    dirw = np.clip(1 + (np.clip(facing * 0.5 + 0.5, 0, 1) * 2 - 1) * prm['rim_dir'], 0, 2)
    rim_c = chroma_v(bst['hi_rgb'], 0.25)
    rel_r = eL / mL
    rim_col = rim_c * np.clip(eL * 1.5, 0, 1) * smoothstep(0.7, 1.6, rel_r) * 1.5
    lin = 1 - (1 - lin) * (1 - np.clip(rim_col * (edge * prm['rim'] * dirw)[..., None], 0, 1))

    # 5) 手置きの光と影（旧方式と同じ）
    aspect = fg_px_w / fg_px_h
    for key in ('light1', 'light2'):
        pp = prm[key]
        m = light_mask(pp, u, v, aspect)
        if m is None:
            continue
        m = m[..., None]
        if pp['amount'] < 0:
            col = np.array(pp['color']) if pp['color'] is not None else np.clip(chroma_v(bst['lo_rgb'], CHROMA_LIMIT) * 0.35, 0, 1)
            lin = lin * (1 + (col - 1) * m * -pp['amount'])
        else:
            col = np.array(pp['color']) if pp['color'] is not None else np.clip(bst['hi_rgb'] / max(bst['hi_rgb'].max(), 1e-4), 0, 1)
            lin = lin * (1 + col * m * pp['amount'] * 2.0) + col * m * pp['amount'] * 0.04

    out = lin_to_srgb(lin)
    out = src + (out - src) * prm['strength']
    return np.concatenate([np.clip(out, 0, 1), a[..., None]], -1)


# ---------------------------------------------------------------- Finish（仕上げ）

FINISH_DEFAULTS = dict(
    halftone=0.35, halftone_cell=7.0, halftone_angle=45.0, halftone_edge=0.7,
    grain=0.06, grain_size=1.4,
    leak=0.3, leak_size=0.45, leak_auto=1.0, leak_x=0.9, leak_y=0.1,
    vignette=0.25, seed=1.0,
)


def hash21(x, y, seed):
    return np.modf(np.sin(x * 12.9898 + y * 78.233 + seed * 37.719) * 43758.5453)[0] % 1.0


def finish(img, prm, leak_color=None):
    """img: HxWx3 sRGB 0..1（キャンバス全体）。"""
    h, w = img.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    u = (xx + 0.5) / w
    v = (yy + 0.5) / h
    c = img.copy()

    # ビネット重み（周辺ほど 1）
    dx = (u - 0.5) * (w / h)
    dy = v - 0.5
    r = np.sqrt(dx * dx + dy * dy) / math.sqrt((0.5 * w / h) ** 2 + 0.25)
    edge_w = smoothstep(0.2, 1.0, r)

    # 網点：回転した格子のセル中心の輝度で点の半径を決める
    cell = prm['halftone_cell']
    ang = math.radians(prm['halftone_angle'])
    ca, sa = math.cos(ang), math.sin(ang)
    px, py = xx * ca + yy * sa, -xx * sa + yy * ca
    cx = (np.floor(px / cell) + 0.5) * cell
    cy = (np.floor(py / cell) + 0.5) * cell
    sx, sy = cx * ca - cy * sa, cx * sa + cy * ca  # セル中心を画面座標へ戻す
    cc = sample_bilinear(img, sx / w, sy / h)
    cl = cc @ LUMA
    rad = np.sqrt(np.clip(1 - cl, 0, 1)) * cell * 0.62
    d = np.sqrt((px - cx) ** 2 + (py - cy) ** 2)
    dot = 1 - smoothstep(rad - 0.75, rad + 0.75, d)
    wgt = prm['halftone'] * (1 - prm['halftone_edge'] + prm['halftone_edge'] * edge_w) * smoothstep(0.95, 0.35, cl)
    c = c * (1 - (dot * wgt * 0.45)[..., None])

    # ビネット
    c = c * (1 - prm['vignette'] * smoothstep(0.45, 1.1, r))[..., None]

    # 光漏れ（screen）
    # 位置と色は入力画像の明るい所から自動で決める（leak_auto=0 で手動位置）
    small = np.asarray(Image.fromarray((img * 255).astype(np.uint8)).resize((32, 18), Image.BOX)).astype(np.float32) / 255
    sl = small @ LUMA
    wgt8 = np.power(sl, 8) + 1e-6
    gy, gx = np.mgrid[0:18, 0:32]
    auto_x = float(((gx + 0.5) / 32 * wgt8).sum() / wgt8.sum())
    auto_y = float(((gy + 0.5) / 18 * wgt8).sum() / wgt8.sum())
    if leak_color is None:
        leak_color = (small * wgt8[..., None]).reshape(-1, 3).sum(0) / wgt8.sum()
        leak_color = leak_color / max(leak_color.max(), 1e-4)
    lx = prm['leak_x'] + (auto_x - prm['leak_x']) * prm['leak_auto']
    ly = prm['leak_y'] + (auto_y - prm['leak_y']) * prm['leak_auto']
    # 画面の外側へ押し出して「画面外から差し込む」感じにする
    ox, oy = lx - 0.5, ly - 0.5
    ol = math.sqrt(ox * ox + oy * oy) + 1e-4
    lx, ly = lx + ox / ol * 0.25, ly + oy / ol * 0.25
    ldx = (u - lx) * (w / h)
    ldy = v - ly
    lr = np.sqrt(ldx * ldx + ldy * ldy)
    lm = np.exp(-(lr / max(prm['leak_size'], 1e-3)) ** 2 * 2.0)
    leak = lm[..., None] * leak_color * prm['leak']
    c = 1 - (1 - c) * (1 - np.clip(leak, 0, 1))

    # グレイン
    gs = prm['grain_size']
    n = hash21(np.floor(xx / gs), np.floor(yy / gs), prm['seed']) - 0.5
    c = c + (n * prm['grain'])[..., None]
    return np.clip(c, 0, 1)


# ---------------------------------------------------------------- 合成ユーティリティ


def place(canvas_w, canvas_h, fg_w, fg_h, x, y, hfrac):
    """前景の配置。下端中央が (x, y)、高さが hfrac。戻り値: (左, 上, 幅, 高さ) px"""
    H = hfrac * canvas_h
    W = H * fg_w / fg_h
    return x * canvas_w - W / 2, y * canvas_h - H, W, H


def run(bg_img, fg_img, x=0.62, y=1.0, hfrac=0.95, prm=None, fin=None, finish_on=True):
    prm = dict(DEFAULTS, **(prm or {}))
    bg = np.asarray(bg_img.convert('RGB')).astype(np.float32) / 255
    CW, CH = bg.shape[1], bg.shape[0]
    L, T, W, H = place(CW, CH, fg_img.width, fg_img.height, x, y, hfrac)
    fg_scaled = fg_img.convert('RGBA').resize((max(1, int(round(W))), max(1, int(round(H)))), Image.LANCZOS)
    fg = np.asarray(fg_scaled).astype(np.float32) / 255

    bg_lin = srgb_to_lin(bg)
    blocks = bg_blocks(bg_lin)
    bst = bg_stats(blocks)
    fst = fg_stats(fg)
    env = blur_grid(blocks[..., :3], prm['shading_spread'])

    def fg_to_bg(u, v):
        return (L + u * W) / CW, (T + v * H) / CH

    out_fg = harmonize(fg, fg_to_bg, env, bst, fst, prm, fg.shape[1], fg.shape[0])

    def composite(layer):
        c = bg.copy()
        x0, y0 = int(round(L)), int(round(T))
        hh, ww = layer.shape[:2]
        sx0, sy0 = max(0, -x0), max(0, -y0)
        dx0, dy0 = max(0, x0), max(0, y0)
        dx1, dy1 = min(CW, x0 + ww), min(CH, y0 + hh)
        sub = layer[sy0:sy0 + dy1 - dy0, sx0:sx0 + dx1 - dx0]
        al = sub[..., 3:4]
        c[dy0:dy1, dx0:dx1] = c[dy0:dy1, dx0:dx1] * (1 - al) + sub[..., :3] * al
        return c

    naive = composite(fg)
    harm = composite(out_fg)
    if finish_on:
        f = dict(FINISH_DEFAULTS, **(fin or {}))
        harm = finish(harm, f)
    return naive, harm


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bg', required=True)
    ap.add_argument('--fg', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--x', type=float, default=0.62)
    ap.add_argument('--y', type=float, default=1.0)
    ap.add_argument('--h', type=float, default=0.95)
    ap.add_argument('--finish', action='store_true')
    ap.add_argument('--compare', action='store_true', help='左に素の合成、右に処理後を並べる')
    a = ap.parse_args()
    naive, harm = run(Image.open(a.bg), Image.open(a.fg), a.x, a.y, a.h, finish_on=a.finish)
    if a.compare:
        out = np.concatenate([naive, harm], 1)
    else:
        out = harm
    Image.fromarray((np.clip(out, 0, 1) * 255).astype(np.uint8)).save(a.out)


if __name__ == '__main__':
    main()
