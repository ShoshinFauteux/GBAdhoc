
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
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 384)
#else
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 10)
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 512)
#endif

/* RUNTIME_JIT_CACHE (PSP): the sizes above are the SMALL tier, always present
 * as static arrays.  A console whose heap can afford it gets these instead,
 * allocated at startup (cpu_threaded.c, dynarec_select_translation_caches).
 * They are the sizes the old BIG_JIT=1 build compiled in. */
#if defined(RUNTIME_JIT_CACHE)
  #define ROM_TRANSLATION_CACHE_SIZE_LARGE (1024 * 1024 * 10)
  #define RAM_TRANSLATION_CACHE_SIZE_LARGE (1024 * 512)
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
