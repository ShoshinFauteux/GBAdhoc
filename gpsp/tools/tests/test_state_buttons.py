#!/usr/bin/env python3
"""The save-state buttons (GitHub issue #31, MXPHunter; 4.6.0-beta.4).

In game, the SAVE STATES page:   X save    SQUARE load    TRIANGLE delete
The game list's state shelf:     X load                   TRIANGLE delete

Until 4.6.0-beta.3 the page was X load / SQUARE save and the shelf deleted
with SQUARE.  Every question stays: a save over a saved slot asks to
overwrite, a load asks (PR #32), a delete asks; X answers each, every other
key keeps.

PART A, the page.  psp/ui_psp.c's screen_state_slots() and ss_panel(), as
written, against stubs that record what was drawn:

  1. An empty slot: X saves it at once (UI_ACT_SAVESTATE, that slot);
     SQUARE says "Slot n is empty" and loads nothing; TRIANGLE does nothing.
  2. A saved slot: X asks "Overwrite Slot n?" and saves only on X;
     SQUARE asks "Load Slot n?" and loads only on X; TRIANGLE asks "Delete
     Slot n?" and deletes only on X; O on any question keeps.
  3. What the page says: the footer names X save, SQUARE load, TRIANGLE
     delete on a saved slot and X save alone on an empty one; the empty
     slot's panel says "X save".

PART B, the shelf.  ui_browser()'s `if (g_browser_state_open) {...}` block,
as written, against stubs:

  4. X on a saved slot launches the game with that slot (returns, slot and
     console handed back); on an empty slot it says so and launches nothing.
  5. TRIANGLE on a saved slot asks "Delete Slot n?"; X deletes that slot's
     files; any other key keeps; TRIANGLE on an empty slot asks nothing.
  6. SQUARE is nothing on the shelf: no question, no delete, no launch.
  7. While the delete question is up, X answers it and does not launch.

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
PANEL = "static void ss_panel(int i)"
SHELF_HEAD = ("      if (g_browser_state_open)\n      {\n"
              "         char state_path[PSP_FILE_PATH_CAP];")


def page_src(src):
    return "\n".join((
        span(src, "enum { SLOT_ASK_DELETE", "static int  g_slots_dirty;"),
        span(src, "#define SS_", "#define SS_PH "),
        static_fn(src, PANEL),
        static_fn(src, PAGE)))


def shelf_src(src):
    i = src.find(SHELF_HEAD)
    assert i >= 0, "the shelf's block is gone"
    return braced(src, i)


CC = ["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
      "-Wno-unused-function", "-Wno-unused-variable",
      "-Wno-unused-but-set-variable", "-Wno-format-truncation",
      "-I" + str(ROOT / "psp"), "-I" + str(ROOT / "frontend-common")]

COMMON = r'''
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
enum { C_SEL = 1, C_ITEM, C_DIM, C_VALUE, C_ACCENT, C_CARD, C_ACCENT_DK,
       C_SHADOW, C_BG_TOP, C_HDR_BOT };
static char drawn[64][96];
static int n_drawn;
static void vid_text(int x, int y, const char *s, int c)
{
   (void)x; (void)y; (void)c;
   if (n_drawn < 64)
      snprintf(drawn[n_drawn++], sizeof(drawn[0]), "%s", s);
}
static int was_drawn(const char *s)
{
   int i;
   for (i = 0; i < n_drawn; i++)
      if (!strcmp(drawn[i], s))
         return 1;
   return 0;
}
static void vid_rect(int x, int y, int w, int h, int c, int a)
{ (void)x; (void)y; (void)w; (void)h; (void)c; (void)a; }
static int vid_text_w(const char *s) { return (int)strlen(s) * 7; }
static int vid_text_y_in(int y, int h) { return y + h / 2; }
'''

PAGE_STUB = COMMON + r'''
#define OV_SCRIM 170
#define OV_SHOT_DWELL 6
#define SCR_MENU 0
#define SCR_STATE_SLOTS 5
#define N PSP_STATE_SLOT_COUNT
#define PSP_STATE_THUMB_WIDTH 64
#define PSP_STATE_THUMB_HEIGHT 42
#define PSP_STATE_THUMB_TEX_HEIGHT 64
typedef enum { UI_ACT_NONE = 0, UI_ACT_RESUME, UI_ACT_SAVESTATE,
               UI_ACT_LOADSTATE, UI_ACT_OTHER } ui_action;

static char g_state_base[PSP_FILE_PATH_CAP] = "/r/Ruby.st0";
static int g_state_slot = 1, g_screen = SCR_STATE_SLOTS;
static unsigned char g_ov_slot[PSP_STATE_SLOT_COUNT];
static char g_ov_stamp[PSP_STATE_SLOT_COUNT][24];
static unsigned char g_ov_shot_none[PSP_STATE_SLOT_COUNT];
static int g_ov_shot_slot = -1, g_ov_dwell, g_ov_linked;
static int g_ov_shot_w = 240, g_ov_shot_h = 160;
static uint16_t *g_ov_thumbs;
static char g_ov_title[100] = "Ruby";
static int g_active = 1;
static int deletes, menus;
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
static void vid_shot_draw(int x, int y, int w, int h)
{ (void)x; (void)y; (void)w; (void)h; }
static void ov_backdrop(int a) { (void)a; }
static void ov_head(const char *t, const char *r) { (void)t; (void)r; }
static void ov_outline(int x, int y, int w, int h, int c, int a)
{ (void)x; (void)y; (void)w; (void)h; (void)c; (void)a; }
static void vid_image(int x, int y, int w, int h, const uint16_t *p, int tw,
                      int th, int sw, int sh, int a)
{ (void)x; (void)y; (void)w; (void)h; (void)p; (void)tw; (void)th;
  (void)sw; (void)sh; (void)a; }
static void vid_image_nearest(int x, int y, int w, int h, const uint16_t *p,
                              int tw, int th, int sw, int sh)
{ (void)x; (void)y; (void)w; (void)h; (void)p; (void)tw; (void)th;
  (void)sw; (void)sh; }
static void ov_plate(const char *m) { snprintf(plate, sizeof(plate), "%s", m); }
static void footer(const char *m) { snprintf(foot, sizeof(foot), "%s", m); }

@FUNCTIONS@

static ui_action press(unsigned edges)
{
   n_drawn = 0;
   plate[0] = foot[0] = '\0';
   return screen_state_slots(edges);
}

static void page(int slot1_saved)
{
   memset(g_ov_slot, 0, sizeof(g_ov_slot));
   g_ov_slot[0] = (unsigned char)slot1_saved;
   g_state_slot = 1;
   g_screen = SCR_STATE_SLOTS;
   g_slot_ask = g_slot_ask_n = g_slot_saving = g_slot_ask_back = 0;
   g_slot_note[0] = '\0';
   deletes = menus = 0;
   press(0);
}

int main(void)
{
   /* 1. an empty slot */
   page(0);
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_SAVESTATE && g_slot_saving == 1);
   CHECK(!g_slot_ask);
   page(0);
   CHECK(press(PSP_CTRL_SQUARE) == UI_ACT_NONE && !g_slot_ask);
   CHECK(!strcmp(plate, "Slot 1 is empty") && !g_slot_saving);
   page(0);
   CHECK(press(PSP_CTRL_TRIANGLE) == UI_ACT_NONE && !g_slot_ask && !deletes);

   /* 2. a saved slot: every button asks, X answers, O keeps */
   page(1);
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_NONE && !g_slot_saving);
   CHECK(!strncmp(plate, "Overwrite Slot 1?", 17));
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_SAVESTATE && g_slot_saving == 1);
   page(1);
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_NONE);
   CHECK(press(PSP_CTRL_CIRCLE) == UI_ACT_NONE && !g_slot_saving && !menus);
   page(1);
   CHECK(press(PSP_CTRL_SQUARE) == UI_ACT_NONE);
   CHECK(!strncmp(plate, "Load Slot 1? Unsaved progress is lost.", 38));
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_LOADSTATE && g_state_slot == 1);
   page(1);
   CHECK(press(PSP_CTRL_SQUARE) == UI_ACT_NONE);
   CHECK(press(PSP_CTRL_SQUARE) == UI_ACT_NONE && !g_slot_ask);  /* keeps */
   page(1);
   CHECK(press(PSP_CTRL_TRIANGLE) == UI_ACT_NONE && !strncmp(plate, "Delete Slot 1?", 14));
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_NONE && deletes == 1 && !g_ov_slot[0]);

   /* 3. what it says */
   page(1);
   CHECK(!strcmp(foot, VID_GLYPH_X " save     " VID_GLYPH_SQ " load     "
                       VID_GLYPH_TRI " delete     " VID_GLYPH_O " back"));
   page(0);
   CHECK(!strcmp(foot, VID_GLYPH_X " save     " VID_GLYPH_O " back"));
   CHECK(was_drawn(VID_GLYPH_X " save") && !was_drawn(VID_GLYPH_SQ " save"));

   puts("page: X saves (asks over a saved slot), SQUARE loads (asks), "
        "TRIANGLE deletes (asks); the footer and panel say so");
   return 0;
}
'''

SHELF_STUB = COMMON + r'''
typedef struct { int st_size; } SceIoStat;
typedef struct { const char *name; } rom_entry;
static struct { int console; } g_pcfg = { 2 };
static rom_entry g_roms[1] = { { "Ruby.gba" } };
static int g_browser_state_open, g_browser_state_slot = 1, g_shelf_del;
static int g_mark_rom = -1;           /* the list marks, dropped on a delete */
static char g_shelf_note[32];
static unsigned char g_browser_state_exists[PSP_STATE_SLOT_COUNT];
static unsigned char g_browser_preview_status[PSP_STATE_SLOT_COUNT];
static int deletes, deleted_slot, launches, toasts;
static char toast[64];

static int browser_rom_state_path(const char *d, int r, unsigned s, char *o,
                                  size_t n)
{ (void)d; (void)r; snprintf(o, n, "slot%u", s); return 0; }
/* claude/folders: the shelf's delete names the slot's base path (the states/
 * mirror or the legacy one) and state_delete adds the slot suffixes. */
static int browser_rom_state_base(const char *d, int r, unsigned s, char *o,
                                  size_t n)
{ (void)d; (void)r; snprintf(o, n, "slot%u", s); return 0; }
static int state_delete(const char *p, unsigned s)
{ (void)p; deletes++; deleted_slot = (int)s;
  g_browser_state_exists[s - 1] = 0; return 0; }
static int browser_rom_has_state(const char *d, int r)
{
   int i, any = 0;
   (void)d; (void)r;
   for (i = 0; i < PSP_STATE_SLOT_COUNT; i++)
      any |= g_browser_state_exists[i];
   return any;
}
static int sceIoGetstat(const char *p, SceIoStat *st)
{
   int s = p[4] - '0';
   (void)st;
   return s >= 1 && s <= PSP_STATE_SLOT_COUNT &&
          g_browser_state_exists[s - 1] ? 0 : -1;
}
static void launch_art_from_browser(int r) { (void)r; }
static void ui_loading_begin(const char *p) { (void)p; launches++; }
static int pcfg_remember_rom(const char *n) { (void)n; return 0; }
static void fe_log(const char *f, ...) { (void)f; }
static void art_free_all(void) {}
static void browser_state_cache_free(void) {}
static void osd_toast(const char *m)
{ toasts++; snprintf(toast, sizeof(toast), "%s", m); }

static int picked_slot;
static fe_console_t picked_console;

/* One frame of the shelf: 0 = it launched the game. */
static int shelf(unsigned edges)
{
   const char *rom_dir = "ms0:/roms";
   int rom = 0, idle = 5;
   char outbuf[PSP_FILE_PATH_CAP], *out = outbuf;
   size_t out_sz = sizeof(outbuf);
   int *out_state_slot = &picked_slot;
   fe_console_t *console_out = &picked_console;
@SHELF@
   (void)idle;
   return 1;
}

static void open_shelf(void)
{
   memset(g_browser_state_exists, 0, sizeof(g_browser_state_exists));
   g_browser_state_exists[0] = 1;      /* slots 1 and 3 hold states */
   g_browser_state_exists[2] = 1;
   g_browser_state_open = 1;
   g_browser_state_slot = 1;
   g_shelf_del = 0;
   g_shelf_note[0] = '\0';
   deletes = deleted_slot = launches = toasts = 0;
   picked_slot = 0;
   picked_console = (fe_console_t)0;
}

int main(void)
{
   static const unsigned keep[] = { PSP_CTRL_CIRCLE, PSP_CTRL_SQUARE,
      PSP_CTRL_TRIANGLE, PSP_CTRL_UP, PSP_CTRL_DOWN, PSP_CTRL_START };
   unsigned k;

   /* 4. X loads the highlighted slot: the game launches with it */
   open_shelf();
   g_browser_state_slot = 3;
   CHECK(shelf(PSP_CTRL_CROSS) == 0 && launches == 1);
   CHECK(picked_slot == 3 && (int)picked_console == 2 && !deletes);
   open_shelf();
   g_browser_state_slot = 2;            /* empty */
   CHECK(shelf(PSP_CTRL_CROSS) == 1 && !launches && toasts == 1);

   /* 5. TRIANGLE asks; X deletes that slot; anything else keeps */
   open_shelf();
   g_browser_state_slot = 3;
   CHECK(shelf(PSP_CTRL_TRIANGLE) == 1 && g_shelf_del == 3 && !deletes);
   CHECK(shelf(PSP_CTRL_CROSS) == 1 && deletes == 1 && deleted_slot == 3);
   CHECK(!launches && !g_shelf_del && g_browser_state_open);
   CHECK(!strcmp(g_shelf_note, "Slot 3 deleted"));
   for (k = 0; k < sizeof(keep) / sizeof(keep[0]); k++)
   {
      open_shelf();
      CHECK(shelf(PSP_CTRL_TRIANGLE) == 1 && g_shelf_del == 1);
      CHECK(shelf(keep[k]) == 1 && !g_shelf_del && !deletes && !launches);
      CHECK(g_browser_state_open && g_browser_state_slot == 1);
   }
   open_shelf();
   g_browser_state_slot = 2;            /* empty: nothing to ask */
   CHECK(shelf(PSP_CTRL_TRIANGLE) == 1 && !g_shelf_del && !deletes);

   /* 6. SQUARE is nothing on the shelf */
   open_shelf();
   CHECK(shelf(PSP_CTRL_SQUARE) == 1 && !g_shelf_del && !deletes && !launches);
   CHECK(g_browser_state_open);

   /* 7. the last slot deleted closes the shelf; X on the question never
    * launches */
   open_shelf();
   g_browser_state_exists[2] = 0;
   CHECK(shelf(PSP_CTRL_TRIANGLE) == 1 && g_shelf_del == 1);
   CHECK(shelf(PSP_CTRL_CROSS) == 1 && deletes == 1 && !launches);
   CHECK(!g_browser_state_open);

   puts("shelf: X loads the slot, TRIANGLE deletes it (asks), SQUARE is "
        "nothing; a question's X never launches");
   return 0;
}
'''


def compile_run(code, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-stbtn-") as folder:
        cfile = Path(folder) / "t.c"
        binary = Path(folder) / "t"
        cfile.write_text(code)
        subprocess.run(CC + [str(cfile), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, result.stdout, result.stderr))
        return result


def run_page(ui, expect_pass):
    return compile_run(PAGE_STUB.replace("@FUNCTIONS@", page_src(ui)),
                       expect_pass)


def run_shelf(ui, expect_pass):
    return compile_run(SHELF_STUB.replace("@SHELF@", shelf_src(ui)),
                       expect_pass)


def text(old, new):
    def mutate(src):
        assert src.count(old) == 1, old
        return src.replace(old, new)
    return mutate


def in_block(head, pattern, repl):
    def mutate(src):
        i = src.find(head)
        assert i >= 0, head
        body = braced(src, i)
        new = re.sub(pattern, repl, body, count=1)
        assert new != body, pattern
        return src[:i] + new + src[i + len(body):]
    return mutate


def swap_page(src):
    """The page as 4.6.0-beta.3 had it: X load, SQUARE save."""
    i = src.find(PAGE + "\n{")
    body = braced(src, i)
    new = (body.replace("if (edges & PSP_CTRL_CROSS)\n      {\n         if (g_ov_slot[cur])",
                        "if (edges & PSP_CTRL_TMP)\n      {\n         if (g_ov_slot[cur])", 1)
           .replace("else if (edges & PSP_CTRL_SQUARE)\n      {\n         if (g_ov_slot[cur])",
                    "else if (edges & PSP_CTRL_CROSS)\n      {\n         if (g_ov_slot[cur])", 1)
           .replace("PSP_CTRL_TMP", "PSP_CTRL_SQUARE", 1))
    assert new != body
    return src[:i] + new + src[i + len(body):]


print(run_page(UI, True).stdout.strip())
print(run_shelf(UI, True).stdout.strip())

PAGE_MUTANTS = (
    ("the beta.3 page: X loads, SQUARE saves", swap_page,
     "UI_ACT_SAVESTATE"),
    ("X saves over a saved slot without asking",
     in_block(PAGE + "\n{", r"g_slot_ask = SLOT_ASK_OVERWRITE;",
              "act = UI_ACT_SAVESTATE; g_slot_saving = g_state_slot;"),
     "UI_ACT_NONE && !g_slot_saving"),
    ("SQUARE on an empty slot saves",
     in_block(PAGE + "\n{", r'snprintf\(g_slot_note, sizeof\(g_slot_note\),\n\s*"Slot %d is empty", g_state_slot\);',
              "{ g_slot_saving = g_state_slot; act = UI_ACT_SAVESTATE; }"),
     "UI_ACT_NONE && !g_slot_ask"),
    ("the footer still says X load",
     text('footer(VID_GLYPH_X " save     " VID_GLYPH_SQ " load     "',
          'footer(VID_GLYPH_X " load     " VID_GLYPH_SQ " save     "'),
     "foot"),
    ("the empty slot's footer says SQUARE save",
     text('      footer(VID_GLYPH_X " save     " VID_GLYPH_O " back");',
          '      footer(VID_GLYPH_SQ " save     " VID_GLYPH_O " back");'),
     "foot"),
    ("the empty panel says SQUARE save",
     text('static const char l2[] = VID_GLYPH_X " save";',
          'static const char l2[] = VID_GLYPH_SQ " save";'),
     "was_drawn"),
)
for label, mutate, expect in PAGE_MUTANTS:
    bad = run_page(mutate(UI), False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant, page (%s): failed as it must" % label)

SHELF_MUTANTS = (
    ("the beta.3 shelf: SQUARE deletes",
     in_block(SHELF_HEAD, r"\(edges & PSP_CTRL_TRIANGLE\) &&",
              "(edges & PSP_CTRL_SQUARE) &&"),
     "g_shelf_del == 3"),
    ("TRIANGLE and SQUARE both delete",
     in_block(SHELF_HEAD, r"\(edges & PSP_CTRL_TRIANGLE\) &&",
              "(edges & (PSP_CTRL_TRIANGLE | PSP_CTRL_SQUARE)) &&"),
     "shelf(PSP_CTRL_SQUARE) == 1 && !g_shelf_del"),
    ("TRIANGLE deletes without asking",
     in_block(SHELF_HEAD, r"g_shelf_del = g_browser_state_slot;",
              "state_delete(\"x\", (unsigned)g_browser_state_slot);"),
     "shelf(PSP_CTRL_TRIANGLE) == 1"),
    ("the question's X launches as well",
     in_block(SHELF_HEAD, r"\n\s*edges = 0;\n\s*\}\n\s*if \(edges\)\n",
              "\n         }\n         if (edges)\n"),
     "Slot 3 deleted"),
    ("X on the shelf loads slot 1 whatever the cursor",
     in_block(SHELF_HEAD, r"\*out_state_slot = g_browser_state_slot;",
              "*out_state_slot = 1;"),
     "picked_slot == 3"),
)
for label, mutate, expect in SHELF_MUTANTS:
    bad = run_shelf(mutate(UI), False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant, shelf (%s): failed as it must" % label)

print("state buttons: %d page and %d shelf mutants fail"
      % (len(PAGE_MUTANTS), len(SHELF_MUTANTS)))
