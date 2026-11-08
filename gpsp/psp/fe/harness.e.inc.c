/* ---- GU color-order test pattern (testpat = 1; no core, no ROM) --------
 * Renders 8 vertical 30px bars of known RGB565 values through the exact
 * production blit path, then GE-dumps the drawbuffer.  The harness asserts
 * the decoded bar colors — a channel-order regression fails loudly. */

/* The bars are the blit path's INPUT, so they must be written in whatever
 * layout the core emits (ADR-0039) — the harness then asserts the GE's
 * output is really red where we asked for red, which is what pins the
 * frontend's half of the format contract absolutely, with no core in the
 * loop.  Spelled as channel triples rather than hex so the two lists cannot
 * drift apart. */
#ifdef USE_PSP_RGB565_FORMAT
#define TESTPAT_PIX(r, g, b) \
   (uint16_t)(((b) << 11) | ((g) << 5) | (r))
#else
#define TESTPAT_PIX(r, g, b) \
   (uint16_t)(((r) << 11) | ((g) << 5) | (b))
#endif

static int run_testpat(void)
{
   static const uint16_t bar[8] = {
      TESTPAT_PIX(31, 63, 31),   /* white   */
      TESTPAT_PIX(31,  0,  0),   /* red     */
      TESTPAT_PIX( 0, 63,  0),   /* green   */
      TESTPAT_PIX( 0,  0, 31),   /* blue    */
      TESTPAT_PIX(31, 63,  0),   /* yellow  */
      TESTPAT_PIX( 0, 63, 31),   /* cyan    */
      TESTPAT_PIX(31,  0, 31),   /* magenta */
      TESTPAT_PIX(16, 32, 16)    /* gray    */
   };
   /* The heap: this runs with no core loaded, so it has room, and a static
    * array was 76,800 B of every build's image for a harness-only test. */
   uint16_t *pat = (uint16_t *)malloc(FE_GBA_WIDTH * FE_GBA_HEIGHT * 2);
   int f, x, y;

   if (!pat)
      return -1;
   for (y = 0; y < FE_GBA_HEIGHT; y++)
      for (x = 0; x < FE_GBA_WIDTH; x++)
         pat[y * FE_GBA_WIDTH + x] = bar[x / 30];

   fe_evt("testpat_start bars=8");
   for (f = 0; f < 120 && g_running; f++)
   {
      plat_video_frame(pat, FE_GBA_WIDTH, FE_GBA_HEIGHT, FE_GBA_WIDTH * 2);
      if (f == 60)
         dump_ge_bmp();
      sceDisplayWaitVblankStart();
      vid_swap();
      g_drew = 0;
   }
   free(pat);
   return 0;
}

/* ---- Gate-4E transport echo (nettest = 1; no core, no ROM) -------------
 * Wire format: "GPNT" magic + type u8 (0 ping / 1 pong / 2 end) + seq u32
 * LE + 15 pad bytes = 24 B.  The join side broadcasts pings at 10 Hz and
 * asserts >= NETTEST_TARGET pongs; the host unicasts every ping back to
 * its source MAC and passes on the same threshold.  Exercises exactly the
 * surface netdrv needs: broadcast, unicast-to-src, RX thread + ring. */

#define NETTEST_TARGET   30
#define NETTEST_PKT_LEN  24

static int run_nettest(int is_host, const char *group, long secs)
{
   nd_transport tp;
   uint64_t start, last_ping = 0, last_tick = 0;
   uint32_t seq = 0, pings = 0, pongs = 0, ends_sent = 0;
   int done = 0, rc;

   fe_evt("nettest_start role=%s group=%s", is_host ? "host" : "join",
          group && group[0] ? group : ADHOC_GROUP_DEFAULT);
   rc = is_host ? adhoc_transport_init(group, 0)
                : adhoc_transport_init_join(group, 0, 15000u);
   if (rc != ADHOC_OK)
   {
      net_error_evt("adhoc_init", rc);
      return -1;
   }
   log_adhoc_up(group);
   adhoc_transport_iface(&tp);

   start = net_now_us();
   while (!done && g_running && net_now_us() - start < (uint64_t)secs * 1000000ull)
   {
      uint8_t src[6], buf[ND_MAX_FRAME];
      int n;
      uint64_t now = net_now_us();

      while ((n = tp.recv(tp.ctx, src, buf, sizeof(buf))) > 0)
      {
         uint32_t rseq = 0;
   FE_EVT_ONLY(rseq);
         if (n < NETTEST_PKT_LEN || memcmp(buf, "GPNT", 4) != 0)
            continue;
         rseq = (uint32_t)buf[5] | ((uint32_t)buf[6] << 8) |
                ((uint32_t)buf[7] << 16) | ((uint32_t)buf[8] << 24);
         if (is_host && buf[4] == 0)      /* ping -> pong to source MAC */
         {
            buf[4] = 1;
            tp.send_to(tp.ctx, src, buf, NETTEST_PKT_LEN);
            pings++;
            if (pings == 1 || pings == NETTEST_TARGET)
               fe_evt("nettest_echo n=%u seq=%u", pings, rseq);
         }
         else if (is_host && buf[4] == 2) /* joiner done */
            done = 1;
         else if (!is_host && buf[4] == 1)
         {
            pongs++;
            if (pongs == 1 || pongs == NETTEST_TARGET)
               fe_evt("nettest_pong n=%u seq=%u", pongs, rseq);
         }
      }

      if (!is_host && now - last_ping >= 100000)
      {
         uint8_t pkt[NETTEST_PKT_LEN];
         memset(pkt, 0, sizeof(pkt));
         memcpy(pkt, "GPNT", 4);
         if (pongs < NETTEST_TARGET)
         {
            pkt[4] = 0;
            pkt[5] = (uint8_t)seq;        pkt[6] = (uint8_t)(seq >> 8);
            pkt[7] = (uint8_t)(seq >> 16); pkt[8] = (uint8_t)(seq >> 24);
            tp.broadcast(tp.ctx, pkt, sizeof(pkt));
            seq++;
         }
         else                             /* enough echoes: tell the host */
         {
            pkt[4] = 2;
            tp.broadcast(tp.ctx, pkt, sizeof(pkt));
            if (++ends_sent >= 5)
               done = 1;
         }
         last_ping = now;
      }

      if (now - last_tick >= 1000000)
      {
         scePowerTick(PSP_POWER_TICK_ALL);
         last_tick = now;
      }
      sceKernelDelayThread(10000);
   }

   fe_evt("nettest_done role=%s sent=%u echoed=%u pongs=%u",
          is_host ? "host" : "join", seq, pings, pongs);
   adhoc_stats_evt();
   adhoc_transport_term();
   return (is_host ? pings : pongs) >= NETTEST_TARGET ? 0 : -1;
}

