#!/usr/bin/env bash
# abi-exit-ci_test_exit.sh -- self-test and mutant tests of T8 (exit_matrix.py +
# test_exit_model.c).  Run from WSL/Linux with docker (pspdev/pspdev) and gcc:
#   bash tests/abi-exit-ci_test_exit.sh <baseline run dir> <scratch out dir>
# e.g. .../mainpsp-tools-out/baseline/run1  .../mainpsp-tools-out/scratch-abi-exit-ci/t8
# Only psp-gcc -E / -dM runs in docker (no PSP build).  The tree is mounted read-only;
# source mutants are COPIES of psp/main_psp.c bind-mounted over it.
#
#   C0  exit_matrix.py --selftest (parser, interpreter, two built-in mutants)
#   C1  `all` on the clean baseline: x2 reproduced, §3.2 order, release-live rows,
#       raw vs -E all '#if'/'macro', every scenario evaluable  -> RESULT ok
#   C2  a second `all` from fresh -E output equals the first (XS gate: check passes)
#   C3  test_exit_model.c + the reference model: every site and scenario equal, x3 flavours
#   M1  model mutant: D3/D4 swapped                        -> model FAILS
#   M2  model mutant: the two inproc_refusal calls merged   -> model FAILS
#   M3  model mutant: audio_hash guard dropped              -> model FAILS (lab/harness)
#   M4  model mutant: gpsp_rtc_peek put behind GPSP_PERF_RIG -> model FAILS (release)
#   M5  model mutant: AV-first sites lose vid_term          -> model FAILS
#   M6  model mutant: EVT-first sites run audio first       -> model FAILS
#   M7  source mutant: play_lock_drop/pwr_hold swapped (18099/18100) -> order check and XS FAIL
#   M8  source mutant: B1 loses vid_term                    -> XS FAILS at B1 (all flavours)
#   M9  source mutant: gpsp_rtc_peek guarded by g_perf_rig  -> release-live check FAILS
#   M10 -E mutant: a call deleted from harness -E output    -> raw vs -E UNEXPLAINED
#   M11 source mutant: pure 3-line shift                    -> all ok, XS ok
#   M12 source mutant: B1 exit code 2 -> 3                  -> XS FAILS at B1
#   M13 shifted golden vs shifted tree with B1 losing vid_term -> XS FAILS at B1
set -u
BASE=${1:?baseline run dir}; OUT=${2:?scratch out dir}
HERE=$(cd "$(dirname "$0")/.." && pwd); SRC=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT"; FAILS=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
EM="python3 $HERE/exit_matrix.py"

$EM --selftest > "$OUT/c0.log" 2>&1 && ok "C0 selftest" || { bad "C0 selftest"; tail -5 "$OUT/c0.log"; }

rm -rf "$OUT/base" "$OUT/base2"
$EM all --baseline "$BASE" --src "$SRC" --out "$OUT/base" > "$OUT/c1.log" 2>&1 \
  && ok "C1 baseline: $(tail -1 "$OUT/c1.log")" || { bad "C1 baseline"; grep -v '  ok  ' "$OUT/c1.log" | tail -20; }
$EM all --baseline "$BASE" --src "$SRC" --out "$OUT/base2" > "$OUT/c2a.log" 2>&1
$EM check --golden "$OUT/base" --cand "$OUT/base2" > "$OUT/c2.log" 2>&1 \
  && ok "C2 regenerated matrix equals the golden (XS ok)" || { bad "C2"; tail -5 "$OUT/c2.log"; }

flavdefs() { case $1 in
  release) echo "-DGPSP_PLAYABLE -DGPSP_NO_TELEMETRY";;
  lab)     echo "-DGPSP_PERF_RIG -DGPSP_KEEP_TELEMETRY -DLAB_HARNESS";;
  harness) echo "-DGPSP_PERF_RIG -DGPSP_KEEP_TELEMETRY";; esac; }
model() {  # model <flavour> <extra -D> -> exit status of the model run
  gcc -std=gnu99 -w -DREWIND -DGBA_LINK $(flavdefs "$1") $2 \
      -DEXPECT_H="\"$OUT/base/$1/exit_expect.h\"" \
      -DTEARDOWN_INC="\"$HERE/tests/abi-exit-ci_teardown_ref.inc.c\"" \
      "$HERE/tests/test_exit_model.c" -o "$OUT/model_$1" || return 9
  "$OUT/model_$1" > "$OUT/model_$1$2.log" 2>&1
}
for f in release lab harness; do
  model $f "" && ok "C3 model $f: $(tail -1 "$OUT/model_$f.log")" || { bad "C3 model $f"; tail -6 "$OUT/model_$f.log"; }
done
mut() {  # mut <label> <define> <flavours that must fail>
  local caught=""
  for f in $3; do model $f "-D$2"; [ $? -eq 1 ] && caught="$caught $f"; done
  [ "$(echo $caught)" = "$(echo $3)" ] && ok "$1 caught in:$caught" || bad "$1 caught only in:'$caught' (wanted $3)"
}
mut "M1 D3/D4 swap"             MUT_SWAP_D3_D4          "release lab harness"
mut "M2 merged inproc_refusal"  MUT_MERGE_REFUSAL       "release lab harness"
mut "M3 audio_hash unguarded"   MUT_UNGUARD_AUDIO_HASH  "lab harness"
mut "M4 rtc_peek rig-only"      MUT_RTC_PEEK_RIG_ONLY   "release lab harness"
mut "M5 AV-first w/o vid_term"  MUT_SITE_NO_VIDTERM_AV  "release lab harness"
mut "M6 EVT-first reordered"    MUT_SITE_AV_BEFORE_EVT  "release lab harness"
# M3 is invisible in release by construction: fe_evt compiles out there.
model release -DMUT_UNGUARD_AUDIO_HASH && ok "M3 control: release unaffected (fe_evt is ((void)0))" || bad "M3 control"

# ---- source mutants (copies of main_psp.c, mounted over the read-only tree)
MC="$SRC/psp/main_psp.c"
srcmut() {  # srcmut <name> <python edit> ; leaves $OUT/<name>/report.txt
  local d="$OUT/$1"; rm -rf "$d"; mkdir -p "$d"
  python3 "$HERE/tests/abi-exit-ci_edit.py" "$MC" "$d/main_psp.c" "$2" || return 1
  $EM all --baseline "$BASE" --src "$SRC" --out "$d/xs" --main-c "$d/main_psp.c" > "$d/all.log" 2>&1
  echo $? > "$d/rc"
  $EM check --golden "$OUT/base" --cand "$d/xs" > "$d/check.log" 2>&1; echo $? > "$d/xs_rc"
}
srcmut m7 "
i = L.index('   play_lock_drop(\"loop_exit\");'); assert L[i+1] == '   pwr_hold(\"teardown\");'
L[i], L[i+1] = L[i+1], L[i]"
grep -q 'ORDER session_end:D3 play_lock_drop' "$OUT/m7/xs/report.txt" && [ "$(cat $OUT/m7/xs_rc)" = 1 ] \
  && ok "M7 D3/D4 swap in source: order check + XS fail" || { bad "M7"; tail -12 "$OUT/m7/all.log"; }
srcmut m8 "
k = [j for j in range(15290, 15301) if L[j].strip() == 'vid_term();']; assert len(k) == 1; L[k[0]] = ''"
n8=$(grep -c 'XS FAIL .* exit_sites.tsv B1$' "$OUT/m8/check.log")
[ "$n8" = 3 ] && ok "M8 B1 without vid_term: XS fails at B1 in 3 flavours" || { bad "M8 ($n8)"; cat "$OUT/m8/check.log" | head; }
srcmut m9 "
k = [j for j, l in enumerate(L) if l.strip() == 'gpsp_rtc_peek(&shown, &frames_t, &wall);']; assert len(k) == 1
L[k[0]] = '      if (g_perf_rig) gpsp_rtc_peek(&shown, &frames_t, &wall);'"
grep -q 'gpsp_rtc_peek .*RIG-GUARDED\|gpsp_rtc_peek .*MISSING' "$OUT/m9/xs/report.txt" && [ "$(cat $OUT/m9/rc)" = 1 ] \
  && ok "M9 rtc_peek behind g_perf_rig: release-live check fails" || { bad "M9"; grep -A5 'release-live' "$OUT/m9/xs/report.txt"; }

# ---- line-shift mutants (verifier, 2026-11-05): site identity must not depend on line
# numbers, and the gate must see the exit code/why, not only the vocabulary.
#   M11 pure 3-line shift above every site      -> all ok, XS ok (no false FAIL)
#   M12 B1 exit code 2 -> 3, nothing else       -> XS FAILS at B1 (3 flavours)
#   M13 golden from a 2000-line-shifted tree, candidate = that tree with B1 losing
#       vid_term (the PLAN §3.6 regenerated-golden case)  -> XS FAILS at B1 (3 flavours)
srcmut m11 "
L[95:95] = ['', '', '']"
[ "$(cat $OUT/m11/rc)" = 0 ] && [ "$(cat $OUT/m11/xs_rc)" = 0 ] \
  && ok "M11 pure line shift: all ok + XS ok" || { bad "M11 (all rc $(cat $OUT/m11/rc), xs rc $(cat $OUT/m11/xs_rc))"; head -12 "$OUT/m11/check.log"; }
srcmut m12 "
k = [j for j in range(15290, 15301) if 'handoff_run(2, \"bad_variant\")' in L[j]]; assert len(k) == 1
L[k[0]] = L[k[0]].replace('handoff_run(2,', 'handoff_run(3,')"
n12=$(grep -c 'XS FAIL .* exit_sites.tsv B1$' "$OUT/m12/check.log")
[ "$n12" = 3 ] && ok "M12 B1 exit code changed: XS fails at B1 in 3 flavours" || { bad "M12 ($n12)"; head "$OUT/m12/check.log"; }
srcmut m13g "
L[95:95] = [''] * 2000"
srcmut m13 "
L[95:95] = [''] * 2000
k = [j for j in range(17290, 17306) if L[j].strip() == 'vid_term();']; assert len(k) == 1; L[k[0]] = ''"
$EM check --golden "$OUT/m13g/xs" --cand "$OUT/m13/xs" > "$OUT/m13/check2.log" 2>&1
n13=$(grep -c 'XS FAIL .* exit_sites.tsv B1$' "$OUT/m13/check2.log")
[ "$(cat $OUT/m13g/rc)" = 0 ] && [ "$n13" = 3 ] && ok "M13 shifted golden, B1 without vid_term: XS fails at B1 in 3 flavours" \
  || { bad "M13 (golden rc $(cat $OUT/m13g/rc), $n13)"; head "$OUT/m13/check2.log"; }

# ---- -E mutant: one call disappears from the harness -E output without a macro or #if
rm -rf "$OUT/m10"; mkdir -p "$OUT/m10/pp"; cp "$OUT/base/pp/"* "$OUT/m10/pp/"
python3 "$HERE/tests/abi-exit-ci_edit.py" "$OUT/m10/pp/main_psp.harness.i" "$OUT/m10/pp/main_psp.harness.i" "
k = [i for i, l in enumerate(L) if l.strip() == 'me_rend_teardown(\"exit\");']
assert len(k) == 1; L[k[0]] = ''"
$EM all --baseline "$BASE" --src "$SRC" --out "$OUT/m10/xs" --pp-dir "$OUT/m10/pp" > "$OUT/m10/all.log" 2>&1
grep -q 'UNEXPLAINED D me_rend_teardown' "$OUT/m10/all.log" && ok "M10 unexplained raw-vs-E difference caught" \
  || { bad "M10"; grep 'raw vs' "$OUT/m10/all.log"; }

echo "abi-exit-ci T8 tests: $([ $FAILS = 0 ] && echo PASS || echo "$FAILS FAIL")"
exit $([ $FAILS = 0 ] && echo 0 || echo 1)
