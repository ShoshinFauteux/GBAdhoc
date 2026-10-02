# setup snippet for run_ppsspp.sh: the design-comparison stage.
# env: UIFX (make_fixtures.py output), DEMO (ui_demo | ui_controls_demo),
#      THEME (0 dark, 1 light), INJECT (1 = the mockups' backdrop, default),
#      FRAMES (autoexit), EXTRA_INI (\n-escaped extra harness lines)
ROM="Pokemon - Emerald Version.gba"
STEM="${ROM%.gba}"
cp "$FX/emerald.gba" "$GAME/roms/$ROM"
# The mockups' slots: 1, 2, 3 and 5 hold a state with a preview, 4 is empty.
# The state bodies are placeholders: the demo saves slot 2 before it loads it.
for k in 1 2 3 5; do
  ext=st$((k - 1))
  head -c 4096 /dev/zero > "$GAME/roms/$STEM.$ext"
  cp "$UIFX/slot$k.thumb" "$GAME/roms/$STEM.$ext.thumb"
done
{
  echo "rom = $ROM"
  echo "heartbeat_s = 5"
  echo "autoexit_frames = ${FRAMES:-700}"
  echo "${DEMO:-ui_demo} = 1"
  [ "${INJECT:-1}" = 1 ] && { cp "$UIFX/ui_backdrop.565" "$GAME/"; echo "ui_backdrop = ui_backdrop.565"; }
  [ -n "${EXTRA_INI:-}" ] && printf '%b\n' "$EXTRA_INI"
} > "$GAME/.gpsp-harness.ini"
printf 'group = GPSP07\ntheme = %s\nscale_gba = 1\n%b\n' "${THEME:-0}" "${CFG:-}" > "$GAME/config.ini"
