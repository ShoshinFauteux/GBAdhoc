# tools/linkbench/Makefile -- host build of the GBA link feasibility bench.
#
# Linux/WSL only.  Builds the unix core (x86 dynarec) with -DLINKBENCH (the
# sound.c FIFO counters; nothing else changes), archives it, and links
# lb_host against it with the shared frontend-common glue.  The core build
# is cleaned first so a psp1 object can never leak in.
#
#   make            -> lb_host
#   make INTERP=1   -> lb_host_interp (dynarec off: the reference arm)
CORE_DIR := ../..
CC       ?= gcc
CFLAGS   := -O2 -g -Wall -DGPSP_PERF_RIG -DLINKBENCH -I$(CORE_DIR)/frontend-common -I$(CORE_DIR)/netdrv \
            -I$(CORE_DIR)/libretro/libretro-common/include
SDLBIN := gpsp_sdl_lb
CORE_DEFS := -DLINKBENCH
ifeq ($(AWFIX),1)
  # bench-only candidate fix for the AW serial frame overflow (serial_proto.c)
  CORE_DEFS := -DLINKBENCH -DLINKBENCH_AWFIX
  SDLBIN := gpsp_sdl_lb_awfix
  CORE_LIB := libcore_awfix.a
  BIN := lb_host_awfix
else ifeq ($(INTERP),1)
  CORE_LIB := libcore_interp.a
  CORE_MAKE := HAVE_DYNAREC=0
  BIN := lb_host_interp
else
  CORE_LIB := libcore_dyn.a
  CORE_MAKE :=
  BIN := lb_host
endif

FE := $(CORE_DIR)/frontend-common
SRCS := lb_host.c $(FE)/fe_host.c $(FE)/fe_evt.c $(FE)/fe_util.c \
        $(FE)/fe_autopilot.c $(FE)/netpacket_host.c $(CORE_DIR)/netdrv/netdrv.c         $(CORE_DIR)/netdrv/gb_link.c
# fe_host.c also drives the GB core; link it so fe_host is used unmodified.
GB   := $(CORE_DIR)/gbcore
GBSRCS := $(GB)/gbcore_tgbdual.c $(wildcard $(GB)/tgbdual/*.c)

all: $(BIN)

$(BIN): lb_host.c $(CORE_LIB) libgb.a
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(CORE_LIB) libgb.a -lz -lm -lpthread -lstdc++

libgb.a: $(GBSRCS)
	rm -rf gbobj && mkdir gbobj
	cd gbobj && $(CC) -O2 -std=gnu11 -fgnu89-inline -fno-strict-aliasing 	  -I../$(GB) -I../$(FE) -c $(addprefix ../,$(GBSRCS))
	ld -r -o gbobj/gb_all.o gbobj/*.o
	objcopy -w --keep-global-symbol='gbcore_*' gbobj/gb_all.o
	ar rcs $@ gbobj/gb_all.o && rm -rf gbobj

$(CORE_LIB):
	$(MAKE) -C $(CORE_DIR) platform=unix clean
	$(MAKE) -C $(CORE_DIR) platform=unix $(CORE_MAKE) CODE_DEFINES="$(CORE_DEFS)" -j$$(nproc)
	cd $(CORE_DIR) && ar rcs tools/linkbench/$(CORE_LIB) \
	  $$(find . -name '*.o' \
	       -not -path './sdl/*' -not -path './psp/*' \
	       -not -path './frontend-common/*' -not -path './netdrv/*' \
	       -not -path './tools/*' -not -path './tests/*' \
	       -not -path './mips/*' -not -path './arm/*' \
	       -not -path './3ds/*' -not -path './jni/*')

clean:
	rm -f lb_host lb_host_interp gpsp_sdl_lb* libcore_*.a libgb.a

.PHONY: all clean

# The SDL desktop twin linked against this core (sdl/Makefile's own archive
# omits the GB core that fe_host.c needs).  Used by p_link.sh.
SDLSRC := $(CORE_DIR)/sdl/main_sdl.c $(SRCS:lb_host.c=) $(CORE_DIR)/netdrv/transport_udp.c
# SDL_ND empty (default) = the PSP ad-hoc netdrv profile (adaptive RTO).
# SDL_ND=-DND_RTO_FIRST_MAX_US=30000 = sdl/Makefile's desktop profile.
$(SDLBIN): $(CORE_LIB) libgb.a
	$(CC) $(CFLAGS) $(SDL_ND) $$(sdl2-config --cflags) -o $@ \
	  $(SDLSRC) $(CORE_LIB) libgb.a $$(sdl2-config --libs) -lz -lm -lpthread -lstdc++
