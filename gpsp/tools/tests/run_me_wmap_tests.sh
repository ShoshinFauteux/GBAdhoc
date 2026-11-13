#!/usr/bin/env bash
# run_me_wmap_tests.sh -- the ME mid-frame log write maps' host test
# (tools/tests/test_me_wmap.c) and its mutants: the real build must PASS and
# every ME_WMAP_MUTANT (gba_memory.c / gba_memory.h) must FAIL, or the test
# would not catch that kind of miss.  Run from gpsp/ (Linux/WSL gcc).
set -u
CC=${CC:-gcc}
T=${TMPDIR:-/tmp}/t_me_wmap.$$
FLAGS="-std=gnu99 -O1 -g -w -ffunction-sections -fdata-sections -DINLINE=inline
       -Ilibretro/libretro-common/include -Ifrontend-common"
rc=0
build() { $CC $FLAGS "$@" -o "$T" tools/tests/test_me_wmap.c -Wl,--gc-sections \
            -Wl,--allow-multiple-definition; }
build -fsanitize=address,undefined || { echo "FAIL: build"; exit 1; }
ASAN_OPTIONS=detect_leaks=0 "$T" || rc=1
for m in 1 2 3 4 5 6; do
  build -DME_WMAP_MUTANT=$m || { echo "FAIL: mutant $m does not build"; rc=1; continue; }
  if "$T" > "$T.out" 2>&1; then
    echo "FAIL: mutant $m passed (the test misses it)"; rc=1
  else
    echo "ok   mutant $m caught: $(grep -m1 '^FAIL:' "$T.out")"
  fi
done
rm -f "$T" "$T.out"
[ $rc = 0 ] && echo "PASS me_wmap + 6 mutants caught"
exit $rc
