#!/usr/bin/env bash
# icsim.sh OUT MAPS [linkplay args...]  -- replay the twin's instruction
#   fetches through a simulated Allegrex I-cache, once per layout map
#   (docs/CACHE-MAP.md).  MAPS = map1[:map2...] from cachemap.py --emit-map
#   (absolute paths under $HOME).  Uses the twin that twin_profile.sh built
#   ($CACHEMAP_HOME/bin/linkplay) -- the maps are only valid for that binary.
#   CACHE="size=16384,ways=2,line=64" overrides the geometry.
set -euo pipefail
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"; MAPS="$2"; shift 2
HERE="$(cd "$(dirname "$0")" && pwd)"
CM=${CACHEMAP_HOME:-$HOME/cachemap}
IMAGE=${CACHEMAP_IMAGE:-drprof-qemu}
WARM=${WARM:-120}
GEOM=${CACHE:-size=16384,ways=2,line=64}
ARGS=""
for a in "$@"; do ARGS="$ARGS ${a//SCRIPTS/$CM/src/tools/gblink/scripts}"; done
cp "$HERE/icsim_plugin.c" "$CM/src/tools/cachemap/icsim_plugin.c"
docker run --rm -v "$HOME":"$HOME" -v "$OUT":/out "$IMAGE" bash -c "
  set -e
  gcc -O2 -shared -fPIC -Wall -I/opt/qemu/include \$(pkg-config --cflags glib-2.0) \
    -o $CM/bin/icsim_plugin.so $CM/src/tools/cachemap/icsim_plugin.c \$(pkg-config --libs glib-2.0)
  MARK=\$(mipsel-linux-gnu-nm $CM/bin/linkplay | awk '\$3==\"gbdual_advance\"{print \$1}')
  /opt/qemu/bin/qemu-mipsel -cpu 74Kf \
    -plugin $CM/bin/icsim_plugin.so,out=/out/icsim.txt,map=$MAPS,mark=\$MARK,skip=$WARM,${GEOM} \
    $CM/bin/linkplay $ARGS > /out/run.log 2>&1 || true
  tail -2 /out/run.log
"
cat "$OUT/icsim.txt"
