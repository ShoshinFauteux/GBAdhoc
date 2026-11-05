#!/usr/bin/env bash
# run_save_starve_test.sh -- how long does a battery save take to reach the
# stick when the game runs BELOW full speed at normal speed (no fast-forward)?
#
# The save writer runs below the main thread.  At full speed it gets the
# vblank wait; a game that cannot reach full speed never waits, so asking the
# writer for a scan (all that normal speed did up to 4.0.0-beta) wrote
# nothing.  The fix (fe_host_sram_ff_nudge): a save not on the stick 1.5 s
# after its last write gets what fast-forward gives it -- main sleeps ~1 ms
# every other frame until the writer confirms.
#
# One sandboxed PPSSPP run of a HARNESS EBOOT with
# testdata/fixtures/emerald_save_slow.inputs (the in-game save at normal
# speed, then 20 s of play) and `pace_slow_us` busy microseconds per frame.
#
#   run_save_starve_test.sh [--slow=US] [--eboot=FILE] [--expect=fixed|starved|none]
#
#   --slow=US     harness pace_slow_us (default 17000; 0 = the control)
#   --eboot=FILE  the harness EBOOT (default psp/EBOOT.PBP)
#   --expect=fixed    FAIL unless the save was on the stick within 4 s of
#                     its last write (the default)
#   --expect=starved  FAIL unless it took longer than 4 s (the unfixed
#                     build's arm: proves the fixture shows the problem)
#   --expect=none     report only
#
# Why 4 s and not 1.5: the nudge looks at a pending save once per settle
# period (30 emulated frames), and at ~29 fps that is a second, so the
# starved case is noticed 1.5-2.1 s after the last write and the flush
# itself needs a few dozen 1 ms sleeps.  PPSSPP, 2026-10-11, slow=17000:
# unfixed build: never on the stick during play (only the exit flush);
# fixed: 2.7 s; control (slow=0), both builds: 0.53 s.
#
# Env: TESTDATA (ROM/save/BIOS directory; default <repo>/testdata), BIOS,
# SANDBOX_ROOT (must be under ~/gpsp-e2e; give each agent its own),
# PPSSPP_DIR, TIMEOUT_S.  Artifacts: tools/e2e/artifacts/savestarve-<stamp>/.
# The same script and keys on hardware: see the header of the .inputs file.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
export SANDBOX_ROOT="${SANDBOX_ROOT:-$HOME/gpsp-e2e/sandboxes}"
TIMEOUT_S="${TIMEOUT_S:-900}"
TESTDATA="${TESTDATA:-$REPO/testdata}"

SLOW=17000
EBOOT="$REPO/psp/EBOOT.PBP"
EXPECT=fixed
for a in "$@"; do
  case "$a" in
    --slow=*)   SLOW="${a#*=}" ;;
    --eboot=*)  EBOOT="${a#*=}" ;;
    --expect=*) EXPECT="${a#*=}" ;;
    *) echo "unknown argument: $a" >&2; exit 2 ;;
  esac
done

ROM="$TESTDATA/Pokemon - Emerald Version (USA, Europe).gba"
BIOS="${BIOS:-$TESTDATA/gba_bios.bin}"
SAV="$TESTDATA/Pokemon Emerald All Shiny Fixed.sav"
INPUTS="$REPO/testdata/fixtures/emerald_save_slow.inputs"

ART="$SCRIPT_DIR/artifacts/savestarve-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"
fail() { echo "FAIL: $*" | tee -a "$ART/verdict.txt"; exit 1; }

[ -x "$PPSSPP_DIR/build/PPSSPPSDL" ] || fail "PPSSPPSDL not built (run setup_ppsspp.sh)"
for f in "$EBOOT" "$ROM" "$BIOS" "$SAV" "$INPUTS"; do
  [ -f "$f" ] || fail "missing: $f"
done

"$SCRIPT_DIR/setup_ppsspp.sh" --instances 1 --skip-build >/dev/null \
  || fail "setup_ppsspp.sh sandbox creation failed"
MS="$SANDBOX_ROOT/inst1/ppsspp"
GAME="$MS/PSP/GAME/gpsp-adhoc"
mkdir -p "$GAME/roms" "$GAME/log"
cp "$EBOOT" "$GAME/EBOOT.PBP"
[ -f "$(dirname "$EBOOT")/gbadhoc_me.prx" ] && cp "$(dirname "$EBOOT")/gbadhoc_me.prx" "$GAME/"
cp "$BIOS"  "$GAME/gba_bios.bin"
cp "$ROM"   "$GAME/roms/emerald.gba"
cp "$SAV"   "$GAME/roms/emerald.sav"
cp "$INPUTS" "$GAME/run.inputs"
EVTLOG="$GAME/log/frontend.log"
cat > "$GAME/.gpsp-harness.ini" <<EOF
script = run.inputs
autoexit_frames = 60000
pace_slow_us = $SLOW
folders = ${FOLDERS:-0}
EOF
rm -f "$GAME"/log/frontend.log

( cd "$PPSSPP_DIR/build" && \
  XDG_CONFIG_HOME="$SANDBOX_ROOT/inst1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  timeout $((TIMEOUT_S + 30)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
    ./PPSSPPSDL --windowed "$GAME/EBOOT.PBP" >"$ART/emu.log" 2>&1 ) &
pid=$!
done_=0
for _ in $(seq 1 "$TIMEOUT_S"); do
  if grep -q "^EVT exit code=" "$EVTLOG" 2>/dev/null; then done_=1; break; fi
  kill -0 "$pid" 2>/dev/null || break
  sleep 1
done
sleep 1
kill "$pid" 2>/dev/null || true      # this run's own process, by pid
wait "$pid" 2>/dev/null || true
cp "$EVTLOG" "$ART/frontend.log" 2>/dev/null || true
[ "$done_" -eq 1 ] || fail "timed out waiting for EVT exit (see $ART)"
LOG="$ART/frontend.log"

grep -q "^EVT ap_mark text=sram_written" "$LOG" || fail "the save was never made (no sram_written)"
grep -q "^EVT ap_mark text=save_script_done" "$LOG" || fail "script did not finish"

echo "--- slow=$SLOW us/frame  eboot=$(md5sum "$EBOOT" | cut -c1-8)" | tee -a "$ART/verdict.txt"
grep -E "^EVT (sram_persisted|sram_flush |sram_flush_slow|sram_nudge_starved|sram_ff_nudge gave_up)" "$LOG" \
  | tee -a "$ART/verdict.txt"

# The save's flush: the worst time from a save's last write to the stick
# having it, over the run (the exit flush is not a sram_persisted line).
WORST=$(grep "^EVT sram_persisted" "$LOG" | sed -n 's/.*ms_since_write=\([0-9]*\).*/\1/p' | sort -n | tail -1)
NPERS=$(grep -c "^EVT sram_persisted" "$LOG")
echo "persisted=$NPERS worst_ms_since_write=${WORST:-none}" | tee -a "$ART/verdict.txt"

case "$EXPECT" in
  fixed)
    [ "$NPERS" -ge 1 ] || fail "the save was never confirmed on the stick during play"
    [ "$WORST" -le 4000 ] || fail "save took ${WORST} ms to reach the stick (> 4000)"
    ;;
  starved)
    if [ "$NPERS" -ge 1 ] && [ "$WORST" -le 4000 ]; then
      fail "expected a starved writer, but the save was on the stick in ${WORST} ms"
    fi
    ;;
esac
echo "SAVE STARVE TEST PASS (expect=$EXPECT) -- artifacts in $ART"
exit 0
