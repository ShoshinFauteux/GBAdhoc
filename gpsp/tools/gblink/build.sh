#!/usr/bin/env bash
# build.sh OUTDIR -- the host GB link tools (POSIX gcc/binutils; WSL on Windows).
#
#   OUTDIR/linkplay   two cabled Game Boys, each driven by an autopilot script
#
# Both TGB Dual instances are made exactly as psp/Makefile makes them (one
# partial link through gbcore/gbcore_instance.ld, the copy renamed gbcoreb_*),
# and fe_autopilot.c is linked twice the same way: the second engine's entry
# points become fe_autopilot_b_*, its host hooks ap_b_*.
# SAN=1 adds AddressSanitizer/UBSan.
set -euo pipefail
OUT="${1:?usage: build.sh OUTDIR}"
cd "$(dirname "$0")/../.."
mkdir -p "$OUT/obj"
O="$OUT/obj"
OPT=(-O2 -g)
[ "${SAN:-0}" = 1 ] && OPT=(-O1 -g -fsanitize=address,undefined)
# CROSS=mipsel-linux-gnu- builds the same tools for qemu-mipsel (the cache
# map's execution profile, tools/cachemap); TGB_OPT=-O3 matches psp/Makefile's
# TGB_CFLAGS; EXTRA_LD=-static for qemu linux-user.
CROSS="${CROSS:-}"
gcc() { command "${CROSS}gcc" ${CROSS_CFLAGS:-} "$@" ${EXTRA_LD:-}; }
ld() { command "${CROSS}ld" "$@"; }
objcopy() { command "${CROSS}objcopy" "$@"; }
nm() { command "${CROSS}nm" "$@"; }
TGB_OPT="${TGB_OPT:-${OPT[0]}}"
CORE_FLAGS=(-std=gnu11 -fgnu89-inline -fno-strict-aliasing -Wall -Wextra
            -Wno-unused-parameter -Werror)
objs=()
for f in apu cpu gb lcd mbc rom sgb; do
  gcc "$TGB_OPT" -g "${CORE_FLAGS[@]}" -c "gbcore/tgbdual/$f.c" -o "$O/$f.o"
  objs+=("$O/$f.o")
done
gcc "${OPT[@]}" "${CORE_FLAGS[@]}" -c gbcore/tgbdual/tgb_shared.c -o "$O/tgb_shared.o"
gcc "${OPT[@]}" -std=gnu11 -Wall -Wextra -Werror -Igbcore -Ifrontend-common \
    -c gbcore/gbcore_tgbdual.c -o "$O/adapter.o"
ld -r -T gbcore/gbcore_instance.ld -o "$O/joined.o" "$O/adapter.o" "${objs[@]}"
objcopy -w --keep-global-symbol='gbcore_*' "$O/joined.o" "$O/gbcore_a.o"
nm -g --defined-only "$O/gbcore_a.o" |
  awk '$3 !~ /^gbcore_/ { bad = 1 } { print $3 " gbcoreb_" substr($3, 8) }
       END { exit bad }' > "$O/gbcoreb.syms"
objcopy --redefine-syms="$O/gbcoreb.syms" "$O/gbcore_a.o" "$O/gbcore_b.o"

gcc "${OPT[@]}" -std=gnu99 -Wall -Wextra -Werror -DGPSP_PERF_RIG \
    -Ifrontend-common -Ilibretro/libretro-common/include \
    -c frontend-common/fe_autopilot.c -o "$O/ap_a.o"
{
  for e in load active frame status ff dump_pending state_pending disconnect_pending take_mark; do
    echo "fe_autopilot_$e fe_autopilot_b_$e"
  done
  echo "fe_host_mem_read ap_b_host_mem_read"
  echo "fe_host_input_inject ap_b_host_input_inject"
  echo "fe_host_sram_crc_now ap_b_host_sram_crc_now"
  echo "fe_evt ap_b_evt"
} > "$O/ap_b.syms"
objcopy --redefine-syms="$O/ap_b.syms" "$O/ap_a.o" "$O/ap_b.o"

gcc "${OPT[@]}" -std=gnu11 -Wall -Wextra -Werror -Igbcore -Ifrontend-common \
    -o "$OUT/linkplay" tools/gblink/linkplay.c gbcore/gbcore_dual.c \
    "$O/gbcore_a.o" "$O/gbcore_b.o" "$O/tgb_shared.o" "$O/ap_a.o" "$O/ap_b.o"
echo "built $OUT/linkplay"

# ---- sesssim: two consoles' complete session stacks in one process ------
# Console H: instances A/B, gbcore_dual, fe_gblink as they are.  Console G:
# instances C/D and renamed copies of gbcore_dual (gbdual2_*) and fe_gblink
# (fe_gblink2_*), so the two consoles share nothing.
for x in c d; do
  sed "s/ gbcoreb_/ gbcore${x}_/" "$O/gbcoreb.syms" > "$O/gbcore$x.syms"
  objcopy --redefine-syms="$O/gbcore$x.syms" "$O/gbcore_a.o" "$O/gbcore_$x.o"
done
gcc "${OPT[@]}" -std=gnu11 -Wall -Wextra -Werror -Igbcore -Ifrontend-common \
    -c gbcore/gbcore_dual.c -o "$O/dual1.o"
gcc "${OPT[@]}" -std=gnu11 -Wall -Wextra -Werror -Igbcore -Ifrontend-common \
    -Inetdrv -Ilibretro/libretro-common/include \
    -c frontend-common/fe_gblink.c -o "$O/gblink1.o"
gcc "${OPT[@]}" -std=gnu11 -Wall -Wextra -Werror -Ifrontend-common \
    -c frontend-common/fe_util.c -o "$O/fe_util.o"
{
  nm -g --defined-only "$O/dual1.o" | awk '{ n = $3; sub(/^gbdual_/, "gbdual2_", n); print $3 " " n }'
  echo "gbcore_api gbcorec_api"
  echo "gbcoreb_api gbcored_api"
} > "$O/dual2.syms"
objcopy --redefine-syms="$O/dual2.syms" "$O/dual1.o" "$O/dual2.o"
{
  nm -g --defined-only "$O/gblink1.o" | awk '{ n = $3; sub(/^fe_gblink_/, "fe_gblink2_", n); print $3 " " n }'
  nm -g --defined-only "$O/dual1.o" | awk '{ n = $3; sub(/^gbdual_/, "gbdual2_", n); print $3 " " n }'
  echo "fe_evt sim_evt2"
} > "$O/gblink2.syms"
objcopy --redefine-syms="$O/gblink2.syms" "$O/gblink1.o" "$O/gblink2.o"
gcc "${OPT[@]}" -std=gnu11 -Wall -Wextra -Werror -Igbcore -Ifrontend-common \
    -o "$OUT/sesssim" tools/gblink/sesssim.c "$O/dual1.o" "$O/dual2.o" \
    "$O/gblink1.o" "$O/gblink2.o" "$O/fe_util.o" \
    "$O/gbcore_a.o" "$O/gbcore_b.o" "$O/gbcore_c.o" "$O/gbcore_d.o" \
    "$O/tgb_shared.o" "$O/ap_a.o" "$O/ap_b.o"
echo "built $OUT/sesssim"
