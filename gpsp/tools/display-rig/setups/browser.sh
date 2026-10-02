# A small library for the browser: Emerald (GBA, state), Kirby (GB),
# Crystal (GBC, state), each with its hero art and box art; AW2 with none.
# env: BSCRIPT (browser script body, \n-escaped) CFG FRAMES LDUMP GEAT
R=$LIB/..
cp "$R/roms/emerald.gba" "$GAME/roms/Pokemon - Emerald Version (USA, Europe).gba"
cp "$R/roms/kirby.gb"    "$GAME/roms/Kirby's Dream Land (USA, Europe).gb"
cp "$R/roms/crystal.gbc" "$GAME/roms/Pokemon - Crystal Version (USA, Europe) (Rev 1).gbc"
cp "$FIX/roms/aw2.gba"   "$GAME/roms/aw2.gba"
for n in "Pokemon - Emerald Version (USA, Europe)" "Kirby's Dream Land (USA, Europe)" \
         "Pokemon - Crystal Version (USA, Europe) (Rev 1)"; do
  cp "$LIB/hero/$n.565" "$GAME/hero/" 2>/dev/null
  cp "$LIB/boxart/$n".* "$GAME/boxart/" 2>/dev/null
done
printf '%b\n' "$BSCRIPT" > "$GAME/shoot.txt"
{
  echo "browser = 1"
  echo "ui_browser_script = shoot.txt"
  echo "me_mode = 1"
  echo "heartbeat_s = 5"
  echo "autoexit_frames = ${FRAMES:-300}"
  echo "gedump_at = ${GEAT:-0}"
  echo "loading_dump = ${LDUMP:-0}"
  [ -n "${EXTRA_INI:-}" ] && printf '%b\n' "$EXTRA_INI"
} > "$GAME/.gpsp-harness.ini"
printf '%b\n' "${CFG:-}" > "$GAME/config.ini"
