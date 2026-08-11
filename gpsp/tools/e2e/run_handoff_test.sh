#!/usr/bin/env bash
# run_handoff_test.sh — ADR-0053 milestone 1 on the rig, end to end.
#
# WHAT IS ACTUALLY UNDER TEST: that a console can finish a run, publish a
# result, export its memory stick, be serviced by the PC, and RELAUNCH ITSELF
# into the next run — repeatedly, unattended, on both consoles at once.
#
# The PSP binary is the SHIPPING binary.  Nothing here is conditional on
# PPSSPP: the same code path runs on hardware.  What the rig cannot reproduce
# is the physical USB enumeration, so `sceUsbActivate` succeeds without a host
# actually mounting anything.  That is precisely why the handoff was designed
# to need no mount/eject signal — it serves a window, releases the volume, and
# looks for the command file.  That design is what makes this test meaningful
# rather than a simulation of itself.
#
# Not covered here, and it must not be claimed: real USB enumeration, and
# whether the PSP's filesystem sees the PC's freshly written CMD.TXT rather
# than a cached directory entry.  On the rig both processes share one kernel's
# page cache, so that question is answered by hardware or not at all.
#
# Usage (inside WSL):
#   tools/e2e/run_handoff_test.sh [--runs N] [--window S]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
SANDBOX_ROOT="${SANDBOX_ROOT:-$HOME/gpsp-e2e/sandboxes}"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
ART="$SCRIPT_DIR/artifacts/handoff"
NOPC=0            # --no-pc: never start hw_loop.py.  The consoles must keep
                  #   relaunching on their own rather than falling to the XMB --
                  #   a chain that ends when the PC goes quiet is not autonomous.
NEGATIVE=0        # --negative: disable the handoff and require this test to FAIL
STARTUPFAIL=0     # --startup-fail: force an EARLY exit (before the main loop).
                  #   The console must STILL publish a result and hand over
                  #   rather than vanish to the XMB.  Staged by withholding the
                  #   ROM, which fails deterministically at find_first_rom.
                  #   An earlier version forced it by making both consoles JOIN
                  #   with nobody hosting -- but whether adhocctl group join
                  #   fails depends on which instance wins a startup race, so
                  #   it passed or failed at random and tested nothing.
RUNS=3
WINDOW=8
TOTAL=120
FRAMES=420          # ~7 s of emulated run; we are testing handoff, not gameplay

while [ $# -gt 0 ]; do
  case "$1" in
    --runs)   RUNS="$2";   shift 2 ;;
    --window) WINDOW="$2"; shift 2 ;;
    --frames) FRAMES="$2"; shift 2 ;;
    --negative) NEGATIVE=1; shift ;;
    --no-pc)    NOPC=1; shift ;;
    --startup-fail) STARTUPFAIL=1; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

fail() { echo "FAIL: $*" >&2; exit 1; }
note() { echo "[handoff] $*"; }

EBOOT="$REPO/psp/EBOOT.PBP"
[ -f "$EBOOT" ] || fail "psp/EBOOT.PBP missing — build it first"
[ -x "$PPSSPP_DIR/build/PPSSPPSDL" ] || fail "PPSSPPSDL not built (setup_ppsspp.sh)"

# Assets live on the memory cards, not in git (ROMs are not committed).
ASSET_SRC="${ASSET_SRC:-/mnt/d/PSP/GAME/gpsp-adhoc}"
[ -f "$ASSET_SRC/roms/emerald.gba" ] || fail "no ROM at $ASSET_SRC/roms/emerald.gba (set ASSET_SRC=)"
[ -f "$ASSET_SRC/gba_bios.bin" ]     || fail "no BIOS at $ASSET_SRC/gba_bios.bin"

rm -rf "$ART"; mkdir -p "$ART"
"$SCRIPT_DIR/setup_ppsspp.sh" --instances 2 --skip-build >/dev/null || fail "sandbox setup failed"

GOLDEN="$ART/golden"; mkdir -p "$GOLDEN"
for i in 1 2; do
  MS="$SANDBOX_ROOT/inst$i/ppsspp/PSP/GAME/gpsp-adhoc"
  mkdir -p "$MS/roms" "$MS/log" "$MS/handoff"
  cp "$EBOOT" "$MS/EBOOT.PBP"
  cp "$ASSET_SRC/gba_bios.bin" "$MS/"
  [ "$STARTUPFAIL" -eq 1 ] || cp "$ASSET_SRC/roms/emerald.gba" "$MS/roms/"
  [ -f "$ASSET_SRC/roms/EMERALD.SAV" ] && cp "$ASSET_SRC/roms/EMERALD.SAV" "$MS/roms/"
  # Solo boot: no host/join keys, so no wireless session is attempted.  The
  # handoff sits after teardown and does not care whether a session ran.
  cat > "$MS/.gpsp-harness.ini" <<EOF
autoexit_frames = $FRAMES
handoff = $((1 - NEGATIVE))
handoff_window_s = $WINDOW
handoff_total_s = $([ "$NOPC" -eq 1 ] && echo $((WINDOW * 2)) || echo $TOTAL)
handoff_max_runs = $((RUNS + 2))
EOF
done
# Golden saves, role-prefixed, so the loop restores them between runs.
if [ -f "$ASSET_SRC/roms/EMERALD.SAV" ]; then
  cp "$ASSET_SRC/roms/EMERALD.SAV" "$GOLDEN/host-EMERALD.SAV"
  cp "$ASSET_SRC/roms/EMERALD.SAV" "$GOLDEN/join-EMERALD.SAV"
fi
note "sandboxes staged; runs=$RUNS window=${WINDOW}s frames=$FRAMES"

# --- PC half, in the background ---------------------------------------------
# --no-eject because there is no volume to eject; the console falls through to
# its window, which is exactly the path hardware takes when an eject is missed.
if [ "$NOPC" -eq 1 ]; then
  ( sleep 1 ) & LOOP_PID=$!      # no PC servicing at all
else
python3 "$REPO/tools/hw_loop.py" \
  --host "$SANDBOX_ROOT/inst1/ppsspp" \
  --join "$SANDBOX_ROOT/inst2/ppsspp" \
  --golden "$GOLDEN" --logs "$ART/logs" \
  --runs "$RUNS" --poll 1 --timeout 300 --no-eject --keep-going --verify \
  >"$ART/hw_loop.log" 2>&1 &
LOOP_PID=$!
fi

launch() {
  ( cd "$PPSSPP_DIR/build" && \
    XDG_CONFIG_HOME="$SANDBOX_ROOT/inst$1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    timeout 900 xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
      ./PPSSPPSDL --windowed \
      "$SANDBOX_ROOT/inst$1/ppsspp/PSP/GAME/gpsp-adhoc/EBOOT.PBP" \
  ) >"$ART/inst$1.emu.log" 2>&1 &
  echo $!
}
PID1=$(launch 1); PID2=$(launch 2)
note "launched inst1=$PID1 inst2=$PID2; waiting for the chain"

wait "$LOOP_PID"; LOOP_RC=$?
[ "$NOPC" -eq 1 ] && sleep $((WINDOW * 6))
sleep 3
kill "$PID1" "$PID2" 2>/dev/null; wait "$PID1" "$PID2" 2>/dev/null

# --- Assertions: what the chain must actually have DONE ----------------------
rc=0
for i in 1 2; do
  role=$([ "$i" -eq 1 ] && echo host || echo join)
  ST="$SANDBOX_ROOT/inst$i/ppsspp/PSP/GAME/gpsp-adhoc/handoff/STATE.TXT"
  RUNSF="$SANDBOX_ROOT/inst$i/ppsspp/PSP/GAME/gpsp-adhoc/handoff/RUNS.TXT"
  cp "$ST" "$ART/$role-STATE.TXT" 2>/dev/null
  [ -f "$ST" ] || { echo "FAIL $role: no STATE.TXT — handoff never ran"; rc=1; continue; }

  # A relaunch is the ONLY thing that proves the loop closed.  Everything else
  # (usb up, window served) can happen once and then die quietly.
  # NB: `grep -c` PRINTS 0 and EXITS 1 when there is no match, so the obvious
  # `$(grep -c ... || echo 0)` appends a SECOND zero and yields a two-line
  # string; every numeric test below then dies with "integer expected", which
  # reads as a failing assertion rather than a broken one.  grep already prints
  # the count; it needs no fallback, only for its exit status to be ignored.
  relaunches=$(grep -c "CMD=RUN -- relaunching" "$ST" 2>/dev/null); relaunches=${relaunches:-0}
  usbfail=$(grep -c "usb up FAILED" "$ST" 2>/dev/null);             usbfail=${usbfail:-0}
  loadexecfail=$(grep -c "LoadExec RETURNED" "$ST" 2>/dev/null);    loadexecfail=${loadexecfail:-0}
  lastrun=$(cat "$RUNSF" 2>/dev/null | tr -d '\n\r')

  echo "--- $role: relaunches=$relaunches usb_fail=$usbfail loadexec_fail=$loadexecfail RUNS.TXT=$lastrun"
  [ "$usbfail" -eq 0 ]      || { echo "FAIL $role: sceUsb bring-up failed"; rc=1; }
  [ "$loadexecfail" -eq 0 ] || { echo "FAIL $role: sceKernelLoadExec returned (relaunch failed)"; rc=1; }
  [ "$relaunches" -ge $((RUNS - 1)) ] || {
    echo "FAIL $role: only $relaunches relaunch(es), expected >= $((RUNS - 1))"; rc=1; }
  [ "${lastrun:-0}" -ge "$RUNS" ] || {
    echo "FAIL $role: run counter reached ${lastrun:-0}, expected >= $RUNS"; rc=1; }
  # The volume must be RELEASED before the command file is read, every cycle.
  # Reading it while exported is the corruption path this design exists to
  # avoid, so assert the ordering actually happened rather than trusting it.
  released=$(grep -c "usb released" "$ST" 2>/dev/null); released=${released:-0}
  [ "$released" -ge "$relaunches" ] || {
    echo "FAIL $role: $relaunches relaunch(es) but only $released release(s)"; rc=1; }
done

if [ "$NOPC" -eq 0 ]; then
# The PC must have collected a distinct log per run per console.
got=$(ls "$ART/logs"/auto*.log 2>/dev/null | wc -l)
want=$((RUNS * 2))
echo "--- collected logs: $got (want $want)"
[ "$got" -eq "$want" ] || { echo "FAIL: expected $want collected logs, got $got"; rc=1; }

# IN-SITU CHECK ON THE ORACLE ITSELF.  This test boots and exits; it never
# trades.  So --verify MUST report "TRADE NOT VERIFIED" for every run.  If it
# ever reports a verified trade here, the oracle returns success for a run in
# which nothing happened, and every chain built on it is worthless.  Asserting
# the NEGATIVE is the only way to know the oracle discriminates at all.
notver=$(grep -c "TRADE NOT VERIFIED" "$ART/hw_loop.log" 2>/dev/null); notver=${notver:-0}
falsepos=$(grep -c "TRADE VERIFIED" "$ART/hw_loop.log" 2>/dev/null); falsepos=${falsepos:-0}
echo "--- oracle on a no-trade run: not-verified=$notver false-positives=$falsepos"
[ "$falsepos" -eq 0 ] || { echo "FAIL: oracle reported a trade in a run that never traded"; rc=1; }
[ "$notver" -ge 1 ]   || { echo "FAIL: oracle never ran (expected NOT VERIFIED every run)"; rc=1; }

# The post-run save must survive the golden restore, else the party oracle has
# nothing to verify and the whole chain is unfalsifiable.
saves=$(ls "$ART/logs"/auto*.sav "$ART/logs"/auto*.SAV 2>/dev/null | wc -l)
echo "--- post-run saves preserved: $saves (want $want)"
[ "$saves" -eq "$want" ] || { echo "FAIL: expected $want preserved saves, got $saves"; rc=1; }

# Each collected log must be a DIFFERENT run, not the same file copied N times.
if [ "$got" -gt 0 ]; then
  uniq_boots=$(grep -h "EVT build stamp=" "$ART/logs"/auto*.log 2>/dev/null | wc -l)
  echo "--- logs carrying a build stamp: $uniq_boots"
  [ "$uniq_boots" -eq "$got" ] || { echo "FAIL: some collected logs have no build stamp"; rc=1; }
fi
fi   # NOPC

echo
# NEGATIVE CONTROL: with handoff disabled the chain cannot form, so the
# assertions above MUST fail.  A gate that passes either way measures nothing --
# this project has shipped three of those already.  Inverting the verdict here
# makes "the test can fail" a thing that is itself tested.
if [ "$NOPC" -eq 1 ]; then
  # THE AUTONOMY TEST.  No PC ever answers.  A console that exits here has
  # left the chain and needs a human to press X -- which is precisely the
  # intervention the loop exists to remove.  It must relaunch instead.
  ok=1
  for i in 1 2; do
    role=$([ "$i" -eq 1 ] && echo host || echo join)
    ST="$SANDBOX_ROOT/inst$i/ppsspp/PSP/GAME/gpsp-adhoc/handoff/STATE.TXT"
    n=$(grep -c "relaunching to stay available" "$ST" 2>/dev/null); n=${n:-0}
    echo "  $role: self-relaunched-with-no-pc=$n"
    [ "$n" -ge 1 ] || { echo "  $role: LEFT THE CHAIN when the PC went quiet"; ok=0; }
  done
  [ "$ok" -eq 1 ] && { echo "PASS (no-pc): consoles stayed in the loop unattended"; exit 0; }
  echo "FAIL (no-pc): a console left the chain when the PC went quiet"; exit 1
fi

if [ "$STARTUPFAIL" -eq 1 ]; then
  # Both consoles were told to JOIN and nobody is hosting, so net_bringup must
  # fail.  The POINT is that a startup failure still reports itself: before
  # ADR-0053's addendum every early exit called sceKernelExitGame() directly,
  # so a console that could not join dropped out of the chain silently and the
  # PC waited for a result that was never coming.  That is the single most
  # likely failure in an unattended chain -- the host being slow to come up
  # after a relaunch is exactly this.
  # Assertion: EVERY console publishes a result whatever happens, and the
  # early-exit path (which used to call sceKernelExitGame() directly and drop
  # the console out of the chain silently) hands over instead.
  published=0; failed_and_handed=0
  for i in 1 2; do
    role=$([ "$i" -eq 1 ] && echo host || echo join)
    ST="$SANDBOX_ROOT/inst$i/ppsspp/PSP/GAME/gpsp-adhoc/handoff/STATE.TXT"
    if [ -s "$ST" ]; then
      published=$((published + 1))
      echo "  $role: published $(head -1 "$ST")"
      grep -qE "reason=(no_rom|net_failed|load_failed|bad_script)" "$ST" \n        && failed_and_handed=$((failed_and_handed + 1))
    else
      echo "  $role: NOTHING PUBLISHED -- vanished to the XMB"
    fi
  done
  echo "  published=$published/2  early-exit-handed-over=$failed_and_handed"
  if [ "$published" -eq 2 ] && [ "$failed_and_handed" -ge 1 ]; then
    echo "PASS (startup-fail): a failed bring-up still reported and handed over"
    exit 0
  fi
  echo "FAIL (startup-fail): a console vanished instead of reporting"; exit 1
fi

if [ "$NEGATIVE" -eq 1 ]; then
  if [ "$rc" -ne 0 ]; then
    echo "PASS (negative control): handoff disabled -> assertions correctly FAILED"
    exit 0
  fi
  echo "FAIL (negative control): handoff disabled but the test still passed."
  echo "                        The assertions do not actually check anything."
  exit 1
fi

if [ "$rc" -eq 0 ]; then
  echo "PASS: $RUNS chained runs on two consoles, self-relaunched, logs collected"
else
  echo "artifacts: $ART"
fi
exit "$rc"
