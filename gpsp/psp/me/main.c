/* gbadhoc-me kernel PRX — boots the Media Engine and runs its dispatcher.
 *
 * Lineage: the boot sequence and cache ops follow DaedalusX64's
 * MediaEngine PRX (GPL-2.0; itself from the melib package, (c) mrbrown).
 * Two deliberate departures, both ADR-0080:
 *
 *  1. NO exported functions.  Daedalus exports InitME/KillME and links
 *     import stubs into the app — but import stubs only resolve if the PRX
 *     is resident before the importing module loads, and this app loads
 *     the PRX itself at runtime (same pattern as usb_handoff.c's firmware
 *     modules).  So the PRX is parameterised through module_start's
 *     argument block instead: the app passes the mailbox pointer in
 *     sceKernelStartModule args, and every later interaction is through
 *     the mailbox.  Nothing to import, no load-order trap.
 *
 *  2. NO function pointers cross the boundary.  Daedalus dispatches
 *     arbitrary app functions on the ME; every job here is compiled into
 *     THIS PRX and selected by ME_CMD_*.  Kernel .text lives in kseg0
 *     (0x88xxxxxx), which the ME sees identically — user .text's
 *     cacheability and GP conventions never become ME problems.
 *
 * ME-side rules (me_dispatch and everything it calls): no syscalls, no
 * printf, no VFPU, no floating point needed; cache ops via
 * __builtin_allegrex_cache only; all mailbox access through the uncached
 * alias the host handed us.
 */
#include <pspsdk.h>
#include <pspkernel.h>
#include <pspsysreg.h>
#include <string.h>

#include "me_mbox.h"

PSP_MODULE_INFO("gbadhoc_me", 0x1006, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

extern void me_stub(void);
extern void me_stub_end(void);

/* ---- ME-side cache ops (run ON the ME) --------------------------------- */
/* Index ops over 16 KiB: correct for a 16K cache, harmlessly doubled work
 * for an 8K one (see me_stub.S header for why 16K).  The double-issue in
 * wbinv is the documented Allegrex cache-op hazard workaround. */

static void me_dcache_wbinv_all(void)
{
   int i;
   for (i = 0; i < 16384; i += 64)
   {
      __builtin_allegrex_cache(0x14, i);
      __builtin_allegrex_cache(0x14, i);
   }
}

static void me_dcache_inv_range(void *addr, int size)
{
   int i, j = (int)addr;
   for (i = j; i < size + j; i += 64)
      __builtin_allegrex_cache(0x19, i);
}

/* ---- ME-side jobs ------------------------------------------------------ */

/* Word copy, 8-way unrolled, returning a additive checksum of everything
 * copied (the checksum is what makes the benchmark unfakeable — the host
 * verifies it against a host-side sum of the same buffer).  Pointers are
 * expected 64-byte aligned and len a multiple of 32; the staging and
 * screen buffers both satisfy this (77,280 = 32*2415; 82,432 = 32*2576). */
static unsigned int me_copy32(const unsigned int *src, unsigned int *dst,
                              unsigned int len)
{
   unsigned int sum = 0;
   unsigned int n = len >> 5;             /* 32-byte chunks */
   while (n--)
   {
      unsigned int a0 = src[0], a1 = src[1], a2 = src[2], a3 = src[3];
      unsigned int a4 = src[4], a5 = src[5], a6 = src[6], a7 = src[7];
      dst[0] = a0; dst[1] = a1; dst[2] = a2; dst[3] = a3;
      dst[4] = a4; dst[5] = a5; dst[6] = a6; dst[7] = a7;
      sum += a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
      src += 8; dst += 8;
   }
   return sum;
}

static unsigned int me_do_copy(volatile me_mbox *mb)
{
   unsigned int src = mb->arg0, dst = mb->arg1;
   unsigned int len = mb->arg2, flags = mb->arg3;
   unsigned int sum;

   /* flags bit0: read src through the ME D-cache (invalidate first so we
    * see the SC's freshly written data).  bit1: write dst through the
    * D-cache (writeback-invalidate after so RAM has it before we ack).
    * Default (0) is uncached both ways — always correct, never fastest. */
   const unsigned int csrc = (flags & 1) ? (src & ~0x40000000u)
                                         : (src |  0x40000000u);
   const unsigned int cdst = (flags & 2) ? (dst & ~0x40000000u)
                                         : (dst |  0x40000000u);
   if (flags & 1)
      me_dcache_inv_range((void *)csrc, (int)len);

   sum = me_copy32((const unsigned int *)csrc, (unsigned int *)cdst, len);

   if (flags & 2)
      me_dcache_wbinv_all();
   return sum;
}

/* ME_CMD_RENDER runs the real scanline renderer (video.cc), transplanted onto
 * the ME by psp/me/me_render_glue.cc — the loop proven bit-exact on the desktop
 * (RENDER_REPLAY + ME_CAP_VALIDATE).  v2: two-phase (snapshot -> input_seq ->
 * render), so the host resumes emulation while the ME renders in parallel. */
extern unsigned int me_render_run(volatile me_mbox *mb, unsigned int seq);

/* ---- the dispatcher (ME entry point, jumped to by me_stub) ------------- */

/* ADR-0080a: IDLE BUS BACKOFF.  The first resident-ME hardware run
 * (fs-me auto321) measured srtt 17->44 ms, the client queue driven to its
 * 64-slot ceiling, and a failed trade — because this loop polled the
 * mailbox FLAT OUT with uncached reads/writes, and every uncached access
 * is a full main-memory transaction on the bus the WLAN DMA shares.  When
 * idle, spin on a CACHED local instead (ME stack is kseg0-cached, so it
 * touches no bus) and poll only every ~50 us.  That is <<1 frame of
 * latency for a posted video job but frees the bus ~99.9 % of idle time.
 * Sized for ~333 MHz: ~16650 cycles / ~4 per iter ~= 4096. */
#define ME_IDLE_SPIN 4096u

static void me_dispatch(volatile me_mbox *mb)
{
   unsigned int seen;

   mb->version = ME_MBOX_VERSION;
   mb->magic   = ME_MBOX_MAGIC;
   seen = mb->cmd_seq;
   mb->done_seq = seen;

   for (;;)
   {
      unsigned int seq = mb->cmd_seq;
      if (seq == seen)
      {
         volatile unsigned int d = ME_IDLE_SPIN;
         while (d)
            d--;
         mb->heartbeat = mb->heartbeat + 1;   /* liveness, throttled */
         continue;
      }
      seen = seq;
      mb->heartbeat = mb->heartbeat + 1;

      switch (mb->cmd)
      {
      case ME_CMD_COPY:
         mb->result = me_do_copy(mb);
         break;
      case ME_CMD_COPY_PITCH:
      {
         /* Row-by-row copy with independent pitches — the video staging job
          * (480-byte rows into a 512-byte-stride texture).
          *
          * ADR-0082: read the SOURCE through the ME's D-cache, not uncached.
          * The bench proved cached reads are 4x faster (701us vs 2716us for a
          * frame) — uncached reads are single-word round trips, cached reads
          * burst.  Correctness: the SC wrote the frame cached and does a
          * ranged writeback before posting (me_host_post_pitch_copy), so RAM
          * is fresh; we invalidate the source span first so the ME cannot
          * serve a stale line.  The DESTINATION stays uncached so the GE
          * sees our stores in RAM with no ME writeback.  This ~2ms saved is
          * what lets the copy finish inside the vblank gap, which is what
          * makes present-at-top (ADR-0082) safe. */
         unsigned int rows = mb->arg2 >> 16;
         unsigned int rowb = mb->arg2 & 0xFFFFu;
         unsigned int sp   = mb->arg4, dp = mb->arg5;
         unsigned int src  = mb->arg0 & ~0x40000000u;   /* cached read */
         unsigned int dst  = mb->arg1 |  0x40000000u;   /* uncached write */
         unsigned int span = rows * sp;
         unsigned int sum  = 0;
         me_dcache_inv_range((void *)src, (int)span);
         while (rows--)
         {
            sum += me_copy32((const unsigned int *)src,
                             (unsigned int *)dst, rowb);
            src += sp; dst += dp;
         }
         mb->result = sum;
         break;
      }
      case ME_CMD_RENDER:
         mb->result = me_render_run(mb, seen);
         break;
      case ME_CMD_BENCH_COPY:
      {
         /* arg3 low 16 bits = flags, high 16 = iteration count.  The host
          * times the whole round trip; result is the last checksum. */
         unsigned int iters = mb->arg3 >> 16;
         unsigned int flags = mb->arg3 & 0xFFFFu;
         unsigned int sum = 0;
         if (!iters)
            iters = 1;
         while (iters--)
         {
            mb->arg3 = flags;                 /* me_do_copy reads flags   */
            sum = me_do_copy(mb);
         }
         mb->result = sum;
         break;
      }
      case ME_CMD_PARK:
         /* Quiet the bus: ack, then poll only every ~64k spins so a
          * parked ME steals almost no memory bandwidth from the WLAN
          * DMA — the contention this project is most afraid of. */
         mb->done_seq = seen;
         for (;;)
         {
            volatile int spin = 65536;
            while (spin--)
               ;
            if (mb->cmd_seq != seen)
               break;
            mb->heartbeat = mb->heartbeat + 1;
         }
         continue;   /* new command: loop picks it up (seen != cmd_seq) */
      case ME_CMD_NOP:
      default:
         mb->result = 0;
         break;
      }
      mb->done_seq = seen;
   }
}

/* ---- kernel-side boot (runs on the main CPU, kernel mode) -------------- */

static int me_boot(volatile me_mbox *mb)
{
   unsigned int k1 = pspSdkSetK1(0);

   memcpy((void *)0xbfc00040, me_stub, (int)((int)me_stub_end - (int)me_stub));
   _sw((unsigned int)me_dispatch, 0xbfc00600);   /* k0 = entry (kseg0 PRX) */
   _sw((unsigned int)mb,          0xbfc00604);   /* a0 = mailbox           */
   sceKernelDcacheWritebackAll();
   sceSysregMeResetEnable();
   sceSysregMeBusClockEnable();
   sceSysregMeResetDisable();

   pspSdkSetK1(k1);
   return 0;
}

/* The app passes: u32[0] = mailbox pointer (uncached user alias). */
int module_start(SceSize args, void *argp)
{
   volatile me_mbox *mb;
   if (args < 4 || !argp)
      return -1;
   mb = (volatile me_mbox *)(*(unsigned int *)argp);
   if (!mb)
      return -1;
   return me_boot(mb);
}

int module_stop(SceSize args, void *argp)
{
   (void)args; (void)argp;
   /* Hold the ME in reset on the way out — the dispatcher dies with it.
    * (Suspend/resume: v0 does not support sleep with the ME live; the
    * host watchdog treats a silent heartbeat as ME-down and falls back.) */
   sceSysregMeResetEnable();
   return 0;
}
