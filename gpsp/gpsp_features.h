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
 * WHAT IS LEFT HERE.  Code-health batch A-6 built 51 former switches into the
 * dynarec unconditionally (cpu_threaded.c, mips/, jit_code.h; docs/BUILD-
 * SWITCHES.md marks them "built in").  What remains is a switch for a reason:
 * the 7 the twin oracles turn off on purpose (DISPATCH_CACHE, XLAT_FAST,
 * RAM_MIRROR_FIX, NV_COND_FIX, SMC_STM_FIXED, XLAT_LAST_TRY_LAZY,
 * BIOS_BOUND_FIX) and the 13 whose off-arm the desktop and host builds still
 * compile (code-health B-1 bakes those).  Two of the 13, SMC_GATES and
 * SMC_PARTIAL_SAFE, can no longer be switched off in a PSP core (see below).
 *
 * SWITCHING A FEATURE OFF.  -U cannot do it: gcc applies -D and -U before any
 * -include, so a define here would beat them.  These all leave this file alone:
 *
 *   make platform=psp1 XLAT_FAST=0     one feature off (the Makefile turns it
 *   CORE_EXTRA="XLAT_FAST=0"           into -DGPSP_NO_XLAT_FAST; tools/build.sh
 *                                      takes it for any profile but release)
 *   -DGPSP_NO_XLAT_FAST                the same define, by hand
 *   -DGPSP_FEATURES_EXPLICIT           this file defines nothing: the compile
 *                                      line is the complete list (the twin's
 *                                      FEATURES=explicit, which GBAdhoc-Lab's
 *                                      gate uses with a manifest's coreFlags),
 *                                      so a variant that omits a switch has it
 *                                      off and -UDISPATCH_CACHE works.
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

/* ---- REQUIRED, not switches: SMC translation gates, selective retirement ---
 * Everything that refines these two was built in by code-health A-6, so a PSP
 * core (and a twin variant) cannot be built without them: gpsp_profile.h stops
 * the compile, GPSP_NO_SMC_GATES / GPSP_NO_SMC_PARTIAL_SAFE only reach that
 * error, and the root Makefile refuses SMC_GATES=0 / SMC_PARTIAL_SAFE=0.  They
 * are still defined here because manifests (coreFlags), make_release.sh's
 * feature list and the twin's explicit lists name them. */
#if !defined(GPSP_NO_SMC_GATES) && !defined(SMC_GATES)
#define SMC_GATES 1
#endif
#if !defined(GPSP_NO_SMC_PARTIAL_SAFE) && !defined(SMC_PARTIAL_SAFE)
#define SMC_PARTIAL_SAFE 1
#endif

/* ---- single-store and block-store SMC coverage ----------------------------- */
#if !defined(GPSP_NO_SMC_SINGLE_HOT) && !defined(SMC_SINGLE_HOT)
#define SMC_SINGLE_HOT 64
#endif
#if !defined(GPSP_NO_SMC_STM_FIXED) && !defined(SMC_STM_FIXED)
#define SMC_STM_FIXED 1
#endif

/* ---- unchanged-code filters and in-place patching -------------------------- */
#if !defined(GPSP_NO_XS_HEAP) && !defined(XS_HEAP)
#define XS_HEAP 1
#endif

/* ---- DMA onto translated code ---------------------------------------------- */
#if !defined(GPSP_NO_DMA_SMC_FLUSH) && !defined(DMA_SMC_FLUSH)
#define DMA_SMC_FLUSH 1
#endif
#if !defined(GPSP_NO_DMA_SMC_RETIRE) && !defined(DMA_SMC_RETIRE)
#define DMA_SMC_RETIRE 1
#endif

/* ---- dispatch and translator speed ----------------------------------------- */
#if !defined(GPSP_NO_DISPATCH_CACHE) && !defined(DISPATCH_CACHE)
#define DISPATCH_CACHE 1
#endif
#if !defined(GPSP_NO_XLAT_FAST) && !defined(XLAT_FAST)
#define XLAT_FAST 1
#endif
#if !defined(GPSP_NO_SPIN_SKIP) && !defined(SPIN_SKIP)
#define SPIN_SKIP 1
#endif

/* ---- scheduler, interrupt and serial fast paths ---------------------------- */
#if !defined(GPSP_NO_SCHED_FAST) && !defined(SCHED_FAST)
#define SCHED_FAST 1
#endif
#if !defined(GPSP_NO_SERIAL_IDLE_FAST) && !defined(SERIAL_IDLE_FAST)
#define SERIAL_IDLE_FAST 1
#endif
/* HALT_HOP: a halted CPU jumps over the H-blank edges nothing can see; the
 * run-time mask timer_lazy defaults to HALT_HOP_DEFAULT (6 = HALT_HOP +
 * HOP_DRAWN).  Exact (corpus, pair twin hashes; claude/kirby-hop2). */
#if !defined(GPSP_NO_HALT_HOP) && !defined(HALT_HOP)
#define HALT_HOP 1
#endif

/* ---- guest correctness ----------------------------------------------------- */
#if !defined(GPSP_NO_FIQ_BANK_FIX) && !defined(FIQ_BANK_FIX)
#define FIQ_BANK_FIX 1
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

#endif /* !GPSP_FEATURES_EXPLICIT */

#endif /* GPSP_FEATURES_H */
