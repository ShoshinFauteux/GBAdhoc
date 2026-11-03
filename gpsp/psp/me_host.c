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
/* A timeout does not cancel an ME command. Keep benchmark buffers until a
 * successful module_stop has physically reset the second core. */
static void *g_me_bench_retained[2];

/* Watchdog state (ADR-0080: the fallback is the feature — the main-CPU
 * path is never removed, the ME merely earns frames while healthy). */
static unsigned g_me_misses, g_me_dead;
static unsigned g_me_last_beat, g_me_beat_stale;
/* See me_host.h.  g_me_retained: the module is loaded, its dispatcher idle and
 * its D-cache empty, across an in-process return.  g_me_handed_back: a live
 * engine was stopped and this process went on. */
static int g_me_retained, g_me_handed_back;

#ifdef GPSP_PERF_RIG
/* THE MODULE'S STAND-IN (harness `me_fake`).  PPSSPP has no Media Engine: the
 * real handshake fails, the module is gone before any game runs, and nothing
 * below this line that matters here is ever reached.  So a user thread plays
 * the module's part of the protocol: it answers the mailbox, beats the
 * heartbeat, refuses renders (the host falls back to the main CPU, as it does
 * for a real refusal), and, being a thread, takes kernel memory as the module
 * does, which is what the return's census has to cope with.
 *
 * mode 1 is the healthy engine.  mode 2 is a wedged one: the first job sent to
 * it never completes, for the path where retaining fails. */
static int g_me_fake;
static SceUID g_me_fake_th = -1;
static volatile int g_me_fake_stop;

static int me_fake_thread(SceSize args, void *argp)
{
   volatile me_mbox *mb = (volatile me_mbox *)ME_UNCACHED(&g_mbox_storage);
   unsigned seen;
   (void)args; (void)argp;
   mb->version = ME_MBOX_VERSION;
   mb->magic   = ME_MBOX_MAGIC;
   seen = mb->cmd_seq;
   mb->done_seq = seen;
   while (!g_me_fake_stop)
   {
      unsigned seq = mb->cmd_seq;
      if (g_me_fake == 2 && seq != seen)
      {
         sceKernelDelayThread(10000);
         continue;
      }
      mb->heartbeat = mb->heartbeat + 1;
      if (seq != seen)
      {
         seen = seq;
         if (mb->cmd == ME_CMD_RENDER)
         {
            mb->refused_seq = seq;
            mb->input_seq   = seq;
         }
         mb->result   = 0;
         mb->done_seq = seq;
      }
      sceKernelDelayThread(100);
   }
   return 0;
}

static int me_fake_start(void)
{
   g_me_fake_stop = 0;
   /* Above the power callback (0x11): me_host_sync spins, and the real ME
    * does not need this CPU to answer. */
   g_me_fake_th = sceKernelCreateThread("me_fake", me_fake_thread, 0x10, 0x1000,
                                        THREAD_ATTR_USER, NULL);
   if (g_me_fake_th < 0)
      return -1;
   sceKernelStartThread(g_me_fake_th, 0, NULL);
   return g_me_fake_th;
}

static void me_fake_stop(void)
{
   g_me_fake_stop = 1;
   sceKernelWaitThreadEnd(g_me_fake_th, NULL);
   sceKernelDeleteThread(g_me_fake_th);
   g_me_fake_th = -1;
}

void me_host_fake(int mode)
{
   g_me_fake = mode;
}
#endif

int me_host_up(void)
{
   return g_mb != NULL && !g_me_dead;
}

int me_host_loaded(void)
{
   return g_me_modid >= 0;
}

/* 1 when the watchdog gave up on the engine this session: whatever state the
 * second core was left in, the in-process return does not bet on a clean
 * restart and relaunches instead. */
int me_host_was_dead(void)
{
   return g_me_dead != 0;
}

static int g_me_need_seh;

void me_host_require_sysevent(int on)
{
   g_me_need_seh = on ? 1 : 0;
}

int me_host_retained(void)
{
   return g_me_retained;
}

int me_host_handed_back(void)
{
   return g_me_handed_back;
}

int me_host_retain(unsigned timeout_us)
{
   unsigned t0;
   if (!me_host_up())
      return 0;
   t0 = (unsigned)sceKernelGetSystemTimeLow();
   while (!me_host_idle())
   {
      if ((unsigned)sceKernelGetSystemTimeLow() - t0 >= timeout_us)
      {
         fe_evt("me_retain failed reason=busy");
         return 0;
      }
      sceKernelDelayThread(1000);
   }
   /* The next game may be on the heap the last one's buffers came from, and
    * this cache writes back when it pleases. */
   if (me_host_sync(timeout_us) != 0)
   {
      fe_evt("me_retain failed reason=sync");
      return 0;
   }
   g_me_retained = 1;
   fe_evt("me_retain beat=%u", g_mb->heartbeat);
   return 1;
}

/* The next game takes up the engine the last one left: the module is resident,
 * nothing to load, nothing to clear (sequence numbers just carry on, and the
 * render desc's new work_gen is what tells the ME to start from nothing).  A
 * sleep may have reset the ME since, so believe only a heartbeat that moves. */
static int me_host_take_up(void)
{
   unsigned beat = g_mb->heartbeat;
   int i;
   g_me_retained = 0;
   for (i = 0; i < 50; i++)
   {
      sceKernelDelayThread(1000);
      if (g_mb->heartbeat != beat)
      {
         g_me_misses = 0;
         g_me_last_beat = g_mb->heartbeat;
         g_me_beat_stale = 0;
         fe_evt("me_init state=up retained=1 beat=%u", g_me_last_beat);
         return 0;
      }
   }
   g_me_dead = 1;
   fe_evt("me_init state=unavailable reason=retained_silent beat=%u", beat);
   return -1;
}

int me_host_init(const char *base_dir)
{
   char path[192];
   unsigned int arg[2];
   SceUID mod;
   int st, i;

   if (g_me_retained)
      return me_host_take_up();
   /* Never clear a mailbox that a prior, unhealthy module may still use. */
   if (me_host_loaded())
      return -1;

   memset(&g_mbox_storage, 0, sizeof(g_mbox_storage));
   sceKernelDcacheWritebackInvalidateRange(&g_mbox_storage,
                                           sizeof(g_mbox_storage));

#ifdef GPSP_PERF_RIG
   if (g_me_fake)
   {
      mod = me_fake_start();
      if (mod < 0)
         return -1;
      g_me_modid = mod;
      goto handshake;
   }
#endif
   snprintf(path, sizeof(path), "%s/gbadhoc_me.prx", base_dir);
   mod = kuKernelLoadModule(path, 0, NULL);
   if (mod < 0)
   {
      fe_evt("me_init state=unavailable reason=load rc=0x%08X path=%s",
             (unsigned)mod, path);
      return -1;
   }

   arg[0] = (unsigned int)ME_UNCACHED(&g_mbox_storage);
   arg[1] = (unsigned int)g_me_need_seh;
   st  = sceKernelStartModule(mod, sizeof(arg), arg, NULL, NULL);
   if (st < 0)
   {
      /* With arg[1] set, this is also how "we could not take the SceMeRpc
       * sysevent slot" arrives: the module refuses rather than coming up in a
       * configuration where sleeping would cold-boot the console. */
      /* 0x5Exxxxxx is our own encoding: the sysevent slot could not be
       * taken, and the low bits are how many handlers were walked looking
       * for it.  walked=0 means the list was empty; a large count means the
       * list is there and nothing in it is named like SceMeRpc. */
      if (((unsigned)st >> 24) == 0xDEu)
         fe_evt("me_init state=unavailable reason=no_sysevent walked=%u",
                (unsigned)st & 0xFFFFFFu);
      else
         fe_evt("me_init state=unavailable reason=start rc=0x%08X need_seh=%d",
                (unsigned)st, g_me_need_seh);
      sceKernelUnloadModule(mod);
      return -1;
   }
   g_me_modid = mod;
#ifdef GPSP_PERF_RIG
handshake:
#endif

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
         g_me_beat_stale = 0;
         fe_evt("me_init state=up beat=%u", mb->heartbeat);
         return 0;
      }
      sceKernelDelayThread(5000);
   }
   /* `version` beside the magic: a module from another build answers with
    * the right magic and the wrong version (me_mbox.h, v3), and that must not
    * read as "the second core never came up". */
   fe_evt("me_init state=unavailable reason=handshake magic=0x%08X "
          "version=%u want=%u",
          (unsigned)((volatile me_mbox *)ME_UNCACHED(&g_mbox_storage))->magic,
          (unsigned)((volatile me_mbox *)ME_UNCACHED(&g_mbox_storage))->version,
          (unsigned)ME_MBOX_VERSION);
   /* module_start succeeded, so module_start() has already patched the ME
    * reset vector and enabled its bus clock. A failed handshake must unwind
    * that platform state before we fall back to CPU rendering. */
   me_host_shutdown();
   return -1;
}

void me_host_shutdown(void)
{
#ifdef GPSP_PERF_RIG
   if (g_me_fake && g_me_modid >= 0)
   {
      me_fake_stop();
      g_me_handed_back |= g_mb != NULL;
      g_me_retained = 0;
      g_me_modid = -1;
      g_mb = NULL;
      return;
   }
#endif
   if (g_me_modid >= 0)
   {
      int st = -1;
      int rc = sceKernelStopModule(g_me_modid, 0, NULL, &st, NULL);
      if (rc < 0 || st < 0)
      {
         g_me_dead = 1;
         fe_evt("me_stop failed rc=0x%08X status=0x%08X buffers_retained=1",
                (unsigned)rc, (unsigned)st);
         return;
      }
      /* module_stop holds the ME in reset: even a delayed command or dirty
       * cache line can no longer touch these quarantined allocations. */
      free(g_me_bench_retained[0]); g_me_bench_retained[0] = NULL;
      free(g_me_bench_retained[1]); g_me_bench_retained[1] = NULL;
      /* module_stop put the kernel's SceMeRpc handler back in its slot, and
       * that handler now waits for an ME firmware that is not running. */
      g_me_handed_back |= g_mb != NULL;
      g_me_retained = 0;
      g_mb = NULL;
      rc = sceKernelUnloadModule(g_me_modid);
      if (rc < 0)
      {
         g_me_dead = 1;
         fe_evt("me_unload failed rc=0x%08X", (unsigned)rc);
         return;
      }
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

int me_host_render_refused(void)
{
   return g_mb && g_mb->refused_seq == g_mb->cmd_seq;
}

int me_host_sync(unsigned timeout_us)
{
   unsigned t0;
   if (!me_host_up() || !me_host_idle())
      return -1;
   me_post(ME_CMD_SYNC, 0, 0, 0, 0, 0, 0);
   t0 = (unsigned)sceKernelGetSystemTimeLow();
   while (!me_host_idle())
      if ((unsigned)sceKernelGetSystemTimeLow() - t0 >= timeout_us)
         return -1;
   return 0;
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

void me_host_diag(unsigned *cmd_seq, unsigned *done_seq, unsigned *input_seq,
                  unsigned *heartbeat)
{
   volatile me_mbox *mb = g_mb;
   if (cmd_seq)   *cmd_seq   = mb ? mb->cmd_seq   : 0u;
   if (done_seq)  *done_seq  = mb ? mb->done_seq  : 0u;
   if (input_seq) *input_seq = mb ? mb->input_seq : 0u;
   if (heartbeat) *heartbeat = mb ? mb->heartbeat : 0u;
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
      unsigned t0 = sceKernelGetSystemTimeLow(), t1 = 0;
      unsigned it = iters;

      FE_EVT_ONLY(t0); FE_EVT_ONLY(t1);
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
      unsigned t0 = 0, t1 = 0;

      FE_EVT_ONLY(t0); FE_EVT_ONLY(t1);
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
               fe_evt("me_bench mode=%u TIMEOUT buffers_retained=1", mode);
               g_me_dead = 1;
               g_me_bench_retained[0] = src;
               g_me_bench_retained[1] = dst;
               src = dst = NULL;
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
