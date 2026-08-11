#!/usr/bin/env bash
# run_ui_smoke.sh — Phase-2 UI + FF smoke (plan §8 / §4.4).
#
#   Phase A (ui_demo=1): boots Emerald, the frontend opens the in-game menu
#     at frame 300 and self-drives it: save state -> load state -> settings
#     (cycle scale fit/stretch/back, persisting config) -> wireless panel ->
#     resume.  GE dumps of the menu/settings/wireless screens land in log/.
#     Asserts the full EVT ladder + the .st0 savestate + config_saved.
#   Phase B (simff=300): a virtual FF hold (Square) engages for 300 frames
#     at the configured multiplier (default 2x): asserts ff_user on/off with
#     the frameskip engage path, and a clean run to exit.
#
# Exit 0 = pass. Prereqs: setup_ppsspp.sh ran once; psp/EBOOT.PBP built.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
SANDBOX_ROOT="${SANDBOX_ROOT:-$HOME/gpsp-e2e/sandboxes}"
TIMEOUT_S="${TIMEOUT_S:-300}"

EBOOT="$REPO/psp/EBOOT.PBP"
ROM="$REPO/testdata/Pokemon - Emerald Version (USA, Europe).gba"
BIOS="$REPO/testdata/gba_bios.bin"

ART="$SCRIPT_DIR/artifacts/uismoke-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"

fail() { echo "FAIL: $*" | tee -a "$ART/verdict.txt"; exit 1; }

[ -x "$PPSSPP_DIR/build/PPSSPPSDL" ] || fail "PPSSPPSDL not built"
[ -f "$EBOOT" ] || fail "psp/EBOOT.PBP missing"
[ -f "$ROM" ]   || fail "test ROM missing"
[ -f "$BIOS" ]  || fail "gba_bios.bin missing"

MS="$SANDBOX_ROOT/inst1/ppsspp"
GAME="$MS/PSP/GAME/gpsp-adhoc"

# softgpu so the GE drawbuffer readbacks (menu screenshots) are real bytes
SOFTGPU_INI="$ART/softgpu.ini"
printf '[Graphics]\nSoftwareRenderer = True\n' > "$SOFTGPU_INI"

run_instance() {  # $1 = phase tag
  rm -f /dev/shm/PPSSPP_ID
  local EVTLOG="$GAME/log/frontend.log"
  ( cd "$PPSSPP_DIR/build" && \
    XDG_CONFIG_HOME="$SANDBOX_ROOT/inst1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    timeout $((TIMEOUT_S + 30)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
      ./PPSSPPSDL --windowed --appendconfig="$SOFTGPU_INI" "$GAME/EBOOT.PBP" \
      >"$ART/emu-$1.log" 2>&1 ) &
  local EMU_PID=$!
  local DONE=0
  for _ in $(seq 1 "$TIMEOUT_S"); do
    if grep -q "^EVT exit code=0" "$EVTLOG" 2>/dev/null; then DONE=1; break; fi
    kill -0 "$EMU_PID" 2>/dev/null || break
    sleep 1
  done
  sleep 1
  kill "$EMU_PID" 2>/dev/null || true
  wait "$EMU_PID" 2>/dev/null || true
  cp "$EVTLOG" "$ART/frontend-$1.log" 2>/dev/null || true
  [ "$DONE" -eq 1 ] || fail "phase $1: timed out waiting for EVT exit code=0"
}

assert_evt() {  # $1 = log, $2.. = patterns
  local log="$1"; shift
  local pat missing=0
  for pat in "$@"; do
    if grep -q "$pat" "$log"; then
      echo "OK   $pat" >> "$ART/verdict.txt"
    else
      echo "MISS $pat" >> "$ART/verdict.txt"
      missing=1
    fi
  done
  return $missing
}

setup_sandbox() {
  "$SCRIPT_DIR/setup_ppsspp.sh" --instances 1 --skip-build >/dev/null \
    || fail "sandbox creation failed"
  mkdir -p "$GAME/roms" "$GAME/log"
  cp "$EBOOT" "$GAME/EBOOT.PBP"
  cp "$BIOS"  "$GAME/gba_bios.bin"
  cp "$ROM"   "$GAME/roms/emerald.gba"
}

# --- Phase A: ui_demo --------------------------------------------------------
setup_sandbox
cat > "$GAME/.gpsp-harness.ini" <<EOF
ui_demo = 1
autoexit_frames = 1500
EOF
run_instance uidemo

assert_evt "$ART/frontend-uidemo.log" \
  "^EVT ui_open" \
  "^EVT ui_demo_start" \
  "^EVT state_save file=.*\.st0 size=425984" \
  "^EVT state_load file=.*\.st0 size=425984" \
  "^EVT ui_screen name=settings" \
  "^EVT video_mode scale=fit filter=nearest" \
  "^EVT video_mode scale=stretch filter=nearest" \
  "^EVT video_mode scale=1x filter=nearest" \
  "^EVT config_saved scale=0" \
  "^EVT ui_screen name=wireless" \
  "^EVT ui_demo_done" \
  "^EVT ui_close" \
  "^EVT exit code=0" || fail "phase A EVT ladder incomplete (see verdict)"

ls "$GAME"/roms/*.st0 >/dev/null 2>&1 || fail "savestate .st0 missing"
[ -f "$GAME/log/ge_ui.bmp" ]    || fail "menu GE dump ge_ui.bmp missing"
[ -f "$GAME/config.ini" ]       || fail "config.ini not persisted"
cp "$GAME/log/ge_ui.bmp" "$GAME/config.ini" "$ART/" 2>/dev/null || true

# --- Phase B: simulated FF hold ---------------------------------------------
setup_sandbox
cat > "$GAME/.gpsp-harness.ini" <<EOF
simff = 300
autoexit_frames = 900
EOF
run_instance simff

assert_evt "$ART/frontend-simff.log" \
  "^EVT ff_user on mult_x10=20" \
  "^EVT ff_user off" \
  "^EVT heartbeat frames=600" \
  "^EVT exit code=0" || fail "phase B EVT ladder incomplete (see verdict)"

# --- Phase C: variant autoboot + private save namespace (VARIANTS.md) --------
"$SCRIPT_DIR/setup_ppsspp.sh" --instances 1 --skip-build >/dev/null \
  || fail "sandbox recreation failed"
mkdir -p "$GAME/log"
cp "$EBOOT" "$GAME/EBOOT.PBP"
cp "$BIOS"  "$GAME/gba_bios.bin"
cp "$ROM"   "$GAME/emerald.gba"        # ROM next to the EBOOT, not roms/
cat > "$GAME/variant.ini" <<EOF
rom = emerald.gba
silent_wireless = 1
role = auto
group = GPSP42
EOF
cat > "$GAME/.gpsp-harness.ini" <<EOF
autoexit_frames = 700
EOF
run_instance variant

assert_evt "$ART/frontend-variant.log" \
  "^EVT variant rom=emerald.gba silent=1 group=GPSP42" \
  "^EVT rom_loaded code=BPEE" \
  "^EVT exit code=0" || fail "phase C EVT ladder incomplete (see verdict)"
grep -q "^LOG rom_path=.*emerald.gba save_path=.*saves/emerald.sav" \
  "$ART/frontend-variant.log" || fail "variant save namespace wrong"
[ -d "$GAME/saves" ] || fail "variant saves/ dir not created"

cat "$ART/verdict.txt"
echo "UI SMOKE PASS — artifacts in $ART"
exit 0
