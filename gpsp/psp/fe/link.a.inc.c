/* ----------------------------------------------------------- wireless --- */

/* g_net_up moved up near the ME video globals (ADR-0080c session gate). */
int g_net_is_host;
static int g_group_lost_logged;
static char g_session_info[48];
static const char *g_save_path_for_backup;

/* ---- link mode: Link Cable vs Wireless Adapter (docs/GBA-LINK-UI.md) ------
 *
 * THE GAME DECIDES (frontend-common/link_games.h), Settings > Link mode
 * overrides it per game (roms/linkmode.txt), and the Wireless menu only asks
 * the HOST of a game that links both ways.  The room's adhocctl name says
 * the rest (psp/room_label.h): GPSPnn adapter (unchanged since 1.0), GCnnXXXX
 * a GBA Link Cable room (the mirror) for game code XXXX, GBnnXXXX a GB/GBC
 * one.  A cable HOST keeps playing while it waits; the session starts the
 * moment the other PSP is on the channel.  The kind of the current session: */
enum { LK_NONE = 0, LK_ADAPTER, LK_CABLE, LK_GB };
static int  g_link_kind;
static int  g_link_wait;      /* cable host: room up, nobody yet, game runs */
static int  g_link_clear;     /* ...the PSP it just turned away still on it */
static int  g_link_menu;      /* the session came from the Wireless menu    */
static char g_link_room[ROOM_LABEL_CAP];
static char g_game_code[5];   /* GBA header code (0xAC), "" on GB/GBC       */
static char g_gb_title[17];   /* GB cartridge title (0x134)                 */
static char g_game_key[100];  /* linkmode.txt key: the ROM under roms/      */
static char g_game_name[64];  /* the table's title, else the ROM's name     */
#define CABLE_PROTOCOL "GBAdhoc cable v1"   /* netdrv JOIN, exact match   */

/* The session chip and the Wireless status read g_session_info.  A cable
 * session names its mode, the room, and once linked the player number (the
 * game itself says Player 1 / Player 2); an adapter one, mode and room. */
static void link_chip(void)
{
   int st;
   if (!g_net_up || g_link_kind == LK_NONE)
      return;
   st = fe_host_gblink_active() ? fe_host_gblink_state() : -1;
   if (st == FE_GBLINK_SETUP || st == FE_GBLINK_DONE ||
       st == FE_GBLINK_FAILED)
   {
      /* the LINKING screen says it all; the chip would sit on its footer */
      osd_chip_session(NULL);
      return;
   }
   if (g_link_kind == LK_ADAPTER)
      snprintf(g_session_info, sizeof(g_session_info),
               "Wireless Adapter  %s", g_link_room);
   else if (g_link_wait)
      snprintf(g_session_info, sizeof(g_session_info),
               "Link Cable  %s  waiting", g_link_room);
   else if (fe_host_gblink_state() == FE_GBLINK_RUNNING ||
            fe_host_gblink_state() == FE_GBLINK_ENDING)
      snprintf(g_session_info, sizeof(g_session_info),
               "Link Cable  %s  P%d", g_link_room, g_net_is_host ? 1 : 2);
   else
      snprintf(g_session_info, sizeof(g_session_info), "Link Cable  %s",
               g_link_room);
   osd_chip_session(g_pcfg.osd_wireless ? g_session_info : NULL);
}

uint64_t net_now_us(void)
{
   /* Microseconds, monotonic (ADHOC-NOTES §11.13) — netdrv timers must
    * never see wall-clock steps (ADR-0010). */
   return (uint64_t)sceKernelGetSystemTimeWide();
}

static void gb_serial_yield(void)
{
   sceKernelDelayThread(1000);
}

#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY)
/* Same clock, video_prof's prototype (ADR-0034 renderer profile). */
static unsigned vprof_clock_us(void)
{
   return (unsigned)sceKernelGetSystemTimeLow();
}
#endif

/* Same clock, fe_evt's prototype (ADR-0021 log-I/O accounting).  The ADR-0067
 * playable build never installs it, because there is no log to account for. */
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY)
static unsigned long long evt_clock_us(void)
{
   return (unsigned long long)sceKernelGetSystemTimeWide();
}
#endif

static void net_error_evt(const char *what, int rc)
{
   FE_EVT_ONLY(what); FE_EVT_ONLY(rc);
   fe_evt("net_error reason=%s stage=%s rc=%d sce=0x%08x",
          rc == ADHOC_ERR_WLAN_OFF ? "wlan_off" : what,
          adhoc_transport_stage(), rc,
          (unsigned)adhoc_transport_last_sce_error());
}

static void adhoc_stats_evt(void)
{
   adhoc_stats st;
   adhoc_transport_get_stats(&st);
   fe_evt("adhoc_stats tx=%u txwb=%u txfail=%u rx=%u ringdrop=%u "
          "oversize=%u rxerr=%u ctlevt=%u ctldisc=%u ctlerr=%u "
          "faultdel=%u faultdrop=%u bulk_tx=%u bulk_txfail=%u bulk_rx=%u "
          "bulk_drop=%u",
          st.tx_pkts, st.tx_would_block, st.tx_fail, st.rx_pkts,
          st.rx_ring_drop, st.rx_oversize, st.rx_err,
          st.ctl_events, st.ctl_disconnects, st.ctl_errors,
          st.fault_delayed, st.fault_dropped, st.bulk_tx, st.bulk_txfail,
          st.bulk_rx, st.bulk_drop);
}

