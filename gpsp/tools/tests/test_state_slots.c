/* test_state_slots.c -- the per-game slot names and the slot lists' window
 * (psp/state_slots.h; docs/UI-OVERLAY.md section 15.2).
 *
 *   1. Every name a 4.5.0 state was ever saved under still means the same
 *      slot: slot k of 1..5 is ".st(k-1)" exactly, for a GBA game and for
 *      GB/GBC (Game.gb.st0, Game.gbc.st0 ...), so states, .thumb and .shot
 *      files already on a stick open where they did.
 *   2. The slots after them follow the same rule up to .st9 -- one digit,
 *      so psp_state_delete_paths' "replace the last digit" stays exact --
 *      and nothing past PSP_STATE_SLOT_COUNT, or slot 0, has a name.
 *   3. The list window (psp_state_slot_top): whatever the cursor does --
 *      a step, a wrap, a jump -- it is on screen, the window never shows
 *      past the last slot, and the window moves only when the cursor would
 *      otherwise leave it, by as little as it can.  With no more slots than
 *      rows it never moves at all (the 4.5.0 page). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "state_slots.h"

static int checks;
#define CHECK(c) do { checks++; if (!(c)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
   exit(1); } } while (0)

/* One step of the window as a list draws it: the cursor's new slot, the
 * window kept or moved, and every invariant of section 3. */
static int window_step(int cur, int top, int n, int rows)
{
   int t = psp_state_slot_top(cur, top, n, rows);
   CHECK(t >= 0);
   CHECK(n <= rows ? t == 0 : t <= n - rows);
   CHECK(cur - 1 >= t && cur - 1 < t + rows);           /* on screen */
   if (cur - 1 >= top && cur - 1 < top + rows && top >= 0 &&
       (n <= rows ? top == 0 : top <= n - rows))
      CHECK(t == top);                  /* already visible: no move */
   else if (cur - 1 < top)
      CHECK(t == cur - 1);              /* above: it becomes the top row */
   else if (n > rows && top <= n - rows)
      CHECK(t == cur - rows);           /* below: the bottom row */
   return t;
}

int main(void)
{
   static const struct { const char *rom, *slot1; fe_console_t con; } games[] = {
      { "ms0:/PSP/GAME/GBAdhoc/roms/Pokemon Emerald.gba",
        "ms0:/PSP/GAME/GBAdhoc/roms/Pokemon Emerald.st0", FE_CONSOLE_GBA },
      { "ms0:/PSP/GAME/GBAdhoc/roms/sub dir/Red.gb",
        "ms0:/PSP/GAME/GBAdhoc/roms/sub dir/Red.gb.st0", FE_CONSOLE_GB },
      { "ms0:/PSP/GAME/GBAdhoc/roms/Crystal.gbc",
        "ms0:/PSP/GAME/GBAdhoc/roms/Crystal.gbc.st0", FE_CONSOLE_GBC },
      { "ms0:/roms.v2/Game.with.dots.gba", "ms0:/roms.v2/Game.with.dots.st0",
        FE_CONSOLE_GBA },
   };
   char slot1[PSP_FILE_PATH_CAP], path[PSP_FILE_PATH_CAP], want[PSP_FILE_PATH_CAP];
   char th[PSP_STATE_THUMB_PATH_CAP], sh[PSP_STATE_SHOT_PATH_CAP];
   unsigned g, slot;

   CHECK(PSP_STATE_SLOT_COUNT == 10);
   CHECK(PSP_STATE_SLOT_ROWS == 5);

   for (g = 0; g < sizeof(games) / sizeof(games[0]); g++)
   {
      size_t stem;
      CHECK(psp_rom_path_suffix(slot1, sizeof(slot1), games[g].rom,
                                psp_state_slot1_suffix(games[g].con)) == 0);
      CHECK(strcmp(slot1, games[g].slot1) == 0);
      stem = strlen(slot1) - 1;               /* everything but the digit */

      /* 1 + 2: slot k is "<stem><k-1>", 1..10; its previews beside it */
      for (slot = 1; slot <= PSP_STATE_SLOT_COUNT; slot++)
      {
         memcpy(want, slot1, stem);
         want[stem] = (char)('0' + slot - 1);
         want[stem + 1] = '\0';
         CHECK(psp_state_path_for_slot(path, sizeof(path), slot1, slot) == 0);
         CHECK(strcmp(path, want) == 0);
         CHECK(psp_state_thumb_path(th, sizeof(th), path) == 0);
         CHECK(strlen(th) == strlen(want) + 6 && !strcmp(th + strlen(want), ".thumb"));
         CHECK(psp_state_shot_path(sh, sizeof(sh), path) == 0);
         CHECK(strlen(sh) == strlen(want) + 5 && !strcmp(sh + strlen(want), ".shot"));
      }
      /* nothing outside 1..10 is named, and the output says so */
      for (slot = 0; slot <= 40; slot += (slot == 0 ? PSP_STATE_SLOT_COUNT + 1 : 7))
      {
         strcpy(path, "junk");
         CHECK(psp_state_path_for_slot(path, sizeof(path), slot1, slot) == -1);
      }
   }
   /* The historic names, spelled out: these files exist on sticks today. */
   {
      static const char *gba[] = { "/r/Ruby.st0", "/r/Ruby.st1", "/r/Ruby.st2",
                                   "/r/Ruby.st3", "/r/Ruby.st4" };
      static const char *gbc[] = { "/r/Gold.gbc.st0", "/r/Gold.gbc.st1",
                                   "/r/Gold.gbc.st2", "/r/Gold.gbc.st3",
                                   "/r/Gold.gbc.st4" };
      for (slot = 1; slot <= 5; slot++)
      {
         CHECK(psp_state_path_for_slot(path, sizeof(path), "/r/Ruby.st0", slot) == 0);
         CHECK(strcmp(path, gba[slot - 1]) == 0);
         CHECK(psp_state_path_for_slot(path, sizeof(path), "/r/Gold.gbc.st0",
                                       slot) == 0);
         CHECK(strcmp(path, gbc[slot - 1]) == 0);
      }
      CHECK(psp_state_path_for_slot(path, sizeof(path), "/r/Ruby.st0", 10) == 0 &&
            !strcmp(path, "/r/Ruby.st9"));
      CHECK(psp_state_path_for_slot(path, sizeof(path), "/r/Red.gb.st0", 6) == 0 &&
            !strcmp(path, "/r/Red.gb.st5"));
   }

   /* 3. the window, 10 slots in 5 rows: the walks a player makes */
   {
      int top = 0, cur = 1, k;
      top = window_step(cur = 5, top, 10, 5);  CHECK(top == 0);  /* slot 5: no move */
      top = window_step(cur = 6, top, 10, 5);  CHECK(top == 1);  /* one row */
      top = window_step(cur = 10, top, 10, 5); CHECK(top == 5);  /* the end */
      top = window_step(cur = 1, top, 10, 5);  CHECK(top == 0);  /* DOWN wraps */
      top = window_step(cur = 10, top, 10, 5); CHECK(top == 5);  /* UP wraps */
      top = window_step(cur = 7, top, 10, 5);  CHECK(top == 5);
      top = window_step(cur = 5, top, 10, 5);  CHECK(top == 4);
      /* a window left out of range (a stale top) is pulled back */
      CHECK(psp_state_slot_top(10, 9, 10, 5) == 5);
      CHECK(psp_state_slot_top(3, -4, 10, 5) == 0);
      /* no more slots than rows: never moves (the five-slot page) */
      for (k = 1; k <= 5; k++)
         CHECK(window_step(k, 0, 5, 5) == 0);
      /* a cursor outside 1..n (never passed; a bad caller) is clamped */
      CHECK(psp_state_slot_top(0, 3, 10, 5) == 0);
      CHECK(psp_state_slot_top(14, 0, 10, 5) == 5);
      /* a random walk of UP / DOWN / wraps / jumps, every count and height */
      srand(4242);
      for (k = 0; k < 200000; k++)
      {
         int n = 1 + rand() % 12, rows = 1 + rand() % 7, r = rand() % 4;
         if (k % 50 == 0)
            top = 0, cur = 1;
         if (cur > n)
            cur = n, top = 0;
         if (top > (n > rows ? n - rows : 0))
            top = 0;
         cur = r == 0 ? (cur > 1 ? cur - 1 : n)
             : r == 1 ? (cur < n ? cur + 1 : 1)
             : 1 + rand() % n;
         top = window_step(cur, top, n, rows);
      }
   }

   printf("test_state_slots: OK (%d checks)\n", checks);
   return 0;
}
