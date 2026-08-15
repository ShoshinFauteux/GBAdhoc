/* config_psp.c — persisted frontend settings (see config_psp.h). */
#include <stdio.h>
#include <string.h>

#include "config_psp.h"
#include "video_psp.h"
#include "fe_util.h"
#include "fe_evt.h"

psp_config g_pcfg;

static char cfg_path[256];

/* Decimal fps -> hundredths, without pulling in strtod (and its locale) for
 * one key.  Accepts "40", "40.5", "40.00"; anything else falls back to `def`
 * rather than half-parsing, because a silently-wrong session rate is a desync
 * the field log would not explain. */
int pcfg_fps_x100(const char *ini, const char *key, int def)
{
   char buf[16];
   const char *p;
   int whole = 0, frac = 0, digits = 0, seen = 0, v;

   if (!fe_ini_get(ini, key, buf, sizeof(buf)) || !buf[0])
      return def;

   for (p = buf; *p == ' ' || *p == '\t'; p++)
      ;
   for (; *p >= '0' && *p <= '9'; p++, seen++)
      whole = whole * 10 + (*p - '0');
   if (*p == '.')
      for (p++; *p >= '0' && *p <= '9' && digits < 2; p++, digits++, seen++)
         frac = frac * 10 + (*p - '0');
   while (*p >= '0' && *p <= '9')            /* extra precision: ignore */
      p++;
   while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;
   if (!seen || *p)
      return def;                            /* junk in the value: keep `def` */

   if (digits == 1)
      frac *= 10;
   else if (digits == 0)
      frac = 0;
   if (whole > 1000)
      return def;
   v = whole * 100 + frac;
   if (v < PCFG_SESSION_FPS_MIN) v = PCFG_SESSION_FPS_MIN;
   if (v > PCFG_SESSION_FPS_MAX) v = PCFG_SESSION_FPS_MAX;
   return v;
}

void pcfg_load(const char *ini_path)
{
   snprintf(cfg_path, sizeof(cfg_path), "%s", ini_path);

   /* ADR-0071: the playable/variant builds default to an UPSCALED picture;
    * the harness stays at 1x so its frame-time numbers remain comparable with
    * every run recorded before this existed.  PCFG_SCALE_DEF / PCFG_PROFILE_DEF
    * come from the build, not from a runtime branch. */
   g_pcfg.scale       = (int)fe_ini_get_int(cfg_path, "scale", PCFG_SCALE_DEF);
   g_pcfg.profile     = (int)fe_ini_get_int(cfg_path, "profile",
                                            PCFG_PROFILE_SPEED);
   g_pcfg.me_mode     = (int)fe_ini_get_int(cfg_path, "me_mode",
                                            PCFG_ME_MODE_DEF);
   g_pcfg.btn_swap    = (int)fe_ini_get_int(cfg_path, "btn_swap", 0);
   g_pcfg.filter      = (int)fe_ini_get_int(cfg_path, "filter",
                                            VID_FILTER_NEAREST);
   g_pcfg.ff_mult_x10 = (int)fe_ini_get_int(cfg_path, "ff_mult_x10", 15);
   g_pcfg.ff_hold     = (int)fe_ini_get_int(cfg_path, "ff_hold", 1);
   g_pcfg.frameskip_sparse = (int)fe_ini_get_int(cfg_path, "frameskip_sparse", 0);
   if (g_pcfg.frameskip_sparse < 0 || g_pcfg.frameskip_sparse > 15)
      g_pcfg.frameskip_sparse = 0;
   g_pcfg.ff_smooth   = (int)fe_ini_get_int(cfg_path, "ff_smooth", 0) ? 1 : 0;
   g_pcfg.ff_audio    = (int)fe_ini_get_int(cfg_path, "ff_audio", 0) ? 1 : 0;
   g_pcfg.theme       = (int)fe_ini_get_int(cfg_path, "theme", 0) ? 1 : 0;
   g_pcfg.show_fps    = (int)fe_ini_get_int(cfg_path, "show_fps", 0) ? 1 : 0;
   g_pcfg.bench_mode  = (int)fe_ini_get_int(cfg_path, "bench_mode", 0) ? 1 : 0;
   /* ADR-0019: default OFF — a session must not cost rendered frames on a
    * console that is holding real time (which the field says both are). */
   g_pcfg.net_frameskip = (int)fe_ini_get_int(cfg_path, "net_frameskip", 0);
   {
      int thr = (int)fe_ini_get_int(cfg_path, "net_skip_threshold", 25);
      if (thr < 5)  thr = 5;
      if (thr > 90) thr = 90;
      snprintf(g_pcfg.net_skip_threshold_str,
               sizeof(g_pcfg.net_skip_threshold_str), "%d", thr);
   }
   if (g_pcfg.net_frameskip < 0 || g_pcfg.net_frameskip > 2)
      g_pcfg.net_frameskip = 0;
   /* ADR-0021: default 1 (prompt) — PdpSend is the one per-frame session
    * cost whose price is only knowable on hardware, so keep it off the emu
    * thread.  2 = deferred (drains in the vblank slack); 0 = inline. */
   g_pcfg.net_tx_thread = (int)fe_ini_get_int(cfg_path, "net_tx_thread", 1);
   if (g_pcfg.net_tx_thread < 0 || g_pcfg.net_tx_thread > 2)
      g_pcfg.net_tx_thread = 1;
   /* ADR-0024: default 1 — no memory-stick write on the emulation thread.
    * Both spellings are accepted so the A/B never silently no-ops. */
   g_pcfg.log_thread =
      (int)fe_ini_get_int(cfg_path, "log_thread",
                          fe_ini_get_int(cfg_path, "net_log_thread", 1));
   if (g_pcfg.log_thread < 0 || g_pcfg.log_thread > 1)
      g_pcfg.log_thread = 1;
   /* ADR-0025: default 1 — the .sav block writes leave the emulation thread
    * too.  0 restores ADR-0020's budgeted in-frame drain for an A/B. */
   g_pcfg.sram_thread = (int)fe_ini_get_int(cfg_path, "sram_thread", 1);
   if (g_pcfg.sram_thread < 0 || g_pcfg.sram_thread > 1)
      g_pcfg.sram_thread = 1;
   /* ADR-0033 REMAPS THIS KEY.  0 = off (unchanged), 1 = default, now
    * FIXED-RATE session clamping (was: ADR-0027/0028's adaptive matcher),
    * 2 = that adaptive matcher, kept for the A/B.  An existing config.ini
    * with `net_pace_match=1` silently adopts the new policy — intended: 1
    * still means "pace during a session", only the how changed. */
   /* ADR-0078 adds 3 = backpressure: FIXED's clamp with a floating base
    * driven by live TX-queue depth.  Host-only in effect; a join running 3
    * degrades to plain fixed. */
   g_pcfg.net_pace_match =
      (int)fe_ini_get_int(cfg_path, "net_pace_match", 1);
   if (g_pcfg.net_pace_match < 0 || g_pcfg.net_pace_match > 3)
      g_pcfg.net_pace_match = 1;
   g_pcfg.net_session_fps_x100 =
      pcfg_fps_x100(cfg_path, "net_session_fps", PCFG_SESSION_FPS_DEF);
   /* ADR-0035: default 1 = snap to a whole-vblank rate.  0 restores the raw
    * fractional target for the A/B. */
   g_pcfg.net_session_fps_snap =
      /* Default 0, not 1: the shipping rate (45.00) is not on the
       * 59.94/N ladder, and snapping would silently demote it to 29.97.
       * See PCFG_SESSION_FPS_DEF. */
      (int)fe_ini_get_int(cfg_path, "net_session_fps_snap", 0);
   if (g_pcfg.net_session_fps_snap < 0 || g_pcfg.net_session_fps_snap > 1)
      g_pcfg.net_session_fps_snap = 1;
   /* Phase 5h: default 2 — the field run has to come back with the video
    * split, and level 1 is one config edit away for the probe-cost A/B. */
   g_pcfg.core_phase = (int)fe_ini_get_int(cfg_path, "core_phase", 2);
   if (g_pcfg.core_phase < 0 || g_pcfg.core_phase > 3)
      g_pcfg.core_phase = 2;
   /* ADR-0034 (amended): default **2 = VRAM**.  The hardware A/B is in and it
    * is not close — PSP-1000, per frame: cached 2513 us, uncached 2215,
    * **VRAM 1800**.  713 us/frame saved against the old default, and most of
    * it came from `gu` (1176 -> 728), not the copy.  Modes 0 and 1 stay for
    * the A/B.  vid_set_blit_mode() falls back to 0 if the VRAM bump does not
    * fit, so a console with a different VRAM layout still boots. */
   g_pcfg.blit_mode = (int)fe_ini_get_int(cfg_path, "blit_mode",
                                          VID_BLIT_VRAM);
   if (g_pcfg.blit_mode < 0 || g_pcfg.blit_mode >= VID_BLIT_MODES)
      g_pcfg.blit_mode = VID_BLIT_CACHED;
   /* ADR-0040: OFF until hardware says otherwise.  The rig can prove the
    * deferred path draws the right pixels; it cannot tell us what the
    * overlap is worth, so the user decides with a config edit. */
   g_pcfg.gu_defer = (int)fe_ini_get_int(cfg_path, "gu_defer", 0);
   g_pcfg.rfu_rx_cap = (int)fe_ini_get_int(cfg_path, "rfu_rx_cap", 0);
   /* Session overlay chip; the WLAN-off warning is never gated by this. */
   g_pcfg.osd_wireless = (int)fe_ini_get_int(cfg_path, "osd_wireless", 1);
   g_pcfg.osd_wireless = g_pcfg.osd_wireless ? 1 : 0;
   if (!fe_ini_get(cfg_path, "group", g_pcfg.group, sizeof(g_pcfg.group)))
      strcpy(g_pcfg.group, "GPSP07");
   if (!fe_ini_get(cfg_path, "nick", g_pcfg.nick, sizeof(g_pcfg.nick)))
      strcpy(g_pcfg.nick, "PSP");
   fe_ini_get(cfg_path, "last_rom", g_pcfg.last_rom, sizeof(g_pcfg.last_rom));

   if (g_pcfg.scale < 0 || g_pcfg.scale >= VID_SCALE_MODES)
      g_pcfg.scale = VID_SCALE_1X;
   g_pcfg.filter = g_pcfg.filter ? 1 : 0;
   /* 20 (2x) is retired — remap saved configs from older builds too. */
   if (g_pcfg.ff_mult_x10 != 0 && g_pcfg.ff_mult_x10 != 15 &&
       g_pcfg.ff_mult_x10 != 30)
      g_pcfg.ff_mult_x10 = 15;

   fe_log("config loaded: scale=%s filter=%s ff_mult_x10=%d ff_hold=%d "
          "group=%s net_frameskip=%d net_skip_threshold=%s net_tx_thread=%d "
          "log_thread=%d sram_thread=%d net_pace_match=%d "
          "net_session_fps=%d.%02d snap=%d core_phase=%d blit_mode=%d "
          "gu_defer=%d rfu_rx_cap=%d",
          vid_scale_name(g_pcfg.scale),
          vid_filter_name(g_pcfg.filter), g_pcfg.ff_mult_x10, g_pcfg.ff_hold,
          g_pcfg.group, g_pcfg.net_frameskip, g_pcfg.net_skip_threshold_str,
          g_pcfg.net_tx_thread, g_pcfg.log_thread, g_pcfg.sram_thread,
          g_pcfg.net_pace_match,
          g_pcfg.net_session_fps_x100 / 100, g_pcfg.net_session_fps_x100 % 100,
          g_pcfg.net_session_fps_snap, g_pcfg.core_phase, g_pcfg.blit_mode,
          g_pcfg.gu_defer, g_pcfg.rfu_rx_cap);
}

/* ADR-0035.  The load path already clamps every enum, so a bad value in the
 * FILE is harmless — but a bad value in MEMORY at save time means something
 * wrote where it should not have, and clamping on the way out would erase the
 * only evidence.  So check before writing, say so loudly, and then clamp.
 *
 * This is the guard the field build did not have: it shipped
 * `blit_mode = 1347637319` into the user's config.ini with no log line, and
 * the corruption was only noticed because a human read the ini.  Every one of
 * these fields is a small enum, so "outside its range" is proof, not
 * suspicion.  Returns the number of fields that were wrong. */
static int pcfg_validate(const char *when)
{
   int bad = 0;

#define PCFG_CHK(field, lo, hi, fallback)                                     \
   do {                                                                       \
      if ((int)g_pcfg.field < (lo) || (int)g_pcfg.field > (hi))               \
      {                                                                       \
         fe_evt("config_corrupt when=%s field=%s value=%d range=%d..%d "       \
                "-> %d", when, #field, (int)g_pcfg.field, (lo), (hi),         \
                (fallback));                                                  \
         g_pcfg.field = (fallback);                                           \
         bad++;                                                               \
      }                                                                       \
   } while (0)

   PCFG_CHK(scale,                0, VID_SCALE_MODES - 1, PCFG_SCALE_DEF);
   PCFG_CHK(profile,              0, 1, PCFG_PROFILE_SPEED);
   PCFG_CHK(me_mode,              0, 1, PCFG_ME_MODE_DEF);
   PCFG_CHK(btn_swap,             0, 1, 0);
   PCFG_CHK(filter,               0, 1, 0);
   PCFG_CHK(ff_hold,              0, 1, 1);
   PCFG_CHK(net_frameskip,        0, 2, 0);
   PCFG_CHK(net_tx_thread,        0, 2, 1);
   PCFG_CHK(log_thread,           0, 1, 1);
   PCFG_CHK(sram_thread,          0, 1, 1);
   PCFG_CHK(net_pace_match,       0, 3, 1);   /* 3 = ADR-0078 backpressure */
   PCFG_CHK(core_phase,           0, 3, 2);
   PCFG_CHK(blit_mode,            0, VID_BLIT_MODES - 1, VID_BLIT_CACHED);
   PCFG_CHK(gu_defer,             0, 1, 0);
   PCFG_CHK(rfu_rx_cap,           0, 16, 0);   /* RFU_PKT_QUEUE deep */
   PCFG_CHK(osd_wireless,         0, 1, 1);
   PCFG_CHK(net_session_fps_x100, PCFG_SESSION_FPS_MIN, PCFG_SESSION_FPS_MAX,
            PCFG_SESSION_FPS_DEF);
   PCFG_CHK(net_session_fps_snap, 0, 1, 1);
#undef PCFG_CHK

   /* The strings are the other half of the story: the field's corruption put
    * a room code's first four bytes into an int and left "07" in `group`, so
    * a short/unterminated `group` is the same event seen from the other
    * side.  `nick` and `last_rom` are only length-checked. */
   g_pcfg.group[sizeof(g_pcfg.group) - 1]     = '\0';
   g_pcfg.nick[sizeof(g_pcfg.nick) - 1]       = '\0';
   g_pcfg.last_rom[sizeof(g_pcfg.last_rom) - 1] = '\0';
   if (g_pcfg.group[0] && strlen(g_pcfg.group) < 5)
   {
      fe_evt("config_corrupt when=%s field=group value=\"%s\" "
             "-> GPSP07 (too short for a room code)", when, g_pcfg.group);
      strcpy(g_pcfg.group, "GPSP07");
      bad++;
   }
   if (bad)
      fe_evt("config_corrupt when=%s fields=%d — MEMORY was wrong, not the "
             "file; suspect a stale object or an overrun", when, bad);
   return bad;
}

void pcfg_save(void)
{
   if (!cfg_path[0])
      return;
   pcfg_validate("save");
   fe_ini_set_int(cfg_path, "scale", g_pcfg.scale);
   fe_ini_set_int(cfg_path, "profile", g_pcfg.profile);
   fe_ini_set_int(cfg_path, "me_mode", g_pcfg.me_mode);
   fe_ini_set_int(cfg_path, "btn_swap", g_pcfg.btn_swap);
   fe_ini_set_int(cfg_path, "filter", g_pcfg.filter);
   fe_ini_set_int(cfg_path, "ff_mult_x10", g_pcfg.ff_mult_x10);
   fe_ini_set_int(cfg_path, "ff_hold", g_pcfg.ff_hold);
   fe_ini_set_int(cfg_path, "frameskip_sparse", g_pcfg.frameskip_sparse);
   fe_ini_set_int(cfg_path, "ff_smooth", g_pcfg.ff_smooth);
   fe_ini_set_int(cfg_path, "ff_audio", g_pcfg.ff_audio);
   fe_ini_set_int(cfg_path, "theme", g_pcfg.theme);
   fe_ini_set_int(cfg_path, "show_fps", g_pcfg.show_fps);
   fe_ini_set_int(cfg_path, "bench_mode", g_pcfg.bench_mode);
   fe_ini_set_int(cfg_path, "net_frameskip", g_pcfg.net_frameskip);
   fe_ini_set_int(cfg_path, "net_tx_thread", g_pcfg.net_tx_thread);
   fe_ini_set_int(cfg_path, "log_thread", g_pcfg.log_thread);
   fe_ini_set_int(cfg_path, "sram_thread", g_pcfg.sram_thread);
   fe_ini_set_int(cfg_path, "net_pace_match", g_pcfg.net_pace_match);
   {
      /* Written back as a decimal so the file keeps reading the way the user
       * typed it; pcfg_fps_x100 round-trips this exactly. */
      char fps[16];
      snprintf(fps, sizeof(fps), "%d.%02d",
               g_pcfg.net_session_fps_x100 / 100,
               g_pcfg.net_session_fps_x100 % 100);
      fe_ini_set(cfg_path, "net_session_fps", fps);
   }
   fe_ini_set_int(cfg_path, "net_session_fps_snap", g_pcfg.net_session_fps_snap);
   fe_ini_set_int(cfg_path, "core_phase", g_pcfg.core_phase);
   fe_ini_set_int(cfg_path, "blit_mode", g_pcfg.blit_mode);
   fe_ini_set_int(cfg_path, "gu_defer", g_pcfg.gu_defer);
   fe_ini_set_int(cfg_path, "rfu_rx_cap", g_pcfg.rfu_rx_cap);
   fe_ini_set_int(cfg_path, "osd_wireless", g_pcfg.osd_wireless);
   fe_ini_set(cfg_path, "group", g_pcfg.group);
   fe_ini_set(cfg_path, "nick", g_pcfg.nick);
   if (g_pcfg.last_rom[0])
      fe_ini_set(cfg_path, "last_rom", g_pcfg.last_rom);
   fe_evt("config_saved scale=%d filter=%d ff_mult_x10=%d ff_hold=%d",
          g_pcfg.scale, g_pcfg.filter, g_pcfg.ff_mult_x10, g_pcfg.ff_hold);
}
