/* icprobe.c -- the Allegrex instruction cache's geometry, measured in user
 * mode on the console itself (docs/CACHE-MAP.md).  Harness only
 * (GPSP_PERF_RIG): `icache_probe = 1` runs it instead of a game.
 *
 * WHY.  The cache map and any cache-aware layout rest on the geometry (size,
 * associativity, line), and nothing in this project had measured it; the
 * figures in circulation (16 KiB, 2-way, 64 B) were quoted, not shown.
 *
 * HOW.  Generated code: K tiny blocks, block i at base + i*S, each jumping to
 * the next, the last looping back until a0 reaches 0.  Every block is one
 * cache line's worth of fetch.  With S equal to the way size, all K blocks
 * fall in ONE set: K <= ways run from the cache, K = ways + 1 misses on every
 * block (LRU) -- a step of several times in ns per block.  Other strides
 * tell the way size apart (S = way/2 puts two blocks per set, so the step
 * moves to K = 2 * ways + 1).  The line size comes from cold runs: blocks
 * packed at stride s < line share lines, so a cold chain at s = 32 costs
 * half the misses of s = 64 when the line is 64 B.
 *
 * Timing is sceKernelGetSystemTimeLow (us; user mode cannot read CP0
 * Count), over enough visits to be milliseconds, minimum of REPS runs
 * (interrupts and other threads only ever ADD time).  PPSSPP does not model
 * the cache: every stride reads flat -- the analyser
 * (tools/cachemap/icprobe_geometry.py) must then report no geometry, which
 * is its negative control. */
#ifdef GPSP_PERF_RIG
#include <pspkernel.h>
#include <psputils.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include "fe_evt.h"
#include "icprobe.h"

#define BUF_BYTES   (1u << 20)      /* 1 MiB: K * S up to 12 * 64 KiB */
#define REPS        7
#define VISITS      240000u         /* block visits per timed run */

/* MIPS I encodings */
#define OP_NOP          0x00000000u
#define OP_J(t)         (0x08000000u | ((((uint32_t)(t)) >> 2) & 0x03FFFFFFu))
#define OP_ADDIU_A0_M1  0x2484FFFFu /* addiu $a0, $a0, -1 */
#define OP_BEQ_A0_0(o)  (0x10800000u | ((uint32_t)(o) & 0xFFFFu))
#define OP_JR_RA        0x03E00008u

static uint32_t *g_buf;

static void sync_code(void)
{
   sceKernelDcacheWritebackInvalidateAll();
   sceKernelIcacheInvalidateAll();
}

/* K blocks at stride S bytes from the buffer's base; returns the entry. */
static void *build_chain(unsigned k, unsigned s)
{
   uint8_t *base = (uint8_t *)g_buf;
   unsigned i;
   for (i = 0; i < k; i++)
   {
      uint32_t *b = (uint32_t *)(base + i * s);
      if (i + 1 < k)
      {
         b[0] = OP_J(base + (i + 1) * s);
         b[1] = OP_NOP;
      }
      else
      {
         /* 0 addiu a0,-1; 1 beq a0,0,+3 (-> 5); 2 nop; 3 j block0; 4 nop;
          * 5 jr ra; 6 nop */
         b[0] = OP_ADDIU_A0_M1;
         b[1] = OP_BEQ_A0_0(3);
         b[2] = OP_NOP;
         b[3] = OP_J(base);
         b[4] = OP_NOP;
         b[5] = OP_JR_RA;
         b[6] = OP_NOP;
      }
   }
   sync_code();
   return base;
}

typedef void (*chain_fn)(uint32_t loops);

static uint32_t time_warm(unsigned k, unsigned s)
{
   chain_fn fn = (chain_fn)build_chain(k, s);
   uint32_t loops = VISITS / k, best = 0xFFFFFFFFu;
   int r;
   fn(4);                                   /* warm */
   for (r = 0; r < REPS; r++)
   {
      uint32_t t0 = sceKernelGetSystemTimeLow(), dt;
      fn(loops);
      dt = sceKernelGetSystemTimeLow() - t0;
      if (dt < best)
         best = dt;
   }
   /* ns per block visit, x10 for one decimal */
   return (uint32_t)((uint64_t)best * 10000u / ((uint64_t)loops * k));
}

/* One pass through K blocks from a cold I-cache, ns x10 per block. */
static uint32_t time_cold(unsigned k, unsigned s)
{
   chain_fn fn = (chain_fn)build_chain(k, s);
   uint32_t best = 0xFFFFFFFFu;
   int r;
   for (r = 0; r < REPS; r++)
   {
      uint32_t t0, dt;
      sceKernelIcacheInvalidateAll();
      t0 = sceKernelGetSystemTimeLow();
      fn(1);
      dt = sceKernelGetSystemTimeLow() - t0;
      if (dt < best)
         best = dt;
   }
   return (uint32_t)((uint64_t)best * 10000u / k);
}

int icprobe_run(void)
{
   static const unsigned strides[] = { 64, 1024, 2048, 4096, 8192, 16384,
                                       32768, 65536 };
   static const unsigned cold_s[] = { 16, 32, 64, 128, 256 };
   unsigned i, k;
   void *raw = memalign(65536, BUF_BYTES);
   if (!raw)
   {
      fe_evt("icprobe_fail reason=memory bytes=%u", BUF_BYTES);
      return -1;
   }
   g_buf = (uint32_t *)raw;
   memset(g_buf, 0, BUF_BYTES);
   fe_evt("icprobe_begin base=%p visits=%u reps=%d", raw, VISITS, REPS);
   for (i = 0; i < sizeof(strides) / sizeof(strides[0]); i++)
      for (k = 1; k <= 12; k++)
      {
         if (k * strides[i] > BUF_BYTES)
            break;
         fe_evt("icprobe_warm s=%u k=%u ns10=%u", strides[i], k,
                time_warm(k, strides[i]));
      }
   /* Cold: 2048 blocks (the chain spans up to 512 KiB, far past the cache),
    * so the per-block cost is the miss cost divided by blocks per line. */
   for (i = 0; i < sizeof(cold_s) / sizeof(cold_s[0]); i++)
      fe_evt("icprobe_cold s=%u k=%u ns10=%u", cold_s[i], 2048u,
             time_cold(2048, cold_s[i]));
   fe_evt("icprobe_end");
   free(raw);
   g_buf = NULL;
   return 0;
}

/* ---- icache_inval_probe = 1: does the KERNEL's ranged invalidate work? ----
 * (docs/JIT-COHERENCY.md.)  The coherency checker found the dynarec's own
 * sync protocol complete in the twin under the kernel's documented behaviour,
 * and pinned the resident-ROM derail's site-B signature on one thing it cannot
 * observe: sceKernelIcacheInvalidateRange takes a DIFFERENT path for
 * size >= 16 KiB (uOFW src/kd/sysmem/start.S: below the I-cache size it runs
 * `cache 0x8` over every line; at or above it walks the tag RAM with
 * `cache 0x0` + TAG_LO/TAG_HI over 8 KiB of indices).  This probe tests that
 * path directly, with no emulator involved.
 *
 * Per trial: NB tiny functions, one per 64-byte line (`jr ra; li v0, OLD+i`),
 * are written, made coherent the safe way, and each CALLED twice so their
 * lines are in the I-cache.  Each is then rewritten to return NEW+i, the D
 * side is written back EXACTLY (sceKernelDcacheWritebackRange, small range),
 * one invalidate MODE is applied, and every function is called once more:
 * a result of OLD+i is a stale line that the mode did not invalidate.
 *
 * Modes:  none      control: how many lines survive to be stale at all
 *         all       sceKernelIcacheInvalidateAll (must be 0)
 *         r_exact   IcacheInvalidateRange(region, NB*64)      (< 16 KiB path)
 *         r_16383   (region, 16383): last size on the line-loop path
 *         r_16384   (region, 16384): first size on the tag-walk path
 *         r_32k     (region, 32768): tag-walk path
 *         r_mid32k  (region - 16384, 32768): tag-walk, region in the middle
 *         d_big     D written back with ONE 64 KiB DcacheWritebackRange (the
 *                   D tag-walk path), then IcacheInvalidateAll: stale here
 *                   means the D big path left the new code unwritten
 * Each at region offsets 0, 4 KiB and 8 KiB + 0xC40 (unaligned to 8 KiB),
 * REPS times.  PPSSPP models no cache: every mode must read 0 there, `none`
 * included (negative control).  On a console, `none` > 0 and `all` = 0 make a
 * valid run; then r_16384/r_32k/r_mid32k > 0 with r_16383 = 0 convicts the
 * kernel's large-range path. */
#define ICINV_NB    64u                 /* functions (lines): 4 KiB */
#define ICINV_OLD   0x1000u
#define ICINV_NEW   0x2000u
#define OP_LI_V0(v) (0x24020000u | ((uint32_t)(v) & 0xFFFFu))   /* addiu v0,zero,v */

typedef uint32_t (*icinv_fn)(void);

static void icinv_write(uint8_t *region, uint32_t val)
{
   unsigned i;
   for (i = 0; i < ICINV_NB; i++)
   {
      uint32_t *b = (uint32_t *)(region + i * 64u);
      b[0] = OP_JR_RA;
      b[1] = OP_LI_V0(val + i);
   }
}

enum { M_NONE, M_ALL, M_EXACT, M_16383, M_16384, M_32K, M_MID32K, M_DBIG, M_COUNT };
static const char *icinv_mode_name[M_COUNT] = {
   "none", "all", "r_exact", "r_16383", "r_16384", "r_32k", "r_mid32k", "d_big" };

/* one trial; returns stale count, *odd = results that are neither */
static unsigned icinv_trial(uint8_t *region, int mode, unsigned *odd)
{
   unsigned i, stale = 0;
   volatile uint32_t sink = 0;
   icinv_write(region, ICINV_OLD);
   sceKernelDcacheWritebackInvalidateAll();
   sceKernelIcacheInvalidateAll();
   for (i = 0; i < ICINV_NB; i++)                  /* fill the I lines */
      sink += ((icinv_fn)(region + i * 64u))();
   for (i = 0; i < ICINV_NB; i++)
      sink += ((icinv_fn)(region + i * 64u))();
   icinv_write(region, ICINV_NEW);
   if (mode == M_DBIG)
      sceKernelDcacheWritebackRange(region, 65536);
   else
      sceKernelDcacheWritebackRange(region, ICINV_NB * 64u);
   switch (mode)
   {
   case M_ALL:    case M_DBIG: sceKernelIcacheInvalidateAll(); break;
   case M_EXACT:  sceKernelIcacheInvalidateRange(region, ICINV_NB * 64u); break;
   case M_16383:  sceKernelIcacheInvalidateRange(region, 16383); break;
   case M_16384:  sceKernelIcacheInvalidateRange(region, 16384); break;
   case M_32K:    sceKernelIcacheInvalidateRange(region, 32768); break;
   case M_MID32K: sceKernelIcacheInvalidateRange(region - 16384, 32768); break;
   default: break;
   }
   *odd = 0;
   for (i = 0; i < ICINV_NB; i++)
   {
      uint32_t v = ((icinv_fn)(region + i * 64u))();
      if (v == ICINV_OLD + i)
         stale++;
      else if (v != ICINV_NEW + i)
         (*odd)++;
   }
   (void)sink;
   return stale;
}

int icinv_probe_run(void)
{
   static const unsigned offs[] = { 0, 4096, 8192 + 0xC40 };
   unsigned o, m, r;
   /* 64 KiB of slack below and 64 KiB above the regions, so every range the
    * probe hands the kernel lies inside the buffer */
   uint8_t *raw = (uint8_t *)memalign(65536, 4 * 65536);
   if (!raw)
   {
      fe_evt("icinv_fail reason=memory");
      return -1;
   }
   memset(raw, 0, 4 * 65536);
   fe_evt("icinv_begin base=%p lines=%u reps=%d", raw, ICINV_NB, REPS);
   for (o = 0; o < sizeof(offs) / sizeof(offs[0]); o++)
   {
      uint8_t *region = raw + 65536 + offs[o];
      for (m = 0; m < M_COUNT; m++)
      {
         unsigned min = 0xFFFFFFFFu, max = 0, sum = 0, odd_sum = 0;
         for (r = 0; r < REPS; r++)
         {
            unsigned odd, s = icinv_trial(region, (int)m, &odd);
            sum += s;
            odd_sum += odd;
            if (s < min) min = s;
            if (s > max) max = s;
         }
         fe_evt("icinv off=%u mode=%s stale_min=%u stale_max=%u stale_sum=%u of=%u odd=%u",
                offs[o], icinv_mode_name[m], min, max, sum, ICINV_NB * REPS, odd_sum);
      }
   }
   fe_evt("icinv_end");
   free(raw);
   return 0;
}
#else
typedef int icprobe_release_unit; /* not an empty translation unit */
#endif
