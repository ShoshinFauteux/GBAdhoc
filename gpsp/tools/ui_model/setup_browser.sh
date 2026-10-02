# setup snippet for run_ppsspp.sh: START settings from the ROM browser
# (ui_controls_demo = 2), over the browser's own page.
# env: THEME (0/1), SHELL (0 shelf, 1 marquee), FRAMES
cp "$FX/emerald.gba" "$GAME/roms/Pokemon - Emerald Version.gba"
cp "$FX/hns.gba" "$GAME/roms/Pokemon - Heart and Soul.gba" 2>/dev/null
cp "$FX/aw2.gba" "$GAME/roms/Advance Wars 2.gba" 2>/dev/null
{
  echo "browser = 1"
  echo "heartbeat_s = 5"
  echo "autoexit_frames = ${FRAMES:-200}"
  echo "ui_controls_demo = 2"
} > "$GAME/.gpsp-harness.ini"
printf 'group = GPSP07\ntheme = %s\nui_shell = %s\nconsole = 0\n' "${THEME:-0}" "${SHELL_UI:-0}" > "$GAME/config.ini"
