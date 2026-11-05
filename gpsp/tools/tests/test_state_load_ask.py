#!/usr/bin/env python3
"""A state load asks first (docs/UI-OVERLAY.md sections 14.2-14.3).

X on a saved slot of the SAVE STATES page loaded it at once and closed the
menu: one press too many, or X where SQUARE was meant, and the game the
player was in was gone.  It now puts the page's question on the plate --
"Load Slot n? Unsaved progress is lost.  X load  O cancel" -- the same plate
and the same rule as overwrite and delete: X answers it, every other key
keeps the game, and no other key acts that frame.  The quick load
(bind_quickload, SELECT+R by default) asks the same question over the game,
unless Settings > "Quick load asks first" (config.ini `quick_load_ask`) is
off, when it loads at once exactly as 4.6.0-beta.2 did.

PART A, the page.  Taken as written from psp/ui_psp.c: the page's question
state (the SLOT_ASK enum and its variables), its geometry (SS_*), the pad's
edge filter (pad_edges), screen_state_slots() and ui_state_load_ask().
Stubbed: drawing (vid_text and the plate / footer record what was said), the
scan, the dwell read, the delete, screen_to / screen_menu.

  1. X on a saved slot asks, and loads nothing; the plate names the slot.
  2. X on the question loads that slot (UI_ACT_LOADSTATE, ui slot = it).
  3. Every other key on the question keeps the game: nothing loads, saves,
     deletes or moves, the page stays (O does not go back to the menu), and
     the next X asks again rather than loading.
  4. X on an empty slot asks nothing and loads nothing ("Slot n is empty").
  5. Overwrite and delete still ask and still answer to X.
  6. The last slot: UP from slot 1 reaches it; the question and the load
     are its own (PSP_STATE_SLOT_COUNT, whatever it is).
  7. The quick load: ui_state_load_ask(1) opens the page on slot 1 with the
     question up; X loads; every other key closes the menu straight back
     into the game (UI_ACT_RESUME) having loaded nothing; a failed load then
     behaves as the page's.  An empty slot 1, a slot out of range or no
     menu asks nothing.
  8. A direction held from the game (or a chord on the d-pad) does not
     auto-repeat into an answer, however long it is held; a fresh press
     still answers.

PART B, the call site.  psp/main_psp.c's quick-save / quick-load dispatch,
taken as written (`if (ctl_fired & CTL_FIRE_SAVE) ... else if (ctl_fired &
CTL_FIRE_LOAD) {...}`), compiled with the real ctl_map.c / config_psp.c /
fe_util.c and fed ctl_shortcuts() of a real pad, after pcfg_load() of a
real config.ini:

  9. quick_load_ask on (the default, no key): the load opens the menu and
     asks for slot 1 and loads nothing itself; nothing in slot 1 says so with
     no menu; a wireless session refuses as before; the quick save is
     untouched.
 10. Any binding: Chandler's swap (bind_quicksave = SELECT+R,
     bind_quickload = SELECT+L) asks on SELECT+L and saves on SELECT+R.
 11. OFF IS OFF: quick_load_ask = 0 runs exactly 4.6.0-beta.2's branch --
     the same calls with the same arguments in the same order, compared
     call by call against beta 2's own text (BETA2_LOAD below) compiled
     with the same stubs, in every case.
 12. The page's question does not depend on the setting, and the menu
     branch's UI_ACT_LOADSTATE (what the question's X returns) loads
     ui_state_slot() and is the only "menu" load.

PART C, CONFIG.INI.  pcfg_load / pcfg_save on a scratch file: no key is on
and a save writes no key (a player who keeps the default keeps the file);
off is written as `quick_load_ask = 0` and read back off; on again keeps the
key as 1; garbage is on; a corrupt in-memory value is repaired to on.

Then mutants of the production text (page, call site, config), each of which
must fail.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UI = (ROOT / "psp/ui_psp.c").read_text(encoding="utf-8")
MAIN = (ROOT / "psp/main_psp.c").read_text(encoding="utf-8")
CFG = (ROOT / "psp/config_psp.c").read_text(encoding="utf-8")

TOKENS = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
                    r"'(?:\\.|[^'\\])*'|[{}]", re.S)


def braced(source, start):
    depth = 0
    for token in TOKENS.finditer(source, start):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:token.end()]
    raise AssertionError("unclosed block")


def static_fn(source, signature):
    i = source.find(signature + "\n{")
    assert i >= 0, signature + " not found"
    return braced(source, i)


def span(source, first, last):
    """From the line starting `first` to the end of the line holding `last`."""
    i = source.find(first)
    assert i >= 0, first + " not found"
    j = source.find(last, i)
    assert j >= 0, last + " not found after " + first
    return source[i:source.index("\n", j) + 1]


PAGE = "static ui_action screen_state_slots(unsigned edges)"
ASK = "int ui_state_load_ask(int slot)"
EDGES = "static unsigned pad_edges(unsigned pad)"


def production(src):
    return "\n".join((
        span(src, "enum { SLOT_ASK_DELETE", "static int  g_slots_dirty;"),
        span(src, "#define SS_", "#define SS_PH "),
        static_fn(src, EDGES),
        static_fn(src, PAGE),
        static_fn(src, ASK)))


CC = ["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
      "-Wno-unused-function", "-Wno-unused-variable",
      "-Wno-unused-but-set-variable",
      # the page's "Slot %d  -  %s" caption: gcc 14 counts a 24-byte stamp
      # into its 48-byte buffer as a maybe
      "-Wno-format-truncation",
      "-I" + str(ROOT / "psp"), "-I" + str(ROOT / "frontend-common")]

# ---------------------------------------------------------------- part A ----
PAGE_STUB = r'''
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "state_slots.h"

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define FE_EVT_ONLY(x) ((void)(x))
#define fe_evt(...) ((void)0)
#define PSP_CTRL_SELECT   0x000001u
#define PSP_CTRL_START    0x000008u
#define PSP_CTRL_UP       0x000010u
#define PSP_CTRL_RIGHT    0x000020u
#define PSP_CTRL_DOWN     0x000040u
#define PSP_CTRL_LEFT     0x000080u
#define PSP_CTRL_LTRIGGER 0x000100u
#define PSP_CTRL_RTRIGGER 0x000200u
#define PSP_CTRL_TRIANGLE 0x001000u
#define PSP_CTRL_CIRCLE   0x002000u
#define PSP_CTRL_CROSS    0x004000u
#define PSP_CTRL_SQUARE   0x008000u
#define VID_GLYPH_TRI "\x7F"
#define VID_GLYPH_O   "\x80"
#define VID_GLYPH_X   "\x81"
#define VID_GLYPH_SQ  "\x82"
enum { C_SEL = 1, C_ITEM, C_DIM, C_VALUE, C_ACCENT, C_CARD };
#define OV_SCRIM 170
#define OV_SHOT_DWELL 6
#define SCR_MENU 0
#define SCR_STATE_SLOTS 5
#define N PSP_STATE_SLOT_COUNT
typedef enum { UI_ACT_NONE = 0, UI_ACT_RESUME, UI_ACT_SAVESTATE,
               UI_ACT_LOADSTATE, UI_ACT_OTHER } ui_action;

static char g_state_base[PSP_FILE_PATH_CAP] = "/r/Ruby.st0";
static int g_state_slot = 1, g_state_top, g_screen = SCR_STATE_SLOTS;
static unsigned char g_ov_slot[PSP_STATE_SLOT_COUNT];
static char g_ov_stamp[PSP_STATE_SLOT_COUNT][24];
static unsigned char g_ov_shot_none[PSP_STATE_SLOT_COUNT];
static int g_ov_shot_slot = -1, g_ov_dwell, g_ov_linked;
static uint16_t *g_ov_thumbs;
static char g_ov_title[100] = "Ruby";
static int g_active = 1;
static unsigned g_prev_pad;
static int g_rep_timer;
static int deletes, menus;

/* What a frame said: every vid_text, and the plate or the footer. */
static char drawn[64][96];
static int n_drawn;
static char plate[128], foot[128];

static void ov_slots_scan(void) {}
static int state_delete(const char *b, unsigned s)
{ (void)b; deletes++; g_ov_slot[s - 1] = 0; return 0; }
static void screen_to(int s) { g_screen = s; }
static ui_action screen_menu(unsigned e, int l)
{ (void)e; (void)l; menus++; return UI_ACT_NONE; }
static unsigned sceKernelGetSystemTimeLow(void) { return 0; }
static int ov_shot_read(int i) { (void)i; return -1; }
static uint16_t *vid_shot_tex(void) { return NULL; }
static void ov_backdrop(int a) { (void)a; }
static void ov_head(const char *t, const char *r) { (void)t; (void)r; }
static void vid_rect(int x, int y, int w, int h, int c, int a)
{ (void)x; (void)y; (void)w; (void)h; (void)c; (void)a; }
static void vid_image(int x, int y, int w, int h, const uint16_t *p, int tw,
                      int th, int sw, int sh, int a)
{ (void)x; (void)y; (void)w; (void)h; (void)p; (void)tw; (void)th;
  (void)sw; (void)sh; (void)a; }
static void vid_text(int x, int y, const char *s, int c)
{
   (void)x; (void)y; (void)c;
   if (n_drawn < 64)
      snprintf(drawn[n_drawn++], sizeof(drawn[0]), "%s", s);
}
static void ov_scrollbar(int x, int y, int h, int p, int v, int t)
{ (void)x; (void)y; (void)h; (void)p; (void)v; (void)t; }
static void ss_panel(int i) { (void)i; }
static void ov_plate(const char *m) { snprintf(plate, sizeof(plate), "%s", m); }
static void footer(const char *m) { snprintf(foot, sizeof(foot), "%s", m); }

@FUNCTIONS@

static ui_action press(unsigned edges)
{
   n_drawn = 0;
   plate[0] = foot[0] = '\0';
   return screen_state_slots(edges);
}

/* One frame of ui_frame: the pad through the real edge filter. */
static ui_action frame(unsigned pad)
{
   return press(pad_edges(pad));
}

static void page(void)
{
   int i;
   memset(g_ov_slot, 0, sizeof(g_ov_slot));
   for (i = 0; i < PSP_STATE_SLOT_COUNT; i++)
      g_ov_stamp[i][0] = '\0';
   g_ov_slot[0] = 1;              /* slots 1, 2 and the last hold states */
   g_ov_slot[1] = 1;
   g_ov_slot[N - 1] = 1;
   g_state_slot = 1;
   g_state_top = 0;
   g_screen = SCR_STATE_SLOTS;
   g_slot_ask = g_slot_ask_n = g_slot_saving = g_slot_ask_back = 0;
   g_slot_note[0] = '\0';
   deletes = menus = 0;
   g_prev_pad = 0;
   g_rep_timer = 0;
   press(0);
}

/* ui_open: the menu's top page, the opening chord swallowed. */
static void menu_open(void)
{
   page();
   g_screen = SCR_MENU;
   g_prev_pad = 0xFFFFFFFFu;
}

int main(void)
{
   static const unsigned others[] = { PSP_CTRL_CIRCLE, PSP_CTRL_SQUARE,
      PSP_CTRL_TRIANGLE, PSP_CTRL_UP, PSP_CTRL_DOWN, PSP_CTRL_LEFT,
      PSP_CTRL_RIGHT, PSP_CTRL_START, PSP_CTRL_SELECT, PSP_CTRL_LTRIGGER,
      PSP_CTRL_RTRIGGER, PSP_CTRL_CIRCLE | PSP_CTRL_SQUARE };
   char want[24];
   unsigned k;
   int f;

   /* 1, 2. X asks; X answers; the load is slot 1's */
   page();
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_NONE);
   CHECK(!strncmp(plate, "Load Slot 1? Unsaved progress is lost.", 38));
   CHECK(strstr(plate, VID_GLYPH_X " load") && strstr(plate, VID_GLYPH_O " cancel"));
   CHECK(press(0) == UI_ACT_NONE && plate[0]);        /* it waits */
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_LOADSTATE);
   CHECK(g_state_slot == 1 && !g_slot_ask);

   /* 3. every other key keeps the game, and acts on nothing else */
   for (k = 0; k < sizeof(others) / sizeof(others[0]); k++)
   {
      page();
      CHECK(press(PSP_CTRL_CROSS) == UI_ACT_NONE && plate[0]);
      CHECK(press(others[k]) == UI_ACT_NONE);
      CHECK(!g_slot_ask && !plate[0]);
      CHECK(g_state_slot == 1 && g_screen == SCR_STATE_SLOTS && !menus);
      CHECK(!deletes && !g_slot_saving && g_ov_slot[0] == 1);
      CHECK(press(PSP_CTRL_CROSS) == UI_ACT_NONE);   /* asks again */
      CHECK(!strncmp(plate, "Load Slot 1? ", 13));
   }

   /* 4. an empty slot: nothing to ask, nothing loaded */
   page();
   CHECK(press(PSP_CTRL_DOWN) == UI_ACT_NONE && g_state_slot == 2);
   CHECK(press(PSP_CTRL_DOWN) == UI_ACT_NONE && g_state_slot == 3);
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_NONE && !g_slot_ask);
   CHECK(!strcmp(plate, "Slot 3 is empty"));
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_NONE);

   /* 5. overwrite and delete: unchanged */
   page();
   CHECK(press(PSP_CTRL_SQUARE) == UI_ACT_NONE && !strncmp(plate, "Overwrite Slot 1?", 17));
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_SAVESTATE && g_slot_saving == 1);
   page();
   CHECK(press(PSP_CTRL_TRIANGLE) == UI_ACT_NONE && !strncmp(plate, "Delete Slot 1?", 14));
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_NONE && deletes == 1 && !g_ov_slot[0]);
   page();
   CHECK(press(PSP_CTRL_SQUARE) == UI_ACT_NONE);
   CHECK(press(PSP_CTRL_CIRCLE) == UI_ACT_NONE && !g_slot_saving && !menus);

   /* 6. the last slot: UP wraps to it; its question, its load */
   page();
   snprintf(want, sizeof(want), "Load Slot %d? ", N);
   CHECK(press(PSP_CTRL_UP) == UI_ACT_NONE && g_state_slot == N);
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_NONE && !strncmp(plate, want, strlen(want)));
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_LOADSTATE && g_state_slot == N);

   /* 7. the quick load: the question over the game, slot 1 */
   menu_open();
   g_state_slot = 4;                   /* where the page was last left */
   CHECK(ui_state_load_ask(1) == 1);
   CHECK(g_screen == SCR_STATE_SLOTS && g_state_slot == 1);
   CHECK(frame(PSP_CTRL_SELECT | PSP_CTRL_RTRIGGER) == UI_ACT_NONE);  /* held */
   CHECK(!strncmp(plate, "Load Slot 1? ", 13));
   CHECK(frame(0) == UI_ACT_NONE && plate[0]);
   CHECK(frame(PSP_CTRL_CROSS) == UI_ACT_LOADSTATE && g_state_slot == 1);
   for (k = 0; k < sizeof(others) / sizeof(others[0]); k++)
   {
      menu_open();
      CHECK(ui_state_load_ask(1) == 1);
      CHECK(frame(0) == UI_ACT_NONE);
      CHECK(frame(others[k]) == UI_ACT_RESUME);   /* back into the game */
      CHECK(!g_slot_ask && !deletes && !g_slot_saving && !menus);
   }
   /* after a keep from the page itself, the page stays (section 3) -- the
    * quick load's "back to the game" does not outlive its question */
   menu_open();
   CHECK(ui_state_load_ask(1) == 1);
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_LOADSTATE);   /* a failed load: */
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_NONE && plate[0]);   /* asks */
   CHECK(press(PSP_CTRL_CIRCLE) == UI_ACT_NONE && g_screen == SCR_STATE_SLOTS);
   /* an empty slot 1 asks nothing (main_psp.c then says "No saved state") */
   menu_open();
   g_ov_slot[0] = 0;
   CHECK(ui_state_load_ask(1) == 0 && !g_slot_ask);
   CHECK(ui_state_load_ask(0) == 0 && ui_state_load_ask(N + 1) == 0);
   g_active = 0;
   g_ov_slot[0] = 1;
   CHECK(ui_state_load_ask(1) == 0 && !g_slot_ask);   /* no menu: no question */
   g_active = 1;

   /* 8. a direction held from the game is not an answer, however long */
   menu_open();
   CHECK(ui_state_load_ask(1) == 1);
   for (f = 0; f < 600; f++)
      CHECK(frame(PSP_CTRL_RIGHT) == UI_ACT_NONE && g_slot_ask);
   CHECK(frame(0) == UI_ACT_NONE && g_slot_ask);
   CHECK(frame(PSP_CTRL_CROSS) == UI_ACT_LOADSTATE);
   menu_open();                       /* a quick load bound to SELECT+UP */
   CHECK(ui_state_load_ask(1) == 1);
   for (f = 0; f < 600; f++)
      CHECK(frame(PSP_CTRL_SELECT | PSP_CTRL_UP) == UI_ACT_NONE && g_slot_ask);
   CHECK(frame(PSP_CTRL_SELECT) == UI_ACT_NONE && g_slot_ask);
   CHECK(frame(0) == UI_ACT_NONE && frame(PSP_CTRL_CROSS) == UI_ACT_LOADSTATE);
   menu_open();                       /* ...and a fresh press still keeps */
   CHECK(ui_state_load_ask(1) == 1);
   CHECK(frame(PSP_CTRL_RIGHT) == UI_ACT_NONE && frame(0) == UI_ACT_NONE);
   CHECK(frame(PSP_CTRL_RIGHT) == UI_ACT_RESUME);
   /* the page's own d-pad still repeats (the cursor walks while held) */
   page();
   CHECK(frame(PSP_CTRL_DOWN) == UI_ACT_NONE && g_state_slot == 2);
   for (f = 0; f < 18; f++)
      frame(PSP_CTRL_DOWN);
   CHECK(g_state_slot == 3);

   puts("page: X asks and X loads, every other key keeps the game; the quick "
        "load asks over the game; a held direction never answers");
   return 0;
}
'''

# ---------------------------------------------------------------- part B ----
# psp/main_psp.c's quick-load branch in 4.6.0-beta.2 (9082e3b), verbatim:
# what quick_load_ask = 0 must still be, call for call.
BETA2_LOAD = r'''
         {
            /* NEVER LOAD DURING A WIRELESS SESSION. Restoring a state while
             * a trade or Mystery Gift transfer is active rewinds the game's
             * RFU state underneath a live transport. */
            if (link_locks_states())
               osd_toast("Cannot load during a wireless session");
            else if (psp_state_load(state_path, "chord") == 0)
            {
               fe_evt("state_load via=chord");
               osd_toast("State loaded");
            }
            else
               osd_toast("No saved state");
         }'''

SITE_STUB = r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ctl_map.h"
#include "config_psp.h"
#include "fe_util.h"

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

/* config_psp.c's needs (as tools/tests/test_ctl_map.c) */
void fe_log(const char *fmt, ...) { (void)fmt; }
const char *vid_scale_name(int mode) { (void)mode; return "t"; }
const char *vid_filter_name(int mode) { (void)mode; return "t"; }

/* What the dispatch did, in order: one line a call. */
static char trace[4096];
static int tracing;
static void say(const char *fmt, ...)
{
   va_list ap;
   size_t n = strlen(trace);
   if (!tracing)
      return;
   va_start(ap, fmt);
   vsnprintf(trace + n, sizeof(trace) - n, fmt, ap);
   va_end(ap);
   strncat(trace, "\n", sizeof(trace) - strlen(trace) - 1);
}
#ifndef GPSP_PLAYABLE
void fe_evt(const char *fmt, ...)
{
   char b[128];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(b, sizeof(b), fmt, ap);
   va_end(ap);
   say("evt %s", b);            /* only while tracing: not pcfg_load's */
}
#endif
static char state_path[64] = "ms0:/r/Ruby.st0";
static int locked, have_state, recover_rc, load_rc, ask_rc;
static int link_locks_states(void) { say("locks"); return locked; }
static void osd_toast(const char *m) { say("toast %s", m); }
static int fe_host_state_recover(const char *p) { say("recover %s", p); return recover_rc; }
static int file_exists(const char *p) { say("exists %s", p); return have_state; }
static void menu_open(void) { say("menu_open"); }
static int ui_state_load_ask(int s) { say("ask %d", s); return ask_rc; }
static void ui_close(void) { say("ui_close"); }
static int psp_state_load(const char *p, const char *via)
{ say("load %s %s", p, via); return load_rc; }
static int state_save_slot(const char *p, unsigned s)
{ say("save %s %u", p, s); return 0; }

/* main_psp.c, as written: the quick save / quick load dispatch. */
static void dispatch(unsigned ctl_fired)
{
@DISPATCH@
}

/* 4.6.0-beta.2's quick load, as written (BETA2_LOAD). */
static void beta2_load(void)
@BETA2@

static const char *ini = "/tmp/@INI@";
static void write_file(const char *t)
{
   FILE *f = fopen(ini, "w");
   CHECK(f && fputs(t, f) >= 0);
   fclose(f);
}

static const char *run(unsigned pad, unsigned pad_new)
{
   unsigned fired = ctl_shortcuts(&g_pcfg.controls, pad, pad_new);
   trace[0] = '\0';
   tracing = 1;
   dispatch(fired);
   tracing = 0;
   return trace;
}

/* Every case of the setting-off branch against beta 2's, call for call. */
static void off_is_beta2(void)
{
   static char ours[4096];
   int a, b, c;
   CHECK(!g_pcfg.quick_load_ask);
   for (a = 0; a < 2; a++)
      for (b = 0; b < 2; b++)
         for (c = 0; c < 2; c++)
         {
            locked = a; load_rc = b ? -1 : 0; have_state = c;
            recover_rc = 0; ask_rc = 1;
            trace[0] = '\0';
            tracing = 1;
            dispatch(CTL_FIRE_LOAD);
            snprintf(ours, sizeof(ours), "%s", trace);
            trace[0] = '\0';
            beta2_load();
            tracing = 0;
            if (strcmp(ours, trace))
            {
               fprintf(stderr, "FAIL off != beta 2 (locked=%d load_rc=%d):\n"
                       "--- ours\n%s--- beta 2\n%s", a, load_rc, ours, trace);
               exit(1);
            }
            CHECK(!strstr(ours, "menu_open") && !strstr(ours, "ask "));
         }
}

int main(void)
{
   const unsigned SEL = CTL_SELECT, L = CTL_L, R = CTL_R;

   /* 9. the default: no key in config.ini, the quick load asks */
   write_file("scale = 1\n");
   memset(&g_pcfg, 0x5A, sizeof(g_pcfg));
   pcfg_load(ini);
   CHECK(g_pcfg.quick_load_ask == 1);
   locked = 0; have_state = 1; recover_rc = 0; load_rc = 0; ask_rc = 1;
   CHECK(!strcmp(run(SEL | R, R),
                 "locks\nrecover ms0:/r/Ruby.st0\nexists ms0:/r/Ruby.st0\n"
                 "evt state_load_ask via=chord\nmenu_open\nask 1\n"));
   have_state = 0;                     /* nothing in slot 1: no menu */
   CHECK(!strcmp(run(SEL | R, R),
                 "locks\nrecover ms0:/r/Ruby.st0\nexists ms0:/r/Ruby.st0\n"
                 "toast No saved state\n"));
   have_state = 1; recover_rc = -1;    /* the recovery failed: the same */
   CHECK(!strcmp(run(SEL | R, R),
                 "locks\nrecover ms0:/r/Ruby.st0\ntoast No saved state\n"));
   recover_rc = 0; ask_rc = 0;         /* the page found nothing after all */
   CHECK(!strcmp(run(SEL | R, R),
                 "locks\nrecover ms0:/r/Ruby.st0\nexists ms0:/r/Ruby.st0\n"
                 "evt state_load_ask via=chord\nmenu_open\nask 1\n"
                 "ui_close\ntoast No saved state\n"));
   ask_rc = 1; locked = 1;             /* a wireless session: refused */
   CHECK(!strcmp(run(SEL | R, R),
                 "locks\ntoast Cannot load during a wireless session\n"));
   locked = 0;
   CHECK(!strcmp(run(SEL | L, L), "locks\nsave ms0:/r/Ruby.st0 1\ntoast State saved\n"));
   CHECK(!strcmp(run(SEL | R, 0), ""));     /* held, not pressed: nothing */

   /* 10. any binding: the action asks, whatever its buttons */
   write_file("scale = 1\nbind_quicksave = SELECT+R\n"
              "bind_quickload = SELECT+L\n");
   pcfg_load(ini);
   CHECK(g_pcfg.quick_load_ask == 1);
   CHECK(!strcmp(run(SEL | L, L),
                 "locks\nrecover ms0:/r/Ruby.st0\nexists ms0:/r/Ruby.st0\n"
                 "evt state_load_ask via=chord\nmenu_open\nask 1\n"));
   CHECK(!strcmp(run(SEL | R, R), "locks\nsave ms0:/r/Ruby.st0 1\ntoast State saved\n"));
   write_file("scale = 1\nbind_quickload = SELECT+UP\n");   /* on the d-pad */
   pcfg_load(ini);
   CHECK(g_pcfg.controls.bind[CTL_SC_LOAD] == (CTL_SELECT | CTL_UP));
   CHECK(strstr(run(SEL | CTL_UP, CTL_UP), "menu_open\nask 1\n"));

   /* 11. off is off: beta 2's branch, call for call */
   write_file("scale = 1\nquick_load_ask = 0\n"
              "bind_quicksave = SELECT+R\nbind_quickload = SELECT+L\n");
   pcfg_load(ini);
   CHECK(g_pcfg.quick_load_ask == 0);
   locked = 0; have_state = 1; load_rc = 0;
   CHECK(!strcmp(run(SEL | L, L),
                 "locks\nload ms0:/r/Ruby.st0 chord\n"
                 "evt state_load via=chord\ntoast State loaded\n"));
   off_is_beta2();
   remove(ini);

   puts("call site: the quick load asks (any binding) and loads nothing "
        "itself; quick_load_ask = 0 is 4.6.0-beta.2's branch, call for call");
   return 0;
}
'''

# ---------------------------------------------------------------- part C ----
CFG_TEST = r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ctl_map.h"
#include "config_psp.h"
#include "fe_util.h"

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
void fe_evt(const char *fmt, ...) { (void)fmt; }
void fe_log(const char *fmt, ...) { (void)fmt; }
const char *vid_scale_name(int mode) { (void)mode; return "t"; }
const char *vid_filter_name(int mode) { (void)mode; return "t"; }

static const char *ini = "/tmp/@INI@";
static void write_file(const char *t)
{
   FILE *f = fopen(ini, "w");
   CHECK(f && fputs(t, f) >= 0);
   fclose(f);
}
static char *read_file(void)
{
   static char buf[16384];
   size_t n;
   FILE *f = fopen(ini, "r");
   CHECK(f);
   n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);
   return buf;
}
static int load(const char *text)
{
   write_file(text);
   memset(&g_pcfg, 0x5A, sizeof(g_pcfg));
   pcfg_load(ini);
   return g_pcfg.quick_load_ask;
}

int main(void)
{
   static char first[16384];
   /* no key: on, and a save writes no key -- twice, byte for byte */
   CHECK(load("scale = 1\n") == 1);
   pcfg_save();
   CHECK(strstr(read_file(), "quick_load_ask") == NULL);
   snprintf(first, sizeof(first), "%s", read_file());
   CHECK(load(first) == 1);
   pcfg_save();
   CHECK(!strcmp(first, read_file()));
   /* off: written, and read back off */
   g_pcfg.quick_load_ask = 0;
   pcfg_save();
   CHECK(strstr(read_file(), "quick_load_ask = 0\n") != NULL);
   memset(&g_pcfg, 0, sizeof(g_pcfg));
   g_pcfg.quick_load_ask = 7;
   pcfg_load(ini);
   CHECK(g_pcfg.quick_load_ask == 0);
   /* on again: the key stays, as 1 */
   g_pcfg.quick_load_ask = 1;
   pcfg_save();
   CHECK(strstr(read_file(), "quick_load_ask = 1\n") != NULL);
   CHECK(load(read_file()) == 1);
   /* anything but a 0 is on: garbage and other numbers */
   CHECK(load("quick_load_ask = maybe\n") == 1);
   CHECK(load("quick_load_ask = 2\n") == 1);
   CHECK(load("quick_load_ask =\n") == 1);
   CHECK(load("quick_load_ask = 0\n") == 0);
   /* a corrupt value in memory is repaired to on by the save */
   load("scale = 1\n");
   g_pcfg.quick_load_ask = 0x5A5A;
   pcfg_save();
   CHECK(g_pcfg.quick_load_ask == 1);
   CHECK(strstr(read_file(), "quick_load_ask") == NULL);
   remove(ini);
   puts("config.ini: quick_load_ask is on with no key and writes none; "
        "0 round-trips; garbage is on");
   return 0;
}
'''


def dispatch_block(src):
    """main(): `if (ctl_fired & CTL_FIRE_SAVE) {...} else if (ctl_fired &
    CTL_FIRE_LOAD) {...}`, as written."""
    i = src.find("         if (ctl_fired & CTL_FIRE_SAVE)\n")
    assert i >= 0, "the quick-save dispatch is gone"
    j = src.find("         else if (ctl_fired & CTL_FIRE_LOAD)\n", i)
    assert j >= 0, "the quick-load dispatch is gone"
    k = src.find("\n         {", j)
    return src[i:k + 1] + braced(src, k + 1)


def compile_run(code, expect_pass, extra=()):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-loadask-") as folder:
        tag = Path(folder).name
        cfile = Path(folder) / "t.c"
        binary = Path(folder) / "t"
        cfile.write_text(code.replace("@INI@", tag + ".ini"))
        subprocess.run(CC + [str(cfile)] + list(extra) + ["-o", str(binary)],
                       check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, result.stdout, result.stderr))
        return result


def run_page(ui, expect_pass):
    return compile_run(PAGE_STUB.replace("@FUNCTIONS@", production(ui)),
                       expect_pass)


def run_linked(code, cfg, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-loadcfg-") as folder:
        c = Path(folder) / "config_psp.c"
        c.write_text(cfg)
        return compile_run(code, expect_pass,
                           ("-I" + str(ROOT / "psp"), str(c),
                            str(ROOT / "psp/ctl_map.c"),
                            str(ROOT / "frontend-common/fe_util.c")))


def run_site(main, cfg, expect_pass):
    code = (SITE_STUB.replace("@DISPATCH@", dispatch_block(main))
            .replace("@BETA2@", BETA2_LOAD))
    return run_linked(code, cfg, expect_pass)


def run_cfg(cfg, expect_pass):
    return run_linked(CFG_TEST, cfg, expect_pass)


def case_body(src, label):
    i = src.find("         case %s:\n" % label)
    assert i >= 0, label + " not handled"
    j = src.find("         case ", i + 10)
    return src[i:j]


def menu_load(src):
    """12: what the question's X returns is loaded as the page's slot."""
    bad = []
    load = case_body(src, "UI_ACT_LOADSTATE")
    if not re.search(r"psp_state_path_for_slot\([^;]*\(unsigned\)ui_state_slot\(\)\)",
                     load, re.S) or 'psp_state_load(selected_state, "menu")' not in load:
        bad.append("UI_ACT_LOADSTATE does not load the page's slot")
    if len(re.findall(r'psp_state_load\([^;]*"menu"\)', src)) != 1:
        bad.append("a menu load outside UI_ACT_LOADSTATE")
    return bad


# The page's question is the page's: no setting reaches screen_state_slots.
assert "quick_load_ask" not in static_fn(UI, PAGE), \
    "the page's question must not depend on the quick-load setting"
assert menu_load(MAIN) == [], menu_load(MAIN)
print("menu branch: UI_ACT_LOADSTATE loads the page's slot, and nothing else "
      "loads as \"menu\"")
print(run_page(UI, True).stdout.strip())
print(run_site(MAIN, CFG, True).stdout.strip())
print(run_cfg(CFG, True).stdout.strip())


def in_fn(signature, pattern, repl):
    """A mutation inside one function only."""
    def mutate(src):
        i = src.find(signature + "\n{")
        body = braced(src, i)
        new = re.sub(pattern, repl, body, count=1)
        assert new != body, pattern
        return src[:i] + new + src[i + len(body):]
    return mutate


def text(old, new):
    def mutate(src):
        assert src.count(old) == 1, old
        return src.replace(old, new)
    return mutate


PAGE_MUTANTS = (
    # (label, mutate(ui_psp.c) -> text, text expected in the failure)
    ("X loads at once, as 4.6.0-beta.2 did",
     in_fn(PAGE, r"g_slot_ask = SLOT_ASK_LOAD;", "act = UI_ACT_LOADSTATE;"),
     "press(PSP_CTRL_CROSS) == UI_ACT_NONE"),
    ("the answer does not load",
     in_fn(PAGE, r"act = UI_ACT_LOADSTATE;", "(void)0;"),
     "UI_ACT_LOADSTATE"),
    ("any key answers the question",
     in_fn(PAGE, r"if \(edges & PSP_CTRL_CROSS\)\n(\s*)\{\n(\s*)int s = g_slot_ask_n",
           r"if (edges)\n\1{\n\2int s = g_slot_ask_n"),
     "press(others[k]) == UI_ACT_NONE"),
    ("a key that keeps the game still acts",
     in_fn(PAGE, r"\n      edges = 0;\n   \}\n", "\n   }\n"),
     "g_state_slot == 1 && !g_slot_ask"),
    ("the question names slot 1 whatever the cursor",
     in_fn(PAGE, r"(g_slot_ask = SLOT_ASK_LOAD;\s*g_slot_ask_n = )g_state_slot;",
           r"\g<1>1;"),
     "strncmp(plate, want, strlen(want))"),
    ("a keep after the quick load stays on the page",
     in_fn(PAGE, r"return UI_ACT_RESUME;", "(void)0;"),
     "frame(others[k]) == UI_ACT_RESUME"),
    ("the page's own question also leaves for the game",
     in_fn(PAGE, r"if \(g_slot_ask_back\)", "if (1)"),
     "press(others[k]) == UI_ACT_NONE"),
    ("a failed quick load leaves for the game on the next keep",
     in_fn(PAGE, r"\n\s*g_slot_ask_back = 0;\s*/\* a failed load stays[^\n]*", ""),
     "g_screen == SCR_STATE_SLOTS"),
    ("the quick load asks for the slot the page was left on",
     in_fn(ASK, r"\n   g_state_slot = slot;", ""),
     "g_state_slot == 1"),
    ("the quick load asks over an empty slot",
     in_fn(ASK, r"\n   if \(!g_ov_slot\[slot - 1\]\)\n      return 0;", ""),
     "ui_state_load_ask(1) == 0 && !g_slot_ask"),
    ("the quick load asks with no menu open",
     in_fn(ASK, r"!g_active \|\| ", ""),
     "ui_state_load_ask(1) == 0 && !g_slot_ask"),
    ("a held direction repeats into an answer",
     in_fn(ASK, r"\n   g_rep_timer = -\(1 << 30\);", ""),
     "frame(PSP_CTRL_RIGHT) == UI_ACT_NONE && g_slot_ask"),
)
for label, mutate, expect in PAGE_MUTANTS:
    bad = run_page(mutate(UI), False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant, page (%s): failed as it must" % label)

ASK_BRANCH = re.compile(r"            else if \(g_pcfg\.quick_load_ask\)\n"
                        r"            \{.*?\n            \}\n", re.S)
SITE_MUTANTS = (
    # (label, mutate(main_psp.c) -> text, text expected in the failure)
    ("the quick load loads at once (4.6.0-beta.2's chord, the ask removed)",
     lambda src: ASK_BRANCH.sub("", src, count=1),
     "evt state_load_ask via=chord"),
    ("the setting read the wrong way round",
     text("            else if (g_pcfg.quick_load_ask)\n",
          "            else if (!g_pcfg.quick_load_ask)\n"),
     "evt state_load_ask via=chord"),
    ("the ask loads as well",
     text("                  menu_open();\n                  if (!ui_state_load_ask(1))",
          "                  menu_open();\n                  psp_state_load(state_path, \"chord\");\n"
          "                  if (!ui_state_load_ask(1))"),
     "evt state_load_ask via=chord"),
    ("the ask is for slot 2",
     text("if (!ui_state_load_ask(1))", "if (!ui_state_load_ask(2))"),
     "ask 1"),
    ("an empty slot 1 still opens the menu",
     text("               if (fe_host_state_recover(state_path) < 0 ||\n"
          "                   !file_exists(state_path))\n"
          "                  osd_toast(\"No saved state\");\n"
          "               else\n",
          "               if (fe_host_state_recover(state_path) < 0)\n"
          "                  osd_toast(\"No saved state\");\n"
          "               else\n"),
     "exists ms0:/r/Ruby.st0"),
    ("a menu that found nothing stays open",
     text("                     ui_close();\n                     osd_toast(\"No saved state\");",
          "                     osd_toast(\"No saved state\");"),
     "ui_close"),
    ("a wireless session no longer refuses the quick load",
     text("            if (link_locks_states())\n"
          "               osd_toast(\"Cannot load during a wireless session\");\n"
          "            /* THE QUICK LOAD ASKS FIRST",
          "            if (link_locks_states() && 0)\n"
          "               osd_toast(\"Cannot load during a wireless session\");\n"
          "            /* THE QUICK LOAD ASKS FIRST"),
     "Cannot load during a wireless session"),
    ("off is not beta 2: the off branch toasts differently",
     text("               fe_evt(\"state_load via=chord\");\n"
          "               osd_toast(\"State loaded\");",
          "               fe_evt(\"state_load via=chord\");\n"
          "               osd_toast(\"Loaded\");"),
     "State loaded"),
    ("off is not beta 2: the off branch stats first",
     text("            else if (psp_state_load(state_path, \"chord\") == 0)\n",
          "            else if (file_exists(state_path) &&\n"
          "                     psp_state_load(state_path, \"chord\") == 0)\n"),
     "load ms0:/r/Ruby.st0 chord"),
)
for label, mutate, expect in SITE_MUTANTS:
    bad = run_site(mutate(MAIN), CFG, False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant, call site (%s): failed as it must" % label)

bad_src = MAIN.replace("(unsigned)ui_state_slot()) == 0\n               ? psp_state_load",
                       "1u) == 0\n               ? psp_state_load", 1)
assert bad_src != MAIN
assert menu_load(bad_src) == ["UI_ACT_LOADSTATE does not load the page's slot"]
print("mutant, menu branch (it loads slot 1, not the page's): seen")
SITE_N = len(SITE_MUTANTS) + 1

CFG_MUTANTS = (
    # (label, mutate(config_psp.c) -> text, text expected in the failure)
    ("the default is off",
     text('"quick_load_ask", 1) ? 1 : 0;', '"quick_load_ask", 0) ? 1 : 0;'),
     'load("scale = 1'),
    ("the key is never read",
     text('(int)fe_ini_get_int(cfg_path, "quick_load_ask", 1) ? 1 : 0;', "1;"),
     "quick_load_ask == 0"),
    ("the key is always written",
     text("      if (!g_pcfg.quick_load_ask ||\n"
          "          fe_ini_get(cfg_path, \"quick_load_ask\", sv, sizeof(sv)))\n",
          "      if (1)\n"),
     'strstr(read_file(), "quick_load_ask") == NULL'),
    ("off is never written",
     text("      if (!g_pcfg.quick_load_ask ||\n"
          "          fe_ini_get(cfg_path, \"quick_load_ask\", sv, sizeof(sv)))\n",
          "      if (fe_ini_get(cfg_path, \"quick_load_ask\", sv, sizeof(sv)))\n"),
     'quick_load_ask = 0\\n'),
    ("a corrupt value is not repaired",
     text("   PCFG_CHK(quick_load_ask,       0, 1, 1);\n", ""),
     "g_pcfg.quick_load_ask == 1"),
)
for label, mutate, expect in CFG_MUTANTS:
    bad = run_cfg(mutate(CFG), False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant, config.ini (%s): failed as it must" % label)
# ...and the call site reads what the config read: a default-off config
# makes the default (no key) quick load load at once.
bad = run_site(MAIN, CFG_MUTANTS[0][1](CFG), False)
assert "g_pcfg.quick_load_ask == 1" in bad.stderr, bad.stderr
print("mutant, config.ini (the default is off): the call site sees it")

print("load asks first: %d page, %d call-site and %d config mutants fail"
      % (len(PAGE_MUTANTS), SITE_N, len(CFG_MUTANTS) + 1))
