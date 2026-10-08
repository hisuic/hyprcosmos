# Hyprcosmos / Cosmic

手を止めると、作業中のウィンドウの映像が宇宙になります。カーソルに沿う
公転、連星、反発、ブラックホール、スパゲッティ化、ワームホール、超新星、
膨張、時間逆行。通常のキー・クリック・スクロールで即座に作業へ戻ります。
アプリの配置・サイズ・所属は変更しません。

20秒の無入力で、壁紙を覆う深い紺色の宇宙背景に切り替わります。
小さなASCIIの `.`・`+`・`*` が銀青色に瞬き、ときどきASCIIの尾を引く
流れ星が横切ります。通常入力で星空も即座に消え、元の壁紙へ戻ります。
壁紙のファイルや壁紙アプリの設定は変更しません。
Cosmic中はWaybarや通常の通知・ウィジェット・OSDなどのlayer-shell UIも
描画だけを隠し、画面全体を宇宙にします。プロセス停止、パネルの予約領域や
アプリの配置変更は行わず、最初の通常入力で元のUIへ戻します。

対応対象は **Hyprland 0.56.2**、コミット
`efb50993780079460b0cbed1363e2166a2de1d9f` と一致する開発ヘッダーです。
実際に確認した環境は Arch Linux / GCC 16 / OpenGL / 1920×1080 scale=1。
プラグインは本体とライブラリーの ABI 一致を読み込み時に検査します。
Hyprland 本体の更新・再ビルドは不要です。他の版への対応は未確認です。

## 導入

旧版からの更新には、下記の `scripts/update.sh` を使用してください。
旧版には解除後の描画パスが残ってクラッシュする不具合があり、今回修正しました。
旧プラグインは修正版の終了処理を使えないため、更新時には先に停止し、
旧版がロードされたまま通常フレームが完了したことを確認してから解除します。
経緯と確認範囲は [検証結果](docs/validation.md) を参照してください。

必要なもの: `Hyprland` と対応する開発ヘッダー、GCC（本体と同じコンパイラー）、
CMake、pkg-config、Lua 5.5、hyprutils、hyprgraphics、Aquamarine、Wayland、
GLES、pixman、libdrm の開発用ファイル。現在の Arch 環境には導入済みです。

```sh
cd hyprcosmos
./scripts/install.sh
hyprctl reload
```

初回はプラグインをビルドしてテストし、設定ディレクトリーにリポジトリ内の
Lua モジュールと `build/cosmic.so` へのシンボリックリンクを作ります。
`hyprland.lua` の実体をバックアップしてから、マーカー付きで次の一行を追記します。

```lua
require("cosmic")
```

`require` だけでは C++ のビルド・配置を代替できません。インストーラーが
それらを済ませます。コードとビルド成果物はこのリポジトリ内に残るため、
導入後にこのディレクトリーを移動する場合は一度解除して再導入してください。

設定先の指定と確認:

```sh
./scripts/install.sh --dry-run
./scripts/install.sh --config /path/to/hyprland.lua
./scripts/build.sh --jobs 2
```

インストーラー自体はセッションを暗黙に再読み込みしません。同時に行う場合は
`--reload --instance <hyprctl instances に表示される signature>` を指定します。

ビルド済みのプラグインは内容ハッシュ付きの読み取り専用ファイルへ公開し、
`build/cosmic.so` は原子的に切り替えるリンクです。再ビルドで使用中の共有
ライブラリーを上書きしません。C++コード更新を反映する場合は、同じパスの
読み込みを本体が保持するため、安全な更新スクリプトで解除・再導入します。

```sh
hyprctl instances
./scripts/update.sh --instance <更新するsignature> --dry-run
./scripts/update.sh --instance <更新するsignature>
# ビルド済みなら --no-build を追加
```

更新には `python3`・`grim`・`timeout` が必要です。選択したセッションの全出力で
フレーム完了を確認しますが、画面画像は保存・出力しません。撮影許可が必要な場合は
通常の許可画面が出ます。許可失敗・消灯中・所有外のrequireなどでは解除前に停止し、
本体の再起動は行いません。手動編集済みrequireは自動更新せず、個別に確認してください。

設定を手動編集した管理ブロックは先に自分で取り除いてください。
Luaの設定変更だけなら通常の `hyprctl reload` で反映できます。

## 操作

| キー | 操作 |
| --- | --- |
| F6 | カーソル重力 → 最後のフォーカス天体 → 連星 |
| F7 | 表示中の形状でカーソル下を選び、固定中心へ吸い込む |
| F8 | 有限長の演出履歴を逆再生／通常再生 |
| F9 | カーソル位置で超新星のデモ |
| F10 | 各モニターの通常領域／別の仮想領域を鑑賞 |
| F11 | 手動プレビュー開始／終了 |
| F12 | 緊急解除 |

F7は初期設定で約2.8秒かけて、螺旋移動・加速する自転・縮小・ねじれを経て
点へ収束します。映像の中心で発動しても移動が見えるようにしています。
消えるのは演出内の映像だけで、アプリは終了しません。通常入力で元の表示へ戻ります。

常設の丸い穴へ自然に入った場合も、瞬間移動ではなく、螺旋状に移動・自転しながら
伸びて縮む吸い込みが再生されます。初期設定では入口が約1.68秒、出口からの出現が
約0.75秒です。転送先が別の仮想領域なら、右上の「OTHER UNIVERSE」に自動表示され、
F10を押さなくても行き先を確認できます。転送後も対象がいる間は小窓が残ります。
小窓内の映像に対するF7/F9も、その表示位置から選択されます。

これらのキーは有効な Cosmic の専用キーになります。`disable()` と
`shutdown()` ではキーの登録も外れます。既存の設定には F6〜F12 の登録が
ないことを確認しました。必要なら `controls` で変更・個別無効化できます。
宇宙内の操作は修飾なしのキーが適しています。修飾キー自体も通常入力として
即復帰するため、修飾つきの設定では最初の修飾キー押下で演出が終了します。

マウス移動は宇宙を終了せず、重力源を動かします。通常表示でのマウス移動は
アイドル時間を延長します。キー長押し・リピート・ボタン保持・ドラッグ中は
自動開始しません。ロック、画面消灯、復帰、ワークスペース／モニター構成の
変更では演出を解除します。
ロック画面、認証・モーダル画面、キーボード操作を要求するランチャーなどは
宇宙表示より優先し、Cosmicを停止／開始抑止します。安全上の警告、Hyprland自身の
通知やIME表示、重力操作用のカーソルは隠しません。IMEが入力grabを保持している間も
開始を抑止するため、一部IMEではその状態が長く続くことがあります。

## 設定

インストーラーの require 行を、例えば次のように編集できます。
設定の手動編集後は自動解除スクリプトがその編集済みブロックを保護するため、
解除時には編集した require ブロックを自分で削除してください。

```lua
require("cosmic").setup({
    idle_timeout = 20,
    max_windows = 24,
    seed = 0xC05C1C,
    effects = {
        cursor_gravity = true, orbit = true, binary = true,
        collisions = true, black_hole = true, spaghetti = true,
        wormholes = true, supernova = true, expansion = true, rewind = true,
    },
    controls = { emergency = "F12", supernova = false },
    exclusions = {
        fullscreen = true, idle_inhibit = true, screenshare = true,
        classes = { "^steam_app_", "^steam$", "^gamescope$", "^mpv$" },
    },
    rendering = { particles = 96, stars = 240, background = 1,
                  snapshot_mb = 128, hide_desktop_ui = true },
})
```

部分設定は現在の設定へマージします。設定値・未知のキー・重複操作キーを
検証します。初期設定は穏やかな軌道です。派手なプリセット:

```lua
local cosmic = require("cosmic")
cosmic.setup(cosmic.preset("demo"))
-- 初期設定へ戻す: cosmic.setup(cosmic.preset("calm"))
```

`rendering.hide_desktop_ui = false` を指定すると、Cosmic中も通常のバーなどを
表示できます。安全上の開始抑止は、この設定を無効にしても残ります。
保護用namespace・認証classの名前判定は補助的なもので、すべての独自UIを
自動分類する保証はありません。通常のxdgウィンドウで作られたウィジェットは
layer-shell UIとは異なり、ほかのアプリと同じく宇宙内の映像として扱います。

`physics` で引力・緩和距離・加速度／速度上限・減衰・反発・吸い込み時間・
ワームホールのクールダウンを変更できます。全項目と範囲は
[lua/cosmic/config.lua](lua/cosmic/config.lua) にあります。
`physics.sink_duration` は吸い込み時間です。極端に低い速度上限との組み合わせでは、
速度上限を守るため、この時間より長くなる場合があります。
履歴は `history_seconds`、`history_hz`、`history_mb` で制限します。
物理更新の `fps` は 10〜120、初期値 60 です。

`rendering.background` は宇宙背景の不透明度（0〜1）で、初期値1は壁紙を
完全に覆います。透かしたい場合だけ小さくしてください。`rendering.stars`
は星数の目安（0〜1024）です。0では星と流れ星を両方止めます。
字形はフォントに依存しない5×7ピクセルのASCIIで、モニター倍率に追従します。
配置は `seed` で再現できます。流れ星は開始18〜42秒後から、以降おおむね48〜96秒
間隔でまれに現れます。流れる方向は360度、大きさは約0.7〜1.8倍から毎回選び、
尾の向き・長さも追従します。約1.3〜2.2秒で消え、転送先の小窓では大きすぎないよう
調整します。転送先にも同じ系統の星空を描画します。

状態と操作をターミナルから確認:

```sh
hyprctl repl 'local s=require("cosmic").status(); for _,k in ipairs({"enabled","initialized","active","bodies","stored","history_frames","snapshot_bytes"}) do print(k.."="..tostring(s[k])) end'
hyprctl eval 'require("cosmic").disable()'
hyprctl eval 'require("cosmic").enable()'
hyprctl eval 'require("cosmic").shutdown()'
hyprctl eval 'require("cosmic").setup()'
```

`status()` は実行状態、対象数、収納数、履歴長、GPU 取り込み容量と、
表示中の仮想位置・角度・変形量を返します。

## 映像更新と上限

映像は **定期更新するスナップショット** です。ウィンドウ内の動きは描画と
同じフレームレートでは更新しません。`snapshot_hz=4` は対象全体で毎秒4回の
取り込みを順番に行う設定で、複数対象では一枚あたりの更新は遅くなります。
この版の本体が提供する、ポップアップと装飾も含む取り込み経路を採用しました。

初期 GPU 取り込み上限は 128 MiB、対象上限は24枚です。1920×1080 の対象一枚は
約7.9 MiB使うため、GPU上限が先に適用されることがあります。物理計算・粒子・
履歴にも上限があり、通常表示中は物理更新・取り込み・連続再描画が止まります。

吸い込みは仮想収納です。アプリは終了せず、通常入力ですべて戻ります。
逆再生は演出の位置・速度・変形・領域・収納のみを戻し、文字入力、ページ、
動画やアプリの処理は戻しません。終了済みのアプリは復活しません。
仮想領域は実ワークスペースと独立していて、F10 で転送先を見られます。

## 解除・復旧

```sh
./scripts/uninstall.sh
hyprctl reload
```

所有記録と一致するリンク・未編集の管理ブロックだけを削除します。
設定のバックアップ、リポジトリ、ビルド成果物、無関係な設定は残ります。
同じ導入先への再インストール・解除済みの再解除は安全に繰り返せます。
導入時に元からあったユーザー自身の require 行は削除しません。

`require("cosmic")` の行を削除して再読み込みした場合も、演出・入力監視・
タイマーを停止し、設定から消えたプラグインを本体が解除します。
プラグイン欠落・ABI不一致・初期化失敗では通常動作を保ち、対処方法を一度通知します。

復旧用には `hyprctl eval 'require("cosmic").disable()'`、または require 行を
削除して `hyprctl reload` を使います。Hyprland 更新後は、その版への対応を
確認してから対応ヘッダーでビルドし直してください。

## 検証と設計資料

```sh
./scripts/build.sh
ctest --test-dir build --output-on-failure
./scripts/fullscreen-test.sh
```

全画面の実検証にはGTK 3・gtk-layer-shell・libpng・grimの開発／実行環境も必要です。
このスクリプトは独立した子Hyprlandとheadless出力でバー・通知の実画像、
復帰入力、保護レイヤー、描画直後の解除・require削除、20秒の開始待機を確認し、
親セッションの設定や有効状態は変更しません。既存の宇宙現象・入力・共有回帰は
`scripts/nested-test.sh` で確認できます。

[実環境とAPI調査](docs/environment.md)、[設計](docs/architecture.md)、
[実際の検証結果](docs/validation.md) に、確認範囲と制約を記録します。
数値計算のテストは発散防止・履歴再生・個別現象を検証し、Lua のテストは
設定検証・初期化・解除・失敗処理を検証します。描画・入力の検証は本体の
ビルド成功と区別し、入れ子セッションと通常セッションの結果を別に記録します。
