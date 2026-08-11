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

#ifndef MAIN_H
#define MAIN_H

#include <stdio.h>

#define TIMER_INACTIVE                0
#define TIMER_PRESCALE                1
#define TIMER_CASCADE                 2

#define TIMER_NO_IRQ                  0
#define TIMER_TRIGGER_IRQ             1

#define TIMER_DS_CHANNEL_NONE         0
#define TIMER_DS_CHANNEL_A            1
#define TIMER_DS_CHANNEL_B            2
#define TIMER_DS_CHANNEL_BOTH         3

typedef struct
{
  s32 count;
  u32 reload;
  u32 prescale;
  fixed8_24 frequency_step;
  u32 direct_sound_channels;
  u32 irq;
  u32 status;
} timer_type;

typedef enum
{
  no_frameskip = 0,
  auto_frameskip,
  auto_threshold_frameskip,
  fixed_interval_frameskip
} frameskip_type;

typedef enum
{
  auto_detect = 0,
  builtin_bios,
  official_bios
} bios_type;

typedef enum
{
  boot_game = 0,
  boot_bios
} boot_mode;

extern u32 gbc_update_count;

extern u32 frame_counter;
extern u32 cpu_ticks;
extern u32 execute_cycles;
extern u32 skip_next_frame;

extern u32 flush_ram_count;

/* Monotonic perf counters (see main.c). Frontends take deltas; nothing in
 * the core resets them. */
extern u32 flush_ram_total;
extern u32 flush_rom_total;
extern u32 flush_ram_full;   /* RAM flushes caused by cache exhaustion */
extern u32 flush_ram_smc;    /* ...by a CPU store into translated RAM */
extern u32 flush_ram_dma;    /* ...by a DMA into translated RAM */

/* ADR-0030: WHERE the SMC-triggering writes land (see main.c).  A flat,
 * exactly-indexed bucket per 256-byte page of IWRAM (32 KiB) and of the EWRAM
 * code window (256 KiB) -- 2.3 KiB of .bss, no hashing, no allocation. */
#define SMC_PROF_PAGE_SHIFT 8
#define SMC_PROF_IW_PAGES   (0x8000  >> SMC_PROF_PAGE_SHIFT)   /*  128 */
#define SMC_PROF_EW_PAGES   (0x40000 >> SMC_PROF_PAGE_SHIFT)   /* 1024 */
#define SMC_PROF_PAGES      (SMC_PROF_IW_PAGES + SMC_PROF_EW_PAGES)

extern u16 smc_prof_page[SMC_PROF_PAGES];
extern u32 smc_prof_hits_iw, smc_prof_hits_ew, smc_prof_hits_oth;
extern u32 smc_prof_lui_iwram, smc_prof_lui_ewram;

void smc_prof_note_iwram(u32 offset);
void smc_prof_note_ewram(u32 offset);
void smc_prof_note_store(u32 base, u32 off);   /* dynarec store stubs only */
void smc_prof_take(unsigned *hits, unsigned *pages, unsigned *iw, unsigned *ew,
                   unsigned *oth, unsigned *ovf,
                   unsigned *hot_addr, unsigned *hot_cnt,
                   unsigned *top_addr, unsigned *top_cnt, unsigned nhot);

/* Phase 5g diagnostic: which translated BLOCK covers the hot SMC address, why
 * the scanner extended it that far, and which GBA PC keeps writing there.
 * See main.c.  Fixed-size .bss, no behaviour change. */
/* The watch is a 256-byte PAGE, not a single address: `EVT smc_addr` reports
 * the hot page (`hot=03007d00`) and the exact addresses inside it drift with
 * the game state (0x03007D3C/48/58 at boot, 0x03007D90 in a link session), so
 * pinning one address would miss the very runs we care about. */
#define SMC_BLK_WATCH_DEFAULT 0x03007D00
#define SMC_BLK_WATCH_SIZE  0x100
#define SMC_BLK_SLOTS   4    /* distinct covering blocks kept per window */
#define SMC_WR_SLOTS    4    /* distinct writer PCs kept per window */
#define SMC_BLK_SNAP   16    /* words snapshotted at the head / around watch */
#define SMC_BLK_BYTES  64    /* per-slot copy of the block's own bytes */

/* Why scan_block stopped extending the block. */
#define SMC_SCAN_UNCOND   1  /* unconditional branch, nothing jumps past it */
#define SMC_SCAN_MAXEXIT  2  /* MAX_EXITS block exits recorded */
#define SMC_SCAN_GATE     3  /* hit a translation gate target */
#define SMC_SCAN_MAXSIZE  4  /* MAX_BLOCK_SIZE instructions */
#define SMC_SCAN_RAMEND   5  /* ran into the 0x3007FF0 / 0x203FFFF0 clamp */

extern u32 smc_blk_watch;
extern u32 smc_blk_xlat;

/* What a flush actually COSTS.  ADR-0029 priced it indirectly (worst frame
 * minus window mean, divided by the flush count) and got ~10 us; that was an
 * upper bound that assumed the whole spike was flushes.  These measure it
 * directly: `smc_flush_us` is time spent inside flush_translation_cache_ram(),
 * `smc_flush_bytes` the tag memset it does, `smc_flush_wide` how often it took
 * the whole-region else-branch (32 KiB IWRAM / 256 KiB EWRAM, which fires when
 * exactly one instruction was translated since the last flush).  The clock is
 * supplied by the frontend so the core stays platform-agnostic; NULL leaves
 * smc_flush_us at 0 and costs one predictable branch per flush. */
extern u32 (*smc_prof_clock)(void);
extern u32 smc_flush_us;
extern u32 smc_flush_bytes;
extern u32 smc_flush_wide;

void smc_blk_note_block(u32 start_pc, u32 end_pc, u32 thumb, u32 reason);
void smc_blk_note_writer(u32 gba_addr, u32 pc);

/* Did the store that just fired the SMC trap actually CHANGE the code?
 * A store of the byte that was already there leaves the translation valid, so
 * flushing for it is pure waste.  At each translation of a block covering the
 * watched page we keep a copy of the block's own bytes; this compares the
 * (already-committed) memory against that copy and buckets the event:
 *   same    the bytes are byte-identical -- the flush bought nothing;
 *   diff    the code really changed -- the flush was necessary;
 *   unknown the address is outside every tracked block (not on the watched
 *           page, or the block is longer than SMC_BLK_BYTES). */
void smc_blk_check_silent(u32 gba_addr);
void smc_blk_take(unsigned *watch, unsigned *xlat, unsigned *ovf,
                  unsigned *start, unsigned *end, unsigned *mode,
                  unsigned *reason, unsigned *cnt,
                  unsigned *wpc, unsigned *waddr, unsigned *wcnt,
                  unsigned *wovf,
                  unsigned *head, unsigned *ctx, unsigned *snap,
                  unsigned *fus, unsigned *fbytes, unsigned *fwide,
                  unsigned *same, unsigned *diff, unsigned *unkn);

/* ---------------------------------------------------------------------------
 * Phase 5h diagnostic: BRACKET THE EMULATED FRAME BY PHASE.
 *
 * ADR-0031 closed the SMC line -- the flush storm is genuine self-modifying
 * code and costs ~1 us an event, 0.13 % of core time -- so the field's
 * `core=11390/21263` is still unattributed.  Attribute it by measuring
 * retro_run's phases directly instead of inferring them from counters.
 *
 * The partition is EXACT by construction: every microsecond of retro_run
 * lands in exactly one bucket, and `tot` must equal fe_host's own `core=`.
 *
 *   tot   the whole retro_run call
 *   |- emu  execute_arm{,_translate}()        the emulated frame
 *   |  |- vid   sum of update_scanline()      per-scanline rendering
 *   |  |- amix  sum of render_gbc_sound()     GBC/PSG mixing (13 call sites)
 *   |  |- dsnd  sum of sound_timer()          direct-sound FIFO -> buffer
 *   |  |- jit   sum of translate_block_*()    dynarec compilation
 *   |  \- cpu   RESIDUE: translated-code execution, block lookup + icache
 *   |            sync, memory stubs, timer/serial/DMA/IRQ bookkeeping,
 *   |            backup-memory emulation
 *   |- blt  video_run()                       post-process + video_cb
 *   |- aout audio_run()                       resample + audio_batch_cb
 *   |- rfu  rfu_frame_update()                emulated wireless adapter
 *   \- oth  RESIDUE: input poll, frameskip policy, option re-check, and the
 *            outer probes themselves
 *
 * COST CONTROL, because the probe is not free.  One clock read is ~1 us on
 * Allegrex (sceKernelGetSystemTimeLow), so the probe count per frame IS the
 * budget, and it is levelled:
 *   0  off                                    0 reads/frame (every non-PSP
 *                                             frontend leaves it here)
 *   1  the coarse split, no inner brackets   ~10 reads/frame
 *   2  + vid (160 calls), amix, jit          ~350 reads/frame
 *   3  + dsnd (~450 calls)                  ~1250 reads/frame
 * Level 2 is the default.  Level 1 exists so a run can A/B the probe cost
 * against a nearly-free build instead of asserting it, and `rd=` reports the
 * reads actually taken, so `rd x clk` prices this line in its own units.
 *
 * Backup memory (flash/EEPROM/SRAM) is COUNTED, not timed: the game polls
 * flash thousands of times a frame and a save burst writes 131072 bytes one
 * call at a time, so a clock read per call would cost orders of magnitude
 * more than the path it measures and would itself change the frame time.
 * `bk=` is the call count; its time is inside `cpu`.
 * ------------------------------------------------------------------------- */
#define CORE_PHASE_OFF     0
#define CORE_PHASE_COARSE  1
#define CORE_PHASE_FINE    2
#define CORE_PHASE_DEEP    3

/* Phase slots, shared with the frontend (fe_host.h FE_CPH_*). */
#define CPH_TOT     0
#define CPH_CPU     1
#define CPH_VID     2
#define CPH_BLT     3
#define CPH_AMIX    4
#define CPH_AOUT    5
#define CPH_DSND    6
#define CPH_JIT     7
#define CPH_RFU     8
#define CPH_OTH     9
#define CPH_NPHASE 10

extern u32 core_phase_lvl;      /* never nonzero unless smc_prof_clock is set */
extern u32 core_phase_clk_ns;   /* measured cost of ONE clock read, in ns */

/* Frame-local accumulators, microseconds; cleared by core_phase_frame_begin */
extern u32 cph_vid, cph_amix, cph_dsnd, cph_jit;
extern u32 cph_reads;                     /* clock reads taken this frame */
extern u32 cph_bkr, cph_bkw;              /* backup-memory calls this frame */
/* ADR-0051: rfu_transfer() is called SYNCHRONOUSLY from write_siocnt(), i.e.
 * from write_io_register16(), i.e. from translated code -- so every RFU word
 * the game exchanges is charged to the `cpu` residue, NOT to the `rfu` phase.
 * `rfu` only ever measured rfu_frame_update(), the once-per-frame tick, which
 * is why it reads ~8 us while the adapter is doing real work.  Unlike backup
 * memory (thousands of calls a frame, counted only), this is one call per SIO
 * word, so it is cheap enough to TIME as well as count. */
extern u32 cph_rfux, cph_rfut;            /* rfu_transfer calls / us this frame */
extern u32 cph_ugba, cph_dman, cph_stmr;  /* loop / DMA / sound-timer calls */

/* OUTER tier (retro_run only).  Returns 0 -- never a timestamp -- when the
 * active level is below `lvl`, so an idle bracket reduces to `acc += 0 - 0`.
 * The outer brackets deliberately do NOT nest-guard: they ENCLOSE the inner
 * ones, and `cpu`/`oth` are their residues. */
u32 core_phase_now(u32 lvl);

/* INNER tier (vid / amix / dsnd / jit), nesting-safe.  Only the OUTERMOST
 * active inner bracket is timed; a nested one returns 0 and adds nothing, so
 * its time stays with the bracket that encloses it and the partition holds.
 * This is not hypothetical: translate_block_##type() calls
 * block_lookup_translate_##type() to patch its external exits
 * (cpu_threaded.c), which re-enters translate_block_##type -- an unguarded
 * bracket double-counts every nested compile, and the first cut of this
 * profile did exactly that until `neg` reported a negative `cpu` residue on
 * 8 frames of a boot window.  The direct-sound drain reaches
 * render_gbc_sound the same way (sound_timer -> dma_transfer ->
 * write_io_register##tfsize -> render_gbc_sound), so at level DEEP `dsnd`
 * legitimately owns that mixing time and `amix` must not claim it twice.
 * The depth is reset every frame, so a non-local exit cannot leak it. */
u32 core_phase_enter(u32 lvl);
void core_phase_leave(u32 lvl, u32 *acc, u32 t0);
void core_phase_set_level(unsigned lvl);  /* `unsigned`, not u32:
                                          * the frontend declares it without
                                          * the core's headers (main_psp.c) */
void core_phase_frame_begin(void);
void core_phase_frame_end(u32 tot, u32 emu, u32 blt, u32 aout, u32 rfu);
void core_phase_take(unsigned *lvl, unsigned *clk_ns, unsigned *frames,
                     unsigned *reads, unsigned *mean, unsigned *max,
                     unsigned *worst, unsigned nph,
                     unsigned *bkr, unsigned *bkw, unsigned *wbkr,
                     unsigned *wbkw, unsigned *ugba, unsigned *dman,
                     unsigned *stmr, unsigned *negc, unsigned *nego,
                     unsigned *rfux, unsigned *rfut,
                     unsigned *wrfux, unsigned *wrfut);

extern char main_path[512];

u16 rand_gen();
void rand_seed(u32 data);

#define cycles_to_run(c) ((c) & 0x7FFF)
#define completed_frame(c) ((c) & 0x80000000)
u32 function_cc update_gba(int remaining_cycles);
void reset_gba(void);

void init_main(void);

void game_name_ext(char *src, char *buffer, char *extension);

bool main_check_savestate(const u8 *src);
unsigned main_write_savestate(u8* ptr);
bool main_read_savestate(const u8 *src);

extern u32 num_skipped_frames;
extern int dynarec_enable;
extern boot_mode selected_boot_mode;
extern int sprite_limit;
extern u32 netplay_num_clients, netplay_client_id;

#ifdef TRACE_REGISTERS
void print_regs(void);
#endif

#ifdef TRACE_EVENTS
  #define trace_update_gba(remcyc)   \
    printf("update_gba: %d remaining cycles\n", (remcyc));
#else  /* TRACE_EVENTS */
  #define trace_update_gba(x)
#endif

#endif


