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

#include "common.h"
#include <ctype.h>

#ifndef SCHED_HOT_IN_REG
/* On MIPS these live in the register block (sched_hot.h, mips_stub.S). */
timer_type timer[4];
u32 cpu_ticks = 0;
u32 execute_cycles = 0;
s32 video_count = 0;
#else
/* Layout hold (docs/LAYOUT-PINNING.md): the bytes the variables that moved
 * into the register block took in this object's .bss and .data, so every
 * object linked after main.o keeps its D-cache sets (the pinned layout's
 * .bss: gbalink_hot, the hash tables, B's region).  Never read. */
#define SCHED_HOLD_BSS_MAIN (112 + 4 * 3 /* timer, video_count, execute_cycles, cpu_ticks */ \
                             + 4 * 2     /* core_phase_lvl, cph_ugba (not cph_stmr) */      \
                             + SCHED_HOLD_FAST + SCHED_HOLD_HOP)
#ifdef SCHED_FAST
#define SCHED_HOLD_FAST 4                /* sched_fast */
#else
#define SCHED_HOLD_FAST 0
#endif
#ifdef HALT_HOP
#define SCHED_HOLD_HOP 4                 /* hop_active */
#else
#define SCHED_HOLD_HOP 0
#endif
__attribute__((used)) static u8 sched_hot_hold_bss[SCHED_HOLD_BSS_MAIN];
#if defined(HALT_HOP) || (defined(TIMER_LAZY) && TIMER_LAZY_DEFAULT)
/* timer_lazy's nonzero default kept it in .data */
__attribute__((used)) static u32 sched_hot_hold_data = 1;
#elif defined(TIMER_LAZY)
__attribute__((used)) static u32 sched_hot_hold_bss2;
#endif
/* ...and in .text: init_main and main_write_savestate came out 32 B (one
 * function-alignment step) shorter each with the packed timers, which moved
 * instance A's cold text after main.o (pipe_latch, me_replay_lines: the
 * lock's CORE anchors) and B's before it.  64 B of zeros, never executed,
 * give the bytes back inside main.o. */
__asm__(".section .text.sched_hot_hold,\"ax\",@progbits\n"
        ".balign 32\n"
        "sched_hot_hold_text:\n"
        ".fill 16, 4, 0\n"
        ".previous\n");
#endif

u32 frame_counter = 0;

u32 last_frame = 0;
u32 flush_ram_count = 0;
u32 gbc_update_count = 0;
u32 oam_update_count = 0;

/* Monotonic performance counters (never reset; frontends take deltas).
 * `flush_ram_count` above is cleared every frame and has always been
 * debug-print-only, so it cannot be used to attribute a frame-time spike.
 * These can: a translation-cache flush throws away generated code and forces
 * re-translation, and on a swapping ROM buffer a page fault is a 32 KiB read
 * from the storage medium in the middle of emulation. */
u32 flush_ram_total = 0;
u32 flush_rom_total = 0;

/* ...and the RAM total split by CAUSE, because the two causes want opposite
 * fixes and the total cannot tell them apart:
 *   flush_ram_full  the RAM translation cache ran out of room
 *                   (translate_block_{arm,thumb}).  A bigger
 *                   RAM_TRANSLATION_CACHE_SIZE makes these rarer.
 *   flush_ram_smc   a CPU store from translated code landed on RAM holding
 *                   translated code (smc_write in the store stubs), so the
 *                   WHOLE RAM cache is thrown away.  A bigger cache does
 *                   nothing for these; only finer-grained invalidation would.
 *   flush_ram_dma   same, but the write came from a DMA transfer
 *                   (CPU_ALERT_SMC, raised only by dma_write_iwram /
 *                   dma_write_ewram, handled in write_io_epilogue).  Split
 *                   out because a DMA knows its whole destination range up
 *                   front and so is the one SMC source a range-invalidate
 *                   could plausibly address.
 * The remainder of flush_ram_total is the deliberate flush_dynarec_caches()
 * path (savestate load, cheat hook install). */
u32 flush_ram_full = 0;
u32 flush_ram_smc = 0;   /* CPU store from translated code */
u32 flush_ram_dma = 0;   /* DMA into tagged IWRAM/EWRAM */
/* Where the CPU store that triggered the flush landed, as a real GBA address.
 * smc_prof_note_store() already derives it and runs immediately before
 * flush_translation_cache_ram_smc() in the smc_write stub, so the partial
 * invalidation path can retire just the block covering it. */
u32 smc_last_write_addr = 0;
u32 flush_ram_partial = 0;   /* single-block retirements (SMC_PARTIAL_SAFE) */

/* ---------------------------------------------------------------------------
 * ADR-0030: SMC write-ADDRESS profile.
 *
 * flush_ram_smc says "the game wrote over translated code 400x/second"; it
 * cannot say WHERE, and the two possible answers want opposite work:
 *   - a handful of tightly clustered pages, hit hard  => genuine overlay
 *     swapping (the game really does copy code into IWRAM); only selective
 *     invalidation can help.
 *   - many scattered pages, each hit a few times      => ordinary DATA writes
 *     landing inside over-extended blocks (literal pools / embedded data that
 *     got tagged as code); tighter block termination would be far cheaper.
 *
 * So bucket every SMC-triggering write by 256-byte page and aggregate over the
 * heartbeat window.  IWRAM is 32 KiB (128 pages) and the EWRAM code window is
 * 256 KiB (1024 pages), which is small enough to index EXACTLY -- no hashing,
 * no collisions, no allocation: a flat u16[1152] (2.3 KiB of .bss).  The per
 * event cost is one bounds check and one u16 increment; the only loop runs
 * once per window, when the line is emitted.
 * ------------------------------------------------------------------------- */
u16 smc_prof_page[SMC_PROF_PAGES];
u32 smc_prof_hits_iw = 0;
u32 smc_prof_hits_ew = 0;
u32 smc_prof_hits_oth = 0;   /* region could not be attributed -- see below */

/* The two `lui` constants the MIPS store stubs load into reg_rv before adding
 * the masked region offset (mips_emit.h, emit_pmemst_stub).  Published BY the
 * emitter rather than re-derived here, so the discriminator below is exact by
 * construction.  Sentinels that no lui can produce (bits 15..0 are always 0)
 * keep an un-emitted build from ever matching. */
u32 smc_prof_lui_iwram = 1;
u32 smc_prof_lui_ewram = 2;

/* Second, finer tier: the EXACT hottest addresses.
 *
 * 256-byte pages answer "clustered or scattered"; they do NOT answer "is this
 * one instruction writing one variable, or a memcpy sweeping a range", and
 * that is the question that separates over-tagged data from a real overlay
 * copy.  A full exact-address histogram would be 32 K + 256 K buckets, so
 * instead this is Space-Saving over 8 slots: a hit increments its slot, a miss
 * on a full table evicts the coldest and inherits its count + 1.  A genuine
 * heavy hitter survives every eviction and its count is accurate to within the
 * evicted minimum; churn among one-off addresses shows up as a large `ovf`.
 * Cost is <=8 compares on an event that already costs a full cache flush. */
#define SMC_PROF_SLOTS  8
u32 smc_prof_slot_addr[SMC_PROF_SLOTS];
u32 smc_prof_slot_cnt[SMC_PROF_SLOTS];
u32 smc_prof_slot_ovf = 0;

static void smc_prof_exact(u32 gba_addr)
{
  unsigned i, min = 0;
  for (i = 0; i < SMC_PROF_SLOTS; i++)
  {
    if (smc_prof_slot_addr[i] == gba_addr && smc_prof_slot_cnt[i])
    {
      smc_prof_slot_cnt[i]++;
      return;
    }
    if (smc_prof_slot_cnt[i] < smc_prof_slot_cnt[min])
      min = i;
  }
  /* Miss: take over the coldest slot (its count is the error bound). */
  if (smc_prof_slot_cnt[min])
    smc_prof_slot_ovf++;
  smc_prof_slot_addr[min] = gba_addr;
  smc_prof_slot_cnt[min]++;
}

/* Saturating so a bucket can never wrap and lie (a window holds ~4k events,
 * but nothing in the core bounds a window's length). */
#define smc_prof_bump(idx) {                                                  \
  u16 *slot_ = &smc_prof_page[idx];                                           \
  if (*slot_ != 0xFFFF) (*slot_)++;                                           \
}

void smc_prof_note_iwram(u32 offset)
{
  if (!smc_blk_watch)
    return;                     /* the profile is off (a release) */
  smc_prof_hits_iw++;
  smc_prof_bump((offset & 0x7FFF) >> SMC_PROF_PAGE_SHIFT);
  smc_prof_exact(0x03000000 + (offset & 0x7FFF));
}

void smc_prof_note_ewram(u32 offset)
{
  if (!smc_blk_watch)
    return;
  smc_prof_hits_ew++;
  smc_prof_bump(SMC_PROF_IW_PAGES +
                ((offset & 0x3FFFF) >> SMC_PROF_PAGE_SHIFT));
  smc_prof_exact(0x02000000 + (offset & 0x3FFFF));
}

/* Called from the dynarec's smc_write stub (mips/mips_stub.S) with the two
 * registers that are still live at the SMC branch:
 *   base  = reg_rv  = lui_constant + masked_offset
 *   off   = reg_a0  = the masked region offset
 * so base - off recovers the lui constant, which names the region exactly.
 * Anything else is counted in `oth` rather than guessed at -- a nonzero `oth`
 * in the log means this assumption broke and the split must not be trusted. */
void smc_prof_note_store(u32 base, u32 off)
{
  u32 lui = base - off;
  if (lui == smc_prof_lui_iwram)
  {
    /* Phase 5g: reg[REG_PC] was stored by the smc_write stub from reg_a2,
     * which the store emitter loads with (pc + 4) for ARM and (pc + 2) for
     * Thumb -- i.e. the GBA PC of the instruction doing the write.  Only the
     * CPU-store path reaches here; DMA writes go through write_io_epilogue and
     * are already counted separately as flush_ram_dma. */
    smc_last_write_addr = 0x03000000 + (off & 0x7FFF);
    if (!smc_blk_watch)
      return;                   /* the address is all a release needs */
    smc_prof_note_iwram(off);
    smc_blk_note_writer(smc_last_write_addr, reg[REG_PC]);
    smc_blk_check_silent(smc_last_write_addr);
  }
  else if (lui == smc_prof_lui_ewram)
  {
    smc_last_write_addr = 0x02000000 + (off & 0x3FFFF);
    if (!smc_blk_watch)
      return;
    smc_prof_note_ewram(off);
    smc_blk_note_writer(smc_last_write_addr, reg[REG_PC]);
    smc_blk_check_silent(smc_last_write_addr);
  }
  else
    smc_prof_hits_oth++;
}

/* Snapshot + reset.  One pass over 1152 halfwords once per heartbeat window
 * (600 frames): counts the distinct pages touched and keeps the `nhot` hottest
 * by insertion, so nothing is ever sorted per event.  Addresses come back as
 * real GBA addresses (0x03xxxxxx / 0x02xxxxxx) for readability. */
void smc_prof_take(unsigned *hits, unsigned *pages, unsigned *iw, unsigned *ew,
                   unsigned *oth, unsigned *ovf,
                   unsigned *hot_addr, unsigned *hot_cnt,
                   unsigned *top_addr, unsigned *top_cnt, unsigned nhot)
{
  unsigned i, j, distinct = 0;

  for (i = 0; i < nhot; i++)
  {
    hot_addr[i] = hot_cnt[i] = 0;
    top_addr[i] = top_cnt[i] = 0;
  }

  for (i = 0; i < SMC_PROF_PAGES; i++)
  {
    unsigned c = smc_prof_page[i];
    if (!c)
      continue;
    distinct++;
    if (c <= hot_cnt[nhot - 1])
      continue;
    for (j = nhot - 1; j > 0 && c > hot_cnt[j - 1]; j--)
    {
      hot_cnt[j]  = hot_cnt[j - 1];
      hot_addr[j] = hot_addr[j - 1];
    }
    hot_cnt[j]  = c;
    hot_addr[j] = (i < SMC_PROF_IW_PAGES)
      ? (0x03000000 + (i << SMC_PROF_PAGE_SHIFT))
      : (0x02000000 + ((i - SMC_PROF_IW_PAGES) << SMC_PROF_PAGE_SHIFT));
  }

  /* Same insertion pass over the 8 exact-address slots. */
  for (i = 0; i < SMC_PROF_SLOTS; i++)
  {
    unsigned c = smc_prof_slot_cnt[i];
    if (!c || c <= top_cnt[nhot - 1])
      continue;
    for (j = nhot - 1; j > 0 && c > top_cnt[j - 1]; j--)
    {
      top_cnt[j]  = top_cnt[j - 1];
      top_addr[j] = top_addr[j - 1];
    }
    top_cnt[j]  = c;
    top_addr[j] = smc_prof_slot_addr[i];
  }

  *pages = distinct;
  *iw    = smc_prof_hits_iw;
  *ew    = smc_prof_hits_ew;
  *oth   = smc_prof_hits_oth;
  *ovf   = smc_prof_slot_ovf;
  *hits  = smc_prof_hits_iw + smc_prof_hits_ew + smc_prof_hits_oth;

  memset(smc_prof_page, 0, sizeof(smc_prof_page));
  memset(smc_prof_slot_addr, 0, sizeof(smc_prof_slot_addr));
  memset(smc_prof_slot_cnt, 0, sizeof(smc_prof_slot_cnt));
  smc_prof_hits_iw = smc_prof_hits_ew = smc_prof_hits_oth = 0;
  smc_prof_slot_ovf = 0;
}

/* ---------------------------------------------------------------------------
 * Phase 5g diagnostic: WHICH translated block covers the hot SMC address.
 *
 * ADR-0030 narrowed the flush storm to one halfword (0x03007D90, 99.6% of all
 * events).  That still leaves two very different explanations:
 *   - the game genuinely EXECUTES code at that address and also writes it, or
 *   - a block that starts somewhere else over-extends across it, so ordinary
 *     data writes land on a code tag that should never have been set.
 * Only the block that owns the tag can tell them apart, so record it: at the
 * end of every RAM block scan, if the watched address falls inside
 * [block_start_pc, block_end_pc) -- exactly the range scan_block tags -- keep
 * the start PC, the mode, the end the scanner settled on, and WHY it stopped.
 * Plus a snapshot of the instruction words at the block head and around the
 * watched address, which is what actually shows whether translation ran past
 * the end of real code.
 *
 * Everything here is fixed-size .bss and only writes on a path that is already
 * about to flush the entire translation cache.  No behaviour change. */
/* 0 = the whole SMC profile is off: the ADR-0030 counters, the exact-address
 * slots and this block/writer probe (smc_prof_note_* and smc_blk_note_block
 * test it first).  A release starts at 0: the profile's only reader is
 * `EVT smc_addr` / `EVT smc_block`, and fe_evt compiles out of a release,
 * while the work cost every SMC trap a page bump, an 8-slot scan and a
 * memcmp (twin, opt round 6: Doom II -0.53 % host instructions).  The code
 * stays linked, so the pinned warm tail (psp/layout/hot.ord) does not move.
 * Explicitly .data, as the nonzero default is in every other build.
 * -DGPSP_NO_SMC_PROFILE gives any build the release behaviour (the twin);
 * -DGPSP_SMC_PROFILE keeps the profile in a release. */
#if (defined(GPSP_PROFILE_RELEASE) && !defined(GPSP_SMC_PROFILE)) || \
    defined(GPSP_NO_SMC_PROFILE)
u32 smc_blk_watch __attribute__((section(".data"))) = 0;
#else
u32 smc_blk_watch = SMC_BLK_WATCH_DEFAULT;
#endif
u32 smc_blk_xlat  = 0;   /* RAM blocks translated since the last take */
/* Same count, but NEVER reset by a take — the on-screen profiler needs a
 * monotonic source it can difference itself.  This is the re-translation
 * churn a RAM cache wipe forces, and it is the one cost `smc_flush_us` does
 * NOT include (that times only the tag memset, which measured ~1% on
 * Unbound while the wipes ran at ~1700/s). */
u32 smc_blk_xlat_total = 0;

u32 (*smc_prof_clock)(void) = NULL;
u32 smc_flush_us    = 0;
u32 smc_flush_bytes = 0;
u32 smc_flush_wide  = 0;

static u32 blk_start[SMC_BLK_SLOTS];
static u32 blk_end[SMC_BLK_SLOTS];
static u32 blk_mode[SMC_BLK_SLOTS];    /* 0 = ARM, 1 = Thumb */
static u32 blk_reason[SMC_BLK_SLOTS];  /* SMC_SCAN_* */
static u32 blk_cnt[SMC_BLK_SLOTS];
static u32 blk_ovf = 0;

/* A copy of each tracked block's own bytes, taken at translation time, so an
 * SMC event can be classified as "the code really changed" or "the same bytes
 * were written again".  Only blocks no longer than SMC_BLK_BYTES are tracked;
 * everything else falls into `unkn`. */
static u8  blk_bytes[SMC_BLK_SLOTS][SMC_BLK_BYTES];
static u32 blk_blen[SMC_BLK_SLOTS];
static u32 smc_same = 0, smc_diff = 0, smc_unkn = 0;

static u32 wr_pc[SMC_WR_SLOTS];        /* GBA PC of the storing instruction */
static u32 wr_addr[SMC_WR_SLOTS];      /* last address that PC wrote */
static u32 wr_cnt[SMC_WR_SLOTS];
static u32 wr_ovf = 0;

static u32 blk_head[SMC_BLK_SNAP];     /* words from the block start PC */
static u32 blk_ctx[SMC_BLK_SNAP];      /* words centred on the watch address */
static u32 blk_snap_pc = 0;            /* 0 = no snapshot taken yet */

/* Read a word of GBA RAM as the CPU would see it.  IWRAM/EWRAM are double
 * buffers: the low half is the SMC tag mirror, the high half the real data
 * (iwram + 0x8000, ewram + 0). */
static u32 smc_blk_rd32(u32 addr)
{
  switch (addr >> 24)
  {
    case 3:  return *(u32 *)(iwram + 0x8000 + (addr & 0x7FFC));
    case 2:  return *(u32 *)(ewram + (addr & 0x3FFFC));
    default: return 0;
  }
}

/* Pointer to the block's real (non-tag) bytes, or NULL outside I/EWRAM. */
static const u8 *smc_blk_codeptr(u32 addr)
{
  switch (addr >> 24)
  {
    case 3:  return iwram + 0x8000 + (addr & 0x7FFF);
    case 2:  return ewram + (addr & 0x3FFFF);
    default: return NULL;
  }
}

static void smc_blk_keep_bytes(unsigned slot, u32 start_pc, u32 end_pc)
{
  const u8 *src = smc_blk_codeptr(start_pc);
  u32 len = end_pc - start_pc;
  if (!src || len == 0 || len > SMC_BLK_BYTES)
  {
    blk_blen[slot] = 0;
    return;
  }
  memcpy(blk_bytes[slot], src, len);
  blk_blen[slot] = len;
}

void smc_blk_check_silent(u32 gba_addr)
{
  unsigned i;
  for (i = 0; i < SMC_BLK_SLOTS; i++)
  {
    const u8 *cur;
    if (!blk_cnt[i] || !blk_blen[i])
      continue;
    if (gba_addr < blk_start[i] || gba_addr >= blk_start[i] + blk_blen[i])
      continue;
    cur = smc_blk_codeptr(blk_start[i]);
    /* The store has already been committed by the time the stub calls us
     * (it sits in the SMC branch's delay slot), so an equal compare means
     * the bytes written were the bytes already there. */
    if (cur && memcmp(cur, blk_bytes[i], blk_blen[i]) == 0)
      smc_same++;
    else
      smc_diff++;
    return;
  }
  smc_unkn++;
}

static void smc_blk_snapshot(u32 start_pc)
{
  unsigned i;
  u32 ctx0 = (smc_blk_watch & ~3U);
  for (i = 0; i < SMC_BLK_SNAP; i++)
  {
    blk_head[i] = smc_blk_rd32(start_pc + i * 4);
    blk_ctx[i]  = smc_blk_rd32(ctx0 + i * 4);
  }
  blk_snap_pc = start_pc | 1;   /* nonzero even for a 0 start PC */
}

/* Called once per RAM block translation, right after scan_block settled
 * block_end_pc.  `reason` is why the scan stopped (SMC_SCAN_*). */
void smc_blk_note_block(u32 start_pc, u32 end_pc, u32 thumb, u32 reason)
{
  unsigned i;
  smc_blk_xlat++;
  smc_blk_xlat_total++;
#ifdef SMC_PARTIAL_SAFE
  /* Hand the block's source extent to the tag table while it is still this
   * block's — external-exit resolution below can recursively translate other
   * blocks and would otherwise overwrite it. */
  ramtag_note_extent(start_pc, end_pc, thumb);
#endif
  /* Does the range scan_block just tagged overlap the watched page? */
  if (!smc_blk_watch ||
      start_pc >= smc_blk_watch + SMC_BLK_WATCH_SIZE || end_pc <= smc_blk_watch)
    return;

  for (i = 0; i < SMC_BLK_SLOTS; i++)
  {
    if (blk_cnt[i] && blk_start[i] == start_pc && blk_mode[i] == thumb)
    {
      blk_cnt[i]++;
      if (end_pc > blk_end[i])
        blk_end[i] = end_pc;
      blk_reason[i] = reason;
      smc_blk_keep_bytes(i, start_pc, end_pc);
      return;
    }
  }
  for (i = 0; i < SMC_BLK_SLOTS; i++)
  {
    if (!blk_cnt[i])
    {
      blk_start[i]  = start_pc;
      blk_end[i]    = end_pc;
      blk_mode[i]   = thumb;
      blk_reason[i] = reason;
      blk_cnt[i]    = 1;
      smc_blk_keep_bytes(i, start_pc, end_pc);
      if (!blk_snap_pc)
        smc_blk_snapshot(start_pc);
      return;
    }
  }
  blk_ovf++;
}

/* Called from the SMC store profiler with the address written and the GBA PC
 * the dynarec stub saved (reg[REG_PC] = storing instruction + 4 ARM / + 2
 * Thumb, see mips_emit.h arm_access_memory_store).  Only the watched address
 * is recorded, so this is a handful of compares on an already-expensive path. */
void smc_blk_note_writer(u32 gba_addr, u32 pc)
{
  unsigned i;
  if (!smc_blk_watch || gba_addr < smc_blk_watch ||
      gba_addr >= smc_blk_watch + SMC_BLK_WATCH_SIZE)
    return;
  for (i = 0; i < SMC_WR_SLOTS; i++)
  {
    if (wr_cnt[i] && wr_pc[i] == pc)
    {
      wr_cnt[i]++;
      wr_addr[i] = gba_addr;
      return;
    }
  }
  for (i = 0; i < SMC_WR_SLOTS; i++)
  {
    if (!wr_cnt[i])
    {
      wr_pc[i]   = pc;
      wr_addr[i] = gba_addr;
      wr_cnt[i]  = 1;
      return;
    }
  }
  wr_ovf++;
}

void smc_blk_take(unsigned *watch, unsigned *xlat, unsigned *ovf,
                  unsigned *start, unsigned *end, unsigned *mode,
                  unsigned *reason, unsigned *cnt,
                  unsigned *wpc, unsigned *waddr, unsigned *wcnt,
                  unsigned *wovf,
                  unsigned *head, unsigned *ctx, unsigned *snap,
                  unsigned *fus, unsigned *fbytes, unsigned *fwide,
                  unsigned *same, unsigned *diff, unsigned *unkn)
{
  unsigned i;

  *same = smc_same;
  *diff = smc_diff;
  *unkn = smc_unkn;
  smc_same = smc_diff = smc_unkn = 0;

  *fus    = smc_flush_us;
  *fbytes = smc_flush_bytes;
  *fwide  = smc_flush_wide;
  smc_flush_us = smc_flush_bytes = smc_flush_wide = 0;

  *watch = smc_blk_watch;
  *xlat  = smc_blk_xlat;
  *ovf   = blk_ovf;
  *wovf  = wr_ovf;
  *snap  = blk_snap_pc;

  for (i = 0; i < SMC_BLK_SLOTS; i++)
  {
    start[i]  = blk_start[i];
    end[i]    = blk_end[i];
    mode[i]   = blk_mode[i];
    reason[i] = blk_reason[i];
    cnt[i]    = blk_cnt[i];
  }
  for (i = 0; i < SMC_WR_SLOTS; i++)
  {
    wpc[i]   = wr_pc[i];
    waddr[i] = wr_addr[i];
    wcnt[i]  = wr_cnt[i];
  }
  for (i = 0; i < SMC_BLK_SNAP; i++)
  {
    head[i] = blk_head[i];
    ctx[i]  = blk_ctx[i];
  }

  /* Reset the per-window state.  The code snapshot is deliberately NOT reset:
   * it is a one-shot artifact and re-taking it every window would just log the
   * same words again. */
  memset(blk_start,  0, sizeof(blk_start));
  memset(blk_end,    0, sizeof(blk_end));
  memset(blk_mode,   0, sizeof(blk_mode));
  memset(blk_reason, 0, sizeof(blk_reason));
  memset(blk_cnt,    0, sizeof(blk_cnt));
  memset(blk_blen,   0, sizeof(blk_blen));
  memset(wr_pc,      0, sizeof(wr_pc));
  memset(wr_addr,    0, sizeof(wr_addr));
  memset(wr_cnt,     0, sizeof(wr_cnt));
  blk_ovf = wr_ovf = 0;
  smc_blk_xlat = 0;
}

/* ---------------------------------------------------------------------------
 * Phase 5h: the per-phase frame bracket (see main.h for the partition and the
 * probe budget).  Everything here is fixed-size .bss and runs once per frame
 * except core_phase_now(), which is one compare when the level is below the
 * probe's own and a clock read otherwise.
 * ------------------------------------------------------------------------- */
/* mips_stub.S starts it at 0 (sched_hot.h) */
typedef char core_phase_off_is_0[CORE_PHASE_OFF == 0 ? 1 : -1];
#ifndef SCHED_HOT_IN_REG
u32 core_phase_lvl    = CORE_PHASE_OFF;
u32 cph_ugba;
#endif
/* Never in the register block: reg[60] is OAM_WMAP (reg_slots.h). */
u32 cph_stmr;
u32 core_phase_clk_ns = 0;

u32 cph_vid, cph_amix, cph_dsnd, cph_jit;
u32 cph_jit_total;   /* monotonic; never reset by core_phase_take() */
u32 cph_reads;
u32 cph_bkr, cph_bkw;
u32 cph_rfux, cph_rfut;   /* ADR-0051: rfu_transfer() calls / us this frame */
u32 cph_dman;

static u32 cph_sum[CPH_NPHASE];    /* window totals, us */
static u32 cph_pmax[CPH_NPHASE];   /* window maxima, us -- each on ITS frame */
static u32 cph_worst[CPH_NPHASE];  /* the breakdown of the worst `tot` frame */
static u32 cph_frames, cph_win_reads;
static u32 cph_negc, cph_nego;     /* frames whose cpu / oth residue went < 0 */
static u32 cph_bkr_win, cph_bkw_win;
static u32 cph_ugba_win, cph_dman_win, cph_stmr_win;
static u32 cph_rfux_win, cph_rfut_win;
static u32 cph_worst_rfux, cph_worst_rfut;
static u32 cph_worst_bkr, cph_worst_bkw;

static u32 cph_depth;   /* inner-bracket nesting guard; see main.h */

u32 core_phase_now(u32 lvl)
{
  if (core_phase_lvl < lvl)
    return 0;
  cph_reads++;
  return smc_prof_clock();
}

u32 core_phase_enter(u32 lvl)
{
  if (core_phase_lvl < lvl)
    return 0;          /* inactive: does not take depth, so it cannot mask a
                        * nested bracket that IS active at its own level */
  if (cph_depth++)
    return 0;
  cph_reads++;
  return smc_prof_clock();
}

void core_phase_leave(u32 lvl, u32 *acc, u32 t0)
{
  if (core_phase_lvl < lvl)
    return;
  if (--cph_depth)
    return;
  cph_reads++;
  {
    u32 dt = smc_prof_clock() - t0;
    *acc += dt;
    /* Monotonic mirror for the JIT bracket only, so the on-screen profiler
     * can difference it without racing core_phase_take()'s reset.  This is
     * dynarec COMPILATION time — the cost that `smc_flush_us` does not
     * include and that the RAM-cache wipes force us to pay over and over. */
    if (acc == &cph_jit)
      cph_jit_total += dt;
  }
}

void core_phase_set_level(unsigned lvl)
{
  core_phase_clk_ns = 0;
  if (!smc_prof_clock)
    lvl = CORE_PHASE_OFF;      /* no clock -> no probes, at any setting */
  if (lvl > CORE_PHASE_DEEP)
    lvl = CORE_PHASE_DEEP;
  core_phase_lvl = lvl;

  if (lvl)
  {
    /* Price ONE clock read on this console, over 256 calls, so the log
     * carries the constant that converts `rd` to microseconds rather than
     * asserting one (same discipline as EVT sess_cost's clk_ns). */
    unsigned i;
    u32 a, b;
    a = smc_prof_clock();
    for (i = 0; i < 256; i++)
      (void)smc_prof_clock();
    b = smc_prof_clock();
    core_phase_clk_ns = (u32)(((u64)(b - a) * 1000ull) / 256ull);
  }
}

void core_phase_frame_begin(void)
{
  cph_vid = cph_amix = cph_dsnd = cph_jit = 0;
  cph_reads = 0;
  cph_bkr = cph_bkw = 0;
  cph_rfux = cph_rfut = 0;
  cph_ugba = cph_dman = cph_stmr = 0;
  cph_depth = 0;   /* a leaked depth cannot outlive one frame */
}

void core_phase_frame_end(u32 tot, u32 emu, u32 blt, u32 aout, u32 rfu)
{
  u32 p[CPH_NPHASE];
  u32 inner, outer;
  unsigned i;

  if (!core_phase_lvl)
    return;

  /* `cpu` and `oth` are RESIDUES, not measurements: whatever inside `emu` no
   * inner bracket claimed, and whatever inside `tot` no outer bracket did.
   * A negative residue would mean the brackets overlap or the clock stepped
   * backwards, so count it (`neg`) and clamp instead of wrapping -- a nonzero
   * `neg` invalidates the partition and must not be quietly absorbed. */
  inner = cph_vid + cph_amix + cph_dsnd + cph_jit;
  outer = emu + blt + aout + rfu;

  p[CPH_TOT]  = tot;
  p[CPH_VID]  = cph_vid;
  p[CPH_AMIX] = cph_amix;
  p[CPH_DSND] = cph_dsnd;
  p[CPH_JIT]  = cph_jit;
  p[CPH_BLT]  = blt;
  p[CPH_AOUT] = aout;
  p[CPH_RFU]  = rfu;

  if (emu >= inner)
    p[CPH_CPU] = emu - inner;
  else
  {
    p[CPH_CPU] = 0;
    cph_negc++;
  }

  if (tot >= outer)
    p[CPH_OTH] = tot - outer;
  else
  {
    p[CPH_OTH] = 0;
    cph_nego++;
  }

  for (i = 0; i < CPH_NPHASE; i++)
  {
    cph_sum[i] += p[i];
    if (p[i] > cph_pmax[i])
      cph_pmax[i] = p[i];
  }

  /* The per-phase maxima above each come from whichever frame happened to be
   * worst for that phase, so they do NOT have to sum to the frame maximum.
   * This snapshot is the one that attributes: the whole breakdown of the
   * single frame with the largest `tot` in the window. */
  if (tot >= cph_worst[CPH_TOT])
  {
    for (i = 0; i < CPH_NPHASE; i++)
      cph_worst[i] = p[i];
    cph_worst_bkr = cph_bkr;
    cph_worst_bkw = cph_bkw;
    cph_worst_rfux = cph_rfux;
    cph_worst_rfut = cph_rfut;
  }

  cph_frames++;
  cph_win_reads += cph_reads;
  cph_bkr_win   += cph_bkr;
  cph_bkw_win   += cph_bkw;
  cph_ugba_win  += cph_ugba;
  cph_dman_win  += cph_dman;
  cph_stmr_win  += cph_stmr;
  cph_rfux_win  += cph_rfux;
  cph_rfut_win  += cph_rfut;
}

void core_phase_take(unsigned *lvl, unsigned *clk_ns, unsigned *frames,
                     unsigned *reads, unsigned *mean, unsigned *max,
                     unsigned *worst, unsigned nph,
                     unsigned *bkr, unsigned *bkw, unsigned *wbkr,
                     unsigned *wbkw, unsigned *ugba, unsigned *dman,
                     unsigned *stmr, unsigned *negc, unsigned *nego,
                     unsigned *rfux, unsigned *rfut,
                     unsigned *wrfux, unsigned *wrfut)
{
  unsigned i;
  u32 f = cph_frames ? cph_frames : 1;

  *lvl    = core_phase_lvl;
  *clk_ns = core_phase_clk_ns;
  *frames = cph_frames;
  *reads  = cph_win_reads / f;
  for (i = 0; i < nph; i++)
  {
    mean[i]  = (i < CPH_NPHASE) ? cph_sum[i] / f : 0;
    max[i]   = (i < CPH_NPHASE) ? cph_pmax[i]    : 0;
    worst[i] = (i < CPH_NPHASE) ? cph_worst[i]   : 0;
  }
  *bkr  = cph_bkr_win / f;
  *bkw  = cph_bkw_win / f;
  *wbkr = cph_worst_bkr;
  *wbkw = cph_worst_bkw;
  *ugba = cph_ugba_win / f;
  *dman = cph_dman_win / f;
  *stmr = cph_stmr_win / f;
  *negc = cph_negc;
  *nego = cph_nego;
  *rfux  = cph_rfux_win / f;
  *rfut  = cph_rfut_win / f;
  *wrfux = cph_worst_rfux;
  *wrfut = cph_worst_rfut;

  memset(cph_sum,   0, sizeof(cph_sum));
  memset(cph_pmax,  0, sizeof(cph_pmax));
  memset(cph_worst, 0, sizeof(cph_worst));
  cph_frames = cph_win_reads = 0;
  cph_negc = cph_nego = 0;
  cph_bkr_win = cph_bkw_win = 0;
  cph_ugba_win = cph_dman_win = cph_stmr_win = 0;
  cph_rfux_win = cph_rfut_win = 0;
  cph_worst_rfux = cph_worst_rfut = 0;
  cph_worst_bkr = cph_worst_bkw = 0;
}

char main_path[512];

static u32 random_state = 0;

// Generate 16 random bits.
u16 rand_gen() {
  random_state = ((random_state * 1103515245) + 12345) & 0x7fffffff;
  return random_state;
}

// Add some random state to the initial seed.
void rand_seed(u32 data) {
  random_state ^= rand_gen() ^ data;
}


#ifdef TIMER_LAZY
/* TIMER_LAZY=1 (build) + timer_lazy (run time; harness `timer_lazy = 1`).
 * PROTOTYPE, OFF by default, NOT a pure speed switch: it changes guest-
 * visible timing (docs/GBA-LINK-MIRROR.md 26; owner-approved as a prototype
 * only, 2026-10-11).
 *
 * Today every overflow of a running timer is a scheduler event: the CPU is
 * stopped exactly there.  For a timer that raises no interrupt and feeds no
 * cascade, the overflow has nothing the CPU must see at that instant -- for
 * a direct-sound timer it pops one FIFO sample and, every 16th time, asks
 * for the refill DMA.  With the switch on such a timer no longer stops the
 * CPU; its overflows (all of them: the loop below) are processed at the
 * next scheduler call, which is at most one H-blank edge (960 cycles) away.
 *
 * What a game can observe:
 *   - the sound FIFO's refill DMA (and the cycles it steals) happens up to
 *     960 cycles later than today;
 *   - the CPU is no longer interrupted at those overflows, so where
 *     update_gba's overshoot clamp applies moves, and with it the cycle
 *     count of everything that follows;
 *   - the timer's counter register is refreshed at scheduler calls, as
 *     today, but no longer at the overflow itself.
 * Timers with their interrupt enabled, and timers a cascade timer counts,
 * keep their exact events. */
/* TIMER_LAZY_DEFAULT: sched_hot.h */
#ifndef SCHED_HOT_IN_REG
u32 timer_lazy = TIMER_LAZY_DEFAULT;
#else
extern u32 timer_lazy;
#endif
#define TIMER_IS_LAZY(i)                                                      \
   (timer_lazy && !timer[i].irq &&                                            \
    !((i) != 3 && timer[(i) + 1].status == TIMER_CASCADE))
#else
#define TIMER_IS_LAZY(i) 0
#endif

#ifdef HALT_HOP
/* HALT_HOP's run-time switch: the harness key / session core mask
 * `timer_lazy` (main_psp.c, fe_gbadual.c copies it to machine B), as a bit
 * mask: bit 1 = HALT_HOP, bit 2 = HOP_DRAWN.  HALT_HOP_DEFAULT is the value a
 * build starts with (6 = both). */
#ifdef TIMER_LAZY
#error "HALT_HOP shares the timer_lazy key with TIMER_LAZY"
#endif
/* HALT_HOP_DEFAULT: sched_hot.h */
#ifndef SCHED_HOT_IN_REG
u32 timer_lazy = HALT_HOP_DEFAULT;
#else
extern u32 timer_lazy;
#endif
#define hop_mask timer_lazy
#endif

#ifdef HALT_HOP
/* HALT_HOP=1 (build) + timer_lazy bit 1 (run time).  While the CPU is halted
 * nothing can read DISPSTAT or VCOUNT, so the scheduler need not stop at the
 * H-blank edges of a line where those edges do nothing else (no H-blank IRQ
 * or DMA, nothing to draw: a skipped frame, the partner machine, V-blank
 * lines).  One jump covers those lines, up to the first edge with an effect
 * of its own (V-blank, frame end, a V-count match with its IRQ enabled); an
 * interrupt that wakes the CPU on the way puts the counters back exactly
 * where they would be.  Nothing moves by a cycle, and no slice with a running
 * CPU changes, so update_gba's overshoot clamp is not involved. */
static u32 hop_vcount;     /* the jump's last line, 0 = no jump running */
/* read on every update_gba pass: in the register block on MIPS (sched_hot.h) */
#ifdef SCHED_HOT_IN_REG
extern u32 sched_hop_active;
#else
static u32 sched_hop_active;
#endif
#define hop_active sched_hop_active
static s32 hop_hb_at;      /* cycles left at the first H-blank edge, -1 = no IF */
static u32 hop_vstart;     /* the jump's first line (its H-blank edge was ahead) */

/* timer_lazy bit 2 (HOP_DRAWN): jump over drawn lines too.  While the CPU is
 * halted nothing writes the LCD registers, VRAM, OAM or palette (no H-blank
 * DMA runs, see halt_hop_begin; a sound DMA writes only its FIFO), so each
 * line whose H-blank edge the jump passed is drawn afterwards exactly as it
 * would have been at its edge, VCOUNT and DISPSTAT showing that line.  Then
 * the jump no longer depends on whether this console draws the machine, and
 * both consoles slice both machines alike. */
static __attribute__((noinline)) void halt_hop_lines(u32 from, u32 to)
{
   u32 ds = read_ioreg(REG_DISPSTAT), vc = read_ioreg(REG_VCOUNT), l;
   for (l = from; l <= to && l < 160; l++)
   {
      write_ioreg(REG_VCOUNT, l);
      write_ioreg(REG_DISPSTAT, (ds & ~0x07u) | (l == (ds >> 8) ? 0x04u : 0u));
      if (reg[OAM_UPDATED])
         oam_update_count++;
      update_scanline();
   }
   write_ioreg(REG_DISPSTAT, ds);
   write_ioreg(REG_VCOUNT, vc);
}

static __attribute__((noinline)) void halt_hop_begin(void)
{
   u32 dispstat = read_ioreg(REG_DISPSTAT);
   u32 vcount = read_ioreg(REG_VCOUNT), last, i;
   /* In H-blank: wait for the next line.  An H-blank IRQ that IE masks only
    * sets its IF bit, which can be done when the CPU can see it. */
   if ((dispstat & 2) || ((dispstat & 0x10) && (read_ioreg(REG_IE) & IRQ_HBLANK)))
      return;
   for (i = 0; i < 4; i++)
      if (dma[i].start_type == DMA_START_HBLANK)
         return;
   if (vcount < 160 && !skip_next_frame && !(hop_mask & 4))
      return;
   hop_vstart = vcount;
   last = vcount;
   /* vcount -> vcount + 1 is free unless it starts V-blank, ends the frame
    * or raises the V-count IRQ */
   while (last + 1 != 160 && last + 1 != 228 &&
          !((dispstat & 0x20) && last + 1 == (dispstat >> 8)))
      last++;
   hop_hb_at = (dispstat & 0x10) ? (s32)(last - vcount) * 1232 + 272 : -1;
   video_count += (s32)(last - vcount) * 1232 + 272;
   write_ioreg(REG_DISPSTAT, dispstat | 0x02);
   write_ioreg(REG_VCOUNT, last);
   hop_vcount = last;
   hop_active = 1;
}

/* The CPU woke with the jump running: where are we really? */
static __attribute__((noinline)) void halt_hop_wake(void)
{
   s32 r = video_count;
   u32 dispstat = read_ioreg(REG_DISPSTAT);
   u32 back = (u32)(r - 1) / 1232u, rest = (u32)(r - 1) % 1232u + 1u;
   u32 vcount = hop_vcount - back;
   hop_active = 0;
   if (r <= hop_hb_at)
      write_ioreg(REG_IF, read_ioreg(REG_IF) | IRQ_HBLANK);
   /* the drawn lines whose H-blank edge has passed: up to this one if it is
    * in its H-blank */
   if (rest <= 272)
      halt_hop_lines(hop_vstart, vcount);
   else if (vcount > hop_vstart)
      halt_hop_lines(hop_vstart, vcount - 1);
   if (rest > 272)
   {
      dispstat &= ~0x02u;
      video_count = (s32)rest - 272;
   }
   else
      video_count = (s32)rest;
   dispstat = vcount == (dispstat >> 8) ? (dispstat | 0x04) : (dispstat & ~0x04u);
   write_ioreg(REG_DISPSTAT, dispstat);
   write_ioreg(REG_VCOUNT, vcount);
}
#endif


static unsigned update_timers(irq_type *irq_raised, unsigned completed_cycles)
{
   unsigned i, ret = 0;
   for (i = 0; i < 4; i++)
   {
      if(timer[i].status == TIMER_INACTIVE)
         continue;

      if(timer[i].status != TIMER_CASCADE)
      {
         timer[i].count -= completed_cycles;
         /* io_registers accessors range: REG_TM0D, REG_TM1D, REG_TM2D, REG_TM3D */
         write_ioreg(REG_TMXD(i), -(timer[i].count >> timer[i].prescale));
      }

      if(timer[i].count > 0)
         continue;

#ifdef TIMER_LAZY
      do {      /* a lazy timer may have overflowed more than once */
#endif
      /* irq_raised value range: IRQ_TIMER0, IRQ_TIMER1, IRQ_TIMER2, IRQ_TIMER3 */
      if(timer[i].irq)
         *irq_raised |= (IRQ_TIMER0 << i);

      if((i != 3) && (timer[i + 1].status == TIMER_CASCADE))
      {
         timer[i + 1].count--;
         write_ioreg(REG_TMXD(i + 1), -timer[i+1].count);
      }

      if(i < 2)
      {
         /* Phase 5h: the direct-sound FIFO drain runs once per DS sample
          * (~450x/frame at 32768 Hz, both channels), so its bracket is the
          * single most expensive probe in the build -- level DEEP only. */
         if(timer[i].direct_sound_channels & 0x01)
         {
            u32 cph_t = core_phase_enter(CORE_PHASE_DEEP);
            ret += sound_timer(timer[i].frequency_step, 0);
            core_phase_leave(CORE_PHASE_DEEP, &cph_dsnd, cph_t);
            cph_stmr++;
         }

         if(timer[i].direct_sound_channels & 0x02)
         {
            u32 cph_t = core_phase_enter(CORE_PHASE_DEEP);
            ret += sound_timer(timer[i].frequency_step, 1);
            core_phase_leave(CORE_PHASE_DEEP, &cph_dsnd, cph_t);
            cph_stmr++;
         }
      }

      timer[i].count += (timer[i].reload << timer[i].prescale);
#ifdef TIMER_LAZY
      } while (timer_lazy && timer[i].count <= 0 &&
               timer[i].status == TIMER_PRESCALE);
#endif
#ifdef TIMER_RELOAD_FIX
      /* A timer that just overflowed reads its RELOAD value, not 0x0000
       * (dr2-nfsu, 2026-10-21).  The register is refreshed above with the
       * count BEFORE the reload, i.e. -(0) = 0x0000 at the exact overflow
       * event, and nothing rewrote it until the next update -- so a game that
       * reads TMxCNT_L right after an overflow (its IRQ handler, or code
       * running until the next scheduler event) saw 0.  Need for Speed
       * Underground's mixer computes its sample count from TM1 with reload
       * 0xFEE0 as (len + pos - 0x10000): pos = 0 gave 0xFFFF0120 bytes, a
       * runaway memset that wipes IWRAM, its own loop included (4.1.1: the
       * 0.15 fps "storm"; mGBA reads 0xFEE0.. and boots on). */
      write_ioreg(REG_TMXD(i), timer[i].status == TIMER_CASCADE ?
                  (u32)(-timer[i].count) :
                  (u32)(-(timer[i].count >> timer[i].prescale)));
#endif
   }
   return ret;
}

void init_main(void)
{
  u32 i;
  for(i = 0; i < 4; i++)
  {
    timer[i].status = TIMER_INACTIVE;
    timer[i].prescale = 0;
    timer[i].irq = 0;
    timer[i].reload = timer[i].count = 0x10000;
    timer[i].direct_sound_channels = TIMER_DS_CHANNEL_NONE;
    timer[i].frequency_step = 0;
  }

  timer[0].direct_sound_channels = TIMER_DS_CHANNEL_BOTH;
  timer[1].direct_sound_channels = TIMER_DS_CHANNEL_NONE;

  frame_counter = 0;
  cpu_ticks = 0;
  execute_cycles = 960;
  video_count = 960;

#ifdef HAVE_DYNAREC
  main_init_dynarec();
#endif
}

/* Frontends with fallible JIT mappings override this weak default. */
__attribute__((weak)) bool gpsp_dynarec_cache_available(void)
{
  return true;
}

void main_init_dynarec(void)
{
#ifdef HAVE_DYNAREC
  if (!dynarec_enable || !gpsp_dynarec_cache_available())
    return;
  init_dynarec_caches();
  init_emitter(gamepak_must_swap() || gamepak_force_swap_stubs != 0);
#endif
}

void main_enable_dynarec(void)
{
#ifdef HAVE_DYNAREC
  if (!dynarec_enable || !gpsp_dynarec_cache_available())
    return;
  /* The game is already running here. Rebuild the emitter helpers and reset
   * translated code without clearing IWRAM/EWRAM as init_dynarec_caches does. */
  init_emitter(gamepak_must_swap() || gamepak_force_swap_stubs != 0);
  flush_dynarec_caches();
#endif
}

__attribute__((weak)) void gpsp_visible_done_hook(void) {}

#ifdef UPDATE_TRACE_LO
static FILE *utf;
void utrace_note(const char *what, u32 a, u32 b)
{
  if (frame_counter < UPDATE_TRACE_LO || frame_counter > UPDATE_TRACE_HI) return;
  if (!utf) utf = fopen("ms0:/utrace.txt", "w");
  if (utf) fprintf(utf, "  %s %08x %08x t=%u\n", what, (unsigned)a, (unsigned)b, (unsigned)cpu_ticks);
}
#endif
#ifdef SERIAL_IDLE_FAST
/* SERIAL_IDLE_FAST (contender #3, OFF by default; docs/DYNAREC-PROFILE.md):
 * update_serial() runs on every update_gba pass (3-7% of a solo frame).  With
 * no transfer pending and no link client role it is provably a no-op except
 * for the protocol fakes' cycle counter, which this does inline:
 *   - POKE / AW1 / AW2 with netplay_client_id == 0: serialpoke_update /
 *     serialaw_update only do `frcnt += cycles` (their IRQ branch needs a
 *     client id), then update_serial finds serial_irq_cycles == 0 -> false;
 *   - DISABLED / GBP / AUTO: no device update, same irq check -> false;
 *   - RFU: NOT taken -- the adapter answers the game even with no partner.
 * Returns 1 when it handled the pass (no IRQ), 0 to take the full path. */
extern u32 serial_irq_cycles;
extern unsigned *const serial_poke_frcnt;
extern unsigned *const serial_aw_frcnt;
/* netplay_client_id is read only where it matters (SCHED_HOT): for DISABLED
 * / GBP / AUTO, update_serial with no IRQ pending returns false whatever the
 * client id, so those modes skip the load (and its cache line). */
static inline int serial_idle_fast(unsigned cycles)
{
  if (serial_irq_cycles)
    return 0;
  switch (serial_mode) {
  case SERIAL_MODE_SERIAL_POKE:
    if (netplay_client_id)
      return 0;
    *serial_poke_frcnt += cycles;
    return 1;
  case SERIAL_MODE_SERIAL_AW1:
  case SERIAL_MODE_SERIAL_AW2:
    if (netplay_client_id)
      return 0;
    *serial_aw_frcnt += cycles;
    return 1;
  case SERIAL_MODE_DISABLED:
  case SERIAL_MODE_GBP:
  case SERIAL_MODE_AUTO:
    return 1;
  default:
    return 0;
  }
}
#endif

#ifdef SCHED_FAST
/* SCHED_FAST=1 (build) + sched_fast (run time; harness `sched_fast = 1`):
 * scheduler fast paths, OFF by default (docs/GBA-LINK-MIRROR.md 24).  Pure
 * speed: the machine is identical with it on or off.
 *
 * 1. THE INTERRUPT CHECK.  update_gba called check_and_raise_interrupts on
 *    every iteration (845 a frame per machine in Street Fighter's fight,
 *    1,358 in an Advance Wars 2 battle) and flag_interrupt for every raised
 *    source.  Both of check_and_raise_interrupts' effects -- waking a HALT
 *    and entering the vector (which also ends STOP) -- require
 *    (IE & IF) != 0 (cpu.h cpu_halt_wake_requested, cpu_irq_delivery_enabled),
 *    so with IE & IF == 0 the call can only return 0 and is skipped.
 *    flag_interrupt is `IF |= raised` plus a check_interrupt() whose result
 *    update_gba discards; the OR is done here. */
/* SCHED_FAST_DEFAULT: sched_hot.h */
#ifdef SCHED_HOT_IN_REG
extern u32 sched_fast;
#else
u32 sched_fast = SCHED_FAST_DEFAULT;
#endif
#endif

#ifdef SPIN_SKIP
int spin_check(u32 *desc);
extern u32 spin_skips, spin_vetoes, spin_loops;
#endif
#ifdef SPIN_SKIP_VERIFY
#define SKIP_VERIFY 1
/* SPIN_SKIP_VERIFY (twin oracle): never skip.  Where a skip
 * would be taken -- a spin descriptor that passes spin_check, or the game
 * table's idle branch -- snapshot the machine.  Until the event the skip
 * would have jumped to, every pass through the same claim site must find the
 * same registers, and at that event (the next update_gba entry that is not a
 * claim) memory must be unchanged: the skipped passes really were exact
 * repeats.  Reaching another claim site first means the loop was left.  A
 * spin loop is one block (plus, threaded, the `b` it branches through), so
 * its event lands on the back-edge and the registers are compared there as
 * well; the table's branch may sit in a loop of several blocks (calls
 * included), so at the event its registers are compared only if the guest is
 * back at the claim site.  Any difference is a violation (printed, and
 * counted per claim site).  The idle-table kind (1) was IDLE_VERIFY's; that
 * oracle is gone, and the kind plumbing stays so SPIN_SKIP_VERIFY reports
 * exactly what it always did. */
static u32 sv_active, sv_kind, sv_claims[2], sv_bad[2];
static u32 sv_reg[17];
static u8 sv_iw[0x8000], sv_ew[0x40000], sv_vr[sizeof(vram)];
static u16 sv_pal[512], sv_oam[512], sv_io[512];
static struct { u32 pc, n; const char *what; } sv_hist[2][16];

static void skip_verify_bad(const char *what)
{
  u32 k;
  sv_bad[sv_kind]++;
  for (k = 0; k < 16; k++)
  {
    if (!sv_hist[sv_kind][k].n)
    {
      sv_hist[sv_kind][k].pc = sv_reg[REG_PC];
      sv_hist[sv_kind][k].what = what;
    }
    if (sv_hist[sv_kind][k].pc == sv_reg[REG_PC] &&
        sv_hist[sv_kind][k].what == what)
    {
      sv_hist[sv_kind][k].n++;
      break;
    }
  }
  if (sv_bad[0] + sv_bad[1] <= 20)
    fprintf(stderr, "%s_VERIFY violation: %s, loop pc %08x now pc %08x\n",
            sv_kind ? "IDLE" : "SPIN", what, (unsigned)sv_reg[REG_PC],
            (unsigned)reg[REG_PC]);
}

static void skip_verify_claim(u32 kind)
{
  memcpy(sv_reg, reg, sizeof(sv_reg));
  memcpy(sv_iw, &iwram[0x8000], 0x8000);
  memcpy(sv_ew, ewram, 0x40000);
  memcpy(sv_vr, vram, sizeof(vram));
  memcpy(sv_pal, palette_ram, sizeof(sv_pal));
  memcpy(sv_oam, oam_ram, sizeof(sv_oam));
  memcpy(sv_io, io_registers, sizeof(sv_io));
  sv_active = 1;
  sv_kind = kind;
  sv_claims[kind]++;
}

/* A pass through a claim site (0 spin, 1 idle table) with cycles left. */
static void skip_verify_pass(u32 kind)
{
  if (sv_active)
  {
    if (kind == sv_kind && reg[REG_PC] == sv_reg[REG_PC])
    {
      if (!memcmp(sv_reg, reg, sizeof(sv_reg)))
        return;
      skip_verify_bad("regs differ at a repeat");
    }
    else
      skip_verify_bad("left the loop before the event");
  }
  skip_verify_claim(kind);
}

/* A threaded spin loop is claimed at its trampoline (the `b` back to the
 * loop start), and its event may land on either side of that `b`: the same
 * state if pc is all that differs and the claim pc branches to it. */
static int sv_trampoline_only(void)
{
  u32 k, op, to = ~0u, at = sv_reg[REG_PC];
  for (k = 0; k < 17; k++)
    if (k != REG_PC && sv_reg[k] != reg[k]) return 0;
  if ((at >> 24) < 0x08 || (at >> 24) > 0x0D) return 0;
  if (sv_reg[REG_CPSR] & 0x20) {
    op = read_memory16(at);
    if ((op >> 11) == 0x1C) to = at + 4 + (u32)(((s32)(op << 21)) >> 20);
  } else {
    op = read_memory32(at);
    if ((op >> 24) == 0xEA) to = at + 8 + (u32)(((s32)(op << 8)) >> 6);
  }
  return to == reg[REG_PC];
}

static void skip_verify_event(void)
{
  const char *what = NULL;
  if (!sv_active) return;
  sv_active = 0;
  if ((!sv_kind || reg[REG_PC] == sv_reg[REG_PC]) &&
      memcmp(sv_reg, reg, sizeof(sv_reg)) &&
      (sv_kind || !sv_trampoline_only())) what = "regs differ";
  else if (memcmp(sv_iw, &iwram[0x8000], 0x8000)) what = "iwram differs";
  else if (memcmp(sv_ew, ewram, 0x40000)) what = "ewram differs";
  else if (memcmp(sv_vr, vram, sizeof(vram))) what = "vram differs";
  else if (memcmp(sv_pal, palette_ram, sizeof(sv_pal))) what = "palette differs";
  else if (memcmp(sv_oam, oam_ram, sizeof(sv_oam))) what = "oam differs";
  else if (memcmp(sv_io, io_registers, sizeof(sv_io))) what = "io differs";
  if (what)
    skip_verify_bad(what);
}
#endif
#if defined(DRPROF_TWIN) && defined(SPIN_SKIP)
static void spin_report(void)
{
#ifdef SPIN_SKIP
  fprintf(stderr, "SPIN_SKIP loops=%u skips=%u vetoes=%u\n",
          (unsigned)spin_loops, (unsigned)spin_skips, (unsigned)spin_vetoes);
#endif
#ifdef SKIP_VERIFY
  {
    u32 kd, k;
    for (kd = 0; kd < 2; kd++)
    {
      fprintf(stderr, "%s_VERIFY claims=%u violations=%u\n", kd ? "IDLE" : "SPIN",
              (unsigned)sv_claims[kd], (unsigned)sv_bad[kd]);
      for (k = 0; k < 16 && sv_hist[kd][k].n; k++)
        fprintf(stderr, "  %s_VERIFY site %08x: %u x %s\n", kd ? "IDLE" : "SPIN",
                (unsigned)sv_hist[kd][k].pc, (unsigned)sv_hist[kd][k].n,
                sv_hist[kd][k].what);
    }
  }
#endif
}
#endif



u32 function_cc update_gba(int remaining_cycles)
{
  DRPH_SCOPE(DRPH_UPD);
#ifdef SPIN_SKIP
  /* A spin loop's back-edge (mips_emit.h generate_branch_spin) asks whether
   * it may end this slice.  Only with cycles left: a due event is processed
   * normally.  Vetoed loops return here with their cycles untouched. */
#ifdef DRPROF_TWIN
  { static int rep; if (!rep) { rep = 1; atexit(spin_report); } }
#endif
  if (reg[REG_SPIN_DESC])
  {
    u32 sdw = reg[REG_SPIN_DESC];
    reg[REG_SPIN_DESC] = 0;
    {
#ifdef SPIN_SKIP
      u32 *sd = (u32 *)sdw;
      if (remaining_cycles >= 0)
      {
        if (!spin_check(sd))
        {
          sd[0] = 1;
          spin_vetoes++;
          return remaining_cycles;
        }
#ifdef SPIN_SKIP_VERIFY
        skip_verify_pass(0);
        return remaining_cycles;
#else
        spin_skips++;
        remaining_cycles = 0;
#endif
      }
#endif
    }
  }
#ifdef SKIP_VERIFY
  skip_verify_event();
#endif
#endif
  u32 changed_pc = 0;
  u32 frame_complete = 0;
  irq_type irq_raised = IRQ_NONE;
  int dma_cycles;
#ifdef GBA_LINK
  /* Link cable stepper (gbalink_core.c): postpone this whole call -- the
   * dynarec returns to the stepper, which later makes exactly this call with
   * exactly this argument.  Invisible to the guest. */
  if (serial_mode == SERIAL_MODE_CABLE && gbalink_entry_yield_q(remaining_cycles))
  {
    return 0x80000000;
  }
#endif
  trace_update_gba(remaining_cycles);
#ifdef STALE_CHECK
  { void stale_scan(u32); stale_scan(0xFFFFFFFFu); }
#endif
#ifdef UPDATE_TRACE_LO
  if (frame_counter >= UPDATE_TRACE_LO && frame_counter <= UPDATE_TRACE_HI)
  {
    /* DIAGNOSTIC: one line per update_gba entry -- where the guest is and a
     * hash of its registers and IWRAM data -- so two builds can be diffed
     * call by call. */
    u32 h = 2166136261u, hi = 2166136261u, k;
    if (!utf) utf = fopen("ms0:/utrace.txt", "w");
    for (k = 0; k < 15; k++) h = (h ^ reg[k]) * 16777619u;
    for (k = 0; k < 0x8000; k += 4) hi = (hi ^ *(const u32 *)(&iwram[0x8000] + k)) * 16777619u;
    if (utf) fprintf(utf, "f=%u t=%u rem=%d pc=%08x cpsr=%08x rh=%08x ih=%08x\n",
                     (unsigned)frame_counter, (unsigned)cpu_ticks, remaining_cycles,
                     (unsigned)reg[REG_PC], (unsigned)reg[REG_CPSR], (unsigned)h, (unsigned)hi);
    if (utf) {
      fprintf(utf, "   ");
      for (k = 0; k < 15; k++) fprintf(utf, " %08x", (unsigned)reg[k]);
      fprintf(utf, " g%u\n", (unsigned)translation_gate_targets);
    }
#ifdef UTRACE_DUMP_T
    if (cpu_ticks == UTRACE_DUMP_T || cpu_ticks == UTRACE_DUMP_T0)
    {
      char nm[48];
      FILE *df;
      snprintf(nm, sizeof(nm), "ms0:/ud_%u.bin", (unsigned)cpu_ticks);
      df = fopen(nm, "wb");
      if (df) {
        fwrite(&iwram[0x8000], 1, 0x8000, df);
        fwrite(ewram, 1, 0x40000, df);
        fwrite(reg, 4, 64, df);
        fwrite(io_registers, 1, 0x400, df);
        fwrite(iwram, 1, 0x8000, df);    /* the code-tag half */
        fclose(df);
      }
    }
#endif
  }
#endif

  remaining_cycles = MAX(remaining_cycles, -64);

  do
  {
    unsigned i;
    // Number of cycles we ask to run - cycles that we did not execute
    // (remaining_cycles can be negative and should be close to zero)
    unsigned completed_cycles = execute_cycles - remaining_cycles;
    cph_ugba++;   /* phase 5h: dynarec -> C round trips per frame */
    cpu_ticks += completed_cycles;
#ifdef GBA_LINK
    if (serial_mode == SERIAL_MODE_CABLE)
      gbalink_account(completed_cycles);
#endif

    remaining_cycles = 0;

    // Timers can trigger DMA (usually sound) and consume cycles
    dma_cycles = update_timers(&irq_raised, completed_cycles);
    // Check for serial port IRQs as well.
#ifdef GBA_LINK
    /* Cable mode: serial_idle_fast takes the full path for it and
     * update_serial only forwards, so call the cable hook itself (inline
     * when the lean hooks are on, serial.h).  Same answer, ~60 MIPS
     * instructions fewer per iteration (docs/GBA-LINK-MIRROR.md 23). */
    if (serial_mode == SERIAL_MODE_CABLE)
    {
      if (gbalink_update_serial_q())
        irq_raised |= IRQ_SERIAL;
    }
    else
#endif
#ifdef SERIAL_IDLE_FAST
    if (!serial_idle_fast(completed_cycles))
#endif
    if (update_serial(completed_cycles))
      irq_raised |= IRQ_SERIAL;

    // Video count tracks the video cycles remaining until the next event
    video_count -= completed_cycles;

    // Ran out of cycles, move to the next video area
    if(video_count <= 0)
    {
#ifdef HALT_HOP
      if (hop_active)
      {
        hop_active = 0;
        if (hop_hb_at >= 0)
          write_ioreg(REG_IF, read_ioreg(REG_IF) | IRQ_HBLANK);
        halt_hop_lines(hop_vstart, hop_vcount);
      }
#endif
      u32 vcount = read_ioreg(REG_VCOUNT);
      u32 dispstat = read_ioreg(REG_DISPSTAT);

      // Check if we are in hrefresh (0) or hblank (1)
      if ((dispstat & 0x02) == 0)
      {
        // Transition from hrefresh to hblank
        dispstat |= 0x02;
        video_count += (272);    // hblank duration, 272 cycles

        // Check if we are drawing (0) or we are in vblank (1)
        if ((dispstat & 0x01) == 0)
        {
          u32 i;

          // Render the scan line
          if(reg[OAM_UPDATED])
            oam_update_count++;

          /* Phase 5h: 160 brackets a frame, the bulk of the level-FINE probe
           * budget.  The cost lands INSIDE `vid`, so `rd x clk` prices it. */
          {
            u32 cph_t = core_phase_enter(CORE_PHASE_FINE);
            update_scanline();
            core_phase_leave(CORE_PHASE_FINE, &cph_vid, cph_t);
          }

          // Trigger the HBlank DMAs if enabled
          {
            cpu_alert_type dma_alert = CPU_ALERT_NONE;
            for (i = 0; i < 4; i++)
            {
              if(dma[i].start_type == DMA_START_HBLANK)
                dma_alert |= dma_transfer(i, &dma_cycles);
            }
#if defined(HAVE_DYNAREC) && defined(DMA_SMC_FLUSH)
            /* A DMA that lands on translated code raises CPU_ALERT_SMC, as
             * the CPU-triggered path in write_io_epilogue already honours.
             * Here it used to be dropped, leaving the old translation live.
             * Flush, and report a PC change so mips_update_gba re-dispatches
             * via lookup_pc instead of returning into the flushed block. */
            if ((dma_alert & CPU_ALERT_SMC) && dynarec_enable)
            {
              dma_smc_flushes++;
              flush_translation_cache_ram_dma();
              changed_pc = 0x40000000;
            }
#else
            (void)dma_alert;
#endif
          }
        }

        // Trigger the hblank interrupt, if enabled in DISPSTAT
        if (dispstat & 0x10)
          irq_raised |= IRQ_HBLANK;
      }
      else
      {
        // Transition from hblank to the next scan line (vdraw or vblank)
        video_count += 960;
        dispstat &= ~0x02;
        vcount++;

        if(vcount == 160)
        {
          // Transition from vrefresh to vblank
          u32 i;
          dispstat |= 0x01;

          /* END OF VISIBLE. The ME capture (ioregs[0..159]) is COMPLETE
           * here, but we currently sit on it for another 68 scanlines and
           * only hand it over when retro_run returns. Those 68 lines are
           * where games do their real work -- VBlank handler, AI, sprite
           * updates, librfu's exchange -- so they are a far larger share of
           * the frame's TIME than of its line count. How much larger has
           * never been measured, and it decides whether the ME can finish
           * inside the frame it belongs to. */
#if ME_MIDFRAME_LOG
          me_capture_visible_end();     /* line-160 log check + map merge */
#endif
          gpsp_visible_done_hook();
#if ME_MIDFRAME_LOG
          me_capture_visible_resume();
#endif

          // Reinit affine transformation counters for the next frame
          video_reload_counters();

          // Trigger VBlank interrupt if enabled
          if (dispstat & 0x8)
            irq_raised |= IRQ_VBLANK;

          // Trigger the VBlank DMAs if enabled
          {
            cpu_alert_type dma_alert = CPU_ALERT_NONE;
            for (i = 0; i < 4; i++)
            {
              if(dma[i].start_type == DMA_START_VBLANK)
                dma_alert |= dma_transfer(i, &dma_cycles);
            }
#if defined(HAVE_DYNAREC) && defined(DMA_SMC_FLUSH)
            /* Same as the HBlank case above. */
            if ((dma_alert & CPU_ALERT_SMC) && dynarec_enable)
            {
              dma_smc_flushes++;
              flush_translation_cache_ram_dma();
              changed_pc = 0x40000000;
            }
#else
            (void)dma_alert;
#endif
          }
        }
        else if (vcount == 228)
        {
          // Transition from vblank to next screen
          vcount = 0;
          dispstat &= ~0x01;

#ifndef GPSP_CHEATS
          /* If there's no cheat hook, run on vblank! */
          if (cheat_master_hook == ~0U)
             process_cheats();
#endif
          /* GPSP_CHEATS: codes run at the top of retro_run instead -- the same
           * guest instant (this vcount-228 edge ends the frame) but outside
           * the dynarec, where invalidating translated code is safe
           * (cheats.h; docs/CHEATS.md section 5). */

/*        printf("frame update (%x), %d instructions total, %d RAM flushes\n",
           reg[REG_PC], instruction_count - last_frame, flush_ram_count);
          last_frame = instruction_count;
*/
/*          printf("%d gbc audio updates\n", gbc_update_count);
          printf("%d oam updates\n", oam_update_count); */
          gbc_update_count = 0;
          oam_update_count = 0;
          flush_ram_count = 0;

          // Force audio generation. Need to flush samples for this frame.
          render_gbc_sound();

          // We completed a frame, tell the dynarec to exit to the main thread
          frame_complete = 0x80000000;
          frame_counter++;
#ifdef STATE_HASH_TRACE
          {
            /* DIAGNOSTIC: per-frame hash of guest-visible state. */
            static FILE *shf;
            u32 h = 2166136261u, k;
            const u8 *p;
            if (!shf) shf = fopen("ms0:/statehash.txt", "w");
            for (k = 0; k < 17; k++) h = (h ^ reg[k]) * 16777619u;
            p = &iwram[0x8000];
            for (k = 0; k < 0x8000; k += 4) h = (h ^ *(const u32 *)(p + k)) * 16777619u;
            {
              u32 he = 2166136261u;
              for (k = 0; k < 0x40000; k += 4) he = (he ^ *(const u32 *)(ewram + k)) * 16777619u;
              if (shf) fprintf(shf, "f=%u ticks=%u r=%08x iw=%08x ew=%08x\n",
                               (unsigned)frame_counter, (unsigned)cpu_ticks,
                               (unsigned)reg[REG_PC], (unsigned)h, (unsigned)he);
            }
            if (shf) fflush(shf);
#ifdef STATE_DUMP_LO
            if ((frame_counter >= STATE_DUMP_LO && frame_counter < STATE_DUMP_LO + 3) ||
                (frame_counter >= STATE_DUMP_HI && frame_counter < STATE_DUMP_HI + 4))
            {
              char nm[64];
              FILE *df;
              snprintf(nm, sizeof(nm), "ms0:/sd_%u.bin", (unsigned)frame_counter);
              df = fopen(nm, "wb");
              if (df) {
                fwrite(&iwram[0x8000], 1, 0x8000, df);
                fwrite(ewram, 1, 0x40000, df);
                fwrite(reg, 4, 64, df);
                fwrite(io_registers, 1, 0x400, df);
                fclose(df);
              }
            }
#endif
          }
#endif
        }

        // Vcount trigger (flag) and IRQ if enabled
        if(vcount == (dispstat >> 8))
        {
          dispstat |= 0x04;
          if(dispstat & 0x20)
            irq_raised |= IRQ_VCOUNT;
        }
        else
          dispstat &= ~0x04;

        write_ioreg(REG_VCOUNT, vcount);
      }
      write_ioreg(REG_DISPSTAT, dispstat);
    }

#ifdef SCHED_FAST
    if (sched_fast)
    {
      if (irq_raised)
        write_ioreg(REG_IF, read_ioreg(REG_IF) | irq_raised);
      if ((read_ioreg(REG_IE) & read_ioreg(REG_IF)) &&
          check_and_raise_interrupts())
        changed_pc = 0x40000000;
    }
    else
#endif
    {
    // Flag any V/H blank interrupts, DMA IRQs, Vcount, etc.
    if (irq_raised)
      flag_interrupt(irq_raised);

    // Raise any pending interrupts. This changes the CPU mode.
    if (check_and_raise_interrupts())
      changed_pc = 0x40000000;
    }

#ifdef HALT_HOP
    if (reg[CPU_HALT_STATE] != CPU_HALT)
    {
      if (hop_active)      /* woken in this pass: before the next stop is set */
        halt_hop_wake();
    }
    else if ((hop_mask & 2) && !hop_active && !frame_complete)
      halt_hop_begin();
#endif
    // Figure out when we need to stop CPU execution. The next event is
    // a video event or a timer event, whatever happens first.
    execute_cycles = MAX(video_count, 0);
    {
#ifdef GBA_LINK
      u32 cc = serial_mode == SERIAL_MODE_CABLE ? gbalink_next_event_q()
                                                : serial_next_event();
#else
      u32 cc = serial_next_event();
#endif
      execute_cycles = MIN(execute_cycles, cc);
    }

    // If we are paused due to a DMA, cap the number of cyles to that amount.
    if (reg[CPU_HALT_STATE] == CPU_DMA) {
      u32 dma_cyc = reg[REG_SLEEP_CYCLES];
      // The first iteration is marked by bit 31 set, just do nothing now.
      if (dma_cyc & 0x80000000)
        dma_cyc &= 0x7FFFFFFF;  // Start counting DMA cycles from now on.
      else
        dma_cyc -= MIN(dma_cyc, completed_cycles);  // Account DMA cycles.

      reg[REG_SLEEP_CYCLES] = dma_cyc;
      if (!dma_cyc)
        reg[CPU_HALT_STATE] = CPU_ACTIVE;   // DMA finished, resume execution.
      else
        execute_cycles = MIN(execute_cycles, dma_cyc);  // Continue sleeping.
    }

    for (i = 0; i < 4; i++)
    {
       if (timer[i].status == TIMER_PRESCALE && !TIMER_IS_LAZY(i) &&
           timer[i].count < execute_cycles)
          execute_cycles = timer[i].count;
    }
#ifdef GBA_LINK
    /* Halted: the loop jumps from event to event; stop at the run horizon
     * (or a blocked transfer) the same way the entry check does.  Resuming
     * calls update_gba(0), which is exactly the next iteration. */
    if (serial_mode == SERIAL_MODE_CABLE &&
        reg[CPU_HALT_STATE] != CPU_ACTIVE && !frame_complete &&
        gbalink_halt_yield_q())
    {
      gbalink_reseed_snapshot();
      return 0x80000000;
    }
#endif
  } while(reg[CPU_HALT_STATE] != CPU_ACTIVE && !frame_complete);

  /* The slice starts here: until an I/O access stores a later cycle,
   * reg[REG_IO_CYCLES] says "slice start" (live timer counters,
   * gba_memory.c).  The value is the cycle budget returned below. */
  reg[REG_IO_CYCLES] = execute_cycles - MIN(execute_cycles, MIN(64, dma_cycles));

#ifdef GBA_LINK
  if (serial_mode == SERIAL_MODE_CABLE)
    gbalink_reseed_snapshot();
#endif

  // We voluntarily limit this. It is not accurate but it would be much harder.
  dma_cycles = MIN(64, dma_cycles);
  dma_cycles = MIN(execute_cycles, dma_cycles);

  return (execute_cycles - dma_cycles) | changed_pc | frame_complete;
}

void reset_gba(void)
{
  gbp_reset();
  init_memory();
  init_main();
  init_cpu();
  reset_sound();
}

#ifdef TRACE_REGISTERS
void print_regs(void)
{
  printf("R0=%08x R1=%08x R2=%08x R3=%08x "
         "R4=%08x R5=%08x R6=%08x R7=%08x "
         "R8=%08x R9=%08x R10=%08x R11=%08x "
         "R12=%08x R13=%08x R14=%08x\n",
         reg[0], reg[1], reg[2], reg[3],
         reg[4], reg[5], reg[6], reg[7],
         reg[8], reg[9], reg[10], reg[11],
         reg[12], reg[13], reg[14]);
}
#endif

bool main_check_savestate(const u8 *src)
{
  int i;
  if (!bson_contains_key(src, "emu", BSON_TYPE_DOC) ||
      !bson_contains_key(src, "timers", BSON_TYPE_DOC)) return false;
  const u8 *p1 = bson_find_key(src, "emu");
  const u8 *p2 = bson_find_key(src, "timers");
  if (!p1 || !p2)
    return false;

  if (!bson_contains_key(p1, "cpu-ticks", BSON_TYPE_INT32) ||
      !bson_contains_key(p1, "exec-cycles", BSON_TYPE_INT32) ||
      !bson_contains_key(p1, "video-count", BSON_TYPE_INT32) ||
      !bson_contains_key(p1, "sleep-cycles", BSON_TYPE_INT32))
    return false;
  /* serial-irq-cycles is optional for forward compatibility with states
   * written before this field existed; missing simply means "no pending
   * serial IRQ", which is also the default after serialproto_reset. */

  {
    static const char *const optional[] = {
      "frame-count", "serial-irq-cycles", "rand-state", "gbp-state"
    };
    unsigned n;
    for (n = 0; n < sizeof(optional) / sizeof(optional[0]); n++)
      if (bson_find_key(p1, optional[n]) &&
          !bson_contains_key(p1, optional[n], BSON_TYPE_INT32)) return false;
  }

  for (i = 0; i < 4; i++)
  {
    char tname[2] = {'0' + i, 0};
    if (!bson_contains_key(p2, tname, BSON_TYPE_DOC)) return false;
    const u8 *p = bson_find_key(p2, tname);
    u32 prescale, status, dsc, irq, frequency_step, reload;
    if (!p)
      return false;

    if (!bson_contains_key(p, "count", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "reload", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "prescale", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "freq-step", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "dsc", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "irq", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "status", BSON_TYPE_INT32))
      return false;

    /* Hardware writes store 65536 - u16. Zero cannot advance an expired
     * timer (and loops forever with lazy catch-up enabled). */
    if (!bson_read_int32(p, "reload", &reload) || !reload || reload > 0x10000u ||
        !bson_read_int32(p, "prescale", &prescale) ||
        (prescale != 0 && prescale != 6 &&
         prescale != 8 && prescale != 10) ||
        !bson_read_int32(p, "status", &status) || status > TIMER_CASCADE ||
        !bson_read_int32(p, "dsc", &dsc) ||
        dsc > TIMER_DS_CHANNEL_BOTH ||
        !bson_read_int32(p, "irq", &irq) || irq > TIMER_TRIGGER_IRQ ||
        /* 0xFF000000 is the largest step with which sound_timer's
         * `fifo_fractional (<= 0xFFFFFF) += step` cannot wrap a u32; past
         * it the wrap makes that loop spin up to ~16M times per timer
         * event.  direct_sound_timer_step clamps to the same bound.  No
         * older writer exceeded it: the unclamped formula (base<<24) /
         * (rate * reload), for both base rates and both sound rates (2^15,
         * 2^16), wraps to <= 0xAAAAAAAA at every reload >= 1.  Keep. */
        !bson_read_int32(p, "freq-step", &frequency_step) ||
        frequency_step > 0xFF000000u)
      return false;
  }

  return true;
}

bool main_read_savestate(const u8 *src)
{
  int i;
  const u8 *p1 = bson_find_key(src, "emu");
  const u8 *p2 = bson_find_key(src, "timers");
  if (!p1 || !p2)
    return false;

  if (!(bson_read_int32(p1, "cpu-ticks", &cpu_ticks) &&
         bson_read_int32(p1, "exec-cycles", &execute_cycles) &&
         bson_read_int32(p1, "video-count", (u32*)&video_count) &&
         bson_read_int32(p1, "sleep-cycles", &reg[REG_SLEEP_CYCLES])))
    return false;

  if (!bson_read_int32(p1, "frame-count", &frame_counter))
    frame_counter = 60 * 10;  // Use "fake" 10 seconds.

  {
    u32 sirq;
    if (bson_read_int32(p1, "serial-irq-cycles", &sirq))
      serial_set_irq_cycles(sirq);
    else
      serial_set_irq_cycles(0);   /* Older states: no pending IRQ. */
  }

  /* random_state is also optional for backwards compat. Missing means
   * 'use whatever is currently in the static'; the RFU path will reseed
   * from cpu_ticks on the next rfu_reset, which is also deterministic. */
  bson_read_int32(p1, "rand-state", &random_state);

  /* gbp-state is optional for backwards compat.  Older states either
   * never had a GBP session active or are post-handshake (steady-state
   * loop where missing the precise gbp_seq_n is harmless within a few
   * frames).  Default: don't touch the in-memory values, gbp_reset
   * runs at content load and that's a safe starting point. */
  {
    u32 gbps;
    if (bson_read_int32(p1, "gbp-state", &gbps))
      gbp_set_state(gbps);
  }

  for (i = 0; i < 4; i++)
  {
    char tname[2] = {'0' + i, 0};
    const u8 *p = bson_find_key(p2, tname);
    /* The narrow fields (main.h) are read through a word; main_check_savestate
     * has bounded each value to its field. */
    u32 w;

    if (!(
      bson_read_int32(p, "count", (u32*)&timer[i].count) &&
      bson_read_int32(p, "reload", &timer[i].reload) &&
      bson_read_int32(p, "prescale", &w) && ((timer[i].prescale = (u8)w), 1) &&
      bson_read_int32(p, "freq-step", &timer[i].frequency_step) &&
      bson_read_int32(p, "dsc", &w) &&
      ((timer[i].direct_sound_channels = (u8)w), 1) &&
      bson_read_int32(p, "irq", &w) && ((timer[i].irq = (u8)w), 1) &&
      bson_read_int32(p, "status", &w) && ((timer[i].status = (u8)w), 1)))
      return false;
  }

  return true;
}

unsigned main_write_savestate(u8* dst)
{
  int i;
  u8 *wbptr, *wbptr2, *startp = dst;
  bson_start_document(dst, "emu", wbptr);
  bson_write_int32(dst, "frame-count", frame_counter);
  bson_write_int32(dst, "cpu-ticks", cpu_ticks);
  bson_write_int32(dst, "exec-cycles", execute_cycles);
  bson_write_int32(dst, "video-count", video_count);
  bson_write_int32(dst, "sleep-cycles", reg[REG_SLEEP_CYCLES]);
  bson_write_int32(dst, "serial-irq-cycles", serial_get_irq_cycles());
  bson_write_int32(dst, "rand-state", random_state);
  bson_write_int32(dst, "gbp-state", gbp_get_state());
  bson_finish_document(dst, wbptr);

  bson_start_document(dst, "timers", wbptr);
  for (i = 0; i < 4; i++)
  {
    char tname[2] = {'0' + i, 0};
    bson_start_document(dst, tname, wbptr2);
    bson_write_int32(dst, "count", timer[i].count);
    bson_write_int32(dst, "reload", timer[i].reload);
    bson_write_int32(dst, "prescale", timer[i].prescale);
    bson_write_int32(dst, "freq-step", timer[i].frequency_step);
    bson_write_int32(dst, "dsc", timer[i].direct_sound_channels);
    bson_write_int32(dst, "irq", timer[i].irq);
    bson_write_int32(dst, "status", timer[i].status);
    bson_finish_document(dst, wbptr2);
  }
  bson_finish_document(dst, wbptr);

  return (unsigned int)(dst - startp);
}
