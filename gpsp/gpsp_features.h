/* gpsp_features.h -- WHAT THIS EMULATOR IS.
 *
 * The dynarec feature set of a PSP build, permanently.  The root Makefile
 * force-includes this file (-include, so nothing can forget it and it cannot
 * arrive late) into every core object of platform=psp1 and
 * platform=drprof-mipsel, C, C++ and assembler alike.  Keep it to comments and
 * preprocessor lines for that reason.  The frontend (psp/Makefile) and the
 * Media Engine module do not read these; GBA_LINK, REWIND and LAYOUT_PIN change
 * what is linked, so they stay make variables, default on for psp1.
 *
 * Other platforms (unix, the SDL desktop build, the host tests) do not include
 * it and compile the #else paths, unchanged.
 *
 * SWITCHING A FEATURE OFF.  -U cannot do it: gcc applies -D and -U before any
 * -include, so a define here would beat them.  These all leave this file alone:
 *
 *   make platform=psp1 XLAT_PURE=0     one feature off (the Makefile turns it
 *   CORE_EXTRA="XLAT_PURE=0"           into -DGPSP_NO_XLAT_PURE; tools/build.sh
 *                                      takes it for any profile but release)
 *   -DGPSP_NO_XLAT_PURE                the same define, by hand
 *   -DGPSP_FEATURES_EXPLICIT           this file defines nothing: the compile
 *                                      line is the complete list.  The twin
 *                                      oracles work this way (tools/drprof/
 *                                      Makefile), so a variant that omits a
 *                                      switch has it off and -UDISPATCH_CACHE
 *                                      keeps working.
 *
 * A value on the compile line (SMC_SINGLE_HOT=32) wins over the one here.
 * gpsp_profile.h still refuses the combinations that are silently wrong, so
 * turning off something another feature needs is a compile error.
 *
 * tools/build.sh records what the compiler actually resolved in the manifest's
 * coreFlags, and tools/make_release.sh refuses a build that is not exactly this
 * list.  docs/BUILD-SWITCHES.md says what each one does and why it ships.
 */
#ifndef GPSP_FEATURES_H
#define GPSP_FEATURES_H

#ifndef GPSP_FEATURES_EXPLICIT

/* ---- SMC translation gates ------------------------------------------------- */
#if !defined(GPSP_NO_SMC_GATES) && !defined(SMC_GATES)
#define SMC_GATES 1
#endif
#if !defined(GPSP_NO_SMC_GATES_SIMPLE) && !defined(SMC_GATES_SIMPLE)
#define SMC_GATES_SIMPLE 1
#endif
#if !defined(GPSP_NO_SMC_GATES_RANKED) && !defined(SMC_GATES_RANKED)
#define SMC_GATES_RANKED 1
#endif
#if !defined(GPSP_NO_SMC_GATE_BITMAP) && !defined(SMC_GATE_BITMAP)
#define SMC_GATE_BITMAP 1
#endif
#if !defined(GPSP_NO_SMC_GATE_CHARGE) && !defined(SMC_GATE_CHARGE)
#define SMC_GATE_CHARGE 1
#endif
#if !defined(GPSP_NO_SMC_GATE_SELFPATCH) && !defined(SMC_GATE_SELFPATCH)
#define SMC_GATE_SELFPATCH 1
#endif
#if !defined(GPSP_NO_ENTRY_GATES) && !defined(ENTRY_GATES)
#define ENTRY_GATES 1
#endif
#if !defined(GPSP_NO_ENTRY_GATES_RANGE) && !defined(ENTRY_GATES_RANGE)
#define ENTRY_GATES_RANGE 1
#endif
#if !defined(GPSP_NO_ENTRY_GATES_FAST) && !defined(ENTRY_GATES_FAST)
#define ENTRY_GATES_FAST 1
#endif

/* ---- selective SMC retirement ---------------------------------------------- */
#if !defined(GPSP_NO_SMC_PARTIAL_SAFE) && !defined(SMC_PARTIAL_SAFE)
#define SMC_PARTIAL_SAFE 1
#endif
#if !defined(GPSP_NO_SMC_PARTIAL_STABLE_THUNK) && !defined(SMC_PARTIAL_STABLE_THUNK)
#define SMC_PARTIAL_STABLE_THUNK 1
#endif
#if !defined(GPSP_NO_SMC_PARTIAL_DIRECT_LINKS) && !defined(SMC_PARTIAL_DIRECT_LINKS)
#define SMC_PARTIAL_DIRECT_LINKS 1
#endif
#if !defined(GPSP_NO_SMC_RETIRE_WINDOW) && !defined(SMC_RETIRE_WINDOW)
#define SMC_RETIRE_WINDOW 1
#endif
#if !defined(GPSP_NO_SMC_RETIRE_LOCAL) && !defined(SMC_RETIRE_LOCAL)
#define SMC_RETIRE_LOCAL 1
#endif
#if !defined(GPSP_NO_SMC_RETIRE_LIST) && !defined(SMC_RETIRE_LIST)
#define SMC_RETIRE_LIST 1
#endif

/* ---- single-store and block-store SMC coverage ----------------------------- */
#if !defined(GPSP_NO_SMC_PARTIAL_SINGLE) && !defined(SMC_PARTIAL_SINGLE)
#define SMC_PARTIAL_SINGLE 1
#endif
#if !defined(GPSP_NO_SMC_SINGLE_THUMB) && !defined(SMC_SINGLE_THUMB)
#define SMC_SINGLE_THUMB 1
#endif
#if !defined(GPSP_NO_SMC_SINGLE_HALF) && !defined(SMC_SINGLE_HALF)
#define SMC_SINGLE_HALF 1
#endif
#if !defined(GPSP_NO_SMC_SINGLE_ANYPC) && !defined(SMC_SINGLE_ANYPC)
#define SMC_SINGLE_ANYPC 1
#endif
#if !defined(GPSP_NO_SMC_SINGLE_EWRAM) && !defined(SMC_SINGLE_EWRAM)
#define SMC_SINGLE_EWRAM 1
#endif
#if !defined(GPSP_NO_SMC_SINGLE_UNTAG) && !defined(SMC_SINGLE_UNTAG)
#define SMC_SINGLE_UNTAG 1
#endif
#if !defined(GPSP_NO_SMC_SINGLE_HOT) && !defined(SMC_SINGLE_HOT)
#define SMC_SINGLE_HOT 64
#endif
#if !defined(GPSP_NO_SMC_STM_RANGE) && !defined(SMC_STM_RANGE)
#define SMC_STM_RANGE 1
#endif
#if !defined(GPSP_NO_SMC_STM_CHAIN) && !defined(SMC_STM_CHAIN)
#define SMC_STM_CHAIN 1
#endif
#if !defined(GPSP_NO_SMC_STM_WIDE) && !defined(SMC_STM_WIDE)
#define SMC_STM_WIDE 1
#endif
#if !defined(GPSP_NO_SMC_STM_AHEAD) && !defined(SMC_STM_AHEAD)
#define SMC_STM_AHEAD 1
#endif
#if !defined(GPSP_NO_SMC_STM_FIXED) && !defined(SMC_STM_FIXED)
#define SMC_STM_FIXED 1
#endif

/* ---- unchanged-code filters and in-place patching -------------------------- */
#if !defined(GPSP_NO_SMC_SAME_SKIP) && !defined(SMC_SAME_SKIP)
#define SMC_SAME_SKIP 1
#endif
#if !defined(GPSP_NO_SMC_SAME_ADAPT) && !defined(SMC_SAME_ADAPT)
#define SMC_SAME_ADAPT 1
#endif
#if !defined(GPSP_NO_SMC_SAME_REPROBE) && !defined(SMC_SAME_REPROBE)
#define SMC_SAME_REPROBE 1024
#endif
#if !defined(GPSP_NO_SMC_IMM_PATCH) && !defined(SMC_IMM_PATCH)
#define SMC_IMM_PATCH 1
#endif
#if !defined(GPSP_NO_XS_HEAP) && !defined(XS_HEAP)
#define XS_HEAP 1
#endif
#if !defined(GPSP_NO_XS_WALK_PAIR) && !defined(XS_WALK_PAIR)
#define XS_WALK_PAIR 1
#endif
#if !defined(GPSP_NO_XS_NOTE_FAST) && !defined(XS_NOTE_FAST)
#define XS_NOTE_FAST 1
#endif

/* ---- translation reuse across full flushes --------------------------------- */
#if !defined(GPSP_NO_XLAT_PURE) && !defined(XLAT_PURE)
#define XLAT_PURE 1
#endif
#if !defined(GPSP_NO_SMC_REUSE) && !defined(SMC_REUSE)
#define SMC_REUSE 1
#endif
#if !defined(GPSP_NO_SMC_REUSE_SPARE_KB) && !defined(SMC_REUSE_SPARE_KB)
#define SMC_REUSE_SPARE_KB 256
#endif
#if !defined(GPSP_NO_SMC_REUSE_MODE) && !defined(SMC_REUSE_MODE)
#define SMC_REUSE_MODE 3
#endif

/* ---- DMA onto translated code ---------------------------------------------- */
#if !defined(GPSP_NO_DMA_SMC_FLUSH) && !defined(DMA_SMC_FLUSH)
#define DMA_SMC_FLUSH 1
#endif
#if !defined(GPSP_NO_DMA_SMC_RETIRE) && !defined(DMA_SMC_RETIRE)
#define DMA_SMC_RETIRE 1
#endif
#if !defined(GPSP_NO_DMA_SMC_ADAPT) && !defined(DMA_SMC_ADAPT)
#define DMA_SMC_ADAPT 1
#endif
#if !defined(GPSP_NO_DMA_ADAPT_V2) && !defined(DMA_ADAPT_V2)
#define DMA_ADAPT_V2 1
#endif

/* ---- dispatch and translator speed ----------------------------------------- */
#if !defined(GPSP_NO_DISPATCH_CACHE) && !defined(DISPATCH_CACHE)
#define DISPATCH_CACHE 1
#endif
#if !defined(GPSP_NO_DISPATCH_PRECISE) && !defined(DISPATCH_PRECISE)
#define DISPATCH_PRECISE 1
#endif
#if !defined(GPSP_NO_XLAT_FAST) && !defined(XLAT_FAST)
#define XLAT_FAST 1
#endif
#if !defined(GPSP_NO_JIT_COST) && !defined(JIT_COST)
#define JIT_COST 1
#endif
#if !defined(GPSP_NO_JIT_CODE_DISCIPLINE) && !defined(JIT_CODE_DISCIPLINE)
#define JIT_CODE_DISCIPLINE 1
#endif
#if !defined(GPSP_NO_SPIN_SKIP) && !defined(SPIN_SKIP)
#define SPIN_SKIP 1
#endif

/* ---- scheduler, interrupt and serial fast paths ---------------------------- */
#if !defined(GPSP_NO_SCHED_FAST) && !defined(SCHED_FAST)
#define SCHED_FAST 1
#endif
#if !defined(GPSP_NO_IRQ_IO_FAST) && !defined(IRQ_IO_FAST)
#define IRQ_IO_FAST 1
#endif
#if !defined(GPSP_NO_SERIAL_IDLE_FAST) && !defined(SERIAL_IDLE_FAST)
#define SERIAL_IDLE_FAST 1
#endif
#if !defined(GPSP_NO_MSR_FAST) && !defined(MSR_FAST)
#define MSR_FAST 1
#endif

/* ---- guest correctness ----------------------------------------------------- */
#if !defined(GPSP_NO_GBA_PC_MASK) && !defined(GBA_PC_MASK)
#define GBA_PC_MASK 1
#endif
#if !defined(GPSP_NO_BADJUMP_SAFE) && !defined(BADJUMP_SAFE)
#define BADJUMP_SAFE 1
#endif
#if !defined(GPSP_NO_DISPATCH_CYCLE_CHECK) && !defined(DISPATCH_CYCLE_CHECK)
#define DISPATCH_CYCLE_CHECK 1
#endif
#if !defined(GPSP_NO_FIQ_BANK_FIX) && !defined(FIQ_BANK_FIX)
#define FIQ_BANK_FIX 1
#endif
#if !defined(GPSP_NO_BX_ALIGN_FIX) && !defined(BX_ALIGN_FIX)
#define BX_ALIGN_FIX 1
#endif
#if !defined(GPSP_NO_BACKUP_BUS_FIX) && !defined(BACKUP_BUS_FIX)
#define BACKUP_BUS_FIX 1
#endif
#if !defined(GPSP_NO_COND_ENTRY_FIX) && !defined(COND_ENTRY_FIX)
#define COND_ENTRY_FIX 1
#endif
#if !defined(GPSP_NO_NV_COND_FIX) && !defined(NV_COND_FIX)
#define NV_COND_FIX 1
#endif
#if !defined(GPSP_NO_BIOS_BOUND_FIX) && !defined(BIOS_BOUND_FIX)
#define BIOS_BOUND_FIX 1
#endif
#if !defined(GPSP_NO_XLAT_LAST_TRY_LAZY) && !defined(XLAT_LAST_TRY_LAZY)
#define XLAT_LAST_TRY_LAZY 1
#endif
#if !defined(GPSP_NO_RAM_MIRROR_FIX) && !defined(RAM_MIRROR_FIX)
#define RAM_MIRROR_FIX 1
#endif
#if !defined(GPSP_NO_TIMER_RELOAD_FIX) && !defined(TIMER_RELOAD_FIX)
#define TIMER_RELOAD_FIX 1
#endif
#if !defined(GPSP_NO_VRAM_EXEC) && !defined(VRAM_EXEC)
#define VRAM_EXEC 1
#endif

/* ---- data placement -------------------------------------------------------- */
#if !defined(GPSP_NO_HOT_DATA_GUARD) && !defined(HOT_DATA_GUARD)
#define HOT_DATA_GUARD 1
#endif
#if !defined(GPSP_NO_HOT_DATA_OFFSET) && !defined(HOT_DATA_OFFSET)
#define HOT_DATA_OFFSET 0x1b00
#endif

#endif /* !GPSP_FEATURES_EXPLICIT */

#endif /* GPSP_FEATURES_H */
