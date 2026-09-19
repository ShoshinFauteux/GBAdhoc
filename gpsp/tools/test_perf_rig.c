/* Compile on host against the real header; no PSP clock or stubs required. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "../psp/perf_rig.h"
static unsigned emissions;
void fe_evt(const char *fmt, ...)
{
   va_list args;
   emissions++;
   va_start(args,fmt); vprintf(fmt,args); puts(""); va_end(args);
}
int main(void)
{
   unsigned f;
   rig_config(300,905,180,100);
   assert(!rig_expired(180000099) && rig_expired(180000100));
   for(f=1;f<=300;f++) rig_note(f,900000,900000);
   assert(!rig_stats.n && !emissions);
   for(f=301;f<=599;f++) rig_note(f,16000,12000);
   rig_note(600,20000,18000);
   assert(emissions==1 && rig_stats.total_n==300 && !rig_stats.n);
   for(f=601;f<=905;f++) rig_note(f,33400,25000);
   assert(emissions==3 && rig_stats.total_n==605 && !rig_stats.n);
   rig_note(906,999999,999999);
   assert(rig_stats.total_n==605);
   rig_emit(906);
   assert(emissions==3);
   rig_config(0,10,0,0);
   rig_note(1,16740,16740);
   assert(rig_stats.hist[1]==1 && rig_stats.over==0);
   rig_note(2,100000,16741);
   assert(rig_stats.hist[7]==1 && rig_stats.over==1);
   assert(!rig_expired(UINT64_MAX));
   puts("perf rig assertions passed");
   return 0;
}
