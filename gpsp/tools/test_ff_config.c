/* Uses the real INI reader/writer and configuration load/save paths. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../psp/config_psp.h"
#include "fe_util.h"

void fe_log(const char *fmt, ...) { (void)fmt; }
const char *vid_scale_name(int mode) { (void)mode; return "test"; }
const char *vid_filter_name(int mode) { (void)mode; return "test"; }

static const char *path = "/tmp/ff-config-test.ini";
static void load_legacy(int mult, int smooth)
{
   FILE *f = fopen(path, "w");
   assert(f);
   fprintf(f, "ff_mult_x10 = %d\nff_smooth = %d\n", mult, smooth);
   fclose(f);
   pcfg_load(path);
}

int main(void)
{
   const int speeds[] = {15, 30, 0, 20, -1, 999};
   const char *names[] = {"3x", "Unlimited", "Unlimited Smooth"};
   unsigned i;
   int smooth, mode;
   FILE *f = fopen(path, "w");
   assert(f);
   fclose(f);
   pcfg_load(path);
   assert(pcfg_ff_mode() == PCFG_FF_3X);
   assert(g_pcfg.ff_mult_x10 == 30 && g_pcfg.ff_smooth == 1);

   f = fopen(path, "w");
   assert(f);
   fputs("ff_mult_x10 = 0\n", f); /* older Max config without a style key */
   fclose(f);
   pcfg_load(path);
   assert(pcfg_ff_mode() == PCFG_FF_UNLIMITED);

   for (i = 0; i < sizeof(speeds)/sizeof(speeds[0]); i++)
      for (smooth = 0; smooth < 2; smooth++)
      {
         int expected = speeds[i] ? PCFG_FF_3X :
                        smooth ? PCFG_FF_SMOOTH : PCFG_FF_UNLIMITED;
         load_legacy(speeds[i], smooth);
         assert(pcfg_ff_mode() == expected);
         assert(g_pcfg.ff_mult_x10 == (speeds[i] ? 30 : 0));
         assert(g_pcfg.ff_smooth == (expected != PCFG_FF_UNLIMITED));
      }

   for (mode = 0; mode < PCFG_FF_COUNT; mode++)
   {
      pcfg_ff_set_mode(mode);
      assert(strcmp(pcfg_ff_name(), names[mode]) == 0);
      pcfg_save();
      memset(&g_pcfg, 0, sizeof(g_pcfg));
      pcfg_load(path);
      assert(pcfg_ff_mode() == mode);
      assert(fe_ini_get_int(path, "ff_mult_x10", -1) == g_pcfg.ff_mult_x10);
   }
   pcfg_ff_set_mode(99);
   assert(pcfg_ff_mode() == PCFG_FF_3X);
   remove(path);
   puts("PASS: defaults, six legacy presets, invalid values, all save/load round trips");
   return 0;
}
