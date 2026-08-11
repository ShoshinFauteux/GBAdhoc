#!/usr/bin/env bash
# run_video_regress.sh — frame-exact video regression oracle (ADR-0033).
#
# WHY: the PSP core's software scanline renderer (video.cc) is ~3 ms of a
# 16.75 ms frame and is the target of VFPU work.  A renderer optimisation that
# is subtly wrong is worse than no optimisation at all, so nothing gets
# optimised until a harness exists that catches a SINGLE wrong pixel.
#
# WHAT IT MEASURES: the CORE's output buffer, upstream of our GU blit — the
# exact pointer retro_video_refresh hands the frontend (fe_host_last_frame).
# The frontend hashes it per frame (FNV-1a over 240x160 halfwords) and emits
#   EVT vhash f=<frame> h=<hash>
# so a golden run is just the ordered list of those lines.  run_gu_color_test.sh
# checks the blit; this checks the renderer.  They are different bugs.
#
# The workload is testdata/fixtures/emerald_vregress.inputs: intro fades and
# affine, title, main menu, overworld scrolling, start menu, party screen.
# The boot leg is fast-forwarded; from `ff off` on, every frame is rendered.
#
# USAGE
#   run_video_regress.sh --capture      capture goldens (run this on the
#                                       UNMODIFIED core, before optimising)
#   run_video_regress.sh                compare against the goldens; any
#                                       differing frame is a FAIL
#   run_video_regress.sh --selftest     run twice and compare the two runs to
#                                       each other — proves the workload is
#                                       deterministic before goldens mean
#                                       anything
#   run_video_regress.sh --perturb=N    rebuild the core with a DELIBERATE
#                                       renderer fault (video.cc
#                                       VREGRESS_PERTURB: 1 = one wrong bit in
#                                       one pixel per frame, 2 = blend forgets
#                                       to saturate blue), compare, and assert
#                                       the comparison FAILS.  A harness that
#                                       has never failed is not evidence, so
#                                       this is the harness's own test.
#                                       Rebuilds the tree afterwards.
#
# Goldens live in tools/e2e/goldens/video_emerald.hashes — plain text, tracked in
# git (testdata/ is gitignored, and the golden IS the reference, not a fixture).
# Exit 0 = pass.  Prereqs: setup_ppsspp.sh ran once; psp/EBOOT.PBP built.
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
INPUTS="$REPO/testdata/fixtures/emerald_vregress.inputs"
GOLDEN_DIR="$SCRIPT_DIR/goldens"
GOLDEN="$GOLDEN_DIR/video_emerald.hashes"

MODE=compare
PERTURB=0
case "${1:-}" in
  --capture)  MODE=capture ;;
  --selftest) MODE=selftest ;;
  --perturb=*) MODE=perturb; PERTURB="${1#--perturb=}" ;;
  "")         ;;
  *) echo "usage: $0 [--capture|--selftest|--perturb=N]"; exit 2 ;;
esac

ART="$SCRIPT_DIR/artifacts/vregress-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"

fail() { echo "FAIL: $*" | tee -a "$ART/verdict.txt"; exit 1; }

[ -x "$PPSSPP_DIR/build/PPSSPPSDL" ] || fail "PPSSPPSDL not built (setup_ppsspp.sh)"
[ -f "$EBOOT" ]  || fail "psp/EBOOT.PBP missing"
[ -f "$ROM" ]    || fail "test ROM missing in testdata/"
[ -f "$BIOS" ]   || fail "gba_bios.bin missing in testdata/"
[ -f "$SAV" ]    || fail "test .sav missing in testdata/"
[ -f "$INPUTS" ] || fail "emerald_vregress.inputs missing"

MS="$SANDBOX_ROOT/inst1/ppsspp"
GAME="$MS/PSP/GAME/gpsp-adhoc"

# one_run <out-hashes> <tag>: fresh sandbox, boot, wait for clean exit, and
# extract the ordered vhash lines.
one_run() {
  local out="$1" tag="$2"
  "$SCRIPT_DIR/setup_ppsspp.sh" --instances 1 --skip-build >/dev/null \
    || fail "sandbox creation failed"
  mkdir -p "$GAME/roms" "$GAME/log"
  cp "$EBOOT"  "$GAME/EBOOT.PBP"
  cp "$BIOS"   "$GAME/gba_bios.bin"
  cp "$ROM"    "$GAME/roms/emerald.gba"
  cp "$SAV"    "$GAME/roms/emerald.sav"
  cp "$INPUTS" "$GAME/run.inputs"
  cat > "$GAME/autopilot.ini" <<EOF
script = run.inputs
vhash = 1
autoexit_frames = 30000
EOF

  rm -f /dev/shm/PPSSPP_ID
  local EVTLOG="$GAME/log/frontend.log"
  ( cd "$PPSSPP_DIR/build" && \
    XDG_CONFIG_HOME="$SANDBOX_ROOT/inst1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    timeout $((TIMEOUT_S + 30)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
      ./PPSSPPSDL --windowed "$GAME/EBOOT.PBP" >"$ART/emu-$tag.log" 2>&1 ) &
  local EMU_PID=$!
  local DONE=0
  for _ in $(seq 1 "$TIMEOUT_S"); do
    if grep -q "^EVT exit code=" "$EVTLOG" 2>/dev/null; then DONE=1; break; fi
    kill -0 "$EMU_PID" 2>/dev/null || break
    sleep 1
  done
  sleep 1
  kill "$EMU_PID" 2>/dev/null || true
  wait "$EMU_PID" 2>/dev/null || true
  cp "$EVTLOG" "$ART/frontend-$tag.log" 2>/dev/null || true
  [ "$DONE" -eq 1 ] || fail "$tag: timed out waiting for EVT exit"

  grep -q "^EVT exit code=0" "$ART/frontend-$tag.log" \
    || fail "$tag: run did not exit cleanly (script failed?)"
  # The script must have reached every marker, or the run covered less than
  # the goldens do and a 'pass' would be meaningless.
  local m
  for m in title_ok mainmenu_ok vregress_overworld vregress_menu \
           vregress_party vregress_done; do
    grep -q "^EVT ap_mark text=$m" "$ART/frontend-$tag.log" \
      || fail "$tag: workload never reached marker '$m'"
  done

  grep "^EVT vhash " "$ART/frontend-$tag.log" | sed 's/^EVT vhash //' > "$out"
  local n; n=$(wc -l < "$out")
  [ "$n" -gt 500 ] || fail "$tag: only $n hashed frames — instrumentation off?"
  echo "  $tag: $n hashed frames"
}

# rebuild <extra-cflags>: core then frontend, in that order (the frontend links
# the core archive, so the core must be built first).
rebuild() {
  local extra="$1"
  ( cd "$REPO" && \
    docker run --rm -v "$PWD":/build -w /build pspdev/pspdev \
      sh -c "make platform=psp1 EXTRA_CFLAGS='$extra' -j8" >"$ART/build-core.log" 2>&1 && \
    docker run --rm -v "$PWD":/build -w /build/psp pspdev/pspdev \
      make -j8 >"$ART/build-psp.log" 2>&1 ) \
    || fail "rebuild with EXTRA_CFLAGS='$extra' failed (see $ART/build-*.log)"
}

case "$MODE" in
perturb)
  [ -f "$GOLDEN" ] || fail "no goldens at $GOLDEN — capture them first"
  echo "rebuilding video.cc with VREGRESS_PERTURB=$PERTURB ..."
  # touch video.cc so the -D actually reaches the object that matters
  touch "$REPO/video.cc"
  rebuild "-DVREGRESS_PERTURB=$PERTURB"
  one_run "$ART/run-perturb.hashes" perturb
  NDIFF=$(diff "$GOLDEN" "$ART/run-perturb.hashes" | grep -c '^<')
  echo "touching video.cc and rebuilding clean ..."
  touch "$REPO/video.cc"
  rebuild ""
  if [ "$NDIFF" -gt 0 ]; then
    echo "ORACLE PROOF PASS — perturbation $PERTURB changed $NDIFF of" \
         "$(wc -l < "$GOLDEN") frame hashes; the harness detects it" \
         | tee "$ART/verdict.txt"
    exit 0
  fi
  fail "ORACLE IS BLIND: perturbation $PERTURB produced identical hashes"
  ;;
capture)
  one_run "$ART/run-a.hashes" capture
  mkdir -p "$GOLDEN_DIR"
  cp "$ART/run-a.hashes" "$GOLDEN"
  echo "GOLDENS CAPTURED -> $GOLDEN ($(wc -l < "$GOLDEN") frames)"
  exit 0
  ;;
selftest)
  one_run "$ART/run-a.hashes" a
  one_run "$ART/run-b.hashes" b
  if diff -q "$ART/run-a.hashes" "$ART/run-b.hashes" >/dev/null; then
    echo "SELFTEST PASS — two runs are frame-identical (workload deterministic)"
    exit 0
  fi
  diff "$ART/run-a.hashes" "$ART/run-b.hashes" | head -20 | tee "$ART/verdict.txt"
  fail "two runs of the SAME build disagree — workload is not deterministic"
  ;;
compare)
  [ -f "$GOLDEN" ] || fail "no goldens at $GOLDEN (run --capture on a known-good build)"
  one_run "$ART/run-a.hashes" cmp
  if diff -q "$GOLDEN" "$ART/run-a.hashes" >/dev/null; then
    echo "VIDEO REGRESS PASS — $(wc -l < "$GOLDEN") frames pixel-identical to golden"
    exit 0
  fi
  {
    echo "first differing frames (golden vs run):"
    diff "$GOLDEN" "$ART/run-a.hashes" | head -30
    echo
    echo "differing frame count: $(diff "$GOLDEN" "$ART/run-a.hashes" | grep -c '^<')"
  } | tee "$ART/verdict.txt"
  fail "renderer output differs from golden — see $ART"
  ;;
esac
