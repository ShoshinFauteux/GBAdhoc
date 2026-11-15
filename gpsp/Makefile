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
	# GBA_LINK and REWIND are make variables and not gpsp_features.h lines
	# because each changes what is compiled and linked, here and in
	# psp/Makefile (same default); =0 is the arm without it.
	GBA_LINK ?= 1
	REWIND ?= 1
	# Layout pinning (docs/LAYOUT-PINNING.md), built in since code-health A-7:
	# every function in its own section, so psp/Makefile's link can put the hot
	# path first in .text in a fixed order (psp/layout/hot.ord), and -DLAYOUT_PIN
	# pins the translation cache's I-cache sets (gpsp_config.h JIT_PIN_*).  The
	# source keeps its `#if defined(LAYOUT_PIN)` arms because the twin builds
	# without it unless a variant asks (tools/drprof/dr_corpus.py allpin, beta).
	# The make kill switch LAYOUT_PIN=0 is gone; any value but 1 stops here.
	ifneq ($(filter-out 1,$(LAYOUT_PIN)),)
    $(error LAYOUT_PIN=$(LAYOUT_PIN): pinning is built in (code-health A-7); drop LAYOUT_PIN)
	endif
	CFLAGS += -ffunction-sections -DLAYOUT_PIN=1
	# SMC_PARTIAL_SAFE=1: experimental selective invalidation with exact,
	# per-ISA block extents and indirect cross-block branches into RAM. ARM
	# block stores are eligible only when they do not write back their base;
	# advancing copy loops retain the proven full-cache flush.
	ifeq ($(SMC_PARTIAL_SAFE),1)
	CFLAGS += -DSMC_PARTIAL_SAFE
	endif
	# SMC_GATES=1: add a translation gate at each self-modified address so
	# future blocks split there.  Independent of SMC_PARTIAL_SAFE on purpose —
	# gates are what bought Unbound its speed AND what broke its audio, so
	# the two must be testable separately.
	ifeq ($(SMC_GATES),1)
	CFLAGS += -DSMC_GATES
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
	# jsmolka nes.gba).
	# FIQ_BANK_FIX=1: bank FIQ's r8-r12 correctly in set_cpu_mode (cpu.cc;
	# OpenLara).  (From claude/self-activate 950c5f3/25cab1b.)
	ifeq ($(FIQ_BANK_FIX),1)
	CFLAGS += -DFIQ_BANK_FIX=1
	endif
	# MSR_FAST_DEFAULT=0: compiled in, msr_fast off (a layout-identical A/B arm).
	ifneq ($(MSR_FAST_DEFAULT),)
	CFLAGS += -DMSR_FAST_DEFAULT=$(MSR_FAST_DEFAULT)
	endif
	ifeq ($(VRAM_EXEC),1)
	CFLAGS += -DVRAM_EXEC=1
	endif
	ifneq ($(GATE_RATIO),)
	CFLAGS += -DSMC_GATE_RATIO=$(GATE_RATIO)
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
	# TIMER_RELOAD_FIX=1: a timer that has just overflowed reads its reload
	# value instead of 0x0000 (main.c update_timers).  Need for Speed
	# Underground computed a 4 GB mixer fill from the 0 and wiped IWRAM
	# (dr2-nfsu 2026-10-21).  Guest-visible.
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
	# SMC_RETIRE_WINDOW_CHECK=1: also walk the old 4 KiB window and count any
	# live overlapping block the short window would miss (smc_window_violations,
	# falls back to the full walk).  In the twin a violation aborts the run,
	# so dr_oracle.py base <variant with -DSMC_RETIRE_WINDOW_CHECK> is the test.
	ifeq ($(SMC_RETIRE_WINDOW_CHECK),1)
	CFLAGS += -DSMC_RETIRE_WINDOW_CHECK=1
	endif
	# SMC_RETIRE_LOCAL_CHECK=1: diagnostic, also walk the gap below the built-in
	# per-64-byte retire bound (SMC_RETIRE_LOCAL) and count (twin: abort on) any
	# live overlapping block the bound would miss.
	ifeq ($(SMC_RETIRE_LOCAL_CHECK),1)
	CFLAGS += -DSMC_RETIRE_LOCAL_CHECK=1
	endif
	# XS_HEAP=1: (cpu-levers cpl2w).  The SMC_SAME_SKIP and
	# SMC_RETIRE_LOCAL tables (up to 69 KiB) come from one heap block taken
	# before the ROM cache instead of .bss, so the hot arrays after them in
	# .bss keep the release layout (D-cache sets).
	ifeq ($(XS_HEAP),1)
	CFLAGS += -DXS_HEAP=1
	endif
	ifeq ($(PCARG_SIDE_PAD),1)
	CFLAGS += -DPCARG_SIDE_PAD=1
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
	# ENTRY_GATES_RANGE_CHECK=1: diagnostic, abort on a live tag outside the
	# range the built-in exhaustion-time gate harvest walks (ENTRY_GATES_RANGE).
	ifeq ($(ENTRY_GATES_RANGE_CHECK),1)
	CFLAGS += -DENTRY_GATES_RANGE_CHECK=1
	endif
	# DISPATCH_CACHE=1: (docs/DYNAREC-PROFILE.md).
	# A direct-mapped {guest pc -> host entry} table probed in asm by
	# mips_indirect_branch_* before the register save + C block lookup.
	# Speed only: guest timing is unchanged (tools/drprof/dr_oracle.py).
	ifeq ($(DISPATCH_CACHE),1)
	CFLAGS += -DDISPATCH_CACHE=1
	endif
	# SPIN_SKIP=1: (cpu-levers).  A loop the translator proves
	# side-effect free (loads + ALU only, every iteration a pure function of
	# memory) ends the scheduler slice when it spins on locations only an
	# event can change; anything else (timer counters, KEYINPUT, serial,
	# cartridge IO) vetoes it at its first spin.  MIPS dynarec only.  Changes
	# timing (by < 1 loop iteration per event): judged by the replayed lockstep.
	ifeq ($(SPIN_SKIP),1)
	CFLAGS += -DSPIN_SKIP=1
	endif
	# DMA_SMC_RETIRE=1: (cpu-levers, twin prototype).  A DMA that
	# lands on translated code retires exactly the tagged words it wrote while
	# selective retirement is active, instead of a full RAM flush (Top Gear
	# Rally's ROM->IWRAM overlays).  Needs SMC_STM_RANGE.
	ifeq ($(DMA_SMC_RETIRE),1)
	CFLAGS += -DDMA_SMC_RETIRE=1
	endif
	ifeq ($(SMC_STM_AHEAD_MUTANT),1)
	CFLAGS += -DSMC_STM_AHEAD_MUTANT=1
	endif
	# SMC_STM_FIXED=1: ON in gpsp_features.h (Chandler Hardy, PR #27; 4.6.0
	# beta; =0 switches it off).  SMC_STM_RANGE's span
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
	# SERIAL_IDLE_FAST=1: contender #3 (docs/DYNAREC-PROFILE.md).
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
	# TIMER_LAZY=1: PROTOTYPE, changes guest-visible timing (overflows of
	# timers with no interrupt and no cascade are not scheduler events).
	# Compiled in but OFF until `timer_lazy` is set (harness key timer_lazy;
	# docs/GBA-LINK-MIRROR.md 26).  Not a decision to ship.
	# HALT_HOP=1: while the CPU is halted, one scheduler jump over H-blank
	# edges nothing can see (main.c halt_hop_begin); exact (corpus 35/35, pair
	# twin hash-identical, claude/kirby-hop2).  Run-time mask timer_lazy
	# (bit 1 HALT_HOP, bit 2 HOP_DRAWN); HALT_HOP_DEFAULT=N its start value
	# (default 6 = both on).
	ifeq ($(HALT_HOP),1)
	CFLAGS += -DHALT_HOP=1
	endif
	ifneq ($(HALT_HOP_DEFAULT),)
	CFLAGS += -DHALT_HOP_DEFAULT=$(HALT_HOP_DEFAULT)
	endif
	ifeq ($(TIMER_LAZY),1)
	CFLAGS += -DTIMER_LAZY=1
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

# RFU_SCHED=1: scheduled ticks for the emulated Wireless Adapter (rfu_sched.h,
# docs/RFU-COUPLING.md).  Off by default.  The frontend needs the same value
# (psp/Makefile, sdl/Makefile); tools/build.sh RFU_SCHED=1 sets both.
ifeq ($(RFU_SCHED),1)
DEFINES += -DRFU_SCHED
endif

# REWIND=1: the rewind's core half (docs/REWIND.md) -- the two pieces of
# machine state a save state does not carry.  Default 1 on psp1 only.
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

# THE FEATURE SET (gpsp_features.h): what a PSP core is, force-included so no
# recipe has to list it.  The twin gets the same header; tools/drprof/Makefile
# says -DGPSP_FEATURES_EXPLICIT because its variants are complete lists.  NAME=0
# on this make switches one feature off: a -U on the compile line would lose to
# the header.  The `ifeq ($(NAME),1)` blocks above stay because NAME=1 and
# NAME=N are still legal, and GBAdhoc-Lab's twin reads them to turn a manifest's
# coreFlags back into defines.  Other platforms never see any of this.
ifneq (,$(filter $(platform),psp1 drprof-mipsel))
GPSP_FEATURE_NAMES := $(shell sed -n 's/^\#define \([A-Z0-9_]*\) .*/\1/p' $(CORE_DIR)/gpsp_features.h)
CFLAGS += -include $(CORE_DIR)/gpsp_features.h
CFLAGS += $(foreach f,$(GPSP_FEATURE_NAMES),$(if $(filter 0,$($(f))),-DGPSP_NO_$(f)))
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

# rm: `ar r` only adds and replaces, so an arm that drops an object (GBA_LINK=0)
# would otherwise keep it as a stale member.
$(TARGET): $(OBJECTS)
ifeq ($(STATIC_LINKING), 1)
	rm -f $@
	$(AR) rcs $@ $(OBJECTS)
else
	$(CC) $(fpic) $(SHARED) $(INCFLAGS) $(OPTIMIZE) -o $@ $(OBJECTS) $(LIBM) $(LDFLAGS)
endif

# STALE-OBJECT SAFETY (ADR-0035's trap, here in the core).  -MMD -MP: a changed
# header rebuilds what includes it.  The flags stamp: a changed compile line
# rebuilds everything, because make does not compare what it compiled with.
# It is rewritten only when its text differs, so an unchanged line rebuilds
# nothing.  Neither changes a byte of any object.  psp/Makefile has the header
# half; its compile line is not stamped, so tools/build.sh still cleans first.
DEPFLAGS := -MMD -MP
FLAGS_STAMP := .core-flags-$(platform)
$(FLAGS_STAMP): FORCE
	@printf '%s\n' '$(subst ','"'"',$(CC) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(ASFLAGS) $(OPTIMIZE))' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi
$(OBJECTS): $(FLAGS_STAMP)
-include $(OBJECTS:.o=.d)

cpu_threaded.o: cpu_threaded.c
	$(CC) $(DEPFLAGS) $(INCFLAGS) $(CFLAGS) $(OPTIMIZE) -Wno-unused-variable -Wno-unused-label -c  -o $@ $<

%.o: %.S
	$(CC) $(DEPFLAGS) $(ASFLAGS) $(CFLAGS) $(OPTIMIZE) -c -o $@ $<

%.o: %.c
	$(CC) $(DEPFLAGS) $(INCFLAGS) $(CFLAGS) $(OPTIMIZE) -c  -o $@ $<

%.o: %.cc
	$(CXX) $(DEPFLAGS) $(INCFLAGS) $(CXXFLAGS) $(OPTIMIZE) -c  -o $@ $<

# What the compiler really resolves for this make line, one NAME=VALUE per
# feature.  tools/build.sh writes it into the manifest's coreFlags, so the list
# is the build's own answer and not a copy of a recipe.
print-features:
	@$(CC) $(CFLAGS) -dM -E -x c /dev/null | \
	  sed -n 's/^#define \([A-Z0-9_]*\) \(.*\)$$/\1=\2/p' | \
	  grep $(foreach f,$(GPSP_FEATURE_NAMES) GBA_LINK REWIND RFU_SCHED,-e '^$(f)=')

clean-objs:
	rm -rf $(OBJECTS)

clean:
	rm -f $(OBJECTS) $(OBJECTS:.o=.d) gbalink_core.o gbalink_core.d $(TARGET) $(FLAGS_STAMP)

FORCE:

.PHONY: clean print-features FORCE
endif
