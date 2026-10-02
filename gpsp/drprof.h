/* drprof.h -- dynarec profiling hooks (docs/DYNAREC-PROFILE.md).
 *
 * TWO SWITCHES, BOTH OFF BY DEFAULT, NEITHER LEGAL IN A RELEASE:
 *
 *   DRPROF_TWIN  the host "twin": the PSP's MIPS translator built for Linux
 *                mipsel (platform=drprof-mipsel) and run under qemu with the
 *                tools/drprof TCG plugin.  Adds, at TRANSLATION time only:
 *                  - a class map: one u16 per emitted MIPS word recording where
 *                    each guest instruction's code starts and what it is
 *                    (op class, S bit, flags actually generated, condition,
 *                    ARM/Thumb), plus pseudo-starts for block prologue, cycle
 *                    updates and block tails;
 *                  - zone markers around translation and cache flushes (empty
 *                    noinline calls the plugin watches).
 *                The EMITTED CODE IS BYTE-IDENTICAL with and without it: the
 *                map is written beside the cache, never into it.  The plugin
 *                counts guest-side MIPS instructions only, so the marker
 *                calls do not distort the translated-code numbers.
 *
 *   DRPROF_HW    the hardware bench (psp/, GBADHOC-DRPROF): a phase word the
 *                asm stubs publish around every C call from translated code,
 *                read by a sampling alarm.  One store before and one after each
 *                cfncall; the emitted memory-stub fast paths are NOT touched.
 *
 * gpsp_profile.h refuses either in a release profile. */
#ifndef DRPROF_H
#define DRPROF_H

/* ---- guest instruction classes (u16 map entries) ------------------------ */
/* bits 0-5 op class, bit 6 writes flags (S bit / flag-setting Thumb op),
 * bit 7 flag code generated (liveness kept it), bit 8 conditional (ARM cond
 * != AL), bit 9 Thumb, bit 10 writes the PC.  0 = not a start. */
enum
{
  DRC_NONE = 0,
  DRC_ALU,        /* data processing with a result                       */
  DRC_MOV,        /* mov / mvn                                           */
  DRC_CMP,        /* tst / teq / cmp / cmn                               */
  DRC_MUL,        /* mul / mla / long multiplies                         */
  DRC_PSR,        /* mrs / msr                                           */
  DRC_LOAD,       /* ldr/ldrb/ldrh/ldrsb/ldrsh, base not SP or PC        */
  DRC_LOAD_SP,    /* ... base is SP (stack: statically "probably IWRAM")  */
  DRC_LOAD_LIT,   /* ... base is PC (literal pool: address is static)     */
  DRC_STORE,      /* str/strb/strh, base not SP                          */
  DRC_STORE_SP,   /* ... base is SP                                      */
  DRC_LDM,        /* ldm / pop                                           */
  DRC_STM,        /* stm / push                                          */
  DRC_B,          /* b / bcond                                           */
  DRC_BL,         /* bl (ARM) / either half of the Thumb bl pair         */
  DRC_BX,         /* bx                                                  */
  DRC_SWI,        /* swi                                                 */
  DRC_SWP,        /* swp                                                 */
  DRC_OTHER,      /* coprocessor / undefined                             */
  DRC_NCLASS,
  /* pseudo-starts: emitted code that belongs to no guest instruction */
  DRC_PROLOGUE = 60,
  DRC_CYCLE_UPD = 61,
  DRC_TAIL = 62,
  DRC_CHEAT = 63
};
#define DRC_F_SBIT   0x0040
#define DRC_F_FLAGS  0x0080
#define DRC_F_COND   0x0100
#define DRC_F_THUMB  0x0200
#define DRC_F_PCW    0x0400
#define DRC_F_SPBASE 0x0800   /* ldm/stm/push/pop based on SP */

/* zones (plugin attributes everything executed inside the innermost one) */
/* DRZ_TWIN: the twin's own bookkeeping (class-map clears); never counted */
enum { DRZ_XLAT = 1, DRZ_FLUSH = 2, DRZ_TWIN = 3 };

/* marks (frame / scene boundaries for the plugin) */
enum { DRM_FRAME = 1, DRM_SCENE = 2, DRM_START = 3, DRM_STOP = 4 };

#ifdef DRPROF_TWIN
#include "common.h"
extern u16 *drprof_rom_map;           /* one entry per rom-cache word */
extern u16 *drprof_ram_map;           /* one entry per ram-cache word */
extern u8 *drprof_stub_end;           /* end of the emitter's handler stubs */
/* Markers are EMPTY noinline functions: the caller publishes the argument in
 * memory first, the plugin reads it when it sees the marker's first
 * instruction execute (so no register access is needed in the plugin). */
extern volatile u32 drprof_zone_arg;
void drprof_zone(void);
u16 drprof_class_arm(u32 opcode, u32 flag_status);
u16 drprof_class_thumb(u32 opcode, u32 flag_status);
void drprof_note(u8 *host, u16 cls);
void drprof_forget_ram(void);
void drprof_forget_rom(u8 *from);
#define DRPROF_ZONE_ENTER(z)   do { drprof_zone_arg = ((u32)(z) << 1) | 1; drprof_zone(); } while (0)
#define DRPROF_ZONE_LEAVE(z)   do { drprof_zone_arg = ((u32)(z) << 1); drprof_zone(); } while (0)
#define DRPROF_NOTE(ptr, cls) drprof_note((ptr), (cls))
/* The guest pc (| 1 for Thumb) of the translation in progress, 0 outside one.
 * Saved and restored around each translate_block_* call (they nest), so the
 * coherency checker (tools/jitcoh) can name the guest block every emitter
 * store belongs to by watching the stores to this word. */
extern volatile u32 drprof_xlat_pc;
#define DRPROF_XLAT_PC_PUSH(key) u32 drprof_xlat_saved_ = drprof_xlat_pc; drprof_xlat_pc = (key)
#define DRPROF_XLAT_PC_POP()     drprof_xlat_pc = drprof_xlat_saved_
#else
#define DRPROF_ZONE_ENTER(z) do { } while (0)
#define DRPROF_ZONE_LEAVE(z) do { } while (0)
#define DRPROF_NOTE(ptr, cls) do { } while (0)
#define DRPROF_XLAT_PC_PUSH(key) do { } while (0)
#define DRPROF_XLAT_PC_POP()     do { } while (0)
#endif

/* ---- DRPROF_HW: the hardware bench's phase word ---------------------------
 * reg[DRPH_IDX] names what the emulation thread is doing; a sampler thread
 * (psp/main_psp.c) reads it at ~1 kHz.  reg[32..63] is REG_USERDEF space that
 * the MIPS dynarec never uses (the ARM one does); shash hashes reg[0..16].
 * Written by: mips_stub.S cfncall (C calls from translated code) and
 * DRPH_SCOPE at the top of the C functions that matter, which saves the
 * caller's phase and restores it on every return path (cleanup attribute).
 * Translated code itself, and the emitter's memory-stub fast paths, write
 * nothing: they are phase 0. */
#define DRPH_IDX 40
enum
{
  DRPH_JIT = 0,      /* translated code + inline stub fast paths + asm glue */
  DRPH_UPD = 1,      /* update_gba (event scheduler) self                   */
  DRPH_DISP = 2,     /* block lookup (C dispatch)                           */
  DRPH_FLUSH = 3,    /* cache flush / SMC handling                          */
  DRPH_CMISC = 4,    /* mode switch, cpsr/spsr bodies, cheats               */
  DRPH_XLAT = 6,     /* translation                                         */
  DRPH_MEMC = 7,     /* C memory slow paths (io, backup, eeprom, gpio, rom) */
  DRPH_SOUND = 8,
  DRPH_DMA = 9,
  DRPH_SERIAL = 10,
  DRPH_VIDEO = 11,   /* update_scanline (+ the renderer when the ME is off) */
  DRPH_IRQ = 12,
  DRPH_RETRO = 13,   /* retro_run outside the CPU loop (audio/video out)    */
  DRPH_OUTSIDE = 15, /* frontend                                            */
  DRPH_N = 16
};
#ifdef DRPROF_HW
static inline u32 drph_set(u32 p)
{
  u32 o = reg[DRPH_IDX];
  reg[DRPH_IDX] = p;
  return o;
}
static inline void drph_restore(u32 *o) { reg[DRPH_IDX] = *o; }
#define DRPH_SCOPE(p) \
  u32 drph_old_ __attribute__((cleanup(drph_restore), unused)) = drph_set(p)
#define DRPH_SET(p) (reg[DRPH_IDX] = (p))
#else
#define DRPH_SCOPE(p) extern int drph_unused_decl_ __attribute__((unused)) /* no code */
#define DRPH_SET(p) do { } while (0)
#endif

#endif /* DRPROF_H */
