# tools/drprof/Makefile -- the dynarec profiling twin and differential oracle.
#
# Runs INSIDE the drprof docker image (tools/drprof/docker/Dockerfile: Ubuntu
# 24.04 + gcc-mipsel-linux-gnu + qemu 9.2.2 built with --enable-plugins), on a
# copy of the tree (dr_build.sh rsyncs it; the root make writes objects next to
# the sources, so never point this at a tree another build is using).
#
#   make VARIANT=base                     -> bin/dr_host_base      (twin, profiled)
#   make VARIANT=x VDEFS="-DFOO=1"        -> bin/dr_host_x         (a translator variant)
#   make plugin                           -> bin/drprof_plugin.so
#
# Every variant is the PSP release dynarec configuration (tools/build.sh
# CORE_COMMON, translated to defines below) plus VDEFS.  The core is CLEAN
# rebuilt per variant: the root make tracks neither headers nor CFLAGS
# (core-vs-frontend-defines), and a stale object is exactly how an A/B
# silently compares a build with itself.
CORE_DIR := ../..
CROSS    ?= mipsel-linux-gnu-
CC       := $(CROSS)gcc
AR       := $(CROSS)ar
VARIANT  ?= base
VDEFS    ?=
# DRPROF_TWIN adds translation-time bookkeeping only; emitted code is
# byte-identical (verified by dr_oracle.py base vs base-notwin).
TWIN     ?= 1

# == tools/build.sh CORE_COMMON (the shipped dynarec configuration) ==========
RELEASE_DEFS := -DSMC_GATES -DSMC_GATES_SIMPLE -DSMC_GATES_RANKED=1 \
  -DSMC_GATE_BITMAP=1 -DSMC_PARTIAL_SAFE -DSMC_PARTIAL_STABLE_THUNK \
  -DSMC_PARTIAL_DIRECT_LINKS -DGBA_PC_MASK=1 -DBADJUMP_SAFE=1 \
  -DSMC_GATE_CHARGE=1 -DDISPATCH_CYCLE_CHECK=1 -DDMA_SMC_FLUSH=1 \
  -DUSE_PSP_RGB565_FORMAT
ifeq ($(TWIN),1)
TWIN_DEFS := -DDRPROF_TWIN
endif
# -g: line tables for addr2line (drprof_plugin hot=); does not change codegen
CORE_DEFS := $(RELEASE_DEFS) $(TWIN_DEFS) $(VDEFS) -g

CFLAGS := -O2 -g -Wall -fno-pic -fno-pie -mno-shared -march=mips32r2 \
          -DGPSP_PERF_RIG $(TWIN_DEFS) -I$(CORE_DIR) \
          -I$(CORE_DIR)/frontend-common -I$(CORE_DIR)/netdrv \
          -I$(CORE_DIR)/libretro/libretro-common/include -I$(CORE_DIR)/gbcore
FE   := $(CORE_DIR)/frontend-common
SRCS := dr_host.c $(FE)/fe_host.c $(FE)/fe_evt.c $(FE)/fe_util.c \
        $(FE)/fe_autopilot.c $(FE)/netpacket_host.c $(CORE_DIR)/netdrv/netdrv.c \
        $(CORE_DIR)/netdrv/gb_link.c $(FE)/fe_gblink.c dr_gbstub.c
GB     := $(CORE_DIR)/gbcore
GBSRCS := $(GB)/gbcore_tgbdual.c $(wildcard $(GB)/tgbdual/*.c)

BIN  := bin/dr_host_$(VARIANT)
CORE := bin/libcore_$(VARIANT).a

all: $(BIN)

$(BIN): dr_host.c $(CORE) bin/libgb.a
	$(CC) $(CFLAGS) -static -no-pie -o $@ $(SRCS) $(CORE) bin/libgb.a -lm -lpthread -lstdc++
	$(CROSS)nm -S --defined-only $@ > $@.nm
	$(CROSS)nm -A --defined-only $(CORE) > $@.members
	echo "$(CORE_DEFS)" > $@.defs

$(CORE):
	mkdir -p bin
	$(MAKE) -C $(CORE_DIR) platform=drprof-mipsel clean >/dev/null
	$(MAKE) -C $(CORE_DIR) platform=drprof-mipsel CODE_DEFINES="$(CORE_DEFS)" -j$$(nproc) >/dev/null
	cp $(CORE_DIR)/gpsp_libretro_drprof_mipsel.a $@
	$(MAKE) -C $(CORE_DIR) platform=drprof-mipsel clean >/dev/null

bin/libgb.a: $(GBSRCS) $(CORE)
	mkdir -p bin && rm -rf gbobj && mkdir gbobj
	cd gbobj && $(CC) -O2 -std=gnu11 -fgnu89-inline -fno-strict-aliasing -fno-pic -fno-pie -mno-shared \
	  -I../$(GB) -I../$(FE) -c $(addprefix ../,$(GBSRCS))
	$(CROSS)ld -r -o gbobj/gb_all.o gbobj/*.o
	# lb_host localizes every non-gbcore_ global; on MIPS that breaks the
	# GOT16/LO16 relocation pairing, so RENAME only the globals that collide
	# with the GBA core (vram, oam, ...) instead.
	$(CROSS)nm --defined-only -g gbobj/gb_all.o | awk '{print $$3}' | sort -u > gbobj/gb.syms
	$(CROSS)nm --defined-only -g bin/libcore_$(VARIANT).a 2>/dev/null | awk 'NF==3{print $$3}' | sort -u > gbobj/core.syms
	comm -12 gbobj/gb.syms gbobj/core.syms | grep -v '^gbcore_' | awk '{print $$1" tgbx_"$$1}' > gbobj/rename.txt
	$(CROSS)objcopy --redefine-syms=gbobj/rename.txt gbobj/gb_all.o
	$(AR) rcs $@ gbobj/gb_all.o && rm -rf gbobj

plugin: bin/drprof_plugin.so
bin/drprof_plugin.so: drprof_plugin.c
	mkdir -p bin
	gcc -O2 -g -shared -fPIC -Wall -I/opt/qemu/include $$(pkg-config --cflags glib-2.0) \
	  -o $@ $< $$(pkg-config --libs glib-2.0)

clean:
	rm -rf bin gbobj

.PHONY: all clean plugin
