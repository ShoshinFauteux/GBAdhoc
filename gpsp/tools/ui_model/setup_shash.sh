# setup snippet for run_ppsspp.sh: a frame-hash (shash) identity run on a
# standard fixture -- the same staging as all-out/smoke/tools/hrun.sh.
# env: ROMF STATEF SAVF SCRIPTF AUTOEXIT [EXTRA_INI] [CFG]
RB=$(basename "$ROMF"); STEM="${RB%.*}"
cp "$ROMF" "$GAME/roms/$RB"
[ -n "${STATEF:-}" ] && cp "$STATEF" "$GAME/roms/$STEM.st0"
[ -n "${SAVF:-}" ] && cp "$SAVF" "$GAME/roms/$STEM.sav"
[ -n "${SCRIPTF:-}" ] && cp "$SCRIPTF" "$GAME/script.txt"
{
  echo "rom = $RB"
  [ -n "${STATEF:-}" ] && echo "load_state = 1"
  [ -n "${SCRIPTF:-}" ] && echo "script = script.txt"
  echo "shash = 1"
  echo "heartbeat_s = 5"
  echo "autoexit_frames = ${AUTOEXIT:-3000}"
  echo "audio_oracle = 1"
  echo "me_mode = 1"
  echo "log_input = 0"
  [ -n "${EXTRA_INI:-}" ] && printf '%b\n' "$EXTRA_INI"
} > "$GAME/.gpsp-harness.ini"
[ -n "${CFG:-}" ] && printf '%b\n' "$CFG" > "$GAME/config.ini"
true
