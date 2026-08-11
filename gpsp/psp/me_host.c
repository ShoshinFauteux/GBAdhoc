/* me_host.c — user-side Media Engine driver (see me_host.h; ADR-0080). */
#include <pspkernel.h>
#include <kubridge.h>

#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "me_host.h"
#include "me/me_mbox.h"
#include "fe_evt.h"

/* The mailbox: 64-byte-aligned main RAM, accessed by BOTH cores only
 * through the uncached alias.  Static so its lifetime cannot be a
 * question; the cache line it occupies is never touched cached. */
static me_mbox __attribute__((aligned(64))) g_mbox_storage;
static volatile me_mbox *g_mb;          /* uncached alias, NULL = ME down */
static SceUID g_me_modid = -1;

/* Watchdog state (ADR-0080: the fallback is the feature — the main-CPU
 * path is never removed, the ME merely earns frames while healthy). */
static unsigned g_me_misses, g_me_dead;
static unsigned g_me_last_beat, g_me_beat_stale;

int me_host_up(void)
{
   return g_mb != NULL && !g_me_dead;
}

int me_host_init(const char *base_dir)
{
   char path[192];
   unsigned int arg;
   SceUID mod;
   int st, i;

   memset(&g_mbox_storage, 0, sizeof(g_mbox_storage));
   sceKernelDcacheWritebackInvalidateRange(&g_mbox_storage,
                                           sizeof(g_mbox_storage));

   snprintf(path, sizeof(path), "%s/gbadhoc_me.prx", base_dir);
   mod = kuKernelLoadModule(path, 0, NULL);
   if (mod < 0)
   {
      fe_evt("me_init state=unavailable reason=load rc=0x%08X path=%s",
             (unsigned)mod, path);
      return -1;
   }

   arg = (unsigned int)ME_UNCACHED(&g_mbox_storage);
   st  = sceKernelStartModule(mod, sizeof(arg), &arg, NULL, NULL);
   if (st < 0)
   {
      fe_evt("me_init state=unavailable reason=start rc=0x%08X", (unsigned)st);
      return -1;
   }
   g_me_modid = mod;

   /* Boot handshake: the dispatcher writes magic+version.  250 ms is two
    * orders of magnitude above an honest boot; a miss means the ME never
    * came up (wrong model, CFW refused kernel load, ...). */
   for (i = 0; i < 50; i++)
   {
      volatile me_mbox *mb = (volatile me_mbox *)ME_UNCACHED(&g_mbox_storage);
      if (mb->magic == ME_MBOX_MAGIC && mb->version == ME_MBOX_VERSION)
      {
         g_mb = mb;
         g_me_misses = g_me_dead = 0;
         g_me_last_beat = mb->heartbeat;
         fe_evt("me_init state=up beat=%u", mb->heartbeat);
         return 0;
      }
      sceKernelDelayThread(5000);
   }
   fe_evt("me_init state=unavailable reason=handshake magic=0x%08X",
          (unsigned)((volatile me_mbox *)ME_UNCACHED(&g_mbox_storage))->magic);
   return -1;
}

void me_host_shutdown(void)
{
   if (g_me_modid >= 0)
   {
      int st;
      sceKernelStopModule(g_me_modid, 0, NULL, &st, NULL);
      sceKernelUnloadModule(g_me_modid);
      g_me_modid = -1;
   }
   g_mb = NULL;
}

/* ---- job posting ------------------------------------------------------- */

static void me_post(unsigned cmd, unsigned a0, unsigned a1, unsigned a2,
                    unsigned a3, unsigned a4, unsigned a5)
{
   g_mb->cmd  = cmd;
   g_mb->arg0 = a0; g_mb->arg1 = a1; g_mb->arg2 = a2;
   g_mb->arg3 = a3; g_mb->arg4 = a4; g_mb->arg5 = a5;
   g_mb->cmd_seq = g_mb->cmd_seq + 1;   /* the ME acts on this write */
}

int me_host_idle(void)
{
   return g_mb && g_mb->done_seq == g_mb->cmd_seq;
}

/* v2 (ME_CMD_RENDER): true once the ME has finished copying the live core
 * arrays for the posted render — emulation may then resume in parallel. */
int me_host_input_done(void)
{
   return g_mb && g_mb->input_seq == g_mb->cmd_seq;
}

int me_host_post_pitch_copy(const void *src, void *dst,
                            unsigned rows, unsigned row_bytes,
                            unsigned src_pitch, unsigned dst_pitch)
{
   if (!me_host_up() || !me_host_idle())
      return -1;
   /* The core wrote src through its cache; the ME reads RAM.  One ranged
    * writeback is the entire coherency contract for the job. */
   sceKernelDcacheWritebackRange(src, rows * src_pitch);
   me_post(ME_CMD_COPY_PITCH, (unsigned)src, (unsigned)dst,
           (rows << 16) | row_bytes, 0, src_pitch, dst_pitch);
   return 0;
}

/* Post ME_CMD_RENDER: run the scanline renderer on the ME.  `desc_uncached` is
 * an ME_UNCACHED(&me_render_desc) address; the CALLER must have written back
 * the snapshot buffers the desc points to before calling.  Non-blocking — poll
 * me_host_idle(), then me_host_result() for the frame checksum. */
int me_host_post_render(unsigned int desc_uncached)
{
   if (!me_host_up() || !me_host_idle())
      return -1;
   me_post(ME_CMD_RENDER, desc_uncached, 0, 0, 0, 0, 0);
   return 0;
}

unsigned int me_host_result(void)
{
   return g_mb ? g_mb->result : 0u;
}

/* ---- watchdog ---------------------------------------------------------- */

#define ME_WD_STALE_LIMIT 30    /* frames with a frozen heartbeat = wedged */

/* ADR-0080b: heartbeat-ONLY.  This answers exactly one question — "is the
 * ME still alive?" — via the dispatcher's per-iteration heartbeat.  It used
 * to ALSO count "busy at deadline" as a failure, but the caller checked it
 * in the same frame it posted a job, so the ME was always busy and the
 * offload self-destructed after 8 frames (auto330).  Per-frame completion
 * timing now lives in me_video_frame, where the post/retire ordering is
 * known; a wedged ME (heartbeat frozen) is the only thing left for the
 * host to detect blind. */
int me_host_watchdog_frame(void)
{
   unsigned beat;
   if (!g_mb || g_me_dead)
      return 1;

   beat = g_mb->heartbeat;
   if (beat == g_me_last_beat)
   {
      if (++g_me_beat_stale >= ME_WD_STALE_LIMIT)
      {
         g_me_dead = 1;
         fe_evt("me_watchdog dead=1 reason=heartbeat beat=%u", beat);
         return 1;
      }
   }
   else
   {
      g_me_last_beat  = beat;
      g_me_beat_stale = 0;
   }
   return 0;
}

/* ---- Stage-0 microbenchmark -------------------------------------------- */

static unsigned host_sum32(const unsigned *p, unsigned len)
{
   unsigned sum = 0, n = len >> 2;
   while (n--)
      sum += *p++;
   return sum;
}

int me_host_bench(unsigned len, unsigned iters)
{
   unsigned *src, *dst;
   unsigned expect;
   unsigned mode;
   int rc = 0;

   if (!me_host_up())
      return -1;
   if (!len)
      len = 76800;                     /* one GBA frame */
   len = (len + 31u) & ~31u;
   if (!iters)
      iters = 32;

   src = (unsigned *)memalign(64, len);
   dst = (unsigned *)memalign(64, len);
   if (!src || !dst)
   {
      free(src); free(dst);
      return -1;
   }
   {
      unsigned i;
      for (i = 0; i < len / 4; i++)
         src[i] = i * 2654435761u + 0x9E3779B9u;
   }
   sceKernelDcacheWritebackInvalidateRange(src, len);
   sceKernelDcacheWritebackInvalidateRange(dst, len);
   expect = host_sum32(src, len);

   /* Host-CPU reference: the same copy the blit does today, timed the
    * same way (memcpy from cached src to uncached dst). */
   {
      unsigned t0 = sceKernelGetSystemTimeLow(), t1;
      unsigned it = iters;
      while (it--)
         memcpy(ME_UNCACHED(dst), src, len);
      t1 = sceKernelGetSystemTimeLow();
      fe_evt("me_bench side=cpu mode=memcpy_ucdst len=%u iters=%u us=%u "
             "us_per=%u", len, iters, t1 - t0, (t1 - t0) / iters);
   }

   /* ME modes: 0 = uncached/uncached, 1 = cached src, 2 = cached dst,
    * 3 = cached both.  The checksum makes the work unfakeable. */
   for (mode = 0; mode < 4; mode++)
   {
      unsigned t0, t1;
      if (!me_host_idle())
         break;
      t0 = sceKernelGetSystemTimeLow();
      me_post(ME_CMD_BENCH_COPY, (unsigned)src, (unsigned)dst,
              len, (iters << 16) | mode, 0, 0);
      /* Boot-time only: waiting here is allowed (never in the frame loop).
       * 5 s guard so a wedged ME cannot hang boot. */
      {
         unsigned spins = 0;
         while (!me_host_idle())
         {
            sceKernelDelayThread(200);
            if (++spins > 25000)
            {
               fe_evt("me_bench mode=%u TIMEOUT — ME wedged, killing", mode);
               g_me_dead = 1;
               rc = -1;
               goto out;
            }
         }
      }
      t1 = sceKernelGetSystemTimeLow();
      fe_evt("me_bench side=me mode=%u len=%u iters=%u us=%u us_per=%u "
             "sum=%s", mode, len, iters, t1 - t0, (t1 - t0) / iters,
             g_mb->result == expect ? "ok" : "BAD");
      if (g_mb->result != expect)
         rc = -1;
   }

out:
   free(src);
   free(dst);
   return rc;
}
