#!/usr/bin/env bash
# run_save_test.sh — Gate-1 in-game save persistence test (plan §5 Phase 1, §7.1).
#
# Two sandboxed PPSSPP runs of the native PSP frontend (psp/EBOOT.PBP):
#   run 1: autopilot (testdata/fixtures/emerald_save.inputs) continues the
#          supplied Emerald save into the overworld under uncapped FF, opens
#          the start menu, performs a real in-game SAVE — every phase synced
#          on RAM predicates, not frame timing — and exits cleanly.
#   run 2: autopilot (emerald_verify.inputs) boots the .sav written by run 1,
#          CONTINUEs, and logs the player position/map from RAM.
# Asserts: run-1 SRAM CRC changed (in-game save actually landed on disk),
# run-2 sram_load CRC == run-1 flushed CRC, and the restored position/map
# equal the saved ones. Exit 0 = pass. Artifacts in
# tools/e2e/artifacts/save-<timestamp>/.
#
# Prereqs: setup_ppsspp.sh ran once; psp/EBOOT.PBP built (pspdev docker).
# Run inside WSL/Linux from anywhere.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
SANDBOX_ROOT="${SANDBOX_ROOT:-$HOME/gpsp-e2e/sandboxes}"
TIMEOUT_S="${TIMEOUT_S:-600}"

EBOOT="$REPO/psp/EBOOT.PBP"
ROM="$REPO/testdata/Pokemon - Emerald Version (USA, Europe).gba"
BIOS="$REPO/testdata/gba_bios.bin"
SAV="$REPO/testdata/Pokemon Emerald All Shiny Fixed.sav"
SAVE_SCRIPT="$REPO/testdata/fixtures/emerald_save.inputs"
VERIFY_SCRIPT="$REPO/testdata/fixtures/emerald_verify.inputs"

ART="$SCRIPT_DIR/artifacts/save-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"

fail() { echo "FAIL: $*" | tee -a "$ART/verdict.txt"; exit 1; }
ok()   { echo "OK   $*" >> "$ART/verdict.txt"; }

[ -x "$PPSSPP_DIR/build/PPSSPPSDL" ] || fail "PPSSPPSDL not built (run setup_ppsspp.sh)"
for f in "$EBOOT" "$ROM" "$BIOS" "$SAV" "$SAVE_SCRIPT" "$VERIFY_SCRIPT"; do
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
EVTLOG="$GAME/log/frontend.log"

# one_run <script-file> <tag>: boots the EBOOT with the given autopilot
# script, waits for "EVT exit code=", collects log+BMPs into $ART as <tag>.*
one_run() {
  local script="$1" tag="$2"
  cp "$script" "$GAME/run.inputs"
  cat > "$GAME/.gpsp-harness.ini" <<EOF
script = run.inputs
autoexit_frames = 60000
EOF
  rm -f "$GAME"/log/frontend.log "$GAME"/log/frame_*.bmp /dev/shm/PPSSPP_ID
  ( cd "$PPSSPP_DIR/build" && \
    XDG_CONFIG_HOME="$SANDBOX_ROOT/inst1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    timeout $((TIMEOUT_S + 30)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
      ./PPSSPPSDL --windowed "$GAME/EBOOT.PBP" >"$ART/$tag.emu.log" 2>&1 ) &
  local pid=$!
  local done=0
  for _ in $(seq 1 "$TIMEOUT_S"); do
    if grep -q "^EVT exit code=" "$EVTLOG" 2>/dev/null; then done=1; break; fi
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
  done
  sleep 1
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  cp "$EVTLOG" "$ART/$tag.frontend.log" 2>/dev/null || true
  for b in "$GAME"/log/frame_*.bmp; do
    [ -f "$b" ] && cp "$b" "$ART/$tag.$(basename "$b")"
  done
  [ "$done" -eq 1 ] || fail "$tag: timed out waiting for EVT exit (see $ART)"
  grep -q "^EVT exit code=0" "$ART/$tag.frontend.log" \
    || fail "$tag: nonzero exit: $(grep "^EVT exit code=" "$ART/$tag.frontend.log")"
}

evt1() { grep "$1" "$ART/run1.frontend.log"; }
evt2() { grep "$1" "$ART/run2.frontend.log"; }

# --- run 1: perform the in-game save -----------------------------------------
SAV_MD5_BEFORE=$(md5sum "$GAME/roms/emerald.sav" | cut -d' ' -f1)
one_run "$SAVE_SCRIPT" run1

for pat in "^EVT ap_loaded" "^EVT ap_mark text=title_ok" \
           "^EVT ap_mark text=overworld_ok" "^EVT ap_mark text=startmenu_open" \
           "^EVT ap_mark text=savedialog_open" "^EVT ap_mark text=sram_written" \
           "^EVT ap_mark text=startmenu_reopen" \
           "^EVT ap_mark text=save_script_done" "^EVT ap_done"; do
  evt1 "$pat" >/dev/null || fail "run1: missing marker: $pat"
done
ok "run1 autopilot sequence complete (ap_done)"

CRC_LOAD1=$(evt1 "^EVT sram_load" | sed -n 's/.*crc=\([0-9a-f]*\).*/\1/p')
CRC_FLUSH1=$(evt1 "^EVT sram_flush" | tail -1 | sed -n 's/.*crc=\([0-9a-f]*\).*/\1/p')
[ -n "$CRC_FLUSH1" ] || fail "run1: no sram_flush (in-game save never hit disk)"
[ "$CRC_FLUSH1" != "$CRC_LOAD1" ] || fail "run1: sram crc unchanged ($CRC_LOAD1)"
ok "run1 sram crc changed: $CRC_LOAD1 -> $CRC_FLUSH1"

SAV_MD5_AFTER=$(md5sum "$GAME/roms/emerald.sav" | cut -d' ' -f1)
[ "$SAV_MD5_BEFORE" != "$SAV_MD5_AFTER" ] || fail "run1: .sav file not rewritten"
ok "run1 .sav rewritten: $SAV_MD5_BEFORE -> $SAV_MD5_AFTER"

POS1=$(evt1 "^EVT ap_val name=pos" | sed -n 's/.*val=\(0x[0-9a-f]*\).*/\1/p')
LOC1=$(evt1 "^EVT ap_val name=loc" | sed -n 's/.*val=\(0x[0-9a-f]*\).*/\1/p')
[ -n "$POS1" ] && [ -n "$LOC1" ] || fail "run1: pos/loc ap_val missing"
ok "run1 saved spot: pos=$POS1 loc=$LOC1"

# --- run 2: restore and verify -----------------------------------------------
one_run "$VERIFY_SCRIPT" run2

for pat in "^EVT ap_mark text=overworld_ok" \
           "^EVT ap_mark text=verify_script_done" "^EVT ap_done"; do
  evt2 "$pat" >/dev/null || fail "run2: missing marker: $pat"
done

CRC_LOAD2=$(evt2 "^EVT sram_load" | sed -n 's/.*crc=\([0-9a-f]*\).*/\1/p')
[ "$CRC_LOAD2" = "$CRC_FLUSH1" ] \
  || fail "run2: loaded crc $CRC_LOAD2 != run1 flushed crc $CRC_FLUSH1"
ok "run2 loaded the run1 save: crc=$CRC_LOAD2"

POS2=$(evt2 "^EVT ap_val name=pos" | sed -n 's/.*val=\(0x[0-9a-f]*\).*/\1/p')
LOC2=$(evt2 "^EVT ap_val name=loc" | sed -n 's/.*val=\(0x[0-9a-f]*\).*/\1/p')
[ "$POS2" = "$POS1" ] || fail "run2: position $POS2 != saved $POS1"
[ "$LOC2" = "$LOC1" ] || fail "run2: map $LOC2 != saved $LOC1"
ok "run2 restored at the saved spot: pos=$POS2 loc=$LOC2"

cat "$ART/verdict.txt"
echo "SAVE TEST PASS — artifacts in $ART"
exit 0
