/* config_psp.c — persisted frontend settings (see config_psp.h). */
#include <stdio.h>
#include <string.h>

#include "config_psp.h"
#include "video_psp.h"
#include "fe_util.h"
#include "fe_evt.h"

psp_config g_pcfg;

static char cfg_path[256];

/* PER-CONSOLE DISPLAY PROFILES, at the end of this file. */
static void pdisp_load(void);
static int  pdisp_write(int console);

/* VID_FILTER_*: 0 nearest, 2 sharp bilinear, and any other non-zero value is
 * bilinear -- what every value but 0 meant before sharp existed, so a file
 * written by anything older loads exactly as it did. */
static int pcfg_filter_clamp(int f)
{
   if (f == VID_FILTER_SHARP)
      return VID_FILTER_SHARP;
   return f ? VID_FILTER_BILINEAR : VID_FILTER_NEAREST;
}

int pcfg_ff_mode(void)
{
   if (g_pcfg.ff_mult_x10 != 0)
      return PCFG_FF_3X;
   return g_pcfg.ff_smooth ? PCFG_FF_SMOOTH : PCFG_FF_UNLIMITED;
}

void pcfg_ff_set_mode(int mode)
{
   if (mode < 0 || mode >= PCFG_FF_COUNT)
      mode = PCFG_FF_3X;
   g_pcfg.ff_mult_x10 = mode == PCFG_FF_3X ? 30 : 0;
   g_pcfg.ff_smooth = mode != PCFG_FF_UNLIMITED;
}

const char *pcfg_ff_name(void)
{
   static const char *const names[PCFG_FF_COUNT] = {
      "3x", "Unlimited", "Unlimited Smooth"
   };
   return names[pcfg_ff_mode()];
}

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
   {
      int digit = *p - '0';
      /* Reject values above 1000 before multiplication can overflow. */
      if (whole > 100 || (whole == 100 && digit > 0))
         return def;
      whole = whole * 10 + digit;
   }
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
   g_pcfg.filter      = (int)fe_ini_get_int(cfg_path, "filter",
                                            VID_FILTER_NEAREST);
   g_pcfg.ff_mult_x10 = (int)fe_ini_get_int(cfg_path, "ff_mult_x10", 30);
   g_pcfg.ff_hold     = (int)fe_ini_get_int(cfg_path, "ff_hold", 1);
   g_pcfg.frameskip_sparse = (int)fe_ini_get_int(cfg_path, "frameskip_sparse", 0);
   if (g_pcfg.frameskip_sparse < 0 || g_pcfg.frameskip_sparse > 15)
      g_pcfg.frameskip_sparse = 0;
   g_pcfg.ff_smooth   = (int)fe_ini_get_int(cfg_path, "ff_smooth", 0) ? 1 : 0;
   g_pcfg.ff_audio    = (int)fe_ini_get_int(cfg_path, "ff_audio", 0) ? 1 : 0;
   g_pcfg.theme       = (int)fe_ini_get_int(cfg_path, "theme", 0) ? 1 : 0;
   g_pcfg.ui_shell    = (int)fe_ini_get_int(cfg_path, "ui_shell", 0) ? 1 : 0;
   g_pcfg.me_dirty    = (int)fe_ini_get_int(cfg_path, "me_dirty", 1) ? 1 : 0;
   g_pcfg.rom_resident = (int)fe_ini_get_int(cfg_path, "rom_resident", 1) ? 1 : 0;
   g_pcfg.show_fps    = (int)fe_ini_get_int(cfg_path, "show_fps", 0) ? 1 : 0;
   g_pcfg.gb_palette  = (int)fe_ini_get_int(cfg_path, "gb_palette", 0);
   if (g_pcfg.gb_palette < 0 || g_pcfg.gb_palette > 15)
      g_pcfg.gb_palette = 0;
   g_pcfg.bench_mode  = (int)fe_ini_get_int(cfg_path, "bench_mode", 0) ? 1 : 0;
#ifdef GPSP_PLAYABLE
   /* Bench mode is a HARNESS facility: it forces uncapped + every-frame
    * render and hijacks the FF chip, which reads as "fast-forward is
    * broken" to anyone who is not benchmarking.  A release build must not
    * be able to enter it, however stale the config.ini on the card is. */
   g_pcfg.bench_mode  = 0;
#endif
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
#ifdef CORE_PHASE_FORCE
   /* Pin the probe level for a measurement build.  Editing core_phase in
    * config.ini does not stick -- pcfg_save() rewrites the file from g_pcfg
    * on exit -- so a hand edit is lost every run, which is why the OSD j%/f%
    * fields have always read 0.  Applied HERE, at the config read, not later
    * in main: subsystems branch on core_phase during init, and forcing it
    * after they have already decided leaves them inconsistent (a late
    * override built fine and then would not boot). */
   g_pcfg.core_phase = CORE_PHASE_FORCE;
#endif
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
   g_pcfg.rfu_pace_max_hold =
      (int)fe_ini_get_int(cfg_path, "rfu_pace_max_hold", 24);
   g_pcfg.net_session_fps_force =
      (int)fe_ini_get_int(cfg_path, "net_session_fps_force", 0) ? 1 : 0;
   g_pcfg.nd_rto_min_us =
      (int)fe_ini_get_int(cfg_path, "nd_rto_min_us", 50000);
   g_pcfg.standby = (int)fe_ini_get_int(cfg_path, "standby", 1);
   g_pcfg.me_boot = (int)fe_ini_get_int(cfg_path, "me_boot", 1) ? 1 : 0;
   /* Session overlay chip; the WLAN-off warning is never gated by this. */
   g_pcfg.osd_wireless = (int)fe_ini_get_int(cfg_path, "osd_wireless", 1);
   g_pcfg.osd_wireless = g_pcfg.osd_wireless ? 1 : 0;
   if (!fe_ini_get(cfg_path, "group", g_pcfg.group, sizeof(g_pcfg.group)))
      strcpy(g_pcfg.group, "GPSP07");
   if (!fe_ini_get(cfg_path, "nick", g_pcfg.nick, sizeof(g_pcfg.nick)))
      strcpy(g_pcfg.nick, "PSP");
   g_pcfg.console = (int)fe_ini_get_int(cfg_path, "console", FE_CONSOLE_GBA);
   if (g_pcfg.console < FE_CONSOLE_GBA || g_pcfg.console > FE_CONSOLE_GBC)
      g_pcfg.console = FE_CONSOLE_GBA;
   fe_ini_get(cfg_path, "last_rom", g_pcfg.last_rom, sizeof(g_pcfg.last_rom));
   /* Every bind_* key is validated here; a bad one is reported
    * (config_bind_invalid) and falls back to its DEFAULT, never to unbound.
    * The legacy `btn_swap` key is read in there too, and only there: a 3.0
    * swapped-A/B config becomes explicit bind_a/bind_b (ctl_load_ini). */
   ctl_load_ini(&g_pcfg.controls, cfg_path);

   if (g_pcfg.scale < 0 || g_pcfg.scale >= VID_SCALE_MODES)
      g_pcfg.scale = VID_SCALE_1X;
   g_pcfg.filter = pcfg_filter_clamp(g_pcfg.filter);
   g_pcfg.loading_art =
      (int)fe_ini_get_int(cfg_path, "loading_art", 1) ? 1 : 0;
   g_pcfg.gbc_color_correction =
      (int)fe_ini_get_int(cfg_path, "gbc_color_correction", 1) ? 1 : 0;
   /* Last: the legacy scale/filter/gb_palette above are the migration
    * defaults for every console's profile, so they must be final first. */
   pdisp_load();
   /* All former capped profiles migrate to the former 3x Smooth policy.
    * Max and Max Smooth retain their respective unlimited selections. */
   pcfg_ff_set_mode(pcfg_ff_mode());

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

   (void)when; /* FE_EVT_ONLY may compile away all uses in host builds. */
   FE_EVT_ONLY(when);   /* names the caller in the report only */

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
   PCFG_CHK(filter,               0, VID_FILTER_MODES - 1, 0);
   PCFG_CHK(ambient,              0, PCFG_AMB_MODES - 1, PCFG_AMBIENT_DEF);
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
   PCFG_CHK(rfu_pace_max_hold,    0, 28, 24);  /* 32 = the game's timeout */
   PCFG_CHK(net_session_fps_force, 0, 1, 0);
   PCFG_CHK(nd_rto_min_us,        0, 2000000, 50000);
   PCFG_CHK(standby,              0, 3, 1);   /* ON: see me_standby_down */
   PCFG_CHK(me_boot,              0, 1, 1);
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
   /* Bindings: same evidence rule.  The load path only ever stores legal,
    * duplicate-free tables, so a repair here means memory went wrong. */
   {
      int fixed = ctl_repair(&g_pcfg.controls, NULL, NULL);
      if (fixed)
      {
         FE_EVT_ONLY(fixed);
         fe_evt("config_corrupt when=%s field=controls repaired=%d", when,
                fixed);
         bad++;
      }
   }
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

int pcfg_remember_rom(const char *name)
{
   if (!cfg_path[0] || !name || !name[0] ||
       strlen(name) >= sizeof(g_pcfg.last_rom))
      return -1;
   if (strcmp(g_pcfg.last_rom, name) == 0)
      return 0;
   /* Settings are already saved when their menu closes. Rewriting every
    * setting here did 36 complete INI read/truncate/write/close cycles
    * before the ROM loader could even start. */
   if (fe_ini_set(cfg_path, "last_rom", name) != 0)
      return -1;
   snprintf(g_pcfg.last_rom, sizeof(g_pcfg.last_rom), "%s", name);
   return 0;
}

int pcfg_remember_console(int console)
{
   if (console < FE_CONSOLE_GBA || console > FE_CONSOLE_GBC)
      return -1;
   g_pcfg.console = console;
   /* The browser's console switch is the settings context: the next
    * Settings screen edits THIS console's display profile. */
   pcfg_display_select(console);
   /* One key, for the reason pcfg_remember_rom gives: pcfg_save() is ~37
    * whole-file INI rewrites, which every TRIANGLE press used to pay. */
   if (!cfg_path[0] || fe_ini_set_int(cfg_path, "console", console) != 0)
      return -1;
   return 0;
}

void pcfg_save(void)
{
   if (!cfg_path[0])
      return;
   pcfg_validate("save");
   pcfg_display_commit();
   /* Legacy keys: what a pre-profile build reads -- GBA's look for it. */
   fe_ini_set_int(cfg_path, "scale", g_pdisp[FE_CONSOLE_GBA].scale);
   fe_ini_set_int(cfg_path, "profile", g_pcfg.profile);
   fe_ini_set_int(cfg_path, "me_mode", g_pcfg.me_mode);
   /* A write-only mirror of the A/B pair for 3.0 (downgrade) -- this build
    * never reads it while bind_a/bind_b are in the file, and a 1 here always
    * comes with both (ctl_legacy_swap).  0 for a player who never remapped,
    * so their file stays byte-identical to 3.0's. */
   fe_ini_set_int(cfg_path, "btn_swap", ctl_legacy_swap(&g_pcfg.controls));
   fe_ini_set_int(cfg_path, "filter", g_pdisp[FE_CONSOLE_GBA].filter);
   fe_ini_set_int(cfg_path, "ff_mult_x10", g_pcfg.ff_mult_x10);
   fe_ini_set_int(cfg_path, "ff_hold", g_pcfg.ff_hold);
   fe_ini_set_int(cfg_path, "frameskip_sparse", g_pcfg.frameskip_sparse);
   fe_ini_set_int(cfg_path, "ff_smooth", g_pcfg.ff_smooth);
   fe_ini_set_int(cfg_path, "ff_audio", g_pcfg.ff_audio);
   fe_ini_set_int(cfg_path, "theme", g_pcfg.theme);
   fe_ini_set_int(cfg_path, "ui_shell", g_pcfg.ui_shell);
   fe_ini_set_int(cfg_path, "me_dirty", g_pcfg.me_dirty);
   fe_ini_set_int(cfg_path, "show_fps", g_pcfg.show_fps);
   fe_ini_set_int(cfg_path, "gbc_color_correction",
                  g_pcfg.gbc_color_correction);
   fe_ini_set_int(cfg_path, "gb_palette",
                  g_pdisp[FE_CONSOLE_GB].gb_palette);
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
   fe_ini_set_int(cfg_path, "rfu_pace_max_hold", g_pcfg.rfu_pace_max_hold);
   fe_ini_set_int(cfg_path, "net_session_fps_force",
                  g_pcfg.net_session_fps_force);
   fe_ini_set_int(cfg_path, "nd_rto_min_us", g_pcfg.nd_rto_min_us);
   fe_ini_set_int(cfg_path, "standby", g_pcfg.standby);
   fe_ini_set_int(cfg_path, "me_boot", g_pcfg.me_boot);
   fe_ini_set_int(cfg_path, "osd_wireless", g_pcfg.osd_wireless);
   fe_ini_set(cfg_path, "group", g_pcfg.group);
   fe_ini_set(cfg_path, "nick", g_pcfg.nick);
   fe_ini_set_int(cfg_path, "console", g_pcfg.console);
   if (g_pcfg.last_rom[0])
      fe_ini_set(cfg_path, "last_rom", g_pcfg.last_rom);
   /* Writes nothing for a player who never remapped (see ctl_map.keep). */
   ctl_save_ini(&g_pcfg.controls, cfg_path);
   {
      int c;
      for (c = 0; c < FE_CONSOLE_COUNT; c++)
         pdisp_write(c);
   }
   fe_evt("config_saved scale=%d filter=%d ff_mult_x10=%d ff_hold=%d",
          g_pcfg.scale, g_pcfg.filter, g_pcfg.ff_mult_x10, g_pcfg.ff_hold);
}

/* ---- PER-CONSOLE DISPLAY PROFILES (see config_psp.h) ---------------------
 *
 * Kept in this file because the profiles ARE config: same INI, same
 * validation, same host tests (tools/test_rom_selection.c links config_psp.c
 * with only fe_util.c, so nothing here may reach the video layer). */

pcfg_display g_pdisp[FE_CONSOLE_COUNT];
static int g_pdisp_live = FE_CONSOLE_GBA;

static const char *const pdisp_tag_lc[FE_CONSOLE_COUNT] = { "gba", "gb", "gbc" };

const char *pcfg_console_tag(int console)
{
   static const char *const tag[FE_CONSOLE_COUNT] = { "GBA", "GB", "GBC" };
   return (console >= 0 && console < FE_CONSOLE_COUNT) ? tag[console] : "GBA";
}

const char *pcfg_ambient_name(int mode)
{
   switch (mode)
   {
   case PCFG_AMB_ART:      return "art";
   case PCFG_AMB_ART_GAME: return "art, else game";
   default:                return "off";
   }
}

/* Same bounds pcfg_load applies to the legacy keys. */
static void pdisp_clamp(pcfg_display *d)
{
   if (d->scale < 0 || d->scale >= VID_SCALE_MODES)
      d->scale = VID_SCALE_1X;
   d->filter = pcfg_filter_clamp(d->filter);
   if (d->ambient < 0 || d->ambient >= PCFG_AMB_MODES)
      d->ambient = PCFG_AMBIENT_DEF;
   if (d->gb_palette < 0 || d->gb_palette > 15)
      d->gb_palette = 0;
}

static int pdisp_key(char *out, size_t sz, const char *what, int console)
{
   int n = snprintf(out, sz, "%s_%s", what, pdisp_tag_lc[console]);
   return n > 0 && (size_t)n < sz;
}

static long pdisp_get(const char *what, int console, long def)
{
   char key[24];
   return pdisp_key(key, sizeof(key), what, console)
      ? fe_ini_get_int(cfg_path, key, def) : def;
}

static int pdisp_set(const char *what, int console, long v)
{
   char key[24];
   return pdisp_key(key, sizeof(key), what, console)
      ? fe_ini_set_int(cfg_path, key, v) : -1;
}

/* MIGRATION lives in the defaults: each absent key falls back to the legacy
 * value pcfg_load has already read and clamped, independently, so a
 * hand-edited file that names only `scale_gb` keeps everything else.
 * Nothing is written here -- the first save writes the new keys. */
static void pdisp_load(void)
{
   int c, migrated = 0;
   for (c = 0; c < FE_CONSOLE_COUNT; c++)
   {
      pcfg_display *d = &g_pdisp[c];
      char key[24], val[16];
      if (pdisp_key(key, sizeof(key), "scale", c) &&
          !fe_ini_get(cfg_path, key, val, sizeof(val)))
         migrated++;
      d->scale   = (int)pdisp_get("scale", c, g_pcfg.scale);
      d->filter  = (int)pdisp_get("filter", c, g_pcfg.filter);
      d->ambient = (int)pdisp_get("ambient", c, PCFG_AMBIENT_DEF);
      /* GBA has no DMG palette; its slot just carries the legacy value. */
      d->gb_palette = c == FE_CONSOLE_GBA ? g_pcfg.gb_palette
                    : (int)pdisp_get("gb_palette", c, g_pcfg.gb_palette);
      pdisp_clamp(d);
   }
   pcfg_display_select(g_pcfg.console);
   fe_log("display profiles (scale/filter/ambient/palette): gba=%d/%d/%d "
          "gb=%d/%d/%d/%d gbc=%d/%d/%d/%d migrated=%d live=%s",
          g_pdisp[0].scale, g_pdisp[0].filter, g_pdisp[0].ambient,
          g_pdisp[1].scale, g_pdisp[1].filter, g_pdisp[1].ambient,
          g_pdisp[1].gb_palette, g_pdisp[2].scale, g_pdisp[2].filter,
          g_pdisp[2].ambient, g_pdisp[2].gb_palette, migrated,
          pcfg_console_tag(g_pdisp_live));
}

void pcfg_display_select(int console)
{
   const pcfg_display *d;
   if (console < 0 || console >= FE_CONSOLE_COUNT)
      console = FE_CONSOLE_GBA;
   g_pdisp_live = console;
   d = &g_pdisp[console];
   g_pcfg.scale      = d->scale;
   g_pcfg.filter     = d->filter;
   g_pcfg.ambient    = d->ambient;
   g_pcfg.gb_palette = d->gb_palette;
}

void pcfg_display_commit(void)
{
   pcfg_display *d = &g_pdisp[g_pdisp_live];
   d->scale      = g_pcfg.scale;
   d->filter     = g_pcfg.filter;
   d->ambient    = g_pcfg.ambient;
   d->gb_palette = g_pcfg.gb_palette;
   pdisp_clamp(d);
}

int pcfg_display_console(void)
{
   return g_pdisp_live;
}

static int pdisp_write(int c)
{
   const pcfg_display *d = &g_pdisp[c];
   int rc = 0;
   rc |= pdisp_set("scale", c, d->scale);
   rc |= pdisp_set("filter", c, d->filter);
   rc |= pdisp_set("ambient", c, d->ambient);
   if (c != FE_CONSOLE_GBA)
      rc |= pdisp_set("gb_palette", c, d->gb_palette);
   return rc ? -1 : 0;
}

int pcfg_display_save(void)
{
   int rc;
   if (!cfg_path[0])
      return -1;
   pcfg_display_commit();
   rc = pdisp_write(g_pdisp_live);
   /* Keep the legacy mirror honest (see pcfg_save). */
   if (g_pdisp_live == FE_CONSOLE_GBA)
   {
      rc |= fe_ini_set_int(cfg_path, "scale", g_pdisp[FE_CONSOLE_GBA].scale);
      rc |= fe_ini_set_int(cfg_path, "filter",
                           g_pdisp[FE_CONSOLE_GBA].filter);
   }
   fe_evt("config_saved display=%s scale=%d filter=%d ambient=%d rc=%d",
          pcfg_console_tag(g_pdisp_live), g_pcfg.scale, g_pcfg.filter,
          g_pcfg.ambient, rc);
   return rc ? -1 : 0;
}
