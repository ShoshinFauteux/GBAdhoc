/* Per-console display profiles (docs/DISPLAY-FEATURES.md) against the real
 * config_psp.c and INI writer: migration from the pre-profile keys, profile
 * isolation, the live-console guard, clamping, and the I/O budget of the
 * in-game TRIANGLE save.  Linked with -Wl,--wrap=fopen to count rewrites. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../psp/config_psp.h"
#include "../psp/video_psp.h"
#include "fe_util.h"

static unsigned reads, writes;
FILE *__real_fopen(const char *, const char *);
FILE *__wrap_fopen(const char *path, const char *mode)
{
   if (mode[0] == 'r') reads++;
   if (mode[0] == 'w') writes++;
   return __real_fopen(path, mode);
}
void fe_log(const char *fmt, ...) { (void)fmt; }
const char *vid_scale_name(int mode) { (void)mode; return "test"; }
const char *vid_filter_name(int mode) { (void)mode; return "test"; }

static const char *path = "/tmp/display-profiles-test.ini";

static void write_file(const char *text)
{
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(text, f);
   fclose(f);
}

static long key(const char *k)
{
   return fe_ini_get_int(path, k, -99);
}

int main(void)
{
   int c;

   /* 1. MIGRATION: a pre-profile file.  Every console starts from today's
    *    values, so the first boot after the upgrade looks identical. */
   write_file("scale = 2\nfilter = 1\ngb_palette = 3\nconsole = 1\n");
   reads = writes = 0;
   pcfg_load(path);
   assert(writes == 0);                         /* loading never writes */
   for (c = 0; c < FE_CONSOLE_COUNT; c++)
   {
      assert(g_pdisp[c].scale == 2);
      assert(g_pdisp[c].filter == 1);
      assert(g_pdisp[c].ambient == PCFG_AMBIENT_DEF);
      assert(g_pdisp[c].gb_palette == 3);
   }
   assert(pcfg_display_console() == FE_CONSOLE_GB);
   assert(g_pcfg.scale == 2 && g_pcfg.filter == 1 && g_pcfg.gb_palette == 3);
   puts("PASS migration: legacy scale/filter/gb_palette seed all three");

   /* 2. Editing the live (GB) profile, then the TRIANGLE-style save: only
    *    the GB keys are rewritten -- 4 files, not pcfg_save's ~50. */
   g_pcfg.scale = VID_SCALE_INT2;
   g_pcfg.ambient = PCFG_AMB_ART;
   pcfg_display_commit();
   assert(g_pdisp[FE_CONSOLE_GB].scale == VID_SCALE_INT2);
   assert(g_pdisp[FE_CONSOLE_GBA].scale == 2);
   assert(g_pdisp[FE_CONSOLE_GBC].scale == 2);
   reads = writes = 0;
   assert(pcfg_display_save() == 0);
   assert(writes == 4);
   assert(key("scale_gb") == VID_SCALE_INT2 && key("ambient_gb") == 1);
   assert(key("scale_gba") == -99);             /* untouched consoles: absent */
   assert(key("scale") == 2);                   /* legacy key untouched */
   printf("PASS display save: %u rewrites for one console\n", writes);

   /* 3. Reload: GB keeps its edit, the others still migrate from legacy. */
   pcfg_load(path);
   assert(g_pdisp[FE_CONSOLE_GB].scale == VID_SCALE_INT2);
   assert(g_pdisp[FE_CONSOLE_GB].ambient == PCFG_AMB_ART);
   assert(g_pdisp[FE_CONSOLE_GBA].scale == 2);
   assert(g_pdisp[FE_CONSOLE_GBC].scale == 2);
   assert(g_pcfg.scale == VID_SCALE_INT2);      /* console = 1 (GB) is live */

   /* 4. The browser's console switch selects the profile it edits. */
   assert(pcfg_remember_console(FE_CONSOLE_GBA) == 0);
   assert(pcfg_display_console() == FE_CONSOLE_GBA);
   assert(g_pcfg.scale == 2 && g_pcfg.ambient == PCFG_AMBIENT_DEF);
   g_pcfg.filter = 0;
   pcfg_display_commit();

   /* 5. Live-console guard: g_pcfg.console moved WITHOUT a select (as a
    *    harness launch does before main selects) must not pour the GBA
    *    mirror into the GBC profile on save. */
   g_pcfg.console = FE_CONSOLE_GBC;
   pcfg_save();
   pcfg_load(path);
   assert(g_pdisp[FE_CONSOLE_GBA].filter == 0);
   assert(g_pdisp[FE_CONSOLE_GBC].filter == 1);
   assert(g_pdisp[FE_CONSOLE_GBC].scale == 2);
   assert(g_pdisp[FE_CONSOLE_GB].scale == VID_SCALE_INT2);
   /* Legacy keys mirror GBA (scale/filter) and GB (palette), for an older
    * build reading this file. */
   assert(key("scale") == 2 && key("filter") == 0 && key("gb_palette") == 3);
   assert(key("scale_gba") == 2 && key("filter_gba") == 0);
   assert(key("gb_palette_gba") == -99);         /* GBA has no DMG palette */
   assert(key("gb_palette_gbc") == 3);
   puts("PASS isolation: per-console edits, legacy mirror, live-console guard");

   /* 6. Hand-edited and corrupt values: each key migrates independently;
    *    out-of-range values clamp. */
   write_file("scale = 1\nscale_gb = 3\nscale_gbc = 9\nambient_gb = 7\n"
              "ambient_gbc = 2\nfilter_gba = 5\n");
   pcfg_load(path);
   assert(g_pdisp[FE_CONSOLE_GBA].scale == 1);
   assert(g_pdisp[FE_CONSOLE_GB].scale == VID_SCALE_INT2);
   assert(g_pdisp[FE_CONSOLE_GBC].scale == VID_SCALE_1X);
   assert(g_pdisp[FE_CONSOLE_GB].ambient == PCFG_AMBIENT_DEF);
   assert(g_pdisp[FE_CONSOLE_GBC].ambient == PCFG_AMB_ART_GAME);
   assert(g_pdisp[FE_CONSOLE_GBA].filter == 1);
   assert(g_pcfg.console == FE_CONSOLE_GBA && g_pcfg.scale == 1);
   puts("PASS partial files and clamping");

   /* 6b. Sharp bilinear (VID_FILTER_SHARP = 2) is a third filter value: it
    *     loads per console and through the legacy key, any OTHER non-zero
    *     value is still bilinear, and a sharp GBA profile writes 2 to the
    *     legacy key too (an older build reads that as bilinear). */
   write_file("filter = 2\nfilter_gb = 0\nfilter_gbc = -3\n");
   pcfg_load(path);
   assert(g_pdisp[FE_CONSOLE_GBA].filter == VID_FILTER_SHARP);
   assert(g_pdisp[FE_CONSOLE_GB].filter == VID_FILTER_NEAREST);
   assert(g_pdisp[FE_CONSOLE_GBC].filter == VID_FILTER_BILINEAR);
   assert(g_pcfg.filter == VID_FILTER_SHARP);
   pcfg_display_select(FE_CONSOLE_GBA);
   assert(pcfg_display_save() == 0);
   assert(key("filter") == 2 && key("filter_gba") == 2);
   pcfg_display_select(FE_CONSOLE_GB);
   g_pcfg.filter = VID_FILTER_SHARP;
   assert(pcfg_display_save() == 0);
   assert(key("filter_gb") == 2 && key("filter") == 2);
   pcfg_load(path);
   assert(g_pdisp[FE_CONSOLE_GB].filter == VID_FILTER_SHARP);
   puts("PASS sharp bilinear: third filter value, per console, INI-compatible");

   /* 7. No config path: nothing persists, nothing crashes. */
   pcfg_load("");
   assert(pcfg_display_save() == -1);

   remove(path);
   return 0;
}
