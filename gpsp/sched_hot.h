/* sched_hot.h -- SCHED_HOT: where the scheduler's per-call state lives.
 *
 * update_gba runs 440-700 times a frame (every H-blank edge, every line end,
 * every sound-timer overflow ends a slice).  Hardware profile (claude/hwprof,
 * report2): 3.3 us a call, with no hot spot in the code -- the state it reads
 * (timer[], video_count, cpu_ticks, execute_cycles, the serial and hop flags,
 * the phase counters) sat on ~9 separate 64-byte lines of .data/.bss, each
 * cold again after the translated code ran (a D-cache miss is 222 ns).
 *
 * On MIPS (the PSP and its twin) that state is placed INSIDE the register
 * block, in reg[] slots no MIPS code uses (cpu.h: the MIPS backend uses
 * nothing between REG_SAVE6 and the slots named below).  update_gba already
 * touches reg[] lines 2 and 3 on every call (REG_SPIN_DESC, REG_IO_CYCLES),
 * so the scheduler state now costs no line of its own.  mips/mips_stub.S
 * defines each variable as a label at its slot (assembler-checked against the
 * indices here); C keeps declaring them by name, so every user is unchanged.
 * The register block is .data.gbahot.b (HOT_DATA_GUARD), so nothing moves:
 * reg[] is 64 words as before.
 *
 * Not guest state: reg[19..63] is never saved (cpu_write_savestate writes
 * REG_ARCH_COUNT words), hashed (twin oracle, STATE_HASH_TRACE: reg[0..16])
 * or cleared by init_cpu (reg[0..31]).  The savestate keys are unchanged.
 *
 * Other architectures (x86, ARM, the interpreter) define the variables in C
 * as before (main.c, serial.c).
 *
 * This header is included by mips_stub.S: #defines only. */
#ifndef SCHED_HOT_H
#define SCHED_HOT_H

#include "reg_slots.h"

#if defined(MIPS_ARCH) && defined(HAVE_DYNAREC)
#define SCHED_HOT_IN_REG 1
#endif

/* reg[] slots (u32 words), from the registry (reg_slots.h): every word of
 * reg[32..63] is claimed there.  cph_stmr (the sound-timer call counter) is
 * NOT in the block: its old slot 60 is the ME write maps' OAM_WMAP, so it
 * stays a C variable in main.c. */
#define SCHED_SLOT_VIDEO_COUNT     RSLOT_VIDEO_COUNT      /* s32 video_count       */
#define SCHED_SLOT_EXECUTE_CYCLES  RSLOT_EXECUTE_CYCLES   /* u32 execute_cycles    */
#define SCHED_SLOT_CPU_TICKS       RSLOT_CPU_TICKS        /* u32 cpu_ticks         */
#define SCHED_SLOT_CPH_UGBA        RSLOT_CPH_UGBA         /* u32 cph_ugba          */
#define SCHED_SLOT_HOP_ACTIVE      RSLOT_HOP_ACTIVE       /* u32 sched_hop_active  */
#define SCHED_SLOT_SCHED_FAST      RSLOT_SCHED_FAST       /* u32 sched_fast        */
#define SCHED_SLOT_SERIAL_MODE     RSLOT_SERIAL_MODE      /* int serial_mode       */
#define SCHED_SLOT_SERIAL_IRQ      RSLOT_SERIAL_IRQ       /* u32 serial_irq_cycles */
#define SCHED_SLOT_TIMER_LAZY      RSLOT_TIMER_LAZY       /* u32 timer_lazy        */
#define SCHED_SLOT_CORE_PHASE_LVL  RSLOT_CORE_PHASE_LVL   /* u32 core_phase_lvl    */
#define SCHED_SLOT_TIMER           RSLOT_TIMER            /* timer_type timer[4]   */
#define SCHED_TIMER_BYTES          16   /* sizeof(timer_type): 4 timers = RSLOT_TIMER_WORDS */

/* Initial values (also used by the C definitions on other architectures).
 * SCHED_SERIAL_MODE_INIT must equal serial.h's SERIAL_MODE_AUTO (checked in
 * serial.c), CORE_PHASE_OFF is 0 (main.h). */
#define SCHED_SERIAL_MODE_INIT     6
#ifndef SCHED_FAST_DEFAULT
#define SCHED_FAST_DEFAULT 0
#endif
#ifndef TIMER_LAZY_DEFAULT
#define TIMER_LAZY_DEFAULT 0
#endif
#ifndef HALT_HOP_DEFAULT
#define HALT_HOP_DEFAULT 6
#endif

#endif /* SCHED_HOT_H */
