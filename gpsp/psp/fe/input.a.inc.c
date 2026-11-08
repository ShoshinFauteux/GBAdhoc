/* ------------------------------------------------- core perf counters ---- */

/* ADR-0028.  gpsp's own monotonic counters, declared here rather than by
 * including the core's private headers (main.h / gba_memory.h are not on the
 * frontend include path, and frontend-common must stay core-agnostic — hence
 * the callback in fe_host_config).  Same boundary discipline as the
 * gpsp_rfu_link_down_hook weak symbol.
 *   flush_rom_total    ROM translation-cache flushes.  One of these
 *                      discards the whole ROM cache's generated code (2 MiB
 *                      on the SMALL tier, 10 MiB on the LARGE one -- see
 *                      `EVT jit_cache` for which this boot got),
 *                      memsets a 256 KiB branch-hash table, and forces every
 *                      subsequent block to be re-translated.
 *   flush_ram_full     RAM cache flushes caused by the RAM JIT cache running
 *                      out of room.  A bigger cache is the lever.
 *   flush_ram_smc      RAM cache flushes caused by a CPU store into RAM that
 *                      holds translated code.  The WHOLE RAM cache goes, plus
 *                      a memset of up to 288 KiB of SMC shadow, and a bigger
 *                      cache does nothing for these (ADR-0029).
 *   flush_ram_dma      the same, but triggered by a DMA transfer.
 *   gamepak_page_loads 32 KiB reads from the memory stick, mid-emulation,
 *                      when the ROM does not fit in RAM.  ADR-0026 priced
 *                      that stick at 34 ms for 4 KiB. */
extern u32 flush_rom_total;
extern u32 flush_ram_full;
extern u32 flush_ram_smc;
extern u32 flush_ram_dma;
extern u32 gamepak_page_loads;
/* What the flushes COST (main.h): time inside flush_translation_cache_ram(),
 * the tag-memset volume, and how often it took the whole-region else-branch
 * (32 KiB IWRAM / 256 KiB EWRAM).  smc_prof_clock is already installed below,
 * so these have been accumulating all along — they were just never shown. */
extern u32 smc_flush_us;
extern u32 smc_flush_bytes;
extern u32 smc_flush_wide;
extern u32 smc_blk_xlat_total;   /* RAM blocks re-translated (monotonic) */
extern u32 cph_jit_total;        /* dynarec compilation us (monotonic) */
extern u32 smc_blk_watch;        /* 256-byte page the block/writer probe eyes */

static void plat_core_counters(unsigned *rom_flush, unsigned *ram_full,
                               unsigned *ram_smc, unsigned *ram_dma,
                               unsigned *page_load)
{
   *rom_flush = (unsigned)flush_rom_total;
   *ram_full  = (unsigned)flush_ram_full;
   *ram_smc   = (unsigned)flush_ram_smc;
   *ram_dma   = (unsigned)flush_ram_dma;
   *page_load = (unsigned)gamepak_page_loads;
}

/* Translation-cache tier (cpu_threaded.c, dynarec_select_translation_caches).
 * The core decides in retro_init, before the ROM page cache eats the heap;
 * the frontend's only inputs are the harness override and the log line. */
extern u32 jit_cache_tier;            /* 0 undecided, 1 small, 2 large */
extern u32 jit_cache_force_small;
extern const char *jit_cache_reason;
extern u32 rom_translation_cache_size;
extern u32 ram_translation_cache_size;
extern unsigned char *rom_translation_cache;
void dynarec_release_translation_caches(void);   /* cpu_threaded.c */
/* XS_HEAP builds only (a core flag the frontend cannot see): weak, NULL
 * without it.  cpu_threaded.c */
void dynarec_heap_tables_release(void) __attribute__((weak));
void heap_trace_mark(void);                       /* heap_trace.c */
void gbcore_prepare(void);                        /* gbcore/gbcore.h */
void heap_trace_report(const char *where);

/* Compiled into every build: the in-process return to the game list checks
 * the heap with these before it trusts the unload (heap_snap below). */
/* THE HEAP BUDGET, MEASURED.  `mem_free` (sceKernelTotalFreeMemSize) does not
 * answer "how much can the frontend still malloc": newlib's heap is ONE
 * partition block taken at the first malloc (all but PSP_HEAP_SIZE_KB's
 * 1 MiB), so the kernel's free figure is only what lies outside it.  This is
 * what lies inside: the free chunks newlib holds (fordblks) plus the part of
 * the heap block it has not sbrk'd yet, found by bisecting sbrk and handing
 * every probe straight back.  Under the (recursive) malloc lock, so no other
 * thread can allocate while a probe is outstanding.  `top` is the largest
 * block that is certainly allocatable: the releasable top chunk plus the
 * un-sbrk'd tail. */
static void heap_budget(unsigned *total, unsigned *top)
{
   struct mallinfo mi;
   unsigned lo = 0, hi = 64u << 20;

   __malloc_lock(_REENT);
   mi = mallinfo();
   while (hi - lo > 4096u)
   {
      unsigned mid = (lo + (hi - lo) / 2u) & ~4095u;
      if (mid <= lo)
         break;
      if (sbrk((ptrdiff_t)mid) != (void *)-1)
      {
         sbrk(-(ptrdiff_t)mid);
         lo = mid;
      }
      else
         hi = mid;
   }
   __malloc_unlock(_REENT);
   *total = (unsigned)mi.fordblks + lo;
   *top   = (unsigned)mi.keepcost + lo;
}

