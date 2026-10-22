/* test_shot_name.c -- psp/shot_name.h (screenshot names, issue #13). */
#include <stdio.h>
#include <string.h>
#include "../../psp/shot_name.h"

static int fails;

static void title_is(const char *rom, const char *want)
{
   char t[SHOT_TITLE_MAX + 8];
   shot_title_from_rom(rom, t, sizeof(t));
   if (strcmp(t, want) != 0)
   {
      fails++;
      fprintf(stderr, "FAIL title(\"%s\") = \"%s\", want \"%s\"\n", rom, t,
              want);
   }
}

int main(void)
{
   char n[160], small[8];
   const char *longname =
      "ms0:/PSP/GAME/GBADHOC/roms/An Extremely Long Fan Translation Title That "
      "Goes On And On Forever (Rev 3) (Hack).gba";

   title_is("ms0:/PSP/GAME/GBADHOC/roms/Pokemon - Emerald Version (USA).gba",
            "Pokemon - Emerald Version (USA)");
   title_is("ef0:/PSP/GAME/GBADHOC/roms/GB/Tetris (World).gb", "Tetris (World)");
   title_is("Zelda.gbc", "Zelda");
   title_is("roms/Pok\xC3\xA9mon \xC3\x89meraude.gba", "Pokemon Emeraude");
   title_is("roms/What?: A \"Game\" <v1> | x*y.gba", "What_ A _Game_ _v1_ _ x_y");
   title_is("roms/..Dots and spaces.. .gba", "Dots and spaces");
   title_is("roms/\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3.gba", "GBAdhoc");
   title_is("roms/\xE3\x83\x9D Red.gba", "Red");
   title_is("roms/.gba", "gba");             /* a dot file: its name */
   title_is("", "GBAdhoc");
   title_is("roms/tab\there.gba", "tab_here");
   title_is("C:\\roms\\Win.gba", "Win");
   title_is("roms/no_extension", "no_extension");
   {
      char t[SHOT_TITLE_MAX + 8];
      shot_title_from_rom(longname, t, sizeof(t));
      if (strlen(t) > SHOT_TITLE_MAX || strncmp(t, "An Extremely Long", 17))
      {
         fails++;
         fprintf(stderr, "FAIL long title \"%s\"\n", t);
      }
   }
   /* a small buffer is honoured */
   shot_title_from_rom("roms/Pokemon.gba", small, sizeof(small));
   if (strcmp(small, "Pokemon") != 0)
   {
      fails++;
      fprintf(stderr, "FAIL small \"%s\"\n", small);
   }

   if (shot_file_name(n, sizeof(n), "Pokemon Emerald", 2026, 10, 22, 9, 5, 7,
                      0, "png") != 0 ||
       strcmp(n, "Pokemon Emerald_2026-10-22_09-05-07.png") != 0)
   {
      fails++;
      fprintf(stderr, "FAIL name \"%s\"\n", n);
   }
   if (shot_file_name(n, sizeof(n), "Tetris", 2011, 1, 2, 23, 59, 59, 3,
                      "bmp") != 0 ||
       strcmp(n, "Tetris_2011-01-02_23-59-59_3.bmp") != 0)
   {
      fails++;
      fprintf(stderr, "FAIL name \"%s\"\n", n);
   }
   if (shot_file_name(small, sizeof(small), "Tetris", 2011, 1, 2, 23, 59, 59,
                      0, "bmp") != -1)
   {
      fails++;
      fprintf(stderr, "FAIL overflow not reported\n");
   }
   if (fails)
   {
      fprintf(stderr, "shot_name: %d FAILED\n", fails);
      return 1;
   }
   printf("shot_name: all checks passed\n");
   return 0;
}
