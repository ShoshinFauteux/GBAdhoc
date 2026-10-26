/* test_text_fold.c -- psp/text_fold.h (GitHub issue #19): UTF-8 file names
 * folded to the ASCII UI font for display, and the FAT long/short name
 * choice the ROM browser opens a game by. */
#include <stdio.h>
#include <string.h>
#include "../../psp/text_fold.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void fold_is(const char *in, const char *want)
{
   char out[128];
   utf8_fold_copy(out, sizeof(out), in);
   if (strcmp(out, want) != 0)
   {
      fails++;
      fprintf(stderr, "FAIL fold: got \"%s\" want \"%s\"\n", out, want);
   }
}

static int ends_gba(const char *n, void *ctx)
{
   size_t l = strlen(n);
   (void)ctx;
   return l >= 4 && (strcmp(n + l - 4, ".gba") == 0 ||
                     strcmp(n + l - 4, ".GBA") == 0);
}

static const char *g_openable;
static int opens_if(const char *n, void *ctx)
{
   (void)ctx;
   return g_openable && strcmp(n, g_openable) == 0;
}

int main(void)
{
   const char *o, *s;
   char t[64];

   CHECK(sizeof(TEXT_FOLD_LATIN1) == 65);
   CHECK(sizeof(TEXT_FOLD_EXTA) == 129);

   fold_is("Pokemon Emerald.gba", "Pokemon Emerald.gba");
   fold_is("Pok\xC3\xA9mon \xC3\x89meraude.gba", "Pokemon Emeraude.gba");
   fold_is("M\xC3\xBCnchen \xC3\xB1 Espa\xC3\xB1" "a", "Munchen n Espana");
   fold_is("Stra\xC3\x9F" "e", "Strase");
   fold_is("\xC5\x81\xC3\xB3" "d\xC5\xBA", "Lodz");            /* Łódź */
   fold_is("\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3", "????"); /* ポケモン */
   fold_is("\xF0\x9F\x98\x80!", "?!");                         /* emoji, 4 bytes */
   fold_is("A\xE2\x80\x94" "B", "A-B");                        /* em dash */
   fold_is("\xEF\xBC\xA1\xEF\xBC\xA2", "AB");                  /* full-width */
   /* the button glyph bytes, alone, are NOT sequences: unchanged */
   fold_is("\x7F\x80\x81\x82\x83\x84", "\x7F\x80\x81\x82\x83\x84");
   /* broken sequences pass byte by byte (and draw as '?' in the font) */
   fold_is("a\xC3", "a\xC3");
   fold_is("\xC0\x80", "\xC0\x80");                            /* overlong */
   fold_is("\xED\xA0\x80", "\xED\xA0\x80");                    /* surrogate */
   fold_is("\xC3\xA9", "e");

   /* a clipped title never ends inside a letter */
   strcpy(t, "Pok\xC3");          utf8_trim_partial(t); CHECK(!strcmp(t, "Pok"));
   strcpy(t, "Pok\xC3\xA9");      utf8_trim_partial(t); CHECK(!strcmp(t, "Pok\xC3\xA9"));
   strcpy(t, "x\xE3\x83");        utf8_trim_partial(t); CHECK(!strcmp(t, "x"));
   strcpy(t, "x\xE3\x83\x9D");    utf8_trim_partial(t); CHECK(!strcmp(t, "x\xE3\x83\x9D"));
   strcpy(t, "\xF0\x9F\x98");     utf8_trim_partial(t); CHECK(!strcmp(t, ""));
   strcpy(t, "abc");              utf8_trim_partial(t); CHECK(!strcmp(t, "abc"));
   strcpy(t, "\x84");             utf8_trim_partial(t); CHECK(!strcmp(t, "\x84"));

   /* ASCII long name: opened and shown by it, no stat asked */
   g_openable = NULL;
   CHECK(fat_rom_pick("Emerald.gba", "Emerald.gba", "EMERALD.GBA", ends_gba,
                      opens_if, NULL, &o, &s) == 1);
   CHECK(!strcmp(o, "Emerald.gba") && !strcmp(s, "Emerald.gba"));
   /* not a ROM */
   CHECK(fat_rom_pick("notes.txt", "", "NOTES.TXT", ends_gba, opens_if, NULL,
                      &o, &s) == 0);
   /* UTF-8 long name that opens (PPSSPP; a firmware that converts) */
   g_openable = "Pok\xC3\xA9mon.gba";
   CHECK(fat_rom_pick("Pok\xC3\xA9mon.gba", "", "", ends_gba, opens_if, NULL,
                      &o, &s) == 1);
   CHECK(o == s && !strcmp(o, "Pok\xC3\xA9mon.gba"));
   /* d_name cut short at the unconvertible letter: the long name from
    * d_private is shown, the 8.3 name is what opens */
   g_openable = NULL;
   CHECK(fat_rom_pick("Pok", "Pok\xC3\xA9mon.gba", "POKMON~1.GBA", ends_gba,
                      opens_if, NULL, &o, &s) == 1);
   CHECK(!strcmp(o, "POKMON~1.GBA") && !strcmp(s, "Pok\xC3\xA9mon.gba"));
   /* nothing usable but the short name */
   CHECK(fat_rom_pick("Pok", "", "POKMON~1.GBA", ends_gba, opens_if, NULL,
                      &o, &s) == 1);
   CHECK(!strcmp(o, "POKMON~1.GBA") && !strcmp(s, "POKMON~1.GBA"));
   /* non-ASCII, does not stat, no short name: still listed */
   CHECK(fat_rom_pick("Pok\xC3\xA9mon.gba", "", "", ends_gba, opens_if, NULL,
                      &o, &s) == 1);
   CHECK(!strcmp(o, "Pok\xC3\xA9mon.gba"));
   /* d_name opens while the d_private long name does not */
   g_openable = "Pok?mon.gba";
   CHECK(fat_rom_pick("Pok?mon.gba", "Pok\xC3\xA9mon.gba", "", ends_gba,
                      opens_if, NULL, &o, &s) == 1);
   CHECK(!strcmp(o, "Pok?mon.gba"));

   if (fails)
   {
      fprintf(stderr, "text_fold: %d FAILED\n", fails);
      return 1;
   }
   printf("text_fold: all checks passed\n");
   return 0;
}
