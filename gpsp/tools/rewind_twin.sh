#!/usr/bin/env bash
# twin.sh NAME [EXTRA_CORE_CFLAGS] -- build the REWIND desktop twin from the
# worktree into ~/rewind-agent/NAME (WSL).  Run inside WSL.
set -uo pipefail
NAME=${1:?name}
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/workdirs.sh"
SRC=${RW_SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}   # the gpsp tree to copy
W=$HOME/rewind-agent
B=$W/$NAME; mkdir -p "$B"
rsync -a --delete --exclude .git --exclude builds --exclude releases --exclude docs \
  --exclude '*.gba' --exclude '*.sav' --exclude 'sdl/obj' --exclude 'sdl/gpsp_sdl' \
  --exclude 'sdl/*.a' --exclude '**/*.o' --exclude '*.prx' --exclude '*.elf' --exclude '*.PBP' \
  --exclude 'psp/me/*.prx' --exclude 'psp/me/*.elf' "$SRC/" "$B/"
cd "$B"
find . -name '*.sh' -o -name '*.py' | xargs -r sed -i 's/\r$//' 2>/dev/null
make platform=unix clean >/dev/null 2>&1 || true
make platform=unix REWIND=1 -j"$(nproc)" >"$W/$NAME.core.log" 2>&1 || true
archive() {
  rm -f sdl/libgpsp_core.a
  ar rcs sdl/libgpsp_core.a $(find . -name '*.o' -not -path './sdl/*' -not -path './psp/*' \
    -not -path './frontend-common/*' -not -path './netdrv/*' -not -path './tools/*' \
    -not -path './tests/*' -not -path './mips/*' -not -path './gbcore/*' \
    -not -path './arm/*' -not -path './3ds/*' -not -path './jni/*')
}
archive
mkdir -p sdl/obj
(cd sdl && rm -f gpsp_sdl && make REWIND=1 EXTRA_CFLAGS=-DGPSP_PERF_RIG gpsp_sdl >"$W/$NAME.link.log" 2>&1) || {
  grep -o "undefined reference to \`[A-Za-z_0-9]*" "$W/$NAME.link.log" | sed 's/.*`//' | sort -u |
    awk '{print "long " $1 "() { return 0; }"}' > mts_stubs.c
  [ -s mts_stubs.c ] || { tail -30 "$W/$NAME.link.log"; exit 1; }
  cc -w -O2 -c -o mts_stubs.o mts_stubs.c
  archive
  (cd sdl && make REWIND=1 EXTRA_CFLAGS=-DGPSP_PERF_RIG gpsp_sdl >"$W/$NAME.link.log" 2>&1) || { tail -30 "$W/$NAME.link.log"; exit 1; }
}
grep -n "warning" "$W/$NAME.link.log" | grep -i 'rewind\|fe_host' | head -20
ls -la sdl/gpsp_sdl && echo BUILD_OK
