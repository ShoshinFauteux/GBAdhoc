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

