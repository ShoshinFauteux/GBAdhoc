/* test_fe_favs.c -- the favourites list: parse, lookup, add, remove,
 * serialise, and the full/overflow edges.  Pure host code. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../frontend-common/fe_favs.c"

static fe_favs F;

static void load(const char *text)
{
   size_t len = strlen(text);
   fe_favs_clear(&F);
   memcpy(F.pool, text, len);
   fe_favs_parse_pool(&F, len);
}

static const char *at(unsigned i) { return F.pool + F.off[i]; }

int main(void)
{
   char out[FE_FAVS_POOL];
   unsigned i;

   /* Parse: CRLF, comments, blanks, whitespace and duplicates. */
   load("# GBAdhoc favourites\r\n"
        "Pokemon - Emerald Version.gba\r\n"
        "\n"
        "  Mother 3.gba  \n"
        "Pokemon - Crystal Version.gbc\n"
        "Pokemon - Emerald Version.gba\n"
        "sub/Shantae.gbc");
   assert(F.n == 4);
   assert(strcmp(at(0), "Pokemon - Emerald Version.gba") == 0);
   assert(strcmp(at(1), "Mother 3.gba") == 0);
   assert(strcmp(at(2), "Pokemon - Crystal Version.gbc") == 0);
   assert(strcmp(at(3), "sub/Shantae.gbc") == 0);
   assert(fe_favs_find(&F, "Mother 3.gba") == 1);
   assert(fe_favs_find(&F, "mother 3.gba") == -1);   /* keyed on the scan's spelling */
   assert(fe_favs_find(&F, "") == -1);
   puts("PASS parse trims, drops comments/blanks/CR and de-duplicates");

   /* Remove from the middle: the pool compacts and every offset survives. */
   assert(fe_favs_remove(&F, "Mother 3.gba") == 0);
   assert(F.n == 3);
   assert(strcmp(at(0), "Pokemon - Emerald Version.gba") == 0);
   assert(strcmp(at(1), "Pokemon - Crystal Version.gbc") == 0);
   assert(strcmp(at(2), "sub/Shantae.gbc") == 0);
   assert(F.used == strlen(at(0)) + strlen(at(1)) + strlen(at(2)) + 3);
   assert(fe_favs_remove(&F, "Mother 3.gba") == -1);
   assert(fe_favs_remove(&F, "sub/Shantae.gbc") == 0);
   assert(fe_favs_remove(&F, "Pokemon - Emerald Version.gba") == 0);
   assert(F.n == 1 && strcmp(at(0), "Pokemon - Crystal Version.gbc") == 0);
   puts("PASS remove compacts the pool from the middle and both ends");

   /* Add, re-add, serialise, and round-trip through the parser. */
   assert(fe_favs_add(&F, "Metroid Fusion.gba") == 0);
   assert(fe_favs_add(&F, "Metroid Fusion.gba") == 0 && F.n == 2);
   assert(fe_favs_add(&F, "") == -1);
   assert(fe_favs_serialise(&F, out, sizeof(out)) ==
          strlen("Pokemon - Crystal Version.gbc\nMetroid Fusion.gba\n"));
   assert(strcmp(out, "Pokemon - Crystal Version.gbc\nMetroid Fusion.gba\n") == 0);
   load(out);
   assert(F.n == 2 && fe_favs_find(&F, "Metroid Fusion.gba") == 1);
   /* A too-small output buffer is truncated, terminated, and reports the
    * full size. */
   assert(fe_favs_serialise(&F, out, 8) ==
          strlen("Pokemon - Crystal Version.gbc\nMetroid Fusion.gba\n"));
   assert(strlen(out) == 7);
   puts("PASS add/serialise round-trips and truncation is reported");

   /* Full list: the 257th entry is refused and nothing else moves. */
   fe_favs_clear(&F);
   for (i = 0; i < FE_FAVS_MAX; i++)
   {
      char name[32];
      snprintf(name, sizeof(name), "g%u.gb", i);
      assert(fe_favs_add(&F, name) == 0);
   }
   assert(F.n == FE_FAVS_MAX);
   assert(fe_favs_add(&F, "one-too-many.gb") == -1);
   assert(F.n == FE_FAVS_MAX && fe_favs_find(&F, "g255.gb") == 255);
   /* Full pool: a name that does not fit is refused, and the list still
    * holds what it had. */
   fe_favs_clear(&F);
   {
      char big[FE_FAVS_POOL];
      memset(big, 'a', sizeof(big) - 1);
      big[sizeof(big) - 1] = '\0';
      assert(fe_favs_add(&F, "keep.gba") == 0);
      assert(fe_favs_add(&F, big) == -1);
      assert(F.n == 1 && strcmp(at(0), "keep.gba") == 0);
   }
   /* Oversized file text: the parser reads at most the pool. */
   {
      char text[FE_FAVS_POOL + 64];
      memset(text, 'b', sizeof(text));
      for (i = 40; i < sizeof(text); i += 41)
         text[i] = '\n';
      fe_favs_clear(&F);
      memcpy(F.pool, text, FE_FAVS_POOL);
      assert(fe_favs_parse_pool(&F, sizeof(text)) == 1);   /* identical lines */
      assert(F.used <= FE_FAVS_POOL);
   }
   puts("PASS list and pool limits refuse without corrupting");
   return 0;
}
