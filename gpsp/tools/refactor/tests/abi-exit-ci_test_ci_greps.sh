#!/usr/bin/env bash
# abi-exit-ci_test_ci_greps.sh -- self-test and mutant tests of T11 ci_greps.sh.
#   bash tests/abi-exit-ci_test_ci_greps.sh <scratch out dir> [<pp dir with main_psp.*.i>]
# Mutants are made in a COPY of psp/, frontend-common/ and netdrv/ (never the tree).
#   C1  base, phase base (+ R1e if a pp dir is given): green, the two KNOWN R4 sites only
#   C2  phase P on base: the P3 work list (every raw exit/handoff in main_psp.c) -> FAIL
#   C3  phase HW2 on base: one fewer (stall_exit whitelisted)               -> FAIL, n-1
#   M1  R1  a new module calls sceKernelExitGame() raw                     -> FAIL
#   M2  R1  main_psp.c calls handoff_run before the hijack macro            -> FAIL
#   M3  R1e one -E exit call loses its pwr_release_all() wrapper            -> FAIL
#   M4  R2  noinline,cold function without section; control with section    -> FAIL / ok
#   M5  R2  noreturn prototype without section                              -> FAIL
#   M6  R3  pwr_hold directly in gpsp_visible_done_hook                     -> FAIL
#   M7  R3  pwr_hold two calls deep from gpsp_rfu_trace_hook                -> FAIL
#   M8  R4  assignment / mutating call / unreviewed call in fe_evt args     -> FAIL x3
#   M9  R5  a .c tests GPSP_NO_TELEMETRY before including fe_evt.h; control -> FAIL / ok
# verifier mutants (2026-11-05; each passed the first version):
#   M1b R1  raw sceKernelExitGame inside a #define of another header       -> FAIL
#   M1c R1  sceKernelExitGame taken by address                              -> FAIL
#   M3b R1e raw `rc ? 0 : sceKernelExitGame()` in lab -E                    -> FAIL
#   M3c R1e unwrapped exit in an #included .inc.c fragment (release -E)     -> FAIL
#   M7b R3  hook -> function in another fragment -> pwr_hold                -> FAIL
#   M7c R3  hook -> macro whose body calls pwr_hold                         -> FAIL
#   M8b R4  a macro forwarding to fe_evt (its arguments escape R4)          -> FAIL
# Epoch C (PLAN-EPOCHC-beta5 §4.2, the C0a-6 probe):
#   M10  R6 a new static function only main() calls, no attributes          -> FAIL
#   M10b R6 the same with noinline but no section                           -> FAIL
#   M10c R6 the same with noinline + section (control)                      -> ok
#   M10d R6 a second caller outside main (control: not main-only)           -> ok
#   M10e R6 always_inline (Epoch C stage s, pasted by the compiler; control) -> ok
#   M10f R6 always_inline together with noinline                            -> FAIL
# The tree may be split (psp/main_psp.c + psp/fe/*.inc.c) and P3 may have landed (no
# hijack #define; FE_EXIT_GAME/FE_HANDOFF in fe/fe_priv.h): every edit finds its file
# and its line by anchor.
set -u
OUT=${1:?scratch out dir}; PP=${2:-}
HERE=$(cd "$(dirname "$0")/.." && pwd); SRC=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT"; FAILS=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
CG="bash $HERE/ci_greps.sh"; ED="python3 $HERE/tests/abi-exit-ci_edit.py"
expect() {  # expect <rc> <label> <grep that must match the output, or -> <cmd...>
  local want=$1 label=$2 pat=$3; shift 3
  "$@" > "$OUT/last.log" 2>&1; local rc=$?
  if [ $rc -ne $want ]; then bad "$label: exit $rc, wanted $want"; tail -6 "$OUT/last.log"; return; fi
  if [ "$pat" != - ] && ! grep -q -- "$pat" "$OUT/last.log"; then bad "$label: no '$pat'"; tail -6 "$OUT/last.log"; return; fi
  ok "$label (exit $rc)$( [ "$pat" != - ] && echo ": $(grep -m1 -- "$pat" "$OUT/last.log" | sed 's/^ *//' | cut -c1-110)")"
}
PPARG=(); [ -n "$PP" ] && PPARG=(--pp "$PP")
expect 0 "C1 base green" "T11: green" $CG --src "$SRC" "${PPARG[@]}"
# the R4 KNOWN rows are whatever ci_greps.known still lists (HW-2 fixes both sites)
nK=$(grep -c '^R4' "$HERE/ci_greps.known")
grep -c '^  KNOWN' "$OUT/last.log" | grep -q "^$nK\$" && ok "C1 exactly $nK KNOWN (the R4 rows of ci_greps.known)" || bad "C1 KNOWN count (want $nK)"
[ -n "$PP" ] && grep 'R1e' "$OUT/last.log" | sed 's/^/        /'
P3=0; grep -q '^#define FE_EXIT_GAME()' "$SRC/psp/fe/fe_priv.h" 2>/dev/null && P3=1
$CG --src "$SRC" --phase P --rules R1 > "$OUT/p.log" 2>&1; rcP=$?
nP=$(grep 'FAIL  R1 ' "$OUT/p.log" | grep -vc '@#define'); nPm=$(grep 'FAIL  R1 psp/main_psp.c' "$OUT/p.log" | grep -vc '@#define')
nPd=$(grep 'FAIL  R1 ' "$OUT/p.log" | grep -c '@#define')
$CG --src "$SRC" --phase HW2 --rules R1 > "$OUT/h.log" 2>&1; rcH=$?
nH=$(grep 'FAIL  R1 ' "$OUT/h.log" | grep -vc '@#define')
if [ $P3 = 1 ]; then
  # P3 has landed: the P contract holds as it stands (the only raw exit, if any, is stall_exit's)
  [ "$nPd" = 0 ] && [ "$nP" -le 1 ] && [ $rcH = 0 ] && [ "$nH" = 0 ] \
    && ok "C2/C3 P3 has landed: phase P leaves $nP raw call (stall_exit, if HW-2 made it raw), phase HW2 is green" \
    || bad "C2/C3 after P3 ($nP calls, $nPd defines; HW2 rc $rcH, $nH)"
else
  [ $rcP = 1 ] && [ "$nPd" = 2 ] && ok "C2 phase P on base: $nP raw calls to convert in P3 ($nPm in main_psp.c) + the $nPd hijack #defines that move to fe_priv.h" || bad "C2 ($nP calls, $nPd defines)"
  [ "$nH" = $((nP - 1)) ] && ok "C3 phase HW2: $nH (stall_exit whitelisted)" || bad "C3 ($nH vs $nP)"
fi

# ---- the mutant tree
T="$OUT/tree"; rm -rf "$T"; mkdir -p "$T"
for d in psp frontend-common netdrv; do
  (cd "$SRC" && git ls-files -z "$d" | grep -z -E '\.(c|h|inc)$' | xargs -0 cp --parents -t "$T") 2>/dev/null \
   || (cd "$SRC" && find "$d" -name '*.[ch]' -exec cp --parents -t "$T" {} +)
done
reset_tree() {
  cp "$SRC/psp/main_psp.c" "$T/psp/main_psp.c"
  [ -d "$SRC/psp/fe" ] && cp "$SRC"/psp/fe/*.inc.c "$T/psp/fe/"
  rm -f "$T/psp/fe_mut_"*.c
}
reset_tree
# the file of the mutant tree that DEFINES a function (the shell or one of its fragments)
deffile() { grep -l "^$1" "$T/psp/main_psp.c" "$T"/psp/fe/*.inc.c 2>/dev/null | head -1; }
expect 0 "C1' copy of the tree is green" "T11: green" $CG --src "$T"

printf '#include "fe_evt.h"\nvoid fe_mut_quit(void) { sceKernelExitGame(); }\n' > "$T/psp/fe_mut_a.c"
expect 1 "M1 raw exit in a new module" "FAIL  R1 psp/fe_mut_a.c" $CG --src "$T" --rules R1; reset_tree

$ED "$T/psp/main_psp.c" "$T/psp/main_psp.c" "
k = [i for i, l in enumerate(L) if l.startswith('#define handoff_run(code, why)')]
k = k or [i for i, l in enumerate(L) if l.startswith('int main(')]      # after P3: any raw call
assert len(k) == 1, k
L.insert(k[0], 'static void fe_mut_early(void) { handoff_run(0, 0); }')"
expect 1 "M2 raw handoff_run in main_psp.c (before the hijack; after P3: anywhere)" "FAIL  R1 psp/main_psp.c" $CG --src "$T" --rules R1; reset_tree

if [ -n "$PP" ]; then
  mkdir -p "$OUT/pp_m3"; cp "$PP"/main_psp.*.i "$OUT/pp_m3/"
  $ED "$OUT/pp_m3/main_psp.harness.i" "$OUT/pp_m3/main_psp.harness.i" "
k = [i for i, l in enumerate(L) if l.strip() == '(pwr_release_all(), sceKernelExitGame());']
assert k; L[k[3]] = L[k[3]].replace('(pwr_release_all(), sceKernelExitGame())', '(sceKernelExitGame())')"
  expect 1 "M3 unwrapped exit in harness -E" "FAIL  R1e psp/main_psp.c\[harness -E\]" $CG --src "$SRC" --rules R1 --pp "$OUT/pp_m3"
else echo "  skip  M3 (no pp dir)"; fi

# ---- verifier mutants (2026-11-05): ways around R1/R1e/R4 the first version missed
printf '#define FE_QUIT() do { sceKernelExitGame(); } while (0)\n' > "$T/psp/fe_mut_g.h"
expect 1 "M1b raw exit inside a #define of another header" "FAIL  R1 psp/fe_mut_g.h" $CG --src "$T" --rules R1; rm -f "$T/psp/fe_mut_g.h"
printf '#include "fe_evt.h"\nvoid (*fe_mut_q)(void) = sceKernelExitGame;\n' > "$T/psp/fe_mut_h.c"
expect 1 "M1c exit taken by address" "FAIL  R1 psp/fe_mut_h.c" $CG --src "$T" --rules R1; reset_tree
if [ -n "$PP" ]; then
  mkdir -p "$OUT/pp_m3b"; cp "$PP"/main_psp.*.i "$OUT/pp_m3b/"
  $ED "$OUT/pp_m3b/main_psp.lab.i" "$OUT/pp_m3b/main_psp.lab.i" "
k = [i for i, l in enumerate(L) if l.strip() == '(pwr_release_all(), sceKernelExitGame());']
assert k; L[k[2]] = L[k[2]].replace('(pwr_release_all(), sceKernelExitGame())', '(rc ? 0 : sceKernelExitGame())')"
  expect 1 "M3b raw exit in a ternary in lab -E" "FAIL  R1e psp/main_psp.c\[lab -E\]" $CG --src "$SRC" --rules R1 --pp "$OUT/pp_m3b"
  mkdir -p "$OUT/pp_m3c"; cp "$PP"/main_psp.*.i "$OUT/pp_m3c/"
  printf '# 1 "fe/fe_quit.inc.c" 1\nstatic void fe_q(void) { sceKernelExitGame(); }\n' >> "$OUT/pp_m3c/main_psp.release.i"
  expect 1 "M3c unwrapped exit in an included fragment (release -E)" "FAIL  R1e psp/main_psp.c\[release -E\]" $CG --src "$SRC" --rules R1 --pp "$OUT/pp_m3c"
fi
printf 'static void fe_mut_far(void) { pwr_hold("far"); }\n' > "$T/psp/fe_mut_r.inc.c"
HF=$(deffile "void gpsp_visible_done_hook(void)"); $ED "$HF" "$HF" "
k = [i for i, l in enumerate(L) if l.startswith('void gpsp_visible_done_hook(void)')][0]
assert L[k+1].strip() == '{'; L[k+1] = '{ fe_mut_far();'"
expect 1 "M7b pwr_hold across fragments" "gpsp_visible_done_hook -> fe_mut_far -> pwr_hold" $CG --src "$T" --rules R3; rm -f "$T/psp/fe_mut_r.inc.c"; reset_tree
HF=$(deffile "void gpsp_visible_done_hook(void)"); $ED "$HF" "$HF" "
k = [i for i, l in enumerate(L) if l.startswith('void gpsp_visible_done_hook(void)')][0]
assert L[k+1].strip() == '{'; L[k+1] = '{ FE_MUT_HOLD();'
L.insert(k, '#define FE_MUT_HOLD() pwr_hold(\"m\")')"
expect 1 "M7c pwr_hold through a macro" "gpsp_visible_done_hook -> FE_MUT_HOLD -> pwr_hold" $CG --src "$T" --rules R3; reset_tree
printf '#include "fe_evt.h"\n#define FE_TRACE(...) fe_evt(__VA_ARGS__)\nvoid fe_mut_t(void) { FE_TRACE("x", audio_stop()); }\n' > "$T/psp/fe_mut_t.c"
expect 1 "M8b macro forwarding to fe_evt" "FAIL  R4 psp/fe_mut_t.c" $CG --src "$T" --rules R4; reset_tree

printf '#include "fe_evt.h"\nstatic void __attribute__((noinline, cold)) fe_boot_x(void) { }\nvoid fe_mut_b(void) { fe_boot_x(); }\n' > "$T/psp/fe_mut_b.c"
expect 1 "M4 cold without section" "FAIL  R2 psp/fe_mut_b.c" $CG --src "$T" --rules R2
printf '#include "fe_evt.h"\nstatic void __attribute__((noinline, cold, section(".text.startup.fe_boot"))) fe_boot_x(void) { }\nvoid fe_mut_b(void) { fe_boot_x(); }\n' > "$T/psp/fe_mut_b.c"
expect 0 "M4 control: cold WITH section" "0 without section" $CG --src "$T" --rules R2; reset_tree
printf '__attribute__((noreturn)) void fe_mut_die(void);\n' > "$T/psp/fe_mut_c.c"
expect 1 "M5 noreturn without section" "FAIL  R2 psp/fe_mut_c.c" $CG --src "$T" --rules R2; reset_tree

HF=$(deffile "void gpsp_visible_done_hook(void)"); $ED "$HF" "$HF" "
k = [i for i, l in enumerate(L) if l.startswith('void gpsp_visible_done_hook(void)')][0]
assert L[k+1].strip() == '{'; L[k+1] = '{ pwr_hold(\"mut\");'"
expect 1 "M6 pwr_hold in a hook" "pwr_hold reachable: gpsp_visible_done_hook -> pwr_hold" $CG --src "$T" --rules R3; reset_tree
HF=$(deffile "void gpsp_rfu_trace_hook("); $ED "$HF" "$HF" "
k = [i for i, l in enumerate(L) if l.startswith('void gpsp_rfu_trace_hook(')][0]
b = next(i for i in range(k, k + 5) if L[i].strip() == '{'); L[b] = '{ fe_mut_mid();'
L.insert(k, 'static void fe_mut_leaf(void) { pwr_hold(\"deep\"); }')
L.insert(k + 1, 'static void fe_mut_mid(void) { fe_mut_leaf(); }')"
expect 1 "M7 pwr_hold two calls deep" "gpsp_rfu_trace_hook -> fe_mut_mid -> fe_mut_leaf -> pwr_hold" $CG --src "$T" --rules R3; reset_tree

printf '#include "fe_evt.h"\nstatic int g_x;\nvoid fe_mut_e(void) {\n fe_evt("a=%%d", g_x = 3);\n fe_evt("b", audio_stop());\n fe_evt("c=%%u", brand_new_getter());\n}\n' > "$T/psp/fe_mut_e.c"
$CG --src "$T" --rules R4 > "$OUT/m8.log" 2>&1; rc=$?
n8=$(grep -c 'FAIL  R4 psp/fe_mut_e.c' "$OUT/m8.log")
[ $rc = 1 ] && [ "$n8" = 3 ] && ok "M8 fe_evt args: assignment, audio_stop(), unreviewed getter all caught" || { bad "M8 ($n8)"; grep FAIL "$OUT/m8.log"; }
reset_tree

printf '#ifndef GPSP_NO_TELEMETRY\nstatic int t;\n#endif\n#include "fe_evt.h"\n' > "$T/psp/fe_mut_f.c"
expect 1 "M9 GPSP_NO_TELEMETRY tested before fe_evt.h" "FAIL  R5 psp/fe_mut_f.c" $CG --src "$T" --rules R5
printf '#include "fe_evt.h"\n#ifndef GPSP_NO_TELEMETRY\nstatic int t;\n#endif\n' > "$T/psp/fe_mut_f.c"
expect 0 "M9 control: include first" "0 too early" $CG --src "$T" --rules R5; reset_tree

# ---- R6 (Epoch C): a new function that only main() calls
r6mut() {  # r6mut <declaration prefix> [<extra line>]
  $ED "$T/psp/main_psp.c" "$T/psp/main_psp.c" "
m = [i for i, l in enumerate(L) if l.startswith('int main(')]; assert len(m) == 1, m
c = [i for i, l in enumerate(L) if l.strip() == 'setup_exit_callback();' and i > m[0]]; assert c, c
L.insert(c[0] + 1, '   fe_mut_boot_run(argc);')
L[m[0]:m[0]] = ['$1 void fe_mut_boot_run(int x) { (void)x; }'] + ([\"\"\"${2:-}\"\"\"] if \"\"\"${2:-}\"\"\" else [])"
}
r6mut 'static'
expect 1 "M10 a new main-only static, no attributes" "it has no noinline and section() .fe_mut_boot_run." $CG --src "$T" --rules R6; reset_tree
r6mut 'static __attribute__((noinline))'
expect 1 "M10b the same with noinline only" "only main() calls it and it has no section()" $CG --src "$T" --rules R6; reset_tree
r6mut 'static __attribute__((noinline, section(\".text.fe_cold.fe_mut_boot_run\")))'
expect 0 "M10c control: noinline + section" "0 new without" $CG --src "$T" --rules R6; reset_tree
r6mut 'static' 'void fe_mut_other(void) { fe_mut_boot_run(1); }'
expect 0 "M10d control: a second caller outside main (not main-only)" "0 new without" $CG --src "$T" --rules R6; reset_tree
r6mut 'static inline __attribute__((always_inline))'
expect 0 "M10e control: always_inline (Epoch C stage s)" "0 new without" $CG --src "$T" --rules R6; reset_tree
r6mut 'static __attribute__((always_inline, noinline))'
expect 1 "M10f always_inline together with noinline" "only main() calls it and it has no section()" $CG --src "$T" --rules R6; reset_tree

echo "abi-exit-ci T11 tests: $([ $FAILS = 0 ] && echo PASS || echo "$FAILS FAIL")"
exit $([ $FAILS = 0 ] && echo 0 || echo 1)
