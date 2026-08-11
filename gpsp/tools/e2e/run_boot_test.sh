#!/usr/bin/env bash
# run_boot_test.sh — Gate-1 boot test (plan §5 Phase 1, §7.1).
#
# Boots our native PSP frontend (psp/EBOOT.PBP) in a sandboxed PPSSPP
# instance under Xvfb, with the supplied Emerald ROM + real BIOS + real save,
# and asserts the full Gate-1 EVT sequence from the ms0: log channel:
#   boot_ok, clock=333, bios=real (sha1 match), rom_loaded code=BPEE,
#   mem_free measured, sram_load of the 128 KiB save, frame_dump BMP at
#   frame 600, heartbeats to 3600 frames, exit code=0, save file intact.
#
# Exit 0 = pass. Artifacts (EVT log, emu log, BMP, expected-vs-seen) are
# collected into tools/e2e/artifacts/boot-<timestamp>/.
#
# Prereqs: tools/e2e/setup_ppsspp.sh already ran once (PPSSPP built);
# psp/EBOOT.PBP built (pspdev docker, see psp/Makefile header).
# Run inside WSL/Linux from anywhere.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
SANDBOX_ROOT="${SANDBOX_ROOT:-$HOME/gpsp-e2e/sandboxes}"
TIMEOUT_S="${TIMEOUT_S:-300}"

EBOOT="$REPO/psp/EBOOT.PBP"
ROM="$REPO/testdata/Pokemon - Emerald Version (USA, Europe).gba"
BIOS="$REPO/testdata/gba_bios.bin"
SAV="$REPO/testdata/Pokemon Emerald All Shiny Fixed.sav"

ART="$SCRIPT_DIR/artifacts/boot-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"

fail() { echo "FAIL: $*" | tee -a "$ART/verdict.txt"; exit 1; }

[ -x "$PPSSPP_DIR/build/PPSSPPSDL" ] || fail "PPSSPPSDL not built (run setup_ppsspp.sh)"
[ -f "$EBOOT" ] || fail "psp/EBOOT.PBP missing (build it: see psp/Makefile header)"
[ -f "$ROM" ]   || fail "test ROM missing in testdata/"
[ -f "$BIOS" ]  || fail "gba_bios.bin missing in testdata/"
[ -f "$SAV" ]   || fail "test .sav missing in testdata/"

# --- 1. Fresh sandbox (recreates ini per docs/TESTING.md) --------------------
"$SCRIPT_DIR/setup_ppsspp.sh" --instances 1 --skip-build >/dev/null \
  || fail "setup_ppsspp.sh sandbox creation failed"

MS="$SANDBOX_ROOT/inst1/ppsspp"
GAME="$MS/PSP/GAME/gpsp-adhoc"
mkdir -p "$GAME/roms" "$GAME/log"
cp "$EBOOT" "$GAME/EBOOT.PBP"
cp "$BIOS"  "$GAME/gba_bios.bin"
cp "$ROM"   "$GAME/roms/emerald.gba"
cp "$SAV"   "$GAME/roms/emerald.sav"
cat > "$GAME/.gpsp-harness.ini" <<EOF
autoexit_frames = 3600
dump_at = 600
EOF
# ADR-0036: plant the exact leftover that caused the incident — an
# autopilot.ini carrying `join=1`, which on the old build silently brought a
# wireless session up during a solo run.  It must now be inert, reported, and
# STILL PRESENT afterwards (we never delete a user's file).
cat > "$GAME/autopilot.ini" <<EOF
join = 1
host = 1
ff = 1
EOF
SAV_MD5_BEFORE=$(md5sum "$GAME/roms/emerald.sav" | cut -d' ' -f1)

# --- 2. Launch under Xvfb (recipe = setup_ppsspp.sh boot-check) --------------
rm -f /dev/shm/PPSSPP_ID
EVTLOG="$GAME/log/frontend.log"
( cd "$PPSSPP_DIR/build" && \
  XDG_CONFIG_HOME="$SANDBOX_ROOT/inst1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  timeout $((TIMEOUT_S + 30)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
    ./PPSSPPSDL --windowed "$GAME/EBOOT.PBP" >"$ART/emu.log" 2>&1 ) &
EMU_PID=$!

# --- 3. Poll for clean exit (EVT exit code=0) --------------------------------
DONE=0
for _ in $(seq 1 "$TIMEOUT_S"); do
  if grep -q "^EVT exit code=0" "$EVTLOG" 2>/dev/null; then DONE=1; break; fi
  kill -0 "$EMU_PID" 2>/dev/null || break
  sleep 1
done
sleep 1   # let final writes land
kill "$EMU_PID" 2>/dev/null || true
wait "$EMU_PID" 2>/dev/null || true

# --- 4. Collect artifacts ----------------------------------------------------
cp "$EVTLOG" "$ART/frontend.log" 2>/dev/null || true
cp "$GAME"/log/frame_*.bmp "$ART/" 2>/dev/null || true

[ "$DONE" -eq 1 ] || fail "timed out waiting for 'EVT exit code=0' (see $ART)"

# --- 5. Assertions: expected-vs-seen -----------------------------------------
declare -a EXPECT=(
  "^EVT boot_ok"
  "^EVT clock=333"
  "^EVT bios=real sha1=300c20df6731a33952ded8c436f7f186d25d3492"
  "^EVT rom_loaded code=BPEE size=16777216"
  "^EVT av_info fps=59.727"
  "^EVT sram_load size=131072"
  "^EVT mem_free="
  "^EVT frame_dump file=.*frame_000600.bmp"
  "^EVT heartbeat frames=3600"
  "^EVT exit code=0"
  # ADR-0036: the planted legacy autopilot.ini must be REPORTED...
  "^EVT legacy_autopilot_ignored file=.*autopilot\.ini"
)
MISSING=0
for pat in "${EXPECT[@]}"; do
  if grep -q "$pat" "$ART/frontend.log"; then
    echo "OK   $pat" >> "$ART/verdict.txt"
  else
    echo "MISS $pat" >> "$ART/verdict.txt"
    MISSING=1
  fi
done

# ADR-0036: ...and must be OBEYED BY NOTHING.  `join=1`/`host=1`/`ff=1` in the
# legacy file would, on the old build, have brought a wireless session up and
# forced fast-forward during what is a deliberately solo run.  Assert the
# absence of every trace of that, and that the user's file still exists.
for pat in "^EVT adhoc_up" "^EVT net_pace_match" "^EVT session_pace"; do
  if grep -q "$pat" "$ART/frontend.log"; then
    echo "MISS legacy autopilot.ini was OBEYED — saw $pat (ADR-0036)" \
      >> "$ART/verdict.txt"
    MISSING=1
  fi
done
if [ -f "$GAME/autopilot.ini" ]; then
  echo "OK   legacy autopilot.ini ignored, reported, and left on disk" \
    >> "$ART/verdict.txt"
else
  echo "MISS legacy autopilot.ini was DELETED — never remove a user's file (ADR-0036)" \
    >> "$ART/verdict.txt"
  MISSING=1
fi
# The renamed channel must still work: dump_at/autoexit above came from it.
if grep -q "^EVT harness_ini active file=.*\.gpsp-harness\.ini" "$ART/frontend.log"; then
  echo "OK   .gpsp-harness.ini is the live channel and announces itself" \
    >> "$ART/verdict.txt"
else
  echo "MISS harness_ini active line (ADR-0036)" >> "$ART/verdict.txt"
  MISSING=1
fi

# BMP screenshot exists and is a plausible 240x160 24bpp BMP (~115 KB)
BMP="$ART/frame_000600.bmp"
if [ -f "$BMP" ] && [ "$(stat -c%s "$BMP")" -ge 100000 ]; then
  echo "OK   screenshot $BMP ($(stat -c%s "$BMP") bytes)" >> "$ART/verdict.txt"
else
  echo "MISS screenshot frame_000600.bmp" >> "$ART/verdict.txt"
  MISSING=1
fi

# Save intact: no in-game save happened, so the .sav must be byte-identical
SAV_MD5_AFTER=$(md5sum "$GAME/roms/emerald.sav" | cut -d' ' -f1)
if [ "$SAV_MD5_BEFORE" = "$SAV_MD5_AFTER" ]; then
  echo "OK   sav intact md5=$SAV_MD5_AFTER" >> "$ART/verdict.txt"
else
  echo "MISS sav changed: $SAV_MD5_BEFORE -> $SAV_MD5_AFTER" >> "$ART/verdict.txt"
  MISSING=1
fi

cat "$ART/verdict.txt"
if [ "$MISSING" -ne 0 ]; then
  echo "BOOT TEST FAIL — artifacts in $ART"
  exit 1
fi
echo "BOOT TEST PASS — artifacts in $ART"
exit 0
