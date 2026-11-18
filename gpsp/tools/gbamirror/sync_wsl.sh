#!/bin/bash
# sync_wsl.sh -- copy this gpsp tree (SRC, default the one this script is in) into WSL
# (~/gbamirror/src) and build
# gl_host there (building on /mnt/c is slow and trips over CRLF checkouts).
set -e
SRC=${SRC:-$(cd "$(dirname "$0")/../.." && pwd)/}
DST=${DST:-$HOME/gbamirror/src}
mkdir -p "$DST"
rsync -a --delete --exclude .git --exclude '*.o' --exclude '*.a' \
  --exclude gl_host --exclude 'psp/*.elf' --exclude '*.PBP' "$SRC" "$DST/"
find "$DST" -type f \( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.S' \
  -o -name 'Makefile*' -o -name '*.sh' -o -name '*.txt' \) -print0 | xargs -0 sed -i 's/\r$//'
cd "$DST/tools/gbamirror" && make "$@"
