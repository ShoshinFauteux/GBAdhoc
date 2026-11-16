# sdl/Makefile — desktop twin frontend (WSL/Linux, SDL2).
#
# Links the gpsp libretro core STATICALLY: builds the core with
# `make platform=unix` in the repo root, then archives its objects into
# libgpsp_core.a (ADR-0006). The core build is cleaned first so stale
# objects from a psp1 build (same .o names, different arch) can never leak in.
#
# This is a TEST TOOL (desktop e2e: trades, exit room, saves), not a product.
# The core runs the INTERPRETER (HAVE_DYNAREC=0): the dynarec is MIPS-only
# (jit_code.h), and porting the x86 one back is not worth its upkeep.  The GB
# stack links the way the dynarec twin's does (tools/drprof/Makefile): one TGB
# Dual instance with only its gbcore_* names global, plus the inert dual-link
# stubs (dr_gbstub.c), so a GB link session reports "unavailable" here.
#
#   make          -> gpsp_sdl (builds core archive if missing)
#   make core     -> force-rebuild the core archive
#   make clean    -> remove frontend objects only
#   make distclean-> also remove the core archive

CORE_DIR := ..
CORE_LIB := libgpsp_core.a

CC      ?= gcc
# ND_* knobs — the desktop/UDP transport profile (ADR-0017).  Payload budget
# and queue sizing keep the Gate-3 defaults (560 B / window 32 / backlog 192:
# the cable protocols stay representable here, unlike the PSP ad-hoc profile).
# ND_RTO_FIRST_MAX_US=30000 pins the LOSS-RECOVERY deadline at ADR-0010's
# measured 30 ms while leaving the adaptive RTO to pace everything else: this
# rig injects 5 % loss with nothing underneath to repair it, and the emulated
# RFU exchange dies if a lost frame takes much longer than that to come back.
# Real ad-hoc has 802.11 retransmitting below us, so the PSP profile leaves
# this unset and lets the estimator govern.
CFLAGS  := -O2 -g -Wall -Wextra -DND_RTO_FIRST_MAX_US=30000 \
           -I$(CORE_DIR)/frontend-common \
           -I$(CORE_DIR)/netdrv \
           -I$(CORE_DIR)/libretro/libretro-common/include \
           $(shell sdl2-config --cflags)
LDLIBS  := $(shell sdl2-config --libs) -lm -lpthread

COMMON_SRC := $(CORE_DIR)/frontend-common/fe_host.c \
              $(CORE_DIR)/frontend-common/fe_evt.c \
              $(CORE_DIR)/frontend-common/fe_util.c \
              $(CORE_DIR)/frontend-common/fe_autopilot.c \
              $(CORE_DIR)/frontend-common/netpacket_host.c
OBJS := obj/main_sdl.o obj/fe_host.o obj/fe_evt.o obj/fe_util.o \
        obj/fe_autopilot.o obj/netpacket_host.o \
        obj/netdrv.o obj/transport_udp.o \
        obj/fe_gblink.o obj/gb_link.o obj/dr_gbstub.o obj/gb_core.o

# The GB core (gbcore/Makefile's flags, plus the PSP's TGB_LAZY_TIMER).  Its
# objects go under obj/gb, never next to the sources, where the PSP build
# keeps MIPS objects of the same names.
GB_DIR    := $(CORE_DIR)/gbcore
GB_SRC    := $(GB_DIR)/gbcore_tgbdual.c \
             $(addprefix $(GB_DIR)/tgbdual/,apu.c cpu.c gb.c lcd.c mbc.c rom.c \
               sgb.c tgb_shared.c)
GB_CFLAGS := -O2 -g -std=gnu11 -fgnu89-inline -fno-strict-aliasing \
             -Wno-unused-parameter -DTGB_LAZY_TIMER -I$(GB_DIR) \
             -I$(CORE_DIR)/frontend-common

# REWIND=1: the rewind prototype (docs/REWIND.md).  Off by default; without
# it not one line of the rewind code is compiled.
ifeq ($(REWIND),1)
CFLAGS += -DREWIND
OBJS   += obj/fe_rewind.o obj/fe_rewind_pack.o
endif
# e.g. EXTRA_CFLAGS=-DGPSP_PERF_RIG for the harness's 1024-step scripts
# (tools/rewind_test.py with a long input script needs it).
CFLAGS += $(EXTRA_CFLAGS)

all: gpsp_sdl

gpsp_sdl: $(OBJS) $(CORE_LIB)
	$(CC) -o $@ $(OBJS) $(CORE_LIB) $(LDLIBS)

obj/main_sdl.o: main_sdl.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c -o $@ $<

obj/fe_%.o: $(CORE_DIR)/frontend-common/fe_%.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c -o $@ $<

obj/netpacket_host.o: $(CORE_DIR)/frontend-common/netpacket_host.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c -o $@ $<

obj/netdrv.o: $(CORE_DIR)/netdrv/netdrv.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c -o $@ $<

obj/transport_udp.o: $(CORE_DIR)/netdrv/transport_udp.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c -o $@ $<

obj/gb_link.o: $(CORE_DIR)/netdrv/gb_link.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c -o $@ $<

obj/dr_gbstub.o: $(CORE_DIR)/tools/drprof/dr_gbstub.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -I$(GB_DIR) -c -o $@ $<

# One relocatable whose only globals are the gbcore_* API: TGB Dual's own
# globals (vram, oam, ...) would collide with the GBA core's (psp/Makefile).
obj/gb_core.o: $(GB_SRC)
	@rm -rf obj/gb && mkdir -p obj/gb
	for s in $(GB_SRC); do \
	  $(CC) $(GB_CFLAGS) -c -o obj/gb/$$(basename $$s .c).o $$s || exit 1; done
	ld -r -o $@ obj/gb/*.o
	objcopy -w --keep-global-symbol='gbcore_*' $@

$(CORE_LIB):
	$(MAKE) -C $(CORE_DIR) platform=unix clean
	$(MAKE) -C $(CORE_DIR) platform=unix HAVE_DYNAREC=0 REWIND=$(REWIND) REWIND_MUTANT=$(REWIND_MUTANT) -j$(shell nproc)
	cd $(CORE_DIR) && ar rcs sdl/$(CORE_LIB) \
	  $$(find . -name '*.o' \
	       -not -path './sdl/*' -not -path './psp/*' \
	       -not -path './frontend-common/*' -not -path './netdrv/*' \
	       -not -path './tools/*' -not -path './gbcore/*' \
	       -not -path './tests/*' \
	       -not -path './mips/*' -not -path './arm/*' \
	       -not -path './3ds/*' -not -path './jni/*')

core:
	rm -f $(CORE_LIB)
	$(MAKE) $(CORE_LIB)

clean:
	rm -rf obj gpsp_sdl

distclean: clean
	rm -f $(CORE_LIB)

.PHONY: all core clean distclean
