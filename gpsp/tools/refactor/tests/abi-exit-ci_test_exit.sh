#!/usr/bin/env bash
# abi-exit-ci_test_exit.sh -- self-test and mutant tests of T8 (exit_matrix.py +
# test_exit_model.c).  Run from WSL/Linux with docker (pspdev/pspdev) and gcc:
#   bash tests/abi-exit-ci_test_exit.sh <baseline run dir> <scratch out dir>
# The baseline run dir is a flavours.sh run (release/ lab/ harness/, each with
# logs/.build-eboot.log) OF THIS TREE, or of one with the same -D set: the -E views
# use its compile lines (S1 below proves an older one is refused).
# Only psp-gcc -E / -dM runs in docker (no PSP build).  The tree is mounted read-only;
# source mutants are COPIES of psp/main_psp.c (the shell) or of one fragment
# psp/fe/X.inc.c, bind-mounted over it.  Every mutant finds its line BY ANCHOR (a
# statement), never by line number: the tree moves under them.
#
#   C0  exit_matrix.py --selftest (parser, interpreter, hold walk, F1 atoms, mutants)
#   C1  `all` on the clean baseline: x2 reproduced, §3.2 order, release-live rows,
#       raw vs -E all '#if'/'macro', every scenario evaluable, hold balance -> RESULT ok
#   S1  the same with a compile line that lacks a default-on define -> STALE BASELINE
#   C2  a second `all` from fresh -E output equals the first (XS gate: check passes)
#   C3  test_exit_model.c + the reference model: every site and scenario equal (call
#       order, fe_evt format strings, hold balance), x3 flavours
#   M1  model mutant: D3/D4 swapped                         -> model FAILS
#   M2  model mutant: the two inproc_refusal calls merged   -> model FAILS
#   M3  model mutant: audio_hash guard dropped              -> model FAILS (lab/harness)
#   M4  model mutant: gpsp_rtc_peek put behind GPSP_PERF_RIG -> model FAILS (release)
#   M5  model mutant: AV-first sites lose vid_term          -> model FAILS
#   M6  model mutant: EVT-first sites run audio first       -> model FAILS
#   M14 model mutant: a log line reworded (evt_drop)        -> model FAILS (lab/harness)
#   M15 model mutant: the return's pwr_release dropped      -> model FAILS
#   M21 model mutant (HW-2 F1 trees only): HOME does not cancel the relaunch -> model FAILS
#   M7  source mutant: play_lock_drop/pwr_hold swapped      -> order check and XS FAIL
#   M8  source mutant: B1 loses vid_term                    -> XS FAILS at B1 (all flavours)
#   M9  source mutant: gpsp_rtc_peek guarded by g_perf_rig  -> release-live check FAILS
#   M10 -E mutant: a call deleted from harness -E output    -> raw vs -E UNEXPLAINED
#   M11 source mutant: pure 3-line shift                    -> all ok, XS ok
#   M12 source mutant: B1 exit code 2 -> 3                  -> XS FAILS at B1
#   M13 shifted golden vs shifted tree with B1 losing vid_term -> XS FAILS at B1
#   M16 source mutant: B1's log line reworded, calls unchanged -> XS FAILS at B1 (lab, harness)
#   M17 source mutant: the teardown's evt_drop line reworded   -> XS FAILS, every scenario (lab, harness)
#   M18 source mutant: the return's pwr_release deleted         -> hold check FAILS (goto holding;
#       after Epoch C C3-2b the last statement of main()'s pass, read as the goto's)
#   M19 fragment mutant (--over): state_save_slot keeps its hold -> hold check FAILS
#   M20 source mutant: a site moved under the boot hold... C4 exits after pwr_release -> XS FAILS at C4 (hold)
set -u
BASE=${1:?baseline run dir}; OUT=${2:?scratch out dir}
HERE=$(cd "$(dirname "$0")/.." && pwd); SRC=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT"; FAILS=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
EM="python3 -I $HERE/exit_matrix.py"
ED="python3 -I $HERE/tests/abi-exit-ci_edit.py"

$EM --selftest > "$OUT/c0.log" 2>&1 && ok "C0 selftest" || { bad "C0 selftest"; grep FAIL "$OUT/c0.log" | head -5; }

rm -rf "$OUT/base" "$OUT/base2"
$EM all --baseline "$BASE" --src "$SRC" --out "$OUT/base" > "$OUT/c1.log" 2>&1 \
  && ok "C1 baseline: $(tail -1 "$OUT/c1.log")" || { bad "C1 baseline"; grep -v '  ok  ' "$OUT/c1.log" | tail -20; }

# S1: a baseline whose compile line lacks a define this tree turns on by default
rm -rf "$OUT/stale"; mkdir -p "$OUT/stale"
for f in release lab harness; do
  mkdir -p "$OUT/stale/$f/logs"
  sed 's/ -DGBADHOC_MGIFT//' "$BASE/$f/logs/.build-eboot.log" > "$OUT/stale/$f/logs/.build-eboot.log"
done
if grep -q -- '-DGBADHOC_MGIFT' "$BASE/release/logs/.build-eboot.log"; then
  $EM all --baseline "$OUT/stale" --src "$SRC" --out "$OUT/stale/xs" > "$OUT/s1.log" 2>&1; rc=$?
  n=$(grep -c '^STALE BASELINE .* -DGBADHOC_MGIFT' "$OUT/s1.log")
  [ $rc = 1 ] && [ "$n" = 3 ] && ok "S1 a baseline without -DGBADHOC_MGIFT is refused in 3 flavours" \
    || { bad "S1 stale baseline (rc $rc, $n lines)"; tail -4 "$OUT/s1.log"; }
else
  bad "S1: the baseline given has no -DGBADHOC_MGIFT itself (it is the stale one)"
fi

$EM all --baseline "$BASE" --src "$SRC" --out "$OUT/base2" > "$OUT/c2a.log" 2>&1
$EM check --golden "$OUT/base" --cand "$OUT/base2" > "$OUT/c2.log" 2>&1 \
  && ok "C2 regenerated matrix equals the golden (XS ok)" || { bad "C2"; tail -5 "$OUT/c2.log"; }

flavdefs() { case $1 in
  release) echo "-DGPSP_PLAYABLE -DGPSP_NO_TELEMETRY";;
  lab)     echo "-DGPSP_PERF_RIG -DGPSP_KEEP_TELEMETRY -DLAB_HARNESS";;
  harness) echo "-DGPSP_PERF_RIG -DGPSP_KEEP_TELEMETRY";; esac; }
featdefs() {  # the feature defines of the flavour's real compile line that the model reads
  local d=""; for x in REWIND GBA_LINK GBADHOC_MGIFT; do
    grep -q -- "-D$x\b" "$BASE/$1/logs/.build-eboot.log" && d="$d -D$x"; done; echo "$d"; }
model() {  # model <flavour> <extra -D> -> exit status of the model run
  gcc -std=gnu99 -w $(featdefs "$1") $(flavdefs "$1") $2 \
      -DEXPECT_H="\"$OUT/base/$1/exit_expect.h\"" \
      -DTEARDOWN_INC="\"${TEARDOWN_INC:-$HERE/tests/abi-exit-ci_teardown_ref.inc.c}\"" \
      "$HERE/tests/test_exit_model.c" -o "$OUT/model_$1" || return 9
  "$OUT/model_$1" > "$OUT/model_$1$2.log" 2>&1
}
for f in release lab harness; do
  model $f "" && ok "C3 model $f: $(tail -1 "$OUT/model_$f.log")" || { bad "C3 model $f"; tail -6 "$OUT/model_$f.log" | cut -c1-600; }
done
mut() {  # mut <label> <define> <flavours that must fail> [<text the model must print>]
  local caught=""
  for f in $3; do model $f "-D$2"; [ $? -eq 1 ] && { [ -z "${4:-}" ] || grep -q -- "$4" "$OUT/model_$f-D$2.log"; } && caught="$caught $f"; done
  [ "$(echo $caught)" = "$(echo $3)" ] && ok "$1 caught in:$caught" || bad "$1 caught only in:'$caught' (wanted $3)"
}
mut "M1 D3/D4 swap"             MUT_SWAP_D3_D4          "release lab harness"
mut "M2 merged inproc_refusal"  MUT_MERGE_REFUSAL       "release lab harness"
mut "M3 audio_hash unguarded"   MUT_UNGUARD_AUDIO_HASH  "lab harness"
mut "M4 rtc_peek rig-only"      MUT_RTC_PEEK_RIG_ONLY   "release lab harness"
mut "M5 AV-first w/o vid_term"  MUT_SITE_NO_VIDTERM_AV  "release lab harness"
mut "M6 EVT-first reordered"    MUT_SITE_AV_BEFORE_EVT  "release lab harness"
mut "M14 log line reworded"     MUT_EVT_TEXT            "lab harness" "log text"
mut "M15 return keeps the hold" MUT_DROP_RELEASE        "release lab harness"
# M21 exists only on a tree with HW-2 F1 (the golden then has the HOME_* scenarios)
if grep -q '^#define EXPECT_F1' "$OUT/base/release/exit_expect.h"; then
  mut "M21 HOME still relaunches (F1's guard dropped)" MUT_F1_RELAUNCH "release lab harness"
else
  echo "  skip  M21 (HOME still relaunches): this tree has no g_exit_req (HW-2 F1 not merged)"
fi
# M3 and M14 are invisible in release by construction: fe_evt compiles out there.
model release -DMUT_UNGUARD_AUDIO_HASH && ok "M3 control: release unaffected (fe_evt is ((void)0))" || bad "M3 control"
model release -DMUT_EVT_TEXT && ok "M14 control: release unaffected" || bad "M14 control"

# ---- source mutants (copies of the shell or of one fragment, mounted over the read-only tree)
MC="$SRC/psp/main_psp.c"
srcmut() {  # srcmut <name> <python edit> [<fragment fe/X.inc.c>]; leaves $OUT/<name>/xs/report.txt
  # Without a fragment the edit is tried on the shell, then (Epoch C C1: the exit
  # helpers and the shared teardown moved there) on fe/teardown.inc.c if the tree has it.
  local d="$OUT/$1" frag="${3:-}"; rm -rf "$d"; mkdir -p "$d"
  if [ -z "$frag" ] && [ -f "$SRC/psp/fe/teardown.inc.c" ] && ! $ED "$MC" "$d/main_psp.c" "$2" 2>/dev/null; then
    frag=fe/teardown.inc.c
  fi
  if [ -n "$frag" ]; then
    $ED "$SRC/psp/$frag" "$d/frag.inc.c" "$2" || return 1
    $EM all --baseline "$BASE" --src "$SRC" --out "$d/xs" --over "$frag=$d/frag.inc.c" > "$d/all.log" 2>&1
  else
    $ED "$MC" "$d/main_psp.c" "$2" || return 1
    $EM all --baseline "$BASE" --src "$SRC" --out "$d/xs" --main-c "$d/main_psp.c" > "$d/all.log" 2>&1
  fi
  echo $? > "$d/rc"
  $EM check --golden "$OUT/base" --cand "$d/xs" > "$d/check.log" 2>&1; echo $? > "$d/xs_rc"
}
# anchors shared by the edits: B1's handoff line, and the vid_term just above it
# (Epoch C C1b: B1 is `exit_early_end(XE_EVT_FIRST, 2, "bad_variant", 0);` and its
# vid_term is in the helper; no_vidterm() then pastes B1's sequence back without it)
B1='import re
h = [j for j, l in enumerate(L) if re.search(r"(FE_HANDOFF|handoff_run)\(2, \"bad_variant\"\)|exit_early_end\(XE_EVT_FIRST, 2, \"bad_variant\", 0\)", l)]; assert len(h) == 1, h
helper = "exit_early_end(" in L[h[0]]
v = [j for j in range(h[0] - 6, h[0]) if L[j].strip() == "vid_term();"]; assert helper or len(v) == 1, v
e = [j for j in range(h[0] - 6, h[0]) if "fe_evt(\"exit code=2 reason=bad_variant\")" in L[j]]; assert len(e) == 1, e
m = [j for j, l in enumerate(L) if l.startswith("int main(")]; assert len(m) == 1, m
def no_vidterm():
    if helper:
        ind = L[h[0]][:len(L[h[0]]) - len(L[h[0]].lstrip())]
        L[h[0]] = ind + "evt_shutdown(); audio_stop(); FE_HANDOFF(2, \"bad_variant\"); FE_EXIT_GAME();"
    else:
        L[v[0]] = ""
'
srcmut m7 "
a = [j for j, l in enumerate(L) if l.strip() in ('play_lock_drop(\"loop_exit\");', 'pwr_hold(\"teardown\");')]
assert len(a) == 2 and a[1] - a[0] <= 8, a
x = L.pop(a[1]); L.insert(a[0], x)"
grep -q 'ORDER session_end:D[34] ' "$OUT/m7/xs/report.txt" && [ "$(cat $OUT/m7/xs_rc)" = 1 ] \
  && ok "M7 D3/D4 swap in source: order check + XS fail" || { bad "M7"; tail -12 "$OUT/m7/all.log"; }
srcmut m8 "$B1
no_vidterm()"
n8=$(grep -c 'XS FAIL .* exit_sites.tsv B1$' "$OUT/m8/check.log")
[ "$n8" = 3 ] && ok "M8 B1 without vid_term: XS fails at B1 in 3 flavours" || { bad "M8 ($n8)"; cat "$OUT/m8/check.log" | head; }
srcmut m9 "
k = [j for j, l in enumerate(L) if l.strip() == 'gpsp_rtc_peek(&shown, &frames_t, &wall);']; assert len(k) == 1
L[k[0]] = '      if (g_perf_rig) gpsp_rtc_peek(&shown, &frames_t, &wall);'"
grep -q 'gpsp_rtc_peek .*RIG-GUARDED\|gpsp_rtc_peek .*MISSING' "$OUT/m9/xs/report.txt" && [ "$(cat $OUT/m9/rc)" = 1 ] \
  && ok "M9 rtc_peek behind g_perf_rig: release-live check fails" || { bad "M9"; grep -A5 'release-live' "$OUT/m9/xs/report.txt"; }

# ---- line-shift mutants (verifier, 2026-11-05): site identity must not depend on line
# numbers, and the gate must see the exit code/why, not only the vocabulary.
#   M11 pure 3-line shift above main()          -> all ok, XS ok (no false FAIL)
#   M12 B1 exit code 2 -> 3, nothing else       -> XS FAILS at B1 (3 flavours)
#   M13 golden from a 2000-line-shifted tree, candidate = that tree with B1 losing
#       vid_term (the PLAN §3.6 regenerated-golden case)  -> XS FAILS at B1 (3 flavours)
srcmut m11 "$B1
L[m[0]:m[0]] = ['', '', '']"
[ "$(cat $OUT/m11/rc)" = 0 ] && [ "$(cat $OUT/m11/xs_rc)" = 0 ] \
  && ok "M11 pure line shift: all ok + XS ok" || { bad "M11 (all rc $(cat $OUT/m11/rc), xs rc $(cat $OUT/m11/xs_rc))"; head -12 "$OUT/m11/check.log"; }
srcmut m12 "$B1
L[h[0]] = L[h[0]].replace('(2, \"bad_variant\")', '(3, \"bad_variant\")').replace(', 2, \"bad_variant\"', ', 3, \"bad_variant\"')"
n12=$(grep -c 'XS FAIL .* exit_sites.tsv B1$' "$OUT/m12/check.log")
[ "$n12" = 3 ] && ok "M12 B1 exit code changed: XS fails at B1 in 3 flavours" || { bad "M12 ($n12)"; head "$OUT/m12/check.log"; }
srcmut m13g "$B1
L[m[0]:m[0]] = [''] * 2000"
srcmut m13 "$B1
no_vidterm()
L[m[0]:m[0]] = [''] * 2000"
$EM check --golden "$OUT/m13g/xs" --cand "$OUT/m13/xs" > "$OUT/m13/check2.log" 2>&1
n13=$(grep -c 'XS FAIL .* exit_sites.tsv B1$' "$OUT/m13/check2.log")
[ "$(cat $OUT/m13g/rc)" = 0 ] && [ "$n13" = 3 ] && ok "M13 shifted golden, B1 without vid_term: XS fails at B1 in 3 flavours" \
  || { bad "M13 (golden rc $(cat $OUT/m13g/rc), $n13)"; head "$OUT/m13/check2.log"; }

# ---- Epoch C C0a-5: log-line text and power-hold balance
srcmut m16 "$B1
L[e[0]] = L[e[0]].replace('reason=bad_variant', 'why=bad_variant')"
n16=$(grep -c 'XS FAIL \(lab\|harness\) exit_sites.tsv B1$' "$OUT/m16/check.log"); r16=$(grep -c 'XS FAIL release' "$OUT/m16/check.log")
[ "$(cat $OUT/m16/rc)" = 0 ] && [ "$n16" = 2 ] && [ "$r16" = 0 ] \
  && ok "M16 B1's log line reworded (same calls): XS fails at B1 in lab + harness; release has no log" \
  || { bad "M16 (rc $(cat $OUT/m16/rc), lab/harness $n16, release $r16)"; head "$OUT/m16/check.log"; }
srcmut m17 "
k = [j for j, l in enumerate(L) if 'fe_evt(\"evt_drop total=%u\"' in l]; assert len(k) == 1, k
L[k[0]] = L[k[0]].replace('evt_drop total=%u', 'evt_drop n=%u')"
nsc=$(($(wc -l < "$OUT/base/lab/exit_scenarios.tsv") - 1))
n17=$(grep -c 'XS FAIL lab exit_scenarios.tsv' "$OUT/m17/check.log"); h17=$(grep -c 'XS FAIL harness exit_scenarios.tsv' "$OUT/m17/check.log")
[ "$n17" = "$nsc" ] && [ "$h17" = "$nsc" ] && ! grep -q 'XS FAIL release' "$OUT/m17/check.log" \
  && ok "M17 the teardown's evt_drop line reworded: XS fails in all $nsc scenarios, lab + harness" \
  || { bad "M17 (lab $n17, harness $h17 of $nsc)"; head -4 "$OUT/m17/check.log" | cut -c1-200; }
srcmut m18 "
g = [j for j, l in enumerate(L) if l.strip() in ('goto session_top;', '}   /* the next game */')]; assert len(g) == 1, g
assert L[g[0] - 1].strip().startswith('pwr_release();'), L[g[0] - 1]
L[g[0] - 1] = ''"
grep -q "HOLD main:.* goto session_top holding \['teardown'\]" "$OUT/m18/xs/report.txt" && [ "$(cat $OUT/m18/rc)" = 1 ] \
  && grep -q 'HOLD scenario LIST_OK: returns to the list holding 1' "$OUT/m18/xs/report.txt" \
  && ok "M18 the return's pwr_release deleted: hold check fails (the goto and the LIST_OK ledger)" \
  || { bad "M18"; grep -n 'HOLD\|hold' "$OUT/m18/xs/report.txt" | head; }
srcmut m19 "
f = [j for j, l in enumerate(L) if l.startswith('static int state_save_slot(')]; assert len(f) == 1, f
r = [j for j in range(f[0], f[0] + 16) if L[j].strip() == 'pwr_release();']; assert len(r) == 1, r
L[r[0]] = ''" fe/misc.a.inc.c
grep -q "HOLD state_save_slot:.* returns holding \['state_save'\]" "$OUT/m19/xs/report.txt" && [ "$(cat $OUT/m19/rc)" = 1 ] \
  && ok "M19 fragment mutant (--over fe/misc.a.inc.c): state_save_slot keeps its hold: hold check fails" \
  || { bad "M19"; grep -n 'HOLD\|hold' "$OUT/m19/xs/report.txt" | head; tail -3 "$OUT/m19/all.log"; }
srcmut m20 "
r = [j for j, l in enumerate(L) if l.strip().startswith('pwr_release();') and 'the boot' in l]; assert len(r) == 1, r
c = [j for j, l in enumerate(L) if 'fe_evt(\"exit code=4 reason=net_failed\")' in l]; assert len(c) == 1 and c[0] < r[0], (c, r)
k = c[0]
while 'net_bringup(' not in L[k]: k -= 1
L[k:k] = [L[r[0]]]; del L[r[0] + 1]"
n20=$(grep -c 'XS FAIL .* exit_sites.tsv C4$' "$OUT/m20/check.log")
[ "$n20" = 3 ] && grep -A2 'exit_sites.tsv C4$' "$OUT/m20/check.log" | grep -q 'hold=- ' \
  && ok "M20 the boot hold released before net_bringup: XS fails at C4 in 3 flavours (hold boot -> none)" \
  || { bad "M20 ($n20)"; head -6 "$OUT/m20/check.log" | cut -c1-240; tail -3 "$OUT/m20/all.log"; }

# ---- -E mutant: one call disappears from the harness -E output without a macro or #if
rm -rf "$OUT/m10"; mkdir -p "$OUT/m10/pp"; cp "$OUT/base/pp/"* "$OUT/m10/pp/"
$ED "$OUT/m10/pp/main_psp.harness.i" "$OUT/m10/pp/main_psp.harness.i" "
k = [i for i, l in enumerate(L) if l.strip() == 'me_rend_teardown(\"exit\");']
assert k; L[k[-1]] = ''          # the last one: the loop teardown's (HW-2 F9 adds C3/C4's above it)"
$EM all --baseline "$BASE" --src "$SRC" --out "$OUT/m10/xs" --pp-dir "$OUT/m10/pp" > "$OUT/m10/all.log" 2>&1
grep -q 'UNEXPLAINED D me_rend_teardown' "$OUT/m10/all.log" && ok "M10 unexplained raw-vs-E difference caught" \
  || { bad "M10"; grep 'raw vs' "$OUT/m10/all.log"; }

echo "abi-exit-ci T8 tests: $([ $FAILS = 0 ] && echo PASS || echo "$FAILS FAIL")"
exit $([ $FAILS = 0 ] && echo 0 || echo 1)
