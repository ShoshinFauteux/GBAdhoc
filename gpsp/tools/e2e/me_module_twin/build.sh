#!/usr/bin/env bash
# build.sh BUILD_DIR [extra -D flags...] -- compile the Media Engine module's
# renderer for the desktop, beside the ME_TIMING_SIM twin (mtm_driver.c says
# what for).  Called by run_me_midframe.sh when MMF_MODULE=1; BUILD_DIR is the
# twin's rsynced source tree.  Leaves mtm_module.o and mtm_driver.o in its
# root, where the twin's archive step picks them up.
#
# The four sources and their defines are psp/me/Makefile's (OBJS, RENDER_DEF),
# minus the pixel-format define: the twin's core is built without
# USE_PSP_RGB565_FORMAT, and the module must agree with the core it is fed by,
# here as on the PSP.
set -euo pipefail
B=$1; shift
cd "$B"
INC="-I. -I./libretro -I./libretro/libretro-common/include"
DEF="-D__LIBRETRO__ -DINLINE=inline -DME_PRX_BUILD -DHAVE_STDINT_H -DHAVE_INTTYPES_H"
# The module's cache ops are MIPS builtins; a desktop has no second cache to
# invalidate.  -fpermissive: the glue casts pointers to 32-bit integers, which
# is what a PSP pointer is and why the driver keeps every buffer below 4 GiB.
NOCACHE='-D__builtin_allegrex_cache(op,addr)=((void)0)'
T=$(mktemp -d)
g++ $INC $DEF "$NOCACHE" "$@" -O2 -fPIC -fno-rtti -fno-exceptions -std=c++11 \
    -fpermissive -w -c -o "$T/video.o" video.cc
g++ $INC $DEF "$NOCACHE" "$@" -O2 -fPIC -fno-rtti -fno-exceptions -std=c++11 \
    -fpermissive -w -Ipsp/me -c -o "$T/glue.o" psp/me/me_render_glue.cc
g++ $INC $DEF "$NOCACHE" "$@" -O2 -fPIC -fno-rtti -fno-exceptions -std=c++11 \
    -fpermissive -w -Ipsp/me -c -o "$T/copy.o" psp/me/me_copy.cc
gcc -O2 -fPIC -c -o "$T/data.o" psp/me/me_render_data.c
ld -r -o "$T/mod.o" "$T/video.o" "$T/glue.o" "$T/copy.o" "$T/data.o"

# Everything the module DEFINES becomes mep_*; what it merely uses must be the
# C library and nothing else -- on the PSP these four objects link against the
# kernel's libc alone, so any other undefined symbol here is a module that
# would not have linked.
nm -u "$T/mod.o" | awk '{print $2}' | sort -u > "$T/undef"
if grep -vxE 'memcpy|memset|memmove|memcmp|__stack_chk_fail|_GLOBAL_OFFSET_TABLE_' "$T/undef"; then
  echo "me_module_twin: the module references the symbols above" >&2
  exit 1
fi
objcopy --prefix-symbols=mep_ "$T/mod.o"
sed 's/.*/mep_& &/' "$T/undef" > "$T/redef"
objcopy --redefine-syms="$T/redef" "$T/mod.o" mtm_module.o
gcc -O2 -fPIC -Wall -Wextra -I. -c -o mtm_driver.o tools/e2e/me_module_twin/mtm_driver.c
rm -rf "$T"
