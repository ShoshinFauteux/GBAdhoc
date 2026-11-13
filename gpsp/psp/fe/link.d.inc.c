/* Replace this process with a fresh boot of ourselves.  On CFW an EBOOT.PBP
 * must be launched the way the FIRMWARE launches it; plain sceKernelLoadExec
 * returns instead of replacing the process.  argp becomes the new argv[0],
 * where every ms0 path is derived from.  Returns ONLY on failure.
 *
 * `gamelist`: this relaunch is "Quit to game list" falling back from the
 * in-process return; argv[1] = "gamelist" tells the new process, whose
 * browser then fades in from black instead of cutting in. */
#define RELAUNCH_ARGV1 "gamelist"

/* XMB LAUNCH (issue #29, docs/XMB-LAUNCH.md).  A per-game launcher EBOOT in
 * the XMB starts this install with argv[1] = "launch" and argv[2] = the ROM:
 * an absolute "dev:/..." path, or one relative to roms/.  The first session
 * boots that ROM with the game list's own save and state paths, so a game
 * is the same game from the list or from its XMB entry; Exit goes back to
 * the XMB and Quit to game list opens the list.  Taken once: the in-process
 * return and every relaunch come back to the browser. */
#define LAUNCH_ARGV1 "launch"
static char g_launch_rom[PSP_ROM_REL_PATH_CAP];

static void relaunch_eboot(int gamelist)
{
   pwr_release_all();
   char eboot[160 + sizeof(RELAUNCH_ARGV1)];
   size_t n;
   struct SceKernelLoadExecVSHParam param;
   snprintf(eboot, 160, "%s/EBOOT.PBP", g_dir_base);
   n = strlen(eboot) + 1;
   if (gamelist)
   {
      memcpy(eboot + n, RELAUNCH_ARGV1, sizeof(RELAUNCH_ARGV1));
      n += sizeof(RELAUNCH_ARGV1);
   }
   memset(&param, 0, sizeof(param));
   param.size = sizeof(param);
   param.args = (SceSize)n;
   param.argp = eboot;
   param.key  = "game";
   sctrlKernelLoadExecVSHMs2(eboot, &param);
}

volatile int g_rfu_activated;
void gpsp_rfu_activated_hook(void)   /* overrides the weak rfu.c stub */
{
   g_rfu_activated = 1;
}

/* The emulated RFU link went down — i.e. the GAME ended the wireless
 * session (the player walked out of the Union Room), as distinct from us
 * tearing the transport down.  Both looked identical in the field logs.
 * Called from the emulation thread, so only latch here; the main loop
 * does the (fflushing) log write. */
static volatile unsigned g_rfu_down_pending;   /* reason<<8 | slot, +1 */

/* THE EXIT-STATE SNAPSHOT (fs-pace campaign, 2026-08-09).
 *
 * Run 305/309's flight recorder shows the CLIENT'S GAME issuing a deliberate
 * RFU_CMD_DISCONNECT on a healthy link during the Trade Center walk-out, and
 * the user sees the recoverable "press A" screen on both consoles.  The v9
 * fixture probes sample too late: link_rfu_2.c:961 consumes disconnectMode
 * and the post-warp game has re-cleaned status/errorState, so every
 * after-the-fact read is zeros.  The only instant the causal state exists is
 * the disconnect itself — which is EXACTLY when rfu.c calls this hook.
 *
 * So snapshot guest RAM here.  Emulation thread == the core's own thread, so
 * fe_host_mem_read is an in-thread memcpy; five reads, link-down only (a
 * handful per session).  ADDRESSES ARE EMERALD USA (pret symbols branch,
 * validated against production gMain):
 *   0x03000E19 sRfuKeepAliveTimer   61+ = overworld.c:2285's watchdog FIRED
 *   0x03005CE4 gRfu.disconnectMode  2 = RFU_DISCONNECT_ERROR = LinkRfu_
 *                                   FatalError (the watchdog path); 1 = NORMAL
 *   0x030050F1 gRfu.status          2 = CONNECTION_ERROR
 *   0x030050EE gRfu.errorState      1 = OCCURRED -> the press-A screen
 *   0x03000E14 sPlayerKeyInterceptCallback  WHICH wait episode was installed
 *   0x03000E10 sPlayerLinkStates[4] 2 = EXITING_ROOM, per byte
 * Junk values on any other ROM are harmless: this is a read-only harness
 * diagnostic keyed to a log line, not a behaviour change. */
static volatile unsigned g_rfu_down_ka, g_rfu_down_dm, g_rfu_down_st;
static volatile unsigned g_rfu_down_es, g_rfu_down_kcb, g_rfu_down_pls;
static volatile unsigned g_rfu_down_rcl;   /* gRfu.readyCloseLink[0..3] */

void gpsp_rfu_link_down_hook(unsigned reason, unsigned slot)
{
   unsigned char b = 0;
   unsigned w = 0;
   if (fe_host_mem_read(0x03000E19, &b, 1) == 0) g_rfu_down_ka = b;
   if (fe_host_mem_read(0x03005CE4, &b, 1) == 0) g_rfu_down_dm = b;
   if (fe_host_mem_read(0x030050F1, &b, 1) == 0) g_rfu_down_st = b;
   if (fe_host_mem_read(0x030050EE, &b, 1) == 0) g_rfu_down_es = b;
   if (fe_host_mem_read(0x03000E14, &w, 4) == 0) g_rfu_down_kcb = w;
   if (fe_host_mem_read(0x03000E10, &w, 4) == 0) g_rfu_down_pls = w;
   /* Did the peer's READY_CLOSE_LINK arrive before the death?  If rcl shows
    * the host ready while pls shows our own echo missing, the stall is
    * provably in the KEY channel alone, and a state-assist repair (write
    * the local link state the protocol already agreed on) is safe. */
   if (fe_host_mem_read(0x030050E4, &w, 4) == 0) g_rfu_down_rcl = w;
   g_rfu_down_pending = ((reason & 0xFF) << 8) | (slot & 0xFF) | 0x10000;
}

static void rfu_link_down_drain(void)
{
   unsigned v = g_rfu_down_pending;
   if (!v)
      return;
   g_rfu_down_pending = 0;
   fe_evt("rfu_link_down reason=%u slot=%u net=%s",
          (v >> 8) & 0xFF, v & 0xFF, g_net_up ? "up" : "down");
   fe_evt("rfu_exit_state ka=%u dm=%u st=%u es=%u kcb=0x%08x pls=0x%08x "
          "rcl=0x%08x",
          g_rfu_down_ka, g_rfu_down_dm, g_rfu_down_st, g_rfu_down_es,
          g_rfu_down_kcb, g_rfu_down_pls, g_rfu_down_rcl);
}

/* ---- ADR-0079: THE EXIT-ECHO STATE ASSIST -------------------------------
 *
 * The Trade Center exit dies the same way every run, and the death snapshot
 * proved it three times over (runs 311/313/315, byte-identical):
 * `rfu_exit_state ka=61 dm=1 kcb=0x080871c5 pls=0x81818183 rcl=0x0`.
 * Decoded: the join is parked in KeyInterCB_WaitForPlayersToExit; it has
 * ALREADY processed the host's EXIT_ROOM key (peer state 0x83 EXITING_ROOM);
 * only its OWN key's round-trip echo is missing (local 0x81 BUSY); and the
 * 61-frame keepalive watchdog (overworld.c:2285) then declares the link
 * fatally dead — on a link that is alive and idle.
 *
 * WHY THE ECHO CANNOT ARRIVE.  On real hardware the child's key reaches the
 * parent and the parent's SAME exchange returns the aggregated keys: the
 * echo completes inside one frame, before the parent's game can move on.
 * Over a transport with real latency the parent processes the key, sees all
 * players exiting, and advances into the exit script — after which it stops
 * relaying keys — before its last aggregated frame reaches the child.  The
 * echo is not late; it is structurally unsendable.  This is why every run
 * fails and why no watchdog-budget stretch can help.
 *
 * THE ASSIST.  When every condition of the lost-echo state is provably
 * present, write the ONE BYTE the echo would have written.  Conditions,
 * all read from guest RAM each frame (two loads in the common case):
 *   1. `rfu_exit_assist` key on, wireless session up, game running;
 *   2. sPlayerKeyInterceptCallback == KeyInterCB_WaitForPlayersToExit
 *      (0x080871C5 — a thumb pointer unique to this ROM, which also makes
 *      it the ROM-identity gate: no other game can present this value);
 *   3. the LOCAL player's link state is BUSY while the PEER's is
 *      EXITING_ROOM — i.e. the exit is mutually agreed and only the echo
 *      is missing;
 *   4. sRfuKeepAliveTimer >= 45 of 61 — the echo has had ~0.8 s to arrive;
 *      a merely-late echo lands long before this and the assist never fires.
 * Then sPlayerLinkStates[local] = EXITING_ROOM.  The game's own
 * WaitForPlayersToExit sees all-exiting on its next frame and runs the
 * REAL exit: EventScript_DoLinkRoomExit -> READY_CLOSE_LINK handshake with
 * a host that is still linked and waiting for exactly that -> clean
 * disconnect on both sides -> warp + trainer-card stamp.  Nothing is
 * skipped: the game executes its own negotiated exit, one lost byte later.
 *
 * The failure mode if the write is wrong is the status quo (the watchdog
 * fires at 61 as it does today).  Default PLAY_EXIT_ASSIST: on for players,
 * off in the harness; Emerald USA only by the pointer gate. */
static int g_exit_assist;              /* harness key rfu_exit_assist */
static unsigned g_exit_assist_n;       /* fired this session */

static void exit_assist_frame(void)
{
   unsigned kcb = 0;
   unsigned char ka = 0, ls[4];
   int local, peer_;

   if (!g_exit_assist || !g_net_up)
      return;
   if (fe_host_mem_read(0x03000E14, &kcb, 4) != 0 || kcb != 0x080871C5u)
      return;
   if (fe_host_mem_read(0x03000E19, &ka, 1) != 0 || ka < 45)
      return;
   if (fe_host_mem_read(0x03000E10, ls, 4) != 0)
      return;
   /* Two-player Trade Center: parent is link player 0, child is 1. */
   local = g_net_is_host ? 0 : 1;
   peer_ = 1 - local;
   if (ls[local] != 0x81 /* BUSY */ || ls[peer_] != 0x83 /* EXITING */)
      return;
   ls[local] = 0x83;
   if (fe_host_mem_write(0x03000E10 + (unsigned)local, &ls[local], 1) == 0)
   {
      g_exit_assist_n++;
      fe_evt("rfu_exit_assist local=%d ka=%u n=%u net=%s",
             local, ka, g_exit_assist_n, g_net_up ? "up" : "down");
   }
}

/* ----- emulated-adapter command/state trace (phase5j) --------------------
 * rfu_link_down only ever reported the four paths that end a *connection*.
 * The event that actually produces gen-3's UNRECOVERABLE link screen is the
 * adapter answering a command with an ERROR: librfu turns any non-zero REQ
 * result into LMAN_MSG_REQ_API_ERROR, pokeemerald maps that to
 * RFU_STATUS_FATAL_ERROR, and CB2_LinkError then sets gWirelessCommType=3 —
 * the variant of the error screen whose only exit is the power switch.  The
 * recoverable "press A" variant needs RFU_STATUS_CONNECTION_ERROR instead.
 * So the whole point of this trace is to make every ERROR answer, and every
 * adapter state transition that could cause one, visible in a field log.
 *
 * Called from the emulation thread; a lock-free SPSC ring keeps it to a
 * store and an index bump, and the main loop does the log write.  Entries
 * are dropped rather than overwritten if the ring fills (the count says so),
 * because the FIRST error is the interesting one. */
#ifndef GPSP_NO_TELEMETRY
#define RFU_TRACE_RING 64
static volatile unsigned g_rfu_tr_buf[RFU_TRACE_RING];
static volatile unsigned g_rfu_tr_head;   /* producer: emulation thread   */
static volatile unsigned g_rfu_tr_tail;   /* consumer: main loop          */
static volatile unsigned g_rfu_tr_lost;
#endif

/* Fired by the core at vcount==160 -- see g_vis_n. Defined here, below
 * frame_t0 and net_now_us. */
void gpsp_visible_done_hook(void)
{
#ifdef REWIND
   /* A frame the rewind is re-running is not for the screen.  If it has a
    * slot its capture is posted here, at the end of its visible lines
    * whatever the post mode, and drawn into the slot (me_rend_fill_desc);
    * if not, it is not posted at all, like a suppressed fast-forward
    * frame's. */
   if (fe_host_rewind_replaying())
   {
      if (g_rw_slot && g_me_rend)
      {
         unsigned t0 = (unsigned)sceKernelGetSystemTimeLow();
         g_mer_in_vis = 1;
         me_rend_frame(1);
         g_mer_in_vis = 0;
         g_rwm.posts++;
         g_rwm.post_us += (unsigned)sceKernelGetSystemTimeLow() - t0;
      }
      return;
   }
#endif
   /* SAME-FRAME: this is the earliest instant the ME can be given work. The
    * capture is complete and the CPU still has all of VBlank to emulate, so
    * the render overlaps real work instead of trailing it. */
   if (mer_sameframe_active() && g_me_rend && !g_mer_posted)
   {
      g_mer_posted = 1;
      g_mer_in_vis = 1;
      me_rend_frame(1);
      g_mer_in_vis = 0;
   }
   /* VISIBLE-END POST: same producer, same function, same once-per-frame
    * guard -- only the moment moves, so the graphics memory the ME snapshots
    * is the memory this frame was drawn with.  See g_mer_vispost. */
   else if (mer_vispost_active() && g_me_rend && !g_mer_posted &&
            !g_blit_suppress)
   {
      g_mer_posted = 1;
      g_mer_in_vis = 1;
      me_rend_frame(1);
      g_mer_in_vis = 0;
   }
   if (!g_frame_start_us)
      return;
   {
      unsigned d = (unsigned)(net_now_us() - g_frame_start_us);
      g_vis_n++;
      g_vis_us += d;
      if (d > g_vis_max)
         g_vis_max = d;
   }
}

/* RELEASE-SAFE PEEK AT WHAT THE GAME IS ASKING THE ADAPTER FOR.
 *
 * The ring below is drained into fe_evt, which compiles out of a player build --
 * so when Mystery Gift fails in the field there is nothing to go on.  These four
 * values are the minimum needed to tell the three failure modes apart, and the
 * Mystery Gift screen prints them:
 *
 *   cmd   the last adapter command the GAME issued (rfu.c's RFU_CMD_*)
 *   st    rfu.c's state: 0 idle, 1 host, 2 connecting, 3 client
 *   S     1 once the game has issued BCRD_START (0x1c) -- it is scanning
 *   C     1 once the game has issued CONNECT (0x1f)   -- it found us
 *
 * S=0 means the game is not searching over RFU at all.  S=1 C=0 means it is
 * searching and rejecting our beacon.  C=1 means discovery worked. */
static volatile unsigned g_rfu_last_cmd = 0xFFF, g_rfu_last_state;
static volatile unsigned g_rfu_seen_bcrd, g_rfu_seen_conn;

void gpsp_rfu_trace_hook(unsigned ev, unsigned a, unsigned b)
{
#ifndef GPSP_NO_TELEMETRY
   unsigned head = g_rfu_tr_head;
#endif

   /* Flag-writes only, like the adhocctl handler: this runs on the emulation
    * thread inside the adapter emulation and must not do work. */
   if (ev == 5 || ev == 1)              /* RFU_TR_CMD / RFU_TR_CMDERR */
   {
      g_rfu_last_cmd   = a;
      g_rfu_last_state = b;
      if (a == 0x1c) g_rfu_seen_bcrd = 1;   /* RFU_CMD_BCRD_START */
      if (a == 0x1f) g_rfu_seen_conn = 1;   /* RFU_CMD_CONNECT    */
   }
   else if (ev == 2)                    /* RFU_TR_STATE */
      g_rfu_last_state = a;

#ifdef GPSP_NO_TELEMETRY
   /* The ring's only reader is rfu_trace_drain's fe_evt lines: a player
    * build keeps the peek above (the Mystery Gift screen prints it) and
    * not the ring. */
   (void)b;
#else
   if (head - g_rfu_tr_tail >= RFU_TRACE_RING)
   {
      g_rfu_tr_lost++;
      return;
   }
   g_rfu_tr_buf[head % RFU_TRACE_RING] =
      ((ev & 0xFF) << 24) | ((a & 0xFFF) << 12) | (b & 0xFFF);
   g_rfu_tr_head = head + 1;
#endif
}

/* Reported, never branched on: gone with telemetry. */
static const char *rfu_tr_state_name(unsigned s) __attribute__((unused));
static const char *rfu_tr_state_name(unsigned s)
{
   static const char *n[] = { "idle", "host", "connecting", "client" };
   return s < 4 ? n[s] : "?";
}

#ifdef GPSP_NO_TELEMETRY
/* rfu_trace_drain(): this arm's definition is in fe/fe_priv.h */
#else
static void rfu_trace_drain(void)
{
   static unsigned reported_lost;

   while (g_rfu_tr_tail != g_rfu_tr_head)
   {
      unsigned v  = g_rfu_tr_buf[g_rfu_tr_tail % RFU_TRACE_RING];
      unsigned ev = (v >> 24) & 0xFF, a = (v >> 12) & 0xFFF, b = v & 0xFFF;
   FE_EVT_ONLY(a); FE_EVT_ONLY(b);
      g_rfu_tr_tail++;

      switch (ev)
      {
      case 1: /* CMDERR — the one that becomes the fatal screen */
         fe_evt("rfu_cmderr cmd=0x%02x state=%s net=%s",
                a, rfu_tr_state_name(b), g_net_up ? "up" : "down");
         break;
      case 2: /* state transition */
         fe_evt("rfu_state new=%s cause=%u net=%s",
                rfu_tr_state_name(a), b, g_net_up ? "up" : "down");
         break;
      case 3:
         fe_evt("rfu_unkcmd cmd=0x%02x state=%s", a, rfu_tr_state_name(b));
         break;
      case 4:
         fe_evt("rfu_qdrop side=%s slot=%u", a ? "host_rx" : "client_rx", b);
         break;
      case 5:
         fe_evt("rfu_cmd cmd=0x%02x state=%s", a, rfu_tr_state_name(b));
         break;
      case 6: /* adapter login (AgbRFU_SoftReset + AgbRFU_checkID) */
      {
         static const char *ph[] = { "reset", "handshake", "logged_in" };
   FE_EVT_ONLY(ph);
         fe_evt("rfu_login phase=%s prev_words=%u",
                a < 3 ? ph[a] : "?", b);
         break;
      }
      case 7:
         /* New high-water of RECV_DATA commands served in ONE emulated
          * frame.  The game drains its 32-slot recvQueue once per frame and
          * `full` is a latch that turns into the game's UNRECOVERABLE error
          * screen, so this is the number to watch: steady state is 1, and
          * anything climbing toward 32 is the mechanism. */
         fe_evt("rfu_rxburst frame_max=%u prev=%u net=%s",
                a, b, g_net_up ? "up" : "down");
         break;
      case 8:
         /* The per-frame delivery cap held a packet back for one frame
          * (ADR-0042).  Seeing these is the cap working; seeing none while
          * rfu_rx_cap is set means it never bound and the run tells us
          * nothing about the hypothesis. */
         fe_evt("rfu_rxhold frame_max=%u delivered=%u", a, b);
         break;
      case 9:
         /* New deepest occupancy of an RFU receive queue (ADR-0044).  If this
          * approaches RFU_PKT_QUEUE the next burst will be dropped, and a
          * dropped RFU packet is payload the games cannot recover.  Read it
          * next to rfu_qdrop: qdrop = 0 with a high mark means the depth is
          * merely adequate, not generous. */
         fe_evt("rfu_qhi side=%s depth=%u", a ? "host_rx" : "client_rx", b);
         break;
      case 10:
         /* ADR-0055: a host packet waited this long in our queue before the
          * game consumed it -- the CLIENT'S ANSWER LATENCY, reported as a new
          * high-water with the running mean beside it.
          *
          * This is the quantity the rfu_rx_cap experiment identified as the
          * one that matters.  Capping deliveries deliberately added a couple
          * of frames of answer latency and the transport collapsed: txq_hi
          * 465, spill 81, the project's first net_error.  The protocol
          * punishes a late reply far harder than it punishes volume, so a
          * multi-frame value here is the thing to chase -- not a busy frame. */
         fe_evt("rfu_answer max_us=%u mean_us=%u", a, b);
         break;
      case 11:
         /* ADR-0058: periodic census, emitted whether or not anything moved.
          * n=0 means the probe never measured anything and its placement is
          * wrong; n>0 with max_us=0 means answer latency is genuinely
          * sub-frame.  Case 10 above cannot tell those apart -- it only fires
          * on a new high-water, so an all-zero session is indistinguishable
          * from a dead instrument, which is how runs 6-9 read. */
         fe_evt("rfu_answer_census n=%u max_us=%u", a, b);
         break;
      case 12:
         /* ADR-0059: the CLIENT's emulated adapter told the game
          * RFU_CMD_RESP_TIMEO -- "your peer's data did not arrive in time".
          * This is the central event of CLIFF-FINDINGS' surviving mechanism
          * and it had no hook until now.  n is cumulative; t is the live
          * rfu_timeout in GBA frames, whose wall-clock worth is t/session_fps
          * seconds and therefore SHRINKS as the session rate rises. */
         fe_evt("rfu_timeo n=%u t_frames=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 13:
         /* ADR-0059: the game just configured the adapter's deadlines.  Once
          * per run.  Every timing table in CLIFF-FINDINGS is parameterised on
          * this number; this is the first time it is on the record. */
         fe_evt("rfu_syscfg timeout_frames=%u rtx_max=%u", a, b);
         break;
      case 14:
         /* ADR-0059: the HOST's rfu_resp_timeout (rtx_max/6 emulated frames)
          * expired and we handed the game a synthetic "clients did not
          * answer".  n is in units of 16.  Expected to be busy at every rate;
          * the number that matters is its rate per emulated frame. */
         fe_evt("rfu_noresp n16=%u rtx_max=%u", a, b);
         break;
      case 15:
         /* ADR-0072: HOW OFTEN, AND FOR HOW LONG, THE GAME THREW THE PLAYER'S
          * INPUT AWAY.
          *
          * pokeemerald's KeyInterCB_SelfIdle (src/overworld.c:2520) never
          * reaches KeyInterCB_ReadButtons while GetLinkRecvQueueLength() > 4,
          * and that queue drains exactly once per frame and exists only on the
          * CHILD.  So the client's controls are dead for as long as our
          * deliveries outrun the game's drain rate -- and the host cannot
          * suffer it at all.
          *
          * `n` is frames over the gate in a 600-frame (~10 s) window; `run` is
          * the longest unbroken stretch, which is what a human actually feels
          * as "it ate my input".  RFU_TR_RXBURST already showed peaks of 9 on
          * the join against 1-2 on the host, but a high-water cannot say
          * whether that was constant or a one-off.  This can. */
         /* ADR-0072 fix: peak moved to its own event (case 20) — the ring's
          * 12-bit fields masked the old (peak<<16) packing to zero here too. */
         fe_evt("rfu_rxgate n=%u/%u run_max=%u net=%s", a, 600u, b,
                g_net_up ? "up" : "down");
         break;
      case 16:
         /* ADR-0072: the recoverable "press A to return to lobby" error, the
          * first time it has ever been observable.  slots is the mask we told
          * the game had dropped; state is our adapter state at the moment we
          * said it. */
         fe_evt("rfu_discans slots=0x%x state=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 17:
         /* ADR-0072: CLUMPING, MEASURED AT THE WIRE.
          *
          * `clumped` is host packets that arrived in the SAME emulated frame
          * as the previous one, out of `n`.  The game drains its receive queue
          * once per frame, so a zero frame-delta is one unit of queue growth
          * and a steady one-per-frame stream — at any rate — cannot grow it at
          * all.  This is the number that decides whether OUR delivery pattern
          * can reach pokeemerald's threshold of 4.
          *
          * It exists because RFU_TR_RXBURST could not answer it: that counter
          * increments on every RECV_DATA command, so it measures how hard the
          * game is POLLING, and the client polls hardest when it is waiting
          * and receiving nothing.  A near-zero reading here retires the
          * clumping hypothesis outright. */
         fe_evt("rfu_arrival clumped=%u/%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 18:
         /* ADR-0074: `queued` is how many host packets were still undelivered
          * when the peer's disconnect arrived -- i.e. how much of the Union
          * Room exit negotiation the immediate teardown destroyed.  mode 0 =
          * wiped now (historical), 1 = deferral armed, 2 = deferral applied
          * after the drain.  ADR-0076 adds: 3 = grace window armed, 4 = grace
          * EXPIRED and the historical teardown ran, 5 = the game issued its
          * own disconnect inside the window -- THE RACE WAS WON and the exit
          * was clean.  A healthy grace arm shows 3 followed by 5. */
         fe_evt("rfu_discq queued=%u mode=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 19:
         /* ADR-0075: the pace census.  `n` = frames in a 600-frame window on
          * which frame-paced delivery deferred an eligible packet; `q_hi` =
          * the deepest OUR client queue got on those frames.  Emitted every
          * window while rfu_frame_pace is on, including n=0, so a gate that
          * never bound cannot be mistaken for a dead instrument.  Read next
          * to rfu_arrival: n should track the clumped fraction, and q_hi
          * bounds the extra latency (q_hi extra frames on the last packet of
          * the worst clump). */
         fe_evt("rfu_pace n=%u/%u q_hi=%u net=%s", a, 600u, b,
                g_net_up ? "up" : "down");
         break;
      case 20:
         /* ADR-0072 fix: the GAME's recvQueue-depth PEAK on its own event so
          * the 12-bit ring keeps it (peak>4 = Gate B input-eat, 32 = FATAL).
          * over_gate = frames past the gate this window. */
         fe_evt("rfu_gqpeak peak=%u over_gate=%u/%u net=%s", a, b, 600u,
                g_net_up ? "up" : "down");
         break;
      case 23:
         fe_evt("rfu_pacecatch n=%u q=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 22:
         /* The bound on the pace hold (rfu.c rfu_pace_max_hold).  `n` =
          * packets discarded this window because the client's undelivered
          * queue was older than the bound; `hi` = the worst single frame.
          *
          * Read WITH rfu_pace: q_hi parked at the cap plus n>0 means the bound
          * is doing its job -- the alternative was the hold crossing the
          * game's 32-frame link timeout and the adapter being reset.  n
          * climbing every window means the host is outrunning this game's ask
          * rate persistently, which is a different problem from a clump. */
         fe_evt("rfu_pacedrop n=%u hi=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 21:
         /* SEND_DATA word count.  `hi` is the largest rfu_plen the guest put
          * on the wire this window; `clamp` how many copies exceeded the
          * 23-word tx buffer and were bounded.  hi<=24 means this ROM never
          * reached the pre-fix overrun; hi>24 with clamp>0 means it did. */
         fe_evt("rfu_txplen hi=%u clamp=%u net=%s",
                a, b, g_net_up ? "up" : "down");
         break;
      case 24:
         /* The client's STANDING host->client backlog (frames): `hi` the
          * worst in this ~10 s window, `now` the current one.  This is the
          * latency that did not drain -- `now` climbing window after window
          * is the growing lag, and what the host's link governor acts on. */
         /* now=0xFFF is rfu.c's "no complete sample yet" (a window that
          * began outside client state): print it as none, never as a number
          * a summarizer could average. */
         if (b == 0xFFFu)
            fe_evt("rfu_backlog hi=%u now=none net=%s", a,
                   g_net_up ? "up" : "down");
         else
            fe_evt("rfu_backlog hi=%u now=%u net=%s", a, b,
                   g_net_up ? "up" : "down");
         break;
      case 25:
         /* Per-window answer latency: how long host packets waited in our
          * queue before the game took them.  ms. */
         fe_evt("rfu_answin mean_ms=%u max_ms=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 26:
         /* Link-token census: packets this adapter transmitted as NEW data vs
          * as an RTX_WAIT RESEND of the previous buffer. */
         fe_evt("rfu_txmix new=%u rtx=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 27:
         fe_evt("rfu_rxmix data=%u none=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 28:
         /* HOST: standing client->host backlog, same definition as
          * rfu_backlog.  The reverse half of the round trip. */
         if (b == 0xFFFu)
            fe_evt("rfu_hbacklog hi=%u now=none net=%s", a,
                   g_net_up ? "up" : "down");
         else
            fe_evt("rfu_hbacklog hi=%u now=%u net=%s", a, b,
                   g_net_up ? "up" : "down");
         break;
      case 29:
         /* Received link packets with no game content (every librfu
          * sub-frame UNI, every payload byte zero) out of all received. */
         fe_evt("rfu_empty n=%u/%u net=%s", a, b, g_net_up ? "up" : "down");
         break;
      case 31:
         fe_evt("rfu_fault corrupted=%u off=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 33:
         /* Hold, never discard: the most packets waiting in the overflow ring
          * at once this window, and how many entered it. */
         fe_evt("rfu_hold hi=%u n=%u net=%s", a, b, g_net_up ? "up" : "down");
         break;
      case 34:
         /* The hold could not take a packet, so it was DISCARDED after all;
          * stale = entries purged because their link had gone. */
         fe_evt("rfu_holdfail n=%u stale=%u net=%s", a, b, g_net_up ? "up" : "down");
         break;
      case 32:
         /* The original bytes of the packet about to be corrupted. */
         if (b == 0xFFFu)
            fe_evt("rfu_fault_bytes off=%u b=%02x", a >> 8, a & 0xFF);
         else
            fe_evt("rfu_fault_bytes off=%u b=%02x%02x", a >> 8, a & 0xFF, b);
         break;
      case 35:
         fe_evt("rfu_pollmix none=%u multi=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 36:
         fe_evt("rfu_catchgq n=%u arr=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 37:
         /* The adapter's one-buffer rule: packets a real adapter would have
          * overwritten before the game looked, removed this window. */
         fe_evt("rfu_supersede n=%u on=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 39:
         /* Longest run of frames a receive queue stood above the supersede
          * allowance this window (client queue, host queues). */
         fe_evt("rfu_qrun client=%u host=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      case 30:
         /* Link shedding: content-free packets removed from the queue this
          * adapter reads, this window; keep = the rfu_shed_keep in force
          * (0 = the A/B's off arm). */
         fe_evt("rfu_shed n=%u keep=%u net=%s", a, b,
                g_net_up ? "up" : "down");
         break;
      default:
         break;
      }
   }
   if (g_rfu_tr_lost != reported_lost)
   {
      reported_lost = g_rfu_tr_lost;
      fe_evt("rfu_trace_lost n=%u", reported_lost);
   }
}
#endif

enum { SIL_OFF = 0, SIL_AUTO, SIL_HOST, SIL_JOIN };
enum { SILST_IDLE = 0, SILST_JOINING, SILST_UP, SILST_FAILED };

static int g_silent_mode;   /* SIL_* */
static int g_silent_state;  /* SILST_* */
static unsigned g_silent_ticks, g_silent_deadline;
static char g_silent_group[16], g_silent_nick[24];
#ifdef GBADHOC_MGIFT
static int g_mg_on; /* listener up; also gates silent RFU bring-up */
#else
#define g_mg_on 0   /* MGIFT=0: never listening */
#endif

static void silent_frame(void)
{
   if (!g_silent_mode || g_silent_state == SILST_FAILED ||
       g_silent_state == SILST_UP)
      return;
   if (!g_rfu_activated)
      return;

   /* Mystery Gift owns infrastructure Wi-Fi directly. This automatic path
    * bypasses the menu interlock, so consume the attempt while MG is active
    * instead of bringing up ad-hoc underneath its AP association. */
#ifdef GBADHOC_MGIFT
   if (g_mg_on)
   {
      g_silent_state = SILST_FAILED;
      fe_evt("silent_fail reason=mystery_gift_active");
      return;
   }
#endif /* GBADHOC_MGIFT */

   if (g_silent_state == SILST_IDLE)
   {
      int as_host = (g_silent_mode == SIL_HOST);
      int rc;
      if (g_net_up)                     /* user already connected via UI */
      {
         g_silent_state = SILST_UP;
         return;
      }
      fe_evt("silent_activate role=%s group=%s",
             as_host ? "host" : "join", g_silent_group);
      /* The same heap guarantee as the UI- and boot-driven bring-up, and
       * one power hold to the renderer's return (net_bringup_rend_off). */
      rc = net_bringup_rend_off(as_host, g_silent_group, g_silent_nick);
      if (rc == ADHOC_ERR_WLAN_OFF)
      {
         osd_chip_session("WLAN switch OFF");
         osd_toast("Turn the WLAN switch ON for wireless");
         fe_evt("silent_fail reason=wlan_off");
         g_silent_state = SILST_FAILED;
         return;
      }
      if (rc != 0)
      {
         osd_toast("Wireless start failed");
         fe_evt("silent_fail reason=bringup stage=%s",
                adhoc_transport_stage());
         g_silent_state = SILST_FAILED;
         return;
      }
      if (as_host)
      {
         char lbl[ROOM_LABEL_CAP];
         fe_evt("silent_host promoted=0");
         osd_toast("Wireless ready (hosting %s)",
                   room_label(g_silent_group, lbl, sizeof(lbl)));
         g_silent_state = SILST_UP;
         return;
      }
      /* join-first: wait a jittered window for a WELCOME */
      g_silent_ticks = 0;
      g_silent_deadline = 240 +
         (unsigned)((sceKernelGetSystemTimeWide() >> 5) % 120);
      g_silent_state = SILST_JOINING;
      return;
   }

   /* SILST_JOINING */
   if (fe_np_stop_reason() == ND_STOP_VERSION)
   {
      fe_evt("silent_fail reason=version");
      g_silent_state = SILST_FAILED;
      return;
   }
   g_silent_ticks++;
   if (fe_np_session_active())
   {
      char lbl[ROOM_LABEL_CAP];
      fe_evt("silent_joined group=%s", g_silent_group);
      osd_toast("Wireless linked (%s)",
                room_label(g_silent_group, lbl, sizeof(lbl)));
      g_silent_state = SILST_UP;
      return;
   }
   if (g_silent_mode == SIL_AUTO && g_silent_ticks > g_silent_deadline)
   {
      /* Nobody hosting: promote.  Transport stays up; only the netdrv
       * role restarts (core stop()/start() is part of the netpacket
       * contract). */
      fe_np_stop();
      if (net_start_np(1, g_silent_group, g_silent_nick, 0) == 0)
      {
         char lbl[ROOM_LABEL_CAP];
         fe_evt("silent_host promoted=1 waited=%u", g_silent_ticks);
         osd_toast("Wireless ready (hosting %s)",
                   room_label(g_silent_group, lbl, sizeof(lbl)));
         g_silent_state = SILST_UP;
      }
      else
      {
         net_teardown();
         fe_evt("silent_fail reason=promote");
         g_silent_state = SILST_FAILED;
      }
   }
}

/* ---- GB/GBC link sessions (frontend-common/fe_gblink.h) ------------------
 *
 * A GB or GBC game's Wireless -> Host / Join brings the ad-hoc session up
 * exactly as for GBA, then starts a link session: each PSP runs both
 * players' Game Boys with their link ports cabled in memory, and only the
 * buttons cross the radio.  While the session sets itself up (and after it
 * has ended) the screen shows its status; the game restarts from its save
 * when the session ends, with the trade (or whatever the players did)
 * saved -- or, if the two PSPs ever disagreed, with nothing written.
 *
 * Harness keys (GPSP_PERF_RIG): gblink_role = host|join starts one
 * automatically (group = the room); gblink_delay = input delay in frames
 * (default 4); gblink_rtc_seed = the host's clock for both machines (fixes a
 * run's clock); gblink_end_after = frames after the autopilot script ends
 * before the host asks to end (default 300).  The run exits when the
 * session is DONE (exit 0) or FAILED (exit 4). */
#define GBLINK_DELAY_DEFAULT 4
static unsigned g_gblink_delay = GBLINK_DELAY_DEFAULT;
/* Harness: the auto input delay's cap in frames (fe_gblink_local.delay_auto,
 * docs/ONLINE-TUNNEL.md section 2); 0 = off, every build's behaviour so far.
 * Host only: the guest takes the delay from CONFIG. */
static unsigned g_gblink_delay_auto;
static long long g_gblink_seed;       /* harness: 0 = this console's clock */
static unsigned g_gblink_end_after = 300;
static unsigned g_gblink_ended_ticks; /* frames since DONE/FAILED */
static int g_gblink_screen;           /* draw the status screen this frame */
#ifdef GPSP_PERF_RIG
static unsigned g_gedump_loops, g_gedump_left = 64;  /* harness gedump_loops */
#endif
static int g_gblink_census;           /* this session's running census taken */
static time_t g_gblink_clock;         /* harness: the frozen cartridge clock */
static time_t gblink_seed_wallclock(void)
{
   return g_gblink_clock;
}
static int g_gblink_exiting;          /* harness: exiting, run no more frames */
static int g_gblink_library = 1;      /* harness 0: never find the partner's
                                       * cartridge locally (force a transfer) */

/* The partner's cartridge in this console's library: roms/ and its first
 * level of folders (the browser's reach), by exact size and header. */
static int gblink_find_in(const char *dir, uint32_t size,
                          const uint8_t header[0x1C], char *path, size_t cap,
                          int depth)
{
   SceUID d = sceIoDopen(dir);
   SceIoDirent ent;
   int found = 0;
   if (d < 0)
      return 0;
   memset(&ent, 0, sizeof(ent));
   while (!found && sceIoDread(d, &ent) > 0)
   {
      char p[PSP_FILE_PATH_CAP];
      const char *dot = strrchr(ent.d_name, '.');
      if (ent.d_name[0] == '.')
         continue;
      if (snprintf(p, sizeof(p), "%s/%s", dir, ent.d_name) >= (int)sizeof(p))
         continue;
      if (FIO_S_ISDIR(ent.d_stat.st_mode))
      {
         if (depth == 0)
            found = gblink_find_in(p, size, header, path, cap, 1);
         continue;
      }
      if (!dot || (strcasecmp(dot, ".gb") && strcasecmp(dot, ".gbc")) ||
          ent.d_stat.st_size != (SceOff)size)
         continue;
      {
         SceUID f = sceIoOpen(p, PSP_O_RDONLY, 0);
         uint8_t h[0x1C];
         if (f < 0)
            continue;
         if (sceIoLseek(f, 0x134, PSP_SEEK_SET) == 0x134 &&
             sceIoRead(f, h, sizeof(h)) == (int)sizeof(h) &&
             !memcmp(h, header, sizeof(h)))
         {
            snprintf(path, cap, "%s", p);
            found = 1;
         }
         sceIoClose(f);
      }
   }
   sceIoDclose(d);
   return found;
}

/* The link session's bulk lane (fe_gblink.h, transport_adhoc.h). */
#define GBLINK_BULK_SLOTS 32
static unsigned g_gblink_bulk_bytes = 1037;   /* 1024 data + 13 header */
static unsigned g_gblink_bulk_rate;           /* 0 = adaptive */
/* Scanline batching ON by default (154 = up to a whole frame while the cable
 * is quiet): proven byte-exact off-hardware (GB-LINK.md section 8), hw3 arm K
 * -1.7..-2.6 ms of core time in busy windows, and the owner found hw4's D
 * noticeably smoother.  hw5 runs the player defaults (this, copies 0, pace
 * slot 0 off); harness `gblink_batch = 1` is the strict control. */
#define GBLINK_BATCH_DEFAULT 154u
static unsigned g_gblink_batch = GBLINK_BATCH_DEFAULT; /* gbdual batch_lines */
/* ...also while the partner's port is armed (gbdual_config_t.batch_armed,
 * docs/GB-CORE-PERF.md): ON by default since 4.5.0 -- with the lazy timer
 * (gbp-b2) real two-PSP Gen 2 trades went 49.8 -> 58.5 fps, no desyncs.
 * Exact; the host decides for both (negotiated, an older build on either
 * side = off).  Harness key gblink_batch_armed = 0 is the control arm. */
#ifndef GBLINK_ARMED_DEFAULT
#define GBLINK_ARMED_DEFAULT 1
#endif
static int g_gblink_batch_armed = GBLINK_ARMED_DEFAULT;
static int g_gblink_live = 1;                 /* link from the running game */
/* Redundant input frames, OFF: hw2's paired A/B (V vs U, same states and
 * scripts) measured the extra datagram at +0.7-1.4 ms of emulation-thread
 * time per frame on the PSP-1000 and +1.4-1.8 ms on the 3000, and the join's
 * stalls DOUBLED with it (34.3 vs 16.0 per 1000 frames) -- the radio lost
 * only 1-2 %, so the copies rescued almost nothing.  Harness key kept. */
static unsigned g_gblink_copies = 0;
static int g_gblink_resume_boot;              /* harness gblink_resume = 2 */
static int g_gblink_pace0;                    /* harness gblink_pace_slot0 */
/* harness gblink_start_frames: the game RUNS this many frames before the
 * session starts (no hold) -- the connecting screen then has a game to dim,
 * as in a player's session.  0 = hold from boot (the rig's default). */
static unsigned g_gblink_start_frames;
/* The beta.4 start fix's three parts as harness A/B arms (PERF_RIG only;
 * 0 = beta.4's behaviour), and a slow card for PPSSPP: link_start_keepalive
 * (the reload keeps the link alive), link_start_grace (a joiner waits 30 s
 * for a starting host), link_start_heard (the version verdict needs a host
 * that is there), gblink_reload_slow_ms (ms per MiB the reload reads), and
 * gblink_host_wait_peer (a role host starts its session when the joiner is
 * admitted, as a menu host does, instead of at once). */
#ifdef GPSP_PERF_RIG
static int g_start_keepalive = 1, g_start_grace = 1, g_start_heard = 1;
static unsigned g_reload_slow_ms;
static int g_gblink_host_wait_peer;
#else
#define g_start_keepalive 1
#define g_start_grace 1
#define g_start_heard 1
#define g_reload_slow_ms 0u
#define g_gblink_host_wait_peer 0
#endif
static int g_gblink_keep_stamp;               /* harness A/B: the old stamp */
static int g_gblink_auto_started;             /* harness: the one session */
static unsigned g_gblink_after;               /* harness: frames to play on */
static unsigned g_gblink_after_left;
static int g_gblink_after_code;
static const char *g_gblink_after_why;
/* A LINK THAT CANNOT PUT THE GAME BACK ENDS THE PROCESS (F5): set with
 * g_running = 0 by gblink_begin and link_end (section 19's reload failed),
 * read by main() as the loop ends.  They exited code 0 with no reason. */
static int g_link_fatal_code;
static const char *g_link_fatal_why;
static int psp_gb_bulk_enable(int on)
{
   /* online: no bulk lane -- the ordered channel carries transfers, and the
    * partner-cartridge transfer must not cross a server (ONLINE-TUNNEL.md 6) */
   if (online_psp_enabled())
      return on ? -1 : 0;
   if (!on)
   {
      adhoc_transport_bulk_close();
      return 0;
   }
   return adhoc_transport_bulk_open(GBLINK_BULK_SLOTS, FE_GBLINK_BULK_MAGIC);
}
static int psp_gb_bulk_send(const uint8_t mac[6], const void *buf, size_t len)
{
   return adhoc_transport_bulk_send(mac, buf, len);
}
static int psp_gb_bulk_recv(void *buf, size_t cap)
{
   return adhoc_transport_bulk_recv(buf, cap);
}

static int psp_gb_find_rom(uint32_t size, const uint8_t header[0x1C],
                           char *path, size_t cap)
{
   if (!g_gblink_library)
      return 0;
   return gblink_find_in(p_rom_dir, size, header, path, cap, 0);
}

/* Start a link session over the session net_bringup just brought up. */
/* The LINKING screen draws over the game as it stood when the link began:
 * the menu's own snapshot (wake_snapshot, the netdrv arena's tail), taken
 * before the session owns the cartridge -- the single machine and its frame
 * may be gone after that (GB/GBC). */
static void link_backdrop_take(void)
{
   wake_snapshot();
   ui_set_backdrop(g_wake_have ? g_wake_frame : NULL, (int)g_wake_w,
                   (int)g_wake_h);
}

#ifdef GBA_LINK
/* Section 19: the screen while a session start or end reloads the game's
 * core (fe_host's gba_session_mem): the LINKING screen (docs/GBA-LINK-UI.md)
 * over the backdrop it already shows, its ring turning one step per stage.
 * The renderer on the ME reads the core's memory, which is being torn down:
 * suspended for the duration.  Each stage presents at once, because the main
 * loop is not presenting while this runs.  No cancel: a half-done reload has
 * nothing to go back to but its own end. */
static unsigned g_reload_steps;
static const char *g_reload_l1;
static void gba_reload_draw(const char *what)
{
   ui_linking_view lv;
   memset(&lv, 0, sizeof(lv));
   lv.room = g_link_room[0] ? g_link_room : NULL;
   lv.l1 = g_reload_l1;
   lv.l2 = what;
   lv.frac = -1;
   lv.busy = 1;
   lv.footer = "";
   g_reload_steps++;
   ui_linking_draw(&lv);
   sceDisplayWaitVblankStart();
   vid_swap();
}

/* The link stays alive through the reload (beta.4 hotfix, online "The host
 * left"): the reload reads the whole ROM from the card and pumps nothing --
 * 2-4 s here, 18-25 s on a tester's console -- and the other PSP gives up
 * on a silent partner after 8 s (netdrv), the relay after 20 s.  Once per
 * MiB read, and at each stage: a PING to the partner (a joiner not yet
 * admitted: its JOIN), nothing else (fe_np_keepalive). */
extern void (*gpsp_rom_load_progress)(unsigned, unsigned);
static void (*g_reload_prev_progress)(unsigned, unsigned);
static void gba_reload_progress(unsigned loaded, unsigned total)
{
   (void)loaded; (void)total;
   if (g_reload_slow_ms)
      sceKernelDelayThread(g_reload_slow_ms * 1000u);
   if (g_net_up && g_start_keepalive)
      fe_np_keepalive();
}

static void gba_reload_note(int phase, const char *what)
{
   static uint64_t t0;
   if (g_net_up && g_start_keepalive)
      fe_np_keepalive();
   if (phase == 0)
   {
      t0 = net_now_us();
      g_reload_steps = 0;
      g_reload_l1 = what;
      me_rend_suspend();
      g_reload_prev_progress = gpsp_rom_load_progress;
      gpsp_rom_load_progress = gba_reload_progress;
   }
   if (phase == 2 || phase == -1)
      gpsp_rom_load_progress = g_reload_prev_progress;
   if (phase == 0)
      gba_reload_draw(NULL);
   else if (phase == 1)
      gba_reload_draw(what);
   if (phase == 2 || phase == -1)
   {
      me_rend_resume();
      cur_frame = NULL;          /* the core's frame buffer was reallocated */
      if (phase == -1 && what)
         osd_toast("%s", what);
   }
   fe_evt("gba_reload phase=%d what=%s ms=%u", phase, what ? what : "-",
          (unsigned)((net_now_us() - t0) / 1000u));
   FE_EVT_ONLY(t0);
}
#endif

#if defined(GPSP_PERF_RIG) && defined(GBA_LINK)
/* Harness sessmem_cycles = N (at frame sessmem_at, default 600): the session
 * start / end reload N times with no radio, a heap census around each, then
 * the run exits.  `heap_exact` is mallinfo's every counter plus the largest
 * block: equal before and after a cycle means the cycle gave the heap back
 * byte for byte (the GBA-LINK-MIRROR.md 19 gate). */
static unsigned g_sessmem_cycles, g_sessmem_at = 600;
static void heap_exact(const char *where, unsigned cyc)
{
   struct mallinfo mi = mallinfo();
   unsigned total = 0, top = 0;
   heap_budget(&total, &top);
   fe_evt("heap_exact at=%s cyc=%u arena=%u ordblks=%u uord=%u ford=%u "
          "keep=%u largest=%u free=%u blocks=%u", where, cyc,
          (unsigned)mi.arena, (unsigned)mi.ordblks, (unsigned)mi.uordblks,
          (unsigned)mi.fordblks, (unsigned)mi.keepcost, heap_largest(), total,
          (unsigned)gamepak_buffer_count);
}
void heap_trace_mark0(void);
static int sessmem_cycle_test(void)
{
   unsigned i;
   int rc = 0;
   heap_exact("pre", 0);
   heap_trace_report("pre");          /* everything live (mark 0) */
   heap_trace_mark();
   for (i = 1; i <= g_sessmem_cycles && rc == 0; i++)
   {
      pwr_hold("session");
      rc = fe_host_gba_sessmem_cycle(1);
      pwr_release();
      heap_exact("session", i);
      if (rc == 0)
      {
         pwr_hold("session");
         rc = fe_host_gba_sessmem_cycle(0);
         pwr_release();
      }
      heap_exact("post", i);
      if (i <= 4)
      {
         /* everything live again: diff against "pre" by size and caller */
         char tag[8];
         snprintf(tag, sizeof(tag), "post%u", i);
         heap_trace_mark0();
         heap_trace_report(tag);
      }
   }
   fe_evt("sessmem_cycles done n=%u rc=%d", i - 1, rc);
   return rc;
}
#endif
/* Frontend-detected reasons a joiner's link did not form (the session only
 * knows "nobody answered"), and the player's cancel. */
enum { LW_NONE = 0, LW_FULL, LW_VERSION, LW_GBA_ROOM, LW_CANCEL };
static int g_link_why;
static unsigned g_link_setup_ticks;   /* frames of SETUP, this session */
static unsigned g_link_prev_pad;      /* the LINKING screen's O edge   */
static int g_link_fail_hold;          /* the failure screen waits for O */

/* A session's end: the reload back to single-player (section 19) runs on
 * this thread, outside the loop's park point -- under pwr_hold. */
static int gblink_close_held(void)
{
   int rc;
   pwr_hold("session");
   rc = fe_host_gblink_close();
   pwr_release();
   return rc;
}

static int gblink_begin(int is_host)
{
   int rc;
#ifndef GPSP_NO_TELEMETRY
   heap_census("gblink_pre");
   g_gblink_census = 0;
#endif
   link_backdrop_take();
   g_link_why = LW_NONE;
   g_link_setup_ticks = 0;
   g_link_fail_hold = 0;
   g_link_prev_pad = g_pad;          /* a held O is not a cancel */
   if (g_cable_reserve)
   {
      /* the boot-time reserve becomes the session's memory */
      free(g_cable_reserve);
      g_cable_reserve = NULL;
#ifndef GPSP_NO_TELEMETRY
      heap_census("cable_reserve_freed");
#endif
   }
   {
      /* Fast link: the Wireless menu's setting, as it stands now.  It only
       * decides when this console hosts; a guest runs the host's.  Harness
       * gblink_input_batch = 2 forces it on, 0 or 1 off. */
      long fb = fe_ini_get_int(HARNESS_INI, "gblink_input_batch", -1);
      int fl = fb < 0 ? g_pcfg.fast_link : fb == 2;
      fe_host_set_fast_link(fl);
      fe_evt("fast_link_setting own=%d from=%s role=%s", fl,
             fb < 0 ? "config" : "harness", is_host ? "host" : "guest");
   }
   pwr_hold("session");               /* A's reload + B's load (section 19) */
   rc = fe_host_gblink_start(is_host, g_gblink_delay, (int64_t)g_gblink_seed);
   pwr_release();
   cur_frame = NULL;                 /* the single machine's frame is gone */
   if (rc == -3)
   {
      /* section 19: the game could not be put back -- nothing to return to */
      fe_evt("gblink_begin fatal reason=reload");
      g_link_fatal_code = 4;
      g_link_fatal_why = "gblink_reload";
      g_running = 0;
   }
   fe_evt("gblink_begin role=%s delay=%u rc=%d", is_host ? "host" : "guest",
          g_gblink_delay, rc);
   if (rc != 0)
   {
      osd_toast("Link could not start");
      net_teardown();
   }
   link_chip();
   return rc;
}

/* "x.x / y.y MB" or "x / y KB". */
static void link_amount(char *out, size_t cap, uint32_t done, uint32_t total)
{
   if (total >= 1024u * 1024u)
   {
      /* tenths of a MiB, rounded */
      unsigned d = (unsigned)(((uint64_t)done * 10u + 524288u) >> 20);
      unsigned t = (unsigned)(((uint64_t)total * 10u + 524288u) >> 20);
      snprintf(out, cap, "%u.%u / %u.%u MB", d / 10u, d % 10u, t / 10u,
               t % 10u);
   }
   else
      snprintf(out, cap, "%u / %u KB", (unsigned)((done + 512u) >> 10),
               (unsigned)((total + 512u) >> 10));
}

/* A setup that did not become a link, in two lines (docs/GBA-LINK-UI.md,
 * the wording table).  No RFU, mirror or lockstep anywhere. */
/* in .data, not .rodata, on purpose: .data has room before its pinned
 * neighbour, .rodata moves instance A's tables (docs/LAYOUT-PINNING.md) */
static char s_link_bios[] = "One PSP lacks gba_bios.bin.";

static void link_fail_text(const fe_gblink_progress *pr, char *l1, size_t c1,
                           char *l2, size_t c2)
{
   int e = pr->error == FE_GBLINK_E_PEER_ABORT && pr->peer_reason >= 0
              ? pr->peer_reason : pr->error;
   if (g_link_why == LW_FULL)
   {
      snprintf(l1, c1, "%s is full: 2 players.", g_link_room);
      snprintf(l2, c2, "Wait for a seat, or host your own room.");
   }
   else if (g_link_why == LW_GBA_ROOM)
   {
      snprintf(l1, c1, "%s is a Game Boy Advance room.", g_link_room);
      snprintf(l2, c2, "Open a Game Boy game there, or pick another room.");
   }
   else if (g_link_why == LW_VERSION || e == FE_GBLINK_E_PROTO ||
            (g_net_is_host && fe_np_refused_kind()))
   {
      snprintf(l1, c1, "%s", link_version_text());
      snprintf(l2, c2, "Update both PSPs to the same release.");
   }
   else if (e == FE_GBLINK_E_ROM_MISSING || e == FE_GBLINK_E_ROM_BAD)
   {
      snprintf(l1, c1, "The games do not match: a different version or "
               "hack.");
      snprintf(l2, c2, "Both PSPs need the same ROM.");
   }
   else if (e == FE_GBLINK_E_BIOS)
      snprintf(l1, c1, "%s", s_link_bios);
   else if (e == FE_GBLINK_E_MEMORY || e == FE_GBLINK_E_START)
   {
      snprintf(l1, c1, "The link could not start: not enough memory.");
      snprintf(l2, c2, "Quit to the game list, open the game, try again.");
   }
   else if (g_net_is_host && e == FE_GBLINK_E_NO_PEER)
   {
      snprintf(l1, c1, "Nobody joined %s.", g_link_room);
      snprintf(l2, c2, "Host again when the other PSP is ready.");
   }
   else
   {
      snprintf(l1, c1, "The host left %s.", g_link_room);
      snprintf(l2, c2, "Have them host again, then rescan.");
   }
}

/* THE LINKING SCREEN, the same on both consoles and for every mode: what is
 * happening, the step and its amount, the loading screen's bar.  Redrawn
 * every loop while a session sets up (the busy ring turns, so a slow step
 * never looks like a hang), and while a failure waits for O. */
static void link_draw_facts(const fe_gblink_progress *ppr)
{
   fe_gblink_progress pr = *ppr;
   ui_linking_view v;
   char l1[80], l2[80], l3[80], amt[32];
   int gb = g_pcfg.console != FE_CONSOLE_GBA;
   const char *room = g_link_room[0] ? g_link_room : NULL;
   char rl[ROOM_LABEL_CAP];
   memset(&v, 0, sizeof(v));
   if (!room)
      room = room_label(g_silent_group, rl, sizeof(rl));
   l1[0] = l2[0] = l3[0] = amt[0] = '\0';
   v.room = room;
   v.frac = -1;
   v.busy = 1;
   v.l1 = l1;
   v.l2 = l2;
   snprintf(l1, sizeof(l1), "Linking %s with the other PSP", room);
   switch (pr.phase)
   {
   case FE_GBLINK_PH_CHECK:
      snprintf(l2, sizeof(l2), "Checking the game...");
      if (pr.total)
         v.frac = (int)((uint64_t)pr.done * 100u / pr.total);
      break;
   case FE_GBLINK_PH_WAIT_PEER:
      if (g_net_is_host)
      {
         snprintf(l1, sizeof(l1), "Hosting %s by Link Cable", room);
         snprintf(l2, sizeof(l2), "Waiting for the other PSP...");
      }
      else
         snprintf(l2, sizeof(l2), "Finding the room...");
      break;
   case FE_GBLINK_PH_HELLO:
      snprintf(l2, sizeof(l2), "Checking the game...");
      v.frac = 100;
      break;
   case FE_GBLINK_PH_ROM_RX:
   case FE_GBLINK_PH_ROM_TX:
      snprintf(l2, sizeof(l2), "%s %s...",
               pr.phase == FE_GBLINK_PH_ROM_RX ? "Receiving" : "Sending",
               pr.phase == FE_GBLINK_PH_ROM_RX
                  ? (pr.peer_title[0] ? pr.peer_title : "the game")
                  : (g_gb_title[0] ? g_gb_title : "your game"));
      link_amount(amt, sizeof(amt), pr.done, pr.total);
      v.amount = amt;
      v.frac = pr.total ? (int)((uint64_t)pr.done * 1000u / pr.total) : 0;
      snprintf(l3, sizeof(l3), "The game is kept in memory, not on the "
               "Memory Stick.");
      v.l3 = l3;
      break;
   case FE_GBLINK_PH_VERIFY:
      snprintf(l2, sizeof(l2), "Checking %s...",
               pr.peer_title[0] ? pr.peer_title : "the other game");
      v.frac = pr.total ? (int)((uint64_t)pr.done * 1000u / pr.total) : 0;
      break;
   case FE_GBLINK_PH_SHARE:
      snprintf(l2, sizeof(l2), "%s", gb ? "Exchanging saves..."
                                       : "Sharing where you are...");
      link_amount(amt, sizeof(amt), pr.done, pr.total);
      v.amount = amt;
      v.frac = pr.total ? (int)((uint64_t)pr.done * 1000u / pr.total) : 0;
      if (!gb)
      {
         snprintf(l3, sizeof(l3), "Both games carry on from where they "
                  "are.");
         v.l3 = l3;
      }
      break;
   case FE_GBLINK_PH_START:
   case FE_GBLINK_PH_RUNNING:
      snprintf(l2, sizeof(l2), "Starting together...");
      v.frac = 1000;
      break;
   case FE_GBLINK_PH_ENDING:
   case FE_GBLINK_PH_DONE:
      snprintf(l1, sizeof(l1), "Ending the link...");
      snprintf(l2, sizeof(l2), "%s", pr.phase == FE_GBLINK_PH_DONE
               ? "Link ended. Game saved." : "Both PSPs agree where to stop.");
      v.footer = "";
      break;
   default:
      if (g_link_why == LW_CANCEL)
      {
         snprintf(l1, sizeof(l1), "Link cancelled.");
         v.footer = "";
         break;
      }
      link_fail_text(&pr, l1, sizeof(l1), l2, sizeof(l2));
      v.fail = 1;
      v.busy = 0;
      if (g_gblink_role)
         v.footer = "";                /* a harness run continues alone */
      break;
   }
   ui_linking_draw(&v);
   g_drew = 1;
}

static void gblink_draw_status(void)
{
   fe_gblink_progress pr;
   fe_host_gblink_facts(&pr);
   link_draw_facts(&pr);
}

/* ---- GB link: smoothness (docs/GB-LINK.md section 14) ---------------------
 *
 * The same policies run a GBA link-cable session (GBA_LINK builds,
 * docs/GBA-LINK-MIRROR.md section 15): it is the same fe_gblink session with
 * fe_gbadual's machines, and gblink_service drives both.  There the session
 * audio owns the resampler step and the vblank pacing for its duration:
 * ADR-0027's pace_frame / pace_extra_vblanks stand aside (they are parked at
 * nominal in a cable session anyway -- net_bringup skips
 * pace_session_begin -- so this only rules out a second controller).
 *
 * Three things a running session does here, each with a config.ini key
 * (the player's kill switch) and a harness override:
 *
 *  gblink_absorb (1)   STALL ABSORPTION.  When the partner's buttons are
 *     late, fe_gblink calls plat_gblink_wait instead of giving the display
 *     frame up: pump the network and keep waiting while the frame can still
 *     make its vblank (the frame's own work, estimated, still fits).  The
 *     machines run on the inputs alone, so this changes when a step returns,
 *     never what it computes.  0 = a late input loses the frame (<= 3.1).
 *
 *  gblink_audio_cushion (3072 frames of 32768 Hz, 94 ms)   The session
 *     starts with this much silence in the audio ring, and the rate follow
 *     holds it there.  The connecting screen empties the ring and before
 *     this the session started at zero: the ring only grows by the
 *     59.94-vs-59.73 Hz surplus (~120 frames a second), so every late frame
 *     or stall of the first minute was a gap.  Below RING_HIGH_WATER so
 *     the follow never meets the nudge.  0 = start empty.
 *
 *  gblink_audio_follow (60 = 6.0 %)   The resampler follows the ring (a
 *     PI controller, gblink_audio_frame): a ring below the cushion plays up
 *     to this much slower, above it up to 0.5 % faster (the vblank surplus).
 *     ADR-0027's trade for GBA sessions: a slight pitch drop instead of gaps
 *     while the session runs below 59.73 fps -- the game itself runs that
 *     much slower then.  The bound only binds in a sustained CPU-bound
 *     stretch: 3 % left PPSSPP at x300 (57.9 fps) at ~380 gaps a minute; 6 %
 *     covers down to ~56 fps (tools/gblink/audio_follow_sim.py).  0 = fixed
 *     rate (and the old high-water nudge).
 *
 *  gblink_nack (1)   RESEND REQUESTS.  A display frame lost waiting for
 *     the partner's buttons asks for them on the bulk lane (fe_gblink.c
 *     BK_NACK) instead of waiting out netdrv's retransmission timeout
 *     behind a lost datagram.  0 = the ordered channel only.
 *
 *  gblink_audio_soft (1)   Underruns that remain fade out and back in
 *     (g_audio_soft) instead of clicking.  0 = silence steps (<= 3.1).
 */
static int g_gbl_absorb = 1;
static int g_gbl_soft = 1;                    /* soft underruns (fade) */
static int g_gbl_nack = 1;                    /* resend requests (BK_NACK) */
static unsigned g_gbl_hash_every;             /* harness: 0 = 240 */
static int g_gbl_hash_legacy;                 /* harness: the beta's hash */
static unsigned g_gbl_cushion = 3072;
static unsigned g_gbl_follow = 60;            /* tenths of a percent */
static unsigned g_gbl_win_ms = 10000;         /* EVT gblink_win period */
static uint32_t g_gbl_work_ema;               /* frame work estimate, us */
static int g_gbl_audio_on;                    /* the session's audio policy */
static uint32_t g_gbl_rate_ema;               /* ring level, x16 */
static int32_t g_gbl_rate_i;                  /* rate follow: integral, ppm */
static uint64_t g_gw_t0;                      /* EVT gblink_win window start */

/* fe_host_config.gblink_wait.  The deadline is this loop iteration's vblank
 * (g_vc_target + 1): stop once it has passed, or once the rest of the frame
 * (its usual work plus the present) would no longer fit before it. */
#define GBL_FRAME_US      16683u
#define GBL_PRESENT_US    1500u
#define GBL_WAIT_MAX_US   12000u
static uint64_t g_gbl_wait_prev;              /* last plat_gblink_wait call */
static uint32_t g_gbl_frame_wait_us;          /* this frame's absorbed waiting */
static int plat_gblink_wait(uint32_t waited_us)
{
   uint32_t elapsed;
   unsigned depth;
   uint64_t now = net_now_us();
   if (waited_us && g_gbl_wait_prev)
      g_gbl_frame_wait_us += (uint32_t)(now - g_gbl_wait_prev);
   g_gbl_wait_prev = now;
   if (!g_gbl_absorb || waited_us >= GBL_WAIT_MAX_US)
      return 0;
   if ((int)(sceDisplayGetVcount() - g_vc_target) >= 1)
      return 0;
   elapsed = (uint32_t)(now - g_frame_start_us);
   if (elapsed + g_gbl_work_ema + GBL_PRESENT_US >= GBL_FRAME_US)
      return 0;
   depth = adhoc_transport_rx_depth();
   if (depth > g_gbl_rxq_max)
      g_gbl_rxq_max = depth;
   fe_np_pump();
   /* Nothing new: give the CPU up briefly (the radio's threads outrank us
    * anyway; this lets the lower ones run instead of a spin). */
   if (!adhoc_transport_rx_depth() && !adhoc_transport_bulk_depth())
      sceKernelDelayThread(100);
   return 1;
}

