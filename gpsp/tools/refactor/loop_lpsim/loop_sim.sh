#!/usr/bin/env bash
# loop_sim.sh SET FIXTURE TWIN [WARM]  -- one dynarec-twin run with BOTH cache
#   plugins on the same trace (T16, gate LP; run in WSL with docker):
#     tools/layout/lpsim_plugin.c      on $LP/loop/sim/SET/FIXTURE.lay      -> sim_old.txt
#     loop_lpsim/loop_lpsim_plugin.c   on $LP/loop/sim/SET/FIXTURE.loop.lay -> sim_new.txt
#   then `loop_lpsim.py report` (J/S/F reproduction + loop delta).
#   The layouts files come from `loop_lpsim.py layouts --lp $LP --set SET ...`.
#   Everything is written under $LP/loop (never $LP/plug or $LP/runs, which the
#   existing tools/layout scripts own).  The fixture table and the twin command
#   line are those of tools/layout/twin_sim.sh.
#   Env: LP_HOME (default ~/lp), DRPROF_IMAGE (drprof-qemu), FRAMES, ACCEPTED (acc).
set -euo pipefail
SET=$1; FX=$2; VARIANT=$3; WARM=${4:-60}
HERE="$(cd "$(dirname "$0")" && pwd)"
LAYOUT="$(cd "$HERE/../../layout" && pwd)"
LP=${LP_HOME:-$HOME/lp}
IMAGE=${DRPROF_IMAGE:-drprof-qemu}
case "$FX" in
  aw2)       ROM=aw2.gba;     ST=aw2.st0;                 SAV=aw2.sav;          SCR=aw2_psp_script.txt; FR=${FRAMES:-6100} ;;
  hns_heavy) ROM=hns.gba;     ST=heart_soul_heavy.st0;    SAV=hns.sav;          SCR=battle.txt;         FR=${FRAMES:-3100} ;;
  ub_rival)  ROM=unbound.gba; ST=unbound_rival_high.st0;  SAV=unbound.sav;      SCR=battle.txt;         FR=${FRAMES:-3100} ;;
  em_battle) ROM=emerald.gba; ST=emerald_3000.st0;        SAV=emerald_3000.sav; SCR=emerald_battle.txt; FR=${FRAMES:-2100} ;;
  doom2)     ROM=doom2.gba;   ST=doom2.st0;               SAV=-;                SCR=-;                  FR=${FRAMES:-2100} ;;
  *) echo "unknown fixture $FX"; exit 2 ;;
esac
OUT=$LP/loop/runs/$SET/$FX
mkdir -p "$OUT" "$LP/loop/plug"
cp "$LAYOUT/lpsim_plugin.c" "$LP/loop/plug/lpsim_plugin.c"
cp "$HERE/loop_lpsim_plugin.c" "$LP/loop/plug/loop_lpsim_plugin.c"
JIT=$(awk '{for(i=1;i<=NF;i++) if ($i ~ /^jit=/) print substr($i,5)}' "$LP/runs/prof/$VARIANT/$FX/run.info" 2>/dev/null || true)
[ -n "$JIT" ] || JIT=$(cat "$LP/bin/dr_host_$VARIANT.jit")
BIN=/w/bin/dr_host_$VARIANT
docker run --rm -u "$(id -u):$(id -g)" -v "$LP":/w -w /w/fx "$IMAGE" bash -c "
  set -e
  P=/w/loop/plug
  for n in lpsim_plugin loop_lpsim_plugin; do
    gcc -O2 -shared -fPIC -Wall -I/opt/qemu/include \$(pkg-config --cflags glib-2.0) \
      -o \$P/\$n.so \$P/\$n.c \$(pkg-config --libs glib-2.0)
  done
  if [ $SAV = - ]; then : > /tmp/run.sav; else cp $SAV /tmp/run.sav; fi
  MARK=\$(awk '\$4==\"drprof_host_mark\"{print \$1}' $BIN.nm)
  O=/w/loop/runs/$SET/$FX
  echo \"jit=$JIT mark=\$MARK frames=$FR warm=$WARM\" > \$O/run.info
  ( time /opt/qemu/bin/qemu-mipsel -cpu 74Kf \
     -plugin \$P/lpsim_plugin.so,out=\$O/sim_old.txt,jit=$JIT,mark=\$MARK,skip=$WARM,layouts=/w/loop/sim/$SET/$FX.lay \
     -plugin \$P/loop_lpsim_plugin.so,out=\$O/sim_new.txt,jit=$JIT,mark=\$MARK,skip=$WARM,layouts=/w/loop/sim/$SET/$FX.loop.lay \
     $BIN --rom $ROM --bios-dir . --save /tmp/run.sav --state $ST $( [ $SCR = - ] || echo --script $SCR ) --frames $FR \
     --hash \$O/hash.txt --log \$O/host.log ) > \$O/run.log 2>&1 || true
  tail -n 4 \$O/run.log
"
python3 "$HERE/loop_lpsim.py" report "$OUT/sim_old.txt" "$OUT/sim_new.txt" --accepted "${ACCEPTED:-acc}" | tee "$OUT/report.txt"
