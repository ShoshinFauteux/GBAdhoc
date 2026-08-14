/* ui_psp.c — native UI v2.
 *
 * v1 was a debug-grade panel menu.  v2 is the release face of the build:
 * a themed full-screen UI (gradient page, header/footer bars, accent
 * selection), a game-gallery ROM browser with box art, and a sectioned
 * settings screen.  The CONTRACT with main_psp.c is unchanged: same
 * ui_action enum, same entry points, same demo hooks — the overhaul is
 * confined to presentation and to one new config bit (osd_wireless).
 *
 * BOX ART: `<appdir>/boxart/<rom-name-minus-.gba>.bmp`, uncompressed
 * 24/32-bpp BMP, any size (nearest-resampled to 112x112 at load).  BMP,
 * not PNG, deliberately: this tree carries no inflate/PNG code and the
 * GE eats raw RGB565 directly.  A ROM with no art gets a styled template
 * card, so a mixed folder still looks intentional.  Art is cached in a
 * small malloc'd pool that exists ONLY while the browser is on screen —
 * the in-game UI never touches it, so session RAM is untouched.
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>

#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "ui_psp.h"
#include "video_psp.h"
#include "osd_psp.h"
#include "config_psp.h"
#include "font_8x16.h"
#include "fe_evt.h"
#include "transport_adhoc.h"

/* ----- theme (libretro RGB565) ------------------------------------------- */
/* Themed palette (RGB565, R in the high bits — rgb565_to_abgr converts for
 * the GE).  Every draw call goes through the C_* macros, so pointing g_thm
 * at a different palette reskins the entire UI in one assignment; page()
 * re-reads g_pcfg.theme each frame, which is what makes the Settings toggle
 * apply live.  The harness's black-background override (g_theme_black) sits
 * above this and only claims the page background.
 *
 * `title` is the header-bar text (both themes keep a dark header, so it
 * stays white); `sel` is selected-row/selected-card text, which must flip
 * to near-black on the light page — the one place the two roles diverge. */
typedef struct {
   uint16_t bg_top, bg_bot, hdr_top, hdr_bot, accent, accent_dk,
            title, sel, item, dim, value, warn, card, shadow;
} ui_theme;

static const ui_theme THM_DARK = {
   0x0863, 0x1948,           /* deep slate blue -> lighter slate          */
   0x2110, 0x10A7,           /* header bar                                */
   0x05F7, 0x02CB,           /* teal accent / dim teal                    */
   0xFFFF, 0xFFFF,           /* header title / selected text: white       */
   0xC618, 0x630C, 0x8F3C,   /* item / dim / light teal values            */
   0xFCC0,                   /* amber                                     */
   0x18E5, 0x0000            /* card body / shadow                        */
};

static const ui_theme THM_LIGHT = {
   0xF79E, 0xD6DB,           /* near-white -> silver blue                 */
   0x2210, 0x334F,           /* header keeps dark slate so the title pops */
   0x0451, 0x0329,           /* deep teal accent / dimmer teal            */
   0xFFFF, 0x0000,           /* header title white / selected text black  */
   0x2124, 0x8C51, 0x0330,   /* near-black items / gray / dark teal       */
   0xA280,                   /* dark amber                                */
   0xEF7D, 0x8410            /* pale card body / soft gray shadow         */
};

static const ui_theme *g_thm = &THM_DARK;

#define C_BG_TOP    (g_thm->bg_top)
#define C_BG_BOT    (g_thm->bg_bot)
#define C_HDR_TOP   (g_thm->hdr_top)
#define C_HDR_BOT   (g_thm->hdr_bot)
#define C_ACCENT    (g_thm->accent)
#define C_ACCENT_DK (g_thm->accent_dk)
#define C_TITLE     (g_thm->title)
#define C_SEL       (g_thm->sel)
#define C_ITEM      (g_thm->item)
#define C_DIM       (g_thm->dim)
#define C_VALUE     (g_thm->value)
#define C_WARN      (g_thm->warn)
#define C_CARD      (g_thm->card)
#define C_SHADOW    (g_thm->shadow)

/* ----- state -------------------------------------------------------------- */
enum { SCR_MENU, SCR_SETTINGS, SCR_WIRELESS, SCR_SCAN };

static int g_active;
static int g_screen;
static int g_cursor;
static int g_settings_dirty;

static unsigned g_prev_pad;
static int g_rep_timer;

static char g_join_group[9];

/* scan results */
static char g_scan_groups[8][9];
static int  g_scan_count = -1;   /* -1 = not scanned yet */

/* main_psp.c: re-applies the session chip after the OSD toggle changes. */
extern void osd_session_chip_refresh(void);

/* ----- demo (harness self-drive) ------------------------------------------ */
typedef struct { unsigned pad; unsigned char hold, gap; } demo_step;
#define DEMO_DUMP 0xFFFFFFFFu
static const demo_step demo_script[] = {
   { 0,               30, 0 },   /* settle on menu     */
   { DEMO_DUMP,        1, 0 },   /* GE dump of menu    */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Save state      */
   { PSP_CTRL_CROSS,   2, 10 },  /* save state         */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Load state      */
   { PSP_CTRL_CROSS,   2, 10 },  /* load state         */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Wireless        */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Settings        */
   { PSP_CTRL_CROSS,   2, 8 },   /* enter settings     */
   /* v2 NOTE: the cursor lands on "Trading profile" (ADR-0071 keeps it
    * first).  The four RIGHT/RIGHT/LEFT/LEFT presses toggle it an EVEN
    * number of times, and g_profile_changed now tracks the DIFFERENCE
    * against the value at entry rather than "was ever touched", so the
    * demo still exits settings without triggering a relaunch. */
   { PSP_CTRL_RIGHT,   2, 4 },
   { DEMO_DUMP,        1, 0 },   /* GE dump of settings*/
   { PSP_CTRL_RIGHT,   2, 4 },
   { PSP_CTRL_LEFT,    2, 4 },
   { PSP_CTRL_LEFT,    2, 4 },
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to menu (cursor -> Resume) */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Save state      */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Load state      */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Wireless        */
   { PSP_CTRL_CROSS,   2, 8 },   /* enter wireless     */
   { DEMO_DUMP,        1, 0 },   /* GE dump wireless   */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back (cursor -> Resume) */
   { PSP_CTRL_CROSS,   2, 8 },   /* resume             */
};
static int g_demo_on = -1;       /* -1 idle, else script index */
static int g_demo_phase;         /* frames left in hold(+)/gap(-) */

/* Asset-shoot arm (README screenshots): when set, the ROM browser dumps the
 * gallery after the box art settles and auto-picks the current game, so a
 * ui_demo run needs no human input from cold boot to finished dump set. */
static int g_ui_shots;
void ui_demo_shots(void) { g_ui_shots = 1; }

/* Page background: black is the HARNESS identity, the slate gradient the
 * playable's.  Runtime-overridable (ui_theme_black=0) so the asset shoot can
 * photograph the playable look from the harness-channel build. */
#ifdef GPSP_PLAYABLE
static int g_theme_black = 0;
#else
static int g_theme_black = 1;
#endif
void ui_set_theme_black(int b) { if (b >= 0) g_theme_black = b ? 1 : 0; }

void ui_demo_start(void)
{
   g_demo_on = 0;
   g_demo_phase = 0;
   fe_evt("ui_demo_start");
}

int ui_demo_running(void)
{
   return g_demo_on >= 0;
}

static unsigned demo_pad(void)
{
   const demo_step *s;
   if (g_demo_on < 0)
      return 0;
   if (g_demo_on >= (int)(sizeof(demo_script) / sizeof(demo_script[0])))
   {
      g_demo_on = -1;
      fe_evt("ui_demo_done");
      return 0;
   }
   s = &demo_script[g_demo_on];
   if (s->pad == DEMO_DUMP)
   {
      extern char g_dir_base[];
      static int dump_n;
      char gp[176];
      snprintf(gp, sizeof(gp), "%s/log/ge_ui_%d.bmp", g_dir_base, dump_n);
      if (vid_dump_ge(gp) == 0)
         fe_evt("ge_dump file=ge_ui_%d.bmp ui=1", dump_n);
      dump_n++;
      g_demo_on++;
      g_demo_phase = 0;
      return 0;
   }
   if (g_demo_phase == 0)
      g_demo_phase = s->hold;
   if (g_demo_phase > 0)
   {
      g_demo_phase--;
      if (g_demo_phase == 0)
         g_demo_phase = -(int)s->gap - 1;
      return s->pad;
   }
   g_demo_phase++;
   if (g_demo_phase == 0)
      g_demo_on++;
   return 0;
}

/* ----- helpers ------------------------------------------------------------ */

const char *ui_group(void)
{
   return g_join_group;
}

/* ADR-0071: relaunch only when the profile actually DIFFERS from its value
 * at settings entry — "touched an even number of times" is not a change.
 * (Also what keeps the harness ui_demo from relaunching the EBOOT.) */
static int g_profile_at_open;

void ui_open(void)
{
   g_active = 1;
   g_screen = SCR_MENU;
   g_cursor = 0;
   g_prev_pad = 0xFFFFFFFFu;   /* swallow the opening chord */
   fe_evt("ui_open");
}

void ui_close(void)
{
   if (g_settings_dirty)
   {
      pcfg_save();
      g_settings_dirty = 0;
   }
   g_active = 0;
   if (g_demo_on >= 0)
   {
      /* The demo's final Resume closes the UI mid-script by design. */
      g_demo_on = -1;
      fe_evt("ui_demo_done");
   }
   fe_evt("ui_close");
}

int ui_active(void)
{
   return g_active;
}

static void screen_to(int scr)
{
   static const char *names[] = { "menu", "settings", "wireless", "scan" };
   if (g_screen != scr && g_settings_dirty)
   {
      pcfg_save();
      g_settings_dirty = 0;
   }
   g_screen = scr;
   g_cursor = 0;
   if (scr == SCR_SETTINGS)
      g_profile_at_open = g_pcfg.me_mode;
   fe_evt("ui_screen name=%s", names[scr]);
}

/* Edge/repeat filter: returns buttons to act on this frame. */
static unsigned pad_edges(unsigned pad)
{
   unsigned edges = pad & ~g_prev_pad;
   unsigned dirs = pad & (PSP_CTRL_UP | PSP_CTRL_DOWN |
                          PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
   if (dirs && dirs == (g_prev_pad & dirs))
   {
      if (++g_rep_timer >= 18)
      {
         g_rep_timer = 13;
         edges |= dirs;
      }
   }
   else
      g_rep_timer = 0;
   g_prev_pad = pad;
   return edges;
}

/* ----- themed page chrome ------------------------------------------------- */

#define HDR_H  30
#define FTR_H  20

/* Full-page background + header bar.  `right` (optional) is right-aligned
 * in the header — room code, game count, session state. */
static void page(const char *title, const char *right)
{
   g_thm = (g_pcfg.theme == 1) ? &THM_LIGHT : &THM_DARK;
   if (g_theme_black)
      /* HARNESS: plain black page — the at-a-glance differentiator from the
       * playable build. */
      vid_rect(0, 0, VID_SCR_W, VID_SCR_H, 0x0000, 255);
   else
      vid_gradient(0, 0, VID_SCR_W, VID_SCR_H, C_BG_TOP, C_BG_BOT, 255);
   vid_gradient(0, 0, VID_SCR_W, HDR_H, C_HDR_TOP, C_HDR_BOT, 255);
   vid_rect(0, HDR_H, VID_SCR_W, 2, C_ACCENT, 255);
   vid_rect(0, HDR_H + 2, VID_SCR_W, 1, C_ACCENT_DK, 160);
   vid_text(14, (HDR_H - FE_FONT_H) / 2, title, C_TITLE);
   if (right && right[0])
      vid_text(VID_SCR_W - 14 - (int)strlen(right) * FE_FONT_W,
               (HDR_H - FE_FONT_H) / 2, right, C_VALUE);
}

static void footer(const char *hint)
{
   vid_rect(0, VID_SCR_H - FTR_H, VID_SCR_W, 1, C_ACCENT_DK, 180);
   vid_rect(0, VID_SCR_H - FTR_H + 1, VID_SCR_W, FTR_H - 1, C_HDR_BOT, 220);
   vid_text_center(VID_SCR_H - FTR_H + 2, hint, C_DIM);
}

/* One list row.  Selection is an accent edge bar + a soft fill, which reads
 * as "modern" at 480x272 far better than v1's full-row alpha slab. */
static void row(int x, int y, int w, int selected, int enabled,
                const char *label, const char *value)
{
   if (selected)
   {
      vid_rect(x, y - 1, w, FE_FONT_H + 2, C_ACCENT, 36);
      vid_rect(x, y - 1, 3, FE_FONT_H + 2, C_ACCENT, 255);
   }
   vid_text(x + 12, y, label, enabled ? (selected ? C_SEL : C_ITEM)
                                      : C_DIM);
   if (value)
      vid_text(x + w - 10 - (int)strlen(value) * FE_FONT_W, y, value,
               enabled ? (selected ? C_VALUE : C_ACCENT_DK) : C_DIM);
}

/* ----- settings screen ---------------------------------------------------- */

/* Sectioned.  Trading profile stays FIRST (ADR-0071: it is the one setting
 * that decides whether a trade completes, and the only one that costs a
 * restart).  Header rows are labels, not stops — the cursor skips them. */
enum { SET_HDR_WL, SET_PROFILE, SET_ROOM, SET_OSD,
       SET_HDR_VID, SET_SCALE, SET_FILTER, SET_THEME,
       SET_HDR_GAME, SET_FFMULT, SET_FFMODE, SET_ABMAP, SET_FPS,
       SET_COUNT };

static const struct { unsigned char header; const char *label; } set_rows[SET_COUNT] = {
   { 1, "WIRELESS" },
   { 0, "Media Engine mode" },
   { 0, "Room code" },
   { 0, "Session overlay" },
   { 1, "VIDEO" },
   { 0, "Video scale" },
   { 0, "Video filter" },
   { 0, "Theme" },
   { 1, "GAMEPLAY" },
   { 0, "Fast-forward" },
   { 0, "FF button (Square)" },
   { 0, "A/B buttons" },
   { 0, "FPS counter" },
};

static int set_row_y(int idx)
{
   /* 13 rows must fit between the header (30) and footer (252) — the Back
    * row paid for one of the Theme/FPS additions (O exits, it was pure
    * redundancy) and the pixel budget paid for the other: headers advance
    * 19 px, items 16.  Last row lands at y=235, text ends at 251 — 1 px
    * clear of the footer.  The selection fill (FE_FONT_H+2 tall) now grazes
    * the next row by 1 px; at alpha 36 it does not read. */
   int y = HDR_H + 4, i;
   for (i = 0; i < idx; i++)
      y += set_rows[i].header ? (FE_FONT_H + 3) : FE_FONT_H;
   return y;
}

static void set_cursor_step(int dir)
{
   do
      g_cursor = (g_cursor + SET_COUNT + dir) % SET_COUNT;
   while (set_rows[g_cursor].header);
}

static int g_profile_changed;

/* The row was "Trading profile" (29.97/57.00) until the ADR-0075 frame-pace
 * fix made full speed the only rate worth shipping; the slot and its
 * restart-to-apply machinery (ADR-0071) now drive the Media Engine renderer
 * instead — the other setting that can only be applied at boot. */
static const char *me_mode_name(int on)
{
   return on ? "on (2nd-core render)" : "off";
}

static const char *ff_mult_name(int x10)
{
   switch (x10)
   {
   case 15: return "1.5x";
   case 30: return "3x";
   case 0:  return "uncapped";
   default: return "1.5x";   /* legacy 2x configs display as their remap */
   }
}

/* Speed and style share one row: six values, no extra line on a settings
 * page that is already full to the pixel. */
static const char *ff_mode_name(void)
{
   static char buf[20];
   snprintf(buf, sizeof(buf), "%s%s", ff_mult_name(g_pcfg.ff_mult_x10),
            g_pcfg.ff_smooth ? " smooth" : "");
   return buf;
}

static void settings_adjust(int id, int dir)
{
   switch (id)
   {
   case SET_PROFILE:
      g_pcfg.me_mode = !g_pcfg.me_mode;
      g_profile_changed = (g_pcfg.me_mode != g_profile_at_open);
      break;
   case SET_SCALE:
      g_pcfg.scale = (g_pcfg.scale + VID_SCALE_MODES + dir) % VID_SCALE_MODES;
      vid_set_mode(g_pcfg.scale, g_pcfg.filter);
      fe_evt("video_mode scale=%s filter=%s",
             vid_scale_name(g_pcfg.scale), vid_filter_name(g_pcfg.filter));
      break;
   case SET_FILTER:
      g_pcfg.filter = !g_pcfg.filter;
      vid_set_mode(g_pcfg.scale, g_pcfg.filter);
      fe_evt("video_mode scale=%s filter=%s",
             vid_scale_name(g_pcfg.scale), vid_filter_name(g_pcfg.filter));
      break;
   case SET_OSD:
      g_pcfg.osd_wireless = !g_pcfg.osd_wireless;
      osd_session_chip_refresh();
      break;
   case SET_ABMAP:
      g_pcfg.btn_swap = !g_pcfg.btn_swap;
      break;
   case SET_THEME:
      g_pcfg.theme = !g_pcfg.theme;   /* applies live — page() re-reads it */
      break;
   case SET_FPS:
      g_pcfg.show_fps = !g_pcfg.show_fps;
      break;
   case SET_FFMULT:
   {
      /* 2x retired: it froze the ME-mode display across three separate
       * present implementations while 1.5x/3x/uncapped all behave.  Rather
       * than ship a haunted speed tier, it no longer exists. */
      /* The row cycles speed THEN style: 1.5x, 3x, uncapped, then the same
       * three "smooth" (frameskip off — every emulated frame is rendered). */
      static const int vals[3] = { 15, 30, 0 };
      int i, idx;
      for (i = 0; i < 3; i++)
         if (vals[i] == g_pcfg.ff_mult_x10)
            break;
      if (i == 3)
         i = 0;
      idx = (g_pcfg.ff_smooth ? 3 : 0) + i;
      idx = (idx + 6 + dir) % 6;
      g_pcfg.ff_smooth   = (idx >= 3);
      g_pcfg.ff_mult_x10 = vals[idx % 3];
      break;
   }
   case SET_FFMODE:
      g_pcfg.ff_hold = !g_pcfg.ff_hold;
      break;
   case SET_ROOM:
   {
      int nn = 0;
      if (strlen(g_pcfg.group) == 6 && strncmp(g_pcfg.group, "GPSP", 4) == 0)
         nn = (g_pcfg.group[4] - '0') * 10 + (g_pcfg.group[5] - '0');
      nn = (nn + 100 + dir) % 100;
      snprintf(g_pcfg.group, sizeof(g_pcfg.group), "GPSP%02d", nn);
      break;
   }
   default:
      return;
   }
   g_settings_dirty = 1;
}

static ui_action screen_settings(unsigned edges)
{
   int i;

   if (set_rows[g_cursor].header)
      g_cursor = SET_PROFILE;
   if (edges & PSP_CTRL_UP)
      set_cursor_step(-1);
   if (edges & PSP_CTRL_DOWN)
      set_cursor_step(+1);
   if (edges & PSP_CTRL_LEFT)
      settings_adjust(g_cursor, -1);
   if (edges & PSP_CTRL_RIGHT)
      settings_adjust(g_cursor, +1);
   if (edges & PSP_CTRL_CROSS)
      settings_adjust(g_cursor, +1);
   if (edges & PSP_CTRL_CIRCLE)
   {
      /* ADR-0071: leaving SETTINGS after changing the profile is the commit
       * point.  The main loop saves and relaunches from there — not from
       * here — so the SRAM flush, thread teardown and log close all happen
       * exactly as they do on a normal exit.  Relaunching out of the UI with
       * the io thread still holding the .sav is how a save gets truncated. */
      if (g_profile_changed) { g_profile_changed = 0;
                               return UI_ACT_RELAUNCH; }
      screen_to(SCR_MENU);
   }

   page("SETTINGS", g_pcfg.group);
   for (i = 0; i < SET_COUNT; i++)
   {
      int y = set_row_y(i);
      if (set_rows[i].header)
      {
         int lx = 24 + ((int)strlen(set_rows[i].label) + 1) * FE_FONT_W;
         vid_text(24, y, set_rows[i].label, C_ACCENT);
         vid_rect(lx, y + FE_FONT_H / 2, 456 - lx, 1, C_ACCENT_DK, 120);
         continue;
      }
      switch (i)
      {
      case SET_PROFILE:
         row(36, y, 408, g_cursor == i, 1, set_rows[i].label,
             me_mode_name(g_pcfg.me_mode));
         break;
      case SET_ROOM:
         row(36, y, 408, g_cursor == i, 1, set_rows[i].label, g_pcfg.group);
         break;
      case SET_OSD:
         row(36, y, 408, g_cursor == i, 1, set_rows[i].label,
             g_pcfg.osd_wireless ? "shown" : "hidden");
         break;
      case SET_SCALE:
         row(36, y, 408, g_cursor == i, 1, set_rows[i].label,
             vid_scale_name(g_pcfg.scale));
         break;
      case SET_FILTER:
         row(36, y, 408, g_cursor == i, 1, set_rows[i].label,
             vid_filter_name(g_pcfg.filter));
         break;
      case SET_FFMULT:
         row(36, y, 408, g_cursor == i, 1, set_rows[i].label, ff_mode_name());
         break;
      case SET_FFMODE:
         row(36, y, 408, g_cursor == i, 1, set_rows[i].label,
             g_pcfg.ff_hold ? "hold" : "toggle");
         break;
      case SET_ABMAP:
         row(36, y, 408, g_cursor == i, 1, set_rows[i].label,
             g_pcfg.btn_swap ? "A=X  B=O" : "A=O  B=X");
         break;
      case SET_THEME:
         row(36, y, 408, g_cursor == i, 1, set_rows[i].label,
             g_pcfg.theme ? "light" : "dark");
         break;
      case SET_FPS:
         row(36, y, 408, g_cursor == i, 1, set_rows[i].label,
             g_pcfg.show_fps ? "on" : "off");
         break;
      }
   }
   if (g_profile_changed)
      footer("Media Engine mode applies on RESTART -- O to apply and relaunch");
   else
      footer("DPAD move/change   X select   O back");
   return UI_ACT_NONE;
}

/* ----- wireless screens --------------------------------------------------- */

enum { WL_HOST, WL_SCAN, WL_JOINCODE, WL_BACK, WL_COUNT };
enum { WLS_DISCONNECT, WLS_BACK, WLS_COUNT };

static ui_action screen_wireless(unsigned edges, int session_active,
                                 const char *session_info)
{
   if (session_active)
   {
      if (edges & PSP_CTRL_UP)
         g_cursor = (g_cursor + WLS_COUNT - 1) % WLS_COUNT;
      if (edges & PSP_CTRL_DOWN)
         g_cursor = (g_cursor + 1) % WLS_COUNT;
      if (edges & PSP_CTRL_CIRCLE)
         screen_to(SCR_MENU);
      if (edges & PSP_CTRL_CROSS)
      {
         if (g_cursor == WLS_DISCONNECT)
         {
            screen_to(SCR_MENU);
            return UI_ACT_NET_DISCONNECT;
         }
         screen_to(SCR_MENU);
      }
      page("WIRELESS", "LINKED");
      /* status card */
      vid_rect(62, 62, 360, 54, C_SHADOW, 90);
      vid_rect(58, 58, 360, 54, C_CARD, 235);
      vid_rect(58, 58, 360, 2, C_ACCENT, 255);
      vid_text(74, 66, "Status", C_DIM);
      vid_text(74, 88, session_info ? session_info : "session active",
               C_VALUE);
      row(58, 140, 360, g_cursor == WLS_DISCONNECT, 1, "Disconnect", NULL);
      row(58, 162, 360, g_cursor == WLS_BACK, 1, "Back", NULL);
      footer("X select   O back");
      return UI_ACT_NONE;
   }

   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + WL_COUNT - 1) % WL_COUNT;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % WL_COUNT;
   if (g_cursor == WL_JOINCODE)
   {
      if (edges & PSP_CTRL_LEFT)
         settings_adjust(SET_ROOM, -1);
      if (edges & PSP_CTRL_RIGHT)
         settings_adjust(SET_ROOM, +1);
   }
   if (edges & PSP_CTRL_CIRCLE)
      screen_to(SCR_MENU);
   if (edges & PSP_CTRL_CROSS)
   {
      switch (g_cursor)
      {
      case WL_HOST:
         snprintf(g_join_group, sizeof(g_join_group), "%s", g_pcfg.group);
         screen_to(SCR_MENU);
         return UI_ACT_NET_HOST;
      case WL_SCAN:
         g_scan_count = -1;
         screen_to(SCR_SCAN);
         return UI_ACT_NONE;
      case WL_JOINCODE:
         snprintf(g_join_group, sizeof(g_join_group), "%s", g_pcfg.group);
         screen_to(SCR_MENU);
         return UI_ACT_NET_JOIN;
      case WL_BACK:
         screen_to(SCR_MENU);
         return UI_ACT_NONE;
      }
   }

   page("WIRELESS", g_pcfg.group);
   vid_text(36, HDR_H + 16, "Link two PSPs over ad-hoc WiFi.  Both consoles",
            C_DIM);
   vid_text(36, HDR_H + 16 + FE_FONT_H + 2, "must use the same room code.",
            C_DIM);
   row(36, 106, 408, g_cursor == WL_HOST, 1, "Host session", NULL);
   row(36, 128, 408, g_cursor == WL_SCAN, 1, "Join: scan for rooms", NULL);
   row(36, 150, 408, g_cursor == WL_JOINCODE, 1, "Join room code",
       g_pcfg.group);
   row(36, 172, 408, g_cursor == WL_BACK, 1, "Back", NULL);
   footer("X select   DPAD change code   O back");
   return UI_ACT_NONE;
}

static ui_action screen_scan(unsigned edges)
{
   int rows = (g_scan_count > 0 ? g_scan_count : 1) + 1;

   if (g_scan_count < 0)
   {
      /* Draw one "scanning" frame; the blocking scan runs on the NEXT
       * frame so the message is visible during the wait. */
      static int drew_notice;
      page("WIRELESS", "SCANNING");
      vid_text_center(120, "Searching for rooms (10s)...", C_ITEM);
      vid_rect(140, 148, 200, 3, C_ACCENT_DK, 200);
      if (drew_notice)
      {
         int n = adhoc_transport_scan(g_scan_groups, 8, 0);
         g_scan_count = (n < 0) ? 0 : n;
         fe_evt("wl_scan result=%d rc_stage=%s", n, adhoc_transport_stage());
         if (n == ADHOC_ERR_WLAN_OFF)
            osd_toast("WLAN switch is OFF");
         drew_notice = 0;
      }
      else
         drew_notice = 1;
      return UI_ACT_NONE;
   }

   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + rows - 1) % rows;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % rows;
   if (edges & PSP_CTRL_CIRCLE)
      screen_to(SCR_WIRELESS);
   if (edges & PSP_CTRL_CROSS)
   {
      if (g_scan_count > 0 && g_cursor < g_scan_count)
      {
         snprintf(g_join_group, sizeof(g_join_group), "%s",
                  g_scan_groups[g_cursor]);
         screen_to(SCR_MENU);
         return UI_ACT_NET_JOIN;
      }
      screen_to(SCR_WIRELESS);
   }

   page("WIRELESS", g_scan_count ? "ROOMS FOUND" : "NO ROOMS");
   if (g_scan_count > 0)
   {
      int i;
      for (i = 0; i < g_scan_count; i++)
         row(36, HDR_H + 16 + i * 22, 408, g_cursor == i, 1,
             g_scan_groups[i], NULL);
      row(36, HDR_H + 16 + g_scan_count * 22, 408,
          g_cursor == g_scan_count, 1, "Back", NULL);
   }
   else
   {
      vid_text_center(HDR_H + 24, "No rooms answered the scan.", C_DIM);
      vid_text_center(HDR_H + 24 + FE_FONT_H + 2,
                      "Have the other PSP host first, then rescan.", C_DIM);
      row(36, HDR_H + 76, 408, 1, 1, "Back", NULL);
   }
   footer("X join   O back");
   return UI_ACT_NONE;
}

/* ----- main menu ---------------------------------------------------------- */

/* M_GAMELIST sits after Settings so the demo script's fixed row counts for
 * rows 0-4 stay valid (it never navigates past Settings). */
enum { M_RESUME, M_SAVESTATE, M_LOADSTATE, M_WIRELESS, M_SETTINGS, M_GAMELIST,
       M_EXIT, M_COUNT };

static ui_action screen_menu(unsigned edges, int session_active)
{
   static const char *labels[M_COUNT] = {
      "Resume", "Save state", "Load state", "Wireless", "Settings",
      "Quit to game list", "Exit"
   };
   int i, top;

   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + M_COUNT - 1) % M_COUNT;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % M_COUNT;
   if (edges & PSP_CTRL_CIRCLE)
      return UI_ACT_RESUME;
   if (edges & PSP_CTRL_CROSS)
   {
      switch (g_cursor)
      {
      case M_RESUME:    return UI_ACT_RESUME;
      case M_SAVESTATE:
         if (session_active)
         {
            osd_toast("Savestates locked during wireless session");
            break;
         }
         return UI_ACT_SAVESTATE;
      case M_LOADSTATE:
         if (session_active)
         {
            osd_toast("Savestates locked during wireless session");
            break;
         }
         return UI_ACT_LOADSTATE;
      case M_WIRELESS:  screen_to(SCR_WIRELESS); break;
      case M_SETTINGS:  screen_to(SCR_SETTINGS); break;
      case M_GAMELIST:  return UI_ACT_GAMELIST;
      case M_EXIT:      return UI_ACT_EXIT;
      }
   }

   page("GBAdhoc", session_active ? "LINKED" : g_pcfg.group);
   top = HDR_H + 22;
   for (i = 0; i < M_COUNT; i++)
   {
      int enabled = !((i == M_SAVESTATE || i == M_LOADSTATE) &&
                      session_active);
      const char *val = (i == M_WIRELESS && session_active) ? "linked" : NULL;
      row(120, top + i * 26, 240, g_cursor == i, enabled, labels[i], val);
   }
   footer("X select   O resume");
   return UI_ACT_NONE;
}

/* ----- frame dispatcher --------------------------------------------------- */

ui_action ui_frame(unsigned pad, int session_active, const char *session_info)
{
   unsigned edges;
   ui_action act = UI_ACT_NONE;

   if (!g_active)
      return UI_ACT_NONE;

   if (ui_demo_running())
      pad = demo_pad();
   edges = pad_edges(pad);

   vid_overlay_begin(1);
   switch (g_screen)
   {
   case SCR_SETTINGS:
      act = screen_settings(edges);
      break;
   case SCR_WIRELESS:
      act = screen_wireless(edges, session_active, session_info);
      break;
   case SCR_SCAN:
      act = screen_scan(edges);
      break;
   default:
      act = screen_menu(edges, session_active);
      break;
   }
   vid_overlay_end();

   if (act == UI_ACT_RESUME || act == UI_ACT_EXIT ||
       act == UI_ACT_NET_HOST || act == UI_ACT_NET_JOIN)
      ui_close();
   return act;
}

/* ----- ROM browser: game gallery ------------------------------------------ */

#define BROWSER_MAX 64

typedef struct { char name[96]; int has_sav; } rom_entry;
static rom_entry g_roms[BROWSER_MAX];

static int rom_scan(const char *rom_dir)
{
   SceUID d = sceIoDopen(rom_dir);
   SceIoDirent ent;
   int n = 0;

   if (d < 0)
      return 0;
   memset(&ent, 0, sizeof(ent));
   while (n < BROWSER_MAX && sceIoDread(d, &ent) > 0)
   {
      size_t l = strlen(ent.d_name);
      if (l > 4 && l < sizeof(g_roms[0].name) &&
          strcasecmp(ent.d_name + l - 4, ".gba") == 0)
      {
         char sav[256];
         SceIoStat st;
         snprintf(g_roms[n].name, sizeof(g_roms[n].name), "%s", ent.d_name);
         snprintf(sav, sizeof(sav), "%s/%.*s.sav", rom_dir, (int)(l - 4),
                  ent.d_name);
         g_roms[n].has_sav = sceIoGetstat(sav, &st) >= 0;
         n++;
      }
      memset(&ent, 0, sizeof(ent));
   }
   sceIoDclose(d);

   /* insertion sort by name (case-insensitive-ish) */
   {
      int i, j;
      for (i = 1; i < n; i++)
      {
         rom_entry key = g_roms[i];
         for (j = i - 1; j >= 0 && strcasecmp(g_roms[j].name, key.name) > 0;
              j--)
            g_roms[j + 1] = g_roms[j];
         g_roms[j + 1] = key;
      }
   }
   return n;
}

/* ---- box art cache -------------------------------------------------------
 * Small malloc'd pool, alive only while the browser runs.  Each slot is a
 * 128x128 RGB565 texture (32 KiB) holding the art nearest-resampled to
 * ART_PX x ART_PX in its top-left corner.  state: 0 free, 1 = art loaded,
 * -1 = tried and missing (a negative result is cached too — retrying a
 * missing file every frame would be 60 directory misses a second on a
 * memory stick that ADR-0069 priced at 12.4 ms per negative lookup). */
#define ART_SLOTS 6
#define ART_PX    112

typedef struct { int rom_idx; int state; uint16_t *tex; } art_slot;
static art_slot g_art[ART_SLOTS];
static int g_art_clock;

static void art_free_all(void)
{
   int i;
   for (i = 0; i < ART_SLOTS; i++)
   {
      free(g_art[i].tex);
      g_art[i].tex = NULL;
      g_art[i].state = 0;
      g_art[i].rom_idx = -1;
   }
}

/* Nearest-resample a 24/32bpp bottom-up-or-top-down BI_RGB BMP into
 * slot->tex.  Row-at-a-time with seeks, so a 500x500 cover never needs a
 * whole-file buffer.  Returns 0 on success. */
static int art_load_bmp(const char *path, uint16_t *tex)
{
   unsigned char hdr[54];
   unsigned char rowbuf[4096];
   SceUID f = sceIoOpen(path, PSP_O_RDONLY, 0);
   int w, h, bpp, comp, topdown, dy;
   unsigned off, rowbytes;

   if (f < 0)
   {
      fe_evt("art_fail stage=open path=%s rc=%08x", path, (unsigned)f);
      return -1;
   }
   if (sceIoRead(f, hdr, 54) != 54 || hdr[0] != 'B' || hdr[1] != 'M')
   {
      fe_evt("art_fail stage=hdr path=%s", path);
      sceIoClose(f);
      return -1;
   }
   off  = hdr[10] | (hdr[11] << 8) | ((unsigned)hdr[12] << 16) |
          ((unsigned)hdr[13] << 24);
   w    = (int)(hdr[18] | (hdr[19] << 8) | ((unsigned)hdr[20] << 16) |
          ((unsigned)hdr[21] << 24));
   h    = (int)(hdr[22] | (hdr[23] << 8) | ((unsigned)hdr[24] << 16) |
          ((unsigned)hdr[25] << 24));
   bpp  = hdr[28] | (hdr[29] << 8);
   comp = hdr[30] | (hdr[31] << 8) | ((unsigned)hdr[32] << 16) |
          ((unsigned)hdr[33] << 24);
   topdown = 0;
   if (h < 0) { h = -h; topdown = 1; }
   if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || comp != 0 ||
       (bpp != 24 && bpp != 32))
   {
      fe_evt("art_fail stage=fmt path=%s w=%d h=%d bpp=%d comp=%d",
             path, w, h, bpp, comp);
      sceIoClose(f);
      return -1;
   }
   rowbytes = ((unsigned)w * (bpp / 8) + 3) & ~3u;
   if (rowbytes > sizeof(rowbuf))
   {
      fe_evt("art_fail stage=rowbuf path=%s rowbytes=%u", path, rowbytes);
      sceIoClose(f);
      return -1;
   }

   for (dy = 0; dy < ART_PX; dy++)
   {
      int sy = dy * h / ART_PX;
      int fy = topdown ? sy : (h - 1 - sy);
      int dx;
      uint16_t *dst = &tex[dy * 128];
      if (sceIoLseek32(f, (int)(off + (unsigned)fy * rowbytes),
                       PSP_SEEK_SET) < 0 ||
          sceIoRead(f, rowbuf, rowbytes) != (int)rowbytes)
      {
         fe_evt("art_fail stage=read path=%s dy=%d fy=%d off=%u", path, dy,
                fy, off);
         sceIoClose(f);
         return -1;
      }
      for (dx = 0; dx < ART_PX; dx++)
      {
         const unsigned char *p = &rowbuf[(dx * w / ART_PX) * (bpp / 8)];
         /* BMP is B,G,R[,A]; GE 5650 texel is PSP channel order (R low). */
         dst[dx] = (uint16_t)((p[2] >> 3) | ((p[1] >> 2) << 5) |
                              ((p[0] >> 3) << 11));
      }
   }
   sceIoClose(f);
   return 0;
}

/* Cache lookup; loads on miss (`load` != 0).  Returns the texture or NULL. */
static const uint16_t *art_get(const char *rom_dir, int rom_idx, int load)
{
   extern char g_dir_base[];
   int i, victim;
   (void)rom_dir;

   for (i = 0; i < ART_SLOTS; i++)
      if (g_art[i].state && g_art[i].rom_idx == rom_idx)
         return g_art[i].state > 0 ? g_art[i].tex : NULL;
   if (!load)
      return NULL;

   victim = g_art_clock;
   g_art_clock = (g_art_clock + 1) % ART_SLOTS;
   if (!g_art[victim].tex)
   {
      g_art[victim].tex = (uint16_t *)memalign(16, 128 * 128 * 2);
      if (!g_art[victim].tex)
         return NULL;
   }
   g_art[victim].rom_idx = rom_idx;
   {
      char path[280];
      size_t l = strlen(g_roms[rom_idx].name);
      snprintf(path, sizeof(path), "%s/boxart/%.*s.bmp", g_dir_base,
               (int)(l > 4 ? l - 4 : l), g_roms[rom_idx].name);
      if (art_load_bmp(path, g_art[victim].tex) == 0)
      {
         sceKernelDcacheWritebackRange(g_art[victim].tex, 128 * 128 * 2);
         g_art[victim].state = 1;
         return g_art[victim].tex;
      }
   }
   g_art[victim].state = -1;   /* negative result cached too */
   return NULL;
}

/* Display name: extension already stripped by the caller; additionally cut
 * region tags for the card face ("Pokemon - Emerald Version (USA, Europe)"
 * -> "Pokemon - Emerald Version"). */
static void rom_display_name(const rom_entry *r, char *out, size_t sz,
                             int cut_region)
{
   size_t l = strlen(r->name);
   size_t n = (l > 4) ? l - 4 : l;
   if (n >= sz)
      n = sz - 1;
   memcpy(out, r->name, n);
   out[n] = '\0';
   if (cut_region)
   {
      char *par = strchr(out, '(');
      while (par > out && par[-1] == ' ')
         par--;
      if (par && par > out)
         *par = '\0';
   }
}

/* Template card for a ROM with no art: accent header strip, wrapped title,
 * GBA badge.  Deliberately styled so a folder with zero art still looks
 * designed rather than broken. */
static void card_template(int x, int y, int size, const rom_entry *r)
{
   char title[64];
   int max_cols = (size - 16) / FE_FONT_W;
   int max_rows = (size - 40) / (FE_FONT_H + 1);
   int line = 0;
   const char *p;

   vid_gradient(x, y, size, size, C_CARD, C_BG_TOP, 255);
   vid_rect(x, y, size, 6, C_ACCENT, 255);
   rom_display_name(r, title, sizeof(title), 1);

   p = title;
   while (*p && line < max_rows)
   {
      char seg[40];
      int n = (int)strlen(p);
      if (n > max_cols)
      {
         /* break at the last space that fits, else hard-break */
         int b = max_cols;
         while (b > 0 && p[b] != ' ')
            b--;
         n = b > 0 ? b : max_cols;
      }
      if (n >= (int)sizeof(seg))
         n = (int)sizeof(seg) - 1;
      memcpy(seg, p, n);
      seg[n] = '\0';
      vid_text(x + 8, y + 14 + line * (FE_FONT_H + 1), seg, C_ITEM);
      p += n;
      while (*p == ' ')
         p++;
      line++;
   }
   vid_text(x + size - 8 - 3 * FE_FONT_W, y + size - FE_FONT_H - 6,
            "GBA", C_ACCENT_DK);
}

static void card_draw(const char *rom_dir, int rom_idx, int cx, int cy,
                      int size, int selected, int load_art)
{
   int x = cx - size / 2, y = cy - size / 2;
   const uint16_t *art = art_get(rom_dir, rom_idx, load_art);

   vid_rect(x + 4, y + 5, size, size, C_SHADOW, 110);       /* drop shadow */
   if (art)
      vid_image(x, y, size, size, art, ART_PX, ART_PX, 255);
   else
      card_template(x, y, size, &g_roms[rom_idx]);
   if (selected)
   {
      vid_rect(x - 2, y - 2, size + 4, 2, C_ACCENT, 255);
      vid_rect(x - 2, y + size, size + 4, 2, C_ACCENT, 255);
      vid_rect(x - 2, y, 2, size, C_ACCENT, 255);
      vid_rect(x + size, y, 2, size, C_ACCENT, 255);
   }
   else
      vid_rect(x, y, size, size, C_BG_TOP, 96);             /* dim veil */
   if (g_roms[rom_idx].has_sav)
   {
      int bw = 4 * FE_FONT_W + 8;
      vid_rect(x + size - bw - 4, y + 4, bw, FE_FONT_H + 2, C_HDR_BOT, 220);
      vid_text(x + size - bw, y + 5, "SAVE", C_VALUE);
   }
}

extern volatile int g_running;               /* main_psp exit flag */

int ui_browser(const char *rom_dir, char *out, size_t out_sz)
{
   int n = rom_scan(rom_dir);
   int cur = 0, i, idle = 0;

   fe_evt("ui_browser roms=%d", n);
   if (n == 0)
   {
      /* Nothing to show: draw a notice for ~3 s, then give up.  The path
       * printed is the one we ACTUALLY scanned (see v1 note re hardcoded
       * paths going stale). */
      for (i = 0; i < 180 && g_running; i++)
      {
         vid_overlay_begin(1);
         page("GBAdhoc", NULL);
         vid_text_center(110, "No .gba ROMs found", C_WARN);
         vid_text_center(140, "Copy ROMs to:", C_ITEM);
         vid_text_center(162, rom_dir, C_ITEM);
         vid_overlay_end();
         sceDisplayWaitVblankStart();
         vid_swap();
      }
      return -1;
   }

   /* preselect last played */
   for (i = 0; i < n; i++)
      if (g_pcfg.last_rom[0] && strcmp(g_roms[i].name, g_pcfg.last_rom) == 0)
         cur = i;

   memset(g_art, 0, sizeof(g_art));
   for (i = 0; i < ART_SLOTS; i++)
      g_art[i].rom_idx = -1;

   g_prev_pad = 0xFFFFFFFFu;
   while (g_running)
   {
      SceCtrlData pd;
      unsigned edges;
      char hdr_right[24], title[64], foot[64];

      sceCtrlPeekBufferPositive(&pd, 1);
      edges = pad_edges(pd.Buttons);

      /* L+R+SELECT in the browser: dump the gallery screen as displayed
       * pixels (plain file I/O — works in every build, including the
       * telemetry-stripped playable, which is the only build whose browser
       * ever shows: a harness ini suppresses it by design, ADR-0067). */
      if ((pd.Buttons & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT))
              == (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT))
      {
         static int shot_armed = 1;
         if (shot_armed)
         {
            extern char g_dir_base[];
            char gp[176];
            snprintf(gp, sizeof(gp), "%s/log", g_dir_base);
            sceIoMkdir(gp, 0777);
            snprintf(gp, sizeof(gp), "%s/log/ge_gallery.bmp", g_dir_base);
            if (vid_dump_ge(gp) == 0)
               fe_evt("ge_dump file=ge_gallery.bmp ui=1");
            shot_armed = 0;
         }
      }

      /* Asset shoot: after the box art has streamed in, dump the gallery as
       * displayed pixels, then auto-pick so the run continues unattended
       * into the in-game menu demo. */
      if (g_ui_shots)
      {
         static int settle;
         if (++settle == 150)   /* let the box art fully stream in first */
         {
            extern char g_dir_base[];
            char gp[176];
            snprintf(gp, sizeof(gp), "%s/log/ge_gallery.bmp", g_dir_base);
            if (vid_dump_ge(gp) == 0)
               fe_evt("ge_dump file=ge_gallery.bmp ui=1");
            edges |= PSP_CTRL_CROSS;
         }
      }

      if (edges & (PSP_CTRL_LEFT | PSP_CTRL_UP))
         { cur = (cur + n - 1) % n; idle = 0; }
      if (edges & (PSP_CTRL_RIGHT | PSP_CTRL_DOWN))
         { cur = (cur + 1) % n; idle = 0; }
      if (edges & PSP_CTRL_LTRIGGER)
         { cur = (cur + n - 5) % n; idle = 0; }
      if (edges & PSP_CTRL_RTRIGGER)
         { cur = (cur + 5) % n; idle = 0; }
      if (edges & PSP_CTRL_CROSS)
      {
         snprintf(out, out_sz, "%s/%s", rom_dir, g_roms[cur].name);
         snprintf(g_pcfg.last_rom, sizeof(g_pcfg.last_rom), "%s",
                  g_roms[cur].name);
         pcfg_save();
         fe_evt("ui_browser_pick rom=%s", g_roms[cur].name);
         art_free_all();
         return 0;
      }

      /* Asset shoot: force the idle state so art loads unconditionally —
       * under PPSSPP, host input polling can produce phantom pad edges that
       * reset `idle` every few frames, silently starving the art loader
       * (the placeholder-cards mystery: no failure, no load, no evidence). */
      if (g_ui_shots)
         idle = 100;

      vid_overlay_begin(1);
      snprintf(hdr_right, sizeof(hdr_right), "%d / %d", cur + 1, n);
      page("GBAdhoc  -  SELECT GAME", hdr_right);

      /* Neighbours first (they sit under the selected card's border),
       * template-only while scrolling; the SELECTED card always loads its
       * art.  After ~1/4 s of rest, neighbours fill in one per frame. */
      if (n > 1)
         card_draw(rom_dir, (cur + n - 1) % n, 96, 128, 84, 0,
                   idle > 15);
      if (n > 2)
         card_draw(rom_dir, (cur + 1) % n, 384, 128, 84, 0,
                   idle > 30);
      if (n > 4)
      {
         card_draw(rom_dir, (cur + n - 2) % n, 18, 128, 60, 0, 0);
         card_draw(rom_dir, (cur + 2) % n, 462, 128, 60, 0, 0);
      }
      /* Art loads only after ~4 frames of rest, so holding a direction
       * scrolls smoothly through template cards instead of paying one
       * memory-stick load per step. */
      card_draw(rom_dir, cur, 240, 128, ART_PX, 1, idle >= 4);

      rom_display_name(&g_roms[cur], title, sizeof(title), 0);
      if (strlen(title) > 52)
         title[52] = '\0';
      vid_text_center(206, title, C_SEL);
      if (g_roms[cur].has_sav)
         vid_text_center(226, "save data present", C_DIM);

      snprintf(foot, sizeof(foot),
               "DPAD browse   L/R jump 5   X play%s",
               n > 1 ? "" : "   (1 game)");
      footer(foot);
      vid_overlay_end();
      sceDisplayWaitVblankStart();
      vid_swap();
      if (idle < 1000)
         idle++;
   }
   art_free_all();
   return -1;
}
