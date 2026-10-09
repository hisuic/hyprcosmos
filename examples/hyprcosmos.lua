-- Hyprcosmos のユーザー専用設定です。このファイルは更新・削除時にも保持されます。
-- 編集後は、使用中の Hyprland セッションで設定をリロードしてください。
-- idle_timeout は秒単位です。通常のキー入力やクリックで元のデスクトップに戻ります。
-- preset = "demo" や effects = { wormholes = false } なども必要に応じて追加できます。
return {
    idle_timeout = 60,
    max_objects = 64,             -- 通常ウィンドウ＋欠片の合計上限（整数 16〜128）
    rendering = {
        hide_desktop_ui = true,
        stars = 240,
    },
    -- F5: Cosmic 中のカーソル下のウィンドウを選択して超新星爆発。
    -- 通常時の F5 はアプリに届きます。F9 の即時衝撃波はそのままです。
    stellar = {
        automatic = true,        -- false で自然発生のみ無効化（F5 は使用可能）
        interval_min = 70,        -- Cosmic 開始後・次の爆発までの待ち時間（秒）
        interval_max = 130,
        charge_seconds = 3.5,     -- 赤く膨張する予兆の時間
        fragment_seconds = 3,    -- 爆発直後の飛散・縮小が落ち着く時間。欠片の寿命ではない
        fragments = 16,          -- 4 または 16。画像は 1 枚を共有して分割表示
        growth = 1.7,            -- 爆発直前の表示倍率（実ウィンドウは変更しない）
    },
    -- 欠片は再爆発せず、通常ウィンドウと同じ重力・衝突・端での反射などに従います。
    -- 上限を超える爆発は開始せず、既存の欠片を消すこともありません。
}
