#!/usr/bin/env bash
# capsim.sh OUT FIXTURE VARIANT LAYOUTS [WARM] -- twin_sim.sh with the ME capture
# mode on (DR_MECAP=1), the I+D plugin, per-layout misses, and d2_drive.
set -euo pipefail
OUT=$1; FX=$2; VARIANT=$3; LAY=$4
LP=${LP_HOME:-$HOME/lpcf}
IMAGE=${DRPROF_IMAGE:-drprof-qemu:btfix}
HERE="$(cd "$(dirname "$0")" && pwd)"
PLUG=${PLUG:-$HERE/lpsimd_plugin.c}
case "$FX" in
  em_battle) ROM=emerald.gba; ST=emerald_3000.st0; SAV=emerald_3000.sav; SCR=emerald_battle.txt; FR=${FRAMES:-2100}; WARM=${5:-300} ;;
  d2_drive)  ROM=d2.gba;      ST=-;                SAV=-;                SCR=d2_drive.txt;       FR=${FRAMES:-3600}; WARM=${5:-1800} ;;
  *) echo "unknown fixture $FX"; exit 2 ;;
esac
mkdir -p "$LP/runs/$OUT" "$LP/plug"
cmp -s "$PLUG" "$LP/plug/lpsim_plugin.c" || cp "$PLUG" "$LP/plug/lpsim_plugin.c"
BIN=/w/bin/dr_host_$VARIANT
docker run --rm -e DR_MECAP=${DR_MECAP:-1} -v "$LP":/w -w /w/fx "$IMAGE" bash -c "
  set -e
  O=/w/runs/$OUT
  if [ ! /w/plug/lpsim_plugin.so -nt /w/plug/lpsim_plugin.c ]; then
    gcc -O2 -shared -fPIC -Wall -I/opt/qemu/include \$(pkg-config --cflags glib-2.0) \
      -o /w/plug/lpsim_plugin.so.\$\$ /w/plug/lpsim_plugin.c \$(pkg-config --libs glib-2.0)
    mv -f /w/plug/lpsim_plugin.so.\$\$ /w/plug/lpsim_plugin.so
  fi
  if [ $SAV = - ]; then : > /tmp/run.sav; else cp $SAV /tmp/run.sav; fi
  STARG=''; [ $ST = - ] || STARG='--state $ST'
  MARK=\$(awk '\$4==\"drprof_host_mark\"{print \$1}' $BIN.nm)
  if [ ! -s $BIN.jit ]; then
    python3 /w/src-$VARIANT/tools/drprof/dr_symcat.py $BIN.nm $BIN.members > /tmp/symcat.txt
    CTL=\$(awk '\$4==\"drprof_ctl\"{print \$1}' $BIN.nm)
    ZARG=\$(awk '\$4==\"drprof_zone_arg\"{print \$1}' $BIN.nm)
    /opt/qemu/bin/qemu-mipsel -cpu 74Kf -plugin /w/bin/drprof_plugin.so,out=/tmp/p.txt,symcat=/tmp/symcat.txt,ctl=\$CTL,zarg=\$ZARG \
      $BIN --rom $ROM --bios-dir . --save /tmp/run.sav \$STARG --frames 3 --log /tmp/h.log >/dev/null 2>&1 || true
    python3 -c \"import re,sys; t=open('/tmp/p.txt').read(); m=re.search(r'rom=([0-9a-f]+)\+([0-9a-f]+) ram=([0-9a-f]+)\+([0-9a-f]+)', t); print('%x:%x' % (int(m.group(1),16), int(m.group(3),16)+int(m.group(4),16)))\" > $BIN.jit
    if [ $SAV = - ]; then : > /tmp/run.sav; else cp $SAV /tmp/run.sav; fi
  fi
  JIT=\$(cat $BIN.jit)
  PL=''
  if [ '$LAY' != - ]; then PL=\",layouts=$LAY,misses=\$O/misses.txt\"; else echo '' > /tmp/empty.lay; PL=',layouts=/tmp/empty.lay'; fi
  echo \"jit=\$JIT mark=\$MARK\" > \$O/run.info
  ( time /opt/qemu/bin/qemu-mipsel -cpu 74Kf \
     -plugin /w/plug/lpsim_plugin.so,out=\$O/sim.txt,counts=\$O/counts.txt,jit=\$JIT,mark=\$MARK,skip=$WARM\$PL \
     $BIN --rom $ROM --bios-dir . --save /tmp/run.sav \$STARG --script $SCR --frames $FR \
     --hash \$O/hash.txt --log \$O/host.log ) > \$O/run.log 2>&1 || true
  tail -n 4 \$O/run.log
"
