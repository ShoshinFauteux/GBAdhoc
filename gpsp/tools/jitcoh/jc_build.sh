#!/usr/bin/env bash
# jc_build.sh -- build a coherency-checker twin (docs/JIT-COHERENCY.md).
#
#   tools/jitcoh/jc_build.sh NAME [PATCH...]        (WSL/Linux, docker)
#   VDEFS="-DFOO=1" tools/jitcoh/jc_build.sh NAME   extra core defines
#
# -> $DRPROF_HOME/bin/dr_host_jc-NAME (+ .nm/.defs) and bin/jitcoh_plugin.so
#
# Every checker twin is the drprof twin (tools/drprof/Makefile: the shipped
# CORE_COMMON dynarec flags) built with -DJITCOH_PSP_CACHE, so the core makes
# the PSP's own cache-maintenance calls (cpu_threaded.c) into the hookable
# shims in dr_host.c.  PATCH files (tools/jitcoh/variants/*.patch, applied
# with `patch -p1` to a private copy of the tree) select the code variants
# compared in the doc -- the shipped ROM publish, the bonly experiment, and
# the deliberately broken sync sites the checker is validated against.  The
# tree itself is never modified.  Each NAME builds in its own copy
# ($DRPROF_HOME/src-jc-NAME), so builds can run in parallel.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/../.." && pwd)"
DRPROF_HOME=${DRPROF_HOME:-$HOME/drprof}
IMAGE=${DRPROF_IMAGE:-drprof-qemu}
NAME=$1; shift
VARIANT=jc-$NAME
SRC=src-jc-$NAME
# JC_PSPCACHE=0 builds the same twin WITHOUT the PSP cache calls (the plain
# drprof twin from this tree): dr_oracle.py jc-<that> jc-base must be IDENTICAL.
if [ "${JC_PSPCACHE:-1}" = 1 ]; then VDEFS="-DJITCOH_PSP_CACHE ${VDEFS:-}"; else VDEFS="${VDEFS:-}"; fi

mkdir -p "$DRPROF_HOME/$SRC" "$DRPROF_HOME/bin"
rsync -a --delete --exclude .git --exclude '*.o' --exclude '*.a' \
  --exclude 'psp/*.PBP' --exclude 'psp/*.elf' --exclude 'tools/drprof/bin' \
  "$TREE/" "$DRPROF_HOME/$SRC/"
for p in "$@"; do
  case "$p" in /*) pf=$p ;; *) pf=$HERE/variants/$p ;; esac
  patch -s -p1 -d "$DRPROF_HOME/$SRC" < "$pf"
  echo "applied $(basename "$pf")"
done

docker run --rm -v "$DRPROF_HOME":/w -w /w/$SRC/tools/drprof "$IMAGE" bash -c "
  set -e
  rm -f bin/dr_host_$VARIANT bin/libcore_$VARIANT.a
  make VARIANT='$VARIANT' VDEFS='$VDEFS' 2>&1 | grep -E 'error|warning: linking|Error' || true
  test -x bin/dr_host_$VARIANT
  cp bin/dr_host_$VARIANT* /w/bin/
  gcc -O2 -g -shared -fPIC -Wall -I/opt/qemu/include \$(pkg-config --cflags glib-2.0) \
    -o /w/bin/jitcoh_plugin.so ../jitcoh/jitcoh_plugin.c \$(pkg-config --libs glib-2.0)
"
B=$DRPROF_HOME/bin/dr_host_$VARIANT
[ "${JC_PSPCACHE:-1}" = 1 ] && for s in sceKernelDcacheWritebackRange sceKernelIcacheInvalidateRange \
         sceKernelDcacheWritebackInvalidateAll sceKernelIcacheInvalidateAll; do
  grep -q " $s\$" "$B.nm" || { echo "MISSING hook symbol $s"; exit 1; }
done
echo "built $B ($(md5sum < "$B" | cut -c1-8)) defs: $(cat "$B.defs")"
