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
 *      steal, a chord shortcut over game buttons). */
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
      for (b = a + 1; b < CTL_ACTIONS; b++)
      {
         CHECK(strcmp(in->key, ctl_info(b)->key) != 0);
         CHECK(in->def != ctl_info(b)->def);
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
   int n = (a < CTL_GAME_COUNT) ? 1 : 1 + (int)(rnd() % 4), i;
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
      if (rnd() % 10 == 0)
         ctl_reset(&m);
      CHECK(ctl_save_ini(&m, ini) == 0);
      CHECK(ctl_load_ini(&d, ini) == 0);
      if (memcmp(m.bind, d.bind, sizeof(m.bind)) != 0)
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

int main(void)
{
   test_table();
   test_rules();
   test_parse_format();
   test_conflicts();
   test_repair();
   test_ini();
   test_pcfg();
   test_menu_button();
   test_remapped();
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
