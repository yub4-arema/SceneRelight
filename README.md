# obs-harmonizer

配信で、前景（立ち絵・アバター）を背景の色味と光に自動でなじませる OBS フィルターです。
背景ソースを指定するだけで、その場に立っているように見せます。背景が動いても毎フレーム追従します。

*English: OBS filters that automatically match a foreground (avatar / character art) to a background source — light color, exposure, contrast, directional shading and rim light — plus a finishing filter (halftone, grain, light leak). GPU only, no CPU readback. GPL-2.0-or-later.*

## できること

フィルターは 2 つあります。

### 1. 背景になじませる (Harmonizer) … 前景のソースに掛ける

| 項目 | 内容 |
| --- | --- |
| 光の色を乗せる | 背景のハイライトの色で前景を染めます。夕焼けの前では白がクリーム色になります |
| 明るさ・コントラストの追従 | 前景の明るさと明暗の幅を背景に寄せます。黒は黒のまま保ちます |
| 暗部を背景の色に寄せる | 影の部分に背景の暗い色（夜なら青など）を少し混ぜます |
| 自動陰影 | 前景の各点の**背後にある背景**の明るさを読み、暗い側は暗く、明るい側は明るくします。右側が木陰の背景なら、前景も右側が暗くなります |
| 縁の光 | 背景の明るい所に接する輪郭を光らせます（ライトラップ）。光源側だけに絞れます |
| 手置きの光と影（2 枠） | 直線（片側を明るく／暗く）か円（スポット）を、位置・向き・ぼかし・量で置けます。量をマイナスにすると影です。色は背景から自動で決めるか、手で指定します |
| 時間方向のなめらかさ | 背景が動いたときのちらつきを抑えます（秒） |

前景と背景が同じシーンにあれば、シーン内の配置（位置・拡大率・回転）を自動で読みます。

### 2. 仕上げ：網点・グレイン・光漏れ (Finish) … シーンやグループに掛ける

前景と背景にまとめて掛けると「同じカメラで撮った感じ」が出ます。

| 項目 | 内容 |
| --- | --- |
| 網点 | 暗い所と画面の周辺ほど濃く入る網点 |
| グレイン | フィルムの粒子。フレームごとに動かせます |
| 光漏れ | 画面の一番明るい所の外側から光が差し込みます。位置と色は自動か手動 |
| 周辺減光 | ビネット |

## 使い方

1. 背景（画像・動画・シーンなど）と前景（立ち絵・アバター）を同じシーンに置く
2. 前景のソースにフィルター「背景になじませる (Harmonizer)」を追加し、「背景ソース」で背景を選ぶ
3. 必要ならシーン（またはグループ）にフィルター「仕上げ (Harmonizer)」を追加する

おすすめの順番：まず「全体の強さ」で効き具合を決め、次に「自動陰影」と「縁の光」を調整します。
自動で足りない影や光は「手置きの光と影」で足します。

## 仕組みと負荷

すべて GPU で処理し、CPU への読み戻しはありません。前景 1 枚あたり 1 フレームの処理は次のとおりです。

```
背景ソース ─ 256x144 に描画 ─ 32x18 のブロック平均 ┬ 統計 4x1（平均色・ハイライト色・暗部の色・明るさの分布）
                                                └ ぼかし ─ 環境光マップ 32x18
前景（フィルターの入力）─ 32x32 ─ 統計 1x1
本体：前景の各画素で、統計と「背後の環境光マップ」を参照して色・陰影・縁の光・手置きの光を計算
```

統計と環境光マップは前フレームとの指数平滑で、ちらつきを抑えます。追加の負荷は、前景の画素数ぶんのシェーダー 1 回と、ごく小さい前処理だけです。

参照実装（numpy）が `tools/reference.py` にあります。シェーダーと同じ式で、見た目の検討に使えます。

```
python3 tools/reference.py --bg 背景.jpg --fg 立ち絵.png --out out.png --compare --finish
```

## インストール

[Releases](https://github.com/yub4-arema/obs-harmonizer/releases) から OS に合ったファイルをダウンロードします。

- **Windows**: zip を展開し、中の `obs-plugins` と `data` を OBS のインストール先（通常は `C:\Program Files\obs-studio`）の同じディレクトリに追加します
- **macOS**: `.pkg` を実行します
- **Linux**: `.deb` を `sudo apt install ./ファイル名.deb` で入れます

OBS を再起動すると、フィルターの一覧に追加されます。

## ビルド

[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) の構成に従っています。

### Windows

必要なもの：

- Visual Studio 2022（「C++ によるデスクトップ開発」ワークロード。Windows SDK 10.0.22621 以降を含む）
- CMake 3.28 以降
- Git

```
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

OBS のソースと依存ライブラリは、初回の構成時に自動で取得されます。

### macOS

必要なもの：Xcode 16 以降、CMake 3.28 以降

```
cmake --preset macos
cmake --build --preset macos
```

### Linux（Ubuntu 24.04 で確認）

```
sudo apt install build-essential cmake ninja-build pkg-config libobs-dev
cmake --preset ubuntu-x86_64
cmake --build build_x86_64
```

## テスト

Linux では OBS をヘッドレス（Xvfb と Mesa のソフトウェア描画）で動かし、実際にフィルターを掛けた 1 フレームを確かめます。テストには合成画像だけを使います。

```
sudo apt install obs-studio xvfb python3-numpy python3-pil libx11-dev
cmake --preset ubuntu-x86_64 -DHZ_BUILD_TOOLS=ON
cmake --build build_x86_64
xvfb-run -a python3 tests/headless_test.py build_x86_64
```

## ライセンス

GPL-2.0-or-later（`LICENSE`）。ビルド設定とワークフローは obs-plugintemplate（GPL-2.0）に基づきます。
