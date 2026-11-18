/* link_games.h -- which way a GBA game links: Link Cable, Wireless Adapter,
 * or both (docs/GBA-LINK-UI.md; the design: docs/design/UI-MOCKUPS.md).
 *
 * THE GAME DECIDES.  The player should almost never have to know which link
 * a game uses, so the Wireless menu asks this table:
 *
 *   LINK_CABLE    Host = a Link Cable room (the mirror: each PSP runs both
 *                 GBAs, only buttons cross the air).  Every UNLISTED game.
 *   LINK_ADAPTER  Host = a Wireless Adapter room (rfu.c, the 1.0 feature).
 *   LINK_BOTH     Host asks once (the HOST chooser).  Join never asks: the
 *                 room's name says which it is.
 *
 * Keyed by the first THREE bytes of the 4-byte header code (0xAC..0xAE), the
 * way gba_memory.c keys its own overrides, so one row covers every region.
 * Sorted by code, binary-searched; codes unique; titles <= 40 characters
 * (the scan list's game column is 230 px; clip_title handles the rest).
 * tools/test_link_mode.c checks all three.
 *
 * Cable rows exist only to NAME the game in another PSP's scan list -- cable
 * is the default anyway.  An adapter row is "no question asked".  Which
 * adapter titles also link by cable is a judgement (open question 2 of the
 * design; docs/GBA-LINK-UI.md "Choices"):
 *   - BOTH where the cable game is the same game and the mirror has run it
 *     (Pokemon FR/LG/E trades and battles, Mario Tennis: the multigame
 *     matrix), or the cable mode is the game's main multiplayer mode
 *     (Mega Man Battle Network 5/6 net battles);
 *   - ADAPTER where the cable is known not to form on the mirror (Mario
 *     Golf, GBA-LINK-MIRROR.md 16.4) or is unvalidated beside a working
 *     adapter mode.  Settings > Link mode overrides either, per game.
 * rfu.c and the RFU routing are untouched: this only picks the default.
 *
 * Pure C, header-only, shared by the PSP UI and the host test. */
#ifndef LINK_GAMES_H
#define LINK_GAMES_H

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { LINK_CABLE = 0, LINK_ADAPTER = 1, LINK_BOTH = 2 };

typedef struct
{
   char code[3];
   unsigned char mode;
   const char *title;
} link_game;

static const link_game LINK_GAMES[] = {   /* sorted by code */
   { {'A','3','X'}, LINK_CABLE,   "Mega Man Battle Network 3 Blue" },
   { {'A','6','B'}, LINK_CABLE,   "Mega Man Battle Network 3 White" },
   { {'A','F','Z'}, LINK_CABLE,   "F-Zero Maximum Velocity" },
   { {'A','G','F'}, LINK_CABLE,   "Golden Sun The Lost Age" },
   { {'A','G','S'}, LINK_CABLE,   "Golden Sun" },
   { {'A','M','K'}, LINK_CABLE,   "Mario Kart Super Circuit" },
   { {'A','R','E'}, LINK_CABLE,   "Mega Man Battle Network" },
   { {'A','S','O'}, LINK_CABLE,   "Sonic Advance" },
   { {'A','W','2'}, LINK_CABLE,   "Advance Wars 2" },
   { {'A','W','R'}, LINK_CABLE,   "Advance Wars" },
   { {'A','X','P'}, LINK_CABLE,   "Pokemon Sapphire" },
   { {'A','X','V'}, LINK_CABLE,   "Pokemon Ruby" },
   { {'B','2','W'}, LINK_ADAPTER, "The Chronicles of Narnia" },
   { {'B','3','A'}, LINK_ADAPTER, "The Lord of the Rings The Third Age" },
   { {'B','4','B'}, LINK_CABLE,   "Mega Man Battle Network 4 Blue Moon" },
   { {'B','4','U'}, LINK_ADAPTER, "Shrek Super Slam" },
   { {'B','4','W'}, LINK_CABLE,   "Mega Man Battle Network 4 Red Sun" },
   { {'B','8','5'}, LINK_ADAPTER, "Hamtaro Ham-Ham Games" },
   { {'B','D','G'}, LINK_ADAPTER, "Digimon Racing" },
   { {'B','G','3'}, LINK_ADAPTER, "Dragon Ball Z Buu's Fury" },
   { {'B','K','R'}, LINK_ADAPTER, "No No No Puzzle Chailien" },
   { {'B','M','G'}, LINK_ADAPTER, "Mario Golf Advance Tour" },
   { {'B','P','E'}, LINK_BOTH,    "Pokemon Emerald" },
   { {'B','P','G'}, LINK_BOTH,    "Pokemon LeafGreen" },
   { {'B','P','R'}, LINK_BOTH,    "Pokemon FireRed" },
   { {'B','R','5'}, LINK_BOTH,    "Mega Man Battle Network 6 Gregar" },
   { {'B','R','6'}, LINK_BOTH,    "Mega Man Battle Network 6 Falzar" },
   { {'B','R','B'}, LINK_BOTH,    "Mega Man Battle Network 5 Protoman" },
   { {'B','R','K'}, LINK_BOTH,    "Mega Man Battle Network 5 Colonel" },
   { {'B','T','M'}, LINK_BOTH,    "Mario Tennis Power Tour" },
};

#define LINK_GAMES_N (sizeof(LINK_GAMES) / sizeof(LINK_GAMES[0]))

/* Byte order, as the table is sorted: '0'..'9' before 'A'..'Z'. */
static inline int link_game_cmp(const char *a, const char b[3])
{
   return memcmp(a, b, 3);
}

/* The row for a 4-byte header code (only the first three bytes count), or
 * NULL when the game is not listed. */
static inline const link_game *link_game_find(const char *code)
{
   size_t lo = 0, hi = LINK_GAMES_N;
   if (!code || !code[0] || !code[1] || !code[2])
      return NULL;
   while (lo < hi)
   {
      size_t mid = (lo + hi) / 2;
      int c = link_game_cmp(code, LINK_GAMES[mid].code);
      if (c == 0)
         return &LINK_GAMES[mid];
      if (c < 0)
         hi = mid;
      else
         lo = mid + 1;
   }
   return NULL;
}

/* LINK_CABLE / LINK_ADAPTER / LINK_BOTH; an unlisted game is LINK_CABLE. */
static inline int link_game_mode(const char *code)
{
   const link_game *g = link_game_find(code);
   return g ? g->mode : LINK_CABLE;
}

/* The game's name for another PSP's scan list, or NULL when unlisted. */
static inline const char *link_game_title(const char *code)
{
   const link_game *g = link_game_find(code);
   return g ? g->title : NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* LINK_GAMES_H */
