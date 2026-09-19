/* Solo benchmark windows: one aggregate line per 300 measured frames. */
#ifndef PSP_PERF_RIG_H
#define PSP_PERF_RIG_H
#if defined(GPSP_PERF_RIG) && !defined(GPSP_KEEP_TELEMETRY)
#error GPSP_PERF_RIG requires GPSP_KEEP_TELEMETRY
#endif
#include <stdint.h>
#include <string.h>
#include "fe_evt.h"
static int g_perf_rig;
static unsigned rig_from, rig_to, rig_timeout;
static uint64_t rig_started;
static struct {
   unsigned n, total_n, over, wall_max, work_max;
   uint64_t wall, work;
   unsigned hist[8];
} rig_stats;
static void rig_config(unsigned from, unsigned to, unsigned timeout_s, uint64_t now)
{
   rig_from=from; rig_to=to; rig_timeout=timeout_s; rig_started=now;
   memset(&rig_stats,0,sizeof(rig_stats));
}
static int rig_expired(uint64_t now)
{
   return rig_timeout && now-rig_started >= (uint64_t)rig_timeout*1000000;
}
static void rig_emit(unsigned frame)
{
   FE_EVT_ONLY(frame);
   if (!rig_stats.n) return;
   fe_evt("perf_window f=%u n=%u wall_us=%llu work_us=%llu "
          "wall_max=%u work_max=%u over=%u hist=%u,%u,%u,%u,%u,%u,%u,%u",
          frame,rig_stats.n,(unsigned long long)rig_stats.wall,
          (unsigned long long)rig_stats.work,rig_stats.wall_max,
          rig_stats.work_max,rig_stats.over,rig_stats.hist[0],rig_stats.hist[1],
          rig_stats.hist[2],rig_stats.hist[3],rig_stats.hist[4],rig_stats.hist[5],
          rig_stats.hist[6],rig_stats.hist[7]);
   unsigned total=rig_stats.total_n;
   memset(&rig_stats,0,sizeof(rig_stats));
   rig_stats.total_n=total;
}
static void rig_note(unsigned frame, unsigned wall, unsigned work)
{
   static const unsigned edge[7]={16740,20000,25000,33400,50000,75000,100000};
   unsigned i;
   if (frame<=rig_from || frame>rig_to) return;
   rig_stats.n++; rig_stats.total_n++;
   rig_stats.wall+=wall; rig_stats.work+=work;
   if(wall>rig_stats.wall_max) rig_stats.wall_max=wall;
   if(work>rig_stats.work_max) rig_stats.work_max=work;
   if(work>16740) rig_stats.over++;
   for(i=0;i<7 && wall>=edge[i];i++) {}
   rig_stats.hist[i]++;
   if(rig_stats.n==300 || frame==rig_to) rig_emit(frame);
}
#endif
