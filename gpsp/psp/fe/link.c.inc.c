static void log_adhoc_up(const char *group)
{
   FE_EVT_ONLY(group);
   nd_transport tp;
   uint8_t mac[6];
   adhoc_transport_iface(&tp);
   tp.local_addr(tp.ctx, mac);
   fe_evt("adhoc_up group=%s mac=%02x:%02x:%02x:%02x:%02x:%02x",
          group && group[0] ? group : ADHOC_GROUP_DEFAULT,
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
   adhoc_ctl_evt("up");
}

/* Start the netdrv/netpacket layer over the (already-up) transport. */
/* Reason text from the last failed netdrv start, for the toast.  A release
 * build has no log, so the message on screen is the only diagnosis the owner
 * ever gets -- it has to be true and it has to be specific. */
static char g_np_start_why[32];
/* A GBA link-cable session (GBA_LINK builds, harness gblink_role + gba_dual:
 * docs/GBA-LINK-MIRROR.md section 14): the GB link session's channel and
 * flow on a GBA game, none of RFU's session pacing. */
static int g_gba_cable;
/* GBA-LINK-MIRROR.md 19: instance B loads at session start (harness
 * gba_dual_late; the boot-time cable reserve is then not taken) */
#ifdef GBA_LINK
static int g_gba_late;
#else
#define g_gba_late 0
#endif
#if defined(GPSP_PERF_RIG) && defined(GBA_LINK)
static unsigned g_gblink_repeat_left;
static int g_gblink_repeat_restart;
static char g_script_path[192];
#endif
/* This boot can run one (instance B loaded for sessions): the harness's
 * gblink_role + gba_dual, or the player's Wireless menu (docs/
 * GBA-LINK-UI.md).  g_gba_cable is then per session: 1 while the session
 * up (or being brought up) is a cable one, 0 for an adapter one. */
static int g_gba_cable_ok;
/* GBA link-cable session memory, reserved at boot (GBA-LINK-MIRROR.md 15.3).
 * The core takes ROM blocks until malloc fails at load ("heap exhausted by
 * design"), so a session's buffers allocated later fail on a console with no
 * spare heap: on the PSP-1000 model AW2 (8 MiB) went resident and left
 * 1.25 MB, and the join's fe_gblink_create failed.  A session holds, at its
 * peak (end), its own and the pre-link state + battery, the partner's state
 * + battery and the final copies: 2,284,912 B measured (PPSSPP, AW2,
 * gblink_pre -> gblink_end).  The reserve is taken before the ROM cache and
 * handed back the moment the session starts, so the session's allocations
 * land in it; the ROM cache pages instead (pg= in EVT gblink_win).
 * Harness cable_reserve_kb overrides (0 = none, the old behaviour). */
static void *g_cable_reserve;

static int net_start_np(int is_host, const char *group, const char *nick,
                        int probe)
{
   static nd_transport tp;               /* fe_np copies the vtable */
   fe_np_config npc;

   if (online_psp_enabled())
      online_psp_iface(&tp);
   else
      adhoc_transport_iface(&tp);
#ifdef GPSP_KEEP_TELEMETRY
   g_np_start_tid = sceKernelGetThreadId();    /* see player_screenshot */
#endif
   memset(&npc, 0, sizeof(npc));
   npc.transport = &tp;
   npc.is_host   = is_host;
   npc.nick      = nick;
   npc.now_us    = net_now_us;
   npc.probe     = probe;
   if ((g_pcfg.console == FE_CONSOLE_GBA && !g_gba_cable ?
        fe_np_start(&npc) : fe_np_start_gb(&npc)) != 0)
   {
      /* Free memory goes in the log because "out of memory" is the reason a
       * 32 MB console fails where a 64 MB one does not, and the number says
       * whether it was exhaustion or fragmentation. */
      fe_evt("net_error reason=np_start why=\"%s\" free=%d max_block=%d",
             fe_np_start_reason(), sceKernelTotalFreeMemSize(),
             sceKernelMaxFreeMemSize());
      snprintf(g_np_start_why, sizeof(g_np_start_why), "%s",
               fe_np_start_reason());
      return ADHOC_ERR_NP_START;
   }
   g_net_up = 1;
   g_net_is_host = is_host;
   g_group_lost_logged = 0;
   {
      /* The chip and the Wireless status line name the room as the menus
       * do ("Room 7", room_label.h); the group id itself is unchanged. */
      char lbl[ROOM_LABEL_CAP];
      snprintf(g_session_info, sizeof(g_session_info), "%s  %s",
               is_host ? "hosting" : "joined",
               room_label(group && group[0] ? group : ADHOC_GROUP_DEFAULT,
                          lbl, sizeof(lbl)));
   }
   /* Settings -> "Session overlay" hides the chip; g_session_info itself is
    * kept regardless — the wireless menu's status line reads it. */
   osd_chip_session(g_pcfg.osd_wireless ? g_session_info : NULL);
   link_chip();                 /* a menu session names its mode instead */
   return 0;
}

/* Re-apply the session-chip visibility after a settings change, without
 * touching the session itself. */
void osd_session_chip_refresh(void)
{
   if (g_net_up && g_link_kind != LK_NONE)
      link_chip();
   else if (g_net_up)
      osd_chip_session(g_pcfg.osd_wireless ? g_session_info : NULL);
}

/* The fast-forward and session-pacing engine moved to psp/ff_psp.c.
 * 1052 lines and 23 state variables; see ff_psp.h for the interface and
 * for the six host symbols it still reads. */
#if defined(FX_SWEEP) && defined(GPSP_PERF_RIG)
/* FILTERS V2 COST SWEEP (rig builds with -DFX_SWEEP only): one run walks the
 * display arms below, FXS_WIN frames each, and prints per arm what the frame
 * cost -- the blit's CPU stage copy, its GE list build, the CPU's stall on
 * the GE, the Media Engine's render time and the core's own time, per frame
 * -- so every filter is priced in ONE run on one console, against arm 0
 * (and arm 0 again last, for drift).  The core state never sees the arms. */
static const struct { signed char sc, sm, fx, col; const char *nm; }
g_fxs_gba[] = {
   { VID_SCALE_FIT, 0, VID_FX_NONE, 0, "fit" },
   { VID_SCALE_FIT, 2, VID_FX_NONE, 0, "fit_sharp" },
   { VID_SCALE_FIT, 0, VID_FX_LCD, 0, "fit_lcd" },
   { VID_SCALE_FIT, 2, VID_FX_VINTAGE, 0, "fit_sharp_vintage" },
   { VID_SCALE_FIT, 0, VID_FX_NONE, 1, "fit_gbacolor" },
   { VID_SCALE_FIT, 0, VID_FX_NONE, 2, "fit_bw" },
   { VID_SCALE_STRETCH, 0, VID_FX_NONE, 0, "stretch" },
   { VID_SCALE_STRETCH, 2, VID_FX_DOT, 1, "stretch_sharp_dot_gbacolor" },
   { VID_SCALE_INT2, 0, VID_FX_NONE, 0, "2x" },
   { VID_SCALE_INT2, 0, VID_FX_SCAN, 2, "2x_scan_bw" },
   { VID_SCALE_FIT, 0, VID_FX_NONE, 0, "fit_again" },
}, g_fxs_gb[] = {
   { VID_SCALE_FIT, 0, VID_FX_NONE, 0, "fit" },
   { VID_SCALE_FIT, 2, VID_FX_LCD, 0, "fit_sharp_lcd" },
   { VID_SCALE_STRETCH, 0, VID_FX_NONE, 0, "stretch" },
   { VID_SCALE_STRETCH, 0, VID_FX_RGB, 0, "stretch_rgb" },
   { VID_SCALE_STRETCH, 2, VID_FX_RGB_VIVID, 0, "stretch_sharp_rgbstrong" },
   { VID_SCALE_INT2, 0, VID_FX_NONE, 0, "2x" },
   { VID_SCALE_INT2, 0, VID_FX_VINTAGE, 0, "2x_vintage" },
   { VID_SCALE_FIT, 0, VID_FX_NONE, 0, "fit_again" },
};
static int g_fxs_arm = -1;
#define FXS_WIN 300u   /* frames per arm */
static void fx_sweep_step(unsigned frames)
{
   static unsigned rn0, rus0, cc0, t0;
   static uint32_t ct0;
   unsigned n, st, stm, gu, gum, wt, wtm, cc, now;
   uint32_t ct, cm;
   int gb = pcfg_display_console() != FE_CONSOLE_GBA;
   int narm = gb ? (int)(sizeof(g_fxs_gb) / sizeof(g_fxs_gb[0]))
                 : (int)(sizeof(g_fxs_gba) / sizeof(g_fxs_gba[0]));
   if (!frames || (frames % FXS_WIN) != 0)
      return;
   now = (unsigned)sceKernelGetSystemTimeLow();
   vid_blit_prof(&n, &st, &stm, &gu, &gum, &wt, &wtm);
   fe_host_core_prof(&cc, &ct, &cm);
   if (g_fxs_arm >= 0 && g_fxs_arm < narm && n)
   {
      unsigned rn = g_fxs_rn - rn0, ru = g_fxs_ru - rus0;
      fe_evt("fxcost console=%s arm=%d name=%s n=%u stage=%u gu=%u wait=%u "
             "me_rend=%u me_n=%u core=%u wall=%u",
             pcfg_console_tag(pcfg_display_console()), g_fxs_arm,
             gb ? g_fxs_gb[g_fxs_arm].nm : g_fxs_gba[g_fxs_arm].nm, n,
             st / n, gu / n, wt / n, rn ? ru / rn : 0u, rn,
             cc - cc0 ? (unsigned)(ct - ct0) / (cc - cc0) : 0u,
             (now - t0) / FXS_WIN);
   }
   if (++g_fxs_arm < narm)
   {
      g_h_scale  = gb ? g_fxs_gb[g_fxs_arm].sc : g_fxs_gba[g_fxs_arm].sc;
      g_h_smooth = gb ? g_fxs_gb[g_fxs_arm].sm : g_fxs_gba[g_fxs_arm].sm;
      g_h_effect = gb ? g_fxs_gb[g_fxs_arm].fx : g_fxs_gba[g_fxs_arm].fx;
      g_h_color  = gb ? 0 : g_fxs_gba[g_fxs_arm].col;
      psp_display_apply("sweep");
   }
   /* The window that follows is the arm's: counters from here. */
   vid_blit_prof(&n, &st, &stm, &gu, &gum, &wt, &wtm);
   rn0 = g_fxs_rn;  rus0 = g_fxs_ru;
   fe_host_core_prof(&cc0, &ct0, &cm);
   t0 = (unsigned)sceKernelGetSystemTimeLow();
}
#endif

static void blit_prof_evt(unsigned frames)
{
   static unsigned last;
   unsigned n, st, stm, gu, gum, wt, wtm;

   if (!frames || frames == last || (frames % 600) != 0)
      return;
   last = frames;
   vid_blit_prof(&n, &st, &stm, &gu, &gum, &wt, &wtm);
   if (!n)
      return;
   /* `wait` is a SUBSET of `gu` (ADR-0038), not another term of `tot` — the
    * sum below is deliberately still stage+gu.  Read it as: of `gu`, `wait`
    * is stalled-on-the-GE and the remainder is list building. */
   fe_evt("blit_prof mode=%d name=%s n=%u stage=%u/%u gu=%u/%u wait=%u/%u "
          "tot=%u",
          vid_blit_mode(), vid_blit_mode_name(vid_blit_mode()), n,
          st / n, stm, gu / n, gum, wt / n, wtm, (st + gu) / n);
}

/* Full wireless bring-up around the booted core.  Returns 0 on success. */
static int g_gblink_role;    /* harness: 0 off, 1 host, 2 join (below) */

static int net_bringup_io(int is_host, const char *group, const char *nick,
                          int probe)
{
   int rc;


   if (g_save_path_for_backup)
      backup_save(g_save_path_for_backup);

   /* ADR-0021: keep sceNetAdhocPdpSend off the emulation thread unless the
    * user turned it off for an A/B.  Must precede init (the thread is part
    * of the bring-up ladder). */
   adhoc_transport_set_tx_thread(g_pcfg.net_tx_thread);

   /* EXPERIMENTAL online play (docs/ONLINE-TUNNEL.md): the same session over
    * a relay / direct path on infrastructure Wi-Fi instead of ad hoc. */
   if (online_psp_enabled())
   {
      rc = online_psp_up(is_host, online_build_id(),
                         g_gblink_role ? 120000u : 30000u);
      if (rc < 0)
      {
         fe_evt("net_error reason=online why=\"%s\"", online_psp_why());
         return ADHOC_ERR_ONLINE;   /* never -1: that reads "WLAN switch is OFF" */
      }
      fe_evt("adhoc_up group=%s via=online", group && group[0] ? group : "-");
      rc = net_start_np(is_host, group, nick, probe);
      if (rc != 0)
      {
         online_psp_down();
         return rc;                 /* ADHOC_ERR_NP_START */
      }
      fe_evt("mem_free=%d max_block=%d net=up via=online",
             sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
      return 0;
   }

   /* A join joins the host's scanned BSS and never creates a group:
    * connect-by-name made a second GBLNK7 whenever its own quick scan
    * missed the host (hw3 run 9, hw4 auto001/003: both consoles up in
    * "GBLNK7", rx=0 for 30 s, even after a scan had found the host). */
   if (is_host)
      rc = adhoc_transport_init(group, 0);
   else
   {
      int tries = 0, ch = 0;
      uint32_t ms = 0;
      uint8_t b[6] = { 0 };
      /* Harness: 90 s, well past any relaunch skew (ejects up to 20 s
       * apart) and the host's 30 s no-peer wait.  Player: 15 s -- the
       * room was on the list a moment ago. */
      rc = adhoc_transport_init_join(group, 0, g_gblink_role ? 90000u
                                                              : 15000u);
      adhoc_transport_join_info(&tries, &ms, &ch, b);
      FE_EVT_ONLY(tries); FE_EVT_ONLY(ms); FE_EVT_ONLY(ch);
      fe_evt("adhoc_join group=%s rc=%d tries=%d ms=%u channel=%d "
             "bssid=%02x:%02x:%02x:%02x:%02x:%02x", group, rc, tries,
             (unsigned)ms, ch, b[0], b[1], b[2], b[3], b[4], b[5]);
   }
   if (rc != ADHOC_OK)
   {
      net_error_evt("adhoc_init", rc);
      return rc;
   }
   log_adhoc_up(group);

   rc = net_start_np(is_host, group, nick, probe);
   if (rc != 0)
   {
      /* Its own code, ADHOC_ERR_NP_START: -1 was ADHOC_ERR_WLAN_OFF, and
       * the 1000 field report (net arena) said "WLAN switch is OFF" with
       * the switch on (r3-B1). */
      adhoc_transport_term();
      return rc;
   }
   if (g_pcfg.console == FE_CONSOLE_GBA && !g_gba_cable)
      skip_policy_session_begin();       /* ADR-0019 */
   /* Order matters: the mode line first (it names the policy that is about to
    * act), then the policy's own `EVT session_pace`. */
   if (g_pcfg.console == FE_CONSOLE_GBA && !g_gba_cable)
   {
      /* Report the rate that will actually be APPLIED, with the request
       * beside it — ADR-0035: the two differ whenever the snap fires, and a
       * line that showed only the request would be the same silent lie the
       * field caught. */
      unsigned vb = 1;
      unsigned req  = (unsigned)g_pcfg.net_session_fps_x100;
      unsigned appl = pace_snap_x100(req, &vb);
   FE_EVT_ONLY(appl);
      fe_evt("net_pace_match mode=%d policy=%s nominal=%u.%02u floor=%u.%02u "
             "fixed=%u.%02u req=%u.%02u snap=%d vblanks=%u ramp=%u.%02u",
             g_pcfg.net_pace_match,
             g_pcfg.net_pace_match == PACE_MODE_FIXED        ? "fixed"    :
             g_pcfg.net_pace_match == PACE_MODE_ADAPTIVE     ? "adaptive" :
             g_pcfg.net_pace_match == PACE_MODE_BACKPRESSURE ? "backpressure"
                                                             : "off",
             PACE_NOMINAL_X100 / 100, PACE_NOMINAL_X100 % 100,
             PACE_FLOOR_X100 / 100, PACE_FLOOR_X100 % 100,
             appl / 100, appl % 100, req / 100, req % 100,
             g_pcfg.net_session_fps_snap, vb,
             PACE_FIXED_RAMP_X100_PER_S / 100,
             PACE_FIXED_RAMP_X100_PER_S % 100);
   }
   if (g_pcfg.console == FE_CONSOLE_GBA && !g_gba_cable)
      pace_session_begin();                         /* ADR-0033 / ADR-0027 */
   fe_evt("net_tx_thread mode=%d", adhoc_transport_tx_thread_active());

   /* Netdrv (ADR-0016 profile: 144 B slots, 384-deep ring) + the 128 KiB
    * sceNet pool just landed: log what is left of the ADR-0007 headroom. */
   fe_evt("mem_free=%d max_block=%d net=up",
          sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
   return 0;
}

/* pwr_hold: see power_cb -- this runs outside the loop's park point. */
static int net_bringup(int is_host, const char *group, const char *nick,
                       int probe)
{
   int rc;
   pwr_hold("net");
   rc = net_bringup_io(is_host, group, nick, probe);
   pwr_release();
   return rc;
}

/* THE BRING-UP MADE WHILE A GAME RUNS (the silent one, the harness's GB link
 * auto-start, the menu's): the renderer out of the way first -- heap-backed,
 * it may hold the largest contiguous block on the 32 MB models, and np_start
 * needs that block for its netdrv arena (run 407) -- and back after, on
 * success and on failure, so a failed join still restores the ME.  ONE hold
 * over all three steps: net_bringup's own ends before the renderer is back
 * on, and power_cb's unparked branch counts on every path that turns it on
 * being under one.  One copy for the three callers, not inlined. */
/* @extract:begin fn:net_bringup_rend_off */
static __attribute__((noinline)) int net_bringup_rend_off(int is_host,
      const char *group, const char *nick)
{
   int rc;
   pwr_hold("net");
   me_rend_suspend();
   rc = net_bringup(is_host, group, nick, 0);
   me_rend_resume();
   pwr_release();
   return rc;
}
/* @extract:end fn:net_bringup_rend_off */

/* Disconnect-crash diagnosis: transport_adhoc.c calls this before each blocking
 * WLAN teardown step.  fe_evt flushes every line, so a mid-run Disconnect that
 * wedges (the force-quit black screen) leaves the offending step as the LAST
 * line in the log — turning an un-reproducible hang into a one-run pinpoint. */
void gpsp_adhoc_step(const char *step)
{
   FE_EVT_ONLY(step);
   fe_evt("adhoc_step name=%s", step);
}

static void net_teardown_io(void)
{
   if (!g_net_up)
      return;
   fe_evt("td_step name=begin");
   adhoc_stats_evt();
   sess_cost_evt(1);                                         /* ADR-0021 */
   fe_evt("td_step name=pre_np_stop");
   fe_np_stop();
   fe_evt("td_step name=pre_transport_term");
   if (online_psp_enabled())
   {
      online_psp_down();
#ifdef GBADHOC_ONLINE
      if (!g_online_boot)
         online_psp_clear();     /* the next session is ad hoc unless asked */
#endif
   }
   else
      adhoc_transport_term();
   fe_evt("td_step name=post_transport_term");
   g_net_up = 0;
   if (g_pcfg.console == FE_CONSOLE_GBA && !g_gba_cable)
      skip_policy_session_end();                            /* ADR-0019 */
   fe_evt("td_step name=post_skip_policy");
   /* ADR-0033: in fixed mode this only re-aims the goal at nominal — the
    * glide back up runs from the main loop over the next ~5 s.  In mode 0/2
    * it is the old hard reset. */
   if (g_pcfg.console == FE_CONSOLE_GBA && !g_gba_cable)
      pace_session_end();
   fe_evt("td_step name=post_pace");
   osd_chip_session(NULL);
   /* The next session picks its own kind (docs/GBA-LINK-UI.md); a harness
    * gblink_role run keeps the one it booted with. */
   if (!g_gblink_role)
      g_gba_cable = 0;
   g_link_kind = LK_NONE;
   g_link_wait = g_link_clear = 0;   /* g_link_menu: link_end clears it */
   fe_np_set_gb_protocol(NULL);
   fe_evt("td_step name=done");
}

/* pwr_hold: see power_cb -- this runs outside the loop's park point. */
static void net_teardown(void)
{
   pwr_hold("net");
   net_teardown_io();
   pwr_release();
}

/* harness `telemetry_period`: frames between the periodic telemetry lines
 * (fe_host heartbeat + net_frame's adhoc_stats/sess_cost/frame_hist); 0 =
 * the usual 600.  A timing arm sets it past the session's length, so the
 * whole session reports once, at teardown, and the log writer sleeps. */
static unsigned g_telemetry_period;

/* What the admission check tells a player (netdrv_wire.h). */
static const char *link_version_text(void)
{
   return fe_np_refused_kind() == 2
             ? "The other PSP runs an older GBAdhoc version."
             : "The other PSP runs a different GBAdhoc version.";
}

/* Per-frame wireless service, called from the main loop (its own counter:
 * core frames freeze while the menu is open, the pump must not). */
static void net_frame(void)
{
   static unsigned ticks;
   if (!g_net_up)
      return;
   ticks++;
   {
      unsigned depth = adhoc_transport_rx_depth();  /* EVT gblink_win rxq_max */
      if (depth > g_gbl_rxq_max)
         g_gbl_rxq_max = depth;
   }
   fe_np_pump();                          /* delivers + emits net_stats */

   /* The driver could not honour the RELIABLE contract: the core's RFU
    * state is unrecoverable from here (ADR-0016).  Say so and tear down
    * rather than keep a dead link nominally "up" — the silent version of
    * this is what produced the field's mystery comm-errors. */
   if (fe_np_failed())
   {
      osd_toast("Wireless error: link overloaded");
      net_teardown();
      return;
   }

   /* Turned away for its version (netdrv_wire.h).  A host stays up for the
    * next PSP.  A cable or GB joiner has the LINKING screen for this
    * (link_setup_watch); an adapter joiner's game is running. */
   if (fe_np_take_refused())
   {
      if (g_net_is_host)
         osd_toast("A PSP with %s GBAdhoc version could not join.",
                   fe_np_refused_kind() == 2 ? "an older" : "a different");
      else if (g_pcfg.console == FE_CONSOLE_GBA && !g_gba_cable)
      {
         osd_toast("%s", link_version_text());
         net_teardown();
         return;
      }
   }

   if ((ticks % 60) == 0)
      scePowerTick(PSP_POWER_TICK_ALL);                    /* no suspend
                                            mid-session (plan §9) */
   if ((ticks % (g_telemetry_period ? g_telemetry_period : 600u)) == 0)
   {
      adhoc_ctl_evt("hb");
      adhoc_stats_evt();
      online_psp_stats_evt("hb");
      sess_cost_evt(0);                    /* ADR-0021 */
   }
   if (g_pcfg.console == FE_CONSOLE_GBA && !g_gba_cable)
      skip_policy_frame();                /* ADR-0019 */
   if (!(online_psp_enabled() ? online_psp_connected()
                              : adhoc_transport_connected()) &&
       !g_group_lost_logged)
   {
      /* adhocctl DISCONNECT event (group dissolved / radio lost): netdrv
       * will also see peer death — log the root cause once. */
      fe_evt("adhoc_group_lost");
      osd_toast("Wireless group lost");
      g_group_lost_logged = 1;
   }
}

/* ----- silent wireless (per-game variant builds; ADR-0013) ---------------
 * When the game completes the RFU adapter login (weak core hook), the
 * frontend auto-negotiates a session on a fixed group with zero UI:
 * join first, and if no host answers within a jittered window, promote to
 * host (contention backoff: the jitter makes simultaneous activations
 * pick different promote times, so one side becomes host first and the
 * other's still-running JOIN retry loop latches onto it).  role=host/join
 * in variant.ini pins the role instead.  WLAN off => persistent overlay
 * warning, no retry storm. */

/* rfu.c, ADR-0042.  The core has no header; the frontend/core interface here
 * is a set of loose externs, so this follows the existing convention. */
extern void rfu_set_rx_cap(unsigned n);
/* ADR-0075.  At most N non-empty host-packet deliveries to the game per
 * emulated frame, gated consistently in RECV_DATA and rfu_data_avail() so the
 * WAITEVENT re-poll spin that sank the rfu_rx_cap arms cannot occur.  1 equals
 * both the game's drain rate and the real radio's cadence.  0 = off. */
extern void rfu_set_frame_pace(unsigned n);
extern void rfu_set_pace_max_hold(unsigned n);
extern void rfu_set_cushion(unsigned n);     /* fixed-depth jitter buffer PoC */
extern void rfu_set_shed_keep(unsigned n);   /* link shedding, 0 = off */
extern void rfu_set_hold(unsigned on);        /* hold, never discard */
extern void rfu_set_supersede(unsigned on);   /* adapter one-buffer rule */
extern void rfu_set_fault_corrupt(unsigned n); /* desync-detector validation */
extern void rfu_set_fault_span(unsigned n);
extern void rfu_set_fault_mode(unsigned m);
extern void adhoc_set_disc_await(int on);    /* candidate mid-run-Disconnect fix */
/* ADR-0059.  Percent, 100 = stock.  Stretches the emulated adapter's two
 * cycle-counted deadlines to compensate for a radio ~10x slower than the one
 * the game was written for.  See the long comment in rfu.c. */
extern void rfu_set_timeout_scale(unsigned to_x100, unsigned rtx_x100);
/* ADR-0062d.  Minimum emulated cycles between netpacket_poll_receive() calls
 * while the adapter waits.  0 = every call, the historical behaviour. */
extern void rfu_set_poll_min_cycles(unsigned n);
/* ADR-0068.  The mirror knob: minimum emulated cycles between polls while the
 * adapter is NOT waiting — the host's cadence, which is otherwise once per
 * frame.  0 = never = the historical behaviour.  See rfu.c. */
extern void rfu_set_idle_poll_cycles(unsigned n);
/* ADR-0074.  1 = hold a peer disconnect until the client's queued host packets
 * have been handed to the game, so the Union Room's exit negotiation is not
 * deleted in transit.  0 = the historical immediate teardown. */
extern void rfu_set_disc_defer(unsigned n);
/* ADR-0076.  Hold the adapter in CLIENT state for N frames after the peer's
 * disconnect so the game can finish its clock-master change and issue its OWN
 * disconnect -- the clean, error-free exit.  The lobby error is a race the
 * slow client loses (link_rfu_2.c:2492 fires unconditionally when our IDLE
 * answer wins); this gives the game the window the real exit sequence needs.
 * 0 = off = historical.  60 (~1 s) is the arm to try first. */
extern void rfu_set_disc_grace(unsigned frames);

/* ADR-0045: the core reads this once per emulated frame, never per command. */
unsigned gpsp_rfu_now_us(void)
{
   return (unsigned)net_now_us();
}

/* ADR-0043 flight recorder.  Called from the EMULATION thread at the moment
 * the adapter is reset out of an active session — i.e. after the game has
 * already declared its fatal error.  It writes no file itself: its lines go
 * through fe_evt like every other event (queued for the io thread while the
 * game runs), so the "no ms0: I/O on the emu thread" rule holds here too, and
 * a release, where fe_evt compiles out, returns before formatting anything.
 * Emits a compact decoded transcript, oldest first. */
void gpsp_rfu_flight_dump_hook(const unsigned *ring, unsigned cap,
                               unsigned start, unsigned n)
{
   static const char *stn[] = { "idle", "host", "conn", "clnt" };
   unsigned i;
   unsigned prev_frame = 0, prev_us = 0;
   unsigned worst_ms = 0, worst_frame = 0;
   FE_EVT_ONLY(worst_frame);
   int have_prev = 0;

#ifdef GPSP_NO_TELEMETRY
   /* Release: every fe_evt below compiles out, but the loop would still
    * format the whole ring -- up to 512 snprintf calls on the emulation
    * thread at the instant a live session ends -- into lines nobody sees. */
   (void)stn; (void)ring; (void)cap; (void)start; (void)n; (void)i;
   (void)prev_frame; (void)prev_us; (void)worst_ms; (void)have_prev;
   return;
#endif

   /* EIGHT entries per line, not one.
    *
    * One-per-line lost 228 of 1024 entries to the event queue on the very
    * first gated run -- `evtio=.../228/16383` -- and lost them SILENTLY: the
    * transcript simply had holes, in a diagnostic whose entire value is being
    * complete.  Packing cuts the line count 8x, which both fits the queue and
    * makes the dump readable.
    *
    * Field order per entry:  <cmd>:<state>:<gap-in-frames>
    * `cmd` >= 0xF0 is a state transition and its low nibble is the cause.
    *
    * Each LINE also carries `+<ms>`: wall-clock milliseconds since the
    * previous line.  ADR-0045 -- the frame gap alone is blind to the failure
    * the user could see happening.  During a save the core trips SMC detection
    * ~4096 times and a frame stretches to 38 ms against a 33.4 ms budget; in
    * frame-count that is identical to a healthy 16 ms frame, so a transcript
    * can read "perfectly regular" while the console falls behind a peer that
    * does not slow down with it.  Real time is the axis the peer lives on. */
   fe_evt("rfu_flight begin n=%u first=%u net=%s fmt=cmd:state:gap +ms=wallclock",
          n, start, g_net_up ? "up" : "down");

   for (i = 0; i < n; i += 8)
   {
      char line[224];
      int  off = 0;
      unsigned j;
      unsigned e0   = ((start + i) % cap) * 2;
      unsigned base = ring[e0] & 0x3FFFFF;
      unsigned bus  = ring[e0 + 1];
      unsigned dms  = have_prev ? (bus - prev_us) / 1000u : 0u;

      for (j = i; j < n && j < i + 8; j++)
      {
         unsigned e     = ((start + j) % cap) * 2;
         unsigned cmd   = (ring[e] >> 24) & 0xFF;
         unsigned st    = (ring[e] >> 22) & 0x3;
         unsigned frame =  ring[e] & 0x3FFFFF;
         unsigned gap   = have_prev ? frame - prev_frame : 0;
         int      w     = snprintf(line + off, sizeof(line) - (size_t)off,
                                   "%s%02x:%s:%u",
                                   j == i ? "" : " ", cmd, stn[st], gap);
         if (w < 0 || (size_t)w >= sizeof(line) - (size_t)off)
            break;
         off += w;
         prev_frame = frame;
         prev_us    = ring[e + 1];
         have_prev  = 1;
      }
      if (dms > worst_ms) { worst_ms = dms; worst_frame = base; }
      fe_evt("rfu_fl f=%u +%ums %s", base, dms, line);
   }

   /* The headline for a human: the longest real-time hole in the capture, and
    * where.  Eight commands span well under a frame in steady state, so a line
    * gap much above ~35 ms means the console stopped keeping up with its peer. */
   fe_evt("rfu_flight end worst_gap_ms=%u at_frame=%u", worst_ms, worst_frame);
}

