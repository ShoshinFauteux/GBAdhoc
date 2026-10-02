#!/usr/bin/env bash
# twin_sim.sh OUT FIXTURE VARIANT LAYOUTS [WARM]  -- one twin run of a fixture
#   through lpsim_plugin (docs/LAYOUT-PINNING.md).  Run in WSL with docker.
#
#   OUT      run directory under $LP_HOME/runs (sim.txt, counts.txt, host.log,
#            hash.txt)
#   FIXTURE  aw2 | hns_heavy | ub_rival | em_battle (rows below = dr_suite.sh)
#   VARIANT  twin binary $LP_HOME/bin/dr_host_VARIANT (twin_build.sh)
#   LAYOUTS  layouts file (container paths, /w = $LP_HOME) or - for a
#            counts-only profile run
#   WARM     frames skipped before counting (default 60: state load and the
#            first translations)
# JIT=lo:hi overrides the twin's translation-cache range; by default it is
# read from a short drprof run's geom line (cached in $LP_HOME/bin).
set -euo pipefail
OUT=$1; FX=$2; VARIANT=$3; LAY=$4; WARM=${5:-60}
HERE="$(cd "$(dirname "$0")" && pwd)"
LP=${LP_HOME:-$HOME/lp}
IMAGE=${DRPROF_IMAGE:-drprof-qemu}
case "$FX" in
  aw2)       ROM=aw2.gba;     ST=aw2.st0;                 SAV=aw2.sav;          SCR=aw2_psp_script.txt; FR=${FRAMES:-6100} ;;
  hns_heavy) ROM=hns.gba;     ST=heart_soul_heavy.st0;    SAV=hns.sav;          SCR=battle.txt;         FR=${FRAMES:-3100} ;;
  hns_light) ROM=hns.gba;     ST=heart_soul_light.st0;    SAV=hns.sav;          SCR=battle.txt;         FR=${FRAMES:-3100} ;;
  ub_rival)  ROM=unbound.gba; ST=unbound_rival_high.st0;  SAV=unbound.sav;      SCR=battle.txt;         FR=${FRAMES:-3100} ;;
  em_battle) ROM=emerald.gba; ST=emerald_3000.st0;        SAV=emerald_3000.sav; SCR=emerald_battle.txt; FR=${FRAMES:-2100} ;;
  *) echo "unknown fixture $FX"; exit 2 ;;
esac
mkdir -p "$LP/runs/$OUT" "$LP/plug"
cmp -s "$HERE/lpsim_plugin.c" "$LP/plug/lpsim_plugin.c" || cp "$HERE/lpsim_plugin.c" "$LP/plug/lpsim_plugin.c"
BIN=/w/bin/dr_host_$VARIANT
docker run --rm -v "$LP":/w -w /w/fx "$IMAGE" bash -c "
  set -e
  O=/w/runs/$OUT
  if [ ! /w/plug/lpsim_plugin.so -nt /w/plug/lpsim_plugin.c ]; then
    gcc -O2 -shared -fPIC -Wall -I/opt/qemu/include \$(pkg-config --cflags glib-2.0) \
      -o /w/plug/lpsim_plugin.so.\$\$ /w/plug/lpsim_plugin.c \$(pkg-config --libs glib-2.0)
    mv -f /w/plug/lpsim_plugin.so.\$\$ /w/plug/lpsim_plugin.so
  fi
  cp $SAV /tmp/run.sav
  MARK=\$(awk '\$4==\"drprof_host_mark\"{print \$1}' $BIN.nm)
  JIT='${JIT:-}'
  if [ -z \"\$JIT\" ]; then
    if [ ! -s $BIN.jit ]; then
      python3 /w/src-$VARIANT/tools/drprof/dr_symcat.py $BIN.nm $BIN.members > /tmp/symcat.txt
      CTL=\$(awk '\$4==\"drprof_ctl\"{print \$1}' $BIN.nm)
      ZARG=\$(awk '\$4==\"drprof_zone_arg\"{print \$1}' $BIN.nm)
      /opt/qemu/bin/qemu-mipsel -cpu 74Kf -plugin /w/bin/drprof_plugin.so,out=/tmp/p.txt,symcat=/tmp/symcat.txt,ctl=\$CTL,zarg=\$ZARG \
        $BIN --rom $ROM --bios-dir . --save /tmp/run.sav --state $ST --frames 3 --log /tmp/h.log >/dev/null 2>&1 || true
      python3 -c \"import re,sys; t=open('/tmp/p.txt').read(); m=re.search(r'rom=([0-9a-f]+)\+([0-9a-f]+) ram=([0-9a-f]+)\+([0-9a-f]+)', t); print('%x:%x' % (int(m.group(1),16), int(m.group(3),16)+int(m.group(4),16)))\" > $BIN.jit
      cp $SAV /tmp/run.sav
    fi
    JIT=\$(cat $BIN.jit)
  fi
  PL=''
  if [ '$LAY' != - ]; then PL=\",layouts=$LAY\"; else echo '' > /tmp/empty.lay; PL=',layouts=/tmp/empty.lay'; fi
  echo \"jit=\$JIT mark=\$MARK\" > \$O/run.info
  ( time /opt/qemu/bin/qemu-mipsel -cpu 74Kf \
     -plugin /w/plug/lpsim_plugin.so,out=\$O/sim.txt,counts=\$O/counts.txt,jit=\$JIT,mark=\$MARK,skip=$WARM\$PL \
     $BIN --rom $ROM --bios-dir . --save /tmp/run.sav --state $ST --script $SCR --frames $FR \
     --hash \$O/hash.txt --log \$O/host.log ) > \$O/run.log 2>&1 || true
  tail -n 4 \$O/run.log
"
