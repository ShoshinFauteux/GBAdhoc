/* Once per loop iteration while the machines run: steer the resampler
 * toward the cushion (gblink_audio_follow).  Proportional plus integral:
 * a session that holds 58 fps for a while needs the step 2.9 % low for as
 * long, and a proportional term alone only gets there with the ring empty
 * (PPSSPP, CPU-bound: ~360 gaps a minute with P alone).  The integral
 * carries the steady rate, the proportional term the moment-to-moment
 * level; both are bounded by -follow/10 % .. +0.5 %.  The gains come from
 * a simulation of this ring (docs/GB-LINK.md 14.4): a faster integral (1 %
 * a second) rang -- a 12-20 s limit cycle swinging the pitch by 2.5 % --
 * while 2 % proportional + 1 % in 6 s integral settles without ringing
 * and, with the 3072-frame cushion, leaves no gaps up to 1 % of frames
 * lost to stalls. */
static void gblink_audio_frame(void)
{
   unsigned level = ring_w - ring_r;
   int32_t target = (int32_t)(g_gbl_cushion ? g_gbl_cushion : 3072u);
   int32_t lo = -(int32_t)g_gbl_follow * 1000, hi = 5000;
   int64_t err_ppm, ppm;
   if (level > RING_FRAMES)
      level = 0;                     /* a flush in progress (see audio_thread) */
   /* ~0.25 s smoothing: the level saw-tooths by a whole output chunk (761
    * frames) as the audio thread pulls, which must not reach the pitch. */
   g_gbl_rate_ema += level - g_gbl_rate_ema / 16u;
   if (!g_gbl_follow)
      return;
   /* the level's error as a share of the cushion, in ppm (-1e6 = empty) */
   err_ppm = ((int64_t)(g_gbl_rate_ema / 16u) - target) * 1000000 / target;
   if (err_ppm > 1000000)
      err_ppm = 1000000;
   g_gbl_rate_i += (int32_t)(err_ppm / 18000);   /* 1 % in 6 s at a 50 % error */
   if (g_gbl_rate_i < lo)
      g_gbl_rate_i = lo;
   if (g_gbl_rate_i > hi)
      g_gbl_rate_i = hi;
   ppm = g_gbl_rate_i + err_ppm / 50;             /* P: 2 % at full scale */
   if (ppm < lo)
      ppm = lo;
   if (ppm > hi)
      ppm = hi;
   g_audio_step = (unsigned)(((uint64_t)audio_nominal_step() *
                              (uint64_t)(1000000 + ppm)) / 1000000u);
}

/* EVT gblink_win: one line per g_gbl_win_ms of wall time while the machines
 * run -- the attribution record (GB-LINK.md section 14):
 *   frames/stalls/st_ep/st_max   display frames emulated / lost to a late
 *                     input / stall episodes / longest run, this window
 *   absorbed/abs_ms   late inputs waited for and still on time (gblink_absorb)
 *   work_us mean/max  the loop's frame work (vblank wait excluded)
 *   core_us mean/max  both machines (gbdual_advance, hooks and waits out)
 *   step_us mean/max  the whole session step (core + hash + crc + sends)
 *   hash_us n/max, crc_us n/max   the periodic sync hash / cartridge check
 *   ovb/late          frames over 16.74 ms of work / that missed their vblank
 *   delay lead_min/lead_mean   input delay; partner frames in hand
 *   rxq_max           netdrv datagrams waiting at a pump, max
 *   ar_min/ar_mean    audio ring level (frames); under = gaps, starved =
 *                     output frames of silence; step = resampler ppm
 *   pg=A/B            32 KiB ROM pages read from the stick this window by
 *                     the player's / the partner's GBA (a GBA link-cable
 *                     session whose ROM does not fit, i.e. the PSP-1000);
 *                     0/0 in a GB session */
#ifdef GBA_LINK
extern uint32_t gbab_gamepak_page_loads;
#define GW_PAGES_B() ((unsigned)gbab_gamepak_page_loads)
#else
#define GW_PAGES_B() 0u
#endif
static unsigned g_gw_pg0a, g_gw_pg0b;
static unsigned g_gw_loops, g_gw_ar_min, g_gw_ovb, g_gw_late;
static uint64_t g_gw_ar_sum, g_gw_work_sum;
static uint32_t g_gw_work_max;
static unsigned g_gw_starved0, g_gw_under0;

static void gblink_win_reset(uint64_t now)
{
   g_gw_t0 = now;
   g_gw_loops = 0;
   g_gw_ar_min = ~0u;
   g_gw_ar_sum = 0;
   g_gw_work_sum = 0;
   g_gw_work_max = 0;
   g_gw_ovb = g_gw_late = 0;
   g_gbl_rxq_max = 0;
   g_gw_starved0 = g_audio_starved;
   g_gw_under0 = g_audio_underruns;
   g_gw_pg0a = (unsigned)gamepak_page_loads;
   g_gw_pg0b = GW_PAGES_B();
}

/* The main loop's measured frame work (work_us), once per iteration, and
 * whether the frame missed its vblank. */
static void gblink_win_work(uint32_t work_us, int late)
{
   /* the absorbed wait is not work (fe_host's core_prof excludes it too) */
   work_us = work_us > g_gbl_frame_wait_us ? work_us - g_gbl_frame_wait_us : 0;
   g_gbl_frame_wait_us = 0;
   g_gbl_wait_prev = 0;
   if (!g_gbl_audio_on)
      return;
   /* plat_gblink_wait's estimate of a frame's work: 1/8 new */
   g_gbl_work_ema = g_gbl_work_ema - g_gbl_work_ema / 8u + work_us / 8u;
   g_gw_work_sum += work_us;
   if (work_us > g_gw_work_max)
      g_gw_work_max = work_us;
   if (work_us > 16740u)
      g_gw_ovb++;
   if (late)
      g_gw_late++;
}

static void gblink_win_note(void)
{
   uint64_t now = net_now_us();
   unsigned level = ring_w - ring_r;
   if (!g_gw_t0)
      gblink_win_reset(now);
   if (level > RING_FRAMES)
      level = 0;
   g_gw_loops++;
   if (level < g_gw_ar_min)
      g_gw_ar_min = level;
   g_gw_ar_sum += level;
   if (now - g_gw_t0 < (uint64_t)g_gbl_win_ms * 1000u)
      return;
#ifndef GPSP_NO_TELEMETRY
   {
      fe_gblink_window w;
      unsigned loops = g_gw_loops ? g_gw_loops : 1;
      unsigned fr;
      memset(&w, 0, sizeof(w));
      fe_host_gblink_window(&w);
      fr = w.frames ? w.frames : 1;
      fe_evt("gblink_win dt_ms=%u loops=%u frames=%u stalls=%u st_ep=%u "
             "st_max=%u absorbed=%u abs_ms=%u abs_max_us=%u wait_ms=%u "
             "work_us=%u/%u core_us=%u/%u step_us=%u/%u hash_us=%u/%u "
             "crc_us=%u/%u ovb=%u late=%u delay=%u lead_min=%d "
             "lead_mean_x10=%u rxq_max=%u ar_min=%u ar_mean=%u under=%u "
             "starved=%u step_ppm=%d nacks=%u resends=%u pg=%u/%u",
             (unsigned)((now - g_gw_t0) / 1000u), g_gw_loops, w.frames,
             w.stalls, w.stall_episodes, w.streak_max, w.absorbed,
             w.absorb_us / 1000u, w.absorb_us_max, w.wait_us / 1000u,
             (unsigned)(g_gw_work_sum / loops), g_gw_work_max,
             w.adv_us / fr, w.adv_us_max, w.step_us / (w.steps ? w.steps : 1),
             w.step_us_max, w.hash_n, w.hash_us_max, w.crc_n, w.crc_us_max,
             g_gw_ovb, g_gw_late, w.delay,
             w.lead_n ? (int)w.lead_min : -1,
             w.lead_n ? (unsigned)(w.lead_sum * 10u / w.lead_n) : 0u,
             g_gbl_rxq_max, g_gw_ar_min == ~0u ? 0u : g_gw_ar_min,
             (unsigned)(g_gw_ar_sum / loops),
             g_audio_underruns - g_gw_under0,
             g_audio_starved - g_gw_starved0,
             (int)((int64_t)g_audio_step * 1000000 /
                   (int64_t)audio_nominal_step()) - 1000000,
             w.nacks, w.resends,
             (unsigned)gamepak_page_loads - g_gw_pg0a,
             GW_PAGES_B() - g_gw_pg0b);
   }
#endif
   gblink_win_reset(now);
}

#ifdef GPSP_PERF_RIG
/* Harness `gblink_cpu_x100` (PPSSPP only, docs/GB-LINK.md section 14): make
 * a link session's frames cost N/100 of what they cost here, by busy-waiting
 * after each emulated frame for (N-100)/100 of that frame's measured session
 * step (core_prof).  PPSSPP runs the two Game Boys ~2.4x faster than a PSP,
 * so without this it never shows a CPU-bound session at all; with it, heavy
 * scenes overrun the frame as they do on the hardware, scaled.  It is not
 * hardware timing: the I-cache, the radio and the WLAN threads are not
 * modelled -- only "the step costs this much". */
static unsigned g_gbl_cpu_x100;
static void gblink_cpu_scale(void)
{
   static unsigned calls0;
   static uint32_t tot0;
   unsigned calls;
   uint32_t tot, mx, d, burn;
   uint64_t t0;
   fe_host_core_prof(&calls, &tot, &mx);
   if (g_gbl_cpu_x100 > 100 && fe_host_gblink_active() && calls == calls0 + 1)
   {
      d = tot - tot0;
      burn = (uint32_t)((uint64_t)d * (g_gbl_cpu_x100 - 100) / 100);
      if (burn > 50000)
         burn = 50000;
      t0 = net_now_us();
      while (net_now_us() - t0 < burn)
         ;
   }
   calls0 = calls;
   tot0 = tot;
}
#endif

/* ---- a menu session's setup and end (docs/GBA-LINK-UI.md) ----------------
 *
 * The room kind the joiner asked for (ROOM_*), and the harness's
 * link_exit_after: frames to play on after a menu session ends, then exit
 * (0 after an agreed end, 4 otherwise) -- the two-PPSSPP rig's end. */
static int g_link_room_kind;
static unsigned g_link_exit_after;

/* While a menu session sets up: O cancels (the footer says so), and a
 * joiner learns what the session cannot tell it -- netdrv refused us (the
 * room has its two players), or nobody answered our JOIN while the room's
 * radio cell is plainly there (the host runs another protocol: a different
 * GBAdhoc release, or a GBA room seen from a GB console). */
/* "The other PSP runs a different version" needs a host that is THERE and
 * does not answer our JOIN: a host on another protocol still pumps (its
 * roster, every second, reaches us).  A host that sends nothing at all is
 * busy -- reloading its game for a session start or end, 18-25 s on a slow
 * card (2026-11-07: a re-join during the host's end-of-session reload read
 * as a version mismatch) -- or gone, and fe_gblink's own no-partner limit
 * (30 s, "The host left") covers that.  Heard = a frame in the last 2 s. */
static int link_host_heard(void)
{
   static uint32_t last_rx;
   static unsigned last_tick, heard_at;     /* heard_at: tick + 1, 0 = never */
   uint32_t rx = fe_np_rx_frames();
   if (g_link_setup_ticks <= 1 || g_link_setup_ticks < last_tick)
   {
      heard_at = 0;                          /* a new session's setup */
      last_rx = rx;
   }
   last_tick = g_link_setup_ticks;
   if (rx != last_rx)
   {
      last_rx = rx;
      heard_at = g_link_setup_ticks + 1;
   }
   return heard_at && g_link_setup_ticks + 1 - heard_at < 2u * 60u;
}

static void link_setup_watch(void)
{
   unsigned pad_new = g_pad & ~g_link_prev_pad;
   int heard;
   fe_gblink_progress pr;
   g_link_prev_pad = g_pad;
   g_link_setup_ticks++;
   if (g_link_why != LW_NONE)
      return;
   if (pad_new & PSP_CTRL_CIRCLE)
   {
      g_link_why = LW_CANCEL;
      fe_evt("link_cancel ticks=%u", g_link_setup_ticks);
      fe_host_gblink_request_end();
      return;
   }
   if (g_net_is_host)
      return;
   heard = link_host_heard();          /* every frame: it tracks the rx count */
   fe_host_gblink_facts(&pr);
   if (pr.peer_up)
      return;
   if (fe_np_stop_reason() == ND_STOP_REFUSED)
      g_link_why = LW_FULL;
   else if (fe_np_stop_reason() == ND_STOP_VERSION)
      g_link_why = LW_VERSION;
   else if (g_link_setup_ticks > 12u * 60u && (heard || !g_start_heard) &&
            (online_psp_enabled() ? online_psp_connected()
                                  : adhoc_transport_connected()))
      g_link_why = g_pcfg.console != FE_CONSOLE_GBA &&
                   g_link_room_kind == ROOM_ADAPTER ? LW_GBA_ROOM
                                                    : LW_VERSION;
   if (g_link_why != LW_NONE)
   {
      fe_evt("link_join_refused why=%d stop=%d ticks=%u", g_link_why,
             fe_np_stop_reason(), g_link_setup_ticks);
      fe_host_gblink_request_end();
   }
}

/* A menu session is DONE or FAILED.  What the player sees next:
 *   - agreed end: the game, "Link ended. Game saved.";
 *   - a cable host whose would-be partner never linked: it keeps hosting
 *     (a toast says why), the game never stopped for long;
 *   - a setup that did not link (joiner, or a GB host nobody joined): the
 *     LINKING screen's failure lines until O;
 *   - a link lost after it ran: GBA, each player carries on and saves as
 *     usual (owner, 2026-10-03; fe_host gba_cable_close); GB, the GB link's
 *     rule (both machines are clones: nothing saved).
 * Never before the partner has acknowledged what we sent (our FINAL or
 * ABORT), for at most ten seconds. */
static int link_end(int st)
{
   fe_gblink_progress pr;
   int host_cable = g_net_is_host && g_link_kind == LK_CABLE;
   int fail_screen, keep_room, desync, was_cable;
   fe_host_gblink_facts(&pr);
   desync = pr.error == FE_GBLINK_E_DESYNC ||
            (pr.error == FE_GBLINK_E_PEER_ABORT &&
             pr.peer_reason == FE_GBLINK_E_DESYNC);
   keep_room = st == FE_GBLINK_FAILED && !pr.ran && host_cable &&
               g_link_why != LW_CANCEL && g_net_up;
   fail_screen = st == FE_GBLINK_FAILED && !pr.ran && !keep_room &&
                 g_link_why != LW_CANCEL;
   if (g_gblink_ended_ticks++ == 0)
   {
      fe_evt("gblink_ended_screen state=%d ran=%d err=%s peer_reason=%d "
             "why=%d keep_room=%d t_ms=%llu", st, pr.ran,
             fe_gblink_error_text(pr.error), pr.peer_reason, g_link_why,
             keep_room, (unsigned long long)(sceKernelGetSystemTimeWide() /
                                             1000));
      adhoc_ctl_evt("end");
#ifndef GPSP_NO_TELEMETRY
      heap_census("gblink_end");
#endif
   }
   if (!fe_host_gblink_flushed() && g_gblink_ended_ticks <= 600)
      return 0;
   if (fail_screen)
   {
      unsigned pad_new = g_pad & ~g_link_prev_pad;
      g_link_prev_pad = g_pad;
      if (!g_link_fail_hold)
      {
         /* The radio goes now (the host is free for the next PSP); the
          * screen stays until O. */
         g_link_fail_hold = 1;
         fe_evt("link_fail_screen");
         net_teardown();
         return 0;
      }
      if (!(pad_new & PSP_CTRL_CIRCLE))
         return 0;
   }
   was_cable = g_link_kind == LK_CABLE || g_gba_cable;
   if (!keep_room)
      net_teardown();
   if (gblink_close_held() == -3)
   {
      /* section 19: the game could not be reloaded after the session */
      fe_evt("link_end fatal reason=reload");
      g_link_fatal_code = 4;
      g_link_fatal_why = "link_end_reload";
      g_running = 0;
      return 0;
   }
   cur_frame = NULL;
   g_gblink_ended_ticks = 0;
   g_gblink_screen = 0;
   g_link_mute = 0;
   g_link_fail_hold = 0;
   if (keep_room)
   {
      int e = pr.error == FE_GBLINK_E_PEER_ABORT && pr.peer_reason >= 0
                 ? pr.peer_reason : pr.error;
      g_link_wait = 1;
      g_link_clear = 1;
      osd_toast("%s", e == FE_GBLINK_E_ROM_MISSING || e == FE_GBLINK_E_ROM_BAD
                ? "A PSP with a different game tried to join."
                : "The other PSP left before the link started.");
      link_chip();
      fe_evt("link_host_keeps_room room=%s", g_link_room);
      g_link_why = LW_NONE;
      return 0;
   }
   if (st == FE_GBLINK_DONE)
      osd_toast("Link ended. Game saved.");
   else if (g_link_why == LW_CANCEL)
      osd_toast("Link cancelled.");
   else if (pr.ran && desync)
      osd_toast("The games fell out of step. Link ended, nothing saved.");
   else if (pr.ran)
      osd_toast("%s", was_cable && g_pcfg.console == FE_CONSOLE_GBA
                ? "Link lost: the other PSP left. Your game carries on."
                : "Link lost: the other PSP left. Nothing saved.");
   g_link_why = LW_NONE;
   g_link_menu = 0;
   if (g_link_exit_after)
   {
      g_gblink_after_code = st == FE_GBLINK_DONE ? 0 : 4;
      g_gblink_after_why = st == FE_GBLINK_DONE ? "gblink_done"
                                                : "gblink_failed";
      g_gblink_after_left = g_link_exit_after;
      fe_evt("link_exit_after frames=%u code=%d", g_link_exit_after,
             g_gblink_after_code);
   }
   return 0;
}

/* Once per loop iteration.  Returns 1 when a harness run should exit now,
 * with *code set. */
static int gblink_service(int have_script, int *code, const char **why)
{
   static unsigned ticks, script_done_ticks;
   static int end_asked;
#if defined(GPSP_PERF_RIG) && defined(GBA_LINK)
   if (g_gblink_repeat_restart)
   {
      /* gblink_repeat: the next session starts from scratch */
      g_gblink_repeat_restart = 0;
      end_asked = 0;
      script_done_ticks = 0;
   }
#endif
   int st;
   ticks++;
   g_gblink_screen = 0;
   if (g_gblink_after_left)
   {
      /* harness: the game plays on after the session, then the run ends */
      if (--g_gblink_after_left == 0)
      {
         g_gblink_exiting = 1;
         *code = g_gblink_after_code;
         *why = g_gblink_after_why;
         return 1;
      }
      return 0;
   }
   /* The boot-time bring-up (host=1 / join=1) left the session to start
    * here once the harness's save state was in. */
   if (g_gblink_role && !g_gblink_auto_started && g_net_up &&
       !g_autoload_state && !fe_host_gblink_active() &&
       (g_gblink_role != 1 || !g_gblink_host_wait_peer || fe_np_gb_peer_ready(NULL)) &&
       fe_host_frame_count() >= g_gblink_start_frames &&
       (g_pcfg.console != FE_CONSOLE_GBA || g_gba_cable))
   {
      g_gblink_auto_started = 1;
      if (gblink_begin(g_gblink_role == 1) != 0)
      {
         *code = 4;
         *why = "gblink_start";
         return 1;
      }
   }
   if (g_gblink_role && !g_gblink_auto_started && !g_net_up && ticks >= 30 &&
       !g_autoload_state &&
       (g_pcfg.console != FE_CONSOLE_GBA || g_gba_cable) &&
       fe_host_frame_count() >= g_gblink_start_frames)
   {
      int rc;
      g_gblink_auto_started = 1;
      /* a GBA cable room's protocol (docs/GBA-LINK-UI.md), as the menu's */
      fe_np_set_gb_protocol(g_gba_cable ? CABLE_PROTOCOL : NULL);
      rc = net_bringup_rend_off(g_gblink_role == 1, g_silent_group, g_pcfg.nick);
      if (rc == 0 && g_gblink_role == 1 && g_gblink_host_wait_peer)
      {
         /* the block above starts the session once the joiner is in */
         g_gblink_auto_started = 0;
         return 0;
      }
      if (rc != 0 || gblink_begin(g_gblink_role == 1) != 0)
      {
         fe_evt("gblink_auto_fail rc=%d stage=%s", rc, adhoc_transport_stage());
         *code = 4;
         *why = "gblink_start";
         return 1;
      }
   }
   /* A cable host waiting (docs/GBA-LINK-UI.md): its game runs; the session
    * starts the moment the other PSP is on the channel -- unless that is
    * the PSP it just turned away, which must leave first. */
   if (g_link_wait && g_net_up && !fe_host_gblink_active())
   {
      int ready = fe_np_gb_peer_ready(NULL);
      if (g_link_clear)
      {
         if (!ready)
         {
            g_link_clear = 0;
            fe_evt("link_host_clear room=%s", g_link_room);
         }
      }
      else if (ready)
      {
         g_link_wait = 0;
         fe_evt("link_host_peer room=%s", g_link_room);
         if (gblink_begin(1) != 0)
            return 0;
      }
   }
   if (!fe_host_gblink_active())
   {
      g_link_mute = 0;
      gblink_audio_session(0);
      fe_np_set_peer_dead_us(0);       /* no session starting: netdrv's 8 s */
      return 0;
   }
   st = fe_host_gblink_state();
   {
      /* A partner that is STARTING (beta.4 hotfix): until its HELLO, the
       * partner may be reloading its game (a cable host does that the
       * moment it admits us, 18-25 s on a slow card, silent on beta.4), so
       * its silence counts against the session's own no-partner limit
       * (fe_gblink NO_PEER_STEPS, 30 s), not netdrv's 8 s.  After it: 8 s. */
      fe_gblink_progress gp;
      gp.have_peer_header = 1;
      if (st == FE_GBLINK_SETUP)
         fe_host_gblink_facts(&gp);
      fe_np_set_peer_dead_us(!gp.have_peer_header && g_start_grace
                             ? LINK_START_GRACE_US : 0u);
   }
   if (st == FE_GBLINK_RUNNING || st == FE_GBLINK_ENDING)
   {
      char l1[64], l2[64];
      g_link_mute = 0;
      gblink_audio_session(1);
      gblink_audio_frame();
      gblink_win_note();
      if (!g_gblink_census)
      {
         /* Both machines and (if it was sent) the partner's cartridge are
          * in memory now: the number the PSP-1000 budget is about. */
         g_gblink_census = 1;
#ifndef GPSP_NO_TELEMETRY
         heap_census("gblink_running");
#endif
      }
      if (g_link_menu)
         link_chip();               /* "Link Cable  Room 7  P1" */
      else
      {
         fe_host_gblink_status(l1, sizeof(l1), l2, sizeof(l2));
         if (g_pcfg.osd_wireless)
            osd_chip_session(l1);
      }
      /* Harness: the script is over; let the games settle, then end. */
      if ((g_gblink_role == 1 || (g_link_menu && g_net_is_host)) &&
          have_script && !end_asked &&
          fe_autopilot_status() == 1 &&
          ++script_done_ticks >= g_gblink_end_after)
      {
         end_asked = 1;
         fe_host_gblink_request_end();
      }
      return 0;
   }
   g_gblink_screen = 1;
   gblink_audio_session(0);
   if (!g_link_mute)
   {
      g_link_mute = 1;
      ring_r = ring_w;           /* what was queued would replay stale */
      fe_evt("gblink_mute on state=%d", st);
   }
   if (st == FE_GBLINK_SETUP && g_link_menu)
      link_setup_watch();
   if ((st == FE_GBLINK_DONE || st == FE_GBLINK_FAILED) && g_link_menu)
      return link_end(st);
   if (st == FE_GBLINK_DONE || st == FE_GBLINK_FAILED)
   {
      if (g_gblink_ended_ticks++ == 0)
      {
         osd_toast("%s", st == FE_GBLINK_DONE ? "Link ended. Game saved."
                                              : "Link ended. Nothing saved.");
         /* When THIS console showed the result (its own clock): hw4's Y
          * ending showed it on the 1000 long before the 3000. */
         fe_evt("gblink_ended_screen state=%d t_ms=%llu", st,
                (unsigned long long)(sceKernelGetSystemTimeWide() / 1000));
         /* The cell at the end: a NO_PEER names its BSSID and the adhocctl
          * peer count here even with the periodic reporting off. */
         adhoc_ctl_evt("end");
#ifndef GPSP_NO_TELEMETRY
         /* Before anything is freed: what the session left (a start that
          * failed for memory is read here -- free vs largest block). */
         heap_census("gblink_end");
#endif
      }
      /* A second on a DONE screen (it was three), four on a failure (there
       * is something to read), then the game again
       * -- and never before the partner has acknowledged everything we sent
       * (our FINAL above all), for at most ten seconds. */
      if ((fe_host_gblink_flushed() || g_gblink_ended_ticks > 600) &&
          (g_gblink_ended_ticks > (st == FE_GBLINK_DONE ? 60u : 240u) ||
           g_gblink_role))
      {
         if (!fe_host_gblink_flushed())
            fe_evt("gblink_linger_timeout ticks=%u", g_gblink_ended_ticks);
         fe_evt("gblink_linger ticks=%u t_ms=%llu", g_gblink_ended_ticks,
                (unsigned long long)(sceKernelGetSystemTimeWide() / 1000));
         net_teardown();
         if (gblink_close_held() == -3)
         {
            /* section 19: the game could not be reloaded after the session */
            *code = 4;
            *why = "gblink_reload_failed";
            return 1;
         }
         cur_frame = NULL;
         g_gblink_ended_ticks = 0;
         g_gblink_screen = 0;
         g_link_mute = 0;
         if (g_gblink_role)
         {
            /* A harness run ends here, or plays on for gblink_after
             * frames first (the live link's "keep playing" check). */
            g_gblink_after_code = st == FE_GBLINK_DONE ? 0 : 4;
            g_gblink_after_why = st == FE_GBLINK_DONE ? "gblink_done"
                                                      : "gblink_failed";
            if (g_gblink_after)
            {
               g_gblink_after_left = g_gblink_after;
               fe_evt("gblink_play_on frames=%u", g_gblink_after);
               return 0;
            }
#if defined(GPSP_PERF_RIG) && defined(GBA_LINK)
            /* Harness gblink_repeat = K (GBA-LINK-MIRROR.md 19): K more
             * sessions in the same process, each from the saved state and
             * the script's first line -- session N+1 must hash as a fresh
             * boot's session does (instance B's static state is restored
             * from its pristine copy and its block zeroed every time). */
            if (g_gblink_repeat_left && st == FE_GBLINK_DONE &&
                g_script_path[0])
            {
               g_gblink_repeat_left--;
               fe_evt("gblink_repeat left=%u", g_gblink_repeat_left);
               fe_autopilot_load(g_script_path);
               g_autoload_state = 1;
               g_gblink_auto_started = 0;
               g_gblink_census = 0;
               g_gblink_repeat_restart = 1;
               return 0;
            }
#endif
            g_gblink_exiting = 1;
            *code = g_gblink_after_code;
            *why = g_gblink_after_why;
            return 1;
         }
      }
   }
   return 0;
}

/* A script's `disconnect`: ask the session to end before the next frame
 * runs, exactly where sesssim asks (tools/gblink/sesssim.c), so the agreed
 * end frame -- and the final hash -- match the reference run. */
static void gblink_script_disconnect(void)
{
#ifdef GPSP_PERF_RIG
   if (fe_autopilot_disconnect_pending() && fe_host_gblink_active())
   {
      fe_evt("gblink_script_disconnect");
      fe_host_gblink_request_end();
   }
#endif
}

/* The LINKING screen while the radio comes up (it blocks: nothing to
 * cancel yet; the online bring-up sets its own footer, O cancel, and polls
 * online_bringup_cancel).  g_link_kind is this session's. */
static void link_bringup_view(int is_host, const char *room,
                              ui_linking_view *lv, char *l1, size_t c1,
                              char *l2, size_t c2)
{
   memset(lv, 0, sizeof(*lv));
   lv->room = room;
   lv->l1 = l1;
   lv->l2 = l2;
   lv->frac = -1;
   lv->busy = 1;
   lv->footer = "";
   if (is_host)
   {
      snprintf(l1, c1, "Starting %s by %s", room,
               g_link_kind == LK_ADAPTER ? "Wireless Adapter" : "Link Cable");
      snprintf(l2, c2, "Opening the room...");
   }
   else
   {
      if (g_link_kind == LK_ADAPTER)
         snprintf(l1, c1, "Joining %s by Wireless Adapter", room);
      else
         snprintf(l1, c1, "Linking %s with the other PSP", room);
      snprintf(l2, c2, "Finding the room (up to 15 s)...");
   }
}

#ifdef GBADHOC_ONLINE
/* THE ONLINE BRING-UP'S CANCEL (PLAN r4-L3; owner 2026-11-08).  The Wi-Fi
 * association, the server lookup and the room can take ~50 s together, and
 * the power switch waits for them (pwr_hold("net") below), so O on the
 * LINKING screen ends them: polled by their waits through
 * online_psp_cancel_hook.  A new press only -- an O already down when the
 * screen came up is not one -- and it stays given once seen.  The screen
 * says so at once ("Cancelling...", the footer's O gone): what the press
 * ends still has to unwind (the room, the association, the net modules),
 * and until then nothing else on the screen would have changed. */
static unsigned g_ol_cancel_down;
static int g_ol_cancelled;
static ui_linking_view *g_ol_cancel_lv;   /* the LINKING screen that is up */
/* @extract:begin fn:online_bringup_cancel */
static int online_bringup_cancel(void)
{
   SceCtrlData pd;
   unsigned o;
   if (g_ol_cancelled)
      return 1;
   sceCtrlPeekBufferPositive(&pd, 1);
   o = pd.Buttons & PSP_CTRL_CIRCLE;
   if (o && !g_ol_cancel_down)
   {
      g_ol_cancelled = 1;
      fe_evt("online_cancel");
      if (g_ol_cancel_lv)
      {
         g_ol_cancel_lv->l2 = "Cancelling...";
         g_ol_cancel_lv->footer = "";
         ui_linking_draw(g_ol_cancel_lv);
         vid_swap();
      }
   }
   g_ol_cancel_down = o;
   return g_ol_cancelled;
}
/* @extract:end fn:online_bringup_cancel */
#endif

/* UI-driven wireless action (host=1 / host=0 join).  docs/GBA-LINK-UI.md:
 * the room's name is built from the mode (host) or read for it (join). */
static void ui_net_action(int is_host)
{
   int rc, kind, num, gba = g_pcfg.console == FE_CONSOLE_GBA;
   char room[ROOM_LABEL_CAP], group[9], code[5];
   char l1[80], l2[80];
   ui_linking_view lv;
#ifdef GBADHOC_ONLINE
   int ol = 0;                         /* this session is an Online one */
#endif
   if (g_net_up)
   {
      osd_toast("Already in a session");
      return;
   }
   /* Interlock, direction 2 (see mgift_start for direction 1).  Mystery Gift
    * has the radio associated with an access point; adhoc_transport_init would
    * take it into IBSS mode underneath the listener.  Refuse rather than
    * silently break one of the two. */
#ifdef GBADHOC_MGIFT
   if (mgift_ui_active())
   {
      osd_toast("Stop Mystery Gift first");
      return;
   }
#endif /* GBADHOC_MGIFT */
#ifdef GBADHOC_ONLINE
   /* EXPERIMENTAL online play (Wireless > Online; docs/ONLINE-TUNNEL.md):
    * the room on the GBAdhoc server first -- a joiner learns the host's link
    * mode from it -- then the same session as a room in the air. */
   {
      int ol_host = 0, ol_cable = 1, ol_wifi = 1;
      char ol_code[16];
      ol = ui_online_take(&ol_host, ol_code, &ol_cable, &ol_wifi);
      if (!ol && !g_online_boot)
         online_psp_clear();
      if (ol)
      {
         /* the same LINKING screen as below, so the code is shared, not cloned */
         /* ol_code is already "" for a host who typed none (online_room.h).
          * A joiner sends the host mode too: "Join with code" into a desktop
          * host's room makes the first PSP in its host (slot 0) */
         online_psp_set(NULL, ol_code, ol_wifi,
                        !gba || ui_host_cable() ? RL_MODE_CABLE : RL_MODE_ADAPTER);
         snprintf(room, sizeof(room), "Online");
         link_bringup_view(is_host, room, &lv, l1, sizeof(l1), l2, sizeof(l2));
         lv.footer = NULL;             /* "O cancel": online_bringup_cancel */
         link_backdrop_take();
         ui_linking_draw(&lv);
         vid_swap();
         /* The switch waits for the bring-up, as for net_bringup's (r4-L3):
          * a sleep inside it would land outside every park point. */
         pwr_hold("net");
         me_rend_suspend();
         {
            SceCtrlData pd;
            sceCtrlPeekBufferPositive(&pd, 1);
            g_ol_cancel_down = pd.Buttons & PSP_CTRL_CIRCLE;
            g_ol_cancelled = 0;
         }
         g_ol_cancel_lv = &lv;
         online_psp_cancel_hook(online_bringup_cancel);
         rc = online_psp_up(is_host, online_build_id(), is_host ? 20000u : 15000u);
         online_psp_cancel_hook(NULL);
         g_ol_cancel_lv = NULL;
         me_rend_resume();
         pwr_release();
         if (rc < 0)
         {
            osd_toast(s_online_fmt, online_psp_why());
            online_psp_clear();
            return;
         }
         if (rc > 0)
            is_host = 1;          /* the first into a desktop host's room hosts it */
      }
   }
#endif
   if (is_host)
   {
      int cable = gba && ui_host_cable();
      num = room_number(g_pcfg.group);
      if (num < 0)
         snprintf(group, sizeof(group), "%.8s", g_pcfg.group);  /* verbatim */
      else if (!gba)
      {
         room_code_gb(g_gb_title, sizeof(g_gb_title), code);
         room_make(ROOM_CABLE_GB, num, code, group, sizeof(group));
      }
      else if (cable)
      {
         room_code_gba(g_game_code, code);
         room_make(ROOM_CABLE_GBA, num, code, group, sizeof(group));
      }
      else
         room_make(ROOM_ADAPTER, num, NULL, group, sizeof(group));
      kind = !gba ? ROOM_CABLE_GB : cable ? ROOM_CABLE_GBA : ROOM_ADAPTER;
   }
   else
   {
      snprintf(group, sizeof(group), "%.8s", ui_group());
#ifdef GBADHOC_ONLINE
      if (ol)
      {
         /* the host's link mode, from the server */
         int m = online_psp_room_mode();
         if (!gba)
         {
            room_code_gb(g_gb_title, sizeof(g_gb_title), code);
            room_make(ROOM_CABLE_GB, 1, code, group, sizeof(group));
         }
         else if (m == RL_MODE_ADAPTER)
            room_make(ROOM_ADAPTER, 1, NULL, group, sizeof(group));
         else
         {
            room_code_gba(g_game_code, code);
            room_make(ROOM_CABLE_GBA, 1, code, group, sizeof(group));
         }
      }
#endif
      kind = room_parse(group, &num, code);
   }
   g_link_room_kind = kind;
   g_link_kind = !gba ? LK_GB : kind == ROOM_CABLE_GBA ? LK_CABLE : LK_ADAPTER;
   if (g_link_kind == LK_CABLE && !g_gba_cable_ok)
   {
      g_link_kind = LK_NONE;
#ifdef GBADHOC_ONLINE
      /* The online bring-up above already joined the room on the server
       * and associated with the AP (F4): take both down again, as a failed
       * bring-up below would (net_teardown_io), or the PRX, the AP and the
       * room stay up behind a game that is not linking.  Under the hold,
       * as net_teardown's: the AP drop and the module unload run outside
       * the loop's park point (see power_cb). */
      if (ol)
      {
         pwr_hold("net");
         online_psp_down();
         online_psp_clear();
         pwr_release();
      }
#endif
      osd_toast("Link Cable needs a restart of the game");
      return;
   }
   g_gba_cable = g_link_kind == LK_CABLE;
   fe_np_set_gb_protocol(g_gba_cable ? CABLE_PROTOCOL : NULL);
   room_label(group, room, sizeof(room));
#ifdef GBADHOC_ONLINE
   if (ol)
      snprintf(room, sizeof(room), s_online_fmt, online_psp_room());
#endif
   snprintf(g_link_room, sizeof(g_link_room), "%s", room);
   fe_evt("link_action role=%s group=%s kind=%d link=%d", is_host ? "host"
          : "join", group, kind, g_link_kind);

   /* The LINKING screen for the blocking adhocctl bring-up (<= 15 s). */
   link_bringup_view(is_host, room, &lv, l1, sizeof(l1), l2, sizeof(l2));
   link_backdrop_take();
   ui_linking_draw(&lv);
   vid_swap();

   /* The renderer off for np_start's heap and back from the post-session
    * heap, under one hold as the online bring-up above. */
   rc = net_bringup_rend_off(is_host, group, g_pcfg.nick);
   if (rc == 0)
   {
      g_link_menu = 1;
      if (g_link_kind == LK_CABLE && is_host)
      {
         /* The host keeps playing; the session starts when the other PSP
          * arrives (gblink_service). */
         g_link_wait = 1;
         g_link_clear = 0;
         osd_toast("Hosting %s by Link Cable. Waiting for the other PSP.",
                   room);
         link_chip();
      }
      else if (g_link_kind == LK_ADAPTER)
      {
         osd_toast("%s %s", is_host ? "Hosting" : "Joined", room);
         link_chip();
      }
      else
         gblink_begin(is_host);         /* cable joiner, or GB/GBC */
      return;
   }
   g_link_kind = LK_NONE;
   if (!g_gblink_role)
      g_gba_cable = 0;
   fe_np_set_gb_protocol(NULL);
   if (rc == ADHOC_ERR_WLAN_OFF)
      osd_toast("WLAN switch is OFF");
   else if (rc == ADHOC_ERR_NO_GROUP)
      osd_toast("%s not found - is the host still there?", room);
   else if (rc == ADHOC_ERR_NP_START)
      osd_toast("Radio OK, netdrv failed: %s", g_np_start_why);
#ifdef GBADHOC_ONLINE
   else if (rc == ADHOC_ERR_ONLINE)
      osd_toast(s_online_fmt, online_psp_why());
#endif
   else
      osd_toast("Wireless start failed (%s)", adhoc_transport_stage());
}

