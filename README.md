# obs-harmonizer

配信で、前景（立ち絵・アバター）を背景の色味と光に自動でなじませる OBS フィルターです。
背景ソースを指定するだけで、その場に立っているように見せます。背景が動いても毎フレーム追従します。

*English: OBS filters that automatically match a foreground (avatar / character art) to a background source — the scene's light color and brightness, shade, light from bright sources in the background (street lamps, windows, signs) and rim light — plus a finishing filter (halftone, grain, light leak). GPU only, no CPU readback. GPL-2.0-or-later.*

## できること

フィルターは 2 つあります。

### 1. 背景になじませる (Harmonizer) … 前景のソースに掛ける

| 項目 | 内容 |
| --- | --- |
| 光の色を乗せる | その場の光の色で前景を染めます。夕焼けの前では白がクリーム色に、夜は青みがかります。昼の青空や川の色は光の色とみなさないので、昼はほとんど変わりません |
| 明るさを合わせる | 夜などの暗い場面で前景を暗くします。明るい場面では変えません |
| コントラストの追従 | 前景の明暗の幅を背景に寄せます（初期値は 0） |
| 暗部に空気の色を乗せる | 黒い服や髪などの暗い所に、背景の暗部の色（夜なら青など）を少し乗せます |
| 自動陰影 | 前景の各点の**背後にある背景**を読みます。まわりより暗い所（木陰など）の前では影に入ります。背景の中のはっきり明るい所（街灯・窓・看板・灯籠）は光源とみなし、その光が**光源の色のまま**、近い側から前景を照らします（光源を向いた側の面と縁が強く光ります）。ふつうの明るさのむらでは何もしないので、昼の背景の色が前景に写り込みません |
| 縁の光 | 背景がまわりより明るい所に接する輪郭を、その場の光の色で光らせます（ライトラップ）。光源側だけに絞れます |
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
背景ソース ─ 256x144 に描画 ─ 32x18 のブロック平均 ┬ 統計 6x1（平均色・ハイライト色・暗部の色・明るさの分布・その場の白と黒）
                                                ├ ぼかし ─ 環境光マップ 32x18
                                                └ 明るい所だけ残してぼかし ─ 光源マップ 32x18
前景（フィルターの入力）─ 32x32 ─ 統計 1x1
本体：前景の各画素で、統計と「背後の環境光マップ・光源マップ」を参照して色・陰影・光源の光・縁の光・手置きの光を計算
```

統計と 2 つのマップは前フレームとの指数平滑で、ちらつきを抑えます。追加の負荷は、前景の画素数ぶんのシェーダー 1 回と、ごく小さい前処理だけです。

参照実装（numpy）が `tools/reference.py` にあります。シェーダーと同じ式で、見た目の検討に使えます。

```
python3 tools/reference.py --bg 背景.jpg --fg 立ち絵.png --out out.png --compare --finish
```

## インストール

[Releases](https://github.com/yub4-arema/obs-harmonizer/releases) から OS に合ったファイルをダウンロードします。
配置先は OBS 公式の[プラグインガイド](https://obsproject.com/kb/plugins-guide)に従っています。

- **Windows**: zip を展開し、中の `obs-harmonizer` フォルダを `C:\ProgramData\obs-studio\plugins` に置きます（公式の推奨場所。下の注意を参照）。
- **macOS**: `.pkg` を実行します（このインストーラーは `/Library/Application Support/obs-studio/plugins/obs-harmonizer.plugin` に入れます）。
- **Linux**: `.deb` を `sudo apt install ./ファイル名.deb` で入れます（`/usr/lib/x86_64-linux-gnu/obs-plugins/` と `/usr/share/obs/obs-plugins/obs-harmonizer/` に入ります）。

OBS を再起動すると、フィルターの一覧に追加されます。

### 手動で置く場合

公式ガイドの配置先は次のとおりです。

| OS | 配置先 |
| --- | --- |
| Windows（推奨） | `C:\ProgramData\obs-studio\plugins` |
| macOS | `~/Library/Application Support/obs-studio/plugins`（プラグインごとの `.plugin` バンドル） |
| Linux | `~/.config/obs-studio/plugins` |

> **Windows の注意**
> `C:\ProgramData\obs-studio\plugins` は、OBS 本体のインストール先（`C:\Program Files\obs-studio`。中に `bin`・`data`・`obs-plugins` がある）とは**別の場所**です。
> `ProgramData` は初期状態では隠しフォルダで、`obs-studio\plugins` も最初は存在しません。エクスプローラーのアドレス欄に `C:\ProgramData` と入力して開き、`obs-studio` と `plugins` のフォルダが無ければ作ってください。
> `C:\Program Files\obs-studio\obs-plugins\64bit` に置く旧来の方法は、公式ガイドで「将来の OBS で動かなくなる」とされているため、使わないでください。

Windows の ProgramData と Linux のユーザーフォルダには、次のフォルダ構成が必要です（公式ガイドの説明）。

- プラグイン名のフォルダの中に `bin` と `data` を置く
- `bin` の中に `64bit` を作り、プラグインの本体（`.dll` / `.so`）を入れる。ファイル名はプラグインのフォルダ名と一致させる
- `data` の中に `locale` を作り、翻訳の `.ini` を入れる

```
obs-harmonizer/
├─ bin/64bit/obs-harmonizer.dll   （Linux は .so）
└─ data/
   ├─ effects/*.effect            シェーダー
   └─ locale/*.ini                翻訳
```

`data` の中身が無いと、シェーダーを読めずフィルターが動きません。

## リポジトリの構成

```
src/        プラグイン本体（C）
              plugin-main.c      プラグインの登録
              harmonizer-filter.c / finish-filter.c   2 つのフィルター
              hz-util.[ch]       共通処理
data/       OBS がプラグインの data として読むファイル
              effects/           シェーダー（*.effect）
              locale/            表示文字列（ja-JP.ini, en-US.ini）
tools/      参照実装（reference.py）とヘッドレス描画ツール
tests/      ヘッドレスのテスト
handover/   開発の記録（見た目の評価の結果、評価用の画像を作るスクリプト）。使うだけなら不要です
CHANGELOG.md  変更履歴
cmake/ build-aux/ .github/ CMakePresets.json buildspec.json
            obs-plugintemplate 由来のビルド設定とワークフロー
```

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
sudo apt install obs-studio libobs-dev ninja-build xvfb python3-numpy python3-pil libx11-dev
cmake --preset ubuntu-x86_64 -DHZ_BUILD_TOOLS=ON
cmake --build build_x86_64
xvfb-run -a python3 tests/headless_test.py build_x86_64
```

## 変更履歴

[CHANGELOG.md](CHANGELOG.md) にあります。

## ライセンス

GPL-2.0-or-later（`LICENSE`）。ビルド設定とワークフローは obs-plugintemplate（GPL-2.0）に基づきます。
