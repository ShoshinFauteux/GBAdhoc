#!/usr/bin/env bash
# twin_profile.sh OUT [linkplay args...]
#   -- the execution profile of the GB link machines, for the cache map
#   (docs/CACHE-MAP.md).  Run in WSL/Linux with docker.
#
# The two TGB Dual instances are built exactly as tools/gblink/build.sh builds
# them (the psp/Makefile recipe: one partial link, the copy renamed
# gbcoreb_*), cross-compiled for mipsel with the PSP's -O3 and -g, and linkplay
# runs under qemu-mipsel with tools/cachemap/icount_plugin.so, which counts
# every executed instruction address.  The first WARM frames (state/battery
# load, boot) are not profiled.
#
# linkplay args are passed through; paths must be absolute and under $HOME
# (it is mounted at the same path).  headless=2 is a HOST console's session
# (slot 0 drawn, slot 1 headless) and the PSP DUAL bench.  Example, the rig's
# Gen 2 trade (arm B):
#   twin_profile.sh ~/cachemap/out/b rom0=~/gbdual/roms/crystal.gbc \
#     rom1=~/gbdual/roms/crystal.gbc sav0=~/gbdual/fixred2/gen2-a.sav \
#     sav1=~/gbdual/fixred2/gen2-b.sav ap0=SCRIPTS/gen2_host.ap \
#     ap1=SCRIPTS/gen2_guest.ap delay=4 rtc=1790500000 max=12000 headless=2
# (SCRIPTS is replaced by the tree's tools/gblink/scripts.)
#
# OUT/ gets icount.txt (addr count), linkplay (the static twin, whose DWARF
# line table maps addresses to source lines), run.log.
set -euo pipefail
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"; shift
HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/../.." && pwd)"
CM=${CACHEMAP_HOME:-$HOME/cachemap}
IMAGE=${CACHEMAP_IMAGE:-drprof-qemu}
WARM=${WARM:-120}
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" "$HERE/docker"
mkdir -p "$CM/src"
rsync -a --delete --exclude .git --exclude '*.o' --exclude '*.a' \
  --exclude 'psp/*.PBP' --exclude 'psp/*.elf' "$TREE/" "$CM/src/"
ARGS=""
for a in "$@"; do ARGS="$ARGS ${a//SCRIPTS/$CM/src/tools/gblink/scripts}"; done
docker run --rm -v "$HOME":"$HOME" -v "$OUT":/out "$IMAGE" bash -c "
  set -e
  cd $CM/src
  if [ ! -x $CM/bin/linkplay ] || [ \"\${REBUILD:-1}\" = 1 ]; then
    CROSS=mipsel-linux-gnu- CROSS_CFLAGS='-march=mips32r2 -mno-abicalls -fno-pic' \
      EXTRA_LD=-static TGB_OPT=-O3 bash tools/gblink/build.sh $CM/bin >$CM/build.log 2>&1 \
      || { tail -20 $CM/build.log; exit 1; }
    gcc -O2 -shared -fPIC -Wall -I/opt/qemu/include \$(pkg-config --cflags glib-2.0) \
      -o $CM/bin/icount_plugin.so tools/cachemap/icount_plugin.c \$(pkg-config --libs glib-2.0)
  fi
  MARK=\$(mipsel-linux-gnu-nm $CM/bin/linkplay | awk '\$3==\"gbdual_advance\"{print \$1}')
  /opt/qemu/bin/qemu-mipsel -cpu 74Kf \
    -plugin $CM/bin/icount_plugin.so,out=/out/icount.txt,mark=\$MARK,skip=$WARM \
    $CM/bin/linkplay $ARGS > /out/run.log 2>&1 || true
  cp $CM/bin/linkplay /out/linkplay
  tail -3 /out/run.log
"
echo "profile: $OUT/icount.txt ($(wc -l < "$OUT/icount.txt") addresses)"
