#!/usr/bin/env bash
# run_folders_test.sh -- the saves/ and states/ folders end to end in PPSSPP
# (4.6.0-beta.4, psp/sidecar_dirs.h).  Harness build (psp/EBOOT.PBP).
#
#   A  an OLD-LAYOUT stick: run 0 (folders = 0, the beta.3 layout) leaves
#      roms/emerald.sav + emerald.st0 + .thumb + .shot beside the ROM.
#   B  run 1 (folders = 1, load_state = 1): the files move to saves/ and
#      states/ byte for byte, nothing is left in roms/, the save loads (same
#      CRC as run 0) and the state loads (rc=0) from the new place.
#   C  state round trip in the new layout: save at frame 150, load it back.
#   D  a move that cannot complete (saves/emerald.sav is a FOLDER in the way):
#      the legacy save is kept, loaded and left untouched.
#   E  ROMs in roms/GBA and roms/GB through the REAL browser (a scripted
#      CROSS / TRIANGLE): both listed, both boot, their saves are
#      saves/GBA/... and saves/GB/....gb.sav.
#
# Env: PPSSPP_DIR, SANDBOX_ROOT (default ~/gpsp-e2e/folders-sb: a sandbox of
# its own, other suites' runs are not disturbed), TESTDATA (the Emerald ROM,
# BIOS and .sav), GB_ROM (a .gb file for E; skipped without one).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
export SANDBOX_ROOT="${SANDBOX_ROOT:-$HOME/gpsp-e2e/folders-sb}"
TESTDATA="${TESTDATA:-$REPO/testdata}"
TIMEOUT_S="${TIMEOUT_S:-240}"
EBOOT="${EBOOT:-$REPO/psp/EBOOT.PBP}"
ROM="$TESTDATA/Pokemon - Emerald Version (USA, Europe).gba"
BIOS="$TESTDATA/gba_bios.bin"
SAV="$TESTDATA/Pokemon Emerald All Shiny Fixed.sav"
GB_ROM="${GB_ROM:-}"

ART="$SCRIPT_DIR/artifacts/folders-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"
fail() { echo "FAIL: $*" | tee -a "$ART/verdict.txt"; exit 1; }
ok()   { echo "OK   $*" | tee -a "$ART/verdict.txt"; }

[ -x "$PPSSPP_DIR/build/PPSSPPSDL" ] || fail "PPSSPPSDL not built"
for f in "$EBOOT" "$ROM" "$BIOS" "$SAV"; do [ -f "$f" ] || fail "missing: $f"; done

"$SCRIPT_DIR/setup_ppsspp.sh" --instances 1 --skip-build >/dev/null \
  || fail "sandbox creation failed"
MS="$SANDBOX_ROOT/inst1/ppsspp"
GAME="$MS/PSP/GAME/gpsp-adhoc"
mkdir -p "$GAME/roms" "$GAME/log"
cp "$EBOOT" "$GAME/EBOOT.PBP"
for x in gbadhoc_me.prx gbadhoc_home.prx; do
  for d in "$(dirname "$EBOOT")" "$(dirname "$EBOOT")/me" "$(dirname "$EBOOT")/home"; do
    [ -f "$d/$x" ] && { cp "$d/$x" "$GAME/"; break; }
  done
done
cp "$BIOS" "$GAME/gba_bios.bin"
EVTLOG="$GAME/log/frontend.log"

# one_run <tag> <ini lines...>
one_run() {
  local tag="$1"; shift
  printf '%s\n' "$@" > "$GAME/.gpsp-harness.ini"
  rm -f "$EVTLOG"
  ( cd "$PPSSPP_DIR/build" && \
    XDG_CONFIG_HOME="$SANDBOX_ROOT/inst1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    timeout $((TIMEOUT_S + 30)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
      ./PPSSPPSDL --windowed "$GAME/EBOOT.PBP" >"$ART/$tag.emu.log" 2>&1 ) &
  local pid=$! done=0
  for _ in $(seq 1 "$TIMEOUT_S"); do
    if grep -q "^EVT exit code=" "$EVTLOG" 2>/dev/null; then done=1; break; fi
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
  done
  sleep 1
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  cp "$EVTLOG" "$ART/$tag.frontend.log" 2>/dev/null || true
  (cd "$GAME" && find . -path ./log -prune -o -type f -print | sort) > "$ART/$tag.files.txt"
  [ "$done" -eq 1 ] || fail "$tag: timed out (see $ART)"
  grep -q "^EVT exit code=0" "$ART/$tag.frontend.log" \
    || fail "$tag: $(grep '^EVT exit code=' "$ART/$tag.frontend.log")"
}
ev() { grep -i "$2" "$ART/$1.frontend.log"; }
md5() { md5sum "$1" | cut -d' ' -f1; }

# ------------------------------------------------------- A: old layout
cp "$ROM" "$GAME/roms/emerald.gba"
cp "$SAV" "$GAME/roms/emerald.sav"
one_run run0 "rom = emerald.gba" "folders = 0" "state_save_at = 240" \
             "autoexit_frames = 300"
ev run0 "^EVT state_save_at f=240 rc=0" >/dev/null || fail "run0: no state saved"
for f in emerald.sav emerald.st0 emerald.st0.thumb emerald.st0.shot; do
  [ -f "$GAME/roms/$f" ] || fail "run0: roms/$f missing"
done
[ ! -e "$GAME/saves" ] && [ ! -e "$GAME/roms/GBA" ] \
  || fail "run0: folders = 0 must not create the folders"
M_SAV=$(md5 "$GAME/roms/emerald.sav"); M_ST=$(md5 "$GAME/roms/emerald.st0")
M_TH=$(md5 "$GAME/roms/emerald.st0.thumb"); M_SH=$(md5 "$GAME/roms/emerald.st0.shot")
CRC0=$(ev run0 "^EVT sram_load" | sed -n 's/.*crc=\([0-9a-f]*\).*/\1/p')
ok "A old-layout stick: roms/emerald.{sav,st0,st0.thumb,st0.shot} (sram crc $CRC0)"

# ------------------------------------------------------- B: migration
one_run run1 "rom = emerald.gba" "folders = 1" "load_state = 1" \
             "autoexit_frames = 200"
ev run1 "^EVT folders save_mig=1 state_mig=1 save=.*/saves/emerald.sav state=.*/states/emerald.st0" \
  >/dev/null || fail "run1: $(ev run1 '^EVT folders ')"
[ "$(ev run1 '^EVT folders_moved' | wc -l)" -ge 4 ] || fail "run1: expected >= 4 folders_moved"
# the originals are removed only after EVERY copy of the group verified:
# all removals follow the last copy (no folders_copy_failed, moved lines last)
ev run1 "folders_copy_failed\|folders_migrate_failed" >/dev/null && fail "run1: a copy failed"
[ "$(md5 "$GAME/saves/emerald.sav")" = "$M_SAV" ] || fail "run1: saves/emerald.sav differs"
[ "$(md5 "$GAME/states/emerald.st0")" = "$M_ST" ] || fail "run1: state differs"
[ "$(md5 "$GAME/states/emerald.st0.thumb")" = "$M_TH" ] || fail "run1: thumb differs"
[ "$(md5 "$GAME/states/emerald.st0.shot")" = "$M_SH" ] || fail "run1: shot differs"
for f in emerald.sav emerald.st0 emerald.st0.thumb emerald.st0.shot; do
  [ ! -e "$GAME/roms/$f" ] || fail "run1: roms/$f still there"
done
[ -f "$GAME/roms/emerald.gba" ] || fail "run1: the ROM moved"
find "$GAME" -name '*.mig' | grep -q . && fail "run1: a .mig was left"
for d in GBA GB GBC; do [ -f "$GAME/roms/$d/.keep" ] || fail "run1: roms/$d/.keep"; done
CRC1=$(ev run1 "^EVT sram_load" | sed -n 's/.*crc=\([0-9a-f]*\).*/\1/p')
[ "$CRC1" = "$CRC0" ] || fail "run1: save crc $CRC1 != $CRC0"
ev run1 "^EVT state_load file=.*/states/emerald.st0 " >/dev/null || fail "run1: state not loaded from states/"
ev run1 "^EVT autoload_state slot=1 rc=0" >/dev/null || fail "run1: autoload rc"
ok "B moved byte-identical, roms/ cleared (ROM kept), save crc $CRC1 and state loaded from the new place"

# ------------------------------------------------------- C: round trip
one_run run2 "rom = emerald.gba" "folders = 1" "state_save_at = 150" \
             "autoexit_frames = 200"
ev run2 "^EVT state_save file=.*/states/emerald.st0 size" >/dev/null || fail "run2: state_save path"
[ "$(md5 "$GAME/states/emerald.st0")" != "$M_ST" ] || fail "run2: state not rewritten"
[ ! -e "$GAME/roms/emerald.st0" ] || fail "run2: wrote beside the ROM"
M_ST2=$(md5 "$GAME/states/emerald.st0")
one_run run3 "rom = emerald.gba" "folders = 1" "load_state = 1" "autoexit_frames = 120"
ev run3 "^EVT autoload_state slot=1 rc=0" >/dev/null || fail "run3: load"
ev run3 "^EVT folders save_mig=0 state_mig=0" >/dev/null || fail "run3: a second move"
[ "$(md5 "$GAME/states/emerald.st0")" = "$M_ST2" ] || fail "run3: state changed by a load"
ok "C state saved to and loaded from states/; second open moves nothing"

# ------------------------------------------------------- D: blocked move
mkdir -p "$GAME/roms/d"
cp "$ROM" "$GAME/roms/d/blocked.gba"
cp "$SAV" "$GAME/roms/d/blocked.sav"
mkdir -p "$GAME/saves/d/blocked.sav"          # a folder where the save goes
M_B=$(md5 "$GAME/roms/d/blocked.sav")
one_run run4 "rom = d/blocked.gba" "folders = 1" "autoexit_frames = 120"
ev run4 "^EVT folders save_mig=-1 .*save=.*/roms/d/blocked.sav" >/dev/null \
  || fail "run4: $(ev run4 '^EVT folders ')"
[ "$(md5 "$GAME/roms/d/blocked.sav")" = "$M_B" ] || fail "run4: legacy save changed"
CRC4=$(ev run4 "^EVT sram_load" | sed -n 's/.*crc=\([0-9a-f]*\).*/\1/p')
[ "$CRC4" = "$CRC0" ] || fail "run4: legacy save not loaded ($CRC4)"
ok "D move blocked: legacy save kept, loaded (crc $CRC4) and unchanged"
rm -rf "$GAME/roms/d" "$GAME/saves/d" "$GAME/states/d"

# ------------------------------------------------------- E: console folders
mv "$GAME/roms/emerald.gba" "$GAME/roms/GBA/emerald.gba"
printf '%s\n' "30 dump b_gba" "60 press CROSS" > "$GAME/bscript_gba.txt"
one_run run5 "browser = 1" "ui_browser_script = bscript_gba.txt" "autoexit_frames = 120"
ev run5 "^EVT ui_browser_pick rom=GBA/emerald.gba" >/dev/null || fail "run5: pick"
ev run5 "^EVT folders save_mig=[01] state_mig=[01] save=.*/saves/GBA/emerald.sav state=.*/states/GBA/emerald.st0" \
  >/dev/null || fail "run5: $(ev run5 '^EVT folders ')"
ok "E1 roms/GBA/emerald.gba listed by the browser, booted, saves/GBA/emerald.sav"
if [ -n "$GB_ROM" ] && [ -f "$GB_ROM" ]; then
  cp "$GB_ROM" "$GAME/roms/GB/game.gb"
  printf '%s\n' "30 press TRIANGLE" "60 dump b_gb" "90 press CROSS" > "$GAME/bscript_gb.txt"
  one_run run6 "browser = 1" "ui_browser_script = bscript_gb.txt" "autoexit_frames = 240"
  ev run6 "^EVT ui_browser_pick rom=GB/game.gb" >/dev/null || fail "run6: pick $(ev run6 'ui_browser_')"
  ev run6 "^EVT folders .*save=.*/saves/GB/game.gb.sav state=.*/states/GB/game.gb.st0" \
    >/dev/null || fail "run6: $(ev run6 '^EVT folders ')"
  ok "E2 roms/GB/game.gb listed on the GB shelf, booted, saves/GB/game.gb.sav"
else
  echo "SKIP E2 (no GB_ROM)" | tee -a "$ART/verdict.txt"
fi

echo "FOLDERS TEST PASS -- artifacts in $ART"
exit 0
