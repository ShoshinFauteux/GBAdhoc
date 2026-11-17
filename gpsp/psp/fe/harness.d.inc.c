/* The session cost heartbeat and the ad hoc cell line (ADR-0021): EVT lines only, so a
 * telemetry build's unit (code-health C2-5); a player build calls nothing. */
#ifdef GPSP_KEEP_TELEMETRY
void sess_cost_evt(int force)
{
   static fe_np_prof  prev;
   static adhoc_stats prev_a;
   static unsigned    prev_evt_lines, prev_evt_us, prev_evt_io_us;
   FE_EVT_ONLY(prev_evt_lines); FE_EVT_ONLY(prev_evt_us);
   FE_EVT_ONLY(prev_evt_io_us);
   static unsigned    prev_core_calls;
   static uint32_t    prev_core_us;
   FE_EVT_ONLY(prev_core_us);
   fe_np_prof  p;
   adhoc_stats a;
   unsigned    el, eu, emx;
   unsigned    eio, eiomx, edrop, ehi;
   unsigned    f, dpdp = 0, denq = 0;
   FE_EVT_ONLY(dpdp); FE_EVT_ONLY(denq);
   unsigned    ccalls, dcc = 0;
   FE_EVT_ONLY(dcc);
   uint32_t    cus, cmax;
   unsigned    srf, sff, ssf, sdf, spl;  /* counters, SLOWEST retro_run */
   unsigned    crf, cff, csf, cdf, cpl;  /* cumulative, for window rates */
   static unsigned prev_crf, prev_cff, prev_csf, prev_cdf, prev_cpl;
   FE_EVT_ONLY(prev_crf); FE_EVT_ONLY(prev_cff); FE_EVT_ONLY(prev_csf);
   FE_EVT_ONLY(prev_cdf); FE_EVT_ONLY(prev_cpl);

   fe_np_prof_get(&p);
   adhoc_transport_get_stats(&a);
   fe_evt_prof(&el, &eu, &emx);
   fe_evt_prof_io(&eio, &eiomx, &edrop, &ehi);
   fe_host_core_prof(&ccalls, &cus, &cmax);        /* ADR-0027 §measurement */
   fe_host_core_spike(NULL, &srf, &sff, &ssf, &sdf, &spl);    /* ADR-0028 */
   plat_core_counters(&crf, &cff, &csf, &cdf, &cpl);

   f = p.frames - prev.frames;
   if (!force && f < SESS_COST_MIN_FRAMES)
      return;      /* deliberately do NOT advance prev: fold into the next */
   if (!f)
      f = 1;
   dpdp = a.tx_calls  - prev_a.tx_calls;
   denq = a.tx_queued + a.tx_inline - (prev_a.tx_queued + prev_a.tx_inline);
   dcc  = ccalls - prev_core_calls;

   fe_evt("sess_cost win=%u clk_ns=%u frame=%u/%u pump=%u/%u "
          "poll=%u/%u/%u/%u rx=%u/%u arq=%u/%u "
          "pdp=%u/%u/%u enq=%u/%u txq=%u/%u/%u evt=%u/%u/%u "
          "evtio=%u/%u/%u/%u txthr=%d core=%u/%u "
          "corewin=%u/%u/%u/%u/%u corespike=%u/%u/%u/%u/%u "
          "pace=%u.%02u/%u.%02u/%u.%02u/%d",
          f, p.clk_ns,
          (p.frame_us - prev.frame_us) / f, p.frame_max_us,
          (p.pump_us  - prev.pump_us)  / f, p.pump_max_us,
          (p.poll_n - prev.poll_n) / f,
          (p.poll_work_n - prev.poll_work_n) / f,
          (p.poll_us - prev.poll_us) / f, p.poll_max_us,
          (p.rx_us  - prev.rx_us)  / f, p.rx_max_us,
          (p.arq_us - prev.arq_us) / f, p.arq_max_us,
          dpdp, dpdp ? (a.tx_us - prev_a.tx_us) / dpdp : 0u, a.tx_max_us,
          denq ? (a.tx_enq_us - prev_a.tx_enq_us) / denq : 0u, a.tx_enq_max_us,
          a.tx_queued - prev_a.tx_queued, a.tx_inline - prev_a.tx_inline,
          a.tx_ring_full - prev_a.tx_ring_full,
          el - prev_evt_lines, eu - prev_evt_us, emx,
          eio - prev_evt_io_us, eiomx, edrop, ehi,
          adhoc_transport_tx_thread_active(),
          dcc ? (unsigned)((cus - prev_core_us) / dcc) : 0u, cmax,
          crf - prev_crf, cff - prev_cff, csf - prev_csf, cdf - prev_cdf,
          cpl - prev_cpl,
          srf, sff, ssf, sdf, spl,
          ff_pace_target_x100() / 100, ff_pace_target_x100() % 100,
          ff_pace_cap_x100() / 100,    ff_pace_cap_x100() % 100,
          ff_pace_peer_x100() / 100,   ff_pace_peer_x100() % 100,
          ff_pace_engaged());

   frame_hist_evt(f);                                       /* ADR-0063 */

   prev = p;
   prev_a = a;
   prev_evt_lines = el;
   prev_evt_us = eu;
   prev_evt_io_us = eio;
   prev_core_calls = ccalls;
   prev_core_us = cus;
   prev_crf = crf;
   prev_cff = cff;
   prev_csf = csf;
   prev_cdf = cdf;
   prev_cpl = cpl;
}

/* The cell this console is in: two consoles in one group must report the
 * SAME bssid and channel (hw3 run 9 / hw4: both "up" in GBLNK7, rx=0). */
static void adhoc_ctl_evt(const char *at)
{
   int ch = -1, peers = -1;
   uint8_t b[6] = { 0 };
   if (adhoc_transport_ctl_info(&ch, b, &peers) != 0)
      return;
   fe_evt("adhoc_ctl at=%s channel=%d bssid=%02x:%02x:%02x:%02x:%02x:%02x "
          "ctl_peers=%d", at, ch, b[0], b[1], b[2], b[3], b[4], b[5], peers);
}
#else
#define sess_cost_evt(force) do { } while (0)
#define adhoc_ctl_evt(at)    do { } while (0)
#endif

