#!/usr/bin/env bash
# drrig_dry.sh NAME STAGE LOGS [XDISPLAY] -- the WHOLE drrig loop against a
# PPSSPP memory stick: install, ONE launch, then the console's USB handoff,
# the PC's collection + golden restore + EBOOT/ini staging, and the relaunch
# into the next arm, for every cycle in the plan.  (tools/rig/resrig's dry.sh
# recipe.)  Runs in WSL; PPSSPP at ~/ppsspp.
#
# PPSSPP does not enumerate USB, so this proves the loop's mechanics, not the
# physical export -- which on a PSP Go also means the rig must be on the M2
# Memory Stick (drrig.py docstring).
set -u
NAME=$1; STAGE=$2; LOGS=$3; XD=${4:-91}
HERE="$(cd "$(dirname "$0")" && pwd)"
SB=$HOME/gpsp-e2e/drrig/$NAME
rm -rf "$SB"; mkdir -p "$SB/ppsspp/PSP/SYSTEM" "$SB/ppsspp/PSP/GAME"
cat > "$SB/ppsspp/PSP/SYSTEM/ppsspp.ini" <<EOI
[General]
FirstRun = False
CheckForNewVersion = False
[Graphics]
GraphicsBackend = 3 (VULKAN)
SoftwareRenderer = True
[Network]
EnableWlan = False
EnableAdhocServer = False
[SystemParam]
NickName = DRRIG
EOI
python3 "$HERE/drrig.py" install --stage "$STAGE" --drive "$SB/ppsspp" || exit 1
G="$SB/ppsspp/PSP/GAME/GBADHOC-DRPROF"
rm -rf "$LOGS"; mkdir -p "$LOGS"
python3 "$HERE/drrig.py" run --stage "$STAGE" --drive "$SB/ppsspp" --logs "$LOGS" \
  --freeze-s "${FREEZE_S:-900}" --timeout "${TMO:-10800}" --poll 1 > "$LOGS/loop.log" 2>&1 &
LP=$!
cd "$SB"
XDG_CONFIG_HOME="$SB" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  xvfb-run -n "$XD" -s "-screen 0 960x544x24" ~/ppsspp/build/PPSSPPSDL --windowed \
  "$G/EBOOT.PBP" > "$SB/emu.log" 2>&1 &
EP=$!
wait $LP
echo "loop rc=$?"
sleep 5
pkill -9 -f "PPSSPPSDL --windowed $G" 2>/dev/null; kill $EP 2>/dev/null; sleep 1
pkill -9 -f "Xvfb :$XD " 2>/dev/null
cp "$SB/emu.log" "$LOGS/" 2>/dev/null
cp -r "$G/handoff" "$LOGS/final-handoff" 2>/dev/null
