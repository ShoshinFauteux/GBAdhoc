/* The LARGEST block malloc can hand out right now, bisected by allocating it
 * and handing it straight back (under the recursive malloc lock).  `top`
 * above is only the top chunk plus the un-sbrk'd tail; a free hole lower in
 * the heap can be bigger, and a live allocation stranded above a hole makes
 * `free` larger than anything one malloc can get.  The residency probe is ONE
 * malloc, so this is the number it is judged by. */
static unsigned heap_largest(void)
{
   unsigned lo = 0, hi = 64u << 20;
   __malloc_lock(_REENT);
   while (hi - lo > 4096u)
   {
      unsigned mid = lo + (hi - lo) / 2u;
      void * volatile p = malloc(mid);
      if (p)
      {
         free(p);
         lo = mid;
      }
      else
         hi = mid;
   }
   __malloc_unlock(_REENT);
   return lo;
}

/* THE HEAP, EXACTLY: everything that says whether two moments have the same
 * heap.  `arena` (bytes newlib has sbrk'd), `used` (uordblks: bytes in live
 * allocations), `chunks` (ordblks: free chunks -- the fragmentation count)
 * and `fordblks` are newlib's own byte counts, not estimates; `free`, `top`
 * and `largest` are the budget figures above (4 KiB bisection).  The
 * in-process return compares a snapshot taken after the unload with the one
 * taken before the ROM loaded (heap_snap_same). */
typedef struct
{
   unsigned arena, used, chunks, fordblks, keep;
   unsigned free, top, largest;
   unsigned kfree, kmax;       /* the kernel partition, outside the heap */
} heap_snap_t;

static void heap_snap(heap_snap_t *s)
{
   struct mallinfo mi;
   __malloc_lock(_REENT);
   mi = mallinfo();
   s->arena    = (unsigned)mi.arena;
   s->used     = (unsigned)mi.uordblks;
   s->chunks   = (unsigned)mi.ordblks;
   s->fordblks = (unsigned)mi.fordblks;
   s->keep     = (unsigned)mi.keepcost;
   heap_budget(&s->free, &s->top);
   s->largest  = heap_largest();
   __malloc_unlock(_REENT);
   s->kfree = (unsigned)sceKernelTotalFreeMemSize();
   s->kmax  = (unsigned)sceKernelMaxFreeMemSize();
}

/* Every field but `arena` and `fordblks`: those two move with newlib's top
 * trim (a probe that grew the heap leaves it a page longer or shorter), and
 * `free` -- fordblks plus the un-sbrk'd tail -- already says the same thing
 * without that noise.  `used` and `chunks` are exact byte and chunk counts. */
static int heap_snap_same(const heap_snap_t *a, const heap_snap_t *b)
{
   return a->used == b->used && a->chunks == b->chunks &&
          a->free == b->free && a->largest == b->largest &&
          a->kfree == b->kfree && a->kmax == b->kmax;
}

#ifndef GPSP_NO_TELEMETRY
static void heap_census(const char *where)
{
   heap_snap_t s;
   heap_snap(&s);
   fe_evt("heap_census at=%s free=%u top=%u largest=%u arena=%u used=%u "
          "chunks=%u fordblks=%u kfree=%u kmax=%u", where, s.free, s.top,
          s.largest, s.arena, s.used, s.chunks, s.fordblks, s.kfree, s.kmax);
}
#endif

/* ---- BOOT MEMORY BUDGET (docs/MEMORY-BUDGET.md) --------------------------
 *
 * The Go's user partition gives this process 5 MiB less than PPSSPP's 64 MiB
 * model, with the same EBOOT (docs/ROM-RESIDENCY.md, 2026-09-27).  PPSSPP
 * cannot shrink its user memory, so `boot_ballast_kb = 5120` (harness only)
 * holds that 5 MiB from the first statement of main(), before the netdrv
 * arena, the renderer, the translation caches or the ROM: everything the
 * process does afterwards sees the Go's heap.  (`rom_ballast_kb` is the older,
 * later ballast: it is taken just before the JIT decision.) */
#ifdef GPSP_PERF_RIG
static unsigned g_boot_ballast_kb, g_boot_ballast_got;
/* Kept in a global: a block nobody can reach is a block the compiler may
 * delete, malloc and memset included (it did, in the first build). */
static void * volatile g_boot_ballast;
static void boot_ballast_take(void)
{
   g_boot_ballast_kb = (unsigned)fe_ini_get_int(HARNESS_INI,
                                                "boot_ballast_kb", 0);
   if (g_boot_ballast_kb)
   {
      /* Held for the life of the process, and touched so it is real. */
      void *b = malloc((size_t)g_boot_ballast_kb << 10);
      if (b)
      {
         memset(b, 0, (size_t)g_boot_ballast_kb << 10);
         g_boot_ballast = b;
         g_boot_ballast_got = g_boot_ballast_kb;
      }
   }
}
#endif

#if defined(GBA_LINK) || defined(GPSP_MEM_BUDGET)
/* THE GBA CORE'S MINIMUM, measured (MEMORY-BUDGET.md, PPSSPP boot_ballast
 * sweep of the link build).  Just before retro_init, init_gamepak_buffer
 * holds a 1 MiB post-load reserve and then takes 1 MiB ROM blocks until
 * malloc fails.  With ZERO blocks it does not fail: gamepak_buffers[0] stays
 * NULL and the first ROM page fault translates from address 0 -- PPSSPP shows
 * "Bad memory access ... at translate_block_arm" forever, i.e. a hang
 * (heap 1,977,912 B at the probe).  One block is enough to run (2,502,200 B:
 * AW2 paged through a single block, exit 0).  So the gate is the reserve,
 * one block and 64 KiB for the small allocations retro_init makes on the way
 * (measured: 4.4 KB).  Asked the same way the core asks: separate mallocs. */
#define MEM_GBA_CORE_MIN ((2u << 20) + (64u << 10))
static int mem_budget_gba_ok(unsigned *largest)
{
   void * volatile a = malloc(1u << 20);
   void * volatile b = a ? malloc(1u << 20) : NULL;
   void * volatile c = b ? malloc(64u << 10) : NULL;
   int ok = c != NULL;
   unsigned lo = 0, hi = 64u << 20;
   free(c);
   free(b);
   free(a);
   if (!ok)
      while (hi - lo > 4096u)   /* only for the message */
      {
         unsigned mid = lo + (hi - lo) / 2u;
         void * volatile p = malloc(mid);
         if (p)
         {
            free(p);
            lo = mid;
         }
         else
            hi = mid;
      }
   *largest = lo;
   return ok;
}
#endif

#ifndef GPSP_NO_TELEMETRY
/* One line, the first thing after boot_ok: what this console gave this binary.
 *   image   = the loaded ELF, text through .bss (__executable_start.._end)
 *   k_free / k_max = the kernel's view OUTSIDE the heap block
 *   heap    = newlib's heap block (arena already sbrk'd + the un-sbrk'd tail)
 *   free / largest = what malloc can still hand out (heap_budget/_largest)
 * MEMORY-BUDGET.md turns these into the per-model table. */
extern char __executable_start[], _end[];
static void boot_mem_census(void)
{
   struct mallinfo mi;
   unsigned total = 0, top = 0, big;
   heap_budget(&total, &top);
   big = heap_largest();
   mi = mallinfo();
   fe_evt("mem_boot image=%u k_free=%d k_max=%d heap=%u free=%u largest=%u "
          "ballast_kb=%u",
          (unsigned)(_end - __executable_start),
          sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize(),
          (unsigned)mi.arena + (total - (unsigned)mi.fordblks), total, big,
#ifdef GPSP_PERF_RIG
          g_boot_ballast_got
#else
          0u
#endif
          );
}
#endif

/* ---- ROM PAGE CACHE SIZE ------------------------------------------------
 *
 * init_gamepak_buffer (retro_init) takes 1 MiB blocks until malloc fails or
 * gamepak_buffer_cap is reached.  The core's default cap is ROM_BUFFER_SIZE
 * (15 on the PSP, root Makefile), so a 32 MiB cart pages on every model.
 *
 * `rom_resident` (CONFIG.INI, not written back; default 1 since 3.0.0, when the
 * Go A/B rig passed 21/21) asks the core to hold the cart whole.  The wish is
 * the cart's size in 1 MiB blocks (a 16 MiB Emerald asks for 16, Unbound for
 * 32), so a cart never costs more than itself.  The core decides in
 * init_gamepak_buffer, the last allocation of startup, AFTER any large
 * translation-cache tier has taken its block: it takes the blocks one by one
 * while holding the 1 MiB post-load reserve, and keeps them only if every one
 * was granted -- the same >= 1 MiB post-load floor a PSP-1000 lives on.  A
 * short heap hands the extra blocks straight back and the cart pages at the
 * default cap.  A PSP-1000 (whose loop always stops at ~13) is unchanged.
 * Decided by the memory actually granted, not the model number.  EVT
 * rom_cache ... plan= says what the core did.
 *
 * Harness keys (GPSP_PERF_RIG builds only) for the hardware A/B:
 *   rom_cap = N          force the cap to N blocks (1..32), no probe
 *   swap_stubs = 1       resident carts still get the paged-ROM load stubs
 *   cache_paranoid = 1   every JIT cache sync is a full D+I cache sync
 *   rom_ballast_kb = N   hold N KiB of heap first, so the ROM lands higher
 *   jit_coherency_scan = N   every N frames, count RAM-cache code words the
 *                        D-cache holds but memory does not (EVT jit_coh)
 *   jit_code_mode = M    JIT_CODE_AB cores only (docs/JIT-CODE-DISCIPLINE.md):
 *                        0 legacy syncs, 1 + 8b48b48, 2 discipline (default),
 *                        3 discipline without the ownership invalidate */
extern u32 gamepak_buffer_cap;
extern u32 gamepak_resident_wanted;
extern const char *gamepak_cap_reason;
extern u32 gamepak_force_swap_stubs;
#ifdef PSP
extern u32 platform_cache_paranoid;
extern u32 jit_coherency_scan(u32 *first);
#endif
#ifdef GPSP_PERF_RIG
static unsigned g_jit_coh_every;
#endif
/* JIT_CODE_AB / JIT_CODE_DISCIPLINE cores (weak: every core links). */
extern u32 jit_code_mode __attribute__((weak));
/* The cart's size in whole 1 MiB blocks (what gamepak_must_swap measures
 * against), or 32 if it cannot be read -- the largest a cart can be. */
static unsigned rom_cart_blocks(const char *path)
{
   SceIoStat st;
   unsigned long long sz;
   memset(&st, 0, sizeof(st));
   if (sceIoGetstat(path, &st) < 0 || st.st_size <= 0)
      return 32;
   sz = (unsigned long long)st.st_size;
   if (sz > (32ull << 20))
      return 32;
   return (unsigned)((sz + (1u << 20) - 1) >> 20);
}

static void rom_cache_select(const char *rom_path)
{
   unsigned cap = gamepak_buffer_cap;          /* the core default */
   const char *why = "default";

   /* The ATTEMPT is the core's (init_gamepak_buffer): it runs after the
    * translation-cache tier has taken its block, so it sees what is really
    * left.  The frontend only states the wish, sized to the cart. */
   gamepak_resident_wanted = g_pcfg.rom_resident ? rom_cart_blocks(rom_path)
                                                 : 0u;
   if (gamepak_resident_wanted > cap)
      why = "resident_requested";
   else if (gamepak_resident_wanted)
      why = "fits_default";   /* the default cap already holds the cart */
#ifdef GPSP_PERF_RIG
   {
      int hcap = (int)fe_ini_get_int(HARNESS_INI, "rom_cap", 0);
      unsigned ballast = (unsigned)fe_ini_get_int(HARNESS_INI,
                                                  "rom_ballast_kb", 0);
      if (hcap >= 1 && hcap <= 32)
      {
         cap = (unsigned)hcap;
         why = "harness";
         gamepak_resident_wanted = 0;   /* forced: no probe */
      }
      gamepak_force_swap_stubs =
         fe_ini_get_int(HARNESS_INI, "swap_stubs", 0) ? 1u : 0u;
      platform_cache_paranoid =
         fe_ini_get_int(HARNESS_INI, "cache_paranoid", 0) ? 1u : 0u;
      g_jit_coh_every =
         (unsigned)fe_ini_get_int(HARNESS_INI, "jit_coherency_scan", 0);
      unsigned ballast_got = 0;
      if (ballast)
      {
         /* Held for the life of the process, and touched so it is real. */
         void *b = malloc((size_t)ballast << 10);
         if (b)
         {
            memset(b, 0, (size_t)ballast << 10);
            ballast_got = ballast;
         }
         fe_evt("rom_ballast kb=%u at=%08x", ballast,
                (unsigned)(uintptr_t)b);
      }
      /* APPLIED values, one line, every run: the rig compares these with what
       * it staged (a key can be read and still clamped or refused). */
      fe_evt("rom_diag rom_cap=%d swap_stubs=%u cache_paranoid=%u "
             "jit_coh_every=%u ballast_kb=%u",
             (hcap >= 1 && hcap <= 32) ? hcap : 0,
             (unsigned)gamepak_force_swap_stubs,
             (unsigned)platform_cache_paranoid, g_jit_coh_every, ballast_got);
      /* Before fe_host_boot: the mode must hold from the first translation. */
      if (&jit_code_mode)
      {
         int m = (int)fe_ini_get_int(HARNESS_INI, "jit_code_mode", -1);
         if (m >= 0 && m <= 3)
            jit_code_mode = (u32)m;
         fe_evt("jit_code mode=%u key=%d", (unsigned)jit_code_mode, m);
      }
#ifdef JIT_COST_KEYS
      /* JIT_COST cores (docs/JIT-COST.md): the run-time choice of each
       * piece, so one binary A/Bs it with the code layout held fixed.
       *   jit_pub_inline = 0|1   publish by kernel calls | user line ops
       *   xlat_scan_fast = 0|1   RAM ARM scan: original | SCANWIN+SCANCOND
       * Absent keys keep the core's default (1). */
      {
         extern u32 jit_pub_inline __attribute__((weak));
         extern u32 xlat_scan_fast __attribute__((weak));
         int pi = (int)fe_ini_get_int(HARNESS_INI, "jit_pub_inline", -1);
         int sf = (int)fe_ini_get_int(HARNESS_INI, "xlat_scan_fast", -1);
         if (&jit_pub_inline && (pi == 0 || pi == 1))
            jit_pub_inline = (u32)pi;
         if (&xlat_scan_fast && (sf == 0 || sf == 1))
            xlat_scan_fast = (u32)sf;
         fe_evt("jit_cost pub_inline=%d scan_fast=%d keys=%d/%d",
                &jit_pub_inline ? (int)jit_pub_inline : -1,
                &xlat_scan_fast ? (int)xlat_scan_fast : -1, pi, sf);
      }
#endif
   }
#endif
   gamepak_buffer_cap = cap;
   fe_evt("rom_cache_cap blocks=%u reason=%s want=%u", cap, why,
          (unsigned)gamepak_resident_wanted);
   (void)why;
}

