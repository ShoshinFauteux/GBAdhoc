#!/usr/bin/env bash
# rewind_all.sh BUILD [PLAY] -- THE standing rewind test: every ROM of the
# 36-ROM corpus and every GBA ROM of the owner's library, on the desktop twin
# ~/rewind-agent/BUILD (tools/rewind_twin.sh BUILD), three runs per game:
#
#   test   the scripted rewinds at frame PLAY (default 4000): 300, 45, 7, 1
#          frames back and once past the start of the history, replaying
#          with the same inputs
#   boot   the same after 250 frames (the rewinds reach power-on)
#   fuzz   the tap storm (seeds 1 and 2, 6000 ticks each) against a run that
#          never rewound
#
# and prints one row per game and a count.  A row passes when every picture
# and sound shown backwards is the forward frame's, every replayed frame
# has the reference's picture, sound, input and save, and every resume
# lands on the save of its frame.  The columns:
#
#   back    pictures shown backwards / wrong
#   fwd     forward frames compared with the reference / wrong
#   rerun   frames re-run for a rewind / machine or save wrong
#   timing  frames whose machine state differed while picture, sound and
#           save did not (rewind_check.py --allow-timing; docs/REWIND.md
#           16.3): counted, not failed
#
# WSL.  Env: RW_JOBS (parallel jobs, default 12), RW_LIB (the library
# directory; read only, nothing is written beside a ROM), RW_ONLY (a grep
# pattern on the ROM name), RW_EXTRA (more emulator options, e.g.
# "--option gpsp_drc=disabled").  Output: ~/rewind-agent/all_BUILD/table.txt
# and one log per game and run.  Exit status 0 = every row passed.
set -uo pipefail
BUILD=${1:?build}; PLAY=${2:-4000}
W=$HOME/rewind-agent; O=$W/all_$BUILD; mkdir -p "$O/tools"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/workdirs.sh"
SRC=${RW_SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}   # the gpsp tree to copy
LIB=${RW_LIB:-$GBADHOC_ROMS}
cp "$SRC"/tools/rewind_*.py "$O/tools/" && sed -i 's/\r$//' "$O"/tools/*.py
ALL="--rewind-inplace --rewind-dirty 3 --rewind-fast-restore --rewind-clock --rewind-burst 4 --rewind-verify --rtc-pin 1700000000 ${RW_EXTRA:-}"
IN=$W/work/in/mash.inputs
one() {
  rom=$1; n=$(basename "$rom" .gba | tr -c 'A-Za-z0-9_.\n-' '_' | cut -c1-40)
  case "$rom" in /mnt/*) n="L_$n";; *) n="C_$n";; esac
  d="$O/$n"; rm -rf "$d"; mkdir -p "$d"
  T="python3 $O/tools/rewind_test.py"
  C="--bin $W/$BUILD/sdl/gpsp_sdl --bios $W/bios --inputs $IN --allow-cut 16 --allow-timing -1"
  timeout 1800 $T test $C --rom "$rom" --play $PLAY --work "$d/t" -- $ALL > "$d/test.log" 2>&1
  timeout 1800 $T test $C --min-redo 100 --rom "$rom" --play 250 --work "$d/b" -- $ALL > "$d/boot.log" 2>&1
  timeout 3600 $T fuzz $C --rom "$rom" --play $PLAY --ticks 6000 --seeds 1,2 --work "$d/f" -- $ALL > "$d/fuzz.log" 2>&1
  python3 - "$n" "$d" <<'PY'
import re, sys
n, d = sys.argv[1:3]
def num(pat, text):
    return sum(int(x) for x in re.findall(pat, text))
tot = dict(back=0, backbad=0, fwd=0, fwdbad=0, rr=0, rrbad=0, tim=0)
v = []
for run in ("test", "boot", "fuzz"):
    try:
        t = open("%s/%s.log" % (d, run), errors="replace").read()
    except OSError:
        t = ""
    ok = "PASS" in t and "FAIL" not in t and "Traceback" not in t
    v.append("ok" if ok else "FAIL")
    tot["back"] += num(r"shown_backwards=(\d+)", t); tot["backbad"] += num(r"shown_backwards=\d+ \(bad (\d+)\)", t)
    tot["fwd"] += num(r"compared=(\d+)", t); tot["fwdbad"] += num(r"compared=\d+ \(bad (\d+)\)", t)
    tot["rr"] += num(r"rerun_for_rewind=(\d+)", t); tot["rrbad"] += num(r"rerun_for_rewind=\d+ \(bad (\d+)\)", t)
    tot["tim"] += num(r"timing_only=(\d+)", t)
print("%-42s test=%-4s boot=%-4s fuzz=%-4s back=%d/%d fwd=%d/%d rerun=%d/%d timing=%d %s" % (
    n, v[0], v[1], v[2], tot["back"], tot["backbad"], tot["fwd"], tot["fwdbad"],
    tot["rr"], tot["rrbad"], tot["tim"], "PASS" if v == ["ok"] * 3 else "FAIL"))
PY
  rm -rf "$d/t" "$d/b" "$d/f"
}
export -f one; export W O BUILD PLAY ALL IN
{ ls "$HOME"/drcorpus/corpus/roms/*.gba; ls "$LIB"/*.gba 2>/dev/null; } |
  { [ -n "${RW_ONLY:-}" ] && grep -i -- "$RW_ONLY" || cat; } | tr '\n' '\0' |
  xargs -0 -P "${RW_JOBS:-12}" -I{} bash -c 'one "$1"' _ {} | sort > "$O/rows.txt"
# (with RW_ONLY the rows replace those games' rows in the table)
{ [ -n "${RW_ONLY:-}" ] && [ -f "$O/table.txt" ] && grep -v -F -f <(awk '{print $1" "}' "$O/rows.txt") "$O/table.txt"; cat "$O/rows.txt"; } | sort > "$O/table.new"
mv "$O/table.new" "$O/table.txt"
cat "$O/table.txt"
p=$(grep -c ' PASS$' "$O/table.txt"); f=$(grep -c ' FAIL$' "$O/table.txt")
echo "rewind_all $BUILD: $p pass, $f fail of $((p + f)); timing-only frames $(awk '{for(i=1;i<=NF;i++) if ($i ~ /^timing=/) {split($i,a,"="); s+=a[2]}} END {print s+0}' "$O/table.txt")"
[ "$f" = 0 ] && [ "$p" -gt 0 ]
