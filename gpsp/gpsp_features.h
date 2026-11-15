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
 * it.  Since code-health B-1 that no longer changes what they run: no source
 * tests the 13 BUILT-IN names below, so the desktop and the host tests compile
 * the arms that ship (the dynarec-only ones wherever HAVE_DYNAREC is, SPIN_SKIP
 * wherever MIPS_ARCH is).
 *
 * WHAT IS LEFT HERE.  Code-health batch A-6 built 51 former switches into the
 * dynarec unconditionally and B-1 built in 13 more (docs/BUILD-SWITCHES.md
 * marks them "built in").  Two kinds of line remain:
 *   - SWITCHES: the 7 the twin oracles turn off on purpose (DISPATCH_CACHE,
 *     XLAT_FAST, RAM_MIRROR_FIX, NV_COND_FIX, SMC_STM_FIXED,
 *     XLAT_LAST_TRY_LAZY, BIOS_BOUND_FIX);
 *   - BUILT-IN NAMES: the 13 B-1 baked.  Nothing tests them; they are defined
 *     only so that a manifest's coreFlags (tools/build.sh print-features), the
 *     Lab registry's fixture `requires` (VRAM_EXEC=1, TIMER_RELOAD_FIX=1,
 *     FIQ_BANK_FIX=1), tools/make_release_check.py and tools/flags.lock keep
 *     naming what every build has.  They cannot be switched off: the root
 *     Makefile refuses NAME=0 and GPSP_NO_NAME stops the compile below.
 *
 * SWITCHING A FEATURE OFF.  -U cannot do it: gcc applies -D and -U before any
 * -include, so a define here would beat them.  These all leave this file alone:
 *
 *   make platform=psp1 XLAT_FAST=0     one switch off (the Makefile turns it
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
 * gpsp_profile.h still refuses the combinations that are silently wrong, so
 * turning off something another feature needs is a compile error.
 *
 * tools/build.sh records what the compiler actually resolved in the manifest's
 * coreFlags, and tools/make_release.sh refuses a build that is not exactly this
 * list.  docs/BUILD-SWITCHES.md says what each one does and why it ships.
 */
#ifndef GPSP_FEATURES_H
#define GPSP_FEATURES_H

#if defined(GPSP_NO_SMC_GATES) || defined(GPSP_NO_SMC_PARTIAL_SAFE) || \
    defined(GPSP_NO_SMC_SINGLE_HOT) || defined(GPSP_NO_XS_HEAP) || \
    defined(GPSP_NO_DMA_SMC_FLUSH) || defined(GPSP_NO_DMA_SMC_RETIRE) || \
    defined(GPSP_NO_SPIN_SKIP) || defined(GPSP_NO_SCHED_FAST) || \
    defined(GPSP_NO_SERIAL_IDLE_FAST) || defined(GPSP_NO_HALT_HOP) || \
    defined(GPSP_NO_FIQ_BANK_FIX) || defined(GPSP_NO_TIMER_RELOAD_FIX) || \
    defined(GPSP_NO_VRAM_EXEC)
#error "built in since code-health B-1 (docs/BUILD-SWITCHES.md): this feature has no off arm"
#endif

#ifndef GPSP_FEATURES_EXPLICIT

/* ==== BUILT-IN NAMES (code-health B-1): always compiled, never tested ====== */

/* SMC translation gates and selective retirement (with everything A-6 built
 * in on top of them), single-store and block-store coverage */
#ifndef SMC_GATES
#define SMC_GATES 1
#endif
#ifndef SMC_PARTIAL_SAFE
#define SMC_PARTIAL_SAFE 1
#endif
/* the widened single-store trap count (cpu_threaded.c smc_wide_gate, built in
 * as SMC_SINGLE_HOT_K 64; this line is its name for coreFlags) */
#ifndef SMC_SINGLE_HOT
#define SMC_SINGLE_HOT 64
#endif
#ifndef XS_HEAP
#define XS_HEAP 1
#endif
/* DMA onto translated code */
#ifndef DMA_SMC_FLUSH
#define DMA_SMC_FLUSH 1
#endif
#ifndef DMA_SMC_RETIRE
#define DMA_SMC_RETIRE 1
#endif
/* translator speed */
#ifndef SPIN_SKIP
#define SPIN_SKIP 1
#endif
/* scheduler, interrupt and serial fast paths.  HALT_HOP: a halted CPU jumps
 * over the H-blank edges nothing can see; the run-time mask timer_lazy
 * defaults to HALT_HOP_DEFAULT (6 = HALT_HOP + HOP_DRAWN). */
#ifndef SCHED_FAST
#define SCHED_FAST 1
#endif
#ifndef SERIAL_IDLE_FAST
#define SERIAL_IDLE_FAST 1
#endif
#ifndef HALT_HOP
#define HALT_HOP 1
#endif
/* guest correctness */
#ifndef FIQ_BANK_FIX
#define FIQ_BANK_FIX 1
#endif
#ifndef TIMER_RELOAD_FIX
#define TIMER_RELOAD_FIX 1
#endif
#ifndef VRAM_EXEC
#define VRAM_EXEC 1
#endif

/* ==== SWITCHES: the twin oracles turn these off ============================ */

/* ---- single-store and block-store SMC coverage ----------------------------- */
#if !defined(GPSP_NO_SMC_STM_FIXED) && !defined(SMC_STM_FIXED)
#define SMC_STM_FIXED 1
#endif

/* ---- dispatch and translator speed ----------------------------------------- */
#if !defined(GPSP_NO_DISPATCH_CACHE) && !defined(DISPATCH_CACHE)
#define DISPATCH_CACHE 1
#endif
#if !defined(GPSP_NO_XLAT_FAST) && !defined(XLAT_FAST)
#define XLAT_FAST 1
#endif

/* ---- guest correctness ----------------------------------------------------- */
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

#endif /* !GPSP_FEATURES_EXPLICIT */

#endif /* GPSP_FEATURES_H */
