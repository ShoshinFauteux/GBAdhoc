#!/usr/bin/env bash
# xlat_spill_guard.sh ROOT DOCKER_IMAGE [MAX_STACK_STORES]
#
# XLAT_THUMB_NO_UNSWITCH's guard (cpu-levers, 2026-10-26).  translate_block_thumb
# is one very large function.  When psp-gcc unswitches its per-instruction loop
# (or anything else doubles it), the allocator keeps translation_ptr in a stack
# slot: about 1500 stack stores instead of about 250-450, and roughly double the
# translation cost.  Nothing in the emitted code or the guest's behaviour shows
# it, so this guard counts the function's stack stores in the linked ELF and
# fails when they exceed the limit (default 700; healthy is 256 without
# SPIN_SKIP and 457 with it, spilled is 1462+).
set -u
ROOT=$1; IMG=$2; MAX=${3:-700}
out=$(docker run --rm -v "$ROOT":/build -w /build "$IMG" sh -c \
  "psp-objdump -d --no-show-raw-insn psp/gpsp_adhoc.elf |
   awk '/<translate_block_thumb>:/{x=1;next} x&&/^[0-9a-f]+ </{x=0} x'" 2>/dev/null)
n_ins=$(printf '%s\n' "$out" | grep -c '^ ')
n_sw=$(printf '%s\n' "$out" | grep -cE 'sw[[:space:]]+[^,]+,-?[0-9]+\(sp\)')
echo "xlat spill guard: translate_block_thumb $n_ins instructions, $n_sw stack stores (limit $MAX)"
[ "$n_ins" -gt 0 ] || { echo "xlat spill guard: translate_block_thumb not found"; exit 1; }
[ "$n_sw" -le "$MAX" ] || { echo "xlat spill guard: translation_ptr spill is back (see XLAT_THUMB_NO_UNSWITCH in cpu_threaded.c)"; exit 1; }
exit 0
