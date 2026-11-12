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


def render(build, d, name, *opts):
    out = os.path.join(d, name + '.rgba')
    cmd = [os.path.join(build, 'hz-headless-render'), os.path.join(build, 'obs-harmonizer.so'),
           os.path.join(ROOT, 'data'), os.path.join(d, 'bg.png'), os.path.join(d, 'fg.png'), out,
           '--x', '0.5', '--h', '0.9', '--y', '0.95', *opts]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    log = p.stdout + p.stderr
    check(p.returncode == 0, f'{name}: render exited 0')
    bad = [l for l in log.splitlines() if 'Error compiling' in l or 'failed to load' in l
           or 'missing vertex shader' in l or 'missing pixel shader' in l]
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

    if FAILED:
        print(f'\n{len(FAILED)} check(s) failed')
        sys.exit(1)
    print('\nall checks passed')


if __name__ == '__main__':
    main()
