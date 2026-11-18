#!/usr/bin/env bash
# pp.sh TAG ARM GAME SCRIPT FRAMES [ini lines...]   (FF=1 TIMEOUT=secs in env)
#   ARM  = dir under $OUTW (a local output folder, not in the repo) holding EBOOT.PBP
#   GAME = rom name in ~/rewind-agent/work/roms (without .gba)
# One PPSSPP under its own Xvfb and its own sandbox; killed by PID only.
set -uo pipefail
TAG=$1; ARM=$2; GAME=$3; SCRIPT=$4; FRAMES=$5; shift 5
W=$HOME/rewind-agent
OUTW=/mnt/c/Users/DrSto/OneDrive/Desktop/GBAdhoc/builds/rewind-out/psp
SB=$W/sandboxes/$TAG
RES=$W/pp2/$TAG
TIMEOUT=${TIMEOUT:-900}
rm -rf "$SB" "$RES"; mkdir -p "$RES"
MS=$SB/ppsspp
G=$MS/PSP/GAME/gpsp-adhoc
mkdir -p "$MS/PSP/SYSTEM" "$G/log" "$G/roms"
cat > "$MS/PSP/SYSTEM/ppsspp.ini" <<EOF
[General]
FirstRun = False
CheckForNewVersion = False

[Graphics]
GraphicsBackend = 3 (VULKAN)

[Network]
EnableWlan = False

[SystemParam]
NickName = RW
MacAddress = 02:00:00:00:00:01
EOF
cp "$OUTW/$ARM/EBOOT.PBP" "$G/EBOOT.PBP"
cp "$OUTW/$ARM/gbadhoc_me.prx" "$G/" 2>/dev/null || true
cp "$W/bios/gba_bios.bin" "$G/gba_bios.bin"
cp "$W/work/roms/$GAME.gba" "$G/roms/$GAME.gba"
cp "$SCRIPT" "$G/script.txt"
# SAV=file in the environment: the cartridge save the game starts with
[ -n "${SAV:-}" ] && cp "$SAV" "$G/roms/$GAME.sav"
{
  echo "rom = $GAME.gba"
  echo "script = script.txt"
  echo "heartbeat_s = 5"
  echo "autoexit_frames = $FRAMES"
  echo "host = 0"
  echo "join = 0"
  [ "${FF:-0}" = 1 ] && echo "ff = 1"
  for l in "$@"; do echo "$l"; done
} > "$G/.gpsp-harness.ini"
cp "$G/.gpsp-harness.ini" "$RES/harness.ini"
( cd "$HOME/ppsspp/build" && \
  XDG_CONFIG_HOME="$SB" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  exec timeout $((TIMEOUT + 30)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
    ./PPSSPPSDL --windowed ${SOFTGPU:+--appendconfig=$W/softgpu.ini} "$G/EBOOT.PBP" >"$RES/emu.log" 2>&1 ) &
EMU=$!
DONE=0
for _ in $(seq 1 "$TIMEOUT"); do
  if grep -q "^EVT exit code=" "$G/log/frontend.log" 2>/dev/null; then DONE=1; break; fi
  kill -0 "$EMU" 2>/dev/null || break
  sleep 1
done
sleep 1
kill "$EMU" 2>/dev/null || true
wait "$EMU" 2>/dev/null || true
cp "$G"/log/frontend.log "$G"/log/rewind_hash.txt "$G"/log/heartbeat.txt "$G"/log/state_*.bin "$G"/log/ge_*.bmp "$RES/" 2>/dev/null
cp "$G/roms/$GAME.sav" "$RES/" 2>/dev/null
cp "$G/roms/$GAME.sav".rw-* "$RES/" 2>/dev/null
rm -rf "$SB"
echo "$TAG done=$DONE $(grep -c . "$RES/frontend.log" 2>/dev/null) lines; $(grep "^EVT exit" "$RES/frontend.log" 2>/dev/null)"
