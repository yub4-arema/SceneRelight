#!/usr/bin/env python3
"""OBS をヘッドレスで動かすテスト（Linux, Xvfb 上で実行）。

  xvfb-run -a python3 tests/headless_test.py build

合成画像だけを使う（著作物の素材は使わない）。確かめること:
  1. シェーダーがコンパイルできる（ログにエラーが出ない）
  2. Harmonizer は前景だけを変え、背景はそのまま
  3. 前景の白が背景の光の色（暖色）に寄る
  4. 前景の陰影が背景の明暗の向きに従う（右が暗い背景 -> 前景も右が暗い）
  5. OBS の出力が tools/reference.py とおおむね一致する
  6. Finish が画面を変える
  7. 暗い背景の中の光源（街灯）が、近い側から前景を照らす。OBS の出力が参照実装と一致する
  8. Finish の色収差・周辺ボケ・グロー・レンズフレアが、画面の狙った場所だけを変え、参照実装と一致する
  9. 背景ぼかし（Lens Blur）が背景だけをぼかし、前景を変えず、参照実装と一致する
"""
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import reference  # noqa: E402

W, H = 1920, 1080
FAILED = []


def check(cond, msg):
    print(('ok   ' if cond else 'FAIL ') + msg)
    if not cond:
        FAILED.append(msg)


def make_assets(d):
    # 背景：左が明るい夕焼け色、右が暗い
    x = np.linspace(0, 1, W)[None, :, None]
    y = np.linspace(0, 1, H)[:, None, None]
    warm = np.array([1.0, 0.72, 0.42])
    dark = np.array([0.18, 0.12, 0.14])
    bg = warm * (1 - x) + dark * x
    bg = bg * (0.85 + 0.15 * (1 - y))
    Image.fromarray((np.clip(bg, 0, 1) * 255).astype(np.uint8)).save(os.path.join(d, 'bg.png'))
    # 夜の背景：暗い青の中、右側に暖色の光源
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    lamp = np.exp(-(((xx - W * 0.72) / (W * 0.05)) ** 2 + ((yy - H * 0.4) / (H * 0.12)) ** 2))
    night = np.array([0.05, 0.09, 0.2]) * (1 - lamp[..., None]) + np.array([1.0, 0.85, 0.55]) * lamp[..., None]
    Image.fromarray((np.clip(night, 0, 1) * 255).astype(np.uint8)).save(os.path.join(d, 'bg_night.png'))
    # 真っ暗な夜：ほぼ黒の中に、小さな暖色の灯りが右側に点在する（灯籠の並ぶ参道など）
    lamps = np.zeros((H, W), np.float32)
    for cx, cy in ((0.63, 0.4), (0.75, 0.3), (0.88, 0.5)):
        lamps += np.exp(-(((xx - W * cx) / (W * 0.015)) ** 2 + ((yy - H * cy) / (H * 0.04)) ** 2))
    lamps = np.clip(lamps, 0, 1)
    dark = np.array([0.03, 0.03, 0.07]) * (1 - lamps[..., None]) + np.array([1.0, 0.75, 0.4]) * lamps[..., None]
    Image.fromarray((np.clip(dark, 0, 1) * 255).astype(np.uint8)).save(os.path.join(d, 'bg_dark.png'))
    # 明るい点が 1 つもない暗い背景（フレアが出ないことの確認用）
    Image.fromarray(np.full((H, W, 3), 14, np.uint8)).save(os.path.join(d, 'bg_black.png'))
    # 灯りのある夜景：建物の窓と、小さく強い街灯 3 つ（グロー・フレア・ぼかしの確認用）
    u, v = xx / W, yy / H
    lights = np.stack([0.05 + 0.10 * (1 - v), 0.08 + 0.12 * (1 - v), 0.2 + 0.25 * (1 - v)], -1)
    rng = np.random.default_rng(3)
    for i in range(14):
        x0 = int(i * W / 14)
        bw = int(W / 14) - 8
        top = int(H * (0.35 + 0.3 * rng.random()))
        lights[top:, x0:x0 + bw] = np.array([0.04, 0.04, 0.08])
        for wy in range(top + 20, H - 40, 46):
            for wx in range(x0 + 10, x0 + bw - 14, 34):
                if rng.random() < 0.45:
                    lights[wy:wy + 22, wx:wx + 16] = np.array([1.0, 0.85, 0.5]) * (0.7 + 0.3 * rng.random())
    for cx, cy, col in ((0.2, 0.3, (1, 0.9, 0.7)), (0.55, 0.22, (0.7, 0.9, 1)), (0.82, 0.38, (1, 0.6, 0.5))):
        rr = np.sqrt((xx - cx * W) ** 2 + (yy - cy * H) ** 2)
        lights += np.exp(-(rr / 6) ** 2)[..., None] * np.array(col) * 1.5
    Image.fromarray((np.clip(lights, 0, 1) * 255).astype(np.uint8)).save(os.path.join(d, 'bg_lights.png'))
    # 前景：白〜灰色の縦長の楕円（縁は少しぼかす）
    fw, fh = 400, 1000
    yy, xx = np.mgrid[0:fh, 0:fw].astype(np.float32)
    r = np.sqrt(((xx - fw / 2) / (fw * 0.45)) ** 2 + ((yy - fh / 2) / (fh * 0.48)) ** 2)
    a = np.clip((1 - r) * 40, 0, 1)
    shade = 0.95 - 0.35 * (yy / fh)
    rgb = np.stack([shade] * 3, -1)
    rgb[(yy > fh * 0.55)] *= 0.25  # 下半分は黒っぽい服
    fg = np.concatenate([rgb, a[..., None]], -1)
    Image.fromarray((fg * 255).astype(np.uint8), 'RGBA').save(os.path.join(d, 'fg.png'))


def render(build, d, name, *opts, bg='bg.png'):
    out = os.path.join(d, name + '.rgba')
    cmd = [os.path.join(build, 'hz-headless-render'), os.path.join(build, 'obs-scene-relight.so'),
           os.path.join(ROOT, 'data'), os.path.join(d, bg), os.path.join(d, 'fg.png'), out,
           '--x', '0.5', '--h', '0.9', '--y', '0.95', *opts]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    log = p.stdout + p.stderr
    check(p.returncode == 0, f'{name}: render exited 0')
    bad = [l for l in log.splitlines() if 'Error compiling' in l or 'failed to load' in l
           or 'missing vertex shader' in l or 'missing pixel shader' in l
           or (l.startswith('error:') and 'Double destroy' not in l)]
    check(not bad, f'{name}: no shader/effect errors' + ('' if not bad else ': ' + bad[0]))
    raw = np.frombuffer(open(out, 'rb').read(), np.uint8).reshape(H, W, 4)[..., :3].astype(np.float32) / 255
    return raw


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build')
    with tempfile.TemporaryDirectory() as d:
        make_assets(d)
        bg_ref = np.asarray(Image.open(os.path.join(d, 'bg.png')).convert('RGB')).astype(np.float32) / 255

        naive = render(build, d, 'naive', '--no-harmonizer')
        # 出力のバイト順は環境で RGBA/BGRA が変わるので、背景と合う方に揃える
        swap = np.abs(naive[..., ::-1] - bg_ref).mean() < np.abs(naive - bg_ref).mean()
        fix = (lambda a: a[..., ::-1]) if swap else (lambda a: a)
        naive = fix(naive)
        check(np.abs(naive[:, :300] - bg_ref[:, :300]).mean() < 0.01, 'naive: background reproduced')

        harm = fix(render(build, d, 'harm'))
        fin = fix(render(build, d, 'finish', '--finish'))

        # 前景の領域（前景の画像から計算）
        fg_img = Image.open(os.path.join(d, 'fg.png'))
        L, T, Wf, Hf = reference.place(W, H, fg_img.width, fg_img.height, 0.5, 0.95, 0.9)
        x0, y0, x1, y1 = int(L) + 40, int(T) + 60, int(L + Wf) - 40, int(T + Hf * 0.45)
        fg_n = naive[y0:y1, x0:x1]
        fg_h = harm[y0:y1, x0:x1]

        check(np.abs(harm[:, :300] - naive[:, :300]).mean() < 0.002, 'harmonizer: background untouched')
        check(np.abs(fg_h - fg_n).mean() > 0.03, 'harmonizer: foreground changed')
        rb_n = (fg_n[..., 0] - fg_n[..., 2]).mean()
        rb_h = (fg_h[..., 0] - fg_h[..., 2]).mean()
        check(rb_h > rb_n + 0.05, f'harmonizer: whites warmed toward background (R-B {rb_n:.3f} -> {rb_h:.3f})')
        mid = (x0 + x1) // 2
        lum = lambda a: (a @ reference.LUMA).mean()
        left_ratio = lum(harm[y0:y1, x0:mid]) / max(lum(naive[y0:y1, x0:mid]), 1e-4)
        right_ratio = lum(harm[y0:y1, mid:x1]) / max(lum(naive[y0:y1, mid:x1]), 1e-4)
        check(left_ratio > right_ratio + 0.03,
              f'harmonizer: shading follows background (left x{left_ratio:.2f}, right x{right_ratio:.2f})')

        _, ref = reference.run(Image.open(os.path.join(d, 'bg.png')), fg_img, 0.5, 0.95, 0.9, finish_on=False)
        diff = np.abs(harm - ref)[y0:y1, x0:x1].mean()
        check(diff < 0.03, f'harmonizer: matches reference implementation (mean abs diff {diff:.4f})')

        check(np.abs(fin - harm).mean() > 0.01, 'finish: frame changed')

        # 夜と光源
        night = fix(render(build, d, 'night', bg='bg_night.png'))
        bgn = Image.open(os.path.join(d, 'bg_night.png'))
        nv, ref = reference.run(bgn, fg_img, 0.5, 0.95, 0.9, finish_on=False)
        check(lum(night[y0:y1, mid:x1]) > lum(night[y0:y1, x0:mid]) * 1.15,
              f'night: lit from the lamp side (left {lum(night[y0:y1, x0:mid]):.3f}, right {lum(night[y0:y1, mid:x1]):.3f})')
        diff = np.abs(night - ref)[y0:y1, x0:x1].mean()
        check(diff < 0.03, f'night: matches reference implementation (mean abs diff {diff:.4f})')

        # 真っ暗な夜に小さな灯り：全体はしっかり暗く、灯りの側だけが照らされる
        dk = fix(render(build, d, 'dark', bg='bg_dark.png'))
        nv, ref = reference.run(Image.open(os.path.join(d, 'bg_dark.png')), fg_img, 0.5, 0.95, 0.9, finish_on=False)
        q = (x1 - x0) // 4
        far = lum(dk[y0:y1, x0:x0 + q]) / lum(nv[y0:y1, x0:x0 + q])
        near = lum(dk[y0:y1, x1 - q:x1]) / lum(nv[y0:y1, x1 - q:x1])
        check(far < 0.6, f'dark: the side away from the lamps is dark (x{far:.2f})')
        check(near > far * 1.1, f'dark: lit from the lamp side (away x{far:.2f}, near x{near:.2f})')
        diff = np.abs(dk - ref)[y0:y1, x0:x1].mean()
        check(diff < 0.03, f'dark: matches reference implementation (mean abs diff {diff:.4f})')


        # ---- Finish の新機能（色収差・周辺ボケ・グロー・レンズフレア）。他の仕上げは切って、1 つずつ確かめる
        lights = Image.open(os.path.join(d, 'bg_lights.png'))
        fin_base = dict(halftone=0.0, grain=0.0, leak=0.0, vignette=0.0)

        def fset(**k):
            o = ['--finish']
            for kk, vv in dict(fin_base, **k).items():
                o += ['--fset', f'{kk}={float(vv)}']
            return o

        base = fix(render(build, d, 'lights', '--no-harmonizer', bg='bg_lights.png'))
        gray = lambda a: a @ reference.LUMA
        yy, xx = np.mgrid[0:H, 0:W]
        rn = np.sqrt(((xx / W - 0.5) * W / H) ** 2 + (yy / H - 0.5) ** 2) / np.sqrt(0.25 * (W / H) ** 2 + 0.25)
        center, edge = rn < 0.15, rn > 0.8

        def vs_ref(name, out, **k):
            ref = reference.finish(base, dict(reference.FINISH_DEFAULTS, **fin_base, **k))
            diff = np.abs(out - ref).mean()
            check(diff < 0.01, f'{name}: matches reference implementation (mean abs diff {diff:.4f})')

        # 色収差：中心は変わらず、周辺だけ色がずれる
        o = fix(render(build, d, 'chroma', '--no-harmonizer', *fset(chromatic=0.5), bg='bg_lights.png'))
        dd = np.abs(o - base).mean(-1)
        check(dd[center].mean() < 0.002 and dd[edge].mean() > 0.004,
              f'chromatic: only the edges change (center {dd[center].mean():.4f}, edge {dd[edge].mean():.4f})')
        vs_ref('chromatic', o, chromatic=0.5)

        # 周辺ボケ：中心は変わらず、周辺は細かい模様が減る
        o = fix(render(build, d, 'peri', '--no-harmonizer', *fset(peri_blur=0.8), bg='bg_lights.png'))
        dd = np.abs(o - base).mean(-1)
        gx = lambda a: np.abs(np.diff(gray(a), axis=1))
        sharp_b, sharp_o = gx(base)[edge[:, 1:]].mean(), gx(o)[edge[:, 1:]].mean()
        check(dd[center].mean() < 0.002 and sharp_o < sharp_b * 0.8,
              f'peri blur: center untouched, edges softer (center {dd[center].mean():.4f}, edge detail {sharp_b:.4f} -> {sharp_o:.4f})')
        vs_ref('peri blur', o, peri_blur=0.8)

        # グロー：明るいもの（前景の白い楕円）のすぐ外側の暗い空が明るくなり、遠くは変わらない
        o = fix(render(build, d, 'glow', '--no-harmonizer', *fset(glow=0.6), bg='bg_lights.png'))
        lamp = np.sqrt((xx - 0.2 * W) ** 2 + (yy - 0.3 * H) ** 2)
        near, far = (xx > 770) & (xx < 800) & (yy > 250) & (yy < 350), (xx > W * 0.7) & (yy < H * 0.15)
        d_near, d_far = (gray(o) - gray(base))[near].mean(), (gray(o) - gray(base))[far].mean()
        check(d_near > 0.03 and abs(d_far) < 0.003,
              f'glow: light spills around a bright object (near +{d_near:.3f}, far {d_far:+.4f})')
        vs_ref('glow', o, glow=0.6)

        # レンズフレア：最も明るい点（左の街灯。右の 2 つは前景の陰か少し暗い）に付く
        o = fix(render(build, d, 'flare', '--no-harmonizer', *fset(flare=0.8), bg='bg_lights.png'))
        dl = (gray(o) - gray(base))
        at_lamp, corner = lamp < 120, (xx > W * 0.88) & (yy > H * 0.88)
        check(dl[at_lamp].mean() > 0.05 and abs(dl[corner].mean()) < 0.01,
              f'flare: placed on the brightest point (at lamp +{dl[at_lamp].mean():.3f}, far corner {dl[corner].mean():+.4f})')
        vs_ref('flare', o, flare=0.8)
        # 画面に明るい点がなければ出ない（暗い背景で、前景だけを置く）
        o = fix(render(build, d, 'flare_dark', *fset(flare=0.8), bg='bg_black.png'))
        o0 = fix(render(build, d, 'flare_dark0', *fset(flare=0.0), bg='bg_black.png'))
        check(np.abs(o - o0).max() < 0.02, f'flare: nothing appears without a bright point (max change {np.abs(o - o0).max():.3f})')

        # ---- 背景ぼかし：背景だけがぼけて、前景は変わらない
        o = fix(render(build, d, 'lensblur', '--no-harmonizer', '--lensblur', '--lset', 'radius=2.0', '--lset', 'bokeh=0.5',
                       bg='bg_lights.png'))
        sharp_b, sharp_o = gx(base)[:, :300].mean(), gx(o)[:, :300].mean()
        fg_in = (xx > x0 + 80) & (xx < x1 - 80) & (yy > y0 + 80) & (yy < y1 - 20)
        check(sharp_o < sharp_b * 0.5, f'lens blur: the background is blurred (detail {sharp_b:.4f} -> {sharp_o:.4f})')
        check(np.abs(o - base)[fg_in].mean() < 0.003, 'lens blur: the foreground is untouched')
        blurred = reference.lens_blur(np.asarray(lights.convert('RGB')).astype(np.float32) / 255, 2.0, 0.5, 0.0)
        # 半径 0 は素通し
        o0 = fix(render(build, d, 'lensblur0', '--no-harmonizer', '--lensblur', '--lset', 'radius=0.0', bg='bg_lights.png'))
        check(np.abs(o0 - base).max() < 0.01, 'lens blur: radius 0 passes the image through')
        # 「背景になじませる」は、ぼかした後の背景を読む（参照実装でも、ぼかした背景を渡して比べる）
        lb = fix(render(build, d, 'lb_harm', '--lensblur', '--lset', 'radius=2.0', '--lset', 'bokeh=0.5', bg='bg_lights.png'))
        blur_img = Image.fromarray((np.clip(blurred, 0, 1) * 255 + 0.5).astype(np.uint8))
        _, ref = reference.run(blur_img, fg_img, 0.5, 0.95, 0.9, finish_on=False)
        diff = np.abs(lb - ref)[y0:y1, x0:x1].mean()
        check(diff < 0.03, f'lens blur + harmonizer: the foreground follows the blurred background (mean abs diff {diff:.4f})')

        bgonly = np.ones((H, W), bool)
        bgonly[int(T) - 5:int(T + Hf) + 5, int(L) - 5:int(L + Wf) + 5] = False
        diff = np.abs(o - blurred)[bgonly].mean()
        check(diff < 0.01, f'lens blur: matches reference implementation (mean abs diff {diff:.4f})')

    if FAILED:
        print(f'\n{len(FAILED)} check(s) failed')
        sys.exit(1)
    print('\nall checks passed')


if __name__ == '__main__':
    main()
