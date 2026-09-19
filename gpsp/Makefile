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
	# Keep 32 MiB cartridges on the paged-ROM path on every PSP model.  A
	# PSP-3000 can otherwise allocate all 32 one-megabyte blocks and Unbound
	# repeatedly diverges during battle (audio/video corruption followed by a
	# freeze).  The PSP-1000 naturally tops out at 15 blocks and is stable; an
	# explicit 15-block cap reproduced that behavior on the 3000 across the
	# interleaved hardware soak.  This remains PSP-only so other ports retain
	# their existing cache policy.
	CFLAGS += -DROM_BUFFER_SIZE=15
	# JIT cache sizing: SMALL (2MB ROM / 384KB RAM code) fits the PSP-1000
	# memory budget.  BIG_JIT=1 drops the define -> 10MB/512KB caches for
	# large-memory (MEMSIZE=1) builds on 2000+ hardware, where CFRU-class
	# ROM hacks (Unbound) overflow the small cache and flush-storm.
	ifneq ($(BIG_JIT),1)
	CFLAGS += -DSMALL_TRANSLATION_CACHE
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
