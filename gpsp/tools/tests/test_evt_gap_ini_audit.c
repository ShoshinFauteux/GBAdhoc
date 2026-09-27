/* Two rig invariants that must not be able to lie (RIG-DOUBLE-BATTLE §2.4/2.8):
 *
 *  1. A STARVED LOG WRITER IS VISIBLE.  The async fe_evt ring drops lines when
 *     its writer thread cannot run (the io thread sits below the emulation
 *     thread).  Every gap must be announced IN-BAND, at the place it happened,
 *     by `EVT evt_gap dropped=N total=T`, and the announced drops must add up
 *     to fe_evt_drops().  Starvation is injected here by never servicing the
 *     ring while writing far more than it holds.
 *
 *  2. A MISSPELLED INI KEY IS VISIBLE.  fe_ini_audit_report() must list every
 *     key in the file with whether anything looked it up.
 *
 * Production fe_evt.c and fe_util.c are compiled in.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../frontend-common/fe_evt.c"
#include "../../frontend-common/fe_util.c"

static void no_wake(void) { }

static int g_unknown, g_known, g_badint, g_dup;
static void audit_emit(void *user, const char *key, const char *raw, int asked,
                       long ival, int is_int)
{
   (void)user; (void)ival; (void)raw;
   if (asked == 2) { g_dup++; assert(!strcmp(key, "rfu_shed_keep") && !strcmp(raw, "0")); return; }
   if (!asked) { g_unknown++; assert(!strcmp(key, "rfu_shed_kep")); }
   else g_known++;
   if (!strcmp(key, "net_session_fps")) { assert(!is_int); g_badint++; }
   if (!strcmp(key, "rfu_shed_keep")) assert(is_int && ival == 2);
}

int main(void)
{
   const char *logp = "/tmp/t_evt_gap.log";
   const char *inip = "/tmp/t_ini_audit.ini";
   char line[600];
   unsigned i, announced = 0, lines_seen = 0, gaps = 0;
   FILE *f;

   /* ---- 1. starvation --------------------------------------------------- */
   assert(fe_evt_init(logp, 0) == 0);
   fe_evt_set_async(no_wake);                 /* a writer that never runs */
   for (i = 0; i < 1000; i++)                 /* ~100 KB into a 16 KB ring */
      fe_evt("filler n=%u %s", i,
             "................................................................");
   assert(fe_evt_drops() > 0);
   fe_evt_service();                          /* the writer finally runs... */
   fe_evt("after_starvation");                /* ...and the next line carries the gap */
   fe_evt_set_async(NULL);
   fe_evt("final evt_drop total=%u", fe_evt_drops());
   fe_evt_close();

   f = fopen(logp, "r");
   assert(f);
   while (fgets(line, sizeof(line), f))
   {
      unsigned d, t;
      if (sscanf(line, "EVT evt_gap dropped=%u total=%u", &d, &t) == 2)
      {
         announced += d;
         gaps++;
         assert(t == announced);
      }
      else if (!strncmp(line, "EVT filler", 10))
         lines_seen++;
   }
   fclose(f);
   printf("  evt starvation: %u lines kept, %u dropped, %u gap marker(s) "
          "announcing %u\n", lines_seen, fe_evt_drops(), gaps, announced);
   assert(gaps >= 1);
   assert(announced == fe_evt_drops());
   assert(lines_seen + fe_evt_drops() == 1000);

   /* ---- 2. ini audit ---------------------------------------------------- */
   f = fopen(inip, "w");
   assert(f);
   fputs("# rig ini\nrfu_shed_keep = 2\nrfu_shed_kep = 0\n"
         "net_session_fps = 57.00\nscript = x.inputs\nrfu_shed_keep = 0\n", f);
   fclose(f);
   fe_ini_audit(inip);
   assert(fe_ini_get_int(inip, "rfu_shed_keep", 0) == 2);
   assert(fe_ini_get_int(inip, "net_session_fps", 7) == 57);  /* strtol */
   {
      char buf[32];
      assert(fe_ini_get(inip, "script", buf, sizeof(buf)) == 1);
   }
   (void)fe_ini_get_int(inip, "not_in_file", 1);
   assert(fe_ini_audit_report(inip, audit_emit, NULL) == 5);
   assert(g_unknown == 1 && g_known == 3 && g_badint == 1 && g_dup == 1);
   assert(fe_ini_get_int(inip, "rfu_shed_keep", 9) == 2);   /* first wins */
   printf("  ini audit: misspelled key reported, non-integer value flagged\n");
   printf("evt gap + ini audit: all tests passed\n");
   return 0;
}
