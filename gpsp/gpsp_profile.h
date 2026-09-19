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
#ifdef GPSP_ROMLOAD_DIAGNOSTICS
#error "release + GPSP_ROMLOAD_DIAGNOSTICS: diagnostic ROM-load instrumentation."
#endif

/* Experiment control arms.  Each of these deliberately DISABLES something the
 * release depends on, so shipping one reads as a performance regression with no
 * apparent cause. */
#ifdef SMC_PARTIAL_SAFE_CONTROL
#error "release + SMC_PARTIAL_SAFE_CONTROL: the A/B control arm, not a release."
#endif

/* Rejected experiments.  SMC_SKIP_SAME white-screens Pokemon Heart & Soul at
 * boot (its compare is wrong); it is kept for the record, not for shipping. */
#ifdef SMC_SKIP_SAME
#error "release + SMC_SKIP_SAME: known to white-screen Heart & Soul at boot."
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

#if defined(SMC_PARTIAL_SAFE) && defined(SMC_PARTIAL)
#error "SMC_PARTIAL_SAFE and SMC_PARTIAL: SAFE silently wins. Pick one."
#endif

#if defined(SMC_GATES_CLUSTER) && defined(SMC_GATES_SIMPLE)
#error "SMC_GATES_CLUSTER and SMC_GATES_SIMPLE: CLUSTER silently wins."
#endif

#if defined(SMC_GATES_SNAPF) && defined(SMC_GATES_SNAP)
#error "SMC_GATES_SNAPF and SMC_GATES_SNAP: SNAPF silently wins."
#endif

#if defined(SMC_GATES_CODED) && (defined(SMC_GATES_SNAP) || defined(SMC_GATES_SNAPF))
#error "SMC_GATES_CODED with SNAP/SNAPF: two gate-address filters at once."
#endif

/* A gate sub-rule without SMC_GATES compiles to nothing at all, so the
 * experiment it was meant to run never happened. */
#ifndef SMC_GATES
#if defined(SMC_GATES_SIMPLE)  || defined(SMC_GATES_CLUSTER) || \
    defined(SMC_GATES_RANKED)  || defined(SMC_GATES_SNAP)    || \
    defined(SMC_GATES_SNAPF)   || defined(SMC_GATES_CODED)   || \
    defined(SMC_GATE_BITMAP)
#error "An SMC_GATE* sub-rule is set without SMC_GATES: it does nothing."
#endif
#endif

/* Selective retirement redirects live blocks through a trampoline; a direct
 * link into one without the stable thunk points at a body that has moved. */
#if (defined(SMC_PARTIAL_DIRECT_LINKS) || defined(SMC_PARTIAL_DIRECT_GATE)) && \
    !defined(SMC_PARTIAL_STABLE_THUNK)
#error "SMC_PARTIAL_DIRECT_* requires SMC_PARTIAL_STABLE_THUNK."
#endif

/* The SMC_PARTIAL_SAFE sub-flags are inert without it, same trap as above. */
#ifndef SMC_PARTIAL_SAFE
#if defined(SMC_PARTIAL_STABLE_THUNK) || defined(SMC_PARTIAL_DIRECT_LINKS) || \
    defined(SMC_PARTIAL_DIRECT_GATE)  || defined(SMC_PARTIAL_SAFE_CONTROL) || \
    defined(SMC_PARTIAL_SAFE_FRAMEFULL) || defined(SMC_PARTIAL_SAFE_NO_TRAMP)
#error "An SMC_PARTIAL_SAFE sub-flag is set without SMC_PARTIAL_SAFE."
#endif
#endif

#endif /* GPSP_PROFILE_H */
