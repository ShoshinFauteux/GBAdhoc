#!/usr/bin/env bash
# dr_build.sh -- build the dynarec twin (and translator variants) in docker.
#
#   tools/drprof/dr_build.sh [VARIANT [VDEFS...]]      (run in WSL/Linux)
#
#   dr_build.sh                       -> bin/dr_host_base + the plugin
#   dr_build.sh inline "-DDRPROF_INLINE_RAM=1"        -> bin/dr_host_inline
#   TWIN=0 dr_build.sh base-notwin    -> the same core without DRPROF_TWIN
#
# The tree is rsynced to $DRPROF_HOME/src (native Linux filesystem: OneDrive
# through /mnt/c is slow and the root make writes objects beside the sources),
# then built by tools/drprof/Makefile inside the drprof-qemu image
# (tools/drprof/docker/Dockerfile).  Binaries land in $DRPROF_HOME/bin, one
# name per variant, and are never overwritten by another variant.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/../.." && pwd)"
DRPROF_HOME=${DRPROF_HOME:-$HOME/drprof}
IMAGE=${DRPROF_IMAGE:-drprof-qemu}
VARIANT=${1:-base}
VDEFS=${2:-}
TWIN=${TWIN:-1}

docker image inspect "$IMAGE" >/dev/null 2>&1 || \
  docker build -t "$IMAGE" "$HERE/docker"

mkdir -p "$DRPROF_HOME/src" "$DRPROF_HOME/bin"
rsync -a --delete --exclude .git --exclude '*.o' --exclude '*.a' \
  --exclude 'psp/*.PBP' --exclude 'psp/*.elf' --exclude 'tools/drprof/bin' \
  "$TREE/" "$DRPROF_HOME/src/"

docker run --rm -v "$DRPROF_HOME":/w -w /w/src/tools/drprof "$IMAGE" bash -c "
  set -e
  rm -f bin/dr_host_$VARIANT bin/libcore_$VARIANT.a
  make plugin >/dev/null
  make VARIANT='$VARIANT' VDEFS='$VDEFS' TWIN='$TWIN' 2>&1 | grep -E 'error|warning: linking|Error' || true
  test -x bin/dr_host_$VARIANT
  cp bin/dr_host_$VARIANT* bin/drprof_plugin.so /w/bin/
"
echo "built $DRPROF_HOME/bin/dr_host_$VARIANT ($(md5sum < "$DRPROF_HOME/bin/dr_host_$VARIANT" | cut -c1-8)) defs: $(cat "$DRPROF_HOME/bin/dr_host_$VARIANT.defs")"
