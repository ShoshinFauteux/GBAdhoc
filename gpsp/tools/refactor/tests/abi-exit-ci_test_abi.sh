#!/usr/bin/env bash
# abi-exit-ci_test_abi.sh -- self-test and mutant tests of T14 abi_asserts.py.
#   bash tests/abi-exit-ci_test_abi.sh <baseline run dir> <scratch out dir>
# Needs docker (pspdev/pspdev): psp-gcc -E and -fsyntax-only only, no build.  Mutants are
# COPIES (main_psp.c, main.h, test_ff_pipeline.c) bind-mounted over the read-only tree.
#   C1  release / lab / harness: fe_abi_asserts.h generated, compiles with the core flags, and
#       the core_fe.h guard is clean (code-health C6-7: the pin file is gone)
#   C2  strict (no abi_asserts.known): exactly the known mismatches fail (gamepak_must_swap until
#       C6-4 lands, then none)
#   M1  main_psp.c: a copy of a core_fe.h name added (extern u32 flush_rom_total)  -> GUARD FAIL
#   M2  main_psp.c: the same copy retyped (extern u16 flush_rom_total)             -> assert fails
#   M3  main_psp.c: a translation-cache copy (cpu.h-typed, kept) retyped u32 -> u16 -> assert fails
#   M5  main_psp.c: VRAM_DIRTY_SHIFT 10 -> 11                               -> assert fails
#   M6  test_ff_pipeline.c: MER_RETIRE_US 9000u -> 9001u                    -> assert fails
#   M9  psp/sleep_q.h: SQ_NONE 1 -> 2                                      -> assert fails
set -u
BASE=${1:?baseline run dir}; OUT=${2:?scratch out dir}
HERE=$(cd "$(dirname "$0")/.." && pwd); SRC=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT"; FAILS=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
AA="python3 $HERE/abi_asserts.py"; ED="python3 $HERE/tests/abi-exit-ci_edit.py"
R=$BASE/release
# The single-file text the main_psp.c mutants edit: psp/main_psp.c itself, or on a split tree
# (PLAN-P-beta5 P4) its split.py inverse, laid over the shell with --main-c (the same TU; the
# fragment-level mutants with --frag are tests/p0a_b5.sh t14's).  P-review 2026-11-08.
MSRC="$SRC/psp/main_psp.c"
if grep -q '^#include "fe/[A-Za-z0-9_.]*\.inc\.c"' "$MSRC"; then
  python3 "$HERE/split.py" inverse --split "$SRC" -o "$OUT/inverse_main_psp.c" > /dev/null || { echo "  FAIL  split.py inverse"; exit 1; }
  MSRC="$OUT/inverse_main_psp.c"; echo "  (split tree: main_psp.c mutants edit its inverse, $MSRC)"
fi
for f in release lab harness; do
  $AA gen --flavour-dir "$BASE/$f" --src "$SRC" --out "$OUT/base/$f" > "$OUT/c1_$f.log" 2>&1 \
    && grep -q '^guard (no copy of a core_fe.h name, [0-9]* names): ok' "$OUT/c1_$f.log" \
    && ok "C1 $f: $(head -1 "$OUT/c1_$f.log"); $(grep '^guard' "$OUT/c1_$f.log")" || { bad "C1 $f"; cat "$OUT/c1_$f.log" | head; }
done
printf '# empty: strict mode\n' > "$OUT/none.known"
$AA gen --flavour-dir "$R" --src "$SRC" --out "$OUT/strict" --known "$OUT/none.known" > "$OUT/c2.log" 2>&1; rc=$?
# P0a T14: every copy SITE is asserted, so a name with two copies (translation_gate_*)
# fails twice; the known list is per name, so count the distinct names that fail (a cite is
# main_psp.c:N, or fe/X.inc.c:N on a split tree)
n=$(grep -oE 'static assertion failed: "(main_psp\.c|fe/[A-Za-z0-9_.]+):[0-9]* [A-Za-z_0-9]*' "$OUT/c2.log" | awk '{print $NF}' | sort -u | wc -l)
nk=$(grep -vc '^#' "$HERE/abi_asserts.known")
[ "$n" = "$nk" ] && { [ "$nk" = 0 ] || [ $rc = 1 ]; } && ok "C2 strict: exactly $nk name(s) fail a static assertion (the abi_asserts.known list)" \
  || { bad "C2 strict rc=$rc n=$n"; grep 'assertion' "$OUT/c2.log"; }

mutant() {  # mutant <label> <name in message> <gen extra args...>
  local label=$1 want=$2; shift 2
  $AA gen --flavour-dir "$R" --src "$SRC" --out "$OUT/mut" "$@" > "$OUT/mut.log" 2>&1; local rc=$?
  if [ $rc = 1 ] && grep -q "static assertion failed: .*$want" "$OUT/mut.log"; then
    ok "$label: $(grep -m1 'static assertion failed' "$OUT/mut.log" | sed 's/.*error: //' | cut -c1-100)"
  else bad "$label (rc=$rc)"; grep -m5 'error\|T14' "$OUT/mut.log"; fi
}
M="$OUT/main_psp.c"
# M1/M2 add a copy right after the shell's #include "core_fe.h" (its comment runs on: the
# copy goes after the line that closes it)
ADD="
k = [i for i, l in enumerate(L) if l.startswith('#include \"core_fe.h\"')]; assert len(k) == 1
j = k[0]
while '*/' not in L[j]: j += 1
L.insert(j + 1, COPY)"
$ED "$MSRC" "$M" "COPY = 'extern u32 flush_rom_total;'$ADD"
$AA gen --flavour-dir "$R" --src "$SRC" --out "$OUT/mut" --main-c "$M" > "$OUT/mut.log" 2>&1; rc=$?
[ $rc = 1 ] && grep -q 'GUARD FAIL flush_rom_total' "$OUT/mut.log" \
  && ok "M1 a copy of a core_fe.h name: $(grep -m1 'GUARD FAIL' "$OUT/mut.log" | sed 's/^ *//' | cut -c1-100)" \
  || { bad "M1 (rc=$rc)"; grep -m5 'GUARD\|guard\|T14' "$OUT/mut.log"; }
$ED "$MSRC" "$M" "COPY = 'extern u16 flush_rom_total;'$ADD"
mutant "M2 a retyped copy of a core_fe.h name" "flush_rom_total" --main-c "$M"
$ED "$MSRC" "$M" "
k = [i for i, l in enumerate(L) if l.strip() == 'extern u32 rom_translation_cache_size;']; assert len(k) == 1
L[k[0]] = 'extern u16 rom_translation_cache_size;'"
mutant "M3 a kept cpu.h copy retyped" "rom_translation_cache_size" --main-c "$M"
$ED "$MSRC" "$M" "
k = [i for i, l in enumerate(L) if l.startswith('#define VRAM_DIRTY_SHIFT 10')]; assert len(k) == 1
L[k[0]] = '#define VRAM_DIRTY_SHIFT 11'"
mutant "M5 VRAM_DIRTY_SHIFT copy" "VRAM_DIRTY_SHIFT" --main-c "$M"
$ED "$SRC/tools/test_ff_pipeline.c" "$OUT/test_ff_pipeline.c" "
k = [i for i, l in enumerate(L) if l.startswith('#define MER_RETIRE_US 9000u')]; assert len(k) == 1
L[k[0]] = '#define MER_RETIRE_US 9001u'"
mutant "M6 test copy of MER_RETIRE_US" "MER_RETIRE_US" --test-c "$OUT/test_ff_pipeline.c"
$ED "$SRC/psp/sleep_q.h" "$OUT/sleep_q.h" "
k = [i for i, l in enumerate(L) if l.startswith('#define SQ_NONE  1')]; assert len(k) == 1
L[k[0]] = '#define SQ_NONE  2'"
mutant "M9 SQ_NONE changed" "SQ_DONE/SQ_NONE" --core-h "$OUT/sleep_q.h" psp/sleep_q.h

echo "abi-exit-ci T14 tests: $([ $FAILS = 0 ] && echo PASS || echo "$FAILS FAIL")"
exit $([ $FAILS = 0 ] && echo 0 || echo 1)
