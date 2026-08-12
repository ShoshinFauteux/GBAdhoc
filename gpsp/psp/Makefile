# psp/Makefile — native PSP frontend -> EBOOT.PBP (Phase 1, plan §5).
#
# Requires the core static lib first (built in the pspdev docker image):
#   docker run --rm -v <repo>:/build -w /build pspdev/pspdev \
#       sh -c 'make platform=psp1 clean && make platform=psp1'
# then this Makefile (same image):
#   docker run --rm -v <repo>:/build -w /build/psp pspdev/pspdev make
#
# Notes:
#  - -G0 matches the core's CFLAGS (no small-data section; FRONTEND-AUDIT §10).
#  - The core archive contains C++ objects (video.cc) built with
#    -fno-rtti -fno-exceptions and no libstdc++ dependencies; linking with
#    psp-gcc works (upstream links the .so with gcc on purpose).

TARGET = gpsp_adhoc
OBJS = main_psp.o usb_handoff.o \
       me_host.o \
       video_psp.o \
       osd_psp.o \
       ui_psp.o \
       config_psp.o \
       font_8x16.o \
       netdrv_core.o \
       transport_adhoc.o \
       ../frontend-common/fe_host.o \
       ../frontend-common/fe_evt.o \
       ../frontend-common/fe_util.o \
       ../frontend-common/fe_autopilot.o \
       ../frontend-common/netpacket_host.o

INCDIR   = ../frontend-common ../netdrv ../libretro/libretro-common/include ..
# ND_* knobs — the PSP/ad-hoc profile (ADR-0016, ADR-0017).  Two changes
# from the Phase-4 halving, both driven by the 2026-08-01 field logs
# (docs/HANDOFF.md issue #2):
#
#  * ND_MAX_PAYLOAD 560 -> 144.  ADR-0003 sized frames for the link-CABLE
#    Advance Wars protocol (~520 B), which is out of scope; RFU — all this
#    product carries — maxes at 104 B and the largest control payload is the
#    138 B roster.  Right-sizing the slot pays for a 4x deeper tx ring
#    (96 -> 384) and a 2x window at NEUTRAL memory: the netdrv instance goes
#    ~322 KiB -> ~347 KiB while the transport's .bss ring drops ~37 -> ~11 KiB.
#    An oversize payload is refused loudly, never truncated (netdrv.h).
#  * RTO floor 30 ms -> 100 ms, ceiling 240 -> 800 ms.  Real ad-hoc RTT is
#    40-80 ms; the loopback-tuned 30 ms floor fired before an ACK could
#    physically arrive (measured retx/acked 2.8, 68 % duplicates, zero radio
#    loss).  The floor only bounds the adaptive estimator from below.
# -MMD -MP: AUTOMATIC HEADER DEPENDENCIES.  Do not remove — this closes a
# silent, shipped, hardware-visible corruption bug (ADR-0035).
#
# build.mak supplies only an implicit `%.o: %.c` rule, so before this line a
# change to a HEADER rebuilt nothing.  Adding a field to `psp_config` in
# config_psp.h therefore left ui_psp.o and osd_psp.o compiled against the OLD
# struct layout while config_psp.o and main_psp.o used the new one — every
# translation unit agreeing on the symbol and disagreeing on the offsets.  It
# does not fail at link and it does not warn.  In the field it wrote
# `blit_mode = 1347637319` (= 0x50535047, the ASCII "GPSP" of a room code
# stored four bytes early) into the user's config.ini.
#
# `-MMD` emits a .d beside each .o listing its headers; `-MP` adds phony
# targets so a DELETED header does not wedge the build; the `-include` at the
# bottom of this file feeds them back to make.
# ADR-0039 pixel format.  MUST MATCH the top-level Makefile's PSP_PIXFMT.
# `psp5650` means the CORE already emits the GE's native channel order, so
# video_psp.c's staging step is a straight copy instead of a per-pixel R/B
# swap.  Set it in ONE place for both builds:
#     make PSP_PIXFMT=rgb565 platform=psp1      (core)
#     make PSP_PIXFMT=rgb565 -C psp             (frontend)
# Getting the two out of step does not fail to build and does not warn -- it
# renders with R and B swapped.  tools/e2e/run_gu_color_test.sh phase A pins
# the frontend side of this absolutely; the boot log's `pixfmt=` says which
# layout the binary in your hand expects.
PSP_PIXFMT ?= psp5650
ifeq ($(PSP_PIXFMT), psp5650)
PIXFMT_DEF = -DUSE_PSP_RGB565_FORMAT
else
PIXFMT_DEF =
endif

# ND_RTO_MAX_US: THE CEILING MUST NOT SIT BELOW WHAT THE ESTIMATOR COMPUTES.
#
# At 59.73 fps the client measured srtt=495ms rttvar=166ms, so RFC 6298 wants
# an RTO of srtt + 4*rttvar = ~1157ms.  The ceiling was 800ms, so EVERY packet
# was guaranteed to time out before its acknowledgement could arrive -- a
# retransmit storm by construction, not by loss.  Measured: retx_pct 421% on
# the client, 360% on the host, txq_hi 336.
#
# 2500000 leaves headroom above the ~1157ms observed without being unbounded;
# the estimator still governs, this only stops the clamp from lying.  Note the
# floor stays at 200000: the problem was never that we resend too slowly.
#
# This does NOT explain retx_age of 3063ms (host) against a 200ms RTO -- that
# is a separate fault, the ARQ scan not running, and is tracked separately.
# ND_WINDOW 128 -> 256: does the window STALL, or is it simply too small?
#
# At 59.73 the host reached txq_hi 886 against a 128-frame window -- a backlog
# 7x deeper than the window can hold in flight -- while pumpgap measured 13-16ms
# mean (the ARQ scan runs fine) and retx_age still hit 1.8-2.1 SECONDS. Two
# explanations fit: the window stalls head-of-line on one unacknowledged packet
# (cumulative-only ACK, no SACK), or it is just too narrow for full-speed
# traffic. Doubling it separates them for one line instead of implementing
# selective acknowledgement on a guess:
#   retx_age and txq_hi collapse  -> the window was too small, keep this
#   both unchanged                -> head-of-line blocking, SACK is the fix
#
# NOT FREE: ND_WINDOW sizes nd_rxslot reorder[ND_WINDOW] (netdrv.c:211), so
# this also doubles the per-peer reorder buffer. Watch EVT mem_free.
# EXTRA_DEFS: build-variant defines from the command line, e.g. the ADR-0067
# playable build (`make EXTRA_DEFS=-DGPSP_PLAYABLE PSP_EBOOT_TITLE="..."`).
# Empty for every normal build, so it cannot change the harness binary.
EXTRA_DEFS ?=
CFLAGS   = -O2 -G0 -Wall -Wextra -MMD -MP $(PIXFMT_DEF) $(EXTRA_DEFS) \
           -DND_MAX_PAYLOAD=144 -DND_WINDOW=256 -DND_TXQ_CAP=384 \
           -DND_RTO_MIN_US=200000 -DND_RTO_MAX_US=2500000
ASFLAGS  = $(CFLAGS)

CORE_LIB = ../gpsp_libretro_psp1.a
# NOTE: deliberately NO -lpspnet / -lpsputility here.  build.mak's default
# tail links -lpspnet, and gcc's implicit spec closure links libpsputility
# (libcglue's __timezone_update pulls sceUtilityGetSystemParamInt from it).
# Naming either one earlier splits that module's import-stub block across
# two archive scans and psp-fixup-imports fails with "stubs out of order"
# (= broken imports on real hardware).  Our sceNetInit/sceUtility* refs
# resolve from those late scans in a single contiguous block.
LIBS = $(CORE_LIB) -lpspgu -lpspge -lpspaudio -lpsppower \
       -lpspusb -lpspusbstor -lpspkubridge -lpspsystemctrl_user -lpspctrl \
       -lpspnet_adhoc -lpspnet_adhocctl -lpspwlan -lm

# ADR-0080: the Media Engine kernel module builds as a sub-make and ships
# beside the EBOOT (loaded at runtime via kuKernelLoadModule; nothing links
# against it).  The rule is defined AFTER the build.mak include below, so it
# does not steal the default goal; it is listed here as an EXTRA_TARGET so
# `all` builds it.
EXTRA_TARGETS   = EBOOT.PBP me/gbadhoc_me.prx
# Generic-build XMB branding (user direction, 2026-08-08): title "GBAdhoc",
# icon + background derived from the user-supplied logo by assets/make_assets.py
# (was "PSP AGB" after the adapter's AGB-015 model number).  The HARNESS build
# overrides these on the command line with assets/harness_* so the instrumented
# console is unmistakable at a glance — inverted to a dark amber field — and
# tools/make_variant.sh does the same per game.
PSP_EBOOT_TITLE ?= GBAdhoc
PSP_EBOOT_ICON  = assets/gbadhoc_icon0.png
PSP_EBOOT_PIC1  = assets/gbadhoc_pic1.png

# Large-memory policy is SPLIT by build flavour (MEMSIZE in PARAM.SFO):
#  - PLAYABLE requests the 64 MiB layout (MEMSIZE=1).  On a 2000+/CFW the
#    ROM cache's greedy 1 MB loop then reaches the full 32 blocks and 32 MB
#    carts (Mother 3, Unbound) run fully memory-resident; a 1000 has no
#    extra RAM and ignores the flag, so its behaviour is unchanged.
#  - HARNESS pins MEMSIZE=0 (the `harness` target below): PSP-1000 is the
#    memory budget target (FRONTEND-AUDIT §8) and PPSSPP models MEMSIZE, so
#    the Gate-1 mem_free measurement must stay honest for the 1000-series.
PSP_LARGE_MEMORY ?= 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# `make harness` — the branded harness build.  The [HARNESS] title + dark
# icon are the XMB half of the differentiation (the in-app half is the black
# page background, ui_psp.c).  These lived only in hand-typed build commands
# for months and silently dropped out twice; now the target owns them.
# AFTER the include, or it becomes the default goal and recurses forever —
# the same trap as me/Makefile's video.o rule.  Third strike.
harness:
	$(MAKE) clean
	$(MAKE) PSP_EBOOT_TITLE="GBAdhoc [HARNESS]" \
	        PSP_EBOOT_ICON=assets/harness_icon0.png \
	        PSP_EBOOT_PIC1=assets/harness_pic1.png \
	        PSP_LARGE_MEMORY=0
.PHONY: harness

$(TARGET).elf: $(CORE_LIB)

# netdrv objects are compiled INTO psp/ (not in-tree next to their sources):
# a MIPS .o left in ../netdrv would be swept into the desktop core archive
# by sdl/Makefile's find (ADR-0005 stale-arch guard).
netdrv_core.o: ../netdrv/netdrv.c ../netdrv/netdrv.h ../netdrv/netdrv_wire.h
	$(CC) $(CFLAGS) -c -o $@ $<

transport_adhoc.o: ../netdrv/transport_adhoc.c ../netdrv/transport_adhoc.h ../netdrv/netdrv.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(CORE_LIB):
	$(error build the core first: make platform=psp1 in the repo root (pspdev docker))

# ADR-0035: feed the -MMD dependency files back in, so touching any header
# rebuilds every object that includes it.  Must come AFTER build.mak (which
# defines the compile rules) and after OBJS is complete.  The leading `-`
# makes a first, .d-less build silent instead of fatal.
DEPS := $(OBJS:.o=.d)
-include $(DEPS)

# `clean` is build.mak's; the .d files are ours, so sweep them too or a
# stale one outlives the object it describes.
clean: clean-deps clean-me
clean-deps:
	-@rm -f $(DEPS)
.PHONY: clean-deps

# ADR-0080: the ME kernel module (defined here, after the include, so it
# never becomes the default goal — that bug once built the PRX and nothing
# else).  PHONY so its own sub-make owns dependency tracking.
.PHONY: me/gbadhoc_me.prx clean-me
me/gbadhoc_me.prx:
	$(MAKE) -C me
clean-me:
	-$(MAKE) -C me clean
