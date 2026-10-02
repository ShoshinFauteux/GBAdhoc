#!/usr/bin/env bash
# run.sh <tag> <eboot dir> <setup snippet> [timeout_s]
# One isolated PPSSPP run of a GBAdhoc harness EBOOT.  The setup snippet is
# sourced with $GAME (the app dir on the emulated stick), $LIB (the owner's
# ROM/art library copy), $FIX (the AW2/H&S fixture) set; it copies ROMs/art
# and writes .gpsp-harness.ini and config.ini.  Everything in log/ lands in
# runs/<tag>/.  XDISPLAY=:N pins the run's Xvfb to display N instead of
# `xvfb-run -a`'s pick.  The server, its lock file and its socket stay inside
# the run's namespace (private /tmp, private network), so concurrent runs on
# the same N do not collide with each other or with anyone else's display.
set -uo pipefail
TAG="$1"; ED="$2"; SETUP="$3"; TO="${4:-600}"
D=${DISPLAY_RIG_DIR:?set DISPLAY_RIG_DIR (holds b-harness/, base-harness/, readme30 lib at ../readme30/lib)}
LIB=$D/../readme30/lib
FIX=/mnt/c/Users/DrSto/OneDrive/Desktop/GBAdhoc/builds/dynarec-profile-out/rig-stage-hnsdiag/base
BIOS=$HOME/wireless-rig/testdata/gba_bios.bin
OUT=$D/runs/$TAG
SB=$HOME/gpsp-e2e/sandboxes/disp-$TAG
MS=$SB/ppsspp
GAME=$MS/PSP/GAME/gpsp-adhoc
rm -rf "$SB" "$OUT"
mkdir -p "$MS/PSP/SYSTEM" "$GAME/log" "$GAME/roms" "$GAME/hero" "$GAME/boxart" "$OUT"
cat > "$MS/PSP/SYSTEM/ppsspp.ini" <<EOF
[General]
FirstRun = False
CheckForNewVersion = False
EnableCheats = False

[Graphics]
GraphicsBackend = 3 (VULKAN)

[Network]
EnableWlan = False

[SystemParam]
NickName = DISPLAY
MacAddress = 02:00:00:00:00:41
EOF
cp "$ED/EBOOT.PBP" "$GAME/EBOOT.PBP"
cp "$ED/gbadhoc_me.prx" "$GAME/" 2>/dev/null
cp "$BIOS" "$GAME/gba_bios.bin"
export GAME LIB FIX
# shellcheck disable=SC1090
source "$SETUP"
SOFT="$SB/softgpu.ini"
printf '[Graphics]\nSoftwareRenderer = True\n' > "$SOFT"
EVT="$GAME/log/frontend.log"
( cd "$HOME/ppsspp/build" && \
  XDG_CONFIG_HOME="$SB" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  SOFT="$SOFT" GAME="$GAME" TO="$TO" \
  XDISPLAY="${XDISPLAY:-}" \
  unshare -rmnpf --mount-proc --propagation private bash -c 'mount -t tmpfs tmpfs /dev/shm && ip link set lo up && if [ -n "$XDISPLAY" ]; then mount -t tmpfs tmpfs /tmp && exec timeout $TO xvfb-run -n "${XDISPLAY#:}" -s "-screen 0 1280x720x24 +extension GLX +render -noreset" ./PPSSPPSDL --windowed --appendconfig="$SOFT" "$GAME/EBOOT.PBP"; else exec timeout $TO xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" ./PPSSPPSDL --windowed --appendconfig="$SOFT" "$GAME/EBOOT.PBP"; fi' > "$OUT/emu.log" 2>&1 ) &
PID=$!
for _ in $(seq 1 $((TO - 10))); do
  if grep -q "^EVT exit" "$EVT" 2>/dev/null; then break; fi
  kill -0 "$PID" 2>/dev/null || break
  sleep 1
done
sleep 2
pkill -TERM -f "sandboxes/disp-$TAG/" 2>/dev/null || true
sleep 1; pkill -KILL -f "sandboxes/disp-$TAG/" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
cp "$GAME"/log/* "$OUT/" 2>/dev/null
cp "$GAME/config.ini" "$OUT/config.after.ini" 2>/dev/null
echo "$TAG: exit=$(grep -m1 '^EVT exit' "$OUT/frontend.log" 2>/dev/null) bmps=$(ls "$OUT"/*.bmp 2>/dev/null | wc -l)"
