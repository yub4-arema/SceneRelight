# obs-harmonizer

配信で、前景（立ち絵・アバター）を背景の色味と光に自動でなじませる OBS フィルターです。
背景ソースを指定するだけで、その場に立っているように見せることができ、背景が動いても毎フレーム追従します。

*English: OBS filters that automatically match a foreground (avatar / character art) to a background source — the scene's light color and brightness, shade, light from bright sources in the background (street lamps, windows, signs) and rim light — plus a finishing filter (halftone, grain, light leak). GPU only, no CPU readback. GPL-2.0-or-later.*

## できること

### 1. 背景になじませる (Harmonizer)

| 項目 | 内容 |
| --- | --- |
| 光の色を乗せる | その場の光の色に前景を合わせます。夕焼けの前では白がクリーム色に、夜は青みがかります。|
| 明るさを合わせる | 夜などの暗い場面で前景を暗くします。|
| コントラストの追従 | 前景の明暗の幅を背景に寄せます（初期値は 0） |
| 暗部に色を乗せる | 黒い服や髪などの暗い所に、背景の暗部の色（夜なら青など）を少し乗せます |
| 自動陰影 | 前景の背後にある背景を読みます。まわりより暗い所（木陰など）の前では影に入ります。背景の中のはっきり明るい所（街灯・窓・看板・灯籠）は光源とみなし、その光が光源の色のまま、前景を照らします（光源を向いた側の面と縁が強く光ります）。|
| 光の表現 | 背景がまわりより明るい所に接する輪郭を、その場の光の色で光らせます（ライトラップ）。光源側だけに絞れます |
| 手置きの光と影 | 直線（片側を明るく／暗く）か円（スポット）を、位置・向き・ぼかし・量で置けます。量をマイナスにすると影です。(2つ設置ができます)|
| 時間方向のなめらかさ | 背景が動いたときのちらつきを抑えます|

前景と背景が同じシーンにあれば、シーン内の配置（位置・拡大率・回転）を自動で読みます。

### 2. 仕上げ：網点・グレイン・光漏れ (Harmonizer)

前景と背景にまとめて掛けることで、同じカメラで撮った雰囲気を表現します。

| 項目 | 内容 |
| --- | --- |
| 網点 | 暗い所と画面の周辺ほど濃く入る網点 |
| グレイン | フィルムの粒子。フレームごとに動かせます |
| 光漏れ | 画面の一番明るい所の外側から光が差し込みます。位置と色は自動か手動を選択できます。 |
| 周辺減光 | ビネット |

## 使い方

1. 背景（画像・動画・シーンなど）と前景（立ち絵・アバター）を同じシーンに置く
2. 前景のソースにフィルター「背景になじませる (Harmonizer)」を追加し、「背景ソース」で背景を選ぶ
3. 必要ならシーン（またはグループ）にフィルター「仕上げ：網点・グレイン・光漏れ (Harmonizer)」を追加する

## 仕組みと負荷

すべて GPU で処理し、CPU への読み戻しはありません。前景 1 枚あたり 1 フレームの処理は次のとおりです。

```
背景ソース ─ 256x144 に描画 ─ 32x18 のブロック平均 ┬ 統計 6x1（平均色・ハイライト色・暗部の色・明るさの分布・その場の白と黒）
                                                ├ ぼかし ─ 環境光マップ 32x18
                                                └ 明るい所だけ残してぼかし ─ 光源マップ 32x18
前景（フィルターの入力）─ 32x32 ─ 統計 1x1
本体：前景の各画素で、統計と「背後の環境光マップ・光源マップ」を参照して色・陰影・光源の光・縁の光・手置きの光を計算
```

統計と 2 つのマップは前フレームとの指数平滑で、ちらつきを抑えます。

参照実装（numpy）が `tools/reference.py` にあります。シェーダーと同じ式で、見た目の検討に使うことができますので、ご活用下さい。

```
python3 tools/reference.py --bg 背景.jpg --fg 立ち絵.png --out out.png --compare --finish
```

## インストール

[Releases](https://github.com/yub4-arema/obs-harmonizer/releases) から OS に合ったファイルをダウンロードします。
配置先は OBS 公式の[プラグインガイド](https://obsproject.com/kb/plugins-guide)に準拠しています。

- **Windows**: zip を展開し、中の `obs-harmonizer` フォルダを `C:\ProgramData\obs-studio\plugins` に置きます（公式の推奨場所。下の注意を参照）。
- **macOS**: `.pkg` を実行します（このインストーラーは `/Library/Application Support/obs-studio/plugins/obs-harmonizer.plugin` に入れます）。
- **Linux**: `.deb` を `sudo apt install ./ファイル名.deb` で入れます（`/usr/lib/x86_64-linux-gnu/obs-plugins/` と `/usr/share/obs/obs-plugins/obs-harmonizer/` に入ります）。

OBS を再起動すると、フィルターの一覧に追加されます。

## 変更履歴

[CHANGELOG.md](CHANGELOG.md) にあります。

## ライセンス

GPL-2.0-or-later（`LICENSE`）。ビルド設定とワークフローは obs-plugintemplate（GPL-2.0）に基づきます。
