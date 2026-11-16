#!/usr/bin/env bash
# p0a_b5.sh -- the P0a repairs (PLAN-P-beta5 §3 P0a) proven on ONE tree, each with a
# mutant that must be caught.  run_all.sh keeps the 01067e74 self-tests; this is the
# "b5 row set" it runs on the other trees (7ffe4236, fbe9da63, and later the P0c tree).
#
#   p0a_b5.sh --tree GPSP_DIR --run RUN_DIR [--cutlist CUTLIST] [--out DIR] [--only a,b]
#     GPSP_DIR  a CLEAN source tree of the commit RUN_DIR was built from (git archive)
#     RUN_DIR   flavours.sh --baseline output of that commit (release, lab, harness;
#               release-stable went with RFU_SCHED in code-health C-4), built with the
#               P0a flavours.sh
#     CUTLIST   the cutlist describing GPSP_DIR's main_psp.c (P4 simulation; T9 owners)
#     RUN2      (env) a second run of the same commit, for the --against case
#     TWIN      (env) a run of a DIFFERENT commit with the same gpsp/ tree, built with
#               the same --build-version/--sde pins (G1 two-commit case)
#     P4_RUN    (env) where the real P4 build lives (default OUT/p4run); reused when it
#               holds the P4 commit already, else built (LOCK=FILE serialises it)
#   Steps (--only): loop t10 t14 t6 xs_raw xs_e t9 cf st t13 pp flav runs t11split p4.
#   pp/flav/t11split/p4 carry the adversarial review's mutants (2026-11-07): the pair
#   arms (MA, MB), the build.sh rewrite prune, missing/leaky logs, another TU, a header,
#   step scope (DS/BF), the --against contract field by field, ci_greps on the split
#   (logical files, KNOWN keys), and the census/loop numbers on the real P4.
#   RUN must be a hermetic flavours.sh run (2026-11-07) for flav's contract cases and p4.
#   Needs WSL: docker (pspdev/pspdev, DOCKER_IMAGE pins it), python3, git.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); R=$(cd "$HERE/.." && pwd)
TREE= RUN= CUT= OUT= ONLY=
while [ $# -gt 0 ]; do
  case "$1" in
    --tree) TREE=$2; shift 2 ;; --run) RUN=$2; shift 2 ;; --cutlist) CUT=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;; --only) ONLY=$2; shift 2 ;;
    *) echo "p0a_b5: unknown arg $1"; exit 2 ;;
  esac
done
[ -d "$TREE/psp" ] && [ -f "$RUN/release/gpsp_adhoc.elf" ] || { echo "p0a_b5: --tree and --run are required"; exit 2; }
OUT=${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/p0a_b5.XXXXXX")}; mkdir -p "$OUT"
PY=python3
export AEC_DOCKER_IMAGE=${DOCKER_IMAGE:-pspdev/pspdev}
M="$TREE/psp/main_psp.c"
FAILS=0; STEPS=()
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
want() { [ -z "$ONLY" ] || [[ ",$ONLY," == *",$1,"* ]]; }
expect_rc() {  # expect_rc <want> <label> <cmd...>
  local w=$1 l=$2; shift 2
  "$@" > "$OUT/last.log" 2>&1; local rc=$?
  if [ "$rc" = "$w" ]; then ok "$l (exit $rc)"; else bad "$l: exit $rc, wanted $w"; tail -6 "$OUT/last.log" | sed 's/^/        /'; fi
}
COMMIT=$(sed -nE 's/.*"sourceCommit": *"([0-9a-f]+)".*/\1/p' "$RUN/release/flavour.json")
echo "p0a_b5: tree $TREE  run $RUN (commit ${COMMIT:0:10})  cutlist ${CUT:-none}  out $OUT"

# ---- T1b/T10/T16: the loop window by anchor; the old hard-coded window fails loudly
if want loop; then
  echo "== loop window (loop_anchor; loop_diff T1b, layout_report T10, loop_lpsim T16)"
  expect_rc 0 "loop_anchor selftest (01067e74 16368-18089 = x4's 16368-18090; 7ffe4236; fbe9da63; mutants)" $PY "$R/loop_anchor.py" --selftest
  for fl in release lab; do
    E="$RUN/$fl/gpsp_adhoc.elf"
    $PY "$R/loop_anchor.py" --elf "$E" --main-c "$M" | sed "s/^/  [$fl] /"
    expect_rc 0 "[$fl] loop_diff by anchor" $PY "$R/loop_diff.py" "$E"
    grep -E "^loop window by anchor|by line|by address" "$OUT/last.log" | sed 's/^/        /'
    expect_rc 0 "[$fl] layout_report by anchor" $PY "$R/layout_report.py" "$RUN/$fl" --src "$TREE" --no-pad-check
    grep -E "main loop by line" "$OUT/last.log" | sed 's/^/        /'
    expect_rc 0 "[$fl] loop_lpsim window by anchor" $PY "$R/loop_lpsim/loop_lpsim.py" window "$E"
    if ! $PY "$R/loop_anchor.py" "$M" | grep -q "lines 16368-180"; then   # not 01067e74 (there the old window IS the anchored code lines)
      expect_rc 2 "[$fl] loop_diff --loop-lines 16368-18090 (the old window) refused" $PY "$R/loop_diff.py" "$E" --loop-lines 16368-18090
      $PY "$R/layout_report.py" "$RUN/$fl" --src "$TREE" --no-pad-check --loop-lines 16368-18090 > "$OUT/last.log" 2>&1
      grep -q "explicit loop lines 16368-18090 are not the anchored window" "$OUT/last.log" \
        && ok "[$fl] layout_report --loop-lines 16368-18090 fails loudly" || bad "[$fl] layout_report accepted the old window"
      $PY "$R/loop_lpsim/loop_lpsim.py" window "$E" --src 16368:18090 > "$OUT/last.log" 2>&1
      grep -q "explicit loop lines 16368-18090 are not the anchored window" "$OUT/last.log" \
        && ok "[$fl] loop_lpsim --src 16368:18090 fails loudly" || bad "[$fl] loop_lpsim accepted the old window"
    fi
    # a source that is not the ELF's is refused (decl_line check)
    sed '1i /* shifted */' "$M" > "$OUT/shifted_main_psp.c"
    expect_rc 2 "[$fl] a source one line off the ELF's (main's DW_AT_decl_line) is refused" $PY "$R/loop_anchor.py" --elf "$E" --main-c "$OUT/shifted_main_psp.c"
  done
  # P-tidy F3: T10 --src/--ref-src and T16 --tree/--tree-b are the trees the window is read from
  expect_rc 0 "T10/T16 read the loop window from the tree given (--src/--ref-src, --tree/--tree-b; 2 tool mutants)" \
    $PY -I "$HERE/p0a_test_src_tree.py" "$RUN" "$TREE"
  grep -cE "^  ok  " "$OUT/last.log" | sed 's/^/        ok rows: /'
fi

# ---- T10 layout_report: every LAYOUT_PAD_*BYTES discovered (no fixed list), each pad
# measured in the ELF/map, and tools/layout/layout_lock.py check called (P0a T10)
if want t10; then
  echo "== T10 layout_report pads + layout.lock"
  npad=$(grep -oE '^LAYOUT_PAD_[A-Z0-9_]*BYTES *\?=' "$TREE/psp/Makefile" | sed 's/ *?=//' | sort -u | wc -l)
  HAVE_LOCK=0; [ -f "$TREE/tools/layout/layout.lock" ] && HAVE_LOCK=1
  for fl in release lab harness; do
    [ -f "$RUN/$fl/gpsp_adhoc.elf" ] || continue
    expect_rc 0 "[$fl] layout_report: pads discovered and measured, lock as the flavour requires" \
      $PY "$R/layout_report.py" "$RUN/$fl" --src "$TREE" --no-pad-check --json "$OUT/t10_$fl.json"
    cp "$OUT/last.log" "$OUT/t10_$fl.log"
    got=$(sed -nE 's/^-- pads \(discovered.*\): ([0-9]+)$/\1/p' "$OUT/t10_$fl.log")
    [ "$got" = "$npad" ] && ok "[$fl] $got pads reported = the $npad LAYOUT_PAD_*BYTES of psp/Makefile" \
      || bad "[$fl] $got pads reported, psp/Makefile declares $npad"
    grep -E "^  (ok|MISMATCH|MISSING|UNDECLARED) " "$OUT/t10_$fl.log" | sed 's/^/      /' | cut -c1-120
    if [ $HAVE_LOCK = 1 ]; then
      case $fl in
        release|lab) grep -q "^  layout lock: ok" "$OUT/t10_$fl.log" && ok "[$fl] layout_lock check $fl: ok (enforced)" \
                       || bad "[$fl] layout_lock check did not report ok" ;;
        harness) grep -q "no layout.lock section for flavour harness" "$OUT/t10_$fl.log" && ok "[$fl] no lock section: not checked" \
                       || bad "[$fl] harness lock handling" ;;
      esac
    fi
  done
  # fbe9da63: PLAN-P-beta5 §3 P0a T10's seven pads, release and Lab
  if [ "${COMMIT:0:8}" = fbe9da63 ]; then
    for spec in "release:LAYOUT_PAD_BYTES=1632 LAYOUT_PAD_RO_BYTES=1488 LAYOUT_PAD_FE_BYTES=2792 LAYOUT_PAD_CORE_BYTES=6348 LAYOUT_PAD_GB_BYTES=1040 LAYOUT_PAD_BSCAN_BYTES=7704 LAYOUT_PAD_MAIN_BYTES=7980" \
                "lab:LAYOUT_PAD_BYTES=708 LAYOUT_PAD_RO_BYTES=1320 LAYOUT_PAD_FE_BYTES=2668 LAYOUT_PAD_CORE_BYTES=4908 LAYOUT_PAD_GB_BYTES=1040 LAYOUT_PAD_BSCAN_BYTES=7928 LAYOUT_PAD_MAIN_BYTES=8016"; do
      fl=${spec%%:*}; miss=""
      for kv in ${spec#*:}; do
        grep -qE "^  ok +${kv%%=*} +${kv#*=} " "$OUT/t10_$fl.log" || miss="$miss $kv"
      done
      [ -z "$miss" ] && ok "[$fl] fbe9da63's seven pads: ${spec#*:}" | cut -c1-200 || bad "[$fl] pads not as the plan records:$miss"
    done
  fi
  # mutants on a copy of the files the report reads (psp/Makefile, lab-pads.env, the lock)
  PT="$OUT/padsrc"
  mkpt() { rm -rf "$PT"; mkdir -p "$PT/psp" "$PT/tools/layout"; cp "$TREE/psp/Makefile" "$PT/psp/"
           for f in lab-pads.env layout.lock layout_lock.py; do [ -f "$TREE/tools/layout/$f" ] && cp "$TREE/tools/layout/$f" "$PT/tools/layout/"; done; }
  LR() { $PY "$R/layout_report.py" "$RUN/release" --src "$PT" --no-pad-check "$@"; }
  mkpt; printf '\nLAYOUT_PAD_ZZ_BYTES ?= 64\n' >> "$PT/psp/Makefile"
  LR > "$OUT/last.log" 2>&1; rc=$?
  [ $rc = 1 ] && grep -qE "^  MISSING +LAYOUT_PAD_ZZ_BYTES" "$OUT/last.log" \
    && ok "M-T10a a new LAYOUT_PAD_ZZ_BYTES in psp/Makefile is discovered (no fixed list) and has no pad: caught" \
    || bad "M-T10a new pad variable not discovered (rc $rc)"
  v=$(grep -oE '^LAYOUT_PAD_[A-Z0-9_]+_BYTES *\?= *[1-9][0-9]*' "$TREE/psp/Makefile" | grep -v '^LAYOUT_PAD_RO_' | head -1)
  if [ -n "$v" ]; then
    var=${v%% *}; val=${v##* }
    mkpt; sed -i -E "s/^($var *\?= *)$val\b/\1$((val + 16))/" "$PT/psp/Makefile"
    LR > "$OUT/last.log" 2>&1; rc=$?
    [ $rc = 1 ] && grep -qE "^  MISMATCH +$var +$((val + 16)) " "$OUT/last.log" \
      && ok "M-T10b $var $val -> $((val + 16)) in psp/Makefile: the ELF pad size disagrees: caught" \
      || bad "M-T10b $var value change not caught (rc $rc)"
    mkpt; sed -i -E "/^$var *\?=/d" "$PT/psp/Makefile"
    LR > "$OUT/last.log" 2>&1; rc=$?
    [ $rc = 1 ] && grep -q "UNDECLARED pad" "$OUT/last.log" \
      && ok "M-T10c $var removed from psp/Makefile: its ELF pad is UNDECLARED: caught" \
      || bad "M-T10c undeclared pad not caught (rc $rc)"
  else echo "        (no sized named pad variable in this tree: M-T10b/c not applicable)"; fi
  if [ $HAVE_LOCK = 1 ]; then
    mkpt; sed -i -E '0,/^at   main +0x[0-9a-f]+/s//at   main                               0x0000/' "$PT/tools/layout/layout.lock"
    LR > "$OUT/last.log" 2>&1; rc=$?
    [ $rc = 1 ] && grep -q "LAYOUT LOCK FAIL (\[release\]" "$OUT/last.log" \
      && ok "M-T10d [release] at main moved in a lock copy: the layout_lock check fails the report" \
      || bad "M-T10d lock mutant not caught (rc $rc)"
  else echo "        (no tools/layout/layout.lock in this tree: lock not checked, M-T10d not applicable)"; fi
fi

# ---- T14 abi_asserts: every copy site asserted (gpsp_rom_load_progress has two from 7ffe4236).
# P-review 2026-11-08 (B3): on a split tree (P4) the copies sit in psp/fe/X.inc.c, so sites are
# found in TU order through the includes (p0a_t14_tu.py grep) and named by their cite (main_psp.c:N
# or fe/X.inc.c:N); a mutant of a fragment copy is laid over the tree with --frag.  Every cite of
# the generated headers must name the right line (p0a_t14_tu.py cites), and the 6 constant asserts
# must be there: a tool that reads the constants from the raw shell only (the pre-review rule) is
# caught, as is a constant that is not found (it fails, never drops silently).
if want t14; then
  echo "== T14 abi_asserts (docker)"
  AS="$OUT/t14"; rm -rf "$AS"; mkdir -p "$AS"
  TU="$PY $HERE/p0a_t14_tu.py"
  sites=$($TU grep '^\s*extern void \(\*gpsp_rom_load_progress\)' "$TREE" | tr '\n' ' ')
  for fl in release lab harness; do
    expect_rc 0 "[$fl] fe_abi_asserts.h generated, compiles, pin 0 lost" $PY "$R/abi_asserts.py" gen --flavour-dir "$RUN/$fl" --src "$TREE" --out "$AS/$fl"
    grep -E "^T14|^pin:|more than one site|^constants:" "$OUT/last.log" | sed 's/^/        /' | cut -c1-200
    grep -qE "^pin: [0-9]+ pinned, 0 lost" "$OUT/last.log" || bad "[$fl] pin lost names"
    grep -qE "^T14 $fl: .*; 6 constants asserted$" "$OUT/last.log" && grep -q '^constants: all 6 found' "$OUT/last.log" \
      && ok "[$fl] the 6 constant asserts are there" || bad "[$fl] constant asserts missing"
    hs=$(grep -oE '"(main_psp\.c|fe/[A-Za-z0-9_.]+):[0-9]+ gpsp_rom_load_progress:' "$AS/$fl/fe_abi_asserts.h" | sed -E 's/^"([^ ]+) .*/\1/' | tr '\n' ' ')
    [ "$hs" = "$sites" ] && ok "[$fl] gpsp_rom_load_progress asserted at every copy: $sites(by anchor)" \
      || bad "[$fl] gpsp_rom_load_progress asserted at '$hs', copies at '$sites'"
    expect_rc 0 "[$fl] every cite of the header names its line on this tree" $TU cites "$AS/$fl/fe_abi_asserts.h" "$TREE"
    tail -1 "$OUT/last.log" | sed 's/^/        /'
  done
  # the copy at a cite, mutated: through --main-c (the shell or a single file) or --frag (a fragment)
  mutate() {  # mutate <cite> <sed expr> <out file> -> echoes the gen args
    local f=${1%:*} n=${1##*:}
    sed -E "${n}$2" "$TREE/psp/$f" > "$3"
    cmp -s "$TREE/psp/$f" "$3" && { echo "MUTANT-NOT-APPLIED"; return; }
    if [ "$f" = main_psp.c ]; then echo "--main-c $3"; else echo "--frag $3 $f"; fi
  }
  for kind in file block; do
    if [ $kind = file ]; then pat='^extern void \(\*gpsp_rom_load_progress\)\(unsigned, unsigned\);'
    else pat='^\s+extern void \(\*gpsp_rom_load_progress\)\(unsigned, unsigned\);'; fi
    c=$($TU grep "$pat" "$TREE" | head -1)
    [ -n "$c" ] || { echo "        (no $kind-scope copy of gpsp_rom_load_progress here: mutant not applicable)"; continue; }
    args=$(mutate "$c" 's/\(unsigned, unsigned\)/(unsigned, int)/' "$AS/mut_$kind.c")
    [ "$args" = MUTANT-NOT-APPLIED ] && { bad "M-T14 $kind-scope mutant not applied at $c"; continue; }
    $PY "$R/abi_asserts.py" gen --flavour-dir "$RUN/release" --src "$TREE" --out "$AS/mut_$kind" $args > "$OUT/last.log" 2>&1; rc=$?
    [ $rc = 1 ] && grep -q "static assertion failed: \"$c gpsp_rom_load_progress" "$OUT/last.log" \
      && ok "M-T14 $kind-scope copy ($c) -> (unsigned, int): its own static assert fails" \
      || { bad "M-T14 $kind-scope copy mutant at $c (rc $rc)"; grep -m3 "assert\|PIN" "$OUT/last.log" | sed 's/^/        /'; }
  done
  # a constant copy, mutated where it is (a fragment after P4): its assert fails; deleted: the gen fails
  c=$($TU grep '^#define VRAM_DIRTY_SHIFT 10' "$TREE" | head -1)
  if [ -n "$c" ]; then
    args=$(mutate "$c" 's/VRAM_DIRTY_SHIFT 10/VRAM_DIRTY_SHIFT 11/' "$AS/mut_shift.c")
    $PY "$R/abi_asserts.py" gen --flavour-dir "$RUN/release" --src "$TREE" --out "$AS/mut_shift" $args > "$OUT/last.log" 2>&1; rc=$?
    [ $rc = 1 ] && grep -q "static assertion failed: \"$c VRAM_DIRTY_SHIFT differs from the core header" "$OUT/last.log" \
      && ok "M-T14c VRAM_DIRTY_SHIFT 10 -> 11 at $c: its constant assert fails" \
      || { bad "M-T14c VRAM_DIRTY_SHIFT mutant at $c (rc $rc)"; grep -m3 "assert\|CONSTANT" "$OUT/last.log" | sed 's/^/        /'; }
  else bad "M-T14c no '#define VRAM_DIRTY_SHIFT 10' in the TU"; fi
  c=$($TU grep '^#define MER_MISS_LIMIT ' "$TREE" | head -1)
  if [ -n "$c" ]; then
    args=$(mutate "$c" 's/^#define MER_MISS_LIMIT /#define MER_MISS_LIMIT_GONE /' "$AS/mut_gone.c")
    $PY "$R/abi_asserts.py" gen --flavour-dir "$RUN/release" --src "$TREE" --out "$AS/mut_gone" $args > "$OUT/last.log" 2>&1; rc=$?
    [ $rc = 1 ] && grep -q "^constants: CONSTANT MISSING: MER_MISS_LIMIT" "$OUT/last.log" \
      && ok "M-T14d MER_MISS_LIMIT renamed away at $c: the gen FAILS (CONSTANT MISSING), not a silent drop" \
      || { bad "M-T14d missing-constant mutant (rc $rc)"; grep -m3 "^constants\|^T14" "$OUT/last.log" | sed 's/^/        /'; }
  else bad "M-T14d no '#define MER_MISS_LIMIT' in the TU"; fi
  # tool mutant: the constants read from the raw shell only (the pre-review rule, abi_asserts.py
  # macro_values(main_raw)): on a split tree it must now FAIL; on a single file it changes nothing
  sed 's/^    tu = tu_lines(main_raw, os.path.join(src, .psp.), over)$/    tu = file_lines(main_raw, "main_psp.c")/' "$R/abi_asserts.py" > "$R/.abi_asserts_raw.py"
  if cmp -s "$R/abi_asserts.py" "$R/.abi_asserts_raw.py"; then bad "tool mutant (raw shell constants) not applied"
  else
    $PY "$R/.abi_asserts_raw.py" gen --flavour-dir "$RUN/release" --src "$TREE" --out "$AS/toolraw" > "$OUT/last.log" 2>&1; rc=$?
    if grep -q '^#include "fe/[A-Za-z0-9_.]*\.inc\.c"' "$TREE/psp/main_psp.c"; then
      [ $rc = 1 ] && grep -q "^constants: CONSTANT MISSING" "$OUT/last.log" \
        && ok "tool mutant (constants from the raw shell only): FAILS on the split tree (rc 1, CONSTANT MISSING)" \
        || bad "tool mutant (raw shell constants) passed on the split tree (rc $rc)"
    else
      [ $rc = 0 ] && ok "tool mutant (raw shell constants): no fragments here, same verdict (rc 0)" || bad "tool mutant on a single file (rc $rc)"
    fi
  fi
  rm -f "$R/.abi_asserts_raw.py"
  # the pre-review header cited fragment lines as main_psp.c:N: on a split tree the cite check catches it
  if grep -q '^#include "fe/[A-Za-z0-9_.]*\.inc\.c"' "$TREE/psp/main_psp.c"; then
    sed -E 's#^/\* fe/[A-Za-z0-9_.]+:([0-9]+) \(#/* main_psp.c:\1 (#' "$AS/release/fe_abi_asserts.h" > "$AS/oldcites.h"
    expect_rc 1 "mutant: the header with its fragment cites relabelled main_psp.c:N (the pre-review form) fails the cite check" \
      $TU cites "$AS/oldcites.h" "$TREE"
    tail -1 "$OUT/last.log" | sed 's/^/        /'
  fi
  if [ "$(echo $sites | wc -w)" -gt 1 ]; then
    # the old tool folded token-identical copies into the first: one site only
    sed 's/        key = (r\[0\], r\[1\], tuple(x.text for x in r\[2\]))/        key = (r[0], tuple(x.text for x in r[2]))/' "$R/abi_asserts.py" > "$R/.abi_asserts_mut.py"
    cmp -s "$R/abi_asserts.py" "$R/.abi_asserts_mut.py" && bad "tool mutant (old dedupe) not applied"
    $PY "$R/.abi_asserts_mut.py" gen --flavour-dir "$RUN/release" --src "$TREE" --out "$AS/toolmut" > /dev/null 2>&1
    rm -f "$R/.abi_asserts_mut.py"
    hs=$(grep -oE '"(main_psp\.c|fe/[A-Za-z0-9_.]+):[0-9]+ gpsp_rom_load_progress:' "$AS/toolmut/fe_abi_asserts.h" | sed -E 's/^"([^ ]+) .*/\1/' | tr '\n' ' ')
    [ "$hs" != "$sites" ] && ok "tool mutant (dedupe by tokens, the old rule): asserts '$hs' only, not '$sites': caught" \
      || bad "tool mutant (old dedupe) still asserts every copy"
  fi
  # P-review r2 (N4): fe_priv.h's core-ABI index is what it says it is: every name T14 asserts (in the
  # headers just generated) plus the six its sentence names, each UNCHECKED; mutants drop an asserted
  # name from the index and a name from the six
  FP="$TREE/psp/fe/fe_priv.h"
  HS="$AS/release/fe_abi_asserts.h $AS/lab/fe_abi_asserts.h $AS/harness/fe_abi_asserts.h"
  if [ -f "$FP" ] && grep -q "The index below is PLAN-P-beta5" "$FP"; then
    expect_rc 0 "fe_priv.h's index = the asserted names + the six it names (N4) + the sites in no flavour (B6)" $TU index "$FP" $HS --src "$TREE"
    sed -n '1,4p' "$OUT/last.log" | sed 's/^/        /' | cut -c1-200
    sed 's/ flush_rom_total, flush_ram_full,/ flush_ram_full,/' "$FP" > "$AS/idx_drop.h"
    cmp -s "$FP" "$AS/idx_drop.h" && bad "M-T14i index mutant not applied" || \
      expect_rc 1 "M-T14i flush_rom_total dropped from the index: caught" $TU index "$AS/idx_drop.h" $HS --src "$TREE"
    sed 's/declares, and gbcore_prepare,/declares, and gbcore,/' "$FP" > "$AS/idx_six.h"
    cmp -s "$FP" "$AS/idx_six.h" && bad "M-T14j six-sentence mutant not applied" || \
      expect_rc 1 "M-T14j gbcore_prepare dropped from the six sentence: caught" $TU index "$AS/idx_six.h" $HS --src "$TREE"
    # P-review r3 (B6): the "main()'s" text names every site in no T14 flavour (abi_asserts unbuilt)
    sed 's/ rom_hash_census (fe\/harness/ (fe\/harness/' "$FP" > "$AS/idx_unb.h"
    cmp -s "$FP" "$AS/idx_unb.h" && bad "M-T14k unbuilt-sentence mutant not applied" || \
      expect_rc 1 "M-T14k rom_hash_census dropped from the sites-in-no-flavour text: caught" $TU index "$AS/idx_unb.h" $HS --src "$TREE"
  else echo "        (no core-ABI index sentence in this tree's fe_priv.h: N4/B6 rows not applicable)"; fi
  # P-review r3 (B6): abi_asserts.py unbuilt, the copy sites none of the headers just generated cites (a raw
  # scan of the TU, every #if arm).  Mutants on a copy of psp/ (no docker): a new extern in an arm no flavour
  # defines is listed; the same extern in no #if is refused (every flavour compiles it: stale headers); a
  # header citing a site the raw scan does not find is refused; NAME(...); at file scope is not a site.
  if grep -q '^#include "fe/[A-Za-z0-9_.]*\.inc\.c"' "$TREE/psp/main_psp.c"; then
    expect_rc 0 "abi_asserts unbuilt: the copy sites in no T14 flavour, from the raw TU" $PY "$R/abi_asserts.py" unbuilt --src "$TREE" --gen "$AS"
    sed -n '1,12p' "$OUT/last.log" | sed 's/^/        /' | cut -c1-200
    n0=$(grep -cE '^  (main_psp\.c|fe/)' "$OUT/last.log")
    UT="$AS/unb"; ufrag=fe/paths.inc.c
    unb_tree() {  # unb_tree <name> <text appended to fe/paths.inc.c> -> a gpsp dir: psp/main_psp.c + psp/fe/
      rm -rf "$UT/$1"; mkdir -p "$UT/$1/psp"; cp "$M" "$UT/$1/psp/"; cp -a "$TREE/psp/fe" "$UT/$1/psp/fe"
      printf '%b' "$2" >> "$UT/$1/psp/$ufrag"
    }
    unb_tree arm '#ifdef T14_NO_FLAVOUR_DEFINES_THIS\nextern unsigned t14_unbuilt_probe;\n#endif\n'
    $PY "$R/abi_asserts.py" unbuilt --src "$UT/arm" --gen "$AS" > "$OUT/last.log" 2>&1; rc=$?
    n1=$(grep -cE '^  (main_psp\.c|fe/)' "$OUT/last.log")
    [ $rc = 0 ] && [ "$n1" = $((n0 + 1)) ] && grep -qE "^  $ufrag:[0-9]+ +t14_unbuilt_probe +extern +#ifdef T14_NO_FLAVOUR_DEFINES_THIS$" "$OUT/last.log" \
      && ok "M-T14u1 an extern in an arm no flavour defines: listed ($n0 -> $n1 sites)" \
      || { bad "M-T14u1 (rc $rc, $n0 -> $n1 sites)"; tail -4 "$OUT/last.log" | sed 's/^/        /'; }
    unb_tree every 'extern unsigned t14_unbuilt_probe;\n'
    expect_rc 1 "M-T14u2 the same extern in no #if (every flavour compiles it, no header cites it): refused" \
      $PY "$R/abi_asserts.py" unbuilt --src "$UT/every" --gen "$AS"
    grep -q 'in no #if group are cited by no generated header' "$OUT/last.log" || bad "M-T14u2 refused for another reason"
    unb_tree macro 'T14_MACRO_CALL(t14_probe_arg);\n'
    $PY "$R/abi_asserts.py" unbuilt --src "$UT/macro" --gen "$AS" > "$OUT/last.log" 2>&1; rc=$?
    [ $rc = 0 ] && [ "$(grep -cE '^  (main_psp\.c|fe/)' "$OUT/last.log")" = "$n0" ] \
      && ok "M-T14u3 NAME(...); at file scope in no #if: a macro call, not a site (rc 0, $n0 sites)" \
      || { bad "M-T14u3 macro-call statement (rc $rc)"; tail -3 "$OUT/last.log" | sed 's/^/        /'; }
    mkdir -p "$UT/hdr/release" "$UT/hdr/lab" "$UT/hdr/harness"
    for fl in lab harness; do cp "$AS/$fl/fe_abi_asserts.h" "$UT/hdr/$fl/"; done
    sed -E 's#^(/\* UNCHECKED .*)$#\1\n *   fe/paths.inc.c:1         t14_no_such_copy                     (mutant)#' \
      "$AS/release/fe_abi_asserts.h" > "$UT/hdr/release/fe_abi_asserts.h"
    cmp -s "$AS/release/fe_abi_asserts.h" "$UT/hdr/release/fe_abi_asserts.h" && bad "M-T14u4 header mutant not applied"
    expect_rc 1 "M-T14u4 a header citing a site the raw scan does not find: refused" \
      $PY "$R/abi_asserts.py" unbuilt --src "$TREE" --gen "$UT/hdr"
    grep -q 'not in the raw scan of the TU' "$OUT/last.log" || bad "M-T14u4 refused for another reason"
    rm -rf "$UT"
  else echo "        (single-file tree: abi_asserts unbuilt rows not applicable)"; fi
fi

# ---- T6 strong_syms.spec: the comments cite the P0c cut base's lines (P0a T6); strict on the
# tree whose psp/main_psp.c is the cut base's (claude/beta5-base 3bc947d0, = phase-p's P0c tree)
CUT_BASE_MAIN=9032d9543ad59ac56a40413f4dccb654f4be6845f8192ad552fc10f40a69ba9a
if want t6; then
  echo "== T6 strong_syms.spec cites"
  if [ "$(sha256sum < "$M" | cut -c1-64)" = "$CUT_BASE_MAIN" ]; then
    expect_rc 0 "every spec cite names its symbol on the cut base (main_psp.c ${CUT_BASE_MAIN:0:12})" $PY "$HERE/p0a_test_cites.py" "$R/strong_syms.spec" "$TREE"
    tail -1 "$OUT/last.log" | sed 's/^/        /'
    sed -E 's/main\.c:1068/main.c:958/' "$R/strong_syms.spec" > "$OUT/fbe_cites.spec"
    expect_rc 1 "mutant: fbe9da63's main.c:958 (before HALT_HOP's +110 lines) is caught" \
      $PY "$HERE/p0a_test_cites.py" "$OUT/fbe_cites.spec" "$TREE"
    tail -1 "$OUT/last.log" | sed 's/^/        /'
    sed -E 's/rfu\.c:411/rfu.c:403/; s/rfu\.c:452/rfu.c:440/; s/main\.c:1068/main.c:935/; s/main_psp\.c:7638/main_psp.c:7541/; s/mgift_net\.c:147-148/mgift_net.c:117-118/' \
      "$R/strong_syms.spec" > "$OUT/old_cites.spec"
    expect_rc 1 "mutant: the 01067e74 cites (rfu.c:403/440, main.c:935, main_psp.c:7541, mgift_net.c:117-118) are caught" \
      $PY "$HERE/p0a_test_cites.py" "$OUT/old_cites.spec" "$TREE"
    tail -1 "$OUT/last.log" | sed 's/^/        /'
  else
    $PY "$HERE/p0a_test_cites.py" "$R/strong_syms.spec" "$TREE" > "$OUT/last.log" 2>&1
    echo "        (cites are the cut base's; on ${COMMIT:0:8}: $(tail -1 "$OUT/last.log"), informational)"
  fi
fi

# ---- T8 exit_matrix: anchors, FE_* names, file-keyed raw view (raw part, no compiler)
if want xs_raw; then
  echo "== T8 exit_matrix raw view"
  expect_rc 0 "p0a_test_exit_matrix (C1-C3, M1-M3)" $PY "$HERE/p0a_test_exit_matrix.py" "$M" ${CUT:+--cutlist "$CUT"}
  sed 's/^/        /' "$OUT/last.log" | grep -E "ok|FAIL" | cut -c1-220
fi

# ---- T8 exit_matrix -E: every flavour + raw on the base, P3's sed, and the P4 overlay
if want xs_e; then
  echo "== T8 exit_matrix -E (docker)"
  XS="$OUT/xs"; rm -rf "$XS"; mkdir -p "$XS"
  expect_rc 0 "exit_matrix all on the base" $PY "$R/exit_matrix.py" all --baseline "$RUN" --src "$TREE" --out "$XS/base"
  grep -E "RESULT|sites per view" "$XS/base/report.txt" | sed 's/^/        /'
  $PY "$HERE/p0a_test_exit_matrix.py" --p3-sed-only "$M" "$XS/p3_main_psp.c" > /dev/null 2>&1
  if [ -f "$XS/p3_main_psp.c" ]; then
    expect_rc 0 "exit_matrix all with P3's sed applied" $PY "$R/exit_matrix.py" all --baseline "$RUN" --src "$TREE" --out "$XS/p3" --main-c "$XS/p3_main_psp.c"
    d=""
    for v in raw release lab harness; do for t in exit_sites exit_steps exit_scenarios; do
      cmp -s "$XS/base/$v/$t.tsv" "$XS/p3/$v/$t.tsv" || d="$d $v/$t"; done; done
    [ -z "$d" ] && ok "P3 sed: exit_sites/steps/scenarios.tsv byte-identical in raw + 3 flavours" || bad "P3 sed changed:$d"
  else bad "could not write the P3 sed mutant"; fi
  if [ -n "$CUT" ]; then
    P4="$OUT/p4tree"; rm -rf "$P4"; cp -a "$TREE" "$P4"
    rm -rf "$OUT/split"; $PY "$R/split.py" split --src "$M" --cutlist "$CUT" --out "$OUT/split" > "$OUT/split.log" 2>&1 || bad "split.py split"
    cp "$OUT/split/psp/main_psp.c" "$P4/psp/main_psp.c"; cp -r "$OUT/split/psp/fe" "$P4/psp/fe"
    expect_rc 0 "exit_matrix all on the P4-simulated tree" $PY "$R/exit_matrix.py" all --baseline "$RUN" --src "$P4" --out "$XS/p4"
    expect_rc 0 "XS check: P4 overlay == base in every flavour (sites keyed by id, scenarios)" $PY "$R/exit_matrix.py" check --golden "$XS/base" --cand "$XS/p4"
    $PY "$R/exit_matrix.py" check --golden "$XS/base" --cand "$XS/p4" 2>&1 | tail -1 | sed 's/^/        /'
    # raw view too (check_gate covers the flavours; the raw tables compared by key here)
    for t in exit_scenarios; do cmp -s "$XS/base/raw/$t.tsv" "$XS/p4/raw/$t.tsv" && ok "P4 raw $t.tsv identical" || bad "P4 raw $t.tsv differs"; done
  fi
fi

# ---- T9 globals: cutlist owners, Makefile-derived defines, refusals, in-tree = copy
if want t9; then
  echo "== T9 globals census"
  if [ -n "$CUT" ]; then
    expect_rc 0 "census with the cutlist: 0 UNASSIGNED, modules.py agrees" $PY "$R/globals.py" census --src "$M" --root "$TREE" --cutlist "$CUT" --tsv "$OUT/g_tree.tsv" --json "$OUT/g_tree.json"
    grep -E "definitions|UNASSIGNED|define sets|macro-globals|owners from" "$OUT/last.log" | sed 's/^/        /'
    if grep -q "g_start_keepalive" "$M"; then   # the PLAN-P-beta5 2.1 names exist (7ffe4236 on)
      $PY "$HERE/p0a_t9_owners.py" "$OUT/g_tree.json" "$M" > "$OUT/own.log" 2>&1 && ok "$(tail -1 "$OUT/own.log")" || { bad "owners: $(tail -3 "$OUT/own.log")"; }
    fi
    mkdir -p "$OUT/gcopy"; cp "$M" "$OUT/gcopy/main_psp.c"
    $PY "$R/globals.py" census --src "$OUT/gcopy/main_psp.c" --root "$TREE" --cutlist "$CUT" --tsv "$OUT/g_copy.tsv" > /dev/null 2>&1
    cmp -s <(cut -f2- "$OUT/g_tree.tsv") <(cut -f2- "$OUT/g_copy.tsv") && ok "census of a copy == in-tree (597-row TSV identical)" || bad "census of a copy differs from in-tree"
    # mutant: the old exclusion (only the exact --src path) -> the copy sees main_psp.c as an extern user
    sed 's/if p == selfp or SELF_TU.match(/if p == selfp or False and SELF_TU.match(/' "$R/globals.py" > "$R/.globals_mut.py"
    $PY "$R/.globals_mut.py" census --src "$OUT/gcopy/main_psp.c" --root "$TREE" --cutlist "$CUT" --tsv "$OUT/g_copy_mut.tsv" > /dev/null 2>&1
    rm -f "$R/.globals_mut.py"
    n=$(paste <(cut -f7 "$OUT/g_tree.tsv") <(cut -f7 "$OUT/g_copy_mut.tsv") | awk -F'\t' '$1 != $2' | wc -l)
    [ "$n" -gt 0 ] && ok "mutant (exclude only the --src path): $n lifetime classes flip on a copy: caught (the MF10 cause)" || bad "MF10 mutant not caught"
    expect_rc 2 "a split shell (#include \"fe/*.inc.c\") is refused" $PY "$R/globals.py" census --src "$OUT/split/psp/main_psp.c" --root "$TREE" --cutlist "$CUT"
    [ -f "$OUT/split/psp/main_psp.c" ] || bad "(no split shell: run xs_e first)"
    $PY "$R/split.py" inverse --split "$OUT/split" -o "$OUT/inverse_main_psp.c" > /dev/null 2>&1
    $PY "$R/globals.py" census --src "$OUT/inverse_main_psp.c" --root "$TREE" --cutlist "$CUT" --tsv "$OUT/g_inv.tsv" > /dev/null 2>&1
    cmp -s <(cut -f2- "$OUT/g_tree.tsv") <(cut -f2- "$OUT/g_inv.tsv") && ok "census of split.py inverse == base" || bad "census of the inverse differs"
  fi
  if [ "$(sha256sum < "$M" | cut -c1-64)" != 942c8b546d6b159bf80660081fde816e31082a99aa6e4442a8b45e8272fe5cd1 ]; then
    expect_rc 2 "no --cutlist on a non-01067e74 source is refused" $PY "$R/globals.py" census --src "$M" --root "$TREE" --cutlist /nonexistent
    expect_rc 2 "the 01067e74 cutlist on this source is refused" $PY "$R/globals.py" census --src "$M" --root "$TREE" --cutlist "$HERE/fixtures/cutlist_01067e74.txt"
  else
    expect_rc 0 "01067e74 without --cutlist: the modules.py fallback is allowed" $PY "$R/globals.py" census --src "$M" --root "$TREE" --cutlist /nonexistent
  fi
fi

# ---- check_flags scans psp/fe
if want cf; then
  echo "== check_flags"
  CT="$OUT/cftree"; rm -rf "$CT"; cp -a "$TREE" "$CT"; cp "$R/../check_flags.py" "$CT/tools/check_flags.py"
  expect_rc 0 "check_flags on the base: clean" $PY "$CT/tools/check_flags.py"
  if [ -d "$OUT/split/psp/fe" ]; then
    cp "$OUT/split/psp/main_psp.c" "$CT/psp/main_psp.c"; cp -r "$OUT/split/psp/fe" "$CT/psp/fe"
    expect_rc 0 "check_flags on the P4-simulated tree: clean" $PY "$CT/tools/check_flags.py"
    sed 's#FE_DIRS = ("psp", "psp/fe",#FE_DIRS = ("psp",#' "$CT/tools/check_flags.py" > "$CT/tools/check_flags_mut.py"
    expect_rc 1 "mutant without psp/fe on the P4 tree: DRIFT dead (caught)" $PY "$CT/tools/check_flags_mut.py"
    grep DRIFT "$OUT/last.log" | sed 's/^/        /'
  fi
fi

# ---- T12 stack_depth: .su stem match
if want st && [ -n "$CUT" ]; then
  echo "== T12 stack_depth .su"
  for fl in release harness; do expect_rc 0 "[$fl] P4-simulated .su (C1) + record-file mutant (M1)" $PY "$HERE/p0a_test_stack_su.py" "$RUN/$fl" "$CUT"
    grep -E "ok|FAIL" "$OUT/last.log" | sed 's/^/        /'; done
fi

# ---- T13 ef_srcscan MGIFT, T11 ci_greps R3 roots / R4 allow
if want t13; then
  echo "== T13 ef_srcscan / T11 ci_greps"
  expect_rc 0 "ef_srcscan MGIFT: release scan has the mgift functions; mutant without the define loses them" $PY "$HERE/p0a_test_srcscan.py" "$M"
  sed 's/^/        /' "$OUT/last.log"
  expect_rc 0 "ci_greps --phase base green" $PY "$R/ci_greps.py" --src "$TREE" --phase base
  grep -E "R3|R4|T11" "$OUT/last.log" | sed 's/^/        /'
  if grep -q "^static void gba_reload_note" "$M"; then
    CG="$OUT/cgtree"; rm -rf "$CG"; mkdir -p "$CG"; cp -a "$TREE/psp" "$TREE/frontend-common" "$CG/" 2>/dev/null
    for d in netdrv gbcore; do [ -d "$TREE/$d" ] && cp -a "$TREE/$d" "$CG/"; done
    sed -i '/^static void gba_reload_note(int phase, const char \*what)$/,/^{$/ s/^{$/{ pwr_hold("p0a_mutant");/' "$CG/psp/main_psp.c"
    expect_rc 1 "R3 mutant: pwr_hold inside gba_reload_note (an extra root) is caught" $PY "$R/ci_greps.py" --src "$CG" --phase base --rules R3
    grep -E "FAIL  R3" "$OUT/last.log" | head -2 | sed 's/^/        /'
  fi
fi

# ---- T3 pp_diff --extra-defs / pp_sweep
if want pp; then
  echo "== T3 pp_diff / variant sweep (docker)"
  expect_rc 2 "pp_diff without --logs is refused" bash "$R/pp_diff.sh" --ref "$TREE" --new "$TREE"
  expect_rc 0 "G0 base vs base, every flavour in the run (the -D sets printed)" bash "$R/pp_diff.sh" --ref "$TREE" --new "$TREE" --logs "$RUN"
  grep -E "\-D set|IDENTICAL|G0" "$OUT/last.log" | sed 's/^/        /' | cut -c1-200
  expect_rc 0 "variant sweep: every build.sh switch alone and in pairs, 4 flavours, derived lines = the logs" bash "$R/pp_sweep.sh" --ref "$TREE" --new "$TREE" --logs "$RUN"
  grep -E "^pp_sweep: ([0-9]+ (switches|flavour)|\[)|^G0var" "$OUT/last.log" | sed 's/^/        /' | cut -c1-220
  sweep_mut() {  # sweep_mut <want rc> <label> <overlay dir> [extra args]: one overlay mutant through the full sweep
    local w=$1 l=$2 o=$3; shift 3
    expect_rc "$w" "$l" bash "$R/pp_sweep.sh" --ref "$TREE" --new "$TREE" --logs "$RUN" --overlay "$o" "$@"
    grep -E "^pp_sweep: DIFFERENT|^G0var" "$OUT/last.log" | sed 's/^/        /' | cut -c1-200
  }
  if grep -q '^#define g_mg_on 0' "$M"; then
    mkdir -p "$OUT/ppovl/psp"; sed 's/^#define g_mg_on 0\b/#define g_mg_on (0)/' "$M" > "$OUT/ppovl/psp/main_psp.c"
    cmp -s "$M" "$OUT/ppovl/psp/main_psp.c" && bad "MGIFT=0-arm mutant was not applied"
    expect_rc 0 "MGIFT=0-arm mutant: the 3 built flavours stay G0-identical (they never see it)" bash "$R/pp_diff.sh" --ref "$TREE" --new "$TREE" --logs "$RUN" --overlay "$OUT/ppovl"
    sweep_mut 1 "MGIFT=0-arm mutant: the sweep catches it" "$OUT/ppovl"
    expect_rc 1 "  ... and the chosen arm alone (--arms MGIFT=0) too" bash "$R/pp_sweep.sh" --ref "$TREE" --new "$TREE" --logs "$RUN" --overlay "$OUT/ppovl" --arms MGIFT=0
    expect_rc 3 "a chosen subset that is identical is PARTIAL (exit 3), never a gate pass" bash "$R/pp_sweep.sh" --ref "$TREE" --new "$TREE" --logs "$RUN" --arms CHEATS=0
  fi
  # adversarial review 2026-11-07 (scratchpad adv/mk_ppmut.sh, run_prune.sh, other_tu.sh)
  if grep -q '^#define g_mgift_consumed 0u$' "$M"; then
    mkdir -p "$OUT/ppMA/psp"
    # was MGIFT=0 x RFU_SCHED=0; RFU_SCHED is no switch since code-health C-4
    awk '{ if ($0=="#define g_mgift_consumed 0u") print "#define g_mgift_consumed 1u"; else print }' "$M" > "$OUT/ppMA/psp/main_psp.c"
    sweep_mut 1 "MA: a change only under MGIFT=0 (g_mgift_consumed): the MGIFT=0 arm catches it" "$OUT/ppMA"
  fi
  if grep -q '^#define g_mg_on 0   /\* MGIFT=0: never listening \*/$' "$M"; then
    mkdir -p "$OUT/ppMB/psp"
    awk '{ if ($0=="#define g_mg_on 0   /* MGIFT=0: never listening */") { print "#ifdef REWIND"; print $0; print "#else"; print "#define g_mg_on 1"; print "#endif" } else print }' "$M" > "$OUT/ppMB/psp/main_psp.c"
    sweep_mut 1 "MB: a change only under REWIND=0 x MGIFT=0 (g_mg_on): caught on every flavour" "$OUT/ppMB"
    # a same-meaning rewrite of build.sh's MGIFT line + an MGIFT=0-only change (it pruned the arm, exit 0)
    NT="$OUT/pptree"; rm -rf "$NT"; cp -a "$TREE" "$NT"
    sed -i 's/^MGIFT="\${MGIFT:-1}"$/MGIFT=${MGIFT:-1}/' "$NT/tools/build.sh"
    sed -i 's/^#define g_mg_on 0   \/\* MGIFT=0: never listening \*\//#define g_mg_on 1   \/* MGIFT=0: never listening *\//' "$NT/psp/main_psp.c"
    if cmp -s "$TREE/tools/build.sh" "$NT/tools/build.sh" || cmp -s "$M" "$NT/psp/main_psp.c"; then bad "prune mutant not applied"
    else
      expect_rc 1 "prune mutant: MGIFT=\${MGIFT:-1} (unquoted) is still the MGIFT switch; the MGIFT=0 change is caught" bash "$R/pp_sweep.sh" --ref "$TREE" --new "$NT" --logs "$RUN"
      grep -E "^pp_sweep: (changed|pruned)" "$OUT/last.log" | sed 's/^/        /' | cut -c1-160
    fi
  fi
  L3="$OUT/logs3"; rm -rf "$L3"
  for f in release lab; do mkdir -p "$L3/$f/logs"; cp "$RUN/$f/logs/.build-eboot.log" "$L3/$f/logs/"; cp "$RUN/$f/flavour.json" "$L3/$f/"; done
  expect_rc 2 "logs without harness: a skipped flavour is exit 2" bash "$R/pp_sweep.sh" --ref "$TREE" --new "$TREE" --logs "$L3"
  expect_rc 0 "  --allow-prune harness lets the sweep pass on the other two" bash "$R/pp_sweep.sh" --ref "$TREE" --new "$TREE" --logs "$L3" --allow-prune harness
  grep -E "^pp_sweep: pruned" "$OUT/last.log" | sed 's/^/        /'
  # a leaky log (the run built with ME_LOWLAT=0 in the shell): the derived line is not the logged one
  LL="$OUT/logsll"; rm -rf "$LL"; cp -r "$RUN" "$LL" 2>/dev/null || { mkdir -p "$LL"; for f in release lab harness; do mkdir -p "$LL/$f/logs"; cp "$RUN/$f/logs/.build-eboot.log" "$LL/$f/logs/"; cp "$RUN/$f/flavour.json" "$LL/$f/"; done; }
  sed -i -E '/ -c -o main_psp\.o main_psp\.c$/ s/ -include fe_features\.h/ -DPLAY_ME_LOWLAT=0 -include fe_features.h/' "$LL/release/logs/.build-eboot.log"
  expect_rc 2 "leaky log (-DPLAY_ME_LOWLAT=0 on release's line): the derivation check refuses it" bash "$R/pp_sweep.sh" --ref "$TREE" --new "$TREE" --logs "$LL"
  grep -E "derived .* line is not the logged one" "$OUT/last.log" | sed 's/^/        /' | cut -c1-200
  # another TU (ui_psp.c) under REWIND=0: the sweep covers every file the step changes
  ul=$(grep -n '^#endif' "$TREE/psp/ui_psp.c" | head -1 | cut -d: -f1)
  mkdir -p "$OUT/ppui/psp"
  awk -v n="$ul" 'NR==n { print "#else"; print "int g_adv_ui_mutant = 1;"; } { print }' "$TREE/psp/ui_psp.c" > "$OUT/ppui/psp/ui_psp.c"
  if grep -B3 "int g_adv_ui_mutant" "$OUT/ppui/psp/ui_psp.c" | grep -qE '^#if'; then
    sweep_mut 1 "other TU: an #else arm added to ui_psp.c's first #if: ui_psp.c is swept and caught" "$OUT/ppui"
  else
    awk 'NR==1 { print "#ifndef REWIND"; print "int g_adv_ui_mutant = 1;"; print "#endif" } { print }' "$TREE/psp/ui_psp.c" > "$OUT/ppui/psp/ui_psp.c"
    sweep_mut 1 "other TU: a REWIND=0-only object in ui_psp.c: ui_psp.c is swept and caught" "$OUT/ppui"
  fi
  # a header only main_psp.c includes, changed under MGIFT=0: swept through its includer
  if [ -f "$TREE/psp/mgift_shortcut.h" ]; then
    mkdir -p "$OUT/pphdr/psp"; { cat "$TREE/psp/mgift_shortcut.h"; printf '#ifndef GBADHOC_MGIFT\n#define GBADHOC_ADV_HDR 1\n#endif\n'; } > "$OUT/pphdr/psp/mgift_shortcut.h"
    expect_rc 0 "a header change that adds only an unused macro: swept through its includers, identical (-P)" bash "$R/pp_sweep.sh" --ref "$TREE" --new "$TREE" --logs "$RUN" --overlay "$OUT/pphdr"
    grep -E "swept through" "$OUT/last.log" | sed 's/^/        /' | cut -c1-200
  fi
  # step scope (DS) and build files (BF) on trees
  expect_rc 0 "DS: a tree against itself is in scope" bash "$R/step_scope.sh" ds --ref "$TREE" --new "$TREE"
  ST="$OUT/sstree"; rm -rf "$ST"; cp -a "$TREE" "$ST"; echo "/* x */" >> "$ST/psp/ui_psp.c"
  expect_rc 1 "DS: a step that touches psp/ui_psp.c is out of scope" bash "$R/step_scope.sh" ds --ref "$TREE" --new "$ST"
  expect_rc 0 "DS: ... unless --scope-allow psp/ui_psp.c (printed)" bash "$R/step_scope.sh" ds --ref "$TREE" --new "$ST" --scope-allow psp/ui_psp.c
  rm -rf "$ST"; cp -a "$TREE" "$ST"; mkdir -p "$ST/psp/fe"; echo "/* frag */" > "$ST/psp/fe/x.inc.c"; echo "/* y */" >> "$ST/psp/main_psp.c"
  expect_rc 0 "DS: main_psp.c and a new psp/fe/ fragment are in scope" bash "$R/step_scope.sh" ds --ref "$TREE" --new "$ST"
  if [ -d "$OUT/pptree" ]; then
    expect_rc 1 "BF: the build.sh rewrite is a build-file change" bash "$R/step_scope.sh" bf --ref "$TREE" --new "$OUT/pptree"
    expect_rc 1 "DS: ... and out of scope" bash "$R/step_scope.sh" ds --ref "$TREE" --new "$OUT/pptree"
  fi
  expect_rc 0 "BF: identical trees" bash "$R/step_scope.sh" bf --ref "$TREE" --new "$TREE"
fi

# ---- T15 flavours --against with the real tools
if want flav; then
  echo "== T15 flavours.sh --against (real tools)"
  if [ -n "${RUN2:-}" ]; then
    expect_rc 0 "--check-only RUN2 --against RUN (two runs of one commit)" bash "$R/flavours.sh" --check-only --out "$RUN2" --against "$RUN"
    echo "        gate: $(cat "$RUN2/gate.txt")"
  fi
  if [ -n "${TWIN:-}" ]; then
    expect_rc 0 "--check-only TWIN --against RUN (another commit, same gpsp/ tree, pinned): G1-identical" bash "$R/flavours.sh" --check-only --out "$TWIN" --against "$RUN"
    echo "        gate: $(cat "$TWIN/gate.txt")"
  fi
  # a reference with a doctored image is refused before any check
  DR="$OUT/doctored"; rm -rf "$DR"; mkdir -p "$DR"
  for fl in release lab harness; do mkdir -p "$DR/$fl"; cp "$RUN/$fl/flavour.json" "$DR/$fl/"; sed -i 's/"dockerId": "sha256:[0-9a-f]*"/"dockerId": "sha256:doctored"/' "$DR/$fl/flavour.json"; done
  expect_rc 2 "--against a manifest with a doctored dockerId exits 2" bash "$R/flavours.sh" --check-only --out "$RUN" --against "$DR"
  grep -qE "dockerId differs|not a hermetic build" "$OUT/last.log" && ok "  refused for: $(grep -oE '(dockerId differs|not a hermetic build)' "$OUT/last.log" | head -1)" || bad "doctored refusal reason"
  # adversarial doctor.sh: each field of the contract on its own (needs a hermetic run)
  if grep -q '"hermetic": "yes"' "$RUN/release/flavour.json"; then
    doct() {  # doct <name> <flavour> <sed>: RUN's records with one field doctored (no ELF needed: refused first)
      local d="$OUT/doct_$1" f; rm -rf "$d"
      for f in release lab harness; do mkdir -p "$d/$f"; cp "$RUN/$f/flavour.json" "$d/$f/"; done
      cp "$RUN/flavours.json" "$RUN/ci_greps.txt" "$d/" 2>/dev/null; sed -i "$3" "$d/$2/flavour.json"
    }
    doct dirty release 's/"workingTreeClean": true/"workingTreeClean": false/'
    doct lock release 's/"layoutLock": "[^"]*"/"layoutLock": "release:differs"/'
    doct xenv lab 's/"extraEnv": ""/"extraEnv": "ME_LOWLAT=0 INPUT_FRESH=0"/'
    doct benv harness 's/"buildEnv": "/"buildEnv": "ME_LOWLAT=0 /'
    doct anc lab 's/"sourceCommit": "[0-9a-f]*"/"sourceCommit": "0000000000000000000000000000000000000000"/'
    for k in dirty lock xenv benv anc; do
      expect_rc 2 "contract: a reference with a doctored field ($k) is refused" bash "$R/flavours.sh" --check-only --out "$RUN" --against "$OUT/doct_$k"
      grep -m1 "FAIL:" "$OUT/last.log" | sed 's/^/        /' | cut -c1-200
    done
  else echo "        (RUN is not hermetic: the field-by-field contract cases need a hermetic run)"; fi
fi

# ---- --runs discover the run's flavours (a hard-coded list once skipped release-stable)
if want runs; then
  echo "== --runs flavour discovery (det_check, sect_cmp, strong_syms checkrun)"
  RS1="$OUT/runs1"; RS2="$OUT/runs2"; rm -rf "$RS1" "$RS2"
  for f in release lab harness; do
    [ -f "$RUN/$f/gpsp_adhoc.elf" ] || continue
    for d in "$RS1" "$RS2"; do mkdir -p "$d/$f"; cp "$RUN/$f/gpsp_adhoc.elf" "$RUN/$f/flavour.json" "$d/$f/"; cp "$RUN/$f/gpsp_adhoc.map" "$d/$f/" 2>/dev/null; done
  done
  expect_rc 0 "det_check --runs on two copies: identical in every discovered flavour" $PY "$R/det_check.py" --runs "$RS1" "$RS2"
  tail -1 "$OUT/last.log" | sed 's/^/        /'
  # the flipped flavour was release-stable until code-health C-4 removed it
  if [ -d "$RS2/harness" ]; then
    $PY "$HERE/p0a_flip_text.py" "$RS2/harness/gpsp_adhoc.elf" > /dev/null
    expect_rc 1 "runs_har mutant: one .text byte flipped in harness: det_check --runs FAILS" $PY "$R/det_check.py" --runs "$RS1" "$RS2"
    expect_rc 1 "runs_har mutant: sect_cmp --runs FAILS" $PY "$R/sect_cmp.py" --runs "$RS1" "$RS2"
    rm -rf "$RS2/lab"
    expect_rc 1 "a flavour in one run only (lab) is a FAIL, not a skip" $PY "$R/det_check.py" --runs "$RS1" "$RS2"
    expect_rc 0 "strong_syms checkrun: every discovered flavour" $PY "$R/strong_syms.py" checkrun --run "$RS1" --golden "$R/strong_syms.golden.tsv"
    grep -E "^SS:|held to" "$OUT/last.log" | sed 's/^/        /'
    rm -f "$RS1/harness/gpsp_adhoc.map"
    expect_rc 1 "strong_syms checkrun: harness without its map FAILS (not skipped)" $PY "$R/strong_syms.py" checkrun --run "$RS1" --golden "$R/strong_syms.golden.tsv"
  fi
fi

# ---- T11 on the split tree: *.inc.c are part of their includer; KNOWN rows by function + content
if want t11split && [ -n "$CUT" ]; then
  echo "== T11 ci_greps on the P4 split"
  SP="$OUT/split11"; rm -rf "$SP"; $PY "$R/split.py" split --src "$M" --cutlist "$CUT" --out "$SP" > /dev/null 2>&1 || bad "split.py split"
  PT="$OUT/p4ci"; rm -rf "$PT"; mkdir -p "$PT"; cp -a "$TREE/psp" "$TREE/frontend-common" "$PT/"
  for d in netdrv gbcore; do [ -d "$TREE/$d" ] && cp -a "$TREE/$d" "$PT/"; done
  cp "$TREE"/*.h "$PT/" 2>/dev/null
  cp "$SP/psp/main_psp.c" "$PT/psp/"; cp -r "$SP/psp/fe" "$PT/psp/"
  $PY "$R/ci_greps.py" --src "$TREE" --phase base > "$OUT/ci_base.txt" 2>&1; rb=$?
  expect_rc 0 "ci_greps --phase base on the P4 split: green" $PY "$R/ci_greps.py" --src "$PT" --phase base
  cp "$OUT/last.log" "$OUT/ci_p4.txt"
  if diff <(grep -v '^logical files' "$OUT/ci_p4.txt" | sed -E 's/ \(psp\/fe\/[^)]*\)//') "$OUT/ci_base.txt" > /dev/null && [ $rb = 0 ]; then
    ok "P4 ci_greps output == base output line for line (fragment locations aside): $(grep -c KNOWN "$OUT/ci_p4.txt") KNOWN"
  else bad "P4 ci_greps output differs from the base's"; diff <(grep -v '^logical files' "$OUT/ci_p4.txt" | sed -E 's/ \(psp\/fe\/[^)]*\)//') "$OUT/ci_base.txt" | head -8 | sed 's/^/        /'; fi
  # mutant: the old file-by-file view (fragments read on their own) fails the correct split
  expect_rc 1 "mutant --no-logical (the old per-file view): the correct split FAILS (R1/R4/R5)" $PY "$R/ci_greps.py" --src "$PT" --phase base --no-logical
  grep -E "FAIL  R[0-9]" "$OUT/last.log" | sed -E 's/^ *FAIL  (R[0-9]+) .*/\1/' | sort | uniq -c | tr '\n' ' ' | sed 's/^/        /'; echo
  # mutant: the KNOWN R4 call copied into another function is not excused by the row
  fk=$(grep -l "fe_host_sram_test_kill_handle" "$PT/psp/fe/"*.inc.c 2>/dev/null | head -1)
  if [ -n "$fk" ]; then
    printf '\nstatic void p0a_adv_r4(void) { fe_evt("x %%d", fe_host_sram_test_kill_handle()); }\n' >> "$fk"
    expect_rc 1 "KNOWN key mutant: the same call in another function (fragment $(basename "$fk")) FAILS" $PY "$R/ci_greps.py" --src "$PT" --phase base --rules R4
    grep -E "FAIL  R4" "$OUT/last.log" | head -1 | sed 's/^/        /' | cut -c1-200
  fi
fi

# ---- the REAL P4: split.py + the cutlist as a commit on this run's commit, built by
# flavours.sh --commit and gated --against RUN on every check (G0 G0var G1 G2 HO SS ST LL CF
# BF DS CI), then the non-byte metrics (T13 census, T1b helpers, T10/T16 window by anchor)
if want p4 && [ -n "$CUT" ]; then
  echo "== P4: a real split build gated --against the run"
  if ! grep -q '"hermetic": "yes"' "$RUN/release/flavour.json"; then
    bad "P4 needs a hermetic RUN (flavours.sh of 2026-11-07): rebuild $RUN"
  else
    REPO=${FLAV_REPO:-$(cd "$R/../../.." && pwd)}    # FLAV_REPO: tools run from a copy (run_all on a tree export)
    P4C=$(bash "$HERE/p4_commit.sh" --repo "$REPO" --base "$COMMIT" --cutlist "$CUT" --out "$OUT/p4c" 2> "$OUT/p4c.err" | tail -1)
    if ! [[ "$P4C" =~ ^[0-9a-f]{40}$ ]]; then bad "p4_commit.sh: $(cat "$OUT/p4c.err")"
    else
      ok "P4 commit ${P4C:0:10} = ${COMMIT:0:10} + split ($(ls "$OUT/p4c/split/psp/fe" | wc -l) fragments)"
      BV=$(sed -nE 's/.*"buildVersion": *"([^"]*)".*/\1/p' "$RUN/release/flavour.json")
      SD=$(sed -nE 's/.*"sourceDateEpoch": *"([^"]*)".*/\1/p' "$RUN/release/flavour.json")
      P4R=${P4_RUN:-$OUT/p4run}
      if [ -f "$P4R/release/flavour.json" ] && grep -q "\"sourceCommit\": \"$P4C\"" "$P4R/release/flavour.json"; then
        expect_rc 0 "P4 (built run $P4R) --check-only --against RUN: the whole generated gate line green" \
          bash "$R/flavours.sh" --check-only --out "$P4R" --against "$RUN"
      else
        expect_rc 0 "P4 built (flavours.sh --commit ${P4C:0:10} --against RUN): the whole generated gate line green" \
          bash "$R/flavours.sh" --commit "$P4C" --out "$P4R" --against "$RUN" --build-version "$BV" --sde "$SD" ${LOCK:+--lock "$LOCK"}
      fi
      echo "        gate: $(cat "$P4R/gate.txt" 2>/dev/null)"
      grep -E "^== .* FAIL" "$P4R/checks.txt" 2>/dev/null | sed 's/^/        /'
      P4T="$OUT/p4src"; rm -rf "$P4T"; cp -a "$TREE" "$P4T"
      cp "$OUT/p4c/split/psp/main_psp.c" "$P4T/psp/"; cp -r "$OUT/p4c/split/psp/fe" "$P4T/psp/"
      for fl in release lab harness; do
        [ -f "$P4R/$fl/gpsp_adhoc.elf" ] || continue
        expect_rc 0 "[$fl] T13 census of P4 --against the base: no function changed status/symbols/callers" \
          $PY "$R/inline_census.py" "$P4R/$fl/gpsp_adhoc.elf" --src "$OUT/p4c/split/psp/main_psp.c" --flavour "$fl" \
          --against "$RUN/$fl/gpsp_adhoc.elf" --src-base "$M"
        grep -E "inlined|IC|change" "$OUT/last.log" | tail -2 | sed 's/^/        /' | cut -c1-200
        $PY "$R/loop_diff.py" "$RUN/$fl/gpsp_adhoc.elf" --main-c "$M" > "$OUT/ld_base_$fl.txt" 2>&1
        $PY "$R/loop_diff.py" "$P4R/$fl/gpsp_adhoc.elf" --main-c "$OUT/p4c/split/psp/main_psp.c" > "$OUT/ld_p4_$fl.txt" 2>&1
        hb=$(grep -E "inlined helpers in|composition" "$OUT/ld_base_$fl.txt" | sed -E 's/ before L:[0-9]+//'); hp=$(grep -E "inlined helpers in|composition" "$OUT/ld_p4_$fl.txt" | sed -E 's/ before L:[0-9]+//')
        [ -n "$hb" ] && [ "$hb" = "$hp" ] && ok "[$fl] T1b inlined helpers in main + window composition equal on P4:$(echo "$hp" | grep "helpers in" | sed -E 's/.*main: //' | cut -c1-110)" \
          || bad "[$fl] T1b inlined helpers: base '$hb' vs P4 '$hp'"
        wb=$(grep -E "^  by (line|address)" "$OUT/ld_base_$fl.txt" | sed -E 's/by line [0-9]+-[0-9]+/by line/'); wp=$(grep -E "^  by (line|address)" "$OUT/ld_p4_$fl.txt" | sed -E 's/by line [0-9]+-[0-9]+/by line/')
        [ -n "$wb" ] && [ "$wb" = "$wp" ] && ok "[$fl] T1b loop window by line and by address equal on P4" || bad "[$fl] T1b window differs"
        if [ "$fl" != harness ]; then
          expect_rc 0 "[$fl] T10 layout_report on P4 (window by anchor in the split shell) --ref the base" $PY "$R/layout_report.py" "$P4R/$fl" --src "$P4T" --ref "$RUN/$fl" --no-pad-check
          grep -E "main loop by line|layout lock" "$OUT/last.log" | sed 's/^/        /' | cut -c1-160
          expect_rc 0 "[$fl] T16 loop_lpsim window by anchor on P4" $PY "$R/loop_lpsim/loop_lpsim.py" window "$P4R/$fl/gpsp_adhoc.elf"
        fi
      done
      # the tool mutants: the old decl_file rule (main_psp.c only) loses the fragments' functions
      sed 's/or f.endswith(.\.inc\.c.))$/)/' "$R/inline_census.py" > "$R/.ic_mut.py"
      if cmp -s "$R/inline_census.py" "$R/.ic_mut.py"; then bad "inline_census mutant not applied"
      else
        $PY "$R/.ic_mut.py" "$P4R/release/gpsp_adhoc.elf" --src "$OUT/p4c/split/psp/main_psp.c" --flavour release \
          --against "$RUN/release/gpsp_adhoc.elf" --src-base "$M" > "$OUT/last.log" 2>&1; rc=$?
        if [ $rc = 1 ] && ! grep -q Traceback "$OUT/last.log"; then
          ok "mutant inline_census by decl_file = main_psp.c: P4 reads as changed (caught): $(grep -cE '^ +[a-z_0-9.]+ +' "$OUT/last.log") rows"
        else bad "inline_census mutant rc $rc"; tail -3 "$OUT/last.log" | sed 's/^/        /'; fi
      fi
      rm -f "$R/.ic_mut.py"
    fi
  fi
fi

echo "p0a_b5: $([ $FAILS -eq 0 ] && echo PASS || echo "$FAILS FAILED")  (out $OUT)"
exit $((FAILS > 0))
