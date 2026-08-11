#!/usr/bin/env bash
# run_soak.sh — Gate-1 30-minute gameplay soak, compressed via uncapped FF
# (plan §5 Phase 1, §7.1).
#
# Boots psp/EBOOT.PBP in a sandboxed PPSSPP with the supplied Emerald
# ROM/BIOS/save and runs testdata/fixtures/emerald_soak.inputs: CONTINUE into
# the overworld, then ~107,520 frames (30 min at 59.7275 Hz) of in-place
# field activity under uncapped fast-forward, ending with an interactivity
# check (start menu opens on a RAM predicate) and a clean exit.
# Asserts: clock=333, heartbeat EVTs form a gapless 600-frame sequence to
# >= 108000 frames, no ap_fail, EVT exit code=0, and the .sav untouched (no
# in-game save happens during the soak). Exit 0 = pass. Artifacts in
# tools/e2e/artifacts/soak-<timestamp>/.
#
# Prereqs: setup_ppsspp.sh ran once; psp/EBOOT.PBP built (pspdev docker).
# Run inside WSL/Linux from anywhere. Wall-clock budget: FF-dependent;
# default timeout 3600 s.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
SANDBOX_ROOT="${SANDBOX_ROOT:-$HOME/gpsp-e2e/sandboxes}"
TIMEOUT_S="${TIMEOUT_S:-3600}"
SOAK_MIN_FRAMES=108000   # >= 30 min of emulated gameplay past the lead-in

EBOOT="$REPO/psp/EBOOT.PBP"
ROM="$REPO/testdata/Pokemon - Emerald Version (USA, Europe).gba"
BIOS="$REPO/testdata/gba_bios.bin"
SAV="$REPO/testdata/Pokemon Emerald All Shiny Fixed.sav"
SOAK_SCRIPT="$REPO/testdata/fixtures/emerald_soak.inputs"

ART="$SCRIPT_DIR/artifacts/soak-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"

fail() { echo "FAIL: $*" | tee -a "$ART/verdict.txt"; exit 1; }
ok()   { echo "OK   $*" >> "$ART/verdict.txt"; }

[ -x "$PPSSPP_DIR/build/PPSSPPSDL" ] || fail "PPSSPPSDL not built (run setup_ppsspp.sh)"
for f in "$EBOOT" "$ROM" "$BIOS" "$SAV" "$SOAK_SCRIPT"; do
  [ -f "$f" ] || fail "missing: $f"
done

# --- sandbox -----------------------------------------------------------------
"$SCRIPT_DIR/setup_ppsspp.sh" --instances 1 --skip-build >/dev/null \
  || fail "setup_ppsspp.sh sandbox creation failed"
MS="$SANDBOX_ROOT/inst1/ppsspp"
GAME="$MS/PSP/GAME/gpsp-adhoc"
mkdir -p "$GAME/roms" "$GAME/log"
cp "$EBOOT" "$GAME/EBOOT.PBP"
cp "$BIOS"  "$GAME/gba_bios.bin"
cp "$ROM"   "$GAME/roms/emerald.gba"
cp "$SAV"   "$GAME/roms/emerald.sav"
cp "$SOAK_SCRIPT" "$GAME/run.inputs"
cat > "$GAME/.gpsp-harness.ini" <<EOF
script = run.inputs
autoexit_frames = 140000
EOF
SAV_MD5_BEFORE=$(md5sum "$GAME/roms/emerald.sav" | cut -d' ' -f1)
EVTLOG="$GAME/log/frontend.log"

# --- launch ------------------------------------------------------------------
rm -f /dev/shm/PPSSPP_ID
( cd "$PPSSPP_DIR/build" && \
  XDG_CONFIG_HOME="$SANDBOX_ROOT/inst1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  timeout $((TIMEOUT_S + 30)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
    ./PPSSPPSDL --windowed "$GAME/EBOOT.PBP" >"$ART/emu.log" 2>&1 ) &
EMU_PID=$!

DONE=0
for _ in $(seq 1 "$TIMEOUT_S"); do
  if grep -q "^EVT exit code=" "$EVTLOG" 2>/dev/null; then DONE=1; break; fi
  kill -0 "$EMU_PID" 2>/dev/null || break
  sleep 5
done
sleep 1
kill "$EMU_PID" 2>/dev/null || true
wait "$EMU_PID" 2>/dev/null || true

cp "$EVTLOG" "$ART/frontend.log" 2>/dev/null || true
for b in "$GAME"/log/frame_*.bmp; do
  [ -f "$b" ] && cp "$b" "$ART/"
done
[ "$DONE" -eq 1 ] || fail "timed out / crashed before EVT exit (see $ART)"

# --- assertions --------------------------------------------------------------
LOG="$ART/frontend.log"
grep -q "^EVT clock=333" "$LOG" || fail "clock != 333"
grep -q "^EVT exit code=0" "$LOG" \
  || fail "nonzero exit: $(grep "^EVT exit code=" "$LOG")"
grep -q "^EVT ap_fail" "$LOG" && fail "autopilot failure: $(grep "^EVT ap_fail" "$LOG")"
for pat in "^EVT ap_mark text=overworld_ok" "^EVT ap_mark text=soak_interactive" \
           "^EVT ap_mark text=soak_script_done" "^EVT ap_done"; do
  grep -q "$pat" "$LOG" || fail "missing marker: $pat"
done
ok "soak script completed (ap_done, interactivity check passed)"

# Heartbeats: gapless 600-frame ladder reaching SOAK_MIN_FRAMES
# NB: heartbeats carry a trailing " t_us=<wall clock>" since the hw-baseline work —
# capture only the frame count, not the rest of the line.
mapfile -t HB < <(grep "^EVT heartbeat frames=" "$LOG" | sed 's/.*frames=\([0-9]*\).*/\1/')
[ "${#HB[@]}" -gt 0 ] || fail "no heartbeats"
EXPECT=600
for h in "${HB[@]}"; do
  [ "$h" -eq "$EXPECT" ] || fail "heartbeat gap: expected $EXPECT, saw $h"
  EXPECT=$((EXPECT + 600))
done
LAST="${HB[-1]}"
[ "$LAST" -ge "$SOAK_MIN_FRAMES" ] \
  || fail "soak too short: last heartbeat $LAST < $SOAK_MIN_FRAMES"
ok "heartbeats gapless 600..$LAST (${#HB[@]} beats, no EVT gap)"

SAV_MD5_AFTER=$(md5sum "$GAME/roms/emerald.sav" | cut -d' ' -f1)
[ "$SAV_MD5_BEFORE" = "$SAV_MD5_AFTER" ] \
  || fail ".sav changed during soak: $SAV_MD5_BEFORE -> $SAV_MD5_AFTER"
ok ".sav intact md5=$SAV_MD5_AFTER"

cat "$ART/verdict.txt"
echo "SOAK PASS — artifacts in $ART"
exit 0
