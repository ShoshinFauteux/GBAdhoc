#!/usr/bin/env bash
# abi-exit-ci_test_strong.sh -- self-test and mutant tests of T6 strong_syms.py.
#   bash tests/abi-exit-ci_test_strong.sh <baseline run dir> <scratch out dir>
# Needs python3; L1-L3 also need docker (pspdev/pspdev) for a 2-file psp-gcc link
# (no PSP build, no SDK startup code).
#
#   C0  --selftest (map parser)
#   C1  the committed golden is green on all three baseline ELFs
#   C2  a golden regenerated from the baseline equals the committed one
#   M1  release ELF: gpsp_visible_done_hook binding GLOBAL -> WEAK        -> FAIL
#   M2  release ELF: g_running renamed in .strtab (definition "gone")     -> FAIL
#   M3  release ELF: in_rate made LOCAL (a static definition)             -> FAIL
#   M4  release map: .text.gpsp_rfu_now_us now owned by rfu.o (stub won)  -> FAIL
#   M5  release map: rfu.o's weak .text.gpsp_rfu_trace_hook gone (core renamed it) -> FAIL
#   M6  lab ELF: a derived-ABI global (pwr_hold) made LOCAL               -> FAIL
#   L1  real psp-gcc link, override defined strong                        -> ok
#   L2  real link, override made `static`                                 -> FAIL
#   L3  real link, override renamed (the weak stub links silently)        -> FAIL
set -u
BASE=${1:?baseline run dir}; OUT=${2:?scratch out dir}
HERE=$(cd "$(dirname "$0")/.." && pwd); SRC=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT"; FAILS=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
SS="python3 $HERE/strong_syms.py"; G="$HERE/strong_syms.golden.tsv"
MUT="python3 $HERE/tests/abi-exit-ci_elfmut.py"; ED="python3 $HERE/tests/abi-exit-ci_edit.py"
expect() {  # expect <want rc> <label> <cmd...>
  local want=$1 label=$2; shift 2
  "$@" > "$OUT/last.log" 2>&1; local rc=$?
  if [ $rc -eq $want ]; then ok "$label (exit $rc)$( [ $want = 1 ] && grep -m1 FAIL "$OUT/last.log" | sed 's/^ */: /')"
  else bad "$label: exit $rc, wanted $want"; tail -5 "$OUT/last.log"; fi
}
R=$BASE/release; L=$BASE/lab
expect 0 "C0 selftest" $SS --selftest
expect 0 "C1 golden green on release/lab/harness" $SS checkrun --run "$BASE" --golden "$G"
$SS golden --run "$BASE" --src "$SRC" --out "$OUT/golden2.tsv" > /dev/null
cmp -s "$G" "$OUT/golden2.tsv" && ok "C2 regenerated golden == committed" || bad "C2 golden differs"

$MUT "$R/gpsp_adhoc.elf" "$OUT/m1.elf" bind gpsp_visible_done_hook 2 > /dev/null
expect 1 "M1 hook binding WEAK" $SS check --elf "$OUT/m1.elf" --map "$R/gpsp_adhoc.map" --flavour release --golden "$G"
$MUT "$R/gpsp_adhoc.elf" "$OUT/m2.elf" rename g_running g_runnin > /dev/null
expect 1 "M2 g_running gone" $SS check --elf "$OUT/m2.elf" --map "$R/gpsp_adhoc.map" --flavour release --golden "$G"
$MUT "$R/gpsp_adhoc.elf" "$OUT/m3.elf" bind in_rate 0 > /dev/null
expect 1 "M3 in_rate LOCAL" $SS check --elf "$OUT/m3.elf" --map "$R/gpsp_adhoc.map" --flavour release --golden "$G"
$ED "$R/gpsp_adhoc.map" "$OUT/m4.map" "
k = [i for i, l in enumerate(L) if l.strip() == '.text.gpsp_rfu_now_us']
assert L[k[0]+1].strip().endswith('main_psp.o')
L[k[0]+1] = L[k[0]+1].replace('main_psp.o', '../gpsp_libretro_psp1.a(rfu.o)')"
expect 1 "M4 stub owns the hook's address" $SS check --elf "$R/gpsp_adhoc.elf" --map "$OUT/m4.map" --flavour release --golden "$G"
$ED "$R/gpsp_adhoc.map" "$OUT/m5.map" "
k = [i for i, l in enumerate(L) if l.strip() == '.text.gpsp_rfu_trace_hook' and 'rfu.o' in L[i+1]]
assert len(k) == 1; L[k[0]] = ' .text.gpsp_rfu_trace_hook_v2'"
expect 1 "M5 core weak stub gone" $SS check --elf "$R/gpsp_adhoc.elf" --map "$OUT/m5.map" --flavour release --golden "$G"
$MUT "$L/gpsp_adhoc.elf" "$OUT/m6.elf" bind pwr_hold 0 > /dev/null
expect 1 "M6 derived ABI pwr_hold LOCAL" $SS check --elf "$OUT/m6.elf" --map "$L/gpsp_adhoc.map" --flavour lab --golden "$G"

# ---- real links with psp-gcc: the failure x1 H6 describes, produced by the toolchain
if command -v docker > /dev/null; then
  K="$OUT/link"; rm -rf "$K"; mkdir -p "$K"
  printf '__attribute__((weak)) void gpsp_visible_done_hook(void) {}\nint core_frame(void) { gpsp_visible_done_hook(); return 0; }\n' > "$K/core.c"
  printf 'int hook_hits;\nvoid gpsp_visible_done_hook(void) { hook_hits++; }\n' > "$K/fe_ok.c"
  printf 'int hook_hits;\nstatic void gpsp_visible_done_hook(void) { hook_hits++; }\nvoid (*keep)(void) = gpsp_visible_done_hook;\n' > "$K/fe_static.c"
  printf 'int hook_hits;\nvoid gpsp_visible_done_hook2(void) { hook_hits++; }\n' > "$K/fe_renamed.c"
  printf 'gpsp_visible_done_hook hook all fe.o core.o\nhook_hits abi all fe.o\n' > "$K/spec"
  KP=$(cd "$K" && pwd)
  case "$KP" in /mnt/*) ;; *) ;; esac
  docker run --rm -u "$(id -u):$(id -g)" -v "$KP:/w" -w /w pspdev/pspdev sh -c '
    psp-gcc -O2 -G0 -ffunction-sections -c core.c -o core.o || exit 1
    for v in ok static renamed; do
      mkdir -p $v && cp fe_$v.c $v/fe.c && cd $v &&
      psp-gcc -O2 -G0 -ffunction-sections -c fe.c -o fe.o &&
      psp-gcc -nostdlib -nostartfiles -Wl,-e,core_frame -Wl,-Map,t.map -o t.elf fe.o ../core.o || exit 1
      cd ..
    done' > "$K/docker.log" 2>&1 || { bad "L* psp-gcc link failed"; tail -5 "$K/docker.log"; }
  expect 0 "L1 real link, strong override"  $SS check --elf "$K/ok/t.elf" --map "$K/ok/t.map" --flavour release --spec "$K/spec"
  expect 1 "L2 real link, static override"  $SS check --elf "$K/static/t.elf" --map "$K/static/t.map" --flavour release --spec "$K/spec"
  expect 1 "L3 real link, renamed override" $SS check --elf "$K/renamed/t.elf" --map "$K/renamed/t.map" --flavour release --spec "$K/spec"
else
  echo "  skip  L1-L3 (no docker)"
fi
echo "abi-exit-ci T6 tests: $([ $FAILS = 0 ] && echo PASS || echo "$FAILS FAIL")"
exit $([ $FAILS = 0 ] && echo 0 || echo 1)
