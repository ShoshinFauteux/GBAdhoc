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

timer_type timer[4];

u32 frame_counter = 0;
u32 cpu_ticks = 0;
u32 execute_cycles = 0;
s32 video_count = 0;

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
  smc_prof_hits_iw++;
  smc_prof_bump((offset & 0x7FFF) >> SMC_PROF_PAGE_SHIFT);
  smc_prof_exact(0x03000000 + (offset & 0x7FFF));
}

void smc_prof_note_ewram(u32 offset)
{
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
    smc_prof_note_iwram(off);
    /* Phase 5g: reg[REG_PC] was stored by the smc_write stub from reg_a2,
     * which the store emitter loads with (pc + 4) for ARM and (pc + 2) for
     * Thumb -- i.e. the GBA PC of the instruction doing the write.  Only the
     * CPU-store path reaches here; DMA writes go through write_io_epilogue and
     * are already counted separately as flush_ram_dma. */
    smc_blk_note_writer(0x03000000 + (off & 0x7FFF), reg[REG_PC]);
    smc_blk_check_silent(0x03000000 + (off & 0x7FFF));
  }
  else if (lui == smc_prof_lui_ewram)
  {
    smc_prof_note_ewram(off);
    smc_blk_note_writer(0x02000000 + (off & 0x3FFFF), reg[REG_PC]);
    smc_blk_check_silent(0x02000000 + (off & 0x3FFFF));
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
u32 smc_blk_watch = SMC_BLK_WATCH_DEFAULT;
u32 smc_blk_xlat  = 0;   /* RAM blocks translated since the last take */

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
u32 core_phase_lvl    = CORE_PHASE_OFF;
u32 core_phase_clk_ns = 0;

u32 cph_vid, cph_amix, cph_dsnd, cph_jit;
u32 cph_reads;
u32 cph_bkr, cph_bkw;
u32 cph_rfux, cph_rfut;   /* ADR-0051: rfu_transfer() calls / us this frame */
u32 cph_ugba, cph_dman, cph_stmr;

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
  *acc += smc_prof_clock() - t0;
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
  init_dynarec_caches();
  init_emitter(gamepak_must_swap());
#endif
}

u32 function_cc update_gba(int remaining_cycles)
{
  u32 changed_pc = 0;
  u32 frame_complete = 0;
  irq_type irq_raised = IRQ_NONE;
  int dma_cycles;
  trace_update_gba(remaining_cycles);

  remaining_cycles = MAX(remaining_cycles, -64);

  do
  {
    unsigned i;
    // Number of cycles we ask to run - cycles that we did not execute
    // (remaining_cycles can be negative and should be close to zero)
    unsigned completed_cycles = execute_cycles - remaining_cycles;
    cph_ugba++;   /* phase 5h: dynarec -> C round trips per frame */
    cpu_ticks += completed_cycles;

    remaining_cycles = 0;

    // Timers can trigger DMA (usually sound) and consume cycles
    dma_cycles = update_timers(&irq_raised, completed_cycles);
    // Check for serial port IRQs as well.
    if (update_serial(completed_cycles))
      irq_raised |= IRQ_SERIAL;

    // Video count tracks the video cycles remaining until the next event
    video_count -= completed_cycles;

    // Ran out of cycles, move to the next video area
    if(video_count <= 0)
    {
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
          for (i = 0; i < 4; i++)
          {
            if(dma[i].start_type == DMA_START_HBLANK)
              dma_transfer(i, &dma_cycles);
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

          // Reinit affine transformation counters for the next frame
          video_reload_counters();

          // Trigger VBlank interrupt if enabled
          if (dispstat & 0x8)
            irq_raised |= IRQ_VBLANK;

          // Trigger the VBlank DMAs if enabled
          for (i = 0; i < 4; i++)
          {
            if(dma[i].start_type == DMA_START_VBLANK)
              dma_transfer(i, &dma_cycles);
          }
        }
        else if (vcount == 228)
        {
          // Transition from vblank to next screen
          vcount = 0;
          dispstat &= ~0x01;

          /* If there's no cheat hook, run on vblank! */
          if (cheat_master_hook == ~0U)
             process_cheats();

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

    // Flag any V/H blank interrupts, DMA IRQs, Vcount, etc.
    if (irq_raised)
      flag_interrupt(irq_raised);

    // Raise any pending interrupts. This changes the CPU mode.
    if (check_and_raise_interrupts())
      changed_pc = 0x40000000;

    // Figure out when we need to stop CPU execution. The next event is
    // a video event or a timer event, whatever happens first.
    execute_cycles = MAX(video_count, 0);
    {
      u32 cc = serial_next_event();
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
       if (timer[i].status == TIMER_PRESCALE &&
           timer[i].count < execute_cycles)
          execute_cycles = timer[i].count;
    }
  } while(reg[CPU_HALT_STATE] != CPU_ACTIVE && !frame_complete);

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

  for (i = 0; i < 4; i++)
  {
    char tname[2] = {'0' + i, 0};
    const u8 *p = bson_find_key(p2, tname);
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

    if (!(
      bson_read_int32(p, "count", (u32*)&timer[i].count) &&
      bson_read_int32(p, "reload", &timer[i].reload) &&
      bson_read_int32(p, "prescale", &timer[i].prescale) &&
      bson_read_int32(p, "freq-step", &timer[i].frequency_step) &&
      bson_read_int32(p, "dsc", &timer[i].direct_sound_channels) &&
      bson_read_int32(p, "irq", &timer[i].irq) &&
      bson_read_int32(p, "status", &timer[i].status)))
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

