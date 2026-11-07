#!/usr/bin/env bash
# p0a_b5.sh -- the P0a repairs (PLAN-P-beta5 §3 P0a) proven on ONE tree, each with a
# mutant that must be caught.  run_all.sh keeps the 01067e74 self-tests; this is the
# "b5 row set" it runs on the other trees (7ffe4236, fbe9da63, and later the P0c tree).
#
#   p0a_b5.sh --tree GPSP_DIR --run RUN_DIR [--cutlist CUTLIST] [--out DIR] [--only a,b]
#     GPSP_DIR  a CLEAN source tree of the commit RUN_DIR was built from (git archive)
#     RUN_DIR   flavours.sh --baseline output of that commit (release, release-stable,
#               lab, harness), built with the P0a flavours.sh
#     CUTLIST   the cutlist describing GPSP_DIR's main_psp.c (P4 simulation; T9 owners)
#     RUN2      (env) a second run of the same commit, for the --against case
#     TWIN      (env) a run of a DIFFERENT commit with the same gpsp/ tree, built with
#               the same --build-version/--sde pins (G1 two-commit case)
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
    expect_rc 0 "[$fl] layout_report by anchor" $PY "$R/layout_report.py" "$RUN/$fl" --no-pad-check
    grep -E "main loop by line" "$OUT/last.log" | sed 's/^/        /'
    expect_rc 0 "[$fl] loop_lpsim window by anchor" $PY "$R/loop_lpsim/loop_lpsim.py" window "$E"
    if ! $PY "$R/loop_anchor.py" "$M" | grep -q "lines 16368-180"; then   # not 01067e74 (there the old window IS the anchored code lines)
      expect_rc 2 "[$fl] loop_diff --loop-lines 16368-18090 (the old window) refused" $PY "$R/loop_diff.py" "$E" --loop-lines 16368-18090
      $PY "$R/layout_report.py" "$RUN/$fl" --no-pad-check --loop-lines 16368-18090 > "$OUT/last.log" 2>&1
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
    expect_rc 2 "the 01067e74 cutlist on this source is refused" $PY "$R/globals.py" census --src "$M" --root "$TREE" --cutlist "$R/cutlist.txt"
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
  expect_rc 0 "variant sweep, every arm build.sh accepts preprocesses" bash "$R/pp_sweep.sh" --ref "$TREE" --new "$TREE" --logs "$RUN"
  grep -E "^pp_sweep|^G0var" "$OUT/last.log" | sed 's/^/        /'
  if grep -q '^#define g_mg_on 0' "$M"; then
    mkdir -p "$OUT/ppovl/psp"; sed 's/^#define g_mg_on 0\b/#define g_mg_on (0)/' "$M" > "$OUT/ppovl/psp/main_psp.c"
    cmp -s "$M" "$OUT/ppovl/psp/main_psp.c" && bad "MGIFT=0-arm mutant was not applied"
    expect_rc 0 "MGIFT=0-arm mutant: the 4 built flavours stay G0-identical (they never see it)" bash "$R/pp_diff.sh" --ref "$TREE" --new "$TREE" --logs "$RUN" --overlay "$OUT/ppovl"
    expect_rc 1 "MGIFT=0-arm mutant: the mgift0 arm of the sweep catches it" bash "$R/pp_sweep.sh" --ref "$TREE" --new "$TREE" --logs "$RUN" --overlay "$OUT/ppovl" --arms mgift0
  fi
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
  for fl in release; do mkdir -p "$DR/$fl"; cp "$RUN/$fl/flavour.json" "$DR/$fl/"; sed -i 's/"dockerId": "sha256:[0-9a-f]*"/"dockerId": "sha256:doctored"/' "$DR/$fl/flavour.json"; done
  expect_rc 2 "--against a manifest with a doctored dockerId exits 2" bash "$R/flavours.sh" --check-only --out "$RUN" --against "$DR" --flavours release
fi

echo "p0a_b5: $([ $FAILS -eq 0 ] && echo PASS || echo "$FAILS FAILED")  (out $OUT)"
exit $((FAILS > 0))
