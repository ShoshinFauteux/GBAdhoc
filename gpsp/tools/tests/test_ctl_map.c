/* test_ctl_map.c -- user control remapping (psp/ctl_map.c, docs/CONTROL-REMAP.md).
 *
 * Built against the production ctl_map.c, config_psp.c and fe_util.c (the
 * real INI reader/writer), twice: as the harness/telemetry flavour and as the
 * release flavour with the bisect-only catch self-test compiled in.
 *
 *   1. DEFAULTS == 3.0, EXHAUSTIVELY.  Every (previous pad, current pad) pair
 *      over the twelve bindable buttons -- 16.7 million -- plus the
 *      unbindable system bits, compared against a verbatim restatement of the
 *      3.0 tests: the game mask (both A/B layouts) and every shortcut.
 *   2. The table: unique keys, legal and distinct defaults.
 *   3. Parse/format: every legal binding round-trips; garbage is refused.
 *   4. The legality rules, the conflict rule and the load-time repair.
 *   5. CONFIG.INI: missing / empty / garbage keys fall back to the DEFAULT,
 *      NONE unbinds, an old config is untouched by a save, and random tables
 *      survive save+load exactly.  The btn_swap migration: a 3.0 swapped
 *      config loads exactly as 3.0 / the 3.1 candidates loaded it, becomes
 *      explicit bind_a/bind_b on the next save, and from then on the key
 *      changes nothing (including the mirror pcfg_save writes).
 *   6. The remapped behaviours the owner asked for (Triangle unbound, a
 *      steal, a chord shortcut over game buttons).
 *   7. TURBO BUTTONS (section 13): the pulse of each rate frame by frame,
 *      Hold and Toggle, normal A with Turbo A, two Turbo A buttons at
 *      different rates, link play, the stop points, fast-forward's three
 *      frames per pad sample; with nothing bound the input is the pre-turbo
 *      input for every one of the 65,536 pad words; the conflict rule; the
 *      CONFIG.INI keys, their keep rule and a byte-identical default file. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctl_map.h"
#include "config_psp.h"
#include "fe_util.h"

/* CONFIG.INI scratch file (helpers below test_repair). */
static const char *ini = "/tmp/ctl-map-test.ini";
static void write_file(const char *text);

#ifndef GPSP_PLAYABLE
static char last_evt[512];
static int  n_evt;
void fe_evt(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(last_evt, sizeof(last_evt), fmt, ap);
   va_end(ap);
   n_evt++;
}
#endif
void fe_log(const char *fmt, ...) { (void)fmt; }
const char *vid_scale_name(int mode) { (void)mode; return "t"; }
const char *vid_filter_name(int mode) { (void)mode; return "t"; }

static int checks;
#define CHECK(c) do { checks++; if (!(c)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
   exit(1); } } while (0)

#define SEL   CTL_SELECT
#define START CTL_START
#define UP    CTL_UP
#define DOWN  CTL_DOWN
#define LEFT  CTL_LEFT
#define RIGHT CTL_RIGHT
#define L     CTL_L
#define R     CTL_R
#define TRI   CTL_TRIANGLE
#define CIR   CTL_CIRCLE
#define CRO   CTL_CROSS
#define SQ    CTL_SQUARE

/* ---- 3.0, verbatim (psp/main_psp.c at d7df3bb, PSP_CTRL_* -> CTL_*) ----- */
enum { JP_B = 0, JP_SELECT = 2, JP_START = 3, JP_UP = 4, JP_DOWN = 5,
       JP_LEFT = 6, JP_RIGHT = 7, JP_A = 8, JP_L = 10, JP_R = 11 };

static uint32_t legacy_mask(unsigned b, int btn_swap)
{
   uint32_t m = 0;
   if (b & UP)    m |= 1u << JP_UP;
   if (b & DOWN)  m |= 1u << JP_DOWN;
   if (b & LEFT)  m |= 1u << JP_LEFT;
   if (b & RIGHT) m |= 1u << JP_RIGHT;
   if (btn_swap)
   {
      if (b & CRO) m |= 1u << JP_A;
      if (b & CIR) m |= 1u << JP_B;
   }
   else
   {
      if (b & CIR) m |= 1u << JP_A;
      if (b & CRO) m |= 1u << JP_B;
   }
   if (b & L)     m |= 1u << JP_L;
   if (b & R)     m |= 1u << JP_R;
   if (b & START) m |= 1u << JP_START;
   if (b & SEL)   m |= 1u << JP_SELECT;
   return m;
}

static unsigned legacy_fired(unsigned pad, unsigned pad_new)
{
   unsigned f = 0;
   /* fast-forward: hold reads the level, toggle and the lock toast the edge */
   if (pad & SQ)      f |= CTL_FIRE_FF_HELD;
   if (pad_new & SQ)  f |= CTL_FIRE_FF_PRESS;
   /* video preset */
   if (pad_new & TRI) f |= CTL_FIRE_VIDEO;
   /* quick save / load (the state_path / menu / script gates stay in main) */
   if (pad & SEL)
   {
      if ((pad_new & L) && !(pad & R))
         f |= CTL_FIRE_SAVE;
      else if ((pad_new & R) && !(pad & L))
         f |= CTL_FIRE_LOAD;
   }
   /* screenshot (ADR-0069) */
   if ((pad_new & (L | R | SEL)) && (pad & (L | R | SEL)) == (L | R | SEL) &&
       !(pad & START))
      f |= CTL_FIRE_SHOT;
   /* pause (wake) screen */
   if ((pad & SEL) && (pad_new & TRI))
      f |= CTL_FIRE_PAUSE;
#ifdef GPSP_CATCH_SELFTEST
   if ((pad & (L | R | UP | TRI)) == (L | R | UP | TRI))
      f |= CTL_FIRE_CATCH;
#endif
   return f;
}

static unsigned expand(unsigned idx)       /* 12-bit index -> CTL mask */
{
   unsigned m = 0;
   int i;
   for (i = 0; i < CTL_NBUTTONS; i++)
      if (idx & (1u << i))
         m |= ctl_button_order(i);
   return m;
}

static void test_defaults_are_30(void)
{
   /* Bits pad.Buttons carries that are never bindable: HOME, HOLD, WLAN
    * switch, REMOTE, NOTE.  They must change nothing. */
   static const unsigned sys[4] = { 0, 0x010000u, 0x060000u, 0x880000u };
   ctl_map m0, m1;
   unsigned cur, prev;
   unsigned long n = 0;

   /* m1: what a 3.0 config with `btn_swap = 1` loads to now -- the
    * migration, not a base layout, has to reproduce 3.0's swapped chain. */
   ctl_defaults(&m0);
   write_file("btn_swap = 1\n");
   CHECK(ctl_load_ini(&m1, ini) == 0);
   for (cur = 0; cur < 4096; cur++)
   {
      unsigned pad = expand(cur) | sys[cur & 3];
      CHECK(ctl_game_mask(&m0, pad) == legacy_mask(pad, 0));
      CHECK(ctl_game_mask(&m1, pad) == legacy_mask(pad, 1));
      for (prev = 0; prev < 4096; prev++)
      {
         unsigned prevpad = expand(prev) | sys[(prev >> 2) & 3];
         unsigned pad_new = pad & ~prevpad;
         unsigned want = legacy_fired(pad, pad_new);
         if (ctl_shortcuts(&m0, pad, pad_new) != want ||
             ctl_shortcuts(&m1, pad, pad_new) != want)
         {
            fprintf(stderr, "FAIL pad=%05x new=%05x want=%03x got=%03x\n",
                    pad, pad_new, want, ctl_shortcuts(&m0, pad, pad_new));
            exit(1);
         }
         n++;
      }
   }
   checks += (int)(n / 1000);
   printf("  defaults == 3.0 for %lu pad transitions, both A/B layouts "
          "(swapped = migrated btn_swap)\n", n);
}

static void test_table(void)
{
   int a, b;
   for (a = 0; a < CTL_ACTIONS; a++)
   {
      const ctl_action_info *in = ctl_info(a);
      CHECK(in && in->key && in->label);
      CHECK(strncmp(in->key, "bind_", 5) == 0);
      CHECK(strlen(in->key) < 32);
      CHECK(ctl_check(a, in->def) == CTL_OK || !ctl_available(a));
      CHECK(((in->flags & CTL_F_GAME) != 0) == (a < CTL_GAME_COUNT));
      /* turbo buttons: the last rows, unbound, and nothing else is */
      CHECK(((in->flags & CTL_F_TURBO) != 0) ==
            (a >= CTL_TURBO_FIRST && a < CTL_TURBO_FIRST + CTL_TURBO_N));
      CHECK(((in->flags & CTL_F_TURBO) != 0) == (a >= CTL_SC_END));
      if (in->flags & CTL_F_TURBO)
         CHECK(in->def == 0 && !(in->flags & (CTL_F_GAME | CTL_F_YIELD)));
      for (b = a + 1; b < CTL_ACTIONS; b++)
      {
         CHECK(strcmp(in->key, ctl_info(b)->key) != 0);
         /* distinct defaults; "unbound" is the one default rows may share */
         CHECK(in->def != ctl_info(b)->def || !in->def);
      }
   }
   CHECK(ctl_info(-1) == NULL && ctl_info(CTL_ACTIONS) == NULL);
#ifdef GPSP_CATCH_SELFTEST
   CHECK(ctl_available(CTL_SC_CATCH));
#else
   CHECK(!ctl_available(CTL_SC_CATCH));
   CHECK(ctl_default(CTL_SC_CATCH) == 0);
#endif
   /* the menu chord is never anybody's default */
   for (a = 0; a < CTL_ACTIONS; a++)
      CHECK((ctl_default(a) & CTL_MENU_CHORD) != CTL_MENU_CHORD);
}

static void test_parse_format(void)
{
   unsigned idx, m;
   char buf[64];
   static const char *const bad[] = {
      "", " ", "+", "L+", "+L", "L++R", "L+L", "FOO", "L+FOO", "NONE+L",
      "0x100", "256", "L R", "L,R", "SELECT+", "none none", "TRIANGLEX",
      "HOME", "L+R+", "-", "\t",
   };
   unsigned i;

   for (idx = 0; idx < 4096; idx++)
   {
      unsigned mask = expand(idx);
      if (ctl_popcount(mask) > CTL_COMBO_MAX)
         continue;
      ctl_format(mask, buf, sizeof(buf), NULL, NULL);
      CHECK(ctl_parse(buf, &m) == 0 && m == mask);
   }
   CHECK(ctl_parse("NONE", &m) == 0 && m == 0);
   CHECK(ctl_parse("  none \r\n", &m) == 0 && m == 0);
   CHECK(ctl_parse(" select + l ", &m) == 0 && m == (SEL | L));
   CHECK(ctl_parse("L+SELECT", &m) == 0 && m == (SEL | L));
   CHECK(ctl_parse("LTRIGGER+rtrigger", &m) == 0 && m == (L | R));
   CHECK(ctl_parse("Triangle", &m) == 0 && m == TRI);
   for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      m = 0xBEEF;
      CHECK(ctl_parse(bad[i], &m) == -1 && m == 0xBEEF);
   }
   ctl_format(0, buf, sizeof(buf), NULL, NULL);
   CHECK(strcmp(buf, "NONE") == 0);
   ctl_format(L | R | SEL, buf, sizeof(buf), NULL, NULL);
   CHECK(strcmp(buf, "SELECT+L+R") == 0);
   ctl_format(SEL | TRI, buf, sizeof(buf), NULL, "none");
   CHECK(strcmp(buf, "SELECT+TRIANGLE") == 0);
   ctl_format(0, buf, sizeof(buf), NULL, "none");
   CHECK(strcmp(buf, "none") == 0);
   /* never overruns, always terminated */
   ctl_format(SEL | START | L | R, buf, 6, NULL, NULL);
   CHECK(strlen(buf) <= 5);
}

static void test_rules(void)
{
   CHECK(ctl_check(CTL_GAME_A, 0) == CTL_OK);
   CHECK(ctl_check(CTL_GAME_A, TRI) == CTL_OK);
   CHECK(ctl_check(CTL_GAME_A, TRI | SQ) == CTL_E_SINGLE);
   CHECK(ctl_check(CTL_GAME_A, 0x10000) == CTL_E_BITS);
   CHECK(ctl_check(CTL_SC_FF, L | R) == CTL_OK);
   CHECK(ctl_check(CTL_SC_FF, START | SEL) == CTL_E_RESERVED);
   CHECK(ctl_check(CTL_SC_FF, START | SEL | L) == CTL_E_RESERVED);
   CHECK(ctl_check(CTL_SC_FF, START | L) == CTL_OK);
   CHECK(ctl_check(CTL_SC_FF, L | R | UP | DOWN) == CTL_OK);
   CHECK(ctl_check(CTL_SC_FF, L | R | UP | DOWN | LEFT) == CTL_E_TOO_MANY);
   CHECK(ctl_check(-1, 0) == CTL_E_ACTION);
   CHECK(ctl_check(CTL_ACTIONS, 0) == CTL_E_ACTION);
   CHECK(strcmp(ctl_error_text(CTL_E_RESERVED),
                "START+SELECT is reserved for the menu") == 0);
}

static int no_dupes(const ctl_map *m)
{
   int a, b;
   for (a = 0; a < CTL_ACTIONS; a++)
   {
      if (!ctl_available(a))
      {
         if (m->bind[a])
            return 0;
         continue;
      }
      if (ctl_check(a, m->bind[a]) != CTL_OK)
         return 0;
      for (b = a + 1; b < CTL_ACTIONS; b++)
         if (m->bind[a] && m->bind[a] == m->bind[b])
            return 0;
   }
   return 1;
}

static unsigned rng = 12345u;
static unsigned rnd(void)
{
   rng = rng * 1103515245u + 12345u;
   return rng >> 8;
}

static unsigned random_binding(int a)
{
   unsigned m = 0;
   int n = (a < CTL_GAME_COUNT || a >= CTL_SC_END) ? 1
                                                   : 1 + (int)(rnd() % 4), i;
   if (rnd() % 8 == 0)
      return 0;
   for (i = 0; i < n; i++)
      m |= ctl_button_order((int)(rnd() % CTL_NBUTTONS));
   return m;
}

static void test_conflicts(void)
{
   ctl_map m;
   int stolen, i;

   /* The new binding steals; the old holder is left unbound; nothing else
    * moves. */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_GAME_A, SQ, &stolen) == CTL_OK);
   CHECK(stolen == CTL_SC_FF && m.bind[CTL_SC_FF] == 0 && m.bind[CTL_GAME_A] == SQ);
   CHECK(m.bind[CTL_GAME_B] == CRO);
   /* the freed Circle is simply unbound now */
   CHECK(ctl_find(&m, CIR, -1) == -1);
   /* a game button can take a shortcut's single button, and back */
   CHECK(ctl_assign(&m, CTL_SC_FF, SQ, &stolen) == CTL_OK && stolen == CTL_GAME_A);
   CHECK(m.bind[CTL_GAME_A] == 0);
   /* chords conflict only when IDENTICAL, in any order */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_SC_FF, L | SEL, &stolen) == CTL_OK);
   CHECK(stolen == CTL_SC_SAVE && m.bind[CTL_SC_SAVE] == 0);
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_SC_FF, L | R, &stolen) == CTL_OK && stolen == -1);
   CHECK(m.bind[CTL_GAME_L] == L && m.bind[CTL_GAME_R] == R);   /* overlap ok */
   /* refused: the map does not change at all */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_SC_VIDEO, START | SEL, &stolen) == CTL_E_RESERVED);
   CHECK(stolen == -1 && m.bind[CTL_SC_VIDEO] == TRI);
   CHECK(ctl_assign(&m, CTL_GAME_B, L | R, &stolen) == CTL_E_SINGLE);
   CHECK(m.bind[CTL_GAME_B] == CRO && m.bind[CTL_GAME_L] == L);
   /* rebinding to its own input is a no-op, not a self-steal */
   CHECK(ctl_assign(&m, CTL_SC_VIDEO, TRI, &stolen) == CTL_OK && stolen == -1);
   CHECK(m.bind[CTL_SC_VIDEO] == TRI);
   /* unbinding never steals */
   CHECK(ctl_assign(&m, CTL_GAME_A, 0, &stolen) == CTL_OK && stolen == -1);
   CHECK(m.bind[CTL_GAME_A] == 0 && m.bind[CTL_GAME_B] == CRO);

   /* invariant under random edits: legal and duplicate-free, always */
   ctl_defaults(&m);
   for (i = 0; i < 200000; i++)
   {
      int a = (int)(rnd() % CTL_ACTIONS);
      unsigned b = random_binding(a);
      int err = ctl_assign(&m, a, b, &stolen);
      if (err == CTL_OK)
         CHECK(m.bind[a] == (ctl_available(a) ? b : m.bind[a]));
      CHECK(no_dupes(&m));
   }

   /* A/B exchanged by hand is a custom table: there is no second default */
   ctl_defaults(&m);
   CHECK(ctl_is_default(&m) && !ctl_legacy_swap(&m));
   CHECK(ctl_assign(&m, CTL_GAME_A, CRO, &stolen) == CTL_OK && stolen == CTL_GAME_B);
   CHECK(ctl_assign(&m, CTL_GAME_B, CIR, &stolen) == CTL_OK && stolen == -1);
   CHECK(!ctl_is_default(&m) && ctl_legacy_swap(&m));
}

static int notes;
static void note_count(void *u, int a, int w) { (void)u; (void)a; (void)w; notes++; }

static void test_repair(void)
{
   ctl_map m;
   ctl_defaults(&m);
   /* an explicit (non-default) binding beats a default one */
   m.bind[CTL_SC_FF] = TRI;
   CHECK(ctl_repair(&m, NULL, NULL) == 1);
   CHECK(m.bind[CTL_SC_FF] == TRI && m.bind[CTL_SC_VIDEO] == 0);
   /* two explicit ones: the earlier action in the table wins */
   ctl_defaults(&m);
   m.bind[CTL_GAME_A] = L | 0;      /* A on L: steals from game L (default) */
   CHECK(ctl_repair(&m, NULL, NULL) == 1 && m.bind[CTL_GAME_L] == 0);
   ctl_defaults(&m);
   m.bind[CTL_GAME_A] = TRI;
   m.bind[CTL_GAME_B] = TRI;
   notes = 0;
   CHECK(ctl_repair(&m, note_count, NULL) == 2 && notes == 2);
   CHECK(m.bind[CTL_GAME_A] == TRI && m.bind[CTL_GAME_B] == 0 &&
         m.bind[CTL_SC_VIDEO] == 0);
   /* an illegal value returns to its default */
   ctl_defaults(&m);
   m.bind[CTL_GAME_A] = L | R;
   m.bind[CTL_SC_SHOT] = START | SEL;
   CHECK(ctl_repair(&m, NULL, NULL) == 2);
   CHECK(m.bind[CTL_GAME_A] == CIR && m.bind[CTL_SC_SHOT] == (L | R | SEL));
   /* a default table needs nothing */
   ctl_defaults(&m);
   CHECK(ctl_repair(&m, NULL, NULL) == 0);
}

/* ---- CONFIG.INI ----------------------------------------------------------- */
static void write_file(const char *text)
{
   FILE *f = fopen(ini, "w");
   CHECK(f != NULL);
   fputs(text, f);
   fclose(f);
}

static char *read_file(void)
{
   static char buf[8192];
   FILE *f = fopen(ini, "r");
   size_t n;
   CHECK(f != NULL);
   n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = 0;
   fclose(f);
   return buf;
}

static void test_ini(void)
{
   ctl_map m, d;
   int a, i, bad;
   char *t;

   /* missing keys: defaults, and a save writes nothing */
   write_file("scale = 1\nbtn_swap = 0\n");
   CHECK(ctl_load_ini(&m, ini) == 0);
   ctl_defaults(&d);
   CHECK(memcmp(m.bind, d.bind, sizeof(m.bind)) == 0);
   CHECK(ctl_save_ini(&m, ini) == 0);
   CHECK(strcmp(read_file(), "scale = 1\nbtn_swap = 0\n") == 0);

   /* THE btn_swap MIGRATION.  An old swapped config: A on Cross, B on
    * Circle, and the next save makes that explicit (bind_a/bind_b only). */
   write_file("scale = 1\nbtn_swap = 1\n");
#ifndef GPSP_PLAYABLE
   n_evt = 0;
#endif
   CHECK(ctl_load_ini(&m, ini) == 0);
#ifndef GPSP_PLAYABLE
   CHECK(n_evt == 1 && strstr(last_evt, "config_migrate key=btn_swap -> "
                              "bind_a=CROSS bind_b=CIRCLE") != NULL);
#endif
   CHECK(m.bind[CTL_GAME_A] == CRO && m.bind[CTL_GAME_B] == CIR);
   CHECK(!ctl_is_default(&m) && ctl_legacy_swap(&m));
   CHECK(ctl_save_ini(&m, ini) == 0);
   CHECK(strcmp(read_file(), "scale = 1\nbtn_swap = 1\nbind_a = CROSS\n"
                             "bind_b = CIRCLE\n") == 0);
   CHECK(ctl_load_ini(&d, ini) == 0);
   CHECK(memcmp(m.bind, d.bind, sizeof(m.bind)) == 0);
   /* ...after which btn_swap changes nothing, whatever it says */
   write_file("btn_swap = 0\nbind_a = CROSS\nbind_b = CIRCLE\n");
   CHECK(ctl_load_ini(&d, ini) == 0);
   CHECK(memcmp(m.bind, d.bind, sizeof(m.bind)) == 0);
   write_file("btn_swap = 1\nbind_a = CIRCLE\nbind_b = CROSS\n");
#ifndef GPSP_PLAYABLE
   n_evt = 0;
#endif
   CHECK(ctl_load_ini(&d, ini) == 0 && ctl_is_default(&d));
#ifndef GPSP_PLAYABLE
   CHECK(n_evt == 0);
#endif
   /* A partial pair (a 3.1 candidate's card: A remapped, B left on the
    * swapped base) resolves exactly as the candidate resolved it: B on
    * Circle, and A's explicit Square beats FF's default Square. */
   write_file("btn_swap = 1\nbind_a = SQUARE\n");
   CHECK(ctl_load_ini(&m, ini) == 0);
   CHECK(m.bind[CTL_GAME_A] == SQ && m.bind[CTL_GAME_B] == CIR &&
         m.bind[CTL_SC_FF] == 0);
   CHECK(ctl_save_ini(&m, ini) == 0);
   t = read_file();
   CHECK(strstr(t, "bind_a = SQUARE\n") && strstr(t, "bind_b = CIRCLE\n") &&
         strstr(t, "bind_ff = NONE\n"));
   CHECK(ctl_load_ini(&d, ini) == 0);
   CHECK(memcmp(m.bind, d.bind, sizeof(m.bind)) == 0);
   /* ...and so do the repair's tie-break and an invalid A: the swapped
    * default is what the old build fell back to and what it ranked as
    * "default" (FF's explicit Cross beats A's swapped-default Cross). */
   write_file("btn_swap = 1\nbind_ff = CROSS\n");
   CHECK(ctl_load_ini(&m, ini) == 0);
   CHECK(m.bind[CTL_SC_FF] == CRO && m.bind[CTL_GAME_A] == 0 &&
         m.bind[CTL_GAME_B] == CIR);
   CHECK(ctl_save_ini(&m, ini) == 0);
   CHECK(strstr(read_file(), "bind_a = NONE\n") != NULL);
   CHECK(ctl_load_ini(&d, ini) == 0);
   CHECK(memcmp(m.bind, d.bind, sizeof(m.bind)) == 0);
   write_file("btn_swap = 2\nbind_a = banana\n");
   CHECK(ctl_load_ini(&m, ini) == 1);
   CHECK(m.bind[CTL_GAME_A] == CRO && m.bind[CTL_GAME_B] == CIR);

   /* garbage, empty, truncated, illegal: the DEFAULT, never unbound */
   write_file("bind_a = \n"
              "bind_b = CIRCLE+CROSS\n"
              "bind_l = banana\n"
              "bind_ff = START+SELECT\n"
              "bind_video = L+R+UP+DOWN+LEFT\n"
              "bind_quicksave = SELECT+L+SELECT+L+SELECT+L+SELECT+L+SELECT+L+"
              "SELECT+L+SELECT+L+SELECT+L+SELECT+L+SELECT+L+SELECT+L\n"
              "bind_r = NONE\n"
              "bind_screenshot =  l + r \n");
#ifndef GPSP_PLAYABLE
   n_evt = 0;
#endif
   bad = ctl_load_ini(&m, ini);
   CHECK(bad == 6);
#ifndef GPSP_PLAYABLE
   CHECK(n_evt == 6);
   CHECK(strstr(last_evt, "config_bind_invalid key=bind_quicksave") != NULL);
#endif
   CHECK(m.bind[CTL_GAME_A] == CIR && m.bind[CTL_GAME_B] == CRO);
   CHECK(m.bind[CTL_GAME_L] == L);
   CHECK(m.bind[CTL_SC_FF] == SQ && m.bind[CTL_SC_VIDEO] == TRI);
   CHECK(m.bind[CTL_SC_SAVE] == (SEL | L));
   CHECK(m.bind[CTL_GAME_R] == 0);                  /* only NONE unbinds */
   CHECK(m.bind[CTL_SC_SHOT] == (L | R));
   /* every key that was in the file is rewritten -- the garbage is replaced
    * by the value actually in force */
   CHECK(ctl_save_ini(&m, ini) == 0);
   t = read_file();
   CHECK(strstr(t, "bind_a = CIRCLE\n") && strstr(t, "bind_b = CROSS\n"));
   CHECK(strstr(t, "bind_l = L\n") && strstr(t, "bind_ff = SQUARE\n"));
   CHECK(strstr(t, "bind_r = NONE\n") && strstr(t, "bind_screenshot = L+R\n"));
   CHECK(strstr(t, "bind_start") == NULL);           /* never in the file */
   CHECK(ctl_load_ini(&d, ini) == 0);
   CHECK(memcmp(m.bind, d.bind, sizeof(m.bind)) == 0);

   /* a hand-edited duplicate: explicit beats default, loser unbound */
   write_file("bind_ff = TRIANGLE\n");
   CHECK(ctl_load_ini(&m, ini) == 0);
   CHECK(m.bind[CTL_SC_FF] == TRI && m.bind[CTL_SC_VIDEO] == 0);

   /* reset keeps the keys in the file honest */
   write_file("");
   ctl_defaults(&m);
   ctl_assign(&m, CTL_SC_VIDEO, 0, &i);
   ctl_assign(&m, CTL_GAME_A, TRI, &i);
   CHECK(ctl_save_ini(&m, ini) == 0);
   ctl_reset(&m);
   CHECK(ctl_save_ini(&m, ini) == 0);
   CHECK(ctl_load_ini(&d, ini) == 0);
   ctl_defaults(&m);
   CHECK(memcmp(m.bind, d.bind, sizeof(m.bind)) == 0);
   t = read_file();
   CHECK(strstr(t, "bind_video = TRIANGLE\n") && strstr(t, "bind_a = CIRCLE\n"));

   /* random tables, random starting files, with and without a legacy
    * btn_swap = 1: save+load exact */
   for (i = 0; i < 3000; i++)
   {
      int swap = (int)(rnd() & 1), k, stolen;
      char seed[2048] = "";
      size_t o = 0;
      if (swap)
         o += (size_t)snprintf(seed, sizeof(seed), "btn_swap = 1\n");
      for (a = 0; a < CTL_ACTIONS; a++)
         if (rnd() % 3 == 0)
         {
            char v[48];
            if (rnd() % 4 == 0)
               snprintf(v, sizeof(v), "garbage%u", rnd() % 100);
            else
               ctl_format(random_binding(a), v, sizeof(v), NULL, NULL);
            o += (size_t)snprintf(seed + o, sizeof(seed) - o, "%s = %s\n",
                                  ctl_info(a)->key, v);
         }
      write_file(seed);
      ctl_load_ini(&m, ini);
      CHECK(no_dupes(&m));
      for (k = 0; k < 6; k++)
      {
         a = (int)(rnd() % CTL_ACTIONS);
         ctl_assign(&m, a, random_binding(a), &stolen);
      }
      for (k = 0; k < CTL_TURBO_N; k++)
         if (rnd() % 3 == 0)
         {
            m.turbo_half[k] = (uint8_t)(1 + rnd() % 3);
            m.turbo_toggle[k] = (uint8_t)(rnd() & 1);
         }
      if (rnd() % 10 == 0)
         ctl_reset(&m);
      CHECK(ctl_save_ini(&m, ini) == 0);
      CHECK(ctl_load_ini(&d, ini) == 0);
      if (memcmp(m.bind, d.bind, sizeof(m.bind)) != 0 ||
          memcmp(m.turbo_half, d.turbo_half, sizeof(m.turbo_half)) != 0 ||
          memcmp(m.turbo_toggle, d.turbo_toggle, sizeof(m.turbo_toggle)) != 0)
      {
         fprintf(stderr, "FAIL round trip %d:\n%s", i, read_file());
         exit(1);
      }
   }
   remove(ini);
}

/* ---- through config_psp.c ------------------------------------------------ */
static void test_pcfg(void)
{
   /* A 3.0 config, as a player's card carries it. */
   static const char *old30 =
      "scale = 1\nprofile = 0\nme_mode = 1\nbtn_swap = 1\nfilter = 0\n"
      "ff_mult_x10 = 30\nff_hold = 1\nframeskip_sparse = 0\nff_smooth = 1\n"
      "theme = 0\nshow_fps = 0\ngroup = GPSP07\nnick = PSP\nconsole = 0\n";
   char *t;
   int s;
   ctl_map d;

   /* A player who never touched anything: no bind_ keys, and btn_swap
    * still written as 0, so the file reads exactly as 3.0's would. */
   write_file("scale = 1\n");
   memset(&g_pcfg, 0x5A, sizeof(g_pcfg));
   pcfg_load(ini);
   ctl_defaults(&d);
   CHECK(memcmp(g_pcfg.controls.bind, d.bind, sizeof(d.bind)) == 0);
   pcfg_save();
   t = read_file();
   CHECK(strstr(t, "bind_") == NULL && strstr(t, "btn_swap = 0\n") != NULL);

   /* The 3.0 swapped player: the swap survives as bind_a/bind_b, and the
    * key is written back as a 1 (the downgrade mirror) beside them. */
   write_file(old30);
   memset(&g_pcfg, 0x5A, sizeof(g_pcfg));
   pcfg_load(ini);
   CHECK(g_pcfg.controls.bind[CTL_GAME_A] == CRO &&
         g_pcfg.controls.bind[CTL_GAME_B] == CIR);
   CHECK(ctl_game_mask(&g_pcfg.controls, CRO) == legacy_mask(CRO, 1));
   CHECK(ctl_game_mask(&g_pcfg.controls, CIR) == legacy_mask(CIR, 1));
   pcfg_save();
   t = read_file();
   CHECK(strstr(t, "bind_a = CROSS\n") && strstr(t, "bind_b = CIRCLE\n"));
   CHECK(strstr(t, "btn_swap = 1\n") != NULL);

   /* remap through the struct, save, reload: kept */
   CHECK(ctl_assign(&g_pcfg.controls, CTL_SC_VIDEO, 0, &s) == CTL_OK);
   CHECK(ctl_assign(&g_pcfg.controls, CTL_SC_FF, L | R, &s) == CTL_OK);
   pcfg_save();
   memset(&g_pcfg, 0, sizeof(g_pcfg));
   pcfg_load(ini);
   CHECK(g_pcfg.controls.bind[CTL_SC_VIDEO] == 0);
   CHECK(g_pcfg.controls.bind[CTL_SC_FF] == (L | R));
   CHECK(g_pcfg.controls.bind[CTL_GAME_A] == CRO);   /* the migrated pair */

   /* Reset to defaults: the swap is gone for good -- the mirror follows
    * the table to 0 and the keys say Circle/Cross. */
   ctl_reset(&g_pcfg.controls);
   pcfg_save();
   t = read_file();
   CHECK(strstr(t, "btn_swap = 0\n") && strstr(t, "bind_a = CIRCLE\n"));
   memset(&g_pcfg, 0, sizeof(g_pcfg));
   pcfg_load(ini);
   CHECK(ctl_is_default(&g_pcfg.controls));
   remove(ini);
}

/* ---- the menu button: CONFIG.INI `menu_button` (section 10) --------------- */
static void test_menu_button(void)
{
   ctl_map m, d;
   int v;
   char *t;

   /* parse / name */
   CHECK(ctl_menu_parse("home", &v) == 0 && v == CTL_MENU_HOME);
   CHECK(ctl_menu_parse("  HOME \r\n", &v) == 0 && v == CTL_MENU_HOME);
   CHECK(ctl_menu_parse("start_select", &v) == 0 && v == CTL_MENU_START_SELECT);
   CHECK(ctl_menu_parse("Start+Select", &v) == 0 && v == CTL_MENU_START_SELECT);
   v = 7;
   CHECK(ctl_menu_parse("", &v) == -1 && v == 7);
   CHECK(ctl_menu_parse("homer", &v) == -1);
   CHECK(ctl_menu_parse("hom", &v) == -1);
   CHECK(ctl_menu_parse("start", &v) == -1);
   CHECK(ctl_menu_parse("1", &v) == -1);
   CHECK(ctl_menu_parse("home home", &v) == -1);
   CHECK(ctl_menu_parse(NULL, &v) == -1);
   CHECK(!strcmp(ctl_menu_name(0), "start_select") &&
         !strcmp(ctl_menu_name(1), "home"));

   /* defaults: START+SELECT, and an old config gains no key on save */
   ctl_defaults(&m);
   CHECK(m.menu_home == CTL_MENU_START_SELECT && !m.menu_keep);
   write_file("scale = 1\nbtn_swap = 0\n");
   CHECK(ctl_load_ini(&m, ini) == 0 && m.menu_home == 0 && !m.menu_keep);
   CHECK(ctl_save_ini(&m, ini) == 0);
   CHECK(strcmp(read_file(), "scale = 1\nbtn_swap = 0\n") == 0);

   /* HOME: written, read back, and it makes the table "custom" */
   m.menu_home = CTL_MENU_HOME;
   CHECK(!ctl_is_default(&m));
   CHECK(ctl_save_ini(&m, ini) == 0);
   CHECK(strcmp(read_file(), "scale = 1\nbtn_swap = 0\nmenu_button = home\n")
         == 0);
   CHECK(ctl_load_ini(&d, ini) == 0 && d.menu_home == CTL_MENU_HOME &&
         d.menu_keep);
   CHECK(memcmp(m.bind, d.bind, sizeof(m.bind)) == 0);

   /* back to START+SELECT: the key is in the file, so it is rewritten (the
    * keep rule) rather than left saying "home" */
   d.menu_home = CTL_MENU_START_SELECT;
   CHECK(ctl_is_default(&d));
   CHECK(ctl_save_ini(&d, ini) == 0);
   CHECK(strstr(read_file(), "menu_button = start_select\n") != NULL);
   CHECK(ctl_load_ini(&m, ini) == 0 && m.menu_home == 0 && m.menu_keep);

   /* hand-edited values */
   write_file("menu_button = HOME\n");
   CHECK(ctl_load_ini(&m, ini) == 0 && m.menu_home == CTL_MENU_HOME);
   write_file("menu_button = START+SELECT\n");
   CHECK(ctl_load_ini(&m, ini) == 0 && m.menu_home == 0);
   /* garbage and empty fall back to START+SELECT, which always works, and
    * count as a rejected value; the next save repairs the line */
   write_file("menu_button = ps_button\n");
#ifndef GPSP_PLAYABLE
   n_evt = 0;
#endif
   CHECK(ctl_load_ini(&m, ini) == 1 && m.menu_home == 0 && m.menu_keep);
#ifndef GPSP_PLAYABLE
   CHECK(n_evt == 1 && strstr(last_evt, "key=menu_button") != NULL);
#endif
   CHECK(ctl_save_ini(&m, ini) == 0);
   CHECK(strcmp(read_file(), "menu_button = start_select\n") == 0);
   write_file("menu_button =\n");
   CHECK(ctl_load_ini(&m, ini) == 1 && m.menu_home == 0);
   /* a value longer than the reader's buffer is not a value we know */
   write_file("menu_button = homehomehomehomehomehomehomehomehomehome\n");
   CHECK(ctl_load_ini(&m, ini) == 1 && m.menu_home == 0);

   /* the mode is independent of every binding, both ways */
   write_file("bind_ff = L+R\nmenu_button = home\nbind_video = NONE\n");
   CHECK(ctl_load_ini(&m, ini) == 0 && m.menu_home == CTL_MENU_HOME &&
         m.bind[CTL_SC_FF] == (L | R) && m.bind[CTL_SC_VIDEO] == 0);

   /* Reset to defaults puts the menu back on START+SELECT, key kept */
   ctl_reset(&m);
   CHECK(m.menu_home == 0 && m.menu_keep && ctl_is_default(&m));
   CHECK(ctl_save_ini(&m, ini) == 0);
   t = read_file();
   CHECK(strstr(t, "menu_button = start_select\n") != NULL);

   /* corruption is repaired to the safe mode */
   ctl_defaults(&m);
   m.menu_home = 7;
   CHECK(ctl_repair(&m, NULL, NULL) == 1 && m.menu_home == 0);
   CHECK(ctl_repair(&m, NULL, NULL) == 0);

   /* through pcfg_load / pcfg_save, beside every other key */
   write_file("scale = 1\nmenu_button = home\n");
   memset(&g_pcfg, 0x5A, sizeof(g_pcfg));
   pcfg_load(ini);
   CHECK(g_pcfg.controls.menu_home == CTL_MENU_HOME);
   pcfg_save();
   t = read_file();
   CHECK(strstr(t, "menu_button = home\n") != NULL);
   memset(&g_pcfg, 0, sizeof(g_pcfg));
   pcfg_load(ini);
   CHECK(g_pcfg.controls.menu_home == CTL_MENU_HOME);
   write_file("scale = 1\n");
   memset(&g_pcfg, 0x5A, sizeof(g_pcfg));
   pcfg_load(ini);
   CHECK(g_pcfg.controls.menu_home == 0);
   pcfg_save();
   CHECK(strstr(read_file(), "menu_button") == NULL);
   remove(ini);
}

/* ---- the behaviours the feature is for ----------------------------------- */
static void test_remapped(void)
{
   ctl_map m;
   int s;

   /* Triangle unbound: pressing it fires nothing and reaches no game button */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_SC_VIDEO, 0, &s) == CTL_OK);
   CHECK(ctl_shortcuts(&m, TRI, TRI) == 0);
   CHECK(ctl_game_mask(&m, TRI) == 0);
   /* ...but SELECT+TRIANGLE still opens the pause screen */
   CHECK(ctl_shortcuts(&m, SEL | TRI, TRI) == CTL_FIRE_PAUSE);

   /* A on Triangle steals it from the video preset */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_GAME_A, TRI, &s) == CTL_OK && s == CTL_SC_VIDEO);
   CHECK(ctl_game_mask(&m, TRI) == (1u << JP_A));
   CHECK(ctl_game_mask(&m, CIR) == 0);
   CHECK(!(ctl_shortcuts(&m, TRI, TRI) & CTL_FIRE_VIDEO));

   /* FF on L+R: a chord over two game buttons, which still reach the game */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_SC_FF, L | R, &s) == CTL_OK && s == -1);
   CHECK(ctl_shortcuts(&m, L | R, R) & CTL_FIRE_FF_HELD);
   CHECK(ctl_shortcuts(&m, L | R, R) & CTL_FIRE_FF_PRESS);
   CHECK(!(ctl_shortcuts(&m, L, L) & CTL_FIRE_FF_HELD));
   CHECK(!(ctl_shortcuts(&m, SQ, SQ) & CTL_FIRE_FF_HELD));     /* Square freed */
   CHECK(ctl_game_mask(&m, L | R) == ((1u << JP_L) | (1u << JP_R)));
   /* ...and the longer-chord rule: L+R+SELECT is the screenshot, so FF
    * stands down while it is held, and the quick save/load still yield */
   CHECK(!(ctl_shortcuts(&m, L | R | SEL, SEL) & CTL_FIRE_FF_HELD));
   CHECK(ctl_shortcuts(&m, L | R | SEL, SEL) & CTL_FIRE_SHOT);

   /* quick save on START+L: START is a modifier, L the trigger */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_SC_SAVE, START | L, &s) == CTL_OK);
   CHECK(ctl_shortcuts(&m, START | L, L) & CTL_FIRE_SAVE);
   CHECK(!(ctl_shortcuts(&m, START | L, START) & CTL_FIRE_SAVE));
   CHECK(!(ctl_shortcuts(&m, SEL | L, L) & CTL_FIRE_SAVE));

   /* screenshot rebound onto START: allowed (not both START and SELECT) */
   CHECK(ctl_assign(&m, CTL_SC_SHOT, START | R, &s) == CTL_OK);
   CHECK(ctl_shortcuts(&m, START | R, R) & CTL_FIRE_SHOT);

   /* everything unbound: no shortcut can ever fire */
   for (s = CTL_GAME_COUNT; s < CTL_ACTIONS; s++)
      m.bind[s] = 0;
   {
      unsigned cur;
      for (cur = 0; cur < 4096; cur++)
         CHECK(ctl_shortcuts(&m, expand(cur), expand(cur)) == 0);
   }
}

/* THE THUMBSTICK (ctl_map.h): four more inputs.  Off adds nothing, Move is
 * the d-pad, Custom is the four stick bits; the dead zone engages late and
 * lets go later; a stick direction binds, chords, parses and round-trips
 * like any button. */
static void test_stick(void)
{
   ctl_map m;
   unsigned st, mask;
   int s, mode, v;
   char buf[64];

   /* centre, and anything inside the engage threshold from rest, is quiet */
   for (v = 128 - CTL_STICK_ON + 1; v < 128 + CTL_STICK_ON; v++)
   {
      st = 0;
      CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, v, 128, &st) == 0);
      st = 0;
      CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, 128, v, &st) == 0);
   }
   /* the four directions and a diagonal (Ly 0 = up) */
   st = 0; CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, 0, 128, &st) == CTL_STICK_LEFT);
   st = 0; CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, 255, 128, &st) == CTL_STICK_RIGHT);
   st = 0; CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, 128, 0, &st) == CTL_STICK_UP);
   st = 0; CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, 128, 255, &st) == CTL_STICK_DOWN);
   st = 0; CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, 255, 0, &st) ==
                 (CTL_STICK_RIGHT | CTL_STICK_UP));
   /* hysteresis: engaged, it holds down to the release threshold */
   st = 0;
   CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, 128 + CTL_STICK_ON, 128, &st) == CTL_STICK_RIGHT);
   CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, 128 + CTL_STICK_OFF_AT, 128, &st) == CTL_STICK_RIGHT);
   CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, 128 + CTL_STICK_OFF_AT - 1, 128, &st) == 0);
   CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, 128 + CTL_STICK_ON - 1, 128, &st) == 0);
   /* a flick straight across lands on the other side */
   st = 0;
   CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, 255, 128, &st) == CTL_STICK_RIGHT);
   CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, 0, 128, &st) == CTL_STICK_LEFT);
   /* the modes: off is nothing, move is the d-pad's own bits */
   st = 0; CHECK(ctl_stick_bits(CTL_STICK_OFF, 255, 0, &st) == 0);
   st = 0; CHECK(ctl_stick_bits(CTL_STICK_MOVE, 255, 0, &st) == (CTL_RIGHT | CTL_UP));
   st = 0; CHECK(ctl_stick_bits(CTL_STICK_MOVE, 0, 255, &st) == (CTL_LEFT | CTL_DOWN));
   CHECK(ctl_stick_bits(CTL_STICK_CUSTOM, 255, 128, NULL) == CTL_STICK_RIGHT);
   /* never anything but its own bits / the d-pad's */
   for (mode = 0; mode < 3; mode++)
      for (v = 0; v < 256; v += 5)
         for (s = 0; s < 256; s += 5)
         {
            unsigned r;
            st = 0;
            r = ctl_stick_bits(mode, v, s, &st);
            CHECK(!(r & ~(mode == CTL_STICK_CUSTOM ? CTL_STICK_ALL :
                          mode == CTL_STICK_MOVE ? (CTL_UP | CTL_DOWN | CTL_LEFT | CTL_RIGHT) : 0u)));
            CHECK(!(st & ~CTL_STICK_ALL));
         }

   /* names */
   for (mode = 0; mode < 3; mode++)
   {
      int back = -1;
      CHECK(ctl_stick_parse(ctl_stick_name(mode), &back) == 0 && back == mode);
   }
   CHECK(ctl_stick_parse(" Custom ", &mode) == 0 && mode == CTL_STICK_CUSTOM);
   CHECK(ctl_stick_parse("", &mode) != 0);
   CHECK(ctl_stick_parse("sideways", &mode) != 0);

   /* the stick's bits are not the buttons', and together they are CTL_ALL */
   CHECK((CTL_STICK_ALL & CTL_BUTTONS) == 0);
   CHECK((CTL_STICK_ALL | CTL_BUTTONS) == CTL_ALL);
   CHECK(ctl_popcount(CTL_STICK_ALL) == 4);
   CHECK(ctl_stick_only(CTL_STICK_UP));
   CHECK(ctl_stick_only(CTL_STICK_UP | CTL_STICK_LEFT));
   CHECK(!ctl_stick_only(CTL_STICK_UP | CTL_L));
   CHECK(!ctl_stick_only(CTL_L));
   CHECK(!ctl_stick_only(0));

   /* ctl_pad_word: the one pad word every loop builds.  The bug it closes:
    * the browser's Settings loop passed raw pad.Buttons, so the Controls
    * page opened from there never saw a stick direction to capture. */
   st = 0;
   CHECK(ctl_pad_word(CTL_CROSS, CTL_STICK_CUSTOM, 255, 128, &st) ==
         (CTL_CROSS | CTL_STICK_RIGHT));
   CHECK(st == CTL_STICK_RIGHT);
   /* the stick's bits are never taken from pad.Buttons */
   st = 0;
   CHECK(ctl_pad_word(CTL_STICK_ALL | CTL_L, CTL_STICK_CUSTOM, 128, 128, &st) ==
         CTL_L);
   st = 0;
   CHECK(ctl_pad_word(0, CTL_STICK_MOVE, 128, 0, &st) == CTL_UP);
   /* off: the buttons alone, and the latch let go */
   st = CTL_STICK_LEFT;
   CHECK(ctl_pad_word(CTL_START | CTL_STICK_UP, CTL_STICK_OFF, 0, 0, &st) ==
         CTL_START);
   CHECK(st == 0);
   CHECK(ctl_pad_word(CTL_R, CTL_STICK_CUSTOM, 128, 255, NULL) ==
         (CTL_R | CTL_STICK_DOWN));

   /* parse / format */
   CHECK(ctl_parse("STICK_UP", &mask) == 0 && mask == CTL_STICK_UP);
   CHECK(ctl_parse("l + stick_left", &mask) == 0 && mask == (CTL_L | CTL_STICK_LEFT));
   ctl_format(CTL_L | CTL_STICK_LEFT, buf, sizeof(buf), NULL, NULL);
   CHECK(strcmp(buf, "L+STICK_LEFT") == 0);
   CHECK(ctl_parse(buf, &mask) == 0 && mask == (CTL_L | CTL_STICK_LEFT));

   /* binding: a game button on the stick, a shortcut on the stick alone and
    * in a chord, and the conflict rule */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_GAME_UP, CTL_STICK_UP, &s) == CTL_OK && s == -1);
   CHECK(ctl_game_mask(&m, CTL_STICK_UP) == (1u << 4));     /* libretro UP */
   CHECK(ctl_game_mask(&m, CTL_UP) == 0);                   /* the d-pad let go of it */
   CHECK(ctl_assign(&m, CTL_SC_LOAD, CTL_STICK_DOWN, &s) == CTL_OK);
   CHECK(ctl_shortcuts(&m, CTL_STICK_DOWN, CTL_STICK_DOWN) & CTL_FIRE_LOAD);
   CHECK(!(ctl_shortcuts(&m, CTL_STICK_DOWN, 0) & CTL_FIRE_LOAD));   /* held: once */
   CHECK(ctl_assign(&m, CTL_SC_SAVE, CTL_L | CTL_STICK_LEFT, &s) == CTL_OK);
   CHECK(ctl_shortcuts(&m, CTL_L | CTL_STICK_LEFT, CTL_STICK_LEFT) & CTL_FIRE_SAVE);
   CHECK(ctl_assign(&m, CTL_SC_VIDEO, CTL_STICK_UP, &s) == CTL_OK && s == CTL_GAME_UP);
   CHECK(m.bind[CTL_GAME_UP] == 0);
   CHECK(ctl_check(CTL_GAME_A, CTL_STICK_UP | CTL_STICK_LEFT) == CTL_E_SINGLE);

   /* CONFIG.INI round trip of stick bindings */
   remove(ini);
   CHECK(ctl_save_ini(&m, ini) == 0);
   {
      ctl_map r;
      CHECK(ctl_load_ini(&r, ini) == 0);
      for (s = 0; s < CTL_ACTIONS; s++)
         CHECK(r.bind[s] == m.bind[s]);
   }
   /* the defaults never mention the stick */
   ctl_defaults(&m);
   for (s = 0; s < CTL_ACTIONS; s++)
      CHECK(!(m.bind[s] & CTL_STICK_ALL));
}

/* ---- TURBO BUTTONS (docs/CONTROL-REMAP.md section 13) --------------------- */

#define BIT_A (1u << JP_A)
#define BIT_B (1u << JP_B)

/* The game input of one emulated frame, composed exactly as main_psp.c's
 * plat_input_bitmask() composes it. */
static uint32_t plat_mask(const ctl_map *m, const ctl_turbo *t, unsigned pad,
                          uint32_t frame)
{
   uint32_t r = ctl_game_mask(m, pad);
   if (t->live)
      r |= ctl_turbo_mask(m, t, frame);
   return r;
}

/* One loop iteration as main_psp.c runs it: sample the pad, step the turbo
 * state, run `per` emulated frames (1, or fast-forward's 3), recording the
 * game input of each into out[]. */
typedef struct { ctl_turbo t; unsigned prev; uint32_t frame; } rig;

static void rig_step(const ctl_map *m, rig *g, unsigned pad, int enabled,
                     int per, uint32_t *out, int *n)
{
   int i;
   ctl_turbo_pad(m, &g->t, pad, pad & ~g->prev, g->frame, enabled);
   g->prev = pad;
   for (i = 0; i < per; i++)
   {
      uint32_t r = plat_mask(m, &g->t, pad, g->frame);
      /* any number of reads of one frame agree (the core polls per button) */
      CHECK(plat_mask(m, &g->t, pad, g->frame) == r);
      if (out)
         out[(*n)++] = r;
      g->frame++;
   }
}

static void rig_init(rig *g, uint32_t frame)
{
   memset(g, 0, sizeof(*g));
   g->frame = frame;
}

/* "X..X..": the frames of out[] on which `bit` is pressed. */
static const char *pattern(const uint32_t *out, int n, uint32_t bit)
{
   static char buf[4][128];
   static int which;
   char *b = buf[which++ & 3];
   int i;
   for (i = 0; i < n && i < 127; i++)
      b[i] = (out[i] & bit) ? 'X' : '.';
   b[i] = 0;
   return b;
}

static void test_turbo(void)
{
   ctl_map m, d;
   rig g;
   uint32_t out[128];
   int n, i, s, v;
   unsigned pad;
   char *t;

   /* ---- the settings ---- */
   CHECK(ctl_turbo_rate(1) == 30 && ctl_turbo_rate(2) == 15 &&
         ctl_turbo_rate(3) == 10);
   CHECK(CTL_TURBO_HALF_DEF == 2);
   /* X walks 10 -> 15 -> 30 -> 10 */
   CHECK(ctl_turbo_half_step(3, +1) == 2 && ctl_turbo_half_step(2, +1) == 1 &&
         ctl_turbo_half_step(1, +1) == 3);
   CHECK(ctl_turbo_half_step(1, -1) == 2 && ctl_turbo_half_step(2, -1) == 3 &&
         ctl_turbo_half_step(3, -1) == 1);
   for (i = 1; i <= 3; i++)
   {
      char b[8];
      snprintf(b, sizeof(b), "%d", ctl_turbo_rate(i));
      CHECK(ctl_turbo_rate_parse(b, &v) == 0 && v == i);
   }
   CHECK(ctl_turbo_rate_parse(" 30 \r\n", &v) == 0 && v == 1);
   v = 9;
   CHECK(ctl_turbo_rate_parse("", &v) == -1 && v == 9);
   CHECK(ctl_turbo_rate_parse("20", &v) == -1);
   CHECK(ctl_turbo_rate_parse("60", &v) == -1);
   CHECK(ctl_turbo_rate_parse("15hz", &v) == -1);
   CHECK(ctl_turbo_rate_parse("1", &v) == -1);
   CHECK(ctl_turbo_rate_parse("fast", &v) == -1);
   CHECK(ctl_turbo_rate_parse(NULL, &v) == -1 && v == 9);
   CHECK(ctl_turbo_mode_parse("hold", &v) == 0 && v == 0);
   CHECK(ctl_turbo_mode_parse(" Toggle \n", &v) == 0 && v == 1);
   CHECK(ctl_turbo_mode_parse("TOGGLE", &v) == 0 && v == 1);
   v = 9;
   CHECK(ctl_turbo_mode_parse("", &v) == -1 && v == 9);
   CHECK(ctl_turbo_mode_parse("1", &v) == -1);
   CHECK(ctl_turbo_mode_parse("held", &v) == -1);
   CHECK(ctl_turbo_mode_parse("toggle toggle", &v) == -1);
   CHECK(!strcmp(ctl_turbo_mode_name(0), "hold") &&
         !strcmp(ctl_turbo_mode_name(1), "toggle"));
   /* keys: the existing naming, unique, and none of them a bind_ key */
   for (i = 0; i < CTL_TURBO_N; i++)
   {
      int a = CTL_TURBO_FIRST + i, j, k, k2;
      CHECK(strncmp(ctl_info(a)->key, "bind_turbo_", 11) == 0);
      for (k = 0; k < 2; k++)
      {
         const char *key = ctl_turbo_key(a, k);
         CHECK(key && strncmp(key, "turbo_", 6) == 0 && strlen(key) < 32);
         CHECK(strstr(key, k ? "_mode" : "_rate") != NULL);
         for (j = 0; j < CTL_ACTIONS; j++)
            CHECK(strcmp(key, ctl_info(j)->key) != 0);
         for (j = i + 1; j < CTL_TURBO_N; j++)
            for (k2 = 0; k2 < 2; k2++)
               CHECK(strcmp(key, ctl_turbo_key(CTL_TURBO_FIRST + j, k2)) != 0);
      }
      CHECK(strcmp(ctl_turbo_key(a, 0), ctl_turbo_key(a, 1)) != 0);
      CHECK(ctl_turbo_key(a, 2) == NULL);
   }
   CHECK(ctl_turbo_key(CTL_GAME_A, 0) == NULL);
   CHECK(ctl_turbo_key(CTL_ACTIONS, 0) == NULL);
   /* A's two press A, B's two press B */
   CHECK(ctl_info(CTL_TURBO_A1)->retro_id == JP_A &&
         ctl_info(CTL_TURBO_A2)->retro_id == JP_A &&
         ctl_info(CTL_TURBO_B1)->retro_id == JP_B &&
         ctl_info(CTL_TURBO_B2)->retro_id == JP_B);

   /* ---- defaults: unbound, 15 a second, Hold ---- */
   ctl_defaults(&m);
   for (i = 0; i < CTL_TURBO_N; i++)
      CHECK(m.bind[CTL_TURBO_FIRST + i] == 0 && m.turbo_half[i] == 2 &&
            m.turbo_toggle[i] == 0 && !m.turbo_rate_keep[i] &&
            !m.turbo_mode_keep[i] && !m.keep[CTL_TURBO_FIRST + i]);
   CHECK(ctl_turbo_bound(&m) == 0 && ctl_is_default(&m));

   /* ---- NOTHING BOUND: the input is today's, for every pad word ----
    * All 65,536 values of the sixteen bindable inputs (the twelve buttons
    * and the stick's four directions), with the system bits mixed in, each
    * as a fresh press on a moving frame number: the turbo state never goes
    * live, so plat_input_bitmask() is ctl_game_mask() and nothing else --
    * and for the twelve buttons that is 3.0's mask (test_defaults_are_30). */
   rig_init(&g, 7);
   for (pad = 0; pad < 0x10000u; pad++)
   {
      unsigned p = pad | ((pad & 3) == 1 ? 0x010000u : (pad & 3) == 2 ?
                          0x060000u : 0u);
      ctl_turbo_pad(&m, &g.t, p, p, g.frame, 1);
      CHECK(!g.t.live && !g.t.run[0] && !g.t.run[1] && !g.t.run[2] &&
            !g.t.run[3]);
      CHECK(plat_mask(&m, &g.t, p, g.frame) == ctl_game_mask(&m, p));
      CHECK(ctl_turbo_mask(&m, &g.t, g.frame) == 0);
      if (!(pad & CTL_STICK_ALL))
         CHECK(plat_mask(&m, &g.t, p, g.frame) == legacy_mask(p, 0));
      g.frame += 1 + (pad & 3);
   }

   /* ---- HOLD: the pulse of each rate, frame by frame ----
    * Turbo A on Triangle; Triangle held for 24 frames from frame 100, then
    * let go.  30 = 1 on / 1 off, 15 = 2 / 2, 10 = 3 / 3, and the first frame
    * of a press is always ON. */
   {
      static const char *const want[4] = { NULL,
         "X.X.X.X.X.X.X.X.X.X.X.X.....",
         "XX..XX..XX..XX..XX..XX......",
         "XXX...XXX...XXX...XXX......." };
      int half;
      for (half = 1; half <= 3; half++)
      {
         ctl_defaults(&m);
         CHECK(ctl_assign(&m, CTL_TURBO_A1, TRI, &s) == CTL_OK &&
               s == CTL_SC_VIDEO);
         m.turbo_half[0] = (uint8_t)half;
         rig_init(&g, 100);
         n = 0;
         for (i = 0; i < 28; i++)
            rig_step(&m, &g, i < 24 ? TRI : 0, 1, 1, out, &n);
         CHECK(strcmp(pattern(out, n, BIT_A), want[half]) == 0);
         /* presses a second, at 60 frames a second */
         {
            int presses = 0;
            rig_init(&g, 5);
            n = 0;
            for (i = 0; i < 60; i++)
               rig_step(&m, &g, TRI, 1, 1, out, &n);
            for (i = 0; i < 60; i++)
               if ((out[i] & BIT_A) && (i == 0 || !(out[i - 1] & BIT_A)))
                  presses++;
            CHECK(presses == ctl_turbo_rate(half));
         }
         /* nothing but A ever comes out of it */
         for (i = 0; i < n; i++)
            CHECK(!(out[i] & ~BIT_A));
      }
   }
   /* the phase belongs to the PRESS, not to the frame counter: wherever it
    * starts, the first frame is a press, so the shortest tap is one press */
   ctl_defaults(&m);
   ctl_assign(&m, CTL_TURBO_A1, TRI, &s);
   m.turbo_half[0] = 3;
   for (i = 0; i < 12; i++)
   {
      rig_init(&g, 1000u + (unsigned)i);
      n = 0;
      rig_step(&m, &g, TRI, 1, 1, out, &n);
      rig_step(&m, &g, 0, 1, 1, out, &n);
      rig_step(&m, &g, 0, 1, 1, out, &n);
      CHECK(strcmp(pattern(out, n, BIT_A), "X..") == 0);
   }
   /* released and pressed again: a fresh train, on at once */
   rig_init(&g, 50);
   n = 0;
   for (i = 0; i < 4; i++) rig_step(&m, &g, TRI, 1, 1, out, &n);
   rig_step(&m, &g, 0, 1, 1, out, &n);
   for (i = 0; i < 4; i++) rig_step(&m, &g, TRI, 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_A), "XXX..XXX.") == 0);
   /* the frame counter wrapping changes nothing a player could feel */
   rig_init(&g, 0xFFFFFFFDu);
   n = 0;
   for (i = 0; i < 9; i++) rig_step(&m, &g, TRI, 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_A), "XXX...XXX") == 0);

   /* ---- FAST-FORWARD: three emulated frames per pad sample ----
    * The pulse is per EMULATED frame, so the game sees the same train as at
    * 1x and it simply goes by three times as fast. */
   ctl_defaults(&m);
   ctl_assign(&m, CTL_TURBO_A1, TRI, &s);
   rig_init(&g, 300);
   n = 0;
   for (i = 0; i < 4; i++)
      rig_step(&m, &g, TRI, 1, 3, out, &n);
   CHECK(n == 12 && strcmp(pattern(out, n, BIT_A), "XX..XX..XX..") == 0);

   /* ---- NORMAL A HELD TOGETHER WITH TURBO A: A is simply held ---- */
   ctl_defaults(&m);
   ctl_assign(&m, CTL_TURBO_A1, TRI, &s);
   rig_init(&g, 10);
   n = 0;
   for (i = 0; i < 8; i++)
      rig_step(&m, &g, TRI | (i >= 2 && i < 6 ? CIR : 0), 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_A), "XXXXXX..") == 0);
   /* ...and the rest of the pad is untouched by a running train */
   rig_init(&g, 10);
   n = 0;
   for (i = 0; i < 8; i++)
      rig_step(&m, &g, TRI | CRO | LEFT, 1, 1, out, &n);
   for (i = 0; i < 8; i++)
      CHECK((out[i] & ~BIT_A) == (BIT_B | (1u << JP_LEFT)));

   /* ---- BOTH TURBO A BUTTONS, DIFFERENT RATES, AND A TURBO B ----
    * Turbo A on Triangle at 30, Turbo A (2) on Square at 10, Turbo B on L
    * at 15: each runs its own train from its own press. */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_TURBO_A1, TRI, &s) == CTL_OK);
   CHECK(ctl_assign(&m, CTL_TURBO_A2, SQ, &s) == CTL_OK && s == CTL_SC_FF);
   CHECK(ctl_assign(&m, CTL_TURBO_B1, L, &s) == CTL_OK && s == CTL_GAME_L);
   m.turbo_half[0] = 1;
   m.turbo_half[1] = 3;
   m.turbo_half[2] = 2;
   rig_init(&g, 40);
   n = 0;
   for (i = 0; i < 12; i++) rig_step(&m, &g, TRI, 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_A), "X.X.X.X.X.X.") == 0);
   rig_init(&g, 40);
   n = 0;
   for (i = 0; i < 12; i++) rig_step(&m, &g, SQ, 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_A), "XXX...XXX...") == 0);
   /* both held: A is pressed whenever either train is on */
   rig_init(&g, 40);
   n = 0;
   for (i = 0; i < 12; i++) rig_step(&m, &g, TRI | SQ, 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_A), "XXX.X.XXX.X.") == 0);
   /* the second pressed three frames after the first: its own phase */
   rig_init(&g, 40);
   n = 0;
   for (i = 0; i < 12; i++)
      rig_step(&m, &g, TRI | (i >= 3 ? SQ : 0), 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_A), "X.XXXXX.XXXX") == 0);
   /* Turbo B beside them presses B and only B; L itself is no longer L */
   rig_init(&g, 40);
   n = 0;
   for (i = 0; i < 8; i++) rig_step(&m, &g, TRI | L, 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_A), "X.X.X.X.") == 0);
   CHECK(strcmp(pattern(out, n, BIT_B), "XX..XX..") == 0);
   for (i = 0; i < 8; i++)
      CHECK(!(out[i] & ~(BIT_A | BIT_B)));

   /* ---- TOGGLE ----
    * Turbo B on Square, Toggle, 15: a press starts auto-fire, letting go
    * changes nothing, the next press stops it. */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_TURBO_B1, SQ, &s) == CTL_OK);
   m.turbo_toggle[2] = 1;
   rig_init(&g, 500);
   n = 0;
   rig_step(&m, &g, SQ, 1, 1, out, &n);                    /* press        */
   rig_step(&m, &g, SQ, 1, 1, out, &n);                    /* still down   */
   for (i = 0; i < 8; i++) rig_step(&m, &g, 0, 1, 1, out, &n);  /* let go  */
   rig_step(&m, &g, SQ, 1, 1, out, &n);                    /* press: stops */
   for (i = 0; i < 5; i++) rig_step(&m, &g, i < 2 ? SQ : 0, 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_B), "XX..XX..XX......") == 0);
   CHECK(!g.t.live);
   /* a third press starts again, on at once */
   n = 0;
   rig_step(&m, &g, SQ, 1, 1, out, &n);
   rig_step(&m, &g, 0, 1, 1, out, &n);
   rig_step(&m, &g, 0, 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_B), "XX.") == 0 && g.t.live);
   /* THE STOP POINTS (menu open, state load, game exit, sleep, a held
    * rewind are all ctl_turbo_stop): auto-fire is over and stays over until
    * the button is PRESSED again -- holding it through the stop is not a
    * press. */
   ctl_turbo_stop(&g.t);
   CHECK(!g.t.live);
   n = 0;
   for (i = 0; i < 6; i++) rig_step(&m, &g, 0, 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_B), "......") == 0);
   rig_init(&g, 600);
   rig_step(&m, &g, SQ, 1, 1, NULL, NULL);
   CHECK(g.t.live);
   ctl_turbo_stop(&g.t);                  /* e.g. the menu opens on that X */
   n = 0;
   for (i = 0; i < 6; i++) rig_step(&m, &g, SQ, 1, 1, out, &n);  /* held  */
   CHECK(strcmp(pattern(out, n, BIT_B), "......") == 0 && !g.t.live);
   /* a HOLD button still down after a stop starts a fresh train instead */
   m.turbo_toggle[2] = 0;
   rig_init(&g, 700);
   n = 0;
   for (i = 0; i < 3; i++) rig_step(&m, &g, SQ, 1, 1, out, &n);
   ctl_turbo_stop(&g.t);
   for (i = 0; i < 5; i++) rig_step(&m, &g, SQ, 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_B), "XX.XX..X") == 0);

   /* ---- LINK PLAY: a turbo button does nothing at all ----
    * enabled == 0: no train, in either mode, and since the conflict rule
    * left its PSP button bound to nothing else, the game sees no input. */
   for (v = 0; v < 2; v++)
   {
      ctl_defaults(&m);
      CHECK(ctl_assign(&m, CTL_TURBO_A1, CRO, &s) == CTL_OK &&
            s == CTL_GAME_B);
      m.turbo_toggle[0] = (uint8_t)v;
      rig_init(&g, 900);
      n = 0;
      for (i = 0; i < 12; i++)
         rig_step(&m, &g, (i % 6) < 4 ? CRO : 0, 0, 1, out, &n);
      for (i = 0; i < n; i++)
         CHECK(out[i] == 0);
      CHECK(!g.t.live);
      /* the rest of the pad still plays */
      n = 0;
      rig_step(&m, &g, CRO | CIR | UP, 0, 1, out, &n);
      CHECK(out[0] == (BIT_A | (1u << JP_UP)));
      /* a session starting stops a running train on that frame... */
      rig_init(&g, 950);
      n = 0;
      for (i = 0; i < 3; i++) rig_step(&m, &g, i ? 0 : CRO, 1, 1, out, &n);
      if (v)
         CHECK(g.t.live);
      rig_step(&m, &g, 0, 0, 1, out, &n);
      CHECK(!g.t.live && out[n - 1] == 0);
      /* ...and when it ends a Toggle is still off (it was stopped, not
       * paused), while a Hold button that is down fires again */
      rig_step(&m, &g, 0, 1, 1, out, &n);
      CHECK(!g.t.live && out[n - 1] == 0);
      rig_step(&m, &g, CRO, 0, 1, out, &n);
      rig_step(&m, &g, CRO, 1, 1, out, &n);
      CHECK(g.t.live == (v ? 0 : 1));
      CHECK(out[n - 1] == (v ? 0u : BIT_A));
   }

   /* ---- THE CONFLICT RULE ---- */
   ctl_defaults(&m);
   /* one input: a chord is refused and the map does not change */
   CHECK(ctl_check(CTL_TURBO_A1, L | R) == CTL_E_SINGLE);
   CHECK(ctl_assign(&m, CTL_TURBO_A1, L | R, &s) == CTL_E_SINGLE && s == -1);
   CHECK(m.bind[CTL_TURBO_A1] == 0 && m.bind[CTL_GAME_L] == L);
   CHECK(ctl_check(CTL_TURBO_B2, 0x10000) == CTL_E_BITS);
   /* any single bindable input, the stick's directions included */
   for (i = 0; i < CTL_NBUTTONS; i++)
      CHECK(ctl_check(CTL_TURBO_B2, ctl_button_order(i)) == CTL_OK);
   /* the owner's example: Turbo A on Cross, Turbo B on Circle.  Each takes
    * the button from the game button that had it, which is left unbound. */
   CHECK(ctl_assign(&m, CTL_TURBO_A1, CRO, &s) == CTL_OK && s == CTL_GAME_B);
   CHECK(ctl_assign(&m, CTL_TURBO_B1, CIR, &s) == CTL_OK && s == CTL_GAME_A);
   CHECK(m.bind[CTL_GAME_A] == 0 && m.bind[CTL_GAME_B] == 0);
   CHECK(ctl_game_mask(&m, CRO | CIR) == 0);       /* no plain press left  */
   CHECK(no_dupes(&m) && !ctl_is_default(&m));
   rig_init(&g, 0);
   n = 0;
   for (i = 0; i < 4; i++) rig_step(&m, &g, CRO, 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_A), "XX..") == 0 &&
         strcmp(pattern(out, n, BIT_B), "....") == 0);
   /* and back: binding A to Cross takes it from Turbo A */
   CHECK(ctl_assign(&m, CTL_GAME_A, CRO, &s) == CTL_OK && s == CTL_TURBO_A1);
   CHECK(m.bind[CTL_TURBO_A1] == 0 && m.bind[CTL_TURBO_B1] == CIR);
   /* a turbo button takes one from another turbo button, and from a
    * one-button shortcut; it overlaps a chord without conflict */
   CHECK(ctl_assign(&m, CTL_TURBO_A2, CIR, &s) == CTL_OK && s == CTL_TURBO_B1);
   CHECK(ctl_assign(&m, CTL_TURBO_B2, SQ, &s) == CTL_OK && s == CTL_SC_FF);
   CHECK(ctl_assign(&m, CTL_TURBO_B1, SEL, &s) == CTL_OK &&
         s == CTL_GAME_SELECT);
   CHECK(m.bind[CTL_SC_SAVE] == (SEL | L));
   /* Reset to defaults: all four unbound, 15, Hold */
   m.turbo_half[1] = 1;
   m.turbo_toggle[3] = 1;
   CHECK(!ctl_is_default(&m));
   ctl_reset(&m);
   CHECK(ctl_is_default(&m) && ctl_turbo_bound(&m) == 0);
   for (i = 0; i < CTL_TURBO_N; i++)
      CHECK(m.turbo_half[i] == 2 && m.turbo_toggle[i] == 0);
   /* a rate or mode alone makes the table "custom" */
   m.turbo_toggle[0] = 1;
   CHECK(!ctl_is_default(&m));
   m.turbo_toggle[0] = 0;
   m.turbo_half[2] = 3;
   CHECK(!ctl_is_default(&m));
   /* the longer-chord rule never looks at a turbo button: Turbo A on
    * SELECT does not make the SELECT chords stand down or fire */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_TURBO_A1, SEL, &s) == CTL_OK);
   CHECK(ctl_shortcuts(&m, SEL | L, L) == CTL_FIRE_SAVE);
   CHECK(ctl_shortcuts(&m, SEL, SEL) == 0);
   ctl_defaults(&d);
   for (i = 0; i < CTL_ACTIONS; i++)
      for (pad = 0; pad < 4096; pad += 7)
         CHECK(ctl_yields(&m, i, expand(pad)) == ctl_yields(&d, i, expand(pad)));
   for (i = CTL_SC_END; i < CTL_ACTIONS; i++)
      CHECK(!ctl_yields(&m, i, CTL_ALL));
   /* corruption (a rate we do not have) is repaired to the default */
   ctl_defaults(&m);
   m.turbo_half[0] = 0;
   m.turbo_half[1] = 9;
   m.turbo_toggle[2] = 7;
   CHECK(ctl_repair(&m, NULL, NULL) == 3);
   CHECK(m.turbo_half[0] == 2 && m.turbo_half[1] == 2 &&
         m.turbo_toggle[2] == 0 && ctl_repair(&m, NULL, NULL) == 0);
   /* ...and until then a bad rate pulses at the default, never divides by 0 */
   ctl_defaults(&m);
   ctl_assign(&m, CTL_TURBO_A1, TRI, &s);
   m.turbo_half[0] = 0;
   rig_init(&g, 0);
   n = 0;
   for (i = 0; i < 8; i++) rig_step(&m, &g, TRI, 1, 1, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_A), "XX..XX..") == 0);

   /* ---- CONFIG.INI ---- */
   /* a player who never binds one: the file is untouched, byte for byte */
   write_file("scale = 1\nbtn_swap = 0\nbind_ff = L+R\n");
   CHECK(ctl_load_ini(&m, ini) == 0 && ctl_turbo_bound(&m) == 0);
   CHECK(ctl_save_ini(&m, ini) == 0);
   CHECK(strcmp(read_file(), "scale = 1\nbtn_swap = 0\nbind_ff = L+R\n") == 0);
   /* bound, with its own rate and mode: three keys, and only the ones that
    * are not the default */
   CHECK(ctl_assign(&m, CTL_TURBO_A1, TRI, &s) == CTL_OK && s == CTL_SC_VIDEO);
   m.turbo_half[0] = 1;
   m.turbo_toggle[0] = 1;
   CHECK(ctl_assign(&m, CTL_TURBO_B2, START, &s) == CTL_OK &&
         s == CTL_GAME_START);
   CHECK(ctl_save_ini(&m, ini) == 0);
   CHECK(strcmp(read_file(),
                "scale = 1\nbtn_swap = 0\nbind_ff = L+R\n"
                "bind_start = NONE\nbind_video = NONE\n"
                "bind_turbo_a = TRIANGLE\nbind_turbo_b2 = START\n"
                "turbo_a_rate = 30\nturbo_a_mode = toggle\n") == 0);
   CHECK(ctl_load_ini(&d, ini) == 0);
   CHECK(memcmp(m.bind, d.bind, sizeof(m.bind)) == 0);
   CHECK(memcmp(m.turbo_half, d.turbo_half, sizeof(m.turbo_half)) == 0);
   CHECK(memcmp(m.turbo_toggle, d.turbo_toggle, sizeof(m.turbo_toggle)) == 0);
   CHECK(d.turbo_rate_keep[0] && d.turbo_mode_keep[0] &&
         !d.turbo_rate_keep[3] && !d.turbo_mode_keep[3]);
   /* the keep rule: back at the default, a key that is in the file is
    * rewritten rather than left saying the old value */
   d.turbo_half[0] = 2;
   d.turbo_toggle[0] = 0;
   CHECK(ctl_assign(&d, CTL_TURBO_A1, 0, &s) == CTL_OK);
   CHECK(ctl_save_ini(&d, ini) == 0);
   t = read_file();
   CHECK(strstr(t, "bind_turbo_a = NONE\n") && strstr(t, "turbo_a_rate = 15\n")
         && strstr(t, "turbo_a_mode = hold\n"));
   CHECK(strstr(t, "turbo_b2_rate") == NULL && strstr(t, "turbo_a2") == NULL);
   CHECK(ctl_load_ini(&m, ini) == 0 && m.turbo_half[0] == 2 &&
         !m.turbo_toggle[0] && m.bind[CTL_TURBO_A1] == 0);
   /* every rate and both modes round-trip on every action */
   for (i = 0; i < CTL_TURBO_N; i++)
      for (v = 0; v < 6; v++)
      {
         remove(ini);
         ctl_defaults(&m);
         m.turbo_half[i] = (uint8_t)(1 + v % 3);
         m.turbo_toggle[i] = (uint8_t)(v / 3);
         CHECK(ctl_save_ini(&m, ini) == 0);
         CHECK(ctl_load_ini(&d, ini) == 0);
         CHECK(memcmp(m.turbo_half, d.turbo_half, sizeof(m.turbo_half)) == 0);
         CHECK(memcmp(m.turbo_toggle, d.turbo_toggle,
                      sizeof(m.turbo_toggle)) == 0);
         CHECK(ctl_turbo_bound(&d) == 0);
      }
   /* hand-edited: garbage, empty and over-long values fall back to the
    * default and are reported; a chord on a turbo button likewise; the next
    * save repairs the lines */
   write_file("turbo_a_rate = 20\n"
              "turbo_a_mode = sticky\n"
              "turbo_b_rate =\n"
              "turbo_b2_mode = toggletoggletoggletoggletoggletoggletoggle\n"
              "turbo_a2_rate = 10\n"
              "turbo_b_mode = TOGGLE\n"
              "bind_turbo_a = L+R\n"
              "bind_turbo_b = stick_up\n");
#ifndef GPSP_PLAYABLE
   n_evt = 0;
#endif
   CHECK(ctl_load_ini(&m, ini) == 5);
#ifndef GPSP_PLAYABLE
   CHECK(n_evt == 5);
#endif
   CHECK(m.turbo_half[0] == 2 && m.turbo_toggle[0] == 0);
   CHECK(m.turbo_half[2] == 2 && m.turbo_toggle[3] == 0);
   CHECK(m.turbo_half[1] == 3 && m.turbo_toggle[2] == 1);
   CHECK(m.bind[CTL_TURBO_A1] == 0 && m.bind[CTL_TURBO_B1] == CTL_STICK_UP);
   CHECK(ctl_save_ini(&m, ini) == 0);
   t = read_file();
   CHECK(strstr(t, "turbo_a_rate = 15\n") && strstr(t, "turbo_a_mode = hold\n")
         && strstr(t, "turbo_b_rate = 15\n") &&
         strstr(t, "turbo_b2_mode = hold\n") &&
         strstr(t, "turbo_a2_rate = 10\n") &&
         strstr(t, "turbo_b_mode = toggle\n") &&
         strstr(t, "bind_turbo_a = NONE\n") &&
         strstr(t, "bind_turbo_b = STICK_UP\n"));
   /* LOAD-TIME DUPLICATES, by the rule the menu applies.  A turbo button on
    * a game button's default input is the explicit one and takes it; two
    * explicit ones go to the earlier action, and every turbo row is later
    * than every other row -- so a file's older keys resolve as they always
    * did, whatever turbo keys are added to it. */
   write_file("bind_turbo_a = CIRCLE\n");
   CHECK(ctl_load_ini(&m, ini) == 0);
   CHECK(m.bind[CTL_TURBO_A1] == CIR && m.bind[CTL_GAME_A] == 0);
   write_file("bind_a = TRIANGLE\nbind_turbo_a = TRIANGLE\n");
   CHECK(ctl_load_ini(&m, ini) == 0);
   CHECK(m.bind[CTL_GAME_A] == TRI && m.bind[CTL_TURBO_A1] == 0 &&
         m.bind[CTL_SC_VIDEO] == 0);
   write_file("bind_turbo_b2 = SQUARE\nbind_turbo_a = SQUARE\n");
   CHECK(ctl_load_ini(&m, ini) == 0);
   CHECK(m.bind[CTL_TURBO_A1] == SQ && m.bind[CTL_TURBO_B2] == 0 &&
         m.bind[CTL_SC_FF] == 0);
   CHECK(no_dupes(&m));
   /* EXISTING CONFIGS RESOLVE EXACTLY AS BEFORE.  Random files of the
    * pre-turbo keys only: no turbo row ever becomes bound, none is written,
    * and adding turbo keys on inputs nothing else uses changes none of the
    * other rows. */
   for (i = 0; i < 2000; i++)
   {
      char seed[2048] = "", seed2[2304];
      size_t o = 0;
      int a;
      unsigned used = 0, free_bit = 0;
      for (a = 0; a < CTL_SC_END; a++)
         if (rnd() % 3 == 0)
         {
            char val[48];
            unsigned b = random_binding(a);
            used |= b;
            ctl_format(b, val, sizeof(val), NULL, NULL);
            o += (size_t)snprintf(seed + o, sizeof(seed) - o, "%s = %s\n",
                                  ctl_info(a)->key, val);
         }
      write_file(seed);
      ctl_load_ini(&m, ini);
      CHECK(ctl_turbo_bound(&m) == 0 && no_dupes(&m));
      CHECK(ctl_save_ini(&m, ini) == 0);
      CHECK(strstr(read_file(), "turbo") == NULL);
      for (a = 0; a < CTL_SC_END; a++)
         used |= ctl_default(a);
      for (a = 0; a < CTL_NBUTTONS && !free_bit; a++)
         if (!(used & ctl_button_order(a)))
            free_bit = ctl_button_order(a);
      if (!free_bit)
         continue;
      {
         char val[24];
         ctl_format(free_bit, val, sizeof(val), NULL, NULL);
         snprintf(seed2, sizeof(seed2), "%sbind_turbo_b = %s\n"
                  "turbo_b_rate = 30\n", seed, val);
      }
      write_file(seed2);
      ctl_load_ini(&d, ini);
      CHECK(memcmp(m.bind, d.bind, CTL_SC_END * sizeof(m.bind[0])) == 0);
      CHECK(d.bind[CTL_TURBO_B1] == free_bit && d.turbo_half[2] == 1);
   }

   /* through pcfg_load / pcfg_save: an old card gains no key... */
   write_file("scale = 1\n");
   memset(&g_pcfg, 0x5A, sizeof(g_pcfg));
   pcfg_load(ini);
   CHECK(ctl_turbo_bound(&g_pcfg.controls) == 0 &&
         g_pcfg.controls.turbo_half[0] == 2 &&
         g_pcfg.controls.turbo_toggle[3] == 0);
   pcfg_save();
   CHECK(strstr(read_file(), "turbo") == NULL);
   {
      /* ...and what a save writes is the same file whether or not this
       * build knows about turbo: saving twice changes nothing */
      static char first[8192];
      snprintf(first, sizeof(first), "%s", read_file());
      memset(&g_pcfg, 0x5A, sizeof(g_pcfg));
      pcfg_load(ini);
      pcfg_save();
      CHECK(strcmp(first, read_file()) == 0);
   }
   /* ...and a bound one survives the round trip beside every other key */
   CHECK(ctl_assign(&g_pcfg.controls, CTL_TURBO_B1, TRI, &s) == CTL_OK);
   g_pcfg.controls.turbo_half[2] = 3;
   g_pcfg.controls.turbo_toggle[2] = 1;
   pcfg_save();
   t = read_file();
   CHECK(strstr(t, "bind_turbo_b = TRIANGLE\n") &&
         strstr(t, "turbo_b_rate = 10\n") &&
         strstr(t, "turbo_b_mode = toggle\n"));
   memset(&g_pcfg, 0, sizeof(g_pcfg));
   pcfg_load(ini);
   CHECK(g_pcfg.controls.bind[CTL_TURBO_B1] == TRI &&
         g_pcfg.controls.turbo_half[2] == 3 &&
         g_pcfg.controls.turbo_toggle[2] == 1 &&
         g_pcfg.controls.bind[CTL_SC_VIDEO] == 0);
   remove(ini);
}

/* ---- TURBO WITH REWIND (docs/CONTROL-REMAP.md section 13f) ---------------
 *
 * main_psp.c calls turbo_frame(pad, pad_new, !g_rw_held): on a loop whose
 * rewind control is held it is ctl_turbo_stop() and nothing else.  Modelled
 * here exactly so: a step with `held` stops, a step without is rig_step.
 * The frame counter the pulse reads keeps counting through a rewind (it is
 * not the rewind timeline), which the rig reproduces by advancing it.
 *
 * This half runs in every flavour.  The table half (rewind is the last
 * SHORTCUT, the turbo rows come after it) needs -DREWIND and is the whole of
 * the third build of this file: the other tests restate 3.0's defaults,
 * which a REWIND build changes (Triangle moves from Video preset). */
static void rw_step(const ctl_map *m, rig *g, unsigned pad, int held,
                    uint32_t *out, int *n)
{
   if (!held)
   {
      rig_step(m, g, pad, 1, 1, out, n);
      return;
   }
   ctl_turbo_stop(&g->t);          /* turbo_frame(.., playing = 0) */
   g->prev = pad;                  /* the loop's pad edge moves on regardless */
   out[(*n)++] = plat_mask(m, &g->t, pad, g->frame);
   g->frame++;
}

static void test_turbo_rewind(void)
{
   ctl_map m;
   rig g;
   uint32_t out[128];
   int n, i, s;

   /* HOLD, with the rewind control held in the middle of a press: nothing
    * fires while it is held, and on release the button that is still down
    * starts a FRESH train (on at once), not the old one's phase. */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_TURBO_A1, SQ, &s) == CTL_OK);
   m.turbo_half[0] = 3;
   rig_init(&g, 100);
   n = 0;
   for (i = 0; i < 20; i++)
      rw_step(&m, &g, SQ, i >= 4 && i < 11, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_A), "XXX........XXX...XXX") == 0);
   /* TOGGLE: a held rewind ends auto-fire, and it stays ended when play
    * resumes -- until the button is pressed again. */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_TURBO_B1, SQ, &s) == CTL_OK);
   m.turbo_toggle[2] = 1;
   rig_init(&g, 200);
   n = 0;
   rw_step(&m, &g, SQ, 0, out, &n);                       /* on            */
   for (i = 0; i < 5; i++) rw_step(&m, &g, 0, 0, out, &n);
   for (i = 0; i < 4; i++) rw_step(&m, &g, 0, 1, out, &n);  /* rewind held */
   CHECK(!g.t.live);
   for (i = 0; i < 6; i++) rw_step(&m, &g, 0, 0, out, &n);  /* play again  */
   CHECK(strcmp(pattern(out, n, BIT_B), "XX..XX..........") == 0);
   rw_step(&m, &g, SQ, 0, out, &n);
   CHECK((out[n - 1] & BIT_B) && g.t.live);
   /* a turbo button pressed WHILE the rewind is held does not arm a Toggle:
    * the press is spent, and letting the rewind go with it still down is
    * not a new press */
   rig_init(&g, 300);
   n = 0;
   rw_step(&m, &g, 0, 1, out, &n);
   rw_step(&m, &g, SQ, 1, out, &n);
   rw_step(&m, &g, SQ, 1, out, &n);
   for (i = 0; i < 6; i++) rw_step(&m, &g, SQ, 0, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_B), ".........") == 0 && !g.t.live);
   /* THE WHOLE GESTURE, not only the button (main_psp.c: playing is
    * !(g_rw_on && (g_rw_held || fe_host_rewind_busy()))).  After the control
    * is let go the rewind is still LANDING for some ticks -- no tick has
    * said PLAY yet -- and `held` here stands for that whole span.  A Hold
    * button down throughout: control held 3 ticks, landing 4 more, then
    * play.  Nothing until play; then a fresh train. */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_TURBO_A1, SQ, &s) == CTL_OK);
   rig_init(&g, 500);
   n = 0;
   for (i = 0; i < 4; i++) rw_step(&m, &g, SQ, 0, out, &n);
   for (i = 0; i < 3; i++) rw_step(&m, &g, SQ, 1, out, &n);   /* held     */
   for (i = 0; i < 4; i++) rw_step(&m, &g, SQ, 1, out, &n);   /* landing  */
   for (i = 0; i < 6; i++) rw_step(&m, &g, SQ, 0, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_A), "XX.........XX..XX") == 0);
   /* a Toggle pressed during the landing is not armed, and one that was
    * running before the gesture is off after a landing or a reset
    * (psp_rewind_land and the "game restarted" path are ctl_turbo_stop) */
   ctl_defaults(&m);
   CHECK(ctl_assign(&m, CTL_TURBO_B1, SQ, &s) == CTL_OK);
   m.turbo_toggle[2] = 1;
   rig_init(&g, 600);
   n = 0;
   rw_step(&m, &g, SQ, 0, out, &n);                        /* on           */
   rw_step(&m, &g, 0, 0, out, &n);
   rw_step(&m, &g, 0, 1, out, &n);                         /* held         */
   rw_step(&m, &g, 0, 1, out, &n);                         /* landing...   */
   rw_step(&m, &g, SQ, 1, out, &n);                        /* ...a press   */
   rw_step(&m, &g, 0, 1, out, &n);
   for (i = 0; i < 5; i++) rw_step(&m, &g, 0, 0, out, &n);
   CHECK(strcmp(pattern(out, n, BIT_B), "XX.........") == 0 && !g.t.live);
   rw_step(&m, &g, SQ, 0, out, &n);
   CHECK(g.t.live);
   ctl_turbo_stop(&g.t);                                   /* landed / reset */
   for (i = 0; i < 4; i++) rw_step(&m, &g, 0, 0, out, &n);
   CHECK(!g.t.live && !(out[n - 1] & BIT_B) && !(out[n - 3] & BIT_B));
   /* the game input of a held loop is the plain mapping, whatever is bound */
   ctl_defaults(&m);
   ctl_assign(&m, CTL_TURBO_A1, SQ, &s);
   rig_init(&g, 400);
   n = 0;
   rw_step(&m, &g, SQ, 0, out, &n);
   rw_step(&m, &g, SQ | CRO | UP, 1, out, &n);
   CHECK(out[1] == (BIT_B | (1u << JP_UP)));

#ifdef REWIND
   {
      ctl_map d;
      unsigned pad;
      /* THE ORDER: every shortcut, rewind last of them, then the four turbo
       * rows and nothing after. */
      CHECK(CTL_SC_REWIND == CTL_SC_END - 1);
      CHECK(CTL_SC_REWIND == CTL_SC_CATCH + 1);
      CHECK(CTL_TURBO_FIRST == CTL_SC_REWIND + 1);
      CHECK(CTL_ACTIONS == CTL_TURBO_FIRST + CTL_TURBO_N);
      CHECK(!strcmp(ctl_info(CTL_SC_REWIND)->key, "bind_rewind"));
      CHECK(!strcmp(ctl_info(CTL_TURBO_A1)->key, "bind_turbo_a") &&
            !strcmp(ctl_info(CTL_TURBO_B2)->key, "bind_turbo_b2"));
      CHECK(!(ctl_info(CTL_SC_REWIND)->flags & CTL_F_TURBO) &&
            (ctl_info(CTL_SC_REWIND)->flags & CTL_F_YIELD));
      for (i = 0; i < CTL_ACTIONS; i++)
         CHECK(((ctl_info(i)->flags & CTL_F_TURBO) != 0) == (i >= CTL_SC_END));
      /* the REWIND defaults are untouched: Triangle is rewind's, Video
       * preset starts blank, the turbo rows are unbound */
      ctl_defaults(&m);
      CHECK(m.bind[CTL_SC_REWIND] == TRI && m.bind[CTL_SC_VIDEO] == 0 &&
            ctl_turbo_bound(&m) == 0 && ctl_is_default(&m) && no_dupes(&m));
      /* WHAT "LAST" IS FOR still holds: an old CONFIG.INI that puts Triangle
       * on something else keeps that, and rewind is left blank... */
      write_file("bind_a = TRIANGLE\n");
      CHECK(ctl_load_ini(&m, ini) == 0);
      CHECK(m.bind[CTL_GAME_A] == TRI && m.bind[CTL_SC_REWIND] == 0);
      write_file("bind_video = TRIANGLE\n");
      CHECK(ctl_load_ini(&m, ini) == 0);
      CHECK(m.bind[CTL_SC_VIDEO] == TRI && m.bind[CTL_SC_REWIND] == 0);
      /* ...and the same files resolve the same with turbo keys added on
       * inputs nothing else uses */
      write_file("bind_a = TRIANGLE\nbind_turbo_b = STICK_UP\n"
                 "turbo_b_mode = toggle\n");
      CHECK(ctl_load_ini(&d, ini) == 0);
      CHECK(d.bind[CTL_GAME_A] == TRI && d.bind[CTL_SC_REWIND] == 0 &&
            d.bind[CTL_TURBO_B1] == CTL_STICK_UP && d.turbo_toggle[2] == 1);
      /* a default file is untouched, byte for byte, in this flavour too */
      write_file("scale = 1\nbtn_swap = 0\n");
      CHECK(ctl_load_ini(&m, ini) == 0 && ctl_is_default(&m));
      CHECK(ctl_save_ini(&m, ini) == 0);
      CHECK(strcmp(read_file(), "scale = 1\nbtn_swap = 0\n") == 0);
      /* a turbo button the player puts on Triangle takes it from rewind,
       * as any explicit binding does, in the menu and at load */
      ctl_defaults(&m);
      CHECK(ctl_assign(&m, CTL_TURBO_A1, TRI, &s) == CTL_OK &&
            s == CTL_SC_REWIND && m.bind[CTL_SC_REWIND] == 0);
      CHECK(!(ctl_shortcuts(&m, TRI, TRI) &
              (CTL_FIRE_REWIND | CTL_FIRE_REWIND_PRESS)));
      write_file("bind_turbo_a = TRIANGLE\n");
      CHECK(ctl_load_ini(&m, ini) == 0);
      CHECK(m.bind[CTL_TURBO_A1] == TRI && m.bind[CTL_SC_REWIND] == 0);
      /* two explicit ones: rewind, the earlier row, keeps it */
      write_file("bind_rewind = SQUARE\nbind_turbo_a = SQUARE\n");
      CHECK(ctl_load_ini(&m, ini) == 0);
      CHECK(m.bind[CTL_SC_REWIND] == SQ && m.bind[CTL_TURBO_A1] == 0 &&
            m.bind[CTL_SC_FF] == 0);
      /* and rewind takes a button back from a turbo row */
      ctl_defaults(&m);
      ctl_assign(&m, CTL_TURBO_B2, SQ, &s);
      CHECK(ctl_assign(&m, CTL_SC_REWIND, SQ, &s) == CTL_OK &&
            s == CTL_TURBO_B2);
      /* THE REWIND CONTROL IS READ EXACTLY AS BEFORE, turbo bound or not:
       * its level and edge for every pad word, and it still stands down
       * for SELECT+TRIANGLE (the longer-chord scan reaches it and stops
       * before the turbo rows). */
      ctl_defaults(&d);
      ctl_defaults(&m);
      CHECK(ctl_assign(&m, CTL_TURBO_A1, CTL_STICK_UP, &s) == CTL_OK);
      CHECK(ctl_assign(&m, CTL_TURBO_B1, CTL_STICK_DOWN, &s) == CTL_OK);
      m.turbo_toggle[2] = 1;
      for (pad = 0; pad < 0x10000u; pad++)
      {
         CHECK(ctl_shortcuts(&m, pad, pad) == ctl_shortcuts(&d, pad, pad));
         CHECK(ctl_shortcuts(&m, pad, 0) == ctl_shortcuts(&d, pad, 0));
         CHECK(ctl_game_mask(&m, pad) == ctl_game_mask(&d, pad));
      }
      CHECK(ctl_shortcuts(&d, TRI, TRI) ==
            (CTL_FIRE_REWIND | CTL_FIRE_REWIND_PRESS));
      CHECK(ctl_shortcuts(&d, TRI, 0) == CTL_FIRE_REWIND);
      CHECK(ctl_yields(&d, CTL_SC_REWIND, SEL | TRI));
      CHECK(!(ctl_shortcuts(&d, SEL | TRI, TRI) & CTL_FIRE_REWIND));
      for (i = CTL_SC_END; i < CTL_ACTIONS; i++)
         CHECK(!ctl_yields(&m, i, CTL_ALL));
      /* NOTHING BOUND, in this flavour: the input is the plain mapping for
       * every pad word, rewind held or not */
      rig_init(&g, 9);
      for (pad = 0; pad < 0x10000u; pad++)
      {
         ctl_turbo_pad(&d, &g.t, pad, pad, g.frame, 1);
         CHECK(!g.t.live &&
               plat_mask(&d, &g.t, pad, g.frame) == ctl_game_mask(&d, pad));
         g.frame++;
      }
      remove(ini);
   }
#endif
}

#ifdef REWIND
/* REWIND LENGTH (config_psp.h, docs/REWIND.md 26): CONFIG.INI `rewind_mb`
 * stays an integer of megabytes; the player's setting is one of three. */
static void test_rewind_length(void)
{
   static const struct { const char *text; int want; } load[] = {
      { "scale = 1\n", 2 },                       /* no key: Medium         */
      { "rewind_mb = 1\n", 1 },
      { "rewind_mb = 2\n", 2 },                   /* an rc1 / s3 card: kept */
      { "rewind_mb = 3\n", 2 },
      { "rewind_mb = 4\n", 4 },
      { "rewind_mb = 5\n", 4 },
      { "rewind_mb = 6\n", 4 },
      { "rewind_mb = 7\n", 4 },
      { "rewind_mb = 8\n", 4 },
      { "rewind_mb = 64\n", 4 },
      { "rewind_mb = 0\n", 1 },
      { "rewind_mb = -3\n", 1 },
      { "rewind_mb = banana\n", 2 },             /* not a number: the default */
      { "rewind_mb =\n", 2 },
   };
   unsigned i;
   long v;
   char *t;

   CHECK(PCFG_REWIND_MB_DEFAULT == 2);
   for (v = -20; v <= 100; v++)
   {
      int s = pcfg_rewind_mb_snap(v);
      CHECK(s == 1 || s == 2 || s == 4);
      CHECK(s <= (v < 1 ? 1 : v));                 /* never above what was asked */
      /* and the nearest such: no allowed size lies between it and v */
      CHECK(!(2 > s && 2 <= v) && !(4 > s && 4 <= v));
      CHECK(pcfg_rewind_mb_snap(s) == s);
   }
   CHECK(pcfg_rewind_mb_snap(3) == 2 && pcfg_rewind_mb_snap(5) == 4 &&
         pcfg_rewind_mb_snap(8) == 4 && pcfg_rewind_mb_snap(0) == 1);
   /* the row cycles Short -> Medium -> Long -> Short, and back */
   CHECK(pcfg_rewind_mb_step(1, +1) == 2 && pcfg_rewind_mb_step(2, +1) == 4 &&
         pcfg_rewind_mb_step(4, +1) == 1);
   CHECK(pcfg_rewind_mb_step(1, -1) == 4 && pcfg_rewind_mb_step(4, -1) == 2 &&
         pcfg_rewind_mb_step(2, -1) == 1);
   CHECK(pcfg_rewind_mb_step(3, +1) == 4 && pcfg_rewind_mb_step(8, +1) == 1);
   CHECK(!strcmp(pcfg_rewind_mb_name(1), "Short") &&
         !strcmp(pcfg_rewind_mb_name(2), "Medium") &&
         !strcmp(pcfg_rewind_mb_name(4), "Long") &&
         !strcmp(pcfg_rewind_mb_name(3), "Medium") &&
         !strcmp(pcfg_rewind_mb_name(0), "Short"));

   /* load: the mapping; save + load: what was loaded comes back, as the
    * integer of megabytes the harness and old builds read */
   for (i = 0; i < sizeof(load) / sizeof(load[0]); i++)
   {
      char want[32];
      write_file(load[i].text);
      memset(&g_pcfg, 0x5A, sizeof(g_pcfg));
      pcfg_load(ini);
      CHECK(g_pcfg.rewind_mb == load[i].want);
      pcfg_save();
      snprintf(want, sizeof(want), "rewind_mb = %d\n", load[i].want);
      t = read_file();
      CHECK(strstr(t, want) != NULL);
      memset(&g_pcfg, 0, sizeof(g_pcfg));
      pcfg_load(ini);
      CHECK(g_pcfg.rewind_mb == load[i].want);
   }
   /* every value the row can hold round-trips, with the toggle beside it */
   for (i = 0; i < 6; i++)
   {
      int mb = i % 3 == 0 ? 1 : i % 3 == 1 ? 2 : 4, on = (int)(i / 3);
      write_file("scale = 1\n");
      memset(&g_pcfg, 0x5A, sizeof(g_pcfg));
      pcfg_load(ini);
      CHECK(g_pcfg.rewind == PCFG_REWIND_DEFAULT && g_pcfg.rewind_mb == 2);
      g_pcfg.rewind = on;
      g_pcfg.rewind_mb = mb;
      pcfg_save();
      memset(&g_pcfg, 0, sizeof(g_pcfg));
      pcfg_load(ini);
      CHECK(g_pcfg.rewind == on && g_pcfg.rewind_mb == mb);
   }
   remove(ini);
}
#endif

int main(void)
{
#ifdef REWIND
   /* The third build: the REWIND table with the turbo rows, the rewind
    * length setting, and nothing else (see test_turbo_rewind). */
   test_turbo_rewind();
   test_rewind_length();
   printf("test_ctl_map: OK (%d checks, REWIND table + turbo)\n", checks);
   return 0;
#endif
   test_stick();
   test_table();
   test_rules();
   test_parse_format();
   test_conflicts();
   test_repair();
   test_ini();
   test_pcfg();
   test_menu_button();
   test_remapped();
   test_turbo();
   test_turbo_rewind();
   test_defaults_are_30();
   printf("test_ctl_map: OK (%d checks%s)\n", checks,
#ifdef GPSP_CATCH_SELFTEST
          ", catch self-test built in"
#else
          ""
#endif
          );
   return 0;
}
