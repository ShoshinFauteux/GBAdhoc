/* reg_slots.h -- THE registry of reg[32..63] (the register block's upper half).
 *
 * reg[0..31] is the CPU state and the dynarec's spill slots (cpu.h's enum,
 * mips_stub.S / x86_stub.S .equ lists, fixed since upstream).  reg[32..63]
 * is "machine defined": the ARM backends keep function pointers in
 * REG_USERDEF + 0..14, and on MIPS / x86 several features each took a slot.
 * Two of them once took the same one (SCHED_HOT's cph_stmr and the ME write
 * maps' OAM_WMAP, both reg[60]) and nothing failed: each feature's slot
 * number lived in its own header.
 *
 * So every MIPS / x86 user of reg[32..63] takes its index from HERE, and
 * REG_SLOT_CLAIMS lists every claim.  Any C file that includes cpu.h compiles
 * reg_slot_registry_check(): one case range per claim, so two claims on one
 * word are a duplicate case value -- a compile error, in every build, on every
 * architecture.  A new slot user: add an RSLOT_ define AND a claim line, and
 * name the slot by RSLOT_ (never a raw number) in C and in the .S files.
 *
 * The ARM backends' REG_USERDEF + 0..14 (32..46) are not claims here: no
 * MIPS / x86 feature below runs on ARM (SCHED_HOT, SPIN_SKIP and DRPROF_HW are
 * MIPS-only); the ME write maps (60, 61) and REG_IO_CYCLES (62) and
 * PAL_UPDATED (63) are above that range.
 *
 * #defines only (included by mips_stub.S and x86_stub.S), except the
 * C-only check at the bottom. */
#ifndef REG_SLOTS_H
#define REG_SLOTS_H

#define RSLOT_FIRST            32
#define RSLOT_END              64   /* reg[] is 64 words */

/* SCHED_HOT (sched_hot.h): the scheduler's per-call state, MIPS only. */
#define RSLOT_VIDEO_COUNT      32
#define RSLOT_EXECUTE_CYCLES   33
#define RSLOT_CPU_TICKS        34
#define RSLOT_CPH_UGBA         35
#define RSLOT_HOP_ACTIVE       36
#define RSLOT_SCHED_FAST       37
#define RSLOT_SERIAL_MODE      38
#define RSLOT_SERIAL_IRQ       39
/* DRPROF_HW (drprof.h DRPH_IDX): the hardware profiler's phase word. */
#define RSLOT_DRPH             40
/* SPIN_SKIP (cpu.h REG_SPIN_DESC): a spin loop's descriptor. */
#define RSLOT_SPIN_DESC        41
/* SCHED_HOT, continued. */
#define RSLOT_TIMER_LAZY       42
#define RSLOT_CORE_PHASE_LVL   43
#define RSLOT_TIMER            44   /* timer_type timer[4]: 16 words, 44..59 */
#define RSLOT_TIMER_WORDS      16
/* ME_MIDFRAME_LOG write maps (cpu.h OAM_WMAP / PAL_WMAP). */
#define RSLOT_OAM_WMAP         60
#define RSLOT_PAL_WMAP         61
/* cpu.h REG_IO_CYCLES: the remaining slice cycles at an I/O access. */
#define RSLOT_IO_CYCLES        62
/* cpu.h PAL_UPDATED: the palette's twin of OAM_UPDATED. */
#define RSLOT_PAL_UPDATED      63

/* Every claim: X(name, first word, words). */
#define REG_SLOT_CLAIMS(X)                                \
  X(VIDEO_COUNT,     RSLOT_VIDEO_COUNT,     1)            \
  X(EXECUTE_CYCLES,  RSLOT_EXECUTE_CYCLES,  1)            \
  X(CPU_TICKS,       RSLOT_CPU_TICKS,       1)            \
  X(CPH_UGBA,        RSLOT_CPH_UGBA,        1)            \
  X(HOP_ACTIVE,      RSLOT_HOP_ACTIVE,      1)            \
  X(SCHED_FAST,      RSLOT_SCHED_FAST,      1)            \
  X(SERIAL_MODE,     RSLOT_SERIAL_MODE,     1)            \
  X(SERIAL_IRQ,      RSLOT_SERIAL_IRQ,      1)            \
  X(DRPH,            RSLOT_DRPH,            1)            \
  X(SPIN_DESC,       RSLOT_SPIN_DESC,       1)            \
  X(TIMER_LAZY,      RSLOT_TIMER_LAZY,      1)            \
  X(CORE_PHASE_LVL,  RSLOT_CORE_PHASE_LVL,  1)            \
  X(TIMER,           RSLOT_TIMER,           RSLOT_TIMER_WORDS) \
  X(OAM_WMAP,        RSLOT_OAM_WMAP,        1)            \
  X(PAL_WMAP,        RSLOT_PAL_WMAP,        1)            \
  X(IO_CYCLES,       RSLOT_IO_CYCLES,       1)            \
  X(PAL_UPDATED,     RSLOT_PAL_UPDATED,     1)

#ifndef __ASSEMBLER__
/* Each claim inside reg[32..63]... */
#define REG_SLOT_IN_RANGE(n, first, words)                                    \
  typedef char reg_slot_in_range_##n[((first) >= RSLOT_FIRST &&               \
                                      (first) + (words) <= RSLOT_END) ? 1 : -1];
REG_SLOT_CLAIMS(REG_SLOT_IN_RANGE)
#undef REG_SLOT_IN_RANGE

/* ...and no word claimed twice: overlapping case ranges do not compile
 * (GCC / Clang "duplicate (or overlapping) case value").  Never called; an
 * unused static inline emits no code. */
#define REG_SLOT_CASE(n, first, words) case (first) ... ((first) + (words) - 1):
static inline int reg_slot_registry_check(int i)
{
  switch (i)
  {
    REG_SLOT_CLAIMS(REG_SLOT_CASE)
      return 1;
  }
  return 0;
}
#undef REG_SLOT_CASE
#endif /* !__ASSEMBLER__ */

#endif /* REG_SLOTS_H */
