/* gpsp_profile.h — WHAT KIND OF BUILD IS THIS, and is that combination legal?
 *
 * Included by both halves of the build: the core (cpu_threaded.c) and the PSP
 * frontend (psp/main_psp.c).  Its whole job is to turn "a flag combination
 * nobody meant to ship" from a thing you discover on hardware into a thing the
 * compiler refuses.
 *
 * WHY IT EXISTS.  Three flavours of this emulator are built regularly and none
 * of them were ever named:
 *
 *   release     what a player installs.  No telemetry, no automation, no
 *               diagnostics, no experimental control arms.
 *   harness     the hardware performance rig: telemetry plus autopilot, so a
 *               job file can drive a console unattended.
 *   diagnostic  release plus investigation instruments (SMC histogram, bad-jump
 *               report), for one question at a time.
 *
 * Their definitions lived in four places -- build.sh, tools/build_perf.py,
 * tools/build_stability.sh and releases/<name>/build.sh -- and the flags are spread
 * across two makefiles that do not track define changes.  That is how the
 * 7283f13 release candidate shipped writing ms0:/badjump.txt: BADJUMP_SAFE was
 * wanted, its logging side was not, and nothing in the build could tell the
 * difference.  Naming the profiles puts the decision in one place; the checks
 * below make the one that matters unmistakable.
 *
 * Every #error here is a combination that is silently WRONG rather than merely
 * unusual: one flag quietly wins over another, or a flag does nothing at all,
 * or an experiment's control arm is still switched on.  A combination that is
 * simply unusual is allowed -- this is not a taste filter.
 */
#ifndef GPSP_PROFILE_H
#define GPSP_PROFILE_H

/* ---- which profile ------------------------------------------------------ */

#if defined(GPSP_PROFILE_RELEASE) + defined(GPSP_PROFILE_HARNESS) + \
    defined(GPSP_PROFILE_DIAGNOSTIC) > 1
#error "Pick ONE build profile: GPSP_PROFILE_RELEASE, _HARNESS or _DIAGNOSTIC."
#endif

/* An unprofiled build is still allowed -- ad-hoc experiments and the non-PSP
 * ports do it all the time -- it just gets no cross-checking.  tools/build.sh
 * always names one. */
#if defined(GPSP_PROFILE_RELEASE)
#define GPSP_PROFILE_NAME "release"
#elif defined(GPSP_PROFILE_HARNESS)
#define GPSP_PROFILE_NAME "harness"
#elif defined(GPSP_PROFILE_DIAGNOSTIC)
#define GPSP_PROFILE_NAME "diagnostic"
#else
#define GPSP_PROFILE_NAME "unprofiled"
#endif

/* ---- what release must never contain ------------------------------------ */

#ifdef GPSP_PROFILE_RELEASE

/* Automation and telemetry.  A player's console must not be drivable by a job
 * file, and must not write an event log to their memory stick. */
#ifdef GPSP_PERF_RIG
#error "release + GPSP_PERF_RIG: that is the hardware harness, not a release."
#endif
#ifdef GPSP_KEEP_TELEMETRY
#error "release + GPSP_KEEP_TELEMETRY: releases do not write event logs."
#endif

/* Diagnostics that write to the Memory Stick.  BADJUMP_REPORT is the one that
 * actually shipped: it appends to ms0:/badjump.txt from inside the dynarec
 * dispatch path, on the emulation thread. */
#ifdef BADJUMP_REPORT
#error "release + BADJUMP_REPORT: writes ms0:/badjump.txt from the dispatcher."
#endif
#ifdef SMC_WRITE_HISTO
#error "release + SMC_WRITE_HISTO: writes ms0:/smchisto.txt every 4000 flushes."
#endif
#ifdef XLAT_DEPTH_PROBE
#error "release + XLAT_DEPTH_PROBE: soak/diagnostic translation-depth instrument."
#endif
#ifdef GPSP_ROMLOAD_DIAGNOSTICS
#error "release + GPSP_ROMLOAD_DIAGNOSTICS: diagnostic ROM-load instrumentation."
#endif
#ifdef IRQ_INTEGRITY_CHECK
#error "release + IRQ_INTEGRITY_CHECK: writes ms0:/irqchk.txt when IRQ state is lost."
#endif
#ifdef STALE_CHECK
#error "release + STALE_CHECK: PPSSPP stale-translation detector, slow and writes ms0."
#endif
/* The 2026-09-24 tracing apparatus (cpu_threaded.c, main.c, gba_memory.c):
 * each changes emulation or timing on purpose. */
#if defined(STATE_HASH_TRACE) || defined(UPDATE_TRACE_LO) || defined(RTC_PIN) || \
    defined(STALE_STACKNOTE)
#error "release + a gate-layout A/B diagnostic switch (docs/BUILD-SWITCHES.md)."
#endif

/* Dynarec profiling (drprof.h, docs/DYNAREC-PROFILE.md).  The twin is a
 * host-only build; the hardware bench adds a phase store around every C call
 * from translated code and runs a sampling alarm. */
#if defined(DRPROF_TWIN) || defined(DRPROF_HW) || defined(DRPROF_NEGCTL)
#error "release + DRPROF_*: dynarec profiling instruments, never shipped."
#endif

/* JIT_CODE_DISCIPLINE's instruments (docs/JIT-CODE-DISCIPLINE.md): zone
 * asserts print to stderr and the audit keeps a shadow of both code zones. */
#if defined(JIT_CODE_CHECK) || defined(JIT_CODE_AUDIT) || defined(JIT_SYNC_STATS)
#error "release + JIT_CODE_CHECK/JIT_CODE_AUDIT/JIT_SYNC_STATS: twin/debug instruments."
#endif
#ifdef JIT_CODE_AB
#error "release + JIT_CODE_AB: the hardware A/B selector (harness key jit_code_mode)."
#endif

/* Experiment control arms.  Each of these deliberately DISABLES something the
 * release depends on, so shipping one reads as a performance regression with no
 * apparent cause. */
#ifdef GPSP_CATCH_SELFTEST
#error "release + GPSP_CATCH_SELFTEST: L+R+UP+TRIANGLE crashes the PSP on purpose."
#endif

#endif /* GPSP_PROFILE_RELEASE */

/* ---- the harness needs its telemetry ------------------------------------ */

#if defined(GPSP_PERF_RIG) && !defined(GPSP_KEEP_TELEMETRY)
#error "GPSP_PERF_RIG without GPSP_KEEP_TELEMETRY: the rig reads the event log."
#endif

/* ---- SMC flag combinations that silently pick a winner ------------------ */
/* These are the ones that cost real hardware experiments: the build succeeds,
 * one rule quietly shadows the other, and the result is attributed to the flag
 * that was not in fact active.  See cpu_threaded.c for each rule. */

/* SMC_GATES and SMC_PARTIAL_SAFE, which the A-6 rules refine, are built in
 * since code-health B-1 (no #if tests them), and SPIN_SKIP is compiled
 * wherever MIPS_ARCH is, so the checks that stood here are gone. */

/* DISPATCH_PRECISE, DISPATCH_ASSOC and DISPATCH_LOOKUP_PC are built in: they
 * are part of DISPATCH_CACHE (code-health A-6), so DISPATCH_CACHE off takes
 * them with it, and DISPATCH_LOOKUP_PC is also off under JIT_CODE_AB (whose
 * legacy modes sync outside the publish bookkeeping its probe relies on).  The
 * 2-way sets still need a table of at least two entries. */
#if defined(DISPATCH_CACHE) && defined(DISPATCH_CACHE_BITS) && (DISPATCH_CACHE_BITS < 2)
#error "DISPATCH_CACHE's 2-way sets (DISPATCH_ASSOC) need DISPATCH_CACHE_BITS >= 2 (it is entries per ISA; sets are half that)."
#endif

#if defined(SMC_REUSE_DEBUG) && !defined(DRPROF_TWIN)
#error "SMC_REUSE_DEBUG is the twin's self-check (stdio, getenv): never in a console build."
#endif
/* SMC_REUSE's contract point 1 (cpu_threaded.c): the translator is a pure
 * function of the block's guest bytes and the environment it checks.  These
 * switches break that or add links a revival does not replay. */

#endif /* GPSP_PROFILE_H */
