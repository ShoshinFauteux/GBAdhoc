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
OBJS = main_psp.o usb_handoff.o icprobe.o heap_trace.o \
       ff_psp.o \
       me_host.o \
       home_host.o \
       sleep_q.o \
       video_psp.o \
       osd_psp.o \
       ui_psp.o \
       sidecar_dirs.o \
       config_psp.o \
       ctl_map.o \
       mgift_net.o \
       mgift_cart.o \
       mgift_payload.o \
       font_8x16.o \
       stb_impl.o \
       netdrv_core.o \
       transport_adhoc.o \
       ../frontend-common/fe_host.o \
       ../frontend-common/fe_evt.o \
       ../frontend-common/fe_util.o \
       ../frontend-common/fe_png.o \
       ../frontend-common/fe_favs.o \
       ../frontend-common/fe_share.o share_psp.o \
       ../frontend-common/fe_linkmode.o \
       ../frontend-common/fe_autopilot.o \
       ../frontend-common/netpacket_host.o \
       ../netdrv/gb_link.o \
       ../frontend-common/fe_gblink.o \
       ../gbcore/gbcore_dual.o \
       ../gbcore/tgbdual/tgb_shared.o \
       ../gbcore/gbcore_tgbdual_linked.o \
       ../gbcore/gbcore_tgbdual_linked_b.o

# Vendored TGB Dual (GB/GBC; gbcore/tgbdual/, from MasterBoy).  Its flags are
# scoped to its own objects and assigned after the build.mak include, beside
# the other object rules: GNU89 inline semantics (it defines non-static
# `inline` functions and takes their addresses), no strict aliasing (VRAM and
# ROM bytes are read through word pointers), and no unused-parameter warning
# (every I/O and opcode handler shares one dispatch signature).  All other
# warnings stay on; see gbcore/tgbdual/LOCAL-PATCHES.md.
#
# -O3, as MasterBoy built it.  -Os was tried (docs/GB-LINK.md section 14):
# a twin-trace I-cache model predicted ~2 ms less a frame for the two cores
# of a link session, and the PSP-3000/PSP-1000 pair measured the opposite --
# +0.4..+1.7 ms a frame, window for window (hardware run perf1, D vs O and
# A vs G).  The model was wrong; TGB_OPT=-Os still builds that variant.
TGB_OPT ?= -O3
TGB_OBJS = $(addprefix ../gbcore/tgbdual/,apu.o cpu.o gb.o lcd.o mbc.o rom.o \
           sgb.o)
TGB_CFLAGS = $(TGB_OPT) -fgnu89-inline -fno-strict-aliasing -Wno-unused-parameter
# TGB_LAZY_TIMER (docs/GB-CORE-PERF.md): the timer walk only when an event
# can fall inside the step.  Exact (tgb_ref state hashes, linkplay).  ON by
# default since 4.5.0: hardware build gbp-b2 (v2) with armed batching took
# real two-PSP Gen 2 trades 49.8 -> 58.5 fps, Gen 1 55.1 -> 59.2, no desyncs
# (2026-10-28/30).  TGB_LAZY_TIMER=0 is the kill switch.  The adapter needs
# nothing (cpu_timer_dirty on state load is defined either way).
TGB_LAZY_TIMER ?= 1
ifeq ($(TGB_LAZY_TIMER),1)
TGB_CFLAGS += -DTGB_LAZY_TIMER
endif
# TGB_FAST_TILE=1 (docs/GB-CORE-PERF.md): tile rows decoded by a 1 KiB table
# instead of bit by bit.  Exact (pixel hashes); OFF: no gain on hardware
# (gbp-c2 vs gbp-b2, 13/13 "same", mean ~+0.7 % worse).
ifeq ($(TGB_FAST_TILE),1)
TGB_CFLAGS += -DTGB_FAST_TILE
endif
# The core is one relocatable object whose only global symbols are the
# gbcore_* API (rule below).  TGB Dual exports ~780 names -- vram, ram, oam,
# halt, speed, info, stack -- and the first link failed on `vram`, which the
# GBA core's mips_stub.o defines too.  Localising them all means no present
# or future GBA-side global can collide with the GB core's state.
GB_CORE_OBJS = ../gbcore/gbcore_tgbdual.o $(TGB_OBJS)
# A GB link session runs two Game Boys (gbcore/gbcore_dual.h), so the object
# is linked twice: instance A keeps the gbcore_* names, instance B is the
# same bytes with them renamed gbcoreb_*.  Everything else in the object is
# already local, so the copies share no state and cost nothing at run time.
# The partial link goes through gbcore/gbcore_instance.ld, which marks the
# instance's .data/.bss for gbcore_power_on.

INCDIR   = ../frontend-common ../netdrv ../libretro/libretro-common/include ..
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
# swap.  THREE things consume it, and they must agree:
#     make PSP_PIXFMT=rgb565 platform=psp1      (core)
#     make PSP_PIXFMT=rgb565 -C psp             (frontend, and it passes the
#                                                value down to me/ for the PRX)
# Getting them out of step does not fail to build and does not warn -- it
# renders with R and B swapped.  The core/frontend pair is now a LINK ERROR
# (gba_memory.c exports a symbol named after its layout, psp/video_psp.c
# references the one it expects); the PRX is loaded at runtime so it cannot be,
# which is why the value is threaded into that sub-make instead.
# tools/e2e/run_gu_color_test.sh phase A pins the frontend side absolutely;
# the boot log's `pixfmt=` says which layout the binary in your hand expects.
PSP_PIXFMT ?= psp5650
ifeq ($(PSP_PIXFMT), psp5650)
PIXFMT_DEF = -DUSE_PSP_RGB565_FORMAT
else
PIXFMT_DEF =
endif

# EXTRA_DEFS: build-variant defines from the command line, e.g. the ADR-0067
# playable build (`make EXTRA_DEFS=-DGPSP_PLAYABLE PSP_EBOOT_TITLE="..."`).
# Empty for every normal build, so it cannot change the harness binary.
EXTRA_DEFS ?=
# -g: line tables for tools/cachemap (docs/CACHE-MAP.md).  Debug info never
# changes code, and the EBOOT is packed from the STRIPPED elf: the release
# EBOOT is byte-identical with and without it (checked 2026-09-30).
# fe_features.h is the frontend's half of gpsp_features.h: force-included, so
# the transport profile and the playable defaults need no recipe.  What stays a
# make variable below changes what is compiled or linked (REWIND, GBA_LINK,
# CHEATS, LAB_HARNESS, LAYOUT_*, HOT_DATA_GUARD) or has to reach a sub-make
# (PSP_PIXFMT); TGB_* are scoped to the GB core's objects on purpose.
CFLAGS   = -O2 -g -G0 -Wall -Wextra -MMD -MP $(PIXFMT_DEF) $(EXTRA_DEFS) \
           -include fe_features.h
ASFLAGS  = $(CFLAGS)

# LAYOUT_PIN (default 1; docs/LAYOUT-PINNING.md): link the hot path FIRST in
# .text, in the order psp/layout/hot.ord gives (generated by
# tools/layout/gen_layout.py from the dynarec twin's profile; never hand-edit
# it), so its I-cache sets no longer move when unrelated code changes size.
# Every function gets its own section for that (-ffunction-sections), except
# the partial-linked GB core below, whose layout is its own.  The core archive
# must be built with the same LAYOUT_PIN (root Makefile; tools/build.sh passes
# both).  LAYOUT_PIN=0 is the pre-pinning link, byte for byte.
#
# LAYOUT_PAD_BYTES=N: an N-byte cold function that nothing calls (main_psp.c,
# layout_pad_cold).  As a probe it is the stand-in for "an unrelated change":
# pinned builds must not move a hot byte, unpinned ones shift everything after
# main_psp.o.  The value is part of the shipped layout; changing it changes
# the binary (tools/build.sh reads this line, make_release.sh checks it).
LAYOUT_PIN ?= 1
LAYOUT_PAD_BYTES ?= 16
ifeq ($(LAYOUT_PIN),1)
CFLAGS  += -ffunction-sections
# HOT_ORD: the ordering file (default the single-player order).  The link
# pair's candidate order is layout/hot_pair.ord (docs/GBA-LINK-MIRROR.md 25):
#   PSP_EXTRA="HOT_ORD=layout/hot_pair.ord" tools/build.sh ...
HOT_ORD ?= layout/hot.ord
LDFLAGS += -Wl,--section-ordering-file=$(HOT_ORD)
endif
ifneq ($(LAYOUT_PAD_BYTES),0)
CFLAGS  += -DLAYOUT_PAD_BYTES=$(LAYOUT_PAD_BYTES)
endif
# LAYOUT_PAD_RO_BYTES=N: the same in .rodata, before instance A's.  Whoever
# changes frontend code or strings re-tunes both pads so that instance A's
# cold text and tables stay where 4.5.0 had them mod 8 KiB
# (docs/LAYOUT-PINNING.md, "Keeping the pads").
LAYOUT_PAD_RO_BYTES ?= 32
CFLAGS  += -DLAYOUT_PAD_RO_BYTES=$(LAYOUT_PAD_RO_BYTES)

# CHEATS (default 1; CHEATS=0 is the kill switch): the per-game cheats menu
# (docs/CHEATS.md).  The ROOT make takes the same switch for the core half
# (cheats.c); tools/build.sh passes one value to both.
CHEATS ?= 1
ifeq ($(CHEATS),1)
CFLAGS += -DGPSP_CHEATS=1
OBJS   += ../frontend-common/fe_cheats.o
endif

# HEAP_TRACE=1 (diagnostic harness builds only, PSP_EXTRA): every allocation
# goes through psp/heap_trace.c, which can name what outlives an unload
# (from claude/inproc-return; docs/INPROC-RETURN.md, GBA-LINK-MIRROR.md 19).
HEAP_TRACE ?= 0
ifeq ($(HEAP_TRACE),1)
CFLAGS  += -DGPSP_HEAP_TRACE
LDFLAGS += -Wl,--wrap=_malloc_r -Wl,--wrap=_free_r -Wl,--wrap=_realloc_r            -Wl,--wrap=_calloc_r -Wl,--wrap=_memalign_r -Wl,--wrap=malloc            -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=memalign
endif

CORE_LIB = ../gpsp_libretro_psp1.a

# GBA_LINK=1: the GBA link mirror (docs/GBA-LINK-MIRROR.md).  The core must be
# built with GBA_LINK=1 too (root make; tools/build.sh does both).  Instance B
# is the WHOLE core archive partially linked and every global it defines
# renamed gbab_* -- the GB link's TGB Dual instance-B move, applied to gpSP.
# Its undefined references (libc, cache syscalls, the gbalink bus) resolve to
# the EBOOT's, so the two instances share nothing else.  Static: it costs every
# game its size in heap (the census in the doc).  Default 1 in both makes.
# REWIND (default 1; REWIND=0 compiles out every line of it): the rewind
# (docs/REWIND.md).  The core must be built with the same value (root make;
# tools/build.sh passes both).
REWIND ?= 1
ifeq ($(REWIND),1)
OBJS += ../frontend-common/fe_rewind.o ../frontend-common/fe_rewind_pack.o
CFLAGS += -DREWIND
endif
# RFU_SCHED=1: scheduled ticks for the emulated Wireless Adapter (rfu_sched.h,
# docs/RFU-COUPLING.md).  Off by default.  The core must be built with the
# same value (root make; tools/build.sh passes both).
# ONLINE (default 1; ONLINE=0 compiles out every line of it): EXPERIMENTAL
# online play (docs/ONLINE-TUNNEL.md), the link over the GBR1 relay /
# hole-punched direct path on infrastructure Wi-Fi.  Compiled in, OFF for the
# player until Settings > Online play is turned on; costs nothing per frame.
# Frontend only.
ONLINE ?= 1
ifeq ($(ONLINE),1)
# the session code is gbadhoc_online.prx (online/), loaded on demand: only
# the loader and the configuration are in the EBOOT (online_psp.c)
OBJS += online_psp.o
ONLINE_PRX = online/gbadhoc_online.prx
CFLAGS += -DGBADHOC_ONLINE
endif
RFU_SCHED ?= 0
ifeq ($(RFU_SCHED),1)
CFLAGS += -DRFU_SCHED
endif
# LAB_HARNESS=1: the GBAdhoc Lab task runner (psp/lab_harness.c; GBAdhoc-Lab
# docs/PROTOCOL.md).  Frontend only.  Off by default; without it lab_harness.c
# is not compiled and main_psp.c's LAB_HOOK()s expand to nothing.
LAB_HARNESS ?= 0
ifeq ($(LAB_HARNESS),1)
OBJS += lab_harness.o labab/budget.o labab/ab_io.o lab_ffpace.o
CFLAGS += -DLAB_HARNESS
endif
GBA_LINK ?= 1
# HOT_DATA_GUARD (default 1, gpsp_features.h; tools/build.sh follows the ROOT
# make's value): the guard's 8 KiB placement comes from this INSERT script, not
# from section alignment (psp/hot_data_guard.ld says why).
HOT_DATA_GUARD ?= 1
ifeq ($(HOT_DATA_GUARD),1)
LDFLAGS += -Wl,-T,hot_data_guard.ld
endif
ifeq ($(GBA_LINK),1)
OBJS += ../frontend-common/fe_gbadual.o ../gbalink/gbalink_bus.o \
        ../gbalink/gbalink_pair.o ../gbalink/gbalink_script.o gbalink_core_b.o \
        fe_autopilot_b.o
CFLAGS += -DGBA_LINK
endif
# NOTE: deliberately NO -lpspnet / -lpsputility here.  build.mak's default
# tail links -lpspnet, and gcc's implicit spec closure links libpsputility
# (libcglue's __timezone_update pulls sceUtilityGetSystemParamInt from it).
# Naming either one earlier splits that module's import-stub block across
# two archive scans and psp-fixup-imports fails with "stubs out of order"
# (= broken imports on real hardware).  Our sceNetInit/sceUtility* refs
# resolve from those late scans in a single contiguous block.
#
# THE SAME TRAP, RE-SPRUNG AND MEASURED (Mystery Gift, 2.0.5).  Mystery Gift
# calls sceNetApctl* and sceNetInet*, so the obvious move is to add
# -lpspnet_apctl -lpspnet_inet here.  Do not: BOTH ARE ALREADY LINKED, and
# naming either one produces
#
#     Warning: could not fixup imports, stubs out of order.
#
#   * -lpspnet_inet comes from psp-gcc's own spec.  `psp-gcc -dumpspecs`
#     *lib: ends with "-lpsputility -lpsprtc -lpspnet_inet -lpspnet_resolver",
#     because libcglue's BSD-socket wrappers import sceNetInet*.
#   * -lpspnet_apctl comes from build.mak's default tail, "-lpspnet
#     -lpspnet_apctl".
#
# Measured: with neither named, the link is clean and every sceNetInet*/
# sceNetApctl* symbol in mgift_net.o resolves (ELF 36c166a9, 0 warnings).  Name
# them and the same ELF grows a split stub block (42308fb2, 1 warning).  So the
# correct change to this list for Mystery Gift is NO CHANGE.
#
# Mystery Gift creates the PSP's connection profile itself, so the player never
# has to leave the emulator for XMB -- but that needs NOTHING added here.  It
# uses sceUtility*NetParam, which lives in libpsputility, and libpsputility is
# already pulled in implicitly (see the paragraph above -- naming it explicitly
# is exactly what breaks the stub ordering).
#
# AND NOTE THE FAILURE MODE, which the paragraph above gets wrong: it is a
# WARNING, not an error.  make still exits 0, psp-fixup-imports gives up, and
# you get an EBOOT with broken imports that built "successfully" and would die
# on real hardware.  Never trust make's exit status for this -- grep the log.
LIBS = $(CORE_LIB) -lpspgu -lpspge -lpspaudio -lpsppower \
       -lpspusb -lpspusbstor -lpspkubridge -lpspsystemctrl_user -lpspctrl \
       -lpspnet_adhoc -lpspnet_adhocctl -lpspwlan -lpsprtc -lm

# ADR-0080: the Media Engine kernel module builds as a sub-make and ships
# beside the EBOOT (loaded at runtime via kuKernelLoadModule; nothing links
# against it).  The rule is defined AFTER the build.mak include below, so it
# does not steal the default goal; it is listed here as an EXTRA_TARGET so
# `all` builds it.
EXTRA_TARGETS   = EBOOT.PBP me/gbadhoc_me.prx home/gbadhoc_home.prx $(ONLINE_PRX)
# Generic-build XMB branding (user direction, 2026-08-08): title "GBAdhoc",
# icon + background derived from the user-supplied logo by assets/make_assets.py
# (was "PSP AGB" after the adapter's AGB-015 model number).  The HARNESS build
# overrides these on the command line with assets/harness_* so the instrumented
# console is unmistakable at a glance — inverted to a dark amber field — and
# tools/make_variant.sh does the same per game.
PSP_EBOOT_TITLE ?= GBAdhoc
PSP_EBOOT_ICON  = assets/gbadhoc_icon0.png
PSP_EBOOT_PIC1  = assets/gbadhoc_pic1.png
# ICON1 (the animated icon, assets/gbadhoc_icon1.pmf from
# tools/logo/make_icon1.py) is NOT set here: tools/build.sh passes
# PSP_EBOOT_ICON1 for the release profile only, and build.mak leaves the
# slot empty otherwise.  ICON0/PIC1 come from tools/bake_logo.py --xmb.

# Large-memory policy is SPLIT by build flavour (MEMSIZE in PARAM.SFO):
#  - PLAYABLE requests the 64 MiB layout (MEMSIZE=1).  A 1000 has no extra
#    RAM and ignores the flag.
#
#    32 MB carts (Mother 3, Unbound) are held WHOLE on a 64 MiB console:
#    CONFIG.INI `rom_resident` (default 1) makes the core probe 32 + 3 MiB
#    after the translation-cache tier has taken its block (main_psp.c
#    rom_cache_select, gba_memory.c init_gamepak_buffer).  A PSP-1000 fails
#    the probe and pages with its 13-15 block cache as before; the paged
#    default cap stays ROM_BUFFER_SIZE=15 (root Makefile).  The 2026-09-16
#    PSP-3000 Unbound divergence at 32 blocks was re-tested on the Go A/B
#    rig: 21/21 frame-identical (docs/ROM-RESIDENCY.md).
#  - HARNESS pins MEMSIZE=0 (the `harness` target below): PSP-1000 is the
#    memory budget target (FRONTEND-AUDIT §8) and PPSSPP models MEMSIZE, so
#    the Gate-1 mem_free measurement must stay honest for the 1000-series.
PSP_LARGE_MEMORY ?= 1

# LAYOUT_PAD_TEXT / LAYOUT_PAD_BSS = N bytes: hardware layout sweeps only
# (docs/JIT-CODE-DISCIPLINE.md).  layout_pad.o is linked first, so every
# other object's code and bss move by N.  Unset (every normal build): the
# object is not linked at all.  tools/build.sh passes it via PSP_EXTRA,
# which it refuses for the release profile.
ifneq ($(LAYOUT_PAD_TEXT)$(LAYOUT_PAD_BSS),)
OBJS := layout_pad.o $(OBJS)
endif

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
# NOT a prerequisite: layout/hot.ord.  build.mak links `$^`, so ld would read
# the ordering file as an input linker script.  tools/build.sh always relinks
# (clean build); by hand, `rm gpsp_adhoc.elf` after regenerating it.

# netdrv objects are compiled INTO psp/ (not in-tree next to their sources):
# a MIPS .o left in ../netdrv would be swept into the desktop core archive
# by sdl/Makefile's find (ADR-0005 stale-arch guard).
netdrv_core.o: ../netdrv/netdrv.c ../netdrv/netdrv.h ../netdrv/netdrv_wire.h
	$(CC) $(CFLAGS) -c -o $@ $<

transport_adhoc.o: ../netdrv/transport_adhoc.c ../netdrv/transport_adhoc.h ../netdrv/netdrv.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Mystery Gift's netdrv backend (2.0.5).  Same out-of-tree rule and the same
# reason as transport_adhoc above: a MIPS .o left in ../netdrv would be swept
# into the desktop core archive by sdl/Makefile's find (ADR-0005).

transport_inet.o: ../netdrv/transport_inet.c ../netdrv/transport_inet.h ../netdrv/netdrv.h
	$(CC) $(CFLAGS) -c -o $@ $<

# GB flags per object (variables defined beside OBJS above).  Changing them
# does not touch any header, so -MMD cannot see it: clean-build after edits.
# The appended TGB_OPT overrides the global -O2 for these objects only.
$(TGB_OBJS) ../gbcore/tgbdual/tgb_shared.o: CFLAGS += $(TGB_CFLAGS)
# Share game data (docs/SAVE-SHARE.md) runs from the ROM browser only and
# none of it is speed-critical; what it costs every boot is its code size,
# which comes out of the PSP-1000's heap.  -Os for its objects (the screens
# in ui_share.inc carry the same switch as a pragma).
../frontend-common/fe_share.o share_psp.o: CFLAGS += -Os
# The menus, the pad map, the config file, Mystery Gift and the cheat parser
# are on no hot path either (none is in layout/hot.ord).
ui_psp.o sidecar_dirs.o ctl_map.o config_psp.o mgift_cart.o mgift_net.o mgift_payload.o ../frontend-common/fe_cheats.o: CFLAGS += -Os
# The GB core keeps one .text per object under LAYOUT_PIN too: it is partial-
# linked twice (gbcore_instance.ld) and none of it is on the GBA hot path.
$(GB_CORE_OBJS) ../gbcore/tgbdual/tgb_shared.o: CFLAGS += -fno-function-sections

../gbcore/gbcore_tgbdual_linked.o: $(GB_CORE_OBJS) ../gbcore/gbcore_instance.ld
	psp-ld -r -T ../gbcore/gbcore_instance.ld -o $@.tmp $(GB_CORE_OBJS)
	psp-objcopy -w --keep-global-symbol='gbcore_*' $@.tmp $@
	rm -f $@.tmp

# Instance B: rename exactly the symbols instance A defines (all gbcore_*;
# the recipe fails if anything else is global).  Undefined references --
# libc -- are not in the list and stay as they are.
../gbcore/gbcore_tgbdual_linked_b.o: ../gbcore/gbcore_tgbdual_linked.o
	psp-nm -g --defined-only $< | awk '$$3 !~ /^gbcore_/ { bad = 1 } \
	  { print $$3 " gbcoreb_" substr($$3, 8) } END { exit bad }' > $@.syms
	psp-objcopy --redefine-syms=$@.syms $< $@
	rm -f $@.syms

gbalink_core_b.o: $(CORE_LIB) gbalink_core_b.ld
	psp-ld -r -d -T gbalink_core_b.ld -o $@.tmp --whole-archive $(CORE_LIB)
	psp-nm -g --defined-only $@.tmp | awk 'NF == 3 { print $$3 " gbab_" $$3 }' > $@.syms
	psp-objcopy --redefine-syms=$@.syms $@.tmp $@
	rm -f $@.tmp $@.syms

# The partner machine's autopilot (bench only): fe_autopilot.o again, its
# API renamed gbab_fe_autopilot_* and its three host hooks pointed at the
# partner's (fe_gbadual.c), so RAM-gated scripts drive both machines.
fe_autopilot_b.o: ../frontend-common/fe_autopilot.o
	psp-nm -g --defined-only $< | awk 'NF == 3 { print $$3 " gbab_" $$3 }' > $@.syms
	printf '%s\n' 'fe_host_input_inject gbab_fe_host_input_inject' \
	  'fe_host_mem_read gbab_fe_host_mem_read' \
	  'fe_host_sram_crc_now gbab_fe_host_sram_crc_now' >> $@.syms
	psp-objcopy --redefine-syms=$@.syms $< $@
	rm -f $@.syms

$(CORE_LIB):
	$(error build the core first: make platform=psp1 in the repo root (pspdev docker))

layout_pad.o: layout_pad.S
	$(CC) -c -o $@ $< -DLAYOUT_PAD_TEXT=$(or $(LAYOUT_PAD_TEXT),0) \
	      -DLAYOUT_PAD_BSS=$(or $(LAYOUT_PAD_BSS),0)

# ADR-0035: feed the -MMD dependency files back in, so touching any header
# rebuilds every object that includes it.  Must come AFTER build.mak (which
# defines the compile rules) and after OBJS is complete.  The leading `-`
# makes a first, .d-less build silent instead of fatal.
DEPS := $(OBJS:.o=.d) $(GB_CORE_OBJS:.o=.d)
-include $(DEPS)

# `clean` is build.mak's; the .d files are ours, so sweep them too or a
# stale one outlives the object it describes.  The GB core's objects are
# not in OBJS (only their linked product is), so they are ours as well.
clean: clean-deps clean-me clean-home clean-online clean-gbcore
clean-deps:
	-@rm -f $(DEPS)
clean-gbcore:
	-@rm -f $(GB_CORE_OBJS) ../gbcore/gbcore_tgbdual_linked_b.o
	-@rm -f gbalink_core_b.o fe_autopilot_b.o ../frontend-common/fe_gbadual.o ../gbalink/*.o
.PHONY: clean-deps clean-gbcore

# ADR-0080: the ME kernel module (defined here, after the include, so it
# never becomes the default goal — that bug once built the PRX and nothing
# else).  PHONY so its own sub-make owns dependency tracking.
.PHONY: me/gbadhoc_me.prx clean-me
me/gbadhoc_me.prx:
	# PSP_PIXFMT must reach the PRX too.  It was a THIRD, independent copy of
	# the pixel-format decision -- me/Makefile hardcoded -DUSE_PSP_RGB565_FORMAT
	# -- so a `PSP_PIXFMT=rgb565` build gave a core and a frontend using one
	# layout and an ME renderer using the other, with R and B swapped in ME mode
	# only, silently.  The core/frontend pair is caught at link time
	# (gba_memory.c's pixfmt guard); the PRX is a separately loaded module and
	# cannot be, so it has to be told.
	$(MAKE) -C me PSP_PIXFMT=$(PSP_PIXFMT) ME_CATCH=$(ME_CATCH) \
	        ME_CATCH_NOREG=$(ME_CATCH_NOREG) ME_PAD_RODATA=$(ME_PAD_RODATA) \
	        ME_PAD_BSS=$(ME_PAD_BSS)
clean-me:
	-$(MAKE) -C me clean

# What fe_features.h resolved to on THIS make line (tools/build.sh records it in
# the manifest's frontendDefines, as the root make's print-features does for
# coreFlags).  -D only: the rest of CFLAGS would write a .d for /dev/null.
FE_FEATURE_NAMES := $(filter-out FE_FEATURES_H,$(shell sed -n 's/^\#define \([A-Z0-9_]*\) .*/\1/p' fe_features.h))
.PHONY: print-features
print-features:
	@$(CC) $(filter -D%,$(CFLAGS)) -include fe_features.h -dM -E -x c /dev/null | \
	  sed -n 's/^#define \([A-Z0-9_]*\) \(.*\)$$/\1=\2/p' | \
	  grep $(foreach f,$(FE_FEATURE_NAMES),-e '^$(f)=')

# Menu = HOME (docs/CONTROL-REMAP.md section 10): the small kernel module that
# reads HOME and switches the system popup off while a game owns it.  Its own
# sub-make and its own PRX, so the Media Engine module is untouched.
.PHONY: online/gbadhoc_online.prx clean-online
online/gbadhoc_online.prx:
	$(MAKE) -C online
clean-online:
	-$(MAKE) -C online clean
.PHONY: home/gbadhoc_home.prx clean-home
home/gbadhoc_home.prx:
	$(MAKE) -C home
clean-home:
	-$(MAKE) -C home clean
