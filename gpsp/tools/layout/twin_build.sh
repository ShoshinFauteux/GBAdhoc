#!/usr/bin/env bash
# twin_build.sh TREE VARIANT [PATCH] [VDEFS]  -- build a drprof twin for the
#   layout study (docs/LAYOUT-PINNING.md).  Run in WSL with docker.
#
#   TREE     a checkout that carries tools/drprof (claude/dynarec-profile or
#            claude/candidate-3.1-dr: the candidate core + the twin hooks)
#   VARIANT  binary name: $LP_HOME/bin/dr_host_VARIANT
#   PATCH    optional diff applied on top (e.g. `git diff claude/candidate-3.1
#            claude/layout-pinning`), so the pinned core is the same twin
#   VDEFS    extra core defines (e.g. -DLAYOUT_PIN=1)
#
# Private home ($LP_HOME, default ~/lp): tools/drprof/dr_build.sh rsyncs with
# --delete into $DRPROF_HOME/src, which other agents share.  The drprof
# Makefile predates the GB dual-link merge, so the GB-link entry points are
# linked as inert stubs (dr_gbstub.c; no GBA fixture starts a GB link).
set -euo pipefail
TREE="$(cd "$1" && pwd)"; VARIANT="$2"; PATCH="${3:-}"; VDEFS="${4:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
LP=${LP_HOME:-$HOME/lp}
IMAGE=${DRPROF_IMAGE:-drprof-qemu}
SRC="$LP/src-$VARIANT"
mkdir -p "$SRC" "$LP/bin"
rsync -a --delete --exclude .git --exclude '*.o' --exclude '*.a' --exclude '*.d' \
  --exclude 'psp/*.PBP' --exclude 'psp/*.elf' --exclude 'tools/drprof/bin' \
  "$TREE/" "$SRC/"
if [ -n "$PATCH" ]; then
  # A Windows checkout can be CRLF (core.autocrlf) while `git diff` is LF:
  # normalise the files the patch touches, and allow NO fuzz -- a fuzzed hunk
  # once landed a .balign in the middle of the dispatcher.
  ( cd "$SRC" && for f in $(sed -n 's#^+++ b/##p' "$PATCH"); do
      [ -f "$f" ] && sed -i 's/\r$//' "$f"; done
    patch -p1 --fuzz=0 --quiet < "$PATCH" )
fi
[ -f "$SRC/tools/drprof/dr_gbstub.c" ] || cp "$HERE/dr_gbstub.c" "$SRC/tools/drprof/dr_gbstub.c"
sed -i -e 's#^\(\s*-I$(CORE_DIR)/libretro/libretro-common/include\)$#\1 -I$(CORE_DIR)/gbcore#' \
       -e 's#^\(\s*$(CORE_DIR)/netdrv/gb_link.c\)$#\1 $(FE)/fe_gblink.c dr_gbstub.c#' \
       "$SRC/tools/drprof/Makefile"
docker run --rm -v "$LP":/w -w "/w/src-$VARIANT/tools/drprof" "$IMAGE" bash -c "
  set -e
  rm -rf bin
  make plugin >/dev/null
  make VARIANT='$VARIANT' VDEFS='$VDEFS' 2>&1 | grep -E 'error|Error' || true
  test -x bin/dr_host_$VARIANT
  cp bin/dr_host_$VARIANT* bin/drprof_plugin.so /w/bin/
"
echo "built $LP/bin/dr_host_$VARIANT ($(md5sum < "$LP/bin/dr_host_$VARIANT" | cut -c1-8)) defs: $(cat "$LP/bin/dr_host_$VARIANT.defs")"
