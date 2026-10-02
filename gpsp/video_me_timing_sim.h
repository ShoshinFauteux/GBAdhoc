/* video_me_timing_sim.h -- desktop model of the Media Engine renderer's
 * PRODUCTION timing.  Included by video.cc only when -DME_TIMING_SIM=1; never
 * part of a PSP or PRX build.
 *
 * ME_CAP_VALIDATE proved the capture struct bit-exact, but it snapshots the
 * graphics memory at line 0 and SKIPS every frame whose memory changes before
 * line 159.  Production does neither: psp/main_psp.c posts the render from
 * plat_video_frame, i.e. after retro_run has emulated the whole of VBlank, and
 * the ME snapshots vram/oam/palette at that moment.  This models exactly that,
 * next to the alternative of posting at vcount 160, and compares both against
 * the frame the CPU renderer actually drew.
 *
 * Run with me_capture_mode == 2 (sdl/gpsp_sdl --me-capture-test): the core
 * captures AND renders, so the CPU frame is the reference.  Two simulated
 * engines each keep their own persistent renderer state (the ME's order_obj
 * lists survive between frames exactly as they do in the PRX):
 *
 *   sim 0 "post"  replayed at frame end (production today)
 *   sim 1 "vis"   replayed at the vcount-160 hook (the candidate fix)
 *
 * reg[OAM_UPDATED] is reproduced with PRODUCTION semantics: in capture mode 1
 * the core never renders, so the flag accumulates every OAM write since the
 * previous line 0.  Mode 2's own renderer consumes it line by line, so a
 * sticky copy is kept here and handed to the capture instead.
 *
 * Output (stderr): one "MTS f=.. cpu=.. post=.. vis=.." line per frame, plus a
 * "MTSD" line naming what changed between line 160 and frame end for every
 * frame whose post-time render differs from the CPU frame.  MTS_DUMP_DIR, if
 * set, receives raw 240x160 RGB565 images of those frames (cpu/post/vis). */
#include <cstdio>
#include <cstdlib>
#include <cstring>

static struct MeTimingSim {
  struct Eng {
    u8  opl[5][160][128];
    u8  opc[5][160];
    u8  oac[160];
    u32 lo[16], lc;
    u16 out[GBA_SCREEN_PITCH * 161];
  } eng[2];
  u32 oam_sticky;
  int in_replay, have_vis;
  unsigned long frame;
  unsigned cpu_hash, vis_hash;
  u16 cpu_px[GBA_SCREEN_PITCH * 161];
  u8  vram160[1024 * 96];
  u8  vram_end[1024 * 96];            /* VRAM at the previous frame end */
  char vis_pages[160];                /* pages written while lines 0-159 drew */
  u16 oam160[512], pal160[512];
  long frames, post_bad, vis_bad, dumps;
  /* WHAT CHANGED WHILE LINES 0-159 DREW (classification, desktop-only cost).
   * First line whose render sees palette / OAM / VRAM differ from line 0, and
   * how many lines the affine reference was reloaded on (i.e. differs from
   * where the previous line's PB/PD step left it). */
  u16 pal0[512], oam0[512];
  u8  vram0[1024 * 96];
  int pal_first, oam_first, vram_first, aff_reloads;
  long n_pal, n_oam, n_vram, n_aff;
  long bad_pal, bad_oam, bad_vram, bad_aff, bad_none;
  long drift;                         /* replays that left the arrays changed */
  long log_frames, log_overflow, log_entries, log_max, log_vram_on;
  unsigned long long render_ns;       /* CPU renderer's own lines, mode 2 */
} g_mts;

static unsigned mts_fnv(const u16 *px)
{
  unsigned h = 2166136261u;
  for (int y = 0; y < 160; y++)
    for (int x = 0; x < 240; x++)
    { h ^= px[y * GBA_SCREEN_PITCH + x]; h *= 16777619u; }
  return h;
}

/* Replay the capture into eng[e].out exactly as psp/me/me_render_glue.cc
 * does, against whatever vram/oam/palette are live right now. */
static unsigned mts_replay(int e)
{
  MeTimingSim::Eng *g = &g_mts.eng[e];
  static u16 sv_io[512];  memcpy(sv_io, io_registers, sizeof(sv_io));
  static u8  sv_opl[5][160][128]; memcpy(sv_opl, obj_priority_list, sizeof(sv_opl));
  static u8  sv_opc[5][160];      memcpy(sv_opc, obj_priority_count, sizeof(sv_opc));
  static u8  sv_oac[160];         memcpy(sv_oac, obj_alpha_count, sizeof(sv_oac));
  static u32 sv_lo[16];           memcpy(sv_lo, layer_order, sizeof(sv_lo));
  s32 sx0=affine_reference_x[0], sx1=affine_reference_x[1];
  s32 sy0=affine_reference_y[0], sy1=affine_reference_y[1];
  u32 sou = reg[OAM_UPDATED], slc = layer_count, smode = me_capture_mode;
  int ssl = sprite_limit, ssk = skip_next_frame;
  u16 *ssp = gba_screen_pixels;

  g_mts.in_replay = 1;
  me_capture_mode = 0;
  memcpy(obj_priority_list, g->opl, sizeof(g->opl));
  memcpy(obj_priority_count, g->opc, sizeof(g->opc));
  memcpy(obj_alpha_count, g->oac, sizeof(g->oac));
  memcpy(layer_order, g->lo, sizeof(g->lo));
  layer_count = g->lc;

  sprite_limit = 1;
  skip_next_frame = 0;
  gba_screen_pixels = g->out;
  affine_reference_x[0] = me_capture_buf->affine_seed[0];
  affine_reference_x[1] = me_capture_buf->affine_seed[1];
  affine_reference_y[0] = me_capture_buf->affine_seed[2];
  affine_reference_y[1] = me_capture_buf->affine_seed[3];
  reg[OAM_UPDATED] = me_capture_buf->oam_updated;
  /* The log is only valid over the line-160 state: engine 1 (vcount-160
   * post) uses it, engine 0 (frame-end post) must not -- as in production. */
  me_replay_lines(me_capture_buf, e == 1 ? &g_mts.aff_reloads : NULL, e == 1);

  memcpy(g->opl, obj_priority_list, sizeof(g->opl));
  memcpy(g->opc, obj_priority_count, sizeof(g->opc));
  memcpy(g->oac, obj_alpha_count, sizeof(g->oac));
  memcpy(g->lo, layer_order, sizeof(g->lo));
  g->lc = layer_count;

  memcpy(io_registers, sv_io, sizeof(sv_io));
  memcpy(obj_priority_list, sv_opl, sizeof(sv_opl));
  memcpy(obj_priority_count, sv_opc, sizeof(sv_opc));
  memcpy(obj_alpha_count, sv_oac, sizeof(sv_oac));
  memcpy(layer_order, sv_lo, sizeof(sv_lo));
  affine_reference_x[0]=sx0; affine_reference_x[1]=sx1;
  affine_reference_y[0]=sy0; affine_reference_y[1]=sy1;
  reg[OAM_UPDATED]=sou; layer_count=slc; gba_screen_pixels=ssp;
  sprite_limit = ssl; skip_next_frame = ssk;
  me_capture_mode = smode;
  g_mts.in_replay = 0;
  return mts_fnv(g->out);
}

/* Called from the capture block for every captured line, BEFORE the CPU's
 * own render can consume reg[OAM_UPDATED]. */
static void mts_capture_line(unsigned vcount)
{
  if (g_mts.in_replay)
    return;
  /* Mode 1 (--me-capture-mode1) IS production: the capture block already
   * latched and cleared the flag.  Only mode 2 needs the sticky copy. */
  if (me_capture_mode == 2)
    g_mts.oam_sticky |= reg[OAM_UPDATED];
  if (vcount == 0)
  {
    if (me_capture_mode == 2)
      me_capture_buf->oam_updated = g_mts.oam_sticky;
    g_mts.oam_sticky = 0;
    memcpy(g_mts.pal0, palette_ram_converted, sizeof(g_mts.pal0));
    memcpy(g_mts.oam0, oam_ram, sizeof(g_mts.oam0));
    memcpy(g_mts.vram0, vram, sizeof(g_mts.vram0));
    g_mts.pal_first = g_mts.oam_first = g_mts.vram_first = -1;
    return;
  }
  if (g_mts.pal_first < 0 && memcmp(g_mts.pal0, palette_ram_converted, sizeof(g_mts.pal0)))
    g_mts.pal_first = (int)vcount;
  if (g_mts.oam_first < 0 && memcmp(g_mts.oam0, oam_ram, sizeof(g_mts.oam0)))
    g_mts.oam_first = (int)vcount;
  if (g_mts.vram_first < 0 && memcmp(g_mts.vram0, vram, sizeof(g_mts.vram0)))
    g_mts.vram_first = (int)vcount;
}

static void mts_dump(const char *tag, const u16 *px)
{
  const char *dir = getenv("MTS_DUMP_DIR");
  char path[512];
  FILE *f;
  if (!dir)
    return;
  {
    /* MTS_DUMP_FRAMES=A-B limits the dumps to frames A..B. */
    const char *r = getenv("MTS_DUMP_FRAMES");
    unsigned long a = 0, b = ~0ul;
    if (r && sscanf(r, "%lu-%lu", &a, &b) >= 1 && (g_mts.frame < a || g_mts.frame > b))
      return;
  }
  snprintf(path, sizeof(path), "%s/f%06lu_%s.raw", dir, g_mts.frame, tag);
  if (!(f = fopen(path, "wb")))
    return;
  for (int y = 0; y < 160; y++)
    fwrite(px + y * GBA_SCREEN_PITCH, 2, 240, f);
  fclose(f);
}

/* vcount 160: frame N is complete on the CPU and in the capture. */
extern "C" void gpsp_visible_done_hook(void)
{
  if (!me_capture_mode || !me_capture_buf || skip_next_frame)
  {
    g_mts.have_vis = 0;
    return;
  }
  g_mts.frame++;
  /* Writes after line 159's render (its HBlank DMA/IRQ) draw nothing on the
   * CPU but ARE in the memory the engine copies here: count them as line 160. */
  if (g_mts.pal_first < 0 && memcmp(g_mts.pal0, palette_ram_converted, sizeof(g_mts.pal0)))
    g_mts.pal_first = 160;
  if (g_mts.oam_first < 0 && memcmp(g_mts.oam0, oam_ram, sizeof(g_mts.oam0)))
    g_mts.oam_first = 160;
  if (g_mts.vram_first < 0 && memcmp(g_mts.vram0, vram, sizeof(g_mts.vram0)))
    g_mts.vram_first = 160;
  /* In mode 1 the CPU drew nothing: there is no reference, only the model's
   * images, which a mode-2 run of the same deterministic input must match. */
  if (me_capture_mode == 2)
    memcpy(g_mts.cpu_px, gba_screen_pixels, sizeof(g_mts.cpu_px));
  else
    memset(g_mts.cpu_px, 0, sizeof(g_mts.cpu_px));
  g_mts.cpu_hash = mts_fnv(g_mts.cpu_px);
  memcpy(g_mts.vram160, vram, sizeof(g_mts.vram160));
  {
    int n = 0, pl = 0;
    g_mts.vis_pages[0] = 0;
    for (int p = 0; p < 96; p++)
      if (memcmp(g_mts.vram_end + p * 1024, vram + p * 1024, 1024) && pl < 150)
        pl += snprintf(g_mts.vis_pages + pl, sizeof(g_mts.vis_pages) - pl,
                       "%s%d", n++ ? "," : "", p);
  }
  memcpy(g_mts.oam160, oam_ram, sizeof(g_mts.oam160));
  memcpy(g_mts.pal160, palette_ram_converted, sizeof(g_mts.pal160));
  g_mts.vis_hash = mts_replay(1);
  /* The replay runs on the LIVE arrays.  Whatever a mid-frame log did to
   * them must be undone exactly -- on the PSP the same drift would leave the
   * engine's persistent VRAM mirror different from the core's. */
  if (memcmp(g_mts.vram160, vram, sizeof(g_mts.vram160)) ||
      memcmp(g_mts.oam160, oam_ram, sizeof(g_mts.oam160)) ||
      memcmp(g_mts.pal160, palette_ram_converted, sizeof(g_mts.pal160)))
  {
    g_mts.drift++;
#if ME_MIDFRAME_LOG
    if (g_mts.drift <= 3)
    {
      const u32 *a = (const u32 *)g_mts.vram160, *b = (const u32 *)vram;
      const me_log_entry *e = (const me_log_entry *)me_capture_buf->log_addr;
      u32 w, i, shown = 0;
      for (w = 0; w < 24576 && shown < 3; w++)
        if (a[w] != b[w])
        {
          shown++;
          fprintf(stderr, "MTS_DRIFT word=%u want=%08x have=%08x log_n=%u flags=%x:",
                  w, a[w], b[w], me_capture_buf->log_n, me_capture_buf->log_flags);
          for (i = 0; i < me_capture_buf->log_n; i++)
            if ((e[i].tag >> 8 & 3) == 2 && (e[i].tag >> 10) == w)
              fprintf(stderr, " [l%u %08x>%08x]", e[i].tag & 255, e[i].old_v, e[i].new_v);
          fprintf(stderr, "\n");
        }
    }
#endif
    fprintf(stderr, "MTS_DRIFT f=%lu vram=%d oam=%d pal=%d\n", g_mts.frame,
            memcmp(g_mts.vram160, vram, sizeof(g_mts.vram160)) != 0,
            memcmp(g_mts.oam160, oam_ram, sizeof(g_mts.oam160)) != 0,
            memcmp(g_mts.pal160, palette_ram_converted, sizeof(g_mts.pal160)) != 0);
  }
  g_mts.have_vis = 1;
#if ME_MIDFRAME_LOG
  /* The engine has "copied" the dirty pages: clear the map exactly where
   * main_psp.c's vcount-160 post does, so the log's shadow bookkeeping runs
   * the production path (pages stay valid across frames) and not the
   * never-cleared-map path this twin would otherwise take. */
  me_vram_map_consumed();
#endif
}

/* Frame end: where psp/main_psp.c's plat_video_frame posts today. */
extern "C" void me_timing_sim_frame_end(void)
{
  unsigned post;
  if (!g_mts.have_vis)
    return;
  g_mts.have_vis = 0;
  post = mts_replay(0);
#if ME_MIDFRAME_LOG
  if (me_capture_buf->log_n)
  {
    g_mts.log_frames++;
    g_mts.log_entries += me_capture_buf->log_n;
    if ((long)me_capture_buf->log_n > g_mts.log_max)
      g_mts.log_max = me_capture_buf->log_n;
  }
  if (me_capture_buf->log_flags & ME_LOGF_OVERFLOW)
    g_mts.log_overflow++;
  if (me_capture_buf->log_flags & ME_LOGF_VRAM_ON)
    g_mts.log_vram_on++;
#endif
  g_mts.frames++;
  if (post != g_mts.cpu_hash) g_mts.post_bad++;
  if (g_mts.vis_hash != g_mts.cpu_hash) g_mts.vis_bad++;
  {
    int pal = g_mts.pal_first >= 0, oam = g_mts.oam_first >= 0;
    int vr = g_mts.vram_first >= 0, aff = g_mts.aff_reloads > 0;
    g_mts.n_pal += pal; g_mts.n_oam += oam; g_mts.n_vram += vr; g_mts.n_aff += aff;
    if (g_mts.vis_hash != g_mts.cpu_hash)
    {
      g_mts.bad_pal += pal; g_mts.bad_oam += oam; g_mts.bad_vram += vr;
      g_mts.bad_aff += aff; g_mts.bad_none += !(pal || oam || vr || aff);
    }
    if (pal || oam || vr || aff)
      fprintf(stderr, "MTSW f=%lu vis_bad=%d aff_reload_lines=%d pal_line=%d "
              "oam_line=%d vram_line=%d\n", g_mts.frame,
              g_mts.vis_hash != g_mts.cpu_hash, g_mts.aff_reloads,
              g_mts.pal_first, g_mts.oam_first, g_mts.vram_first);
  }
  {
    /* Per frame: BG1/BG2 scroll as captured (line 80), and which VRAM pages
     * were written while the frame drew ("vis") and during its VBlank ("vbl"). */
    const u16 *r = me_capture_buf->ioregs[80];
    char vbl[160]; int n = 0, pl = 0;
    vbl[0] = 0;
    for (int p = 0; p < 96; p++)
      if (memcmp(g_mts.vram160 + p * 1024, vram + p * 1024, 1024) && pl < 150)
        pl += snprintf(vbl + pl, sizeof(vbl) - pl, "%s%d", n++ ? "," : "", p);
    fprintf(stderr, "MTS f=%lu cpu=%08x post=%08x vis=%08x bg1=%d,%d bg2=%d,%d "
            "vram_vis=[%s] vram_vbl=[%s]\n", g_mts.frame, g_mts.cpu_hash, post,
            g_mts.vis_hash, (s16)r[10], (s16)r[11], (s16)r[12], (s16)r[13],
            g_mts.vis_pages, vbl);
  }
  memcpy(g_mts.vram_end, vram, sizeof(g_mts.vram_end));

  if (post != g_mts.cpu_hash || g_mts.vis_hash != g_mts.cpu_hash)
  {
    /* What moved between the vcount-160 snapshot and the post-time one. */
    char pages[400]; int np = 0, pl = 0, no = 0, npal = 0;
    pages[0] = 0;
    for (int p = 0; p < 96; p++)
      if (memcmp(g_mts.vram160 + p * 1024, vram + p * 1024, 1024))
      {
        np++;
        if (pl < (int)sizeof(pages) - 8)
          pl += snprintf(pages + pl, sizeof(pages) - pl, "%s%d", np > 1 ? "," : "", p);
      }
    for (int i = 0; i < 128; i++)
      if (memcmp(g_mts.oam160 + i * 4, oam_ram + i * 4, 6)) no++;
    for (int i = 0; i < 512; i++)
      if (g_mts.pal160[i] != palette_ram_converted[i]) npal++;
    const u16 *r80 = me_capture_buf->ioregs[80];
    fprintf(stderr, "MTSD f=%lu post_bad=%d vis_bad=%d vram_pages=%d[%s] "
            "oam_entries=%d pal=%d dispcnt=%04x bg0=%d,%d bg1=%d,%d bg2=%d,%d "
            "bg3=%d,%d oamupd=%u\n",
            g_mts.frame, post != g_mts.cpu_hash, g_mts.vis_hash != g_mts.cpu_hash,
            np, pages, no, npal, r80[0], r80[8], r80[9], r80[10], r80[11],
            r80[12], r80[13], r80[14], r80[15], me_capture_buf->oam_updated);
    if (getenv("MTS_DUMP_DIR") && g_mts.dumps < 2000)
    {
      const char *r = getenv("MTS_DUMP_FRAMES");
      unsigned long a = 0, b = ~0ul;
      if (!r || sscanf(r, "%lu-%lu", &a, &b) < 1 ||
          (g_mts.frame >= a && g_mts.frame <= b))
        g_mts.dumps++;
      mts_dump("cpu", g_mts.cpu_px);
      mts_dump("post", g_mts.eng[0].out);
      mts_dump("vis", g_mts.eng[1].out);
    }
  }
}

static void mts_report(void)
{
  fprintf(stderr, "MTS_SUMMARY frames=%ld post_mismatch=%ld vis_mismatch=%ld\n",
          g_mts.frames, g_mts.post_bad, g_mts.vis_bad);
  fprintf(stderr, "MTS_DRIFT_TOTAL %ld\n", g_mts.drift);
#if ME_MIDFRAME_LOG
  {
    extern unsigned long long me_log_ns;
    fprintf(stderr, "MTS_COST recorder_ns=%llu cpu_render_ns=%llu per_frame: "
            "recorder=%.2f us render=%.2f us\n", me_log_ns, g_mts.render_ns,
            g_mts.frames ? me_log_ns / 1000.0 / g_mts.frames : 0.0,
            g_mts.frames ? g_mts.render_ns / 1000.0 / g_mts.frames : 0.0);
  }
  fprintf(stderr, "MTS_WORK frames=%u tracked=%u pal_checks=%u oam_checks=%u map_scans=%u "
          "page_diffs=%u page_copies=%u base_copies=%u entries=%u overflows=%u\n",
          me_log_stats.frames, me_log_stats.tracked_frames, me_log_stats.pal_checks,
          me_log_stats.oam_checks, me_log_stats.map_scans, me_log_stats.page_diffs,
          me_log_stats.page_copies, me_log_stats.base_copies, me_log_stats.entries,
          me_log_stats.overflows);
#endif
  fprintf(stderr, "MTS_LOG frames=%ld entries=%ld max=%ld overflow=%ld vram_tracking=%ld\n",
          g_mts.log_frames, g_mts.log_entries, g_mts.log_max, g_mts.log_overflow,
          g_mts.log_vram_on);
  fprintf(stderr, "MTS_CAUSES midframe: aff=%ld pal=%ld oam=%ld vram=%ld | "
          "vis_bad with: aff=%ld pal=%ld oam=%ld vram=%ld none=%ld\n",
          g_mts.n_aff, g_mts.n_pal, g_mts.n_oam, g_mts.n_vram, g_mts.bad_aff,
          g_mts.bad_pal, g_mts.bad_oam, g_mts.bad_vram, g_mts.bad_none);
}
__attribute__((constructor)) static void mts_install(void) { atexit(mts_report); }
#define MTS_CAPTURE_LINE(vc) mts_capture_line(vc)
