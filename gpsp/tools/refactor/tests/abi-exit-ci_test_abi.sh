#!/usr/bin/env bash
# abi-exit-ci_test_abi.sh -- self-test and mutant tests of T14 abi_asserts.py.
#   bash tests/abi-exit-ci_test_abi.sh <baseline run dir> <scratch out dir>
# Needs docker (pspdev/pspdev): psp-gcc -E and -fsyntax-only only, no build.  Mutants are
# COPIES (main_psp.c, main.h, test_ff_pipeline.c) bind-mounted over the read-only tree.
#   C1  release / lab / harness: fe_abi_asserts.h generated and compiles with the core flags
#   C2  strict (no abi_asserts.known): exactly the 7 known mismatches fail, nothing else
#   M1  main_psp.c: one core_phase_take parameter unsigned* -> int*        -> assert fails
#   M2  main.h:     the core's core_phase_take loses its last parameter     -> assert fails
#   M3  main_psp.c: extern u32 flush_ram_full -> u16                       -> assert fails
#   M4  main_psp.c: extern u32 cph_jit_total gains `const` (qualifier)      -> assert fails
#   M5  main_psp.c: VRAM_DIRTY_SHIFT 10 -> 11                               -> assert fails
#   M6  test_ff_pipeline.c: MER_RETIRE_US 9000u -> 9001u                    -> assert fails
#   M7  main_psp.c: the KNOWN `reg` copy becomes unsigned short[]          -> assert fails
#       (a known normalisation does not hide new drift)
#   M8  main_psp.c: a pinned copy retyped to a frontend-only u16 typedef   -> PIN FAIL
#   M9  psp/sleep_q.h: SQ_NONE 1 -> 2                                      -> assert fails
set -u
BASE=${1:?baseline run dir}; OUT=${2:?scratch out dir}
HERE=$(cd "$(dirname "$0")/.." && pwd); SRC=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT"; FAILS=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
AA="python3 $HERE/abi_asserts.py"; ED="python3 $HERE/tests/abi-exit-ci_edit.py"
R=$BASE/release
for f in release lab harness; do
  $AA gen --flavour-dir "$BASE/$f" --src "$SRC" --out "$OUT/base/$f" > "$OUT/c1_$f.log" 2>&1 \
    && grep -q '^pin: [0-9]* pinned, 0 lost, 0 new' "$OUT/c1_$f.log" \
    && ok "C1 $f: $(head -1 "$OUT/c1_$f.log"); $(grep '^pin:' "$OUT/c1_$f.log")" || { bad "C1 $f"; cat "$OUT/c1_$f.log" | head; }
done
printf '# empty: strict mode\n' > "$OUT/none.known"
$AA gen --flavour-dir "$R" --src "$SRC" --out "$OUT/strict" --known "$OUT/none.known" > "$OUT/c2.log" 2>&1; rc=$?
# P0a T14: every copy SITE is asserted, so a name with two copies (translation_gate_*)
# fails twice; the known list is per name, so count the distinct names that fail
n=$(grep -o 'static assertion failed: "main_psp.c:[0-9]* [A-Za-z_0-9]*' "$OUT/c2.log" | awk '{print $NF}' | sort -u | wc -l)
[ $rc = 1 ] && [ "$n" = 7 ] && ok "C2 strict: exactly 7 names fail a static assertion (the abi_asserts.known list)" \
  || { bad "C2 strict rc=$rc n=$n"; grep 'assertion' "$OUT/c2.log"; }

mutant() {  # mutant <label> <name in message> <gen extra args...>
  local label=$1 want=$2; shift 2
  $AA gen --flavour-dir "$R" --src "$SRC" --out "$OUT/mut" "$@" > "$OUT/mut.log" 2>&1; local rc=$?
  if [ $rc = 1 ] && grep -q "static assertion failed: .*$want" "$OUT/mut.log"; then
    ok "$label: $(grep -m1 'static assertion failed' "$OUT/mut.log" | sed 's/.*error: //' | cut -c1-100)"
  else bad "$label (rc=$rc)"; grep -m5 'error\|T14' "$OUT/mut.log"; fi
}
M="$OUT/main_psp.c"
$ED "$SRC/psp/main_psp.c" "$M" "
k = [i for i, l in enumerate(L) if l.startswith('void core_phase_take(unsigned *lvl, unsigned *clk_ns, unsigned *frames,')]
assert len(k) == 1; L[k[0]] = L[k[0]].replace('unsigned *frames', 'int *frames')"
mutant "M1 frontend core_phase_take param type" "core_phase_take" --main-c "$M"
$ED "$SRC/main.h" "$OUT/main.h" "
k = [i for i, l in enumerate(L) if l.startswith('void core_phase_take(')]
assert len(k) == 1
j = next(i for i in range(k[0], k[0] + 12) if L[i].rstrip().endswith(');'))
import re; L[j] = re.sub(r',\s*unsigned \*\w+\);', ');', L[j]) if re.search(r',\s*unsigned \*\w+\);', L[j]) else L[j]
assert L[j].rstrip().endswith(');')"
grep -q 'core_phase_take' "$OUT/main.h" && mutant "M2 core header drops a parameter" "core_phase_take" --core-h "$OUT/main.h" main.h
$ED "$SRC/psp/main_psp.c" "$M" "
k = [i for i, l in enumerate(L) if l.strip() == 'extern u32 flush_ram_full;']; assert len(k) == 1
L[k[0]] = 'extern u16 flush_ram_full;'"
mutant "M3 extern var type" "flush_ram_full" --main-c "$M"
$ED "$SRC/psp/main_psp.c" "$M" "
k = [i for i, l in enumerate(L) if l.startswith('extern u32 cph_jit_total;')]; assert len(k) == 1
L[k[0]] = L[k[0]].replace('extern u32', 'extern const u32')"
mutant "M4 qualifier added" "cph_jit_total" --main-c "$M"
$ED "$SRC/psp/main_psp.c" "$M" "
k = [i for i, l in enumerate(L) if l.startswith('#define VRAM_DIRTY_SHIFT 10')]; assert len(k) == 1
L[k[0]] = '#define VRAM_DIRTY_SHIFT 11'"
mutant "M5 VRAM_DIRTY_SHIFT copy" "VRAM_DIRTY_SHIFT" --main-c "$M"
$ED "$SRC/tools/test_ff_pipeline.c" "$OUT/test_ff_pipeline.c" "
k = [i for i, l in enumerate(L) if l.startswith('#define MER_RETIRE_US 9000u')]; assert len(k) == 1
L[k[0]] = '#define MER_RETIRE_US 9001u'"
mutant "M6 test copy of MER_RETIRE_US" "MER_RETIRE_US" --test-c "$OUT/test_ff_pipeline.c"
$ED "$SRC/psp/main_psp.c" "$M" "
k = [i for i, l in enumerate(L) if l.startswith('extern unsigned int   reg[];')]; assert len(k) == 1
L[k[0]] = 'extern unsigned short reg[];'"
mutant "M7 new drift on a KNOWN entry" "reg" --main-c "$M"

# ---- verifier mutants (2026-11-05)
#   M8  main_psp.c: `extern u32 flush_ram_full` -> `extern fe_count_t flush_ram_full` with
#       fe_count_t = unsigned short (a frontend-only typedef): the discovery pass used to
#       move it to UNCHECKED and pass (rc 0); the pin must fail it
#   M9  psp/sleep_q.h: SQ_NONE 1 -> 2                                      -> assert fails
$ED "$SRC/psp/main_psp.c" "$M" "
k = [i for i, l in enumerate(L) if l.strip() == 'extern u32 flush_ram_full;']; assert len(k) == 1
L[k[0]] = 'typedef unsigned short fe_count_t;'
L.insert(k[0] + 1, 'extern fe_count_t flush_ram_full;')"
$AA gen --flavour-dir "$R" --src "$SRC" --out "$OUT/mut" --main-c "$M" > "$OUT/mut.log" 2>&1; rc=$?
[ $rc = 1 ] && grep -q 'PIN FAIL flush_ram_full' "$OUT/mut.log" \
  && ok "M8 copy retyped to a frontend typedef: $(grep -m1 'PIN FAIL' "$OUT/mut.log" | sed 's/^ *//' | cut -c1-100)" \
  || { bad "M8 (rc=$rc)"; grep -m5 'PIN\|T14\|pin' "$OUT/mut.log"; }
$ED "$SRC/psp/sleep_q.h" "$OUT/sleep_q.h" "
k = [i for i, l in enumerate(L) if l.startswith('#define SQ_NONE  1')]; assert len(k) == 1
L[k[0]] = '#define SQ_NONE  2'"
mutant "M9 SQ_NONE changed" "SQ_DONE/SQ_NONE" --core-h "$OUT/sleep_q.h" psp/sleep_q.h

echo "abi-exit-ci T14 tests: $([ $FAILS = 0 ] && echo PASS || echo "$FAILS FAIL")"
exit $([ $FAILS = 0 ] && echo 0 || echo 1)
