#!/usr/bin/env python3
"""Ten slots in five rows: the lists scroll (docs/UI-OVERLAY.md section 15.2).

The SAVE STATES page and the browser's state shelf were drawn for five rows
and keep them; with PSP_STATE_SLOT_COUNT = 10 they show a window of five
that follows the cursor (state_slots.h psp_state_slot_top, the cheats list's
rule), with the 2 px ov_scrollbar beside it.

Taken as written from psp/ui_psp.c: the page's question state, its geometry
(SS_*), screen_state_slots() and browser_state_panel().  Stubbed: drawing
(vid_text / vid_image / ov_scrollbar record what was drawn, and where), the
scan, the dwell read, screen_to / screen_menu, the shelf's preview loader.

  1. The page opens on rows 1-5; the row under the cursor is drawn at its
     own place, with its own slot's thumbnail.
  2. UP from slot 1 is slot 10, and the rows are 6-10, slot 10 in the last
     row.  DOWN from 10 is 1 again, rows 1-5.
  3. Walking down past row 5 scrolls a row at a time; walking back up does
     not jump (the window stays while the cursor is in it).
  4. The scrollbar: drawn, its thumb where the window is.
  5. An action in the scrolled window is the cursor's slot: SQUARE on empty
     slot 7 saves slot 7; TRIANGLE on saved slot 10 asks about slot 10.
  6. The shelf: the same window over its five cards, each card's slot and
     preview its own, the scrollbar right of the cards.
  7. "n of 10": every slot is a row somewhere (all ten reachable, each once
     per lap).
  8. Share game data stays on slots 1-5: psp/ui_share.inc's compile-time
     check, as written, builds with this PSP's ten and refuses a PSP with
     fewer slots than Share's five (fe_share_classify's own refusal of
     .st5-.st9 is tools/tests/test_fe_share.c's).

Then mutants of the production text, each of which must fail.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UI = (ROOT / "psp/ui_psp.c").read_text(encoding="utf-8")

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
    i = source.find(first)
    assert i >= 0, first + " not found"
    j = source.find(last, i)
    assert j >= 0, last + " not found after " + first
    return source[i:source.index("\n", j) + 1]


PAGE = "static ui_action screen_state_slots(unsigned edges)"
SHELF = "static void browser_state_panel(const char *rom_dir, int cur, int n)"


def production(src):
    return "\n".join((
        span(src, "enum { SLOT_ASK_DELETE", "static int  g_slots_dirty;"),
        span(src, "#define SS_", "#define SS_PH "),
        static_fn(src, PAGE),
        static_fn(src, SHELF)))


STUB = r'''
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
enum { C_SEL = 1, C_ITEM, C_DIM, C_VALUE, C_ACCENT, C_CARD, C_BG_TOP,
       C_HDR_BOT };
#define OV_SCRIM 170
#define OV_SHOT_DWELL 6
#define PANEL_FRAMES 8
#define SCR_MENU 0
#define SCR_STATE_SLOTS 5
#define N PSP_STATE_SLOT_COUNT
#define TEX (PSP_STATE_THUMB_WIDTH * PSP_STATE_THUMB_TEX_HEIGHT)
typedef enum { UI_ACT_NONE = 0, UI_ACT_RESUME, UI_ACT_SAVESTATE,
               UI_ACT_LOADSTATE, UI_ACT_OTHER } ui_action;
typedef struct { int unused; } rom_entry;

static char g_state_base[PSP_FILE_PATH_CAP] = "/r/Ruby.st0";
static int g_state_slot = 1, g_state_top, g_screen = SCR_STATE_SLOTS;
static unsigned char g_ov_slot[N];
static char g_ov_stamp[N][24];
static unsigned char g_ov_shot_none[N];
static int g_ov_shot_slot = -1, g_ov_dwell, g_ov_linked;
static uint16_t thumbs[N * TEX];
static uint16_t *g_ov_thumbs = thumbs;
static char g_ov_title[100] = "Ruby";
static int g_active = 1;
static unsigned g_prev_pad;
static int g_rep_timer;
/* the shelf */
static int g_browser_state_open = 1, g_browser_panel_t = PANEL_FRAMES;
static int g_browser_state_slot = 1, g_browser_state_top;
static unsigned char g_browser_state_exists[N];
static unsigned char g_browser_preview_status[N];
static uint16_t previews[N * TEX];
static uint16_t *g_browser_previews = previews;
static int g_shelf_del;
static char g_shelf_note[64];
static rom_entry g_roms[1];
static int deletes, menus;

/* What a frame drew: text with its place, images with theirs. */
static struct { int x, y; char s[64]; } txt[96];
static int n_txt;
static struct { int x, y; const uint16_t *p; } img[32];
static int n_img;
static struct { int x, y, h, pos, view, total; } bar;
static int n_bar;
static char plate[128];

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
{
   (void)w; (void)h; (void)tw; (void)th; (void)sw; (void)sh; (void)a;
   if (n_img < 32) { img[n_img].x = x; img[n_img].y = y; img[n_img++].p = p; }
}
static void vid_text(int x, int y, const char *s, int c)
{
   (void)c;
   if (n_txt < 96)
   {
      txt[n_txt].x = x; txt[n_txt].y = y;
      snprintf(txt[n_txt++].s, sizeof(txt[0].s), "%s", s);
   }
}
static int vid_text_y_in(int y, int h) { return y + h / 2 - 5; }
static void ov_scrollbar(int x, int y, int h, int p, int v, int t)
{
   bar.x = x; bar.y = y; bar.h = h; bar.pos = p; bar.view = v; bar.total = t;
   n_bar++;
}
static void ss_panel(int i) { (void)i; }
static void ov_plate(const char *m) { snprintf(plate, sizeof(plate), "%s", m); }
static void footer(const char *m) { (void)m; }
static int ease_out(int t) { return t; }
static void browser_preview_load_one(const char *d, int c) { (void)d; (void)c; }
static void rom_display_name(const rom_entry *r, char *o, size_t n, int f)
{ (void)r; (void)f; snprintf(o, n, "Ruby"); }
static void clip_title(char *t, int px) { (void)t; (void)px; }

@FUNCTIONS@

static void clear(void) { n_txt = n_img = n_bar = 0; plate[0] = '\0'; }
static ui_action press(unsigned edges)
{
   clear();
   return screen_state_slots(edges);
}
/* y of "Slot k" this frame, or -1 */
static int slot_y(int k)
{
   char l[16];
   int i;
   snprintf(l, sizeof(l), "Slot %d", k);
   for (i = 0; i < n_txt; i++)
      if (!strcmp(txt[i].s, l))
         return txt[i].y;
   return -1;
}
/* the rows drawn: first and last slot, and that they are five in order */
static int rows(int first, int last)
{
   int k, prev = -1;
   for (k = 1; k <= N; k++)
   {
      int y = slot_y(k);
      if ((k >= first && k <= last) != (y >= 0))
         return 0;
      if (y >= 0 && prev >= 0 && y <= prev)
         return 0;
      if (y >= 0)
         prev = y;
   }
   return last - first + 1 == PSP_STATE_SLOT_ROWS;
}
static int thumb_at(int y, const uint16_t *base, int k)
{
   int i;
   for (i = 0; i < n_img; i++)
      if (img[i].y == y && img[i].p == base + (size_t)(k - 1) * TEX)
         return 1;
   return 0;
}

static void page(void)
{
   int i;
   memset(g_ov_slot, 0, sizeof(g_ov_slot));
   for (i = 0; i < N; i++)
      g_ov_stamp[i][0] = '\0';
   g_ov_slot[0] = 2;              /* slots 1, 2, 8 and 10 hold states */
   g_ov_slot[1] = 2;
   g_ov_slot[7] = 2;
   g_ov_slot[9] = 2;
   g_state_slot = 1;
   g_state_top = 0;
   g_screen = SCR_STATE_SLOTS;
   g_slot_ask = g_slot_ask_n = g_slot_saving = 0;
   g_slot_note[0] = '\0';
   deletes = menus = 0;
   press(0);
}

static void shelf(void)
{
   clear();
   browser_state_panel("/r", 0, 1);
}

int main(void)
{
   int k, y1, sy1;

   CHECK(N == 10 && PSP_STATE_SLOT_ROWS == 5);

   /* 1. rows 1-5 at open, each at its row with its own thumbnail */
   page();
   CHECK(rows(1, 5));
   y1 = slot_y(1);
   CHECK(slot_y(2) - y1 == 34 && slot_y(5) - y1 == 4 * 34);
   CHECK(thumb_at(y1 + 5, thumbs, 1) && thumb_at(y1 + 34 + 5, thumbs, 2));

   /* 4. the scrollbar, at the window's top */
   CHECK(n_bar == 1 && bar.pos == 0 && bar.view * 2 == bar.total);
   CHECK(bar.x > 212 && bar.x + 2 <= 228);   /* between rows and panel */

   /* 2. UP from slot 1: slot 10, rows 6-10, slot 10 in the last row */
   CHECK(press(PSP_CTRL_UP) == UI_ACT_NONE && g_state_slot == 10);
   CHECK(rows(6, 10) && slot_y(10) == y1 + 4 * 34 && slot_y(6) == y1);
   CHECK(thumb_at(y1 + 5 + 4 * 34, thumbs, 10));
   CHECK(thumb_at(y1 + 5 + 2 * 34, thumbs, 8));
   CHECK(bar.pos * 2 == bar.total);           /* the bottom half */
   /* 5. TRIANGLE there is slot 10's */
   CHECK(press(PSP_CTRL_TRIANGLE) == UI_ACT_NONE && g_slot_ask_n == 10);
   CHECK(strstr(plate, "Slot 10?") != NULL);
   press(PSP_CTRL_CIRCLE);
   CHECK(press(PSP_CTRL_DOWN) == UI_ACT_NONE && g_state_slot == 1 && rows(1, 5));

   /* 3. a row at a time down, no jump back up */
   page();
   for (k = 0; k < 5; k++)
      press(PSP_CTRL_DOWN);
   CHECK(g_state_slot == 6 && rows(2, 6) && slot_y(6) == y1 + 4 * 34);
   press(PSP_CTRL_DOWN);
   CHECK(g_state_slot == 7 && rows(3, 7));
   /* 5. SQUARE on empty slot 7 saves slot 7 */
   CHECK(press(PSP_CTRL_SQUARE) == UI_ACT_SAVESTATE && g_slot_saving == 7);
   press(PSP_CTRL_UP);
   press(PSP_CTRL_UP);
   CHECK(g_state_slot == 5 && rows(3, 7));    /* the window stays */
   press(PSP_CTRL_UP);
   press(PSP_CTRL_UP);
   CHECK(g_state_slot == 3 && rows(3, 7));
   press(PSP_CTRL_UP);
   CHECK(g_state_slot == 2 && rows(2, 6));    /* ...until it must move */

   /* 7. every slot once a lap, each drawn while the cursor is on it */
   page();
   for (k = 1; k <= 2 * N; k++)
   {
      int s = (k - 1) % N + 1;
      CHECK(g_state_slot == s && slot_y(s) >= 0);
      press(PSP_CTRL_DOWN);
   }

   /* 6. the shelf: its window over five cards */
   memset(g_browser_state_exists, 0, sizeof(g_browser_state_exists));
   memset(g_browser_preview_status, 0, sizeof(g_browser_preview_status));
   g_browser_state_exists[8] = g_browser_preview_status[8] = 1;   /* slot 9 */
   g_browser_state_slot = 1;
   g_browser_state_top = 0;
   shelf();
   CHECK(rows(1, 5) && slot_y(2) - slot_y(1) == 35);
   sy1 = slot_y(1);
   CHECK(n_bar == 1 && bar.pos == 0 && bar.view * 2 == bar.total);
   g_browser_state_slot = 10;                 /* the browser's UP from 1 */
   shelf();
   CHECK(rows(6, 10) && slot_y(6) == sy1 && slot_y(10) == sy1 + 4 * 35);
   {
      int y9 = slot_y(9), i, found = 0;
      for (i = 0; i < n_img; i++)
         if (img[i].p == previews + (size_t)8 * TEX &&
             img[i].y > y9 - 35 && img[i].y <= y9)
            found = 1;
      CHECK(found && n_img == 1);             /* slot 9's preview, in its card */
   }
   CHECK(bar.pos * 2 == bar.total && bar.x > txt[0].x);
   g_browser_state_slot = 5;
   shelf();
   CHECK(rows(5, 9));                         /* back up a row at a time */

   puts("ten slots, five rows: the page and the shelf scroll with the "
        "cursor, every row its own slot");
   return 0;
}
'''


def run(functions, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-sspage-") as folder:
        cfile = Path(folder) / "sp.c"
        binary = Path(folder) / "sp"
        cfile.write_text(STUB.replace("@FUNCTIONS@", functions))
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-unused-variable",
                        "-Wno-unused-but-set-variable",
                        # the page's "Slot %d  -  %s" caption: gcc 14 counts
                        # a 24-byte stamp into its 48-byte buffer as a maybe
                        "-Wno-format-truncation",
                        "-I" + str(ROOT / "psp"),
                        "-I" + str(ROOT / "frontend-common"),
                        str(cfile), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, result.stdout, result.stderr))
        return result


print(run(production(UI), True).stdout.strip())

# 8. Share's five within this PSP's slots, at compile time.
SHARE = (ROOT / "psp/ui_share.inc").read_text(encoding="utf-8")
FIT = re.search(r"^typedef char share_slots_fit\[[^\n]*$", SHARE, re.M)
assert FIT, "ui_share.inc's FE_SHARE_SLOTS <= PSP_STATE_SLOT_COUNT check is gone"


def share_fit(prelude):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-sharefit-") as folder:
        c = Path(folder) / "f.c"
        c.write_text(prelude + '#include "fe_share.h"\n#include "state_slots.h"\n'
                     + FIT.group(0) + "\nint main(void) { return (int)sizeof"
                     "(share_slots_fit) - 1; }\n")
        return subprocess.run(["cc", "-std=gnu99", "-fsyntax-only",
                               "-I" + str(ROOT / "psp"),
                               "-I" + str(ROOT / "frontend-common"), str(c)],
                              capture_output=True, text=True).returncode


assert share_fit("") == 0, "Share's slots must fit this PSP's"
# a PSP with four slots (state_slots.h's count, read first and kept)
assert share_fit("#include \"state_slots.h\"\n#undef PSP_STATE_SLOT_COUNT\n"
                 "#define PSP_STATE_SLOT_COUNT 4\n") != 0, \
    "ui_share.inc must refuse a PSP with fewer slots than Share"
print("share: FE_SHARE_SLOTS (5) <= PSP_STATE_SLOT_COUNT (10) at compile time; "
      "a 4-slot PSP does not build")


def in_fn(signature, pattern, repl):
    def mutate(src):
        i = src.find(signature + "\n{")
        body = braced(src, i)
        new = re.sub(pattern, repl, body, count=1)
        assert new != body, pattern
        return src[:i] + new + src[i + len(body):]
    return mutate


MUTANTS = (
    ("the page does not scroll",
     in_fn(PAGE, r"g_state_top = psp_state_slot_top\([^;]*;", "(void)0;"),
     "rows(6, 10)"),
    ("the page draws every slot (no window)",
     in_fn(PAGE, r"i < PSP_STATE_SLOT_COUNT &&\s*i < g_state_top \+ PSP_STATE_SLOT_ROWS;",
           "i < PSP_STATE_SLOT_COUNT;"),
     "rows(1, 5)"),
    ("a row drawn at its slot's place, not its row's",
     in_fn(PAGE, r"SS_ROW_Y\(i - g_state_top\)", "SS_ROW_Y(i)"),
     "slot_y(6) == y1"),
    ("the thumbnail of the row, not of the slot",
     in_fn(PAGE, r"g_ov_thumbs \+ \(size_t\)i \*", "g_ov_thumbs + (size_t)(i - g_state_top) *"),
     "thumb_at(y1 + 5 + 4 * 34, thumbs, 10)"),
    ("the scrollbar stays at the top",
     in_fn(PAGE, r"g_state_top \* SS_PITCH,", "0,"),
     "bar.pos * 2 == bar.total"),
    ("no scrollbar on the page",
     in_fn(PAGE, r"ov_scrollbar\(SS_BAR_X[^;]*;", "(void)0;"),
     "n_bar == 1"),
    ("the shelf does not scroll",
     in_fn(SHELF, r"g_browser_state_top = psp_state_slot_top\([^;]*;", "(void)0;"),
     "rows(6, 10)"),
    ("the shelf's card at its slot's place",
     in_fn(SHELF, r"\(i - g_browser_state_top\) \* 35", "i * 35"),
     "slot_y(6) == sy1"),
)
for label, mutate, expect in MUTANTS:
    bad = run(production(mutate(UI)), False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant (%s): failed as it must" % label)
print("state page: %d mutants fail" % len(MUTANTS))
