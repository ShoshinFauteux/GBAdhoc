#!/usr/bin/env bash
# build.sh OUT [GPSP_ROOT] -- build tools/gbref/tgb_ref (host, WSL gcc) against
# the GB core under GPSP_ROOT (default: this tree), with psp/Makefile's core
# flags.  Two roots -> two binaries: before/after comparisons of a core change.
set -euo pipefail
OUT=$1
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${2:-$(cd "$HERE/../.." && pwd)}"
T=$(mktemp -d)
for n in apu cpu gb lcd mbc rom sgb tgb_shared; do
  gcc -std=gnu11 -O2 -w ${GBREF_DEFS:-} -fgnu89-inline -fno-strict-aliasing -c "$ROOT/gbcore/tgbdual/$n.c" -o "$T/$n.o"
done
gcc -std=gnu11 -O2 -w -I"$ROOT/gbcore" -I"$ROOT/frontend-common" -o "$OUT" \
    "$HERE/tgb_ref.c" "$ROOT/gbcore/gbcore_tgbdual.c" "$T"/*.o -lm
rm -rf "$T"
echo "built $OUT from $ROOT"
