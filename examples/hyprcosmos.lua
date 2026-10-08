-- Hyprcosmos のユーザー専用設定です。このファイルは更新・削除時にも保持されます。
-- 編集後は、使用中の Hyprland セッションで設定をリロードしてください。
-- idle_timeout は秒単位です。通常のキー入力やクリックで元のデスクトップに戻ります。
-- preset = "demo" や effects = { wormholes = false } なども必要に応じて追加できます。
return {
    idle_timeout = 60,
    rendering = {
        hide_desktop_ui = true,
        stars = 240,
    },
}
