#!/usr/bin/env bash
# ppsspp_run.sh TAG EBOOT_DIR ROM_FILE STATE_FILE SAV_FILE SCRIPT OUTROOT [INI_EXTRA_FILE]
# One headless PPSSPP run of a harness EBOOT (WSL, ~/ppsspp build), in a
# private namespace so other agents' rigs cannot collide (readme30 recipe).
# Stages roms/<rom>, its .st0/.sav, the autopilot script and a harness ini
# with shash=1, load_state=1; copies log/ back to OUTROOT/TAG.
set -uo pipefail
TAG=$1; EB=$2; ROM=$3; ST=$4; SAV=$5; SC=$6; OUTROOT=$7; EXTRA=${8:-}
BIOS=${BIOS:-$HOME/gbalink/bios/gba_bios.bin}
PPSSPP_DIR=${PPSSPP_DIR:-$HOME/ppsspp}
TIMEOUT=${TIMEOUT:-1500}
OUT="$OUTROOT/$TAG"
SB="$HOME/gpsp-e2e/sandboxes/gbalink-$TAG"
MS="$SB/ppsspp"; GAME="$MS/PSP/GAME/gpsp-adhoc"
RB=$(basename "$ROM"); STEM="${RB%.*}"
rm -rf "$SB" "$OUT"; mkdir -p "$MS/PSP/SYSTEM" "$GAME/log" "$GAME/roms" "$OUT"
cat > "$MS/PSP/SYSTEM/ppsspp.ini" <<EOI
[General]
FirstRun = False
CheckForNewVersion = False
[Graphics]
GraphicsBackend = 3 (VULKAN)
[Network]
EnableWlan = False
[SystemParam]
NickName = GBALINK
EOI
cp "$EB/EBOOT.PBP" "$GAME/"; cp "$EB/gbadhoc_me.prx" "$GAME/" 2>/dev/null || true
cp "$BIOS" "$GAME/gba_bios.bin"
cp "$ROM" "$GAME/roms/$RB"; cp "$ST" "$GAME/roms/$STEM.st0"; cp "$SAV" "$GAME/roms/$STEM.sav"
cp "$SC" "$GAME/script.txt"
{ echo "rom = $RB"; echo "load_state = 1"; echo "script = script.txt"
  echo "shash = 1"; echo "heartbeat_s = 5"; echo "autoexit_frames = ${AUTOEXIT:-30000}"
  [ -n "$EXTRA" ] && cat "$EXTRA"; } > "$GAME/.gpsp-harness.ini"
cp "$GAME/.gpsp-harness.ini" "$OUT/harness.ini"
SOFT="$SB/softgpu.ini"; printf '[Graphics]\nSoftwareRenderer = True\n' > "$SOFT"
EVT="$GAME/log/frontend.log"
( cd "$PPSSPP_DIR/build" && XDG_CONFIG_HOME="$SB" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  SOFT="$SOFT" GAME="$GAME" TO="$TIMEOUT" \
  unshare -rmnpf --mount-proc --propagation private bash -c 'mount -t tmpfs tmpfs /dev/shm && ip link set lo up && exec timeout $TO xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" ./PPSSPPSDL --windowed --appendconfig="$SOFT" "$GAME/EBOOT.PBP"' > "$OUT/emu.log" 2>&1 ) &
PID=$!
for _ in $(seq 1 "$TIMEOUT"); do
  grep -q "^EVT exit" "$EVT" 2>/dev/null && break
  kill -0 "$PID" 2>/dev/null || break
  sleep 1
done
sleep 2
P1="sandboxes/gbalink-"; pkill -TERM -f "$P1$TAG/" 2>/dev/null; sleep 1; pkill -KILL -f "$P1$TAG/" 2>/dev/null
wait "$PID" 2>/dev/null
cp -r "$GAME/log/." "$OUT/" 2>/dev/null
echo "$TAG: $(grep -m1 '^EVT exit' "$OUT/frontend.log" 2>/dev/null) shash=$(grep -c '^f=' "$OUT/shash.txt" 2>/dev/null)"
