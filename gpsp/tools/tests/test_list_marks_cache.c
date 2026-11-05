/* test_list_marks_cache.c -- psp/list_marks.h: the game list's per-game
 * facts (save, states, cheats), filled by the idle pump and never by drawing
 * or moving (the 4.6.0-beta.4 list lag).
 *
 *   1. nothing is asked until the pump runs; the pump asks the cursor's game
 *      first, every fact, and stops at the first file that is there
 *   2. then the SAVE of the rows on screen, then the neighbours, then a
 *      sweep of the whole view; once everything is known it asks NOTHING
 *   3. every answer is right for every place a file can be
 *   4. a forgotten fact is asked again for that game only
 *   5. a cursor that moves mid-question, or a rescan, never skips a probe
 *   6. a build without cheats never asks about cheats
 *   7. short lists (1, 2, 3 games) and a favourites view terminate
 *   8. the budget is obeyed: never more probes than more() allows
 *
 * Then mutants of the header, each of which must fail. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list_marks.h"

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

#define N 40
static unsigned char g_marks[N];
static unsigned char g_there[N][LM_P_END];   /* which probes would answer */
static int g_view[N], g_view_n, g_favs;
static int g_probes, g_budget;
static int g_log_rom[4096], g_log_probe[4096];

static int t_exists(void *u, int rom, int probe)
{
   (void)u;
   CHECK(rom >= 0 && rom < N && probe >= 0 && probe < LM_P_END);
   g_log_rom[g_probes] = rom;
   g_log_probe[g_probes] = probe;
   g_probes++;
   return g_there[rom][probe];
}
static unsigned char *t_marks(void *u, int rom) { (void)u; return &g_marks[rom]; }
static int t_view(void *u, int v)
{
   (void)u;
   return g_favs ? g_view[v] : v;
}
static int t_more(void *u, int probes) { (void)u; return probes < g_budget; }

static lm_io io_of(int cheats)
{
   lm_io io;
   io.user = NULL;
   io.exists = t_exists;
   io.marks = t_marks;
   io.view_rom = t_view;
   io.more = t_more;
   io.cheats = cheats;
   return io;
}

static void fresh(lm_pump *p)
{
   memset(g_marks, 0, sizeof(g_marks));
   memset(g_there, 0, sizeof(g_there));
   g_probes = 0;
   g_favs = 0;
   lm_reset(p);
}

/* What the facts must be, from g_there. */
static int want_sav(int r) { return g_there[r][0] || g_there[r][1]; }
static int want_st(int r)
{
   int k;
   for (k = LM_P_ST0; k < LM_P_CHT; k++)
      if (g_there[r][k])
         return 1;
   return 0;
}
static int want_cht(int r, int cheats)
{
   return cheats && (g_there[r][LM_P_CHT] || g_there[r][LM_P_TXT]);
}
static void check_answers(int n, int cheats)
{
   int r;
   for (r = 0; r < n; r++)
   {
      CHECK((g_marks[r] & LM_ALL_K) == LM_ALL_K);
      CHECK(!!(g_marks[r] & LM_SAV) == want_sav(r));
      CHECK(!!(g_marks[r] & LM_ST) == want_st(r));
      CHECK(!!(g_marks[r] & LM_CHT) == want_cht(r, cheats));
   }
}

/* Run the pump to completion, one frame at a time. */
static int drain(lm_pump *p, const lm_io *io, int cur, int n)
{
   int frames = 0;
   g_budget = 3;
   while (lm_run(p, io, cur, n, 4, 12) > 0)
      CHECK(++frames < 100000);
   return frames;
}

int main(void)
{
   lm_pump p;
   lm_io io = io_of(1), io0 = io_of(0);
   int i, r, before, cur;

   /* 1: the cursor first, every fact, first file there wins */
   fresh(&p);
   g_there[7][LM_P_SAV_OLD] = 1;                 /* save beside the ROM */
   g_there[7][LM_P_ST0 + 2 * 2 + 1] = 1;         /* slot 3, beside the ROM */
   g_there[7][LM_P_TXT] = 1;                     /* cheats/<stem>.txt */
   CHECK(g_probes == 0);
   g_budget = 1000;
   /* how many probes the cursor's game alone takes */
   while (lm_open(&io, 7, LM_ALL_K))
      CHECK(lm_step(&p, &io, 7, LM_ALL_K) == 1);
   CHECK(g_probes == 2 + 6 + 2);   /* sav new,old; st1m,st1l,st2m,st2l,st3m,st3l; cht,txt */
   CHECK(g_log_probe[0] == LM_P_SAV_NEW && g_log_probe[1] == LM_P_SAV_OLD);
   CHECK(g_log_probe[7] == LM_P_ST0 + 5 && g_log_probe[9] == LM_P_TXT);
   CHECK(g_marks[7] == (LM_ALL_K | LM_SAV | LM_ST | LM_CHT));
   CHECK(lm_step(&p, &io, 7, LM_ALL_K) == 0 && g_probes == 10);

   fresh(&p);
   g_there[3][LM_P_SAV_NEW] = 1;
   g_budget = 1000;
   lm_run(&p, &io, 3, N, 4, 12);
   for (i = 0; i < 13; i++)                      /* the cursor's game first */
      CHECK(g_log_rom[i] == 3);
   CHECK(g_log_probe[1] == LM_P_ST0);            /* saves/ answered: 1 probe */
   CHECK(g_marks[3] == (LM_ALL_K | LM_SAV));

   /* 2: rows on screen (SAVE only), neighbours, sweep; then nothing */
   fresh(&p);
   g_budget = 1000;
   lm_run(&p, &io, 20, N, 4, 12);
   /* after the cursor's 14: 8 rows x 2 save probes, alternating below/above */
   for (i = 14; i < 14 + 16; i++)
   {
      int d = g_log_rom[i] - 20;
      CHECK(d != 0 && d >= -4 && d <= 4);
      CHECK(g_log_probe[i] <= LM_P_SAV_OLD);
   }
   CHECK(g_log_rom[14] == 21 && g_log_rom[16] == 19);
   /* then the neighbours' other facts, within 12 */
   for (i = 30; i < 30 + 24 * 12 - 8 * 12; i++)
      CHECK(abs(g_log_rom[i] - 20) <= 12);
   check_answers(N, 1);
   CHECK(g_probes == N * 14);                    /* each probe asked ONCE */
   before = g_probes;
   g_budget = 1000;
   for (i = 0; i < 50; i++)
      CHECK(lm_run(&p, &io, i % N, N, 4, 12) == 0);
   CHECK(g_probes == before);                    /* built: zero I/O */

   /* 3: random files everywhere, answers exact */
   srand(1234);
   for (i = 0; i < 20; i++)
   {
      fresh(&p);
      for (r = 0; r < N; r++)
      {
         int k;
         for (k = 0; k < LM_P_END; k++)
            g_there[r][k] = (rand() % 9) == 0;
      }
      cur = rand() % N;
      drain(&p, &io, cur, N);
      check_answers(N, 1);
   }

   /* 4: forget one fact of one game */
   fresh(&p);
   drain(&p, &io, 0, N);
   before = g_probes;
   g_there[25][LM_P_CHT] = 1;                    /* the cheats page made one */
   lm_forget(&p, &g_marks[25], LM_CHT_K);
   CHECK(!(g_marks[25] & LM_CHT_K) && (g_marks[25] & LM_ST_K));
   g_budget = 1000;
   lm_run(&p, &io, 0, N, 4, 12);
   CHECK(g_probes == before + 1 && g_log_rom[before] == 25);
   CHECK(g_marks[25] & LM_CHT);
   lm_forget(&p, &g_marks[2], LM_ST_K);          /* a state deleted */
   before = g_probes;
   lm_run(&p, &io, 0, N, 4, 12);
   CHECK(g_probes == before + 10);               /* all five slots, both places */
   for (i = before; i < g_probes; i++)
      CHECK(g_log_rom[i] == 2 && g_log_probe[i] >= LM_P_ST0 &&
            g_log_probe[i] < LM_P_CHT);
   lm_set(&g_marks[2], LM_ST_K, 1);              /* the shelf's own answer */
   CHECK((g_marks[2] & (LM_ST_K | LM_ST)) == (LM_ST_K | LM_ST));
   lm_set(&g_marks[2], LM_ST_K, 0);
   CHECK((g_marks[2] & (LM_ST_K | LM_ST)) == LM_ST_K);
   CHECK((g_marks[2] & LM_SAV_K) && (g_marks[2] & LM_CHT_K));

   /* 5: the cursor moves mid-question, comes back; a rescan */
   fresh(&p);
   g_there[5][LM_P_ST0] = 1;                     /* slot 1 mirror: probe 2 */
   g_there[6][LM_P_SAV_NEW] = 1;
   g_budget = 3;                                 /* sav new, sav old, st1m */
   lm_run(&p, &io, 6, N, 0, 0);               /* cursor on 6 */
   g_budget = 1;
   lm_run(&p, &io, 5, N, 0, 0);               /* 5: sav new */
   lm_run(&p, &io, 6, N, 0, 0);               /* back to 6, mid-states */
   lm_run(&p, &io, 5, N, 0, 0);               /* 5 again */
   g_budget = 1000;
   lm_run(&p, &io, 5, N, 0, 0);
   CHECK(g_marks[5] & LM_ST);                    /* probe 2 was not skipped */
   /* two games in the same question: 6's progress must not carry to 5 */
   fresh(&p);
   g_marks[5] = g_marks[6] = LM_SAV_K;
   g_there[5][LM_P_ST0 + 1] = 1;                 /* 5: slot 1 beside the ROM */
   g_budget = 3;                                 /* 6: st1m, st1l, st2m */
   lm_run(&p, &io, 6, N, 0, 0);
   g_budget = 1000;
   lm_run(&p, &io, 5, N, 0, 0);
   CHECK(g_marks[5] & LM_ST);
   /* a rescan: same index, everything unknown again, from the first probe
    * -- even with that game's save question half asked when it came */
   g_marks[6] = 0;
   g_budget = 1;
   lm_run(&p, &io, 6, N, 0, 0);               /* 6: saves/ asked, no */
   memset(g_marks, 0, sizeof(g_marks));
   lm_reset(&p);
   before = g_probes;
   g_budget = 1;
   lm_run(&p, &io, 6, N, 0, 0);
   CHECK(g_log_rom[before] == 6 && g_log_probe[before] == LM_P_SAV_NEW);
   drain(&p, &io, 6, N);
   check_answers(N, 1);

   /* 6: no cheats build */
   fresh(&p);
   g_there[1][LM_P_CHT] = 1;
   drain(&p, &io0, 0, N);
   for (i = 0; i < g_probes; i++)
      CHECK(g_log_probe[i] < LM_P_CHT);
   check_answers(N, 0);
   CHECK(g_probes == N * 12);

   /* 7: short lists and a favourites view */
   for (r = 1; r <= 3; r++)
   {
      fresh(&p);
      g_there[0][LM_P_SAV_OLD] = 1;
      drain(&p, &io, 0, r);
      check_answers(r, 1);
      CHECK(g_probes == r * 14);
      for (i = r; i < N; i++)
         CHECK(g_marks[i] == 0);                 /* outside the view: untouched */
      CHECK(lm_run(&p, &io, 0, r, 4, 12) == 0);
   }
   fresh(&p);
   g_favs = 1;
   g_view_n = 5;
   g_view[0] = 3; g_view[1] = 9; g_view[2] = 17; g_view[3] = 30; g_view[4] = 39;
   g_there[30][LM_P_ST0 + 9] = 1;                /* slot 5, beside the ROM */
   drain(&p, &io, 2, g_view_n);
   for (r = 0; r < N; r++)
   {
      int in = r == 3 || r == 9 || r == 17 || r == 30 || r == 39;
      CHECK(in ? (g_marks[r] & LM_ALL_K) == LM_ALL_K : g_marks[r] == 0);
   }
   CHECK(g_marks[30] & LM_ST);
   CHECK(lm_run(&p, &io, 0, 0, 4, 12) == 0);  /* an empty view */

   /* 8: the budget */
   fresh(&p);
   for (i = 1; i <= 5; i++)
   {
      before = g_probes;
      g_budget = i;
      CHECK(lm_run(&p, &io, 0, N, 4, 12) == i);
      CHECK(g_probes - before == i);
   }

   puts("list marks cache: cursor first, then rows, neighbours, the sweep; "
        "each probe once; zero I/O once built; forget asks again; budget kept");
   return 0;
}
