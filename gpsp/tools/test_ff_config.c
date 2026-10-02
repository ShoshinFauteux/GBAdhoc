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

static int fps_value(const char *value, int def)
{
   FILE *f = fopen(path, "w");
   int result;
   assert(f);
   fprintf(f, "session_fps = %s\n", value);
   fclose(f);
   result = pcfg_fps_x100(path, "session_fps", def);
   return result;
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
   assert(fps_value("1000", 1234) == PCFG_SESSION_FPS_MAX);
   assert(fps_value("59.5", 1234) == 5950);
   assert(fps_value("2147483648", 1234) == 1234);
   assert(fps_value("999999999999999", 1234) == 1234);

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

   /* 3.0's "A/B buttons" (btn_swap) has no row and no field any more.  A
    * 3.0 card with it set loads cleanly, keeps Cross-is-A as an explicit
    * bind_a/bind_b pair, and after one save the key no longer matters. */
   f = fopen(path, "w");
   assert(f);
   fputs("scale = 1\nprofile = 0\nme_mode = 1\nbtn_swap = 1\nfilter = 0\n"
         "ff_mult_x10 = 30\nff_hold = 1\ngroup = GPSP07\n", f);
   fclose(f);
   memset(&g_pcfg, 0, sizeof(g_pcfg));
   pcfg_load(path);
   assert(g_pcfg.controls.bind[CTL_GAME_A] == CTL_CROSS &&
          g_pcfg.controls.bind[CTL_GAME_B] == CTL_CIRCLE);
   assert(pcfg_ff_mode() == PCFG_FF_3X && g_pcfg.ff_hold == 1);
   pcfg_save();
   assert(fe_ini_get_int(path, "btn_swap", -1) == 1);   /* 3.0's mirror */
   {
      char v[16];
      assert(fe_ini_get(path, "bind_a", v, sizeof(v)) && !strcmp(v, "CROSS"));
      assert(fe_ini_get(path, "bind_b", v, sizeof(v)) && !strcmp(v, "CIRCLE"));
   }
   assert(fe_ini_set_int(path, "btn_swap", 0) == 0);    /* now inert */
   memset(&g_pcfg, 0, sizeof(g_pcfg));
   pcfg_load(path);
   assert(g_pcfg.controls.bind[CTL_GAME_A] == CTL_CROSS);
   /* a fresh card never gains bind_ keys and reads btn_swap = 0 */
   f = fopen(path, "w");
   assert(f);
   fclose(f);
   memset(&g_pcfg, 0, sizeof(g_pcfg));
   pcfg_load(path);
   pcfg_save();
   {
      char v[16];
      assert(!fe_ini_get(path, "bind_a", v, sizeof(v)));
      assert(fe_ini_get_int(path, "btn_swap", -1) == 0);
   }
   remove(path);
   puts("PASS: defaults, six legacy presets, invalid values, all save/load round trips, btn_swap migration");
   return 0;
}
