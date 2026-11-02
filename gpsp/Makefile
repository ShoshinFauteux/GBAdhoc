DEBUG=0
OVERCLOCK_60FPS ?= 0
FRONTEND_SUPPORTS_RGB565=1
FORCE_32BIT_ARCH=0
MMAP_JIT_CACHE=0

UNAME=$(shell uname -a)

# platform
ifeq ($(platform),)
platform = unix
ifeq ($(shell uname -s),)
   platform = win
else ifneq ($(findstring MINGW,$(shell uname -s)),)
   platform = win
else ifneq ($(findstring Darwin,$(shell uname -s)),)
   platform = osx
else ifneq ($(findstring win,$(shell uname -s)),)
   platform = win
endif
endif

ifeq ($(platform), unix)
ifeq ($(firstword $(filter x86_64,$(UNAME))),x86_64)
	HAVE_DYNAREC := 1
	CPU_ARCH := x86_32
else ifeq ($(firstword $(filter amd64,$(UNAME))),amd64)
	HAVE_DYNAREC := 1
	CPU_ARCH := x86_32
else ifeq ($(firstword $(filter x86,$(UNAME))),x86)
	FORCE_32BIT_ARCH = 1
	HAVE_DYNAREC := 1
	CPU_ARCH := x86_32
endif
endif

FORCE_32BIT :=

ifeq ($(FORCE_32BIT_ARCH),1)
	FORCE_32BIT := -m32
endif

# system platform
system_platform = unix
ifeq ($(shell uname -a),)
	EXE_EXT = .exe
	system_platform = win
else ifneq ($(findstring Darwin,$(shell uname -a)),)
	system_platform = osx
	arch = intel
	ifeq ($(shell uname -p),powerpc)
		arch = ppc
	endif
	ifeq ($(shell uname -p),arm)
		arch = arm
	endif
else ifneq ($(findstring MINGW,$(shell uname -a)),)
	system_platform = win
endif

ifneq (,$(findstring webos,$(CROSS_COMPILE)))
	ifneq (,$(findstring aarch64,$(CROSS_COMPILE)))
		CPU_ARCH := arm64
		HAVE_DYNAREC := 1
		MMAP_JIT_CACHE = 1
	else
		CPU_ARCH := arm
	endif
endif

TARGET_NAME	:= gpsp
GIT_VERSION := "$(shell git rev-parse --short HEAD || echo unknown)"
ifneq ($(GIT_VERSION),"unknown")
	CFLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
endif
LIBM		   := -lm
CORE_DIR    := .
LDFLAGS     :=

# Unix
ifeq ($(platform), unix)
	TARGET := $(TARGET_NAME)_libretro.so
	fpic := -fPIC
	SHARED := -shared $(FORCE_32BIT) -Wl,--version-script=link.T
	ifneq ($(findstring Haiku,$(shell uname -a)),)
		LIBM :=
	endif
	CFLAGS += $(FORCE_32BIT)
	LDFLAGS += -Wl,--no-undefined
	ifeq ($(HAVE_DYNAREC),1)
		MMAP_JIT_CACHE = 1
	endif

# Linux portable
else ifeq ($(platform), linux-portable)
	TARGET := $(TARGET_NAME)_libretro.so
	fpic := -fPIC -nostdlib
	SHARED := -shared $(FORCE_32BIT) -Wl,--version-script=link.T
	LIBM :=
	CFLAGS += $(FORCE_32BIT)
	ifeq ($(HAVE_DYNAREC),1)
		MMAP_JIT_CACHE = 1
	endif

# OS X
else ifeq ($(platform), osx)
	TARGET := $(TARGET_NAME)_libretro.dylib
	fpic := -fPIC
	ifeq ($(arch),ppc)
		CFLAGS += -DMSB_FIRST -D__ppc__
	endif
	OSXVER = `sw_vers -productVersion | cut -d. -f 2`
	OSX_LT_MAVERICKS = `(( $(OSXVER) <= 9)) && echo "YES"`
	ifeq ($(OSX_LT_MAVERICKS),YES)
		fpic += -mmacosx-version-min=10.1
	endif
	SHARED := -dynamiclib
	ifeq ($(HAVE_DYNAREC),1)
		MMAP_JIT_CACHE = 1
	endif

	ifeq ($(CROSS_COMPILE),1)
		TARGET_RULE   = -target $(LIBRETRO_APPLE_PLATFORM) -isysroot $(LIBRETRO_APPLE_ISYSROOT)
		CFLAGS   += $(TARGET_RULE)
		LDFLAGS  += $(TARGET_RULE)
	endif

	CFLAGS  += $(ARCHFLAGS)
	LDFLAGS += $(ARCHFLAGS)

# iOS
else ifneq (,$(findstring ios,$(platform)))

	TARGET := $(TARGET_NAME)_libretro_ios.dylib
	fpic := -fPIC
	SHARED := -dynamiclib
	CPU_ARCH := arm

	ifeq ($(IOSSDK),)
		IOSSDK := $(shell xcodebuild -version -sdk iphoneos Path)
	endif

	ifeq ($(platform),ios-arm64)
		CC = cc -arch arm64 -isysroot $(IOSSDK)
		CXX = c++ -arch arm64 -isysroot $(IOSSDK)
	else
		CC = cc -marm -arch armv7 -isysroot $(IOSSDK)
		CXX = c++ -marm -arch armv7 -isysroot $(IOSSDK)
	endif
	CFLAGS += -DIOS -DHAVE_POSIX_MEMALIGN

	ifeq ($(platform),$(filter $(platform),ios9 ios-arm64))
		MINVERSION = -miphoneos-version-min=8.0
	else
		MINVERSION = -miphoneos-version-min=5.0
	endif
	SHARED += $(MINVERSION)
	CC += $(MINVERSION)
	CXX += $(MINVERSION)

# tvOS
else ifeq ($(platform), tvos-arm64)
	TARGET := $(TARGET_NAME)_libretro_tvos.dylib
	fpic := -fPIC
	SHARED := -dynamiclib
	CPU_ARCH := arm
	CFLAGS += -DIOS -DHAVE_POSIX_MEMALIGN

	ifeq ($(IOSSDK),)
		IOSSDK := $(shell xcodebuild -version -sdk appletvos Path)
	endif
   CC = clang -arch arm64 -isysroot $(IOSSDK)
   CXX = clang++ -arch arm64 -isysroot $(IOSSDK)
   MINVERSION = -mappletvos-version-min=11.0
   SHARED += $(MINVERSION)
   CC += $(MINVERSION)
   CXX += $(MINVERSION)

# iOS Theos
else ifeq ($(platform), theos_ios)
	DEPLOYMENT_IOSVERSION = 5.0
	TARGET = iphone:latest:$(DEPLOYMENT_IOSVERSION)
	ARCHS = armv7 armv7s
	TARGET_IPHONEOS_DEPLOYMENT_VERSION=$(DEPLOYMENT_IOSVERSION)
	THEOS_BUILD_DIR := objs
	include $(THEOS)/makefiles/common.mk

	CFLAGS += -DIOS -DHAVE_POSIX_MEMALIGN -marm
	CPU_ARCH := arm
	LIBRARY_NAME = $(TARGET_NAME)_libretro_ios

# QNX
else ifeq ($(platform), qnx)
	TARGET := $(TARGET_NAME)_libretro_qnx.so
	fpic := -fPIC
	SHARED := -shared -Wl,--version-script=link.T
	MMAP_JIT_CACHE = 1
	CPU_ARCH := arm

	CC = qcc -Vgcc_ntoarmv7le
	CXX = qcc -Vgcc_ntoarmv7le
	AR = qcc -Vgcc_ntoarmv7le
	CFLAGS += -D__BLACKBERRY_QNX_
	HAVE_DYNAREC := 1

# Lightweight PS3 Homebrew SDK
else ifeq ($(platform), psl1ght)
	TARGET := $(TARGET_NAME)_libretro_$(platform).a
	CC = $(PS3DEV)/ppu/bin/ppu-gcc$(EXE_EXT)
	CXX = $(PS3DEV)/ppu/bin/ppu-g++$(EXE_EXT)
	AR = $(PS3DEV)/ppu/bin/ppu-ar$(EXE_EXT)
	CFLAGS += -DMSB_FIRST -D__ppc__
	STATIC_LINKING = 1
	
# Nintendo Switch (libnx)
else ifeq ($(platform), libnx)
	include $(DEVKITPRO)/libnx/switch_rules
	TARGET := $(TARGET_NAME)_libretro_$(platform).a
	CFLAGS += -O3 -fomit-frame-pointer -ffast-math -I$(DEVKITPRO)/libnx/include/ -fPIE
	CFLAGS += -specs=$(DEVKITPRO)/libnx/switch.specs
	CFLAGS += -D__SWITCH__ -DHAVE_LIBNX
	CFLAGS += -DARM -D__aarch64__=1 -march=armv8-a -mtune=cortex-a57 -mtp=soft -ffast-math
	STATIC_LINKING=1
	STATIC_LINKING_LINK = 1

# Nintendo Game Cube / Wii / WiiU
else ifneq (,$(filter $(platform), ngc wii wiiu))
	TARGET := $(TARGET_NAME)_libretro_$(platform).a
	CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc$(EXE_EXT)
	CXX = $(DEVKITPPC)/bin/powerpc-eabi-g++$(EXE_EXT)
	AR = $(DEVKITPPC)/bin/powerpc-eabi-ar$(EXE_EXT)
	CFLAGS += -DGEKKO -mcpu=750 -meabi -mhard-float -DHAVE_STRTOF_L
        CFLAGS += -ffunction-sections -fdata-sections -D__wiiu__ -D__wut__
	STATIC_LINKING = 1

# PSP
else ifeq ($(platform), psp1)
	TARGET := $(TARGET_NAME)_libretro_$(platform).a
	CC = psp-gcc$(EXE_EXT)
	CXX = psp-g++$(EXE_EXT)
	AR = psp-ar$(EXE_EXT)
	CFLAGS += -DPSP -G0 -DMIPS_HAS_R2_INSTS
	# DEFAULT ROM page-cache cap: 15 one-megabyte blocks.  This is the PAGED
	# cap, and also what the JIT tier's headroom is computed from
	# (JIT_LARGE_HEADROOM = ROM_BUFFER_SIZE + 1 + 4 MiB) -- never set it to 32.
	# Full residency is the RUNTIME decision instead: CONFIG.INI rom_resident
	# (default on) makes init_gamepak_buffer probe 32 + 3 MiB on the heap the JIT
	# tier left, so a 64 MiB console holds a 32 MiB cart whole and a PSP-1000
	# (heap ~22 MiB) fails the probe and pages exactly as before.  The
	# 2026-09-16 Unbound divergence at 32 blocks was re-tested by the Go A/B
	# rig (21/21 frame-identical to the paged golden); docs/ROM-RESIDENCY.md.
	ROM_BUFFER_SIZE ?= 15
	CFLAGS += -DROM_BUFFER_SIZE=$(ROM_BUFFER_SIZE)
	# JIT cache sizing is decided AT STARTUP, not here (RUNTIME_JIT_CACHE,
	# cpu_threaded.c dynarec_select_translation_caches).  One EBOOT for every
	# model: the SMALL tier (2MB ROM / 384KB RAM code, static) fits the
	# PSP-1000 budget and is what a 1000 always gets; a console whose heap
	# can afford it (a 2000/3000/Go with the 64 MiB layout) allocates the
	# LARGE tier (10MB / 384KB) instead, which is what CFRU-class ROM hacks
	# (Unbound) need to stop flush-storming.
	#
	# BIG_JIT=1 compiled the large tier in for every model.  It is gone
	# because a PSP-1000 cannot afford it and one EBOOT now serves both; a
	# harness can still force the small tier with `jit_small = 1`.  Refused
	# loudly rather than ignored: a silently dropped core flag has already
	# cost two hardware experiments (core-vs-frontend-defines).
	ifdef BIG_JIT
	$(error BIG_JIT is obsolete: translation caches are sized at startup (RUNTIME_JIT_CACHE). Drop BIG_JIT)
	endif
	CFLAGS += -DSMALL_TRANSLATION_CACHE -DRUNTIME_JIT_CACHE
	# LAYOUT_PIN=1 (OPT-IN; docs/LAYOUT-PINNING.md): every function in its own
	# section, so psp/Makefile's link can put the hot path first in .text in a
	# fixed order (psp/layout/hot.ord), and -DLAYOUT_PIN pins the translation
	# cache's I-cache sets (gpsp_config.h JIT_PIN_*).  Code is unchanged; only
	# addresses move.  OFF by default until a hardware bench validates it (owner
	# guardrail); LAYOUT_PIN=0 is the pre-pinning layout, byte for byte.  Pass
	# the SAME value to psp/Makefile (tools/build.sh does).
	LAYOUT_PIN ?= 0
	ifeq ($(LAYOUT_PIN),1)
	CFLAGS += -ffunction-sections -DLAYOUT_PIN=1
	endif
	# SMC_SKIP_SAME=1: idempotent-store filter in the JIT store stubs
	# (mips/mips_emit.h) — skip the full RAM-cache wipe when a store to
	# tagged memory writes the value already there.
	#
	# NOTE FOR FUTURE ME: these knobs only take effect HERE, in the core
	# archive.  Passing them to psp/Makefile does nothing — mips_emit.h is
	# compiled into gpsp_libretro_psp1.a, not into the frontend — and make
	# does not track define changes, so the core must be CLEAN-rebuilt when
	# they change (ADR-0035, the franken-build trap, third occurrence).
	ifeq ($(SMC_SKIP_SAME),1)
	CFLAGS += -DSMC_SKIP_SAME
	endif
	# SMC_PARTIAL=1: retire only the block covering the written address
	# instead of wiping the whole RAM translation cache (cpu_threaded.c
	# flush_translation_cache_ram_block).  EXPERIMENTAL — see the safety note
	# there about direct branch patching; validate with the harness frame-dump
	# oracle before this goes anywhere near a shipping build.
	ifeq ($(SMC_PARTIAL),1)
	CFLAGS += -DSMC_PARTIAL
	endif
	# SMC_PARTIAL_SAFE=1: experimental selective invalidation with exact,
	# per-ISA block extents and indirect cross-block branches into RAM. ARM
	# block stores are eligible only when they do not write back their base;
	# advancing copy loops retain the proven full-cache flush.
	ifeq ($(SMC_PARTIAL_SAFE),1)
	CFLAGS += -DSMC_PARTIAL_SAFE
	endif
	ifeq ($(SMC_PARTIAL_SAFE_CONTROL),1)
	CFLAGS += -DSMC_PARTIAL_SAFE_CONTROL
	endif
	ifeq ($(SMC_PARTIAL_SAFE_NO_TRAMP),1)
	CFLAGS += -DSMC_PARTIAL_SAFE_NO_TRAMP
	endif
	ifeq ($(SMC_PARTIAL_SAFE_FRAMEFULL),1)
	CFLAGS += -DSMC_PARTIAL_SAFE_FRAMEFULL
	endif
	# Keep a permanent per-block RAM entry thunk while selective invalidation is
	# active.  Retiring a block redirects the thunk to the dispatcher; a later
	# translation points the same thunk at the new body, so inbound RAM links do
	# not pay the dispatcher forever.
	ifeq ($(SMC_PARTIAL_STABLE_THUNK),1)
	CFLAGS += -DSMC_PARTIAL_STABLE_THUNK
	endif
	ifeq ($(SMC_PARTIAL_DIRECT_LINKS),1)
	CFLAGS += -DSMC_PARTIAL_DIRECT_LINKS
	endif
	ifeq ($(SMC_PARTIAL_DIRECT_GATE),1)
	CFLAGS += -DSMC_PARTIAL_DIRECT_GATE
	endif
	# SMC_GATES=1: add a translation gate at each self-modified address so
	# future blocks split there.  Independent of SMC_PARTIAL on purpose —
	# gates are what bought Unbound its speed AND what broke its audio, so
	# the two must be testable separately.
	ifeq ($(SMC_GATES),1)
	CFLAGS += -DSMC_GATES
	endif
	# SMC_GATES_SIMPLE=1: use the 2.0.1 add-only gate rule instead of 2.0.2's
	# cluster+evict.  Add-only never moves an existing gate, so blocks already
	# translated can never disagree with the table.  Only meaningful together
	# with SMC_GATES=1.
	ifeq ($(SMC_GATES_SIMPLE),1)
	CFLAGS += -DSMC_GATES_SIMPLE
	endif
	# SMC_GATES_CLUSTER=1: add-only AND cluster grouping, no eviction.  Keeps
	# the half of 2.0.2's rule that cut dispatcher lookups and drops the half
	# that moved gates and crashed Heart & Soul.  Overrides SMC_GATES_SIMPLE.
	ifeq ($(SMC_GATES_CLUSTER),1)
	CFLAGS += -DSMC_GATES_CLUSTER
	endif
	# CLUSTER_BYTES=N: gate-clustering window (default 128, cpu_threaded.c).
	# Must stay BELOW the spacing between a game's distinct patched regions or
	# they merge into one gate; H&S's two mixer copies are 152 bytes apart.
	ifneq ($(CLUSTER_BYTES),)
	CFLAGS += -DSMC_GATE_CLUSTER=$(CLUSTER_BYTES)
	endif
	# GATE_SLOTS=N: size of the translation gate table (default 8, cpu.h).
	# More slots cover more self-modifying regions, but scan_block compares
	# block_end_pc against every live gate on EVERY instruction it translates,
	# so the scan cost is O(slots) per instruction compiled.  Measure j% (needs
	# core_phase=2), not xlat -- xlat counts blocks, and more gates make blocks
	# smaller, so it rises even when the work falls.
	ifneq ($(GATE_SLOTS),)
	CFLAGS += -DMAX_TRANSLATION_GATES=$(GATE_SLOTS)
	endif
	# SMC_GATES_SNAPF=1: snap the gate FORWARD to the first proven instruction
	# boundary at/after the write.  A gate must be both a valid code entry AND
	# at/after the write (so the previous block ends before the SMC region).
	# SNAP (backwards) kept validity but lost isolation: 1.43M flushes, 31-48
	# fps vs 116k and 59.9 for add-only.
	ifeq ($(SMC_GATES_SNAPF),1)
	CFLAGS += -DSMC_GATES_SNAPF=1
	endif
	# SMC_GATES_RANKED=1: promote an address to a gate only once it has earned
	# one.  SHIPPED.
	#
	# The rule is NOT frequency, and this comment used to say it was -- it
	# described an SMC_GATE_PROMOTE_HITS threshold (default 256) that the code
	# does not have.  Counting was tried and failed twice: an absolute
	# threshold is outgrown by any cold address, and a ratio to the hottest
	# candidate degenerates because a promoted gate stops counting and the
	# maximum freezes.  Both crashed.
	#
	# What the code actually requires, all three (cpu_threaded.c,
	# smc_gate_earned):
	#   SMC_GATE_MIN_SAMPLE   64 writes of evidence, and
	#   chg * 2 >= hits       at least half of them CHANGED the word.
	# TWO conditions, not three.  A staleness rule (SMC_CAND_STALE_FRAMES)
	# was proposed and WITHDRAWN; it is not in cpu_threaded.c.
	# Measured on H&S: 0300168c (real code being patched) is 62953 changed vs
	# 120 same; 03001404 (a constant re-stored into a code block, and the
	# address that crashed us) is 1 changed vs 66 same.  That gap is
	# structural, not tuned.  Add-only: once promoted, never moved.
	ifeq ($(SMC_GATES_RANKED),1)
	CFLAGS += -DSMC_GATES_RANKED=1
	endif
	# SMC_GATE_BITMAP=1: preserve the exact gate table and block boundaries,
	# but test the current halfword through a bitmap instead of comparing it
	# with every live gate while scan_block compiles each instruction. Only
	# meaningful together with SMC_GATES=1.
	ifeq ($(SMC_GATE_BITMAP),1)
	CFLAGS += -DSMC_GATE_BITMAP=1
	endif
	# ENTRY_GATES=1: when the RAM translation cache is EXHAUSTED, every RAM
	# block start becomes a gate, so a routine entered at many points (Driver
	# 2's unrolled rasterisers: ~45 entries each, every entry a block holding
	# the rest of the routine, ~380 KB of code for ~4 KB of guest) is
	# translated once instead of once per entry.  Armed only by an exhaustion
	# flush: a game that never fills the cache never gets one.  Needs
	# SMC_GATES=1 and SMC_GATE_BITMAP=1 (cpu_threaded.c ENTRY_GATES).
	ifeq ($(ENTRY_GATES),1)
	ifneq ($(SMC_GATE_BITMAP),1)
	$(error ENTRY_GATES=1 needs SMC_GATES=1 SMC_GATE_BITMAP=1)
	endif
	CFLAGS += -DENTRY_GATES=1
	endif
	# XLAT_FAST=1: translator host-work speedups that leave everything the
	# translator produces (emitted code, tags, counters) unchanged
	# (cpu_threaded.c XLAT_FAST; claude/bench-xlat).
	ifeq ($(XLAT_FAST),1)
	CFLAGS += -DXLAT_FAST=1
	endif
	# XLAT_NO=PIECE[:PIECE...]: switch single XLAT_FAST pieces off for an A/B
	# (LIMITS GATEMAP EXITPF RAMEND DCLEAR SCANFN), e.g. XLAT_NO=SCANFN:DCLEAR.
	ifneq ($(XLAT_NO),)
	CFLAGS += $(foreach p,$(subst :, ,$(XLAT_NO)),-DXLAT_NO_$(p)=1)
	endif
	# JIT_COST=1: cost of producing translated code (cpu_threaded.c JIT_COST;
	# claude/jit-cost, docs/JIT-COST.md).  Exact: same emitted code, same tags,
	# same guest state.  Pieces: PUBINL (publish with user-mode cache line ops
	# instead of two kernel calls; runtime jit_pub_inline), SCANWIN (RAM ARM
	# scan: gate bitmaps tested only in 64-byte windows that hold a gate),
	# SCANCOND (RAM ARM scan: flag-write bit from a table built from the
	# original switch).  The scan pieces need XLAT_FAST's SCANFN and GATEMAP
	# and are chosen at run time too (xlat_scan_fast), so one binary A/Bs them.
	ifeq ($(JIT_COST),1)
	CFLAGS += -DJIT_COST=1
	endif
	# JIT_COST_NO=PIECE[:PIECE...]: compile single JIT_COST pieces out.
	ifneq ($(JIT_COST_NO),)
	CFLAGS += $(foreach p,$(subst :, ,$(JIT_COST_NO)),-DJIT_COST_NO_$(p)=1)
	endif
	# XLAT_SPLIT_DIAG=1: diagnostic only -- scan/emit time split (cpu_threaded.c).
	ifeq ($(XLAT_SPLIT_DIAG),1)
	CFLAGS += -DXLAT_SPLIT_DIAG=1
	endif
	# VRAM_EXEC=1: translate and run code the game copied into VRAM
	# (cpu_threaded.c VRAM_EXEC; Big Mutha Truckers, Street Racing Syndicate,
	# jsmolka nes.gba).  Off by default.
	# FIQ_BANK_FIX=1: bank FIQ's r8-r12 correctly in set_cpu_mode (cpu.cc;
	# OpenLara).  Off by default.  (From claude/self-activate 950c5f3/25cab1b.)
	ifeq ($(FIQ_BANK_FIX),1)
	CFLAGS += -DFIQ_BANK_FIX=1
	endif
	# MSR_FAST=1 (needs FIQ_BANK_FIX=1): CPSR writes that switch mode and SPSR
	# restores bank the registers in mips/mips_stub.S instead of calling C
	# (OpenLara, IRQ dispatchers); the pending-IRQ case keeps the C path.
	# Exact.  Run-time kill switch `msr_fast` (config.ini / harness key).
	ifeq ($(MSR_FAST),1)
	CFLAGS += -DMSR_FAST=1
	endif
	# MSR_FAST_DEFAULT=0: compiled in, msr_fast off (a layout-identical A/B arm).
	ifneq ($(MSR_FAST_DEFAULT),)
	CFLAGS += -DMSR_FAST_DEFAULT=$(MSR_FAST_DEFAULT)
	endif
	# BX_ALIGN_FIX=1: a BX into ARM state with address bit 1 set goes to the
	# word below, as on the ARM7TDMI (cpu_threaded.c block_lookup_address_dual,
	# mips_stub.S).  gpSP rounded UP and skipped the first ARM instruction of
	# Camelot's "bx pc" IRQ veneer (corpus-classify 2026-10-27).  Same code size.
	ifeq ($(BX_ALIGN_FIX),1)
	CFLAGS += -DBX_ALIGN_FIX=1
	endif
	# BACKUP_BUS_FIX=1: a 16/32-bit dynarec load from SRAM/flash (0x0E) returns
	# the byte in every lane, as the 8-bit bus does and the interpreter already
	# did (mips_emit.h region 14).  gpSP returned the bare byte (FFTA f692
	# LDR from 0x0ec54d2c: 0xff vs 0xffffffff; corpus-classify).  Stub-only.
	ifeq ($(BACKUP_BUS_FIX),1)
	CFLAGS += -DBACKUP_BUS_FIX=1
	endif
	# COND_ENTRY_FIX=1: a branch into the middle of a run of same-condition ARM
	# instructions gets its own condition check (cpu_threaded.c translate_block_arm).
	# gpSP emitted one header per run, so the landing point and everything after it
	# ran unconditionally (Mario Golf f1c2 NEEDS-REVIEW; corpus-classify).
	ifeq ($(COND_ENTRY_FIX),1)
	CFLAGS += -DCOND_ENTRY_FIX=1
	endif
	ifeq ($(VRAM_EXEC),1)
	CFLAGS += -DVRAM_EXEC=1
	endif
	# HOT_DATA_GUARD=1: the stub's hot data block (memory_map_read, reg,
	# palette/OAM, spsr, reg_mode, patcher tables) at a FIXED D-cache set
	# offset, HOT_DATA_OFFSET (default 0x1b00 = lab-cpl2xvf's placement;
	# mips/mips_stub.S).  Off by default.
	ifeq ($(HOT_DATA_GUARD),1)
	CFLAGS += -DHOT_DATA_GUARD=1
	ifneq ($(HOT_DATA_OFFSET),)
	CFLAGS += -DHOT_DATA_OFFSET=$(HOT_DATA_OFFSET)
	endif
	endif
	ifneq ($(GATE_RATIO),)
	CFLAGS += -DSMC_GATE_RATIO=$(GATE_RATIO)
	endif
	# SMC_GATES_SNAP=1: gate the START OF THE BLOCK containing the SMC write,
	# found by walking back over the interior tag run to a VALID_TAG.  A gate
	# is used as a code ENTRY point, but is harvested from a DATA write
	# address; snapping guarantees it is a real instruction boundary without
	# discarding the gate (which is what SMC_GATES_CODED did, at -13 fps).
	# REJECTED: walking BACKWARDS keeps validity and loses isolation -- the
	# block starting at that head still spans the SMC region.  1.43M flushes
	# and 31-48 fps, against 116k and a locked 59.9 for add-only.
	ifeq ($(SMC_GATES_SNAP),1)
	CFLAGS += -DSMC_GATES_SNAP=1
	endif
	# SMC_GATES_CODED=1: refuse to gate an address that is not already the
	# start of a translated block.  Gates come from smc_last_write_addr (a
	# DATA address) but are used as CODE entries, so a gate can force a block
	# to start mid-instruction or on half-written code; that block then scans
	# garbage and emits branch targets into OAM/VRAM/SRAM.
	# REJECTED: it rejects 0300168c, the address behind 97.8% of all flushes,
	# because at the moment of the write that address is INTERIOR to its block
	# (a block only starts there once the gate exists).  59.9 -> 46-58 fps.
	ifeq ($(SMC_GATES_CODED),1)
	CFLAGS += -DSMC_GATES_CODED=1
	endif
	# SMC_WRITE_HISTO=1: DIAGNOSTIC, and part of the `diagnostic` profile.
	# Bucket the address written and the pc of the storing instruction for
	# every SMC flush, dumped to ms0:/smchisto.txt every 4000 flushes, plus the
	# count of unmappable block exits contained.  Answers "what is actually
	# causing 10k flushes a battle" before any more guessing about stacks or
	# gates.  NEVER IN A RELEASE, same reason as BADJUMP_REPORT.
	ifeq ($(SMC_WRITE_HISTO),1)
	CFLAGS += -DSMC_WRITE_HISTO=1
	endif
	# SMC_PARTIAL_SINGLE=1: with SMC_PARTIAL_SAFE, also admit single-word ARM
	# stores from IWRAM code into IWRAM to the selective-retirement path
	# (cpu_threaded.c smc_writer_safe_range).  Bench campaign 2026-10-20.
	ifeq ($(SMC_PARTIAL_SINGLE),1)
	CFLAGS += -DSMC_PARTIAL_SINGLE
	endif
	# TIMER_RELOAD_FIX=1: a timer that has just overflowed reads its reload
	# value instead of 0x0000 (main.c update_timers).  Need for Speed
	# Underground computed a 4 GB mixer fill from the 0 and wiped IWRAM
	# (dr2-nfsu 2026-10-21).  Guest-visible, so off by default.
	ifeq ($(TIMER_RELOAD_FIX),1)
	CFLAGS += -DTIMER_RELOAD_FIX
	endif
	# SMC_EVTRACE / PC_RING: DIAGNOSTIC, twin only (tools/drprof VDEFS), in no
	# profile.  SMC_EVTRACE logs every SMC event (writer, word, retirements,
	# fallback reason) to $SMC_EVT_FILE between frame_counter $SMC_EVT_LO..HI;
	# PC_RING keeps the interpreter's last 256k instructions and dumps them
	# when $PC_RING_PC is entered with r1 negative.
	# SMC_FALLBACK_STATS=1: DIAGNOSTIC.  Count why SMC traps fall back to a
	# full RAM flush (EVT core_health smcfb=).
	# smc-coverage 2026-10-20: widen SMC_PARTIAL_SINGLE, one class per switch
	# (all need SMC_PARTIAL_SINGLE=1; cpu_threaded.c smc_writer_safe_range).
	#   SMC_SINGLE_THUMB=1  Thumb STR/STRH/STRB single stores
	#   SMC_SINGLE_HALF=1   ARM STRH
	#   SMC_SINGLE_ANYPC=1  writers outside IWRAM (ROM/EWRAM/BIOS)
	#   SMC_SINGLE_EWRAM=1  EWRAM targets (smc_retire_span)
	#   SMC_SINGLE_HOT=K    widened classes engage only after K of their traps
	#                       (or once selective retirement is already active)
	ifneq ($(SMC_SINGLE_HOT),)
	CFLAGS += -DSMC_SINGLE_HOT=$(SMC_SINGLE_HOT)
	endif
	#   SMC_SINGLE_UNTAG=1  also lift dead block-start tags in the written range
	ifeq ($(SMC_SINGLE_UNTAG),1)
	CFLAGS += -DSMC_SINGLE_UNTAG
	endif
	ifeq ($(SMC_SINGLE_THUMB),1)
	CFLAGS += -DSMC_SINGLE_THUMB
	endif
	ifeq ($(SMC_SINGLE_HALF),1)
	CFLAGS += -DSMC_SINGLE_HALF
	endif
	ifeq ($(SMC_SINGLE_ANYPC),1)
	CFLAGS += -DSMC_SINGLE_ANYPC
	endif
	ifeq ($(SMC_SINGLE_EWRAM),1)
	CFLAGS += -DSMC_SINGLE_EWRAM
	endif
	# SMC_BLOCK_EXACT=1: tag-check every non-final word of a non-SP block
	# store (mips_emit.h aligned store stub) and retire what it hits; block
	# stores' final-word traps then take the exact single-word path.  Needs
	# SMC_PARTIAL_SINGLE=1 (and SMC_SINGLE_THUMB=1 for Thumb STMIA).
	ifeq ($(SMC_BLOCK_EXACT),1)
	CFLAGS += -DSMC_BLOCK_EXACT
	endif
	# SMC_BLOCK_SAME=1 (with SMC_BLOCK_EXACT): a block-store word that hits
	# code but leaves it unchanged retires nothing (exact; no trap involved).
	ifeq ($(SMC_BLOCK_SAME),1)
	CFLAGS += -DSMC_BLOCK_SAME
	endif
	# SMC_COVERAGE_MUTANT_BLOCK=1: ORACLE SELF-TEST ONLY.  The block-word hit
	# handler retires nothing (the 2.0.2 hole reopened).
	ifeq ($(SMC_COVERAGE_MUTANT_BLOCK),1)
	CFLAGS += -DSMC_COVERAGE_MUTANT_BLOCK_IGNORE
	endif
	# SMC_COVERAGE_MUTANT=1: ORACLE SELF-TEST ONLY.  smc_retire_span skips ARM
	# blocks that start inside the written range; the oracle must catch it.
	ifeq ($(SMC_COVERAGE_MUTANT),1)
	CFLAGS += -DSMC_COVERAGE_MUTANT_SKIP_LAST
	endif
	ifeq ($(SMC_FALLBACK_STATS),1)
	CFLAGS += -DSMC_FALLBACK_STATS
	endif
	# STALE_CHECK=1: DIAGNOSTIC (needs SMC_PARTIAL_SAFE and SMC_FALLBACK_STATS).
	# Hash each RAM block's guest bytes at translation and re-check on reuse.
	ifeq ($(STALE_CHECK),1)
	CFLAGS += -DSTALE_CHECK
	endif
	# STALE_CHECK_ALL_ENTRIES=1: DIAGNOSTIC (with STALE_CHECK).  Route every
	# RAM-bound block exit through the dispatcher so the stale check sees every
	# block entry, not only lookups.  Changes timing.
	ifeq ($(STALE_CHECK_ALL_ENTRIES),1)
	CFLAGS += -DSTALE_CHECK_ALL_ENTRIES
	endif
	# SMC_CLASS_PROBE=1: DIAGNOSTIC, in NO profile.  Classify every SMC trap by
	# writer shape (ARM/Thumb, single/STM, writeback) to ms0:/smcclass.txt.
	ifeq ($(SMC_CLASS_PROBE),1)
	CFLAGS += -DSMC_CLASS_PROBE=1
	endif
	# SMC_WRITER_PROBE=1: DIAGNOSTIC, and in NO profile.  Dump the distinct ARM
	# block-store shapes smc_writer_safe_range() is asked about, once, to
	# <gamedir>/log/writer.txt.  Exists to extend that fingerprint from measured
	# opcodes instead of from a comment -- Heart & Soul's mixer is believed to
	# differ from Unbound's only in its +0x48 offset, and believing is not
	# knowing.  NEVER IN A RELEASE, same reason as BADJUMP_REPORT.
	ifeq ($(SMC_WRITER_PROBE),1)
	CFLAGS += -DSMC_WRITER_PROBE=1
	endif
	# SMC_SCAN_SPCLAMP=1: end scan_block at the stack pointer instead of at the
	# top of IWRAM (0x3007FF0).  Stock scans run forward through the live stack
	# and tag it as code, so ordinary PUSH traffic then trips the SMC check and
	# forces a full cache flush (10122 measured in one battle).  PERFORMANCE.
	# NOT IN THE SHIPPED SET: no recorded hardware verdict.  See
	# docs/BUILD-SWITCHES.md.
	ifeq ($(SMC_SCAN_SPCLAMP),1)
	CFLAGS += -DSMC_SCAN_SPCLAMP=1
	endif
	# GBA_PC_MASK=1: mask the emulated pc to 28 bits before the region lookup.
	# The GBA ignores address bits 28-31, so a branch to 0x58581818 executes
	# ROM at 0x08581818 on hardware; gpSP instead falls through to its ~0
	# sentinel, which nothing checks, and the PSP jumps to 0xFFFFFFFF.
	# memory_map_read[] (8K x 32KB) and def_seq_cycles[16] are both sized for
	# 28 bits already, so an unmasked pc also indexes them out of bounds.
	# SHIPPED.  Note it is not sufficient on its own: a masked garbage target
	# lands in a region the translator DOES handle, so the containment at the
	# patch site (see cpu_threaded.c, BLOCK_LOOKUP_UNMAPPABLE) is what stops a
	# garbage exit reaching generated code.
	ifeq ($(GBA_PC_MASK),1)
	CFLAGS += -DGBA_PC_MASK=1
	endif
	# BADJUMP_SAFE=1: never `jr` to an unmappable guest pc.  block_lookup
	# returns ~0 (bad region) or NULL (translate failed); nothing checks
	# either, so mips_indirect_branch_* jumps to it and the PSP dies hard.
	# With this, that becomes a GBA soft-reset: the console survives.
	# Cannot regress -- the path it replaces crashes 100% of the time.
	ifeq ($(BADJUMP_SAFE),1)
	CFLAGS += -DBADJUMP_SAFE=1
	endif
	# SMC_GATE_CHARGE=1: a block that ends at a translation gate charges the
	# cycles of its tail before the (uncharged, unchecked) indirect exit, so a
	# gate no longer lets the code in front of it run for free.  Timing change
	# for gated (SMC) code only; ungated blocks emit identical code.
	ifeq ($(SMC_GATE_CHARGE),1)
	CFLAGS += -DSMC_GATE_CHARGE=1
	endif
	# DISPATCH_CYCLE_CHECK=1: mips_indirect_branch_arm/_thumb run update_gba
	# when the cycle budget is spent, so a guest loop closed only by indirect
	# branches (bx/mov pc/ldr pc/pop {pc}) can no longer spin forever without
	# ever completing a frame.  Two instructions per indirect branch.
	ifeq ($(DISPATCH_CYCLE_CHECK),1)
	CFLAGS += -DDISPATCH_CYCLE_CHECK=1
	endif
	# DMA_SMC_FLUSH=1: an HBlank/VBlank DMA that lands on translated code
	# (CPU_ALERT_SMC from dma_transfer) flushes the RAM translation cache and
	# re-dispatches, as the CPU-triggered DMA path already does.  Upstream
	# dropped that alert in update_gba, leaving the old translation live.
	ifeq ($(DMA_SMC_FLUSH),1)
	CFLAGS += -DDMA_SMC_FLUSH=1
	endif
	# CHEATS (default 1; CHEATS=0 is the kill switch): GBAdhoc cheats
	# (docs/CHEATS.md).  Codes run once per frame at the top of retro_run, work
	# RAM only, with a RAM translation flush when a code changes translated
	# bytes.  Off: cheats.c/main.c/libretro.c are upstream's.  The frontend
	# takes the same switch (psp/Makefile); tools/build.sh passes it to both.
	CHEATS ?= 1
	ifeq ($(CHEATS),1)
	CFLAGS += -DGPSP_CHEATS=1
	endif
	# DRPROF_HW=1: the hardware dynarec-profile bench (drprof.h, psp/ sampler).
	# Adds a phase store around every C call from translated code.  Never in a
	# release (gpsp_profile.h).  The frontend needs -DDRPROF_HW too.
	ifeq ($(DRPROF_HW),1)
	CFLAGS += -DDRPROF_HW=1
	endif
	# SMC_RETIRE_WINDOW=1: PROTOTYPE, OFF by default (docs/DYNAREC-PROFILE.md).
	# Bound the partial-retire tag walk by the longest live block extent
	# instead of MAX_BLOCK_SIZE*4.  Measurement only.
	ifeq ($(SMC_RETIRE_WINDOW),1)
	CFLAGS += -DSMC_RETIRE_WINDOW=1
	endif
	# SMC_RETIRE_WINDOW_CHECK=1: also walk the old 4 KiB window and count any
	# live overlapping block the short window would miss (smc_window_violations,
	# falls back to the full walk).  In the twin a violation aborts the run,
	# so dr_oracle.py base <variant with -DSMC_RETIRE_WINDOW_CHECK> is the test.
	ifeq ($(SMC_RETIRE_WINDOW_CHECK),1)
	CFLAGS += -DSMC_RETIRE_WINDOW_CHECK=1
	endif
	# SMC_RETIRE_LOCAL=1: OFF by default (slow-titles, needs SMC_RETIRE_WINDOW).
	# The retire walks start at a per-64-byte-chunk bound on where a block
	# covering the write can start, instead of one longest-block-seen below
	# it (cpu_threaded.c smc_back_*).  Same retirements, shorter walks.
	# SMC_RETIRE_LOCAL_CHECK=1: diagnostic, also walk the gap and count (twin:
	# abort on) any live overlapping block the bound would miss.
	ifeq ($(SMC_RETIRE_LOCAL),1)
	CFLAGS += -DSMC_RETIRE_LOCAL=1
	endif
	ifeq ($(SMC_RETIRE_LOCAL_CHECK),1)
	CFLAGS += -DSMC_RETIRE_LOCAL_CHECK=1
	endif
	# SMC_RETIRE_LIST=1: OFF by default (slow-titles).  smc_retire_span's
	# validation walk skips interior-mark pairs a word at a time and lists the
	# block-start tags; the retire pass visits only those.  Same retirements.
	ifeq ($(SMC_RETIRE_LIST),1)
	CFLAGS += -DSMC_RETIRE_LIST=1
	endif
	# XS_HEAP=1: OFF by default (cpu-levers cpl2w).  The SMC_SAME_SKIP and
	# SMC_RETIRE_LOCAL tables (up to 69 KiB) come from one heap block taken
	# before the ROM cache instead of .bss, so the hot arrays after them in
	# .bss keep the release layout (D-cache sets).
	ifeq ($(XS_HEAP),1)
	CFLAGS += -DXS_HEAP=1
	endif
	# XS_WALK_PAIR=1: OFF by default (cpu-levers cpl2w).  xs_blocks_current
	# tests two tag slots per aligned load (a pair with every byte 0 or 1
	# holds no block start).  Same blocks, same order.
	ifeq ($(XS_WALK_PAIR),1)
	CFLAGS += -DXS_WALK_PAIR=1
	endif
	# SMC_STM_CHAIN=1: OFF by default (cpu-levers).  A non-final block-store
	# word that lands on tagged IWRAM/EWRAM marks the next word, so the final
	# word's checked store traps (the STM block-store hole: FIFA 07).  The
	# aligned stub keeps its size; its tag test is a tail after all stubs,
	# padded so the stub area grows by exactly 8 KiB.  A word rewritten with
	# its own value starts no chain (value filter).  Needs SMC_STM_RANGE.
	ifeq ($(SMC_STM_CHAIN),1)
	CFLAGS += -DSMC_STM_CHAIN=1
	endif
	ifeq ($(SMC_STM_CHAIN_MUTANT),1)
	CFLAGS += -DSMC_STM_CHAIN_MUTANT=1
	endif
	ifeq ($(SMC_STM_CHAIN_MUTANT_NOPENDING),1)
	CFLAGS += -DSMC_STM_CHAIN_MUTANT_NOPENDING=1
	endif
	ifeq ($(SMC_STM_CHAIN_MUTANT_NOFILTER),1)
	CFLAGS += -DSMC_STM_CHAIN_MUTANT_NOFILTER=1
	endif
	# SMC_SAME_REPROBE=N: SMC_SAME_ADAPT's first re-probe threshold (default
	# 256 single-word retirements while off).
	ifneq ($(SMC_SAME_REPROBE),)
	CFLAGS += -DSMC_SAME_REPROBE=$(SMC_SAME_REPROBE)
	endif
	# ENTRY_GATES_RANGE=1: OFF by default (slow-titles, needs ENTRY_GATES).
	# The exhaustion-time gate harvest walks only the tag range the following
	# full flush clears (iwram/ewram_code_min..max) instead of all 144K tags.
	# ENTRY_GATES_RANGE_CHECK=1: diagnostic, abort on a live tag outside it.
	ifeq ($(ENTRY_GATES_RANGE),1)
	CFLAGS += -DENTRY_GATES_RANGE=1
	endif
	ifeq ($(ENTRY_GATES_RANGE_CHECK),1)
	CFLAGS += -DENTRY_GATES_RANGE_CHECK=1
	endif
	# DISPATCH_CACHE=1: PROTOTYPE, OFF by default (docs/DYNAREC-PROFILE.md).
	# A direct-mapped {guest pc -> host entry} table probed in asm by
	# mips_indirect_branch_* before the register save + C block lookup.
	# Speed only: guest timing is unchanged (tools/drprof/dr_oracle.py).
	ifeq ($(DISPATCH_CACHE),1)
	CFLAGS += -DDISPATCH_CACHE=1
	endif
	# DISPATCH_PRECISE=1: OFF by default (cpu-levers).  With DISPATCH_CACHE,
	# SMC retirement forgets only the retired blocks' entries instead of
	# emptying the cache; only canonical RAM pcs are cached (mirror-safe).
	# Exact: twin state hashes identical to the build without it.
	ifeq ($(DISPATCH_PRECISE),1)
	CFLAGS += -DDISPATCH_PRECISE=1
	endif
	# SMC_SAME_SKIP=1: OFF by default (cpu-levers).  A single-store SMC trap
	# in IWRAM skips the retirement when every block it would retire is still
	# exactly what it was translated from (per-halfword record of what each
	# translation read, checked over the blocks' whole extents).  The trap
	# itself is unchanged.  Changes guest timing (blocks keep their marks), so
	# it is judged by the gate's replayed lockstep, not bit identity.
	ifeq ($(SMC_SAME_SKIP),1)
	CFLAGS += -DSMC_SAME_SKIP=1
	endif
	# SPIN_SKIP=1: OFF by default (cpu-levers).  A loop the translator proves
	# side-effect free (loads + ALU only, every iteration a pure function of
	# memory) ends the scheduler slice when it spins on locations only an
	# event can change; anything else (timer counters, KEYINPUT, serial,
	# cartridge IO) vetoes it at its first spin.  MIPS dynarec only.  Changes
	# timing (by < 1 loop iteration per event): judged by the replayed lockstep.
	ifeq ($(SPIN_SKIP),1)
	CFLAGS += -DSPIN_SKIP=1
	endif
	# NO_GAME_SPEED_HINTS=1: OFF by default (claude/idle-general).  The game
	# table (gba_over.h) keeps its cartridge facts -- save type, RTC, rumble,
	# serial mode -- but its idle-loop address and translation-gate seeds are
	# ignored (gba_memory.c says what each was worth).  Pair it with
	# SPIN_SKIP=1, which proves the real waits from the code itself.  Changes
	# timing on the table's titles.
	ifeq ($(NO_GAME_SPEED_HINTS),1)
	CFLAGS += -DNO_GAME_SPEED_HINTS=1
	endif
	# SMC_GATE_SELFPATCH=1: OFF by default (claude/idle-general).  The ranked
	# SMC gate rule also promotes a store that patches the code just ahead of
	# itself (same contiguous translated run), even when it re-stores the
	# same bytes, and gates the lowest word it wrote.  Learns Mario Golf's
	# table gate (0x03000d00) at run time.  Needs SMC_GATES_RANKED.
	ifeq ($(SMC_GATE_SELFPATCH),1)
	CFLAGS += -DSMC_GATE_SELFPATCH=1
	endif
	# SMC_SAME_ADAPT=1: OFF by default (cpu-levers).  Refines SMC_SAME_SKIP:
	# stop recording translated IWRAM halfwords while the skip rate does not
	# pay for the recording (Hot Wheels: all cost, no traps); re-probe after
	# single-word retirements.  Exact like SMC_SAME_SKIP (it only skips less).
	ifeq ($(SMC_SAME_ADAPT),1)
	CFLAGS += -DSMC_SAME_ADAPT=1
	endif
	# SMC_STM_RANGE=1: OFF by default (cpu-levers).  A short writeback STMIA
	# (2-4 registers) from IWRAM into IWRAM retires exactly its own span
	# instead of taking a full RAM flush (FIFA 07's code generator).
	ifeq ($(SMC_STM_RANGE),1)
	CFLAGS += -DSMC_STM_RANGE=1
	endif
	# XLAT_PURE=1: OFF by default (multi-version reuse step 1).  scan_block
	# clears every instruction's update_cycles slot at the top of its loop, so a
	# block's translation depends only on its own guest bytes (a precondition of
	# reusing translations).  Changes timing where a stale slot used to be 1.
	ifeq ($(XLAT_PURE),1)
	CFLAGS += -DXLAT_PURE=1
	endif
	# SMC_REUSE=1: OFF by default (multi-version reuse step 2).  A full RAM
	# flush keeps the native code; a RAM block whose guest bytes are unchanged
	# is revived instead of translated again (cpu_threaded.c SMC_REUSE).
	# Speed only; needs XLAT_PURE=1.
	ifeq ($(SMC_REUSE),1)
	ifneq ($(XLAT_PURE),1)
	$(error SMC_REUSE=1 needs XLAT_PURE=1)
	endif
	CFLAGS += -DSMC_REUSE=1
	ifneq ($(SMC_REUSE_SPARE_KB),)
	CFLAGS += -DSMC_REUSE_SPARE_KB=$(SMC_REUSE_SPARE_KB)
	endif
	# SMC_REUSE_MODE=0|1|2|3 (default 3): off / record only / on / on without cost rules, as one
	# initialised word, so hardware A/B arms share the code layout.
	ifneq ($(SMC_REUSE_MODE),)
	CFLAGS += -DSMC_REUSE_MODE=$(SMC_REUSE_MODE)
	endif
	ifeq ($(SMC_REUSE_ARMING),1)
	CFLAGS += -DSMC_REUSE_ARMING=1
	endif
	endif
	# SMC_IMM_PATCH=1: OFF by default (xlat-cost 2026-10-28).  An IWRAM ARM
	# data-processing immediate that the game keeps rewriting is emitted in a
	# fixed lui/ori form and patched in place on the next imm-only store
	# instead of retiring the block (Doom II / Duke / Ice Nine renderers).
	# Needs JIT_CODE_DISCIPLINE (and the SMC single-store path).
	ifeq ($(SMC_IMM_PATCH),1)
	ifneq ($(JIT_CODE_DISCIPLINE),1)
	$(error SMC_IMM_PATCH=1 needs JIT_CODE_DISCIPLINE=1)
	endif
	CFLAGS += -DSMC_IMM_PATCH=1
	endif
	# XS_NOTE_FAST=1: OFF by default (xlat-cost).  xs_note_block records eight
	# halfwords at a time where that is exactly what the per-halfword path
	# does.  Exact (hash-identical).  Needs SMC_SAME_SKIP.
	ifeq ($(XS_NOTE_FAST),1)
	CFLAGS += -DXS_NOTE_FAST=1
	endif
	# ENTRY_GATES_FAST=1: OFF by default (xlat-cost).  The entry-gate harvest
	# tests four tag slots per load.  Exact.  Needs ENTRY_GATES.
	ifeq ($(ENTRY_GATES_FAST),1)
	CFLAGS += -DENTRY_GATES_FAST=1
	endif
	# SMC_STM_WIDE=1: OFF by default (cpu-levers, twin prototype).  Widens
	# SMC_STM_RANGE to 2-16 registers and lets such a store ACTIVATE selective
	# retirement (Hot Wheels' STMIA r0!,{r4-r11} code generator).  Needs
	# SMC_STM_RANGE.
	ifeq ($(SMC_STM_WIDE),1)
	CFLAGS += -DSMC_STM_WIDE=1
	endif
	# DMA_SMC_RETIRE=1: OFF by default (cpu-levers, twin prototype).  A DMA that
	# lands on translated code retires exactly the tagged words it wrote while
	# selective retirement is active, instead of a full RAM flush (Top Gear
	# Rally's ROM->IWRAM overlays).  Needs SMC_STM_RANGE.
	ifeq ($(DMA_SMC_RETIRE),1)
	CFLAGS += -DDMA_SMC_RETIRE=1
	endif
	# DMA_SMC_ADAPT=1: OFF by default (twin prototype).  DMA_SMC_RETIRE only
	# while it measures cheaper than the full flush (probe/backoff controller,
	# EVT dma_retire_mode logs each decision).  Needs DMA_SMC_RETIRE.
	ifeq ($(DMA_SMC_ADAPT),1)
	CFLAGS += -DDMA_SMC_ADAPT=1
	endif
	# DMA_ADAPT_V2=1: OFF by default (tgr-regression).  DMA_SMC_ADAPT's cost
	# model re-priced from the twin (per translated halfword AND block, walk
	# slot, full flush) and ties go to the full flush (cpu_threaded.c says
	# why: Top Gear Rally +5 % on hardware).  Needs DMA_SMC_ADAPT.
	ifeq ($(DMA_ADAPT_V2),1)
	CFLAGS += -DDMA_ADAPT_V2=1
	endif
	# SMC_STM_AHEAD=1: OFF by default (twin prototype).  SMC_STM_WIDE retires
	# forward over the tagged run after an advancing copy's write (bounded by
	# SMC_STM_AHEAD_BYTES / SMC_STM_AHEAD_BLOCKS).  Needs SMC_STM_WIDE.
	ifeq ($(SMC_STM_AHEAD),1)
	CFLAGS += -DSMC_STM_AHEAD=1
	endif
	ifeq ($(SMC_STM_AHEAD_MUTANT),1)
	CFLAGS += -DSMC_STM_AHEAD_MUTANT=1
	endif
	# SMC_STM_FIXED=1: OFF by default (perf/tmgc).  SMC_STM_RANGE's span
	# retirement also for STMIA Rn WITHOUT writeback (ipatix's mixer pastes
	# its resampling loop with STMIA LR,{..}: The Morrow's Golden Country).
	# No forward widening for this shape.  Needs SMC_STM_RANGE.
	ifeq ($(SMC_STM_FIXED),1)
	CFLAGS += -DSMC_STM_FIXED=1
	endif
	# XLAT_THUMB_NO_UNSWITCH=1: OFF by default (cpu-levers).  Host-compiler
	# only: stops psp-gcc -O3 unswitching translate_block_thumb's loop on
	# ram_region, which doubles the function and spills translation_ptr to
	# the stack.  Emitted code and guest behaviour unchanged.  tools/build.sh
	# runs tools/xlat_spill_guard.sh when it is on.
	ifeq ($(XLAT_THUMB_NO_UNSWITCH),1)
	CFLAGS += -DXLAT_THUMB_NO_UNSWITCH=1
	endif
	# STATIC_ALU_PC=1: OFF by default (cpu-levers).  `add/sub pc, pc, #imm`
	# (S clear) becomes a direct branch linked to the target's block instead of
	# an indirect dispatch.  Exact: guest state bit-identical (needs
	# DISPATCH_CYCLE_CHECK=1).  MIPS dynarec only.
	ifeq ($(STATIC_ALU_PC),1)
	CFLAGS += -DSTATIC_ALU_PC=1
	endif
	# SERIAL_IDLE_FAST=1: contender #3, OFF by default (docs/DYNAREC-PROFILE.md).
	# Skips update_serial's call chain when no transfer is pending and this
	# console has no link client role; exact by construction (main.c).
	ifeq ($(SERIAL_IDLE_FAST),1)
	CFLAGS += -DSERIAL_IDLE_FAST=1
	endif
	# SCHED_FAST=1: scheduler fast paths in update_gba, compiled in but OFF
	# until the run-time switch `sched_fast` is set (harness key sched_fast;
	# docs/GBA-LINK-MIRROR.md 24).  Pure speed, exact by construction.
	ifeq ($(SCHED_FAST),1)
	CFLAGS += -DSCHED_FAST=1
	endif
	# IRQ_IO_FAST=1: 16-bit stores to IE / IF / IME inline in the I/O store
	# stub, compiled in but OFF until `irq_io_fast` is set before the game
	# loads (harness key irq_io_fast; docs/GBA-LINK-MIRROR.md 25).  Exact.
	ifeq ($(IRQ_IO_FAST),1)
	CFLAGS += -DIRQ_IO_FAST=1
	endif
	# TIMER_LAZY=1: PROTOTYPE, changes guest-visible timing (overflows of
	# timers with no interrupt and no cascade are not scheduler events).
	# Compiled in but OFF until `timer_lazy` is set (harness key timer_lazy;
	# docs/GBA-LINK-MIRROR.md 26).  Not a decision to ship.
	ifeq ($(TIMER_LAZY),1)
	CFLAGS += -DTIMER_LAZY=1
	endif
	# JIT_CODE_DISCIPLINE=1: OFF by default (docs/JIT-CODE-DISCIPLINE.md).
	# Code memory in three zones, every code write recorded through one API,
	# one publish point (D writeback + I invalidate of exactly what was
	# written), and a whole I-cache invalidate on every ownership change
	# (cache flush, emitter rebuild, tier lend).  Replaces 8b48b48's whole
	# invalidate on every ROM publish.  Emulation is unchanged.
	ifeq ($(JIT_CODE_DISCIPLINE),1)
	CFLAGS += -DJIT_CODE_DISCIPLINE=1
	endif
	# JIT_CODE_CHECK=1: DIAGNOSTIC.  With JIT_CODE_DISCIPLINE, assert the zones
	# on every recorded write and on every pointer a lookup returns (prints to
	# stderr; meant for the twin and desktop builds).
	ifeq ($(JIT_CODE_CHECK),1)
	CFLAGS += -DJIT_CODE_CHECK=1
	endif
	# JIT_CODE_AB=1: HARNESS ONLY.  With JIT_CODE_DISCIPLINE, compile the
	# original sync model in as well and pick one at startup (harness key
	# jit_code_mode: 0 legacy, 1 + 8b48b48, 2 discipline, 3 discipline without
	# the ownership invalidate), so a hardware A/B runs ONE binary and the code
	# layout cannot differ between arms.
	ifeq ($(JIT_CODE_AB),1)
	CFLAGS += -DJIT_CODE_AB=1
	endif
	# XLAT_DEPTH_PROBE=1: DIAGNOSTIC (soak/diagnostic profiles).  Records the
	# deepest nested block translation and the lowest $sp seen at translation
	# entry (xlat_depth_max / xlat_sp_min) for the frontend's health line.
	ifeq ($(XLAT_DEPTH_PROBE),1)
	CFLAGS += -DXLAT_DEPTH_PROBE=1
	endif
	# IRQ_INTEGRITY_CHECK=1: DIAGNOSTIC (soak/diagnostic profiles).  At every
	# IRQ return (BIOS `subs pc, lr, #4`) compare r0-r14, CPSR and the return
	# address with the IRQ entry snapshot -- the GBA BIOS and ABI guarantee they
	# match -- and on a mismatch write ms0:/irqchk.txt plus a RAM dump.  One
	# struct copy per interrupt.  NEVER IN A RELEASE (Memory Stick writes).
	ifeq ($(IRQ_INTEGRITY_CHECK),1)
	CFLAGS += -DIRQ_INTEGRITY_CHECK=1
	endif
	# BADJUMP_REPORT=1: DIAGNOSTIC, and part of the `diagnostic` profile.
	# When the emulated pc leaves every region block_lookup_translate handles,
	# upstream hands the dispatcher ~0 and it jumps there -- "Bad Execution
	# Address / PC: ffffffff", which is how that crash presents.  This dumps
	# the registers, the gate table and the previous block entry to
	# ms0:/badjump.txt, and turns badjump_recover's file append back on.
	# NEVER IN A RELEASE: it writes to the memory stick from inside the dynarec
	# dispatch path, on the emulation thread.  gpsp_profile.h refuses it.
	ifeq ($(BADJUMP_REPORT),1)
	CFLAGS += -DBADJUMP_REPORT=1
	endif
	# GPSP_PROFILE=release|harness|diagnostic.  Names the build so
	# gpsp_profile.h can reject combinations that are silently wrong (a
	# release with telemetry, two SMC rules where one shadows the other).
	# tools/build.sh always sets it; leaving it unset means "unprofiled",
	# which still compiles but gets no cross-checking.
	ifeq ($(GPSP_PROFILE),release)
	CFLAGS += -DGPSP_PROFILE_RELEASE=1
	endif
	ifeq ($(GPSP_PROFILE),harness)
	CFLAGS += -DGPSP_PROFILE_HARNESS=1
	endif
	ifeq ($(GPSP_PROFILE),diagnostic)
	CFLAGS += -DGPSP_PROFILE_DIAGNOSTIC=1
	endif
	# SMC_SP_CHECKED=1: send SP-based block STORES down the general path so
	# their final word reaches execute_store_u32 and the SMC check.  The SP
	# fast path emits raw stores with no check at all, which is the last
	# known hole after the STM detector fix (mips/mips_emit.h).  Costs speed
	# on the hottest memory path, so it is a flag and not a default until
	# hardware says it is the bug.
	ifeq ($(SMC_SP_CHECKED),1)
	CFLAGS += -DSMC_SP_CHECKED
	endif
	# ADR-0039 pixel format.  MUST MATCH psp/Makefile's PSP_PIXFMT -- the
	# core decides the layout, the frontend's blit assumes it, and nothing
	# at link time checks.  Build both with `make PSP_PIXFMT=rgb565` to get
	# the old libretro layout back for a hardware A/B.
	PSP_PIXFMT ?= psp5650
	ifeq ($(PSP_PIXFMT), psp5650)
		CFLAGS += -DUSE_PSP_RGB565_FORMAT
	endif
	CFLAGS += -I$(shell psp-config --pspsdk-path)/include
	CFLAGS += -march=allegrex -mfp32 -mgp32 -mlong32 -mabi=eabi
	CFLAGS += -fomit-frame-pointer -ffast-math
	CFLAGS += -falign-functions=32 -falign-loops -falign-labels -falign-jumps
	STATIC_LINKING = 1
	HAVE_DYNAREC = 1
	CPU_ARCH := mips

# Vita
else ifeq ($(platform), vita)
	TARGET := $(TARGET_NAME)_libretro_$(platform).a
	CC = arm-vita-eabi-gcc$(EXE_EXT)
	CXX = arm-vita-eabi-g++$(EXE_EXT)
	AR = arm-vita-eabi-ar$(EXE_EXT)
	CFLAGS += -DVITA
	CFLAGS += -marm -mcpu=cortex-a9 -mfloat-abi=hard
	CFLAGS += -Wall -mword-relocations
	CFLAGS += -fomit-frame-pointer -ffast-math
	CFLAGS += -mword-relocations -fno-unwind-tables -fno-asynchronous-unwind-tables 
	CFLAGS += -ftree-vectorize -fno-optimize-sibling-calls
	ASFLAGS += -mcpu=cortex-a9
	STATIC_LINKING = 1
	HAVE_DYNAREC = 1
	OVERCLOCK_60FPS = 1
	CPU_ARCH := arm

# CTR(3DS)
else ifeq ($(platform), ctr)
	TARGET := $(TARGET_NAME)_libretro_$(platform).a
	CC = $(DEVKITARM)/bin/arm-none-eabi-gcc$(EXE_EXT)
	CXX = $(DEVKITARM)/bin/arm-none-eabi-g++$(EXE_EXT)
	AR = $(DEVKITARM)/bin/arm-none-eabi-ar$(EXE_EXT)
	CFLAGS += -DARM11 -D_3DS
	CFLAGS += -march=armv6k -mtune=mpcore -mfloat-abi=hard
	CFLAGS += -Wall -mword-relocations
	CFLAGS += -fomit-frame-pointer -ffast-math
	CPU_ARCH := arm
	HAVE_DYNAREC = 1
	STATIC_LINKING = 1

# Raspberry Pi 3
else ifeq ($(platform), rpi3)
	TARGET := $(TARGET_NAME)_libretro.so
	fpic := -fPIC
	SHARED := -shared -Wl,--version-script=link.T -Wl,--no-undefined
	CFLAGS += -marm -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard
	CFLAGS += -fomit-frame-pointer -ffast-math
	CPU_ARCH := arm
	MMAP_JIT_CACHE = 1
	HAVE_DYNAREC = 1

# Raspberry Pi 2
else ifeq ($(platform), rpi2)
	TARGET := $(TARGET_NAME)_libretro.so
	fpic := -fPIC
	SHARED := -shared -Wl,--version-script=link.T -Wl,--no-undefined
	CFLAGS += -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
	CFLAGS += -fomit-frame-pointer -ffast-math
	CPU_ARCH := arm
	MMAP_JIT_CACHE = 1
	HAVE_DYNAREC = 1

# Raspberry Pi 1
else ifeq ($(platform), rpi1)
	TARGET := $(TARGET_NAME)_libretro.so
	fpic := -fPIC
	SHARED := -shared -Wl,--version-script=link.T -Wl,--no-undefined
	CFLAGS += -DARM11
	CFLAGS += -marm -mfpu=vfp -mfloat-abi=hard -march=armv6j
	CFLAGS += -fomit-frame-pointer -ffast-math
	CPU_ARCH := arm
	MMAP_JIT_CACHE = 1
	HAVE_DYNAREC = 1

# Classic Platforms ####################
# Platform affix = classic_<ISA>_<µARCH>
# Help at https://modmyclassic.com/comp

# (armv7 a7, hard point, neon based) ### 
# NESC, SNESC, C64 mini 
else ifeq ($(platform), classic_armv7_a7)
	TARGET := $(TARGET_NAME)_libretro.so
	fpic := -fPIC
    SHARED := -shared -Wl,--version-script=link.T  -Wl,--no-undefined -fPIC
	CFLAGS += -Ofast \
	-flto=4 -fwhole-program -fuse-linker-plugin \
	-fdata-sections -ffunction-sections -Wl,--gc-sections \
	-fno-stack-protector -fno-ident -fomit-frame-pointer \
	-falign-functions=1 -falign-jumps=1 -falign-loops=1 \
	-fno-unwind-tables -fno-asynchronous-unwind-tables -fno-unroll-loops \
	-fmerge-all-constants -fno-math-errno \
	-marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
	ASFLAGS += $(CFLAGS)
	ARCH = arm
	CPU_ARCH := arm
	MMAP_JIT_CACHE = 1
	HAVE_DYNAREC = 1
	ifeq ($(shell echo `$(CC) -dumpversion` "< 4.9" | bc -l), 1)
	  CFLAGS += -march=armv7-a
	else
	  CFLAGS += -march=armv7ve
	  # If gcc is 5.0 or later
	  ifeq ($(shell echo `$(CC) -dumpversion` ">= 5" | bc -l), 1)
	    LDFLAGS += -static-libgcc -static-libstdc++
	  endif
	endif

#######################################

# Xbox 360
else ifeq ($(platform), xenon)
	TARGET := $(TARGET_NAME)_libretro_xenon360.a
	CC = xenon-gcc$(EXE_EXT)
	CXX = xenon-g++$(EXE_EXT)
	AR = xenon-ar$(EXE_EXT)
	CFLAGS += -D__LIBXENON__ -m32 -D__ppc__
	STATIC_LINKING = 1

# Nintendo Game Cube
else ifeq ($(platform), ngc)
	TARGET := $(TARGET_NAME)_libretro_$(platform).a
	CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc$(EXE_EXT)
	CXX = $(DEVKITPPC)/bin/powerpc-eabi-g++$(EXE_EXT)
	AR = $(DEVKITPPC)/bin/powerpc-eabi-ar$(EXE_EXT)
	CFLAGS += -DGEKKO -DHW_DOL -mrvl -mcpu=750 -meabi -mhard-float -DMSB_FIRST -D__ppc__
	STATIC_LINKING = 1

# Nintendo Wii
else ifeq ($(platform), wii)
	TARGET := $(TARGET_NAME)_libretro_$(platform).a
	CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc$(EXE_EXT)
	CXX = $(DEVKITPPC)/bin/powerpc-eabi-g++$(EXE_EXT)
	AR = $(DEVKITPPC)/bin/powerpc-eabi-ar$(EXE_EXT)
	CFLAGS += -DGEKKO -DHW_RVL -mrvl -mcpu=750 -meabi -mhard-float -DMSB_FIRST -D__ppc__
	STATIC_LINKING = 1

# aarch64 (armv8)
else ifeq ($(platform), arm64)
	TARGET := $(TARGET_NAME)_libretro.so
	SHARED := -shared -Wl,--version-script=link.T
	fpic := -fPIC
	CFLAGS += -fomit-frame-pointer -ffast-math
	LDFLAGS += -Wl,--no-undefined
	HAVE_DYNAREC := 1
	MMAP_JIT_CACHE = 1
	CPU_ARCH := arm64

# ARM
else ifneq (,$(findstring armv,$(platform)))
	TARGET := $(TARGET_NAME)_libretro.so
	SHARED := -shared -Wl,--version-script=link.T
	CPU_ARCH := arm
	MMAP_JIT_CACHE = 1
	fpic := -fPIC
	ifneq (,$(findstring cortexa5,$(platform)))
		CFLAGS += -marm -mcpu=cortex-a5
		ASFLAGS += -mcpu=cortex-a5
	else ifneq (,$(findstring cortexa8,$(platform)))
		CFLAGS += -marm -mcpu=cortex-a8
		ASFLAGS += -mcpu=cortex-a8
	else ifneq (,$(findstring cortexa9,$(platform)))
		CFLAGS += -marm -mcpu=cortex-a9
		ASFLAGS += -mcpu=cortex-a9
	else ifneq (,$(findstring cortexa15a7,$(platform)))
		CFLAGS += -marm -mcpu=cortex-a15.cortex-a7
		ASFLAGS += -mcpu=cortex-a15.cortex-a7
	else
		CFLAGS += -marm
	endif
	ifneq (,$(findstring softfloat,$(platform)))
		CFLAGS += -mfloat-abi=softfp
		ASFLAGS += -mfloat-abi=softfp
	else ifneq (,$(findstring hardfloat,$(platform)))
		CFLAGS += -mfloat-abi=hard
		ASFLAGS += -mfloat-abi=hard
	endif
	# Dynarec works at least in rpi, take a look at issue #11
	ifeq (,$(findstring no-dynarec,$(platform)))
		HAVE_DYNAREC := 1
	endif
	LDFLAGS += -Wl,--no-undefined

# DYNAREC PROFILING TWIN (tools/drprof, docs/DYNAREC-PROFILE.md).
# The PSP's own ARM->MIPS translator built as a static Linux mipsel archive,
# so the exact emitter the PSP runs can be executed under qemu-mipsel (with an
# instruction-counting TCG plugin) and compared frame by frame against other
# translator variants.  Never shipped: nothing links this but tools/drprof.
# Differences from psp1 that reach emitted code are listed in
# docs/DYNAREC-PROFILE.md ("fidelity of the twin"): the patch handler uses
# sltiu/branch where the PSP uses Allegrex `min`, and synci where the PSP uses
# `cache`.  Everything else in mips_emit.h is shared.
else ifeq ($(platform), drprof-mipsel)
	TARGET := $(TARGET_NAME)_libretro_drprof_mipsel.a
	CC = mipsel-linux-gnu-gcc
	CXX = mipsel-linux-gnu-g++
	AR = mipsel-linux-gnu-ar
	CFLAGS += -fno-pic -fno-pie -mno-shared -march=mips32r2 -mhard-float
	CFLAGS += -fomit-frame-pointer -ffast-math -DMIPS_HAS_R2_INSTS -DDRPROF_MIPSEL_LINUX
	STATIC_LINKING = 1
	HAVE_DYNAREC := 1
	CPU_ARCH := mips

# MIPS
else ifeq ($(platform), mips32)
	TARGET := $(TARGET_NAME)_libretro.so
	SHARED := -shared -nostdlib -Wl,--version-script=link.T
	fpic := -fPIC -DPIC
	CFLAGS += -fomit-frame-pointer -ffast-math -march=mips32 -mtune=mips32r2 -mhard-float
	CFLAGS += -DMIPS_HAS_R2_INSTS
	HAVE_DYNAREC := 1
	CPU_ARCH := mips

# MIPS64
else ifeq ($(platform), mips64n32)
	TARGET := $(TARGET_NAME)_libretro.so
	SHARED := -shared -nostdlib -Wl,--version-script=link.T
	fpic := -fPIC -DPIC
	CFLAGS += -fomit-frame-pointer -ffast-math -march=mips64 -mabi=n32 -mhard-float
	HAVE_DYNAREC := 1
	CPU_ARCH := mips

# PS2
else ifeq ($(platform), ps2)
	TARGET := $(TARGET_NAME)_libretro_$(platform).a
	CC = mips64r5900el-ps2-elf-gcc$(EXE_EXT)
	CXX = mips64r5900el-ps2-elf-g++$(EXE_EXT)
	AR = mips64r5900el-ps2-elf-ar$(EXE_EXT)
	CFLAGS += -fomit-frame-pointer -ffast-math
	CFLAGS += -DPS2 -DUSE_XBGR1555_FORMAT -DSMALL_TRANSLATION_CACHE -DROM_BUFFER_SIZE=16
	CFLAGS += -D_EE -I$(PS2SDK)/ee/include/ -I$(PS2SDK)/common/include/
	HAVE_DYNAREC = 1
	CPU_ARCH := mips
	STATIC_LINKING = 1

# emscripten
else ifeq ($(platform), emscripten)
	TARGET := $(TARGET_NAME)_libretro_$(platform).bc
	STATIC_LINKING = 1

# GCW0 (OD and OD Beta)
else ifeq ($(platform), gcw0)
	TARGET := $(TARGET_NAME)_libretro.so
	CC = /opt/gcw0-toolchain/usr/bin/mipsel-linux-gcc
	CXX = /opt/gcw0-toolchain/usr/bin/mipsel-linux-g++
	AR = /opt/gcw0-toolchain/usr/bin/mipsel-linux-ar
	SHARED := -shared -nostdlib -Wl,--version-script=link.T
	fpic := -fPIC -DPIC
	CFLAGS += -fomit-frame-pointer -ffast-math -march=mips32 -mtune=mips32r2 -mhard-float
	CFLAGS += -DMIPS_HAS_R2_INSTS
	HAVE_DYNAREC := 1
	CPU_ARCH := mips

# RETROFW
else ifeq ($(platform), retrofw)
	TARGET := $(TARGET_NAME)_libretro.so
	CC = /opt/retrofw-toolchain/usr/bin/mipsel-linux-gcc
	CXX = /opt/retrofw-toolchain/usr/bin/mipsel-linux-g++
	AR = /opt/retrofw-toolchain/usr/bin/mipsel-linux-ar
	SHARED := -shared -nostdlib -Wl,--version-script=link.T
	fpic := -fPIC -DPIC
	CFLAGS += -fomit-frame-pointer -ffast-math -march=mips32  -mhard-float
	HAVE_DYNAREC := 1
	CPU_ARCH := mips

# RS90
else ifeq ($(platform), rs90)
	TARGET := $(TARGET_NAME)_libretro.so
	CC = /opt/rs90-toolchain/usr/bin/mipsel-linux-gcc
	CXX = /opt/rs90-toolchain/usr/bin/mipsel-linux-g++
	AR = /opt/rs90-toolchain/usr/bin/mipsel-linux-ar
	SHARED := -shared -nostdlib -Wl,--version-script=link.T
	fpic := -fPIC -DPIC
	CFLAGS += -fomit-frame-pointer -ffast-math -march=mips32 -mtune=mips32
	CFLAGS += -DSMALL_TRANSLATION_CACHE -DROM_BUFFER_SIZE=4
	HAVE_DYNAREC := 1
	CPU_ARCH := mips

# MIYOO
else ifeq ($(platform), miyoo)
	TARGET := $(TARGET_NAME)_libretro.so
	CC = /opt/miyoo/usr/bin/arm-linux-gcc
	CXX = /opt/miyoo/usr/bin/arm-linux-g++
	AR = /opt/miyoo/usr/bin/arm-linux-ar
	SHARED := -shared -nostdlib -Wl,--version-script=link.T
	fpic := -fPIC -DPIC
	CFLAGS += -fomit-frame-pointer -ffast-math -march=armv5te -mtune=arm926ej-s
	CFLAGS += -DSMALL_TRANSLATION_CACHE
	HAVE_DYNAREC := 1
	CPU_ARCH := arm
	

else ifeq ($(platform), miyoomini)
	TARGET := $(TARGET_NAME)_plus_libretro.so
	CC = /opt/miyoomini-toolchain/usr/bin/arm-linux-gcc
	CXX = /opt/miyoomini-toolchain/usr/bin/arm-linux-g++
	AR = /opt/miyoomini-toolchain/usr/bin/arm-linux-ar
	fpic := -fPIC
	SHARED := -shared -Wl,--version-script=link.T -Wl,--no-undefined
	CFLAGS += -Ofast \
	-flto=4 -fwhole-program -fuse-linker-plugin \
	-fdata-sections -ffunction-sections -Wl,--gc-sections \
	-fno-stack-protector -fno-ident -fomit-frame-pointer \
	-falign-functions=1 -falign-jumps=1 -falign-loops=1 \
	-fno-unwind-tables -fno-asynchronous-unwind-tables -fno-unroll-loops \
	-fmerge-all-constants -fno-math-errno \
	-marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
	CXXFLAGS = $(CFLAGS) -std=gnu++11
	CPPFLAGS += $(CFLAGS)
	ASFLAGS += $(CFLAGS)
	CPU_ARCH := arm
	MMAP_JIT_CACHE = 1
	HAVE_DYNAREC = 1
	HAVE_NEON = 1
	ARCH = arm
	BUILTIN_GPU = neon

# Windows
else
	TARGET := $(TARGET_NAME)_libretro.dll
	CC ?= gcc
	CXX ?= g++
	SHARED := -shared -static-libgcc -static-libstdc++ -s -Wl,--version-script=link.T
	CFLAGS += -D__WIN32__ -D__WIN32_LIBRETRO__

	# Windows32/64 has dynarec support
	HAVE_DYNAREC := 1
	CPU_ARCH := x86_32
	MMAP_JIT_CACHE = 1
endif

ifeq ($(MMAP_JIT_CACHE), 1)
CFLAGS += -DMMAP_JIT_CACHE
endif

# Add -DTRACE_EVENTS to trace relevant events (IRQs, SMC, etc)
# Add -DTRACE_INSTRUCTIONS to trace instruction execution
# Can add -DTRACE_REGISTERS to additionally print register values
ifeq ($(DEBUG), 1)
	OPTIMIZE      := -O0 -g
else
	OPTIMIZE      := -O3 -DNDEBUG
endif

DEFINES := -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H -D__LIBRETRO__ -DINLINE=inline -Wall

ifeq ($(HAVE_DYNAREC), 1)
DEFINES += -DHAVE_DYNAREC
endif

ifeq ($(CPU_ARCH), arm)
	DEFINES += -DARM_ARCH
else ifeq ($(CPU_ARCH), arm64)
	DEFINES += -DARM64_ARCH
else ifeq ($(CPU_ARCH), mips)
	DEFINES += -DMIPS_ARCH
else ifeq ($(CPU_ARCH), x86_32)
	DEFINES += -DX86_ARCH
endif

# GBA_LINK=1: the two-core link cable (gbalink_core.c, docs/GBA-LINK-MIRROR.md).
# A core flag: it adds update_gba hooks and one store to the I/O write stub.
ifeq ($(GBA_LINK),1)
DEFINES += -DGBA_LINK
ifeq ($(GBALINK_LEAN_OUTLINE),1)
DEFINES += -DGBALINK_LEAN_OUTLINE
endif
endif

# REWIND=1: the rewind prototype's core half (docs/REWIND.md) -- the two
# pieces of machine state a save state does not carry.  Off by default.
ifeq ($(REWIND),1)
CFLAGS += -DREWIND
# REWIND_MUTANT=n: tests only (tools/rewind_test.py mutant).  A store path
# that forgets to mark the rewind's written-page map: 1 = the dynarec's
# byte-store stub, 2 = DMA.  Never in a build that is handed to anyone.
ifneq ($(REWIND_MUTANT),)
CFLAGS += -DREWIND_MUTANT=$(REWIND_MUTANT)
endif
# REWIND_COUNT=1: measurement only, counts the stores the page marks ride on.
ifeq ($(REWIND_COUNT),1)
CFLAGS += -DREWIND_COUNT
endif
endif

include Makefile.common

OBJECTS := $(SOURCES_C:.c=.o) $(SOURCES_ASM:.S=.o) $(SOURCES_CC:.cc=.o)

WARNINGS_DEFINES =
CODE_DEFINES =

COMMON_DEFINES += $(CODE_DEFINES) $(WARNINGS_DEFINES) $(fpic)

CFLAGS += $(DEFINES) $(COMMON_DEFINES)

ifeq ($(FRONTEND_SUPPORTS_RGB565), 1)
	CFLAGS += -DFRONTEND_SUPPORTS_RGB565
endif

# check if user compiles a 60 FPS overclock version
ifeq ($(OVERCLOCK_60FPS),1)
	CFLAGS += -DOVERCLOCK_60FPS
endif


ifeq ($(platform), ctr)
ifeq ($(HAVE_DYNAREC), 1)
OBJECTS += 3ds/3ds_utils.o 3ds/3ds_cache_utils.o

ifeq ($(strip $(CTRULIB)),)
$(error "Please set CTRULIB in your environment. export CTRULIB=<path to>libctru")
endif

CFLAGS  += -I$(CTRULIB)/include

endif
endif

# Escape hatch for one-off builds (profiling probes, oracle perturbations):
#   make platform=psp1 EXTRA_CFLAGS=-DSOMETHING
CFLAGS  += $(EXTRA_CFLAGS)

CXXFLAGS = $(CFLAGS) -fno-rtti -fno-exceptions -std=c++11

ifeq ($(platform), theos_ios)
COMMON_FLAGS := -DIOS $(COMMON_DEFINES) $(INCFLAGS) -I$(THEOS_INCLUDE_PATH) -Wno-error
$(LIBRARY_NAME)_CFLAGS += $(COMMON_FLAGS) $(CFLAGS)
${LIBRARY_NAME}_FILES = $(SOURCES_C) $(SOURCES_ASM) $(SOURCES_CC)
include $(THEOS_MAKE_PATH)/library.mk
else
all: $(TARGET)

# Linking with gcc on purpose, we do not use any libstdc++ dependencies at all, only libc is required.

$(TARGET): $(OBJECTS)
ifeq ($(STATIC_LINKING), 1)
	$(AR) rcs $@ $(OBJECTS)
else
	$(CC) $(fpic) $(SHARED) $(INCFLAGS) $(OPTIMIZE) -o $@ $(OBJECTS) $(LIBM) $(LDFLAGS)
endif

cpu_threaded.o: cpu_threaded.c
	$(CC) $(INCFLAGS) $(CFLAGS) $(OPTIMIZE) -Wno-unused-variable -Wno-unused-label -c  -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) $(CFLAGS) $(OPTIMIZE) -c -o $@ $<

%.o: %.c
	$(CC) $(INCFLAGS) $(CFLAGS) $(OPTIMIZE) -c  -o $@ $<

%.o: %.cc
	$(CXX) $(INCFLAGS) $(CXXFLAGS) $(OPTIMIZE) -c  -o $@ $<

clean-objs:
	rm -rf $(OBJECTS)

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: clean
endif
