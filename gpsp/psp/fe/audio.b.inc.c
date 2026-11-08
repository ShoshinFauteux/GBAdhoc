static unsigned audio_nominal_step(void)
{
   return (unsigned)(((uint64_t)in_rate << 16) / OUT_RATE);
}

/* The session's machines are running: the audio policy on (cushion, soft
 * underruns, rate follow) or, at its end, off and nominal again. */
static void gblink_audio_session(int on)
{
   if (on == g_gbl_audio_on)
      return;
   g_gbl_audio_on = on;
   if (on)
   {
      unsigned i, n = g_gbl_cushion;
      if (n > RING_FRAMES / 2)
         n = RING_FRAMES / 2;
      /* Producer side (this thread): fill, then publish. */
      for (i = 0; i < n; i++)
      {
         unsigned idx = (ring_w + i) & RING_MASK;
         audio_ring[idx * 2 + 0] = 0;
         audio_ring[idx * 2 + 1] = 0;
      }
/* @extract:begin spsc:gblink_publish */
      RING_BARRIER();
      ring_w += n;
/* @extract:end spsc:gblink_publish */
      g_gbl_rate_ema = (ring_w - ring_r) * 16u;
      g_gbl_rate_i = 0;
      g_audio_soft = g_gbl_soft;
      g_audio_trim = g_gbl_follow ? RING_HIGH_WATER + 1024u : 0u;
      {
         /* the telemetry window starts with the machines */
         fe_gblink_window w;
         fe_host_gblink_window(&w);
         g_gw_t0 = 0;
         g_gbl_work_ema = 12000u;
      }
      fe_evt("gblink_audio on cushion=%u follow=%u absorb=%d soft=%d "
             "level=%u", n, g_gbl_follow, g_gbl_absorb, g_gbl_soft,
             ring_w - ring_r);
   }
   else
   {
      g_audio_soft = 0;
      g_audio_trim = 0;
      g_audio_step = audio_nominal_step();
      fe_evt("gblink_audio off");
   }
}

