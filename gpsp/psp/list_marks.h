/* list_marks.h -- what the game list knows about each game without asking
 * the Memory Stick while you scroll (4.6.0-beta.5, the beta.4 list lag).
 *
 * THE BUG.  beta.4 (issue #31 + the saves/ and states/ folders) taught the
 * list three facts per game: a battery save (the row's SAVE), save states
 * and a cheats file (the lit footer hints).  Each was a sceIoGetstat at the
 * moment it was drawn or the cursor arrived: the SAVE badge of a row
 * scrolling into view became TWO stats (saves/, then beside the ROM), and
 * the marks became up to 5 x 5 + 2 = 27 stats the instant the cursor
 * settled -- ~170 ms after a press, which is just when the next tap
 * arrives.  At 1-5 ms a stat (more on the Go's internal storage) that is a
 * 30-130 ms freeze on every tap: the "latency" the owner felt.
 *
 * THE RULE NOW.  Drawing and moving never touch the stick.  Each game's
 * facts are bits in a byte the list entry already had (rom_entry.has_sav
 * was a signed char: no new memory), each with a KNOWN bit.  A pump fills
 * them one stat at a time, only on frames where nothing was pressed and
 * nothing moves, under a time budget: the cursor's game first (all three
 * facts), then the SAVE of the rows on screen, then the games either side
 * of the cursor, then a sweep of the whole list.  Until a fact is known the
 * list draws it as absent; a stat that answers lights it.  A change the
 * browser makes itself (a state deleted, the cheats page) forgets that one
 * game's fact, and the pump looks again.
 *
 * Pure C with the stick behind a callback, so tools/tests/test_list_marks_
 * cache.c runs it on the host. */
#ifndef PSP_LIST_MARKS_H
#define PSP_LIST_MARKS_H

/* The byte's bits. */
#define LM_SAV_K   0x01      /* the battery-save question is answered */
#define LM_SAV     0x02      /*   ...and there is one                 */
#define LM_ST_K    0x04      /* save states answered                  */
#define LM_ST      0x08
#define LM_CHT_K   0x10      /* cheats file answered                  */
#define LM_CHT     0x20
#define LM_ALL_K   (LM_SAV_K | LM_ST_K | LM_CHT_K)

#ifndef LM_SLOTS
#define LM_SLOTS 5           /* PSP_STATE_SLOT_COUNT */
#endif

/* One probe is one stat.  The save: saves/<rel> first, then beside the ROM.
 * Each state slot: its states/ mirror, then beside the ROM.  Cheats:
 * cheats/<stem>.cht, then .txt.  The first that is there answers yes. */
enum
{
   LM_P_SAV_NEW = 0,
   LM_P_SAV_OLD = 1,
   LM_P_ST0     = 2,                       /* slot s: 2+2(s-1) mirror, +1 legacy */
   LM_P_CHT     = LM_P_ST0 + 2 * LM_SLOTS,
   LM_P_TXT,
   LM_P_END
};

typedef struct
{
   void *user;
   /* 1 = the probe's file is there, 0 = it is not (an error is "not"). */
   int (*exists)(void *user, int rom, int probe);
   unsigned char *(*marks)(void *user, int rom);
   int (*view_rom)(void *user, int view);
   /* Called before every probe with the probes made so far this run:
    * nonzero = make another.  The time budget lives here. */
   int (*more)(void *user, int probes);
   int cheats;               /* this build has a cheats question */
} lm_io;

typedef struct
{
   int rom, probe;           /* the question in progress                 */
   int sweep, swept;         /* the sweep's next view row; rows in a row
                              * found fully known (== n: nothing left)   */
} lm_pump;

static inline void lm_reset(lm_pump *p)
{
   p->rom = -1;
   p->probe = 0;
   p->sweep = 0;
   p->swept = 0;
}

/* Forget `bits` (any of LM_SAV_K | LM_ST_K | LM_CHT_K) of one game: its
 * answer is cleared and the pump asks again. */
static inline void lm_forget(lm_pump *p, unsigned char *m, unsigned bits)
{
   if (bits & LM_SAV_K) *m &= (unsigned char)~(LM_SAV_K | LM_SAV);
   if (bits & LM_ST_K)  *m &= (unsigned char)~(LM_ST_K | LM_ST);
   if (bits & LM_CHT_K) *m &= (unsigned char)~(LM_CHT_K | LM_CHT);
   p->swept = 0;
   if (p->rom >= 0)
      p->probe = 0;          /* the question in progress restarts */
}

/* An answer found some other way (the state shelf stats every slot when it
 * opens): recorded, so the pump never asks it. */
static inline void lm_set(unsigned char *m, unsigned known, int yes)
{
   unsigned val = known << 1;             /* LM_x is LM_x_K << 1 */
   *m = (unsigned char)((*m & ~(known | val)) | known | (yes ? val : 0));
}

/* The bits of `want` still unanswered for this game (no I/O). */
static inline unsigned lm_open(const lm_io *io, int rom, unsigned want)
{
   return want & LM_ALL_K & ~(unsigned)*io->marks(io->user, rom);
}

/* ONE probe towards the first unanswered question of `want`.  1 = a probe
 * was made, 0 = nothing of `want` is left (and nothing was touched). */
static inline int lm_step(lm_pump *p, const lm_io *io, int rom, unsigned want)
{
   unsigned char *m = io->marks(io->user, rom);
   int lo, hi, k, yes;
   unsigned kbit;

   if (!io->cheats && (want & LM_CHT_K) && !(*m & LM_CHT_K))
      *m |= LM_CHT_K;                     /* no cheats in this build: no */
   want &= LM_ALL_K & ~*m;
   if (!want)
      return 0;
   if (want & LM_SAV_K)
      lo = LM_P_SAV_NEW, hi = LM_P_SAV_OLD, kbit = LM_SAV_K;
   else if (want & LM_ST_K)
      lo = LM_P_ST0, hi = LM_P_CHT - 1, kbit = LM_ST_K;
   else
      lo = LM_P_CHT, hi = LM_P_TXT, kbit = LM_CHT_K;
   if (p->rom != rom || p->probe < lo || p->probe > hi)
   {
      p->rom = rom;
      p->probe = lo;
   }
   k = p->probe;
   yes = io->exists(io->user, rom, k) > 0;
   if (yes || k == hi)
   {
      lm_set(m, kbit, yes);
      p->probe = hi + 1;
   }
   else
      p->probe = k + 1;
   return 1;
}

static inline int lm_wrap(int v, int n)
{
   return ((v % n) + n) % n;
}

/* What to ask next: a game (g_roms index) and the questions wanted of it,
 * or -1 when every game in the view is answered.  `rows_up`/`rows_dn` are
 * the rows on screen either side of the cursor, `win` the neighbours whose
 * every fact is fetched ahead of the cursor. */
static inline int lm_pick(lm_pump *p, const lm_io *io, int cur, int n,
                          int rows_up, int rows_dn, int win, unsigned *want)
{
   int d, rom;
   if (n <= 0)
      return -1;
   rom = io->view_rom(io->user, lm_wrap(cur, n));
   if (lm_open(io, rom, LM_ALL_K))
   {
      *want = LM_ALL_K;
      return rom;
   }
   for (d = 1; d <= (rows_up > rows_dn ? rows_up : rows_dn); d++)
   {
      if (d <= rows_dn && d < n &&
          lm_open(io, rom = io->view_rom(io->user, lm_wrap(cur + d, n)),
                  LM_SAV_K))
         return *want = LM_SAV_K, rom;
      if (d <= rows_up && d < n &&
          lm_open(io, rom = io->view_rom(io->user, lm_wrap(cur - d, n)),
                  LM_SAV_K))
         return *want = LM_SAV_K, rom;
   }
   for (d = 1; d <= win && d < n; d++)
   {
      if (lm_open(io, rom = io->view_rom(io->user, lm_wrap(cur + d, n)),
                  LM_ALL_K))
         return *want = LM_ALL_K, rom;
      if (lm_open(io, rom = io->view_rom(io->user, lm_wrap(cur - d, n)),
                  LM_ALL_K))
         return *want = LM_ALL_K, rom;
   }
   if (p->sweep >= n)
      p->sweep = 0;
   while (p->swept < n)
   {
      rom = io->view_rom(io->user, p->sweep);
      if (lm_open(io, rom, LM_ALL_K))
         return *want = LM_ALL_K, rom;
      p->sweep = (p->sweep + 1) % n;
      p->swept++;
   }
   return -1;
}

/* One frame's worth: probes while io->more() agrees and anything is left.
 * Returns the probes made.  Call it only on a frame where nothing was
 * pressed and nothing moves. */
static inline int lm_run(lm_pump *p, const lm_io *io, int cur, int n,
                         int rows_up, int rows_dn, int win)
{
   int done = 0, rom;
   unsigned want;
   while (io->more(io->user, done))
   {
      rom = lm_pick(p, io, cur, n, rows_up, rows_dn, win, &want);
      if (rom < 0)
         break;
      if (lm_step(p, io, rom, want))
         done++;
   }
   return done;
}

#endif
