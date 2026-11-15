/* ---- Mystery Gift (2.0.5) ------------------------------------------------
 *
 * A Mystery Gift session is NOT a trading session and shares none of its state,
 * but it does reuse the whole stack above the radio.  The phone plays a Pokemon
 * distribution cart: it speaks netdrv's own GPN1 frames, with rfu.c's RFU1
 * frames inside them and the game's Mystery Gift link protocol inside those.
 * GBAdhoc translates the radio and nothing else -- infrastructure Wi-Fi to the
 * phone's hotspot instead of the AGB-015 adapter the cart used -- and then
 * FireRed's own Mystery Gift menu receives a real Wonder Card and saves it
 * itself.  We never touch the save.
 *
 * OWNERSHIP, as implemented (this comment previously described a design that
 * was not taken -- it named netdrv/transport_inet.c as a third nd_transport
 * backend, and that file is not in psp/Makefile's OBJS and is never linked):
 *
 *   psp/mgift_net.c   brings up infrastructure Wi-Fi AND owns the UDP socket
 *                     (sceNetInetSocket/Bind), talking to the phone directly.
 *   psp/mgift_cart.c  connects the received parcel to the core's netpacket
 *                     seam.
 *
 * NO netdrv session is created for Mystery Gift.  netdrv, rfu.c and the ad-hoc
 * path are untouched, which is what keeps trading from regressing; the two
 * modes are interlocked as mutually exclusive below because they cannot share
 * the radio.
 *
 * Protocol spec: MysteryGiftStation/GBAdhoc_MysteryGift_Protocol_v2.md.
 */
#ifdef GBADHOC_MGIFT
static char       g_mg_l1[64];         /* screen line 1: what is happening */
static char       g_mg_l2[64];         /* screen line 2: detail */
/* LINE 2 IS SHARED (r4-L1): a status message ("Open MYSTERY GIFT in the
 * game", the IP and game, "no gift yet") owns it until this frame; only then
 * do the b/c/cmd diagnostics take it, and only when they change.  The
 * diagnostics used to be rewritten every frame, after the message, so the
 * player never saw a message for more than one frame. */
#define MG_L2_HOLD_FRAMES 180u
static unsigned   g_mg_l2_hold;
static int        g_mg_l2_diag;        /* line 2 holds the diagnostics */
static void mg_l2_hold(void)
{
   g_mg_l2_hold = fe_host_frame_count() + MG_L2_HOLD_FRAMES;
   g_mg_l2_diag = 0;
}
#endif /* GBADHOC_MGIFT */
static char       g_mg_game[5];        /* running ROM header code at 0xAC */
#ifdef GBADHOC_MGIFT
static int        g_mg_linked;         /* the game connected to the cart */

/* THE DISTRIBUTION NETWORK.
 *
 * The player names their Android Mobile Hotspot exactly this, with Security set
 * to Open and Band 2.4 GHz, and we create a matching connection profile on the
 * PSP automatically.  Two halves of one agreed constant: nothing to type on
 * either device beyond naming the hotspot once.
 *
 * OPEN, not WPA.  Android's own hotspot screen offers Security: Open, and that
 * is the only kind of access point a PSP can join with no key at all.  An
 * app-started Wi-Fi Direct group was tried first and abandoned: it cannot be
 * open (P2P mandates an 8-character passphrase), it is the same radio as the
 * hotspot so Android refuses to run both, and -- decisively -- the PSP could not
 * see the group when scanning at all. */
static unsigned   g_mg_secs;           /* seconds spent waiting, for the UI */

int         mgift_ui_active(void) { return g_mg_on; }
const char *mgift_ui_line1(void)  { return g_mg_l1; }
const char *mgift_ui_line2(void)  { return g_mg_l2; }
#endif /* GBADHOC_MGIFT */

/* The running game 4-character header code, read from the ROM FILE rather than
 * from the core.  gba_memory.h declares `extern char gamepak_code[5]` but
 * nothing ever defines it -- a stale declaration that would not link -- and the
 * alternative is exporting a new core global for four bytes the frontend can
 * read for itself.  Called once, at boot. */
static void rom_loading_progress(u32 loaded, u32 total)
{
   ui_loading_update(total ? "Reading ROM" : "Checking cartridge", loaded, total);
}

static void rom_loading_stage(const char *stage)
{
   ui_loading_update(stage, 0, 0);
}

static void mgift_read_game_code(const char *rom_path)
{
   FILE *fh;

   g_mg_game[0] = '\0';
   if (!rom_path || !rom_path[0])
      return;
   fh = fopen(rom_path, "rb");
   if (!fh)
      return;
   if (fseek(fh, 0xAC, SEEK_SET) == 0 && fread(g_mg_game, 1, 4, fh) == 4)
      g_mg_game[4] = '\0';
   else
      g_mg_game[0] = '\0';
   fclose(fh);
}

#ifdef GBADHOC_MGIFT
/* The OSD must read "Mystery Gift", not "joined" or "host" (user direction).
 * It reuses the session chip because the two modes are mutually exclusive, so
 * nothing can ever need both at once. */
static void mgift_chip(void)
{
   snprintf(g_session_info, sizeof(g_session_info), "Mystery Gift");
   osd_chip_session(g_pcfg.osd_wireless ? g_session_info : NULL);
}

static void mgift_stop_io(int quiet)
{
   if (!g_mg_on)
      return;
   /* Reverse order of mgift_start: the cart first (it tells the game goodbye
    * through the netpacket seam, which must still be attached), then the card
    * socket, then the association. */
   mgift_cart_stop();
   mgnet_card_close();
   mgnet_wifi_down();
   /* The profile STAYS.  It is named "GBAdhoc Mystery Gift" and carries the
    * agreed SSID, so it shows up in the network row here and in the PSP's own
    * settings, ready for next time -- which is the point: the player should
    * never have to create it.  mgnet_profile_ensure adopts it again rather than
    * making a second one. */
   /* Logged before the state is cleared: it said linked=0 always (r4-L2). */
   fe_evt("mg_stop linked=%d", g_mg_linked);
   g_mg_on     = 0;
   g_mg_linked = 0;
   g_session_info[0] = '\0';
   osd_chip_session(NULL);
   snprintf(g_mg_l1, sizeof(g_mg_l1), "Not listening");
   g_mg_l2[0] = '\0';
   g_mg_l2_diag = 0;
   if (!quiet)
      osd_toast("Mystery Gift off");
}

/* pwr_hold: see power_cb -- this runs outside the loop's park point. */
static void mgift_stop(int quiet)
{
   pwr_hold("mgift");
   mgift_stop_io(quiet);
   pwr_release();
}

/* The games the cart serves: mgift_cart.c answers a GAME_DATA only with
 * these codes (English scripts).  Anything else never asks, so Wi-Fi must
 * not come up for it: that association blocks the main thread for up to
 * MGNET_CONNECT_US (30 s) and writes a network profile, all for a game
 * that cannot receive a gift. */
/* @extract:begin fn:mgift_game_ok */
static int mgift_game_ok(void)
{
   return !strcmp(g_mg_game, "BPRE") || !strcmp(g_mg_game, "BPGE") ||
          !strcmp(g_mg_game, "BPEE");
}
/* @extract:end fn:mgift_game_ok */

/* @extract:begin fn:mgift_start_io */
static void mgift_start_io(void)
{
   int rc;

   /* Interlock, direction 1.  The trading link holds the radio in ad-hoc mode;
    * associating with an AP would pull its IBSS out from under netdrv. */
   if (g_net_up)
   {
      osd_toast("Leave the wireless session first");
      return;
   }
   if (g_mg_on)
   {
      osd_toast("Mystery Gift is already listening");
      return;
   }
   /* Before Wi-Fi, not after: mgift_cart_start() succeeds for every GBA
    * game (the netpacket seam always exists), so its "no wireless" answer
    * below could only come after a 30 s association. */
   if (!mgift_game_ok())
   {
      snprintf(g_mg_l1, sizeof(g_mg_l1), "This game has no Mystery Gift");
      snprintf(g_mg_l2, sizeof(g_mg_l2), "Needs FireRed, LeafGreen or Emerald (US/EU English)");
      osd_toast("%s", g_mg_l1);
      fe_evt("mg_refused game=%s", g_mg_game[0] ? g_mg_game : "?");
      return;
   }

   /* One feedback frame: the association blocks for seconds (DHCP + WPA against
    * a phone hotspot) and silence there looks exactly like a hang. */
   vid_overlay_begin(1);
   vid_text_center(110, "Mystery Gift", 0xFFFF);
   vid_text_center(140, "Joining your network...", 0x65BF);
   vid_overlay_end();
   vid_swap();

   g_mg_linked = 0;
   g_mg_secs   = 0;
   snprintf(g_mg_l1, sizeof(g_mg_l1), "Joining your network...");
   g_mg_l2[0] = '\0';

   /* Automatic selects the exact open hotspot after the network stack is up.
    * The saved profile label is independent of the phone's SSID.
    * (The renderer goes and comes back under mgift_start's pwr_hold.) */
   me_rend_suspend();
   rc = mgnet_wifi_up(ui_mgift_conf(), 0);
   me_rend_resume();

   if (rc != MGNET_OK)
   {
      /* Every failure gets its own sentence.  A release build has no log, so
       * the toast is the only diagnosis the owner will ever get, and "it did
       * not work" is the one message that helps nobody. */
      switch (rc)
      {
      case MGNET_ERR_WLAN_OFF:
         snprintf(g_mg_l1, sizeof(g_mg_l1), "WLAN switch is OFF");
         break;
      case MGNET_ERR_NO_CONFIG:
         snprintf(g_mg_l1, sizeof(g_mg_l1), "Could not prepare the hotspot");
         snprintf(g_mg_l2, sizeof(g_mg_l2), "Profile error %08X; check free slots", mgnet_last_sce());
         break;
      case MGNET_ERR_CONNECT:
      case MGNET_ERR_CONNECT_TIMEOUT:
         snprintf(g_mg_l1, sizeof(g_mg_l1), "Could not join the network");
         snprintf(g_mg_l2, sizeof(g_mg_l2), "Is the hotspot on? (%s 0x%08X)",
                  mgnet_stage(), mgnet_last_sce());
         break;
      default:
         snprintf(g_mg_l1, sizeof(g_mg_l1), "Wi-Fi failed (%s 0x%08X)",
                  mgnet_stage(), mgnet_last_sce());
         break;
      }
      osd_toast("%s", g_mg_l1);
      mgnet_profile_release();
      return;
   }

   /* The card socket: one datagram from the phone carrying a real 332-byte
    * Wonder Card.  No netdrv and no session -- see mgift_net.h. */
   if (mgnet_card_open() != 0)
   {
      snprintf(g_mg_l1, sizeof(g_mg_l1), "Could not open the socket");
      osd_toast("%s", g_mg_l1);
      mgnet_wifi_down();
      mgnet_profile_release();
      return;
   }

   /* Now become a distribution cart to the emulated game.  This attaches to the
    * core's netpacket seam; it does not touch netdrv or the ad-hoc path. */
   /* mgift_game_ok() above has refused every game without Mystery Gift, so
    * this fails only if the core gave no netpacket interface (a defensive
    * path: the GBA core always has one).  Say what failed, not "no
    * wireless", which the game check now answers. */
   if (mgift_cart_start() != 0)
   {
      snprintf(g_mg_l1, sizeof(g_mg_l1), "Could not attach to the game");
      g_mg_l2[0] = '\0';
      osd_toast("%s", g_mg_l1);
      mgnet_card_close();
      mgnet_wifi_down();
      mgnet_profile_release();
      return;
   }

   g_mg_on = 1;
   /* This attempt's adapter facts only (r4-L6): the S/C flags were only
    * ever set, so a second start in the same game -- or one after a Union
    * Room visit -- showed S1 C1 before the game had done anything. */
   g_rfu_seen_bcrd = 0;
   g_rfu_seen_conn = 0;
   g_rfu_last_cmd = 0xFFF;
   g_rfu_last_state = 0;
   snprintf(g_mg_l1, sizeof(g_mg_l1), "Waiting for a gift");
   snprintf(g_mg_l2, sizeof(g_mg_l2), "%s  game %s", mgnet_local_ip(),
            g_mg_game[0] ? g_mg_game : "?");
   mg_l2_hold();
   mgift_chip();
   osd_toast("Mystery Gift listening");
   fe_evt("mg_start ip=%s game=%s port=%d", mgnet_local_ip(), g_mg_game,
          MGNET_LISTEN_PORT);
}
/* @extract:end fn:mgift_start_io */

/* pwr_hold: see power_cb -- this runs outside the loop's park point. */
static void mgift_start(void)
{
   pwr_hold("mgift");
   mgift_start_io();
   pwr_release();
}
#endif /* GBADHOC_MGIFT */

/* The running game as the link menus need it, read once at boot while the
 * path is in scope: the GBA header code (g_mg_game, read just before), the
 * GB title, the linkmode.txt key (the ROM under roms/, the key favourites
 * use) and a name for screens (the table's title, else the ROM's). */
static void link_read_identity(const char *rom_path)
{
   const char *base, *t;
   size_t n = strlen(p_rom_dir);
   g_game_code[0] = g_gb_title[0] = '\0';
   if (g_pcfg.console == FE_CONSOLE_GBA)
      snprintf(g_game_code, sizeof(g_game_code), "%.4s", g_mg_game);
   else if (rom_path && rom_path[0])
   {
      FILE *fh = fopen(rom_path, "rb");
      if (fh)
      {
         if (fseek(fh, 0x134, SEEK_SET) == 0 &&
             fread(g_gb_title, 1, 16, fh) == 16)
            g_gb_title[16] = '\0';
         else
            g_gb_title[0] = '\0';
         fclose(fh);
      }
   }
   if (!rom_path)
      rom_path = "";
   base = strrchr(rom_path, '/');
   if (n && !strncmp(rom_path, p_rom_dir, n) && rom_path[n] == '/')
      snprintf(g_game_key, sizeof(g_game_key), "%s", rom_path + n + 1);
   else
      snprintf(g_game_key, sizeof(g_game_key), "%s",
               base ? base + 1 : rom_path);
   t = g_game_code[0] ? link_game_title(g_game_code) : NULL;
   if (t)
      snprintf(g_game_name, sizeof(g_game_name), "%s", t);
   else
   {
      char *dot;
      snprintf(g_game_name, sizeof(g_game_name), "%s",
               base ? base + 1 : rom_path);
      dot = strrchr(g_game_name, '.');
      if (dot && dot != g_game_name)
         *dot = '\0';
   }
}

/* Save and load states are locked while a session (or Mystery Gift) is up
 * -- but not while a cable host only waits: the link starts from the live
 * game whenever the other PSP arrives (docs/GBA-LINK-UI.md). */
static int link_locks_states(void)
{
   return (g_net_up && !g_link_wait) || g_mg_on || fe_host_gblink_active();
}

/* Any link or wireless session at all, a cable host that is only WAITING
 * and Mystery Gift included: the test cheats and turbo buttons share. */
static int link_session_any(void)
{
   int on = g_net_up || g_mg_on || fe_host_gblink_active();
#ifdef GBA_LINK
   on = on || fe_gbadual_active();
#endif
   return on;
}

/* TURBO BUTTONS ARE OFF IN EVERY LINK OR WIRELESS SESSION, as cheats are and
 * by the same test (docs/CONTROL-REMAP.md section 13): what crosses a link
 * is the player's own presses.  While one is up a turbo button does nothing
 * at all -- its PSP button is bound to nothing else (the conflict rule), so
 * the game sees no input from it -- and pressing one says why.
 *
 * Called once per loop iteration that is about to run the game, with that
 * iteration's pad.  A REWIND build passes `playing = 0` for the whole of a
 * rewind gesture (control held, or a landing still in progress): every
 * train stops, so nothing fires while the game runs backwards and a Toggle
 * does not carry on by itself when play resumes.  psp_rewind_land() and the
 * reset of docs/REWIND.md 22 stop them too.  The rewind's
 * input log holds each frame's RESOLVED buttons, pulses included, so a
 * replayed stretch is exact and no turbo state is ever part of a snapshot. */
#ifdef GPSP_PERF_RIG
/* Harness `ui_turbo_lock = 1`: turbo buttons behave, and the pages read, as
 * they do in a link session -- which one PPSSPP cannot hold. */
static int g_turbo_lock_sim;
#else
#define g_turbo_lock_sim 0
#endif

static void turbo_frame(unsigned pad, unsigned pad_new, int playing)
{
   uint8_t was[CTL_TURBO_N];
   int locked, i;
   if (!playing)
   {
      ctl_turbo_stop(&g_turbo);
      return;
   }
   locked = link_session_any() || g_turbo_lock_sim;
   ui_set_turbo_locked(locked);
   memcpy(was, g_turbo.run, sizeof(was));
   ctl_turbo_pad(&g_pcfg.controls, &g_turbo, pad, pad_new,
                 fe_host_frame_count(), !locked);
   if (locked && (pad_new & ctl_turbo_bound(&g_pcfg.controls)))
   {
      fe_evt("turbo_locked pad_new=0x%04x", pad_new);
      osd_toast("Turbo is off for link play");
      return;
   }
   /* A Toggle that starts or stops says so: auto-fire that is running with
    * nothing held must never be a surprise. */
   for (i = 0; i < CTL_TURBO_N; i++)
      if (g_pcfg.controls.turbo_toggle[i] && was[i] != g_turbo.run[i])
      {
         const ctl_action_info *in = ctl_info(CTL_TURBO_FIRST + i);
         fe_evt("turbo_toggle key=%s on=%d f=%u", in->key, g_turbo.run[i],
                fe_host_frame_count());
         osd_toast("%s: auto-fire %s", in->label,
                   g_turbo.run[i] ? "on" : "off");
      }
}

#ifdef GPSP_CHEATS
/* CHEATS ARE OFF IN EVERY LINK OR WIRELESS SESSION (docs/CHEATS.md section 6).
 * Wider than link_locks_states(): a cable host that is only WAITING counts
 * too (the link starts from the live game the moment the other PSP arrives,
 * and both consoles must then compute the same frames), and so does Mystery
 * Gift.  Polled once per main-loop iteration rather than hooked into each
 * way a session can start or end: there are a dozen of those, and a missed
 * one would be a desync.  The player's choices are kept; they stop being
 * applied, and are applied again when the session is over. */
static void cheats_link_poll(void)
{
   int lock = link_session_any();
   if (lock == fe_cheats_locked())
      return;
   fe_cheats_lock(lock);
   fe_evt("cheats_lock on=%d enabled=%u", lock, fe_cheats_enabled_count());
   if (fe_cheats_enabled_count())
      osd_toast(lock ? "Cheats off for link play" : "Cheats back on");
}
#endif

/* The session as the menus draw it (ui_set_link). */
static void link_ui_view(void)
{
   ui_link_view v;
   memset(&v, 0, sizeof(v));
   if (g_net_up && g_link_kind != LK_NONE)
   {
      int st = fe_host_gblink_state();
      v.state = g_link_wait ? UI_LINK_HOSTING : UI_LINK_LINKED;
      v.cable = g_link_kind != LK_ADAPTER;
      v.player = g_link_kind == LK_ADAPTER ? (g_net_is_host ? 1 : 2)
               : (st == FE_GBLINK_RUNNING || st == FE_GBLINK_ENDING)
                    ? (g_net_is_host ? 1 : 2) : 0;
      v.room = g_link_room;
      v.game = g_game_name;
      v.fast = -1;
      if (st == FE_GBLINK_RUNNING || st == FE_GBLINK_ENDING)
         v.fast = fe_host_fast_link(&v.fast_src);
   }
   ui_set_link(&v);
}

#ifdef GBADHOC_MGIFT
/* Once per frontend frame: poll the phone's parcel and advance the local cart.
 * Keep this separate from net_frame(), which owns trading-session pacing. */
static void mgift_frame(void)
{
   if (!g_mg_on)
      return;

   /* Acknowledge only after the cart accepts the complete parcel. Identical
    * retries preserve an in-progress session; a different gift gets busy. */
   {
      static mgift_parcel parcel;
      if (mgnet_card_poll(&parcel))
      {
         int rc = mgift_cart_set_parcel(&parcel);
         mgnet_card_reply(parcel.id, rc == 0 ? 0 : 1);
         if (rc == 0)
         {
            snprintf(g_mg_l1, sizeof(g_mg_l1), "Gift ready");
            snprintf(g_mg_l2, sizeof(g_mg_l2), "Open MYSTERY GIFT in the game");
            mg_l2_hold();
            osd_toast("Gift uploaded from phone");
         }
      }
   }

   /* Drive the cart: advertise, handshake, and run the transfer once the game
    * opens its own Mystery Gift menu. */
   mgift_cart_frame();

   {
      /* DIAGNOSTICS THAT SURVIVE A RELEASE BUILD.
       *
       * fe_evt compiles out here, so if the game never sees the cart the owner
       * has nothing to report but "it did not work" -- which is what happened
       * the first time.  These three numbers split the whole problem:
       *
       *   b = broadcasts we have sent.  0 means the cart never attached to the
       *       emulator's link layer at all (wrong game, or no RFU mode).
       *   c = connection requests the GAME sent us.  0 with b rising means it
       *       is not recognising the beacon -- the 24 broadcast bytes or the
       *       distributor serial.
       *   >0 means we are past discovery and into the block protocol. */
      mgift_cart_stats s;
      mgift_cart_get_stats(&s);
      if (!g_mg_linked && s.linked)
      {
         g_mg_linked = 1;
         snprintf(g_mg_l1, sizeof(g_mg_l1), "Game connected");
         osd_toast("Game connected to the cart");
      }
      {
         static unsigned last[6];
         static const char *last_st;
         unsigned now[6];
         const char *st = mgift_cart_status();
         now[0] = (unsigned)s.bcasts;    now[1] = (unsigned)s.connects;
         now[2] = g_rfu_last_cmd & 0xFF; now[3] = g_rfu_last_state;
         now[4] = (unsigned)g_rfu_seen_bcrd;
         now[5] = (unsigned)g_rfu_seen_conn;
         if ((int)(fe_host_frame_count() - g_mg_l2_hold) >= 0 &&
             (!g_mg_l2_diag || st != last_st ||
              memcmp(now, last, sizeof(now)) != 0))
         {
            memcpy(last, now, sizeof(now));
            last_st = st;
            g_mg_l2_diag = 1;
            snprintf(g_mg_l2, sizeof(g_mg_l2), "%s b%u c%u cmd%02x st%u S%u C%u",
                     st, now[0], now[1], now[2], now[3], now[4], now[5]);
         }
      }
   }

   /* Say how long we have been waiting.  "Looking for the Station" that never
    * changes is indistinguishable from a hang, and the usual cause is the
    * phone being on a different network rather than anything being broken. */
   if (!g_mg_linked)
   {
      unsigned s = fe_host_frame_count() / 60u;
      if (s != g_mg_secs)
      {
         g_mg_secs = s;
         if (s >= 10u && (s % 5u) == 0u && !mgift_cart_has_card())
         {
            snprintf(g_mg_l2, sizeof(g_mg_l2),
                     "%s  no gift yet (%us)", mgnet_local_ip(), s);
            mg_l2_hold();
         }
      }
   }

   /* A gift transfer is seconds of mostly-silent waiting; do not let the
    * console suspend in the middle of one. */
   scePowerTick(PSP_POWER_TICK_ALL);
}
#endif /* GBADHOC_MGIFT */

