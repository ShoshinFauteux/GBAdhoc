#!/usr/bin/env bash
# run_gu_color_test.sh — GU blit color-order regression check (hw finding a).
#
# The 2026-08-01 PSP-1000 baseline showed the GU blit rendering R/B-swapped
# colors on the real GE (libretro RGB565 uploaded as GU_PSM_5650, which is
# PSP channel order, R in bits 0-4).  PPSSPP's texture-decode/framebuffer-
# encode round trip made its WINDOW look correct, and the core-buffer BMP
# dumps bypass the GU path entirely — so nothing on the old harness could
# see the bug.  This test reads back what the GE actually rendered:
#
#   Phase A (testpat): EBOOT renders 8 known RGB565 color bars through the
#     production blit path, dumps the GE DRAWBUFFER (raw VRAM decoded with
#     the real 5650 layout), and check_ge_colors.py asserts every bar.
#   Phase B (in-game): boots Emerald, dumps core buffer + GE drawbuffer at
#     frame 600, asserts the GE region is pixel-identical to the core BMP.
#
# Runs PPSSPP with SoftwareRenderer=True (via --appendconfig; the
# --graphics=software CLI flag forces a GL window that dies under Xvfb):
# softgpu renders into emulated VRAM, so the readback bytes are exactly
# what the GE wrote (and softgpu is the most hardware-faithful renderer
# PPSSPP has).  The presentation backend stays the sandbox default
# (Vulkan/lavapipe, the verified Xvfb recipe).
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

# ADR-0034: the same pixel-exact check is the correctness gate for the blit
# staging placement A/B (`config.ini blit_mode`).  The rig cannot price the
# three modes -- PPSSPP models neither Allegrex's caches nor uncached write
# cost -- but softgpu renders into emulated VRAM, so it CAN prove that
# writing through the uncached mirror or into VRAM produces byte-identical
# output.  Run it once per mode before trusting any of them on hardware.
#
# ADR-0040 adds --gu-defer=1, which is a CORRECTNESS gate rather than a
# performance one: with the sync deferred, a display list is still running
# while the loop moves on, and the two things that must never race it are the
# buffer swap and the drawbuffer readback.  Both phases of this test do
# exactly those -- phase A dumps mid-render, phase B compares the dump to the
# core buffer -- so a missing flush shows up as torn or stale pixels.
BLIT_MODE=""
GU_DEFER=""
for arg in "$@"; do
  case "$arg" in
    --blit-mode=*) BLIT_MODE="${arg#--blit-mode=}" ;;
    --gu-defer=*)  GU_DEFER="${arg#--gu-defer=}" ;;
    *) echo "usage: $0 [--blit-mode=0|1|2] [--gu-defer=0|1]" >&2
       echo "       0 cached (default), 1 uncached mirror, 2 VRAM (ADR-0034)" >&2
       echo "       --gu-defer=1 runs the deferred-sync path (ADR-0040)" >&2
       exit 2 ;;
  esac
done
case "$BLIT_MODE" in ""|0|1|2) ;; *) echo "--blit-mode wants 0, 1 or 2" >&2; exit 2 ;; esac
case "$GU_DEFER" in ""|0|1) ;; *) echo "--gu-defer wants 0 or 1" >&2; exit 2 ;; esac

ART="$SCRIPT_DIR/artifacts/gucolor${BLIT_MODE:+-bm$BLIT_MODE}${GU_DEFER:+-gd$GU_DEFER}-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"

fail() { echo "FAIL: $*" | tee -a "$ART/verdict.txt"; exit 1; }

[ -x "$PPSSPP_DIR/build/PPSSPPSDL" ] || fail "PPSSPPSDL not built"
[ -f "$EBOOT" ] || fail "psp/EBOOT.PBP missing"
[ -f "$ROM" ]   || fail "test ROM missing in testdata/"
[ -f "$BIOS" ]  || fail "gba_bios.bin missing in testdata/"

MS="$SANDBOX_ROOT/inst1/ppsspp"
GAME="$MS/PSP/GAME/gpsp-adhoc"

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
  [ "$DONE" -eq 1 ] || fail "phase $1: timed out waiting for EVT exit code=0"
}

# --- Phase A: test pattern ---------------------------------------------------
"$SCRIPT_DIR/setup_ppsspp.sh" --instances 1 --skip-build >/dev/null \
  || fail "sandbox creation failed"
mkdir -p "$GAME/log"
cp "$EBOOT" "$GAME/EBOOT.PBP"
cat > "$GAME/.gpsp-harness.ini" <<EOF
testpat = 1
EOF
[ -n "$BLIT_MODE" ] && echo "blit_mode = $BLIT_MODE" >> "$GAME/.gpsp-harness.ini"
[ -n "$GU_DEFER" ] && echo "gu_defer = $GU_DEFER" >> "$GAME/.gpsp-harness.ini"
run_instance testpat
cp "$GAME/log/frontend.log" "$ART/frontend-testpat.log" 2>/dev/null || true
GEBMP=$(ls "$GAME"/log/ge_*.bmp 2>/dev/null | head -1)
[ -n "$GEBMP" ] || fail "no GE dump produced in testpat phase"
cp "$GEBMP" "$ART/"
python3 "$SCRIPT_DIR/check_ge_colors.py" bars "$GEBMP" | tee -a "$ART/verdict.txt"
[ "${PIPESTATUS[0]}" -eq 0 ] || fail "testpat bar colors wrong (GE channel order?)"

# --- Phase B: in-game GE vs core-buffer compare ------------------------------
"$SCRIPT_DIR/setup_ppsspp.sh" --instances 1 --skip-build >/dev/null \
  || fail "sandbox recreation failed"
mkdir -p "$GAME/roms" "$GAME/log"
cp "$EBOOT" "$GAME/EBOOT.PBP"
cp "$BIOS"  "$GAME/gba_bios.bin"
cp "$ROM"   "$GAME/roms/emerald.gba"
cat > "$GAME/.gpsp-harness.ini" <<EOF
autoexit_frames = 700
dump_at = 600
gedump_at = 600
EOF
[ -n "$BLIT_MODE" ] && echo "blit_mode = $BLIT_MODE" >> "$GAME/.gpsp-harness.ini"
[ -n "$GU_DEFER" ] && echo "gu_defer = $GU_DEFER" >> "$GAME/.gpsp-harness.ini"
run_instance ingame
cp "$GAME/log/frontend.log" "$ART/frontend-ingame.log" 2>/dev/null || true
CORE="$GAME/log/frame_000600.bmp"
GE="$GAME/log/ge_000600.bmp"
[ -f "$CORE" ] || fail "core-buffer BMP missing"
[ -f "$GE" ]   || fail "GE drawbuffer BMP missing"
cp "$CORE" "$GE" "$ART/"
python3 "$SCRIPT_DIR/check_ge_colors.py" compare "$CORE" "$GE" 120 56 \
  | tee -a "$ART/verdict.txt"
[ "${PIPESTATUS[0]}" -eq 0 ] || fail "GE output != core buffer (GU path bug)"

echo "GU COLOR TEST PASS — artifacts in $ART"
exit 0
