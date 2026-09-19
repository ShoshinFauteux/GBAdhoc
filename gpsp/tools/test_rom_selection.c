/* Production config + INI writer, with real filesystem I/O counted at fopen. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../psp/config_psp.h"
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

int main(void)
{
   const char *path = "/tmp/rom-selection.ini";
   const char *original = "# Keep my settings\nscale = 2\nstandby = 1\n"
      "custom_future_key = keep me\nmgift_conf = 3\nff_mult_x10 = 0\n"
      "ff_smooth = 1\nlast_rom = previous.gba\n";
#ifndef BASELINE
   const char *expected = "# Keep my settings\nscale = 2\nstandby = 1\n"
      "custom_future_key = keep me\nmgift_conf = 3\nff_mult_x10 = 0\n"
      "ff_smooth = 1\nlast_rom = Pokemon Heart & Soul.gba\n";
   char buf[4096], long_name[sizeof(g_pcfg.last_rom)+1];
#endif
   FILE *f = fopen(path, "w");
   assert(f); fputs(original, f); fclose(f);
   pcfg_load(path);
   reads = writes = 0;
#ifdef BASELINE
   strcpy(g_pcfg.last_rom, "Pokemon Heart & Soul.gba");
   pcfg_save();
   printf("BASELINE selection: %u reads, %u complete config rewrites\n", reads, writes);
   assert(reads == 36 && writes == 36);
#else
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
   assert(pcfg_ff_mode() == PCFG_FF_SMOOTH);
   assert(strcmp(g_pcfg.last_rom, "Pokemon Heart & Soul.gba") == 0);
   memset(long_name, 'x', sizeof(long_name)); long_name[sizeof(long_name)-1] = 0;
   reads = writes = 0;
   assert(pcfg_remember_rom(NULL) == -1);
   assert(pcfg_remember_rom("") == -1);
   assert(pcfg_remember_rom(long_name) == -1);
   assert(reads == 0 && writes == 0);
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
