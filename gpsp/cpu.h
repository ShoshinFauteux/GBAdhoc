/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef CPU_H
#define CPU_H

#include <stdbool.h>
#include "gpsp_config.h"

// System mode and user mode are represented as the same here

typedef u32 cpu_mode_type;

// Bit 4 indicates privilege level
#define MODE_USER         0x00   // Non-privileged mode
#define MODE_SYSTEM       0x10   // Privileged modes
#define MODE_IRQ          0x11
#define MODE_FIQ          0x12
#define MODE_SUPERVISOR   0x13
#define MODE_ABORT        0x14
#define MODE_UNDEFINED    0x15
#define MODE_INVALID      0x16

// Discards privilege bit
#define REG_MODE(m) (reg_mode[(m) & 0xF])
#define REG_SPSR(m) (spsr[(m) & 0xF])
#define PRIVMODE(m) ((m) >> 4)

#define CPU_ACTIVE          0
#define CPU_HALT            1
#define CPU_STOP            2
#define CPU_DMA             3  /* CPU is idling due to DMA transfer */

/* HALT wakes on any enabled interrupt request (IE & IF), even when IRQ
 * delivery is masked by IME or CPSR.I. STOP deliberately keeps its existing
 * stricter wake path in check_and_raise_interrupts(). */
static inline bool cpu_irq_request_pending(u32 ie, u32 irq_flags)
{
  return (ie & irq_flags) != 0;
}

static inline bool cpu_irq_delivery_enabled(u32 ie, u32 irq_flags,
                                             u32 ime, u32 cpsr)
{
  return !(cpsr & 0x80) && ime && cpu_irq_request_pending(ie, irq_flags);
}

static inline bool cpu_halt_wake_requested(u32 halt_state, u32 ie,
                                            u32 irq_flags)
{
  return halt_state == CPU_HALT && cpu_irq_request_pending(ie, irq_flags);
}

typedef u8 cpu_alert_type;

#define CPU_ALERT_NONE         0
#define CPU_ALERT_HALT   (1 << 0)
#define CPU_ALERT_SMC    (1 << 1)
#define CPU_ALERT_IRQ    (1 << 2)

typedef u16 irq_type;

#define IRQ_NONE     0x0000
#define IRQ_VBLANK   0x0001
#define IRQ_HBLANK   0x0002
#define IRQ_VCOUNT   0x0004
#define IRQ_TIMER0   0x0008
#define IRQ_TIMER1   0x0010
#define IRQ_TIMER2   0x0020
#define IRQ_TIMER3   0x0040
#define IRQ_SERIAL   0x0080
#define IRQ_DMA0     0x0100
#define IRQ_DMA1     0x0200
#define IRQ_DMA2     0x0400
#define IRQ_DMA3     0x0800
#define IRQ_KEYPAD   0x1000
#define IRQ_GAMEPAK  0x2000

typedef enum
{
  // CPU status & registers
  REG_SP            = 13,
  REG_LR            = 14,
  REG_PC            = 15,
  REG_CPSR          = 16,
  CPU_MODE          = 17,
  CPU_HALT_STATE    = 18,
  REG_ARCH_COUNT    = 19,

  // This is saved separately
  REG_BUS_VALUE     = 19,

  // Dynarec signaling and spilling
  // (Not really part of the CPU state)
  REG_N_FLAG        = 20,
  REG_Z_FLAG        = 21,
  REG_C_FLAG        = 22,
  REG_V_FLAG        = 23,
  REG_SLEEP_CYCLES  = 24,
  OAM_UPDATED       = 25,
  REG_SAVE          = 26,
  REG_SAVE2         = 27,
  REG_SAVE3         = 28,
  REG_SAVE4         = 29,
  REG_SAVE5         = 30,
  REG_SAVE6         = 31,

  /* Machine defined storage */
  REG_USERDEF       = 32,

  /* Set by every palette store (C write_palette*, the MIPS and x86 store
   * stubs), cleared only by the ME_MIDFRAME_LOG capture (video_me_log.h).
   * The palette's twin of OAM_UPDATED.  The last slot: the ARM backends use
   * REG_USERDEF + 0..14, MIPS and x86 nothing above REG_SAVE6. */
  PAL_UPDATED       = 63,

  REG_MAX           = 64
} ext_reg_numbers;

extern u32 instruction_count;

void execute_arm(u32 cycles);
u32 check_and_raise_interrupts(void);
cpu_alert_type check_interrupt(void);
cpu_alert_type flag_interrupt(irq_type irq_raised);
void set_cpu_mode(cpu_mode_type new_mode);
#ifdef IRQ_INTEGRITY_CHECK
/* DIAGNOSTIC: IRQ entry/return register-integrity invariant (cpu_threaded.c) */
void irqchk_enter(u32 site);
void irqchk_exit(u32 old_mode, u32 address);
#define IRQCHK_ENTER(site) irqchk_enter(site)
#define IRQCHK_EXIT(old_mode, address) irqchk_exit(old_mode, address)
void irqchk_reset(void);
#define IRQCHK_RESET() irqchk_reset()
#else
#define IRQCHK_ENTER(site) do { } while (0)
#define IRQCHK_EXIT(old_mode, address) do { } while (0)
#define IRQCHK_RESET() do { } while (0)
#endif

u32 function_cc execute_load_u8(u32 address);
u32 function_cc execute_load_u16(u32 address);
u32 function_cc execute_load_u32(u32 address);
u32 function_cc execute_load_s8(u32 address);
u32 function_cc execute_load_s16(u32 address);
void function_cc execute_store_u8(u32 address, u32 source);
void function_cc execute_store_u16(u32 address, u32 source);
void function_cc execute_store_u32(u32 address, u32 source);
void function_cc execute_store_aligned_u32(u32 address, u32 source);
u32 execute_arm_translate(u32 cycles);
void init_translater(void);

bool cpu_check_savestate(const u8 *src);
unsigned cpu_write_savestate(u8* dst);
bool cpu_read_savestate(const u8 *src);

u8 function_cc *block_lookup_address_arm(u32 pc);
u8 function_cc *block_lookup_address_thumb(u32 pc);
u8 function_cc *block_lookup_address_dual(u32 pc);
bool translate_block_arm(u32 pc, bool ram_region);
bool translate_block_thumb(u32 pc, bool ram_region);

#if defined(MMAP_JIT_CACHE)
extern u8* rom_translation_cache;
extern u8* ram_translation_cache;
#elif defined(_3DS)
#define rom_translation_cache ((u8*)0x02000000 - ROM_TRANSLATION_CACHE_SIZE)
#define ram_translation_cache (rom_translation_cache - RAM_TRANSLATION_CACHE_SIZE)
extern u8* rom_translation_cache_ptr;
extern u8* ram_translation_cache_ptr;
#elif defined(VITA)
extern u8* rom_translation_cache;
extern u8* ram_translation_cache;
extern int sceBlock;
#elif defined(RUNTIME_JIT_CACHE)
/* Sized at startup (dynarec_select_translation_caches): the static SMALL
 * arrays in the stub file by default, or one heap block of the LARGE sizes
 * when the console has the memory.  Every size below must be read from these
 * variables, never from the *_TRANSLATION_CACHE_SIZE macros. */
extern u8 *rom_translation_cache;
extern u8 *ram_translation_cache;
extern u32 rom_translation_cache_size;
extern u32 ram_translation_cache_size;
#else
extern u8 rom_translation_cache[ROM_TRANSLATION_CACHE_SIZE];
extern u8 ram_translation_cache[RAM_TRANSLATION_CACHE_SIZE];
#endif
#if !defined(RUNTIME_JIT_CACHE)
#define rom_translation_cache_size ((u32)ROM_TRANSLATION_CACHE_SIZE)
#define ram_translation_cache_size ((u32)RAM_TRANSLATION_CACHE_SIZE)
#endif
extern u8 *rom_translation_ptr;
extern u8 *ram_translation_ptr;

/* Overridable from the root make as GATE_SLOTS=N.  Every core object must see
 * the same value: it sizes translation_gate_target_pc[] and smc_gate_last_hit[]
 * AND bounds the per-instruction scan in scan_block, so a mismatch would be a
 * silent buffer overrun rather than a link error. */
#ifndef MAX_TRANSLATION_GATES
#define MAX_TRANSLATION_GATES 8
#endif

#ifdef DISPATCH_CACHE
#ifndef DISPATCH_CACHE_BITS
#define DISPATCH_CACHE_BITS 9     /* 512 entries per ISA, 4 KiB per table */
#endif
void dispatch_cache_clear(void);
#endif

extern u32 idle_loop_target_pc;
extern u32 translation_gate_targets;
extern u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
void smc_gates_reset(void);   /* SMC_GATES builds: forget gate hit stamps */
void smc_gates_refresh_values(void); /* Savestate: rebase ranked observations */
#ifdef DMA_SMC_FLUSH
/* HBlank/VBlank DMAs that landed on translated code and forced a full RAM
 * flush from update_gba (main.c).  Monotonic; frontends declare it weak. */
extern u32 dma_smc_flushes;
#endif
#ifdef XLAT_DEPTH_PROBE
/* Soak/diagnostic: deepest nested block translation and lowest frame address
 * seen at translation entry since the last reset (0 = none yet).  Frontends
 * that must link without the flag declare these weak. */
extern u32 xlat_depth_max;
extern u32 xlat_sp_min;
void xlat_depth_probe_reset(void);
#endif

extern u32 rom_branch_hash[ROM_BRANCH_HASH_SIZE];

void flush_translation_cache_rom(void);
void flush_translation_cache_ram(void);
void flush_translation_cache_ram_smc(void);   /* dynarec stubs only */
void flush_translation_cache_ram_dma(void);   /* dynarec stubs only */
void dump_translation_cache(void);
void init_dynarec_caches(void);
void flush_dynarec_caches(void);
void init_emitter(bool);
void init_bios_hooks(void);

#if defined(RUNTIME_JIT_CACHE)
/* Translation-cache tier chosen at startup.  See cpu_threaded.c. */
enum
{
  JIT_CACHE_UNDECIDED = 0,
  JIT_CACHE_SMALL     = 1,   /* the static arrays (PSP-1000 budget)        */
  JIT_CACHE_LARGE     = 2    /* one heap block, 64 MiB consoles            */
};
extern u32 jit_cache_tier;          /* JIT_CACHE_*                          */
extern u32 jit_cache_force_small;   /* set by the frontend BEFORE retro_init */
extern const char *jit_cache_reason;
void dynarec_select_translation_caches(void);
#endif

extern u32 reg_mode[7][7];
extern u32 spsr[6];

extern const u32 cpu_modes[16];
extern const u32 cpsr_masks[4][2];
extern const u32 spsr_masks[4];

void init_cpu(void);
void move_reg();

extern const u8 bit_count[256];

#endif
