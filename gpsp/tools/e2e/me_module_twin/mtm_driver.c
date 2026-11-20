/* mtm_driver.c -- the Media Engine MODULE's own code, run on the desktop.
 *
 * ME_TIMING_SIM (video_me_timing_sim.h) models the engine with the twin's own
 * build of video.cc and its own arrays.  It never runs psp/me/: not
 * me_render_glue.cc, not video.cc as the module compiles it (ME_PRX_BUILD),
 * not the work area the host hands over (psp/me/me_mbox.h).  This does.
 *
 * tools/e2e/me_module_twin/build.sh compiles the module's four renderer
 * sources for the desktop, exactly as psp/me/Makefile lists them, and renames
 * every symbol they define to mep_*, so a second, independent renderer sits
 * beside the twin's.  This file plays the host: for every frame the model
 * replays at the vcount-160 post it builds a real me_render_desc, posts
 * ME_CMD_RENDER through me_render_run(), and returns the hash of the stage
 * the module wrote.  video_me_timing_sim.h prints it as `mod=` on the MTS
 * line, and it must equal `vis=` on every frame.
 *
 * WHAT IS REAL.  The module's C: descriptor handling, the dirty-page patch of
 * its persistent VRAM mirror, the work-area pointers, the fresh-area rule,
 * the refusal, the log apply, the pitched write-out -- and the PSP's address
 * conventions, because the module computes with them: every buffer it is
 * given lives in one arena mapped twice, at 0x10000000 and again at
 * 0x50000000, so `addr | 0x40000000` (the uncached alias) and
 * `addr & ~0x40000000` (the cached one) name the same bytes, as on hardware.
 *
 * WHAT IS NOT.  It is x86 code from a desktop compiler, there are no caches
 * (the cache ops compile to nothing), and there is one CPU.  Whether the MIPS
 * module is correct, whether two cores with separate caches agree, and what
 * any of it costs are questions only a console answers.
 *
 * LIFECYCLE EVENTS (MTM_EVERY=N, default 0 = none): every N frames one of
 *   rebuild   the host rebuilt the renderer: new work_gen, area scrambled
 *   sleep     the area was lost and the ME reset, the host did NOT rebuild
 *             (power_cb teardown_skipped): same gen, me_render_boot()
 *   move      same gen, the area is somewhere else
 *   refuse    a desc with no work area, then the real one
 * `refuse` must leave the picture untouched.  The other three restart the
 * module from an empty area, which is what a freshly loaded module is: the
 * OBJ lists are empty until the game next writes OAM, so a few frames after
 * such an event may differ from the model -- the comparison script counts
 * them separately and they must stop.
 *
 * MTM_FAULT=stale is the oracle's own test: the dirty-page map is handed over
 * with every page marked clean, so the module's mirror goes stale.  A run
 * whose VRAM changes must then report mismatches and MTM_MIRROR lines; if it
 * does not, `mod_bad 0` means nothing. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "psp/me/me_mbox.h"

extern unsigned int mep_me_render_run(volatile me_mbox *mb, unsigned int seq);
extern void mep_me_render_boot(void);

#define MTM_LO     0x10000000u            /* "cached" view of the arena      */
#define MTM_HI     (MTM_LO | 0x40000000u) /* the same bytes, "uncached"      */
#define MTM_BYTES  0x200000u

#define OFF_DESC   0x000000u
#define OFF_CAP    0x001000u              /* me_capture_frame, <= 64 KiB     */
#define OFF_LOG    0x020000u              /* log entries, <= 192 KiB         */
#define OFF_VRAM   0x050000u
#define OFF_OAM    0x068000u
#define OFF_PAL    0x068400u
#define OFF_CLEAN  0x068800u
#define OFF_OUT    0x070000u              /* 256-pixel pitch stage           */
#define OFF_WORK_A 0x090000u
#define OFF_WORK_B 0x0E0000u
#define STAGE_PITCH 256u

static unsigned char *g_lo;
static me_mbox g_mb;
static unsigned g_seq, g_gen, g_work, g_full_copy = 1;
static unsigned long g_frames, g_mirror_bad, g_events, g_refusals, g_refusal_bad;
static unsigned g_every, g_rng = 0x9E3779B9u;
static int g_fault_stale;

static void mtm_report(void)
{
   fprintf(stderr, "MTM_SUMMARY frames=%lu mirror_bad=%lu events=%lu "
           "refusals=%lu refusal_bad=%lu\n", g_frames, g_mirror_bad, g_events,
           g_refusals, g_refusal_bad);
}

static void mtm_init(void)
{
   int fd = memfd_create("mtm", 0);
   const char *e = getenv("MTM_EVERY");
   void *a, *b;
   if (fd < 0 || ftruncate(fd, MTM_BYTES) != 0)
   {
      perror("mtm: arena");
      exit(3);
   }
   a = mmap((void *)(uintptr_t)MTM_LO, MTM_BYTES, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
   b = mmap((void *)(uintptr_t)MTM_HI, MTM_BYTES, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
   if (a != (void *)(uintptr_t)MTM_LO || b != (void *)(uintptr_t)MTM_HI)
   {
      fprintf(stderr, "mtm: cannot map the arena at %08x/%08x\n", MTM_LO, MTM_HI);
      exit(3);
   }
   g_lo   = (unsigned char *)a;
   g_work = MTM_LO + OFF_WORK_A;
   g_gen  = 1;
   g_every = e ? (unsigned)strtoul(e, NULL, 10) : 0;
   e = getenv("MTM_FAULT");
   g_fault_stale = e && !strcmp(e, "stale");
   mep_me_render_boot();                 /* the ME leaves reset */
   atexit(mtm_report);
}

/* What a work area holds after the OS has had the pool: anything. */
static void mtm_scramble(unsigned work)
{
   unsigned *p = (unsigned *)(uintptr_t)work, i;
   for (i = 0; i < ME_WORK_BYTES / 4; i++)
   {
      g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
      p[i] = g_rng;
   }
}

static unsigned mtm_post(unsigned work, unsigned work_bytes, int with_map)
{
   me_render_desc *d = (me_render_desc *)(g_lo + OFF_DESC);
   d->vram       = MTM_LO + OFF_VRAM;
   d->oam        = MTM_LO + OFF_OAM;
   d->palette    = MTM_LO + OFF_PAL;
   d->capture    = MTM_LO + OFF_CAP;
   d->out        = MTM_LO + OFF_OUT;
   d->out_pitch  = STAGE_PITCH;
   d->vram_clean = with_map ? MTM_LO + OFF_CLEAN : 0u;
   d->work       = work;
   d->work_bytes = work_bytes;
   d->work_gen   = g_gen;
   g_mb.cmd      = ME_CMD_RENDER;
   g_mb.arg0     = (MTM_LO + OFF_DESC) | 0x40000000u;   /* as ME_UNCACHED() */
   g_mb.cmd_seq  = ++g_seq;
   g_mb.result   = mep_me_render_run(&g_mb, g_seq);
   g_mb.done_seq = g_seq;
   return g_mb.result;
}

/* One vcount-160 post.  `cap` is the frame's me_capture_frame, `log_off` the
 * offset of its log_addr field, `log` its entries.  Returns the FNV-1a hash
 * of the 240x160 frame the module wrote (same hash as mts_fnv). */
unsigned mtm_frame(const void *vram, const void *oam, const void *pal,
                   const void *cap, unsigned cap_bytes, unsigned log_off,
                   const void *log, unsigned log_bytes,
                   const unsigned char *clean, unsigned long frame)
{
   const unsigned short *out;
   unsigned h = 2166136261u, sum = 0, x, y;
   uintptr_t log_at = MTM_LO + OFF_LOG;

   if (!g_lo)
      mtm_init();
   if (cap_bytes > OFF_LOG - OFF_CAP || log_bytes > OFF_VRAM - OFF_LOG)
   {
      fprintf(stderr, "mtm: capture/log larger than the arena slots\n");
      exit(3);
   }
   /* The host's side of the post: the live arrays as they are right now. */
   memcpy(g_lo + OFF_VRAM, vram, 1024 * 96);
   memcpy(g_lo + OFF_OAM, oam, 1024);
   memcpy(g_lo + OFF_PAL, pal, 1024);
   memcpy(g_lo + OFF_CLEAN, clean, 96);
   if (g_fault_stale)
      memset(g_lo + OFF_CLEAN, 1, 96);
   memcpy(g_lo + OFF_CAP, cap, cap_bytes);
   if (log_bytes)
      memcpy(g_lo + OFF_LOG, log, log_bytes);
   memcpy(g_lo + OFF_CAP + log_off, &log_at, sizeof(log_at));

   if (g_every && frame % g_every == 0)
   {
      static const char *const name[4] = { "rebuild", "sleep", "move", "refuse" };
      unsigned ev = (unsigned)(frame / g_every) % 4u;
      g_events++;
      fprintf(stderr, "MTM f=%lu event=%s\n", frame, name[ev]);
      switch (ev)
      {
      case 0:                            /* me_rend_init: new gen, full copy */
         mtm_scramble(g_work);
         g_gen++;
         g_full_copy = 1;
         break;
      case 1:                            /* slept without a teardown */
         mtm_scramble(g_work);
         mep_me_render_boot();
         break;
      case 2:
         g_work = (g_work == MTM_LO + OFF_WORK_A) ? MTM_LO + OFF_WORK_B
                                                  : MTM_LO + OFF_WORK_A;
         mtm_scramble(g_work);
         break;
      default:
      {
         /* No area, a misaligned one, a short one: each must be refused,
          * complete, and leave the stage alone. */
         static const unsigned bad[3][2] = {
            { 0u, ME_WORK_BYTES }, { MTM_LO + OFF_WORK_A + 4u, ME_WORK_BYTES },
            { MTM_LO + OFF_WORK_A, ME_WORK_BYTES - 64u } };
         unsigned i;
         for (i = 0; i < 3; i++)
         {
            memset(g_lo + OFF_OUT, 0x5A, STAGE_PITCH * 160 * 2);
            mtm_post(bad[i][0], bad[i][1], !g_full_copy);
            g_refusals++;
            if (g_mb.refused_seq != g_seq || g_mb.input_seq != g_seq ||
                g_lo[OFF_OUT] != 0x5A || g_lo[OFF_OUT + STAGE_PITCH * 160 * 2 - 1] != 0x5A)
               g_refusal_bad++;
         }
         break;
      }
      }
   }

   sum = mtm_post(g_work, ME_WORK_BYTES, !g_full_copy);
   if (g_mb.refused_seq == g_seq || g_mb.input_seq != g_seq)
   {
      fprintf(stderr, "mtm: a valid render was refused at frame %lu\n", frame);
      exit(3);
   }
   g_full_copy = 0;                      /* mirror valid: dirty pages only */
   g_frames++;

   /* The module's VRAM mirror must be the line-160 VRAM: it is what the next
    * frame's dirty pages are patched onto. */
   if (memcmp((void *)(uintptr_t)(g_work + ME_WORK_VRAM_OFF), vram, 1024 * 96))
   {
      if (g_mirror_bad++ < 5)
         fprintf(stderr, "MTM_MIRROR f=%lu differs\n", frame);
   }

   out = (const unsigned short *)(g_lo + OFF_OUT);
   for (y = 0; y < 160; y++)
      for (x = 0; x < 240; x++)
      {
         h ^= out[y * STAGE_PITCH + x];
         h *= 16777619u;
      }
   (void)sum;
   return h;
}
