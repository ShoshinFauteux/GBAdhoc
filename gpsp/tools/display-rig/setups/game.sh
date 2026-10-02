# env: ROMSRC ROMNAME [STATE] [HERO] [BOXART] [SCRIPT] [LOADST] [FRAMES] [GEAT]
#      [GEEVERY] [LDUMP] [SHASH] [UIDEMO] [CFG (config.ini body, \n-escaped)]
#      [EXTRA_INI]
cp "$ROMSRC" "$GAME/roms/$ROMNAME"
if [ -n "${STATE:-}" ]; then
  case "$ROMNAME" in
    *.gba) cp "$STATE" "$GAME/roms/${ROMNAME%.gba}.st0" ;;
    *)     cp "$STATE" "$GAME/roms/$ROMNAME.st0" ;;
  esac
fi
[ -n "${SAV:-}" ] && cp "$SAV" "$GAME/roms/${ROMNAME%.*}.sav"
[ -n "${HERO:-}" ] && cp "$HERO" "$GAME/hero/${ROMNAME%.*}.565"
[ -n "${BOXART:-}" ] && cp "$BOXART" "$GAME/boxart/${ROMNAME%.*}.png"
[ -n "${SCRIPT:-}" ] && cp "$SCRIPT" "$GAME/inputs.txt"
{
  echo "rom = $ROMNAME"
  [ -n "${SCRIPT:-}" ] && echo "script = inputs.txt"
  echo "load_state = ${LOADST:-0}"
  echo "shash = ${SHASH:-0}"
  echo "me_mode = 1"
  echo "heartbeat_s = 5"
  echo "autoexit_frames = ${FRAMES:-600}"
  echo "gedump_at = ${GEAT:-0}"
  echo "gedump_every = ${GEEVERY:-0}"
  echo "loading_dump = ${LDUMP:-0}"
  echo "ui_demo = ${UIDEMO:-0}"
  [ -n "${EXTRA_INI:-}" ] && printf '%b\n' "$EXTRA_INI"
} > "$GAME/.gpsp-harness.ini"
printf '%b\n' "${CFG:-}" > "$GAME/config.ini"
