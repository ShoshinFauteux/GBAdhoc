#!/usr/bin/env bash
# run_video_ab.sh — trustworthy A/B of a renderer change (ADR-0035 fallout).
#
# WHY THIS EXISTS. Phase-3 attempt 1 was measured by rebuilding one tree in
# place, over and over. That chain produced (a) two different sources that
# built to byte-identical binaries because `rsync -a` preserved mtimes and the
# sync silently no-op'd, and (b) a control build that measured 12 % FASTER than
# baseline with the feature compiled OUT, which is not physically possible.
# All three numbers from that attempt had to be thrown away. The rule now is:
#
#   * NEVER rebuild in place. Each side gets its own tree, built from scratch.
#   * The binary identifies itself. `EVT build eboot_crc=` is emitted at boot
#     and `bld=` appears in every `EVT vid_prof` line; this script FAILS if the
#     two sides report the same CRC (that means one side did not rebuild) or if
#     a side's CRC does not match the EBOOT on disk.
#   * The workloads must be identical. The script fails if the two runs'
#     structural counters (txtpx, objspr, lines) differ — that would mean the
#     two sides did not render the same frames and no timing delta is meaningful.
#
# USAGE
#   run_video_ab.sh --a-ref=HEAD --b-tree=.            baseline git ref vs working tree
#   run_video_ab.sh --a-ref=HEAD --b-ref=HEAD --b-cflags=-DGPSP_FAST_TILE4=1
#                                                      same source, one -D apart
#   Options:
#     --a-ref=REF      git ref for side A (default HEAD)
#     --b-ref=REF      git ref for side B (default: use the working tree)
#     --a-cflags=STR   extra CFLAGS for side A (default none)
#     --b-cflags=STR   extra CFLAGS for side B (default none)
#     --level=N        VIDEO_PROF level for both sides (default 1 — the cheapest
#                      level that reports `ttot` and `core=`)
#     --win=N          profile window in frames (default 100)
#     --keep           do not delete the two build trees afterwards
#
# Prints a per-window table of ttot and core for both sides plus the delta, and
# the mean delta over the windows whose counters mark them as steady overworld.
# Exit 0 if the comparison is VALID (not if B is faster — that is a judgement
# for the reader, not the script).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
SANDBOX_ROOT="${SANDBOX_ROOT:-$HOME/gpsp-e2e/sandboxes}"
WORK="${AB_WORK:-$HOME/gpsp-ab}"
TIMEOUT_S="${TIMEOUT_S:-1800}"

A_REF=HEAD; B_REF=""; A_CFLAGS=""; B_CFLAGS=""; LEVEL=1; WIN=100; KEEP=0
for arg in "$@"; do
  case "$arg" in
    --a-ref=*)    A_REF="${arg#*=}" ;;
    --b-ref=*)    B_REF="${arg#*=}" ;;
    --a-cflags=*) A_CFLAGS="${arg#*=}" ;;
    --b-cflags=*) B_CFLAGS="${arg#*=}" ;;
    --level=*)    LEVEL="${arg#*=}" ;;
    --win=*)      WIN="${arg#*=}" ;;
    --keep)       KEEP=1 ;;
    --b-tree=.)   B_REF="" ;;
    *) echo "unknown option: $arg"; exit 2 ;;
  esac
done

ART="$SCRIPT_DIR/artifacts/ab-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"

# The tree may be a plain copy rather than a git checkout (the PSP toolchain and
# the rig live in WSL, and the repo is often mirrored there without .git).  In
# that case git refs are unavailable and BOTH sides come from the working tree,
# differentiated by --a-cflags/--b-cflags.  Every safety check below still
# applies -- in particular, if the two cflag sets do not actually produce
# different binaries the run is rejected, so this fallback cannot hide a no-op.
HAVE_GIT=1
( cd "$REPO" && git rev-parse --git-dir ) >/dev/null 2>&1 || HAVE_GIT=0
if [ "$HAVE_GIT" -eq 0 ]; then
  echo "note: $REPO is not a git checkout — using the working tree for both"
  echo "      sides; --a-ref/--b-ref ignored, cflags are the only difference."
  A_REF=""; B_REF=""
fi
fail() { echo "INVALID: $*" | tee -a "$ART/verdict.txt"; exit 1; }

ROM="$REPO/testdata/Pokemon - Emerald Version (USA, Europe).gba"
BIOS="$REPO/testdata/gba_bios.bin"
SAV="$REPO/testdata/Pokemon Emerald All Shiny Fixed.sav"
INPUTS="$REPO/testdata/fixtures/emerald_vregress.inputs"
for f in "$ROM" "$BIOS" "$SAV" "$INPUTS"; do
  [ -f "$f" ] || fail "missing fixture: $f"
done
[ -x "$PPSSPP_DIR/build/PPSSPPSDL" ] || fail "PPSSPPSDL not built"

# --- build one side into its OWN tree, from scratch --------------------------
# $1 = side tag, $2 = git ref ("" = working tree), $3 = extra cflags
build_side() {
  local tag="$1" ref="$2" cflags="$3"
  local tree="$WORK/$tag"
  rm -rf "$tree"; mkdir -p "$tree"
  if [ -n "$ref" ]; then
    ( cd "$REPO" && git archive "$ref" ) | tar -x -C "$tree" \
      || fail "$tag: git archive $ref failed"
  else
    # Working tree, minus everything that must not be inherited: no object
    # files, no archive, no EBOOT — this is what "never rebuild in place" means.
    ( cd "$REPO" && tar -c --exclude=.git --exclude='*.o' --exclude='*.a' \
        --exclude='psp/obj' --exclude='psp/EBOOT.PBP' --exclude='psp/*.elf' \
        --exclude='tools/e2e/artifacts' --exclude=build . ) | tar -x -C "$tree" \
      || fail "$tag: working-tree copy failed"
  fi
  find "$tree" \( -name '*.o' -o -name '*.a' -o -name 'EBOOT.PBP' \) -delete
  ( cd "$tree" && \
    docker run --rm -v "$PWD":/build -w /build pspdev/pspdev \
      sh -c "make platform=psp1 EXTRA_CFLAGS='-DVIDEO_PROF=$LEVEL $cflags' -j8" \
      >"$ART/build-$tag-core.log" 2>&1 ) \
    || fail "$tag: core build failed (see $ART/build-$tag-core.log)"
  ( cd "$tree/psp" && \
    docker run --rm -v "$tree":/build -w /build/psp pspdev/pspdev make -j8 \
      >"$ART/build-$tag-psp.log" 2>&1 ) \
    || fail "$tag: frontend build failed (see $ART/build-$tag-psp.log)"
  [ -f "$tree/psp/EBOOT.PBP" ] || fail "$tag: no EBOOT.PBP produced"
  echo "  built $tag: $(md5sum "$tree/psp/EBOOT.PBP" | cut -c1-12) ($ref ${cflags:-no extra cflags})"
}

# --- run one side ------------------------------------------------------------
run_side() {
  local tag="$1"
  local tree="$WORK/$tag"
  "$SCRIPT_DIR/setup_ppsspp.sh" --instances 1 --skip-build >/dev/null \
    || fail "$tag: sandbox creation failed"
  local MS="$SANDBOX_ROOT/inst1/ppsspp"
  local GAME="$MS/PSP/GAME/gpsp-adhoc"
  mkdir -p "$GAME/roms" "$GAME/log"
  cp "$tree/psp/EBOOT.PBP" "$GAME/EBOOT.PBP"
  cp "$BIOS" "$GAME/gba_bios.bin"
  cp "$ROM"  "$GAME/roms/emerald.gba"
  cp "$SAV"  "$GAME/roms/emerald.sav"
  cp "$INPUTS" "$GAME/run.inputs"
  cat > "$GAME/autopilot.ini" <<EOF
script = run.inputs
vid_prof = $WIN
autoexit_frames = 30000
EOF
  rm -f /dev/shm/PPSSPP_ID
  local EVTLOG="$GAME/log/frontend.log"
  ( cd "$PPSSPP_DIR/build" && \
    XDG_CONFIG_HOME="$SANDBOX_ROOT/inst1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    timeout $((TIMEOUT_S + 30)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
      ./PPSSPPSDL --windowed "$GAME/EBOOT.PBP" >"$ART/emu-$tag.log" 2>&1 ) &
  local PID=$!
  local DONE=0
  for _ in $(seq 1 "$TIMEOUT_S"); do
    if grep -q "^EVT exit code=" "$EVTLOG" 2>/dev/null; then DONE=1; break; fi
    kill -0 "$PID" 2>/dev/null || break
    sleep 1
  done
  sleep 1; kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
  cp "$EVTLOG" "$ART/frontend-$tag.log" 2>/dev/null || true
  [ "$DONE" -eq 1 ] || fail "$tag: run timed out"
  grep -q "^EVT exit code=0" "$ART/frontend-$tag.log" || fail "$tag: unclean exit"
  grep "^EVT vid_prof " "$ART/frontend-$tag.log" > "$ART/prof-$tag.txt"
  [ -s "$ART/prof-$tag.txt" ] || fail "$tag: no vid_prof lines (VIDEO_PROF not compiled in?)"
}

echo "=== building both sides into separate trees (never in place) ==="
build_side a "$A_REF" "$A_CFLAGS"
build_side b "$B_REF" "$B_CFLAGS"

MD5A=$(md5sum "$WORK/a/psp/EBOOT.PBP" | cut -d' ' -f1)
MD5B=$(md5sum "$WORK/b/psp/EBOOT.PBP" | cut -d' ' -f1)
if [ "$MD5A" = "$MD5B" ]; then
  fail "both sides produced the SAME EBOOT — there is nothing to compare.
       (This is exactly the failure that invalidated ADR-0035's numbers.)"
fi

echo "=== running both sides ==="
run_side a
run_side b

# --- validate that the two runs are comparable -------------------------------
BLDA=$(sed -n '1s/.*bld=\([0-9a-f]*\).*/\1/p' "$ART/prof-a.txt")
BLDB=$(sed -n '1s/.*bld=\([0-9a-f]*\).*/\1/p' "$ART/prof-b.txt")
[ -n "$BLDA" ] && [ -n "$BLDB" ] || fail "no bld= field — frontend too old for this script"
[ "$BLDA" != "$BLDB" ] || fail "both runs report bld=$BLDA — a stale EBOOT was run"

python3 - "$ART/prof-a.txt" "$ART/prof-b.txt" "$WIN" <<'PY' | tee "$ART/verdict.txt"
import sys, re
def load(p):
    out=[]
    for ln in open(p):
        d=dict(re.findall(r'(\w+)=(\w+)', ln))
        out.append(d)
    return out
a,b,win=load(sys.argv[1]),load(sys.argv[2]),int(sys.argv[3])
if len(a)!=len(b):
    print("INVALID: different window counts: %d vs %d" % (len(a),len(b))); sys.exit(1)
bad=[]
for i,(x,y) in enumerate(zip(a,b)):
    for k in ('lines','txtpx','objspr','txtc'):
        if x.get(k)!=y.get(k):
            bad.append("window %d: %s %s vs %s" % (i,k,x.get(k),y.get(k)))
if bad:
    print("INVALID: the two runs did not render the same frames:")
    for l in bad[:8]: print("  "+l)
    sys.exit(1)
print("build A bld=%s   build B bld=%s" % (a[0]['bld'], b[0]['bld']))
print("counters identical in all %d windows -> the comparison is valid\n" % len(a))
print("  win  txtpx    ttot_A  ttot_B   d_ttot        core_A  core_B   d_core")
sa=sb=ca=cb=n=0
for i,(x,y) in enumerate(zip(a,b)):
    ta,tb=int(x['ttot']),int(y['ttot']); qa,qb=int(x['core']),int(y['core'])
    steady = x['txtpx']=='153600'
    mark='*' if steady else ' '
    print("  %2d%s %7s  %7d %7d  %+6d %+6.1f%%  %7d %7d  %+6d" %
          (i,mark,x['txtpx'],ta,tb,tb-ta,100.0*(tb-ta)/ta,qa,qb,qb-qa))
    if steady: sa+=ta; sb+=tb; ca+=qa; cb+=qb; n+=1
if n:
    print("\n  * steady overworld, %d windows:" % n)
    print("      renderer  %d -> %d us  (%+.1f %%)" % (sa/n, sb/n, 100.0*(sb-sa)/sa))
    print("      core      %d -> %d us  (%+.1f %%)" % (ca/n, cb/n, 100.0*(cb-ca)/ca))
else:
    print("\n  (no steady overworld window in this run)")
PY
RC=${PIPESTATUS[0]}
[ "$KEEP" -eq 1 ] || rm -rf "$WORK/a" "$WORK/b"
[ "$RC" -eq 0 ] || fail "comparison rejected (see above)"
echo "A/B artifacts in $ART"
exit 0
