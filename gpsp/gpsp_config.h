
#ifndef GPSP_CONFIG_H
#define GPSP_CONFIG_H

#define GPSP_NAME                "gpSP"
#define GPSP_VERSION             "v1.1.0"
#define GPSP_NETPACKET_VERSION   "gpSP v1.0"

/* Default ROM buffer size in megabytes (this is a maximum value!) */
#ifndef ROM_BUFFER_SIZE
#define ROM_BUFFER_SIZE 32
#endif

/* Cache sizes and their config knobs */
#if defined(SMALL_TRANSLATION_CACHE)
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 2)
  /* ONE RAM code cache size on every tier and on both cores of a link
   * session: two copies of the same machine then fill -- and, with
   * ENTRY_GATES, gate -- at the same moment whatever tier each one got.
   * 384 KiB, not 512: this array is static in both link cores, and instance
   * B's state is padded to whole MiB blocks (psp/gbalink_core_b.ld) with
   * under 128 KiB to spare, so 512 here cost a PSP-1000 1.2 MiB of image. */
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 384)
#else
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 10)
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 512)
#endif

/* RAM_CACHE_KB (twin sizing study only, never in a PSP build): override the
 * RAM code cache size, e.g. -DRAM_CACHE_KB=768. */
#ifdef RAM_CACHE_KB
  #undef RAM_TRANSLATION_CACHE_SIZE
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * RAM_CACHE_KB)
#endif

/* RUNTIME_JIT_CACHE (PSP): the sizes above are the SMALL tier, always present
 * as static arrays.  A console whose heap can afford it gets these instead,
 * allocated at startup (cpu_threaded.c, dynarec_select_translation_caches).
 * They are the sizes the old BIG_JIT=1 build compiled in. */
#if defined(RUNTIME_JIT_CACHE)
  #define ROM_TRANSLATION_CACHE_SIZE_LARGE (1024 * 1024 * 10)
  #define RAM_TRANSLATION_CACHE_SIZE_LARGE (1024 * 384)   /* = the SMALL tier's, see above */
#endif

/* SMC_REUSE's spare region (cpu_threaded.c, contract point 5): the RAM code
 * cache is SMC_REUSE_SPARE bytes LARGER than the size the guest-visible
 * accounting uses, and dormant code plus records may never occupy more than
 * that spare.  So the real cache can never fill before the virtual one, and
 * every cache-full flush happens at exactly the block it would without
 * SMC_REUSE.  The accounting size is the one above, on every tier. */
#ifdef SMC_REUSE
  #ifndef SMC_REUSE_SPARE_KB
    #define SMC_REUSE_SPARE_KB 128
  #endif
  #define SMC_REUSE_SPARE (1024 * SMC_REUSE_SPARE_KB)
  /* (spelled out: a macro naming RAM_TRANSLATION_CACHE_SIZE would expand to
   * the redefinition below) */
  #if defined(RAM_CACHE_KB)
    #define SMC_REUSE_VIRT_SIZE (1024 * RAM_CACHE_KB)
  #elif defined(SMALL_TRANSLATION_CACHE)
    #define SMC_REUSE_VIRT_SIZE (1024 * 384)
  #else
    #define SMC_REUSE_VIRT_SIZE (1024 * 512)
  #endif
  #if RAM_TRANSLATION_CACHE_SIZE != SMC_REUSE_VIRT_SIZE
    #error "SMC_REUSE_VIRT_SIZE must spell the RAM cache size above"
  #endif
  #undef RAM_TRANSLATION_CACHE_SIZE
  #define RAM_TRANSLATION_CACHE_SIZE (SMC_REUSE_VIRT_SIZE + SMC_REUSE_SPARE)
  #if defined(RUNTIME_JIT_CACHE)
    #if RAM_TRANSLATION_CACHE_SIZE_LARGE != SMC_REUSE_VIRT_SIZE
      #error "SMC_REUSE: the tiers must share one accounting size"
    #endif
    #undef RAM_TRANSLATION_CACHE_SIZE_LARGE
    #define RAM_TRANSLATION_CACHE_SIZE_LARGE RAM_TRANSLATION_CACHE_SIZE
  #endif
#endif

/* LAYOUT_PIN (PSP, OPT-IN: defined only by `make LAYOUT_PIN=1`, off by
 * default; docs/LAYOUT-PINNING.md): where the translation caches sit
 * in the Allegrex instruction cache.  It is 16 KiB, 2-way, with 64 B lines
 * (measured; docs/CACHE-MAP.md), so a line's set is address bits 6..12 and
 * only (address mod 8 KiB) decides what it competes with.  The emitter writes
 * its hottest code -- the memory-access stubs every guest load and store goes
 * through -- at the START of the ROM cache (mips_emit.h init_emitter), so the
 * cache's base decides whether those stubs fight the dispatcher and
 * update_gba.  Unpinned, the base was wherever the heap (LARGE tier) or .bss
 * (SMALL tier) happened to put it, and moved with every unrelated change.
 * Pinned, both tiers start at JIT_PIN_OFFSET mod JIT_PIN_WAY: the sets the
 * pinned static hot path (psp/layout/hot.ord) leaves free, chosen by the
 * twin's cache simulation.  Costs at most JIT_PIN_WAY + JIT_PIN_OFFSET bytes
 * of alignment per tier. */
#if defined(LAYOUT_PIN)
  #define JIT_PIN_WAY 8192
  #ifndef JIT_PIN_OFFSET
  #define JIT_PIN_OFFSET 0x1500
  #endif
#endif

/* Should be an upperbound to the maximum number of bytes a single JIT'ed
   instruction can take. STM/LDM are tipically the biggest ones */
#define TRANSLATION_CACHE_LIMIT_THRESHOLD (1024 * 2)

/* Hash table size for ROM trans cache lookups */
#define ROM_BRANCH_HASH_BITS                           16
#define ROM_BRANCH_HASH_SIZE   (1 << ROM_BRANCH_HASH_BITS)

/* RFU Multiplayer config, do not mess around too much with it */
#define MAX_RFU_NETPLAYERS       32

/* Serial modes (multiplayer serial). */
#define MAX_SERMULT_NETPLAYERS    4

#endif
