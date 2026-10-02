/* video_me_log.h -- ME_MIDFRAME_LOG, included by video.cc only.
 *
 * The contract is in video.h; the design, costs and measurements are in
 * docs/ME-MIDFRAME.md.  Two halves:
 *
 *   APPLY   (every build with the flag, including the ME PRX): undo the log
 *           over the line-160 snapshot, then redo it line by line.
 *   RECORD  (host only, never in the PRX): build the log from the capture
 *           block of update_scanline and from the vcount-160 hook.
 *
 * Invariant the RECORD half keeps, and the APPLY half relies on: for every
 * logged address the entries form one chain in time order, each old value is
 * the previous entry's new value (or the line-0 value), and the last new
 * value is the line-160 value.  Undo-then-redo therefore leaves the engine's
 * copy exactly at the line-160 state whatever the entries are, and a log that
 * cannot keep the invariant (overflow) is dropped whole. */

#define MLT_LINE(t)   ((t) & 0xFF)
#define MLT_REGION(t) (((t) >> 8) & 3)
#define MLT_INDEX(t)  ((t) >> 10)

/* ---- APPLY ---------------------------------------------------------------*/

static inline void ml_store(u32 tag, u32 v)
{
  u32 i = MLT_INDEX(tag);
  switch (MLT_REGION(tag))
  {
    case ME_LOG_PAL:
      if (i < 512) palette_ram_converted[i] = (u16)v;
      break;
    case ME_LOG_OAM:
      if (i < 512) oam_ram[i] = (u16)v;
      break;
    case ME_LOG_VRAM:
      if (i < (1024 * 96) / 4) ((u32 *)vram)[i] = v;
      break;
    default:                                /* ME_LOG_OAMREP: no data */
      break;
  }
}

/* Walk back to the line-0 state.  Returns non-zero if OAM was involved. */
static int ml_undo(const me_log_entry *e, u32 n)
{
  int oam = 0;
  while (n--)
  {
    u32 r = MLT_REGION(e[n].tag);
    if (r == ME_LOG_OAM || r == ME_LOG_OAMREP)
      oam = 1;
    ml_store(e[n].tag, e[n].old_v);
  }
  return oam;
}

/* Apply every entry of `line` (the log is in line order).  Returns the new
 * cursor. */
static u32 ml_redo_line(const me_log_entry *e, u32 n, u32 k, u32 line)
{
  while (k < n && MLT_LINE(e[k].tag) == line)
  {
    u32 r = MLT_REGION(e[k].tag);
    if (r == ME_LOG_OAM || r == ME_LOG_OAMREP)
      reg[OAM_UPDATED] = 1;                 /* re-sort OBJs before this line */
    ml_store(e[k].tag, e[k].new_v);
    k++;
  }
  return k;
}

/* ---- RECORD (host) -------------------------------------------------------*/
#ifndef ME_PRX_BUILD

u8 *me_vram_shadow = NULL;
me_log_stats_t me_log_stats;      /* work counters (telemetry, the model) */

#if ME_TIMING_SIM
/* Desktop cost measurement only: wall time inside the recorder. */
#include <time.h>
unsigned long long me_log_ns;
static unsigned long long ml_now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (unsigned long long)t.tv_sec * 1000000000ull + t.tv_nsec;
}
#define ML_T0() unsigned long long ml_t0 = ml_now()
#define ML_T1() (me_log_ns += ml_now() - ml_t0)
#else
#define ML_T0() do {} while (0)
#define ML_T1() do {} while (0)
#endif

static struct MidframeLog {
  int on;                     /* this frame is being logged                  */
  me_log_entry *buf;
  u32 cap, n, flags;
  int vram_hold;              /* hysteresis: frames of per-line VRAM tracking */
  int pal_hit, vram_hit;      /* this frame changed palette / relevant VRAM  */
  int oam_logged;
  int just_merged;            /* map merged at 160, no emulation since       */
  int resync;                 /* palette/OAM shadows must be recopied        */
  u16 pal[512];               /* palette as the last check saw it            */
  u16 oam[512];
  u8  map0[96];               /* the dirty map as line 0 found it            */
  u8  vis[96];                /* 0 = page reset by a per-line check          */
  u8  valid[96];              /* me_vram_shadow page == VRAM as last checked */
  u32 rel[3];                 /* pages this line's layers read (bitmask)     */
  u16 relkey[5];              /* DISPCNT, BG0-3CNT the mask was built from   */
} g_ml;

static inline int ml_bit(const u32 *m, u32 p) { return (m[p >> 5] >> (p & 31)) & 1; }

static void ml_set_range(u32 *m, u32 lo, u32 hi)    /* bytes [lo, hi) */
{
  u32 p;
  for (p = lo >> 10; p < 96 && (p << 10) < hi; p++)
    m[p >> 5] |= 1u << (p & 31);
}

/* Which 1 KiB VRAM pages can this line's picture depend on?  Tiled modes: all
 * of BG VRAM when any BG is on (text-BG tile fetches are not clipped to the
 * char block in video.cc and can run past 0x10000), OBJ VRAM when OBJs are on.
 * Bitmap modes: only the DISPLAYED frame (so drawing the back page is not a
 * mid-frame change) and the upper OBJ half, the only OBJ tiles video.cc draws
 * there (order_obj skips tiles < 512). */
static void ml_relevance(const u16 *r, u32 *m)
{
  u32 disp = eswap16(r[REG_DISPCNT]);
  u32 mode = disp & 7;
  m[0] = m[1] = m[2] = 0;
  if (disp & 0x80)
    return;                                 /* forced blank: reads nothing */
  if (mode <= 2)
  {
    if (disp & 0x0F00) ml_set_range(m, 0, 0x18000);
    else if (disp & 0x1000) ml_set_range(m, 0x10000, 0x18000);
    return;
  }
  if (mode > 5)
  {
    ml_set_range(m, 0, 0x18000);            /* invalid mode: assume anything */
    return;
  }
  if (disp & 0x0400)
  {
    u32 base = (mode != 3 && (disp & 0x10)) ? 0xA000 : 0;
    u32 size = mode == 3 ? 0x12C00 : mode == 4 ? 0x9600 : 0xA000;
    ml_set_range(m, base, base + size);
  }
  if (disp & 0x1000)
    ml_set_range(m, 0x14000, 0x18000);
}

static inline void ml_push(u32 line, u32 region, u32 idx, u32 o, u32 nv)
{
  if (g_ml.n < g_ml.cap)
  {
    me_log_entry *e = &g_ml.buf[g_ml.n++];
    e->tag = line | (region << 8) | (idx << 10);
    e->old_v = o;
    e->new_v = nv;
  }
  else
    g_ml.flags |= ME_LOGF_OVERFLOW;
}

static void ml_resync_page(u32 p)
{
  me_log_stats.page_copies++;
  memcpy(me_vram_shadow + (p << 10), vram + (p << 10), 1024);
  g_ml.valid[p] = 1;
}

/* Recompute the relevance mask if this line's layer setup changed; a page
 * that just became relevant is recopied first, so its old values are right
 * from here on (what it held before this line was not on screen). */
static void ml_update_relevance(void)
{
  static const u8 keys[5] = { REG_DISPCNT, REG_BG0CNT, REG_BG1CNT, REG_BG2CNT,
                              REG_BG3CNT };
  u32 old[3], k, p;
  int same = 1;
  for (k = 0; k < 5; k++)
    if (g_ml.relkey[k] != io_registers[keys[k]])
    {
      g_ml.relkey[k] = io_registers[keys[k]];
      same = 0;
    }
  if (same)
    return;
  old[0] = g_ml.rel[0]; old[1] = g_ml.rel[1]; old[2] = g_ml.rel[2];
  ml_relevance(io_registers, g_ml.rel);
  if (!me_vram_shadow)
    return;
  for (p = 0; p < 96; p++)
    if (ml_bit(g_ml.rel, p) && !ml_bit(old, p) && !g_ml.valid[p])
      ml_resync_page(p);
}

static void ml_check_palette(u32 line)
{
  me_log_stats.pal_checks++;
  const u32 *cur = (const u32 *)palette_ram_converted;
  u32 *sh = (u32 *)g_ml.pal;
  u32 w;
  for (w = 0; w < 256; w++)
    if (sh[w] != cur[w])
    {
      u16 *s16 = (u16 *)&sh[w];
      const u16 *c16 = (const u16 *)&cur[w];
      int h;
      for (h = 0; h < 2; h++)
        if (s16[h] != c16[h])
        {
          ml_push(line, ME_LOG_PAL, w * 2 + h, s16[h], c16[h]);
          s16[h] = c16[h];
        }
      g_ml.pal_hit = 1;
    }
}

static void ml_check_oam(u32 line)
{
  me_log_stats.oam_checks++;
  const u32 *cur = (const u32 *)oam_ram;
  u32 *sh = (u32 *)g_ml.oam;
  u32 w;
  ml_push(line, ME_LOG_OAMREP, 0, 0, 0);    /* the CPU renderer re-sorts here */
  for (w = 0; w < 256; w++)
    if (sh[w] != cur[w])
    {
      u16 *s16 = (u16 *)&sh[w];
      const u16 *c16 = (const u16 *)&cur[w];
      int h;
      for (h = 0; h < 2; h++)
        if (s16[h] != c16[h])
        {
          ml_push(line, ME_LOG_OAM, w * 2 + h, s16[h], c16[h]);
          s16[h] = c16[h];
        }
    }
  g_ml.oam_logged = 1;
}

/* Per-line VRAM check (only while tracking is active): every page the map
 * says was written since the last check is either diffed into the log
 * (relevant, shadow valid) or just invalidated (not on screen now). */
static void ml_check_vram(u32 line)
{
  me_log_stats.map_scans++;
  const u32 *mw = (const u32 *)vram_clean;
  u32 q;
  for (q = 0; q < 96 / 4; q++)
  {
    u32 p;
    if (mw[q] == 0x01010101u)
      continue;                             /* four clean pages */
    for (p = q * 4; p < q * 4 + 4; p++)
    {
      if (vram_clean[p])
        continue;
      vram_clean[p] = 1;
      g_ml.vis[p] = 0;
      if (ml_bit(g_ml.rel, p) && g_ml.valid[p])
      {
        me_log_stats.page_diffs++;
        u32 *sh = (u32 *)(me_vram_shadow + (p << 10));
        const u32 *cur = (const u32 *)(vram + (p << 10));
        u32 w;
        for (w = 0; w < 256; w++)
          if (sh[w] != cur[w])
          {
            ml_push(line, ME_LOG_VRAM, (p << 8) + w, sh[w], cur[w]);
            sh[w] = cur[w];
            g_ml.vram_hit = 1;
          }
      }
      else if (ml_bit(g_ml.rel, p))
        ml_resync_page(p);                  /* cannot happen; stay correct */
      else
        g_ml.valid[p] = 0;
    }
  }
}

/* Capture block, line 0, BEFORE reg[OAM_UPDATED] is latched and cleared. */
static void ml_frame_start_body(void);
static void ml_frame_start(void)
{
  ML_T0();
  ml_frame_start_body();
  ML_T1();
}
static void ml_frame_start_body(void)
{
  u32 p;
  g_ml.on = me_capture_buf->log_cap && me_capture_buf->log_addr;
  if (!g_ml.on)
    return;
  g_ml.buf = (me_log_entry *)me_capture_buf->log_addr;
  g_ml.cap = me_capture_buf->log_cap;
  g_ml.n = 0;
  g_ml.flags = 0;
  g_ml.pal_hit = g_ml.vram_hit = g_ml.oam_logged = 0;
  /* Line-0 baselines.  The shadows are kept current by every logged line
   * and by the line-160 check, so only writes since then (the flags) or a
   * gap in logging (setup; a frameskipped frame leaves the flags set) need
   * a recopy.  Store paths that bypass the flags (state load) set them. */
  me_log_stats.frames++;
  if (g_ml.vram_hold && me_vram_shadow)
    me_log_stats.tracked_frames++;
  if (g_ml.resync || reg[PAL_UPDATED])
  {
    memcpy(g_ml.pal, palette_ram_converted, sizeof(g_ml.pal));
    me_log_stats.base_copies++;
  }
  if (g_ml.resync || reg[OAM_UPDATED])
  {
    memcpy(g_ml.oam, oam_ram, sizeof(g_ml.oam));
    me_log_stats.base_copies++;
  }
  g_ml.resync = 0;
  reg[PAL_UPDATED] = 0;             /* from here on: this frame's writes */

  /* Split the dirty map: what is marked now was written before line 0 (the
   * VBlank, or since the last post); from here on, marks are this frame's
   * visible lines.  me_capture_visible_end() merges the two back. */
  memcpy(g_ml.map0, vram_clean, sizeof(g_ml.map0));
  {
    const u32 *mw = (const u32 *)vram_clean;
    u32 q;
    for (q = 0; q < 96 / 4; q++)
      if (mw[q] != 0x01010101u)
        for (p = q * 4; p < q * 4 + 4; p++)
          if (!vram_clean[p])
            g_ml.valid[p] = 0;
  }
  memset(vram_clean, 1, sizeof(g_ml.map0));
  vram_clean[VRAM_DIRTY_ANY] = 1;
  memset(g_ml.vis, 1, sizeof(g_ml.vis));
  for (p = 0; p < 5; p++)
    g_ml.relkey[p] = 0xFFFF;                /* force a fresh mask */
  g_ml.rel[0] = g_ml.rel[1] = g_ml.rel[2] = 0;
  if (g_ml.vram_hold && me_vram_shadow)
  {
    g_ml.flags |= ME_LOGF_VRAM_ON;
    ml_update_relevance();                  /* resyncs invalid relevant pages */
  }
}

/* Capture block, lines 1-159 (and 160 from the hook), before the render. */
static void ml_line_body(u32 line);
static void ml_line(u32 line)
{
  ML_T0();
  ml_line_body(line);
  ML_T1();
}
static void ml_line_body(u32 line)
{
  if (!g_ml.on)
    return;
  if (reg[OAM_UPDATED])
  {
    ml_check_oam(line);
    /* Mode 1 never renders, so nothing else consumes the flag: clear it so
     * the next line only sees new writes.  Mode 2's renderer clears it
     * itself after this; line 160 leaves it for the next frame's line 0. */
    if (me_capture_mode == 1 && line < 160)
      reg[OAM_UPDATED] = 0;
  }
  if (reg[PAL_UPDATED])             /* set by every palette store path */
  {
    reg[PAL_UPDATED] = 0;
    ml_check_palette(line);
  }
  if (g_ml.vram_hold && me_vram_shadow)
  {
    ml_update_relevance();          /* every line: a page can come on screen */
    if (!vram_clean[VRAM_DIRTY_ANY])/* one load unless something was written */
    {
      vram_clean[VRAM_DIRTY_ANY] = 1;
      ml_check_vram(line);
    }
  }
}

/* Right after me_capture_visible_end() (the vcount-160 post) every marked
 * page's shadow validity is already exact.  Any later clear (the FF profiles
 * post at frame end, after the VBlank has written) must invalidate what it
 * clears, or the next tracked frame would diff against a stale shadow. */
void me_vram_map_consumed(void)
{
  u32 p;
  for (p = 0; p < 96; p++)
    if (!vram_clean[p])
    {
      if (!g_ml.just_merged)
        g_ml.valid[p] = 0;
      vram_clean[p] = 1;
    }
}

void me_capture_log_setup(void *capv, void *entries, u32 n_entries)
{
  me_capture_frame *c = (me_capture_frame *)capv;
  c->log_addr  = (uintptr_t)entries;
  c->log_cap   = entries ? n_entries : 0;
  c->log_n     = 0;
  c->log_flags = 0;
  /* A (re)built engine: the shadow's memory may be new or may have been
   * handed back and reclaimed (standby); trust none of it. */
  memset(g_ml.valid, 0, sizeof(g_ml.valid));
  g_ml.resync = 1;
}

/* main.c, right after gpsp_visible_done_hook(): emulation resumes. */
void me_capture_visible_resume(void)
{
  g_ml.just_merged = 0;
}

static void ml_visible_end_body(void);
void me_capture_visible_end(void)
{
  ML_T0();
  ml_visible_end_body();
  ML_T1();
}
static void ml_visible_end_body(void)
{
  u32 rel_all[3] = { 0, 0, 0 };
  u32 p, ln;
  int vram_seen = 0;
  int live;
  if (!g_ml.on)
    return;
  /* Capture may have been torn down mid-frame: the map still has to be
   * merged back, but the capture and log buffers are no longer ours. */
  live = me_capture_mode && me_capture_buf;

  if (live)
    ml_line(160);                           /* writes after line 159's render */
  g_ml.on = 0;                              /* (ml_line needs it set) */


  /* VRAM, frame level: pages written during the visible lines that some
   * line of this frame reads.  Most frames wrote none: skip the union. */
  {
    const u32 *mw = (const u32 *)vram_clean, *vw = (const u32 *)g_ml.vis;
    u32 q, any = 0;
    for (q = 0; q < 96 / 4; q++)
      any |= (mw[q] & vw[q]) ^ 0x01010101u;
    if (!any)
    {
      /* Nothing written since line 0: the map is exactly map0 again. */
      memcpy(vram_clean, g_ml.map0, sizeof(g_ml.map0));
      goto map_done;
    }
  }
  if (live)
  {
    u16 key = 0xFFFF;
    u32 m[3];
    for (ln = 0; ln < 160; ln++)
    {
      const u16 *r = me_capture_buf->ioregs[ln];
      u16 k = r[REG_DISPCNT] ^ (u16)(r[REG_BG0CNT] * 3) ^ (u16)(r[REG_BG1CNT] * 5)
              ^ (u16)(r[REG_BG2CNT] * 7) ^ (u16)(r[REG_BG3CNT] * 11);
      if (ln && k == key)
        continue;
      key = k;
      ml_relevance(r, m);
      rel_all[0] |= m[0]; rel_all[1] |= m[1]; rel_all[2] |= m[2];
    }
  }
  for (p = 0; p < 96; p++)
  {
    int vis = !vram_clean[p] || !g_ml.vis[p];
    if (!vram_clean[p])
      g_ml.valid[p] = 0;                    /* written, not diffed */
    if (vis && ml_bit(rel_all, p))
      vram_seen = 1;
    /* Hand the ME back every page dirtied since its last copy. */
    vram_clean[p] &= g_ml.map0[p] & g_ml.vis[p];
  }
  if (vram_seen)
    g_ml.vram_hit = 1;
map_done:

  /* Hysteresis. */
  if (g_ml.pal_hit)
    g_ml.flags |= ME_LOGF_PAL;
  if (g_ml.vram_hit && me_vram_shadow)
    g_ml.vram_hold = ME_LOG_HOLD;
  else if (g_ml.vram_hold)
    g_ml.vram_hold--;

  if (g_ml.oam_logged)
    reg[OAM_UPDATED] = 1;   /* next line 0 re-sorts even if the log is dropped */

  g_ml.just_merged = 1;
  if (!live)
    return;
  if (g_ml.flags & ME_LOGF_OVERFLOW)
  {
    me_log_stats.overflows++;
    g_ml.n = 0;
  }
  me_log_stats.entries += g_ml.n;
  me_capture_buf->log_n = g_ml.n;
  me_capture_buf->log_flags = g_ml.flags;
}

#define MFL_FRAME_START() ml_frame_start()
#define MFL_LINE(vc)      ml_line(vc)
#else
#define MFL_FRAME_START() do {} while (0)
#define MFL_LINE(vc)      do {} while (0)
#endif /* ME_PRX_BUILD */
