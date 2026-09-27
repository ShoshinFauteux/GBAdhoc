/* Production config + INI writer, with real filesystem I/O counted at fopen. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../psp/config_psp.h"
#include "rom_paths.h"
#include "state_slots.h"
#include "fe_util.h"

static unsigned reads, writes;
static void make_max_relative_path(char out[PSP_ROM_REL_PATH_CAP])
{
   memset(out, 'd', 250);
   out[250] = '/';
   memset(out + 251, 'e', 30);
   out[281] = '/';
   memset(out + 282, 'f', 37);
   memcpy(out + PSP_ROM_REL_PATH_CAP - 5, ".gba", 5);
}
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

int main(void)
{
   const char *path = "/tmp/rom-selection.ini";
   const char *original = "# Keep my settings\nscale = 2\nstandby = 1\n"
      "custom_future_key = keep me\nmgift_conf = 3\nff_mult_x10 = 0\n"
      "ff_smooth = 1\nconsole = 2\nlast_rom = previous.gba\n";
#ifndef BASELINE
   const char *expected = "# Keep my settings\nscale = 2\nstandby = 1\n"
      "custom_future_key = keep me\nmgift_conf = 3\nff_mult_x10 = 0\n"
      "ff_smooth = 1\nconsole = 2\nlast_rom = Pokemon Heart & Soul.gba\n";
   char buf[4096], long_name[sizeof(g_pcfg.last_rom)+1];
#endif
   char root[144], relative[PSP_ROM_REL_PATH_CAP];
   char full[PSP_FILE_PATH_CAP], too_small[PSP_FILE_PATH_CAP];
   char expected_full[PSP_FILE_PATH_CAP];
   char slot_path[PSP_FILE_PATH_CAP];
   char thumb_path[PSP_STATE_THUMB_PATH_CAP];
   size_t full_len;
   int n;
   FILE *f = fopen(path, "w");
   memset(root, 'r', sizeof(root) - 1);
   root[sizeof(root) - 1] = '\0';
   make_max_relative_path(relative);
   assert(psp_rom_path_join(full, sizeof(full), root, relative) == 0);
   full_len = strlen(root) + 1 + strlen(relative);
   assert(strlen(full) == full_len);
   snprintf(expected_full, sizeof(expected_full), "%s/%s", root, relative);
   assert(strcmp(full, expected_full) == 0);
   memset(too_small, 'x', sizeof(too_small));
   assert(psp_rom_path_join(too_small, full_len + 1, root, relative) == 0);
   assert(psp_rom_path_join(too_small, full_len, root, relative) == -1);
   assert(too_small[0] == '\0');
   assert(psp_rom_path_suffix(full, sizeof(full), full, ".sav") == 0);
   snprintf(expected_full, sizeof(expected_full), "%s/%.*s.sav", root,
            (int)strlen(relative) - 4, relative);
   assert(strcmp(full, expected_full) == 0);
   memset(too_small, 'x', sizeof(too_small));
   n = snprintf(too_small, sizeof(too_small), "%s", expected_full);
   assert(n > 0);
   assert(psp_rom_path_suffix(too_small, (size_t)n, too_small, ".bak") == -1);
   assert(too_small[0] == '\0');
   assert(psp_state_path_for_slot(slot_path, sizeof(slot_path),
                                  "/roms/example.gba.st0", 1) == 0);
   assert(strcmp(slot_path, "/roms/example.gba.st0") == 0);
   assert(psp_state_path_for_slot(slot_path, sizeof(slot_path),
                                  "/roms/example.gba.st0", 5) == 0);
   assert(strcmp(slot_path, "/roms/example.gba.st4") == 0);
   assert(psp_state_path_for_slot(slot_path, sizeof(slot_path),
                                  "/roms/example.gba.st0", 0) == -1);
   assert(psp_state_path_for_slot(slot_path, sizeof(slot_path),
                                  "/roms/example.gba.st0", 6) == -1);
   assert(psp_state_path_for_slot(slot_path, sizeof(slot_path),
                                  "/roms/example.gba.st0", 1) == 0);
   assert(psp_state_thumb_path(thumb_path, sizeof(thumb_path), slot_path) == 0);
   assert(strcmp(thumb_path, "/roms/example.gba.st0.thumb") == 0);
   assert(f); fputs(original, f); fclose(f);
   pcfg_load(path);
   reads = writes = 0;
#ifdef BASELINE
   strcpy(g_pcfg.last_rom, "Pokemon Heart & Soul.gba");
   pcfg_save();
   printf("BASELINE selection: %u reads, %u complete config rewrites\n", reads, writes);
   assert(reads == 36 && writes == 36);
#else
   assert(g_pcfg.console == FE_CONSOLE_GBC);
   assert(pcfg_remember_rom("Pokemon Heart & Soul.gba") == 0);
   assert(reads == 1 && writes == 1);
   puts("PASS selection: one read/write; repeated ROM: zero I/O");
   reads = writes = 0;
   assert(pcfg_remember_rom("Pokemon Heart & Soul.gba") == 0);
   assert(reads == 0 && writes == 0);
   f = fopen(path, "r"); assert(f);
   buf[fread(buf, 1, sizeof(buf)-1, f)] = 0; fclose(f);
   assert(strcmp(buf, expected) == 0);
   pcfg_load(path);
   assert(g_pcfg.scale == 2 && g_pcfg.standby == 1);
   assert(g_pcfg.console == FE_CONSOLE_GBC);
   assert(pcfg_ff_mode() == PCFG_FF_SMOOTH);
   assert(strcmp(g_pcfg.last_rom, "Pokemon Heart & Soul.gba") == 0);
   {
      char max_relative[PSP_ROM_REL_PATH_CAP];
      make_max_relative_path(max_relative);
      assert(pcfg_remember_rom(max_relative) == 0);
      pcfg_load(path);
      assert(strcmp(g_pcfg.last_rom, max_relative) == 0);
   }
   memset(long_name, 'x', sizeof(long_name)); long_name[sizeof(long_name)-1] = 0;
   reads = writes = 0;
   assert(pcfg_remember_rom(NULL) == -1);
   assert(pcfg_remember_rom("") == -1);
   assert(pcfg_remember_rom(long_name) == -1);
   assert(reads == 0 && writes == 0);
   g_pcfg.console = FE_CONSOLE_GB;
   pcfg_save();
   pcfg_load(path);
   assert(g_pcfg.console == FE_CONSOLE_GB);
   f = fopen(path, "w"); assert(f); fputs("console = 99\n", f); fclose(f);
   pcfg_load(path);
   assert(g_pcfg.console == FE_CONSOLE_GBA);
   f = fopen(path, "w"); assert(f); fputs("# default console\n", f); fclose(f);
   pcfg_load(path);
   assert(g_pcfg.console == FE_CONSOLE_GBA);
   pcfg_load("/nonexistent-parent/CONFIG.INI");
   snprintf(buf, sizeof(buf), "%s", g_pcfg.last_rom);
   assert(pcfg_remember_rom("missing.gba") == -1);
   assert(strcmp(g_pcfg.last_rom, buf) == 0); /* failure remains retryable */
   pcfg_load("");
   assert(pcfg_remember_rom("missing.gba") == -1);
   puts("PASS preservation, reload, invalid input, failed write, no config path");
#endif
   remove(path);
   return 0;
}
