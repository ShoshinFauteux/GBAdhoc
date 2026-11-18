#!/usr/bin/env bash
# ppsave.sh ARM [TAG] [extra ini...] -- Emerald: save in the START menu, rewind
# across the save, let go, play on; MIPS dynarec in PPSSPP, threaded writer.
set -uo pipefail
ARM=$1; TAG=${2:-ppsave}; shift; shift || true
W=$HOME/rewind-agent; S=$W/s9
T=${RW_TOOLS:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}   # this tree's tools/
mkdir -p $S/pp
{
  echo "wait 300"
  for i in $(seq 1 30); do echo "press A 4"; echo "wait 40"; done
  for i in 1 2 3; do echo "press B 4"; echo "wait 30"; done
  echo "press START 4"; echo "wait 40"
  for i in 1 2 3 4; do echo "press DOWN 4"; echo "wait 12"; done
  for i in $(seq 1 8); do echo "press A 4"; echo "wait 70"; done
  echo "wait 500"
  echo "evt saved"
  echo "rewind ${PP_HOLD:-3000}"
  echo "wait 60"
  echo "rewind 30"; echo "wait 5"; echo "rewind 3"; echo "wait 2"; echo "rewind 3"; echo "wait 400"
  echo "wait 100000"
} > $S/pp/$TAG.inputs
sed 's/\r$//' $T/rewind_ppsspp.sh > $S/pp/rewind_ppsspp.sh
SAV=$S/emerald_owner.sav FF=${PP_FF:-1} TIMEOUT=${PP_TIMEOUT:-600} bash $S/pp/rewind_ppsspp.sh $TAG $ARM em $S/pp/$TAG.inputs ${PP_FRAMES:-3300} \
  "rewind = 1" "rewind_product = 1" "rewind_verify = 1" "rewind_hash = 1" "$@"
R=$W/pp2/$TAG
python3 $S/tools/rewind_check.py $R/rewind_hash.txt --min-redo 0 --new-timeline --allow-cut 16 --max-step 12 | cut -c1-330
grep -h "sram_load\|rewind_save\|sram_flush\|rewind_tick\|rewind_ring\|rewind_mem \|rewind_enable\|vmem" $R/frontend.log | cut -c1-330
python3 - $R <<'PY'
import sys, binascii, os
r = sys.argv[1]
first = {}; ends = []; last = None
for line in open(os.path.join(r, 'rewind_hash.txt')):
    t = line.split()
    if not t: continue
    if t[0] == 'F':
        first.setdefault(int(t[1]), t[7]); last = t
    elif t[0] == 'E':
        ends.append(t)
sav = os.path.join(r, 'em.sav')
crc = '%08x' % (binascii.crc32(open(sav, 'rb').read()) & 0xffffffff) if os.path.exists(sav) else None
wrote = sorted(f for f in first if f - 1 in first and first[f] != first[f - 1])
print('save written in frames %s..%s (%d); landings %s; first-pass save at first landing %s; '
      'last frame %s save in memory %s; file on disk %s -> %s' % (
          wrote[0] if wrote else None, wrote[-1] if wrote else None, len(wrote),
          [(e[1], e[2]) for e in ends], first.get(int(ends[0][1])) if ends else None,
          last[1], last[7], crc, 'SAME' if crc == last[7] else 'DIFFERENT'))
print('left beside the save:', [n for n in os.listdir(r) if 'rw-' in n])
PY
