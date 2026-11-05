#!/usr/bin/env python3
"""The quick save can ask before overwriting (GitHub issue #31; docs/UI-OVERLAY.md
section 15.2).

Settings > "Quick save asks first" (config.ini `quick_save_ask`), OFF by
default.  On, the quick save (bind_quicksave, SELECT+L by default) over a
saved slot 1 opens the SAVE STATES page with "Overwrite Slot 1?" on the
plate: X saves and the menu closes back into the game; any other key is
Resume.  An empty slot 1 saves at once.  Off, the quick save is the branch
every release had, call for call.

PART A, the page.  psp/ui_psp.c's question state, pad_edges,
screen_state_slots, ui_state_save_ask and ui_state_save_done, as written:

  1. ui_state_save_ask(1) on a saved slot 1: the page on slot 1, the
     overwrite question up; X returns UI_ACT_SAVESTATE for slot 1, and the
     save's done closes the menu (0: the caller toasts); every other key is
     UI_ACT_RESUME, nothing saved.
  2. An empty slot, a slot out of range, no menu: nothing asked.
  3. A held direction never answers; a fresh press does.
  4. The page's own overwrite (X on a saved slot) still keeps the page open
     and reports on the plate -- the quick save's "back to the game" does not
     leak into it.

PART B, the call site: main_psp.c's quick save / quick load dispatch as
written, with the real ctl_map.c / config_psp.c / fe_util.c:

  5. No key in config.ini: off; SELECT+L saves at once with exactly the
     calls it always made (no stat, no menu).
  6. quick_save_ask = 1 and a state in slot 1: the menu opens and asks;
     nothing is saved by the dispatch itself.  Slot 1 empty: saved at once,
     no menu.  The page found nothing after all: the menu closes and it
     saves.  A wireless session still refuses.  The quick load is untouched.

PART C, CONFIG.INI: no key is off and writes no key, twice byte for byte;
1 round-trips; 0 written back once present; garbage is off; a corrupt
in-memory value is repaired to off.

Then mutants, each of which must fail.
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
    i = source.find(first)
    assert i >= 0, first + " not found"
    j = source.find(last, i)
    assert j >= 0, last + " not found after " + first
    return source[i:source.index("\n", j) + 1]


PAGE = "static ui_action screen_state_slots(unsigned edges)"
ASK = "int ui_state_save_ask(int slot)"
DONE = "int ui_state_save_done(int rc)"
EDGES = "static unsigned pad_edges(unsigned pad)"


def production(src):
    return "\n".join((
        span(src, "enum { SLOT_ASK_DELETE", "static int  g_slots_dirty;"),
        span(src, "#define SS_", "#define SS_PH "),
        static_fn(src, EDGES),
        static_fn(src, PAGE),
        static_fn(src, ASK),
        static_fn(src, DONE)))


CC = ["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
      "-Wno-unused-function", "-Wno-unused-variable",
      "-Wno-unused-but-set-variable", "-Wno-format-truncation",
      "-I" + str(ROOT / "psp"), "-I" + str(ROOT / "frontend-common")]

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
static int g_state_slot = 1, g_screen = SCR_STATE_SLOTS;
static unsigned char g_ov_slot[PSP_STATE_SLOT_COUNT];
static char g_ov_stamp[PSP_STATE_SLOT_COUNT][24];
static unsigned char g_ov_shot_none[PSP_STATE_SLOT_COUNT];
static int g_ov_shot_slot = -1, g_ov_dwell, g_ov_linked;
static uint16_t *g_ov_thumbs;
static char g_ov_title[100] = "Ruby";
static int g_active = 1;
static unsigned g_prev_pad;
static int g_rep_timer;
static int deletes, menus, closes;
static char plate[128], foot[128];

static void ov_slots_scan(void) {}
static int state_delete(const char *b, unsigned s)
{ (void)b; deletes++; g_ov_slot[s - 1] = 0; return 0; }
static void screen_to(int s) { g_screen = s; }
static ui_action screen_menu(unsigned e, int l)
{ (void)e; (void)l; menus++; return UI_ACT_NONE; }
static void ui_close(void) { closes++; g_active = 0; }
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
{ (void)x; (void)y; (void)s; (void)c; }
static void ss_panel(int i) { (void)i; }
static void ov_plate(const char *m) { snprintf(plate, sizeof(plate), "%s", m); }
static void footer(const char *m) { snprintf(foot, sizeof(foot), "%s", m); }

@FUNCTIONS@

static ui_action press(unsigned edges)
{
   plate[0] = foot[0] = '\0';
   return screen_state_slots(edges);
}
static ui_action frame(unsigned pad) { return press(pad_edges(pad)); }

static void menu_open(void)
{
   memset(g_ov_slot, 0, sizeof(g_ov_slot));
   g_ov_slot[0] = 1;
   g_ov_slot[1] = 1;
   g_state_slot = 4;
   g_screen = SCR_MENU;
   g_active = 1;
   g_slot_ask = g_slot_ask_n = g_slot_saving = g_slot_ask_back = 0;
   g_slot_save_back = 0;
   g_slot_note[0] = '\0';
   deletes = menus = closes = 0;
   g_prev_pad = 0xFFFFFFFFu;
   g_rep_timer = 0;
}

int main(void)
{
   static const unsigned others[] = { PSP_CTRL_CIRCLE, PSP_CTRL_SQUARE,
      PSP_CTRL_TRIANGLE, PSP_CTRL_UP, PSP_CTRL_DOWN, PSP_CTRL_LEFT,
      PSP_CTRL_RIGHT, PSP_CTRL_START, PSP_CTRL_SELECT, PSP_CTRL_LTRIGGER,
      PSP_CTRL_RTRIGGER };
   unsigned k;
   int f;

   /* 1. the question, X, and the menu closing after the save */
   menu_open();
   CHECK(ui_state_save_ask(1) == 1);
   CHECK(g_screen == SCR_STATE_SLOTS && g_state_slot == 1);
   CHECK(frame(PSP_CTRL_SELECT | PSP_CTRL_LTRIGGER) == UI_ACT_NONE);
   CHECK(!strncmp(plate, "Overwrite Slot 1?", 17));
   CHECK(frame(0) == UI_ACT_NONE && g_slot_ask);
   CHECK(frame(PSP_CTRL_CROSS) == UI_ACT_SAVESTATE && g_slot_saving == 1);
   CHECK(ui_state_save_done(0) == 0 && closes == 1);
   for (k = 0; k < sizeof(others) / sizeof(others[0]); k++)
   {
      menu_open();
      CHECK(ui_state_save_ask(1) == 1);
      CHECK(frame(0) == UI_ACT_NONE);
      CHECK(frame(others[k]) == UI_ACT_RESUME);
      CHECK(!g_slot_ask && !g_slot_saving && !deletes && !menus);
   }

   /* 2. nothing to ask */
   menu_open();
   g_ov_slot[0] = 0;
   CHECK(ui_state_save_ask(1) == 0 && !g_slot_ask);
   CHECK(ui_state_save_ask(0) == 0 && ui_state_save_ask(N + 1) == 0);
   menu_open();
   g_active = 0;
   CHECK(ui_state_save_ask(1) == 0 && !g_slot_ask);

   /* 3. a held direction is no answer */
   menu_open();
   CHECK(ui_state_save_ask(1) == 1);
   for (f = 0; f < 600; f++)
      CHECK(frame(PSP_CTRL_RIGHT) == UI_ACT_NONE && g_slot_ask);
   CHECK(frame(0) == UI_ACT_NONE && frame(PSP_CTRL_CROSS) == UI_ACT_SAVESTATE);

   /* 4. the page's own overwrite stays on the page */
   menu_open();
   screen_to(SCR_STATE_SLOTS);
   g_state_slot = 2;
   g_prev_pad = 0;
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_NONE && g_slot_ask);
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_SAVESTATE && g_slot_saving == 2);
   CHECK(ui_state_save_done(0) == 1 && !closes);
   CHECK(!strcmp(g_slot_note, "Slot 2 saved"));
   /* ...even right after a quick save's question was kept */
   menu_open();
   CHECK(ui_state_save_ask(1) == 1);
   CHECK(frame(0) == UI_ACT_NONE && frame(PSP_CTRL_CIRCLE) == UI_ACT_RESUME);
   menu_open();
   screen_to(SCR_STATE_SLOTS);
   g_state_slot = 1;
   g_prev_pad = 0;
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_NONE);
   CHECK(press(PSP_CTRL_CROSS) == UI_ACT_SAVESTATE);
   CHECK(ui_state_save_done(0) == 1 && !closes);

   puts("page: the quick save's question saves on X and goes back to the "
        "game, keeps on anything else; the page's own overwrite stays");
   return 0;
}
'''

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
void fe_log(const char *fmt, ...) { (void)fmt; }
const char *vid_scale_name(int mode) { (void)mode; return "t"; }
const char *vid_filter_name(int mode) { (void)mode; return "t"; }

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
   say("evt %s", b);
}
#endif
static char state_path[64] = "ms0:/r/Ruby.st0";
static int locked, have_state, recover_rc, ask_rc, save_rc;
static int link_locks_states(void) { say("locks"); return locked; }
static void osd_toast(const char *m) { say("toast %s", m); }
static int fe_host_state_recover(const char *p) { say("recover %s", p); return recover_rc; }
static int file_exists(const char *p) { say("exists %s", p); return have_state; }
static void menu_open(void) { say("menu_open"); }
static int ui_state_load_ask(int s) { say("ask %d", s); return ask_rc; }
static int ui_state_save_ask(int s) { say("save_ask %d", s); return ask_rc; }
static void ui_close(void) { say("ui_close"); }
static int psp_state_load(const char *p, const char *via)
{ say("load %s %s", p, via); return 0; }
static int state_save_slot(const char *p, unsigned s)
{ say("save %s %u", p, s); return save_rc; }

static void dispatch(unsigned ctl_fired)
{
@DISPATCH@
}

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

int main(void)
{
   const unsigned SEL = CTL_SELECT, L = CTL_L, R = CTL_R;

   /* 5. the default: off, the quick save as it always was */
   write_file("scale = 1\n");
   memset(&g_pcfg, 0x5A, sizeof(g_pcfg));
   pcfg_load(ini);
   CHECK(g_pcfg.quick_save_ask == 0);
   locked = 0; have_state = 1; recover_rc = 0; ask_rc = 1; save_rc = 0;
   CHECK(!strcmp(run(SEL | L, L), "locks\nsave ms0:/r/Ruby.st0 1\ntoast State saved\n"));
   save_rc = -1;
   CHECK(!strcmp(run(SEL | L, L), "locks\nsave ms0:/r/Ruby.st0 1\ntoast Save failed\n"));
   save_rc = 0; locked = 1;
   CHECK(!strcmp(run(SEL | L, L), "locks\ntoast Cannot save during a wireless session\n"));
   locked = 0;

   /* 6. on */
   write_file("scale = 1\nquick_save_ask = 1\n");
   pcfg_load(ini);
   CHECK(g_pcfg.quick_save_ask == 1);
   CHECK(!strcmp(run(SEL | L, L),
                 "locks\nrecover ms0:/r/Ruby.st0\nexists ms0:/r/Ruby.st0\n"
                 "evt state_save_ask via=chord\nmenu_open\nsave_ask 1\n"));
   have_state = 0;                      /* empty: saved at once */
   CHECK(!strcmp(run(SEL | L, L),
                 "locks\nrecover ms0:/r/Ruby.st0\nexists ms0:/r/Ruby.st0\n"
                 "save ms0:/r/Ruby.st0 1\ntoast State saved\n"));
   have_state = 1; ask_rc = 0;          /* the page found it empty */
   CHECK(!strcmp(run(SEL | L, L),
                 "locks\nrecover ms0:/r/Ruby.st0\nexists ms0:/r/Ruby.st0\n"
                 "evt state_save_ask via=chord\nmenu_open\nsave_ask 1\n"
                 "ui_close\nsave ms0:/r/Ruby.st0 1\ntoast State saved\n"));
   ask_rc = 1; locked = 1;
   CHECK(!strcmp(run(SEL | L, L), "locks\ntoast Cannot save during a wireless session\n"));
   locked = 0;
   /* the quick load is its own: it still asks its own question */
   CHECK(strstr(run(SEL | R, R), "menu_open\nask 1\n") != NULL);
   remove(ini);
   puts("call site: off saves at once as always; on asks over a saved "
        "slot 1, saves an empty one at once");
   return 0;
}
'''

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
   return g_pcfg.quick_save_ask;
}

int main(void)
{
   static char first[16384];
   CHECK(load("scale = 1\n") == 0);
   pcfg_save();
   CHECK(strstr(read_file(), "quick_save_ask") == NULL);
   snprintf(first, sizeof(first), "%s", read_file());
   CHECK(load(first) == 0);
   pcfg_save();
   CHECK(!strcmp(first, read_file()));
   g_pcfg.quick_save_ask = 1;
   pcfg_save();
   CHECK(strstr(read_file(), "quick_save_ask = 1\n") != NULL);
   CHECK(load(read_file()) == 1);
   g_pcfg.quick_save_ask = 0;
   pcfg_save();
   CHECK(strstr(read_file(), "quick_save_ask = 0\n") != NULL);
   CHECK(load(read_file()) == 0);
   CHECK(load("quick_save_ask = maybe\n") == 0);
   CHECK(load("quick_save_ask = 2\n") == 0);
   CHECK(load("quick_save_ask =\n") == 0);
   CHECK(load("quick_save_ask = 1\n") == 1);
   load("scale = 1\n");
   g_pcfg.quick_save_ask = 0x5A5A;
   pcfg_save();
   CHECK(g_pcfg.quick_save_ask == 0);
   CHECK(strstr(read_file(), "quick_save_ask") == NULL);
   remove(ini);
   puts("config.ini: quick_save_ask is off with no key and writes none; "
        "1 round-trips; garbage is off");
   return 0;
}
'''


def dispatch_block(src):
    i = src.find("         if (ctl_fired & CTL_FIRE_SAVE)\n")
    assert i >= 0, "the quick-save dispatch is gone"
    j = src.find("         else if (ctl_fired & CTL_FIRE_LOAD)\n", i)
    assert j >= 0, "the quick-load dispatch is gone"
    k = src.find("\n         {", j)
    return src[i:k + 1] + braced(src, k + 1)


def compile_run(code, expect_pass, extra=()):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-qsave-") as folder:
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
    with tempfile.TemporaryDirectory(prefix="gbadhoc-qsavecfg-") as folder:
        c = Path(folder) / "config_psp.c"
        c.write_text(cfg)
        return compile_run(code, expect_pass,
                           ("-I" + str(ROOT / "psp"), str(c),
                            str(ROOT / "psp/ctl_map.c"),
                            str(ROOT / "frontend-common/fe_util.c")))


def run_site(main, cfg, expect_pass):
    return run_linked(SITE_STUB.replace("@DISPATCH@", dispatch_block(main)),
                      cfg, expect_pass)


def run_cfg(cfg, expect_pass):
    return run_linked(CFG_TEST, cfg, expect_pass)


def in_fn(signature, pattern, repl):
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


print(run_page(UI, True).stdout.strip())
print(run_site(MAIN, CFG, True).stdout.strip())
print(run_cfg(CFG, True).stdout.strip())

PAGE_MUTANTS = (
    ("the save's done does not close the menu",
     in_fn(DONE, r"\n      ui_close\(\);", ""),
     "closes == 1"),
    ("every save from the page goes back to the game",
     in_fn(DONE, r"if \(back\)", "if (1)"),
     "!closes"),
    ("the quick save's X is not marked",
     in_fn(PAGE, r"g_slot_save_back = back;", "(void)back;"),
     "closes == 1"),
    ("the ask asks over an empty slot",
     in_fn(ASK, r"\n   if \(!g_ov_slot\[slot - 1\]\)\n      return 0;", ""),
     "ui_state_save_ask(1) == 0 && !g_slot_ask"),
    ("a keep stays on the page",
     in_fn(ASK, r"\n   g_slot_ask_back = 1;", ""),
     "closes == 1"),
    ("a held direction answers",
     in_fn(ASK, r"\n   g_rep_timer = -\(1 << 30\);[^\n]*", ""),
     "g_slot_ask"),
    ("the question is the load's",
     in_fn(ASK, r"SLOT_ASK_OVERWRITE", "SLOT_ASK_LOAD"),
     "Overwrite Slot 1?"),
)
for label, mutate, expect in PAGE_MUTANTS:
    bad = run_page(mutate(UI), False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant, page (%s): failed as it must" % label)

SITE_MUTANTS = (
    ("the setting ignored: it always asks",
     text("            else if (g_pcfg.quick_save_ask &&\n",
          "            else if (1 &&\n"),
     "locks\\nsave ms0:/r/Ruby.st0 1"),
    ("the setting read the wrong way round",
     text("            else if (g_pcfg.quick_save_ask &&\n",
          "            else if (!g_pcfg.quick_save_ask &&\n"),
     "run(SEL | L, L)"),
    ("off stats the slot first",
     text("            else if (g_pcfg.quick_save_ask &&\n"
          "                     fe_host_state_recover(state_path) >= 0 &&\n",
          "            else if (fe_host_state_recover(state_path) >= 0 &&\n"
          "                     g_pcfg.quick_save_ask &&\n"),
     "locks\\nsave ms0:/r/Ruby.st0 1"),
    ("an empty slot opens the menu",
     text("                     fe_host_state_recover(state_path) >= 0 &&\n"
          "                     file_exists(state_path))\n",
          "                     fe_host_state_recover(state_path) >= 0)\n"),
     "exists ms0:/r/Ruby.st0"),
    ("the page finding nothing saves nothing",
     text("                  ui_close();\n                  osd_toast(state_save_slot(state_path, 1) == 0\n"
          "                               ? \"State saved\" : \"Save failed\");",
          "                  ui_close();"),
     "ui_close\\nsave"),
)
for label, mutate, expect in SITE_MUTANTS:
    bad = run_site(mutate(MAIN), CFG, False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant, call site (%s): failed as it must" % label)

CFG_MUTANTS = (
    ("the default is on",
     text('fe_ini_get_int(cfg_path, "quick_save_ask", 0) == 1 ? 1 : 0;',
          'fe_ini_get_int(cfg_path, "quick_save_ask", 1) ? 1 : 0;'),
     'load("scale = 1'),
    ("the key is always written",
     text("      if (g_pcfg.quick_save_ask ||\n"
          "          fe_ini_get(cfg_path, \"quick_save_ask\", sv, sizeof(sv)))\n",
          "      if (1)\n"),
     'strstr(read_file(), "quick_save_ask") == NULL'),
    ("on is never written",
     text("      if (g_pcfg.quick_save_ask ||\n"
          "          fe_ini_get(cfg_path, \"quick_save_ask\", sv, sizeof(sv)))\n",
          "      if (fe_ini_get(cfg_path, \"quick_save_ask\", sv, sizeof(sv)))\n"),
     "quick_save_ask = 1"),
    ("a corrupt value is not repaired",
     text("   PCFG_CHK(quick_save_ask,       0, 1, 0);\n", ""),
     "g_pcfg.quick_save_ask == 0"),
)
for label, mutate, expect in CFG_MUTANTS:
    bad = run_cfg(mutate(CFG), False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant, config.ini (%s): failed as it must" % label)

print("quick save asks: %d page, %d call-site and %d config mutants fail"
      % (len(PAGE_MUTANTS), len(SITE_MUTANTS), len(CFG_MUTANTS)))
