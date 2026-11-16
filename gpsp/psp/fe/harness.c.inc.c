/* ADR-0034: read the core renderer's per-path profile out once per window.
 * Emits the window MEAN of every bucket plus the breakdown of the single
 * worst scanline-render frame — the phase maxima must never be summed
 * (ADR-0032), so `vid_worst` is what a max is allowed to be read against.
 * All zero (and n==0) unless the core was built with -DVIDEO_PROF=1. */
#ifdef GPSP_KEEP_TELEMETRY
static unsigned vp_sum[VP_COUNT], vp_worst[VP_COUNT], vp_max_tot, vp_win_n;
static unsigned vp_core_mean;
#endif
/* (unused in a player build: every reader is telemetry or rig code) */
static unsigned vp_build_crc __attribute__((unused));   /* CRC32 of the running EBOOT */

/* ADR-0035: identify the BINARY in the profile line, not just the run.
 * A stale EBOOT in a sandbox produced two "different" builds with identical
 * timings and cost most of a session to spot.  A profile whose build cannot be
 * identified is not a measurement, so the CRC of the running EBOOT goes in
 * every `vid_prof` line and is emitted once at boot.  ~1.6 MiB of CRC, only
 * when profiling is enabled.  Telemetry builds only: its only outputs are
 * fe_evt lines and the rig's run identity. */
#ifdef GPSP_KEEP_TELEMETRY
static unsigned eboot_crc(void)
{
   char path[160];
   /* Borrowed for the read, not a static: it runs once, on harness or
    * vid_prof runs only, and was 4 KiB of every player's .bss. */
   unsigned char *buf;
   SceUID fd;
   unsigned crc = 0, total = 0;
   int n;

   snprintf(path, sizeof(path), "%s/EBOOT.PBP", g_dir_base);
   buf = (unsigned char *)malloc(4096);
   if (!buf)
      return 0;
   fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
   if (fd < 0)
   {
      free(buf);
      return 0;
   }
   while ((n = sceIoRead(fd, buf, 4096)) > 0)
   {
      crc = fe_crc32(crc, buf, (size_t)n);
      total += (unsigned)n;
   }
   sceIoClose(fd);
   free(buf);
   fe_evt("build eboot_crc=%08x size=%u", crc, total);
   return crc;
}
#endif

/* ---- VRAM DIRTY-PAGE PROBE ---------------------------------------------
 *
 * The question.  The ME renderer's input handshake costs a measured ~1.45 ms
 * of MAIN-CPU SPIN every frame (`EVT me_rend wait_mean_us`, symmetric across
 * both consoles, drops=0 -- so it is handshake latency, not render time).
 * That wait exists because me_render_glue.cc does a flat
 * `memcpy(vram, main_ram, 96 KB)` on the ME from uncached main RAM, ~67 MB/s.
 * VRAM is 98 % of the 98 KB copied.
 *
 * IF a typical frame only dirties a small fraction of VRAM, copying just the
 * dirty pages collapses that wait and hands ~1 ms/frame back on BOTH
 * consoles -- more than the whole historical client-side deficit.  IF games
 * rewrite most of VRAM every frame, the idea is dead and we stop here.
 *
 * Why a shadow compare and not write barriers.  Production dirty-tracking
 * has to instrument the DYNAREC's store path (mips_emit.h `emit_pmemst_stub`,
 * region 6) -- translated code does not go through gba_memory.c's
 * `write_vram##type()` macros, so C-side instrumentation alone would
 * undercount badly.  That is real work in the hottest store path and it is
 * not worth doing speculatively.  This probe answers the same question with
 * ZERO hot-path cost, and it either justifies that work or kills it.
 *
 * How.  Every `g_vp_period` frames, a two-frame cycle:
 *   phase 1  copy VRAM to a shadow.
 *   phase 2  the NEXT frame, compare page by page and count the pages that
 *            differ.  Comparing consecutive frames is the point -- sampling
 *            frame N against frame N-16 would measure a 16-frame union and
 *            overstate a per-frame copy.
 *
 * Cost is confined to those two frames, is self-timed into `cost_us`, and is
 * reported with the result so the instrument's own price is in its own
 * output.  `vram_probe = 0` (the default) allocates nothing and never runs.
 *
 *   EVT vram_probe n=<samples> pages=<mean/96> max=<n> kb=<mean KB>
 *                  cost_us=<mean> period=<n>
 */
#define VP_PAGE_SHIFT  10                 /* 1 KiB pages */
#define VP_PAGES       (1024 * 96 >> VP_PAGE_SHIFT)   /* 96 */
static int      g_vp_period;              /* harness key `vram_probe`, 0=off */
static unsigned char *g_vp_shadow;
static int      g_vp_phase;               /* 0 idle, 1 shadow taken          */
static unsigned g_vp_ctr, g_vp_n, g_vp_sum, g_vp_max, g_vp_cost;
static unsigned g_vp_marked, g_vp_missed, g_vp_marks0;

static __attribute__((noinline)) void vram_probe_frame(void)
{
   unsigned t0, i, dirty = 0;

   if (!g_vp_period)
      return;
   if (!g_vp_shadow)
   {
      g_vp_shadow = (unsigned char *)malloc(1024 * 96);
      if (!g_vp_shadow)
      {
         g_vp_period = 0;                 /* never let an instrument fail a run */
         fe_evt("vram_probe off reason=alloc");
         return;
      }
   }

   if (g_vp_phase == 0)
   {
      if (++g_vp_ctr < (unsigned)g_vp_period)
         return;
      g_vp_ctr = 0;
      t0 = (unsigned)sceKernelGetSystemTimeLow();
      memcpy(g_vp_shadow, vram, 1024 * 96);
      /* DO NOT clear the map here any more. It is now load-bearing: the ME
       * copies exactly the pages it marks, and clearing it mid-frame would
       * drop pages the ME has not read yet -- silent stale-VRAM rendering.
       * Coverage has already been established (missed=0 over 255 marked
       * pages on build 58026acf), so the probe reverts to its original job
       * of counting how many pages actually change per frame. */
      g_vp_marks0 = vram_dirty_marks;
      g_vp_cost += (unsigned)sceKernelGetSystemTimeLow() - t0;
      g_vp_phase = 1;
      return;
   }

   /* phase 2: consecutive-frame delta */
   t0 = (unsigned)sceKernelGetSystemTimeLow();
   for (i = 0; i < VP_PAGES; i++)
   {
      unsigned off = i << VP_PAGE_SHIFT;
      int changed = memcmp(g_vp_shadow + off, vram + off,
                           1u << VP_PAGE_SHIFT) != 0;
      int marked  = vram_clean[i] == 0;   /* inverted map */
      if (changed)
         dirty++;
      if (marked)
         g_vp_marked++;
      /* THE NUMBER THAT DECIDES THE DESIGN: a page that really changed and
       * was never marked.  Copy only marked pages while this is non-zero and
       * the ME renders stale VRAM -- silent corruption, not a crash. */
      if (changed && !marked)
         g_vp_missed++;
   }
   g_vp_cost += (unsigned)sceKernelGetSystemTimeLow() - t0;
   g_vp_phase = 0;

   g_vp_n++;
   g_vp_sum += dirty;
   if (dirty > g_vp_max)
      g_vp_max = dirty;

   if (g_vp_n >= 32)
   {
      fe_evt("vram_probe n=%u pages=%u.%u/%u max=%u kb=%u cost_us=%u "
             "period=%u marked=%u missed=%u marks=%u",
             g_vp_n,
             g_vp_sum / g_vp_n, (g_vp_sum * 10u / g_vp_n) % 10u, VP_PAGES,
             g_vp_max,
             g_vp_sum / g_vp_n,        /* 1 KiB pages, so pages == KiB */
             g_vp_cost / (g_vp_n * 2u),
             (unsigned)g_vp_period,
             g_vp_marked, g_vp_missed, vram_dirty_marks - g_vp_marks0);
      g_vp_n = g_vp_sum = g_vp_max = g_vp_cost = 0;
      g_vp_marked = g_vp_missed = 0;
   }
}

#ifdef GPSP_KEEP_TELEMETRY
static void vid_prof_frame(unsigned win)
{
   unsigned cur[VP_COUNT];
   char line[768];
   int n = video_prof_read_reset(cur, VP_COUNT);
   int i, off = 0;

   if (n <= 0)
      return;
   for (i = 0; i < n; i++)
      vp_sum[i] += cur[i];
   if (cur[VP_T_TOTAL] >= vp_max_tot)
   {
      vp_max_tot = cur[VP_T_TOTAL];
      for (i = 0; i < n; i++)
         vp_worst[i] = cur[i];
   }
   if (++vp_win_n < win)
      return;

   /* Cross-check against the whole retro_run call over the SAME window, so
    * the renderer's share is a ratio of two measurements rather than of one
    * measurement and a remembered number (ADR-0032). */
   {
      static unsigned prev_calls;
      static uint32_t prev_total;
      unsigned calls = 0; uint32_t total = 0, mx = 0;
      unsigned d;
      fe_host_core_prof(&calls, &total, &mx);
      d = calls - prev_calls;
      vp_core_mean = d ? (total - prev_total) / d : 0;
      prev_calls = calls; prev_total = total;
   }

   for (i = 0; i < n && off < (int)sizeof(line) - 24; i++)
      off += snprintf(line + off, sizeof(line) - off, "%s=%u ",
                      video_prof_name(i), vp_sum[i] / win);
   fe_evt("vid_prof f=%u bld=%08x core=%u tmax=%u %s", win, vp_build_crc,
          vp_core_mean, vp_max_tot, line);

   off = 0;
   for (i = 0; i < n && off < (int)sizeof(line) - 24; i++)
      off += snprintf(line + off, sizeof(line) - off, "%s=%u ",
                      video_prof_name(i), vp_worst[i]);
   fe_evt("vid_worst %s", line);

   memset(vp_sum, 0, sizeof(vp_sum));
   memset(vp_worst, 0, sizeof(vp_worst));
   vp_max_tot = 0;
   vp_win_n   = 0;
}
#else
/* Every output of the window is an fe_evt line, compiled out here; the
 * core's counters are still drained so they cannot wrap. */
static void vid_prof_frame(unsigned win)
{
   unsigned cur[VP_COUNT];
   (void)win;
   (void)video_prof_read_reset(cur, VP_COUNT);
}
#endif

