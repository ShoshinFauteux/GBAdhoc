#!/usr/bin/env python3
"""The game list's marks, marquee and position (GitHub issue #31;
docs/UI-OVERLAY.md section 15.7).

psp/ui_psp.c's browser_footer, list_position, marquee_step and browser_marks,
as written, against stubs, in both the cheats build and the one without:

  1. The footer: "✕ play  ◀ states  ▶ cheats  □ star  START settings";
     "◀ states" lit (C_VALUE) only when the game has states, "▶ cheats" only
     when it has a cheats file; everything else C_DIM.
  2. browser_marks reads the game's marks byte (psp/list_marks.h) and
     never stats anything (the 4.6.0-beta.4 list lag): a KNOWN fact lights
     its hint when it is there, an unknown one stays unlit.  Filling the
     byte is tools/tests/test_list_marks_cache.c.
  3. list_position: nothing when the list fits; the thumb at the top for the
     first game, flush with the bottom for the last, inside the track always,
     never shorter than 8 px.
  4. marquee_step: nothing when it fits; rests MARQ_DWELL frames, slides to
     exactly `over`, rests, starts again; a new game starts from 0.

Then mutants, each of which must fail.
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


FOOT = "static void browser_footer(int n_view, int cur_is_fav)"
POS = "static void list_position(int x, int y, int h, int cur, int n, int rows)"
MARQ = "static int marquee_step(int rom_idx, int over)"
MARKS = "static void browser_marks(int rom)"


def production(src):
    i = src.find("#define MARQ_DWELL")
    consts = src[i:src.index("\n", src.find("#define MARQ_SPEED", i)) + 1]
    return "\n".join((consts, static_fn(src, FOOT), static_fn(src, POS),
                      static_fn(src, MARQ), static_fn(src, MARKS)))


STUB = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list_marks.h"
#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define PSP_FILE_PATH_CAP 256
#define PSP_ROM_REL_PATH_CAP 128
#define PSP_STATE_SLOT_COUNT 5
#define IDLE_SELECTED 10
#define VID_GLYPH_TRI "\x7F"
#define VID_GLYPH_O   "\x80"
#define VID_GLYPH_X   "\x81"
#define VID_GLYPH_SQ  "\x82"
#define VID_GLYPH_LEFT  "\x85"
#define VID_GLYPH_RIGHT "\x86"
enum { C_DIM = 1, C_VALUE, C_CARD, C_ACCENT_DK };
typedef struct { int st_size; } SceIoStat;
typedef struct { const char *name; unsigned char marks; } rom_entry;
static rom_entry g_roms[2] = { { "Pokemon/Ruby.gba", 0 }, { "Golf.gba", 0 } };
char g_dir_base[128] = "ms0:/PSP/GAME/GBAdhoc";
static int g_browser_state_open, g_browser_favs;
static signed char g_mark_states, g_mark_cheats;
static int g_marq_t, g_marq_idx = -1;
static int settled = 1, stats;
static const char *files[8];          /* what "exists" */

static int scroll_settled(void) { return settled; }
static int sceIoGetstat(const char *p, SceIoStat *st)
{
   int i;
   (void)st;
   stats++;
   for (i = 0; i < 8; i++)
      if (files[i] && !strcmp(files[i], p))
         return 0;
   return -1;
}
static char said[8][64];
static int said_col[8], n_said;
static void vid_text(int x, int y, const char *s, int c)
{ (void)x; (void)y; snprintf(said[n_said], 64, "%s", s); said_col[n_said++] = c; }
static int vid_text_w(const char *s) { return (int)strlen(s) * 7; }
static int rects[4][4], n_rects;
static void vid_rect(int x, int y, int w, int h, int c, int a)
{ (void)a; (void)c; rects[n_rects][0] = x; rects[n_rects][1] = y;
  rects[n_rects][2] = w; rects[n_rects][3] = h; n_rects++; }

@FUNCTIONS@

static int col_of(const char *s)
{
   int i;
   for (i = 0; i < n_said; i++)
      if (!strcmp(said[i], s))
         return said_col[i];
   return -1;
}
static void foot(void) { n_said = 0; browser_footer(10, 0); }

int main(void)
{
   int c, prev, f, top, bot;
   /* 1, 2 */
   memset(files, 0, sizeof(files));
   g_roms[0].marks = 0;                        /* nothing known yet */
   browser_marks(0);
   CHECK(stats == 0 && g_mark_states == -1 && g_mark_cheats == -1);
   foot();
   CHECK(col_of(VID_GLYPH_LEFT " states") == C_DIM);
   CHECK(col_of(VID_GLYPH_X " play") == C_DIM && col_of("START settings") == C_DIM);
   g_roms[0].marks = LM_ST_K | LM_ST | LM_CHT_K | LM_CHT | LM_SAV_K;
   browser_marks(0);
   CHECK(g_mark_states == 1);
   foot();
   CHECK(col_of(VID_GLYPH_LEFT " states") == C_VALUE);
#ifdef GPSP_CHEATS
   CHECK(g_mark_cheats == 1 && col_of(VID_GLYPH_RIGHT " cheats") == C_VALUE);
#else
   CHECK(col_of("L/R page") == C_DIM);
#endif
   CHECK(col_of(VID_GLYPH_SQ " star") == C_DIM);
   g_roms[1].marks = LM_ST_K | LM_CHT_K;        /* known: neither */
   browser_marks(1);
   CHECK(g_mark_states == 0 && g_mark_cheats == 0);
   foot();
   CHECK(col_of(VID_GLYPH_LEFT " states") == C_DIM);
#ifdef GPSP_CHEATS
   CHECK(col_of(VID_GLYPH_RIGHT " cheats") == C_DIM);
   g_roms[1].marks = LM_ST_K | LM_CHT_K | LM_CHT;   /* cheats, no states */
   browser_marks(1);
   CHECK(g_mark_cheats == 1 && g_mark_states == 0);
   foot();
   CHECK(col_of(VID_GLYPH_RIGHT " cheats") == C_VALUE);
   CHECK(col_of(VID_GLYPH_LEFT " states") == C_DIM);
#endif
   g_roms[1].marks = LM_ST;                     /* a value without KNOWN */
   browser_marks(1);
   CHECK(g_mark_states == -1);
   CHECK(stats == 0);                           /* never a stat */

   /* 3 */
   n_rects = 0;
   list_position(300, 40, 200, 0, 7, 7);
   CHECK(n_rects == 0);
   for (c = 0; c < 87; c++)
   {
      n_rects = 0;
      list_position(300, 40, 200, c, 87, 7);
      CHECK(n_rects == 2 && rects[0][1] == 40 && rects[0][3] == 200);
      top = rects[1][1];
      bot = rects[1][1] + rects[1][3];
      CHECK(top >= 40 && bot <= 240 && rects[1][3] >= 8);
      if (c == 0)
         CHECK(top == 40);
      if (c == 86)
         CHECK(bot == 240);
      if (c)
         CHECK(top >= prev);
      prev = top;
   }
   n_rects = 0;
   list_position(300, 40, 200, 999, 1000, 7);
   CHECK(rects[1][3] == 8 && rects[1][1] + 8 <= 240);

   /* 4 */
   CHECK(marquee_step(3, 0) == 0 && marquee_step(3, -5) == 0);
   g_marq_idx = -1;
   for (f = 0; f < MARQ_DWELL; f++)
      CHECK(marquee_step(5, 30) == 0);
   prev = 0;
   for (f = 0; f < 30 * MARQ_SPEED + MARQ_DWELL - 1; f++)
   {
      c = marquee_step(5, 30);
      CHECK(c >= prev && c <= 30);
      prev = c;
   }
   CHECK(prev == 30);
   for (f = 0; f < 2 * MARQ_DWELL + 30 * MARQ_SPEED; f++)
      if (marquee_step(5, 30) == 0)
         break;
   CHECK(f < 2 * MARQ_DWELL + 30 * MARQ_SPEED);   /* it starts again */
   marquee_step(5, 30);
   CHECK(marquee_step(6, 30) == 0 && g_marq_t <= 1);  /* a new game */
   puts("list: marks lit only when known and there, never a stat; the "
        "position bar spans first to last; the marquee rests, slides, rests");
   return 0;
}
'''

CC = ["cc", "-I" + str(ROOT / "psp"), "-std=gnu99", "-Wall", "-Wextra", "-Werror",
      "-Wno-unused-function", "-Wno-unused-variable", "-Wno-format-truncation",
      "-Wno-maybe-uninitialized"]


def run(ui, expect_pass, cheats):
    code = STUB.replace("@FUNCTIONS@", production(ui))
    with tempfile.TemporaryDirectory(prefix="gbadhoc-list-") as folder:
        c = Path(folder) / "t.c"
        b = Path(folder) / "t"
        c.write_text(code)
        subprocess.run(CC + (["-DGPSP_CHEATS"] if cheats else []) +
                       [str(c), "-o", str(b)], check=True)
        r = subprocess.run([str(b)], capture_output=True, text=True)
        if (r.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s cheats=%s\n%s%s" % (
                expect_pass, cheats, r.stdout, r.stderr))
        return r


def in_fn(signature, old, new):
    def mutate(src):
        i = src.find(signature + "\n{")
        body = braced(src, i)
        assert body.count(old) >= 1, old
        new_body = body.replace(old, new, 1)
        return src[:i] + new_body + src[i + len(body):]
    return mutate


print(run(UI, True, True).stdout.strip())
print(run(UI, True, False).stdout.strip() + " (no cheats build)")

MUTANTS = (
    ("states always lit", in_fn(FOOT, "if (g_mark_states > 0)", "if (1)"),
     "C_DIM", True),
    ("cheats hint lit by states", in_fn(FOOT, "if (g_mark_cheats > 0)", "if (g_mark_states > 0)"),
     "cheats", True),
    ("unknown states read as there", in_fn(MARKS, "(m & LM_ST) != 0) : -1;", "(m & LM_ST) != 0) : 1;"),
     "g_mark_states == -1", True),
    ("cheats from the states bit", in_fn(MARKS, "(m & LM_CHT) != 0", "(m & LM_ST) != 0"),
     "g_mark_cheats", True),
    ("states without the KNOWN bit", in_fn(MARKS, "(m & LM_ST_K) ?", "(m & (LM_ST_K | LM_ST)) ?"),
     "g_mark_states == -1", True),
    ("the thumb overruns the bottom", in_fn(POS, "ty = (h - th) * cur / (n - 1);", "ty = h * cur / n;"),
     "bot", True),
    ("the bar on a list that fits", in_fn(POS, "if (n <= rows || n < 2)", "if (n < 2)"),
     "n_rects == 0", True),
    ("no rest before the slide", in_fn(MARQ, "   if (t < MARQ_DWELL)\n      return 0;\n", ""),
     "marquee_step(5, 30) == 0", True),
)
for label, mutate, expect, cheats in MUTANTS:
    bad = run(mutate(UI), False, cheats)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant (%s): failed as it must" % label)
print("list marks: %d mutants fail" % len(MUTANTS))
