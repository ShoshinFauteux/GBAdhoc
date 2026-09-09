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
#include "stb_image.h"
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

/* Backgrounds are FLAT.  A full-screen ramp has only 32 steps of blue to
 * spend at RGB565, so it bands; with the GE dither that used to be on it
 * stippled as well.  Depth comes from translucent surfaces over a solid
 * field instead -- the same call RetroShell PSP makes. */
/* Monochrome by design: greys carry structure, and the ONE saturated
 * thing on screen is the box art.  A coloured chrome competes with it. */
/* GREY MEANS g6 == r5 * 2, ALWAYS.
 *
 * RGB565 gives green SIX bits and red/blue five, so the obvious encoding of a
 * grey -- r=v>>3, g=v>>2, b=v>>3 -- puts green a fraction higher than red and
 * blue once the hardware expands the channels back out.  It is invisible in
 * the midtones and clearly green in the darkest ones: #0E0E0E encoded that way
 * decodes to #080C08, a +4 green cast, and a PSP Go's panel shows it.
 *
 * So the darks are written as exact neutrals (g6 = 2 * r5) rather than as the
 * closest encoding of a hex triple.  Black, dark grey, white accents. */
static const ui_theme THM_DARK = {
   0x0000, 0x0000,           /* pure black field                          */
   0x1082, 0x1082,           /* #101010 header bar   (r5 2, g6 4, b5 2)   */
   0xFFFF, 0x8C51,           /* white accent / #8B8B8B dim accent         */
   0xFFFF, 0xFFFF,           /* header title / selected text: white       */
   0xCE59, 0x6B4D, 0xEF5D,   /* #C9C9C9 item / #6B6B6B dim / #E8E8E8 value*/
   0xFFFF,                   /* warn: white -- the words carry the alarm  */
   0x18C3, 0x0000            /* #181818 card body / black shadow          */
};

static const ui_theme THM_LIGHT = {
   0xF79E, 0xF79E,           /* #F2F2F0 flat off-white                    */
   0xE71C, 0xE71C,           /* #E3E3E1 header bar                        */
   0x0000, 0x6B4D,           /* black accent / #6B6B6B dim accent         */
   0x0000, 0x0000,           /* header title / selected text: black       */
   0x2945, 0x8C71, 0x1082,   /* #2B2B2B item / #8C8C8C dim / #101010 value*/
   0x0000,                   /* warn                                      */
   0xFFFF, 0xAD55            /* white card body / #A8A8A8 shadow (neutral)*/
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
static int g_set_scroll;      /* SETTINGS list scroll, px (see set_row_y) */

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

/* ---- WAKE-FROM-SLEEP OVERLAY ------------------------------------------
 *
 * Drawn over the frame the console was showing when it went to sleep, which
 * the suspend path snapshots before it hands the volatile partition back.
 *
 * The frozen frame is the whole point: it says "your game is exactly where you
 * left it" before the player has read a single word.  So it is drawn at full
 * size and full fidelity, then dimmed -- and the plate is deliberately small,
 * because the more of the frame it hides the less it makes that promise.
 *
 * Modal and self-contained: it owns the pad and the swap chain until it
 * returns.  Nothing emulates while it is up, which is correct -- the link is
 * down, the frame is a still, and there is nothing to keep in real time.
 *
 * Returns 0 to resume, 1 to leave for the game list. */

static void footer(const char *hint);   /* defined with the other chrome */
extern volatile int g_running;          /* main_psp exit flag (HOME) */

int ui_wake_menu(const uint16_t *frame, int frame_w, int frame_h,
                 const char *game)
{
   static const char *lbl[2] = { "Continue", "Quit to game list" };
   const int LX = 20;                 /* wordmark + title margin */
   int sel = 0, first = 1;
   unsigned prev = 0;
   char title[96];

   g_thm = (g_pcfg.theme == 1) ? &THM_LIGHT : &THM_DARK;

   /* The filename is not the game's name.  Drop the extension; the region
    * tag stays because two dumps of the same game differ by exactly that. */
   {
      const char *dot;
      snprintf(title, sizeof(title), "%s", game ? game : "");
      dot = strrchr(title, '.');
      if (dot && (dot - title) > 0)
         title[dot - title] = 0;
   }

   /* MUST loop on g_running, not for(;;).
    *
    * HOME is the first thing a lot of people will press on a console they
    * picked up days later, and it fires the exit callback, which sets
    * g_running = 0 and then WAITS for the app to leave.  A modal loop that
    * never looks at the flag leaves the kernel at "Please wait..." forever,
    * with a hard reset as the only way out.  Every other modal screen in this
    * file already loops on g_running; this one did not.
    *
    * Falling out returns 0 (resume), which is the harmless answer: the main
    * loop's own `while (g_running)` is false by then, so it goes straight to
    * the normal shutdown -- SRAM flushed, session torn down, ExitGame. */
   while (g_running)
   {
      SceCtrlData pad;
      unsigned now, edges;
      int i;

      sceCtrlPeekBufferPositive(&pad, 1);
      now   = pad.Buttons;
      edges = now & ~prev;
      prev  = now;
      /* Swallow whatever was held as the console woke: the player slid a
       * switch, they have not chosen anything yet. */
      if (first)
         { edges = 0; first = 0; }

      if (edges & (PSP_CTRL_UP | PSP_CTRL_DOWN))
         sel ^= 1;
      if (edges & PSP_CTRL_CROSS)
         return sel;
      if (edges & PSP_CTRL_CIRCLE)
         return 0;              /* O is always "back to what I was doing" */

      vid_overlay_begin(1);

      /* 1. THE GAME, AS IT WAS.  This is the whole idea -- it says "you are
       *    exactly where you left off" before a word has been read. */
      if (frame)
         vid_image_screen(frame, 256, 256, frame_w, frame_h, 255);
      /* 2. Hold it back far enough for type to read over any scene, bright
       *    or dark.  No panel: the picture IS the backdrop, and a plate over
       *    it only hides the thing worth showing. */
      vid_rect(0, 0, VID_SCR_W, VID_SCR_H, C_BG_BOT, frame ? 170 : 240);
      /*    ...then a ramp climbing out of the bottom edge, so the choices and
       *    the footer sit on something solid while the top of the frame stays
       *    as visible as the scrim allows.  One strip, interpolated per pixel
       *    -- a stack of rects bands badly at 565 (see vid_gradient_a). */
      vid_gradient_a(0, VID_SCR_H - 120, VID_SCR_W, 120, C_BG_BOT, 0, 165);

      /* 3. Identity, hard into the top-left corner. */
      vid_logo(LX, 18, C_TITLE, 255);
      if (title[0])
         vid_text(LX + 2, 48, title, C_DIM);

      /* 4. The choices, same left margin, same selection language as the
       *    browser: an accent edge bar and a soft fill, never a slab. */
      /*    Flush to the screen's left edge: the selection bleeds off it
       *    rather than floating, which is what stops this reading as a dialog
       *    box without one being drawn. */
      for (i = 0; i < 2; i++)
      {
         int ry = 168 + i * 32;
         int w  = vid_text_w(lbl[i]) + 40;
         if (i == sel)
         {
            vid_rect(0, ry - 6, w, FE_FONT_H + 12, C_ACCENT, 40);
            vid_rect(0, ry - 6, 4, FE_FONT_H + 12, C_ACCENT, 255);
         }
         vid_text(18, ry, lbl[i], i == sel ? C_SEL : C_ITEM);
      }

      /* 5. Hints on the screen's own bottom edge, in the same footer the
       *    rest of the UI uses -- not floating under a box. */
      footer("X select     O resume     DPAD change");

      vid_overlay_end();
      sceDisplayWaitVblankStart();
      vid_swap();
   }
   return 0;      /* HOME: let the main loop run its clean shutdown */
}

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
   {
      g_profile_at_open = g_pcfg.me_mode;
      g_set_scroll = 0;
   }
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
      vid_rect(0, 0, VID_SCR_W, VID_SCR_H, C_BG_TOP, 255);
   vid_rect(0, 0, VID_SCR_W, HDR_H, C_HDR_TOP, 255);
   vid_rect(0, HDR_H, VID_SCR_W, 1, C_ACCENT, 255);
   vid_text_hd(14, (HDR_H - FE_FONT_H) / 2 - 2, title, C_TITLE);
   if (right && right[0])
      vid_text(VID_SCR_W - 14 - vid_text_w(right),
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
      vid_text(x + w - 10 - vid_text_w(value), y, value,
               enabled ? (selected ? C_VALUE : C_ACCENT_DK) : C_DIM);
}

/* ----- settings screen ---------------------------------------------------- */

/* Sectioned.  Trading profile stays FIRST (ADR-0071: it is the one setting
 * that decides whether a trade completes, and the only one that costs a
 * restart).  Header rows are labels, not stops — the cursor skips them. */
/* SET_PROFILE (Media Engine mode) was removed 2026-09-08: the single-core
 * pipeline is deprecated.  It has nowhere near the headroom to be useful --
 * the ME renderer is what makes full speed possible -- and offering it as a
 * choice only let someone end up in the slow path by accident.
 *
 * THE CODE PATH STAYS.  me_rend_teardown() falls back to CPU rendering from
 * six places (watchdog, input wedge, late frame, np_start, exit, failed ME
 * init), it is the reference the me_capture_mode=2 validation oracle compares
 * against, and it is the only renderer that exists under PPSSPP, which has no
 * Media Engine.  `me_mode` also survives as a config.ini key so a bug report
 * can still be bisected without a special build.  Only the menu row is gone. */
enum { SET_HDR_WL, SET_ROOM, SET_OSD,
       SET_HDR_VID, SET_SCALE, SET_FILTER, SET_THEME, SET_SHELL,
       SET_HDR_GAME, SET_FFMULT, SET_FFMODE, SET_ABMAP, SET_FPS,
       SET_COUNT };

static const struct { unsigned char header; const char *label; } set_rows[SET_COUNT] = {
   { 1, "WIRELESS" },
   { 0, "Room code" },
   { 0, "Session overlay" },
   { 1, "VIDEO" },
   { 0, "Video scale" },
   { 0, "Video filter" },
   { 0, "Theme" },
   { 0, "Menu style" },
   { 1, "GAMEPLAY" },
   { 0, "Fast-forward" },
   { 0, "FF button (Square)" },
   { 0, "A/B buttons" },
   { 0, "FPS counter" },
};

/* The list outgrew the page when "Menu style" landed.  The pitch has
 * already been shaved twice (see below), so instead the page scrolls:
 * rows keep their absolute layout and the whole column is shifted by
 * one offset, which keeps each section header travelling with its
 * items. */
#define SET_VIEW_TOP  (HDR_H + 4)
#define SET_VIEW_BOT  244

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

/* Follow the cursor, and pull the section header in with the first item
 * under it -- an item that scrolls in headerless reads as orphaned. */
static void set_scroll_follow(void)
{
   int top = set_row_y(g_cursor);
   int bot = top + FE_FONT_H;
   int max = set_row_y(SET_COUNT - 1) + FE_FONT_H - SET_VIEW_BOT;

   if (g_cursor > 0 && set_rows[g_cursor - 1].header)
      top = set_row_y(g_cursor - 1);
   if (max < 0)
      max = 0;
   if (top - g_set_scroll < SET_VIEW_TOP)
      g_set_scroll = top - SET_VIEW_TOP;
   if (bot - g_set_scroll > SET_VIEW_BOT)
      g_set_scroll = bot - SET_VIEW_BOT;
   if (g_set_scroll > max)
      g_set_scroll = max;
   if (g_set_scroll < 0)
      g_set_scroll = 0;
}

static int g_profile_changed;

/* me_mode_name() was here.  It rendered the Media Engine row's value and
 * went with the row; the mode itself is still read from config.ini and still
 * falls back automatically.  See the SET_PROFILE note above the enum. */

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
   case SET_SHELL:
      g_pcfg.ui_shell = !g_pcfg.ui_shell;
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
      g_cursor = SET_ROOM;
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
   set_scroll_follow();
   for (i = 0; i < SET_COUNT; i++)
   {
      int y = set_row_y(i) - g_set_scroll;
      if (y < SET_VIEW_TOP - 2 || y > SET_VIEW_BOT)
         continue;
      if (set_rows[i].header)
      {
         int lx = 24 + vid_text_w(set_rows[i].label) + FE_FONT_W;
         vid_text(24, y, set_rows[i].label, C_ACCENT);
         vid_rect(lx, y + FE_FONT_H / 2, 456 - lx, 1, C_ACCENT_DK, 120);
         continue;
      }
      switch (i)
      {
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
      case SET_SHELL:
         row(36, y, 408, g_cursor == i, 1, set_rows[i].label,
             g_pcfg.ui_shell ? "marquee" : "shelf");
         break;
      case SET_FPS:
         row(36, y, 408, g_cursor == i, 1, set_rows[i].label,
             g_pcfg.show_fps ? "on" : "off");
         break;
      }
   }
   /* The relaunch-to-apply footer went with the Media Engine row: nothing
    * sets g_profile_changed any more, so the branch could never be taken and
    * the text advertised a setting that no longer exists.  The mechanism
    * itself stays for the next setting that needs a reboot. */
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

typedef struct { char name[96]; int has_sav; unsigned size; } rom_entry;
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
         /* dread already carried the stat -- no second I/O per ROM. */
         g_roms[n].size = (unsigned)ent.d_stat.st_size;
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

/* ---- box art -------------------------------------------------------------
 *
 * Box art is PORTRAIT (a GBA box is roughly 5:7), and it used to be squashed
 * into a 112-square and then, in the marquee shell, stretched 4.3x to fill
 * the screen.  Both are gone: art keeps its aspect, and there are two tiers
 * so neither view is fed a texture built for the other.
 *
 *   THUMB  128x176 inside a 128x256 texture (GE demands power-of-two dims),
 *          64 KiB a slot.  Ten slots is a screenful plus what you are about
 *          to scroll onto.
 *   HERO   256x358 inside a 256x512 texture, 256 KiB, exactly one of them,
 *          rebuilt when the selection settles.  The marquee draws it at
 *          480 wide -- a 1.9x stretch instead of 4.3x.
 *
 * Both are freed when the browser exits, so this pool costs the running
 * emulator nothing.
 *
 * SOURCE FILES: `<appdir>/boxart/<rom-name-minus-.gba>.<ext>`, tried as .png
 * then .jpg then .bmp.  PNG/JPEG go through stb_image (whole-file decode);
 * BMP keeps the old row-streaming reader, which needs no buffer at all.
 * Feed it the highest-resolution scan you have: the resampler below is a BOX
 * FILTER, so a 1000px source genuinely looks better than a 200px one.  Point
 * sampling a big scan down to 176px aliases so badly it looks WORSE than a
 * small source, which is the trap the previous version fell into.
 */
#define ART_TEX_W   128
#define ART_TEX_H   256
#define ART_W       128
#define ART_H       176
/* HERO TEXTURE SIZE IS CHOSEN AT RUNTIME.
 *
 * The GE samples power-of-two textures only, and the PSP screen is 480x272 --
 * not a power of two in either axis.  A 512x512 texture is the smallest that
 * holds a full-screen image, so pre-composed art lands in it at NATIVE size
 * and is drawn 1:1: no downsample, no upscale, exactly the pixels the artist
 * made.  It costs 512 KiB.
 *
 * A 256x256 texture costs 128 KiB but forces 480 -> 256 -> 480, which is two
 * lossy resamples and visibly soft.  It exists only as the fallback.
 *
 * WHICH ONE IS NOT DECIDED BY CONSOLE MODEL.  A PSP-1000 has half the RAM of
 * a 2000/3000/Go, so model would be a fair proxy -- but only a proxy.  What
 * actually matters is whether half a megabyte is available CONTIGUOUSLY at
 * this moment, and a field log has shown max_block as low as 196 KiB on a
 * 64 MB console.  So ask the allocator, which answers the real question and
 * also covers the fragmented case model detection would miss. */
#define HERO_TEX_BIG    512
#define HERO_TEX_SMALL  256
#define HERO_W      256
#define HERO_H      358
#define ART_SLOTS   10

/* THE 32 MB CONSOLE IS THE ONE THAT MATTERS.  A PSP-1000 has half the RAM of
 * a 2000/3000, and this pool is the largest thing the frontend allocates
 * outside the core.  Ten thumbs is 640 KiB, the hero another 256 KiB, and a
 * PNG decode transiently wants the file plus w*h*3 on top -- comfortably over
 * a megabyte at the worst moment.  A field log showed max_block=200704 while
 * the hero alone asks for 256 KiB contiguous, so "there is plenty of RAM" was
 * never true; it was only ever true on the 64 MB unit this was tested on.
 *
 * So every allocation here is CONDITIONAL on the console being able to spare
 * it, and every failure degrades to no-art rather than to a wedged browser. */
/* NO PRE-FLIGHT BUDGET CHECK.  There was one, and it broke all box art.
 *
 * It asked sceKernelTotalFreeMemSize()/MaxFreeMemSize() whether an allocation
 * would fit.  Those report the KERNEL PARTITION, but main_psp.c declares
 * PSP_HEAP_SIZE_KB(-1024) -- "give the heap everything except 1 MB" -- so
 * malloc draws from a pool those functions do not describe.  They returned a
 * constant ~750 KB no matter how much heap was free, and a 2 MB floor on top
 * of that denied every request, down to a 64 KB thumbnail.  Observed live:
 * "art_budget deny want=65536 block=524288 free=765952", repeated for every
 * cover in the library.
 *
 * The allocation IS the test.  memalign returns NULL when it cannot be
 * satisfied, every caller here already handles NULL by degrading to no art,
 * and that path costs nothing when memory is plentiful. */

/* Decode ceiling.  A 2000x2800 scan is 16 MB decoded, which the browser can
 * afford but nothing is gained by it; stb is asked to fail beyond this so a
 * pathological file cannot take the frontend down with it. */
#define ART_SRC_MAX_PX (1600 * 1600)

/* aw/ah is the sub-rect actually filled.  Box-art scans are not all the
 * same shape -- libretro boxarts are square, a real scan is 5:7 -- so the
 * image is fitted INSIDE the panel at its own aspect and the caller is
 * told what it got.  Stretching every cover to one frame is what makes a
 * gallery look wrong without the eye being able to say why. */
typedef struct { int rom_idx; int state; int aw, ah; uint16_t *tex; } art_slot;
static art_slot g_art[ART_SLOTS];
static int g_art_clock;
static uint16_t *g_hero;
static int g_hero_idx = -1;
static int g_hero_state;
static int g_hero_tex;          /* edge of the allocated texture, 512 or 256 */
static int g_hero_w, g_hero_h;
/* 1 when the backdrop came from hero/ -- artwork a person composed FOR this
 * shell, already dark and already quiet on the left.  The heavy scrim exists
 * to beat raw box art into something text can sit on; applying it to art that
 * has already been treated crushes it to black. */
static int g_hero_precomposed;

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
   free(g_hero);
   g_hero = NULL;
   g_hero_idx = -1;
   g_hero_state = 0;
   g_hero_tex = 0;
}

/* Box-average `src` (w x h, `nc` bytes per pixel, R first) into an RGB565
 * dw x dh image at the top-left of a `stride`-wide texture.  Averaging is
 * what makes a high-resolution scan pay off; nearest sampling throws away
 * 95% of the pixels and keeps whichever ones happen to land on the grid. */
static void art_resample(const unsigned char *src, int w, int h, int nc,
                         uint16_t *dst, int stride, int dw, int dh)
{
   int dy;
   for (dy = 0; dy < dh; dy++)
   {
      int y0 = dy * h / dh, y1 = (dy + 1) * h / dh;
      int dx;
      if (y1 <= y0) y1 = y0 + 1;
      for (dx = 0; dx < dw; dx++)
      {
         int x0 = dx * w / dw, x1 = (dx + 1) * w / dw;
         unsigned r = 0, g = 0, b = 0, n = 0;
         int sx, sy;
         if (x1 <= x0) x1 = x0 + 1;
         for (sy = y0; sy < y1; sy++)
         {
            const unsigned char *row = src + (size_t)sy * w * nc;
            for (sx = x0; sx < x1; sx++)
            {
               const unsigned char *p = row + (size_t)sx * nc;
               r += p[0]; g += p[1]; b += p[2];
               n++;
            }
         }
         if (!n) n = 1;
         r /= n; g /= n; b /= n;
         dst[dy * stride + dx] = (uint16_t)((r >> 3) | ((g >> 2) << 5) |
                                            ((b >> 3) << 11));
      }
   }
}

/* Read a whole file into a malloc'd buffer. */
static unsigned char *art_slurp(const char *path, int *out_len)
{
   SceUID f = sceIoOpen(path, PSP_O_RDONLY, 0);
   int len, got;
   unsigned char *buf;

   if (f < 0)
      return NULL;
   len = sceIoLseek32(f, 0, PSP_SEEK_END);
   sceIoLseek32(f, 0, PSP_SEEK_SET);
   if (len <= 0 || len > 8 * 1024 * 1024)
   {
      sceIoClose(f);
      return NULL;
   }
   buf = (unsigned char *)malloc((size_t)len);
   if (!buf)
   {
      sceIoClose(f);
      return NULL;
   }
   got = sceIoRead(f, buf, len);
   sceIoClose(f);
   if (got != len)
   {
      free(buf);
      return NULL;
   }
   *out_len = len;
   return buf;
}

/* Largest w x h inside the box that keeps the source aspect. */
static void art_fit(int sw, int sh, int boxw, int boxh, int *ow, int *oh)
{
   int w = boxw, h = (int)((long)boxw * sh / (sw ? sw : 1));
   if (h > boxh)
   {
      h = boxh;
      w = (int)((long)boxh * sw / (sh ? sh : 1));
   }
   *ow = w < 1 ? 1 : w;
   *oh = h < 1 ? 1 : h;
}

/* PNG/JPEG through stb_image.  Returns 0 on success and reports the fitted
 * size back through dw and dh, at most the box passed in. */
static int art_load_stb(const char *path, uint16_t *tex, int stride,
                        int *dw, int *dh)
{
   int flen = 0, w = 0, h = 0, nc = 0, fw, fh;
   unsigned char *file = art_slurp(path, &flen);
   unsigned char *pix;

   if (!file)
      return -1;
   if (!stbi_info_from_memory(file, flen, &w, &h, &nc) ||
       w <= 0 || h <= 0 || (long)w * h > ART_SRC_MAX_PX)
   {
      fe_evt("art_fail stage=info path=%s w=%d h=%d", path, w, h);
      free(file);
      return -1;
   }
   pix = stbi_load_from_memory(file, flen, &w, &h, &nc, 3);
   free(file);
   if (!pix)
   {
      fe_evt("art_fail stage=decode path=%s", path);
      return -1;
   }
   art_fit(w, h, *dw, *dh, &fw, &fh);
   art_resample(pix, w, h, 3, tex, stride, fw, fh);
   stbi_image_free(pix);
   fe_evt("art_load path=%s src=%dx%d -> %dx%d", path, w, h, fw, fh);
   *dw = fw;
   *dh = fh;
   return 0;
}

/* Uncompressed BMP, streamed a row at a time so a big cover never needs a
 * whole-file buffer.  Kept because it costs nothing and a card full of BMPs
 * from an older install must keep working.  Point-sampled: a BMP big enough
 * to alias is a BMP that should have been a PNG. */
static int art_load_bmp(const char *path, uint16_t *tex, int stride,
                        int *pdw, int *pdh)
{
   int dw = *pdw, dh = *pdh;
   unsigned char hdr[54];
   unsigned char rowbuf[8192];
   SceUID f = sceIoOpen(path, PSP_O_RDONLY, 0);
   int w, h, bpp, comp, topdown, dy;
   unsigned off, rowbytes;

   if (f < 0)
      return -1;
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
   if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || comp != 0 ||
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
   art_fit(w, h, dw, dh, &dw, &dh);
   *pdw = dw;
   *pdh = dh;

   for (dy = 0; dy < dh; dy++)
   {
      int sy = dy * h / dh;
      int fy = topdown ? sy : (h - 1 - sy);
      int dx;
      uint16_t *dst = &tex[dy * stride];
      if (sceIoLseek32(f, (int)(off + (unsigned)fy * rowbytes),
                       PSP_SEEK_SET) < 0 ||
          sceIoRead(f, rowbuf, rowbytes) != (int)rowbytes)
      {
         fe_evt("art_fail stage=read path=%s dy=%d fy=%d off=%u", path, dy,
                fy, off);
         sceIoClose(f);
         return -1;
      }
      for (dx = 0; dx < dw; dx++)
      {
         const unsigned char *p = &rowbuf[(dx * w / dw) * (bpp / 8)];
         /* BMP is B,G,R[,A]; GE 5650 texel is PSP channel order (R low). */
         dst[dx] = (uint16_t)((p[2] >> 3) | ((p[1] >> 2) << 5) |
                              ((p[0] >> 3) << 11));
      }
   }
   sceIoClose(f);
   return 0;
}

/* ---- .565: the texture, already made ---------------------------------
 *
 * The GE samples RGB565.  A PNG costs an inflate, a per-byte unfilter pass,
 * an 888->565 conversion over 130k pixels and then a copy -- to produce bytes
 * that could simply have been stored.  A .565 file IS the texture, laid out
 * at the texture stride, so loading it is ONE sceIoRead into the destination.
 *
 * No fidelity is lost: the texture is 16-bit whichever route the pixels take.
 * The conversion merely happens on a PC instead, where it can be dithered --
 * which the truncation below cannot -- so gradients come out cleaner.
 *
 * Header is 16 bytes, so the pixel data stays 16-byte aligned for the read.
 * See tools/heroforge/raw565.py for the writer. */
#define R565_MAGIC  0x52353635u        /* '565R' little-endian */
#define R565_HDR    16

static int art_load_565(const char *path, uint16_t *tex, int stride,
                        int *dw, int *dh)
{
   unsigned char hdr[R565_HDR];
   SceUID f = sceIoOpen(path, PSP_O_RDONLY, 0);
   unsigned magic, fstride;
   int w, h, y, got;

   if (f < 0)
      return -1;
   if (sceIoRead(f, hdr, R565_HDR) != R565_HDR)
   {
      sceIoClose(f);
      return -1;
   }
   magic   = (unsigned)hdr[0] | ((unsigned)hdr[1] << 8) |
             ((unsigned)hdr[2] << 16) | ((unsigned)hdr[3] << 24);
   w       = hdr[4] | (hdr[5] << 8);
   h       = hdr[6] | (hdr[7] << 8);
   fstride = hdr[8] | (hdr[9] << 8);
   if (magic != R565_MAGIC || w <= 0 || h <= 0 ||
       w > stride || h > stride || fstride == 0)
   {
      fe_evt("art_fail stage=565hdr path=%s w=%d h=%d stride=%u",
             path, w, h, fstride);
      sceIoClose(f);
      return -1;
   }

   if ((int)fstride == stride)
   {
      /* The file matches the texture: one read, no repacking at all. */
      got = sceIoRead(f, tex, stride * h * 2);
      if (got != stride * h * 2)
      {
         fe_evt("art_fail stage=565read path=%s got=%d", path, got);
         sceIoClose(f);
         return -1;
      }
   }
   else
   {
      for (y = 0; y < h; y++)
         if (sceIoRead(f, tex + (size_t)y * stride, fstride * 2)
             != (int)fstride * 2)
         {
            sceIoClose(f);
            return -1;
         }
   }
   sceIoClose(f);
   *dw = w;
   *dh = h;
   return 0;
}

/* Write what we just decoded, so the next visit is a single read.  Best
 * effort: a full or read-only card simply means we decode again next time,
 * which is exactly today's behaviour and not worth reporting as a failure. */
static void art_cache_565(const char *path, const uint16_t *tex, int stride,
                          int w, int h)
{
   unsigned char hdr[R565_HDR];
   SceUID f;
   int y;

   if (w <= 0 || h <= 0)
      return;
   f = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (f < 0)
      return;
   memset(hdr, 0, sizeof(hdr));
   hdr[0] = '5'; hdr[1] = '6'; hdr[2] = '5'; hdr[3] = 'R';
   hdr[4] = (unsigned char)(w & 0xFF);  hdr[5] = (unsigned char)(w >> 8);
   hdr[6] = (unsigned char)(h & 0xFF);  hdr[7] = (unsigned char)(h >> 8);
   hdr[8] = (unsigned char)(stride & 0xFF);
   hdr[9] = (unsigned char)(stride >> 8);
   if (sceIoWrite(f, hdr, R565_HDR) == R565_HDR)
      for (y = 0; y < h; y++)
         if (sceIoWrite(f, tex + (size_t)y * stride, stride * 2) != stride * 2)
            break;
   sceIoClose(f);
   fe_evt("art_cached path=%s %dx%d", path, w, h);
}

/* Try png, jpg, bmp in that order.  Returns 0 on success;
 * dw and dh come in as the box to fit and come back as the sub-rect
 * actually written. */
static int art_load_any(int rom_idx, const char *dir, uint16_t *tex,
                        int stride, int *dw, int *dh)
{
   extern char g_dir_base[];
   /* .565 first: it is the texture itself and costs one read. */
   static const char *ext[] = { "565", "png", "jpg", "jpeg", "bmp" };
   size_t l = strlen(g_roms[rom_idx].name);
   int stem = (int)(l > 4 ? l - 4 : l);
   unsigned e;

   int boxw = *dw, boxh = *dh;
   unsigned t0 = (unsigned)sceKernelGetSystemTimeLow();

   for (e = 0; e < sizeof(ext) / sizeof(ext[0]); e++)
   {
      char path[300];
      SceIoStat st;
      /* A failed attempt may have shrunk the box; every candidate gets
       * the same frame to fit into. */
      *dw = boxw; *dh = boxh;
      snprintf(path, sizeof(path), "%s/%s/%.*s.%s", g_dir_base, dir, stem,
               g_roms[rom_idx].name, ext[e]);
      if (sceIoGetstat(path, &st) < 0)
         continue;
      if (ext[e][0] == '5')
      {
         if (art_load_565(path, tex, stride, dw, dh) == 0)
         {
            fe_evt("art_time fmt=565 us=%u %s",
                   (unsigned)sceKernelGetSystemTimeLow() - t0, path);
            return 0;
         }
      }
      else if (ext[e][0] == 'b')
      {
         if (art_load_bmp(path, tex, stride, dw, dh) == 0)
         {
            fe_evt("art_time fmt=bmp us=%u %s",
                   (unsigned)sceKernelGetSystemTimeLow() - t0, path);
            return 0;
         }
      }
      else if (art_load_stb(path, tex, stride, dw, dh) == 0)
      {
         /* Decoded the slow way -- leave a .565 behind so this title is a
          * single read from now on.  Lazy, one title at a time, only for
          * something the user actually looked at: there is deliberately no
          * batch pass, because converting a 100-ROM library at boot would
          * make the emulator look hung on first run. */
         char cpath[300];
         snprintf(cpath, sizeof(cpath), "%s/%s/%.*s.565", g_dir_base, dir,
                  stem, g_roms[rom_idx].name);
         fe_evt("art_time fmt=png us=%u %s",
                (unsigned)sceKernelGetSystemTimeLow() - t0, path);
         art_cache_565(cpath, tex, stride, *dw, *dh);
         return 0;
      }
   }
   return -1;
}

/* Cache lookup; loads on miss (`load` != 0).  Returns the texture or NULL.
 * A negative result is cached too -- retrying a missing file every frame
 * would be 60 directory misses a second on a memory stick that ADR-0069
 * priced at 12.4 ms per negative lookup. */
static const uint16_t *art_get(const char *rom_dir, int rom_idx, int load,
                               art_slot *out)
{
   int i, victim;
   (void)rom_dir;

   for (i = 0; i < ART_SLOTS; i++)
      if (g_art[i].state && g_art[i].rom_idx == rom_idx)
      {
         if (out)
            *out = g_art[i];
         return g_art[i].state > 0 ? g_art[i].tex : NULL;
      }
   if (!load)
      return NULL;

   victim = g_art_clock;
   g_art_clock = (g_art_clock + 1) % ART_SLOTS;
   if (!g_art[victim].tex)
   {
      g_art[victim].tex = (uint16_t *)memalign(16, ART_TEX_W * ART_TEX_H * 2);
      if (!g_art[victim].tex)
         return NULL;
   }
   memset(g_art[victim].tex, 0, ART_TEX_W * ART_TEX_H * 2);
   g_art[victim].rom_idx = rom_idx;
   g_art[victim].aw = ART_W;
   g_art[victim].ah = ART_H;
   if (art_load_any(rom_idx, "boxart", g_art[victim].tex, ART_TEX_W,
                    &g_art[victim].aw, &g_art[victim].ah) == 0)
   {
      sceKernelDcacheWritebackRange(g_art[victim].tex,
                                    ART_TEX_W * ART_TEX_H * 2);
      g_art[victim].state = 1;
      if (out)
         *out = g_art[victim];
      return g_art[victim].tex;
   }
   g_art[victim].state = -1;
   return NULL;
}

/* The marquee's full-bleed background.  One texture, rebuilt only when the
 * selection changes AND the stick has been idle -- a hero decode is the most
 * expensive thing the browser does, and doing it mid-scroll would stutter
 * exactly when the user is moving. */
static const uint16_t *hero_get(int rom_idx, int load)
{
   if (g_hero_idx == rom_idx && g_hero_state)
      return g_hero_state > 0 ? g_hero : NULL;
   if (!load)
      return g_hero_state > 0 && g_hero_idx == rom_idx ? g_hero : NULL;

   if (!g_hero)
   {
      /* Native first, half size second, no backdrop last.  Every step down is
       * a worse picture but never an unstable one -- the shell reads fine
       * with no hero at all. */
      static const int tries[] = { HERO_TEX_BIG, HERO_TEX_SMALL };
      unsigned t;
      for (t = 0; t < sizeof(tries) / sizeof(tries[0]); t++)
      {
         unsigned bytes = (unsigned)tries[t] * tries[t] * 2;
         g_hero = (uint16_t *)memalign(16, bytes);
         if (g_hero)
         {
            g_hero_tex = tries[t];
            break;
         }
      }
      if (!g_hero)
      {
         g_hero_state = -1;
         g_hero_idx = rom_idx;
         return NULL;
      }
      fe_evt("hero_tex edge=%d bytes=%u", g_hero_tex,
             (unsigned)g_hero_tex * g_hero_tex * 2);
   }
   memset(g_hero, 0, (size_t)g_hero_tex * g_hero_tex * 2);
   g_hero_idx = rom_idx;
   g_hero_precomposed = 0;

   /* hero/<rom>.png first: purpose-made 16:9 art.  It is fitted to the FULL
    * texture rather than the 5:7 box the cover path uses, because it is
    * already the right shape -- passing it the portrait box would letterbox
    * a widescreen image inside a widescreen frame. */
   /* At 512 this is the screen itself, so the draw below is 1:1. */
   g_hero_w = (g_hero_tex >= 480) ? 480 : g_hero_tex;
   g_hero_h = (g_hero_w * 272) / 480;
   if (art_load_any(rom_idx, "hero", g_hero, g_hero_tex,
                    &g_hero_w, &g_hero_h) == 0)
   {
      sceKernelDcacheWritebackRange(g_hero,
                                    (size_t)g_hero_tex * g_hero_tex * 2);
      g_hero_state = 1;
      g_hero_precomposed = 1;
      return g_hero;
   }

   /* Otherwise derive one from the cover, as before. */
   g_hero_w = HERO_W;
   g_hero_h = HERO_H;
   if (art_load_any(rom_idx, "boxart", g_hero, g_hero_tex,
                    &g_hero_w, &g_hero_h) == 0)
   {
      sceKernelDcacheWritebackRange(g_hero,
                                    (size_t)g_hero_tex * g_hero_tex * 2);
      g_hero_state = 1;
      return g_hero;
   }
   g_hero_state = -1;
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
   /* No-Intro writes "Legend of Zelda, The".  Restoring the article to the
    * front costs nothing, reads properly, and matters most on exactly the
    * titles long enough to be truncated. */
   {
      char *c = strstr(out, ", The");
      if (c && (c[5] == '\0' || c[5] == ' ' || c[5] == '-'))
      {
         char tmp[96];
         size_t head = (size_t)(c - out);
         snprintf(tmp, sizeof(tmp), "The %.*s%s", (int)head, out, c + 5);
         snprintf(out, sz, "%s", tmp);
      }
   }
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
static void card_template(int x, int y, int w, int h, const rom_entry *r)
{
   char title[64];
   int max_cols = (w - 16) / (FE_FONT_W - 1);
   int max_rows = (h - 40) / (FE_FONT_H + 1);
   int line = 0;
   const char *p;

   vid_rect(x, y, w, h, C_CARD, 255);
   vid_rect(x, y, w, 4, C_ACCENT, 255);
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
   vid_text(x + w - 8 - vid_text_w("GBA"), y + h - FE_FONT_H - 6,
            "GBA", C_ACCENT_DK);
}

/* ============================ SHELLS =====================================
 *
 * SHELL A ("Shelf")   -- default. Vertical list left, one art panel right.
 * SHELL B ("Marquee") -- the selected art fills the screen, dimmed, with the
 *                        list floating over it.
 *
 * Both draw with the primitives the old coverflow already used: solid rects,
 * text and one vid_image per art. No new engine, no scaler.  `ui_shell` in
 * config.ini picks between them and needs a restart, matching how Media
 * Engine mode is toggled.
 *
 * Why vertical: the selection sits at a FIXED row and the list slides
 * underneath it, so the eye never chases the cursor. The old coverflow moved
 * five cards at once and drew four of them only to throw a dim veil over
 * them -- four art fetches per frame spent on decoration, which is the other
 * half of why art was perpetually loading.
 */
static int g_ui_shell;                    /* 0 = Shelf, 1 = Marquee */

/* ---- motion -------------------------------------------------------------
 *
 * The list used to snap: rows were positioned straight from the index, so a
 * d-pad press teleported the whole column by one row height.  Now `g_scroll`
 * chases the selection and the rows are drawn from IT, so the list slides.
 *
 * Fixed point, 8 fractional bits.  There is no FPU cost worth avoiding here
 * -- the browser draws a dozen primitives at 60 Hz -- but integer maths keeps
 * the row positions exactly reproducible, which matters because the harness
 * screenshots the gallery and compares it.
 *
 * ~120 ms to settle.  Faster reads as a snap with a smear on it; slower
 * fights you when the d-pad is held down, because the list is still moving
 * when the next repeat arrives.  Exponential rather than linear so it leaves
 * immediately (responsive) and arrives gently (finished).
 *
 * WRAPPING IS THE TRAP.  The list is a carousel: going from the last entry to
 * the first changes `cur` by -(n-1), and easing across that gap sends the
 * column flying through the entire library.  Any step of more than half the
 * list is a wrap, and a wrap is the one case that must NOT animate. */
#define SCROLL_FP     8                     /* fractional bits            */
#define SCROLL_ONE    (1 << SCROLL_FP)
#define SCROLL_DUR    8                     /* frames per move -> ~133 ms */

static int g_scroll;                        /* current, in FP rows        */
static int g_scroll_from, g_scroll_to;      /* tween endpoints            */
static int g_scroll_t;                      /* frames elapsed, 0..DUR     */
static int g_scroll_cur = -1;               /* selection g_scroll_to is for */

/* Smoothstep, u and result both 0..256.  u*u*(3-2u) in fixed point. */
static int smoothstep(int u)
{
   return (((u * u) >> SCROLL_FP) * (3 * SCROLL_ONE - 2 * u)) >> SCROLL_FP;
}

/* A FIXED-DURATION TWEEN, NOT EXPONENTIAL DECAY.
 *
 * The first version chased the target by a fraction of the remaining
 * distance each frame.  That is the textbook approach and it feels wrong,
 * because the pixel steps it produces on a 25 px row are
 *     10, 6, 4, 2, 1, 1, 1, 0, 0, 0
 * -- a lurch, then a crawl, then several frames where nothing moves at all.
 * Reported as "a bit stuttery", and the numbers agree.
 *
 * Smoothstep over a fixed 8 frames gives
 *      1, 2, 4, 5, 5, 4, 2, 2
 * which accelerates and decelerates symmetrically and never stalls.
 *
 * Re-targeting mid-tween starts a fresh tween FROM THE CURRENT POSITION, so
 * holding the d-pad keeps the column gliding instead of restarting it. */
static void scroll_track(int cur, int n)
{
   if (g_scroll_cur < 0)                    /* first frame: no animation  */
   {
      g_scroll_cur = cur;
      g_scroll = g_scroll_from = g_scroll_to = cur * SCROLL_ONE;
      g_scroll_t = SCROLL_DUR;
      return;
   }
   if (cur != g_scroll_cur)
   {
      int step = cur - g_scroll_cur;
      g_scroll_cur = cur;
      g_scroll_from = g_scroll;             /* glide on from here         */
      g_scroll_to   = cur * SCROLL_ONE;
      g_scroll_t    = 0;
      /* A wrap is a jump of more than half the library; snap through it
       * rather than scrolling the whole way round. */
      if (n > 2 && (step > n / 2 || step < -(n / 2)))
      {
         g_scroll = g_scroll_from = g_scroll_to;
         g_scroll_t = SCROLL_DUR;
      }
   }
   if (g_scroll_t < SCROLL_DUR)
   {
      int e;
      g_scroll_t++;
      e = smoothstep((g_scroll_t * SCROLL_ONE) / SCROLL_DUR);
      g_scroll = g_scroll_from +
                 (((g_scroll_to - g_scroll_from) * e) >> SCROLL_FP);
   }
   else
      g_scroll = g_scroll_to;
}

/* True once the tween has finished.  DECODING IS GATED ON THIS.
 *
 * Nothing in this browser can decode a PNG inside a frame budget -- a 512
 * square cover takes hundreds of milliseconds on Allegrex -- so a decode
 * started while the list is moving stops the list dead until it finishes.
 * The original gate was `idle >= 2`, i.e. 33 ms after the last press, while
 * d-pad repeat arrives every ~100 ms.  Every repeat therefore kicked off a
 * decode, which is why holding a direction hitched and sometimes stalled
 * long enough to draw a whole cover before moving on.
 *
 * The rule is simply: never decode while anything is animating, and not
 * until the user has actually stopped. */
static int scroll_settled(void)
{
   return g_scroll_t >= SCROLL_DUR && g_scroll == g_scroll_to;
}

/* Frames of stillness before each kind of decode is allowed.  The selected
 * cover is what the user is waiting for, so it goes first; neighbours are
 * speculative and wait until the browser is plainly idle. */
#define IDLE_SELECTED  10        /* ~170 ms */
#define IDLE_HERO      14        /* ~230 ms */
#define IDLE_PREFETCH  24        /* ~400 ms */

/* Pixels the column is offset by, relative to the selected row's slot. */
static int scroll_px(int cur, int row_h)
{
   return ((cur * SCROLL_ONE - g_scroll) * row_h) >> SCROLL_FP;
}

/* ---- art fade ------------------------------------------------------------
 * A decoded cover used to appear between one frame and the next.  Ramping it
 * in over ~8 frames also disguises the decode latency, which is the same
 * event seen from the other side. */
#define ART_FADE_STEP 32

static int  g_art_fade;                     /* 0..255 for the current art */
static int  g_art_fade_idx = -1;

static int art_alpha(int rom_idx, const void *art)
{
   if (rom_idx != g_art_fade_idx)
   {
      g_art_fade_idx = rom_idx;
      g_art_fade = 0;
   }
   if (!art)
      return 0;
   g_art_fade += ART_FADE_STEP;
   if (g_art_fade > 255)
      g_art_fade = 255;
   return g_art_fade;
}

#define SHELF_ROWS   7                    /* visible list rows */
#define SHELF_SEL    3                    /* the fixed cursor row */
#define SHELF_ROW_H  25
#define SHELF_TOP    48
#define ART_X        (480 - 20 - ART_W)
#define ART_Y        40

/* Warm the cache around the cursor so scrolling never waits on a decode.
 * Deliberately loads at most ONE per call: a burst of six decodes would
 * stutter worse than the pop-in it is meant to cure. */
static void art_prefetch(const char *rom_dir, int cur, int n)
{
   static const int off[] = { 0, 1, -1, 2, -2, 3, -3 };
   unsigned k;
   for (k = 0; k < sizeof(off) / sizeof(off[0]); k++)
   {
      int idx = ((cur + off[k]) % n + n) % n;
      int j, have = 0;
      for (j = 0; j < ART_SLOTS; j++)
         if (g_art[j].rom_idx == idx && g_art[j].state != 0)
            have = 1;
      if (!have)
      {
         art_get(rom_dir, idx, 1, NULL);  /* decode exactly one, then stop */
         return;
      }
   }
}

static void meta_line(const rom_entry *r, char *out, size_t sz)
{
   unsigned mb = (unsigned)((r->size + (1u << 19)) >> 20);
   snprintf(out, sz, "%u MB", mb ? mb : 1u);
}

/* Trim to a PIXEL budget: the face is proportional, so a column count
 * clips "WWWW" and "iiii" at wildly different widths. */
static void clip_title(char *t, int px)
{
   int n = (int)strlen(t);
   if (vid_text_w(t) <= px)
      return;
   /* Leave room for the ellipsis, then trim until the whole thing fits.
    * A hard cut is ambiguous -- "Pokemon Mystery Dungeon - Red Rescue Tea"
    * reads as a title someone typed wrong.  Three dots say "there is more"
    * and cost eleven pixels. */
   while (n > 1)
   {
      t[--n] = 0;
      if (vid_text_w(t) + vid_text_w("...") <= px)
         break;
   }
   /* Do not leave a dangling space or separator before the dots. */
   while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '-'))
      t[--n] = 0;
   strcpy(t + n, "...");
}

/* HORIZONTAL SCROLL FOR THE SELECTED TITLE.
 *
 * Truncation is honest but it still hides the end of a long name, and the
 * selected row is the one the user is actually reading.  So that one -- and
 * only that one, because more than one moving thing is noise -- slides left
 * to reveal the rest, then returns.
 *
 * Dwell at each end: motion that starts the instant you arrive is unreadable,
 * and motion that never rests is exhausting.  ~1.3 s still, then ~30 px/s,
 * then still again before it snaps back. */
#define MARQ_DWELL   80          /* frames held at each end (~1.3 s)      */
#define MARQ_SPEED   2           /* frames per pixel travelled            */

static int g_marq_t;
static int g_marq_idx = -1;

/* Pixels to shift the selected title left; 0 when it fits or is resting. */
static int marquee_offset(int rom_idx, const char *full, int px)
{
   /* MEASURE WITH THE FACE IT IS DRAWN IN.  This asked vid_text_w (Regular
    * 15 px) about a string rendered with vid_text_hd (SemiBold 19 px), so
    * anything that overflowed at 19 but fitted at 15 never scrolled -- which
    * is most of them.  The ones that did scroll also stopped short, because
    * `over` was computed from the narrower face. */
   int over = vid_text_hd_w(full) - px;
   int travel, cycle, t;

   if (rom_idx != g_marq_idx)
   {
      g_marq_idx = rom_idx;
      g_marq_t = 0;
   }
   if (over <= 0)
      return 0;                      /* fits: never moves */
   g_marq_t++;
   travel = over * MARQ_SPEED;
   cycle  = MARQ_DWELL + travel + MARQ_DWELL;
   t = g_marq_t % cycle;
   if (t < MARQ_DWELL)
      return 0;
   if (t < MARQ_DWELL + travel)
      return (t - MARQ_DWELL) / MARQ_SPEED;
   return over;
}

/* ---- SHELL A: Shelf ---------------------------------------------------- */
static void shell_shelf(const char *rom_dir, int cur, int n, int idle)
{
   const uint16_t *art;
   char buf[64], title[64];
   int i, sy;

   /* APPLY THE THEME.  g_thm was only ever assigned inside page(), which
    * the browser does not call -- so the gallery rendered with whatever
    * palette the last menu screen happened to leave behind.  Boot went
    * to the browser dark; opening Settings and coming back made it
    * light.  Set it where the drawing happens. */
   g_thm = (g_pcfg.theme == 1) ? &THM_LIGHT : &THM_DARK;

   scroll_track(cur, n);

   vid_rect(0, 0, 480, 272, C_BG_TOP, 255);

   vid_logo(20, 8, C_ACCENT, 255);
   snprintf(buf, sizeof(buf), "%d games", n);
   vid_text(ART_X + ART_W - vid_text_w(buf), 12, buf, C_DIM);

   /* The selection band does NOT move -- it is the fixed thing the list
    * slides under.  Only the rows take the scroll offset. */
   vid_rect(0, SHELF_TOP + SHELF_SEL * SHELF_ROW_H - 4, 300, SHELF_ROW_H,
            C_CARD, 255);
   vid_rect(0, SHELF_TOP + SHELF_SEL * SHELF_ROW_H - 4, 3, SHELF_ROW_H,
            C_ACCENT, 255);

   sy = scroll_px(cur, SHELF_ROW_H);

   /* CLIP THE LIST.  A row sliding in from above used to be drawn wherever
    * the tween put it, which with the wordmark now occupying y=8..30 meant
    * the incoming title crossed the logo.  Clipping cuts it at the boundary
    * instead, so the column slides UNDER the header the way it should. */
   vid_clip(0, SHELF_TOP - 6, 300, 246 - (SHELF_TOP - 6));

   /* One row of overscan each side, so a row sliding in is drawn before it
    * reaches the visible band instead of appearing at the edge. */
   for (i = -1; i <= SHELF_ROWS; i++)
   {
      int rel = i - SHELF_SEL;
      int idx = ((cur + rel) % n + n) % n;
      int y   = SHELF_TOP + i * SHELF_ROW_H + sy;
      int d   = rel < 0 ? -rel : rel;
      uint16_t col = (d == 0) ? C_SEL : (d == 1) ? C_ITEM : C_DIM;

      if (y < SHELF_TOP - SHELF_ROW_H || y > 240)
         continue;

      /* Short libraries do not wrap: padding a 3-game list with repeats
       * reads as a bug, not as a carousel. */
      if (n < SHELF_ROWS && (cur + rel < 0 || cur + rel >= n))
         continue;

      rom_display_name(&g_roms[idx], title, sizeof(title), 1);
      clip_title(title, 232);
      vid_text(20, y, title, col);
      if (g_roms[idx].has_sav)
         vid_text(288 - vid_text_w("SAVE"), y, "SAVE",
                  d == 0 ? C_ACCENT : C_ACCENT_DK);
   }
   vid_clip_off();

   {
      /* The cover is TOP-anchored in the panel and the caption follows its
       * real bottom edge, not the frame's.  Centring instead left a square
       * libretro boxart floating with dead space under it and pushed the
       * caption into the footer. */
      art_slot a;
      int ax = ART_X, ay = ART_Y, bot;
      a.aw = ART_W; a.ah = ART_H;
      art = art_get(rom_dir, cur, idle >= IDLE_SELECTED && scroll_settled(), &a);
      if (art)
      {
         int al = art_alpha(cur, art);
         ax = ART_X + (ART_W - a.aw) / 2;
         vid_rect(ax + 3, ay + 4, a.aw, a.ah, C_SHADOW, (90 * al) / 255);
         vid_image(ax, ay, a.aw, a.ah, art,
                   ART_TEX_W, ART_TEX_H, a.aw, a.ah, al);
         bot = ay + a.ah;
      }
      else
      {
         art_alpha(cur, NULL);          /* arm the fade for when it lands */
         card_template(ART_X, ART_Y, ART_W, ART_H, &g_roms[cur]);
         bot = ART_Y + ART_H;
      }
      meta_line(&g_roms[cur], buf, sizeof(buf));
      vid_text(ART_X, bot + 10, buf, C_DIM);
      vid_text(ART_X, bot + 28,
               g_roms[cur].has_sav ? "save present" : "no save",
               g_roms[cur].has_sav ? C_VALUE : C_DIM);
   }

   vid_rect(0, 246, 480, 1, C_CARD, 255);
   vid_text(20, 252, "X play", C_DIM);
   vid_text(116, 252, "L/R page", C_DIM);
   vid_text(244, 252, "START settings", C_DIM);
}

/* ---- SHELL B: Marquee -------------------------------------------------- */
#define MQ_ROW_H  24
#define MQ_MID    176            /* the selected row; everything else moves */

static void shell_marquee(const char *rom_dir, int cur, int n, int idle)
{
   int sy;
   (void)rom_dir;   /* the hero cache is keyed by index, not path */

   /* APPLY THE THEME.  g_thm was only ever assigned inside page(), which
    * the browser does not call -- so the gallery rendered with whatever
    * palette the last menu screen happened to leave behind.  Boot went
    * to the browser dark; opening Settings and coming back made it
    * light.  Set it where the drawing happens. */
   g_thm = (g_pcfg.theme == 1) ? &THM_LIGHT : &THM_DARK;

   scroll_track(cur, n);
   /* The hero is a SEPARATE, larger decode of the same file -- the shelf
    * thumbnail stretched to 480 wide is the blurry mess this replaces. */
   const uint16_t *hero = hero_get(cur, idle >= IDLE_HERO && scroll_settled());
   char buf[64], title[64];
   int i;

   vid_rect(0, 0, 480, 272, C_BG_TOP, 255);
   if (hero)
   {
      /* Full-bleed COVER: scale to the screen width, keep the aspect, and
       * let the overflow crop off the top and bottom.  Fitting instead
       * would letterbox the background, which reads as a bug. */
      int bh = (int)((long)480 * g_hero_h / (g_hero_w ? g_hero_w : 1));
      /* Fades in on arrival, so a hero that took a moment to decode eases
       * into place rather than replacing the flat background in one frame. */
      vid_image(0, (272 - bh) / 2, 480, bh, hero,
                g_hero_tex, g_hero_tex, g_hero_w, g_hero_h,
                art_alpha(cur, hero));
      /* Scrim by PROVENANCE.  Art from hero/ was composed for this shell --
       * already exposed for white text, already quiet on the left -- so it
       * needs a light touch.  A cover the emulator cropped itself has had no
       * such treatment and still needs the heavy one.  Using 205 on both
       * turns the good art black. */
      vid_rect(0, 0, 480, 272, C_BG_TOP,
               g_hero_precomposed ? 85 : 205);

      /* Footer band, drawn HERE rather than baked into the art -- baking it
       * put a hard horizontal seam across every image.  ONE alpha ramp, not
       * a stack of rects: four 8 px steps were invisible on the dark theme
       * and four obvious white bands on the light one, because the eye reads
       * banding against a pale ground far more easily. */
      vid_gradient_a(0, 232, 480, 40, C_BG_TOP, 0, 190);
   }

   vid_logo(20, 10, C_ACCENT, 255);

   {
      /* Drawn scrolled and then masked, so the text slides UNDER the edge of
       * the panel instead of appearing to run off the screen. */
      char full[64];
      int off;
      rom_display_name(&g_roms[cur], full, sizeof(full), 1);
      off = marquee_offset(cur, full, 244);
      vid_clip(20, 68, 244, 30);
      vid_text_hd(20 - off, 74, full, C_SEL);
      vid_clip_off();
   }

   meta_line(&g_roms[cur], buf, sizeof(buf));
   vid_text(20, 102, buf, C_ITEM);
   vid_text(20 + vid_text_w(buf) + 14, 102,
            g_roms[cur].has_sav ? "save present" : "no save",
            g_roms[cur].has_sav ? C_VALUE : C_DIM);

   /* FIVE rows, not three.  At 24 px pitch they span 128..224, which clears
    * the metadata line (ends ~117) and the footer ramp (starts 240).  The
    * selected row stays at 176 so nothing else on the screen moves. */
   sy = scroll_px(cur, MQ_ROW_H);
   for (i = -3; i <= 3; i++)          /* one row of overscan each side */
   {
      int idx = ((cur + i) % n + n) % n;
      int y   = MQ_MID + i * MQ_ROW_H + sy;
      int d   = i < 0 ? -i : i;
      if (n < 3 && i != 0)
         continue;
      if (y < MQ_MID - 2 * MQ_ROW_H - 4 || y > MQ_MID + 2 * MQ_ROW_H + 4)
         continue;
      rom_display_name(&g_roms[idx], title, sizeof(title), 1);
      clip_title(title, 232);
      /* The far rows fade out, so five entries do not read as a wall of
       * text competing with the artwork behind them. */
      vid_text(32, y, title,
               d == 0 ? C_SEL : (d == 1 ? C_ITEM : C_DIM));
   }
   vid_rect(20, MQ_MID - 4, 3, 21, C_ACCENT, 255);

   vid_text(20, 252, "X play", C_DIM);
   vid_text(116, 252, "L/R page", C_DIM);
   vid_text(244, 252, "START settings", C_DIM);
}

extern volatile int g_running;               /* main_psp exit flag */

/* START in the browser opens the SAME settings screen the in-game menu
 * uses -- previously the only way in was through a running game, so a
 * fresh install could not be configured at all.  Runs the screen
 * directly rather than through ui_frame(): the menu state machine is
 * for the in-game overlay, and ui_open() there would offer Resume and
 * Save state against a core that has not started.
 *
 * Returns 1 if a boot-time setting changed and the caller must relaunch
 * (Media Engine mode is latched before the browser runs), else 0. */
static int browser_settings(void)
{
   int relaunch = 0;

   screen_to(SCR_SETTINGS);
   g_cursor = SET_ROOM;
   g_prev_pad = 0xFFFFFFFFu;      /* swallow the opening START */
   while (g_running && g_screen == SCR_SETTINGS)
   {
      SceCtrlData pd;
      unsigned edges;
      sceCtrlPeekBufferPositive(&pd, 1);
      edges = pad_edges(pd.Buttons);
      vid_overlay_begin(1);
      if (screen_settings(edges) == UI_ACT_RELAUNCH)
      {
         relaunch = 1;
         g_screen = SCR_MENU;   /* screen_settings already consumed O */
      }
      vid_overlay_end();
      sceDisplayWaitVblankStart();
      vid_swap();
   }
   if (g_settings_dirty)
   {
      pcfg_save();
      g_settings_dirty = 0;
   }
   /* Menu style needs no restart from here -- the browser is still the
    * thing drawing, so re-latching applies it on the next frame. */
   g_ui_shell = g_pcfg.ui_shell;
   g_prev_pad = 0xFFFFFFFFu;
   return relaunch;
}

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
   g_ui_shell = g_pcfg.ui_shell;
   g_scroll_cur = -1;                 /* no slide on the first frame */
   g_art_fade_idx = -1;

   g_prev_pad = 0xFFFFFFFFu;
   while (g_running)
   {
      SceCtrlData pd;
      unsigned edges;

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

      /* The list is vertical now, so UP/DOWN step and LEFT/RIGHT page
       * alongside the shoulder buttons. */
      if (edges & PSP_CTRL_UP)
         { cur = (cur + n - 1) % n; idle = 0; }
      if (edges & PSP_CTRL_DOWN)
         { cur = (cur + 1) % n; idle = 0; }
      if (edges & (PSP_CTRL_LTRIGGER | PSP_CTRL_LEFT))
         { cur = (cur + n - SHELF_ROWS) % n; idle = 0; }
      if (edges & (PSP_CTRL_RTRIGGER | PSP_CTRL_RIGHT))
         { cur = (cur + SHELF_ROWS) % n; idle = 0; }
      if (edges & PSP_CTRL_START)
      {
         if (browser_settings())
         {
            art_free_all();
            return 1;
         }
         idle = 0;
         continue;
      }
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

      /* Warm the cache around the cursor -- at most one decode per frame, so
       * holding a direction still scrolls smoothly. This is what removes the
       * art popping in as you move. */
      if (idle >= IDLE_PREFETCH && scroll_settled())
         art_prefetch(rom_dir, cur, n);

      if (g_ui_shell)
         shell_marquee(rom_dir, cur, n, idle);
      else
         shell_shelf(rom_dir, cur, n, idle);

      vid_overlay_end();
      sceDisplayWaitVblankStart();
      vid_swap();
      if (idle < 1000)
         idle++;
   }
   art_free_all();
   return -1;
}
