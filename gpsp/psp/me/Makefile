# gbadhoc_me.prx — kernel module that boots the Media Engine (ADR-0080).
# Built as a sub-make from psp/Makefile; the .prx ships beside EBOOT.PBP
# and is loaded at runtime with kuKernelLoadModule (the usb_handoff.c
# pattern), so nothing here touches the EBOOT link.
TARGET = gbadhoc_me
OBJS = me_stub.o main.o me_render_data.o me_render_glue.o video.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra
# The renderer (video.cc, from the repo root) + its glue need the core headers
# and the core's C++ dialect.  main.c / me_render_data.c stay plain C (CFLAGS).
RENDER_INC = -I../.. -I../../libretro -I../../libretro/libretro-common/include
# ADR-0039 pixel format, inherited from psp/Makefile (which inherits the
# top-level default).  This used to be hardcoded here, which made it a THIRD
# independent copy of the same decision: a `PSP_PIXFMT=rgb565` build gave a core
# and a frontend on one layout and an ME renderer on the other, with R and B
# swapped in ME mode only, silently.  The core/frontend pair is a link error
# now; the PRX is loaded at runtime and cannot be, so it is told instead.
PSP_PIXFMT ?= psp5650
ifeq ($(PSP_PIXFMT), psp5650)
ME_PIXFMT_DEF = -DUSE_PSP_RGB565_FORMAT
else
ME_PIXFMT_DEF =
endif
RENDER_DEF = $(ME_PIXFMT_DEF) -D__LIBRETRO__ -DINLINE=inline \
             -DHAVE_STDINT_H -DHAVE_INTTYPES_H
CXXFLAGS = $(CFLAGS) $(RENDER_INC) $(RENDER_DEF) -fno-rtti -fno-exceptions -std=c++11
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
PRX_EXPORTS = exports.exp

USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

LDFLAGS = -nostartfiles
# sceSysregMe* stubs live in libpspkernel.a itself (verified with psp-nm;
# same single-library link as the DaedalusX64 reference).
LIBS = -lpspkernel

PSP_FW_VERSION = 500

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# AFTER the include so it does not steal the default goal (the PRX target).
# video.cc lives in the repo root; compile it into the PRX with the render flags.
video.o: ../../video.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<
