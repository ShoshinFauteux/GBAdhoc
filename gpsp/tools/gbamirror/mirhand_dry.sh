#!/usr/bin/env bash
# mirhand_dry.sh STAGE LOGS [XDISPLAY] -- the takeover of a PARKED rig console
# against a PPSSPP memory stick (WSL, ~/ppsspp), as freezerig_dry.sh does it:
#   1. a DRPROF rig (dynarec-profile-out's base) runs a short job and PARKS in
#      its handoff in GBADHOC-DRPROF -- the PSP-3000's state today;
#   2. mirhand backup + install INTO that folder while it is parked, inside a
#      window seen open;
#   3. mirhand run: CMD=RUN relaunches the parked app into the mirror EBOOT,
#      every arm of STAGE's plan, then STOP (parks again).
# Proves the mechanics only (no USB enumeration in PPSSPP).
set -u
STAGE=$1; LOGS=$2; XD=${3:-96}
HERE="$(cd "$(dirname "$0")" && pwd)"
MH="$HERE/mirhand.py"
source "$HERE/../workdirs.sh"
DRPROF_BASE=${DRPROF_BASE:-$GBADHOC_WORK/dynarec-profile-out/rig-stage/base}
SB=$HOME/gpsp-e2e/mirhand
G="$SB/ppsspp/PSP/GAME/GBADHOC-DRPROF"
W="--window-s 20 --need-s 5 --poll 0.5"
rm -rf "$SB" "$LOGS"; mkdir -p "$SB/ppsspp/PSP/SYSTEM" "$G" "$LOGS"
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
NickName = MIRHAND
EOI
cp -r "$DRPROF_BASE/." "$G/"
cat > "$G/.gpsp-harness.ini" <<EOI
rom = aw2.gba
script = aw2.txt
load_state = 1
autoexit_frames = 300
heartbeat_s = 5
handoff = 1
handoff_window_s = 20
handoff_park_s = 0
handoff_max_runs = 100000
handoff_total_s = 900
run_id = drprof-parked
arm = A-BASE
EOI
say() { echo "[dry $(date +%H:%M:%S)] $*" | tee -a "$LOGS/dry.log"; }
say "1. DRPROF rig launched; waiting for it to park"
( cd "$SB" && XDG_CONFIG_HOME="$SB" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  xvfb-run -n "$XD" -s "-screen 0 960x544x24" ~/ppsspp/build/PPSSPPSDL --windowed \
  "$G/EBOOT.PBP" > "$LOGS/emu.log" 2>&1 ) &
EP=$!
for i in $(seq 1 300); do [ -f "$G/handoff/RESULT.TXT" ] && break; sleep 1; done
[ -f "$G/handoff/RESULT.TXT" ] || { say "FAIL: DRPROF never parked"; exit 1; }
say "   parked: $(tr '\n' ' ' < "$G/handoff/RESULT.TXT")"
say "2. backup + install while parked"
python3 "$MH" backup --drive "$SB/ppsspp" --out "$LOGS/backup" 2>&1 | tail -2 | tee -a "$LOGS/dry.log"
python3 "$MH" install --stage "$STAGE" --drive "$SB/ppsspp" --backup "$LOGS/backup" $W \
  --window-wait-s 600 2>&1 | tee -a "$LOGS/dry.log"
[ "${PIPESTATUS[0]}" = 0 ] || { say "FAIL: install"; exit 1; }
say "3. run loop"
python3 "$MH" run --stage "$STAGE" --drive "$SB/ppsspp" --logs "$LOGS/rig" $W \
  --dry-windows --timeout 3600 > "$LOGS/loop.log" 2>&1
say "   loop rc=$?"
sleep 3
pkill -9 -f "PPSSPPSDL --windowed $G" 2>/dev/null; kill $EP 2>/dev/null; sleep 1
pkill -9 -f "Xvfb :$XD " 2>/dev/null
cp -r "$G/handoff" "$LOGS/final-handoff" 2>/dev/null
python3 "$MH" report --logs "$LOGS/rig" --out "$LOGS/layout" | tee -a "$LOGS/dry.log"
