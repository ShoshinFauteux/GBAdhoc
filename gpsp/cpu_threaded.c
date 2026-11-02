/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

// Not-so-important todo:
// - stm reglist writeback when base is in the list needs adjustment
// - block memory needs psr swapping and user mode reg swapping

#include "common.h"
#include "drprof.h"   /* DRPROF_TWIN hooks; no-ops otherwise */
#ifdef SMC_RETIRE_WINDOW
static u32 smc_max_extent[2];   /* see ramtag_note_extent */
/* The short window is exact only if EVERY live RAM block had its extent
 * noted (ramtag_note_extent) since the last full RAM flush.  Extents are only
 * noted while smc_partial_active, so the window is ARMED by a full RAM flush
 * taken while partial retirement is active (activation always takes one) and
 * disarmed whenever partial retirement is switched off.  Disarmed = the
 * original MAX_BLOCK_SIZE*4 walk. */
static u32 smc_window_armed;
/* Blocks found live and overlapping the write in [lo-4KiB, lo-span): must
 * stay 0.  Counted by the SMC_RETIRE_WINDOW_CHECK build (and the twin). */
u32 smc_window_violations;
#ifdef SMC_RETIRE_LOCAL
/* SMC_RETIRE_LOCAL (slow-titles 2026-10-27): SMC_RETIRE_WINDOW's bound, per
 * 64-byte chunk instead of per region.  smc_back[r][c] is the largest
 * distance from chunk c's first byte back to the start of any block noted
 * (ramtag_note_extent) since the last full RAM flush whose extent reaches
 * into chunk c (0 when every such block starts inside the chunk).  A live
 * block covering a byte of chunk c therefore starts at or after
 * (c << 6) - smc_back[r][c]: the retire walks start there instead of one
 * longest-block-seen below the write.  Same armed condition as the window
 * (every live block noted since the flush that reset the table).  Values only
 * grow until that flush, so a retired block's entry stays a safe over-bound.
 * Payback (twin): smc_retire_span walked ~7k instructions per trap through a
 * window sized by a 0x22c-byte unrolled loop elsewhere in IWRAM. */
#define SMC_LOCAL_SHIFT 6
#ifdef XS_HEAP
static u16 *smc_back_iw, *smc_back_ew;      /* dynarec_heap_tables_init */
#else
static u16 smc_back_iw[0x8000 >> SMC_LOCAL_SHIFT];
static u16 smc_back_ew[0x40000 >> SMC_LOCAL_SHIFT];
#endif
static u32 smc_back_lo[2] = { ~0u, ~0u }, smc_back_hi[2];   /* dirty chunks */
static inline u16 *smc_back_tab(u32 rgn)
{
  return rgn ? smc_back_ew : smc_back_iw;
}
/* [start_off, end_off): canonical offsets of a block just noted */
static void smc_back_note(u32 rgn, u32 start_off, u32 end_off)
{
  u16 *t = smc_back_tab(rgn);
  u32 c = start_off >> SMC_LOCAL_SHIFT, ce = (end_off - 1) >> SMC_LOCAL_SHIFT;
  if (c < smc_back_lo[rgn]) smc_back_lo[rgn] = c;
  if (ce > smc_back_hi[rgn]) smc_back_hi[rgn] = ce;
#ifdef SMC_RETIRE_LOCAL_MUTANT
  if (ce > c) ce--;   /* MUTANT (oracle self-test only): last chunk not noted */
#endif
  for (c++; c <= ce; c++) {
    u32 d = (c << SMC_LOCAL_SHIFT) - start_off;
    if (d > t[c])
      t[c] = (u16)d;
  }
}
static void smc_back_reset(void)
{
  u32 r;
  for (r = 0; r < 2; r++) {
    if (smc_back_lo[r] <= smc_back_hi[r])
      memset(smc_back_tab(r) + smc_back_lo[r], 0,
             (smc_back_hi[r] - smc_back_lo[r] + 1) * sizeof(u16));
    smc_back_lo[r] = ~0u;
    smc_back_hi[r] = 0;
  }
}
/* Lowest halfword index at which a noted block overlapping the byte range
 * [lo_off, hi_off) can start. */
static u32 smc_back_first(u32 rgn, u32 lo_off, u32 hi_off)
{
  const u16 *t = smc_back_tab(rgn);
  u32 c = lo_off >> SMC_LOCAL_SHIFT, ce = (hi_off - 1) >> SMC_LOCAL_SHIFT;
  u32 lb = lo_off & ~((1u << SMC_LOCAL_SHIFT) - 1);
  for (; c <= ce; c++) {
    u32 b = (c << SMC_LOCAL_SHIFT) - t[c];
    if (b < lb)
      lb = b;
  }
  return lb >> 1;
}
#ifdef SMC_RETIRE_LOCAL_CHECK
/* DIAGNOSTIC: nothing live in [first, local) may overlap [lo_off, hi_off). */
u32 smc_local_violations;
static void smc_local_check(const u16 *tagp, u32 base, u32 first, u32 local,
                            u32 lo_off, u32 hi_off);
#endif
/* Raise `first` (a halfword index) to the chunk bound when the window is
 * armed (smc_back is exact only then). */
#ifdef SMC_RETIRE_LOCAL_CHECK
#define SMC_LOCAL_CHECK(t, b, f, l, lo, hi) smc_local_check(t, b, f, l, lo, hi)
#else
#define SMC_LOCAL_CHECK(t, b, f, l, lo, hi) do { } while (0)
#endif
/* LAYOUT NEUTRALITY (cpu-levers cpl2w): one out-of-line call, so the range
 * path inlined into the pinned flush_translation_cache_ram_smc grows by the
 * call alone.  Returns the (possibly raised) first halfword index. */
static __attribute__((noinline)) u32 smc_local_narrow(u32 first, u32 rgn,
    u32 lo_off, u32 hi_off, const u16 *tagp, u32 base)
{
  if (smc_window_armed) {
    u32 local_ = smc_back_first(rgn, lo_off, hi_off);
    SMC_LOCAL_CHECK(tagp, base, first, local_, lo_off, hi_off);
    if (local_ > first)
      first = local_;
  }
  return first;
}
#define SMC_LOCAL_NARROW(first, rgn, lo_off, hi_off, tagp, base)              \
  ((first) = smc_local_narrow((first), (rgn), (lo_off), (hi_off), (tagp), (base)))
/* With SMC_SAME_ADAPT the per-block note and the flush-time reset ride the
 * existing out-of-line helpers xs_extent_note and xs_clear_all, so
 * ramtag_note_extent and flush_translation_cache_ram keep their size. */
#if defined(SMC_SAME_ADAPT) && defined(SMC_SAME_SKIP) && \
    defined(SMC_PARTIAL_SINGLE)
#define SMC_LOCAL_COLD 1
#endif
#else
#define SMC_LOCAL_NARROW(first, rgn, lo_off, hi_off, tagp, base) do { } while (0)
#endif
#else
#define SMC_LOCAL_NARROW(first, rgn, lo_off, hi_off, tagp, base) do { } while (0)
#endif
#include "gpsp_profile.h"   /* named build profiles + illegal-flag rejection */
#if defined(VITA)
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#elif defined(PS2)
#include <kernel.h>
#endif
#if defined(RUNTIME_JIT_CACHE)
#include <malloc.h>   /* memalign */
#include <stdint.h>   /* uintptr_t */
#endif

u8 *last_rom_translation_ptr = NULL;
u8 *last_ram_translation_ptr = NULL;

#if defined(MMAP_JIT_CACHE)
u8* rom_translation_cache;
u8* ram_translation_cache;
u8 *rom_translation_ptr;
u8 *ram_translation_ptr;
#elif defined(VITA)
u8* rom_translation_cache;
u8* ram_translation_cache;
u8 *rom_translation_ptr;
u8 *ram_translation_ptr;
int sceBlock;
#elif defined(_3DS) 
u8* rom_translation_cache_ptr;
u8* ram_translation_cache_ptr;
u8 *rom_translation_ptr = rom_translation_cache;
u8 *ram_translation_ptr = ram_translation_cache;
#elif defined(RUNTIME_JIT_CACHE)
/* The SMALL tier lives in the stub file's .bss exactly where the fixed-size
 * arrays used to; these pointers start there and move only if startup
 * allocates the LARGE tier (dynarec_select_translation_caches, below). */
extern u8 rom_translation_cache_static[ROM_TRANSLATION_CACHE_SIZE];
extern u8 ram_translation_cache_static[RAM_TRANSLATION_CACHE_SIZE];
u8 *rom_translation_cache = rom_translation_cache_static;
u8 *ram_translation_cache = ram_translation_cache_static;
u32 rom_translation_cache_size = ROM_TRANSLATION_CACHE_SIZE;
u32 ram_translation_cache_size = RAM_TRANSLATION_CACHE_SIZE;
u8 *rom_translation_ptr = rom_translation_cache_static;
u8 *ram_translation_ptr = ram_translation_cache_static;
#else
u8 *rom_translation_ptr = rom_translation_cache;
u8 *ram_translation_ptr = ram_translation_cache;
#endif
/* Note, see stub files for more cache definitions */

u32 iwram_code_min = ~0U;
u32 iwram_code_max =  0U;
u32 ewram_code_min = ~0U;
u32 ewram_code_max =  0U;

#define INITIAL_ROM_WATERMARK   16   // To avoid NULL aliasing
u32 rom_cache_watermark = INITIAL_ROM_WATERMARK;
#ifdef IRQ_IO_FAST
/* IRQ_IO_FAST=1 (build) + irq_io_fast (run time, read when the emitter
 * builds its stubs: set it before the game loads; harness `irq_io_fast = 1`).
 * mips/mips_emit.h, the region-4 store stub.  Default 0: the stub is byte
 * for byte the one it always was. */
#ifndef IRQ_IO_FAST_DEFAULT
#define IRQ_IO_FAST_DEFAULT 0
#endif
u32 irq_io_fast = IRQ_IO_FAST_DEFAULT;
#endif
#ifdef MSR_FAST
/* MSR_FAST=1 (build) + msr_fast (run time: config.ini / harness key
 * msr_fast): CPSR writes that switch mode, and SPSR restores, bank the
 * registers in mips/mips_stub.S (msr_fast_store) instead of the C call.
 * Read on every switch; 0 = the C path every build before it ran. */
#ifndef MSR_FAST_DEFAULT
#define MSR_FAST_DEFAULT 1
#endif
u32 msr_fast = MSR_FAST_DEFAULT;
#endif

u8 *bios_swi_entrypoint = NULL;

// Contains an offset table to rom_translation cache area
// It features a chaining linked list for collisions
// The rom area has a small header section that contains:
//  - PC value for the entry
//  - Offset to the next entry (if any)
typedef struct
{
  u32 pc_value;
  u32 next_entry;
} hashhdr_type;

u32 rom_branch_hash[ROM_BRANCH_HASH_SIZE];

typedef struct
{
  u8 *block_offset;
  u16 flag_data;
  u8 condition;
  u8 update_cycles;
} block_data_type;

typedef struct
{
  u32 branch_target;
  u8 *branch_source;
} block_exit_type;

// Div (6) and DivArm (7)
#define is_div_swi(swinum) (((swinum) & 0xFE) == 0x06)

#define arm_decode_data_proc_reg(opcode)                                      \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_data_proc_imm(opcode)                                      \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 imm = opcode & 0xFF;                                                    \
  u32 imm_ror = ((opcode >> 8) & 0x0F) * 2                                    \

#define arm_decode_psr_reg(opcode)                                            \
  u32 psr_pfield = ((opcode >> 16) & 1) | ((opcode >> 18) & 2);               \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_psr_imm(opcode)                                            \
  u32 psr_pfield = ((opcode >> 16) & 1) | ((opcode >> 18) & 2);               \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 imm = opcode & 0xFF;                                                    \
  u32 imm_ror = ((opcode >> 8) & 0x0F) * 2                                    \

#define arm_decode_branchx(opcode)                                            \
  u32 rn = opcode & 0x0F                                                      \

#define arm_decode_multiply()                                                 \
  u32 rd = (opcode >> 16) & 0x0F;                                             \
  u32 rn = (opcode >> 12) & 0x0F;                                             \
  u32 rs = (opcode >> 8) & 0x0F;                                              \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_multiply_long()                                            \
  u32 rdhi = (opcode >> 16) & 0x0F;                                           \
  u32 rdlo = (opcode >> 12) & 0x0F;                                           \
  u32 rs = (opcode >> 8) & 0x0F;                                              \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_swap()                                                     \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_half_trans_r()                                             \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_half_trans_of()                                            \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 offset = ((opcode >> 4) & 0xF0) | (opcode & 0x0F)                       \

#define arm_decode_data_trans_imm()                                           \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 offset = opcode & 0x0FFF                                                \

#define arm_decode_data_trans_reg()                                           \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_block_trans()                                              \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 reg_list = opcode & 0xFFFF                                              \

#define arm_decode_branch()                                                   \
  s32 offset = ((s32)(opcode & 0xFFFFFF) << 8) >> 6                           \

#define thumb_decode_shift()                                                  \
  u32 imm = (opcode >> 6) & 0x1F;                                             \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_add_sub()                                                \
  u32 rn = (opcode >> 6) & 0x07;                                              \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_add_sub_imm()                                            \
  u32 imm = (opcode >> 6) & 0x07;                                             \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_imm()                                                    \
  u32 imm = opcode & 0xFF                                                     \

#define thumb_decode_alu_op()                                                 \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_hireg_op()                                               \
  u32 rs = (opcode >> 3) & 0x0F;                                              \
  u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07)                           \

#define thumb_decode_mem_reg()                                                \
  u32 ro = (opcode >> 6) & 0x07;                                              \
  u32 rb = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_mem_imm()                                                \
  u32 imm = (opcode >> 6) & 0x1F;                                             \
  u32 rb = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_add_sp()                                                 \
  u32 imm = opcode & 0x7F                                                     \

#define thumb_decode_rlist()                                                  \
  u32 reg_list = opcode & 0xFF                                                \

#define thumb_decode_branch_cond()                                            \
  s32 offset = (s8)(opcode & 0xFF)                                            \

#define thumb_decode_branch()                                                 \
  u32 offset = opcode & 0x07FF                                                \

/* Include the right emitter headers */
#if defined(MIPS_ARCH)
  #include "mips/mips_emit.h"
#elif defined(ARM_ARCH)
  #include "arm/arm_emit.h"
#elif defined(ARM64_ARCH)
  #include "arm/arm64_emit.h"
#else
  #include "x86/x86_emit.h"
#endif

/* JIT_CODE_DISCIPLINE (docs/JIT-CODE-DISCIPLINE.md): zones, one writer API,
 * one publish point.  Every macro it adds expands to the original code when
 * the switch is off. */
#include "jit_code.h"

/* Only the MIPS emitter needs a larger private entry for SMC retirement.
 * Other emitters use the ordinary block prologue for RAM and ROM blocks. */
#ifndef ram_block_prologue_size
#define ram_block_prologue_size block_prologue_size
#endif

/* Cache invalidation */

#ifdef JIT_SYNC_STATS
/* Twin measurement of the ORIGINAL sync path (docs/JIT-CODE-DISCIPLINE.md):
 * how often each kind of sync runs.  rom_pub is also the count of whole
 * I-cache invalidates the 8b48b48 fix performs on a PSP. */
struct { u32 rom_pub, ram_pub, calls, lines; } jit_legacy_stats;
#define JIT_LEGACY_SYNC_NOTE(b, e) do {                                        \
  jit_legacy_stats.calls++;                                                   \
  jit_legacy_stats.lines += (u32)((((uintptr_t)(e) + 63) & ~(uintptr_t)63) -  \
                                  ((uintptr_t)(b) & ~(uintptr_t)63)) >> 6;    \
} while (0)
#else
#define JIT_LEGACY_SYNC_NOTE(b, e) do { } while (0)
#endif

#if defined(JITCOH_PSP_CACHE)
  /* DRPROF TWIN ONLY (tools/jitcoh, docs/JIT-COHERENCY.md).  The twin compiles
   * the PSP's cache maintenance below UNCHANGED, against four out-of-line
   * functions that carry the PSP kernel's names and arguments
   * (tools/drprof/dr_host.c).  qemu is coherent and needs none of them; each
   * posts its call to a mailbox that the coherency checker plugin watches,
   * and the plugin applies the Allegrex semantics to its cache model -- so
   * every maintenance call the PSP makes is seen one-to-one, with the same
   * arguments, in the same order.  Never defined in a PSP build. */
  void sceKernelDcacheWritebackRange(const void *p, unsigned int size);
  void sceKernelIcacheInvalidateRange(const void *p, unsigned int size);
  void sceKernelDcacheWritebackInvalidateAll(void);
  void sceKernelIcacheInvalidateAll(void);
#endif

#if defined(PSP) || defined(JITCOH_PSP_CACHE)
  /* DIAGNOSTIC (harness `cache_paranoid = 1`): every sync writes back and
   * invalidates the WHOLE data cache and invalidates the WHOLE instruction
   * cache, instead of the range.  It removes every way a ranged sync can
   * miss a line (unaligned ends, a patch outside the synced range), so a
   * hardware-only divergence that goes away with it is a coherency hole.
   * Emulation is unchanged; only host time is spent. */
  u32 platform_cache_paranoid;
#ifdef GBA_LINK
  /* GBA link mirror hang study (docs/GBA-LINK-MIRROR.md section 13): how
   * often this core hands the kernel a range that takes the firmware's
   * large-range path (docs/JIT-COHERENCY.md section 4: an I invalidate of
   * 16 KiB or more walks the tag RAM instead of the exact line loop; a D
   * writeback switches at 64 KiB).  Counters only; nothing changes what is
   * synced.  Each core instance has its own copy (gbab_* in instance B). */
  u32 jit_big_i_calls, jit_big_i_max, jit_big_d_calls, jit_big_d_max;
  static inline void jit_big_note(u32 n, int dside)
  {
    if (dside) {
      if (n >= 65536u) { jit_big_d_calls++; if (n > jit_big_d_max) jit_big_d_max = n; }
    } else if (n >= 16384u) {
      jit_big_i_calls++;
      if (n > jit_big_i_max) jit_big_i_max = n;
    }
  }
#define JIT_BIG_NOTE(n, d) jit_big_note((u32)(n), (d))
#else
#define JIT_BIG_NOTE(n, d) do { } while (0)
#endif
  void platform_cache_sync(void *baseaddr, void *endptr) {
    if (platform_cache_paranoid) {
      sceKernelDcacheWritebackInvalidateAll();
      sceKernelIcacheInvalidateAll();
      return;
    }
    JIT_BIG_NOTE(((char*)endptr) - ((char*)baseaddr), 1);
    JIT_BIG_NOTE(((char*)endptr) - ((char*)baseaddr), 0);
    sceKernelDcacheWritebackRange(baseaddr, ((char*)endptr) - ((char*)baseaddr));
    sceKernelIcacheInvalidateRange(baseaddr, ((char*)endptr) - ((char*)baseaddr));
  }
#endif

#if defined(PSP)
  /* DIAGNOSTIC (harness `jit_coherency_scan = N`): count the words of live
   * RAM-cache code whose CACHED view differs from main memory.  The CPU fetches
   * instructions from memory (through the I-cache), never from the D-cache, so
   * a word that differs here is code the emitter wrote that instruction fetch
   * cannot see yet -- a missing writeback.  The RAM cache holds only code
   * below ram_translation_ptr (its tag table is at the far end), so between
   * frames every mismatch is a real hole.  Reads through the uncached alias;
   * never writes.  `first` receives the first mismatching host address. */
  u32 jit_coherency_scan(u32 *first)
  {
    const u32 *p   = (const u32 *)ram_translation_cache;
    const u32 *end = (const u32 *)ram_translation_ptr;
    u32 n = 0;
    for (; p < end; p++) {
      const volatile u32 *mem =
        (const volatile u32 *)(0x40000000u | (u32)p);
      if (*mem != *p) {
        if (!n && first)
          *first = (u32)p;
        n++;
      }
    }
    return n;
  }
#elif defined(JITCOH_PSP_CACHE)
  /* platform_cache_sync is the PSP one, above */
#elif defined(PS2)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    FlushCache(0);   // Dcache flush
    FlushCache(2);   // Icache invalidate
  }
#elif defined(VITA)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    sceKernelSyncVMDomain(sceBlock, baseaddr, ((char*)endptr) - ((char*)baseaddr) + 64);
  }
#elif defined(_3DS)
  #include "3ds/3ds_utils.h"
  void platform_cache_sync(void *baseaddr, void *endptr) {
    ctr_flush_invalidate_cache();
  }
#elif defined(ARM_ARCH) || defined(ARM64_ARCH)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    __clear_cache(baseaddr, endptr);
  }
#elif defined(MIPS_ARCH)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    JIT_LEGACY_SYNC_NOTE(baseaddr, endptr);
    __builtin___clear_cache(baseaddr, endptr);
  }
#else
  /* x86 CPUs have icache consistency checks */
  void platform_cache_sync(void *baseaddr, void *endptr) {}
#endif

#ifndef JIT_CODE_DISCIPLINE
/* The ARM lookup's normal success path keeps the RANGED sync.  The bisection
 * (docs/RESIDENT-ICACHE-FIX.md) cured the derail with a whole I-cache
 * invalidate at the OUT-OF-LINE copy only -- the Thumb/dual lookups,
 * init_bios_hooks and badjump_recover -- while the copy inlined into this path
 * (site E) did not matter; and a whole invalidate on every ARM publish costs
 * 18% on AW2 in the candidate.  So this path keeps the old behaviour.
 * (Under JIT_CODE_DISCIPLINE the publish point owns this decision.) */
static void translate_icache_sync_ranged(void) {
    if (last_rom_translation_ptr < rom_translation_ptr) {
        platform_cache_sync(last_rom_translation_ptr, rom_translation_ptr);
        last_rom_translation_ptr = rom_translation_ptr;
    }
    if (last_ram_translation_ptr < ram_translation_ptr) {
        platform_cache_sync(last_ram_translation_ptr, ram_translation_ptr);
        last_ram_translation_ptr = ram_translation_ptr;
    }
}
#endif

#if defined(JIT_CODE_DISCIPLINE) && defined(JIT_CODE_AB)
/* JIT_CODE_AB legacy modes: the original translate_icache_sync, verbatim
 * apart from the mode test (and counted, so the rig can compare). */
static void jit_legacy_icache_sync(void)
{
    if (last_rom_translation_ptr < rom_translation_ptr) {
        jit_code_stats.publishes++;
#if defined(PSP) || defined(JITCOH_PSP_CACHE)
        if (jit_code_mode == JCM_ROMWHOLE) {
            /* 8b48b48, docs/RESIDENT-ICACHE-FIX.md */
            sceKernelDcacheWritebackRange(last_rom_translation_ptr,
              (char *)rom_translation_ptr - (char *)last_rom_translation_ptr);
            sceKernelIcacheInvalidateAll();
            jit_code_stats.whole_i++;
        } else
#endif
        platform_cache_sync(last_rom_translation_ptr, rom_translation_ptr);
        last_rom_translation_ptr = rom_translation_ptr;
    }
    if (last_ram_translation_ptr < ram_translation_ptr) {
        jit_code_stats.publishes++;
        platform_cache_sync(last_ram_translation_ptr, ram_translation_ptr);
        last_ram_translation_ptr = ram_translation_ptr;
    }
}
#endif

void translate_icache_sync() {
#ifdef JIT_CODE_DISCIPLINE
#ifdef JIT_CODE_AB
    if (jit_code_mode < JCM_DISCIPLINE) {
      jit_legacy_icache_sync();
      return;
    }
#endif
    /* The one publish point: every recorded write, ROM and RAM zone alike,
     * plus a whole I-cache invalidate if code memory changed owner. */
    jit_code_publish();
#else
#ifdef JIT_SYNC_STATS
    if (last_rom_translation_ptr < rom_translation_ptr) jit_legacy_stats.rom_pub++;
    if (last_ram_translation_ptr < ram_translation_ptr) jit_legacy_stats.ram_pub++;
#endif
    // Cache emitted code can only grow
    if (last_rom_translation_ptr < rom_translation_ptr) {
#if defined(PSP) || defined(JITCOH_PSP_CACHE)
        /* NEW ROM CODE INVALIDATES THE WHOLE I-CACHE, not just its own range.
         *
         * The resident-ROM crash (Heart & Soul dies the frame after a state
         * load; BADJUMP_SAFE catches the derail and soft-resets the GBA) was
         * bisected on the EXACT failing binary by one-instruction patches of
         * its nine inlined sync sites, so the layout the bug needs never moved
         * (docs/RESIDENT-ICACHE-FIX.md, 2026-10-01, PSP Go + PSP-3000):
         *   - whole D+I at this site (ROM publish, out-of-line copy): fixed;
         *     every other single site: still dies;
         *   - this site's ranged D writeback + WHOLE I invalidate: fixed;
         *     ranged I invalidate + WHOLE D writeback: dies;
         *   - the same ranges rounded out to whole 64-byte lines: dies.
         * So instruction fetch runs stale I-cache lines that lie OUTSIDE the
         * range just written, and nothing data-side is missing.  The ranged
         * invalidate cannot reach them; a whole invalidate (256 line ops, a
         * few microseconds) can, and ROM publishes are rare once a game is
         * warm.  The D side stays ranged: the new bytes are all that need to
         * reach memory. */
        JIT_BIG_NOTE((char *)rom_translation_ptr - (char *)last_rom_translation_ptr, 1);
        sceKernelDcacheWritebackRange(last_rom_translation_ptr,
          (char *)rom_translation_ptr - (char *)last_rom_translation_ptr);
        sceKernelIcacheInvalidateAll();
#else
        platform_cache_sync(last_rom_translation_ptr, rom_translation_ptr);
#endif
        last_rom_translation_ptr = rom_translation_ptr;
    }
#ifdef GBA_LINK
    /* The first publish can come before any RAM flush set this (the BIOS
     * hooks are translated when the dynarec is enabled, inside
     * retro_load_game).  On the SMALL tier nothing else sets it, so the RAM
     * half used to hand the kernel [0, ram_translation_ptr): measured in the
     * mirror's instance B (forced SMALL), one 151,144,832-byte I invalidate
     * and D writeback at boot (docs/GBA-LINK-MIRROR.md section 13,
     * docs/JIT-COHERENCY.md section 6).  The cache's own start is the
     * correct low end.  Link builds only; release is the owner's call. */
    if (!last_ram_translation_ptr)
        last_ram_translation_ptr = ram_translation_cache;
#endif
    if (last_ram_translation_ptr < ram_translation_ptr) {
        platform_cache_sync(last_ram_translation_ptr, ram_translation_ptr);
        last_ram_translation_ptr = ram_translation_ptr;
    }
#endif
}

/* End of Cache invalidation */


#define check_pc_region(pc)                                                   \
  new_pc_region = (pc >> 15);                                                 \
  if(new_pc_region != pc_region)                                              \
  {                                                                           \
    pc_region = new_pc_region;                                                \
    pc_address_block = memory_map_read[new_pc_region];                        \
                                                                              \
    if(!pc_address_block)                                                     \
      pc_address_block = load_gamepak_page(pc_region & 0x3FF);                \
  }                                                                           \

#define translate_arm_instruction()                                           \
  check_pc_region(pc);                                                        \
  opcode = readaddress32(pc_address_block, (pc & 0x7FFF));                    \
  condition = block_data[block_data_position].condition;                      \
                                                                              \
  if((condition != last_condition) || (condition >= 0x20))                    \
  {                                                                           \
    if((last_condition & 0x0F) != 0x0E)                                       \
    {                                                                         \
      generate_branch_patch_conditional(backpatch_address, translation_ptr);  \
    }                                                                         \
                                                                              \
    last_condition = condition;                                               \
                                                                              \
    condition &= 0x0F;                                                        \
                                                                              \
    if(condition != 0x0E)                                                     \
    {                                                                         \
      arm_conditional_block_header();                                         \
    }                                                                         \
  }                                                                           \
  emit_trace_arm_instruction(pc);                                             \
                                                                              \
  switch((opcode >> 20) & 0xFF)                                               \
  {                                                                           \
    case 0x00:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], -rm */                                            \
          arm_access_memory(store, down, post, u16, half_reg);                \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* MUL rd, rm, rs */                                                \
          arm_multiply(no, no);                                               \
          cycle_count += 2;  /* variable 1..4, pick 2 as an aprox. */         \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* AND rd, rn, reg_op */                                              \
        arm_data_proc(and, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x01:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* MULS rd, rm, rs */                                             \
            arm_multiply(no, yes);                                            \
            cycle_count += 2;  /* variable 1..4, pick 2 as an aprox. */       \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], -rm */                                          \
            arm_access_memory(load, down, post, u16, half_reg);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s8, half_reg);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s16, half_reg);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ANDS rd, rn, reg_op */                                             \
        arm_data_proc(ands, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x02:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], -rm */                                            \
          arm_access_memory(store, down, post, u16, half_reg);                \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* MLA rd, rm, rs, rn */                                            \
          arm_multiply(yes, no);                                              \
          cycle_count += 3;  /* variable 2..5, pick 3 as an aprox. */         \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* EOR rd, rn, reg_op */                                              \
        arm_data_proc(eor, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x03:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* MLAS rd, rm, rs, rn */                                         \
            arm_multiply(yes, yes);                                           \
            cycle_count += 3;  /* variable 2..5, pick 3 as an aprox. */       \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], -rm */                                          \
            arm_access_memory(load, down, post, u16, half_reg);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s8, half_reg);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s16, half_reg);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* EORS rd, rn, reg_op */                                             \
        arm_data_proc(eors, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x04:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn], -imm */                                             \
        arm_access_memory(store, down, post, u16, half_imm);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SUB rd, rn, reg_op */                                              \
        arm_data_proc(sub, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x05:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn], -imm */                                         \
            arm_access_memory(load, down, post, u16, half_imm);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s8, half_imm);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s16, half_imm);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SUBS rd, rn, reg_op */                                             \
        arm_data_proc(subs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x06:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn], -imm */                                             \
        arm_access_memory(store, down, post, u16, half_imm);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSB rd, rn, reg_op */                                              \
        arm_data_proc(rsb, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x07:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn], -imm */                                         \
            arm_access_memory(load, down, post, u16, half_imm);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s8, half_imm);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s16, half_imm);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSBS rd, rn, reg_op */                                             \
        arm_data_proc(rsbs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x08:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +rm */                                            \
          arm_access_memory(store, up, post, u16, half_reg);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* UMULL rd, rm, rs */                                              \
          arm_multiply_long(u64, no, no);                                     \
          cycle_count += 3;  /* this is an aproximation :P */                 \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADD rd, rn, reg_op */                                              \
        arm_data_proc(add, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x09:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* UMULLS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(u64, no, yes);                                  \
            cycle_count += 3;  /* this is an aproximation :P */               \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +rm */                                          \
            arm_access_memory(load, up, post, u16, half_reg);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s8, half_reg);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s16, half_reg);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADDS rd, rn, reg_op */                                             \
        arm_data_proc(adds, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0A:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +rm */                                            \
          arm_access_memory(store, up, post, u16, half_reg);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* UMLAL rd, rm, rs */                                              \
          arm_multiply_long(u64_add, yes, no);                                \
          cycle_count += 3;  /* Between 2 and 5 cycles? */                    \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADC rd, rn, reg_op */                                              \
        arm_data_proc(adc, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0B:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* UMLALS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(u64_add, yes, yes);                             \
            cycle_count += 3;  /* Between 2 and 5 cycles? */                  \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +rm */                                          \
            arm_access_memory(load, up, post, u16, half_reg);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s8, half_reg);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s16, half_reg);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADCS rd, rn, reg_op */                                             \
        arm_data_proc(adcs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0C:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +imm */                                           \
          arm_access_memory(store, up, post, u16, half_imm);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SMULL rd, rm, rs */                                              \
          arm_multiply_long(s64, no, no);                                     \
          cycle_count += 2;  /* Between 1 and 4 cycles? */                    \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SBC rd, rn, reg_op */                                              \
        arm_data_proc(sbc, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0D:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* SMULLS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(s64, no, yes);                                  \
            cycle_count += 2;  /* Between 1 and 4 cycles? */                  \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +imm */                                         \
            arm_access_memory(load, up, post, u16, half_imm);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s8, half_imm);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s16, half_imm);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SBCS rd, rn, reg_op */                                             \
        arm_data_proc(sbcs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0E:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +imm */                                           \
          arm_access_memory(store, up, post, u16, half_imm);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SMLAL rd, rm, rs */                                              \
          arm_multiply_long(s64_add, yes, no);                                \
          cycle_count += 3;  /* Between 2 and 5 cycles? */                    \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSC rd, rn, reg_op */                                              \
        arm_data_proc(rsc, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0F:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* SMLALS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(s64_add, yes, yes);                             \
            cycle_count += 3;  /* Between 2 and 5 cycles? */                  \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +imm */                                         \
            arm_access_memory(load, up, post, u16, half_imm);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s8, half_imm);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s16, half_imm);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSCS rd, rn, reg_op */                                             \
        arm_data_proc(rscs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x10:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn - rm] */                                            \
          arm_access_memory(store, down, pre, u16, half_reg);                 \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SWP rd, rm, [rn] */                                              \
          arm_swap(u32);                                                      \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MRS rd, cpsr */                                                    \
        arm_psr(reg, read, cpsr);                                             \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x11:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - rm] */                                          \
            arm_access_memory(load, down, pre, u16, half_reg);                \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - rm] */                                         \
            arm_access_memory(load, down, pre, s8, half_reg);                 \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - rm] */                                         \
            arm_access_memory(load, down, pre, s16, half_reg);                \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* TST rd, rn, reg_op */                                              \
        arm_data_proc_test(tst, reg_flags);                                   \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x12:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn - rm]! */                                             \
        arm_access_memory(store, down, pre_wb, u16, half_reg);                \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        if(opcode & 0x10)                                                     \
        {                                                                     \
          /* BX rn */                                                         \
          arm_bx();                                                           \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* MSR cpsr, rm */                                                  \
          arm_psr(reg, store, cpsr);                                          \
        }                                                                     \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x13:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - rm]! */                                         \
            arm_access_memory(load, down, pre_wb, u16, half_reg);             \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - rm]! */                                        \
            arm_access_memory(load, down, pre_wb, s8, half_reg);              \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - rm]! */                                        \
            arm_access_memory(load, down, pre_wb, s16, half_reg);             \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* TEQ rd, rn, reg_op */                                              \
        arm_data_proc_test(teq, reg_flags);                                   \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x14:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn - imm] */                                           \
          arm_access_memory(store, down, pre, u16, half_imm);                 \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SWPB rd, rm, [rn] */                                             \
          arm_swap(u8);                                                       \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MRS rd, spsr */                                                    \
        arm_psr(reg, read, spsr);                                             \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x15:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - imm] */                                         \
            arm_access_memory(load, down, pre, u16, half_imm);                \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - imm] */                                        \
            arm_access_memory(load, down, pre, s8, half_imm);                 \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - imm] */                                        \
            arm_access_memory(load, down, pre, s16, half_imm);                \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* CMP rn, reg_op */                                                  \
        arm_data_proc_test(cmp, reg);                                         \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x16:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn - imm]! */                                            \
        arm_access_memory(store, down, pre_wb, u16, half_imm);                \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MSR spsr, rm */                                                    \
        arm_psr(reg, store, spsr);                                            \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x17:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - imm]! */                                        \
            arm_access_memory(load, down, pre_wb, u16, half_imm);             \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - imm]! */                                       \
            arm_access_memory(load, down, pre_wb, s8, half_imm);              \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - imm]! */                                       \
            arm_access_memory(load, down, pre_wb, s16, half_imm);             \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* CMN rd, rn, reg_op */                                              \
        arm_data_proc_test(cmn, reg);                                         \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x18:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + rm] */                                              \
        arm_access_memory(store, up, pre, u16, half_reg);                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ORR rd, rn, reg_op */                                              \
        arm_data_proc(orr, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x19:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + rm] */                                          \
            arm_access_memory(load, up, pre, u16, half_reg);                  \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + rm] */                                         \
            arm_access_memory(load, up, pre, s8, half_reg);                   \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + rm] */                                         \
            arm_access_memory(load, up, pre, s16, half_reg);                  \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ORRS rd, rn, reg_op */                                             \
        arm_data_proc(orrs, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1A:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + rm]! */                                             \
        arm_access_memory(store, up, pre_wb, u16, half_reg);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MOV rd, reg_op */                                                  \
        arm_data_proc_unary(mov, reg, no_flags);                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1B:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + rm]! */                                         \
            arm_access_memory(load, up, pre_wb, u16, half_reg);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + rm]! */                                        \
            arm_access_memory(load, up, pre_wb, s8, half_reg);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + rm]! */                                        \
            arm_access_memory(load, up, pre_wb, s16, half_reg);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MOVS rd, reg_op */                                                 \
        arm_data_proc_unary(movs, reg_flags, flags);                          \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1C:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + imm] */                                             \
        arm_access_memory(store, up, pre, u16, half_imm);                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* BIC rd, rn, reg_op */                                              \
        arm_data_proc(bic, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1D:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + imm] */                                         \
            arm_access_memory(load, up, pre, u16, half_imm);                  \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + imm] */                                        \
            arm_access_memory(load, up, pre, s8, half_imm);                   \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + imm] */                                        \
            arm_access_memory(load, up, pre, s16, half_imm);                  \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* BICS rd, rn, reg_op */                                             \
        arm_data_proc(bics, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1E:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + imm]! */                                            \
        arm_access_memory(store, up, pre_wb, u16, half_imm);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MVN rd, reg_op */                                                  \
        arm_data_proc_unary(mvn, reg, no_flags);                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1F:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + imm]! */                                        \
            arm_access_memory(load, up, pre_wb, u16, half_imm);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + imm]! */                                       \
            arm_access_memory(load, up, pre_wb, s8, half_imm);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + imm]! */                                       \
            arm_access_memory(load, up, pre_wb, s16, half_imm);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MVNS rd, rn, reg_op */                                             \
        arm_data_proc_unary(mvns, reg_flags, flags);                          \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x20:                                                                \
      /* AND rd, rn, imm */                                                   \
      arm_data_proc(and, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x21:                                                                \
      /* ANDS rd, rn, imm */                                                  \
      arm_data_proc(ands, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x22:                                                                \
      /* EOR rd, rn, imm */                                                   \
      arm_data_proc(eor, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x23:                                                                \
      /* EORS rd, rn, imm */                                                  \
      arm_data_proc(eors, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x24:                                                                \
      /* SUB rd, rn, imm */                                                   \
      arm_static_alu_pc(0, sub);                                              \
      break;                                                                  \
                                                                              \
    case 0x25:                                                                \
      /* SUBS rd, rn, imm */                                                  \
      arm_data_proc(subs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x26:                                                                \
      /* RSB rd, rn, imm */                                                   \
      arm_data_proc(rsb, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x27:                                                                \
      /* RSBS rd, rn, imm */                                                  \
      arm_data_proc(rsbs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x28:                                                                \
      /* ADD rd, rn, imm */                                                   \
      arm_static_alu_pc(1, add);                                              \
      break;                                                                  \
                                                                              \
    case 0x29:                                                                \
      /* ADDS rd, rn, imm */                                                  \
      arm_data_proc(adds, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x2A:                                                                \
      /* ADC rd, rn, imm */                                                   \
      arm_data_proc(adc, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x2B:                                                                \
      /* ADCS rd, rn, imm */                                                  \
      arm_data_proc(adcs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x2C:                                                                \
      /* SBC rd, rn, imm */                                                   \
      arm_data_proc(sbc, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x2D:                                                                \
      /* SBCS rd, rn, imm */                                                  \
      arm_data_proc(sbcs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x2E:                                                                \
      /* RSC rd, rn, imm */                                                   \
      arm_data_proc(rsc, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x2F:                                                                \
      /* RSCS rd, rn, imm */                                                  \
      arm_data_proc(rscs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x30 ... 0x31:                                                       \
      /* TST rn, imm */                                                       \
      arm_data_proc_test(tst, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x32:                                                                \
      /* MSR cpsr, imm */                                                     \
      arm_psr(imm, store, cpsr);                                              \
      break;                                                                  \
                                                                              \
    case 0x33:                                                                \
      /* TEQ rn, imm */                                                       \
      arm_data_proc_test(teq, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x34 ... 0x35:                                                       \
      /* CMP rn, imm */                                                       \
      arm_data_proc_test(cmp, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x36:                                                                \
      /* MSR spsr, imm */                                                     \
      arm_psr(imm, store, spsr);                                              \
      break;                                                                  \
                                                                              \
    case 0x37:                                                                \
      /* CMN rn, imm */                                                       \
      arm_data_proc_test(cmn, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x38:                                                                \
      /* ORR rd, rn, imm */                                                   \
      arm_data_proc(orr, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x39:                                                                \
      /* ORRS rd, rn, imm */                                                  \
      arm_data_proc(orrs, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x3A:                                                                \
      /* MOV rd, imm */                                                       \
      arm_data_proc_unary(mov, imm, no_flags);                                \
      break;                                                                  \
                                                                              \
    case 0x3B:                                                                \
      /* MOVS rd, imm */                                                      \
      arm_data_proc_unary(movs, imm_flags, flags);                            \
      break;                                                                  \
                                                                              \
    case 0x3C:                                                                \
      /* BIC rd, rn, imm */                                                   \
      arm_data_proc(bic, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x3D:                                                                \
      /* BICS rd, rn, imm */                                                  \
      arm_data_proc(bics, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x3E:                                                                \
      /* MVN rd, imm */                                                       \
      arm_data_proc_unary(mvn, imm, no_flags);                                \
      break;                                                                  \
                                                                              \
    case 0x3F:                                                                \
      /* MVNS rd, imm */                                                      \
      arm_data_proc_unary(mvns, imm_flags, flags);                            \
      break;                                                                  \
                                                                              \
    case 0x40:                                                                \
      /* STR rd, [rn], -imm */                                                \
      arm_access_memory(store, down, post, u32, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x41:                                                                \
      /* LDR rd, [rn], -imm */                                                \
      arm_access_memory(load, down, post, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x42:                                                                \
      /* STRT rd, [rn], -imm */                                               \
      arm_access_memory(store, down, post, u32, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x43:                                                                \
      /* LDRT rd, [rn], -imm */                                               \
      arm_access_memory(load, down, post, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x44:                                                                \
      /* STRB rd, [rn], -imm */                                               \
      arm_access_memory(store, down, post, u8, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x45:                                                                \
      /* LDRB rd, [rn], -imm */                                               \
      arm_access_memory(load, down, post, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x46:                                                                \
      /* STRBT rd, [rn], -imm */                                              \
      arm_access_memory(store, down, post, u8, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x47:                                                                \
      /* LDRBT rd, [rn], -imm */                                              \
      arm_access_memory(load, down, post, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x48:                                                                \
      /* STR rd, [rn], +imm */                                                \
      arm_access_memory(store, up, post, u32, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x49:                                                                \
      /* LDR rd, [rn], +imm */                                                \
      arm_access_memory(load, up, post, u32, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4A:                                                                \
      /* STRT rd, [rn], +imm */                                               \
      arm_access_memory(store, up, post, u32, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x4B:                                                                \
      /* LDRT rd, [rn], +imm */                                               \
      arm_access_memory(load, up, post, u32, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4C:                                                                \
      /* STRB rd, [rn], +imm */                                               \
      arm_access_memory(store, up, post, u8, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4D:                                                                \
      /* LDRB rd, [rn], +imm */                                               \
      arm_access_memory(load, up, post, u8, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x4E:                                                                \
      /* STRBT rd, [rn], +imm */                                              \
      arm_access_memory(store, up, post, u8, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4F:                                                                \
      /* LDRBT rd, [rn], +imm */                                              \
      arm_access_memory(load, up, post, u8, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x50:                                                                \
      /* STR rd, [rn - imm] */                                                \
      arm_access_memory(store, down, pre, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x51:                                                                \
      /* LDR rd, [rn - imm] */                                                \
      arm_access_memory(load, down, pre, u32, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x52:                                                                \
      /* STR rd, [rn - imm]! */                                               \
      arm_access_memory(store, down, pre_wb, u32, imm);                       \
      break;                                                                  \
                                                                              \
    case 0x53:                                                                \
      /* LDR rd, [rn - imm]! */                                               \
      arm_access_memory(load, down, pre_wb, u32, imm);                        \
      break;                                                                  \
                                                                              \
    case 0x54:                                                                \
      /* STRB rd, [rn - imm] */                                               \
      arm_access_memory(store, down, pre, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x55:                                                                \
      /* LDRB rd, [rn - imm] */                                               \
      arm_access_memory(load, down, pre, u8, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x56:                                                                \
      /* STRB rd, [rn - imm]! */                                              \
      arm_access_memory(store, down, pre_wb, u8, imm);                        \
      break;                                                                  \
                                                                              \
    case 0x57:                                                                \
      /* LDRB rd, [rn - imm]! */                                              \
      arm_access_memory(load, down, pre_wb, u8, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x58:                                                                \
      /* STR rd, [rn + imm] */                                                \
      arm_access_memory(store, up, pre, u32, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x59:                                                                \
      /* LDR rd, [rn + imm] */                                                \
      arm_access_memory(load, up, pre, u32, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x5A:                                                                \
      /* STR rd, [rn + imm]! */                                               \
      arm_access_memory(store, up, pre_wb, u32, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x5B:                                                                \
      /* LDR rd, [rn + imm]! */                                               \
      arm_access_memory(load, up, pre_wb, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x5C:                                                                \
      /* STRB rd, [rn + imm] */                                               \
      arm_access_memory(store, up, pre, u8, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x5D:                                                                \
      /* LDRB rd, [rn + imm] */                                               \
      arm_access_memory(load, up, pre, u8, imm);                              \
      break;                                                                  \
                                                                              \
    case 0x5E:                                                                \
      /* STRB rd, [rn + imm]! */                                              \
      arm_access_memory(store, up, pre_wb, u8, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x5F:                                                                \
      /* LDRBT rd, [rn + imm]! */                                             \
      arm_access_memory(load, up, pre_wb, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x60:                                                                \
      /* STR rd, [rn], -rm */                                                 \
      arm_access_memory(store, down, post, u32, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x61:                                                                \
      /* LDR rd, [rn], -rm */                                                 \
      arm_access_memory(load, down, post, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x62:                                                                \
      /* STRT rd, [rn], -rm */                                                \
      arm_access_memory(store, down, post, u32, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x63:                                                                \
      /* LDRT rd, [rn], -rm */                                                \
      arm_access_memory(load, down, post, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x64:                                                                \
      /* STRB rd, [rn], -rm */                                                \
      arm_access_memory(store, down, post, u8, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x65:                                                                \
      /* LDRB rd, [rn], -rm */                                                \
      arm_access_memory(load, down, post, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x66:                                                                \
      /* STRBT rd, [rn], -rm */                                               \
      arm_access_memory(store, down, post, u8, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x67:                                                                \
      /* LDRBT rd, [rn], -rm */                                               \
      arm_access_memory(load, down, post, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x68:                                                                \
      /* STR rd, [rn], +rm */                                                 \
      arm_access_memory(store, up, post, u32, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x69:                                                                \
      /* LDR rd, [rn], +rm */                                                 \
      arm_access_memory(load, up, post, u32, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6A:                                                                \
      /* STRT rd, [rn], +rm */                                                \
      arm_access_memory(store, up, post, u32, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x6B:                                                                \
      /* LDRT rd, [rn], +rm */                                                \
      arm_access_memory(load, up, post, u32, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6C:                                                                \
      /* STRB rd, [rn], +rm */                                                \
      arm_access_memory(store, up, post, u8, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6D:                                                                \
      /* LDRB rd, [rn], +rm */                                                \
      arm_access_memory(load, up, post, u8, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x6E:                                                                \
      /* STRBT rd, [rn], +rm */                                               \
      arm_access_memory(store, up, post, u8, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6F:                                                                \
      /* LDRBT rd, [rn], +rm */                                               \
      arm_access_memory(load, up, post, u8, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x70:                                                                \
      /* STR rd, [rn - rm] */                                                 \
      arm_access_memory(store, down, pre, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x71:                                                                \
      /* LDR rd, [rn - rm] */                                                 \
      arm_access_memory(load, down, pre, u32, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x72:                                                                \
      /* STR rd, [rn - rm]! */                                                \
      arm_access_memory(store, down, pre_wb, u32, reg);                       \
      break;                                                                  \
                                                                              \
    case 0x73:                                                                \
      /* LDR rd, [rn - rm]! */                                                \
      arm_access_memory(load, down, pre_wb, u32, reg);                        \
      break;                                                                  \
                                                                              \
    case 0x74:                                                                \
      /* STRB rd, [rn - rm] */                                                \
      arm_access_memory(store, down, pre, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x75:                                                                \
      /* LDRB rd, [rn - rm] */                                                \
      arm_access_memory(load, down, pre, u8, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x76:                                                                \
      /* STRB rd, [rn - rm]! */                                               \
      arm_access_memory(store, down, pre_wb, u8, reg);                        \
      break;                                                                  \
                                                                              \
    case 0x77:                                                                \
      /* LDRB rd, [rn - rm]! */                                               \
      arm_access_memory(load, down, pre_wb, u8, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x78:                                                                \
      /* STR rd, [rn + rm] */                                                 \
      arm_access_memory(store, up, pre, u32, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x79:                                                                \
      /* LDR rd, [rn + rm] */                                                 \
      arm_access_memory(load, up, pre, u32, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x7A:                                                                \
      /* STR rd, [rn + rm]! */                                                \
      arm_access_memory(store, up, pre_wb, u32, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x7B:                                                                \
      /* LDR rd, [rn + rm]! */                                                \
      arm_access_memory(load, up, pre_wb, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x7C:                                                                \
      /* STRB rd, [rn + rm] */                                                \
      arm_access_memory(store, up, pre, u8, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x7D:                                                                \
      /* LDRB rd, [rn + rm] */                                                \
      arm_access_memory(load, up, pre, u8, reg);                              \
      break;                                                                  \
                                                                              \
    case 0x7E:                                                                \
      /* STRB rd, [rn + rm]! */                                               \
      arm_access_memory(store, up, pre_wb, u8, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x7F:                                                                \
      /* LDRBT rd, [rn + rm]! */                                              \
      arm_access_memory(load, up, pre_wb, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x80:                                                                \
      /* STMDA rn, rlist */                                                   \
      arm_block_memory(store, down_a, no, no);                                \
      break;                                                                  \
                                                                              \
    case 0x81:                                                                \
      /* LDMDA rn, rlist */                                                   \
      arm_block_memory(load, down_a, no, no);                                 \
      break;                                                                  \
                                                                              \
    case 0x82:                                                                \
      /* STMDA rn!, rlist */                                                  \
      arm_block_memory(store, down_a, down, no);                              \
      break;                                                                  \
                                                                              \
    case 0x83:                                                                \
      /* LDMDA rn!, rlist */                                                  \
      arm_block_memory(load, down_a, down, no);                               \
      break;                                                                  \
                                                                              \
    case 0x84:                                                                \
      /* STMDA rn, rlist^ */                                                  \
      arm_block_memory(store, down_a, no, yes);                               \
      break;                                                                  \
                                                                              \
    case 0x85:                                                                \
      /* LDMDA rn, rlist^ */                                                  \
      arm_block_memory(load, down_a, no, yes);                                \
      break;                                                                  \
                                                                              \
    case 0x86:                                                                \
      /* STMDA rn!, rlist^ */                                                 \
      arm_block_memory(store, down_a, down, yes);                             \
      break;                                                                  \
                                                                              \
    case 0x87:                                                                \
      /* LDMDA rn!, rlist^ */                                                 \
      arm_block_memory(load, down_a, down, yes);                              \
      break;                                                                  \
                                                                              \
    case 0x88:                                                                \
      /* STMIA rn, rlist */                                                   \
      arm_block_memory(store, no, no, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x89:                                                                \
      /* LDMIA rn, rlist */                                                   \
      arm_block_memory(load, no, no, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x8A:                                                                \
      /* STMIA rn!, rlist */                                                  \
      arm_block_memory(store, no, up, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x8B:                                                                \
      /* LDMIA rn!, rlist */                                                  \
      arm_block_memory(load, no, up, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x8C:                                                                \
      /* STMIA rn, rlist^ */                                                  \
      arm_block_memory(store, no, no, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x8D:                                                                \
      /* LDMIA rn, rlist^ */                                                  \
      arm_block_memory(load, no, no, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0x8E:                                                                \
      /* STMIA rn!, rlist^ */                                                 \
      arm_block_memory(store, no, up, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x8F:                                                                \
      /* LDMIA rn!, rlist^ */                                                 \
      arm_block_memory(load, no, up, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0x90:                                                                \
      /* STMDB rn, rlist */                                                   \
      arm_block_memory(store, down_b, no, no);                                \
      break;                                                                  \
                                                                              \
    case 0x91:                                                                \
      /* LDMDB rn, rlist */                                                   \
      arm_block_memory(load, down_b, no, no);                                 \
      break;                                                                  \
                                                                              \
    case 0x92:                                                                \
      /* STMDB rn!, rlist */                                                  \
      arm_block_memory(store, down_b, down, no);                              \
      break;                                                                  \
                                                                              \
    case 0x93:                                                                \
      /* LDMDB rn!, rlist */                                                  \
      arm_block_memory(load, down_b, down, no);                               \
      break;                                                                  \
                                                                              \
    case 0x94:                                                                \
      /* STMDB rn, rlist^ */                                                  \
      arm_block_memory(store, down_b, no, yes);                               \
      break;                                                                  \
                                                                              \
    case 0x95:                                                                \
      /* LDMDB rn, rlist^ */                                                  \
      arm_block_memory(load, down_b, no, yes);                                \
      break;                                                                  \
                                                                              \
    case 0x96:                                                                \
      /* STMDB rn!, rlist^ */                                                 \
      arm_block_memory(store, down_b, down, yes);                             \
      break;                                                                  \
                                                                              \
    case 0x97:                                                                \
      /* LDMDB rn!, rlist^ */                                                 \
      arm_block_memory(load, down_b, down, yes);                              \
      break;                                                                  \
                                                                              \
    case 0x98:                                                                \
      /* STMIB rn, rlist */                                                   \
      arm_block_memory(store, up, no, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x99:                                                                \
      /* LDMIB rn, rlist */                                                   \
      arm_block_memory(load, up, no, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x9A:                                                                \
      /* STMIB rn!, rlist */                                                  \
      arm_block_memory(store, up, up, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x9B:                                                                \
      /* LDMIB rn!, rlist */                                                  \
      arm_block_memory(load, up, up, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x9C:                                                                \
      /* STMIB rn, rlist^ */                                                  \
      arm_block_memory(store, up, no, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x9D:                                                                \
      /* LDMIB rn, rlist^ */                                                  \
      arm_block_memory(load, up, no, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0x9E:                                                                \
      /* STMIB rn!, rlist^ */                                                 \
      arm_block_memory(store, up, up, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x9F:                                                                \
      /* LDMIB rn!, rlist^ */                                                 \
      arm_block_memory(load, up, up, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0xA0 ... 0xAF:                                                       \
    {                                                                         \
      /* B offset */                                                          \
      arm_b();                                                                \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xB0 ... 0xBF:                                                       \
    {                                                                         \
      /* BL offset */                                                         \
      arm_bl();                                                               \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xF0 ... 0xFF:                                                       \
    {                                                                         \
      u32 swinum = (opcode >> 16) & 0xFF;                                     \
      if (swinum == 6) {                                                      \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div(arm);                                                     \
      }                                                                       \
      else if (swinum == 7) {                                                 \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div_arm(arm);                                                 \
      }                                                                       \
      else {                                                                  \
        arm_swi();                                                            \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
  }                                                                           \
                                                                              \
  pc += 4                                                                     \

#define arm_flag_status()                                                     \

/* See the (low word) BL case in translate_thumb_instruction.  Only the MIPS
 * emitter is built and verified in this tree; other back ends keep upstream
 * behaviour. */
#if defined(MIPS_ARCH) && !defined(NO_BL_FIX)
#define thumb_bl_low_alone(lr_value) {                                        \
  generate_load_imm(reg_a0, (lr_value));                                      \
  generate_store_reg(reg_a0, REG_LR);                                         \
}
#else
#define thumb_bl_low_alone(lr_value) do { (void)(lr_value); } while (0)
#endif

#define translate_thumb_instruction()                                         \
  flag_status = block_data[block_data_position].flag_data;                    \
  check_pc_region(pc);                                                        \
  last_opcode = opcode;                                                       \
  opcode = readaddress16(pc_address_block, (pc & 0x7FFF));                    \
  emit_trace_thumb_instruction(pc);                                           \
  u8 hiop = opcode >> 8;                                                      \
                                                                              \
  switch(hiop)                                                                \
  {                                                                           \
    case 0x00 ... 0x07:                                                       \
      /* LSL rd, rs, imm */                                                   \
      thumb_shift(shift, lsl, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x08 ... 0x0F:                                                       \
      /* LSR rd, rs, imm */                                                   \
      thumb_shift(shift, lsr, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x10 ... 0x17:                                                       \
      /* ASR rd, rs, imm */                                                   \
      thumb_shift(shift, asr, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x18 ... 0x19:                                                       \
      /* ADD rd, rs, rn */                                                    \
      thumb_data_proc(add_sub, adds, reg, rd, rs, rn);                        \
      break;                                                                  \
                                                                              \
    case 0x1A ... 0x1B:                                                       \
      /* SUB rd, rs, rn */                                                    \
      thumb_data_proc(add_sub, subs, reg, rd, rs, rn);                        \
      break;                                                                  \
                                                                              \
    case 0x1C ... 0x1D:                                                       \
      /* ADD rd, rs, imm */                                                   \
      thumb_data_proc(add_sub_imm, adds, imm, rd, rs, imm);                   \
      break;                                                                  \
                                                                              \
    case 0x1E ... 0x1F:                                                       \
      /* SUB rd, rs, imm */                                                   \
      thumb_data_proc(add_sub_imm, subs, imm, rd, rs, imm);                   \
      break;                                                                  \
                                                                              \
    /* MOV r0..7, imm */                                                      \
    case 0x20: thumb_data_proc_unary(imm, movs, imm, 0, imm); break;          \
    case 0x21: thumb_data_proc_unary(imm, movs, imm, 1, imm); break;          \
    case 0x22: thumb_data_proc_unary(imm, movs, imm, 2, imm); break;          \
    case 0x23: thumb_data_proc_unary(imm, movs, imm, 3, imm); break;          \
    case 0x24: thumb_data_proc_unary(imm, movs, imm, 4, imm); break;          \
    case 0x25: thumb_data_proc_unary(imm, movs, imm, 5, imm); break;          \
    case 0x26: thumb_data_proc_unary(imm, movs, imm, 6, imm); break;          \
    case 0x27: thumb_data_proc_unary(imm, movs, imm, 7, imm); break;          \
                                                                              \
    /* CMP r0, imm */                                                         \
    case 0x28: thumb_data_proc_test(imm, cmp, imm, 0, imm); break;            \
    case 0x29: thumb_data_proc_test(imm, cmp, imm, 1, imm); break;            \
    case 0x2A: thumb_data_proc_test(imm, cmp, imm, 2, imm); break;            \
    case 0x2B: thumb_data_proc_test(imm, cmp, imm, 3, imm); break;            \
    case 0x2C: thumb_data_proc_test(imm, cmp, imm, 4, imm); break;            \
    case 0x2D: thumb_data_proc_test(imm, cmp, imm, 5, imm); break;            \
    case 0x2E: thumb_data_proc_test(imm, cmp, imm, 6, imm); break;            \
    case 0x2F: thumb_data_proc_test(imm, cmp, imm, 7, imm); break;            \
                                                                              \
    /* ADD r0..7, imm */                                                      \
    case 0x30: thumb_data_proc(imm, adds, imm, 0, 0, imm); break;             \
    case 0x31: thumb_data_proc(imm, adds, imm, 1, 1, imm); break;             \
    case 0x32: thumb_data_proc(imm, adds, imm, 2, 2, imm); break;             \
    case 0x33: thumb_data_proc(imm, adds, imm, 3, 3, imm); break;             \
    case 0x34: thumb_data_proc(imm, adds, imm, 4, 4, imm); break;             \
    case 0x35: thumb_data_proc(imm, adds, imm, 5, 5, imm); break;             \
    case 0x36: thumb_data_proc(imm, adds, imm, 6, 6, imm); break;             \
    case 0x37: thumb_data_proc(imm, adds, imm, 7, 7, imm); break;             \
                                                                              \
    /* SUB r0..7, imm */                                                      \
    case 0x38: thumb_data_proc(imm, subs, imm, 0, 0, imm); break;             \
    case 0x39: thumb_data_proc(imm, subs, imm, 1, 1, imm); break;             \
    case 0x3A: thumb_data_proc(imm, subs, imm, 2, 2, imm); break;             \
    case 0x3B: thumb_data_proc(imm, subs, imm, 3, 3, imm); break;             \
    case 0x3C: thumb_data_proc(imm, subs, imm, 4, 4, imm); break;             \
    case 0x3D: thumb_data_proc(imm, subs, imm, 5, 5, imm); break;             \
    case 0x3E: thumb_data_proc(imm, subs, imm, 6, 6, imm); break;             \
    case 0x3F: thumb_data_proc(imm, subs, imm, 7, 7, imm); break;             \
                                                                              \
    case 0x40:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* AND rd, rs */                                                    \
          thumb_data_proc(alu_op, ands, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* EOR rd, rs */                                                    \
          thumb_data_proc(alu_op, eors, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* LSL rd, rs */                                                    \
          thumb_shift(alu_op, lsl, reg);                                      \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* LSR rd, rs */                                                    \
          thumb_shift(alu_op, lsr, reg);                                      \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x41:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* ASR rd, rs */                                                    \
          thumb_shift(alu_op, asr, reg);                                      \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* ADC rd, rs */                                                    \
          thumb_data_proc(alu_op, adcs, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* SBC rd, rs */                                                    \
          thumb_data_proc(alu_op, sbcs, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* ROR rd, rs */                                                    \
          thumb_shift(alu_op, ror, reg);                                      \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x42:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* TST rd, rs */                                                    \
          thumb_data_proc_test(alu_op, tst, reg, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* NEG rd, rs */                                                    \
          thumb_data_proc_unary(alu_op, neg, reg, rd, rs);                    \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* CMP rd, rs */                                                    \
          thumb_data_proc_test(alu_op, cmp, reg, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* CMN rd, rs */                                                    \
          thumb_data_proc_test(alu_op, cmn, reg, rd, rs);                     \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x43:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* ORR rd, rs */                                                    \
          thumb_data_proc(alu_op, orrs, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* MUL rd, rs */                                                    \
          thumb_data_proc(alu_op, muls, reg, rd, rs, rd);                     \
          cycle_count += 2;  /* Between 1 and 4 extra cycles */               \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* BIC rd, rs */                                                    \
          thumb_data_proc(alu_op, bics, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* MVN rd, rs */                                                    \
          thumb_data_proc_unary(alu_op, mvns, reg, rd, rs);                   \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x44:                                                                \
      /* ADD rd, rs */                                                        \
      thumb_data_proc_hi(add);                                                \
      break;                                                                  \
                                                                              \
    case 0x45:                                                                \
      /* CMP rd, rs */                                                        \
      thumb_data_proc_test_hi(cmp);                                           \
      break;                                                                  \
                                                                              \
    case 0x46:                                                                \
      /* MOV rd, rs */                                                        \
      thumb_data_proc_mov_hi();                                               \
      break;                                                                  \
                                                                              \
    case 0x47:                                                                \
      /* BX rs */                                                             \
      thumb_bx();                                                             \
      break;                                                                  \
                                                                              \
    case 0x48 ... 0x4F:                                                       \
      /* LDR r0..7, [pc + imm] */                                             \
      {                                                                       \
        thumb_decode_imm();                                                   \
        u32 rdreg = (hiop & 7);                                               \
        u32 aoff = (pc & ~2) + (imm*4) + 4;                                   \
        /* ROM + same page -> optimize as const load */                       \
        if (!ram_region && (((aoff + 4) >> 15) == (pc >> 15))) {              \
          u32 value = readaddress32(pc_address_block, (aoff & 0x7FFF));       \
          thumb_load_pc_pool_const(rdreg, value);                             \
        } else {                                                              \
          thumb_access_memory(load, imm, rdreg, 0, 0, pc_relative, aoff, u32);\
        }                                                                     \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x50 ... 0x51:                                                       \
      /* STR rd, [rb + ro] */                                                 \
      thumb_access_memory(store, mem_reg, rd, rb, ro, reg_reg, 0, u32);       \
      break;                                                                  \
                                                                              \
    case 0x52 ... 0x53:                                                       \
      /* STRH rd, [rb + ro] */                                                \
      thumb_access_memory(store, mem_reg, rd, rb, ro, reg_reg, 0, u16);       \
      break;                                                                  \
                                                                              \
    case 0x54 ... 0x55:                                                       \
      /* STRB rd, [rb + ro] */                                                \
      thumb_access_memory(store, mem_reg, rd, rb, ro, reg_reg, 0, u8);        \
      break;                                                                  \
                                                                              \
    case 0x56 ... 0x57:                                                       \
      /* LDSB rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, s8);         \
      break;                                                                  \
                                                                              \
    case 0x58 ... 0x59:                                                       \
      /* LDR rd, [rb + ro] */                                                 \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, u32);        \
      break;                                                                  \
                                                                              \
    case 0x5A ... 0x5B:                                                       \
      /* LDRH rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, u16);        \
      break;                                                                  \
                                                                              \
    case 0x5C ... 0x5D:                                                       \
      /* LDRB rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, u8);         \
      break;                                                                  \
                                                                              \
    case 0x5E ... 0x5F:                                                       \
      /* LDSH rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, s16);        \
      break;                                                                  \
                                                                              \
    case 0x60 ... 0x67:                                                       \
      /* STR rd, [rb + imm] */                                                \
      thumb_access_memory(store, mem_imm, rd, rb, 0, reg_imm, (imm * 4),      \
       u32);                                                                  \
      break;                                                                  \
                                                                              \
    case 0x68 ... 0x6F:                                                       \
      /* LDR rd, [rb + imm] */                                                \
      thumb_access_memory(load, mem_imm, rd, rb, 0, reg_imm, (imm * 4), u32); \
      break;                                                                  \
                                                                              \
    case 0x70 ... 0x77:                                                       \
      /* STRB rd, [rb + imm] */                                               \
      thumb_access_memory(store, mem_imm, rd, rb, 0, reg_imm, imm, u8);       \
      break;                                                                  \
                                                                              \
    case 0x78 ... 0x7F:                                                       \
      /* LDRB rd, [rb + imm] */                                               \
      thumb_access_memory(load, mem_imm, rd, rb, 0, reg_imm, imm, u8);        \
      break;                                                                  \
                                                                              \
    case 0x80 ... 0x87:                                                       \
      /* STRH rd, [rb + imm] */                                               \
      thumb_access_memory(store, mem_imm, rd, rb, 0, reg_imm,                 \
       (imm * 2), u16);                                                       \
      break;                                                                  \
                                                                              \
    case 0x88 ... 0x8F:                                                       \
      /* LDRH rd, [rb + imm] */                                               \
      thumb_access_memory(load, mem_imm, rd, rb, 0, reg_imm, (imm * 2), u16); \
      break;                                                                  \
                                                                              \
    /* STR r0..7, [sp + imm] */                                               \
    case 0x90:                                                                \
      thumb_access_memory(store, imm, 0, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x91:                                                                \
      thumb_access_memory(store, imm, 1, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x92:                                                                \
      thumb_access_memory(store, imm, 2, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x93:                                                                \
      thumb_access_memory(store, imm, 3, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x94:                                                                \
      thumb_access_memory(store, imm, 4, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x95:                                                                \
      thumb_access_memory(store, imm, 5, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x96:                                                                \
      thumb_access_memory(store, imm, 6, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x97:                                                                \
      thumb_access_memory(store, imm, 7, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
                                                                              \
    /* LDR r0..7, [sp + imm] */                                               \
    case 0x98:                                                                \
      thumb_access_memory(load, imm, 0, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x99:                                                                \
      thumb_access_memory(load, imm, 1, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9A:                                                                \
      thumb_access_memory(load, imm, 2, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9B:                                                                \
      thumb_access_memory(load, imm, 3, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9C:                                                                \
      thumb_access_memory(load, imm, 4, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9D:                                                                \
      thumb_access_memory(load, imm, 5, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9E:                                                                \
      thumb_access_memory(load, imm, 6, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9F:                                                                \
      thumb_access_memory(load, imm, 7, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
                                                                              \
    /* ADD r0..7, pc, +imm */                                                 \
    case 0xA0: thumb_load_pc(0); break;                                       \
    case 0xA1: thumb_load_pc(1); break;                                       \
    case 0xA2: thumb_load_pc(2); break;                                       \
    case 0xA3: thumb_load_pc(3); break;                                       \
    case 0xA4: thumb_load_pc(4); break;                                       \
    case 0xA5: thumb_load_pc(5); break;                                       \
    case 0xA6: thumb_load_pc(6); break;                                       \
    case 0xA7: thumb_load_pc(7); break;                                       \
                                                                              \
    /* ADD r0..7, sp, +imm */                                                 \
    case 0xA8: thumb_load_sp(0); break;                                       \
    case 0xA9: thumb_load_sp(1); break;                                       \
    case 0xAA: thumb_load_sp(2); break;                                       \
    case 0xAB: thumb_load_sp(3); break;                                       \
    case 0xAC: thumb_load_sp(4); break;                                       \
    case 0xAD: thumb_load_sp(5); break;                                       \
    case 0xAE: thumb_load_sp(6); break;                                       \
    case 0xAF: thumb_load_sp(7); break;                                       \
                                                                              \
    case 0xB0 ... 0xB3:                                                       \
      if((opcode >> 7) & 0x01)                                                \
      {                                                                       \
        /* ADD sp, -imm */                                                    \
        thumb_adjust_sp(down);                                                \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADD sp, +imm */                                                    \
        thumb_adjust_sp(up);                                                  \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0xB4:                                                                \
      /* PUSH rlist */                                                        \
      thumb_block_memory(store, down, no, 13);                                \
      break;                                                                  \
                                                                              \
    case 0xB5:                                                                \
      /* PUSH rlist, lr */                                                    \
      thumb_block_memory(store, push_lr, push_lr, 13);                        \
      break;                                                                  \
                                                                              \
    case 0xBC:                                                                \
      /* POP rlist */                                                         \
      thumb_block_memory(load, no, up, 13);                                   \
      break;                                                                  \
                                                                              \
    case 0xBD:                                                                \
      /* POP rlist, pc */                                                     \
      thumb_block_memory(load, no, pop_pc, 13);                               \
      break;                                                                  \
                                                                              \
    case 0xC0:                                                                \
      /* STMIA r0!, rlist */                                                  \
      thumb_block_memory(store, no, up, 0);                                   \
      break;                                                                  \
                                                                              \
    case 0xC1:                                                                \
      /* STMIA r1!, rlist */                                                  \
      thumb_block_memory(store, no, up, 1);                                   \
      break;                                                                  \
                                                                              \
    case 0xC2:                                                                \
      /* STMIA r2!, rlist */                                                  \
      thumb_block_memory(store, no, up, 2);                                   \
      break;                                                                  \
                                                                              \
    case 0xC3:                                                                \
      /* STMIA r3!, rlist */                                                  \
      thumb_block_memory(store, no, up, 3);                                   \
      break;                                                                  \
                                                                              \
    case 0xC4:                                                                \
      /* STMIA r4!, rlist */                                                  \
      thumb_block_memory(store, no, up, 4);                                   \
      break;                                                                  \
                                                                              \
    case 0xC5:                                                                \
      /* STMIA r5!, rlist */                                                  \
      thumb_block_memory(store, no, up, 5);                                   \
      break;                                                                  \
                                                                              \
    case 0xC6:                                                                \
      /* STMIA r6!, rlist */                                                  \
      thumb_block_memory(store, no, up, 6);                                   \
      break;                                                                  \
                                                                              \
    case 0xC7:                                                                \
      /* STMIA r7!, rlist */                                                  \
      thumb_block_memory(store, no, up, 7);                                   \
      break;                                                                  \
                                                                              \
    case 0xC8:                                                                \
      /* LDMIA r0!, rlist */                                                  \
      thumb_block_memory(load, no, up, 0);                                    \
      break;                                                                  \
                                                                              \
    case 0xC9:                                                                \
      /* LDMIA r1!, rlist */                                                  \
      thumb_block_memory(load, no, up, 1);                                    \
      break;                                                                  \
                                                                              \
    case 0xCA:                                                                \
      /* LDMIA r2!, rlist */                                                  \
      thumb_block_memory(load, no, up, 2);                                    \
      break;                                                                  \
                                                                              \
    case 0xCB:                                                                \
      /* LDMIA r3!, rlist */                                                  \
      thumb_block_memory(load, no, up, 3);                                    \
      break;                                                                  \
                                                                              \
    case 0xCC:                                                                \
      /* LDMIA r4!, rlist */                                                  \
      thumb_block_memory(load, no, up, 4);                                    \
      break;                                                                  \
                                                                              \
    case 0xCD:                                                                \
      /* LDMIA r5!, rlist */                                                  \
      thumb_block_memory(load, no, up, 5);                                    \
      break;                                                                  \
                                                                              \
    case 0xCE:                                                                \
      /* LDMIA r6!, rlist */                                                  \
      thumb_block_memory(load, no, up, 6);                                    \
      break;                                                                  \
                                                                              \
    case 0xCF:                                                                \
      /* LDMIA r7!, rlist */                                                  \
      thumb_block_memory(load, no, up, 7);                                    \
      break;                                                                  \
                                                                              \
    case 0xD0:                                                                \
      /* BEQ label */                                                         \
      thumb_conditional_branch(eq);                                           \
      break;                                                                  \
                                                                              \
    case 0xD1:                                                                \
      /* BNE label */                                                         \
      thumb_conditional_branch(ne);                                           \
      break;                                                                  \
                                                                              \
    case 0xD2:                                                                \
      /* BCS label */                                                         \
      thumb_conditional_branch(cs);                                           \
      break;                                                                  \
                                                                              \
    case 0xD3:                                                                \
      /* BCC label */                                                         \
      thumb_conditional_branch(cc);                                           \
      break;                                                                  \
                                                                              \
    case 0xD4:                                                                \
      /* BMI label */                                                         \
      thumb_conditional_branch(mi);                                           \
      break;                                                                  \
                                                                              \
    case 0xD5:                                                                \
      /* BPL label */                                                         \
      thumb_conditional_branch(pl);                                           \
      break;                                                                  \
                                                                              \
    case 0xD6:                                                                \
      /* BVS label */                                                         \
      thumb_conditional_branch(vs);                                           \
      break;                                                                  \
                                                                              \
    case 0xD7:                                                                \
      /* BVC label */                                                         \
      thumb_conditional_branch(vc);                                           \
      break;                                                                  \
                                                                              \
    case 0xD8:                                                                \
      /* BHI label */                                                         \
      thumb_conditional_branch(hi);                                           \
      break;                                                                  \
                                                                              \
    case 0xD9:                                                                \
      /* BLS label */                                                         \
      thumb_conditional_branch(ls);                                           \
      break;                                                                  \
                                                                              \
    case 0xDA:                                                                \
      /* BGE label */                                                         \
      thumb_conditional_branch(ge);                                           \
      break;                                                                  \
                                                                              \
    case 0xDB:                                                                \
      /* BLT label */                                                         \
      thumb_conditional_branch(lt);                                           \
      break;                                                                  \
                                                                              \
    case 0xDC:                                                                \
      /* BGT label */                                                         \
      thumb_conditional_branch(gt);                                           \
      break;                                                                  \
                                                                              \
    case 0xDD:                                                                \
      /* BLE label */                                                         \
      thumb_conditional_branch(le);                                           \
      break;                                                                  \
                                                                              \
    case 0xDF:                                                                \
    {                                                                         \
      u32 swinum = opcode & 0xFF;                                             \
      if (swinum == 6) {                                                      \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div(thumb);                                                   \
      }                                                                       \
      else if (swinum == 7) {                                                 \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div_arm(thumb);                                               \
      }                                                                       \
      else {                                                                  \
        thumb_swi();                                                          \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xE0 ... 0xE7:                                                       \
    {                                                                         \
      /* B label */                                                           \
      thumb_b();                                                              \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xF0 ... 0xF7:                                                       \
    {                                                                         \
      /* (low word) BL label */                                               \
      /* Normally fused with the high word that follows (thumb_bl).  But a    \
         translation gate at the high word -- or MAX_BLOCK_SIZE / the RAM-end \
         clamp -- ends the block right here, and the high word then starts    \
         a block of its own as thumb_blh, which computes its target from LR.  \
         So when this is the block's last instruction, perform the low        \
         word's architectural effect: LR = PC + 4 + (offset << 12).  Without  \
         it the high word branched to (stale LR + offset). */                 \
      if((pc + 2) == block_end_pc)                                            \
      {                                                                       \
        u32 bl_lr = pc + 4 + ((s32)((opcode & 0x07FF) << 21) >> 9);           \
        thumb_bl_low_alone(bl_lr);                                            \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xF8 ... 0xFF:                                                       \
    {                                                                         \
      /* (high word) BL label */                                              \
      /* This might not be preceeding a BL low word (Golden Sun 2), if so     \
         it must be handled like an indirect branch. */                       \
      if((last_opcode >= 0xF000) && (last_opcode < 0xF800))                   \
      {                                                                       \
        thumb_bl();                                                           \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        thumb_blh();                                                          \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
  }                                                                           \
                                                                              \
  pc += 2                                                                     \

#define thumb_flag_modifies_all()                                             \
  flag_status |= 0xFF                                                         \

#define thumb_flag_modifies_zn()                                              \
  flag_status |= 0xCC                                                         \

#define thumb_flag_modifies_znc()                                             \
  flag_status |= 0xEE                                                         \

#define thumb_flag_modifies_zn_maybe_c()                                      \
  flag_status |= 0xCE                                                         \

#define thumb_flag_modifies_c()                                               \
  flag_status |= 0x22                                                         \

#define thumb_flag_requires_c()                                               \
  flag_status |= 0x200                                                        \

#define thumb_flag_requires_all()                                             \
  flag_status |= 0xF00                                                        \

#define thumb_flag_status()                                                   \
{                                                                             \
  u16 flag_status = 0;                                                        \
  switch((opcode >> 8) & 0xFF)                                                \
  {                                                                           \
    /* left shift by imm */                                                   \
    case 0x00 ... 0x07:                                                       \
      thumb_flag_modifies_zn();                                               \
      if(((opcode >> 6) & 0x1F) != 0)                                         \
      {                                                                       \
        thumb_flag_modifies_c();                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    /* right shift by imm */                                                  \
    case 0x08 ... 0x17:                                                       \
      thumb_flag_modifies_znc();                                              \
      break;                                                                  \
                                                                              \
    /* add, subtract */                                                       \
    case 0x18 ... 0x1F:                                                       \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    /* mov reg, imm */                                                        \
    case 0x20 ... 0x27:                                                       \
      thumb_flag_modifies_zn();                                               \
      break;                                                                  \
                                                                              \
    /* cmp reg, imm; add, subtract */                                         \
    case 0x28 ... 0x3F:                                                       \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    case 0x40:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* AND rd, rs */                                                    \
          thumb_flag_modifies_zn();                                           \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* EOR rd, rs */                                                    \
          thumb_flag_modifies_zn();                                           \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* LSL rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* LSR rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x41:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* ASR rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* ADC rd, rs */                                                    \
          thumb_flag_modifies_all();                                          \
          thumb_flag_requires_c();                                            \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* SBC rd, rs */                                                    \
          thumb_flag_modifies_all();                                          \
          thumb_flag_requires_c();                                            \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* ROR rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    /* TST, NEG, CMP, CMN */                                                  \
    case 0x42:                                                                \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    /* ORR, MUL, BIC, MVN */                                                  \
    case 0x43:                                                                \
      thumb_flag_modifies_zn();                                               \
      break;                                                                  \
                                                                              \
    case 0x45:                                                                \
      /* CMP rd, rs */                                                        \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    /* mov might change PC (fall through if so) */                            \
    case 0x46:                                                                \
      if((opcode & 0xFF87) != 0x4687)                                         \
        break;                                                                \
                                                                              \
    /* branches (can change PC) */                                            \
    case 0x47:                                                                \
    case 0xBD:                                                                \
    case 0xD0 ... 0xE7:                                                       \
    case 0xF0 ... 0xFF:                                                       \
      thumb_flag_requires_all();                                              \
      break;                                                                  \
  }                                                                           \
  block_data[block_data_position].flag_data = flag_status;                    \
}                                                                             \

// I/EWRAM memory tagging
// Code emitted in the RAM cache has tags (16 bit values) in the mirror tag ram
// that indicate that the address contains code. The following values are used:
// 0x0000 : this is just data (never translated)
// 0x00XX : not used (since first byte is zero)
// 0x0101 : this is code that is not the start of a translated block
// 0xXXXX : this is the start of a translated block, starting from 0xFFFF downwards
//          LSB is always set (we decrement by two) to ensure both bytes != 0
//
// The tag value is an index to a `ramtag_type` structure that sits at the end
// of the RAM CACHE (grows like a stack). For simplicity we start tags at 0xFFFF
// and grow like a stack.

#define LAST_TAG_NUM       0x0101
#define INITIAL_TOP_TAG    0xFFFF
#define CODE_TAG_BLOCK16   0x0101
#define CODE_TAG_BLOCK32   0x01010101

#define VALID_TAG(tagn) (tagn > LAST_TAG_NUM)

/* (The direct-link/stable-thunk cross-check, and every other illegal SMC flag
 * combination, is now asserted together in gpsp_profile.h.) */

#ifdef SMC_PARTIAL_SAFE
#define SMC_NOTE_CODE_BYTES(te, blkptr, type)                                 \
  do { if (smc_partial_active)                                                \
    (te)->code_bytes_##type = (u32)(ram_translation_ptr - (blkptr));          \
  } while (0)
#elif defined(SMC_PARTIAL)
/* ram_translation_ptr has advanced past the block just emitted, so the size is
 * simply the distance from its entry point. */
#define SMC_NOTE_CODE_BYTES(te, blkptr, type)                                 \
  (te)->code_bytes = (u32)(ram_translation_ptr - (blkptr))
#else
#define SMC_NOTE_CODE_BYTES(te, blkptr, type)  do { } while (0)
#endif

#define allocate_tag_arm(location) {   \
  location[0] = ram_block_tag;         \
  /* Could be another thumb inst */    \
  if (!location[1])                    \
    location[1] = CODE_TAG_BLOCK16;    \
  ram_block_tag -= 2;                  \
}

#define allocate_tag_thumb(location) { \
  location[0] = ram_block_tag;         \
  ram_block_tag -= 2;                  \
}

typedef struct
{
  u32 offset_arm;     // Cache offset to the ARM-mode public entry
  u32 offset_thumb;   // Cache offset to the Thumb-mode public entry
#ifdef SMC_PARTIAL_SAFE
  /* The same guest address can have both ARM and Thumb translations. Keep
   * their source extents separate so translating one mode cannot hide an
   * overlapping block in the other mode from an SMC retirement. */
  u32 blk_end_arm;
  u32 blk_end_thumb;
  u32 code_bytes_arm;
  u32 code_bytes_thumb;
#ifdef STALE_CHECK
  u32 src_hash_arm;   /* FNV-1a of the guest bytes [start, stale_end) at */
  u32 src_hash_thumb; /* translation time; see stale_check_entry()        */
  /* The checker's OWN canonical end, recorded in every mode.  blk_end_* is
   * only written while smc_partial_active, and SMC_INIT_TAG_METADATA only
   * clears it then, so before activation (and after every state load) a
   * fresh tag inherits blk_end/hash from a previous user of the same tag
   * slot: comparing against that produced the 13,853 Mario Golf
   * "mismatches" of 2026-10-20 (smc-coverage).  These fields are cleared at
   * EVERY tag allocation and written at every translation. */
  u32 stale_end_arm;
  u32 stale_end_thumb;
#endif
#elif defined(SMC_PARTIAL)
  /* Source extent [start, end) of this block, recorded by
   * ramtag_note_extent() as soon as scan_block settles it.  Partial
   * invalidation needs it to answer "does this block actually span the
   * written address?" — without it the retire path can only guess from tag
   * adjacency, and guessing either misses overlapping blocks (v1: fast but
   * corrupted audio) or retires far too many (v2: correct but slower than
   * the full flush it replaced). */
  u32 blk_start;
  u32 blk_end;
  u32 code_bytes;   /* translated size, so the retire path knows whether an
                     * entry-point trampoline fits without clobbering the
                     * block emitted after it */
#endif
} ramtag_type;

#ifdef STALE_CHECK
/* DIAGNOSTIC (bench campaign 2026-10-20, suggested by the xlat worker): prove
 * selective SMC retirement never leaves stale code.  Every RAM block records
 * a hash of its guest source bytes when translated; every lookup that reuses
 * the translation re-hashes them.  A mismatch means the block was entered
 * after its source changed without being retired: a stale-code bug.  A
 * vhash difference with zero mismatches is timing/layout only. */
extern u32 smc_fallback_stats[16];
static u32 stale_src_hash(u32 start, u32 end)
{
  const u8 *p;
  u32 h = 2166136261u, n;
  if (start >= 0x03000000 && start < 0x04000000)
    p = iwram + 0x8000 + (start & 0x7FFF);
  else
    p = ewram + (start & 0x3FFFF);
  n = end - start;
  while (n--)
    h = (h ^ *p++) * 16777619u;
  return h;
}

static void stale_check_entry(ramtag_type *te, u32 pc, u32 thumb)
{
  static int logged;
  u32 end = thumb ? te->stale_end_thumb : te->stale_end_arm;
  u32 want = thumb ? te->src_hash_thumb : te->src_hash_arm;
  if (!end || end <= pc || end - pc > 0x4000)   /* sanity bound */
    return;
  smc_fallback_stats[10]++;
  if (stale_src_hash(pc, end) != want) {
    smc_fallback_stats[11]++;
#ifdef STALE_ABORT
    /* Twin/corpus oracle: a stale entry is a hard failure (dr_corpus kind
     * "assert"), not a log line. */
    fprintf(stderr, "STALE_ABORT pc=%08x end=%08x %s frame=%u\n",
            (unsigned)pc, (unsigned)end, thumb ? "thumb" : "arm",
            (unsigned)frame_counter);
    abort();
#endif
    if (logged < 8) {
      FILE *f = fopen("ms0:/stale.txt", logged ? "a" : "w");
      if (f) {
        fprintf(f, "STALE frame=%u pc=%08x end=%08x %s last_smc_write=%08x"
                   " partial_active=%u\n",
                (unsigned)frame_counter, (unsigned)pc, (unsigned)end,
                thumb ? "thumb" : "arm", (unsigned)smc_last_write_addr,
#ifdef SMC_PARTIAL_SAFE
                (unsigned)smc_partial_active
#else
                0u
#endif
                );
        fclose(f);
      }
      logged++;
    }
  }
}
#define STALE_CHECK_ENTRY(te, pc, thumb) stale_check_entry((te), (pc), (thumb))
#define STALE_INIT_TAG(te) do {                                               \
  (te)->stale_end_arm = (te)->stale_end_thumb = 0;                            \
} while (0)
#else
#define STALE_INIT_TAG(te) do { } while (0)
#define STALE_CHECK_ENTRY(te, pc, thumb) do { } while (0)
#endif

#ifdef SMC_PARTIAL_SAFE
#define SMC_INIT_TAG_METADATA(te) do {                                        \
  if (smc_partial_active) {                                                   \
    (te)->blk_end_arm = 0;                                                    \
    (te)->blk_end_thumb = 0;                                                  \
    (te)->code_bytes_arm = 0;                                                 \
    (te)->code_bytes_thumb = 0;                                               \
  }                                                                           \
} while (0)
#else
#define SMC_INIT_TAG_METADATA(te) do { } while (0)
#endif

static u32 ram_block_tag = INITIAL_TOP_TAG;

inline static ramtag_type* get_ram_tag(u16 tagval) {
  ramtag_type *tbl = (ramtag_type*)&ram_translation_cache[ram_translation_cache_size];
  s16 tgidx = (s16)(tagval);
  return &tbl[tgidx >> 1];  /* Since LSB is always 1 and thus unused */
}

#ifdef SMC_PARTIAL_SAFE
u32 smc_partial_active;
#ifdef SMC_BLOCK_EXACT
/* A block-store word hit code that could not be retired exactly; the next
 * SMC trap full-flushes (smc_block_word_hit).  Cleared by every full flush. */
static u32 smc_block_pending_full;
#endif

/* Selective retirement temporarily clears a hot patch domain's tag bytes so
 * the remaining stores in the same mixer patch batch do not each trap. Keep
 * its block-start tags here and restore them on the next real lookup, avoiding
 * consumption of a new tag number on every audio tick. */
#define SMC_RETIRED_TAGS 512
typedef struct { u32 addr; u16 tag; } smc_retired_tag_type;
static smc_retired_tag_type smc_retired_tags[SMC_RETIRED_TAGS];
static u32 smc_retired_tag_count;

/* Set when any live RAM block's source wraps past the end of its region's
 * canonical window (a block entered through a mirror near the top, or one that
 * starts inside the last 16 bytes and so misses the scan clamp).  Such a block
 * covers [start, end-of-region) AND [region base, ...), which a single [low,
 * high) overlap test cannot express, so while one is live every selective
 * retirement falls back to the full flush.  Cleared only by the full RAM
 * flush, which is also the only thing that kills every live block. */
static u32 smc_partial_wrap_live;

static void smc_restore_retired_tag(u32 pc, u16 *tagp, u32 thumb)
{
  u32 i;
  u32 addr = ((pc >> 24) == 3) ? 0x03000000 + (pc & 0x7FFF)
                               : 0x02000000 + (pc & 0x3FFFF);
  if (*tagp)
    return;
  for (i = 0; i < smc_retired_tag_count; i++)
    if (smc_retired_tags[i].addr == addr) {
      *tagp = smc_retired_tags[i].tag;
      if (!thumb && !tagp[1])
        tagp[1] = CODE_TAG_BLOCK16;
      return;
    }
}

static int smc_remember_retired_tag(u32 addr, u16 tag)
{
  u32 i;
  for (i = 0; i < smc_retired_tag_count; i++)
    if (smc_retired_tags[i].addr == addr) {
      smc_retired_tags[i].tag = tag;
      return 1;
    }
  if (smc_retired_tag_count == SMC_RETIRED_TAGS)
    return 0;
  smc_retired_tags[smc_retired_tag_count].addr = addr;
  smc_retired_tags[smc_retired_tag_count].tag = tag;
  smc_retired_tag_count++;
  return 1;
}

#ifdef SMC_PARTIAL_STABLE_THUNK
/* A stable thunk is the address returned to every RAM caller.  Its first two
 * words are either a direct jump to the current body or, while retired, the
 * first half of a four-instruction dispatcher path. */
static void smc_stable_thunk_link(u8 *entry, u8 *body)
{
#ifdef SMC_REUSE
  /* A revived block's thunk normally still holds exactly this jump: leave the
   * code memory (and so the I-cache) alone. */
  if (address32(entry, 0) == ((mips_opcode_j << 26) |
                              (mips_absolute_offset(body) & 0x3FFFFFF)) &&
      address32(entry, 4) == 0)
    return;
#endif
  generate_branch_patch_unconditional(entry, body);
  address32(entry, 4) = 0;
#ifdef JIT_CODE_DISCIPLINE
  /* Only ever called while a lookup translates (SMC_RAM_COMMIT_TRANSLATION):
   * that lookup's publish runs before control can reach the thunk. */
#ifndef JIT_CODE_AUDIT_NEGCTL   /* the audit's negative control drops this */
  JIT_CODE_WROTE(entry, entry + 8);
#endif
#else
  platform_cache_sync(entry, entry + 8);
#endif
}

#if defined(DISPATCH_CACHE) && defined(DISPATCH_PRECISE)
static void dispatch_cache_forget(u32 pc, u32 thumb);
#endif
static void smc_stable_thunk_dispatch(u8 *entry, u32 pc, u32 thumb)
{
  u8 *translation_ptr = entry;
#if defined(DISPATCH_CACHE) && defined(DISPATCH_PRECISE)
  /* This thunk now jumps back into the dispatcher: a cached {pc -> thunk}
   * would loop.  Every retirement and every aborted translation comes
   * through here, so this is the one place the cache must forget. */
  dispatch_cache_forget(pc, thumb);
#endif
  generate_load_imm(reg_a0, pc);
  if (thumb) {
    mips_emit_j(mips_absolute_offset(mips_indirect_branch_thumb));
  } else {
    mips_emit_j(mips_absolute_offset(mips_indirect_branch_arm));
  }
  mips_emit_nop();
#ifdef JIT_CODE_DISCIPLINE
  /* Called from a translating lookup (the abort path) or from SMC
   * retirement; both publish before control can reach the thunk. */
  JIT_CODE_WROTE(entry, translation_ptr);
#else
  platform_cache_sync(entry, translation_ptr);
#endif
}

#define SMC_RAM_NEEDS_TRANSLATION(te, type)                                   \
  (smc_partial_active ? !(te)->code_bytes_##type : !(te)->offset_##type)
#define SMC_RAM_PREPARE_TRANSLATION(te, type, body) do {                       \
  if (smc_partial_active) {                                                   \
    if (!(te)->offset_##type)                                                 \
      (te)->offset_##type = (u32)((body) - 16 - ram_translation_cache);       \
    /* Nonzero also breaks a recursive lookup cycle while this body emits. */ \
    (te)->code_bytes_##type = 1;                                              \
  } else {                                                                    \
    (te)->offset_##type = (u32)((body) - ram_translation_cache);              \
  }                                                                           \
} while (0)
#define SMC_RAM_COMMIT_TRANSLATION(te, type, body) do {                        \
  if (smc_partial_active)                                                     \
    smc_stable_thunk_link(&ram_translation_cache[(te)->offset_##type],        \
                          (body));                                             \
} while (0)
#define SMC_RAM_ABORT_TRANSLATION(te, type, pc, is_thumb) do {                 \
  if (smc_partial_active) {                                                   \
    (te)->code_bytes_##type = 0;                                              \
    smc_stable_thunk_dispatch(                                                \
      &ram_translation_cache[(te)->offset_##type], (pc), (is_thumb));         \
  }                                                                           \
} while (0)
#define SMC_RAM_PUBLIC_ENTRY(te, type)                                        \
  (&ram_translation_cache[(te)->offset_##type])
#define SMC_RAM_LIVE_OFFSET(te, type) ((te)->code_bytes_##type)
#ifdef SMC_PARTIAL_DIRECT_LINKS
#define SMC_RAM_LINK_NEEDS_DISPATCH(ram_region, target) (!(ram_region))
#else
#define SMC_RAM_LINK_NEEDS_DISPATCH(ram_region, target)                       \
  (!(ram_region) || smc_partial_target_is_gated(GBA_PC(target)))
#endif
#else
#define SMC_RAM_NEEDS_TRANSLATION(te, type) (!(te)->offset_##type)
#define SMC_RAM_PREPARE_TRANSLATION(te, type, body)                           \
  ((te)->offset_##type = (u32)((body) - ram_translation_cache))
#define SMC_RAM_COMMIT_TRANSLATION(te, type, body) do { } while (0)
#define SMC_RAM_ABORT_TRANSLATION(te, type, pc, is_thumb) do { } while (0)
#define SMC_RAM_PUBLIC_ENTRY(te, type)                                        \
  (&ram_translation_cache[(te)->offset_##type])
#define SMC_RAM_LIVE_OFFSET(te, type) ((te)->offset_##type)
#define SMC_RAM_LINK_NEEDS_DISPATCH(ram_region, target)                       \
  (!(ram_region) || smc_partial_target_is_gated(GBA_PC(target)))
#endif

#define SMC_RESTORE_RETIRED_TAG_arm(pc, tagp) \
  do { if (smc_partial_active) smc_restore_retired_tag((pc), (tagp), 0); } while (0)
#define SMC_RESTORE_RETIRED_TAG_thumb(pc, tagp) \
  do { if (smc_partial_active) smc_restore_retired_tag((pc), (tagp), 1); } while (0)
#else
#define SMC_RESTORE_RETIRED_TAG_arm(pc, tagp) do { } while (0)
#define SMC_RESTORE_RETIRED_TAG_thumb(pc, tagp) do { } while (0)
#define SMC_RAM_NEEDS_TRANSLATION(te, type) (!(te)->offset_##type)
#define SMC_RAM_PREPARE_TRANSLATION(te, type, body)                           \
  ((te)->offset_##type = (u32)((body) - ram_translation_cache))
#define SMC_RAM_COMMIT_TRANSLATION(te, type, body) do { } while (0)
#define SMC_RAM_ABORT_TRANSLATION(te, type, pc, is_thumb) do { } while (0)
#define SMC_RAM_PUBLIC_ENTRY(te, type)                                        \
  (&ram_translation_cache[(te)->offset_##type])
#endif

// This function will return a pointer to a translated block of code. If it
// doesn't exist it will translate it, if it does it will pass it back.

// type should be "arm", "thumb", or "dual." For arm or thumb the PC should
// be a real PC, for dual the least significant bit will determine if it's
// ARM or Thumb mode.

/* GBA_PC_MASK: the GBA decodes only address bits 24-27 -- GBATEK lists
 * 0x10000000-0xFFFFFFFF as "not used (upper 4bits of address bus unused)",
 * so on hardware 0x58581818 IS 0x08581818 and executes from ROM.  gpSP does
 * not mask, so such a pc misses every case of the region switch and falls
 * through to the ~0 sentinel, which nothing checks -- that is the "Bad
 * Execution Address / PC: ffffffff" crash.  It is also an out-of-bounds
 * index: memory_map_read[] is 8K entries for pc>>15 (28 bits exactly) and
 * def_seq_cycles[] is 16 entries for pc>>24.  Masking restores the hardware
 * behaviour and the array bounds in one go. */
#ifdef GBA_PC_MASK
#define GBA_PC(x) ((x) & 0x0FFFFFFFu)
#else
#define GBA_PC(x) (x)
#endif

#define block_lookup_address_pc_arm()                                         \
  u32 thumb = 0;                                                              \
  pc = GBA_PC(pc) & ~0x03

#define block_lookup_address_pc_thumb()                                       \
  u32 thumb = 1;                                                              \
  pc = GBA_PC(pc) & ~0x01                                                     \


/* What the dispatcher gets when the emulated PC is outside every region the
 * lookup handles.  Upstream returns ~0 and block_lookup_address_* passes it
 * straight through -- it only tests for NULL -- so mips_stub.S does `jr $2`
 * into 0xFFFFFFFF and the PSP dies with "Bad Execution Address".  That is
 * the crash signature we caught in PPSSPP.
 *
 * BADJUMP_REPORT keeps the fault identical (0xF_______ is just as unmapped)
 * but folds the offending 28-bit GBA pc into the address, so the crash
 * screen prints the value we actually need to diagnose it. */
/* THE VALUE block_lookup_translate_* RETURNS FOR A PC IT CANNOT TRANSLATE.
 *
 * Its switch handles regions 0x0 (BIOS), 0x2 (EWRAM), 0x3 (IWRAM) and
 * 0x8..0xD (ROM).  Anything else -- I/O, palette, VRAM, OAM, SRAM -- falls
 * through to this sentinel.  Upstream chose a non-NULL value deliberately, so
 * one garbage exit from a speculative scan does not abort an otherwise good
 * translation (see the comment at the return).  The hazard it created is that
 * (u8 *)(~0) is also a perfectly plausible-looking POINTER, and every caller
 * that only tests `if (ret)` accepts it. */
#define BLOCK_LOOKUP_UNMAPPABLE ((u8 *)(~0))

/* The same region set as a pure function of the target address.
 *
 * Needed because the translator has to classify a block exit BEFORE it commits
 * translation_ptr: at that point it can still emit a dispatch island into the
 * block's own budget, whereas by the time the eager patch loop runs the
 * pointer is published and there is nowhere left to put one. */
static inline int gba_pc_translatable(u32 pc)
{
  u32 region = GBA_PC(pc) >> 24;
  return region == 0x0 || region == 0x2 || region == 0x3 ||
#ifdef VRAM_EXEC
         region == 0x6 ||
#endif
         (region >= 0x8 && region <= 0xD);
}

/* Block exits routed through the dispatcher instead of being patched to
 * BLOCK_LOOKUP_UNMAPPABLE.  Every one of these is a `j 0x3FFFFFF` -- a jump to
 * 0x0FFFFFFC -- that a pre-fix build wrote into a live block, so the counter is
 * the measurement of the freeze exposure rather than a claim about it.  Cheap
 * enough to keep in the release build: it only moves during translation. */
u32 badjump_contained = 0;

#ifdef BADJUMP_REPORT
/* pc >> 4, not pc & 0x0FFFFFFF: the first attempt masked off the TOP nibble,
 * which is the very part that decides the region -- it printed f8581818 for
 * a pc whose region 0x8 the switch above actually handles, proving the real
 * pc had a high nibble set and that the mask had eaten it.  Shifting keeps
 * the top 28 bits and drops only the instruction-alignment nibble. */
/* The bad TARGET is already known -- 0x58581818, identically on two runs.
 * What is not known is who branched there, so report the previous block
 * entry instead.  Same fault, same timing, different field. */
extern u32 badjump_prev_pc;
void badjump_report(u32 pc);
/* One run, whole picture: dump everything interesting to ms0:/badjump.txt
 * (PPSSPP maps that to a host folder) and THEN return the sentinel, so the
 * fault is unchanged.  Diagnostic only -- BADJUMP_REPORT never ships. */
#define BADJUMP_SENTINEL(pc) (badjump_report((pc)), BLOCK_LOOKUP_UNMAPPABLE)
#else
#define BADJUMP_SENTINEL(pc) (BLOCK_LOOKUP_UNMAPPABLE)
#endif

/* XLAT_DEPTH_PROBE (soak/diagnostic only): how deep does eager exit
 * resolution nest, and how low does the main-thread stack get while it does?
 * Each level is ~380-430 B on the 256 KB PSP main stack (measured from the
 * psp-gcc prologues), and every savestate reload empties the ROM cache, so
 * the first frames after a load are the worst case.  The frontend reads and
 * resets these in its core_health line. */
#ifdef DMA_SMC_FLUSH
u32 dma_smc_flushes;   /* incremented by update_gba (main.c) */
#endif

#ifdef XLAT_DEPTH_PROBE
u32 xlat_depth_max;
u32 xlat_sp_min;
static u32 xlat_depth;
void xlat_depth_probe_reset(void)
{
  xlat_depth_max = 0;
  xlat_sp_min = 0;
}
#define XLAT_PROBE_ENTER() do {                                               \
  u32 xlat_sp_ = (u32)(uintptr_t)__builtin_frame_address(0);                  \
  if (++xlat_depth > xlat_depth_max) xlat_depth_max = xlat_depth;             \
  if (!xlat_sp_min || xlat_sp_ < xlat_sp_min) xlat_sp_min = xlat_sp_;         \
} while (0)
#define XLAT_PROBE_LEAVE() do { xlat_depth--; } while (0)
#else
#define XLAT_PROBE_ENTER() do { } while (0)
#define XLAT_PROBE_LEAVE() do { } while (0)
#endif

#ifdef VRAM_EXEC
/* VRAM_EXEC: code executing from VRAM (Blue Roses' engine -- Big Mutha
 * Truckers, Street Racing Syndicate -- copies routines to OBJ VRAM and jumps
 * there; jsmolka nes.gba too).  Without it the lookup has no case for region
 * 6 and BADJUMP_SAFE resets the GBA.
 *
 * Design (docs: compat-fix report).  Nothing can branch DIRECTLY into VRAM:
 * ARM B/BL reach +-32 MiB and Thumb BL +-4 MiB, and VRAM is further than that
 * from ROM, EWRAM and IWRAM, so every entry is an indirect branch through the
 * C dispatcher -- the only code that ever links INTO a VRAM block is another
 * VRAM block.  So VRAM code is translated like ROM code (no RAM tags, no SMC
 * gates) into the ROM translation cache, found through its own small hash,
 * and kept out of DISPATCH_CACHE (whose asm probe would skip the check
 * below).  Invalidation is by page: every VRAM store (the region-6 store stub,
 * write_vram*, DMA) clears vram_xclean[page] (gba_memory.h VRAM_X_MARK);
 * translating code in a page sets it again.  Each VRAM lookup first checks the
 * pages that hold code; if any was written since, the whole VRAM hash is
 * forgotten and the code retranslated on demand.  Old blocks stay as dead
 * space in the ROM cache (only stale VRAM blocks can still link to them) until
 * the next ROM-cache flush, which also forgets the VRAM hash. */
#define VRAM_BRANCH_HASH_BITS 8
#define VRAM_BRANCH_HASH_SIZE (1u << VRAM_BRANCH_HASH_BITS)
static u32 vram_branch_hash[VRAM_BRANCH_HASH_SIZE];
static u8  vram_code_page[VRAM_DIRTY_PAGES];
static u8  vram_code_list[VRAM_DIRTY_PAGES];
static u32 vram_code_pages;
u32 vram_exec_blocks, vram_exec_drops;   /* translations / hash drops */

static inline u32 vram_page_of(u32 pc)
{
  u32 o = pc & 0x1FFFF;
  if (o >= 0x18000)
    o -= 0x8000;
  return o >> VRAM_DIRTY_SHIFT;
}

static void vram_code_forget(void)
{
  u32 i;
  memset(vram_branch_hash, 0, sizeof(vram_branch_hash));
  for (i = 0; i < vram_code_pages; i++)
    vram_code_page[vram_code_list[i]] = 0;
  vram_code_pages = 0;
}

/* scan_block has settled [start, end): those pages now hold code. */
static void vram_code_note(u32 start, u32 end)
{
  u32 a;
  for (a = start & ~((1u << VRAM_DIRTY_SHIFT) - 1); a < end;
       a += 1u << VRAM_DIRTY_SHIFT) {
    u32 pg = vram_page_of(a);
    if (!vram_code_page[pg]) {
      vram_code_page[pg] = 1;
      vram_code_list[vram_code_pages++] = (u8)pg;
    }
    vram_xclean[pg] = 1;
  }
  vram_exec_blocks++;
}

static void vram_code_check(void)
{
  u32 i;
  for (i = 0; i < vram_code_pages; i++)
    if (!vram_xclean[vram_code_list[i]]) {
      vram_exec_drops++;
      vram_code_forget();
      return;
    }
}

/* SELF-ACTIVATION (claude/self-activate).  The region-6 store stubs are
 * built dormant (mips_emit.h vram_x_slots): until the game first jumps into
 * VRAM, a VRAM store costs exactly what it did in 4.1.1.  That first jump
 * always arrives here (every entry into VRAM is an indirect branch through
 * the C lookup -- see above -- and every region-6 translation, eager links
 * included, goes through vram_lookup_translate_*), so this is where the marks
 * are armed: the sb goes into each stub's jr delay slot and is published
 * before the lookup returns (block_lookup_address_* publish on success).
 *
 * Why nothing written before this moment can be missed: xclean only has to
 * describe pages written SINCE their code was translated, and no VRAM code
 * has been translated yet -- vram_code_page/list are empty (forgotten again
 * here; a ROM-cache flush forgets them too), and vram_code_note sets a page
 * clean only after the translator has read it, with the marks already live.
 * The C and DMA marks (VRAM_X_MARK) and the bulk memsets were never dormant
 * (one sb on paths a VRAM store rarely takes).  The trigger is the guest's
 * own jump, so linked consoles running the same guest arm at the same
 * instruction, and the marks change no guest-visible state either way.  Once
 * armed it stays armed until init_emitter rebuilds the stubs dormant (ROM
 * load, dynarec re-enable; both flush the ROM cache, forgetting VRAM code). */
u32 vram_exec_activations;
static void __attribute__((noinline)) vram_exec_activate(void)
{
  u32 i;
#ifdef JIT_CODE_DISCIPLINE
  u32 sealed = jit_stub_zone_sealed;
  jit_stub_zone_sealed = 0;   /* a recorded patch of the STUB zone */
#endif
  vram_code_forget();
  for (i = 0; i < vram_x_slot_n; i++) {
    u32 *w = vram_x_slots[i];
    *w = vram_x_slot_word;
#ifdef JIT_CODE_DISCIPLINE
    JIT_CODE_WROTE(w, w + 1);   /* published before the lookup returns */
#else
    platform_cache_sync(w, w + 1);
#endif
  }
#ifdef JIT_CODE_DISCIPLINE
  jit_stub_zone_sealed = sealed;
#endif
  vram_exec_active = 1;
  vram_exec_activations++;
}

#define VRAM_CODE_NOTE(start, end) do {                                       \
  if ((GBA_PC(start) >> 24) == 6) vram_code_note((start), (end));            \
} while (0)
#define DC_PUT_OK(pc) 1   /* VRAM blocks never reach the put: see above */
#else
#define VRAM_CODE_NOTE(start, end) do { } while (0)
#define DC_PUT_OK(pc) 1
#endif

#ifdef VRAM_EXEC
/* the region-6 case of block_lookup_translate_builder (see VRAM_EXEC above),
 * out of line (compat-cost): inline it grew block_lookup_translate_arm/thumb
 * and changed what the compiler inlines on the hot dispatch path (PSP harness
 * hot core: block_lookup_address_dual 404 -> 56 bytes), for a case only
 * games that run code from VRAM ever reach. */
#define vram_lookup_builder(type)                                             \
static u8 * __attribute__((noinline))                                         \
 vram_lookup_translate_##type(u32 pc, u32 thumb)                              \
{                                                                             \
  u32 key = pc | thumb;                                                       \
  u32 hash_target = ((key * 2654435761U) >> (32 - VRAM_BRANCH_HASH_BITS))     \
                                          & (VRAM_BRANCH_HASH_SIZE - 1);      \
  hashhdr_type *bhdr;                                                         \
  u32 blk_offset, *blk_offset_addr;                                           \
  if (!vram_exec_active)                                                      \
    vram_exec_activate();   /* the game's first jump into VRAM */            \
  vram_code_check();   /* written since translated? forget it all */          \
  blk_offset = vram_branch_hash[hash_target];                                 \
  blk_offset_addr = &vram_branch_hash[hash_target];                           \
  while(blk_offset)                                                           \
  {                                                                           \
    bhdr = (hashhdr_type*)&rom_translation_cache[blk_offset];                 \
    if(bhdr->pc_value == key)                                                 \
      return &rom_translation_cache[                                          \
              blk_offset + sizeof(hashhdr_type) + block_prologue_size];       \
    blk_offset = bhdr->next_entry;                                            \
    blk_offset_addr = &bhdr->next_entry;                                      \
  }                                                                           \
  {                                                                           \
    u8 *blkptr;                                                               \
    bool result;                                                              \
    bhdr = (hashhdr_type*)rom_translation_ptr;                                \
    bhdr->pc_value = key;                                                     \
    bhdr->next_entry = 0;                                                     \
    JIT_CODE_META(bhdr, bhdr + 1);                                            \
    *blk_offset_addr = (u32)(rom_translation_ptr - rom_translation_cache);    \
    JIT_CODE_META(blk_offset_addr, blk_offset_addr + 1);                      \
    rom_translation_ptr += sizeof(hashhdr_type);                              \
    blkptr = rom_translation_ptr + block_prologue_size;                       \
    DRPROF_XLAT_PC_PUSH(pc | thumb);                                          \
    DRPROF_BLK_BEGIN();                                                       \
    result = translate_block_##type(pc, false);                               \
    DRPROF_BLK_END(result);                                                   \
    DRPROF_XLAT_PC_POP();                                                     \
    if (result)                                                               \
      return blkptr;                                                          \
    vram_code_forget();   /* never leave a half-built block hashed */         \
  }                                                                           \
  return NULL;                                                                \
}
vram_lookup_builder(arm)
vram_lookup_builder(thumb)

/* Region 6 is NOT a case of block_lookup_translate_* (self-activate): it
 * falls to the default, BLOCK_LOOKUP_UNMAPPABLE, so the shared lookup -- and
 * block_lookup_address_*, which inline it on the dispatch path -- compile
 * exactly as without VRAM_EXEC.  The two callers that can meet a VRAM pc take
 * it from there: the dispatcher through BADJUMP_GUARD_X's existing sentinel
 * test (vram_or_badjump below, out of line, after which the result is NOT
 * put into DISPATCH_CACHE: its asm probe would skip vram_code_check), and the
 * translator's eager links through XLAT_LINK_LOOKUP.  This replaces
 * compat-cost's DC_PUT_OK test, which every C lookup paid. */
#ifdef BADJUMP_REPORT
#define VRAM_LOOKUP_CASE(type)                                                \
    case 0x6:                                                                 \
      return BLOCK_LOOKUP_UNMAPPABLE;   /* not a bad jump: no report */
#else
#define VRAM_LOOKUP_CASE(type)
#endif
#define XLAT_LINK_LOOKUP(type, thumb, target)                                 \
  (((GBA_PC(target) >> 24) == 6) ?                                            \
    vram_lookup_translate_##type((target), (thumb)) :                         \
    block_lookup_translate_##type(target))
#ifndef BADJUMP_SAFE
#error "VRAM_EXEC needs BADJUMP_SAFE (its dispatcher path is BADJUMP_GUARD's)"
#endif
#else
#define VRAM_LOOKUP_CASE(type)
#define XLAT_LINK_LOOKUP(type, thumb, target) block_lookup_translate_##type(target)
#endif

#ifdef SMC_REUSE
/* SMC_REUSE (make SMC_REUSE=1, off by default; docs/BUILD-SWITCHES.md).
 * The implementation and its contract are above translate_block_arm. */
struct smc_reuse_rec_s;
static struct smc_reuse_rec_s *smc_reuse_find(u32 key);
static u32 smc_reuse_gen;         /* bumped by every gate-table change */
static u8 *smc_reuse_body(struct smc_reuse_rec_s *r);
static u32 smc_reuse_size(struct smc_reuse_rec_s *r);
static bool smc_reuse_revive(struct smc_reuse_rec_s *r);
#define SMC_REUSE_DECL()            struct smc_reuse_rec_s *smc_rr
#define SMC_REUSE_FIND(key, body)   do {                                      \
  smc_rr = smc_reuse_find(key);                                               \
  if (smc_rr) (body) = smc_reuse_body(smc_rr);                                \
} while (0)
#define SMC_REUSE_TRANSLATE(call)   (smc_rr ? smc_reuse_revive(smc_rr) : (call))
#ifdef SMC_REUSE_DEBUG
static void smc_reuse_lktrace(u32 key, u32 off, u32 cb);
#define SMC_REUSE_LKTRACE(key, te, type)   smc_reuse_lktrace((key), (te)->offset_##type, (te)->code_bytes_##type)
#else
#define SMC_REUSE_LKTRACE(key, te, type) do { } while (0)
#endif
#ifdef SMC_REUSE_DEBUG
static void smc_reuse_dump_maybe(void);
#define SMC_REUSE_DUMP() smc_reuse_dump_maybe()
#else
#define SMC_REUSE_DUMP() do { } while (0)
#endif
/* Memory-stub call sites, logged as the emitter writes them, so a record
 * need not scan its body for them (smc_reuse_commit).  Every `jal` the
 * translator emits goes through mips_emit_jal; the log keeps the addresses in
 * order and drops any the emitter has since rewound over.  It is reset when a
 * RAM block starts; a block with more sites than fit falls back to the scan. */
#define SMC_REUSE_MAXSITES 1024
static u8 *smc_reuse_sitelog[SMC_REUSE_MAXSITES];
static u32 smc_reuse_nlog;        /* > SMC_REUSE_MAXSITES: overflowed */
static u32 smc_reuse_rec_on = 1;  /* recording (smc_reuse_window) */
static inline void smc_reuse_log_site(u8 *at)
{
  u32 n = smc_reuse_nlog;
  if (n > SMC_REUSE_MAXSITES || !smc_reuse_rec_on)
    return;
  while (n && smc_reuse_sitelog[n - 1] >= at)
    n--;
  if (n == SMC_REUSE_MAXSITES)
    n = SMC_REUSE_MAXSITES + 1;
  else
    smc_reuse_sitelog[n++] = at;
  smc_reuse_nlog = n;
}
/* Run-time-mutable data words inside a body (SPIN_SKIP's descriptor), logged
 * as the emitter writes them: a record keeps their emitted values with the
 * call sites, and a revival restores both. */
#define SMC_REUSE_MAXMUT 64
static u8 *smc_reuse_mutlog[SMC_REUSE_MAXMUT];
static u32 smc_reuse_nmut;        /* > SMC_REUSE_MAXMUT: overflowed, never record */
static inline void smc_reuse_log_mutable(u8 *at, u32 nwords)
{
  u32 n = smc_reuse_nmut;
  while (nwords-- && n <= SMC_REUSE_MAXMUT) {
    if (n == SMC_REUSE_MAXMUT) { n++; break; }
    smc_reuse_mutlog[n++] = at;
    at += 4;
  }
  smc_reuse_nmut = n;
}
#undef SMC_REUSE_MUTABLE
#define SMC_REUSE_MUTABLE(ptr, nwords) smc_reuse_log_mutable((ptr), (nwords))
#undef mips_emit_jal
#define mips_emit_jal(offset) do {                                            \
  smc_reuse_log_site(translation_ptr);                                        \
  mips_emit_jump(jal, offset);                                                \
} while (0)
#ifdef SMC_PARTIAL_SAFE
/* code_bytes is "live" (nonzero) and "a trampoline fits" (>= 8); a revived
 * body sits below ram_translation_ptr, so the distance cannot be used. */
#define SMC_REUSE_CODE_BYTES(te, type) do {                                   \
  if (smc_rr && smc_partial_active)                                           \
    (te)->code_bytes_##type = smc_reuse_size(smc_rr);                         \
} while (0)
#else
#define SMC_REUSE_CODE_BYTES(te, type) do { } while (0)
#endif
#else
#define SMC_REUSE_DUMP() do { } while (0)
#define SMC_REUSE_LKTRACE(key, te, type) do { } while (0)
#define SMC_REUSE_DECL()            int smc_rr_unused_
#define SMC_REUSE_FIND(key, body)   do { (void)smc_rr_unused_; } while (0)
#define SMC_REUSE_TRANSLATE(call)   (call)
#define SMC_REUSE_CODE_BYTES(te, type) do { } while (0)
#endif

#define block_lookup_translate_builder(type)                                  \
u8 function_cc *block_lookup_translate_##type(u32 pc)                         \
{                                                                             \
  u8 pcregion = (GBA_PC(pc) >> 24);                                           \
  u16 *location;                                                              \
  u32 block_tag;                                                              \
                                                                              \
  block_lookup_address_pc_##type();                                           \
                                                                              \
  switch(pcregion)                                                            \
  {                                                                           \
    case 0x2:                                                                 \
    case 0x3:                                                                 \
    {                                                                         \
      u16* tagp = (pcregion == 2) ? (u16 *)(ewram + (pc & 0x3FFFF) + 0x40000) \
                                  : (u16 *)(iwram + (pc & 0x7FFF));           \
      ramtag_type* trentry;                                                   \
      SMC_RESTORE_RETIRED_TAG_##type(pc, tagp);                               \
      /* Allocate a tag if not a valid one, and initialize header */          \
      if (!VALID_TAG(*tagp)) {                                                \
        allocate_tag_##type(tagp);                                            \
        trentry = get_ram_tag(*tagp);                                         \
        trentry->offset_arm = 0;                                              \
        trentry->offset_thumb = 0;                                            \
        SMC_INIT_TAG_METADATA(trentry);                                       \
        STALE_INIT_TAG(trentry);                                              \
      } else {                                                                \
        trentry = get_ram_tag(*tagp);                                         \
      }                                                                       \
                                                                              \
      SMC_REUSE_LKTRACE(pc | thumb, trentry, type);                           \
      if (SMC_RAM_NEEDS_TRANSLATION(trentry, type)) {                         \
        bool result;                                                          \
        u32 cph_t;                                                            \
        u8 *blkptr = ram_translation_ptr + ram_block_prologue_size;           \
        SMC_REUSE_DECL();                                                     \
        SMC_REUSE_FIND(pc | thumb, blkptr);                                   \
        SMC_RAM_PREPARE_TRANSLATION(trentry, type, blkptr);                   \
        /* Phase 5h: dynarec COMPILATION, separated from execution.  A        \
         * few tens of calls a frame in steady state, and the bracket is      \
         * nesting-safe because translate_block re-enters itself through      \
         * block_lookup_translate below -- see main.h. */                     \
        cph_t = core_phase_enter(CORE_PHASE_FINE);                            \
        XLAT_PROBE_ENTER();                                                   \
        DRPROF_ZONE_ENTER(DRZ_XLAT);                                          \
        DRPROF_XLAT_PC_PUSH(pc | thumb);                                      \
        DRPROF_BLK_BEGIN();                                                   \
        result = SMC_REUSE_TRANSLATE(translate_block_##type(pc, true));       \
        DRPROF_BLK_END(result);                                               \
        DRPROF_XLAT_PC_POP();                                                 \
        DRPROF_ZONE_LEAVE(DRZ_XLAT);                                          \
        XLAT_PROBE_LEAVE();                                                   \
        core_phase_leave(CORE_PHASE_FINE, &cph_jit, cph_t);                   \
                                                                              \
        if (result) {                                                         \
          SMC_NOTE_CODE_BYTES(trentry, blkptr, type);                         \
          SMC_REUSE_CODE_BYTES(trentry, type);                                \
          SMC_REUSE_DUMP();                                                   \
          SMC_RAM_COMMIT_TRANSLATION(trentry, type, blkptr);                  \
          return SMC_RAM_PUBLIC_ENTRY(trentry, type);                         \
        }                                                                     \
        SMC_RAM_ABORT_TRANSLATION(trentry, type, pc, thumb);                  \
      } else {                                                                \
        STALE_CHECK_ENTRY(trentry, pc, thumb);                                \
        return SMC_RAM_PUBLIC_ENTRY(trentry, type);                           \
      }                                                                       \
      return NULL;                                                            \
    }                                                                         \
                                                                              \
    VRAM_LOOKUP_CASE(type)                                                    \
    case 0x0:                                                                 \
    case 0x8 ... 0xD:                                                         \
    {                                                                         \
      u32 key = pc | thumb;                                                   \
      u32 hash_target = ((key * 2654435761U) >> (32 - ROM_BRANCH_HASH_BITS))  \
                                              & (ROM_BRANCH_HASH_SIZE - 1);   \
                                                                              \
      hashhdr_type *bhdr;                                                     \
      u32 blk_offset = rom_branch_hash[hash_target];                          \
      u32 *blk_offset_addr = &rom_branch_hash[hash_target];                   \
      while(blk_offset)                                                       \
      {                                                                       \
        bhdr = (hashhdr_type*)&rom_translation_cache[blk_offset];             \
        if(bhdr->pc_value == key)                                             \
          return &rom_translation_cache[                                      \
                  blk_offset + sizeof(hashhdr_type) + block_prologue_size];   \
                                                                              \
        blk_offset = bhdr->next_entry;                                        \
        blk_offset_addr = &bhdr->next_entry;                                  \
      }                                                                       \
                                                                              \
      { /* Not found, go ahead and translate, and backfill the hash table */  \
        u8 *blkptr;                                                           \
        bool result;                                                          \
        u32 cph_t;                                                            \
        bhdr = (hashhdr_type*)rom_translation_ptr;                            \
        bhdr->pc_value = key;                                                 \
        bhdr->next_entry = 0;                                                 \
        JIT_CODE_META(bhdr, bhdr + 1);                                        \
        *blk_offset_addr = (u32)(rom_translation_ptr - rom_translation_cache);\
        JIT_CODE_META(blk_offset_addr, blk_offset_addr + 1);                  \
        rom_translation_ptr += sizeof(hashhdr_type);                          \
        blkptr = rom_translation_ptr + block_prologue_size;                   \
        cph_t = core_phase_enter(CORE_PHASE_FINE);                            \
        XLAT_PROBE_ENTER();                                                   \
        DRPROF_ZONE_ENTER(DRZ_XLAT);                                          \
        DRPROF_XLAT_PC_PUSH(pc | thumb);                                      \
        DRPROF_BLK_BEGIN();                                                   \
        result = translate_block_##type(pc, false);                           \
        DRPROF_BLK_END(result);                                               \
        DRPROF_XLAT_PC_POP();                                                 \
        DRPROF_ZONE_LEAVE(DRZ_XLAT);                                          \
        XLAT_PROBE_LEAVE();                                                   \
        core_phase_leave(CORE_PHASE_FINE, &cph_jit, cph_t);                   \
                                                                              \
        if (result)                                                           \
          return blkptr;   /* ROM block: never retired, no size needed */     \
      }                                                                       \
      return NULL;                                                            \
    }                                                                         \
  }                                                                           \
                                                                              \
  /* Do not return NULL since it could indeed happen that some branch         \
     points to some random place (perhaps due to being garbage). This can     \
     happen when especulatively compiling code in RAM. Perhaps the game       \
     patches these instructions later, which would trigger a flush */         \
  return BADJUMP_SENTINEL(pc);                                                \
}                                                                             \

block_lookup_translate_builder(arm);
block_lookup_translate_builder(thumb);

/* STALE_CHECK_ALL_ENTRIES (DIAGNOSTIC, smc-coverage 2026-10-20): send EVERY
 * exit that targets RAM through a dispatch island, in every mode, so that
 * stale_check_entry sees every entry into a RAM block and not only lookups
 * (eagerly patched RAM->RAM links otherwise bypass it).  Changes timing
 * (dispatcher hops); answers only "is stale code ever ENTERED". */
#if defined(STALE_CHECK) && defined(STALE_CHECK_ALL_ENTRIES)
#define STALE_ALL_ENTRIES_COND(ram_region, target) 1
#else
#define STALE_ALL_ENTRIES_COND(ram_region, target)                              (smc_partial_active && SMC_RAM_LINK_NEEDS_DISPATCH(ram_region, target))
#endif

#ifdef SMC_PARTIAL_SAFE
static int smc_partial_target_is_gated(u32 addr)
{
  u32 i;
  for (i = 0; i < translation_gate_targets; i++)
    if (translation_gate_target_pc[i] == addr)
      return 1;
  return 0;
}
#endif

u8 function_cc *block_lookup_address_dual(u32 pc)
{
  u32 thumb = pc & 0x01;
  if(thumb) {
    pc &= ~1;
    reg[REG_CPSR] |= 0x20;
    return block_lookup_address_thumb(pc);
  } else {
#ifdef BX_ALIGN_FIX
    /* ARM7TDMI: a BX into ARM state with address bit 1 set fetches from
     * the word BELOW (the bus ignores A1).  Camelot's IRQ veneer ("bx pc" at
     * a halfword-aligned address, Golden Sun 2 / Mario Golf / Mario Tennis)
     * lands on the "mrs r3, cpsr" at pc+2; rounding up skipped it. */
    pc &= ~0x03;
#else
    pc = (pc + 2) & ~0x03;
#endif
    reg[REG_CPSR] &= ~0x20;
    return block_lookup_address_arm(pc);
  }
}

#ifdef BADJUMP_REPORT
u32 badjump_prev_pc;   /* pc of the lookup before the current one */
#define BADJUMP_NOTE_PC(p) do { badjump_prev_pc = (p); } while (0)
#else
#define BADJUMP_NOTE_PC(p) do { } while (0)
#endif

#ifdef STALE_CHECK
/* DIAGNOSTIC: a LIVE translated IWRAM block (its start tag unchanged, and under
 * SMC_PARTIAL_SAFE its body not retired) whose guest source words no longer
 * match what was translated.  That is stale code the SMC machinery failed to
 * catch; it is reported at the next dispatch/event after the write. */
#define STALE_RECS  256
#define STALE_WORDS 128
typedef struct { u32 start, end, thumb, tag, live; u32 w[STALE_WORDS]; } stale_rec;
static stale_rec stale_recs[STALE_RECS];
static u32 stale_rr, stale_reports, stale_checks;
static u16 *stale_tagp(u32 pc) { return (u16 *)(iwram + (pc & 0x7FFF)); }
void stale_note(u32 start, u32 end, u32 thumb)
{
  u32 i, n, slot = STALE_RECS;
  stale_rec *r;
  if ((start >> 24) != 3 || end <= start) return;
#ifdef STALE_STACKNOTE
  if ((start & 0x7FFF) >= 0x7000) {
    static u32 sn;
    FILE *f;
    if (sn++ < 200 && (f = fopen("ms0:/stacknote.txt", "a"))) {
      fprintf(f, "xlat %s %08x..%08x frame %u REG_PC %08x lr %08x sp %08x cpsr %08x first %08x\n",
              thumb ? "thumb" : "arm", (unsigned)start, (unsigned)end,
              (unsigned)frame_counter, (unsigned)reg[REG_PC], (unsigned)reg[14],
              (unsigned)reg[13], (unsigned)reg[REG_CPSR],
              (unsigned)*(u32 *)(iwram + 0x8000 + (start & 0x7FFC)));
      fclose(f);
    }
  }
#endif
  for (i = 0; i < STALE_RECS; i++)
    if (stale_recs[i].live && stale_recs[i].start == start &&
        stale_recs[i].thumb == thumb) { slot = i; break; }
  if (slot == STALE_RECS)
    for (i = 0; i < STALE_RECS; i++)
      if (!stale_recs[i].live) { slot = i; break; }
  if (slot == STALE_RECS) slot = (stale_rr++) % STALE_RECS;
  r = &stale_recs[slot];
  r->start = start; r->end = end; r->thumb = thumb;
  r->tag = *stale_tagp(start); r->live = 1;
  n = (end - start + 3) >> 2;
  if (n > STALE_WORDS) { n = STALE_WORDS; r->end = start + 4 * n; }
  for (i = 0; i < n; i++)
    r->w[i] = *(u32 *)(iwram + 0x8000 + ((start + 4 * i) & 0x7FFC));
}
void stale_scan(u32 where)
{
  u32 i, k, n;
  stale_checks++;
#ifdef STALE_DISPATCH_ONLY
  /* Cheap mode: at a dispatch, examine only the block being entered, and do
   * the full sweep only once per 64 update_gba calls. */
  if (where != 0xFFFFFFFFu) {
    u32 pc = where & 0x0FFFFFFE;
    if ((pc >> 24) != 3) return;
    for (i = 0; i < STALE_RECS; i++) {
      stale_rec *r = &stale_recs[i];
      if (!r->live || r->start != pc || r->thumb != (where & 1)) continue;
      if (*stale_tagp(r->start) != r->tag) { r->live = 0; continue; }
#ifdef SMC_PARTIAL_SAFE
      if (smc_partial_active) {
        ramtag_type *te = get_ram_tag(r->tag);
        if (!(r->thumb ? te->code_bytes_thumb : te->code_bytes_arm)) { r->live = 0; continue; }
      }
#endif
      n = (r->end - r->start + 3) >> 2;
      for (k = 0; k < n; k++) {
        u32 a = r->start + 4 * k;
        if (*(u32 *)(iwram + 0x8000 + (a & 0x7FFC)) != r->w[k]) {
          FILE *f;
          r->live = 0;
          if (++stale_reports > 200) return;
          f = fopen("ms0:/stale.txt", "a");
          if (f) {
            fprintf(f, "EXECUTING STALE (dispatch) %s block %08x..%08x word %08x was %08x now %08x frame %u\n",
                    r->thumb ? "thumb" : "arm", (unsigned)r->start, (unsigned)r->end,
                    (unsigned)a, (unsigned)r->w[k],
                    (unsigned)*(u32 *)(iwram + 0x8000 + (a & 0x7FFC)),
                    (unsigned)frame_counter);
            fclose(f);
          }
          break;
        }
      }
    }
    return;
  }
  if (stale_checks & 63) return;
#endif
  for (i = 0; i < STALE_RECS; i++) {
    stale_rec *r = &stale_recs[i];
    u16 tag;
    if (!r->live) continue;
    tag = *stale_tagp(r->start);
    if (tag != r->tag || !VALID_TAG(tag)) { r->live = 0; continue; }
    if (r->live == 2) {
      /* already reported stale: is the guest now EXECUTING it? */
      u32 pc = (where == 0xFFFFFFFFu) ? reg[REG_PC] : where;
      if (pc >= r->start && pc < r->end && stale_reports < 200) {
        FILE *f = fopen("ms0:/stale.txt", "a");
        stale_reports++;
        if (f) {
          fprintf(f, "EXECUTING STALE %s block %08x..%08x at pc %08x (%s) frame %u\n",
                  r->thumb ? "thumb" : "arm", (unsigned)r->start, (unsigned)r->end,
                  (unsigned)pc, where == 0xFFFFFFFFu ? "update_gba" : "dispatch",
                  (unsigned)frame_counter);
          fclose(f);
        }
        r->live = 0;
      }
      continue;
    }
#ifdef SMC_PARTIAL_SAFE
    if (smc_partial_active) {
      ramtag_type *te = get_ram_tag(tag);
      if (!(r->thumb ? te->code_bytes_thumb : te->code_bytes_arm)) { r->live = 0; continue; }
    }
#endif
    n = (r->end - r->start + 3) >> 2;
    for (k = 0; k < n; k++) {
      u32 a = r->start + 4 * k;
      u32 now = *(u32 *)(iwram + 0x8000 + (a & 0x7FFC));
      if (now != r->w[k]) {
        FILE *f;
        r->live = 2;
        if (++stale_reports > 60) return;
        f = fopen("ms0:/stale.txt", "a");
        if (f) {
          fprintf(f, "STALE #%u live %s block %08x..%08x  word %08x was %08x now %08x"
                     "  frame %u  where %08x  REG_PC %08x  smc %u flush %u  gates",
                  (unsigned)stale_reports, r->thumb ? "thumb" : "arm",
                  (unsigned)r->start, (unsigned)r->end, (unsigned)a,
                  (unsigned)r->w[k], (unsigned)now, (unsigned)frame_counter,
                  (unsigned)where, (unsigned)reg[REG_PC],
                  (unsigned)flush_ram_smc, (unsigned)flush_ram_total);
          for (k = 0; k < translation_gate_targets; k++)
            fprintf(f, " %08x", (unsigned)translation_gate_target_pc[k]);
          fprintf(f, "\n");
          fclose(f);
        }
        break;
      }
    }
  }
}
#define STALE_NOTE(s, e, t) stale_note((s), (e), (t))
#define STALE_SCAN(w) stale_scan(w)
#else
#define STALE_NOTE(s, e, t) do { } while (0)
#define STALE_SCAN(w) do { } while (0)
#endif

#ifdef IRQ_INTEGRITY_CHECK
/* DIAGNOSTIC.  On the GBA an interrupt is invisible to the code it interrupts:
 * the BIOS IRQ stub saves r0-r3/r12/lr, the game's handler preserves r4-r11
 * by ABI, and `subs pc, lr, #4` restores CPSR from SPSR_irq.  So at every IRQ
 * RETURN, r0-r14 of the resumed mode and the CPSR must equal what they were at
 * the matching IRQ ENTRY.  Any difference is the emulator losing guest state
 * inside the handler -- timing-independent, so it discriminates a real
 * dynarec defect from a mere change of interrupt cadence. */
typedef struct { u32 r[15]; u32 cpsr, ret, mode, ifie, frame, site, smc, flush; } irqchk_rec;
static irqchk_rec irqchk_stack[16];
static u32 irqchk_depth, irqchk_entries, irqchk_exits, irqchk_bad, irqchk_unmatched;
/* A savestate load or reset abandons any IRQ in flight. */
void irqchk_reset(void) { irqchk_depth = 0; }
void irqchk_enter(u32 site)
{
  irqchk_rec *e;
  u32 i;
  irqchk_entries++;
  if (irqchk_depth >= 16) { irqchk_unmatched++; return; }
  e = &irqchk_stack[irqchk_depth++];
  for (i = 0; i < 15; i++) e->r[i] = reg[i];
  e->cpsr  = REG_SPSR(MODE_IRQ);
  e->ret   = REG_MODE(MODE_IRQ)[6] - 4;
  e->mode  = reg[CPU_MODE];
  e->ifie  = ((u32)read_ioreg(REG_IF) << 16) | read_ioreg(REG_IE);
  e->frame = frame_counter;
  e->site  = site;
  e->smc   = flush_ram_smc;
  e->flush = flush_ram_total;
}
void irqchk_exit(u32 old_mode, u32 address)
{
  irqchk_rec *e;
  u32 i, diff = 0;
  if (old_mode != MODE_IRQ || reg[CPU_MODE] == MODE_IRQ)
    return;                                  /* not an IRQ return */
  irqchk_exits++;
  if (!irqchk_depth) { irqchk_unmatched++; return; }
  e = &irqchk_stack[--irqchk_depth];
  for (i = 0; i < 15; i++)
    if (reg[i] != e->r[i]) diff |= 1u << i;
  if (address != e->ret) diff |= 1u << 15;
  if (reg[REG_CPSR] != e->cpsr) diff |= 1u << 16;
  if (reg[CPU_MODE] != e->mode) diff |= 1u << 17;
  if (diff) {
    FILE *f;
    irqchk_bad++;
    if (irqchk_bad > 40) return;
    f = fopen("ms0:/irqchk.txt", "a");
    if (!f) return;
    fprintf(f, "=== IRQ STATE LOST #%u  diff=%05x  frame %u->%u  site %u  depth %u"
               "  entries %u exits %u\n",
            (unsigned)irqchk_bad, (unsigned)diff, (unsigned)e->frame,
            (unsigned)frame_counter, (unsigned)e->site, (unsigned)irqchk_depth,
            (unsigned)irqchk_entries, (unsigned)irqchk_exits);
    fprintf(f, "  IF:IE at entry %08x   smc flushes during %u   ram flushes during %u\n",
            (unsigned)e->ifie, (unsigned)(flush_ram_smc - e->smc),
            (unsigned)(flush_ram_total - e->flush));
    fprintf(f, "  ret  entry %08x exit %08x | cpsr entry %08x exit %08x | mode %02x %02x\n",
            (unsigned)e->ret, (unsigned)address, (unsigned)e->cpsr,
            (unsigned)reg[REG_CPSR], (unsigned)e->mode, (unsigned)reg[CPU_MODE]);
    for (i = 0; i < 15; i++)
      fprintf(f, "  r%-2u %08x %08x%s\n", (unsigned)i, (unsigned)e->r[i],
              (unsigned)reg[i], (diff >> i) & 1 ? "   <-- CHANGED" : "");
    fprintf(f, "  gates (%u):", (unsigned)translation_gate_targets);
    for (i = 0; i < translation_gate_targets; i++)
      fprintf(f, " %08x", (unsigned)translation_gate_target_pc[i]);
    fprintf(f, "\n  smc_last_write %08x  smc flushes %u  ram flushes %u\n",
            (unsigned)smc_last_write_addr, (unsigned)flush_ram_smc,
            (unsigned)flush_ram_total);
    fclose(f);
    /* The first two: guest RAM as the resumed code is about to see it. */
    if (irqchk_bad <= 2) {
      char nm[48];
      snprintf(nm, sizeof(nm), "ms0:/irqlost-%u-iwram.bin", (unsigned)irqchk_bad);
      if ((f = fopen(nm, "wb"))) { fwrite(&iwram[0x8000], 1, 0x8000, f); fclose(f); }
      snprintf(nm, sizeof(nm), "ms0:/irqlost-%u-ewram.bin", (unsigned)irqchk_bad);
      if ((f = fopen(nm, "wb"))) { fwrite(ewram, 1, 0x40000, f); fclose(f); }
      snprintf(nm, sizeof(nm), "ms0:/irqlost-%u-regs.bin", (unsigned)irqchk_bad);
      if ((f = fopen(nm, "wb"))) { fwrite(reg, 4, 64, f); fclose(f); }
    }
  }
#ifdef IRQ_INTEGRITY_SUMMARY
  /* PPSSPP runs only: a heartbeat proving the checker is live.  Left out of
   * hardware diagnostics -- a synchronous Memory Stick write every ~16k IRQs. */
  if ((irqchk_exits & 0x3FFF) == 0) {
    FILE *f = fopen("ms0:/irqchk.txt", "a");
    if (f) {
      fprintf(f, "summary: frame %u entries %u exits %u bad %u unmatched %u depth %u\n",
              (unsigned)frame_counter, (unsigned)irqchk_entries,
              (unsigned)irqchk_exits, (unsigned)irqchk_bad,
              (unsigned)irqchk_unmatched, (unsigned)irqchk_depth);
      fclose(f);
    }
  }
#endif
}
#endif

#ifdef BADJUMP_REPORT
u32 badjump_prev_pc;
static u32 badjump_executed_pc = 0xFFFFFFFF;
void badjump_report(u32 pc)
{
  static int seen = 0;
  FILE *f;
  unsigned g;
  /* Garbage targets reached only by eager translation are routine (223 in one
   * battle) and each report is a synchronous Memory Stick write on the
   * emulation thread.  The first few show the pattern; the one that matters
   * is the EXECUTED fault, which badjump_recover reports without this cap. */
  if (seen >= 32 && pc != badjump_executed_pc)
    return;
  f = fopen("ms0:/badjump.txt", "a");
  if (!f) return;
  fprintf(f, "=== badjump #%d ===\n", ++seen);
  fprintf(f, "bad target pc  : %08x\n", (unsigned)pc);
  fprintf(f, "prev lookup pc : %08x\n", (unsigned)badjump_prev_pc);
  fprintf(f, "smc_last_write : %08x\n", (unsigned)smc_last_write_addr);
  fprintf(f, "cpsr           : %08x  (T=%u)\n", (unsigned)reg[REG_CPSR],
          (unsigned)((reg[REG_CPSR] >> 5) & 1));
  fprintf(f, "gates (%u):", (unsigned)translation_gate_targets);
  for (g = 0; g < translation_gate_targets; g++)
    fprintf(f, " %08x", (unsigned)translation_gate_target_pc[g]);
  fprintf(f, "\n");
  for (g = 0; g < 16; g++)                 /* r13=SP r14=LR r15=PC */
    fprintf(f, "r%-2u=%08x%s", g, (unsigned)reg[g],
            (g % 4 == 3) ? "\n" : "  ");
  fprintf(f, "\nflush ram total %u smc %u\n",
          (unsigned)flush_ram_total, (unsigned)flush_ram_smc);
  fclose(f);
}
#endif
/* BADJUMP_SAFE: an unmappable guest pc must never reach `jr $v0`.
 *
 * block_lookup_translate returns (u8*)(~0) when the pc is outside every
 * region it handles, and NULL when translation failed 4x.  Neither is checked
 * anywhere: block_lookup_address_* only tests `if (ret)`, so ~0 sails through,
 * and mips_stub.S mips_indirect_branch_* does `jr $v0` straight into it.  That
 * is the hard PSP crash -- "Bad Execution Address, PC: ffffffff" (or 00000000
 * once the pc is masked).  Verified in the disassembly: the crash RA lands at
 * mips_indirect_branch_arm+0x44, the instruction after the JAL to
 * block_lookup_address_arm.
 *
 * Garbage branch targets are ROUTINE, not exceptional -- 223 in one battle,
 * every one from a block a gate started at a non-instruction boundary.  Almost
 * none are ever executed; the crash is the rare one that is.  The guest has
 * already gone wrong by the time we arrive, so there is nothing to preserve:
 * treat it as a guest fault and soft-reset the GBA instead of taking the
 * console down.  Emulator, frontend and memory stick all survive.
 *
 * This cannot regress anything -- the path it replaces crashes 100% of the
 * time, so there is no working behaviour to lose. */
#ifdef BADJUMP_SAFE
u32 badjump_recoveries = 0;
/* Pointing the CPU at the reset vector is NOT a reset: mode, IRQ enables,
 * timers and sound DMA all carry on, so the BIOS boots in the wrong mode with
 * the game's hardware still live.  Field report 2026-09-24 (H&S, PSP-3000,
 * 3.0.0): a black screen over a stale sound buffer, the "zombie" that before
 * DISPATCH_CYCLE_CHECK spun the console solid.  So the jump to 0 only has to
 * survive to the end of THIS frame; the frontend reads this flag after
 * retro_run() and performs a real reset_gba() between frames. */
volatile u32 badjump_reset_pending = 0;
u32 badjump_last_pc = 0;
#ifdef BADJUMP_REPORT
static void badjump_dump(const char *name, const void *p, unsigned len)
{
  FILE *f = fopen(name, "wb");
  if (f) { fwrite(p, 1, len, f); fclose(f); }
}
#endif
static u8 *badjump_recover(u32 pc)
{
  u8 *r;
  badjump_recoveries++;
  badjump_last_pc = pc;
  badjump_reset_pending = 1;
#ifdef BADJUMP_REPORT
  /* The EXECUTED fault: full context (bypasses the routine-report cap) and,
   * for the first two, the guest RAM exactly as the faulting code saw it --
   * the savestate a player can take afterwards shows only the aftermath. */
  badjump_executed_pc = pc;
  badjump_report(pc);
  if (badjump_recoveries <= 2)
  {
    char name[48];
    snprintf(name, sizeof(name), "ms0:/badjump-%u-iwram.bin",
             (unsigned)badjump_recoveries);
    badjump_dump(name, &iwram[0x8000], 0x8000);   /* data half */
    snprintf(name, sizeof(name), "ms0:/badjump-%u-ewram.bin",
             (unsigned)badjump_recoveries);
    badjump_dump(name, ewram, 0x40000);
    snprintf(name, sizeof(name), "ms0:/badjump-%u-regs.bin",
             (unsigned)badjump_recoveries);
    badjump_dump(name, reg, sizeof(u32) * 64);
  }
#endif
#ifdef BADJUMP_REPORT
  /* DIAGNOSTIC BUILDS ONLY.  This is reached from mips_indirect_branch_* --
   * i.e. from inside a translated block, with the dynarec's register state
   * live -- so fopen/fprintf/fclose here is a synchronous Memory Stick write
   * on the emulation thread.  It also wrote ms0:/badjump.txt out of the
   * shipped 7283f13 candidate, which the release audit's forbidden-string
   * list did not cover. */
  {
    FILE *f = fopen("ms0:/badjump.txt", "a");
    if (f) {
      fprintf(f, "*** RECOVERED #%u: guest pc %08x was unmappable, GBA reset\n",
              (unsigned)badjump_recoveries, (unsigned)pc);
      fclose(f);
    }
  }
#endif
  reg[REG_CPSR] &= ~0x20;            /* ARM mode */
  reg[REG_PC]    = 0x0;              /* GBA reset vector */
  r = block_lookup_translate_arm(0x0);
  /* BADJUMP_GUARD returns this pointer straight out of block_lookup_address_*,
   * skipping the translate_icache_sync() that function does on its normal
   * success path.  After any ROM-cache flush the reset-vector block is newly
   * emitted here, so without a sync the Allegrex can fetch stale I-cache
   * lines / un-written-back D-cache bytes for it.  PPSSPP does not model this,
   * which is why the guard was only ever proven there. */
  translate_icache_sync();
  if (r && r != BLOCK_LOOKUP_UNMAPPABLE) {
    JIT_CODE_CHECK_ENTRY(r);
    return r;
  }
  return bios_swi_entrypoint;        /* last resort: known-good block */
}
#define BADJUMP_GUARD(ret, pc)                                            \
  do { if ((ret) == BLOCK_LOOKUP_UNMAPPABLE) return badjump_recover(pc); } while (0)
#define BADJUMP_EXHAUSTED(pc)  return badjump_recover(pc)
#ifdef VRAM_EXEC
/* The dispatcher's region-6 lookups (see VRAM_LOOKUP_CASE): the sentinel
 * test BADJUMP_GUARD always made is the only thing on the common path.  The
 * same retries and publish as block_lookup_address_*, minus the dispatch
 * cache put.  One per instruction set, each with badjump_recover's one-
 * argument shape, so the call in the lookup's cold path is unchanged. */
#define vram_or_badjump_builder(type, thumb)                                  \
static u8 * __attribute__((noinline)) vram_or_badjump_##type(u32 pc)          \
{                                                                             \
  unsigned i;                                                                 \
  if ((GBA_PC(pc) >> 24) != 6)                                                \
    return badjump_recover(pc);                                               \
  for (i = 0; i < 4; i++) {                                                   \
    u8 *ret = vram_lookup_translate_##type(pc, (thumb));                      \
    if (ret && ret != BLOCK_LOOKUP_UNMAPPABLE) {                              \
      translate_icache_sync(); BADJUMP_NOTE_PC(pc);                           \
      JIT_CODE_CHECK_ENTRY(ret);                                              \
      return ret;                                                             \
    }                                                                         \
  }                                                                           \
  return badjump_recover(pc);                                                 \
}
vram_or_badjump_builder(arm, 0)
vram_or_badjump_builder(thumb, 1)
#define BADJUMP_GUARD_X(ret, pc, type)                                        \
  do { if ((ret) == BLOCK_LOOKUP_UNMAPPABLE)                                  \
         return vram_or_badjump_##type(pc); } while (0)
/* (cpu-levers vx) The exhausted path calls the same helper (a non-VRAM pc
 * goes straight to badjump_recover there), so the two cold tail calls merge
 * as they do without VRAM_EXEC and block_lookup_address_* keep their size. */
#define BADJUMP_EXHAUSTED_X(pc, type)  return vram_or_badjump_##type(pc)
#else
#define BADJUMP_GUARD_X(ret, pc, type) BADJUMP_GUARD(ret, pc)
#define BADJUMP_EXHAUSTED_X(pc, type)  BADJUMP_EXHAUSTED(pc)
#endif
#else
#define BADJUMP_GUARD(ret, pc) do { } while (0)
#define BADJUMP_GUARD_X(ret, pc, type) do { } while (0)
#define BADJUMP_EXHAUSTED(pc)  return NULL
#define BADJUMP_EXHAUSTED_X(pc, type)  BADJUMP_EXHAUSTED(pc)
#endif

#ifdef DISPATCH_CACHE
/* DISPATCH_CACHE (prototype, docs/DYNAREC-PROFILE.md): a direct-mapped
 * {guest pc -> host entry} table probed by mips_indirect_branch_* in asm
 * BEFORE they save the guest registers and call into C.  Filled here, on the
 * success path of the C lookup, so it only ever holds pointers this function
 * itself returned; emptied by every event that can retire or move a
 * translation (all three flush entry points below).  Keys are the exact bits
 * the lookup uses (GBA_PC-masked, aligned, shifted), so a hit returns
 * precisely what the C path would have returned for that pc.  The cycle
 * check (DISPATCH_CYCLE_CHECK) still runs first, unchanged: guest timing is
 * identical by construction, which dr_oracle.py verifies. */
u32 dispatch_cache_arm[2 << DISPATCH_CACHE_BITS] __attribute__((aligned(64)));
u32 dispatch_cache_thumb[2 << DISPATCH_CACHE_BITS] __attribute__((aligned(64)));
#ifdef GBA_PC_MASK
#define DC_KEY_ARM(pc)   ((((pc) & 0x0FFFFFFFu) >> 2))
#define DC_KEY_THUMB(pc) ((((pc) & 0x0FFFFFFFu) >> 1))
#else
#define DC_KEY_ARM(pc)   ((pc) >> 2)
#define DC_KEY_THUMB(pc) ((pc) >> 1)
#endif
#define DC_MASK ((1u << DISPATCH_CACHE_BITS) - 1)
#if defined(XLAT_FAST) && !defined(XLAT_NO_DCLEAR)
/* XLAT_FAST DCLEAR: every RAM flush and every partial retirement empties
 * both tables -- 8 KiB of memset, 12+ times a frame on the SMC-heavy titles,
 * while only the slots filled since the last clear can hold anything.  So
 * remember those slots (each at most once: a slot is listed when it goes from
 * empty to full) and reset just them.  The tables end up exactly as the
 * memset leaves them (every word 0xFFFFFFFF).  The first clear after boot
 * is a full memset, since .bss zeros are not "empty". */
static u16 dc_filled[2 << DISPATCH_CACHE_BITS];   /* (table << BITS) | slot */
static u32 dc_filled_n;
static u32 dc_list_valid;
void dispatch_cache_clear(void)
{
  if (!dc_list_valid) {
    memset(dispatch_cache_arm, 0xFF, sizeof(dispatch_cache_arm));
    memset(dispatch_cache_thumb, 0xFF, sizeof(dispatch_cache_thumb));
    dc_list_valid = 1;
  } else {
    u32 n;
    for (n = 0; n < dc_filled_n; n++) {
      u32 e = dc_filled[n];
      u32 *tbl = (e >> DISPATCH_CACHE_BITS) ? dispatch_cache_thumb
                                            : dispatch_cache_arm;
      u32 i = (e & DC_MASK) * 2;
      tbl[i] = 0xFFFFFFFFu;
      tbl[i + 1] = 0xFFFFFFFFu;
    }
  }
  dc_filled_n = 0;
}
#define DC_NOTE_FILL(tbl, i_) do {                                            \
  if ((tbl)[i_] == 0xFFFFFFFFu && dc_list_valid)                              \
    dc_filled[dc_filled_n++] = (u16)((((tbl) == &dispatch_cache_thumb[0])       \
                                      << DISPATCH_CACHE_BITS) | ((i_) / 2));  \
} while (0)
#else
void dispatch_cache_clear(void)
{
  memset(dispatch_cache_arm, 0xFF, sizeof(dispatch_cache_arm));
  memset(dispatch_cache_thumb, 0xFF, sizeof(dispatch_cache_thumb));
}
#define DC_NOTE_FILL(tbl, i_) do { } while (0)
#endif
#ifdef DISPATCH_PRECISE
/* DISPATCH_PRECISE (cpu-levers, 2026-10-26): SMC retirement forgets only the
 * retired blocks' own entries instead of emptying the whole cache (Doom II
 * retires ~200 blocks a frame; each wipe cost the next indirect branches a
 * C lookup: twin dispatch-cache miss rate 59-70%).
 *
 * Exact by construction: an entry is {pc -> SMC_RAM_PUBLIC_ENTRY of the
 * block whose tag is at pc}, and retirement redirects exactly that thunk
 * (smc_stable_thunk_dispatch, which calls dispatch_cache_forget(start)).
 * The one many-to-one case is RAM mirroring: 0x0300ae14 and 0x03002e14 share
 * a tag but not a key.  So only CANONICAL RAM pcs are cached (IWRAM
 * 03000000-03007FFF, EWRAM 02000000-0203FFFF after GBA_PC masking); a
 * mirrored pc always takes the C lookup, which returns what it always did.
 * Full flushes (RAM, ROM, init) still empty everything.  A forgotten slot
 * gets the key DC_TOMB, which no pc produces (keys are at most 28 bits), so
 * the XLAT_FAST fill list never lists a slot twice. */
#define DC_TOMB 0xFFFFFFFEu
#define DC_FILL_OK(pc) dc_fill_ok(pc)
static inline int dc_fill_ok(u32 pc)
{
  u32 r = (pc >> 24) & 0xF;
  if (r == 3) return !(pc & 0x00FF8000u);
  if (r == 2) return !(pc & 0x00FC0000u);
  return 1;
}
static void dispatch_cache_forget(u32 pc, u32 thumb)
{
  u32 r = (pc >> 24) & 0xF, k, i;
  u32 *tbl;
  if (r == 3) pc = 0x03000000u | (pc & 0x7FFFu);
  else if (r == 2) pc = 0x02000000u | (pc & 0x3FFFFu);
  else return;                          /* only RAM blocks retire */
  if (thumb) { k = pc >> 1; tbl = dispatch_cache_thumb; }
  else       { k = pc >> 2; tbl = dispatch_cache_arm; }
  i = (k & DC_MASK) * 2;
  if (tbl[i] == k)
    tbl[i] = DC_TOMB;
}
#else
#define DC_FILL_OK(pc) 1
#endif
#define DC_PUT(tbl, key, ptr) do {                                            \
  u32 k_ = (key), i_ = (k_ & DC_MASK) * 2;                                    \
  DC_NOTE_FILL(tbl, i_);                                                      \
  (tbl)[i_] = k_; (tbl)[i_ + 1] = (u32)(uintptr_t)(ptr);                      \
} while (0)
#else
#define DC_PUT(tbl, key, ptr) do { } while (0)
#endif

u8 function_cc *block_lookup_address_arm(u32 pc)
{
  unsigned i;
  DRPH_SCOPE(DRPH_DISP);
  STALE_SCAN(pc);
  for (i = 0; i < 4; i++) {
    u8 *ret = block_lookup_translate_arm(pc);
    BADJUMP_GUARD_X(ret, pc, arm);
    if (ret) {
#ifdef JIT_CODE_DISCIPLINE
      translate_icache_sync(); BADJUMP_NOTE_PC(pc);
      JIT_CODE_CHECK_ENTRY(ret);
#else
      translate_icache_sync_ranged(); BADJUMP_NOTE_PC(pc);
#endif
      if (DC_FILL_OK(GBA_PC(pc)))
        DC_PUT(dispatch_cache_arm, DC_KEY_ARM(pc), ret);
      return ret;
    }
  }

  /* Same reason as badjump_recover: stdio from the dispatch path blocks the
   * emulation thread on real hardware.  Diagnostic builds only. */
#ifdef BADJUMP_REPORT
  printf("bad jump %x (%x)\n", (unsigned)pc, (unsigned)reg[REG_PC]);
  fflush(stdout);
#endif
  BADJUMP_EXHAUSTED_X(pc, arm);
}

u8 function_cc *block_lookup_address_thumb(u32 pc)
{
  unsigned i;
  DRPH_SCOPE(DRPH_DISP);
  STALE_SCAN(pc | 1);
  for (i = 0; i < 4; i++) {
    u8 *ret = block_lookup_translate_thumb(pc);
    BADJUMP_GUARD_X(ret, pc, thumb);
    if (ret) {
      translate_icache_sync(); BADJUMP_NOTE_PC(pc);
      JIT_CODE_CHECK_ENTRY(ret);
      if (DC_FILL_OK(GBA_PC(pc)))
        DC_PUT(dispatch_cache_thumb, DC_KEY_THUMB(pc), ret);
      return ret;
    }
  }
  /* Same reason as badjump_recover: stdio from the dispatch path blocks the
   * emulation thread on real hardware.  Diagnostic builds only. */
#ifdef BADJUMP_REPORT
  printf("bad jump %x (%x)\n", (unsigned)pc, (unsigned)reg[REG_PC]);
  fflush(stdout);
#endif
  BADJUMP_EXHAUSTED_X(pc, thumb);
}


// Potential exit point: If the rd field is pc for instructions is 0x0F,
// the instruction is b/bl/bx, or the instruction is ldm with PC in the
// register list.
// All instructions with upper 3 bits less than 100b have an rd field
// except bx, where the bits must be 0xF there anyway, multiplies,
// which cannot have 0xF in the corresponding fields, and msr, which
// has 0x0F there but doesn't end things (therefore must be special
// checked against). Because MSR and BX overlap both are checked for.

#define arm_exit_point                                                        \
 (((opcode < 0x8000000) && ((opcode & 0x000F000) == 0x000F000) &&             \
  ((opcode & 0xDB0F000) != 0x120F000)) ||                                     \
  ((opcode & 0x12FFF10) == 0x12FFF10) ||                                      \
  ((opcode & 0x8108000) == 0x8108000) ||                                      \
  ((opcode >= 0xA000000) && (opcode < 0xF000000)) ||                          \
  ((opcode >= 0xF000000) && (!is_div_swi((opcode >> 16) & 0xFF))))            \

/* XLAT_FAST EXITPF: a cheap necessary condition in front of arm_exit_point.
 * Every term of arm_exit_point needs bit 15 of the (28-bit) opcode -- the
 * Rd == PC tests (0xF000), BX (0x12FFF10 covers bits 19..8), LDM with PC in
 * the list (0x8108000) -- except the ones that need opcode >= 0xA000000
 * (branches, coprocessor, SWI).  So "bit 15 clear and below 0xA000000"
 * proves "not an exit point", which is most instructions, in two tests. */
#if defined(XLAT_FAST) && !defined(XLAT_NO_EXITPF)
#define arm_exit_point_scan                                                   \
 (((opcode & 0x8000) || (opcode >= 0xA000000)) && arm_exit_point)
#else
#define arm_exit_point_scan arm_exit_point
#endif
#define thumb_exit_point_scan thumb_exit_point

#define arm_opcode_branch                                                     \
  ((opcode & 0xE000000) == 0xA000000)                                         \

#define arm_opcode_swi                                                        \
  ((opcode & 0xF000000) == 0xF000000)                                         \

#define arm_opcode_unconditional_branch                                       \
  (condition == 0x0E)                                                         \

#define arm_load_opcode()                                                     \
  opcode = readaddress32(pc_address_block, (block_end_pc & 0x7FFF));          \
  condition = opcode >> 28;                                                   \
                                                                              \
  opcode &= 0xFFFFFFF;                                                        \
                                                                              \
  block_end_pc += 4                                                           \

#define arm_branch_target()                                                   \
  branch_target = (block_end_pc + 4 + (((s32)(opcode & 0xFFFFFF) << 8) >> 6)) \

// Contiguous conditional block flags modification - it will set 0x20 in the
// condition's bits if this instruction modifies flags. Taken from the CPU
// switch so it'd better be right this time.

#define arm_set_condition(_condition)                                         \
  block_data[block_data_position].condition = _condition;                     \
  switch((opcode >> 20) & 0xFF)                                               \
  {                                                                           \
    case 0x01:                                                                \
    case 0x03:                                                                \
    case 0x09:                                                                \
    case 0x0B:                                                                \
    case 0x0D:                                                                \
    case 0x0F:                                                                \
      if((((opcode >> 5) & 0x03) == 0) || ((opcode & 0x90) != 0x90))          \
        block_data[block_data_position].condition |= 0x20;                    \
      break;                                                                  \
                                                                              \
    case 0x05:                                                                \
    case 0x07:                                                                \
    case 0x11:                                                                \
    case 0x13:                                                                \
    case 0x15 ... 0x17:                                                       \
    case 0x19:                                                                \
    case 0x1B:                                                                \
    case 0x1D:                                                                \
    case 0x1F:                                                                \
      if((opcode & 0x90) != 0x90)                                             \
        block_data[block_data_position].condition |= 0x20;                    \
      break;                                                                  \
                                                                              \
    case 0x12:                                                                \
      if(((opcode & 0x90) != 0x90) && !(opcode & 0x10))                       \
        block_data[block_data_position].condition |= 0x20;                    \
      break;                                                                  \
                                                                              \
    case 0x21:                                                                \
    case 0x23:                                                                \
    case 0x25:                                                                \
    case 0x27:                                                                \
    case 0x29:                                                                \
    case 0x2B:                                                                \
    case 0x2D:                                                                \
    case 0x2F ... 0x37:                                                       \
    case 0x39:                                                                \
    case 0x3B:                                                                \
    case 0x3D:                                                                \
    case 0x3F:                                                                \
      block_data[block_data_position].condition |= 0x20;                      \
    break;                                                                    \
  }                                                                           \

#define arm_instruction_width 4

#define arm_base_cycles()                                                     \
  cycle_count += def_seq_cycles[pc >> 24][1]                                  \

// For now this just sets a variable that says flags should always be
// computed.

#define arm_dead_flag_eliminate()                                             \
  flag_status = 0xF                                                           \

// The following Thumb instructions can exit:
// b, bl, bx, swi, pop {... pc}, and mov pc, ..., the latter being a hireg
// op only. Rather simpler to identify than the ARM set.

#define thumb_exit_point                                                      \
  (((opcode >= 0xD000) && (opcode < 0xDF00)) ||                               \
   (((opcode & 0xFF00) == 0xDF00) &&                                          \
    (!is_div_swi(opcode & 0xFF))) ||                                          \
   ((opcode >= 0xE000) && (opcode < 0xE800)) ||                               \
   ((opcode & 0xFF00) == 0x4700) ||                                           \
   ((opcode & 0xFF00) == 0xBD00) ||                                           \
   ((opcode & 0xFF87) == 0x4687) ||                                           \
   ((opcode >= 0xF800)))                                                      \

#define thumb_opcode_branch                                                   \
  (((opcode >= 0xD000) && (opcode < 0xDF00)) ||                               \
   ((opcode >= 0xE000) && (opcode < 0xE800)) ||                               \
   (opcode >= 0xF800))                                                        \

#define thumb_opcode_swi                                                      \
  ((opcode & 0xFF00) == 0xDF00)                                               \

#define thumb_opcode_unconditional_branch                                     \
  ((opcode < 0xD000) || (opcode >= 0xDF00))                                   \

#define thumb_load_opcode()                                                   \
  last_opcode = opcode;                                                       \
  opcode = readaddress16(pc_address_block, (block_end_pc & 0x7FFF));          \
                                                                              \
  block_end_pc += 2                                                           \

#define thumb_branch_target()                                                 \
  if(opcode < 0xE000)                                                         \
  {                                                                           \
    branch_target = block_end_pc + 2 + ((s8)(opcode & 0xFF) * 2);             \
  }                                                                           \
  else                                                                        \
                                                                              \
  if(opcode < 0xF800)                                                         \
  {                                                                           \
    branch_target = block_end_pc + 2 + ((s32)((opcode & 0x7FF) << 21) >> 20); \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    if((last_opcode >= 0xF000) && (last_opcode < 0xF800))                     \
    {                                                                         \
      branch_target =                                                         \
       (block_end_pc + ((s32)((last_opcode & 0x07FF) << 21) >> 9) +           \
       ((opcode & 0x07FF) * 2));                                              \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      goto no_direct_branch;                                                  \
    }                                                                         \
  }                                                                           \

#define thumb_set_condition(_condition)                                       \

#define thumb_instruction_width 2

#define thumb_base_cycles()                                                   \
  cycle_count += def_seq_cycles[pc >> 24][0]                                  \

// Here's how this works: each instruction has three different sets of flag
// attributes, each consisiting of a 4bit mask describing how that instruction
// interacts with the 4 main flags (N/Z/C/V).
// The first set, in bits 0:3, is the set of flags the instruction may
// modify. After this pass this is changed to the set of flags the instruction
// should modify - if the bit for the corresponding flag is not set then code
// does not have to be generated to calculate the flag for that instruction.

// The second set, in bits 7:4, is the set of flags that the instruction must
// modify (ie, for shifts by the register values the instruction may not
// always modify the C flag, and thus the C bit won't be set here).

// The third set, in bits 11:8, is the set of flags that the instruction uses
// in its computation, or the set of flags that will be needed after the
// instruction is done. For any instructions that change the PC all of the
// bits should be set because it is (for now) unknown what flags will be
// needed after it arrives at its destination. Instructions that use the
// carry flag as input will have it set as well.

// The algorithm is a simple liveness analysis procedure: It starts at the
// bottom of the instruction stream and sets a "currently needed" mask to
// the flags needed mask of the current instruction. Then it moves down
// an instruction, ANDs that instructions "should generate" mask by the
// "currently needed" mask, then ANDs the "currently needed" mask by
// the 1's complement of the instruction's "must generate" mask, and ORs
// the "currently needed" mask by the instruction's "flags needed" mask.

#define thumb_dead_flag_eliminate()                                           \
{                                                                             \
  u32 needed_mask = 0xff;                                                     \
                                                                              \
  while(--block_data_position >= 0)                                           \
  {                                                                           \
    flag_status = block_data[block_data_position].flag_data;                  \
    block_data[block_data_position].flag_data =                               \
     (flag_status & needed_mask);                                             \
    needed_mask &= ~((flag_status >> 4) & 0x0F);                              \
    needed_mask |= flag_status >> 8;                                          \
  }                                                                           \
}                                                                             \

#define MAX_BLOCK_SIZE   1024   // 2/4KiB blocks max
#define MAX_EXITS          32   // This covers 99% blocks

/* SMC_GATE_CHARGE: charge a gated block's tail before its exit.
 *
 * generate_translation_gate ends a block with an indirect jump that neither
 * subtracts the pending cycle_count nor checks the budget, so the instructions
 * between the last cycle-update point and the gate cost the guest nothing.
 * A gate inside a hot loop (the M4A mixer's endpoint gates) therefore makes
 * that loop run partly for free, which moves timer/sound-DMA timing -- the
 * PPSSPP full/control audio divergence.  Charging here restores the cycle
 * total the ungated layout would have charged by the next check point.
 *
 * Scoped to SMC_SCAN_GATE on purpose: blocks that end at MAX_BLOCK_SIZE /
 * MAX_EXITS / the RAM-end clamp keep upstream behaviour, so games that never
 * earn a gate emit byte-identical code. */
#ifdef SMC_GATE_CHARGE
#define SMC_GATE_CHARGE_TAIL()                                                \
  do {                                                                        \
    if (scan_exit_reason == SMC_SCAN_GATE)                                    \
    {                                                                         \
      generate_cycle_update();                                                \
    }                                                                         \
  } while (0)
#else
#define SMC_GATE_CHARGE_TAIL() do { } while (0)
#endif

/* WHERE AN UNMAPPABLE BLOCK EXIT IS SENT, AND WHY IT EMITS NOTHING.
 *
 * The first version of this fix emitted a four-instruction dispatch island per
 * untranslatable exit -- load the guest pc, jump to mips_indirect_branch_*.  It
 * was correct and it was expensive: measured on hardware at +16.4% frame work on
 * heart_soul_light and +8.2% on unbound_double_high, both consoles.  The reason
 * is a feedback loop rather than the loop's own cost.  Every island is 16 bytes
 * of GENERATED code in the RAM translation cache, on workloads that already
 * re-translate constantly; the cache fills sooner, a full flush retires
 * everything, re-translation emits the islands again, and round.  The counter
 * shows it directly: xlat 705 -> 831 in the same window.
 *
 * So exits go to bios_swi_entrypoint instead, and nothing is emitted at all --
 * one patched word per exit.  That block:
 *
 *   * is pre-generated once by init_bios_hooks() BELOW rom_cache_watermark, so
 *     it survives every ROM and RAM cache flush and its address never moves;
 *   * is already designated the safe fallback by this file -- badjump_recover
 *     returns it as its "last resort: known-good block";
 *   * is already patched into RAM blocks by both translate_block_* for a
 *     branch_target of 0x00000008, so a RAM->ROM direct link to it is an
 *     established pattern and not a new hazard.
 *
 * The invariant is unchanged and is the whole point: a translated block never
 * contains a direct jump to an address the translator could not resolve.  An
 * executed garbage branch now enters the BIOS SWI handler, which is defined
 * behaviour for the console even though the guest has already gone wrong -- as
 * against 7283f13, where it fetched from 0x0FFFFFFC and the PSP died.
 *
 * (The emit budget this used to reserve is gone with the islands.  Measured
 * separately: reserving it cost nothing, so it was never the problem -- but
 * with nothing emitted there is nothing to reserve for.) */

#if defined(SMC_SAME_SKIP) && defined(SMC_PARTIAL_SINGLE)
/* SMC_SAME_SKIP (cpu-levers, 2026-10-26): do not retire translations that a
 * single-store SMC trap left exactly as they were compiled.
 *
 * Doom II and Duke Nukem Advance patch their IWRAM renderers from a handful
 * of writer pcs, and ~70% of those traps store the value the word ALREADY
 * had when the covering blocks were translated (twin SMC event trace, 64
 * frames: Doom 2514 of 3542, Duke 1097 of 1642).  Each such trap retired the
 * blocks and the next entry retranslated them byte for byte.
 *
 * The record: for every IWRAM halfword a translation reads, the value it read
 * (xs_val) and a valid bit (xs_ok).  A scan that finds the halfword already
 * valid with a DIFFERENT value cannot tell which block read what (a write no
 * trap reported, or a retirement path that does not clear records -- the
 * M4A range path, harmlessly); it poisons the halfword (xs_bad) until the
 * next full RAM flush, and a poisoned halfword never skips.  The valid bit is cleared only
 * where no live translation can still cover the halfword: the full RAM flush
 * and the two places partial retirement removes a halfword's code marks
 * after retiring every block that covered it.  A clear site this misses can
 * only cause a poison (a missed skip), never a wrong skip.
 *
 * Invariant: a valid, unpoisoned record of halfword h equals what EVERY live
 * block covering h read.  Proof: a live block L covering h scanned h; at that
 * scan the record was either set to L's value or already equal to it (else
 * poison).  The record changes later only through a clear, and no clear
 * happens while L lives.
 *
 * Skip rule (xs_blocks_current): skip the retirement only when every live
 * block retirement would retire has, for EVERY halfword of its source extent,
 * a valid unpoisoned record equal to memory now.  By the invariant each such
 * block is then exactly what translating it again would produce, whatever
 * wrote memory in between -- including a store no trap reported (the STM
 * hole), DMA or a BIOS CpuSet, whose changed halfwords no longer match.
 *
 * Why the 2026-09-12 SMC_SKIP_SAME (mips_emit.h, never shipped) failure does
 * not carry over: that filter lived in the emitted store stub, compared the
 * new value with the PRE-STORE word and, when equal, skipped the trap itself
 * -- so it changed which stores trap, and it trusted memory to describe the
 * translated code (false after any unreported write).  Here no emitted code
 * changes, every trap still happens (same smc_write path and re-dispatch),
 * and the comparison is against what the translations read, over their whole
 * extents.  IWRAM only; EWRAM traps retire as before. */
#ifdef XS_HEAP
/* XS_HEAP (cpu-levers cpl2w): the tables live in one heap block taken
 * before the ROM cache (dynarec_heap_tables_init), so .bss -- and every hot
 * array after these in it -- keeps the release build's layout. */
static u16 *xs_val;
static u8  *xs_ok, *xs_bad;
#else
static u16 xs_val[0x8000 / 2] __attribute__((aligned(16)));
static u8  xs_ok[0x8000 / 16];
static u8  xs_bad[0x8000 / 16];
#endif
static u32 xs_ready;
u32 smc_same_skips, smc_same_poison;
#ifdef SMC_SAME_ADAPT
/* SMC_SAME_ADAPT (cpu-levers, 2026-10-26): record only while it pays.
 *
 * The record costs ~40 host instructions per IWRAM halfword translated
 * (xs_note_half), whether or not a single-store trap ever asks.  Hot Wheels
 * Stunt Track Challenge retranslates IWRAM ARM code constantly with NO
 * single-store traps (twin, 1200 f: 9.45M halfwords noted, 0 checks: +34 %
 * translation cost, -6.2 % on the 3000); Mario Tennis notes 6.2M for 1180
 * skips.  Doom notes 1.4M for 84k skips, Duke 1.4M for 42k.  A skip saves a
 * retire + retranslate, thousands of instructions, so the break-even is about
 * one skip per ~140 noted halfwords.
 *
 * Policy, general and deterministic (no game keys, no timers):
 *   ON  -> OFF: after every XS_WINDOW recorded halfwords, if fewer than
 *               XS_WINDOW / 256 traps were skipped in that window.
 *   OFF -> ON : after XS_REPROBE single-word IWRAM retirements while off --
 *               the events a skip could have saved -- re-probe for a window
 *               (doubling, up to 64x, after each probe that never paid).
 * While OFF nothing is recorded and no trap is checked (every trap retires,
 * as without SMC_SAME_SKIP).
 *
 * Switching ON mid-run would break the record's invariant for blocks
 * translated while OFF (they read memory nobody recorded, so a later record
 * by another block could disagree with them unseen).  So every translation
 * remembers whether it was recorded (xs_rec, one bit per tag and mode, set in
 * ramtag_note_extent), and xs_blocks_current refuses to skip unless every
 * covering block was.  Among recorded blocks the invariant and its proof are
 * unchanged; an unrecorded block can only cause a retirement.  With trapped
 * stores alone the bit is redundant (a changing store retires every covering
 * block and clears the record); it matters when untrapped STM non-final
 * words change code twice.  tools/drprof/synth/xs_norec.s builds exactly that case:
 * SMC_SAME_ADAPT_MUTANT_NOREC (no bit) executes a stale translation there,
 * the real check retires it.  XS_WINDOW is overridable for that test only. */
#ifndef XS_WINDOW                    /* overridable for the mutant test only */
#define XS_WINDOW     65536u
#endif
#define XS_MIN_SKIPS  (XS_WINDOW / 256)
/* SMC_SAME_REPROBE=N (cpu-levers cpl2w): the first re-probe threshold and
 * the value a paying window resets it to (default 256; cpl2w 1024 = the 4x
 * start, so a title that never pays -- Top Gear Rally, Mario Tennis --
 * spends three fewer probes before the backoff ceiling). */
#ifdef SMC_SAME_REPROBE
#define XS_REPROBE    ((u32)(SMC_SAME_REPROBE))
#else
#define XS_REPROBE    256u
#endif
static u32 xs_on = 1, xs_win_noted, xs_win_skips, xs_off_retires, xs_dirty;
u32 xs_turned_off, xs_turned_on;
#ifdef XS_HEAP
static u8 (*xs_rec)[0x10000 / 16];
#else
static u8 xs_rec[2][0x10000 / 16];       /* [thumb][tag >> 1] bit */
#endif
/* Nothing reads the record before partial invalidation is active, and
 * activation is a full RAM flush, so recording starts there. */
#define XS_ON() (xs_on && smc_partial_active)
#define XS_REC_GET(tag, m) ((xs_rec[m][(tag) >> 4] >> (((tag) >> 1) & 7)) & 1)
static inline void xs_rec_set(u32 tag, u32 m, u32 v)
{
  u8 bit = (u8)(1u << ((tag >> 1) & 7));
  if (v) xs_rec[m][tag >> 4] |= bit; else xs_rec[m][tag >> 4] &= (u8)~bit;
}
/* ramtag_note_extent, IWRAM block of `halves` halfwords just scanned */
/* Re-probe backoff: a probe (the windows from switching ON to switching OFF)
 * that never had one paying window doubles the retirements needed before the
 * next probe, up to XS_REPROBE << 6; a paying window resets it.  Top Gear
 * Rally retires steadily but never pays: without the backoff it re-probed 28
 * times in 1200 f (twin +2.7 % core); Doom never stops paying. */
static u32 xs_reprobe = XS_REPROBE, xs_probe_paid;
static inline void xs_adapt_note(u32 tag, u32 thumb, u32 halves)
{
  xs_rec_set(tag, thumb, xs_on);
  if (!xs_on)
    return;
  xs_win_noted += halves;
  if (xs_win_noted >= XS_WINDOW) {
    if (xs_win_skips < XS_MIN_SKIPS) {
      xs_on = 0;
      xs_off_retires = 0;
      xs_turned_off++;
      if (xs_probe_paid)
        xs_reprobe = XS_REPROBE;
      else if (xs_reprobe < (XS_REPROBE << 6))
        xs_reprobe <<= 1;
    } else
      xs_probe_paid = 1;
    xs_win_noted = xs_win_skips = 0;
  }
}
static inline void xs_adapt_retired(void)
{
  if (!xs_on && ++xs_off_retires >= xs_reprobe) {
    xs_probe_paid = 0;
    xs_on = 1;
    xs_win_noted = xs_win_skips = 0;
    xs_turned_on++;
  }
}
#else
#define XS_ON() 1
#endif
#ifdef XS_COST_PROBE
u32 xs_calls, xs_slots, xs_halves;
#endif
/* xs_min[word]: a lower bound (halfword index) on the start of every live
 * block covering either halfword of the word -- the scan window
 * xs_blocks_current walks.  Lowered by every scan of the word; reset only
 * when neither halfword has a valid record, i.e. no live block covers it. */
#ifdef XS_HEAP
static u16 *xs_min;
#else
static u16 xs_min[0x8000 / 4];
#endif
static inline void xs_note_half(u32 off, u32 start)
{
  u32 h = (off & 0x7FFF) >> 1, w = h >> 1;
  u32 s = (start & 0x7FFF) >> 1;
  u8 bit = (u8)(1u << (h & 7));
  u16 cur = *(u16 *)(iwram + 0x8000 + (h << 1));
  if (xs_bad[h >> 3] & bit)
    return;
  if (!(xs_ok[h >> 3] & bit)) {
    u8 obit = (u8)(1u << ((h ^ 1) & 7));
    if (!(xs_ok[(h ^ 1) >> 3] & obit) || s < xs_min[w])
      xs_min[w] = (u16)s;
    xs_val[h] = cur;
    xs_ok[h >> 3] |= bit;
  } else if (s < xs_min[w]) {
    xs_min[w] = (u16)s;
  }
  if ((xs_ok[h >> 3] & bit) && xs_val[h] != cur) {
    xs_bad[h >> 3] |= bit;
    smc_same_poison++;
  }
}
#ifdef SMC_SAME_ADAPT
static __attribute__((noinline)) void xs_note_block(u32 start, u32 end)
{
  u32 a;
  xs_dirty = 1;
#ifdef XS_NOTE_FAST
  /* XS_NOTE_FAST (xlat-cost 2026-10-28): eight halfwords (one xs_ok byte)
   * at a time where that is exactly what xs_note_half would do eight times.
   *  - none recorded, none poisoned: record all eight; both halves of each
   *    word were unrecorded, so xs_min[w] = s (the first half sets it, the
   *    second finds the first recorded and s < s is false).
   *  - all recorded, none poisoned, all equal to memory: only
   *    xs_min[w] = min(xs_min[w], s); no poison.
   * Anything else (partly recorded, poisoned, a differing value, a ragged
   * head or tail) goes through xs_note_half one halfword at a time, in the
   * same order, so xs_val/xs_ok/xs_bad/xs_min/smc_same_poison end up
   * identical.  Doom notes ~1.2k IWRAM halfwords a frame at ~35 host
   * instructions each (twin). */
  {
    const u32 s = (start & 0x7FFF) >> 1;
    a = start;
    while (a < end) {
      u32 h = (a & 0x7FFF) >> 1;
      if (!(h & 7) && a + 16 <= end && !xs_bad[h >> 3]) {
        u32 ok = xs_ok[h >> 3];
        u32 *val = (u32 *)&xs_val[h];
        const u32 *cur = (const u32 *)(iwram + 0x8000 + (h << 1));
        u16 *mn = &xs_min[h >> 1];
        if (!ok) {
          val[0] = cur[0]; val[1] = cur[1]; val[2] = cur[2]; val[3] = cur[3];
          xs_ok[h >> 3] = 0xFF;
          mn[0] = mn[1] = mn[2] = mn[3] = (u16)s;
          a += 16;
          continue;
        }
        if (ok == 0xFF && !((val[0] ^ cur[0]) | (val[1] ^ cur[1]) |
                            (val[2] ^ cur[2]) | (val[3] ^ cur[3]))) {
          if (s < mn[0]) mn[0] = (u16)s;
          if (s < mn[1]) mn[1] = (u16)s;
          if (s < mn[2]) mn[2] = (u16)s;
          if (s < mn[3]) mn[3] = (u16)s;
          a += 16;
          continue;
        }
      }
      xs_note_half(a, start);
      a += 2;
    }
  }
#else
  for (a = start; a < end; a += 2)
    xs_note_half(a, start);
#endif
}
#endif
#ifdef SMC_SAME_ADAPT
/* ramtag_note_extent's one call (see LAYOUT NEUTRALITY at xs_trap_skip): the
 * block [start_pc, end_pc) whose start tag is valid was just scanned.  It
 * takes ramtag_note_extent's own arguments, so the call there is a bare tail
 * jump, and redoes the IWRAM canonicalisation itself.  IWRAM only. */
static __attribute__((noinline)) void xs_extent_note(u32 start_pc,
                                                     u32 end_pc, u32 thumb)
{
  u32 off = start_pc & 0x7FFF, len = end_pc - start_pc;
  u32 start = 0x03000000 + off;
  u32 end = (off + len > 0x8000) ? 0x03008000 : start + len;

#if defined(SMC_RETIRE_WINDOW) && defined(SMC_LOCAL_COLD)
  /* ramtag_note_extent's smc_back_note, moved here.  Only blocks whose start
   * tag is valid get here, and only those can be retired by a walk, so the
   * bound still covers every block a retire walk could visit.  A wrapping
   * extent is skipped as before (smc_partial_wrap_live full-flushes). */
  {
    u32 rgn = (start_pc >> 24) == 2, size = rgn ? 0x40000 : 0x8000;
    u32 o = start_pc & (size - 1);
    if (o + len <= size)
      smc_back_note(rgn, o, o + len);
  }
#endif
  if ((start_pc >> 24) != 3)
    return;
  if (XS_ON())
    xs_note_block(start, end);
  xs_adapt_note(((u16 *)iwram)[off >> 1], thumb ? 1 : 0, (end - start) >> 1);
}
#endif
static inline void xs_clear_half(u32 h)          /* h: IWRAM halfword index */
{
  xs_ok[h >> 3] &= (u8)~(1u << (h & 7));
}
#ifdef DRPROF_TWIN
static void xs_report(void)
{
  fprintf(stderr, "SMC_SAME_SKIP skips=%u poison=%u\n",
          (unsigned)smc_same_skips, (unsigned)smc_same_poison);
#ifdef SMC_SAME_ADAPT
  fprintf(stderr, "SMC_SAME_ADAPT on=%u off_events=%u on_events=%u\n",
          (unsigned)xs_on, (unsigned)xs_turned_off, (unsigned)xs_turned_on);
#endif
#ifdef XS_COST_PROBE
  fprintf(stderr, "XS_COST calls=%u slots=%u halves=%u\n", (unsigned)xs_calls,
          (unsigned)xs_slots, (unsigned)xs_halves);
#endif
}
#endif
static __attribute__((noinline)) void xs_clear_all(void)
{
#if defined(SMC_RETIRE_WINDOW) && defined(SMC_LOCAL_COLD)
  smc_back_reset();      /* flush_translation_cache_ram's, moved here */
#endif
#ifdef DRPROF_TWIN
  if (!xs_ready) atexit(xs_report);
#endif
#ifdef SMC_SAME_ADAPT
  /* Only xs_note_half sets xs_ok/xs_bad bits, so with nothing recorded since
   * the last clear they are still zero (Mario Tennis full-flushes often:
   * the two memsets were most of its flush time).  xs_rec needs no clear:
   * ramtag_note_extent rewrites a tag's bit at every translation, before the
   * tag can be seen live by xs_blocks_current. */
  if (xs_dirty || !xs_ready) {
    memset(xs_ok, 0, 0x8000 / 16);
    memset(xs_bad, 0, 0x8000 / 16);
    xs_dirty = 0;
  }
#else
  memset(xs_ok, 0, 0x8000 / 16);
  memset(xs_bad, 0, 0x8000 / 16);
#endif
  xs_ready = 1;
}
#ifdef SMC_SAME_ADAPT
/* Recorded in bulk once the scan has settled the extent (xs_note_block, from
 * ramtag_note_extent): the same halfwords in the same order, read from memory
 * the scan does not change -- and nothing left in the scan loop, where the
 * inlined per-instruction note cost the twin +22 % scan time on Hot Wheels
 * even while recording was off. */
#define XS_NOTE_ARM()   do { } while (0)
#define XS_NOTE_THUMB() do { } while (0)
#else
#define XS_NOTE_ARM()   do { if (pc >= 0x03000000) {                          \
    xs_note_half(block_end_pc, pc); xs_note_half(block_end_pc + 2, pc); } } while (0)
#define XS_NOTE_THUMB() do { if (pc >= 0x03000000)                            \
    xs_note_half(block_end_pc, pc); } while (0)
#endif
#define XS_CLEAR_HALF(region_is_iwram, h) do {                                \
    if (region_is_iwram) xs_clear_half(h); } while (0)
#else
#define XS_NOTE_ARM()   do { } while (0)
#define XS_NOTE_THUMB() do { } while (0)
#define XS_CLEAR_HALF(region_is_iwram, h) do { } while (0)
#endif

block_data_type block_data[MAX_BLOCK_SIZE];
block_exit_type block_exits[MAX_EXITS];

#if defined(SMC_PARTIAL_SINGLE) && !defined(DR2NFSU_MUTANT_WORDTAG)
/* TAG EACH HALFWORD OF AN ARM INSTRUCTION ON ITS OWN (dr2-nfsu, 2026-10-21).
 *
 * The stock macro marks the word only when BOTH halfword tags are zero.  That
 * was harmless while every SMC event wiped the whole tag map, but
 * smc_retire_single_word() clears the 0x0101 interior marks of the one word it
 * retired -- and when that word is the FIRST instruction of a block, its low
 * halfword holds the block-start tag, which stays.  The block is then
 * retranslated from the same start tag (no allocate_tag_arm, so nothing sets
 * location[1]) and this check sees a non-zero word and skips it: the HIGH
 * halfword of the block's first instruction stays untagged while the block is
 * live.  An STRB/STRH/STR to that halfword raises no SMC event and the block
 * runs stale code.
 *
 * Need for Speed Underground hits it exactly: a runaway byte memset
 * (STRB r3,[r12],#1 at 0x030066e0) walks over its own loop; the write to
 * 0x030066dd retires block 030066dc and clears 0x030066de's mark, the writes
 * to 0x030066de/df go unreported, and the loop runs its old SUB from then on
 * (twin SMC event trace: dc,dd,e0,e1... vs dc,dd,de,df,e0,e1 with the full
 * flush).  Per-halfword marking re-tags whatever half is zero, which only ever
 * adds marks (never more traps than a word with both halves marked).  The
 * common all-zero word keeps the single 32-bit test and store; only a word
 * with one half already non-zero takes the halfword tests. */
#define smc_write_arm_yes() {                                                 \
  XS_NOTE_ARM();                                                              \
  intptr_t offset = (pc < 0x03000000) ? 0x40000 : -0x8000;                    \
  intptr_t tag_off = (block_end_pc & 0x7FFF) + offset;                        \
  u32 tag_w = address32(pc_address_block, tag_off);                           \
  if(tag_w == 0)                                                              \
    address32(pc_address_block, tag_off) = CODE_TAG_BLOCK32;                  \
  else if(!(tag_w & 0xFFFF) || !(tag_w >> 16))                                \
  {                                                                           \
    if(address16(pc_address_block, tag_off) == 0)                             \
      address16(pc_address_block, tag_off) = CODE_TAG_BLOCK16;                \
    if(address16(pc_address_block, tag_off + 2) == 0)                         \
      address16(pc_address_block, tag_off + 2) = CODE_TAG_BLOCK16;            \
  }                                                                           \
}
#else
#define smc_write_arm_yes() {                                                 \
  XS_NOTE_ARM();                                                              \
  intptr_t offset = (pc < 0x03000000) ? 0x40000 : -0x8000;                    \
  if(address32(pc_address_block, (block_end_pc & 0x7FFF) + offset) == 0)      \
  {                                                                           \
    address32(pc_address_block, (block_end_pc & 0x7FFF) + offset) =           \
      CODE_TAG_BLOCK32;                                                       \
  }                                                                           \
}
#endif

#define smc_write_thumb_yes() {                                               \
  XS_NOTE_THUMB();                                                            \
  intptr_t offset = (pc < 0x03000000) ? 0x40000 : -0x8000;                    \
  if(address16(pc_address_block, (block_end_pc & 0x7FFF) + offset) == 0)      \
  {                                                                           \
    address16(pc_address_block, (block_end_pc & 0x7FFF) + offset) =           \
      CODE_TAG_BLOCK16;                                                       \
  }                                                                           \
}

#define smc_write_arm_no()                                                    \

#define smc_write_thumb_no()                                                  \


/* A BLOCK THAT STOPS AT A TRANSLATION GATE MUST STILL TAG THE GATE WORD.
 *
 * scan_block tags each address at the top of its loop and tests for a gate
 * only after advancing, so the word AT the gate is left untagged: it becomes
 * "code" only once the block that starts there is itself translated.  Gates
 * are placed at the FINAL word of the block stores that patch code, and that
 * final word is the only one an STM/PUSH checks -- every earlier word goes
 * through execute_aligned_store32, which has no SMC check.  Together that is a
 * hole: while the block ending at the gate is live but the block starting at
 * it has not been translated yet, a store whose earlier words land in the live
 * block and whose checked final word lands on the gate raises NO SMC event,
 * and the live block keeps executing the pre-patch instructions.
 *
 * Measured (PPSSPP, heart_soul_heavy, timing-neutral A/B, 2026-09-24): with
 * only the high endpoint gate 0x0300168c, SoundMainRAM's `stm lr, {r0, r1}`
 * at 0x03001644 rewrites 0x03001688 (unchecked) and 0x0300168c (checked but
 * untagged).  Blocks 030015bc..0300168c and 03001604..0300168c stayed live
 * holding the old `adds sb, r6, sb, asr #23` at 0x03001688 and ran it once
 * per mixer channel: one wrong mixed sample, which is the whole "full vs
 * control" audio divergence.  The low endpoint gate hides it for that one
 * store only -- and not at all when the gate table is full and the low gate
 * cannot be installed.
 *
 * Tagging the gate word as interior code (never as a block start: allocate_tag
 * still happens only when the gate block itself is looked up) makes the final
 * word's check fire whenever a live block precedes it.  Words already tagged
 * are left alone, ROM scans are unaffected, and ungated scans never get here. */
#define smc_tag_gate_arm_yes() {                                              \
  intptr_t offset = (pc < 0x03000000) ? 0x40000 : -0x8000;                    \
  check_pc_region(block_end_pc);                                              \
  if(address16(pc_address_block, (block_end_pc & 0x7FFF) + offset) == 0)      \
    address16(pc_address_block, (block_end_pc & 0x7FFF) + offset) =           \
      CODE_TAG_BLOCK16;                                                       \
  if(address16(pc_address_block, ((block_end_pc + 2) & 0x7FFF) + offset) == 0)\
    address16(pc_address_block, ((block_end_pc + 2) & 0x7FFF) + offset) =     \
      CODE_TAG_BLOCK16;                                                       \
}

#define smc_tag_gate_thumb_yes() {                                            \
  intptr_t offset = (pc < 0x03000000) ? 0x40000 : -0x8000;                    \
  check_pc_region(block_end_pc);                                              \
  if(address16(pc_address_block, (block_end_pc & 0x7FFF) + offset) == 0)      \
    address16(pc_address_block, (block_end_pc & 0x7FFF) + offset) =           \
      CODE_TAG_BLOCK16;                                                       \
}

#define smc_tag_gate_arm_no()
#define smc_tag_gate_thumb_no()

#ifdef NO_GATE_TAG_FIX   /* DIAGNOSTIC A/B only: restore the pre-fix hole */
#undef smc_tag_gate_arm_yes
#undef smc_tag_gate_thumb_yes
#define smc_tag_gate_arm_yes()
#define smc_tag_gate_thumb_yes()
#endif

/* SMC_SCAN_SPCLAMP: stop scanning at the STACK POINTER, not at the top of
 * IWRAM.
 *
 * The stock clamp is block_end_pc == 0x3007FF0 -- the last words of IWRAM --
 * so a scan starting in IWRAM code runs forward through the whole live stack
 * and tags every byte of it as code (SMC_SCAN_RAMEND names exactly this).
 * Once the stack is tagged, ordinary PUSH/STM traffic trips the SMC check and
 * forces a FULL cache flush: one PPSSPP battle measured 10122 SMC flushes,
 * and six of eight gate slots had been spent within a few hundred bytes of SP.
 *
 * Code does not live above SP, so stopping there costs nothing real, and the
 * clamp is safe by construction: worst case is an earlier block boundary,
 * which the translator already handles everywhere.
 *
 * MEASURED: NULL RESULT, do not retry as-is.  The stack grows DOWN, so the
 * live stack sits ABOVE sp and pushes land just BELOW it -- this clamp
 * protects [sp, 0x3008000), which is the wrong side.  The gated stack
 * addresses seen in the field (0x3007a38..0x3007c64) were all below an sp
 * of 0x3007c6c, so the clamp never covered them: s/x were unchanged
 * (s234 x7101 vs s242 x7355) and the crash still landed on b1c9. */
#ifdef SMC_SCAN_SPCLAMP
#define SMC_SCAN_PAST_SP(endpc)                                            \
  ((endpc) >= 0x3000000 && (endpc) < 0x3008000 &&                          \
   reg[REG_SP] >= 0x3000000 && reg[REG_SP] < 0x3008000 &&                  \
   (endpc) >= reg[REG_SP])
#else
#define SMC_SCAN_PAST_SP(endpc) (0)
#endif

#ifdef SMC_WRITE_HISTO
void smc_cover_note(u32 s, u32 e, u32 reason);
#endif

#if defined(SMC_GATE_BITMAP) && defined(SMC_GATES)
/* One bit per RAM halfword. This is a compiled view of the existing gate
 * table: it changes the cost of asking "is this PC gated?", never which PCs
 * are gates or where translated blocks end. */
static u8 smc_gate_iwram[0x8000 >> 4];
static u8 smc_gate_ewram[0x40000 >> 4];

static void smc_gate_map_add(u32 pc)
{
  u32 off;
  u8 *map;
  if ((pc >> 24) == 3) {
    off = (pc & 0x7FFF) >> 1;
    map = smc_gate_iwram;
  } else if ((pc >> 24) == 2) {
    off = (pc & 0x3FFFF) >> 1;
    map = smc_gate_ewram;
  } else {
    return;
  }
  map[off >> 3] |= (u8)(1u << (off & 7));
#ifdef SMC_REUSE
  smc_reuse_gen++;
#endif
}

static void smc_gate_map_rebuild(void)
{
  u32 i;
#ifdef SMC_REUSE
  smc_reuse_gen++;
#endif
  memset(smc_gate_iwram, 0, sizeof(smc_gate_iwram));
  memset(smc_gate_ewram, 0, sizeof(smc_gate_ewram));
  for (i = 0; i < translation_gate_targets; i++)
    smc_gate_map_add(translation_gate_target_pc[i]);
}

static inline int smc_gate_map_has(u32 pc)
{
  u32 off;
  const u8 *map;
  if ((pc >> 24) == 3) {
    off = (pc & 0x7FFF) >> 1;
    map = smc_gate_iwram;
  } else if ((pc >> 24) == 2) {
    off = (pc & 0x3FFFF) >> 1;
    map = smc_gate_ewram;
  } else {
    return 0;
  }
  return map[off >> 3] & (1u << (off & 7));
}

#ifdef ENTRY_GATES
/* ENTRY_GATES (make ENTRY_GATES=1; needs SMC_GATES + SMC_GATE_BITMAP).  A RAM routine entered at many
 * points (a computed jump into an unrolled loop) is translated once per entry,
 * each block holding the whole rest of the routine: Driver 2's two rasterisers
 * (~4 KB of guest code, ~45 entries each) become ~380 KB of native code and
 * the RAM cache fills every few frames.  When the RAM cache is EXHAUSTED,
 * remember where every block started; from then on a scan stops at those
 * addresses and the block jumps straight to the next one, so each stretch is
 * translated once.  Armed only by an exhaustion flush: a game that never fills
 * the cache never gets one.  One bit per RAM halfword, like the SMC gate map. */
static u8 entry_gate_iwram[0x8000 >> 4];
static u8 entry_gate_ewram[0x40000 >> 4];
u32 entry_gates_armed, entry_gate_count;
#ifdef ENTRY_GATES_RANGE
/* ENTRY_GATES_RANGE (slow-titles 2026-10-27): walk only the tag range the
 * full flush that follows will clear ([*_code_min & ~15, *_code_max + 8),
 * the same bounds and the same whole-region fallback as
 * flush_translation_cache_ram), instead of all 16K + 128K halfwords.  Every
 * tag outside it is zero -- the flush relies on exactly that -- so the
 * harvest is the same.  Payback exhausts the RAM cache every ~2 frames and
 * the two full walks cost the twin ~3M instructions each. */
static void entry_gates_bounds(u32 cmin, u32 cmax, u32 size, u32 *lo, u32 *hi)
{
  if (!cmax) { *lo = *hi = 0; return; }
  if (cmax > cmin) {
    *lo = (cmin & ~15U) >> 1;
    *hi = (MIN(cmax + 8, size) + 1) >> 1;
#ifdef ENTRY_GATES_RANGE_MUTANT
    *hi = *lo + ((*hi - *lo) >> 1);   /* oracle mutant: drop the upper half */
#endif
  } else {
    *lo = 0;
    *hi = size >> 1;
  }
}
#endif
#ifdef ENTRY_GATES_FAST
/* ENTRY_GATES_FAST (xlat-cost 2026-10-28): the harvest tests four tag slots
 * at a time.  Tags are 0 (data), 0x0101 (interior) or a block start with a
 * byte >= 2 (XS_WALK_PAIR's argument), so four slots whose bytes are all 0
 * or 1 hold no start.  Same slots, same bits, same count. */
typedef u32 __attribute__((may_alias)) eg_tagpair;
static u32 entry_gates_walk(const u16 *t, u32 lo, u32 hi, u8 *map)
{
  u32 i = lo, n = 0, k;
  while (i < hi && (i & 3)) {
    if (VALID_TAG(t[i])) { map[i >> 3] |= (u8)(1u << (i & 7)); n++; }
    i++;
  }
  for (; i + 4 <= hi; i += 4) {
    const eg_tagpair *q = (const eg_tagpair *)&t[i];
    if (!((q[0] | q[1]) & 0xFEFEFEFEu))
      continue;
    for (k = i; k < i + 4; k++)
      if (VALID_TAG(t[k])) { map[k >> 3] |= (u8)(1u << (k & 7)); n++; }
  }
  for (; i < hi; i++)
    if (VALID_TAG(t[i])) { map[i >> 3] |= (u8)(1u << (i & 7)); n++; }
  return n;
}
#endif
static void entry_gates_harvest(void)
{
  const u16 *t = (const u16 *)iwram;
  u32 i, n = 0;
  u32 ilo = 0, ihi = 0x4000, elo = 0, ehi = 0x20000;
#ifdef ENTRY_GATES_RANGE
  entry_gates_bounds(iwram_code_min, iwram_code_max, 0x8000, &ilo, &ihi);
  entry_gates_bounds(ewram_code_min, ewram_code_max, 0x40000, &elo, &ehi);
#ifdef ENTRY_GATES_RANGE_CHECK
  for (i = 0; i < 0x4000; i++)
    if ((i < ilo || i >= ihi) && VALID_TAG(t[i])) abort();
  for (i = 0; i < 0x20000; i++)
    if ((i < elo || i >= ehi) &&
        VALID_TAG(((const u16 *)(ewram + 0x40000))[i])) abort();
#endif
#endif
#ifdef ENTRY_GATES_FAST
  (void)i;
  n += entry_gates_walk(t, ilo, ihi, entry_gate_iwram);
  t = (const u16 *)(ewram + 0x40000);
  n += entry_gates_walk(t, elo, ehi, entry_gate_ewram);
#else
  for (i = ilo; i < ihi; i++)
    if (VALID_TAG(t[i])) { entry_gate_iwram[i >> 3] |= (u8)(1u << (i & 7)); n++; }
  t = (const u16 *)(ewram + 0x40000);
  for (i = elo; i < ehi; i++)
    if (VALID_TAG(t[i])) { entry_gate_ewram[i >> 3] |= (u8)(1u << (i & 7)); n++; }
#endif
  entry_gates_armed++;
  entry_gate_count = n;
#ifdef SMC_REUSE
  smc_reuse_gen++;
#endif
}
static void entry_gates_reset(void)
{
  memset(entry_gate_iwram, 0, sizeof(entry_gate_iwram));
  memset(entry_gate_ewram, 0, sizeof(entry_gate_ewram));
  entry_gates_armed = entry_gate_count = 0;
#ifdef SMC_REUSE
  smc_reuse_gen++;
#endif
}
static inline int entry_gate_has(u32 pc)
{
  u32 off;
  if (!entry_gates_armed)
    return 0;
  if ((pc >> 24) == 3) {
    off = (pc & 0x7FFF) >> 1;
    return entry_gate_iwram[off >> 3] & (1u << (off & 7));
  }
  if ((pc >> 24) == 2) {
    off = (pc & 0x3FFFF) >> 1;
    return entry_gate_ewram[off >> 3] & (1u << (off & 7));
  }
  return 0;
}
#define ENTRY_GATE_SCAN(tag_gate_op)                                         else if (entry_gate_has(block_end_pc)) {                                     tag_gate_op;                                                               scan_exit_reason = SMC_SCAN_GATE;                                          entry_gate_stop = 1;                                                       goto block_end;                                                          }
#else
#define ENTRY_GATE_SCAN(tag_gate_op)
#endif

#define SMC_GATE_MAP_ADD(pc)       smc_gate_map_add(pc)
#define SMC_GATE_MAP_REBUILD()     smc_gate_map_rebuild()
#if defined(XLAT_FAST) && !defined(XLAT_NO_GATEMAP)
/* XLAT_FAST GATEMAP: the same two bitmap tests as smc_gate_map_has() and
 * entry_gate_has(), with the per-region choice of map (and the entry-gate
 * "armed" test) made once per region instead of once per scanned
 * instruction: scan_block keeps the maps of the region (pc >> 24) it last
 * saw and looks them up again only when block_end_pc leaves it.  Neither map
 * changes during a scan (gates are added by SMC events, entry gates by an
 * exhaustion flush, never while a block is being scanned). */
static void xlat_gate_maps(u32 region, const u8 **smc, const u8 **ent,
                           u32 *mask)
{
  *smc = NULL;
  *ent = NULL;
  *mask = 0;
  if (region == 3) {
    *smc = smc_gate_iwram;
    *mask = 0x7FFF;
#ifdef ENTRY_GATES
    if (entry_gates_armed)
      *ent = entry_gate_iwram;
#endif
  } else if (region == 2) {
    *smc = smc_gate_ewram;
    *mask = 0x3FFFF;
#ifdef ENTRY_GATES
    if (entry_gates_armed)
      *ent = entry_gate_ewram;
#endif
  }
}
#ifdef ENTRY_GATES
#define XLAT_ENTRY_GATE_TEST(tag_gate_op, off, bit)                           \
  if (xg_ent && (xg_ent[(off) >> 3] & (bit))) {                               \
    tag_gate_op;                                                              \
    scan_exit_reason = SMC_SCAN_GATE;                                         \
    entry_gate_stop = 1;                                                      \
    goto block_end;                                                           \
  }
#else
#define XLAT_ENTRY_GATE_TEST(tag_gate_op, off, bit)
#endif
#define SMC_SCAN_GATE_END(tag_gate_op) do {                                   \
  u32 xg_r = block_end_pc >> 24;                                              \
  if (xg_r != xg_reg) {                                                       \
    xg_reg = xg_r;                                                            \
    xlat_gate_maps(xg_r, &xg_smc, &xg_ent, &xg_mask);                         \
  }                                                                           \
  if (xg_smc) {                                                               \
    u32 xg_off = (block_end_pc & xg_mask) >> 1;                               \
    u32 xg_bit = 1u << (xg_off & 7);                                          \
    if (xg_smc[xg_off >> 3] & xg_bit) {                                       \
      tag_gate_op;                                                            \
      scan_exit_reason = SMC_SCAN_GATE;                                       \
      goto block_end;                                                         \
    }                                                                         \
    XLAT_ENTRY_GATE_TEST(tag_gate_op, xg_off, xg_bit)                         \
  }                                                                           \
} while (0)
#define XLAT_GATEMAP_VARS                                                     \
  u32 xg_reg = ~0U, xg_mask = 0;                                              \
  const u8 *xg_smc = NULL, *xg_ent = NULL;
#else
#define SMC_SCAN_GATE_END(tag_gate_op) do {                                \
  if (smc_gate_map_has(block_end_pc)) {                                    \
    tag_gate_op;                                                           \
    scan_exit_reason = SMC_SCAN_GATE;                                      \
    goto block_end;                                                        \
  }                                                                        \
  ENTRY_GATE_SCAN(tag_gate_op)                                             \
} while (0)
#endif
#else
#define SMC_GATE_MAP_ADD(pc)       do { } while (0)
#define SMC_GATE_MAP_REBUILD()     do { } while (0)
#define SMC_SCAN_GATE_END(tag_gate_op) do {                                \
  for (i = 0; i < translation_gate_targets; i++) {                          \
    if (block_end_pc == translation_gate_target_pc[i]) {                   \
      tag_gate_op;                                                         \
      scan_exit_reason = SMC_SCAN_GATE;                                    \
      goto block_end;                                                      \
    }                                                                      \
  }                                                                        \
} while (0)
#endif

/* XLAT_FAST RAMEND: scan_block stops when block_end_pc reaches 0x3007FF0 or
 * 0x203FFF0 (the last words of IWRAM / EWRAM).  block_end_pc only ever
 * steps up by the instruction width from the block start, at most
 * MAX_BLOCK_SIZE times, so at most one of the two can be reached, and which
 * one (if any) is known before the scan starts: compare against that one
 * address instead of two constants per instruction.  1 is never reached
 * (block_end_pc stays even). */
#if defined(XLAT_FAST) && !defined(XLAT_NO_RAMEND)
static inline u32 xlat_ramend_stop(u32 start, u32 width)
{
  static const u32 stops[2] = { 0x3007FF0, 0x203FFF0 };
  u32 k;
  for (k = 0; k < 2; k++) {
    u32 d = stops[k] - start;
    if (stops[k] > start && d <= MAX_BLOCK_SIZE * width && (d % width) == 0)
      return stops[k];
  }
  return 1;
}
#define XLAT_RAMEND_VARS(type)                                               \
  const u32 xs_stop_pc = xlat_ramend_stop(block_end_pc, type##_instruction_width);
#define XLAT_SCAN_RAMEND(endpc) ((endpc) == xs_stop_pc)
#else
#define XLAT_RAMEND_VARS(type)
#define XLAT_SCAN_RAMEND(endpc) (((endpc) == 0x3007FF0) || ((endpc) == 0x203FFF0))
#endif
#ifndef XLAT_GATEMAP_VARS
#define XLAT_GATEMAP_VARS
#endif
/* The scan's three hook points.  These defaults are the scan as it always
 * was; JIT_COST's fast RAM ARM scan (xlat_scan_arm_ram_fast) redefines them
 * around its own expansion only. */
#define SCAN_VARS                       XLAT_GATEMAP_VARS
#define SCAN_SET_CONDITION(type, c)     type##_set_condition(c)
#define SCAN_GATE_END(tag_gate_op)      SMC_SCAN_GATE_END(tag_gate_op)
/* XLAT_PURE (make XLAT_PURE=1, off by default; ported from claude/smc-reuse
 * bb8b7b2).
 *
 * block_data[] is static and scan_block clears an instruction's update_cycles
 * flag only at the BOTTOM of its loop, which the last instruction of a block
 * never reaches when the scan leaves through an exit point.  The emit loop
 * then tests that slot to decide whether to commit the pending cycles before
 * the block's last instruction, so it reads whatever an EARLIER translation
 * left there: 1 if some earlier block had a branch target at the same
 * instruction index, 0 if a longer block cleared it since.  The same guest
 * code is therefore translated two ways depending on translation history.
 * With this switch the flag is cleared at the top of the loop, for every
 * scanned instruction, so a block's code depends only on its own guest bytes
 * (a precondition of reusing a translation).  It CHANGES emulation timing
 * wherever the stale flag used to be 1.
 *
 * XLAT_PURE_PROBE (twin diagnostic, -D only, meaningless with XLAT_PURE):
 * logs each translation whose last slot carries such a stale 1 ("XPP" lines,
 * first XPP_LOG_MAX) and counts them, so a PURE-vs-base divergence can be
 * tied to the first impure translation that precedes it. */
#ifdef XLAT_PURE
#define SCAN_CLEAR_SLOT() (block_data[block_data_position].update_cycles = 0)
#else
#define SCAN_CLEAR_SLOT() do { } while (0)
#endif

#if defined(XLAT_PURE_PROBE) && !defined(XLAT_PURE)
#ifndef XPP_LOG_MAX
#define XPP_LOG_MAX 4000
#endif
u32 xpp_count, xpp_logged;
/* Before the branch-target pass: the slot the scan left uncleared, if it is
 * inside the block (an exit-point exit), and its flag. */
#define XPP_SNAP(width)                                                       \
  u32 xpp_idx = block_data_position;                                          \
  u32 xpp_in = (block_start_pc + xpp_idx * (width)) < block_end_pc &&         \
               xpp_idx < MAX_BLOCK_SIZE;                                      \
  u32 xpp_stale = xpp_in ? block_data[xpp_idx].update_cycles : 0
/* After it: impure when no branch of THIS block targets that slot. */
#define XPP_CHECK(width, thumb)                                               \
  if (xpp_stale) {                                                            \
    u32 xpp_k, xpp_legit = 0;                                                 \
    for (xpp_k = 0; xpp_k < block_exit_position; xpp_k++)                     \
      if (block_exits[xpp_k].branch_target ==                                 \
          block_start_pc + xpp_idx * (width)) xpp_legit = 1;                  \
    if (!xpp_legit) {                                                         \
      xpp_count++;                                                            \
      if (xpp_logged < XPP_LOG_MAX) {                                         \
        xpp_logged++;                                                         \
        fprintf(stderr, "XPP f=%u pc=%08x end=%08x t=%u n=%u\n",              \
                (unsigned)frame_counter, (unsigned)block_start_pc,            \
                (unsigned)block_end_pc, (unsigned)(thumb),                    \
                (unsigned)xpp_count);                                         \
      }                                                                       \
    }                                                                         \
  }
#else
#define XPP_SNAP(width)
#define XPP_CHECK(width, thumb)
#endif

#define scan_block(type, smc_write_op)                                        \
{                                                                             \
  __label__ block_end;                                                        \
  SCAN_VARS                                                                   \
  XLAT_RAMEND_VARS(type)                                                      \
  /* Find the end of the block */                                             \
  do                                                                          \
  {                                                                           \
    check_pc_region(block_end_pc);                                            \
    SCAN_CLEAR_SLOT();                                                        \
    smc_write_##type##_##smc_write_op();                                      \
    type##_load_opcode();                                                     \
    type##_flag_status();                                                     \
                                                                              \
    if(type##_exit_point_scan)                                                \
    {                                                                         \
      /* Branch/branch with link */                                           \
      if(type##_opcode_branch)                                                \
      {                                                                       \
        __label__ no_direct_branch;                                           \
        type##_branch_target();                                               \
        block_exits[block_exit_position].branch_target = branch_target;       \
        block_exit_position++;                                                \
                                                                              \
        /* Give the branch target macro somewhere to bail if it turns out to  \
           be an indirect branch (ala malformed Thumb bl) */                  \
        no_direct_branch:;                                                    \
      }                                                                       \
                                                                              \
      /* SWI branches to the BIOS, unless it's an HLE call, then it is        \
         not parsed as an exit_point but rather an "instruction" of sorts. */ \
      if(type##_opcode_swi)                                                   \
      {                                                                       \
        block_exits[block_exit_position].branch_target = 0x00000008;          \
        block_exit_position++;                                                \
      }                                                                       \
                                                                              \
      SCAN_SET_CONDITION(type, condition | 0x10);                             \
                                                                              \
      /* Only unconditional branches can end the block. */                    \
      if(type##_opcode_unconditional_branch)                                  \
      {                                                                       \
        /* Check to see if any prior block exits branch after here,           \
           if so don't end the block. Starts from the top and works           \
           down because the most recent branch is most likely to              \
           join after the end (if/then form) */                               \
        for(i = block_exit_position - 2; i >= 0; i--)                         \
        {                                                                     \
          if(block_exits[i].branch_target == block_end_pc)                    \
            break;                                                            \
        }                                                                     \
                                                                              \
        if(i < 0)                                                             \
        {                                                                     \
          scan_exit_reason = SMC_SCAN_UNCOND;                                 \
          break;                                                              \
        }                                                                     \
      }                                                                       \
      if(block_exit_position == MAX_EXITS)                                    \
      {                                                                       \
        scan_exit_reason = SMC_SCAN_MAXEXIT;                                  \
        break;                                                                \
      }                                                                       \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      SCAN_SET_CONDITION(type, condition);                                    \
    }                                                                         \
                                                                              \
    SCAN_GATE_END(smc_tag_gate_##type##_##smc_write_op());                    \
                                                                              \
    block_data[block_data_position].update_cycles = 0;                        \
    block_data_position++;                                                    \
    if((block_data_position == MAX_BLOCK_SIZE) ||                             \
     XLAT_SCAN_RAMEND(block_end_pc) ||                                        \
     SMC_SCAN_PAST_SP(block_end_pc))                                          \
    {                                                                         \
      scan_exit_reason = (block_data_position == MAX_BLOCK_SIZE)              \
                       ? SMC_SCAN_MAXSIZE : SMC_SCAN_RAMEND;                  \
      break;                                                                  \
    }                                                                         \
  } while(1);                                                                 \
                                                                              \
  block_end:;                                                                 \
}                                                                             \

#define arm_fix_pc()                                                          \
  pc &= ~0x03                                                                 \

#define thumb_fix_pc()                                                        \
  pc &= ~0x01                                                                 \

#define update_pc_limits()                                                    \
if (ram_region) {                                                             \
  if (pc >= 0x3000000) {                                                      \
    iwram_code_min = MIN(pc & 0x7FFF, iwram_code_min);                        \
    iwram_code_max = MAX(pc & 0x7FFF, iwram_code_max);                        \
  } else {                                                                    \
    ewram_code_min = MIN(pc & 0x3FFFF, ewram_code_min);                       \
    ewram_code_max = MAX(pc & 0x3FFFF, ewram_code_max);                       \
  }                                                                           \
}                                                                             \

/* XLAT_FAST (make XLAT_FAST=1; claude/bench-xlat): translator-side speedups
 * that change NOTHING the translator produces -- same emitted bytes, same
 * tags, same counters -- only the host work spent producing them.  Each piece
 * has its own XLAT_NO_<piece> define so a bisect can switch it off alone.
 *
 * LIMITS: update_pc_limits() ran once per guest instruction, a read-modify-
 * write of two globals that the emitter's calls force through memory (twin:
 * ~18 host instructions per guest instruction, ~8% of translate_block_arm on
 * Doom II, which translates ~17k guest instructions a frame).  The pcs of one
 * block are consecutive, so MIN/MAX over them is MIN/MAX of the first and the
 * last -- unless the block crosses a region or wraps a mirror boundary, where
 * the masked offsets are not monotonic; those take the original
 * per-instruction path.  The limits are read only by
 * flush_translation_cache_ram(), so they are committed exactly where a flush
 * can happen: before the cache-full flush inside the emit loop (covering the
 * instructions translated so far, as before) and right after the loop,
 * before the exit lookups that can translate (and flush) recursively. */
#if defined(XLAT_FAST) && !defined(XLAT_NO_LIMITS)
#define XLAT_FAST_LIMITS 1
static void note_pc_limits_range(u32 first, u32 last, u32 width)
{
  u32 iw = first >= 0x3000000;
  if ((last >= 0x3000000) == iw) {
    u32 mask = iw ? 0x7FFF : 0x3FFFF;
    u32 lo = first & mask, hi = last & mask;
    if (hi >= lo && (last - first) == (hi - lo)) {
      if (iw) {
        iwram_code_min = MIN(lo, iwram_code_min);
        iwram_code_max = MAX(hi, iwram_code_max);
      } else {
        ewram_code_min = MIN(lo, ewram_code_min);
        ewram_code_max = MAX(hi, ewram_code_max);
      }
      return;
    }
  }
  {
    const bool ram_region = true;
    u32 pc;
    for (pc = first; ; pc += width) {
      update_pc_limits();
      if (pc == last)
        break;
    }
  }
}
/* lim_first: this block's first pc; lim_last: the latest pc translated
 * (~0U until the first instruction). */
#define pc_limits_step()        do { lim_last = pc; } while (0)
#define pc_limits_commit(width) do {                                         \
  if (ram_region && lim_last != ~0U)                                        \
    note_pc_limits_range(lim_first, lim_last, (width));                     \
} while (0)
#else
#define pc_limits_step()        update_pc_limits()
#define pc_limits_commit(width) do { } while (0)
#endif

/* XLAT_FAST SCANFN: the RAM ARM scan as its own function.  Inlined into
 * translate_block_arm -- a few hundred KB of code with every emitter case
 * live -- the scan loop keeps its state in stack slots (the twin shows a
 * load or store of a spilled local on most of its ~65 instructions per
 * guest instruction).  Out of line it has the registers to itself.  The
 * body is the same scan_block(arm, yes) expansion over the same variables,
 * copied in and out, so its effects (tags, block_data[], block_exits[],
 * the pc-region cache and every counter) are those of the inline scan. */
#if defined(XLAT_FAST) && !defined(XLAT_NO_SCANFN)
#define XLAT_SCANFN_ARM 1
typedef struct {
  u32 pc, block_end_pc, pc_region, new_pc_region;
  u8 *pc_address_block;
  u32 block_exit_position;
  s32 block_data_position;
  u32 opcode, condition, branch_target;
  u32 scan_exit_reason;
#ifdef ENTRY_GATES
  u32 entry_gate_stop;
#endif
} xlat_scan_state;

static __attribute__((noinline)) void xlat_scan_arm_ram(xlat_scan_state *st)
{
  const u32 pc = st->pc;
  u32 block_end_pc = st->block_end_pc;
  u32 pc_region = st->pc_region, new_pc_region = 0;
  u8 *pc_address_block = st->pc_address_block;
  u32 block_exit_position = st->block_exit_position;
  s32 block_data_position = st->block_data_position;
  /* written before they are read, in the scan and after it */
  u32 opcode = 0, condition = 0, branch_target = 0;
  u32 scan_exit_reason = st->scan_exit_reason;
#ifdef ENTRY_GATES
  u32 entry_gate_stop = st->entry_gate_stop;
#endif
  s32 i;

  scan_block(arm, yes);

  st->block_end_pc = block_end_pc;
  st->pc_region = pc_region;
  st->new_pc_region = new_pc_region;
  st->pc_address_block = pc_address_block;
  st->block_exit_position = block_exit_position;
  st->block_data_position = block_data_position;
  st->opcode = opcode;
  st->condition = condition;
  st->branch_target = branch_target;
  st->scan_exit_reason = scan_exit_reason;
#ifdef ENTRY_GATES
  st->entry_gate_stop = entry_gate_stop;
#endif
}
#endif

/* JIT_COST scan pieces (docs/JIT-COST.md): a second RAM ARM scan,
 * xlat_scan_arm_ram_fast, chosen at run time by xlat_scan_fast (harness key
 * of the same name), so one binary A/Bs it against xlat_scan_arm_ram with the
 * code layout held fixed.  Its body is xlat_scan_arm_ram's, word for word;
 * only the scan's hook macros differ, and each replacement computes exactly
 * what the default computes.  The twin's per-address profile put the scan at
 * ~55 host instructions per guest instruction (Doom II), ~12 of them the gate
 * bitmap test and ~12-15 the flag-write switch.
 *
 * SCANWIN: the gate test (SMC gate bitmap, and the entry-gate bitmap when
 * armed) is skipped while block_end_pc stays in a 64-byte window whose four
 * bitmap bytes are all zero -- 16 ARM instructions per window.  Only a window
 * that holds a gate pays the per-instruction test, which is then the original
 * one.  Exact: a zero byte fails both bitmap tests for every pc it covers,
 * and neither map changes during a scan (see GATEMAP).  The window key is
 * block_end_pc >> 6, so it also changes when the region does.
 *
 * SCANCOND: arm_set_condition's switch ORs 0x20 into the condition when the
 * instruction writes the flags.  Its answer depends only on opcode bits
 * 20..27 (the case) and 4..7 (the tests inside the cases), so it is one bit
 * of a 256 x 16 table -- built at first use BY RUNNING THE SAME MACRO over
 * every combination of those bits, so it cannot disagree with it; twin
 * builds also check it against the macro on random opcodes at build time. */
#if defined(JIT_COST) && defined(XLAT_SCANFN_ARM) && \
    defined(SMC_GATE_BITMAP) && defined(SMC_GATES) && !defined(XLAT_NO_GATEMAP) && \
    (!defined(JIT_COST_NO_SCANWIN) || !defined(JIT_COST_NO_SCANCOND))
#define JIT_SCAN_FAST 1
u32 xlat_scan_fast = 1;

#ifndef JIT_COST_NO_SCANCOND
static u16 xlat_arm_flagset[256];
static u32 xlat_arm_flagset_ready;
/* arm_set_condition's 0x20, for any (28-bit) opcode, from the macro itself */
static u32 xlat_arm_flagset_ref(u32 opcode)
{
  s32 block_data_position = 0;
  u8 saved = block_data[0].condition;
  u32 r;
  arm_set_condition(0);
  r = block_data[0].condition & 0x20;
  block_data[0].condition = saved;
  return r;
}
static void xlat_arm_flagset_build(void)
{
  u32 idx, nib;
  for (idx = 0; idx < 256; idx++) {
    u32 m = 0;
    for (nib = 0; nib < 16; nib++)
      if (xlat_arm_flagset_ref((idx << 20) | (nib << 4)))
        m |= 1u << nib;
    xlat_arm_flagset[idx] = (u16)m;
  }
#ifdef DRPROF_TWIN
  {
    /* The table's premise, checked: no other opcode bit matters. */
    u32 x = 0x12345678u, k;
    for (k = 0; k < 200000; k++) {
      u32 op;
      x = x * 1664525u + 1013904223u;
      op = x & 0x0FFFFFFFu;
      if ((((xlat_arm_flagset[(op >> 20) & 0xFF] >> ((op >> 4) & 0xF)) & 1)
           << 5) != xlat_arm_flagset_ref(op)) {
        fprintf(stderr, "JIT_COST SCANCOND table mismatch at %07x\n",
                (unsigned)op);
        abort();
      }
    }
  }
#endif
  xlat_arm_flagset_ready = 1;
}
#ifdef DRPROF_TWIN
/* Twin only: build (and self-test) before main, so the check is not counted
 * in a measured segment.  The PSP builds the table at the first scan. */
__attribute__((constructor)) static void xlat_arm_flagset_twin_init(void)
{
  xlat_arm_flagset_build();
}
#endif
#endif

static __attribute__((noinline)) void xlat_scan_arm_ram_fast(xlat_scan_state *st)
{
  const u32 pc = st->pc;
  u32 block_end_pc = st->block_end_pc;
  u32 pc_region = st->pc_region, new_pc_region = 0;
  u8 *pc_address_block = st->pc_address_block;
  u32 block_exit_position = st->block_exit_position;
  s32 block_data_position = st->block_data_position;
  /* written before they are read, in the scan and after it */
  u32 opcode = 0, condition = 0, branch_target = 0;
  u32 scan_exit_reason = st->scan_exit_reason;
#ifdef ENTRY_GATES
  u32 entry_gate_stop = st->entry_gate_stop;
#endif
  s32 i;

#ifndef JIT_COST_NO_SCANCOND
  if (!xlat_arm_flagset_ready)
    xlat_arm_flagset_build();
#undef SCAN_SET_CONDITION
#define SCAN_SET_CONDITION(type, c)                                           \
  block_data[block_data_position].condition = (c) |                           \
    (((xlat_arm_flagset[(opcode >> 20) & 0xFF] >> ((opcode >> 4) & 0xF)) & 1) \
     << 5)
#endif
#ifndef JIT_COST_NO_SCANWIN
#undef SCAN_VARS
#undef SCAN_GATE_END
#define SCAN_VARS                                                             \
  XLAT_GATEMAP_VARS                                                           \
  u32 xw_key = ~0U, xw_any = 0;
#define SCAN_GATE_END(tag_gate_op) do {                                       \
  if ((block_end_pc >> 6) != xw_key) {                                        \
    u32 xw_r = block_end_pc >> 24;                                            \
    xw_key = block_end_pc >> 6;                                               \
    if (xw_r != xg_reg) {                                                     \
      xg_reg = xw_r;                                                          \
      xlat_gate_maps(xw_r, &xg_smc, &xg_ent, &xg_mask);                       \
    }                                                                         \
    xw_any = 0;                                                               \
    if (xg_smc) {                                                             \
      u32 xw_b = ((block_end_pc & xg_mask) >> 4) & ~3u;                       \
      xw_any = xg_smc[xw_b] | xg_smc[xw_b + 1] |                              \
               xg_smc[xw_b + 2] | xg_smc[xw_b + 3];                           \
      if (xg_ent)                                                             \
        xw_any |= xg_ent[xw_b] | xg_ent[xw_b + 1] |                           \
                  xg_ent[xw_b + 2] | xg_ent[xw_b + 3];                        \
    }                                                                         \
  }                                                                           \
  if (xw_any)                                                                 \
    SMC_SCAN_GATE_END(tag_gate_op);                                           \
} while (0)
#endif

  scan_block(arm, yes);

  /* back to the defaults for every other expansion */
#undef SCAN_VARS
#undef SCAN_SET_CONDITION
#undef SCAN_GATE_END
#define SCAN_VARS                       XLAT_GATEMAP_VARS
#define SCAN_SET_CONDITION(type, c)     type##_set_condition(c)
#define SCAN_GATE_END(tag_gate_op)      SMC_SCAN_GATE_END(tag_gate_op)

  st->block_end_pc = block_end_pc;
  st->pc_region = pc_region;
  st->new_pc_region = new_pc_region;
  st->pc_address_block = pc_address_block;
  st->block_exit_position = block_exit_position;
  st->block_data_position = block_data_position;
  st->opcode = opcode;
  st->condition = condition;
  st->branch_target = branch_target;
  st->scan_exit_reason = scan_exit_reason;
#ifdef ENTRY_GATES
  st->entry_gate_stop = entry_gate_stop;
#endif
}
#endif


#ifdef SMC_IMM_PATCH
/* SMC_IMM_PATCH (xlat-cost 2026-10-28; twin prototype, OFF by default).
 *
 * WHAT IT IS FOR.  Doom II, Duke Nukem Advance and Ice Nine rewrite the
 * immediate field of a handful of ARM data-processing instructions in their
 * IWRAM renderers, thousands of times a second (twin census, 1200 f: Doom
 * 33k of 45k single-store retirements are an imm-only change to one of 21
 * words; Duke 22k of 32k / 17 words; Ice 18k of 24k / 23 words).  Each one
 * retires the covering block and the next entry retranslates it: that is
 * most of their xlat + flush time (Doom 21 % of core work).
 *
 * THE MECHANISM, general (no game key, no table of addresses):
 *  1. LEARN.  When a single-store trap at IWRAM word w RETIRES (no
 *     same-value skip) and the change is one a patch could have served --
 *     the word the translations read (SMC_SAME_SKIP record, both halves,
 *     unpoisoned) and the word now are both patchable (immp_class_ok: ARM
 *     DP immediate, not MSR, Rd/Rn != pc) and differ only in bits 11:0 --
 *     w's bit is set.  (Learning any retire to a DP-imm word learned 3386
 *     words on Top Gear Rally and 751 on Mario Golf for no patch at all.)  The retirement that follows removes every live translation
 *     covering w (if it cannot, the caller full-flushes, which clears
 *     everything here).
 *  2. EMIT.  While w's bit is set, every ARM RAM translation of w emits the
 *     fixed form (mips_emit.h IMMP_OR) and registers the host address of its
 *     lui at commit.  A translation of w that does NOT (the word is no longer
 *     patchable, or a Thumb block covers it) marks w bad.  The emit test is
 *     gated per block (immp_blk_on, set after the scan only when the
 *     block's extent holds a learned word) and the missing-site check runs
 *     at commit, so the per-instruction emit loop carries no hook: per-
 *     instruction hooks cost Top Gear Rally +8 % of translate_block_arm with
 *     nothing learned.
 *  3. PATCH.  At a later single-store trap at w, before anything else: if w
 *     is learned, not bad, and the new word differs from the registered one
 *     only in bits 11:0 (and, where a site's shifter carry is live, keeps the
 *     rotation's carry: rotate zero / non-zero and bit 31 the same), every
 *     registered site's two immediates are rewritten (all sites validated
 *     first; JIT_CODE_WROTE, published by the trap path's own publish), the
 *     SMC_SAME_SKIP record of w is updated, and the trap is handled with no
 *     retirement.  Anything else falls through to the normal path, which
 *     retires and re-learns.
 * Invariant: w learned and not bad => every live translation covering w is a
 * registered fixed-form site.  Bit set only immediately before a retirement
 * of w; a later non-fixed translation of w sets bad; bad and the site list
 * are cleared only by a retirement of w or a full RAM flush.  Sites of
 * retired blocks stay in the list (the RAM cache is append-only between full
 * flushes, and a retired body is never entered again), so they are patched
 * too, harmlessly; each site is validated (exact lui/ori pattern with the
 * registered immediate) before any is written.
 *
 * DETERMINISM.  Everything here is reset at every full RAM flush (which
 * every state load, reset and ROM load performs), so the emitted code is a
 * function of the guest's history since its last flush, as before.  Guest
 * timing still moves wherever translation volume moves the full-flush
 * points (ram-cache-pressure-codegen); emulated semantics do not.
 *
 * LAYOUT.  The emitted code of a game that never rewrites an immediate is
 * byte-identical; the helpers are out of line, outside the pinned order.
 * translate_block_arm itself grows (IMMP_OR's second emit path, twin +11.6
 * KB) and flush_translation_cache_ram (immp_reset), both pinned in hot.ord:
 * a hardware build needs the layout regenerated. */
#ifndef JIT_CODE_DISCIPLINE
#error "SMC_IMM_PATCH records its code patches through JIT_CODE_DISCIPLINE"
#endif
#define IMMP_K      16
#define IMMP_SLOTS  255
static u8 immp_bit[0x8000 / 32], immp_bad[0x8000 / 32];
static u8 immp_slot[0x8000 / 4];
typedef struct
{
  u32 old;                 /* the ARM word every registered site encodes  */
  u32 n;
  u8 *site[IMMP_K];        /* host address of the `lui a0`                */
  u8 *esc[IMMP_K];         /* EXEC_STALE_CHECK's opcode lui (twin), or 0  */
  u8 cl[IMMP_K];           /* 1 = shifter carry live at that site         */
} immp_ent;
static immp_ent immp_tab[IMMP_SLOTS + 1];
static u32 immp_nslots, immp_dirty;
u32 immp_patches, immp_learned, immp_refused[8], immp_sites, immp_selftest_fail;
static inline u32 immp_rot(u32 op);
static inline int immp_site_ok(u8 *p, u32 v, u32 rlui);
static u32 immp_esc_pc;
/* 1 while translate_block_arm emits a RAM block whose extent holds a learned
 * word (set once after the scan): every per-instruction hook tests it first,
 * so blocks without one translate as before at the cost of one load. */
static u32 immp_blk_on;
static u8 *immp_cur_esc;
static struct { u8 *host, *esc; u32 pc, op; u8 cl; } immp_pend[MAX_BLOCK_SIZE];
static u32 immp_npend;
#define IMMP_W(pc)       (((pc) & 0x7FFF) >> 2)
#define IMMP_GET(m, w)   ((m)[(w) >> 3] & (1u << ((w) & 7)))
#define IMMP_SET(m, w)   ((m)[(w) >> 3] |= (u8)(1u << ((w) & 7)))
#define IMMP_CLR(m, w)   ((m)[(w) >> 3] &= (u8)~(1u << ((w) & 7)))
#define IMMP_LEARNED(pc) (((pc) >> 24) == 3 && IMMP_GET(immp_bit, IMMP_W(pc)))

static inline int immp_class_ok(u32 op)
{
  u32 opc = (op >> 21) & 0xF;
  if (((op >> 25) & 7) != 1 || (op >> 28) == 0xF)
    return 0;
  if (opc >= 8 && opc <= 11 && !(op & (1u << 20)))
    return 0;                                   /* MSR imm / undefined */
  if (((op >> 12) & 0xF) == 15 || ((op >> 16) & 0xF) == 15)
    return 0;
  return 1;
}
/* emit-time: is the instruction being emitted (at guest `pc`) a site? */
static inline int immp_want(u32 pc, u32 rd, u32 rn)
{
  return immp_blk_on && rd != 15 && rn != 15 && IMMP_LEARNED(pc);
}
static inline int immp_want_op(int ram_region, u32 pc, u32 op)
{
  (void)ram_region;
  return immp_blk_on && IMMP_LEARNED(pc) && immp_class_ok(op);
}
#if defined(EXEC_STALE_CHECK) || defined(LAB_LOCKSTEP)
static void immp_note_op(u8 *host, u32 pc)  /* EXEC_STALE_CHECK's load */
{
  immp_cur_esc = host;
  immp_esc_pc = pc;
}
#endif
static void immp_note(u8 *host, u32 pc, u32 op, u32 cl)
{
  if (immp_npend < MAX_BLOCK_SIZE) {
    immp_pend[immp_npend].host = host;
    immp_pend[immp_npend].esc = immp_esc_pc == pc ? immp_cur_esc : NULL;
    immp_pend[immp_npend].pc = pc;
    immp_pend[immp_npend].op = op;
    immp_pend[immp_npend].cl = (u8)cl;
    immp_npend++;
  }
  immp_cur_esc = NULL;
}
/* after the scan: does [start, end) of a RAM block hold a learned word? */
static void immp_blk_begin(int ram_region, u32 start, u32 end)
{
  u32 a;
  immp_blk_on = 0;
  if (!ram_region || !immp_dirty || (start >> 24) != 3)
    return;
  for (a = start & ~3u; a < end; a += 4)
    if (IMMP_LEARNED(a)) { immp_blk_on = 1; return; }
}
static void immp_bad_range(u32 start, u32 end)   /* a Thumb block's extent */
{
  u32 a;
  if ((start >> 24) != 3)
    return;
  for (a = start & ~3u; a < end; a += 4)
    if (IMMP_LEARNED(a))
      IMMP_SET(immp_bad, IMMP_W(a));
}
/* translate_block_arm's commit: register the sites it emitted.  A learned
 * word of the block's extent that got no site (not emitted in the fixed
 * form: a flag-only op, a different encoding path, a full pend list) leaves
 * a translation a patch cannot reach: the word goes bad, so it retires. */
#ifdef SMC_REUSE
/* SMC_REUSE x SMC_IMM_PATCH (cand2, 2026-10-28).  Reuse revives a recorded
 * body after comparing only the guest bytes it was translated from; this
 * switch rewrites bodies in place and keeps a list of the live fixed-form
 * sites of every learned word.  Two rules keep both contracts:
 *  R1 (record).  A block whose extent held a learned word when it was
 *     translated -- the only kind that can hold a fixed-form site, and so the
 *     only kind a patch ever writes -- is never recorded.  Every recorded
 *     body is therefore byte for byte what the translator emitted from the
 *     record's guest bytes, whatever was patched since.
 *  R2 (revive).  A record whose extent holds a learned word NOW is not
 *     revived: the block is translated, so the fixed-form sites are emitted
 *     and registered (or the word goes bad) exactly as without reuse, and the
 *     invariant "learned and not bad => every live translation covering w is
 *     a registered site" survives revivals.  (Learned state is cleared at
 *     every flush, virtual ones included, so R2 bites within an epoch only.)
 * Both are tests on IMMP state only: they turn a revival into the
 * translation that would have happened anyway, so reuse stays exact
 * (smc_reuse_vptr is advanced by the translation as usual).
 * SMC_REUSE_IMMP_MUTANT (twin oracle self-test only) drops both rules. */
static u32 immp_blk_learned;    /* the last ARM RAM commit held a learned word */
u32 immp_reuse_norec, immp_reuse_nofind;
static int immp_covers(u32 start, u32 end)
{
  u32 a;
  if (!immp_dirty || (start >> 24) != 3)
    return 0;
  for (a = start & ~3u; a < end; a += 4)
    if (IMMP_LEARNED(a))
      return 1;
  return 0;
}
#endif
static void immp_commit(u32 start, u32 end)
{
  u32 k, a;
#ifdef SMC_REUSE
  immp_blk_learned = immp_blk_on;
#endif
  if (!immp_blk_on) {
    immp_npend = 0;
    return;
  }
  immp_blk_on = 0;
  for (a = start & ~3u, k = 0; a < end; a += 4) {
    while (k < immp_npend && immp_pend[k].pc < a)
      k++;
    if (IMMP_LEARNED(a) && !(k < immp_npend && immp_pend[k].pc == a))
      IMMP_SET(immp_bad, IMMP_W(a));
  }
  for (k = 0; k < immp_npend; k++) {
    u32 w = IMMP_W(immp_pend[k].pc), sl = immp_slot[w];
    immp_ent *e;
    if (!sl) {
      if (immp_nslots >= IMMP_SLOTS) { IMMP_SET(immp_bad, w); continue; }
      sl = immp_slot[w] = (u8)++immp_nslots;
      immp_tab[sl].n = 0;
    }
    e = &immp_tab[sl];
    if (!e->n)
      e->old = immp_pend[k].op;
    if (e->old != immp_pend[k].op || e->n >= IMMP_K) {
      IMMP_SET(immp_bad, w);
      continue;
    }
#ifdef DRPROF_TWIN
    /* self-test: the fixed form encodes what immp_rot says (the patch and
     * its validation both rely on immp_rot == the translator's ror) */
    if (!immp_site_ok(immp_pend[k].host, immp_rot(immp_pend[k].op),
                      (0x0Fu << 26) | ((u32)reg_a0 << 16)))
      immp_selftest_fail++;
#endif
    e->site[e->n] = immp_pend[k].host;
    e->esc[e->n] = immp_pend[k].esc;
    e->cl[e->n] = immp_pend[k].cl;
    e->n++;
    immp_sites++;
  }
  immp_npend = 0;
}
static void immp_reset(void)          /* every full RAM flush */
{
  immp_npend = 0;
  if (!immp_dirty)
    return;
  memset(immp_bit, 0, sizeof(immp_bit));
  memset(immp_bad, 0, sizeof(immp_bad));
  memset(immp_slot, 0, sizeof(immp_slot));
  immp_nslots = 0;
  immp_dirty = 0;
}
/* a single-store trap at `word` is about to retire: learn it */
static void immp_learn(u32 word)
{
  u32 w;
  if ((word >> 24) != 3)
    return;
  w = IMMP_W(word);
  if (IMMP_GET(immp_bit, w)) {
    /* every covering translation is about to go: so do its sites */
    IMMP_CLR(immp_bad, w);
    if (immp_slot[w])
      immp_tab[immp_slot[w]].n = 0;
    return;
  }
  {
    /* learn only what a patch could have served: the word the translations
     * read (xs record, both halves, unpoisoned) and the word now are both
     * patchable DP-immediate forms differing only in bits 11:0 */
    u32 h = (word & 0x7FFC) >> 1, hb = 3u << (h & 7);
    u32 nw = *(u32 *)(iwram + 0x8000 + (word & 0x7FFC)), ow, x;
    if ((xs_ok[h >> 3] & hb) != hb || (xs_bad[h >> 3] & hb))
      return;
    ow = xs_val[h] | ((u32)xs_val[h + 1] << 16);
    x = ow ^ nw;
    if (!x || (x & ~0xFFFu) || !immp_class_ok(ow) || !immp_class_ok(nw))
      return;
  }
  {
    IMMP_SET(immp_bit, w);
    immp_dirty = 1;
    immp_learned++;
  }
}
static inline u32 immp_rot(u32 op)
{
  u32 r = ((op >> 8) & 0xF) * 2, v = op & 0xFF;
  return r ? (v >> r) | (v << (32 - r)) : v;
}
static inline int immp_site_ok(u8 *p, u32 v, u32 rlui)
{
  u32 a = ((u32 *)p)[0], b = ((u32 *)p)[1];
  return (a & 0xFFFF0000u) == rlui && (a & 0xFFFF) == (v >> 16) &&
         (b & 0xFFFF0000u) == ((rlui & 0x001F0000u) | (0x0Du << 26) |
                               ((rlui & 0x001F0000u) << 5)) &&
         (b & 0xFFFF) == (v & 0xFFFF);
}
static inline void immp_site_put(u8 *p, u32 v)
{
  u32 *q = (u32 *)p;
  q[0] = (q[0] & 0xFFFF0000u) | (v >> 16);
  q[1] = (q[1] & 0xFFFF0000u) | (v & 0xFFFF);
  JIT_CODE_WROTE(p, p + 8);
}
#if defined(STALE_CHECK)
static void stale_note_extent(u32 start_pc, u32 end_pc, u32 thumb);
#endif
/* the trap at IWRAM `word`: 1 = handled by patching (no retirement) */
static int immp_try(u32 word)
{
  u32 w, nw, ow, k, vo, vn;
  immp_ent *e;
  const u32 lui_a0 = (0x0Fu << 26) | ((u32)reg_a0 << 16);
  const u32 lui_a2 = (0x0Fu << 26) | ((u32)reg_a2 << 16);
  if ((word >> 24) != 3)
    return 0;
  w = IMMP_W(word);
  if (!IMMP_GET(immp_bit, w))
    return 0;
  if (IMMP_GET(immp_bad, w) || !immp_slot[w] || !immp_tab[immp_slot[w]].n) {
    immp_refused[0]++;
    return 0;
  }
  e = &immp_tab[immp_slot[w]];
  nw = *(u32 *)(iwram + 0x8000 + (word & 0x7FFC));
  ow = e->old;
  if (nw == ow) { immp_refused[1]++; return 0; }      /* xs path's case */
  if ((nw ^ ow) & ~0xFFFu) { immp_refused[2]++; return 0; }
  vo = immp_rot(ow);
  vn = immp_rot(nw);
  for (k = 0; k < e->n; k++) {
    if (e->cl[k] && ((((ow >> 8) & 0xF) != 0) != (((nw >> 8) & 0xF) != 0) ||
                     (((ow >> 8) & 0xF) && ((vo ^ vn) >> 31)))) {
      immp_refused[3]++;
      return 0;                     /* the rotation's carry would change */
    }
    if (!immp_site_ok(e->site[k], vo, lui_a0) ||
        (e->esc[k] && !immp_site_ok(e->esc[k], ow, lui_a2))) {
      immp_refused[4]++;
      return 0;
    }
  }
  for (k = 0; k < e->n; k++) {
#ifdef SMC_IMM_PATCH_MUTANT_ONESITE
    if (k) break;      /* MUTANT (oracle self-test only): miss every other site */
#endif
#ifdef SMC_IMM_PATCH_MUTANT_ALU
    /* MUTANT (oracle self-test only): the first site's ALU immediate is
     * written wrong (bit 0 flipped) while its opcode load is right: invisible
     * to the EXEC_STALE opcode compare, visible to IMMP_VERIFY. */
    immp_site_put(e->site[k], k ? vn : vn ^ 1);
#else
    immp_site_put(e->site[k], vn);
#endif
    if (e->esc[k])
      immp_site_put(e->esc[k], nw);
  }
  e->old = nw;
#if defined(SMC_SAME_SKIP) && defined(SMC_PARTIAL_SINGLE)
  {
    /* every live block covering w now encodes nw: so must the record */
    u32 h = (word & 0x7FFC) >> 1;
    if (xs_ready) {
      xs_val[h] = (u16)nw;
      xs_val[h + 1] = (u16)(nw >> 16);
    }
  }
#endif
#if defined(STALE_CHECK)
  {
    /* re-hash every live ARM block covering w (STALE_CHECK's record) */
    u16 *tagp = (u16 *)iwram;
    u32 lo = (word & 0x7FFC) >> 1, i0 = lo > 2 * MAX_BLOCK_SIZE ? lo - 2 * MAX_BLOCK_SIZE : 0, i;
    for (i = i0; i <= lo; i++)
      if (VALID_TAG(tagp[i])) {
        ramtag_type *te = get_ram_tag(tagp[i]);
        u32 st = 0x03000000 + (i << 1);
        if (SMC_RAM_LIVE_OFFSET(te, arm) && te->stale_end_arm > (word & ~3u))
          stale_note_extent(st, te->stale_end_arm, 0);
      }
  }
#endif
  jit_code_publish();
  immp_patches++;
  return 1;
}
#ifdef DRPROF_TWIN
static void immp_report(void)
{
  fprintf(stderr, "SMC_IMM_PATCH patches=%u learned=%u sites=%u selftest_fail=%u refused=%u/%u/%u/%u/%u\n",
          (unsigned)immp_patches, (unsigned)immp_learned, (unsigned)immp_sites,
          (unsigned)immp_selftest_fail,
          (unsigned)immp_refused[0], (unsigned)immp_refused[1],
          (unsigned)immp_refused[2], (unsigned)immp_refused[3],
          (unsigned)immp_refused[4]);
#ifdef SMC_REUSE
  fprintf(stderr, "SMC_IMM_PATCH x SMC_REUSE norec=%u nofind=%u%c",
          (unsigned)immp_reuse_norec, (unsigned)immp_reuse_nofind, 10);
#endif
}
__attribute__((constructor)) static void immp_report_init(void)
{
  atexit(immp_report);
}
#endif
#else
#endif
#ifdef SPIN_SKIP
/* SPIN_SKIP (cpu-levers lever 4, 2026-10-26): fast-forward a provably
 * side-effect-free polling loop to the next scheduled event.
 *
 * WHAT IS A SPIN LOOP.  A backward branch B (B, or Thumb B/Bcc; never BL/SWI)
 * whose target T lies in the same block, with every instruction in [T, B)
 * drawn from: loads (no writeback), data processing, flag-setting compares,
 * and nothing else -- no store, no call, no SWI, no mode/PSR access, no write
 * to pc.  Two register rules make one iteration a pure function of memory:
 *   1. every register (and every one of N, Z, C, V) READ in the loop is
 *      either never written in it, or written earlier in the same iteration;
 *      a conditional ARM instruction or a partial flag write counts as a read
 *      of what it may leave unchanged;
 *   2. every register a load ADDRESS uses is never written in the loop --
 *      or (claude/idle-general) is set earlier in the same iteration to a
 *      constant, a ROM literal-pool word or an immediate, so the address is
 *      that constant (`ldr r0, =FLAG; ldrh r1, [r0]` inside the loop).
 * Then, as long as no memory the loop reads changes, every iteration leaves
 * exactly the state the previous one left, and the loop never exits.
 *
 * TWO MORE SHAPES (claude/idle-general, 2026-10-27), both still one straight
 * run [T, B) inside the block being translated:
 *  - EXIT IN THE MIDDLE: the body may hold conditional B/Bcc whose target is
 *    outside [T, B].  Taken, it leaves the loop (the block exits there); not
 *    taken, it only read its flags (rule 1 applies).  B is reached only when
 *    every one of them fell through, so a pass through B is still a whole
 *    iteration of the one path analysed.  A target INSIDE [T, B] would make
 *    the path actually run differ from that one: rejected (synth
 *    spin_inskip.s; SPIN_SKIP_MUTANT_INSKIP drops the check).
 *  - THREADED BACK-EDGE: B branches FORWARD to an unconditional `b T`
 *    (`beq 1f; b out; 1: b T` -- what compilers emit for a wait whose exit
 *    is the fall-through, Final Fantasy Tactics Advance's main wait).  B is
 *    then the loop's back-edge and the `b` a trampoline.  Only in ROM: the
 *    trampoline lies outside this block, so a RAM store could change it
 *    without retiring this translation.
 *
 * WHY THE NEXT EVENT.  Between two scheduled events (update_gba's slice: the
 * next video event, a non-lazy timer, serial, DMA sleep) nothing but the CPU
 * writes memory -- IRQs are raised, and DMAs started, only inside update_gba
 * -- and the loop does not write.  So the remaining iterations of the slice
 * are exact repeats and the loop may end the slice now (reg_cycles = 0, the
 * same thing a per-game idle_loop_target_pc does).  Timing moves by less than
 * one iteration: the loop reaches the event exactly instead of overshooting.
 *
 * THE READ SET (runtime, spin_check).  The rule above is about memory only an
 * event can change.  Some locations change without one: lazy timer counters,
 * serial, the cartridge GPIO/EEPROM/flash, BIOS open bus.  So the
 * loop's load addresses (rule 2 makes them fixed for the loop) are checked
 * -- a constant one at translation, a register-based one whenever it spins
 * (remembered once passed): RAM, palette, VRAM, OAM and ROM pass; IO passes only
 * for DISPSTAT, VCOUNT, IE, IF, IME and the DMA control words, which change
 * only at events or on CPU stores, and KEYINPUT/KEYCNT: this core latches the
 * pad once per frame, between two slices (update_input from retro_run,
 * gbalink_update_input at a link frame's start).  A loop that polls anything else -- a timer
 * counter, say -- is not a spin loop (a constant address) or is vetoed for
 * the life of its translation, and runs as before, iteration by iteration,
 * to the cycle.
 *
 * A loop with no loads (`b .`) is the degenerate case: only an IRQ ends it.
 * Not keyed on any address or ROM.  The descriptor (veto word + loads) is
 * emitted into the translation cache right after the branch, so it lives and
 * dies with the block. */

#define SPIN_MAX_BODY   16
#define SPIN_MAX_LOADS  4
#define SPIN_FN 16
#define SPIN_FZ 17
#define SPIN_FC 18
#define SPIN_FV 19
#define SPIN_FLAGS_NZCV (0xFu << 16)
#define SPIN_FLAGS_NZ   ((1u << SPIN_FN) | (1u << SPIN_FZ))

/* descriptor: [0] veto (0 = may skip), [1] number of loads,
 * then per load: w0 = kind | size_log2 << 2 | rb << 4 | ro << 8 |
 *                     shift << 12 | negate << 17,  w1 = imm (or address),
 *                w2 = the last address spin_check passed (0: none yet)
 * kind 0: address w1; 1: reg[rb] + w1; 2: reg[rb] +/- (reg[ro] << shift) */
#define SPIN_DESC_WORDS (2 + 3 * SPIN_MAX_LOADS)

u32 spin_skips = 0, spin_vetoes = 0, spin_loops = 0;

typedef struct {
  u32 all_w;      /* everything written anywhere in the body (pass 1) */
  u32 w;          /* written so far in this iteration (pass 2) */
  u32 bad;
  u32 nloads;
  u32 *desc;
  u32 pass;
  u32 kc;         /* registers holding a known constant here (pass 2) */
  u32 kv[16];     /* ... and their values */
  u8 *blk;        /* the block's page, for literal pools */
  u32 page;       /* its pc >> 15 */
} spin_ana;

static inline void spin_read(spin_ana *a, u32 mask)
{
#ifndef SPIN_SKIP_MUTANT_REGS
  if (a->pass == 2 && (mask & a->all_w & ~a->w))
    a->bad = 1;
#else
  (void)a; (void)mask;   /* MUTANT (twin tests only): no register rule */
#endif
}

static inline void spin_write(spin_ana *a, u32 mask)
{
  if (a->pass == 1) a->all_w |= mask;
  else { a->w |= mask; a->kc &= ~mask; }
}

/* rd now holds k on every iteration (called after its SPIN_W). */
static inline void spin_const(spin_ana *a, u32 rd, u32 k)
{
  if (a->pass == 2) { a->kc |= 1u << rd; a->kv[rd] = k; }
}

/* A word literal at lit, if it is cartridge ROM in this block's page (never
 * the GPIO words): the same value on every pass. */
static void spin_literal(spin_ana *a, u32 rd, u32 lit)
{
  u32 off = lit & 0x01FFFFFF;
  if ((lit >> 24) >= 0x08 && (lit >> 24) <= 0x0D && !(lit & 3) &&
      (lit >> 15) == a->page && (off + 4 <= 0xC4 || off >= 0xCA))
    spin_const(a, rd, readaddress32(a->blk, (lit & 0x7FFF)));
}

static inline u32 spin_cond_flags(u32 cond)
{
  switch (cond) {
    case 0x0: case 0x1: return 1u << SPIN_FZ;
    case 0x2: case 0x3: return 1u << SPIN_FC;
    case 0x4: case 0x5: return 1u << SPIN_FN;
    case 0x6: case 0x7: return 1u << SPIN_FV;
    case 0x8: case 0x9: return (1u << SPIN_FC) | (1u << SPIN_FZ);
    case 0xA: case 0xB: return (1u << SPIN_FN) | (1u << SPIN_FV);
    case 0xC: case 0xD: return SPIN_FLAGS_NZCV & ~(1u << SPIN_FC);
    default: return 0;
  }
}

static int spin_addr_ok(u32 addr, u32 size);

/* A load: its address registers must not be written anywhere in the loop
 * -- or must hold a constant set earlier in the same iteration (a ROM
 * literal or an immediate: `ldr r0, =FLAG; ldrh r1, [r0]` inside the loop),
 * and then the address is that constant. */
static void spin_load(spin_ana *a, u32 kind, u32 size_log2, u32 rb, u32 ro,
                      u32 shift, u32 neg, u32 imm)
{
  u32 used = 0;
  if (kind >= 1) used |= 1u << rb;
  if (kind == 2) used |= 1u << ro;
  spin_read(a, used);
  if (a->pass == 1)
    return;
  if (kind >= 1 && (used & a->kc) == used) {
    if (kind == 1)
      imm += a->kv[rb];
    else
      imm = neg ? a->kv[rb] - (a->kv[ro] << shift)
                : a->kv[rb] + (a->kv[ro] << shift);
    kind = 0;
    used = 0;
  }
  if (used & a->all_w)
    a->bad = 1;
  if (kind == 0) {
    /* a fixed address: decided now (a loop polling a timer counter, say,
     * is no spin loop at all), never listed for spin_check */
    if (!spin_addr_ok(imm, 1u << size_log2))
      a->bad = 1;
    return;
  }
  if (a->nloads == SPIN_MAX_LOADS) {
    a->bad = 1;
    return;
  }
  a->desc[2 + 3 * a->nloads] = kind | (size_log2 << 2) | (rb << 4) |
                               (ro << 8) | (shift << 12) | (neg << 17);
  a->desc[3 + 3 * a->nloads] = imm;
  a->desc[4 + 3 * a->nloads] = 0;
  a->nloads++;
}

#define SPIN_R(m)  spin_read(a, (m))
#define SPIN_W(m)  spin_write(a, (m))
#define SPIN_RW(m) do { spin_read(a, (m)); spin_write(a, (m)); } while (0)

static void spin_thumb_op(spin_ana *a, u32 op, u32 pc)
{
  u32 rd = op & 7, rs = (op >> 3) & 7;
  switch (op >> 11) {
    case 0x00: case 0x01: case 0x02:            /* lsl/lsr/asr #imm */
      SPIN_R(1u << rs);
      if ((op >> 11) == 0 && ((op >> 6) & 31) == 0)
        SPIN_RW(1u << SPIN_FC);                 /* lsl #0 keeps C */
      else
        SPIN_W(1u << SPIN_FC);
      SPIN_W((1u << rd) | SPIN_FLAGS_NZ);
      return;
    case 0x03:                                  /* add/sub reg/imm3 */
      SPIN_R(1u << rs);
      if (!(op & 0x0400))
        SPIN_R(1u << ((op >> 6) & 7));
      SPIN_W((1u << rd) | SPIN_FLAGS_NZCV);
      return;
    case 0x04:                                  /* mov #imm8 */
      SPIN_W((1u << ((op >> 8) & 7)) | SPIN_FLAGS_NZ);
      spin_const(a, (op >> 8) & 7, op & 0xFF);
      return;
    case 0x05:                                  /* cmp #imm8 */
      SPIN_R(1u << ((op >> 8) & 7));
      SPIN_W(SPIN_FLAGS_NZCV);
      return;
    case 0x06: case 0x07:                       /* add/sub #imm8 */
      SPIN_R(1u << ((op >> 8) & 7));
      SPIN_W((1u << ((op >> 8) & 7)) | SPIN_FLAGS_NZCV);
      return;
    case 0x08:
      if (!(op & 0x0400)) {                     /* ALU */
        u32 alu = (op >> 6) & 15;
        switch (alu) {
          case 0x0: case 0x1: case 0xC: case 0xE:   /* and eor orr bic */
            SPIN_R((1u << rd) | (1u << rs));
            SPIN_W((1u << rd) | SPIN_FLAGS_NZ);
            return;
          case 0x2: case 0x3: case 0x4: case 0x7:   /* shifts by register */
          case 0xD:                                 /* mul (C: leave as read) */
            SPIN_R((1u << rd) | (1u << rs));
            SPIN_RW(1u << SPIN_FC);
            SPIN_W((1u << rd) | SPIN_FLAGS_NZ);
            return;
          case 0x5: case 0x6:                       /* adc sbc */
            SPIN_R((1u << rd) | (1u << rs) | (1u << SPIN_FC));
            SPIN_W((1u << rd) | SPIN_FLAGS_NZCV);
            return;
          case 0x8:                                 /* tst */
            SPIN_R((1u << rd) | (1u << rs));
            SPIN_W(SPIN_FLAGS_NZ);
            return;
          case 0x9:                                 /* neg */
            SPIN_R(1u << rs);
            SPIN_W((1u << rd) | SPIN_FLAGS_NZCV);
            return;
          case 0xA: case 0xB:                       /* cmp cmn */
            SPIN_R((1u << rd) | (1u << rs));
            SPIN_W(SPIN_FLAGS_NZCV);
            return;
          case 0xF:                                 /* mvn */
            SPIN_R(1u << rs);
            SPIN_W((1u << rd) | SPIN_FLAGS_NZ);
            return;
        }
      } else {                                  /* hi-register ops */
        u32 hd = (op & 7) | ((op >> 4) & 8), hs = (op >> 3) & 15;
        switch ((op >> 8) & 3) {
          case 0:                               /* add */
            if (hd == 15) break;
            SPIN_R((1u << hd) | (1u << hs));
            SPIN_W(1u << hd);
            return;
          case 1:                               /* cmp */
            SPIN_R((1u << hd) | (1u << hs));
            SPIN_W(SPIN_FLAGS_NZCV);
            return;
          case 2:                               /* mov */
            if (hd == 15) break;
            SPIN_R(1u << hs);
            SPIN_W(1u << hd);
            return;
        }
      }
      break;
    case 0x09:                                  /* ldr rd, [pc, #imm] */
      spin_load(a, 0, 2, 0, 0, 0, 0, ((pc + 4) & ~2u) + (op & 0xFF) * 4);
      SPIN_W(1u << ((op >> 8) & 7));
      spin_literal(a, (op >> 8) & 7, ((pc + 4) & ~2u) + (op & 0xFF) * 4);
      return;
    case 0x0A: case 0x0B: {                     /* register offset */
      u32 sub = (op >> 9) & 7, ro = (op >> 6) & 7;
      static const u8 szl[8] = { 0, 0, 0, 0, 2, 1, 0, 1 };
      if (sub < 3) {                            /* str strh strb */
#ifdef SPIN_SKIP_MUTANT_STORE
        /* MUTANT (twin tests only): treat the store as harmless */
        SPIN_R((1u << rd) | (1u << rs) | (1u << ro));
        return;
#endif
        break;
      }
      spin_load(a, 2, szl[sub], rs, ro, 0, 0, 0);
      SPIN_W(1u << rd);
      return;
    }
#ifdef SPIN_SKIP_MUTANT_STORE
    case 0x0C: case 0x0E: case 0x10:            /* MUTANT: str #imm */
      SPIN_R((1u << rd) | (1u << rs));
      return;
    case 0x12:
      SPIN_R((1u << ((op >> 8) & 7)) | (1u << 13));
      return;
#endif
    case 0x0D: case 0x0F:                       /* ldr / ldrb #imm */
      spin_load(a, 1, (op >> 11) == 0x0D ? 2 : 0, rs, 0, 0, 0,
                ((op >> 6) & 31) << ((op >> 11) == 0x0D ? 2 : 0));
      SPIN_W(1u << rd);
      return;
    case 0x11:                                  /* ldrh #imm */
      spin_load(a, 1, 1, rs, 0, 0, 0, ((op >> 6) & 31) << 1);
      SPIN_W(1u << rd);
      return;
    case 0x13:                                  /* ldr rd, [sp, #imm] */
      spin_load(a, 1, 2, 13, 0, 0, 0, (op & 0xFF) * 4);
      SPIN_W(1u << ((op >> 8) & 7));
      return;
    case 0x14:                                  /* add rd, pc, #imm */
      SPIN_W(1u << ((op >> 8) & 7));
      return;
    case 0x15:                                  /* add rd, sp, #imm */
      SPIN_R(1u << 13);
      SPIN_W(1u << ((op >> 8) & 7));
      return;
  }
  a->bad = 1;
}

static void spin_arm_op(spin_ana *a, u32 op, u32 pc)
{
  u32 cond = op >> 28;
  u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
  u32 partial = 0;

  if (cond == 0xF) { a->bad = 1; return; }
  if (cond != 0xE) {
    SPIN_R(spin_cond_flags(cond));
    partial = 1;                    /* a skipped write leaves the old value */
  }

  if ((op & 0x0E000090) == 0x00000090 && (op & 0x60)) {
    /* ldrh/ldrsb/ldrsh, pre-indexed without writeback */
    u32 sh = (op >> 5) & 3;
    if (!(op & (1u << 20)) || !(op & (1u << 24)) || (op & (1u << 21)) ||
        rd == 15) { a->bad = 1; return; }
    if (op & (1u << 22)) {
      u32 imm = ((op >> 4) & 0xF0) | (op & 0xF);
      if (!(op & (1u << 23))) imm = -imm;
      if (rn == 15) spin_load(a, 0, sh == 2 ? 0 : 1, 0, 0, 0, 0, pc + 8 + imm);
      else spin_load(a, 1, sh == 2 ? 0 : 1, rn, 0, 0, 0, imm);
    } else {
      if (rn == 15 || rm == 15) { a->bad = 1; return; }
      spin_load(a, 2, sh == 2 ? 0 : 1, rn, rm, 0, !(op & (1u << 23)), 0);
    }
    if (partial) SPIN_R(1u << rd);
    SPIN_W(1u << rd);
    return;
  }

  if ((op & 0x0C000000) == 0x00000000 && (op & 0x0E000090) != 0x00000090 &&
      (op & 0x0E000090) != 0x00000090) {
    u32 opc = (op >> 21) & 15, s = (op >> 20) & 1;
    u32 logical = (opc <= 1) || (opc >= 8 && opc <= 9) || opc >= 12;
    u32 writes_rd = !(opc >= 8 && opc <= 11);
    u32 reads_rn = !(opc == 13 || opc == 15);
    u32 c_from_shifter = 0;          /* 1: written, 2: maybe unchanged */
    if (!s && !writes_rd) { a->bad = 1; return; }      /* mrs/msr/bx/... */
    if ((op & 0x0FFFFFF0) == 0x012FFF10) { a->bad = 1; return; }
    if (writes_rd && rd == 15) { a->bad = 1; return; }
    if (reads_rn) SPIN_R(1u << rn);
    if (op & (1u << 25)) {
      c_from_shifter = ((op >> 8) & 15) ? 1 : 2;
    } else {
      u32 type = (op >> 5) & 3;
      SPIN_R(1u << rm);
      if (op & 0x10) {
        SPIN_R(1u << ((op >> 8) & 15));
        c_from_shifter = 2;
      } else if (((op >> 7) & 31) == 0) {
        if (type == 0) c_from_shifter = 2;              /* lsl #0 */
        else if (type == 3) {                           /* rrx */
          SPIN_R(1u << SPIN_FC);
          c_from_shifter = 1;
        } else c_from_shifter = 1;
      } else c_from_shifter = 1;
    }
    if (opc == 5 || opc == 6 || opc == 7)               /* adc sbc rsc */
      SPIN_R(1u << SPIN_FC);
    if (s) {
      u32 fw = logical ? SPIN_FLAGS_NZ : SPIN_FLAGS_NZCV;
      if (logical) {
        if (c_from_shifter == 2) SPIN_RW(1u << SPIN_FC);
        else fw |= 1u << SPIN_FC;
      }
      if (partial) SPIN_R(fw);
      SPIN_W(fw);
    }
    if (writes_rd) {
      if (partial) SPIN_R(1u << rd);
      SPIN_W(1u << rd);
      if (!partial && opc == 13 && (op & (1u << 25))) {     /* mov #imm */
        u32 r = ((op >> 8) & 15) * 2, v = op & 0xFF;
        spin_const(a, rd, r ? (v >> r) | (v << (32 - r)) : v);
      }
    }
    return;
  }

  if ((op & 0x0C000000) == 0x04000000) {
    /* ldr/ldrb, pre-indexed without writeback */
    u32 sz = (op & (1u << 22)) ? 0 : 2;
    if (!(op & (1u << 20)) || !(op & (1u << 24)) || (op & (1u << 21)) ||
        rd == 15) { a->bad = 1; return; }
    if (!(op & (1u << 25))) {
      u32 imm = op & 0xFFF;
      if (!(op & (1u << 23))) imm = -imm;
      if (rn == 15) spin_load(a, 0, sz, 0, 0, 0, 0, pc + 8 + imm);
      else spin_load(a, 1, sz, rn, 0, 0, 0, imm);
    } else {
      if (op & 0x70) { a->bad = 1; return; }    /* lsl #n only */
      if (rn == 15 || rm == 15 || (op & 0x10)) { a->bad = 1; return; }
      spin_load(a, 2, sz, rn, rm, (op >> 7) & 31, !(op & (1u << 23)), 0);
    }
    if (partial) SPIN_R(1u << rd);
    SPIN_W(1u << rd);
    if (!partial && sz == 2 && rn == 15 && !(op & (1u << 25)))
      spin_literal(a, rd, pc + 8 + ((op & (1u << 23)) ? (op & 0xFFF)
                                                      : -(op & 0xFFF)));
    return;
  }

  a->bad = 1;
}

/* A direct B/Bcc at p (never BL, BLX or SWI; p in the block's 32 KiB page):
 * its target, and its condition in *cond (0xE: always); ~0 if p is not one. */
static u32 spin_branch_at(u8 *pc_address_block, u32 p, u32 thumb, u32 *cond)
{
  if (thumb) {
    u32 op = readaddress16(pc_address_block, (p & 0x7FFF));
    if ((op >> 12) == 0xD && ((op >> 8) & 15) < 0xE) {   /* not udf/swi */
      *cond = (op >> 8) & 15;
      return p + 4 + ((u32)(s32)(s8)(op & 0xFF) << 1);
    }
    if ((op >> 11) == 0x1C) {
      *cond = 0xE;
      return p + 4 + (u32)(((s32)(op << 21)) >> 20);
    }
  } else {
    u32 op = readaddress32(pc_address_block, (p & 0x7FFF));
    if ((op & 0x0F000000) == 0x0A000000 && (op >> 28) != 0xF) {
      *cond = op >> 28;
      return p + 8 + (u32)(((s32)(op << 8)) >> 6);
    }
  }
  return ~0u;
}

/* Returns the number of descriptor words (0: not a spin loop). */
static u32 spin_analyze(u32 target, u32 branch_pc, u32 thumb,
                        u8 *pc_address_block, u32 block_start_pc, u32 *desc)
{
  spin_ana an, *a = &an;
  u32 width = thumb ? 2 : 4, p, cond = 0xE, loop, c2;

  if ((block_start_pc >> 15) != (branch_pc >> 15) ||
      spin_branch_at(pc_address_block, branch_pc, thumb, &cond) != target)
    return 0;

  loop = target;
  if (target > branch_pc) {
    /* threaded back-edge: forward to an unconditional `b` into this block */
    if ((branch_pc >> 24) < 0x08 || (branch_pc >> 24) > 0x0D ||
        (target >> 15) != (branch_pc >> 15))
      return 0;
    loop = spin_branch_at(pc_address_block, target, thumb, &c2);
    if (loop == ~0u || c2 != 0xE)
      return 0;
  }
  if (loop < block_start_pc || loop > branch_pc ||
      (branch_pc - loop) / width > SPIN_MAX_BODY)
    return 0;

  memset(a, 0, sizeof(*a));
  a->desc = desc;
  a->blk = pc_address_block;
  a->page = branch_pc >> 15;
  for (a->pass = 1; a->pass <= 2; a->pass++) {
    a->w = 0;
    a->kc = 0;
    for (p = loop; p < branch_pc; p += width) {
      u32 c3, t3 = spin_branch_at(pc_address_block, p, thumb, &c3);
      if (t3 != ~0u) {
        /* exit in the middle: out of [loop, branch_pc] only */
        if (c3 == 0xE)
          return 0;
#ifndef SPIN_SKIP_MUTANT_INSKIP
        if (t3 >= loop && t3 <= branch_pc)
          return 0;
#endif
        spin_read(a, spin_cond_flags(c3));
        continue;
      }
#ifdef SMC_IMM_PATCH
      /* a learned word's immediate can change without a retirement */
      if (!thumb && IMMP_LEARNED(p))
        return 0;
#endif
      if (thumb)
        spin_thumb_op(a, readaddress16(pc_address_block, (p & 0x7FFF)), p);
      else
        spin_arm_op(a, readaddress32(pc_address_block, (p & 0x7FFF)), p);
      if (a->bad)
        return 0;
    }
    /* the back-edge itself reads its condition flags */
    if (cond != 0xE)
      spin_read(a, spin_cond_flags(cond));
    if (a->bad)
      return 0;
  }
  /* No loads at all (`b .`, `cmp r0, #0; bne .`): it can only be left by
   * an interrupt, which only an event raises -- the same case, simpler. */
  desc[0] = 0;
  desc[1] = a->nloads;
  spin_loops++;
  return 2 + 3 * a->nloads;
}

/* May the location [addr, addr+size) change between two events? */
static int spin_addr_ok(u32 addr, u32 size)
{
  u32 region = addr >> 24, off, last;
  addr &= ~(size - 1);
  switch (region) {
    case 0x02: case 0x03: case 0x05: case 0x06: case 0x07:
      return 1;
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C:
      off = addr & 0x01FFFFFF;
      return off + size <= 0xC4 || off >= 0xCA;   /* cartridge GPIO */
    case 0x04:
      off = addr & 0x00FFFFFF;
      last = off + size - 1;
      if (last >= 0x400) return 0;
      /* DISPSTAT VCOUNT | IE IF | IME | KEYINPUT KEYCNT | DMA0-3 CNT_H */
      if (off >= 0x004 && last <= 0x007) return 1;
      if (off >= 0x200 && last <= 0x203) return 1;
      if (off >= 0x208 && last <= 0x209) return 1;
      if (off >= 0x130 && last <= 0x133) return 1;     /* KEYINPUT KEYCNT */
      if ((off & ~1u) == 0x0BA || (off & ~1u) == 0x0C6 ||
          (off & ~1u) == 0x0D2 || (off & ~1u) == 0x0DE)
        return last <= (off | 1);
      return 0;
    default:
      return 0;
  }
}

/* update_gba: may this loop end the slice?  Registers are in reg[].  Runs on
 * every skip -- hundreds a frame for a game waiting on VBlank, with the same
 * registers each time -- so an address that passed is remembered (w2) and
 * passes again without the region test.  Fixed addresses (kind 0) were
 * decided at translation and are not listed. */
int spin_check(u32 *desc)
{
  u32 i, n = desc[1];
  for (i = 0; i < n; i++) {
    u32 *l = &desc[2 + 3 * i], w = l[0], rb = (w >> 4) & 15, addr;
    if ((w & 3) == 1)
      addr = reg[rb] + l[1];
    else {
      u32 x = reg[(w >> 8) & 15] << ((w >> 12) & 31);
      addr = (w & (1u << 17)) ? reg[rb] - x : reg[rb] + x;
    }
    if (addr == l[2])
      continue;
    if (!spin_addr_ok(addr, 1u << ((w >> 2) & 3)))
      return 0;
    l[2] = addr;
  }
  return 1;
}
#endif /* SPIN_SKIP */

/* STATIC_ALU_PC (make STATIC_ALU_PC=1; cpu-levers lever 5, OFF by default).
 *
 * `add pc, pc, #imm` / `sub pc, pc, #imm` (S clear, any condition) jumps to a
 * constant: the pc operand reads as this instruction + 8 and the immediate is
 * fixed.  Compilers emit it as a forward skip (Doom II's column loop at
 * 03002e18: `addcc pc, pc, #20`).  The translator handled it like any write to
 * r15 -- an indirect branch through mips_indirect_branch_arm, which probes the
 * dispatch cache (or calls block_lookup_address_arm) on every taken pass.
 *
 * Here it is emitted as the direct branch a `b`/`bcc` to the same address
 * gets, linked to the block that STARTS at the target -- always an external
 * link, never into the middle of this block.  That keeps it exact:
 *   - the scan is untouched (it already treats these as exit points that end
 *     the block only when unconditional), so block extents, update_cycles
 *     points and block_exits[] are what they were;
 *   - the indirect path entered the block keyed by the target too
 *     (block_lookup_address_arm), so the guest runs the same blocks;
 *   - cycles: conditional -> neither path charges the pending count;
 *     unconditional -> both subtract it first; then both run update_gba with
 *     the target in a0 when the budget is spent (bltzal update_trampoline
 *     here, DISPATCH_CYCLE_CHECK in the stub), which this switch requires.
 * Guest state must therefore be bit-identical with the switch on; only the
 * host path and when the target block is translated (eagerly, as for any
 * direct exit) change.  Not converted: S set (CPSR restore), a target not
 * word-aligned, in another region, or 0, and any past the SALU_MAX_EXITS-th
 * in one block -- all keep the indirect path. */
#ifdef STATIC_ALU_PC
#ifndef DISPATCH_CYCLE_CHECK
#error "STATIC_ALU_PC relies on DISPATCH_CYCLE_CHECK for its cycle equivalence."
#endif
#define SALU_MAX_EXITS 8
u32 salu_converted;    /* exits emitted direct (translation-time count) */
#ifdef DRPROF_TWIN
static void salu_report(void)
{
  fprintf(stderr, "STATIC_ALU_PC converted=%u\n", (unsigned)salu_converted);
}
__attribute__((constructor)) static void salu_report_init(void)
{
  atexit(salu_report);
}
#endif

static inline u32 salu_static_target(u32 opcode, u32 pc, u32 is_add)
{
  u32 imm = opcode & 0xFF, ror = ((opcode >> 8) & 0x0F) * 2, v, t;
  if ((((opcode >> 12) & 0x0F) != REG_PC) || (((opcode >> 16) & 0x0F) != REG_PC))
    return 0;
  v = ror ? ((imm >> ror) | (imm << (32 - ror))) : imm;
  t = is_add ? (pc + 8 + v) : (pc + 8 - v);
#ifdef STATIC_ALU_PC_MUTANT
  t += 4;   /* oracle test only: the pc+8 bias got wrong by one word */
#endif
  if ((t & 3) || (t >> 24) != (pc >> 24))
    return 0;
  return t;
}

#define arm_static_alu_pc(is_add, name)                                       \
{                                                                             \
  u32 salu_t = (salu_n < SALU_MAX_EXITS) ?                                    \
    salu_static_target(opcode, pc, is_add) : 0;                               \
  if (salu_t)                                                                 \
  {                                                                           \
    salu_exits[salu_n].branch_target = salu_t;                                \
    if(condition == 0x0E)                                                     \
    {                                                                         \
      generate_branch_cycle_update(salu_exits[salu_n].branch_source, salu_t); \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      generate_branch_no_cycle_update(salu_exits[salu_n].branch_source,       \
                                      salu_t);                                \
    }                                                                         \
    salu_n++;                                                                 \
    salu_converted++;                                                         \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arm_data_proc(name, imm, no_flags);                                       \
  }                                                                           \
}
#else
#define arm_static_alu_pc(is_add, name) arm_data_proc(name, imm, no_flags)
#endif

#ifdef SMC_REUSE
/* SMC_REUSE -- do not re-translate RAM code that has not changed.
 *
 * WHY.  Some games rewrite their own RAM code with the same bytes, or run code
 * from the stack and then overwrite it, several times a frame (Camelot's sound
 * driver: Mario Tennis / Mario Golf / Golden Sun; Iridion II; Payback).  Every
 * such write is a full RAM flush, and the same blocks are then translated
 * again: 99.6% of Mario Tennis's RAM translations are repeats.
 *
 * WHAT.  A full RAM flush still does everything the guest can observe -- every
 * code tag is zeroed, the tag numbering restarts, all the SMC bookkeeping is
 * reset -- but the native code is left where it is ("dormant").  Each RAM block
 * is followed in the cache by a small RECORD: its guest pc and mode, the guest
 * bytes it was translated from, and its external exits.  When the lookup would
 * translate a block, and a record for that pc exists whose guest bytes are
 * still identical, the block is REVIVED instead: the side effects of the
 * translation are replayed (tags, code limits, extent note, the virtual cache
 * pointer, and the same lookups of the same exit targets in the same order)
 * and the old body is used.  Nothing else changes.
 *
 * THE CONTRACT, which is what makes this a speed-only change:
 *  1. The translator is a pure function of (pc, mode, the guest bytes of the
 *     scanned extent, the gate tables, smc_partial_active, the idle-loop pc and
 *     the cheat hook).  For RAM blocks it reads guest memory only as opcodes
 *     inside the extent (the one literal-pool fold is ROM-only).  The record
 *     keeps the extent plus 4 bytes; everything else in that list is the
 *     ENVIRONMENT, and any change to it drops every record (smc_reuse_env_ok).
 *  2. A dormant body is reachable only through the lookup: the flush zeroed
 *     every tag, and a live block's exits were all looked up when it was
 *     translated or revived.  So a body runs only after its bytes were compared.
 *  3. Exits are not trusted: revival looks every target up again (exactly the
 *     calls a translation makes) and re-patches a jump whose target moved.
 *  4. The guest-visible effects of a translation are the tags it writes and
 *     the moment the cache "fills" (a full flush, and ENTRY_GATES arming).
 *     Tags are replayed by smc_reuse_retag().  Fullness is decided on a VIRTUAL
 *     pointer that advances exactly as the real one would without this switch
 *     (smc_reuse_vptr, the record's size and chk), so the cache fills at the
 *     same block as before.
 *  5. The one known exception to (4): the real cache also holds dormant bodies
 *     and records, so it can fill while the virtual one has room.  That is
 *     handled as an ordinary cache-full flush, and counted in
 *     smc_reuse_early_full; a flush that finds the cache more than half used
 *     resets it for real to keep that rare, and the switch stays inert in
 *     any game that is not flushing constantly (see ARMING below), which is
 *     every game that fills its cache in the library sweep.  NOT yet exact:
 *     an exact answer needs either spare cache bytes that the virtual
 *     pointer never counts, or a purge that keeps the tags.
 *
 * Code memory is not written on a virtual flush or on a revival whose links
 * are unchanged, so neither needs an I-cache sync. */
typedef struct smc_reuse_rec_s
{
  u32 key;      /* guest pc | thumb */
  u32 end;      /* scan end (raw pc) */
  u32 first;    /* cache offset of the block's first emitted byte */
  u32 body;     /* cache offset of the body (what the lookup returns) */
  u32 size;     /* emitted bytes, first .. committed translation_ptr */
  u32 chk;      /* offset from first of the last cache-limit check */
  u32 reason;   /* scan_exit_reason */
  u32 nexits;   /* smc_reuse_exit[nexits] follows, */
  u32 nsites;   /* then smc_reuse_call[nsites] memory-access call sites, */
  u32 srclen;   /* then srclen guest bytes */
  u32 next;     /* cache offset of the previous record in this slot, or 0 */
  u32 epoch;    /* smc_reuse_epoch when last live (translated or revived) */
} smc_reuse_rec;
typedef struct { u32 src, target; } smc_reuse_exit;

/* MULTI-VERSION.  A slot heads a chain of records, newest first: every
 * record a slot has received since the last real reset, whatever its key.
 * So one pc keeps every version of its code that was translated (a game
 * that patches a routine between a few fixed variants revives each of them)
 * and two pcs that share a slot no longer evict each other.  The chain is
 * valid for exactly as long as the records are: both die only at a real
 * reset, which also clears the index (smc_reuse_drop).  find walks at most
 * SMC_REUSE_WALK records of the chain -- a bound on the cost of a miss, not a
 * correctness rule: a version past it is simply translated again. */
#define SMC_REUSE_SLOTS 1024
#ifndef SMC_REUSE_WALK
#define SMC_REUSE_WALK  8
#endif
static u32 smc_reuse_index[SMC_REUSE_SLOTS];   /* cache offset of a chain head */
static u32 smc_reuse_vptr;        /* where ram_translation_ptr would be */
static u8 *smc_reuse_water;       /* ram_translation_ptr as last seen */
static u32 smc_reuse_force_real;
/* ARMING.  Records cost cache space, which contract point 5 says can make
 * the real cache fill before the virtual one.  So the switch is inert -- no
 * record written, nothing kept, nothing revived, the cache used byte for byte
 * as without it -- unless the game is flushing its RAM code for reasons other
 * than a full cache at least SMC_REUSE_ARM_FLUSHES times per
 * SMC_REUSE_ARM_FRAMES frames (re-evaluated every window), and it stands down
 * for the rest of the session the first time the cache does fill. */
#define SMC_REUSE_ARM_FRAMES   64
#define SMC_REUSE_ARM_FLUSHES  32
static u32 smc_reuse_armed, smc_reuse_win_frame, smc_reuse_win_flushes,
           smc_reuse_last_full, smc_reuse_full_seen;
#ifndef SMC_REUSE_REC_RATIO
#define SMC_REUSE_REC_RATIO 8
#endif
#define SMC_REUSE_REC_PROBE 8
static u32 smc_reuse_win_xlat, smc_reuse_win_hits, smc_reuse_win_n;
static u32 smc_reuse_find_on = 1;
extern u32 smc_reuse_xlat, smc_reuse_hits, smc_reuse_paused;
static void smc_reuse_window(void)
{
#ifndef SMC_REUSE_ARMING
  /* The spare invariant makes reuse exact everywhere: no arming needed.
   * (make SMC_REUSE_ARMING=1 restores bb8b7b2's window as a cost knob.)
   * What remains is cost: a record costs its block a pass over the call
   * sites and a copy of the guest bytes, and pays only when it is revived.
   * So recording pauses for a window after one in which translations
   * outnumbered revivals SMC_REUSE_REC_RATIO to one (Hot Wheels: 15 to 1),
   * and every SMC_REUSE_REC_PROBE-th window records anyway to look again.
   * Lookups never pause.  Recording or not changes host work only: every
   * flush, full or otherwise, still comes at the guest instruction it would
   * without this switch (the spare invariant), so this is exact as well;
   * and it depends only on frame_counter and the translation history, so a
   * twin and a console decide alike. */
  smc_reuse_armed = 1;
  if ((u32)(frame_counter - smc_reuse_win_frame) >= SMC_REUSE_ARM_FRAMES) {
    u32 x = smc_reuse_xlat - smc_reuse_win_xlat;
    u32 h = smc_reuse_hits - smc_reuse_win_hits;
    smc_reuse_win_n++;
    smc_reuse_rec_on = (smc_reuse_win_n % SMC_REUSE_REC_PROBE) == 0 ||
                       x < SMC_REUSE_ARM_FRAMES ||
                       x <= h * SMC_REUSE_REC_RATIO;
    if (!smc_reuse_rec_on)
      smc_reuse_paused++;
    /* Lookups likewise: a window without a single revival skips them for
     * the next one (probe windows still look). */
    smc_reuse_find_on = (smc_reuse_win_n % SMC_REUSE_REC_PROBE) == 0 || h > 0;
    smc_reuse_win_xlat = smc_reuse_xlat;
    smc_reuse_win_hits = smc_reuse_hits;
    smc_reuse_win_frame = frame_counter;
  }
  return;
#endif
  if ((u32)(frame_counter - smc_reuse_win_frame) >= SMC_REUSE_ARM_FRAMES) {
    smc_reuse_armed = !smc_reuse_full_seen &&
                      smc_reuse_win_flushes >= SMC_REUSE_ARM_FLUSHES;
    smc_reuse_win_flushes = 0;
    smc_reuse_win_frame = frame_counter;
  }
}
static struct { u32 gen, idle, cheat, pa, ngates, size; u8 *base; } smc_reuse_env;
/* SMC_REUSE_MODE (root make, default 3 since 4.5.0: hardware contest Lab runs
 * r5ac510 (3000) / r149b9c (1000), mode 3 vs 2 = 5 better, 19 same, 0 worse,
 * mean work -1.1 %): a hardware A/B with the code layout held fixed
 * -- the arms differ in this one initialised word only.
 *   0 off: no record, no revival, every flush real (guest-exact as without
 *     SMC_REUSE, and the cache is used as without it)
 *   1 record only: the full recording cost, never a revival
 *   2 on
 *   3 on without the cost rules (no recording/lookup pause, every matching
 *     record revived: the step-4 behaviour) */
#ifndef SMC_REUSE_MODE
#define SMC_REUSE_MODE 3
#endif
/* Biased so that no arm's value is 0: a zero would move the word to .bss. */
u32 smc_reuse_mode_w = 0x100 + SMC_REUSE_MODE;
#define smc_reuse_mode (smc_reuse_mode_w & 0xFF)
/* SMC_REUSE_EPOCH (default 1): a record is revived at most once per flush
 * epoch, and never in the epoch it was live in.  Reuse pays where a flush
 * threw a working set away (TGR, Tennis, Golf: the whole set comes back in
 * place).  A block that a single-block SMC retirement took out of a live
 * epoch (Doom, Duke, Payback: one routine toggling between versions) is
 * translated afresh instead: on hardware, reviving its old body cost more in
 * cache misses than the translation it saved (docs: cachesim m0/m1/m2).
 * Counts only, so a twin and a console decide alike. */
#ifndef SMC_REUSE_EPOCH
#define SMC_REUSE_EPOCH 1
#endif
/* ...unless the translation it saves is big: a body of at least this many
 * emitted bytes is revived in its own epoch too (Heart & Soul's 1-2 KiB
 * battle routine: the saving outweighs the misses there). */
#ifndef SMC_REUSE_SAME_MIN
#define SMC_REUSE_SAME_MIN 1024
#endif
static u32 smc_reuse_epoch = 1;
u32 smc_reuse_same_epoch, smc_reuse_same_skip;
#ifdef SMC_REUSE_DEBUG
static u32 smc_reuse_szh[2][10], smc_reuse_szb[2];
#endif
/* The key a same-epoch match just sent to translation: its record would only
 * duplicate the one that matched (still in the cache, revivable next epoch). */
static u32 smc_reuse_skip_key = 1;
u32 smc_reuse_hits, smc_reuse_stale, smc_reuse_early_full, smc_reuse_resets,
    smc_reuse_kept, smc_reuse_records, smc_reuse_relinked, smc_reuse_unlearnt,
    smc_reuse_rom2ram, smc_reuse_hits_old, smc_reuse_walked, smc_reuse_cut,
    smc_reuse_xlat, smc_reuse_nospare, smc_reuse_sitebad, smc_reuse_paused;

#ifdef DRPROF_TWIN
#include <stdio.h>
#include <stdlib.h>
static void smc_reuse_report(void)
{
  fprintf(stderr, "smc_reuse: same_epoch=%u same_skip=%u%c", (unsigned)smc_reuse_same_epoch,
          (unsigned)smc_reuse_same_skip, 10);
#ifdef SMC_REUSE_DEBUG
  { int e, b;
    for (e = 0; e < 2; e++) {
      fprintf(stderr, "smc_reuse: %s revivals bytes=%u sizes(<64,<128..):", e ? "same-epoch" : "cross-epoch",
              (unsigned)smc_reuse_szb[e]);
      for (b = 0; b < 10; b++) fprintf(stderr, " %u", (unsigned)smc_reuse_szh[e][b]);
      fprintf(stderr, "%c", 10);
    } }
#endif
  fprintf(stderr, "smc_reuse: paused=%u sitebad=%u xlat=%u nospare=%u hits_old=%u walked=%u cut=%u rom2ram=%u unlearnt=%u relinked=%u hits=%u records=%u stale=%u kept=%u resets=%u "
          "early_full=%u fl_total=%u fl_full=%u fl_smc=%u fl_dma=%u\n",
          (unsigned)smc_reuse_paused, (unsigned)smc_reuse_sitebad,
          (unsigned)smc_reuse_xlat, (unsigned)smc_reuse_nospare,
          (unsigned)smc_reuse_hits_old, (unsigned)smc_reuse_walked,
          (unsigned)smc_reuse_cut, (unsigned)smc_reuse_rom2ram,
          (unsigned)smc_reuse_unlearnt, (unsigned)smc_reuse_relinked,
          (unsigned)smc_reuse_hits, (unsigned)smc_reuse_records,
          (unsigned)smc_reuse_stale, (unsigned)smc_reuse_kept,
          (unsigned)smc_reuse_resets, (unsigned)smc_reuse_early_full,
          (unsigned)flush_ram_total, (unsigned)flush_ram_full,
          (unsigned)flush_ram_smc, (unsigned)flush_ram_dma);
#ifdef SMC_REUSE_DEBUG
  { extern u32 smc_reuse_dry_ok, smc_reuse_dry_bad, smc_reuse_dry_ic, smc_reuse_dry_full;
    fprintf(stderr, "smc_reuse dry: ok=%u bad=%u full=%u relearnt_call_sites=%u%c",
            (unsigned)smc_reuse_dry_ok, (unsigned)smc_reuse_dry_bad,
            (unsigned)smc_reuse_dry_full, (unsigned)smc_reuse_dry_ic, 10); }
#endif
}
#endif

static void smc_reuse_drop(void)
{
#ifdef DRPROF_TWIN
  static int once;
  if (!once) { once = 1; atexit(smc_reuse_report); }
#endif
  memset(smc_reuse_index, 0, sizeof(smc_reuse_index));
  smc_reuse_water = ram_translation_cache;
}

/* 1 = every record in the index was made in the environment we are in. */
static int smc_reuse_env_ok(void)
{
#ifdef SMC_PARTIAL_SAFE
  u32 pa = smc_partial_active;
#else
  u32 pa = 0;
#endif
  if (smc_reuse_env.gen == smc_reuse_gen &&
      smc_reuse_env.idle == idle_loop_target_pc &&
      smc_reuse_env.cheat == cheat_master_hook &&
      smc_reuse_env.pa == pa &&
      smc_reuse_env.ngates == translation_gate_targets &&
      smc_reuse_env.size == ram_translation_cache_size &&
      smc_reuse_env.base == ram_translation_cache &&
      ram_translation_ptr >= smc_reuse_water)
    return 1;
  smc_reuse_env.gen = smc_reuse_gen;
  smc_reuse_env.idle = idle_loop_target_pc;
  smc_reuse_env.cheat = cheat_master_hook;
  smc_reuse_env.pa = pa;
  smc_reuse_env.ngates = translation_gate_targets;
  smc_reuse_env.size = ram_translation_cache_size;
  smc_reuse_env.base = ram_translation_cache;
  /* The cache was reset behind this switch's back (init, a tier change): the
   * real pointer is then also the virtual one. */
  if (ram_translation_ptr < smc_reuse_water)
    smc_reuse_vptr = (u32)(ram_translation_ptr - ram_translation_cache);
  smc_reuse_drop();
  return 0;
}

static inline u32 smc_reuse_slot(u32 key)
{
  return (key * 2654435761u) >> 22;            /* 10 bits */
}

/* Guest bytes and tag halfwords of a RAM pc, and the room to the region end. */
static u8 *smc_reuse_mem(u32 pc, u16 **tagp, u32 *room)
{
  if ((pc >> 24) == 2) {
    u32 off = pc & 0x3FFFF;
    *room = 0x40000 - off;
    *tagp = (u16 *)(ewram + 0x40000 + off);
    return ewram + off;
  } else {
    u32 off = pc & 0x7FFF;
    *room = 0x8000 - off;
    *tagp = (u16 *)(iwram + off);
    return iwram + 0x8000 + off;
  }
}

/* translate_block's limit formula, for the VIRTUAL cache (the size without
 * this switch, gpsp_config.h SMC_REUSE_VIRT_SIZE) and for the real one. */
static u32 smc_reuse_limit_off(void)
{
  return ram_translation_cache_size - SMC_REUSE_SPARE
         - TRANSLATION_CACHE_LIMIT_THRESHOLD
         - (0x10000 - ram_block_tag) / 2 * sizeof(ramtag_type);
}
static u32 smc_reuse_real_off(void)
{
  return ram_translation_cache_size - TRANSLATION_CACHE_LIMIT_THRESHOLD
         - (0x10000 - ram_block_tag) / 2 * sizeof(ramtag_type);
}

/* The limit translate_block must test against so that it reports "full" at
 * the instruction where the cache would have filled without this switch. */
static u8 *smc_reuse_limit(u8 *first, u8 *real_limit)
{
  s32 room = (s32)smc_reuse_limit_off() - (s32)smc_reuse_vptr;
  u8 *v = first + room;
  return v < real_limit ? v : real_limit;
}

/* THE SPARE INVARIANT (contract point 5): real pointer - virtual pointer
 * <= SMC_REUSE_SPARE.  A translation advances both by the same bytes, a
 * revival only the virtual one; only a record or a kept flush can raise the
 * excess, and each is refused when it would break the bound.  Since the real
 * limit is the virtual one + SMC_REUSE_SPARE, the real cache then always has
 * at least the virtual cache's room left. */
static inline s32 smc_reuse_excess(u32 extra)
{
  return (s32)(ram_translation_ptr - ram_translation_cache) + (s32)extra -
         (s32)smc_reuse_vptr;
}

static void smc_reuse_note_full(u8 *first, u8 *translation_ptr)
{
  if ((s32)smc_reuse_vptr + (s32)(translation_ptr - first) <=
      (s32)smc_reuse_limit_off())
    smc_reuse_early_full++;            /* contract point 5 */
}

/* MEMORY-ACCESS CALL SITES.  A translated block calls the memory stubs with
 * `jal`, and the stubs RE-POINT that jal at run time to the stub for the
 * region the access actually hit (emit_phand, mips/mips_emit.h).  So a body
 * that has run is not the body the translator emitted: it has learnt regions.
 * That is not invisible -- with the learnt sites left in place Payback's
 * per-frame state diverges at frame 1364, and with them reset it does not --
 * so a record lists every `jal` into the stub zone as (offset from first, the
 * word the translator emitted) and a revival puts each one back.  The stub
 * zone is below rom_cache_watermark and is rebuilt only with the whole
 * dynarec, which resets the RAM cache and so drops every record. */
typedef struct { u32 off, word; } smc_reuse_call;
static smc_reuse_call smc_reuse_sitebuf[SMC_REUSE_MAXSITES];
static int smc_reuse_site(const u8 *at)
{
  u32 w = address32(at, 0), target;
  if ((w >> 26) != mips_opcode_jal)
    return 0;
  target = ((u32)(uintptr_t)at & 0xF0000000) | ((w & 0x3FFFFFF) << 2);
  return target >= (u32)(uintptr_t)rom_translation_cache &&
         target < (u32)(uintptr_t)rom_translation_cache + rom_cache_watermark;
}

static u8 *smc_reuse_src_copy(smc_reuse_rec *r)
{
  return (u8 *)(r + 1) + r->nexits * sizeof(smc_reuse_exit) +
         r->nsites * sizeof(smc_reuse_call);
}

static smc_reuse_rec *smc_reuse_find(u32 key)
{
  u32 off, room, n, seen;
  u16 *tagp;
  u8 *src;
  smc_reuse_rec *r = NULL;

  if (smc_reuse_mode < 2 || (!smc_reuse_find_on && smc_reuse_mode == 2) ||
      !smc_reuse_env_ok() ||
      !smc_reuse_armed)
    return NULL;
#ifdef SMC_REUSE_DEBUG   /* twin only: bisect a divergence without rebuilding */
  {
    static int dbg = -1;
    if (dbg < 0) { const char *e = getenv("SMC_REUSE_DBG"); dbg = e ? atoi(e) : 0; }
    if ((dbg & 1) || ((dbg & 2) && !(key & 1)) || ((dbg & 4) && (key & 1)) ||
        ((dbg & 8) && (key >> 24) == 3 && (key & 0x7FFF) >= 0x7000) ||
        ((dbg & 16) && frame_counter >= (u32)(getenv("SMC_REUSE_FROM") ? atoi(getenv("SMC_REUSE_FROM")) : 0)))
      return NULL;
    {
      static u32 xkey = 1; static const char *xe;
      if (xkey == 1) { xe = getenv("SMC_REUSE_KEY"); xkey = xe ? strtoul(xe, NULL, 16) : 0; }
      if (xkey && key == xkey) return NULL;
      {   /* SMC_REUSE_MAXHITS=n: no revival after the n-th (bisect) */
        static long mh = -2;
        if (mh == -2) { const char *m = getenv("SMC_REUSE_MAXHITS"); mh = m ? atol(m) : -1; }
        if (mh >= 0 && smc_reuse_hits >= (u32)mh) {
          extern u32 smc_reuse_dump_pending, smc_reuse_dump_done;
          static long tk = -2;
          if (tk == -2) { const char *m = getenv("SMC_REUSE_DUMPKEY"); tk = m ? (long)strtoul(m, NULL, 16) : -1; }
          if (tk >= 0 && key == (u32)tk && !smc_reuse_dump_done) smc_reuse_dump_pending = 1;
          return NULL;
        }
      }
      if ((dbg & 64) && frame_counter >= (u32)(getenv("SMC_REUSE_FROM") ? atoi(getenv("SMC_REUSE_FROM")) : 0))
        fprintf(stderr, "RV %u %08x%c", (unsigned)frame_counter, (unsigned)key, 10);
    }
  }
#endif
  src = smc_reuse_mem(key & ~1u, &tagp, &room);
  off = smc_reuse_index[smc_reuse_slot(key)];
  for (n = 0, seen = 0; off; off = r->next) {
    if (n == SMC_REUSE_WALK) {
      smc_reuse_cut++;                 /* older versions not looked at */
      break;
    }
    n++;
    smc_reuse_walked++;
    r = (smc_reuse_rec *)(ram_translation_cache + off);
    if (r->key != key)
      continue;
#ifdef SMC_REUSE_DEBUG   /* mutant 256: trust the record without comparing */
    if (getenv("SMC_REUSE_DBG") && (atoi(getenv("SMC_REUSE_DBG")) & 256))
      return r;
#endif
#ifdef SMC_REUSE_DEBUG   /* mutant 2048: single-version (newest of this pc only) */
    if (seen && getenv("SMC_REUSE_DBG") && (atoi(getenv("SMC_REUSE_DBG")) & 2048))
      break;
#endif
    if (r->srclen <= room &&
        !memcmp(src, smc_reuse_src_copy(r), r->srclen)) {
#if defined(SMC_IMM_PATCH) && !defined(SMC_REUSE_IMMP_MUTANT)
      if (immp_covers(key & ~1u, r->end)) {
        immp_reuse_nofind++;           /* R2: translate, register the sites */
        return NULL;
      }
#endif
#if SMC_REUSE_EPOCH
      if (r->epoch == smc_reuse_epoch && r->size < SMC_REUSE_SAME_MIN &&
          smc_reuse_mode == 2) {
        smc_reuse_same_epoch++;        /* live this epoch: translate afresh */
        smc_reuse_skip_key = key;
        return NULL;
      }
#endif
      if (seen)
        smc_reuse_hits_old++;          /* not the newest version of this pc */
      return r;
    }
    seen++;
  }
  if (seen)
    smc_reuse_stale++;
  return NULL;
}

static u8 *smc_reuse_body(smc_reuse_rec *r)
{
  return ram_translation_cache + r->body;
}
static u32 smc_reuse_size(smc_reuse_rec *r)
{
  return r->size;
}

/* Called by translate_block_* for every RAM block, right after it publishes
 * ram_translation_ptr and before it resolves the exits. */
static void smc_reuse_commit(u32 start, u32 end, u32 thumb, u32 reason,
                             u8 *first, u32 chk, const block_exit_type *ex,
                             u32 nex)
{
  u8 *p = ram_translation_ptr, *src;
  u16 *tagp;
  u32 i, n = 0, room, len = end - start, srclen, need, ns = 0;
  smc_reuse_call *sp;
  smc_reuse_rec *r;
  smc_reuse_exit *xe;

  smc_reuse_env_ok();
  smc_reuse_vptr += (u32)(p - first);
  smc_reuse_water = p;
  smc_reuse_xlat++;
  smc_reuse_window();
  if (smc_reuse_skip_key == (start | thumb)) {
    smc_reuse_skip_key = 1;            /* (1 is no key: bit 0 = thumb, pc 0) */
    smc_reuse_same_skip++;
    return;
  }
  smc_reuse_skip_key = 1;
  if (!smc_reuse_armed || !smc_reuse_mode ||
      (!smc_reuse_rec_on && smc_reuse_mode != 3))
    return;
#if defined(SMC_IMM_PATCH) && !defined(SMC_REUSE_IMMP_MUTANT)
  if (!thumb && immp_blk_learned) {    /* R1: the body may be patched later */
    immp_reuse_norec++;
    return;
  }
#endif

  src = smc_reuse_mem(start, &tagp, &room);
  if (len > room)                       /* wraps the region: never reused */
    return;
  if (reason == SMC_SCAN_GATE && len + 4 > room)
    return;                             /* gate word in the next region */
  srclen = (len + 4 <= room) ? len + 4 : len;
  for (i = 0; i < nex; i++)
    if (ex[i].branch_source)
      n++;
  if (smc_reuse_nlog <= SMC_REUSE_MAXSITES) {
    /* one pass: the log is in emission order, so sites come out sorted */
    for (i = 0; i < smc_reuse_nlog; i++) {
      u8 *at = smc_reuse_sitelog[i];
      if (at < p && smc_reuse_site(at)) {
        smc_reuse_sitebuf[ns].off = (u32)(at - first);
        smc_reuse_sitebuf[ns].word = address32(at, 0);
        ns++;
      }
    }
  } else {
    for (i = 0; i < (u32)(p - first); i += 4)
      if (smc_reuse_site(first + i))
        ns++;
  }
#ifdef SMC_REUSE_DEBUG   /* twin: the log must find what the scan finds */
  {
    u32 scan = 0;
    for (i = 0; i < (u32)(p - first); i += 4)
      if (smc_reuse_site(first + i))
        scan++;
    if (scan != ns)
      smc_reuse_sitebad++;
  }
#endif
  /* The mutable words go with the call sites and are restored the same way.
   * A block with more than the log holds, or with too many sites for the
   * fast path, is simply not recorded. */
  if (smc_reuse_nmut) {
    if (smc_reuse_nmut > SMC_REUSE_MAXMUT || smc_reuse_nlog > SMC_REUSE_MAXSITES)
      return;
    for (i = 0; i < smc_reuse_nmut; i++) {
      u8 *at = smc_reuse_mutlog[i];
      if (at >= p)
        continue;
      if (ns == SMC_REUSE_MAXSITES)
        return;
      smc_reuse_sitebuf[ns].off = (u32)(at - first);
      smc_reuse_sitebuf[ns].word = address32(at, 0);
      ns++;
    }
  }
  need = (sizeof(*r) + n * sizeof(*xe) + ns * sizeof(*sp) + srclen + 3) & ~3u;
  if (smc_reuse_excess(need) > (s32)SMC_REUSE_SPARE) {
    smc_reuse_nospare++;
    return;                             /* spare full: this block is not reusable */
  }

  r = (smc_reuse_rec *)p;
  r->key = start | thumb;
  r->end = end;
  r->first = (u32)(first - ram_translation_cache);
  r->body = r->first + ram_block_prologue_size;
  r->size = (u32)(p - first);
  r->chk = chk;
  r->reason = reason;
  r->nexits = n;
  r->nsites = ns;
  r->srclen = srclen;
  r->next = smc_reuse_index[smc_reuse_slot(r->key)];
  r->epoch = smc_reuse_epoch;
  xe = (smc_reuse_exit *)(r + 1);
  for (i = 0; i < nex; i++)
    if (ex[i].branch_source) {
      xe->src = (u32)(ex[i].branch_source - ram_translation_cache);
      xe->target = ex[i].branch_target;
      xe++;
    }
  sp = (smc_reuse_call *)xe;
  if (smc_reuse_nlog <= SMC_REUSE_MAXSITES) {
    memcpy(sp, smc_reuse_sitebuf, ns * sizeof(*sp));
    sp += ns;
  } else {
    for (i = 0; i < (u32)(p - first); i += 4)
      if (smc_reuse_site(first + i)) {
        sp->off = i;
        sp->word = address32(first, i);
        sp++;
      }
  }
  memcpy(sp, src, srclen);
  ram_translation_ptr = p + need;
  smc_reuse_water = ram_translation_ptr;
#ifdef JIT_CODE_DISCIPLINE
  /* A record is data, but it is written inside the RAM code zone, over bytes
   * that may have been code before the last real reset.  Recording it keeps
   * the rule "every write into a code zone is published" without exception
   * (and the audit build clean); it joins its own block's span, so it costs
   * no extra kernel call. */
  JIT_CODE_WROTE(p, p + need);
#endif
  smc_reuse_index[smc_reuse_slot(r->key)] = (u32)(p - ram_translation_cache);
  smc_reuse_records++;
}

/* The tags scan_block writes for [start, end): smc_write_*_yes per
 * instruction, smc_tag_gate_*_yes when the scan stopped at a gate, and
 * update_pc_limits per instruction. */
static void smc_reuse_retag(u32 start, u32 end, u32 thumb, u32 reason)
{
  u16 *tagp;
  u32 room, len = end - start, i, lo, hi;
  smc_reuse_mem(start, &tagp, &room);

  if (thumb) {
    for (i = 0; i < len; i += 2)
      if (tagp[i >> 1] == 0)
        tagp[i >> 1] = CODE_TAG_BLOCK16;
  } else {
    for (i = 0; i < len; i += 4) {
      u32 w = address32(tagp, i);
      if (w == 0)
        address32(tagp, i) = CODE_TAG_BLOCK32;
#if defined(SMC_PARTIAL_SINGLE) && !defined(DR2NFSU_MUTANT_WORDTAG)
      else if (!(w & 0xFFFF) || !(w >> 16)) {   /* smc_write_arm_yes */
        if (address16(tagp, i) == 0)
          address16(tagp, i) = CODE_TAG_BLOCK16;
        if (address16(tagp, i + 2) == 0)
          address16(tagp, i + 2) = CODE_TAG_BLOCK16;
      }
#endif
    }
  }
  if (reason == SMC_SCAN_GATE) {
    u16 *g;
    smc_reuse_mem(end, &g, &room);
    if (g[0] == 0)
      g[0] = CODE_TAG_BLOCK16;
    if (!thumb) {
      smc_reuse_mem(end + 2, &g, &room);
      if (g[0] == 0)
        g[0] = CODE_TAG_BLOCK16;
    }
  }
  lo = start;
  hi = end - (thumb ? 2 : 4);
  if (start >= 0x3000000) {
    iwram_code_min = MIN(lo & 0x7FFF, iwram_code_min);
    iwram_code_max = MAX(hi & 0x7FFF, iwram_code_max);
  } else {
    ewram_code_min = MIN(lo & 0x3FFFF, ewram_code_min);
    ewram_code_max = MAX(hi & 0x3FFFF, ewram_code_max);
  }
}

#ifdef SMC_REUSE_DEBUG
/* Twin-only self check (SMC_REUSE_DBG bit 128): before a block is revived it
 * is ALSO translated, without being committed, and the fresh code is compared
 * with the dormant body word by word.  This tests contract point 1 directly. */
static smc_reuse_rec *smc_reuse_dry_rec;
u32 smc_reuse_dry_ok, smc_reuse_dry_bad, smc_reuse_dry_ic, smc_reuse_dry_full;
static void smc_reuse_dry_compare(u8 *first, u8 *tp, u32 end, u32 reason,
                                  u32 chk, const block_exit_type *ex, u32 nex)
{
  smc_reuse_rec *r = smc_reuse_dry_rec;
  smc_reuse_exit *xe = (smc_reuse_exit *)(r + 1);
  u8 *old = ram_translation_cache + r->first;
  u32 size = (u32)(tp - first), i, k, n = 0, bad = 0;
  const char *why = NULL;
  for (i = 0; i < nex; i++)
    if (ex[i].branch_source) {
      if (n < r->nexits && ((u32)(ex[i].branch_source - first) != xe[n].src - r->first ||
                            ex[i].branch_target != xe[n].target))
        why = "exit";
      n++;
    }
  if (n != r->nexits) why = "nexits";
  if (end != r->end) why = "end";
  if (reason != r->reason) why = "reason";
  if (chk != r->chk) why = "chk";
  if (size != r->size) why = "size";
  if (!why)
    for (i = 0; i < size; i += 4) {
      u32 a = address32(old, i), b = address32(first, i);
      if (a == b) continue;
      if (r->body - r->first == 32 && i >= 16 && i < 32)
        continue;                                     /* stable thunk */
      if ((a >> 26) == 2 && (b >> 26) == 2) {
        u32 ta = (a & 0x3FFFFFF) << 2, tb = (b & 0x3FFFFFF) << 2;
        if (ta - ((u32)(uintptr_t)old & 0x0FFFFFFF) == tb - ((u32)(uintptr_t)first & 0x0FFFFFFF))
          continue;                                   /* internal jump */
        for (k = 0; k < r->nexits; k++)
          if (xe[k].src - r->first == i) break;
        if (k < r->nexits) continue;                  /* a patched exit */
      }
      if (!bad++)
        fprintf(stderr, "DRY MISMATCH f=%u key=%08x word=%u old=%08x new=%08x%c",
                (unsigned)frame_counter, (unsigned)r->key, (unsigned)(i >> 2),
                (unsigned)a, (unsigned)b, 10);
    }
  if (why)
    fprintf(stderr, "DRY MISMATCH f=%u key=%08x %s: end %08x/%08x size %u/%u chk %u/%u nex %u/%u%c",
            (unsigned)frame_counter, (unsigned)r->key, why, (unsigned)r->end, (unsigned)end,
            (unsigned)r->size, (unsigned)size, (unsigned)r->chk, (unsigned)chk,
            (unsigned)r->nexits, (unsigned)n, 10);
  if (why && (size != r->size) && smc_reuse_dry_bad < 400) {
    u32 m = size < r->size ? size : r->size, j;
    for (i = 0; i < m; i += 4) {
      u32 x = address32(old, i), y = address32(first, i);
      if (x != y && !((x >> 26) == 3 && (y >> 26) == 3) && !((x >> 26) == 2 && (y >> 26) == 2)) break;
    }
    fprintf(stderr, "DRY DUMP key=%08x at word %u gpc_regs sp=%08x lr=%08x old:", (unsigned)r->key,
            (unsigned)(i >> 2), (unsigned)reg[REG_SP], (unsigned)reg[REG_LR]);
    for (j = (i >= 12 ? i - 12 : 0); j < i + 16 && j < r->size; j += 4) fprintf(stderr, " %08x", (unsigned)address32(old, j));
    fprintf(stderr, " new:");
    for (j = (i >= 12 ? i - 12 : 0); j < i + 16 && j < size; j += 4) fprintf(stderr, " %08x", (unsigned)address32(first, j));
    fprintf(stderr, "%c", 10);
  }
  if (why || bad) smc_reuse_dry_bad++; else smc_reuse_dry_ok++;
}
#endif

/* Stand in for translate_block_{arm,thumb}(pc, true) on a block whose record
 * matched.  Same return value, same guest-visible effects, same nested
 * lookups; the body is the one already in the cache. */
#ifdef SMC_REUSE_DEBUG
/* Twin bisect aid: SMC_REUSE_TRACEHIT (a revival) or SMC_REUSE_DUMPKEY with
 * SMC_REUSE_MAXHITS (the translation that replaces it) marks one lookup; when
 * it completes, the guest-invisible state a translation leaves behind is
 * written to SMC_REUSE_DUMPFILE, for a diff between the two runs. */
u32 smc_reuse_dump_pending, smc_reuse_dump_done;
static void smc_reuse_dump_maybe(void)
{
  FILE *f;
  const char *fn;
  u32 i, n;
  if (!smc_reuse_dump_pending || smc_reuse_dump_done)
    return;
  smc_reuse_dump_pending = 0;
  smc_reuse_dump_done = 1;
  fn = getenv("SMC_REUSE_DUMPFILE");
  f = fopen(fn ? fn : "/tmp/rdump.bin", "wb");
  if (!f) return;
  fprintf(stderr, "DUMP f=%u%c", (unsigned)frame_counter, 10);
  fwrite("TAGI", 1, 4, f); fwrite(iwram, 1, 0x8000, f);
  fwrite("TAGE", 1, 4, f); fwrite(ewram + 0x40000, 1, 0x40000, f);
  fwrite("MEMI", 1, 4, f); fwrite(iwram + 0x8000, 1, 0x8000, f);
  fwrite("REGS", 1, 4, f); fwrite(reg, 4, 64, f);
#ifdef SMC_SAME_ADAPT
  fwrite("XSVA", 1, 4, f); fwrite(xs_val, 2, 0x8000 / 2, f);
  fwrite("XSOK", 1, 4, f); fwrite(xs_ok, 1, 0x8000 / 16, f);
  fwrite("XSBA", 1, 4, f); fwrite(xs_bad, 1, 0x8000 / 16, f);
  fwrite("XSMI", 1, 4, f); fwrite(xs_min, 2, 0x8000 / 4, f);
  fwrite("XSRE", 1, 4, f); fwrite(xs_rec, 1, 2 * (0x10000 / 16), f);
  { u32 v[6] = { xs_on, xs_win_noted, xs_win_skips, xs_off_retires, xs_reprobe, xs_probe_paid };
    fwrite("XSSC", 1, 4, f); fwrite(v, 4, 6, f); }
#endif
  { u32 v[8] = { iwram_code_min, iwram_code_max, ewram_code_min, ewram_code_max,
                 smc_max_extent[0], smc_max_extent[1], ram_block_tag, smc_reuse_vptr };
    fwrite("LIMS", 1, 4, f); fwrite(v, 4, 8, f); }
#if defined(SMC_RETIRE_WINDOW)
  fwrite("BKIW", 1, 4, f); fwrite(smc_back_iw, 2, 0x8000 >> SMC_LOCAL_SHIFT, f);
  fwrite("BKEW", 1, 4, f); fwrite(smc_back_ew, 2, 0x40000 >> SMC_LOCAL_SHIFT, f);
  fwrite("BKLH", 1, 4, f); fwrite(smc_back_lo, 4, 2, f); fwrite(smc_back_hi, 4, 2, f);
#endif
  {   /* the key's code: public entry .. +512, relative jumps decoded */
    const char *k = getenv("SMC_REUSE_DUMPPC");
    if (k) {
      u32 pc = strtoul(k, NULL, 16), j;
      u16 tg = ((u16 *)iwram)[(pc & 0x7FFF) >> 1];
      fwrite("CODE", 1, 4, f);
      if (VALID_TAG(tg)) {
        ramtag_type *te = get_ram_tag(tg);
        u8 *e = ram_translation_cache + te->offset_arm;
        fprintf(stderr, "DUMPCODE tag=%04x off=%u cb=%u%c", tg, (unsigned)te->offset_arm,
                (unsigned)te->code_bytes_arm, 10);
        for (j = 0; j < 512; j += 4) {
          u32 w = address32(e, j);
          if ((w >> 26) == 2 || (w >> 26) == 3) {   /* j / jal: make relative */
            u32 t = ((u32)(uintptr_t)(e + j) & 0xF0000000) | ((w & 0x3FFFFFF) << 2);
            u32 rel = t - (u32)(uintptr_t)e;
            if (t >= (u32)(uintptr_t)ram_translation_cache &&
                t < (u32)(uintptr_t)ram_translation_cache + ram_translation_cache_size)
              w = (w & 0xFC000000) | 0x02000000 | (rel & 0x00FFFFFF);   /* tagged as RAM-relative */
          }
          fwrite(&w, 4, 1, f);
          if (j < 400)
            fprintf(stderr, "DUMPW %u %08x%c", (unsigned)j, (unsigned)w, 10);
        }
      }
    }
  }
  /* tag entries: extents and liveness, not host offsets */
  n = (0x10000 - ram_block_tag) / 2;
  fwrite("TENT", 1, 4, f);
  for (i = 0; i < n; i++) {
    ramtag_type *te = get_ram_tag((u16)(INITIAL_TOP_TAG - 2 * i));
    u32 v[4] = { te->blk_end_arm, te->blk_end_thumb,
                 te->code_bytes_arm ? (te->code_bytes_arm >= 8 ? 2 : 1) : 0,
                 te->code_bytes_thumb ? (te->code_bytes_thumb >= 8 ? 2 : 1) : 0 };
    fwrite(v, 4, 4, f);
  }
  fclose(f);
}
#endif
#ifdef SMC_REUSE_DEBUG
static void smc_reuse_lktrace(u32 key, u32 off, u32 cb)
{
  static long t0 = -2, t1;
  if (t0 == -2) {
    const char *a = getenv("SMC_REUSE_LK_T0"), *b = getenv("SMC_REUSE_LK_T1");
    t0 = a ? atol(a) : -1; t1 = b ? atol(b) : 0;
  }
  if (t0 >= 0 && cpu_ticks >= (u32)t0 && cpu_ticks <= (u32)t1)
    fprintf(stderr, "LK t=%u key=%08x off=%u cb=%u vptr=%u%c", (unsigned)cpu_ticks,
            (unsigned)key, (unsigned)off, (unsigned)cb, (unsigned)smc_reuse_vptr, 10);
}
#endif
#ifdef SMC_REUSE_DEBUG
static int smc_reuse_exp(int bit)
{
  static int e = -1;
  if (e < 0) { const char *v = getenv("SMC_REUSE_EXP"); e = v ? atoi(v) : 0; }
  return (e & bit) != 0;
}
#endif
static bool smc_reuse_revive(smc_reuse_rec *r)
{
  u32 key = r->key, thumb = key & 1, start = key & ~1u, i, n = r->nexits;
  smc_reuse_exit *xe = (smc_reuse_exit *)(r + 1);
  const smc_reuse_call *sp = (const smc_reuse_call *)(xe + n);
#ifdef SMC_REUSE_DEBUG   /* SMC_REUSE_TRACEHIT=n: describe revival n (0-based) */
  {
    static long th = -2;
    if (th == -2) { const char *m = getenv("SMC_REUSE_TRACEHIT"); th = m ? atol(m) : -1; }
    if (th >= 0 && smc_reuse_hits == (u32)th) {
      extern u32 smc_reuse_dump_pending;
      u32 k, changed = 0;
      smc_reuse_dump_pending = 1;
      for (k = 0; k < r->nsites; k++)
        if (address32(ram_translation_cache + r->first + sp[k].off, 0) != sp[k].word) changed++;
      fprintf(stderr, "TRACEHIT f=%u key=%08x end=%08x reason=%u size=%u chk=%u nexits=%u nsites=%u learnt=%u srclen=%u first=%u vptr=%u real=%u limv=%u limr=%u newest=%d%c",
        (unsigned)frame_counter, (unsigned)key, (unsigned)r->end, (unsigned)r->reason,
        (unsigned)r->size, (unsigned)r->chk, (unsigned)n, (unsigned)r->nsites, (unsigned)changed,
        (unsigned)r->srclen, (unsigned)r->first, (unsigned)smc_reuse_vptr,
        (unsigned)(ram_translation_ptr - ram_translation_cache),
        (unsigned)smc_reuse_limit_off(), (unsigned)smc_reuse_real_off(),
        (int)(smc_reuse_index[smc_reuse_slot(key)] == (u32)((u8 *)r - ram_translation_cache)), 10);
      for (k = 0; k < n; k++)
        fprintf(stderr, "TRACEHIT exit %u src=%u target=%08x cur=%08x%c", (unsigned)k,
                (unsigned)xe[k].src, (unsigned)xe[k].target,
                (unsigned)address32(ram_translation_cache + xe[k].src, 0), 10);
    }
  }
#endif

  for (i = 0; i < r->nsites; i++) {
    u8 *at = ram_translation_cache + r->first + sp[i].off;
#ifdef SMC_REUSE_DEBUG   /* bisect WHICH learnt stub is not neutral: keep some */
    if (address32(at, 0) != sp[i].word && getenv("SMC_REUSE_KEEPCOL")) {
      static int kc = -1, kr = -1;
      u32 w = address32(at, 0), t, k;
      if (kc < 0) { kc = strtol(getenv("SMC_REUSE_KEEPCOL"), NULL, 0);
                    kr = getenv("SMC_REUSE_KEEPROW") ? strtol(getenv("SMC_REUSE_KEEPROW"), NULL, 0) : 0x7FFF; }
      t = ((u32)(uintptr_t)at & 0xF0000000) | ((w & 0x3FFFFFF) << 2);
      for (k = 0; k < 15 * 16; k++)
        if ((k < 176 ? (&tmemld[0][0])[k] : (&tmemst[0][0])[k - 176]) == t) break;
      if (k < 240 && ((kc >> (k & 15)) & 1) && ((kr >> (k >> 4)) & 1))
        continue;
      if (k == 240) {
        static u32 odd; u32 o = sp[i].word, ot = ((u32)(uintptr_t)at & 0xF0000000) | ((o & 0x3FFFFFF) << 2), ok;
        for (ok = 0; ok < 240; ok++)
          if ((ok < 176 ? (&tmemld[0][0])[ok] : (&tmemst[0][0])[ok - 176]) == ot) break;
        if (odd++ < 12)
          fprintf(stderr, "ODD f=%u key=%08x off=%u cur=%08x (stub+%x) orig=%08x (idx %u) tmemld=%p tmemst=%p%c",
                  (unsigned)frame_counter, (unsigned)key, (unsigned)sp[i].off, (unsigned)w,
                  (unsigned)(t - (u32)(uintptr_t)rom_translation_cache), (unsigned)o, (unsigned)ok,
                  (void *)tmemld, (void *)tmemst, 10);
      }
    }
#endif
    if (address32(at, 0) != sp[i].word
#ifdef SMC_REUSE_DEBUG   /* experiment bit 8: keep learnt sites */
        && !smc_reuse_exp(8)
#endif
        ) {
      address32(at, 0) = sp[i].word;
      smc_reuse_unlearnt++;
#ifdef JIT_CODE_DISCIPLINE
      JIT_CODE_WROTE(at, at + 4);
#else
      platform_cache_sync(at, at + 4);
#endif
    }
  }

#ifdef SMC_REUSE_DEBUG
  {
    static int dbg = -1;
    if (dbg < 0) { const char *e = getenv("SMC_REUSE_DBG"); dbg = e ? atoi(e) : 0; }
    if (dbg & 128) {
      smc_reuse_dry_rec = r;
      if (thumb) translate_block_thumb(start, true);
      else translate_block_arm(start, true);
      smc_reuse_dry_rec = NULL;
    }
  }
#endif
#ifdef SMC_REUSE_DEBUG   /* mutant 512: revive without replaying the tags */
  if (!(getenv("SMC_REUSE_DBG") && (atoi(getenv("SMC_REUSE_DBG")) & 512)))
#endif
  smc_reuse_retag(start, r->end, thumb, r->reason);
#ifdef SMC_REUSE_DEBUG   /* experiment bit 4: no note_block replay */
  if (!smc_reuse_exp(4))
#endif
  smc_blk_note_block(start, r->end, thumb, r->reason);

  /* The tag table grows down from the top of the cache with every block
   * start, revived or translated.  A translation tests the real pointer
   * against it; a revival emits nothing, so it must test it here, or the
   * table could grow into bodies that are already in the cache. */
  if (smc_reuse_vptr + r->chk <= smc_reuse_limit_off() &&
      (u32)(ram_translation_ptr - ram_translation_cache) > smc_reuse_real_off())
    smc_reuse_early_full++;            /* contract point 5: never, by the invariant */
  if (smc_reuse_vptr + r->chk > smc_reuse_limit_off() ||
      (u32)(ram_translation_ptr - ram_translation_cache) > smc_reuse_real_off()) {
    /* Without this switch the cache fills while this block is emitted. */
    flush_ram_full++;
#ifdef ENTRY_GATES
    entry_gates_harvest();
#endif
    flush_translation_cache_ram();
    return false;
  }
  smc_reuse_vptr += r->size;
  smc_reuse_hits++;
#ifdef SMC_REUSE_DEBUG   /* revival size histogram, same-epoch vs cross-epoch */
  {
    u32 b = 0, z = r->size;
    while (z >= 64 && b < 9) { z >>= 1; b++; }
    smc_reuse_szh[r->epoch == smc_reuse_epoch][b]++;
    smc_reuse_szb[r->epoch == smc_reuse_epoch] += r->size;
  }
#endif
  r->epoch = smc_reuse_epoch;

  for (i = 0; i < n; i++) {
    u8 *src = ram_translation_cache + xe[i].src, *target;
    if (xe[i].target == 0x00000008)
      target = bios_swi_entrypoint;
    else if (thumb)
      target = XLAT_LINK_LOOKUP(thumb, 1, xe[i].target);
    else
      target = XLAT_LINK_LOOKUP(arm, 0, xe[i].target);
    if (!target || target == BLOCK_LOOKUP_UNMAPPABLE)
      return false;
#ifdef SMC_REUSE_DEBUG   /* mutant 1024: look the exits up but never re-patch */
    if (getenv("SMC_REUSE_DBG") && (atoi(getenv("SMC_REUSE_DBG")) & 1024))
      continue;
#endif
    if (address32(src, 0) != ((mips_opcode_j << 26) |
                              (mips_absolute_offset(target) & 0x3FFFFFF))
#ifdef SMC_REUSE_DEBUG   /* experiment bit 2: always re-patch */
        || smc_reuse_exp(2)
#endif
        ) {
      JIT_PATCH_BRANCH(src, target);
      smc_reuse_relinked++;
#ifndef JIT_CODE_DISCIPLINE
      platform_cache_sync(src, src + 4);
#endif
    }
  }
#ifdef SMC_REUSE_DEBUG   /* experiment bit 1: forget every dispatch entry */
  if (smc_reuse_exp(1))
    dispatch_cache_clear();
#endif
  return true;
}

/* flush_translation_cache_ram: 1 = keep the native code (a virtual flush). */
static int smc_reuse_on_flush(void)
{
  int keep;
#ifndef SMC_REUSE_ARMING
  /* Exact by the spare invariant: always on.  A flush keeps the code when
   * all of it fits in the spare (the virtual pointer restarts at 0). */
  keep = smc_reuse_env_ok() && !smc_reuse_force_real && smc_reuse_mode &&
         smc_reuse_excess(0) + (s32)smc_reuse_vptr <= (s32)SMC_REUSE_SPARE;
  smc_reuse_force_real = 0;
  smc_reuse_vptr = 0;
  smc_reuse_epoch++;
  if (keep)
    smc_reuse_kept++;
  else {
    smc_reuse_resets++;
    smc_reuse_drop();
  }
  return keep;
#endif
  if (flush_ram_full == smc_reuse_last_full)
    smc_reuse_win_flushes++;             /* not a cache-full flush */
  else
    /* A game that fills its RAM cache is where contract point 5 can bite:
     * stand down until the dynarec is reset (reset, state load, ROM load). */
    smc_reuse_full_seen = 1, smc_reuse_armed = 0;
  if (smc_reuse_force_real)
    smc_reuse_full_seen = 0;
  smc_reuse_last_full = flush_ram_full;
  smc_reuse_window();
  keep = smc_reuse_env_ok() && smc_reuse_armed && !smc_reuse_force_real &&
             (u32)(ram_translation_ptr - ram_translation_cache) <=
               ram_translation_cache_size / 2;
  smc_reuse_force_real = 0;
  smc_reuse_vptr = 0;
  if (keep)
    smc_reuse_kept++;
  else {
    smc_reuse_resets++;
    smc_reuse_drop();
  }
  return keep;
}
#endif


bool translate_block_arm(u32 pc, bool ram_region)
{
  DRPH_SCOPE(DRPH_XLAT);
  u32 opcode = 0;
  u32 last_opcode;
  u32 condition;
  u32 last_condition;
  u32 pc_region = (pc >> 15);
  u32 new_pc_region;
  u8 *pc_address_block = memory_map_read[pc_region];
  u32 block_start_pc = pc;
  u32 block_end_pc = pc;
  u32 block_exit_position = 0;
  s32 block_data_position = 0;
  u32 external_block_exit_position = 0;
  u32 branch_target;
  u32 cycle_count = 0;
  u8 *translation_target;
  u8 *backpatch_address = NULL;
  u8 *translation_ptr = NULL;
  u8 *translation_cache_limit = NULL;
  s32 i;
  u32 flag_status;
  u32 scan_exit_reason = 0;   /* phase 5g diagnostic: why the scan stopped */
#ifdef ENTRY_GATES
  u32 entry_gate_stop = 0;
  u8 *entry_gate_branch_source = NULL;
  u32 entry_gate_branch_target = 0;
#endif
#ifdef SMC_PARTIAL_DIRECT_GATE
  u8 *smc_gate_branch_source = NULL;
  u32 smc_gate_branch_target = 0;
#endif
#ifdef STATIC_ALU_PC
  block_exit_type salu_exits[SALU_MAX_EXITS];
  u32 salu_n = 0;
  block_exit_type external_block_exits[MAX_EXITS + SALU_MAX_EXITS];
#else
  block_exit_type external_block_exits[MAX_EXITS];
#endif
#ifdef SMC_REUSE
  u8 *smc_reuse_first = NULL;
  u32 smc_reuse_chk = 0;
#endif
  generate_block_extra_vars_arm();
  arm_fix_pc();
#ifdef SMC_IMM_PATCH
  immp_npend = 0;
  immp_blk_on = 0;
#endif

  if(!pc_address_block)
    pc_address_block = load_gamepak_page(pc_region & 0x3FF);

  if (ram_region) {
    translation_ptr = ram_translation_ptr;
    translation_cache_limit = &ram_translation_cache[
       ram_translation_cache_size - TRANSLATION_CACHE_LIMIT_THRESHOLD
       - (0x10000 - ram_block_tag) / 2 * sizeof(ramtag_type)];
  } else {
    translation_ptr = rom_translation_ptr;
    translation_cache_limit =
     rom_translation_cache + rom_translation_cache_size -
     TRANSLATION_CACHE_LIMIT_THRESHOLD;
  }

#ifdef SMC_REUSE
  if (ram_region) {
    smc_reuse_first = translation_ptr;
    smc_reuse_nlog = 0;
    smc_reuse_nmut = 0;
    translation_cache_limit = smc_reuse_limit(translation_ptr,
                                              translation_cache_limit);
  }
#endif
#ifdef JIT_CODE_DISCIPLINE
  /* Everything this block emits lies in [jit_emit_start, the committed
   * translation_ptr); it is recorded as one span at commit. */
  u8 *jit_emit_start = translation_ptr;
#endif
  DRPROF_NOTE(translation_ptr, DRC_PROLOGUE);
  generate_block_prologue();

  /* This is a function because it's used a lot more than it might seem (all
     of the data processing functions can access it), and its expansion was
     massacreing the compiler. */

#ifdef XLAT_SPLIT_DIAG
  /* DIAGNOSTIC ONLY (never shipped): hardware split of RAM ARM translation
   * into scan / emit loop, reported through two phase counters that are
   * zero on the titles studied (dsnd = scan us, rfu = emit us). */
  u32 xsd_t0 = (ram_region && core_phase_lvl >= CORE_PHASE_FINE) ? smc_prof_clock() : 0;
#endif
  if(ram_region)
  {
#ifdef XLAT_SCANFN_ARM
    {
      xlat_scan_state st;
      st.pc = pc;
      st.block_end_pc = block_end_pc;
      st.pc_region = pc_region;
      st.pc_address_block = pc_address_block;
      st.block_exit_position = block_exit_position;
      st.block_data_position = block_data_position;
      st.scan_exit_reason = scan_exit_reason;
#ifdef ENTRY_GATES
      st.entry_gate_stop = entry_gate_stop;
#endif
#ifdef JIT_SCAN_FAST
      if (xlat_scan_fast)
        xlat_scan_arm_ram_fast(&st);
      else
#endif
      xlat_scan_arm_ram(&st);
      block_end_pc = st.block_end_pc;
      pc_region = st.pc_region;
      new_pc_region = st.new_pc_region;
      pc_address_block = st.pc_address_block;
      block_exit_position = st.block_exit_position;
      block_data_position = st.block_data_position;
      opcode = st.opcode;
      condition = st.condition;
      branch_target = st.branch_target;
      scan_exit_reason = st.scan_exit_reason;
#ifdef ENTRY_GATES
      entry_gate_stop = st.entry_gate_stop;
#endif
    }
#else
    scan_block(arm, yes);
#endif
    /* scan_block has just tagged exactly [block_start_pc, block_end_pc). */
    smc_blk_note_block(block_start_pc, block_end_pc, 0, scan_exit_reason);
    STALE_NOTE(block_start_pc, block_end_pc, 0);
#ifdef SMC_WRITE_HISTO
    smc_cover_note(block_start_pc, block_end_pc, scan_exit_reason);
#endif
#ifdef SMC_IMM_PATCH
    immp_blk_begin(ram_region, block_start_pc, block_end_pc);
#endif
  }
  else
  {
    scan_block(arm, no);
    VRAM_CODE_NOTE(block_start_pc, block_end_pc);
  }

  XPP_SNAP(arm_instruction_width);
  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target > block_start_pc) &&
     (branch_target < block_end_pc))
    {
      block_data[(branch_target - block_start_pc) /
       arm_instruction_width].update_cycles = 1;
#ifdef COND_ENTRY_FIX
      /* A conditional entry point needs its own condition check.  The
       * translator emits one header per run of same-condition instructions,
       * so a branch landing mid-run executed the rest of the run
       * unconditionally (Mario Golf's stack-resident loop: bne back into
       * ANDEQ words with Z clear; corpus-classify 2026-10-28).  0x40, like
       * the flag-setting 0x20, makes translate_arm_instruction close the run
       * here (its skip lands on the entry) and open a fresh header. */
      if((block_data[(branch_target - block_start_pc) /
          arm_instruction_width].condition & 0x0F) != 0x0E)
        block_data[(branch_target - block_start_pc) /
         arm_instruction_width].condition |= 0x40;
#endif
    }
  }
  XPP_CHECK(arm_instruction_width, 0);

#ifdef XLAT_SPLIT_DIAG
  u32 xsd_t1 = 0;
  if (xsd_t0) { xsd_t1 = smc_prof_clock(); cph_dsnd += xsd_t1 - xsd_t0; }
#endif
  arm_dead_flag_eliminate();

  block_exit_position = 0;
  block_data_position = 0;

  last_condition = 0x0E;

#ifdef XLAT_FAST_LIMITS
  u32 lim_first = pc, lim_last = ~0U;
#endif
  while(pc != block_end_pc)
  {
    block_data[block_data_position].block_offset = translation_ptr;
    arm_base_cycles();

    if (pc == cheat_master_hook)
    {
      DRPROF_NOTE(translation_ptr, DRC_CHEAT);
      arm_process_cheats();
    }

    pc_limits_step();
    DRPROF_NOTE(translation_ptr, drprof_class_arm(
      readaddress32(pc_address_block, (pc & 0x7FFF)), 0xF));
    translate_arm_instruction();
    block_data_position++;

    /* If it went too far the cache needs to be flushed and the process
       restarted. Because we might already be nested several stages in
       a simple recursive call here won't work, it has to pedal out to
       the beginning. */

    if(translation_ptr > translation_cache_limit) {
      pc_limits_commit(4);
      if (ram_region) {
#ifdef SMC_REUSE_DEBUG
        if (smc_reuse_dry_rec) {           /* a dry run never flushes */
          smc_reuse_dry_full++;
          return false;
        }
#endif
#ifdef SMC_REUSE
        smc_reuse_note_full(smc_reuse_first, translation_ptr);
#endif
        flush_ram_full++;
#ifdef ENTRY_GATES
        entry_gates_harvest();
#endif
        flush_translation_cache_ram();
      }
      else
        flush_translation_cache_rom();
      return false;
    }

    /* If the next instruction is a block entry point update the
       cycle counter and update */
    if (pc != block_end_pc &&
        block_data[block_data_position].update_cycles)
    {
      DRPROF_NOTE(translation_ptr, DRC_CYCLE_UPD);
      generate_cycle_update();
    }
  }

#ifdef XLAT_SPLIT_DIAG
  if (xsd_t0) cph_rfut += smc_prof_clock() - xsd_t1;
#endif
  pc_limits_commit(4);

  /* This can happen if the last instruction is *not* inconditional */
  if ((last_condition & 0x0F) != 0x0E) {
    generate_branch_patch_conditional(backpatch_address, translation_ptr);
  }
#ifdef SMC_REUSE
  if (ram_region)
    smc_reuse_chk = (u32)(translation_ptr - smc_reuse_first);
#endif

  /* Unconditionally generate translation targets. In case we hit one or
     in the unlikely case that block was too big (and not finalized) */
#ifdef ENTRY_GATES
  if (ram_region && entry_gate_stop && block_exit_position < MAX_EXITS) {
    generate_cycle_update();
    entry_gate_branch_target = pc;
    mips_emit_j_filler(entry_gate_branch_source);
    mips_emit_nop();
  } else
#endif
#ifdef SMC_PARTIAL_DIRECT_GATE
  if (ram_region && smc_partial_active && scan_exit_reason == SMC_SCAN_GATE) {
    smc_gate_branch_target = pc;
    mips_emit_j_filler(smc_gate_branch_source);
    mips_emit_nop();
  } else
#endif
  {
    /* Braced: generate_translation_gate is several statements, so the bare
     * `else` above used to guard only its first one. */
    DRPROF_NOTE(translation_ptr, DRC_TAIL);
    SMC_GATE_CHARGE_TAIL();
    generate_translation_gate(arm);
  }

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target >= block_start_pc) && (branch_target < block_end_pc))
    {
      /* Internal branch, patch to recorded address */
      translation_target =
       block_data[(branch_target - block_start_pc) /
        arm_instruction_width].block_offset;

      generate_branch_patch_unconditional(block_exits[i].branch_source,
       translation_target);
    }
    else
    {
      /* External branch, save for later */
      external_block_exits[external_block_exit_position].branch_target =
       branch_target;
      external_block_exits[external_block_exit_position].branch_source =
       block_exits[i].branch_source;
      external_block_exit_position++;
    }
  }

#ifdef ENTRY_GATES
  if (entry_gate_branch_source) {
    external_block_exits[external_block_exit_position].branch_target =
     entry_gate_branch_target;
    external_block_exits[external_block_exit_position].branch_source =
     entry_gate_branch_source;
    external_block_exit_position++;
  }
#endif
#ifdef STATIC_ALU_PC
  /* Always external (see STATIC_ALU_PC): linked to the block that starts at
   * the target, like the dispatcher would have found. */
  for(i = 0; i < (s32)salu_n; i++)
    external_block_exits[external_block_exit_position++] = salu_exits[i];
#endif
#ifdef SMC_PARTIAL_SAFE
  /* ROM survives a RAM-cache flush, so a direct ROM->RAM jump could later
   * enter an overwritten cache slot. Route only those persistent links
   * through a local island. RAM->RAM links stay direct on the normal path;
   * their source disappears whenever the RAM cache is fully reset. */
  for(i = 0; i < external_block_exit_position; i++)
  {
    branch_target = external_block_exits[i].branch_target;
    if (STALE_ALL_ENTRIES_COND(ram_region, branch_target) &&
        (((GBA_PC(branch_target) >> 24) == 2) ||
         ((GBA_PC(branch_target) >> 24) == 3)))
    {
      translation_target = translation_ptr;
      generate_load_pc(reg_a0, GBA_PC(branch_target));
      mips_emit_j(mips_absolute_offset(mips_indirect_branch_arm));
      mips_emit_nop();
      generate_branch_patch_unconditional(
        external_block_exits[i].branch_source, translation_target);
      external_block_exits[i].branch_source = NULL;
    }
  }
#endif

  /* AN UNTRANSLATABLE EXIT MUST NOT BE PATCHED IN AS A DIRECT JUMP.
   *
   * block_lookup_translate_* answers BLOCK_LOOKUP_UNMAPPABLE -- (u8 *)(~0) --
   * for a target outside the regions it handles, and the eager patch loop
   * below only ever tested for NULL.  The sentinel therefore reached
   * generate_branch_patch_unconditional, which emits
   *
   *     j ((0xFFFFFFFF / 4) & 0x3FFFFFF)   ==   j 0x3FFFFFF
   *
   * and MIPS `j` keeps the delay slot's top four PC bits, so the first
   * execution of that branch fetches from 0x0FFFFFFC.  Nothing is mapped
   * there on a PSP: the console takes an instruction-fetch fault with no
   * handler and freezes hard, with no dispatcher call anywhere in the path --
   * which is why BADJUMP_SAFE, which guards block_lookup_address_*, never saw
   * it and why the freeze survived that fix.
   *
   * Speculatively scanned blocks produce these targets routinely -- 223 in one
   * Heart & Soul battle, every one from a block a translation gate started at
   * a non-instruction boundary, and almost none ever executed.  So the rule is
   * containment rather than prediction: send the branch through the runtime
   * dispatcher.  That is lazy (an exit that is never taken costs nothing at
   * all) and it puts the fault back inside BADJUMP_SAFE's reach, where an
   * unmappable guest pc resets the GBA instead of the console.
   *
   * It runs HERE, before translation_ptr is published, because the island has
   * to come out of this block's emit budget; the region test is a pure
   * function of the target address, so nothing needs translating to decide.
   * Worst case is MAX_EXITS islands of 16 bytes = 512 B, and the loop above
   * can only claim an exit this one would (their region sets are disjoint), so
   * the two together stay inside TRANSLATION_CACHE_LIMIT_THRESHOLD's 2 KB. */
  for(i = 0; i < external_block_exit_position; i++)
  {
    if (!external_block_exits[i].branch_source)
      continue;
    branch_target = external_block_exits[i].branch_target;
    if (branch_target == 0x00000008 || gba_pc_translatable(branch_target))
      continue;
    badjump_contained++;
    generate_branch_patch_unconditional(
      external_block_exits[i].branch_source, bios_swi_entrypoint);
    external_block_exits[i].branch_source = NULL;
  }

#ifdef SMC_REUSE_DEBUG
  if (ram_region && smc_reuse_dry_rec) {
    smc_reuse_dry_compare(smc_reuse_first, translation_ptr, block_end_pc,
                          scan_exit_reason, smc_reuse_chk, external_block_exits,
                          external_block_exit_position);
#ifdef SMC_IMM_PATCH
    immp_npend = 0;      /* a dry run registers nothing (no immp_commit) */
    immp_blk_on = 0;
#endif
    return false;
  }
#endif
  if (ram_region)
    ram_translation_ptr = translation_ptr;
  else
    rom_translation_ptr = translation_ptr;
#ifdef JIT_CODE_DISCIPLINE
  JIT_CODE_EMITTED(jit_emit_start, translation_ptr);
  DRPROF_BLK_EMITTED(jit_emit_start, translation_ptr,
    jit_emit_start + (ram_region ? ram_block_prologue_size : block_prologue_size),
    block_start_pc, block_end_pc, 0 | (ram_region ? 2 : 0));
#ifdef SMC_IMM_PATCH
  if (ram_region)
    immp_commit(block_start_pc, block_end_pc);
#endif
#endif
#ifdef SMC_REUSE
  if (ram_region)
    smc_reuse_commit(block_start_pc, block_end_pc, 0, scan_exit_reason,
                     smc_reuse_first, smc_reuse_chk, external_block_exits,
                     external_block_exit_position);
#endif

#ifdef SMC_PARTIAL_DIRECT_GATE
  if (smc_gate_branch_source) {
    translation_target = block_lookup_translate_arm(smc_gate_branch_target);
    if (!translation_target || translation_target == BLOCK_LOOKUP_UNMAPPABLE)
      return false;
    JIT_PATCH_BRANCH(smc_gate_branch_source, translation_target);
  }
#endif

  for(i = 0; i < external_block_exit_position; i++)
  {
    if (!external_block_exits[i].branch_source)
      continue;
    branch_target = external_block_exits[i].branch_target;
    if(branch_target == 0x00000008)
      translation_target = bios_swi_entrypoint;
    else
      translation_target = XLAT_LINK_LOOKUP(arm, 0, branch_target);
    /* Backstop for the classifier above: the sentinel must never reach
     * generate_branch_patch_unconditional.  Aborting the translation is the
     * already-established safe path -- the caller retries, then BADJUMP_SAFE
     * resets the guest. */
    if (!translation_target || translation_target == BLOCK_LOOKUP_UNMAPPABLE)
      return false;
#ifdef SMC_REUSE
    if (!ram_region && (((GBA_PC(branch_target) >> 24) == 2) ||
                        ((GBA_PC(branch_target) >> 24) == 3)))
      smc_reuse_rom2ram++;   /* a link the lookup cannot guard: see contract 2 */
#endif
    JIT_PATCH_BRANCH(external_block_exits[i].branch_source,
                     translation_target);
  }
  return true;
}

bool translate_block_thumb(u32 pc, bool ram_region)
{
  DRPH_SCOPE(DRPH_XLAT);
  u32 opcode = 0;
  u32 last_opcode;
  u32 condition;
  u32 pc_region = (pc >> 15);
  u32 new_pc_region;
  u8 *pc_address_block = memory_map_read[pc_region];
  u32 block_start_pc = pc;
  u32 block_end_pc = pc;
  u32 block_exit_position = 0;
  s32 block_data_position = 0;
  u32 external_block_exit_position = 0;
  u32 branch_target;
  u32 cycle_count = 0;
  u8 *translation_target;
  u8 *backpatch_address = NULL;
  u8 *translation_ptr = NULL;
  u8 *translation_cache_limit = NULL;
  s32 i;
  u32 flag_status;
  u32 scan_exit_reason = 0;   /* phase 5g diagnostic: why the scan stopped */
#ifdef ENTRY_GATES
  u32 entry_gate_stop = 0;
  u8 *entry_gate_branch_source = NULL;
  u32 entry_gate_branch_target = 0;
#endif
#ifdef SMC_PARTIAL_DIRECT_GATE
  u8 *smc_gate_branch_source = NULL;
  u32 smc_gate_branch_target = 0;
#endif
  block_exit_type external_block_exits[MAX_EXITS];
#ifdef SMC_REUSE
  u8 *smc_reuse_first = NULL;
  u32 smc_reuse_chk = 0;
#endif
  generate_block_extra_vars_thumb();
  thumb_fix_pc();
#ifdef SMC_IMM_PATCH
  immp_blk_on = 0;
#endif

  if(!pc_address_block)
    pc_address_block = load_gamepak_page(pc_region & 0x3FF);

  if (ram_region) {
    translation_ptr = ram_translation_ptr;
    translation_cache_limit = &ram_translation_cache[
       ram_translation_cache_size - TRANSLATION_CACHE_LIMIT_THRESHOLD
       - (0x10000 - ram_block_tag) / 2 * sizeof(ramtag_type)];
  } else {
    translation_ptr = rom_translation_ptr;
    translation_cache_limit = &rom_translation_cache[
       rom_translation_cache_size - TRANSLATION_CACHE_LIMIT_THRESHOLD];
  }

#ifdef SMC_REUSE
  if (ram_region) {
    smc_reuse_first = translation_ptr;
    smc_reuse_nlog = 0;
    smc_reuse_nmut = 0;
    translation_cache_limit = smc_reuse_limit(translation_ptr,
                                              translation_cache_limit);
  }
#endif
#ifdef JIT_CODE_DISCIPLINE
  /* Everything this block emits lies in [jit_emit_start, the committed
   * translation_ptr); it is recorded as one span at commit. */
  u8 *jit_emit_start = translation_ptr;
#endif
  DRPROF_NOTE(translation_ptr, DRC_PROLOGUE);
  generate_block_prologue();

  /* This is a function because it's used a lot more than it might seem (all
     of the data processing functions can access it), and its expansion was
     massacreing the compiler. */

  if(ram_region)
  {
    scan_block(thumb, yes);
    smc_blk_note_block(block_start_pc, block_end_pc, 1, scan_exit_reason);
    STALE_NOTE(block_start_pc, block_end_pc, 1);
#ifdef SMC_WRITE_HISTO
    smc_cover_note(block_start_pc, block_end_pc, scan_exit_reason);
#endif
  }
  else
  {
    scan_block(thumb, no);
    VRAM_CODE_NOTE(block_start_pc, block_end_pc);
  }

  XPP_SNAP(thumb_instruction_width);
  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target > block_start_pc) &&
     (branch_target < block_end_pc))
    {
      block_data[(branch_target - block_start_pc) /
       thumb_instruction_width].update_cycles = 1;
    }
  }
  XPP_CHECK(thumb_instruction_width, 1);

  thumb_dead_flag_eliminate();

  block_exit_position = 0;
  block_data_position = 0;

#ifdef XLAT_FAST_LIMITS
  u32 lim_first = pc, lim_last = ~0U;
#endif
  while(pc != block_end_pc)
  {
#ifdef XLAT_THUMB_NO_UNSWITCH
    /* XLAT_THUMB_NO_UNSWITCH (cpu-levers, 2026-10-26): host compiler only.
     * It changes no emitted code and no guest behaviour.
     *
     * psp-gcc 15.2 -O3 unswitches this loop on `ram_region`
     * (-fopt-info-loop: "unswitching loop ... ram_region != 0"; the tests are
     * the cache-overflow flush, pc_limits_commit and the Thumb literal-pool
     * check).  That compiles the whole Thumb decoder twice, once for RAM and
     * once for ROM: 37.7k MIPS instructions.  In the doubled function the
     * allocator keeps translation_ptr in a stack slot, giving 1545 stack
     * stores and 2973 stack loads, about one pair per emitted word.
     *
     * The empty asm tells the compiler ram_region may change on each
     * iteration, so there is nothing to unswitch on.  It emits no
     * instruction (a "+r" operand already in a register), and the value
     * never really changes.  tools/xlat_spill_guard.sh fails the build if
     * the spill comes back. */
    __asm__ ("" : "+r" (ram_region));
#endif
    block_data[block_data_position].block_offset = translation_ptr;
    thumb_base_cycles();

    if (pc == cheat_master_hook)
    {
      DRPROF_NOTE(translation_ptr, DRC_CHEAT);
      thumb_process_cheats();
    }

    pc_limits_step();
    DRPROF_NOTE(translation_ptr, drprof_class_thumb(
      readaddress16(pc_address_block, (pc & 0x7FFF)),
      block_data[block_data_position].flag_data));
#ifdef DRPROF_NEGCTL
    /* The oracle's NEGATIVE control (twin only): one extra cycle per Thumb
     * instruction is a real timing change and dr_oracle.py must flag it. */
    cycle_count++;
#endif
    translate_thumb_instruction();
    block_data_position++;

    /* If it went too far the cache needs to be flushed and the process
       restarted. Because we might already be nested several stages in
       a simple recursive call here won't work, it has to pedal out to
       the beginning. */

    if(translation_ptr > translation_cache_limit)
    {
      pc_limits_commit(2);
      if (ram_region) {
#ifdef SMC_REUSE_DEBUG
        if (smc_reuse_dry_rec) {           /* a dry run never flushes */
          smc_reuse_dry_full++;
          return false;
        }
#endif
#ifdef SMC_REUSE
        smc_reuse_note_full(smc_reuse_first, translation_ptr);
#endif
        flush_ram_full++;
#ifdef ENTRY_GATES
        entry_gates_harvest();
#endif
        flush_translation_cache_ram();
      }
      else
        flush_translation_cache_rom();
      return false;
    }

    /* If the next instruction is a block entry point update the
       cycle counter and update */
    if (pc != block_end_pc &&
        block_data[block_data_position].update_cycles)
    {
      DRPROF_NOTE(translation_ptr, DRC_CYCLE_UPD);
      generate_cycle_update();
    }
  }
  pc_limits_commit(2);
#ifdef SMC_REUSE
  if (ram_region)
    smc_reuse_chk = (u32)(translation_ptr - smc_reuse_first);
#endif

  /* Unconditionally generate translation targets. In case we hit one or
     in the unlikely case that block was too big (and not finalized) */
#ifdef ENTRY_GATES
  if (ram_region && entry_gate_stop && block_exit_position < MAX_EXITS) {
    generate_cycle_update();
    entry_gate_branch_target = pc;
    mips_emit_j_filler(entry_gate_branch_source);
    mips_emit_nop();
  } else
#endif
#ifdef SMC_PARTIAL_DIRECT_GATE
  if (ram_region && smc_partial_active && scan_exit_reason == SMC_SCAN_GATE) {
    smc_gate_branch_target = pc;
    mips_emit_j_filler(smc_gate_branch_source);
    mips_emit_nop();
  } else
#endif
  {
    DRPROF_NOTE(translation_ptr, DRC_TAIL);
    SMC_GATE_CHARGE_TAIL();   /* braced for the same reason as the ARM side */
    generate_translation_gate(thumb);
  }

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target >= block_start_pc) && (branch_target < block_end_pc))
    {
      /* Internal branch, patch to recorded address */
      translation_target =
       block_data[(branch_target - block_start_pc) /
        thumb_instruction_width].block_offset;

      generate_branch_patch_unconditional(block_exits[i].branch_source,
       translation_target);
    }
    else
    {
      /* External branch, save for later */
      external_block_exits[external_block_exit_position].branch_target =
       branch_target;
      external_block_exits[external_block_exit_position].branch_source =
       block_exits[i].branch_source;
      external_block_exit_position++;
    }
  }

#ifdef ENTRY_GATES
  if (entry_gate_branch_source) {
    external_block_exits[external_block_exit_position].branch_target =
     entry_gate_branch_target;
    external_block_exits[external_block_exit_position].branch_source =
     entry_gate_branch_source;
    external_block_exit_position++;
  }
#endif
#ifdef SMC_PARTIAL_SAFE
  for(i = 0; i < external_block_exit_position; i++)
  {
    branch_target = external_block_exits[i].branch_target;
    if (STALE_ALL_ENTRIES_COND(ram_region, branch_target) &&
        (((GBA_PC(branch_target) >> 24) == 2) ||
         ((GBA_PC(branch_target) >> 24) == 3)))
    {
      translation_target = translation_ptr;
      generate_load_pc(reg_a0, GBA_PC(branch_target));
      mips_emit_j(mips_absolute_offset(mips_indirect_branch_thumb));
      mips_emit_nop();
      generate_branch_patch_unconditional(
        external_block_exits[i].branch_source, translation_target);
      external_block_exits[i].branch_source = NULL;
    }
  }
#endif

  /* AN UNTRANSLATABLE EXIT MUST NOT BE PATCHED IN AS A DIRECT JUMP.
   *
   * block_lookup_translate_* answers BLOCK_LOOKUP_UNMAPPABLE -- (u8 *)(~0) --
   * for a target outside the regions it handles, and the eager patch loop
   * below only ever tested for NULL.  The sentinel therefore reached
   * generate_branch_patch_unconditional, which emits
   *
   *     j ((0xFFFFFFFF / 4) & 0x3FFFFFF)   ==   j 0x3FFFFFF
   *
   * and MIPS `j` keeps the delay slot's top four PC bits, so the first
   * execution of that branch fetches from 0x0FFFFFFC.  Nothing is mapped
   * there on a PSP: the console takes an instruction-fetch fault with no
   * handler and freezes hard, with no dispatcher call anywhere in the path --
   * which is why BADJUMP_SAFE, which guards block_lookup_address_*, never saw
   * it and why the freeze survived that fix.
   *
   * Speculatively scanned blocks produce these targets routinely -- 223 in one
   * Heart & Soul battle, every one from a block a translation gate started at
   * a non-instruction boundary, and almost none ever executed.  So the rule is
   * containment rather than prediction: send the branch through the runtime
   * dispatcher.  That is lazy (an exit that is never taken costs nothing at
   * all) and it puts the fault back inside BADJUMP_SAFE's reach, where an
   * unmappable guest pc resets the GBA instead of the console.
   *
   * It runs HERE, before translation_ptr is published, because the island has
   * to come out of this block's emit budget; the region test is a pure
   * function of the target address, so nothing needs translating to decide.
   * Worst case is MAX_EXITS islands of 16 bytes = 512 B, and the loop above
   * can only claim an exit this one would (their region sets are disjoint), so
   * the two together stay inside TRANSLATION_CACHE_LIMIT_THRESHOLD's 2 KB. */
  for(i = 0; i < external_block_exit_position; i++)
  {
    if (!external_block_exits[i].branch_source)
      continue;
    branch_target = external_block_exits[i].branch_target;
    if (branch_target == 0x00000008 || gba_pc_translatable(branch_target))
      continue;
    badjump_contained++;
    generate_branch_patch_unconditional(
      external_block_exits[i].branch_source, bios_swi_entrypoint);
    external_block_exits[i].branch_source = NULL;
  }

#ifdef SMC_REUSE_DEBUG
  if (ram_region && smc_reuse_dry_rec) {
    smc_reuse_dry_compare(smc_reuse_first, translation_ptr, block_end_pc,
                          scan_exit_reason, smc_reuse_chk, external_block_exits,
                          external_block_exit_position);
    return false;
  }
#endif
  if (ram_region)
    ram_translation_ptr = translation_ptr;
  else
    rom_translation_ptr = translation_ptr;
#ifdef JIT_CODE_DISCIPLINE
  JIT_CODE_EMITTED(jit_emit_start, translation_ptr);
  DRPROF_BLK_EMITTED(jit_emit_start, translation_ptr,
    jit_emit_start + (ram_region ? ram_block_prologue_size : block_prologue_size),
    block_start_pc, block_end_pc, 1 | (ram_region ? 2 : 0));
#ifdef SMC_IMM_PATCH
  if (ram_region)
    immp_bad_range(block_start_pc, block_end_pc);
#endif
#endif
#ifdef SMC_REUSE
  if (ram_region)
    smc_reuse_commit(block_start_pc, block_end_pc, 1, scan_exit_reason,
                     smc_reuse_first, smc_reuse_chk, external_block_exits,
                     external_block_exit_position);
#endif

#ifdef SMC_PARTIAL_DIRECT_GATE
  if (smc_gate_branch_source) {
    translation_target = block_lookup_translate_thumb(smc_gate_branch_target);
    if (!translation_target || translation_target == BLOCK_LOOKUP_UNMAPPABLE)
      return false;
    JIT_PATCH_BRANCH(smc_gate_branch_source, translation_target);
  }
#endif

  for(i = 0; i < external_block_exit_position; i++)
  {
    if (!external_block_exits[i].branch_source)
      continue;
    branch_target = external_block_exits[i].branch_target;
    if(branch_target == 0x00000008)
      translation_target = bios_swi_entrypoint;
    else
      translation_target = XLAT_LINK_LOOKUP(thumb, 1, branch_target);
    /* Backstop for the classifier above: the sentinel must never reach
     * generate_branch_patch_unconditional.  Aborting the translation is the
     * already-established safe path -- the caller retries, then BADJUMP_SAFE
     * resets the guest. */
    if (!translation_target || translation_target == BLOCK_LOOKUP_UNMAPPABLE)
      return false;
#ifdef SMC_REUSE
    if (!ram_region && (((GBA_PC(branch_target) >> 24) == 2) ||
                        ((GBA_PC(branch_target) >> 24) == 3)))
      smc_reuse_rom2ram++;   /* a link the lookup cannot guard: see contract 2 */
#endif
    JIT_PATCH_BRANCH(external_block_exits[i].branch_source,
                     translation_target);
  }
  return true;
}

void init_bios_hooks(void)
{
  /* init_emitter has just written the handler stubs into
   * [rom_translation_cache, watermark), and nothing else ever syncs that
   * range: translate_icache_sync only covers [last, ptr), and last is set
   * past the stubs right below.  Emitted code reaches them by `j`/`b`, so
   * they must be visible to instruction fetch.  With the cache in the
   * EBOOT's .bss this held by luck (the stubs are re-emitted identically at
   * the same address on every load); with a heap-allocated cache the bytes
   * under them were ordinary data a moment ago, so make it explicit.  Cost:
   * one writeback + invalidate of a few KiB per ROM load / emitter rebuild. */
#ifdef JIT_CODE_DISCIPLINE
  /* An emitter rebuild is an ownership change: the STUB zone was just
   * rewritten (and with a heap-allocated cache it may have held data).  The
   * stubs are recorded and published, with a whole I-cache invalidate, by
   * the lookup of the BIOS entry below -- before any of them can run. */
  jit_code_owner_changed(JOWN_REBUILD);
  JIT_CODE_WROTE(rom_translation_cache,
                 &rom_translation_cache[rom_cache_watermark]);
#else
  platform_cache_sync(rom_translation_cache,
                      &rom_translation_cache[rom_cache_watermark]);
#endif

  // Pre-generate this entry point so that we can safely invoke fast
  // SWI calls from ROM and RAM regardless of cache flushes.
  rom_translation_ptr = &rom_translation_cache[rom_cache_watermark];
  last_rom_translation_ptr = rom_translation_ptr;
#ifdef DRPROF_TWIN
  drprof_stub_end = rom_translation_ptr;   /* emitter stubs end here */
#endif
  bios_swi_entrypoint = block_lookup_address_arm(0x8);
  rom_cache_watermark = (u32)(rom_translation_ptr - rom_translation_cache);
#ifdef JIT_CODE_DISCIPLINE
  /* The STUB zone now ends here and is immutable until the next rebuild. */
  jit_code_publish();
  jit_stub_zone_sealed = 1;
#endif
}

void flush_translation_cache_ram(void)
{
  DRPH_SCOPE(DRPH_FLUSH);
  DRPROF_ZONE_ENTER(DRZ_FLUSH);
#ifdef SMC_REUSE
  /* 1 = a VIRTUAL flush: everything below happens except that the native
   * code stays in place, dormant, for smc_reuse_revive. */
  int smc_reuse_keep = smc_reuse_on_flush();
#else
#define smc_reuse_keep 0
#endif
#ifdef JIT_CODE_DISCIPLINE
  /* The RAM zone's addresses are about to hold a new generation of code. */
  /* SMC_REUSE_WHOLE_I (hardware A/B only): invalidate the whole I-cache on a
   * virtual flush as well, exactly as often as without SMC_REUSE. */
#ifdef SMC_REUSE_WHOLE_I
  jit_code_owner_changed(JOWN_RAM_FLUSH);
#else
  if (!smc_reuse_keep)
    jit_code_owner_changed(JOWN_RAM_FLUSH);
#endif
#endif
#ifdef DRPROF_TWIN
  if (!smc_reuse_keep)
    drprof_forget_ram();
#endif
#ifdef DISPATCH_CACHE
  dispatch_cache_clear();
#endif
  /* Flushes RAM caches avoiding doing too much work (ie. wiping unused memory) */
  flush_ram_count++;
  flush_ram_total++;
  /*printf("ram flush %d (pc %x), %x to %x, %x to %x\n",
   flush_ram_count, reg[REG_PC], iwram_code_min, iwram_code_max,
   ewram_code_min, ewram_code_max);*/

  /* Phase 5g: price the flush directly instead of inferring it from a frame
   * spike.  Two clock reads on a path that is already the expensive one. */
  u32 flush_t0 = smc_prof_clock ? smc_prof_clock() : 0;

#ifdef SMC_PARTIAL_SAFE
  smc_retired_tag_count = 0;
  smc_partial_wrap_live = 0;
#ifdef SMC_BLOCK_EXACT
  smc_block_pending_full = 0;
#endif
#endif
#ifdef SMC_RETIRE_WINDOW
  smc_max_extent[0] = smc_max_extent[1] = 0;
#if defined(SMC_RETIRE_LOCAL) && !defined(SMC_LOCAL_COLD)
  smc_back_reset();
#endif
#endif

  if (!smc_reuse_keep) {
    last_ram_translation_ptr = ram_translation_cache;
    ram_translation_ptr = ram_translation_cache;
  }
#ifndef SMC_REUSE
#undef smc_reuse_keep
#endif
#if defined(SMC_SAME_SKIP) && defined(SMC_PARTIAL_SINGLE)
  xs_clear_all();        /* every RAM translation is gone */
#ifdef SMC_IMM_PATCH
  immp_reset();
#endif
#endif

  // Proceed to clean the SMC area if needed
  // (also try to memset as little as possible for performance)
  if (iwram_code_max) {
    if(iwram_code_max > iwram_code_min) {
      iwram_code_min &= ~15U;
      iwram_code_max = MIN(iwram_code_max + 8, 0x8000);
      memset(&iwram[iwram_code_min], 0, iwram_code_max - iwram_code_min);
      smc_flush_bytes += iwram_code_max - iwram_code_min;
    } else {
      memset(iwram, 0, 0x8000);
      smc_flush_bytes += 0x8000;
      smc_flush_wide++;
    }
  }

  if (ewram_code_max) {
    if(ewram_code_max > ewram_code_min) {
      ewram_code_min &= ~15U;
      ewram_code_max = MIN(ewram_code_max + 8, 0x40000);
      memset(&ewram[0x40000 + ewram_code_min], 0, ewram_code_max - ewram_code_min);
      smc_flush_bytes += ewram_code_max - ewram_code_min;
    } else {
      memset(&ewram[0x40000], 0, 0x40000);
      smc_flush_bytes += 0x40000;
      smc_flush_wide++;
    }
  }

  if (smc_prof_clock)
    smc_flush_us += smc_prof_clock() - flush_t0;

  iwram_code_min = ~0U;
  iwram_code_max =  0U;
  ewram_code_min = ~0U;
  ewram_code_max =  0U;
  ram_block_tag = INITIAL_TOP_TAG;
#ifdef SMC_RETIRE_WINDOW
  /* Every RAM block is gone; from here on each one is noted iff partial
   * retirement is active. */
  smc_window_armed = smc_partial_active ? 1 : 0;
#endif
  DRPROF_ZONE_LEAVE(DRZ_FLUSH);
}

#ifdef EXEC_STALE_CHECK
/* DIAGNOSTIC (dr2-nfsu, twin only; mips_emit.h emit_trace_instruction):
 * an executed RAM instruction whose guest opcode no longer matches the one
 * it was translated from is stale code.  Counts to stderr at exit; the first
 * 16 are listed; EXEC_STALE_ABORT=1 in the environment aborts on the first. */
static u32 esc_hits, esc_checks;
static int esc_abort = -1;
#ifdef SMC_IMM_PATCH
/* IMMP_VERIFY (xlat-cost 2026-10-28; rides on EXEC_STALE_CHECK, twin only):
 * the opcode compare above sees a site whose EXEC_STALE opcode load was not
 * patched, but not one whose ALU immediate is wrong while its opcode load is
 * right.  So every time an instruction at a learned, not-bad word executes,
 * EVERY registered site of that word is re-derived from guest memory: the
 * `lui/ori a0` pair must encode immp_rot(word now), the opcode load the word
 * now, and the entry's record the word now.  (Invariant, see SMC_IMM_PATCH:
 * learned and not bad => every listed site encodes the current word; sites
 * of retired blocks stay listed and patched, so they are checked too.) */
static u32 immp_vchecks, immp_vhits;
static void immp_verify(u32 pc, u32 cur)
{
  const u32 lui_a0 = (0x0Fu << 26) | ((u32)reg_a0 << 16);
  const u32 lui_a2 = (0x0Fu << 26) | ((u32)reg_a2 << 16);
  u32 w = IMMP_W(pc), k;
  immp_ent *e;
  if (IMMP_GET(immp_bad, w) || !immp_slot[w])
    return;
  e = &immp_tab[immp_slot[w]];
  if (!e->n)
    return;
  immp_vchecks++;
  for (k = 0; k < e->n; k++)
    if (e->old != cur || !immp_site_ok(e->site[k], immp_rot(cur), lui_a0) ||
        (e->esc[k] && !immp_site_ok(e->esc[k], cur, lui_a2))) {
      if (immp_vhits++ < 16)
        fprintf(stderr, "IMMP_VERIFY frame=%u pc=%08x word=%08x old=%08x site=%u/%u\n",
                (unsigned)frame_counter, (unsigned)pc, (unsigned)cur,
                (unsigned)e->old, (unsigned)k, (unsigned)e->n);
      if (esc_abort)
        abort();
      return;
    }
}
#endif
static void esc_report(void)
{
  fprintf(stderr, "EXEC_STALE checks=%u hits=%u\n", (unsigned)esc_checks,
          (unsigned)esc_hits);
#ifdef SMC_IMM_PATCH
  fprintf(stderr, "IMMP_VERIFY checks=%u hits=%u\n", (unsigned)immp_vchecks,
          (unsigned)immp_vhits);
#endif
}
void exec_stale_check(u32 pc, u32 mode, u32 op)
{
  u32 cur;
  const u8 *p = (pc >> 24) == 3 ? iwram + 0x8000 + (pc & 0x7FFF)
                                : ewram + (pc & 0x3FFFF);
  if (esc_abort < 0) {
    const char *e = getenv("EXEC_STALE_ABORT");
    esc_abort = e && *e == '1';
    atexit(esc_report);
  }
  esc_checks++;
  {
    static int tf = -2; static FILE *tfp; static u32 tn;
    if (tf == -2) { const char *e = getenv("ESC_TRACE_F"); tf = e ? atoi(e) : -1; }
    if (tf >= 0 && (int)frame_counter == tf && tn < 400000) {
      if (!tfp) { const char *e = getenv("ESC_TRACE_FILE"); tfp = fopen(e ? e : "/tmp/esct.txt", "w"); }
      if (tfp) { tn++; fprintf(tfp, "%08x %08x r0=%08x r1=%08x r2=%08x r3=%08x r7=%08x r8=%08x sp=%08x lr=%08x%c",
                 (unsigned)pc, (unsigned)op, (unsigned)reg[0], (unsigned)reg[1], (unsigned)reg[2], (unsigned)reg[3],
                 (unsigned)reg[7], (unsigned)reg[8], (unsigned)reg[13], (unsigned)reg[14], 10); }
    }
  }
  cur = mode ? *(const u32 *)p : *(const u16 *)p;
#ifdef SMC_IMM_PATCH
  if (mode && IMMP_LEARNED(pc))
    immp_verify(pc, cur);
#endif
  if (cur == op)
    return;
  if (esc_hits++ < 16)
    fprintf(stderr, "EXEC_STALE frame=%u pc=%08x %s translated=%08x now=%08x "
            "last_smc_write=%08x\n", (unsigned)frame_counter, (unsigned)pc,
            mode ? "arm" : "thumb", (unsigned)op, (unsigned)cur,
            (unsigned)smc_last_write_addr);
  if (esc_abort)
    abort();
}
#endif

#ifdef SMC_EVTRACE
/* DIAGNOSTIC (twin only): one line per SMC event in a frame window. */
static FILE *smc_evt_f;
static int smc_evt_on(void)
{
  static int lo = -1, hi;
  if (lo < 0) { const char *e = getenv("SMC_EVT_LO"); lo = e ? atoi(e) : 0;
    e = getenv("SMC_EVT_HI"); hi = e ? atoi(e) : 0x7fffffff; }
  if ((int)frame_counter < lo || (int)frame_counter > hi) return 0;
  if (!smc_evt_f) { const char *e = getenv("SMC_EVT_FILE");
    smc_evt_f = fopen(e ? e : "smcevt.txt", "w"); }
  return smc_evt_f != NULL;
}
#define SMC_EVT(...) do { if (smc_evt_on()) fprintf(smc_evt_f, __VA_ARGS__); } while (0)
#else
#define SMC_EVT(...) do { } while (0)
#endif

#ifdef SMC_FALLBACK_STATS
/* DIAGNOSTIC (bench campaign 2026-10-20): why an SMC trap took the full RAM
 * flush.  [0] writer not range-safe, [1] activation, [2] range gate installed
 * or moved, [3] low/high not gated, [4] range: wrap/cross-region/bad bounds,
 * [5] range: final word untagged, [6] range: domain end, [7] range: live block
 * without extent / short trampoline, [8] range: retired-tag table full,
 * [9] partial retirements done.  Read by the frontend as `smcfb=`. */
u32 smc_fallback_stats[16];   /* [10] stale checks, [11] stale MISMATCHES,
                               * [12..14] SMC_BLOCK_EXACT word hits /
                               * retired / deferred to a full flush */
#define SMC_FB(i) do { SMC_EVT("  FB %d\n", (int)(i)); smc_fallback_stats[i]++; } while (0)
#else
#define SMC_FB(i) SMC_EVT("  FB %d\n", (int)(i))
#endif

/* Self-modifying-code entry points: identical behaviour to
 * flush_translation_cache_ram(), separate counters.  The cache-exhaustion
 * path in translate_block_{arm,thumb} calls the plain one directly after
 * bumping flush_ram_full; the dynarec stubs call one of these two.  Splitting
 * them tells a "the JIT cache is too small" problem (a bigger cache is the
 * lever) apart from a "this game keeps writing over its own code" problem
 * (only finer-grained invalidation is), and then tells the two SMC *sources*
 * apart, which want different mitigations:
 *   _smc  a single CPU store from translated code landed on a tagged
 *         halfword (smc_write in the store stubs).
 *   _dma  a DMA transfer wrote into tagged IWRAM/EWRAM.  CPU_ALERT_SMC is
 *         raised ONLY by dma_write_iwram/dma_write_ewram (gba_memory.c), so
 *         the write_io_epilogue path is exactly and only the DMA case.  A
 *         DMA knows its whole destination range up front, so this is the
 *         source a range-invalidate could actually address. */
#ifdef SMC_PARTIAL_SAFE
/* Selective SMC invalidation used by the current hardware experiment.
 * Tags identify block starts, while per-mode end addresses record exact
 * source ranges. */
/* Record a block's source extent in CANONICAL addresses.
 *
 * start_pc/end_pc are the raw pcs the scan walked, so a block translated from
 * a mirror (0x0300xxxx + k*0x8000, 0x02xxxxxx + k*0x40000) used to store a
 * raw end that the canonical overlap test in flush_translation_cache_ram_range
 * compares against canonical write addresses -- a block starting at canonical
 * S but recorded with a mirror end could be judged "not overlapping" a write
 * it really covers.  Tags are already canonical (they index by offset), so
 * rebasing the end onto base + offset(start) + length makes the extent exact
 * for every non-wrapping block; wrapping ones set smc_partial_wrap_live. */
#ifdef SMC_RETIRE_WINDOW
/* SMC_RETIRE_WINDOW (prototype, docs/DYNAREC-PROFILE.md): the longest source
 * extent recorded for a live RAM block since the last full RAM flush, per
 * region ([0] IWRAM, [1] EWRAM).  No block can start further than this below
 * a write and still cover it, so flush_translation_cache_ram_range scans
 * from lo - max_extent instead of lo - MAX_BLOCK_SIZE*4 (4 KiB): the twin
 * measured that tag walk at ~41k instructions per SMC event on Heart & Soul. */
#endif


#ifdef STALE_CHECK
static void stale_note_extent(u32 start_pc, u32 end_pc, u32 thumb)
{
  u16 *tagp;
  u32 off, base, size, len, canon_end;
  if (start_pc >= 0x03000000 && start_pc < 0x04000000) {
    tagp = (u16 *)iwram; base = 0x03000000; size = 0x8000;
  } else if (start_pc >= 0x02000000 && start_pc < 0x03000000) {
    tagp = (u16 *)(ewram + 0x40000); base = 0x02000000; size = 0x40000;
  } else
    return;
  off = start_pc & (size - 1);
  len = end_pc - start_pc;
  if (off + len > size)
    return;                    /* wrapping block: not checked (stays 0) */
  canon_end = base + off + len;
  if (VALID_TAG(tagp[off >> 1])) {
    ramtag_type *te = get_ram_tag(tagp[off >> 1]);
    if (thumb) {
      te->stale_end_thumb = canon_end;
      te->src_hash_thumb = stale_src_hash(base + off, canon_end);
    } else {
      te->stale_end_arm = canon_end;
      te->src_hash_arm = stale_src_hash(base + off, canon_end);
    }
  }
}
#endif


#if defined(XLAT_CENSUS) && defined(DRPROF_TWIN)
/* XLAT_CENSUS (twin diagnosis only, 2026-10-27): what would a translation
 * reuse mechanism save?  Every RAM block translation is classified by the
 * FNV hash of the guest bytes it was translated from:
 *   same_last  the previous translation at this (pc, mode) had the same bytes
 *              (what a one-record-per-pc reuse such as SMC_REUSE can revive)
 *   seen_any   this (pc, mode, bytes) was translated at any time before
 *              (what a multi-version reuse / an overlay cache could revive)
 * and attributed to the cause of the last RAM invalidation before it
 * (0 boot/cache-full/other, 1 DMA alert, 2 CPU store trap).  DMA alerts are
 * counted by source region with the distinct (source, destination) keys. */
#include <stdio.h>
#include <stdlib.h>
u32 census_cause, census_dma_src, census_dma_dst;
static u32 *cen_last;               /* [mode][iwram 16K + ewram 128K halfwords] */
static unsigned long long *cen_seen;  /* open addressing, 0 = empty */
#define CEN_SEEN_BITS 21
static u32 cen_x[3], cen_same[3], cen_any[3], cen_hw[3];
static u32 cen_dma[17], cen_keys, cen_seen_full;
static u32 cen_dkey[4096];
static u32 cen_frames0 = ~0u;
static void census_report(void)
{
  int c;
  fprintf(stderr, "XC frames=%u keys=%u dma_rom=%u dma_ew=%u dma_iw=%u dma_oth=%u seen_full=%u\n",
          (unsigned)(frame_counter - cen_frames0), (unsigned)cen_keys,
          (unsigned)(cen_dma[8] + cen_dma[9] + cen_dma[10] + cen_dma[11] + cen_dma[12] + cen_dma[13]),
          (unsigned)cen_dma[2], (unsigned)cen_dma[3], (unsigned)cen_dma[16],
          (unsigned)cen_seen_full);
  for (c = 0; c < 3; c++)
    fprintf(stderr, "XC cause=%d xlat=%u halves=%u same_last=%u seen_any=%u\n", c,
            (unsigned)cen_x[c], (unsigned)cen_hw[c], (unsigned)cen_same[c],
            (unsigned)cen_any[c]);
}
static void census_init(void)
{
  cen_last = calloc(2 * (0x4000 + 0x20000), 4);
  cen_seen = calloc(1u << CEN_SEEN_BITS, 8);
  cen_frames0 = frame_counter;
  atexit(census_report);
}
void census_dma_alert(void)
{
  u32 k, h;
  if (!cen_last) census_init();
  cen_dma[MIN(census_dma_src >> 24, 16)]++;
  k = census_dma_src ^ (census_dma_dst * 0x9E3779B1u);
  h = (k * 2654435761u) >> 20;
  while (cen_dkey[h & 4095] && cen_dkey[h & 4095] != k) h++;
  if (!cen_dkey[h & 4095] && cen_keys < 4000) { cen_dkey[h & 4095] = k; cen_keys++; }
  census_cause = 1;
}

#ifdef XLAT_REUSE_SIM
/* XLAT_REUSE_SIM (twin sizing only, 2026-10-27): what would a MULTI-VERSION
 * SMC_REUSE revive under the real cache lifetime?
 *
 * Model (SMC_REUSE's design, generalised): every RAM translation leaves its
 * body plus a record (40 B header + 16 B exits + the guest bytes) in a
 * virtual 384 KiB RAM cache; flushes and retirements are "virtual" (bodies
 * stay dormant); a translation whose (pc, mode, guest-byte hash) has a live
 * record is a REVIVE and costs no cache bytes.  When the virtual cache
 * overflows, it resets for real and every record dies.  The index keeps
 * RS_WAYS versions per (pc, mode) set, LRU.  Two flavours:
 *   A  capacity only (the ceiling of a well-built reuse);
 *   B  A + every record dropped when the gate count or smc_partial_active
 *      changes (bb8b7b2's environment rule).
 * Host bytes per block: the RAM translation pointer's advance between
 * consecutive RAM translations (running average when that is not usable).
 * Counted from frame 300 of the run (the profiles' window). */
#define RS_SETS 16384
#define RS_WAYS 4
#ifndef RS_CAP
#define RS_CAP  RAM_TRANSLATION_CACHE_SIZE   /* the real cache's size */
#endif
typedef struct { u32 key, hash, gen, lru; } rs_ent;
static rs_ent *rs_tab[2];
static u32 rs_v[2], rs_gen[2] = { 1, 1 }, rs_clock, rs_live[2], rs_live_bytes[2];
static u32 rs_peak[2], rs_peak_bytes[2], rs_resets[2], rs_env_drops, rs_ngates, rs_pa;
static u32 rs_blocks, rs_halves, rs_rev_b[2], rs_rev_h[2];
static u8 *rs_last_ptr;
static unsigned long long rs_bytes_sum, rs_halves_sum;
static void rs_report(void)
{
  int f;
  fprintf(stderr, "RS blocks=%u halves=%u env_drops=%u bytes_per_half=%.2f\n",
          (unsigned)rs_blocks, (unsigned)rs_halves, (unsigned)rs_env_drops,
          rs_halves_sum ? (double)rs_bytes_sum / rs_halves_sum : 0.0);
  for (f = 0; f < 2; f++)
    fprintf(stderr, "RS flavour=%c revive_blocks=%u revive_halves=%u resets=%u peak_records=%u peak_record_bytes=%u\n",
            'A' + f, (unsigned)rs_rev_b[f], (unsigned)rs_rev_h[f], (unsigned)rs_resets[f],
            (unsigned)rs_peak[f], (unsigned)rs_peak_bytes[f]);
}
static void rs_reset(int f)
{
  rs_gen[f]++;
  rs_v[f] = 0;
  rs_live[f] = rs_live_bytes[f] = 0;
}
static void rs_xlat(u32 pc, u32 thumb, u32 hsh, u32 halves, int counted)
{
  u32 key = pc | (thumb & 1), bytes, set, w, f;
  if (!rs_tab[0]) {
    rs_tab[0] = calloc(RS_SETS * RS_WAYS, sizeof(rs_ent));
    rs_tab[1] = calloc(RS_SETS * RS_WAYS, sizeof(rs_ent));
    atexit(rs_report);
  }
  /* host bytes of this block */
  if (rs_last_ptr && ram_translation_ptr > rs_last_ptr &&
      ram_translation_ptr - rs_last_ptr < 16384) {
    rs_bytes_sum += ram_translation_ptr - rs_last_ptr;
    rs_halves_sum += halves;
  }
  rs_last_ptr = ram_translation_ptr;
  bytes = rs_halves_sum ? (u32)(halves * rs_bytes_sum / rs_halves_sum) : halves * 8;
  /* flavour B's environment */
  if (translation_gate_targets != rs_ngates || smc_partial_active != rs_pa) {
    rs_ngates = translation_gate_targets;
    rs_pa = smc_partial_active;
    rs_env_drops++;
    rs_reset(1);
  }
  if (counted) { rs_blocks++; rs_halves += halves; }
  set = ((key * 2654435761u) >> 18) & (RS_SETS - 1);
  rs_clock++;
  for (f = 0; f < 2; f++) {
    rs_ent *e = rs_tab[f] + set * RS_WAYS, *victim = e;
    int hit = 0;
    for (w = 0; w < RS_WAYS; w++) {
      if (e[w].gen == rs_gen[f] && e[w].key == key && e[w].hash == hsh) {
        e[w].lru = rs_clock; hit = 1; break;
      }
      if (e[w].gen != rs_gen[f] || e[w].lru < victim->lru)
        if (victim->gen == rs_gen[f]) victim = &e[w];
    }
    if (hit) {
      if (counted) { rs_rev_b[f]++; rs_rev_h[f] += halves; }
      continue;
    }
    {
      u32 rec = 56 + halves * 2 + 4;
      if (rs_v[f] + bytes + rec > RS_CAP) {
        rs_reset(f);
        if (counted) rs_resets[f]++;
        victim = e;
      }
      rs_v[f] += bytes + rec;
      if (victim->gen != rs_gen[f]) rs_live[f]++;
      rs_live_bytes[f] += rec;
      victim->key = key; victim->hash = hsh; victim->gen = rs_gen[f]; victim->lru = rs_clock;
      if (rs_live[f] > rs_peak[f]) rs_peak[f] = rs_live[f];
      if (rs_live_bytes[f] > rs_peak_bytes[f]) rs_peak_bytes[f] = rs_live_bytes[f];
    }
  }
}
#endif
static void census_xlat(u32 start_pc, u32 end_pc, u32 thumb)
{
  const u8 *mem; u32 off, lim, i, idx, hsh = 2166136261u, c = census_cause;
  unsigned long long key; u32 slot;
  if (!cen_last) census_init();
  if ((start_pc >> 24) == 3) {
    off = start_pc & 0x7FFF; lim = 0x8000; mem = iwram + 0x8000; idx = off >> 1;
  } else if ((start_pc >> 24) == 2) {
    off = start_pc & 0x3FFFF; lim = 0x40000; mem = ewram; idx = 0x4000 + (off >> 1);
  } else
    return;
  if (end_pc <= start_pc) return;
  if (off + (end_pc - start_pc) < lim) lim = off + (end_pc - start_pc);
  for (i = off; i < lim; i++) hsh = (hsh ^ mem[i]) * 16777619u;
  hsh ^= (lim - off) * 0x85EBCA6Bu;
  if (!hsh) hsh = 1;
#ifdef XLAT_REUSE_SIM
  rs_xlat(start_pc, thumb, hsh, (lim - off) >> 1,
          frame_counter - cen_frames0 >= 300);
#endif
  cen_x[c]++;
  cen_hw[c] += (lim - off) >> 1;
  idx += thumb ? (0x4000 + 0x20000) : 0;
  if (cen_last[idx] == hsh) cen_same[c]++;
  cen_last[idx] = hsh;
  key = ((unsigned long long)(start_pc | (thumb & 1)) << 32) | hsh;
  slot = (u32)((key * 0x9E3779B97F4A7C15ull) >> (64 - CEN_SEEN_BITS));
  for (i = 0; i < 64; i++, slot = (slot + 1) & ((1u << CEN_SEEN_BITS) - 1)) {
    if (cen_seen[slot] == key) { cen_any[c]++; return; }
    if (!cen_seen[slot]) { cen_seen[slot] = key; return; }
  }
  cen_seen_full++;
}
#endif
void ramtag_note_extent(u32 start_pc, u32 end_pc, u32 thumb)
{
  u16 *tagp;
  u32 off, base, size, len, canon_end;
#if defined(XLAT_CENSUS) && defined(DRPROF_TWIN)
  census_xlat(start_pc, end_pc, thumb);
#endif
#if defined(DMA_SMC_ADAPT) || defined(SMC_WORK_STATS)
  if (end_pc > start_pc && ((start_pc >> 24) == 2 || (start_pc >> 24) == 3)) {
    extern u32 smc_work_halves;
    smc_work_halves += (end_pc - start_pc) >> 1;
#if defined(DMA_ADAPT_V2) || defined(SMC_WORK_WINDOWS)
    { extern u32 smc_work_blocks; smc_work_blocks++; }
#endif
  }
#endif

#ifdef STALE_CHECK
  stale_note_extent(start_pc, end_pc, thumb);
#endif
#ifdef SMC_EVTRACE_XLAT
  /* DIAGNOSTIC (slow-titles): one line per RAM translation with a hash of
   * its guest source -- how often identical code is translated again. */
  if ((start_pc >> 24) == 3 && end_pc > start_pc && end_pc - start_pc <= 0x1000) {
    u32 a, h = 2166136261u;
    for (a = start_pc; a < end_pc; a += 2)
      h = (h ^ *(u16 *)(iwram + 0x8000 + (a & 0x7FFE))) * 16777619u;
    SMC_EVT("X %08x %08x %u %08x\n", (unsigned)start_pc, (unsigned)end_pc,
            (unsigned)thumb, (unsigned)h);
  }
#endif
  if (!smc_partial_active)
    return;

  if (start_pc >= 0x03000000 && start_pc < 0x04000000) {
    tagp = (u16 *)iwram;
    base = 0x03000000;
    size = 0x8000;
  } else if (start_pc >= 0x02000000 && start_pc < 0x03000000) {
    tagp = (u16 *)(ewram + 0x40000);
    base = 0x02000000;
    size = 0x40000;
  } else {
    return;
  }
  off = start_pc & (size - 1);
  len = end_pc - start_pc;          /* > 0 and <= MAX_BLOCK_SIZE * 4 */
#ifdef SMC_RETIRE_WINDOW
  if (len > smc_max_extent[base == 0x02000000])
    smc_max_extent[base == 0x02000000] = len;
#endif
  if (off + len > size) {
    smc_partial_wrap_live = 1;
    canon_end = base + size;
  } else {
    canon_end = base + off + len;
#if defined(SMC_RETIRE_WINDOW) && defined(SMC_RETIRE_LOCAL) && \
    !defined(SMC_LOCAL_COLD)
    smc_back_note(base == 0x02000000, off, off + len);
#endif
  }

  if (VALID_TAG(tagp[off >> 1])) {
    ramtag_type *te = get_ram_tag(tagp[off >> 1]);
    if (thumb)
      te->blk_end_thumb = canon_end;
    else
      te->blk_end_arm = canon_end;
#if defined(SMC_SAME_ADAPT)
    xs_extent_note(start_pc, end_pc, thumb);
#endif
  }
}

#if defined(SMC_RETIRE_WINDOW) && defined(SMC_LOCAL_COLD) && \
    !defined(SMC_RETIRE_WINDOW_CHECK) && !defined(SMC_RETIRE_LOCAL_CHECK)
/* flush_translation_cache_ram_range's window start, out of line (cpl2w). */
static __attribute__((noinline)) u32 smc_window_first(u32 rgn, u32 lo_off,
                                                      u32 hi_off)
{
  /* armed (the caller checks): the short window, then the chunk bound */
  u32 span = smc_max_extent[rgn] + 4, first, local_;
  if (span > MAX_BLOCK_SIZE * 4)
    span = MAX_BLOCK_SIZE * 4;
  first = (lo_off > span) ? (lo_off - span) >> 1 : 0;
  local_ = smc_back_first(rgn, lo_off, hi_off);
  return local_ > first ? local_ : first;
}
#endif
/* Retire every translation whose source overlaps [low, high). Cross-block
 * branches into RAM are indirect under this flag, so clearing an offset
 * cannot leave a generated jump pointing at the retired cache entry. */
static void flush_translation_cache_ram_range(u32 low, u32 high)
{
  u16 *tagp;
  u32 base, bytes, lo_off, hi_off, first, last, i, target_slot, domain_end;

#if defined(DISPATCH_CACHE) && !defined(DISPATCH_PRECISE)
  /* A retired block's stable thunk jumps straight back into
   * mips_indirect_branch_*: a surviving {pc -> thunk} entry would loop.
   * (DISPATCH_PRECISE: smc_stable_thunk_dispatch forgets that entry.) */
  dispatch_cache_clear();
#endif

  if (smc_partial_wrap_live ||
      high <= low || (low >> 24) != ((high - 1) >> 24)) {
    SMC_FB(4);
    flush_translation_cache_ram();
    return;
  }

  if ((low >> 24) == 3) {
    tagp = (u16 *)iwram;
    base = 0x03000000;
    bytes = 0x8000;
  } else if ((low >> 24) == 2) {
    tagp = (u16 *)(ewram + 0x40000);
    base = 0x02000000;
    bytes = 0x40000;
  } else {
    flush_translation_cache_ram();
    return;
  }

  lo_off = low - base;
  hi_off = high - base;
  if (lo_off >= bytes || hi_off > bytes) {
    SMC_FB(4);
    flush_translation_cache_ram();
    return;
  }

  /* The first STM in SoundMainRAM is followed by three more stores that
   * patch the same generated mixer block. A full flush naturally clears all
   * of their tags on the first trap. Preserve that atomic behavior by
   * extending retirement through the gated block beginning at the reported
   * final word; later stores in the batch then see no live tags. */
  target_slot = (hi_off - 4) >> 1;
  if (!VALID_TAG(tagp[target_slot])) {
    SMC_FB(5);
    flush_translation_cache_ram();
    return;
  }
  {
    ramtag_type *target = get_ram_tag(tagp[target_slot]);
    domain_end = target->blk_end_arm;
    if (target->blk_end_thumb > domain_end)
      domain_end = target->blk_end_thumb;
  }
  if (domain_end <= high || domain_end > base + bytes) {
    SMC_FB(6);
    flush_translation_cache_ram();
    return;
  }
  high = domain_end;
  hi_off = high - base;

  /* ARM is the wider ISA, so this is the hard maximum source span for
   * either mode. Exact extents filter unrelated starts inside the window. */
#if defined(SMC_RETIRE_WINDOW) && defined(SMC_LOCAL_COLD) && \
    !defined(SMC_RETIRE_WINDOW_CHECK) && !defined(SMC_RETIRE_LOCAL_CHECK)
  /* LAYOUT NEUTRALITY (cpl2w): the window start and its chunk narrowing in
   * one out-of-line call; the same arithmetic as the block below. */
  first = (lo_off > MAX_BLOCK_SIZE * 4)
        ? (lo_off - MAX_BLOCK_SIZE * 4) >> 1 : 0;   /* the window unarmed */
  if (smc_window_armed)
    first = smc_window_first(base == 0x02000000, lo_off, hi_off);
#elif defined(SMC_RETIRE_WINDOW)
  {
    u32 span = smc_max_extent[base == 0x02000000] + 4;
    u32 full = (lo_off > MAX_BLOCK_SIZE * 4)
             ? (lo_off - MAX_BLOCK_SIZE * 4) >> 1 : 0;
    if (!smc_window_armed || span > MAX_BLOCK_SIZE * 4)
      span = MAX_BLOCK_SIZE * 4;
    first = (lo_off > span) ? (lo_off - span) >> 1 : 0;
    SMC_LOCAL_NARROW(first, base == 0x02000000, lo_off, hi_off, tagp, base);
#ifdef SMC_RETIRE_WINDOW_CHECK
    /* The assumption, checked: nothing live between the full window's start
     * and the short one's may overlap the write.  A block there without an
     * extent, or with one reaching `low`, would be missed. */
    for (i = full; i < first; i++) {
      if (VALID_TAG(tagp[i])) {
        ramtag_type *te = get_ram_tag(tagp[i]);
        if ((SMC_RAM_LIVE_OFFSET(te, arm) &&
             (!te->blk_end_arm || te->blk_end_arm > low)) ||
            (SMC_RAM_LIVE_OFFSET(te, thumb) &&
             (!te->blk_end_thumb || te->blk_end_thumb > low))) {
          smc_window_violations++;
#ifdef DRPROF_TWIN
          fprintf(stderr, "SMC_RETIRE_WINDOW VIOLATION: live block at %08x "
                  "overlaps write %08x but lies outside the %u-byte window\n",
                  base + (i << 1), low, span);
          abort();
#endif
          first = full;        /* fall back to the proven walk */
          break;
        }
      }
    }
#else
    (void)full;
#endif
  }
#else
  first = (lo_off > MAX_BLOCK_SIZE * 4)
        ? (lo_off - MAX_BLOCK_SIZE * 4) >> 1 : 0;
#endif
  last = (hi_off + 1) >> 1;
  if (last > (bytes >> 1))
    last = bytes >> 1;

  /* Validate the complete retirement set before changing any entry. */
  for (i = first; i < last; i++) {
    if (VALID_TAG(tagp[i])) {
      ramtag_type *te = get_ram_tag(tagp[i]);
      u32 start = base + (i << 1);

      if ((SMC_RAM_LIVE_OFFSET(te, arm) && (!te->blk_end_arm ||
#ifndef SMC_PARTIAL_SAFE_NO_TRAMP
           (start < high && te->blk_end_arm > low &&
            te->code_bytes_arm < 8) ||
#endif
           0)) ||
          (SMC_RAM_LIVE_OFFSET(te, thumb) && (!te->blk_end_thumb ||
#ifndef SMC_PARTIAL_SAFE_NO_TRAMP
           (start < high && te->blk_end_thumb > low &&
            te->code_bytes_thumb < 8) ||
#endif
           0))) {
        SMC_FB(7);
        flush_translation_cache_ram();
        return;
      }
    }
  }

  for (i = first; i < last; i++) {
    if (VALID_TAG(tagp[i])) {
      ramtag_type *te = get_ram_tag(tagp[i]);
      u32 start = base + (i << 1);

      if (SMC_RAM_LIVE_OFFSET(te, arm) &&
          start < high && te->blk_end_arm > low) {
#ifdef SMC_PARTIAL_STABLE_THUNK
        SMC_EVT("  R arm %08x..%08x\n", (unsigned)start, (unsigned)te->blk_end_arm);
        smc_stable_thunk_dispatch(SMC_RAM_PUBLIC_ENTRY(te, arm), start, 0);
        te->code_bytes_arm = 0;
#else
#ifndef SMC_PARTIAL_SAFE_NO_TRAMP
        u8 *entry = &ram_translation_cache[te->offset_arm];
        u8 *translation_ptr = entry - 16;
        generate_load_imm(reg_a0, start);
        mips_emit_j(mips_absolute_offset(mips_indirect_branch_arm));
        mips_emit_nop();
        generate_branch_patch_unconditional(entry, entry - 16);
        address32(entry, 4) = 0;
#ifdef JIT_CODE_DISCIPLINE
        JIT_CODE_WROTE(entry - 16, entry + 8);
#else
        platform_cache_sync(entry - 16, entry + 8);
#endif
#endif
        te->offset_arm = 0;
#endif
        flush_ram_partial++;
      }
      if (SMC_RAM_LIVE_OFFSET(te, thumb) &&
          start < high && te->blk_end_thumb > low) {
#ifdef SMC_PARTIAL_STABLE_THUNK
        SMC_EVT("  R thumb %08x..%08x\n", (unsigned)start, (unsigned)te->blk_end_thumb);
        smc_stable_thunk_dispatch(SMC_RAM_PUBLIC_ENTRY(te, thumb), start, 1);
        te->code_bytes_thumb = 0;
#else
#ifndef SMC_PARTIAL_SAFE_NO_TRAMP
        u8 *entry = &ram_translation_cache[te->offset_thumb];
        u8 *translation_ptr = entry - 16;
        generate_load_imm(reg_a0, start);
        mips_emit_j(mips_absolute_offset(mips_indirect_branch_thumb));
        mips_emit_nop();
        generate_branch_patch_unconditional(entry, entry - 16);
        address32(entry, 4) = 0;
#ifdef JIT_CODE_DISCIPLINE
        JIT_CODE_WROTE(entry - 16, entry + 8);
#else
        platform_cache_sync(entry - 16, entry + 8);
#endif
#endif
        te->offset_thumb = 0;
#endif
        flush_ram_partial++;
      }
    }
  }

#ifdef JIT_CODE_DISCIPLINE
  /* Every retirement above patched existing code: publish it now, once for
   * the whole batch, before anything can run (the store stub's next stop
   * is the lookup, but the rule is not allowed to depend on that). */
  jit_code_publish();
#endif

  /* Suppress the other stores in this patch iteration, as a full flush does,
   * but keep the trapping STM's FINAL word tagged. Only that highest-address
   * store uses execute_store_u32 and performs an SMC check; retaining the low
   * word instead lets a later channel iteration escape when the generated
   * target has not run yet to reconstruct its tags. The other three STMs in
   * this iteration target different final words and remain atomic with it. */
  {
    u16 trigger_tag = tagp[target_slot];
    for (i = lo_off >> 1; i < (hi_off >> 1); i++)
      if (VALID_TAG(tagp[i]) &&
          !smc_remember_retired_tag(base + (i << 1), tagp[i])) {
        SMC_FB(8);
        flush_translation_cache_ram();
        return;
      }
    memset(&tagp[lo_off >> 1], 0, hi_off - lo_off);
    tagp[target_slot] = trigger_tag;
  }
}

#elif defined(SMC_PARTIAL)
/* Record a freshly scanned block's source extent against its tag.  Called
 * from smc_blk_note_block() the moment scan_block settles block_end_pc — and
 * crucially BEFORE the external-exit resolution that can recursively
 * translate other blocks, so no nested translation can clobber it.  The tag
 * itself is already allocated: block_lookup_translate() does that before it
 * ever calls translate_block. */
u32 smc_max_blk_len = 64;   /* longest RAM block seen, bytes (bounds the scan) */

/* UNCONDITIONAL-BRANCH BARRIERS — one bit per halfword.
 *
 * TempGBA's insight (cpu.c partial_clear_metadata): no translated block can
 * span an unconditional branch, because nothing falls through one.  So every
 * block that could possibly cover an address lies between the nearest
 * barriers around it — a bound that comes from code GEOMETRY and therefore
 * cannot be invalidated by our own bookkeeping, which is what defeated every
 * tag-adjacency scheme (v1-v6).
 *
 * gpSP already knows where these are: scan_block reports SMC_SCAN_UNCOND
 * when it stops on one.  It simply never recorded the position.
 * IWRAM 32 KiB -> 2 KiB of bits, EWRAM 256 KiB -> 16 KiB. */
static u8 uncond_iw[0x8000 >> 4];
static u8 uncond_ew[0x40000 >> 4];

static u8 *uncond_map(u32 gba_addr, u32 *hw_off, u32 *hw_lim)
{
  if (gba_addr >= 0x03000000) {
    *hw_off = (gba_addr & 0x7FFF) >> 1;  *hw_lim = 0x8000 >> 1;
    return uncond_iw;
  }
  *hw_off = (gba_addr & 0x3FFFF) >> 1;   *hw_lim = 0x40000 >> 1;
  return uncond_ew;
}

void ramtag_note_extent(u32 start_pc, u32 end_pc, u32 thumb)
{
  u16 *tagp;
  u32  off;

  if (end_pc > start_pc && (end_pc - start_pc) > smc_max_blk_len)
    smc_max_blk_len = end_pc - start_pc;

  if (start_pc >= 0x03000000 && start_pc < 0x04000000) {
    tagp = (u16 *)iwram;   off = (start_pc & 0x7FFF) >> 1;
  } else if (start_pc >= 0x02000000 && start_pc < 0x03000000) {
    tagp = (u16 *)(ewram + 0x40000); off = (start_pc & 0x3FFFF) >> 1;
  } else {
    return;                              /* ROM block: no RAM tag to annotate */
  }

  if (VALID_TAG(tagp[off])) {
    ramtag_type *te = get_ram_tag(tagp[off]);
    te->blk_start = start_pc;
    te->blk_end   = end_pc;
  }
  (void)thumb;
}

/* Record "no block spans this point" at a block end that stopped on an
 * unconditional branch.  Called only for SMC_SCAN_UNCOND. */
void ramtag_note_barrier(u32 end_pc)
{
  u32 h, lim;
  u8 *map;
  if (end_pc < 0x02000000 || end_pc >= 0x04000000)
    return;
  map = uncond_map(end_pc, &h, &lim);
  if (h < lim)
    map[h >> 3] |= (u8)(1u << (h & 7));
}

/* Partial RAM invalidation (experiment).
 *
 * Measured on Pokemon Unbound: one self-modifying ARM block in IWRAM
 * (030061e4-03006358) patches four words of its own body ~1450x/second, and
 * every one of those writes wipes the ENTIRE RAM translation cache.  The
 * telemetry priced the collateral at ~5 block re-translations per write
 * (xlat ~37000 for ~7040 writes), and dynarec compilation then eats 40-61%
 * of the frame.
 *
 * The tag array doubles as a block map: 0 = no code, CODE_TAG_BLOCK16
 * (0x0101) = an instruction inside a block, anything greater = a block START
 * whose value indexes the ramtag table.  So the block covering an address is
 * found by walking back to the nearest block-start tag and forward to the
 * end of the tagged run.  Clearing just those tags retires exactly one block;
 * everything else keeps its translation.
 *
 * NOTE ON SAFETY — this is why it is behind a flag and lives in the lab.
 * translate_block patches DIRECT branches between blocks
 * (generate_branch_patch_unconditional at the end of translate_block_arm).
 * If another block already holds a patched jump into the block we retire, it
 * will keep jumping to the stale translation.  The deterministic frame-dump
 * oracle in the harness is what decides whether that happens in practice for
 * this game: identical dump = safe here, different dump = this needs
 * back-reference bookkeeping before it can ever ship.
 *
 * ram_translation_ptr is deliberately NOT rewound — the retired code is
 * simply abandoned.  The cache-exhaustion path in translate_block already
 * calls the full flush when it fills, so the leak is self-limiting. */
static void flush_translation_cache_ram_block(u32 gba_addr)
{
  u16 *tagbase, *tagp;
  u32 off, lim;

  if (gba_addr >= 0x03000000) {          /* IWRAM: tags live 32 KiB below */
    tagbase = (u16 *)(iwram);
    off     = (gba_addr & 0x7FFF) >> 1;
    lim     = 0x8000 >> 1;
  } else {                               /* EWRAM: tags live at +0x40000 */
    tagbase = (u16 *)(ewram + 0x40000);
    off     = (gba_addr & 0x3FFFF) >> 1;
    lim     = 0x40000 >> 1;
  }

  tagp = tagbase;
  if (!tagp[off])                        /* not code after all — nothing to do */
    return;

  /* WHY A WINDOW AND NOT JUST "THE" BLOCK.
   *
   * v1 walked back to the nearest block start and retired that one block.
   * It ran Unbound at 59.9 fps with a byte-identical video frame — and
   * silently corrupted audio (hash e9c60c3e vs baseline 358b532f, proven
   * deterministic by a repeat run).  A trampoline at the retired entry point
   * changed nothing, which ruled out stale jumps.
   *
   * The real flaw: gpSP starts a block at every branch target, so SEVERAL
   * blocks can span one address.  A block starting further back can reach
   * across the written word, and its own start tag halts the backward walk,
   * so v1 never found it.  That block stays live holding stale code — which
   * is why the M4A sound driver kept playing outdated mixer code while
   * graphics were unaffected.  The full flush was correct precisely because
   * it killed every block regardless of overlap.
   *
   * A block is at most MAX_BLOCK_SIZE (1024) instructions = 4 KiB, so any
   * block covering this address must START within 4 KiB behind it.  Retire
   * every block starting in that window.  Over-invalidation is safe (it only
   * costs re-translation); under-invalidation is the bug above. */
  {
    /* v2 used a flat 4 KiB window (MAX_BLOCK_SIZE) and was CORRECT but slow:
     * 26.9 fps, worse than the 29.4 baseline, because it retired every block
     * in the window whether or not it reached the write.
     *
     * Tighter and still exact: scan_block tags EVERY instruction slot of a
     * block, so a block that spans this address has an unbroken run of tags
     * from its own start all the way through it.  Walking back only while the
     * tags stay non-zero therefore reaches every block that can possibly
     * cover the write, and stops dead at the first untagged byte — no block
     * spans a gap.  A fixed window can only be larger than this, never more
     * correct. */
    /* Window bounded by the LONGEST BLOCK SEEN SO FAR, not by tag adjacency.
     *
     * Each earlier attempt failed in its own way and this combines what they
     * proved.  Following the tag run (v3/v5) is fragile: retiring punches
     * holes in it (v4 -> wrong audio) and never clearing lets the run grow
     * until the scan covers all of IWRAM (v5 -> the emulator stalled before
     * its first heartbeat).  A flat 4 KiB window (v2) was correct but retired
     * everything in it and ran slower than the full flush.
     *
     * Any block spanning this address must START no earlier than
     * smc_max_blk_len bytes behind it — that is a hard bound, independent of
     * the tag map's shape, so fragmentation cannot defeat it and the scan can
     * never run away.  Measured blocks here are ~400 bytes, far below the
     * 4 KiB MAX_BLOCK_SIZE ceiling, so the window stays small.  Coverage is
     * then tested EXACTLY against each candidate's recorded extent.
     * Tags are never cleared: they mean "code lives here", which stays true,
     * and clearing them would blind the SMC check to future writes. */
    u32 bh, blim;
    u8 *bmap = uncond_map(gba_addr, &bh, &blim);
    u32 cap  = (smc_max_blk_len >> 1) + 2;      /* hard ceiling, in slots */
    u32 lo = off, hi = off, i, steps = 0;

    /* This write may have edited the very instruction a barrier stands on,
     * so drop barriers across the written word before trusting them. */
    if (bh < blim) {
      bmap[bh >> 3]       &= (u8)~(1u << (bh & 7));
      if (bh + 1 < blim)
        bmap[(bh+1) >> 3] &= (u8)~(1u << ((bh+1) & 7));
    }

    /* Walk back to the nearest barrier — provably past every block that can
     * reach this address.  The cap is a safety valve: if no barrier turns up
     * within the longest block ever seen, fall back to the full flush rather
     * than scan further.  Correctness never depends on the walk being short. */
    while (lo > 0 && steps < cap &&
           !(bmap[(lo - 1) >> 3] & (1u << ((lo - 1) & 7)))) {
      lo--; steps++;
    }
    if (steps >= cap) {
      flush_translation_cache_ram();
      return;
    }

    while (hi + 1 < lim && tagp[hi + 1] == CODE_TAG_BLOCK16)
      hi++;                              /* forward to the end of this block */

    for (i = lo; i <= hi; i++) {
      if (VALID_TAG(tagp[i])) {
        ramtag_type *te = get_ram_tag(tagp[i]);
        /* Retire only blocks that genuinely span the written address.  With
         * a recorded extent this is exact: no missed overlaps (v1's audio
         * corruption) and no needless retirements (v2 being slower than the
         * full flush).  Blocks with no extent recorded are retired anyway —
         * unknown means assume guilty. */
        if (!te->blk_end ||
            (gba_addr >= te->blk_start && gba_addr < te->blk_end)) {
          /* Retire by clearing the CACHE OFFSETS only — never the tags.
           *
           * v4 zeroed the retired block's tags and was still wrong (a third
           * audio hash, 50bdf44f).  Reason: the walk-back above relies on an
           * unbroken run of tags from a block's start through the written
           * address, and zeroing tags punches holes in runs that OTHER,
           * overlapping blocks still depend on — so the next write stops
           * early at the hole and misses blocks starting before it.  Our own
           * bookkeeping was destroying the map we navigate by.
           *
           * The tag only needs to mean "code lives at this halfword", which
           * stays true.  block_lookup_translate reuses the existing tag when
           * VALID_TAG holds and re-translates whenever the offset is 0, so
           * clearing the offset alone is a complete retirement — and the tag
           * map stays contiguous forever. */
          /* LEAVE A TRAMPOLINE AT THE OLD ENTRY POINT.
           *
           * Retiring by clearing offsets is enough only while nothing holds a
           * patched direct jump into this block.  Translation gates break that
           * assumption on purpose: they make the self-modified addresses into
           * block STARTS, and block starts are the only thing
           * generate_branch_patch_unconditional ever targets.  So the gates
           * manufacture inbound jumps pointing exactly at the words the sound
           * driver rewrites — and a retired block still gets entered through
           * them, running the stale translation.  Audible as music that plays
           * correctly, screeches, recovers, screeches: dispatcher entry is
           * fine, stale-jump entry is garbage.
           *
           * Overwriting the entry with a jump to the block-lookup dispatcher
           * makes every stale jump land somewhere that re-translates the
           * modified code.  (Tried once before against a DIFFERENT bug —
           * missed overlapping blocks — where it correctly changed nothing.) */
          /* SIZE GUARD.  The trampoline is 4 instructions = 16 bytes.  A block
           * whose translation is shorter than that would have the trampoline
           * spill into whatever was emitted next — and translation gates make
           * short blocks common by design, because they deliberately split at
           * the self-modified words.  Corrupting a neighbouring block is
           * exactly the kind of damage that shows up as intermittently
           * mangled audio, so refuse to write one that does not fit and take
           * the safe full flush instead. */
          if (te->blk_start && te->code_bytes >= 16) {
            unsigned k;
            for (k = 0; k < 2; k++) {
              u32 coff = k ? te->offset_thumb : te->offset_arm;
              if (coff) {
                u8 *entry = &ram_translation_cache[coff];
                u8 *translation_ptr = entry;
                mips_emit_lui(reg_a0, (te->blk_start >> 16) & 0xFFFF);
                mips_emit_ori(reg_a0, reg_a0, te->blk_start & 0xFFFF);
                mips_emit_j(((u32)(k ? &mips_indirect_branch_thumb
                                     : &mips_indirect_branch_arm)) >> 2);
                mips_emit_nop();
#ifdef JIT_CODE_DISCIPLINE
                JIT_CODE_WROTE(entry, translation_ptr);
#else
                platform_cache_sync(entry, translation_ptr);
#endif
              }
            }
          }
          te->offset_arm   = 0;
          te->offset_thumb = 0;
          flush_ram_partial++;
        }
      }
    }

    /* MAKE THE RE-TRANSLATION CHEAP, not just rare.
     *
     * Measured: correct invalidation alone gets 31.5 fps vs 29.4 baseline —
     * only 7%.  Retiring the right blocks was never the expensive part; the
     * cost is recompiling ONE 372-byte self-modifying routine ~1450 times a
     * second.  The 59.9 fps variants were fast only because they skipped
     * that necessary work.
     *
     * gpSP already supports translation gates (scan_block stops when
     * block_end_pc hits one) but nothing ever populates the list.  Adding a
     * gate at the written address splits future blocks there, so the word
     * being patched sits on a block boundary and only a SMALL block has to be
     * rebuilt each time instead of the whole routine.  This is what TempGBA's
     * partial_flush_ram_stub does after its partial clear.
     *
     * 8 slots, and Unbound's storm is 4 addresses, so they fit. */
    /* (gate insertion now lives in flush_translation_cache_ram_smc, behind
     * SMC_GATES, so gates and partial invalidation can be A/B'd separately) */
#ifdef JIT_CODE_DISCIPLINE
    jit_code_publish();   /* the trampolines patched existing code */
#endif
    return;
  }
#if 0  /* v1, kept for the record — see the comment above for why it is wrong */
  while (off > 0 && tagp[off] == CODE_TAG_BLOCK16)
    off--;

  if (!VALID_TAG(tagp[off])) {
    flush_translation_cache_ram();
    return;
  }

  {
    ramtag_type *trentry = get_ram_tag(tagp[off]);
    /* THE CORRECTNESS PIECE.  Clearing the tag alone is NOT enough: other
     * blocks hold patched DIRECT jumps into this block's entry point, and
     * they would keep running the stale translation.  Measured consequence:
     * Unbound's video stayed byte-identical while its audio hash changed
     * (e9c60c3e vs 358b532f) because the M4A sound driver in IWRAM is
     * exactly the self-modifying code being retired.
     *
     * So leave a TRAMPOLINE at the old entry point instead of abandoning it.
     * gpSP only ever patches branches to block STARTS, so every stale jump
     * lands here and gets re-dispatched through the normal lookup, which
     * re-translates the modified code.  4 instructions, written once per
     * retirement. */
    u32 blk_pc = ((gba_addr >= 0x03000000) ? 0x03000000 : 0x02000000)
               + (off << 1);
    unsigned k;
    for (k = 0; k < 2; k++)
    {
      u32 coff = k ? trentry->offset_thumb : trentry->offset_arm;
      if (coff)
      {
        u8 *entry = &ram_translation_cache[coff];
        u8 *translation_ptr = entry;
        mips_emit_lui(reg_a0, (blk_pc >> 16) & 0xFFFF);
        mips_emit_ori(reg_a0, reg_a0, blk_pc & 0xFFFF);
        mips_emit_j(((u32)(k ? &mips_indirect_branch_thumb
                             : &mips_indirect_branch_arm)) >> 2);
        mips_emit_nop();
        platform_cache_sync(entry, translation_ptr);
      }
    }
    trentry->offset_arm   = 0;
    trentry->offset_thumb = 0;
  }

  tagp[off++] = 0;                       /* retire the start ... */
  while (off < lim && tagp[off] == CODE_TAG_BLOCK16)
    tagp[off++] = 0;                     /* ... and its instruction run */

  flush_ram_partial++;
#endif  /* v1 */
}
#endif

#ifdef SMC_GATES
/* Split future blocks at the self-modified address, so each rebuild is a small
 * block instead of the whole routine.  Hoisted OUT of the partial-invalidation
 * path so the two can be tested independently: gates alone change block
 * boundaries (and therefore when timers/DMA get serviced), which is the
 * remaining suspect for Unbound's mangled audio now that stale jumps are
 * ruled out. */
/* WHICH ADDRESSES GET A GATE.
 *
 * A gate is not free.  It ends a block with an indirect branch, so every
 * pass through it is a dispatcher lookup; its only benefit is that a write
 * there retires a small block instead of a large one.  Measured per gate
 * (SMC hits vs lookups at that PC), two rules came out of it:
 *
 *  1. ONE GATE PER PATCHED REGION.  A self-modifying routine patches several
 *     words a few bytes apart -- the M4A sound mixer rewrites 4 words within
 *     56 bytes, in two copies of its loop 152 bytes apart.  The first gate
 *     already makes every later write in the region retire a short block;
 *     further gates there saved no translation at all and each added ~25k
 *     lookups a second, because they sit in the mixer's inner loop.  So a
 *     write within SMC_GATE_CLUSTER bytes after an existing gate counts as a
 *     hit on that gate.  128 keeps the mixer's two loop copies apart.
 *
 *  2. REPLACE GATES THAT HAVE GONE QUIET.  The 8 slots fill with whatever is
 *     written first.  Pokemon Heart & Soul spends all of them at boot on
 *     stack and copy writes that never recur, so its music engine never got
 *     one: 457 KB/s re-translated, and a PSP-1000 dropped to 37 fps whenever
 *     music played.  Once the table is full, a gate not hit for
 *     SMC_GATE_IDLE_FRAMES is replaced.  Gates from the game table
 *     (gba_over.h) carry no stamp and are never replaced.
 *
 * Hardware A/B, Heart & Soul on a PSP-1000 with music on: 37-60 fps -> a
 * locked 60; re-translation 457 -> 48 KB/s, full flushes 8.0 -> 1.7/s.
 * PPSSPP, Unbound: lookups -49%, translation unchanged, core time -10%.
 * Unbound never fills the table, so rule 2 never fires there. */
/* Overridable from the root make as CLUSTER_BYTES=N.  Sized to the GAME's
 * layout, not picked for roundness: H&S's M4A mixer patches two loop copies
 * 152 bytes apart, so a window under that gates each copy separately while
 * anything over it merges them and leaves the second ungated.  64 was too
 * small (slots wasted inside one region: 105k dispatcher lookups vs 59k). */
#ifndef SMC_GATE_CLUSTER
#define SMC_GATE_CLUSTER      128
#endif
#define SMC_GATE_IDLE_FRAMES  60

/* frame_counter + 1 at the gate's last hit; 0 = loaded from the game table */
static u32 smc_gate_last_hit[MAX_TRANSLATION_GATES];
#if defined(SMC_GATES) && !defined(SMC_GATES_SIMPLE) && !defined(SMC_GATES_CLUSTER)
static u32 smc_gate_prev_now;   /* detects savestate clock rewind */
#endif

#ifdef SMC_GATES_RANKED
static void smc_cand_forget(void);   /* defined with the candidate table below */
#endif

/* Clear ROM-specific gate learning after a new cartridge is loaded.
 *
 * Ranked candidates contain addresses, write counts, and the last value seen
 * at each address.  Reusing those observations across ROMs could let one
 * game's evidence promote a gate in another game.  This function is called
 * from gba_memory.c after the new cartridge's fixed gate table is populated.
 * It is intentionally NOT called by dynarec reset or cache-flush paths:
 * clearing candidates during a same-ROM reset or savestate load discarded
 * useful learning and caused a measured performance loss.  See the withdrawn
 * reset-boundary experiment below (around flush_dynarec_caches). */
void smc_gates_reset(void)
{
  memset(smc_gate_last_hit, 0, sizeof(smc_gate_last_hit));
#if defined(SMC_GATES) && !defined(SMC_GATES_SIMPLE) && !defined(SMC_GATES_CLUSTER)
  /* Savestate-rewind re-anchor: a stale stamp against a fresh frame_counter is
   * the underflow that made eviction fire on every write. */
  smc_gate_prev_now = 0;
#endif
#ifdef SMC_GATES_RANKED
  smc_cand_forget();
#endif
  SMC_GATE_MAP_REBUILD();
#ifdef ENTRY_GATES
  /* Same rule as the SMC gates (gbalink_core.c lk_cold_start): entry gates
   * move block boundaries, so a new cartridge and a link session's cold
   * start both begin with none. */
  entry_gates_reset();
#endif
}

/* SMC_GATES_CODED: only gate an address that is ALREADY the start of a
 * translated block.
 *
 * Gates are placed at smc_last_write_addr -- a DATA write address -- but a
 * gate is then used as a CODE entry: it forces blocks to end there, so the
 * next block STARTS there.  Nothing guarantees a write address is an
 * instruction boundary, and mid-copy it is not even finished code.  Scanning
 * from such an address yields nonsense: one PPSSPP battle logged 223 bad
 * branch targets (into OAM/VRAM/SRAM) and every one came from the block at
 * the gated address 0x03001404.  Most are never executed; when one is, the
 * dispatcher jumps to it and the console dies.
 *
 * A VALID_TAG at the address means a block was translated starting exactly
 * there, which is proof it is a real instruction boundary.  Gating it still
 * pays: it stops OTHER blocks from spanning the address, which is the
 * overlap the one-slot-per-halfword tag map cannot represent. */
/* SMC_GATES_SNAP: gate the START OF THE BLOCK containing the write, not the
 * write address itself.
 *
 * Gates are harvested from smc_last_write_addr -- a DATA address -- but a
 * gate is consumed as a CODE entry: it forces blocks to end there, so the
 * next block STARTS there.  Nothing makes a data-write address an
 * instruction boundary, and mid-copy it is not finished code either.  That
 * is the whole crash: one battle logged 223 garbage branch targets, every
 * one from the block at the gated address 0x03001404.
 *
 * A VALID_TAG exists at an address ONLY because allocate_tag_* ran with that
 * exact pc, i.e. a block was translated starting there -- so it is a proven
 * instruction boundary, in the right mode.  Walk back over the interior
 * (CODE_TAG_BLOCK16) run to find it.
 *
 * This is what SMC_GATES_CODED should have been.  That one demanded the
 * write address ITSELF be a block start, which rejected 0x0300168c -- the
 * address behind 97.8% of all flushes, interior to its block -- and cost
 * 59.9 -> 46-58 fps.  Snapping keeps that gate, just anchored to the head of
 * its block, so scans still stop before the self-modifying region.
 *
 * Bonus: every write inside one block snaps to the same gate, so the table
 * stops churning through near-duplicates.  That is SMC_GATE_CLUSTER grouping
 * done by real block boundaries instead of a fixed byte window. */
/* SMC_GATES_SNAPF: snap FORWARD to the first proven instruction boundary at
 * or after the write address.
 *
 * A gate address has to satisfy TWO things, and SMC_GATES_SNAP only got one:
 *   (1) validity  -- it is used as a code entry, so it must be a real
 *                    instruction boundary, else the block scanned from it is
 *                    garbage (223 bad jumps, all from gated 0x03001404);
 *   (2) isolation -- it must be at/after the write, so the previous block
 *                    ENDS before the self-modifying region instead of
 *                    spanning it.  That is the entire speed mechanism.
 * SNAP walked BACKWARDS to the block head: valid, but the block starting at
 * that head still covers the SMC region, so isolation was lost -- measured
 * 1,428,000 flushes and 31-48 fps, against 116,000 and a locked 59.9 for
 * add-only.  Snapping forward keeps the anchor AND the isolation.
 *
 * Stride is 4, unconditionally: start+4k is a valid boundary for ARM (whose
 * instructions are 4 bytes) and for Thumb (2 bytes, so every even offset is
 * one), which sidesteps needing the block mode.  A Thumb block that starts on
 * a BL second half is safe -- translate_block_thumb inits opcode = 0, so
 * last_opcode is 0 on the first instruction and thumb_branch_target takes the
 * no_direct_branch path instead of computing a target from stale bits. */
#ifdef SMC_GATES_SNAPF
static u32 smc_gate_snapf(u32 gba_addr)
{
  u8 *base; u32 off, region, mask, k, delta;
  if (gba_addr >= 0x3000000) {
    base = iwram;           mask = 0x7FFF;  region = 0x3000000;
  } else {
    base = ewram + 0x40000; mask = 0x3FFFF; region = 0x2000000;
  }
  off = (gba_addr & mask) & ~1u;
  for (k = 0; k < 512; k++) {              /* find the enclosing block head */
    u16 t = *(u16 *)(base + off);
    if (VALID_TAG(t)) break;                /* proven instruction boundary */
    if (t != CODE_TAG_BLOCK16) return 0;    /* data -- nothing to gate */
    if (off < 2) return 0;
    off -= 2;
  }
  if (k == 512) return 0;                   /* gave up; do not guess */
  delta = ((gba_addr & mask) & ~1u) - off;  /* write, relative to the head */
  return region + off + ((delta + 3u) & ~3u);
}
#define SMC_GATE_PICK(a) smc_gate_snapf(a)
#elif defined(SMC_GATES_SNAP)

#define SMC_GATE_SNAP_MAX 512        /* halfword slots (1 KB) before giving up */
static u32 smc_gate_snap(u32 gba_addr)
{
  u8 *base; u32 off, region, k;
  if (gba_addr >= 0x3000000) {
    base = iwram;            off = gba_addr & 0x7FFF;  region = 0x3000000;
  } else {
    base = ewram + 0x40000;  off = gba_addr & 0x3FFFF; region = 0x2000000;
  }
  off &= ~1u;                          /* tags are one u16 per halfword */
  for (k = 0; k < SMC_GATE_SNAP_MAX; k++) {
    u16 t = *(u16 *)(base + off);
    if (VALID_TAG(t))            return region + off;  /* proven block start */
    if (t != CODE_TAG_BLOCK16)   return 0;   /* data -- nothing to gate */
    if (off < 2)                 return 0;
    off -= 2;
  }
  return 0;                              /* run too long; do not guess */
}
#define SMC_GATE_PICK(a) smc_gate_snap(a)
#else
#define SMC_GATE_PICK(a) ((a) & ~3u)
#endif

#ifdef SMC_GATES_CODED
static int smc_gate_addr_is_block_start(u32 gba_addr)
{
  const u16 *tagp = (gba_addr >= 0x3000000)
    ? (const u16 *)(iwram + (gba_addr & 0x7FFF))
    : (const u16 *)(ewram + (gba_addr & 0x3FFFF) + 0x40000);
  u16 t = *tagp;
  return VALID_TAG(t);
}
#define SMC_GATE_REJECT(gpc) (!smc_gate_addr_is_block_start(gpc))
#else
#define SMC_GATE_REJECT(gpc) (0)
#endif

/* SMC_GATES_RANKED: promote an address to a gate only once it has proven
 * itself HOT.  Both shipped rules pick gates blind -- add-only takes the
 * first eight addresses it ever sees, eviction cycles round-robin -- and
 * neither asks how much a gate would actually buy.
 *
 * Measured on the H&S rival battle, over one 8000-flush window:
 *
 *   0300168c   7723 writes   real code, value CHANGES (62953 vs 120 same)
 *              a block starts here and runs 030016 8c..030017c4
 *              -> the gate that matters; this is 97.8% of all flushes
 *
 *   03001404     67 writes   DATA inside a code block, value IDEMPOTENT
 *              (66 same, 1 changed).  No block starts here; TWO overlapping
 *              blocks span it (030013e8..03001414 and 030013f8..03001414).
 *              Gating it forces a block to start ON THE DATA WORD, which
 *              then scans garbage -- 223 bad branch targets in one battle,
 *              and the crash when one of them is executed.
 *
 * The two differ by ~115x in hit count, so ranking separates them cleanly:
 * gate the hot one, never spend a slot on the cold one.  Add-only semantics
 * are kept -- once promoted a gate is never moved -- so the table cannot
 * churn onto a bad address later, which is what made eviction unsafe. */
#ifdef SMC_GATES_RANKED
#ifndef SMC_GATE_RATIO
#define SMC_GATE_RATIO 4            /* within 1/4 of the hottest */
#endif
#define SMC_GATE_MIN_SAMPLE 64     /* writes before judging an address */
/* WITHDRAWN: A STALENESS RULE ON THE CANDIDATE ROWS.
 *
 * A gap over ~60 frames restarted a row's statistics, the reasoning being that
 * an address hot in one role (a boot copy destination, the previous
 * soundtrack's mixer body) should not promote on one write when the game later
 * uses it in another.
 *
 * REMOVED after a hardware session on 2026-09-18 showed degraded performance.
 * The rule and the thing it gates are the same order of magnitude: earning a
 * gate needs SMC_GATE_MIN_SAMPLE writes AT ONE ADDRESS, and if that takes
 * longer than the window the row restarts forever and the gate is NEVER
 * earned -- which is the documented 457 KB/s re-translation, 37 fps regime.
 * I never measured writes-per-frame at a single address before choosing 60,
 * so the window may well have been inside the accumulation time.
 *
 * It was also the only change on this branch with no demonstrated defect
 * behind it -- unlike the cross-ROM leak, the role-change case was a
 * hypothesis.  And it is defence in depth rather than the guard: with the
 * containment fix, a wrongly promoted gate is no longer fatal, it is a
 * dispatcher lookup.  That is what makes this safe to drop.
 *
 * If it is ever wanted back, the window has to be derived from measured
 * writes-per-frame at the gate address, with a wide margin -- not chosen.
 *
 * An absolute floor on `chg` was tried first and was worse than useless: at
 * SMC_GATE_MIN_SAMPLE 64 a floor of 32 is exactly what `chg * 2 >= hits`
 * already requires. */
#define SMC_GATE_CAND 32            /* candidates tracked before promotion */
static u32 smc_cand_addr[SMC_GATE_CAND];
static u32 smc_cand_hits[SMC_GATE_CAND];
static u32 smc_cand_used;

/* Returns 1 when this address has earned a gate. */
static u32 smc_cand_val[SMC_GATE_CAND];   /* last value seen at the address */
static u32 smc_cand_chg[SMC_GATE_CAND];   /* times it actually CHANGED */
#ifdef SMC_GATE_SELFPATCH
static u32 smc_cand_sp[SMC_GATE_CAND];    /* times it was a forward self-patch */
static u32 smc_gate_sp_now;               /* this trap is one (smc_add_gate) */
#endif

/* Gate only addresses whose writes genuinely MODIFY memory.
 *
 * Frequency was the wrong discriminator and failed twice: an absolute
 * threshold is outgrown by any cold address (crash b2c8), and a ratio to the
 * hottest candidate silently degenerates into one, because a promoted gate
 * stops counting and the max freezes (crash b3c8).  Counting only ever
 * delays the bad promotion.
 *
 * The real property is structural and measured:
 *     0300168c   120 same / 62953 changed  -> code being patched. SAFE, and
 *                a block legitimately starts here once gated.
 *     03001404    66 same /     1 changed  -> a constant being re-stored.
 *                DATA inside a code block, spanned by two overlapping
 *                blocks, no block starts here.  Gating it puts a block on a
 *                data word, which scans garbage and crashes.
 *
 * An idempotent store did not modify anything, so the address is not
 * self-modifying code in any useful sense -- it is data, and data must never
 * become a code entry point.  1 vs 62953 is a structural gap, not a tuned
 * constant.  Still add-only: promotion is permanent. */
static u32 *smc_gba_ptr(u32 addr)
{
  return (addr >= 0x3000000)
    ? (u32 *)(iwram + 0x8000 + (addr & 0x7FFF))
    : (u32 *)(ewram + (addr & 0x3FFFF));
}

/* A savestate restores guest RAM but intentionally preserves the ranked
 * candidate hit/change counts: discarding them costs thousands of frames of
 * mixer learning.  Rebase only the remembered words so the first write after
 * load is compared with the restored memory, not the pre-load timeline. */
void smc_gates_refresh_values(void)
{
  u32 k;
  for (k = 0; k < smc_cand_used; k++)
    smc_cand_val[k] = *smc_gba_ptr(smc_cand_addr[k]);
}

#ifdef SMC_GATE_SELFPATCH
#define SMC_CAND_SP(k) smc_cand_sp[k]
#else
#define SMC_CAND_SP(k) 0u
#endif

static int smc_gate_earned(u32 gpc)
{
  u32 k, coldest = 0, cur = *smc_gba_ptr(gpc);
  for (k = 0; k < smc_cand_used; k++)
    if (smc_cand_addr[k] == gpc) {
      smc_cand_hits[k]++;
      if (cur != smc_cand_val[k]) smc_cand_chg[k]++;
      smc_cand_val[k] = cur;
#ifdef SMC_GATE_SELFPATCH
      smc_cand_sp[k] += smc_gate_sp_now;
#endif
      SMC_EVT("C %08x k=%u hits=%u chg=%u sp=%u used=%u\n", (unsigned)gpc,
              (unsigned)k, (unsigned)smc_cand_hits[k],
              (unsigned)smc_cand_chg[k], (unsigned)SMC_CAND_SP(k),
              (unsigned)smc_cand_used);
      /* enough evidence, and the writes really do rewrite the bytes -- or
       * (SMC_GATE_SELFPATCH) they are mostly code patching the code just
       * ahead of itself, see smc_gate_selfpatch */
#ifdef SMC_GATE_SELFPATCH
      return smc_cand_hits[k] >= SMC_GATE_MIN_SAMPLE &&
             (smc_cand_chg[k] * 2 >= smc_cand_hits[k] ||
              smc_cand_sp[k] * 2 >= smc_cand_hits[k]);
#else
      return smc_cand_hits[k] >= SMC_GATE_MIN_SAMPLE &&
             smc_cand_chg[k] * 2 >= smc_cand_hits[k];
#endif
    }
  if (smc_cand_used >= SMC_GATE_CAND) {
    for (k = 1; k < SMC_GATE_CAND; k++)
      if (smc_cand_hits[k] < smc_cand_hits[coldest]) coldest = k;
  } else coldest = smc_cand_used++;
  SMC_EVT("C %08x new slot=%u evicts=%08x/%u used=%u\n", (unsigned)gpc,
          (unsigned)coldest, (unsigned)smc_cand_addr[coldest],
          (unsigned)smc_cand_hits[coldest], (unsigned)smc_cand_used);
  smc_cand_addr[coldest] = gpc;
  smc_cand_hits[coldest] = 1;
  smc_cand_chg[coldest]  = 0;
#ifdef SMC_GATE_SELFPATCH
  smc_cand_sp[coldest]   = smc_gate_sp_now;
#endif
  smc_cand_val[coldest]  = cur;
  return 0;
}
static void smc_cand_forget(void)
{
  memset(smc_cand_addr, 0, sizeof(smc_cand_addr));
  memset(smc_cand_hits, 0, sizeof(smc_cand_hits));
  memset(smc_cand_val,  0, sizeof(smc_cand_val));
  memset(smc_cand_chg,  0, sizeof(smc_cand_chg));
#ifdef SMC_GATE_SELFPATCH
  memset(smc_cand_sp,   0, sizeof(smc_cand_sp));
#endif
  smc_cand_used = 0;
}

#define SMC_GATE_EARNED(gpc) smc_gate_earned(gpc)
#else
#define SMC_GATE_EARNED(gpc) (1)
void smc_gates_refresh_values(void) {}
#endif

#ifdef SMC_GATES_CLUSTER
/* Add-only, PLUS 2.0.2's cluster grouping -- and deliberately WITHOUT its idle
 * eviction, which is the part that crashed Heart & Soul.
 *
 * 2.0.2's rule did two separable things.  Clustering declines to spend a slot
 * on a write within SMC_GATE_CLUSTER bytes of an existing gate, because extra
 * gates inside one patched region save no translation and cost a dispatcher
 * lookup on every pass (measured: lookups 196k -> 59k).  Eviction reassigns a
 * quiet gate's ADDRESS, which is what let already-translated blocks disagree
 * with the table.  Only the second one moves a gate, so clustering can be kept
 * and eviction dropped.  Once a gate is placed here it is never moved.  */
static void smc_add_gate(u32 gba_addr)
{
  u32 gpc = SMC_GATE_PICK(gba_addr), g;
#if defined(SMC_GATES_SNAP) || defined(SMC_GATES_SNAPF)
  if (!gpc) return;   /* no proven code entry for this write */
#endif
  if (SMC_GATE_REJECT(gpc)) return;   /* not a proven code entry */

  for (g = 0; g < translation_gate_targets; g++)
    if (translation_gate_target_pc[g] == gpc)
      return;                       /* already gated */

  for (g = 0; g < translation_gate_targets; g++)
  {
    u32 gp = translation_gate_target_pc[g];
    if (gpc > gp && gpc - gp <= SMC_GATE_CLUSTER)
      return;                       /* same patched region -- covered already */
  }

  if (!SMC_GATE_EARNED(gpc)) return;   /* not hot enough yet */
  if (translation_gate_targets < MAX_TRANSLATION_GATES) {
    translation_gate_target_pc[translation_gate_targets++] = gpc;
    SMC_GATE_MAP_ADD(gpc);
  }
  /* Table full: leave every existing gate exactly where it is. */
}
#elif defined(SMC_GATES_SIMPLE)
/* The 2.0.1 gate rule: ADD ONLY.  An address either already has a gate or
 * takes a free slot; once written, a gate's address is NEVER changed.
 *
 * 2.0.2 replaced this with cluster-128 + idle eviction, which measured better
 * in PPSSPP.  But eviction means the gate table MUTATES while blocks compiled
 * against the old layout are still live, and gpSP's tag map holds one slot per
 * halfword — it cannot represent the overlapping blocks that creates.  On
 * 2026-09-12 a stock build (no gates at all) survived a full rival battle and
 * four Growls at the highest flush rate we have ever measured (~1900/s), while
 * every gated build died.  That puts the gate rule itself under suspicion, and
 * this is the variable to eliminate first: Heart & Soul has never run on the
 * simple rule, because it did not boot at all before 2.0.2.
 *
 * There is also a concrete defect in the evicting version: `now` derives from
 * frame_counter, which savestates restore, while smc_gate_last_hit[] does not.
 * Loading a state with a lower frame count underflows the idle comparison to
 * ~4.29e9, so every gate reads as infinitely idle and churns constantly. */
#ifdef SMC_GATE_SELFPATCH
#if !defined(SMC_GATES_RANKED) || defined(SMC_GATES_SNAP) || \
    defined(SMC_GATES_SNAPF) || defined(SMC_GATES_CODED)
#error SMC_GATE_SELFPATCH is written for SMC_GATES_SIMPLE + SMC_GATES_RANKED
#endif
/* SMC_GATE_SELFPATCH (claude/idle-general, 2026-10-27): code that patches
 * the code just AHEAD of itself earns a gate at the start of what it wrote,
 * whether or not the bytes change.
 *
 * Measured on the twin, Mario Golf (fixture mgolf, frames 760-820): a setup
 * routine at 0x03000cdc..0x03000cf8 runs four two-word STMIAs (r1 = the
 * target, no writeback) into the unrolled loop it falls into at 0x03000d00.
 * The stored words are the ones already there (the retranslated source hash
 * is identical after every store), so the ranked rule's change test sees
 * chg = 0 in 505 of 505 samples and never promotes -- the rule that keeps a
 * re-stored DATA word (H&S 0x03001404) from becoming a code entry.  But
 * every store still traps (the writer is not a range-safe shape) and takes
 * a full RAM flush, and the writer's own retranslation scans straight
 * through the next target, so the next STM traps again: 4 full flushes a
 * pass, 456 per 60 frames.  The game table seeds a gate at 0x03000d00, the
 * first written word: the writer's block then ends there, one flush a pass,
 * and xlat is ~1/3 lower (twin: seeds worth ~18 % of Golf's core work).
 *
 * The evidence used instead of "the bytes changed": the trapping store and
 * its whole written range lie in one contiguous run of translated code,
 * the range AHEAD of the store.  That run is what the writer's next scan
 * re-covers, so isolating it is what pays; and the word is inside straight-
 * line code the writer itself executes through (a data word re-stored by
 * unrelated code fails "ahead of the writer, contiguous with it").  The
 * gate goes on the lowest word the store wrote -- for an STM, the first
 * register's word, which the trap (raised on the LAST word only) does not
 * report -- so no word of the write stays inside the writer's block.
 * Promotion still needs SMC_GATE_MIN_SAMPLE traps at that address, a
 * majority of them self-patches; add-only and the table size are unchanged.
 *
 * Returns the gate address for a self-patching trap, else 0. */
#ifndef SMC_SELFPATCH_SPAN
#define SMC_SELFPATCH_SPAN (MAX_BLOCK_SIZE * 4)   /* one ARM block, at most */
#endif
static u32 smc_gate_selfpatch(u32 addr)
{
  u32 thumb = (reg[REG_CPSR] >> 5) & 1;
  u32 pc = GBA_PC(reg[REG_PC]) - (thumb ? 2 : 4);
  u32 n = 1, op, lo, hi, a, region, mask;
  const u8 *mem;
  const u16 *tags;

  if ((pc >> 24) != (addr >> 24))
    return 0;
  if ((addr >> 24) == 0x03) {
    mem = iwram + 0x8000; tags = (const u16 *)iwram;  mask = 0x7FFF;
  } else if ((addr >> 24) == 0x02) {
    mem = ewram; tags = (const u16 *)(ewram + 0x40000); mask = 0x3FFFF;
  } else
    return 0;
  region = addr & ~mask;
  if (thumb) {
    op = *(const u16 *)(mem + (pc & mask & ~1u));
    if ((op & 0xF800) == 0xC000)                    /* STMIA rb!, {list} */
      n = __builtin_popcount(op & 0xFF);
    else if ((op & 0xFE00) == 0xB400)               /* PUSH {list, lr} */
      n = __builtin_popcount(op & 0xFF) + ((op >> 8) & 1);
  } else {
    op = *(const u32 *)(mem + (pc & mask & ~3u));
    if ((op & 0x0E100000) == 0x08000000)            /* STM, any mode */
      n = __builtin_popcount(op & 0xFFFF);
  }
  if (!n)
    return 0;
  hi = (addr & ~3u) + 4;                            /* the trap: last word */
  lo = hi - 4 * n;
  if ((lo & ~mask) != region || lo <= pc || hi - pc > SMC_SELFPATCH_SPAN)
    return 0;
  for (a = pc & ~1u; a < hi; a += 2)                /* one contiguous run */
    if (!tags[(a & mask) >> 1])
      return 0;
  return lo;
}
#endif

static void smc_add_gate(u32 gba_addr)
{
  u32 gpc = SMC_GATE_PICK(gba_addr), g;
#if defined(SMC_GATES_SNAP) || defined(SMC_GATES_SNAPF)
  if (!gpc) return;   /* no proven code entry for this write */
#endif
#ifdef SMC_GATE_SELFPATCH
  {
    u32 sp = smc_gate_selfpatch(gba_addr);
    smc_gate_sp_now = sp != 0;
    if (sp) gpc = sp;
  }
#endif
  if (SMC_GATE_REJECT(gpc)) return;   /* not a proven code entry */

  for (g = 0; g < translation_gate_targets; g++)
    if (translation_gate_target_pc[g] == gpc)
      return;                       /* already gated — nothing to do */

  if (!SMC_GATE_EARNED(gpc)) {
    SMC_EVT("G %08x no\n", (unsigned)gpc);
    return;   /* not hot enough yet */
  }
  SMC_EVT("G %08x ADD n=%u\n", (unsigned)gpc, (unsigned)translation_gate_targets);
  if (translation_gate_targets < MAX_TRANSLATION_GATES) {
    translation_gate_target_pc[translation_gate_targets++] = gpc;
    SMC_GATE_MAP_ADD(gpc);
  }
  /* Table full: leave every existing gate exactly where it is. */
}
#else
static void smc_add_gate(u32 gba_addr)
{
  u32 gpc = SMC_GATE_PICK(gba_addr), now = frame_counter + 1, g, victim;
#if defined(SMC_GATES_SNAP) || defined(SMC_GATES_SNAPF)
  if (!gpc) return;   /* no proven code entry for this write */
#endif

  /* frame_counter is RESTORED by savestates (main.c "frame-count") but
   * smc_gate_last_hit[] is not, so loading a state made earlier drags `now`
   * backwards while the hit stamps stay large.  The idle test below is an
   * unsigned subtraction, so it then underflows to ~4.29e9 -- always >=
   * SMC_GATE_IDLE_FRAMES -- and eviction fires on EVERY smc write instead of
   * on genuinely idle gates.  At ~11k smc writes a battle that churns the
   * table thousands of times, and every fresh address is another chance to
   * gate something that is not an instruction boundary.  Measured: 223 bad
   * jumps and a crash in battle 1, every run.  Re-anchor on a backwards
   * jump so eviction stays what it was meant to be. */
  if (now < smc_gate_prev_now)
    for (g = 0; g < MAX_TRANSLATION_GATES; g++)
      if (smc_gate_last_hit[g])
        smc_gate_last_hit[g] = now;
  smc_gate_prev_now = now;

  if (SMC_GATE_REJECT(gpc)) return;   /* not a proven code entry */

  for (g = 0; g < translation_gate_targets; g++)
    if (translation_gate_target_pc[g] == gpc)
    {
      if (smc_gate_last_hit[g])
        smc_gate_last_hit[g] = now;
      return;
    }

  for (g = 0; g < translation_gate_targets; g++)
  {
    u32 gp = translation_gate_target_pc[g];
    if (gpc > gp && gpc - gp <= SMC_GATE_CLUSTER)
    {
      if (smc_gate_last_hit[g])
        smc_gate_last_hit[g] = now;
      return;
    }
  }

  if (!SMC_GATE_EARNED(gpc)) return;   /* not hot enough yet */
  if (translation_gate_targets < MAX_TRANSLATION_GATES)
  {
    smc_gate_last_hit[translation_gate_targets] = now;
    translation_gate_target_pc[translation_gate_targets++] = gpc;
    SMC_GATE_MAP_ADD(gpc);
    return;
  }

  victim = MAX_TRANSLATION_GATES;
  for (g = 0; g < translation_gate_targets; g++)
    if (smc_gate_last_hit[g] &&
        (victim == MAX_TRANSLATION_GATES ||
         smc_gate_last_hit[g] < smc_gate_last_hit[victim]))
      victim = g;
  if (victim < MAX_TRANSLATION_GATES &&
      now - smc_gate_last_hit[victim] >= SMC_GATE_IDLE_FRAMES)
  {
    translation_gate_target_pc[victim] = gpc;
    smc_gate_last_hit[victim] = now;
    SMC_GATE_MAP_REBUILD();
  }
}
#endif  /* SMC_GATES_SIMPLE */
#else
/* No gate machinery (every non-PSP build): savestate.c still calls this.
 * (Ported from claude/gba-link-bench: the host core did not link.) */
void smc_gates_refresh_values(void) {}
#endif

#ifdef SMC_PARTIAL_SAFE
/* Describe the complete write that led to the SMC trap. ARM STM emits
 * unchecked stores for every word except the highest address.
 * A non-writeback STM is still safe to retire selectively once we decode the
 * register count and cover that full range. Advancing copies retain the full
 * flush: an earlier iteration can have written code without its final word
 * landing on a tag, and the later full flush is what makes that sequence safe.
 * Thumb block stores always write back, so they follow the same fallback. */
/* WITHDRAWN: HARDENING OF THIS FINGERPRINT.
 *
 * The predicate below is narrow but not sound: an ARM STMIA lr,{r0,r1} whose
 * highest written word sits 0x3c bytes past the writer is a 32-bit opcode match
 * plus an offset, and nothing in it is specific to a sound engine.  I added
 * three requirements -- writer and target both in IWRAM, one latched writer pc,
 * and N repetitions before activating -- on the reasoning that all three are
 * structurally true of M4A's SoundMainRAM.
 *
 * REMOVED after a hardware session on 2026-09-18 showed degraded performance.
 * This function is the gate on selective invalidation, so any extra condition
 * that turns out not to hold for the ROM in hand costs the FULL FLUSH on every
 * mixer write -- which is a large, load-dependent slowdown and exactly what was
 * reported.  I had not verified on hardware that Unbound's writer satisfies the
 * IWRAM assumption, and a regression hunt is the wrong moment to be carrying an
 * unverified extra condition on the hot path.
 *
 * The risk it was addressing remains real but theoretical: a false match hands
 * a game selective retirement on a store whose earlier words bypassed the SMC
 * check.  Re-introducing it needs the writer pc and its region CONFIRMED from a
 * diagnostic build on hardware first -- ms0:/smchisto.txt reports storing pcs.
 *
 * Anything uncertain keeps the established full flush, which is the whole
 * point: this function's job is to say yes to one known routine, not to
 * classify stores in general. */
#ifdef SMC_WRITER_PROBE
/* DIAGNOSTIC ONLY, and in no build profile.  Record the distinct writer shapes
 * this function sees, so the fingerprint can be extended from measurement rather
 * than from a source comment.  Writes once, when the table first fills or at the
 * sample cap, then goes quiet -- this runs on the emulation thread and must not
 * turn into per-write file I/O. */
#define SWP_MAX 24
static u32 swp_pc[SWP_MAX], swp_addr[SWP_MAX], swp_op[SWP_MAX];
static u32 swp_hits[SWP_MAX], swp_used, swp_dumped, swp_seen;

static void swp_note(u32 pc, u32 addr, u32 op, int admitted)
{
  u32 i;
  FILE *f;
  swp_seen++;
  for (i = 0; i < swp_used; i++)
    if (swp_pc[i] == pc && swp_op[i] == op &&
        (swp_addr[i] - swp_pc[i]) == (addr - pc)) {
      swp_hits[i]++;
      goto maybe_dump;
    }
  if (swp_used < SWP_MAX) {
    i = swp_used++;
    swp_pc[i] = pc; swp_addr[i] = addr; swp_op[i] = op; swp_hits[i] = 1;
    /* `admitted` is folded into the opcode slot's sign bit-free companion via
     * hits; the dump prints the decode, which makes admission obvious. */
    (void)admitted;
  }
maybe_dump:
  if (swp_dumped || (swp_used < SWP_MAX && swp_seen < 20000))
    return;
  swp_dumped = 1;
  f = fopen("ms0:/PSP/GAME/GBADHOC-PERF/log/writer.txt", "w");
  if (!f)
    return;
  fprintf(f, "SMC writer shapes seen by smc_writer_safe_range\n");
  fprintf(f, "total traps=%u distinct=%u\n\n", (unsigned)swp_seen,
          (unsigned)swp_used);
  fprintf(f, "  writer_pc  target    delta  opcode    rlist base wb up pre  hits\n");
  for (i = 0; i < swp_used; i++) {
    u32 op = swp_op[i];
    fprintf(f,
      "  %08x   %08x  %5x  %08x  %04x  r%-3u %u  %u  %u   %u\n",
      (unsigned)swp_pc[i], (unsigned)swp_addr[i],
      (unsigned)(swp_addr[i] - swp_pc[i]), (unsigned)op,
      (unsigned)(op & 0xFFFF), (unsigned)((op >> 16) & 0xF),
      (unsigned)((op >> 21) & 1), (unsigned)((op >> 23) & 1),
      (unsigned)((op >> 24) & 1), (unsigned)swp_hits[i]);
  }
  fprintf(f, "\nUnbound admits: rlist=0003 base=r14 wb=0 up=1 pre=0 delta=3c\n");
  fclose(f);
}
#endif

/* How far past its own pc an M4A mixer patch store may reach and still be
 * recognised.  Measured maximum on Heart & Soul is 0x148; Unbound's is 0x3c. */
#define SMC_MIXER_PATCH_SPAN 0x200

#ifdef SMC_PARTIAL_SINGLE
static int smc_single_store;   /* set by smc_writer_safe_range per event */
/* SMC_SINGLE_UNTAG applies only to retirements caused by a WIDENED class or
 * a block-store word hit (stack/overlay code that is usually dead after the
 * write).  dr2's in-place patches (NFS, Smashing Drive, Doom) re-execute the
 * patched block at once; lifting its tags there only churned the retired-tag
 * table (twin: NFS Carbon +5%, Smashing +7% core). */
static int smc_untag_now;
/* Which single stores are admitted (smc-coverage 2026-10-20).  dr2 admitted
 * only IWRAM writers into IWRAM.  Neither restriction is a correctness
 * condition -- the store reports its whole write wherever it runs from, and
 * smc_write always resumes through the lookup -- so each is lifted by its own
 * default-off switch:
 *   SMC_SINGLE_ANYPC   writer anywhere (ROM, EWRAM, BIOS), not only IWRAM
 *   SMC_SINGLE_EWRAM   target EWRAM as well as IWRAM (smc_retire_span) */
#ifdef SMC_SINGLE_ANYPC
#define SMC_SINGLE_WRITER_OK(pc) 1
#else
#define SMC_SINGLE_WRITER_OK(pc) (((pc) >> 24) == 0x03)
#endif
#ifdef SMC_SINGLE_EWRAM
#define SMC_SINGLE_TARGET_OK(a) (((a) >> 24) == 0x03 || ((a) >> 24) == 0x02)
#else
#define SMC_SINGLE_TARGET_OK(a) (((a) >> 24) == 0x03)
#endif
/* SMC_SINGLE_HOT=K: a WIDENED single-store class (anything beyond dr2's ARM
 * STR/STRB from IWRAM into IWRAM) is only admitted once selective retirement
 * is already active, or after K widened traps since the caches were last
 * initialised.  Before that the trap takes the 4.1.1 path unchanged (full
 * flush + gate).  Reason: the corpus oracle (2026-10-20) showed that without
 * it nearly every game ACTIVATES selective retirement on its boot-time code
 * copies (a handful of ROM/Thumb writers), changing timing library-wide for
 * no gain; the titles that benefit trap thousands of times. */
static u32 smc_wide_traps;
static int smc_wide_gate(void)
{
#ifdef SMC_SINGLE_HOT
  /* Counted even when selective mode is already on (M4A mixer games): the
   * corpus showed Pokemon titles otherwise switch their few boot-time
   * widened traps to the new path for no gain. */
  if (smc_wide_traps >= SMC_SINGLE_HOT)
    return 1;
  smc_wide_traps++;
  return 0;
#else
  return 1;
#endif
}
#ifdef SMC_SINGLE_HOT
void smc_coverage_state_loaded(void) { smc_wide_traps = 0; }
#endif
#define SMC_DR2_CLASS(pc, a) ((((pc) >> 24) == 0x03) && (((a) >> 24) == 0x03))
#endif
static int smc_writer_safe_range(u32 *low, u32 *high)
{
  u32 pc = GBA_PC(reg[REG_PC]);
  u32 addr = smc_last_write_addr;
  u32 op;
  u8 *blk;

#ifdef SMC_PARTIAL_SAFE_CONTROL
  (void)low;
  (void)high;
  return 0;
#endif

  /* The first correctness oracle proved that selectively retiring arbitrary
   * single stores is still too broad for Heart & Soul. Only admit the exact
   * class needed by Unbound; all Thumb and non-STM writes keep full flushes. */
  if (reg[REG_CPSR] & 0x20) {
#if defined(SMC_PARTIAL_SINGLE) && defined(SMC_SINGLE_THUMB)
    /* SMC_SINGLE_THUMB (smc-coverage 2026-10-20): a Thumb single store --
     * STR/STRH/STRB, register or immediate offset, or STR [SP, #imm] --
     * reports its whole write exactly like an ARM STR (no unchecked words),
     * so the same exact retirement applies.  The store emitter loads REG_PC
     * with pc + 2 for Thumb.  Thumb STMIA/PUSH stay on the full flush. */
    u32 top;
    pc -= 2;
    /* Decode only from a RESIDENT page.  4.1.1 returned before touching
     * memory for every Thumb trap; paging a ROM block in here would change
     * the paged-ROM state (twin: Heart & Soul light diverged at f2005 with
     * no widened store ever admitted).  Not resident = not admitted. */
    blk = memory_map_read[pc >> 15];
    if (!blk)
      return 0;
    op = readaddress16(blk, pc & 0x7FFF);
    top = op & 0xFE00;
    if ((top == 0x5000 || top == 0x5200 || top == 0x5400 ||
         (op & 0xF800) == 0x6000 || (op & 0xF800) == 0x7000 ||
         (op & 0xF800) == 0x8000 || (op & 0xF800) == 0x9000
        ) &&
        SMC_SINGLE_WRITER_OK(pc) && SMC_SINGLE_TARGET_OK(addr) &&
        smc_wide_gate()) {
      *low = addr & ~3u;
      *high = *low + 4;
      smc_single_store = 1;
      smc_untag_now = 1;
      return 1;
    }
#endif
    return 0;
  }

  pc -= 4;
  blk = memory_map_read[pc >> 15];
  if (!blk)
    blk = load_gamepak_page((pc >> 15) & 0x3FF);
  op = readaddress32(blk, pc & 0x7FFF);
#ifdef SMC_WRITER_PROBE
  if ((op & 0x0E000000) == 0x08000000)
    swp_note(pc, addr, op, 0);
#endif
#ifdef SMC_PARTIAL_SINGLE
  /* A single-word ARM store (STR/STRB) executing from IWRAM into IWRAM.
   * SMC_CLASS_PROBE (2026-10-20) measured the slow issue #9 titles: Doom II,
   * Duke Nukem Advance and Smashing Drive raise 100% of their SMC traps here,
   * from 4-6 writer pcs that each patch ONE fixed word of their own render
   * loop, 12-22 times a frame -- and every trap was a full RAM flush.
   *
   * The block-copy hole (2.0.2) does not apply: a single store has no
   * unchecked earlier words, so the trapping word IS the whole write and the
   * range below is exact.  STRB writes inside that word; retiring the word's
   * covering blocks is a superset.  Everything after this -- the one-time gate
   * install with its full flush, gated-target checks, retirement -- is the
   * proven M4A path unchanged.  Writers outside IWRAM, Thumb stores, halfword
   * stores and every STM except the mixer's keep the full flush. */
  if (((op & 0x0C100000) == 0x04000000       /* LDR/STR class, L=0: a store */
#ifdef SMC_SINGLE_HALF
       /* SMC_SINGLE_HALF: ARM STRH (halfword store, L=0) -- one write. */
       || (op & 0x0E1000F0) == 0x000000B0
#endif
      ) && SMC_SINGLE_WRITER_OK(pc) && SMC_SINGLE_TARGET_OK(addr) &&
      (((op & 0x0C100000) == 0x04000000 && SMC_DR2_CLASS(pc, addr)) ||
       smc_wide_gate())) {
    *low = addr & ~3u;
    *high = *low + 4;
    smc_single_store = 1;
    smc_untag_now = !((op & 0x0C100000) == 0x04000000 && SMC_DR2_CLASS(pc, addr));
    return 1;
  }
#endif
#if defined(SMC_PARTIAL_SINGLE) && defined(SMC_BLOCK_EXACT)
  /* SMC_BLOCK_EXACT: every earlier word of this block store went through the
   * checked aligned stub (smc_block_word_hit retired what it hit), so the
   * trapping final word is the only unhandled part of the write: the store
   * is a single word as far as retirement is concerned.  M4A's mixer patch
   * keeps its established gated range path (same predicate as below), so
   * Unbound and Heart & Soul keep their 4.1.1 block layout. */
  /* Writeback block stores (advancing copies: Hot Wheels and LEGO copy code
   * into IWRAM every frame with STMIA r0!,{r4-r11}) keep the full flush: one
   * flush, after which the rest of the copy runs untrapped, is far cheaper
   * than retiring the destination block by block (twin: +27% core on Hot
   * Wheels).  That is cost policy only -- their earlier words are still
   * handled exactly by the aligned stub. */
  if ((op & 0x0E300000) == 0x08000000 &&       /* STM, L=0, W=0 */
      !(((op >> 16) & 0xF) == REG_LR && !(op & 0x01000000) &&
        (op & 0x00800000) && !(op & 0x00400000) && !(op & 0x00200000) &&
        addr > pc && addr - pc <= SMC_MIXER_PATCH_SPAN && (op & 0xFFFF)) &&
      SMC_SINGLE_WRITER_OK(pc) && SMC_SINGLE_TARGET_OK(addr) &&
      smc_wide_gate()) {
    *low = addr & ~3u;
    *high = *low + 4;
    smc_single_store = 1;
    smc_untag_now = 1;
    return 1;
  }
#endif
  if ((op & 0x0E000000) == 0x08000000) {
    u32 list, count = 0;
    /* Loads cannot raise this store trap; treat an inconsistent decode as a
     * reason to use the established full flush. */
    if ((op & 0x00100000) || (op & 0x00200000))
      return 0;
    /* Admit M4A's SoundMainRAM patch stores: a non-writeback ARM STMIA through
     * LR into the nearby generated mixer body.
     *
     * This used to require the checked word to sit EXACTLY 0x3c bytes past the
     * writer, which is CFRU's (Unbound's) layout, and to be exactly {r0,r1}.
     * Heart & Soul runs the same routine family elsewhere and therefore kept the
     * coarse path on every mixer write.  Measured with SMC_WRITER_PROBE on
     * heart_soul_heavy, 2815 traps: FOUR writer sites across SIXTEEN deltas
     * (0x48..0x148), two storing two words and two storing four, the dominant
     * one matching CFRU's shape on every field except the delta and accounting
     * for 66% of traps.
     *
     * The delta was never a correctness condition.  The range returned below is
     * derived from the decoded register count and covers every word the store
     * writes, which is precisely the hazard a wider match is feared to expose --
     * see the note above this function.  For STMIA with U=1 and P=0 the trapping
     * address is the highest word, which is what makes that subtraction right for
     * any count.  So the exact offset was conservatism: one known routine rather
     * than one known routine FAMILY.
     *
     * WRITEBACK (bit 21) IS NOW CHECKED, and that is not cosmetic.  The old
     * condition never tested it because the exact delta already excluded every
     * writeback store it could have met; with a range that accident is gone.  The
     * safety argument holds only for non-writeback stores, because an advancing
     * copy can write code whose final word never lands on a tag.  The probe found
     * exactly those -- base r0, list 07f8, W set -- copying into the stack, and
     * they must keep the full flush.
     *
     * The delta stays bounded: a self-patching mixer writes near itself, and a
     * store reaching far past its own pc is a different shape.  That bound is
     * identity conservatism, not correctness, but it limits what a false match
     * could reach. */
    if (((op >> 16) & 0xF) != REG_LR ||
        (op & 0x01000000) || !(op & 0x00800000) || (op & 0x00400000) ||
        (op & 0x00200000) ||                 /* W: writeback must be clear */
        addr <= pc || addr - pc > SMC_MIXER_PATCH_SPAN)
      return 0;
    list = op & 0xFFFF;
    while (list) {
      count += list & 1;
      list >>= 1;
    }
    if (!count)
      return 0;
    *low = addr - (count - 1) * 4;
    *high = addr + 4;
    return 1;
  }
  return 0;
}

#if defined(SMC_PARTIAL_SINGLE) && defined(SMC_PARTIAL_STABLE_THUNK)
/* Retire every live RAM translation whose recorded source covers the one
 * word [word, word+4) that a single ARM store just rewrote, and nothing else.
 *
 * Why this is exact without a gate: a single store reports its whole write
 * (no unchecked earlier words, unlike STM -- the 2.0.2 block-copy hole), and
 * SMC_PARTIAL_SAFE records an exact canonical [start, end) for every live
 * block, so "covers the word" is decidable.  The scan window reaches back by
 * the longest recorded extent (SMC_RETIRE_WINDOW), which no covering block can
 * start before.  A live block with no recorded extent makes coverage unknowable:
 * that is the established full flush.  After the store the stub always resumes
 * through the lookup (smc_write -> lookup_pc_noflags), so the running block is
 * never continued past the write.
 *
 * The rewritten word's interior marks (CODE_TAG 0x0101) are then cleared: the
 * slow issue #9 titles patch the same word 12-22 times a frame, and each write
 * before the patched code runs again would otherwise trap and retire nothing.
 * Retranslation re-marks the word.  Block-start tags stay put: their entries
 * now have code_bytes == 0, so the next lookup retranslates them. */
/* smc_retire_span: the region-generic form of the above.  Retire every live
 * translation (either mode) whose recorded canonical source [start, end)
 * overlaps [lo, hi) -- a word-aligned range inside ONE region (IWRAM or
 * EWRAM) that the trapping store wrote IN FULL -- then clear the range's
 * interior marks.  Returns 0 without changing anything when coverage is not
 * decidable (wrapping block live, a live block in the window without an
 * extent, range outside the canonical window): the caller full-flushes.
 *
 * The exactness argument is the single-word one, unchanged: SMC_PARTIAL_SAFE
 * records an exact extent for every block translated since the last full
 * flush (activation takes one), the window reaches back by the longest such
 * extent, and a covering block must start inside it.  What the CALLER must
 * guarantee is that [lo, hi) is the WHOLE write: no word of this store, and
 * no earlier store, went to tagged memory unreported (the block-copy hole). */
#if defined(SMC_RETIRE_LIST) && !defined(SMC_RETIRE_LIST_N)
#define SMC_RETIRE_LIST_N 64
#endif
#if defined(DMA_SMC_ADAPT) || defined(SMC_WORK_STATS)
/* Deterministic work counters (DMA_SMC_ADAPT's cost model; SMC_WORK_STATS
 * prints them on the twin): tag slots walked by selective retirement, and
 * guest halfwords scanned by RAM translations. */
u32 smc_work_walk, smc_work_halves;
#if defined(DMA_ADAPT_V2) || defined(SMC_WORK_WINDOWS)
u32 smc_work_blocks;   /* RAM translations (DMA_ADAPT_V2's per-block price) */
#endif
#if defined(SMC_WORK_WINDOWS) && defined(DRPROF_TWIN)
/* DIAGNOSTIC (twin only, tgr-regression 2026-10-28): cumulative SMC work
 * counters once per emulated frame (host frame index = drprof's F index),
 * to stderr, so the cost model's prices can be fitted against the twin's
 * per-frame zone instruction counts. */
void smc_work_window_dump(void)
{
  static u32 n;
  n++;
  fprintf(stderr, "WW %u %u %u %u %u %u %u %u %u %u %u %u\n", (unsigned)n,
          (unsigned)frame_counter, (unsigned)smc_work_halves,
          (unsigned)smc_work_walk, (unsigned)flush_ram_total,
          (unsigned)flush_ram_partial, (unsigned)flush_ram_smc,
          (unsigned)flush_ram_dma, (unsigned)jit_code_stats.publishes,
          (unsigned)jit_code_stats.spans, (unsigned)jit_code_stats.lines,
          (unsigned)smc_work_blocks);
}
#endif
#define SMC_WORK_WALK(n) (smc_work_walk += (u32)(n))
#else
#define SMC_WORK_WALK(n) ((void)0)
#endif
static int smc_retire_span(u32 lo, u32 hi)
{
  u16 *tagp;
  u32 base, bytes, lo_off, hi_off, span, first, last, i;
  u32 rgn;
#if defined(DISPATCH_CACHE) && !defined(DISPATCH_PRECISE)
  int dc_cleared = 0;
#endif
#ifdef SMC_SINGLE_UNTAG
  u32 ulo = hi, uhi = lo;      /* union of the retired blocks' extents */
#endif

  if ((lo >> 24) == 3) {
    tagp = (u16 *)iwram;
    base = 0x03000000;
    bytes = 0x8000;
    rgn = 0;
  } else if ((lo >> 24) == 2) {
    tagp = (u16 *)(ewram + 0x40000);
    base = 0x02000000;
    bytes = 0x40000;
    rgn = 1;
  } else
    return 0;
  if (hi <= lo || (lo & 3) || (hi & 3) || ((hi - 1) >> 24) != (lo >> 24))
    return 0;
  lo_off = lo - base;
  hi_off = hi - base;
  if (hi_off > bytes || smc_partial_wrap_live)
    return 0;
#ifdef SMC_RETIRE_WINDOW
  span = smc_max_extent[rgn] + 4;
  if (!smc_window_armed || span > MAX_BLOCK_SIZE * 4)
    span = MAX_BLOCK_SIZE * 4;
#else
  (void)rgn;
  span = MAX_BLOCK_SIZE * 4;
#endif
  first = (lo_off > span) ? (lo_off - span) >> 1 : 0;
  SMC_LOCAL_NARROW(first, rgn, lo_off, hi_off, tagp, base);
  last = (hi_off - 2) >> 1;            /* last halfword slot of the range */
  SMC_WORK_WALK(last - first + 1);

  /* Validate before changing anything: every live block in the window must
   * have a known extent. */
#ifdef SMC_RETIRE_LIST
  /* SMC_RETIRE_LIST (slow-titles): the validation walk skips aligned pairs of
   * interior marks a word at a time and remembers where the block-start tags
   * are, so the retire pass below visits only those (same slots, same order);
   * more than SMC_RETIRE_LIST_N of them -> the retire pass walks as before. */
  u32 rl_ix[SMC_RETIRE_LIST_N], rl_n = 0, rl_k = 0;
#endif
  for (i = first; i <= last; i++) {
#ifdef SMC_RETIRE_LIST
    if (!(i & 1) && i < last &&
        ((const u32 *)tagp)[i >> 1] == CODE_TAG_BLOCK32) {
      i++;
      continue;
    }
#endif
    if (VALID_TAG(tagp[i])) {
      ramtag_type *te = get_ram_tag(tagp[i]);
      if ((SMC_RAM_LIVE_OFFSET(te, arm) && !te->blk_end_arm) ||
          (SMC_RAM_LIVE_OFFSET(te, thumb) && !te->blk_end_thumb))
        return 0;
#ifdef SMC_RETIRE_LIST
      if (rl_n < SMC_RETIRE_LIST_N)
        rl_ix[rl_n] = i;
      rl_n++;
#endif
    }
  }
#ifdef SMC_RETIRE_LIST
  for (i = first; ; i++) {
    if (rl_n <= SMC_RETIRE_LIST_N) {
      if (rl_k >= rl_n)
        break;
      i = rl_ix[rl_k++];
    } else if (i > last)
      break;
#else
  for (i = first; i <= last; i++) {
#endif
    if (VALID_TAG(tagp[i])) {
      ramtag_type *te = get_ram_tag(tagp[i]);
      u32 start = base + (i << 1);
#if defined(DISPATCH_CACHE) && !defined(DISPATCH_PRECISE)
      /* Empty the dispatch cache before the FIRST retirement only: a cached
       * pc may map to a retired thunk (which itself dispatches: a loop).
       * Nothing retired, nothing to empty (smc-coverage: the unconditional
       * 8 KiB memset was a third of Hot Wheels' retire cost). */
      if (!dc_cleared &&
          ((SMC_RAM_LIVE_OFFSET(te, arm) && start < hi && te->blk_end_arm > lo) ||
           (SMC_RAM_LIVE_OFFSET(te, thumb) && start < hi && te->blk_end_thumb > lo))) {
        dispatch_cache_clear();
        dc_cleared = 1;
      }
#endif
#ifndef SMC_COVERAGE_MUTANT_SKIP_LAST
      if (SMC_RAM_LIVE_OFFSET(te, arm) && start < hi &&
          te->blk_end_arm > lo) {
#else
      /* MUTANT (oracle self-test only): never retire a covering ARM block
       * that STARTS inside the written range itself. */
      if (SMC_RAM_LIVE_OFFSET(te, arm) && start < lo &&
          te->blk_end_arm > lo) {
#endif
        SMC_EVT("  R arm %08x..%08x\n", (unsigned)start, (unsigned)te->blk_end_arm);
        smc_stable_thunk_dispatch(SMC_RAM_PUBLIC_ENTRY(te, arm), start, 0);
        te->code_bytes_arm = 0;
        flush_ram_partial++;
#ifdef SMC_SINGLE_UNTAG
        if (start < ulo) ulo = start;
        if (te->blk_end_arm > uhi) uhi = te->blk_end_arm;
#endif
      }
      if (SMC_RAM_LIVE_OFFSET(te, thumb) && start < hi &&
          te->blk_end_thumb > lo) {
        SMC_EVT("  R thumb %08x..%08x\n", (unsigned)start, (unsigned)te->blk_end_thumb);
        smc_stable_thunk_dispatch(SMC_RAM_PUBLIC_ENTRY(te, thumb), start, 1);
        te->code_bytes_thumb = 0;
        flush_ram_partial++;
#ifdef SMC_SINGLE_UNTAG
        if (start < ulo) ulo = start;
        if (te->blk_end_thumb > uhi) uhi = te->blk_end_thumb;
#endif
      }
    }
  }
#ifdef JIT_CODE_DISCIPLINE
  jit_code_publish();
#endif
#ifdef SMC_SINGLE_UNTAG
  if (smc_untag_now)
  /* SMC_SINGLE_UNTAG: do for the retired blocks what a full flush does for
   * every block -- take their code marks off guest memory -- but only where
   * no block that is STILL LIVE covers the halfword.  Region U is the union
   * of the retired blocks' extents (or just the written range when nothing
   * was retired).  A halfword of U no live extent covers loses its interior
   * mark (CODE_TAG_BLOCK16), and a block-start tag there whose entry has no
   * live translation in either mode is lifted into smc_retired_tags, which
   * the next lookup at that address restores (SMC_RESTORE_RETIRED_TAG; the
   * mechanism the mixer range path already uses).  Without this, every later
   * store into a retired block's remaining words traps again and retires
   * nothing: Top Gear Rally's stack (167k traps / 1200 frames), Hot Wheels'
   * per-frame code copy (50k traps vs 374 full flushes on 4.1.1).
   * A live block keeps every one of its marks, so no write to live code can
   * go unreported.  A full retired-tag table keeps the tag (never a flush). */
  {
    u32 u_lo, u_hi, n, j, k, scan0;
    u8 cov[(MAX_BLOCK_SIZE * 8) / 16];   /* 1 bit per halfword of U */
    if (ulo >= uhi) { ulo = lo; uhi = hi; }
#ifdef SMC_STM_CHAIN
    /* ...and the written range itself, which may hold chain marks past the
     * retired extents (a block store's later words over data). */
    else { if (lo < ulo) ulo = lo; if (hi > uhi) uhi = hi; }
#endif
    if (ulo < base) ulo = base;
    if (uhi > base + bytes) uhi = base + bytes;
    u_lo = (ulo - base) >> 1;
    u_hi = (uhi - base + 1) >> 1;
    n = u_hi - u_lo;
    if (n <= sizeof(cov) * 8) {
      memset(cov, 0, (n + 7) >> 3);
      scan0 = ((ulo - base) > span) ? ((ulo - base) - span) >> 1 : 0;
      SMC_LOCAL_NARROW(scan0, rgn, ulo - base, uhi - base, tagp, base);
      SMC_WORK_WALK(u_hi - scan0 + n);
      for (i = scan0; i < u_hi; i++) {
        if (VALID_TAG(tagp[i])) {
          ramtag_type *te = get_ram_tag(tagp[i]);
          u32 e[2], m;
          e[0] = SMC_RAM_LIVE_OFFSET(te, arm) ? te->blk_end_arm : 0;
          e[1] = SMC_RAM_LIVE_OFFSET(te, thumb) ? te->blk_end_thumb : 0;
          for (m = 0; m < 2; m++) {
            u32 e_off;
            if (!e[m]) continue;
            e_off = (e[m] - base + 1) >> 1;          /* exclusive, halfwords */
            for (k = (i > u_lo ? i : u_lo); k < u_hi && k < e_off; k++)
              cov[(k - u_lo) >> 3] |= (u8)(1u << ((k - u_lo) & 7));
          }
        }
      }
      for (j = 0; j < n; j++) {
        u32 h = u_lo + j;
        if (cov[j >> 3] & (1u << (j & 7)))
          continue;
        XS_CLEAR_HALF(rgn == 0, h);   /* no live block covers h */
        if (tagp[h] == CODE_TAG_BLOCK16)
          tagp[h] = 0;
#ifndef SMC_UNTAG_MARKS_ONLY   /* experiment: interior marks only */
        else if (VALID_TAG(tagp[h])) {
          ramtag_type *te = get_ram_tag(tagp[h]);
          if (!SMC_RAM_LIVE_OFFSET(te, arm) && !SMC_RAM_LIVE_OFFSET(te, thumb) &&
              smc_remember_retired_tag(base + (h << 1), tagp[h]))
            tagp[h] = 0;
        }
#endif
      }
    }
  }
#endif
  for (i = lo_off >> 1; i < (hi_off >> 1); i++) {
    XS_CLEAR_HALF(rgn == 0, i);   /* every covering block was just retired */
    if (tagp[i] == CODE_TAG_BLOCK16)
      tagp[i] = 0;
  }
  return 1;
}

#ifdef SMC_STM_CHAIN
/* SMC_STM_CHAIN (cpu-levers 2026-10-27): a non-final word of a block store
 * (ARM STM, Thumb STMIA; not the SP fast path) just landed on a tagged word
 * of IWRAM or EWRAM -- translated code, or a mark this function left.  Only
 * the FINAL word of a block store is SMC-checked, so a store whose earlier
 * words overwrite code while its final word lands on untagged data went
 * unreported (FIFA 07's sound generator, the 03005d54 stale B, every
 * release).  Mark the NEXT word with interior marks if it has none: the
 * next word's stub sees them and passes the mark on, and the final word's
 * ordinary check traps.  The trap then describes the write (SMC_STM_RANGE
 * retires its whole span exactly; anything else takes its usual path).
 * The marks are cleared like any interior mark: by the trap's untag of the
 * written range, or by the next full flush, whose range code_max is raised
 * to cover them.  A stray mark costs one trap later, never correctness. */
/* VALUE FILTER: a word stored with the value it already holds changes no
 * code, so it starts no chain (Mario Golf re-copies unchanged code with block
 * stores: chaining those cost the twin +8.9 %, as SMC_BLOCK_WORDS' check
 * cost the 1000 +11.6 %).  Sound because every other write to a tagged word
 * traps and retires, so a tagged word holds what was translated from it.
 * Once a word has changed code, smc_chain_pending carries the chain over the
 * following words whatever their values, until the trap it leads to
 * (smc_trap_entry clears it; a chain that runs off the region's end leaves it
 * set, which costs at most one needless mark later). */
#ifndef SMC_STM_RANGE
#error SMC_STM_CHAIN needs SMC_STM_RANGE (smc_trap_entry ends a chain)
#endif
u32 smc_chain_marks, smc_chain_same;
u32 smc_chain_pending;
static inline void smc_chain_store(u16 *tagp, u8 *data, u32 off, u32 size,
                                   u32 val, u32 *code_max)
{
  u32 w = off & ~3u, nxt = w + 4;
  u32 *p = (u32 *)(data + w);
  u16 *t;
#ifndef SMC_STM_CHAIN_MUTANT_NOFILTER   /* MUTANT: chain on every hit */
#ifdef SMC_STM_CHAIN_MUTANT_NOPENDING   /* MUTANT: unchanged word ends it */
  if (*p == val) {
#else
  if (*p == val && !smc_chain_pending) {
#endif
    smc_chain_same++;
    return;                        /* unchanged: no chain (the value stands) */
  }
#endif
  *p = val;
  smc_chain_pending = 1;
  if (nxt + 4 > size)
    return;                        /* the region's last word: nothing after */
  t = &tagp[nxt >> 1];
#ifndef SMC_STM_CHAIN_MUTANT       /* MUTANT (oracle self-test): no chain */
  if (!t[0] && !t[1]) {
    t[0] = t[1] = CODE_TAG_BLOCK16;
    smc_chain_marks++;
    if (*code_max < nxt + 4)
      *code_max = nxt + 4;
  }
#endif
}
void smc_chain_store_iw(u32 off, u32 val)
{
  smc_chain_store((u16 *)iwram, iwram + 0x8000, off & 0x7FFF, 0x8000, val,
                  &iwram_code_max);
}
void smc_chain_store_ew(u32 off, u32 val)
{
  smc_chain_store((u16 *)(ewram + 0x40000), ewram, off & 0x3FFFF, 0x40000,
                  val, &ewram_code_max);
}
#endif
#ifdef SMC_BLOCK_EXACT
/* Called from the aligned (non-final block-store word) IWRAM/EWRAM store stub
 * when the word it just stored was tagged as code (mips_emit.h,
 * SMC_BLOCK_EXACT).  Same arguments as smc_prof_note_store.  The stub returns
 * into the running store sequence afterwards, so nothing here may flush the
 * whole cache (the running block would continue in freed cache): when exact
 * retirement is not possible the full flush is deferred to the next SMC trap
 * (smc_block_pending_full), which is no later than 4.1.1 handled such a word
 * -- 4.1.1 never noticed it at all.  While selective retirement is inactive
 * every trap is a full flush anyway, so nothing is done (4.1.1 behaviour). */
u32 smc_block_hits, smc_block_retired, smc_block_deferred;
void smc_block_word_hit(u32 base_plus_off, u32 off)
{
  u32 lui = base_plus_off - off, addr;
  if (lui == smc_prof_lui_iwram)
    addr = 0x03000000 + (off & 0x7FFC);
  else if (lui == smc_prof_lui_ewram)
    addr = 0x02000000 + (off & 0x3FFFC);
  else
    return;
  smc_block_hits++;
  SMC_FB(12);
  if (!smc_partial_active)
    return;
#ifndef SMC_COVERAGE_MUTANT_BLOCK_IGNORE
  smc_untag_now = 1;
  if (smc_retire_span(addr, addr + 4)) {
    smc_block_retired++;
    SMC_FB(13);
  } else {
    smc_block_pending_full = 1;
    smc_block_deferred++;
    SMC_FB(14);
  }
#endif
}
#endif

#ifdef SMC_SAME_SKIP
static __attribute__((noinline)) u32 xs_trap_skip(u32 word);
#endif
static int smc_retire_single_word(u32 word)
{
#ifdef SMC_IMM_PATCH
  if (immp_try(word))
    return 1;
#endif
#ifdef SMC_SAME_SKIP
  /* Nothing translated from `word` is stale: report it handled, as a
   * retirement would be (counted in xs_trap_skip; see LAYOUT NEUTRALITY). */
  word = xs_trap_skip(word);   /* 0 = skip; else `word` back, so it need */
  if (!word)                   /* not live across the call               */
    return 1;
#endif
#ifdef SMC_IMM_PATCH
  immp_learn(word);
#endif
#if defined(SMC_SINGLE_EWRAM)
  return smc_retire_span(word, word + 4);
#else
  /* The validated 2026-10-20 (dr2) path: IWRAM only. */
  if ((word >> 24) != 3)
    return 0;
  return smc_retire_span(word, word + 4);
#endif
}

#ifdef SMC_SAME_SKIP
/* SMC_SAME_SKIP's decision for a single-store trap at IWRAM word `word`:
 * 1 = every live block smc_retire_span would retire for this word is
 * CURRENT -- each halfword of its whole recorded source [start, end) holds
 * exactly what the translation read (xs record valid, not poisoned, equal to
 * memory now) -- so retiring them would retranslate identical code.
 * 0 = anything else, including "no live block covers the word" (retire_span
 * then only clears marks, as before) and every case retire_span itself
 * refuses (unknown extent, wrapping block, outside the window).
 *
 * Checking the WHOLE extent, not just the trapping word, is what makes this
 * independent of the block-store hole: a word some earlier store changed
 * without a trap (STM non-final word, the stale-FIFA case) no longer matches
 * its record, so the trap retires the block exactly as before.  The record is
 * what every live reader read because a scan that finds a valid record with a
 * different value poisons it instead of overwriting it (xs_note_half). */
static __attribute__((noinline)) int xs_blocks_current(u32 word);
/* LAYOUT NEUTRALITY (cpu-levers 2026-10-27).  flush_translation_cache_ram_smc,
 * ramtag_note_extent and flush_translation_cache_ram are in the pinned hot
 * order (psp/layout/hot.ord).  The first cut of this lever grew them by up to
 * 1.1 KiB.  That moved 59 of the 83 pinned functions to other I-cache sets
 * (dma_tf_loop16/32, trigger_dma, write_io_register8, ...).  On the 3000,
 * cpl2 cost AW2 +7.5 %, FE8 +7.7 % and Emerald +3.6 % work per frame, with
 * no extra instructions on the twin.  So every piece of the lever that sits
 * in a pinned function is ONE call to an out-of-line helper, and those
 * functions stay within their cpl1 link slack. */
#ifdef SMC_DIFF_CENSUS
/* DIAGNOSTIC (xlat-cost 2026-10-28, twin only): for every IWRAM trap that
 * RETIRES (no same-value skip), what changed between the word the
 * translations read (xs_val) and memory now.  Classes: ARM data-processing
 * immediate field only (bits 11:0), ARM load/store immediate offset only,
 * ARM branch offset only, ARM condition only, other ARM, Thumb halfword
 * (imm8 / imm5 / other), unrecorded. */
static u32 sdc_n[16];
static u32 sdc_words[0x2000];
static void sdc_report(void)
{
  static const char *nm[16] = { "unrec", "same", "arm_dp_imm", "arm_ldst_off",
    "arm_b_off", "arm_cond", "arm_other", "thumb_imm8", "thumb_imm5",
    "thumb_other", "arm_dp_imm_S", "arm_ldst_other", "", "", "", "" };
  u32 k, d = 0;
  for (k = 0; k < 0x2000; k++) d += sdc_words[k] != 0;
  fprintf(stderr, "SMC_DIFF");
  for (k = 0; k < 12; k++) fprintf(stderr, " %s=%u", nm[k], (unsigned)sdc_n[k]);
  fprintf(stderr, " distinct_words=%u\n", (unsigned)d);
}
static void sdc_note(u32 word)
{
  static int init;
  u32 h, ow, nw, x;
  if (!init) { init = 1; atexit(sdc_report); }
  if ((word >> 24) != 3) return;
  h = (word & 0x7FFC) >> 1;
  if (sdc_words[h >> 1] < 0xFFFFFFFF) sdc_words[h >> 1]++;
  if ((xs_ok[h >> 3] & (3u << (h & 7))) != (3u << (h & 7))) { sdc_n[0]++; return; }
  ow = xs_val[h] | ((u32)xs_val[h + 1] << 16);
  nw = *(u32 *)(iwram + 0x8000 + (h << 1));
  x = ow ^ nw;
  if (!x) { sdc_n[1]++; return; }
  if (reg[REG_CPSR] & 0x20) {
    /* Thumb: classify the changed halfword(s) by the low one that changed */
    u32 hw = (x & 0xFFFF) ? (nw & 0xFFFF) : (nw >> 16);
    u32 hx = (x & 0xFFFF) ? (x & 0xFFFF) : (x >> 16);
    if (!(hx & ~0xFFu) && ((hw >> 13) == 1 || (hw >> 12) == 9 || (hw >> 12) == 13))
      sdc_n[7]++;
    else if (!(hx & ~0x07C0u)) sdc_n[8]++;
    else sdc_n[9]++;
    return;
  }
  if (!(x & 0x0FFFFFFF)) { sdc_n[5]++; return; }
  switch ((nw >> 25) & 7) {
    case 1:
      if (!(x & ~0xFFFu)) { sdc_n[(nw & (1u << 20)) ? 10 : 2]++; return; }
      break;
    case 2:
      if (!(x & ~0xFFFu)) { sdc_n[3]++; return; }
      sdc_n[11]++; return;
    case 5:
      if (!(x & ~0xFFFFFFu)) { sdc_n[4]++; return; }
      break;
  }
  sdc_n[6]++;
}
#endif
static __attribute__((noinline)) u32 xs_trap_skip(u32 word)
{
  if (XS_ON() && xs_blocks_current(word)) {
    smc_same_skips++;
#ifdef SMC_SAME_ADAPT
    xs_win_skips++;
#endif
    return 0;
  }
#ifdef SMC_DIFF_CENSUS
  sdc_note(word);
#endif
#ifdef SMC_SAME_ADAPT
  /* A retirement follows (or, if that fails, a full flush: either way the
   * event a skip would have saved). */
  if ((word >> 24) == 3)
    xs_adapt_retired();
#endif
  return word;
}
#ifdef XS_WALK_PAIR
/* XS_WALK_PAIR (cpu-levers cpl2w; from SMC_WALK_FAST fb5fee5): the first
 * block-start tag at or after halfword slot i, up to `last` (last + 1 when
 * none).  Tags are 0 (data), CODE_TAG_BLOCK16 0x0101 (interior), or a block
 * start > 0x0101 with a byte >= 2 (counts down from 0xFFFF, odd), so a pair
 * of slots whose bytes are all 0 or 1 holds no block start: one aligned load
 * tests two slots.  Exact: the same slots, in the same order, as testing
 * VALID_TAG one slot at a time.  xs_blocks_current's window on Doom/Duke is
 * ~235 slots for ~4.6 block starts. */
typedef u32 __attribute__((may_alias)) xs_tagpair;
static inline u32 xs_tag_next(const u16 *tagp, u32 i, u32 last)
{
  if ((i & 1) && i <= last) {
    if (VALID_TAG(tagp[i]))
      return i;
    i++;
  }
  while (i + 1 <= last) {
    if (*(const xs_tagpair *)&tagp[i] & 0xFEFEFEFEu) {
      if (VALID_TAG(tagp[i]))
        return i;
#ifndef XS_WALK_PAIR_MUTANT      /* MUTANT (oracle self-test only) */
      if (VALID_TAG(tagp[i + 1]))
        return i + 1;
#endif
    }
    i += 2;
  }
  if (i <= last && VALID_TAG(tagp[i]))
    return i;
  return last + 1;
}
#endif
static __attribute__((noinline)) int xs_blocks_current(u32 word)
{
  u16 *tagp = (u16 *)iwram;
  const u32 base = 0x03000000, bytes = 0x8000;
  u32 lo = word & ~3u, hi = lo + 4, lo_off, span, first, last, i, m;
  int found = 0;

  if ((lo >> 24) != 3 || !xs_ready || smc_partial_wrap_live)
    return 0;
  lo_off = lo - base;
  if (lo_off + 4 > bytes)
    return 0;
#ifdef SMC_RETIRE_WINDOW
  span = smc_max_extent[0] + 4;
  if (!smc_window_armed || span > MAX_BLOCK_SIZE * 4)
    span = MAX_BLOCK_SIZE * 4;
#else
  span = MAX_BLOCK_SIZE * 4;
#endif
  first = (lo_off > span) ? (lo_off - span) >> 1 : 0;
  last = (lo_off + 2) >> 1;            /* last halfword slot of the word */
  {
    /* Neither half recorded: no live block covers the word. */
    u32 h0 = lo_off >> 1;
    if (!(xs_ok[h0 >> 3] & (3u << (h0 & 7))))
      return 0;
    /* Every live block covering the word scanned it, so starts at or after
     * xs_min (see xs_note_half); retire_span's window is the outer bound. */
    if (xs_min[lo_off >> 2] > first)
      first = xs_min[lo_off >> 2];
  }
#ifdef XS_COST_PROBE
  xs_calls++; xs_slots += last - first + 1;
#endif
#ifdef XS_WALK_PAIR
  for (i = xs_tag_next(tagp, first, last); i <= last;
       i = xs_tag_next(tagp, i + 1, last)) {
#else
  for (i = first; i <= last; i++) {
#endif
    ramtag_type *te;
    u32 start, e[2];
    if (!VALID_TAG(tagp[i]))
      continue;
    te = get_ram_tag(tagp[i]);
    start = base + (i << 1);
    e[0] = SMC_RAM_LIVE_OFFSET(te, arm) ? te->blk_end_arm : 1;
    e[1] = SMC_RAM_LIVE_OFFSET(te, thumb) ? te->blk_end_thumb : 1;
    for (m = 0; m < 2; m++) {
      u32 h, h_end;
      if (e[m] == 1)
        continue;                      /* no live translation in this mode */
      if (!e[m])
        return 0;                      /* live without an extent: unknowable */
      if (!(start < hi && e[m] > lo))
        continue;                      /* does not cover the word */
      if (e[m] > base + bytes)
        return 0;
#if defined(SMC_SAME_ADAPT) && !defined(SMC_SAME_ADAPT_MUTANT_NOREC)
      if (!XS_REC_GET(tagp[i], m))
        return 0;                      /* translated while not recording */
#endif
      h_end = (e[m] - base + 1) >> 1;
#ifdef SMC_SAME_SKIP_MUTANT_NOCHECK
      /* MUTANT (oracle self-test only): trust any covering block. */
      h_end = i;
#endif
#ifdef XS_COST_PROBE
      xs_halves += h_end - i;
#endif
      for (h = i; h < h_end; ) {
        if (!(h & 7) && h + 8 <= h_end) {
          /* 8 halfwords at once: all recorded, none poisoned, all equal */
          const u32 *r = (const u32 *)&xs_val[h];
          const u32 *g = (const u32 *)(iwram + 0x8000 + (h << 1));
          if (xs_ok[h >> 3] != 0xFF || xs_bad[h >> 3] ||
              ((r[0] ^ g[0]) | (r[1] ^ g[1]) | (r[2] ^ g[2]) | (r[3] ^ g[3])))
            return 0;
          h += 8;
        } else {
          u8 bit = (u8)(1u << (h & 7));
          if (!(xs_ok[h >> 3] & bit) || (xs_bad[h >> 3] & bit) ||
              xs_val[h] != *(u16 *)(iwram + 0x8000 + (h << 1)))
            return 0;
          h++;
        }
      }
      found = 1;
    }
  }
  return found;
}
#endif
#endif

#ifdef SMC_PARTIAL_SAFE_FRAMEFULL
static u32 smc_partial_last_full_frame = ~0u;
#endif

#ifdef SMC_GATES
/* The ordinary gate is harvested from smc_last_write_addr, which is the
 * highest word written by an ARM STM.  SoundMainRAM's checked store writes
 * two words, so placing the boundary there still lets a translated block
 * span the first modified instruction.  The exact writer decoder above gives
 * us the complete range; for that proven layout, anchor the gate at the first
 * word instead.
 *
 * Keep the established high-word gate as well.  It is part of the existing
 * block/timing layout; the deterministic audio oracle detects removing it
 * even when both reference video frames remain identical.  The caller still
 * performs a full RAM-cache flush immediately after adding this one-time
 * boundary, so no block translated without it survives. */
static int smc_partial_install_range_gate(u32 low, u32 high)
{
  u32 i, found_high = 0, found_low = 0;
  int changed = 0;

  low &= ~3u;
  high = (high - 4) & ~3u;

  for (i = 0; i < translation_gate_targets; i++) {
    found_high |= translation_gate_target_pc[i] == high;
    found_low  |= translation_gate_target_pc[i] == low;
  }

  /* Preserve the original checked-word boundary first, then add the range
   * start.  Installing both in one event lets the mandatory full flush below
   * establish a single, deterministic block layout. */
  if (!found_high && translation_gate_targets < MAX_TRANSLATION_GATES) {
    translation_gate_target_pc[translation_gate_targets++] = high;
    SMC_GATE_MAP_ADD(high);
    changed = 1;
  }
#ifdef GATE_HIGH_ONLY
  found_low = 1;   /* DIAGNOSTIC: one-gate layout (checked word only) */
#endif
  if (!found_low && translation_gate_targets < MAX_TRANSLATION_GATES) {
    translation_gate_target_pc[translation_gate_targets++] = low;
    SMC_GATE_MAP_ADD(low);
    changed = 1;
  }

  return changed;
}
#endif

#elif defined(SMC_PARTIAL)
/* Was the store that raised this SMC a BLOCK COPY -- a multi-word store
 * that advances its base register through memory?
 *
 * Partial invalidation assumes every write into translated code is
 * reported.  Block stores break that: every word but the last goes through
 * execute_aligned_store32, which has no SMC check, and only the final word
 * reaches execute_store_u32.  A copy loop therefore overwrites code in
 * chunks, and a chunk whose final word lands on data is never reported at
 * all.  Upstream's full flush hides this -- any later event in the same copy
 * wipes everything -- but retiring single blocks leaves the unreported
 * chunks running their old translations.
 *
 * Pokemon Heart & Soul copies a routine into IWRAM with STMIA r0!, {r3-r10}.
 * SMC_GATES splits the routine at the reported addresses, so the unreported
 * words sit in blocks that are never retired; the routine runs a mix of new
 * and old code, computes a garbage length and fills the whole address space
 * (a white screen for ~12 s, then corrupted memory).  Either flag alone
 * boots the game.
 *
 * A copy loop moves its base (ARM STM with writeback, Thumb STMIA/PUSH);
 * those get the full flush, exactly as before partial invalidation existed.
 * An in-place patch does not -- Unbound's sound driver rewrites its own code
 * with STMIA lr, {r0,r1} -- and keeps the fast path unchanged.
 *
 * reg[REG_PC] was set by the smc_write stub from the store emitter's reg_a2:
 * the writer's PC + 4 (ARM) or + 2 (Thumb), for single and block stores
 * alike.  The opcode is fetched the way the translator fetches it. */
/* Did the instruction that triggered this SMC check write a whole register
 * list?  If so its earlier words bypassed the check entirely (see below), so
 * the partial flush is not safe and the caller must wipe the RAM cache. */
static int smc_writer_is_block_copy(void)
{
  u32 pc = reg[REG_PC];
  u32 op;
  u8 *blk;

  if (reg[REG_CPSR] & 0x20)
  {
    pc -= 2;
    blk = memory_map_read[pc >> 15];
    if (!blk)
      blk = load_gamepak_page((pc >> 15) & 0x3FF);
    op = readaddress16(blk, pc & 0x7FFF);
    return (op & 0xF800) == 0xC000 ||    /* STMIA rb!, {rlist} */
           (op & 0xFE00) == 0xB400;      /* PUSH {rlist[, lr]} */
  }

  pc -= 4;
  blk = memory_map_read[pc >> 15];
  if (!blk)
    blk = load_gamepak_page((pc >> 15) & 0x3FF);
  op = readaddress32(blk, pc & 0x7FFF);
  /* ANY ARM block store, writeback or not.
   *
   * 2.0.2 tested for writeback (W=1, mask 0x0E300000 / 0x08200000) because
   * the Heart & Soul boot hang was a register-list copy loop that used it.
   * That was too narrow and shipped the same bug in a second form: EVERY
   * non-final word of an ARM STM is emitted as execute_aligned_store32
   * (mips_emit.h arm_block_memory_store), which carries no SMC check at all,
   * regardless of writeback.  Only the final word reaches execute_store_u32
   * and lands here.  So `STMIA r0, {r1-r8}` over code answered "not a block
   * copy", took the partial flush around the last word alone, and left the
   * earlier words' stale translations live — executing freed code, which on
   * hardware is an unhandled exception and an instant power-off.
   *
   * Bits 27-25 == 100 selects block data transfer; L (bit 20) == 0 selects
   * store.  P/U/S/W are all irrelevant to whether the copy skipped the check.
   * Over-detecting only costs a full flush instead of a partial one, which is
   * slower but never wrong; under-detecting corrupts the cache. */
  return (op & 0x0E100000) == 0x08000000; /* any ARM STM, any cond */
}
#endif

/* THE OPTIMISATION THAT LOOKS OBVIOUS HERE AND IS WRONG.
 *
 * Full-flushing on every block copy costs real speed: battle entry and saving
 * in Heart & Soul hammer IWRAM, so the flushes land in bursts and the frame
 * pacer visibly over-corrects afterwards (59 -> 65 fps, the player character
 * speeds up, then it settles).  The tempting fix is that only the words THIS
 * copy wrote can hold stale translations, and the opcode names exactly how
 * many there were -- so retire just [last - (n-1)*4, last] and leave the rest
 * of the cache alone.  The geometry is right: both drivers in mips_emit.h
 * walk the register list ascending and mark the HIGHEST set register as the
 * final store, the only one that reaches execute_store_u32, so the address
 * recorded in smc_last_write_addr is the top of the copy.
 *
 * Implementing it as a LOOP over flush_translation_cache_ram_block() does not
 * work, and was tried on hardware 2026-09-11: Heart & Soul went back to a
 * white screen on boot, the exact pre-2.0.2 symptom.  That routine ZEROES the
 * block's tag run as its last act, so the first call blinds every later one --
 * they read tag 0, conclude "no code here", return early, and leave live
 * blocks that still cover the range holding stale translations.  This is the
 * v1/v4 failure already in the SMC notes ("retiring punches holes in runs
 * other blocks depend on"), rediscovered by not reading them.
 *
 * A range retire has to be ONE pass: walk back once from the low end, retire
 * through the high end, clear tags once at the finish.  Until that exists,
 * flush everything -- the speed is worth nothing if the game does not boot. */

#ifdef SMC_WRITE_HISTO
/* Where do the SMC flushes actually COME from?  A battle measured 10122 of
 * them and six of eight gate slots sat near sp, which looked like stack
 * traffic -- but clamping the scan at sp changed nothing, so the guess was
 * wrong.  Bucket both the WRITE address and the PC of the storing
 * instruction (mips_stub.S smc_write stores it to REG_PC before calling in)
 * so the next change is aimed at measured hot spots instead of a hunch. */
/* Word-granular view of the hot region.  256B buckets cannot tell "copies a
 * routine" from "updates one data word that happens to sit inside a block
 * scan_block tagged as code" -- and those want opposite fixes. */
/* WHICH block scan covers the hot data word?  The SMC check fires on any
 * non-zero tag, including the interior CODE_TAG_BLOCK16 that scan_block
 * writes for every address it scans -- so 0x0300168c need not be a block
 * START to cause flushes, it only has to fall inside some block's scanned
 * range.  Record the distinct blocks whose [start,end) spans it. */
#define SMC_COVER_ADDR 0x3001404u
#define SMC_COVER_MAX 24
static u32 smc_cov_s[SMC_COVER_MAX], smc_cov_e[SMC_COVER_MAX];
static u32 smc_cov_r[SMC_COVER_MAX], smc_cov_n[SMC_COVER_MAX];
static u32 smc_cov_used;
/* Does the hot word actually CHANGE?  SMC_SKIP_SAME (skip the flush when the
 * store writes an identical value) white-screened H&S, but its premise was
 * never tested on this game.  If the value is the same every time, all these
 * flushes are unnecessary and fixing that filter recovers everything; if it
 * genuinely changes, the whole avenue is dead.  The store has already landed
 * by the time we get here, so compare against what we saw last flush. */
static u32 smc_val_prev, smc_val_seen, smc_val_same, smc_val_diff;
void smc_cover_note(u32 s, u32 e, u32 reason)
{
  u32 k;
  if (!(s <= SMC_COVER_ADDR && SMC_COVER_ADDR < e)) return;
  for (k = 0; k < smc_cov_used; k++)
    if (smc_cov_s[k] == s && smc_cov_e[k] == e) { smc_cov_n[k]++; return; }
  if (smc_cov_used >= SMC_COVER_MAX) return;
  smc_cov_s[smc_cov_used] = s; smc_cov_e[smc_cov_used] = e;
  smc_cov_r[smc_cov_used] = reason; smc_cov_n[smc_cov_used] = 1;
  smc_cov_used++;
}

#define SMC_FINE_BASE 0x3001400u
#define SMC_FINE_WORDS 256                 /* covers 0x3001400..0x30017ff */
static u32 smc_h_fine_w[SMC_FINE_WORDS];   /* write address */
static u32 smc_h_fine_p[SMC_FINE_WORDS];   /* storing pc */
static u32 smc_h_addr_iw[128], smc_h_addr_ew[1024];
static u32 smc_h_pc_iw[128], smc_h_pc_other;
static u32 smc_h_n;
static void smc_histo_note(u32 addr, u32 storepc)
{
  if (addr >= 0x3000000) smc_h_addr_iw[(addr & 0x7FFF) >> 8]++;
  else                   smc_h_addr_ew[(addr & 0x3FFFF) >> 8]++;
  if (storepc >= 0x3000000 && storepc < 0x3008000)
    smc_h_pc_iw[(storepc & 0x7FFF) >> 8]++;
  else smc_h_pc_other++;
  if (addr - SMC_FINE_BASE < SMC_FINE_WORDS * 4u)
    smc_h_fine_w[(addr - SMC_FINE_BASE) >> 2]++;
  if (storepc - SMC_FINE_BASE < SMC_FINE_WORDS * 4u)
    smc_h_fine_p[(storepc - SMC_FINE_BASE) >> 2]++;
  if (addr == SMC_COVER_ADDR) {
    u32 cur = *(u32 *)(iwram + 0x8000 + (SMC_COVER_ADDR & 0x7FFF));
    if (smc_val_seen && cur == smc_val_prev) smc_val_same++; else smc_val_diff++;
    smc_val_prev = cur; smc_val_seen = 1;
  }
  if (++smc_h_n % 4000) return;
  {
    FILE *f = fopen("ms0:/smchisto.txt", "w");
    unsigned k;
    if (!f) return;
    /* Every counter here is a u32, which is `unsigned long` on this ABI while
     * %u/%x name `unsigned int`.  Same width on a PSP, undefined anywhere else;
     * cast rather than switch to %lu, which would be wrong on a target where
     * u32 is `unsigned int`. */
    fprintf(f, "smc flushes: %u\n", (unsigned)smc_h_n);
    fprintf(f, "unmappable block exits contained: %u\n",
            (unsigned)badjump_contained);
    fprintf(f, "-- WRITE addr, IWRAM 256B buckets --\n");
    for (k = 0; k < 128; k++) if (smc_h_addr_iw[k])
      fprintf(f, "  %08x %u\n", 0x3000000 + (k << 8),
              (unsigned)smc_h_addr_iw[k]);
    fprintf(f, "-- WRITE addr, EWRAM 256B buckets --\n");
    for (k = 0; k < 1024; k++) if (smc_h_addr_ew[k])
      fprintf(f, "  %08x %u\n", 0x2000000 + (k << 8),
              (unsigned)smc_h_addr_ew[k]);
    fprintf(f, "-- STORING pc, IWRAM 256B buckets --\n");
    for (k = 0; k < 128; k++) if (smc_h_pc_iw[k])
      fprintf(f, "  %08x %u\n", 0x3000000 + (k << 8),
              (unsigned)smc_h_pc_iw[k]);
    fprintf(f, "  (storing pc outside IWRAM: %u)\n", (unsigned)smc_h_pc_other);
    fprintf(f, "-- hot word %08x: same=%u changed=%u --\n",
            (unsigned)SMC_COVER_ADDR, (unsigned)smc_val_same,
            (unsigned)smc_val_diff);
    fprintf(f, "-- blocks whose scan COVERS %08x --\n",
            (unsigned)SMC_COVER_ADDR);
    for (k = 0; k < smc_cov_used; k++)
      fprintf(f, "  %08x..%08x  reason=%u  seen=%u\n",
              (unsigned)smc_cov_s[k], (unsigned)smc_cov_e[k],
              (unsigned)smc_cov_r[k], (unsigned)smc_cov_n[k]);
    fprintf(f, "-- FINE 0x%08x.. : word  writes  storing-pc --\n",
            (unsigned)SMC_FINE_BASE);
    for (k = 0; k < SMC_FINE_WORDS; k++)
      if (smc_h_fine_w[k] || smc_h_fine_p[k])
        fprintf(f, "  %08x  w=%-8u pc=%u\n",
                (unsigned)(SMC_FINE_BASE + (k << 2)),
                (unsigned)smc_h_fine_w[k], (unsigned)smc_h_fine_p[k]);
    fclose(f);
  }
}
#define SMC_HISTO_NOTE(a, p) smc_histo_note((a), (p))
#else
#define SMC_HISTO_NOTE(a, p) do { } while (0)
#endif

#ifdef SMC_CLASS_PROBE
/* DIAGNOSTIC ONLY, in no profile (bench campaign 2026-10-20).  Classify every
 * SMC trap by the shape of the storing instruction, ARM and Thumb alike, so
 * the safe-range fingerprint can be widened from measurement.  The 4.1.1
 * fingerprint admits only M4A's mixer patch (ARM STMIA via LR, no writeback);
 * the slow issue #9 titles flush 5-22 times a frame and nobody has looked at
 * what their writers are.  Dumped once, after SCP_DUMP_AT traps, to
 * ms0:/smcclass.txt -- the emulation thread must not do per-trap file I/O. */
enum { SCP_ARM_STR, SCP_ARM_STRH, SCP_ARM_STM_WB, SCP_ARM_STM_NOWB,
       SCP_ARM_OTHER, SCP_T_STR, SCP_T_STRH, SCP_T_STRB, SCP_T_STMIA,
       SCP_T_PUSH, SCP_T_OTHER, SCP_ADMITTED, SCP_N };
static const char *const scp_name[SCP_N] = {
  "arm_str/strb(single)", "arm_strh(single)", "arm_stm_writeback",
  "arm_stm_nowriteback", "arm_other", "thumb_str", "thumb_strh", "thumb_strb",
  "thumb_stmia", "thumb_push", "thumb_other", "(admitted_partial_range)" };
#define SCP_PCS 64
/* Rewritten at every power-of-two trap count from 256 on (smc-coverage
 * 2026-10-20: the original single dump at 30000 traps never fired on the
 * low-trap fixtures), so the last file on disk is at most 2x stale. */
static u32 scp_count[SCP_N], scp_traps, scp_next_dump = 256;
static u32 scp_pc[SCP_PCS], scp_pc_hits[SCP_PCS], scp_pc_cls[SCP_PCS];
static u32 scp_pc_lo[SCP_PCS], scp_pc_hi[SCP_PCS], scp_pc_op[SCP_PCS];
static u32 scp_pc_adm[SCP_PCS], scp_pc_thumb[SCP_PCS], scp_pc_lr[SCP_PCS];

static void scp_note(int admitted)
{
  u32 thumb = reg[REG_CPSR] & 0x20;
  u32 pc = GBA_PC(reg[REG_PC]) - (thumb ? 2 : 4);
  u32 addr = smc_last_write_addr, op, cls, i;
  u8 *blk = memory_map_read[pc >> 15];
  if (!blk)
    blk = load_gamepak_page((pc >> 15) & 0x3FF);
  if (thumb) {
    op = readaddress16(blk, pc & 0x7FFF);
    if ((op & 0xF800) == 0x6000 || (op & 0xFE00) == 0x5000 ||
        (op & 0xF800) == 0x9000)            cls = SCP_T_STR;
    else if ((op & 0xF800) == 0x8000 || (op & 0xFE00) == 0x5200)
                                            cls = SCP_T_STRH;
    else if ((op & 0xF800) == 0x7000 || (op & 0xFE00) == 0x5400)
                                            cls = SCP_T_STRB;
    else if ((op & 0xF800) == 0xC000)       cls = SCP_T_STMIA;
    else if ((op & 0xFE00) == 0xB400)       cls = SCP_T_PUSH;
    else                                    cls = SCP_T_OTHER;
  } else {
    op = readaddress32(blk, pc & 0x7FFF);
    if ((op & 0x0E000000) == 0x08000000)
      cls = (op & 0x00200000) ? SCP_ARM_STM_WB : SCP_ARM_STM_NOWB;
    else if ((op & 0x0C100000) == 0x04000000)  cls = SCP_ARM_STR;
    else if ((op & 0x0E1000F0) == 0x000000B0)  cls = SCP_ARM_STRH;
    else                                       cls = SCP_ARM_OTHER;
  }
  scp_count[cls]++;
  if (admitted)
    scp_count[SCP_ADMITTED]++;
  scp_traps++;
  for (i = 0; i < SCP_PCS; i++) {
    if (scp_pc_hits[i] && scp_pc[i] == pc && scp_pc_thumb[i] == thumb) {
      scp_pc_hits[i]++;
      scp_pc_adm[i] += admitted ? 1 : 0;
      if (addr < scp_pc_lo[i]) scp_pc_lo[i] = addr;
      if (addr > scp_pc_hi[i]) scp_pc_hi[i] = addr;
      break;
    }
    if (!scp_pc_hits[i]) {
      scp_pc[i] = pc; scp_pc_hits[i] = 1; scp_pc_cls[i] = cls;
      scp_pc_lo[i] = scp_pc_hi[i] = addr; scp_pc_op[i] = op;
      scp_pc_adm[i] = admitted ? 1 : 0; scp_pc_thumb[i] = thumb;
      scp_pc_lr[i] = reg[REG_LR];
      break;
    }
  }
  if (scp_traps >= scp_next_dump) {
    FILE *f = fopen("ms0:/smcclass.txt", "w");
    scp_next_dump <<= 1;
    if (!f)
      return;
    fprintf(f, "SMC traps classified: %u (frame %u) dma_flushes=%u\n",
            (unsigned)scp_traps, (unsigned)frame_counter,
#ifdef DMA_SMC_FLUSH
            (unsigned)dma_smc_flushes
#else
            0u
#endif
            );
    for (i = 0; i < SCP_N; i++)
      fprintf(f, "  %-26s %u\n", scp_name[i], (unsigned)scp_count[i]);
    fprintf(f, "\nwriter pcs (first %u distinct): pc T class opcode hits"
               " admitted target_lo..hi lr_at_first\n", SCP_PCS);
    for (i = 0; i < SCP_PCS && scp_pc_hits[i]; i++)
      fprintf(f, "  %08x %u %-22s %08x %7u %7u %08x..%08x %08x\n",
              (unsigned)scp_pc[i], (unsigned)(scp_pc_thumb[i] ? 1 : 0),
              scp_name[scp_pc_cls[i]], (unsigned)scp_pc_op[i],
              (unsigned)scp_pc_hits[i], (unsigned)scp_pc_adm[i],
              (unsigned)scp_pc_lo[i], (unsigned)scp_pc_hi[i],
              (unsigned)scp_pc_lr[i]);
    fclose(f);
  }
}
#define SCP_NOTE(adm) scp_note(adm)
#else
#define SCP_NOTE(adm) do { } while (0)
#endif

void flush_translation_cache_ram_smc(void)
{
  DRPH_SCOPE(DRPH_FLUSH);
  SMC_EVT("E f=%u pc=%08x t=%u a=%08x w=%08x\n", (unsigned)frame_counter,
          (unsigned)reg[REG_PC], (unsigned)((reg[REG_CPSR] >> 5) & 1),
          (unsigned)smc_last_write_addr,
          (unsigned)*(u32 *)(((smc_last_write_addr >> 24) == 3 ?
             iwram + 0x8000 + (smc_last_write_addr & 0x7FFC) :
             ewram + (smc_last_write_addr & 0x3FFFC))));
#ifdef SMC_EVTRACE
  {
    static u32 nreg;
    if (smc_evt_on() && nreg++ < 64) {
      u32 k;
      fprintf(smc_evt_f, "  regs");
      for (k = 0; k < 16; k++) fprintf(smc_evt_f, " %08x", (unsigned)reg[k]);
      fprintf(smc_evt_f, "\n");
      if (nreg == 1 || nreg == 8 || nreg == 20) {
        u32 a0 = (smc_last_write_addr & 0x7FFC) - 0x100;
        for (k = 0; k < 0x200; k += 4) {
          if (!(k & 31)) fprintf(smc_evt_f, "\n  %08x:", (unsigned)(0x03000000 + a0 + k));
          fprintf(smc_evt_f, " %08x", (unsigned)*(u32 *)(iwram + 0x8000 + a0 + k));
        }
        fprintf(smc_evt_f, "\n  tags:");
        for (k = 0; k < 0x200; k += 2) {
          if (!(k & 63)) fprintf(smc_evt_f, "\n  %08x:", (unsigned)(0x03000000 + a0 + k));
          fprintf(smc_evt_f, " %04x", (unsigned)*(u16 *)(iwram + a0 + k));
        }
        fprintf(smc_evt_f, "\n");
        if (nreg == 1 && getenv("SMC_EVT_EW")) {
          u32 e0 = strtoul(getenv("SMC_EVT_EW"), NULL, 16) & 0x3FFFC;
          for (k = 0; k < 0x400; k += 4) {
            if (!(k & 31)) fprintf(smc_evt_f, "\n  %08x:", (unsigned)(0x02000000 + e0 + k));
            fprintf(smc_evt_f, " %08x", (unsigned)*(u32 *)(ewram + e0 + k));
          }
          fprintf(smc_evt_f, "\n");
        }
      }
    }
  }
#endif
#ifdef SMC_PARTIAL_SAFE
  u32 partial_low = 0, partial_high = 0;
  int range_safe;
#ifdef SMC_PARTIAL_SINGLE
  smc_single_store = 0;
  smc_untag_now = 0;
#endif
  range_safe = smc_writer_safe_range(&partial_low, &partial_high);
#ifdef SMC_PARTIAL_SINGLE
  SMC_EVT("R rs=%d single=%d lo=%08x hi=%08x act=%d op=%08x\n", range_safe,
          smc_single_store, (unsigned)partial_low, (unsigned)partial_high,
          smc_partial_active,
          (unsigned)(((GBA_PC(reg[REG_PC]) >> 24) == 3 &&
                      !(reg[REG_CPSR] & 0x20)) ?
             *(u32 *)(iwram + 0x8000 + ((GBA_PC(reg[REG_PC]) - 4) & 0x7FFC)) :
             0));
#endif
  int range_gate_changed = 0;
#ifdef GATE_ONLY_FULLFLUSH
  /* DIAGNOSTIC: install the endpoint gates but never activate partial
   * retirement -- every SMC event is a full RAM flush. */
  int activating = 0;
#else
  int activating = range_safe && !smc_partial_active;
#endif
#endif
  DRPROF_ZONE_ENTER(DRZ_FLUSH);
  flush_ram_smc++;
#ifdef SMC_PARTIAL_SAFE
  SCP_NOTE(range_safe);
#else
  SCP_NOTE(0);
#endif
  SMC_HISTO_NOTE(smc_last_write_addr, reg[REG_PC]);
#ifdef UPDATE_TRACE_LO
  { void utrace_note(const char *, u32, u32);
    utrace_note("smc", reg[REG_PC], smc_last_write_addr); }
#endif
#ifdef SMC_BLOCK_EXACT
  if (smc_block_pending_full) {     /* see smc_block_word_hit */
    SMC_FB(7);
    flush_translation_cache_ram();
    DRPROF_ZONE_LEAVE(DRZ_FLUSH);
    return;
  }
#endif
#if defined(SMC_PARTIAL_SINGLE) && defined(SMC_PARTIAL_STABLE_THUNK)
  /* Single-word stores take their own path: no gate, exact retirement of the
   * covering blocks (smc_retire_single_word).  The first one activates
   * selective retirement exactly as the mixer path does. */
  if (range_safe && smc_single_store) {
    if (!smc_partial_active) {
      smc_partial_active = 1;
      flush_translation_cache_rom();
      SMC_FB(1);
      flush_translation_cache_ram();
    } else if (smc_retire_single_word(partial_low)) {
      SMC_FB(9);
    } else {
      SMC_FB(7);
      flush_translation_cache_ram();
    }
    DRPROF_ZONE_LEAVE(DRZ_FLUSH);
    return;
  }
#endif
#ifdef SMC_GATES
#ifdef SMC_PARTIAL_SAFE
  if (range_safe)
    range_gate_changed = smc_partial_install_range_gate(partial_low,
                                                        partial_high);
  else
#endif
    smc_add_gate(smc_last_write_addr);
#endif
#ifdef SMC_PARTIAL_SAFE
  {
    if (activating) {
      /* ROM translations survive a RAM flush and may contain direct links
       * into RAM. Rebuild them once under the active indirect-link policy
       * before any selective retirement can occur. */
      smc_partial_active = 1;
      flush_translation_cache_rom();
    }
    /* The event that installs or moves the range-start gate gets a full
     * flush.  Subsequent events can retire selectively only after every live
     * block has therefore been scanned with that exact boundary. */
    int partial_ready = smc_partial_active && range_safe && !activating &&
                        !range_gate_changed &&
                        smc_partial_target_is_gated(partial_low & ~3u) &&
                        /* Retirement also requires the final written word to
                         * begin at its established block boundary.  If the
                         * gate table filled after low was learned but before
                         * this range's high endpoint was added, low alone is
                         * not enough to prove the block layout. */
                        smc_partial_target_is_gated((partial_high - 4) & ~3u);
#ifdef SMC_PARTIAL_SAFE_FRAMEFULL
    if (partial_ready && smc_partial_last_full_frame != frame_counter) {
      smc_partial_last_full_frame = frame_counter;
      flush_translation_cache_ram();
    } else
#endif
    if (partial_ready) {
      SMC_FB(9);
      flush_translation_cache_ram_range(partial_low, partial_high);
    } else {
      if (!range_safe) SMC_FB(0);
      else if (activating) SMC_FB(1);
      else if (range_gate_changed) SMC_FB(2);
      else SMC_FB(3);
      flush_translation_cache_ram();
    }
  }
#elif defined(SMC_PARTIAL)
  if (smc_writer_is_block_copy())
    flush_translation_cache_ram();
  else
    flush_translation_cache_ram_block(smc_last_write_addr);
#else
  flush_translation_cache_ram();
#endif
  DRPROF_ZONE_LEAVE(DRZ_FLUSH);
}

#ifdef SMC_STM_RANGE
#if !defined(SMC_PARTIAL_SINGLE) || !defined(SMC_PARTIAL_STABLE_THUNK) || \
    defined(SMC_BLOCK_EXACT)
#error SMC_STM_RANGE needs SMC_PARTIAL_SINGLE + SMC_PARTIAL_STABLE_THUNK, without SMC_BLOCK_EXACT
#endif
#if defined(SMC_STM_WIDE) && !defined(SMC_STM_RANGE_MAX)
#define SMC_STM_RANGE_MAX 16
#endif
#ifndef SMC_STM_RANGE_MAX
#define SMC_STM_RANGE_MAX 4
#endif
/* SMC_STM_RANGE (cpu-levers 2026-10-27): the smc_write stub's entry (fnptrs
 * slot 10) when the switch is on.  A short increment-after writeback STM --
 * STMIA Rn!, 2..SMC_STM_RANGE_MAX registers, from IWRAM into IWRAM -- retires
 * exactly its own write instead of taking the full RAM flush.
 *
 * Measured on the twin: FIFA 07's sound-code generator writes its routine
 * with STMIA r12!,{r0,r7,r9} and STMIA r12!,{r4,r7}: 4090 traps in 1500 f,
 * every one a full RAM flush ("writer not range-safe"), and retranslation was
 * 63 % of FIFA's core work.
 *
 * Exactness: the trap is the STM's FINAL word (only that word is checked),
 * and the instruction wrote [final - 4*(n-1), final + 4) in full, known from
 * the opcode.  smc_retire_span's contract -- the range is the WHOLE write --
 * therefore holds for this store as it does for a single STR's word.  The
 * block-store hole for EARLIER stores (an STM whose final word lands on
 * untagged memory raises no trap) is unchanged by this switch.
 *
 * Everything else -- Thumb, other STM shapes, longer copies (Hot Wheels'
 * STMIA r0!,{r4-r11}: one flush is cheaper than retiring a copy block by
 * block), selective retirement not yet active, the first SMC_SINGLE_HOT
 * widened traps, an undecidable span -- goes to flush_translation_cache_ram_smc
 * unchanged.
 *
 * LAYOUT NEUTRALITY: this sits outside the pinned hot order, and
 * flush_translation_cache_ram_smc is byte-identical with the switch on or
 * off; only the stub's function-pointer slot changes. */
#if defined(SMC_STM_FIXED) && !defined(SMC_STM_RANGE)
#error SMC_STM_FIXED needs SMC_STM_RANGE
#endif
/* SMC_STM_FIXED (perf/tmgc 2026-11-02): the same span retirement for the
 * fixed-base shape, STMIA Rn without writeback (P0 U1 S0 W0 L0).
 *
 * Measured on the twin (4.5.0 core defines): ipatix's "ImprovedSoundMixer"
 * (The Morrow's Golden Country, and the other FE/Pokemon hacks that ship it)
 * pastes its resampling inner loop into IWRAM whenever a channel's pitch
 * ratio picks a different routine than the one in place --
 * STMIA LR,{R0,R1} / STMIA LR,{R2,R8-R10}, eight stores, LR moved by
 * ADD/SUB between them.  Every one was "writer not range-safe", a full RAM
 * flush: ~4 a frame on the title screen, and translate + flush was 87 % of
 * the core's instructions there (5.5 M a frame against base FE8's 0.27 M).
 *
 * Exactness: identical to the writeback shape.  Writeback changes Rn AFTER
 * the store, not which words the store wrote: STMIA Rn,{n regs} writes
 * [Rn, Rn + 4n), and its final (trapping) word is Rn + 4(n-1), so
 * [final - 4(n-1), final + 4) is again the whole write.  SMC_STM_CHAIN
 * carries a changed earlier word to that trap exactly as it does for
 * STMIA Rn!.  The forward widening (SMC_STM_AHEAD) is for an ADVANCING copy
 * and is not applied: a fixed-base patcher does not walk on into what
 * follows, so widening would only retire code nobody is about to write. */
#ifdef SMC_STM_AHEAD
#ifndef SMC_STM_WIDE
#error SMC_STM_AHEAD needs SMC_STM_WIDE
#endif
#ifndef SMC_STM_AHEAD_BYTES
#define SMC_STM_AHEAD_BYTES 1024
#endif
#ifndef SMC_STM_AHEAD_BLOCKS
#define SMC_STM_AHEAD_BLOCKS 8
#endif
/* SMC_STM_AHEAD (cpu-levers 2026-10-27, twin prototype): an advancing copy
 * (STMIA Rn!) that traps on live code is about to overwrite what follows it.
 * Exact retirement of just its own span left every later STM of the copy to
 * trap again (Hot Wheels: 774 full flushes became 58k selective traps, ~48 a
 * frame).  So the span retired is widened FORWARD over the run of tagged
 * words that continues from the write's end, bounded by SMC_STM_AHEAD_BYTES
 * and by SMC_STM_AHEAD_BLOCKS block starts; the first untagged word ends it.
 *
 * Safety: smc_retire_span retires every live block overlapping the range, so
 * widening it retires MORE code, never less; the written span is still inside
 * the range.  Its interior-mark clear over the widened part removes only
 * marks of blocks it has just retired (every covering block of the range is
 * retired).  The cost of a too-wide guess is retranslation, not correctness.
 * hi is canonical IWRAM (0x03000000 + off) here. */
static u32 smc_stm_ahead(u32 hi)
{
  const u16 *tagp = (const u16 *)iwram;
  u32 off = hi - 0x03000000, lim = off + SMC_STM_AHEAD_BYTES, starts = 0;
  if (off >= 0x8000)
    return hi;
  if (lim > 0x8000)
    lim = 0x8000;
  while (off < lim) {
    u32 t0 = tagp[off >> 1], t1 = tagp[(off >> 1) + 1];
    if (!t0 && !t1)
      break;
    if ((VALID_TAG(t0) || VALID_TAG(t1)) && ++starts > SMC_STM_AHEAD_BLOCKS)
      break;
    off += 4;
  }
  return 0x03000000 + off;
}
u32 smc_stm_ahead_bytes;
#endif
void smc_trap_entry(void)
{
#if defined(XLAT_CENSUS) && defined(DRPROF_TWIN)
  { extern u32 census_cause; census_cause = 2; }
#endif
#ifdef SMC_STM_CHAIN
  smc_chain_pending = 0;           /* the chain (if any) ends in this trap */
#endif
#ifdef SMC_STM_WIDE
  /* SMC_STM_WIDE: the shape below may also ACTIVATE selective retirement
   * (Hot Wheels never raises any other range-safe event, so it stayed on the
   * full flush for good).  Activation is the single-store path's: ROM flush
   * once (direct links into RAM), then a full RAM flush. */
  if (!(reg[REG_CPSR] & 0x20)) {
#else
  if (smc_partial_active && !(reg[REG_CPSR] & 0x20)) {
#endif
    u32 pc = GBA_PC(reg[REG_PC]) - 4;          /* as smc_writer_safe_range */
    u32 addr = smc_last_write_addr;
    u8 *blk = memory_map_read[pc >> 15];
    if (blk && (pc >> 24) == 3 && (addr >> 24) == 3) {
      u32 op = readaddress32(blk, pc & 0x7FFF);
      u32 n = (u32)__builtin_popcount(op & 0xFFFF);
      u32 hi = (addr & ~3u) + 4, lo = hi - 4 * n;
#ifdef SMC_STM_RANGE_MUTANT_LASTWORD
      lo = hi - 4;      /* MUTANT (oracle self-test only): final word only */
#endif
#ifdef SMC_STM_FIXED
      /* STMIA Rn (W0): fixed base, no forward widening (see above) */
      int fixed = (op & 0x0FF00000) == 0x08800000;
#else
      const int fixed = 0;
#endif
      if (((op & 0x0FF00000) == 0x08A00000 || fixed) && /* STMIA Rn! (P0 U1 S0 W1 L0) */
          n >= 2 && n <= SMC_STM_RANGE_MAX &&
          (lo >> 15) == ((hi - 4) >> 15) &&    /* one 32 KiB IWRAM mirror */
          smc_wide_gate()) {
        DRPH_SCOPE(DRPH_FLUSH);
        DRPROF_ZONE_ENTER(DRZ_FLUSH);
        flush_ram_smc++;
        smc_single_store = 1;
        smc_untag_now = 1;
#ifdef SMC_STM_WIDE
        if (!smc_partial_active) {
          smc_partial_active = 1;
          flush_translation_cache_rom();
          SMC_FB(1);
          flush_translation_cache_ram();
        } else
#endif
#ifdef SMC_STM_AHEAD
        if (!fixed) {
          u32 hx = smc_stm_ahead(hi);
          smc_stm_ahead_bytes += hx - hi;
#ifdef SMC_STM_AHEAD_MUTANT
          lo = hi - 4;  /* MUTANT (oracle self-test): earlier words dropped */
#endif
          hi = hx;
        }
#endif
        if (smc_retire_span(lo, hi)) {
          SMC_FB(9);
        } else {
          SMC_FB(7);
          flush_translation_cache_ram();
        }
        DRPROF_ZONE_LEAVE(DRZ_FLUSH);
        return;
      }
    }
  }
  flush_translation_cache_ram_smc();
}
#endif

#if defined(DMA_ADAPT_V2) && !defined(DMA_SMC_ADAPT)
#error DMA_ADAPT_V2 needs DMA_SMC_ADAPT
#endif
#ifdef DMA_SMC_RETIRE
#ifndef SMC_STM_RANGE
#error DMA_SMC_RETIRE needs SMC_STM_RANGE (smc_retire_span + stable thunks)
#endif
/* DMA_SMC_RETIRE (cpu-levers 2026-10-27, twin prototype): dma_write_iwram /
 * dma_write_ewram record the lowest and highest TAGGED word every transfer
 * since the last consumption wrote (gba_memory.c; only on a tag hit, so an
 * ordinary DMA pays nothing).  Every instruction word of a live block is
 * tagged (scan_block), so [lo, hi) covers every live code word the DMA
 * changed: smc_retire_span's contract.  One region only; anything else, or
 * selective retirement not active, takes the full flush as before.
 *
 * Measured on the twin: Top Gear Rally DMA3s code overlays from ROM into
 * 030075d8..03007cff 3.2 times a frame, each a full RAM flush, and 31 % of the
 * resulting retranslation is unrelated low-IWRAM code. */
u32 dma_smc_lo[2] = { ~0u, ~0u }, dma_smc_hi[2];
u32 dma_smc_retired;
#ifdef DMA_SMC_ADAPT
/* DMA_SMC_ADAPT: exact DMA retirement only while it pays.
 *
 * Exact retirement keeps every other translation live, so later DMAs and
 * stores in the same frame land on tagged code and alert again where a full
 * flush had left them nothing to hit.  Twin, 1200 f: Mario Golf -8.0 % and
 * FIFA 07 -4.3 % core (few DMA alerts, much collateral saved), Top Gear Rally
 * +9.7 % (DMA alerts 3797 -> 10009, store traps 9.1k -> 21.5k, xlat only
 * -5 %).  Which way a game goes is a property of its code, so it is MEASURED,
 * not listed:
 *
 *   cost of a window, per frame, in units of one translated guest halfword:
 *     halfwords translated (RAM)  x 1    (~150 host instr each)
 *     tag slots walked by retire  x 1/8  (~24 host instr each)
 *     full RAM flushes            x 128  (~17k host instr each)
 *   Prices measured on the twin (TGR, Mario Golf, FIFA 07; hb0 vs exact DMA
 *   retire, smcwork counters).  A first model that counted SMC events at a
 *   flat price misjudged TGR, whose DMA retires walk ~12k slots a frame.
 *
 * The controller starts in the full-flush mode (the established behaviour)
 * and must find exact retirement cheaper by the margin below to keep it.
 * Windows are DMA_ADAPT_WINDOW frames, closed at the next DMA alert (nothing
 * here matters to a game that raises none).  After `gap` windows in the
 * current mode, one window is spent in the other mode (a probe).  The probe
 * mode is kept when it costs less than 7/8 of the window before it; then gap
 * resets to DMA_ADAPT_GAP0.  Otherwise the mode reverts and gap doubles, up to
 * DMA_ADAPT_GAP0 << 5, so a game whose answer is settled probes about once
 * per 4096 frames.  Deterministic: frame_counter and event counts only.
 * Every decision is logged (EVT dma_retire_mode on the PSP harness builds,
 * SMC_EVT on the twin) so a hardware run shows each game's mode. */
#ifndef DMA_ADAPT_WINDOW
#define DMA_ADAPT_WINDOW 64u
#endif
#define DMA_ADAPT_GAP0 2u
extern u32 smc_work_walk, smc_work_halves;
u32 dma_rt_mode = 0, dma_rt_switches, dma_rt_probes;
static u32 drt_f0, drt_x0, drt_ref, drt_since, drt_probing;
static u32 drt_gap = DMA_ADAPT_GAP0;
extern void fe_evt(const char *fmt, ...) __attribute__((weak));
#ifdef DMA_ADAPT_V2
/* DMA_ADAPT_V2 (tgr-regression 2026-10-28): a cost model fitted to what the
 * work costs, and ties go to the full flush.
 *
 * WHY.  Hardware A/B lab-xvr2 vs lab-f1c2: Top Gear Rally +5 % on both
 * consoles; the twin over the whole 3930-frame fixture agrees (+4.0 %
 * instructions; the 1200-frame census before saw only the heavy part).
 * The controller made two mistakes there:
 *   1. f=5574 the scene lightens 4x DURING a retire probe (probe 1951 vs
 *      the heavy-scene reference 7509), so retire was kept;
 *   2. it then stayed in retire: every later flush probe measured a tie
 *      under the old prices (2528 vs 2430, 2482 vs 2425, 2355 vs 2311), and
 *      a tie kept the current mode.  The twin's instruction counts for those
 *      windows say retire costs +10..13 % of ALL core work.
 * The old prices (halfword x1 ~150 instr, walk slot x1/8, full flush x128)
 * explain the twin's paired windows worse than a constant (R^2 -1.9 on the
 * 546 windows below) and get the SIGN wrong for TGR (always-retire: model
 * -322M instructions, twin +891M over 4000 frames).  A translated halfword
 * costs ~100 instructions, not 150; every translated BLOCK has a fixed cost
 * (~1.4k: lookup, entry-gate harvest, publish, notes) and every retired
 * block another (~1.9k: stable thunk, untag) that the halfword count misses
 * -- retire mode translates more, smaller blocks.
 *
 * WHAT.  cost/frame = halves*P_HALF + blocks*P_BLOCK + walk*P_WALK +
 * full flushes*P_FULL + retired blocks*P_RETIRED, in twin instructions.
 * Prices: least squares over 546 paired 64-frame windows (flush-only vs
 * always-retire vs adaptive twins of TGR, Mario Golf, FIFA 07, Mario Tennis,
 * Hot Wheels; SMC_WORK_WINDOWS), R^2 0.974 against the twin's core
 * instruction counts; per-title totals within ~15 %.  The decision rule is
 * unchanged (a probe must be 1/8 cheaper than the window before it to be
 * kept), so the incumbent wins ties.  That bias is deliberate: the first
 * window after a switch pays the switch itself (a retire probe starts on
 * code a flush just discarded), which the model cannot separate.  Measured:
 * with ties going to the flush instead (DMA_ADAPT_TIE_FLUSH, kept as a
 * sub-switch) Mario Golf left retire at f=1517 on a 7 % flush probe and
 * never came back (-3.6 % vs -5.0 %); TGR gains nothing from it, because
 * with these prices its f=5766 flush probe is 18 % cheaper and reverts.
 * Mistake 1 then costs only the gap until that probe (DMA_ADAPT_GAP0
 * windows).  Deterministic: event counts and frame_counter only. */
#ifndef DA2_P_HALF
#define DA2_P_HALF    100u   /* guest halfword translated (RAM) */
#endif
#ifndef DA2_P_BLOCK
#define DA2_P_BLOCK  1360u   /* RAM block translated: fixed part */
#endif
#ifndef DA2_P_WALK
#define DA2_P_WALK     20u   /* tag slot walked by smc_retire_span */
#endif
#ifndef DA2_P_FULL
#define DA2_P_FULL  23000u   /* full RAM flush */
#endif
#ifndef DA2_P_RETIRED
#define DA2_P_RETIRED 1920u  /* block retired selectively (thunk, untag) */
#endif
extern u32 smc_work_blocks;
static u32 drt_s[5];
static u32 da2_cost(u32 frames, int reset)
{
  u32 c[5], i;
  unsigned long long w;
  c[0] = smc_work_halves; c[1] = smc_work_blocks;
  c[2] = smc_work_walk;   c[3] = flush_ram_total; c[4] = flush_ram_partial;
  w = (unsigned long long)(c[0] - drt_s[0]) * DA2_P_HALF +
      (unsigned long long)(c[1] - drt_s[1]) * DA2_P_BLOCK +
      (unsigned long long)(c[2] - drt_s[2]) * DA2_P_WALK +
      (unsigned long long)(c[3] - drt_s[3]) * DA2_P_FULL +
      (unsigned long long)(c[4] - drt_s[4]) * DA2_P_RETIRED;
  if (reset)
    for (i = 0; i < 5; i++)
      drt_s[i] = c[i];
  w /= frames ? frames : 1;
  return w > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)w;
}
#endif
static void dma_adapt_tick(void)
{
  u32 f = frame_counter, frames, cost;
#ifndef DMA_ADAPT_V2
  u32 w = smc_work_halves + (smc_work_walk >> 3) + (flush_ram_total << 7);
#endif
  if (f < drt_f0 || drt_f0 == 0) {          /* first alert, or a state load */
    drt_f0 = f ? f : 1;
#ifdef DMA_ADAPT_V2
    da2_cost(1, 1);
#else
    drt_x0 = w;
#endif
    return;
  }
  frames = f - drt_f0;
  if (frames < DMA_ADAPT_WINDOW)
    return;
#ifdef DMA_ADAPT_V2
  cost = da2_cost(frames, 1);
#else
  cost = (w - drt_x0) / frames;
#endif
  if (drt_probing) {
#if defined(DMA_ADAPT_V2) && defined(DMA_ADAPT_TIE_FLUSH)
    /* the probe ran in dma_rt_mode; drt_ref is the other mode's window */
    u32 rc = dma_rt_mode ? cost : drt_ref, fc = dma_rt_mode ? drt_ref : cost;
    int want_retire = (unsigned long long)rc * 8 < (unsigned long long)fc * 7;
    int keep = (u32)want_retire == dma_rt_mode;
#else
    int keep = cost * 8 < drt_ref * 7;
#endif
    dma_rt_probes++;
    if (keep) {
      dma_rt_switches++;
      drt_gap = DMA_ADAPT_GAP0;
    } else {
      dma_rt_mode ^= 1;
      if (drt_gap < (DMA_ADAPT_GAP0 << 5))
        drt_gap <<= 1;
    }
    SMC_EVT("  DA f=%u probe %s mode=%u cost=%u ref=%u gap=%u\n", (unsigned)f,
            keep ? "keep" : "revert", (unsigned)dma_rt_mode, (unsigned)cost,
            (unsigned)drt_ref, (unsigned)drt_gap);
    if (fe_evt)
      fe_evt("dma_retire_mode f=%u probe=%s mode=%s cost=%u ref=%u gap=%u",
             (unsigned)f, keep ? "keep" : "revert",
             dma_rt_mode ? "retire" : "flush", (unsigned)cost,
             (unsigned)drt_ref, (unsigned)drt_gap);
    drt_probing = 0;
    drt_since = 0;
  } else {
    drt_ref = cost;
    if (++drt_since >= drt_gap) {
      drt_probing = 1;
      dma_rt_mode ^= 1;
    }
  }
  drt_f0 = f;
#ifndef DMA_ADAPT_V2
  drt_x0 = w;
#endif
}
#endif
static int dma_smc_retire(void)
{
  int ok = 0;
  u32 r = (dma_smc_hi[0] != 0) ? 0 : 1;
#ifdef DMA_SMC_ADAPT
  if (smc_partial_active)
    dma_adapt_tick();
  if (smc_partial_active && dma_rt_mode && dma_smc_hi[r] &&
      !dma_smc_hi[r ^ 1]) {
#else
  if (smc_partial_active && dma_smc_hi[r] && !dma_smc_hi[r ^ 1]) {
#endif
    SMC_EVT("  D retire %08x..%08x\n", (unsigned)dma_smc_lo[r],
            (unsigned)dma_smc_hi[r]);
    smc_single_store = 1;
    smc_untag_now = 1;
    ok = smc_retire_span(dma_smc_lo[r], dma_smc_hi[r]);
  }
  dma_smc_lo[0] = dma_smc_lo[1] = ~0u;
  dma_smc_hi[0] = dma_smc_hi[1] = 0;
  return ok;
}
#endif

void flush_translation_cache_ram_dma(void)
{
  flush_ram_dma++;
#if defined(XLAT_CENSUS) && defined(DRPROF_TWIN)
  { void census_dma_alert(void); census_dma_alert(); }
#endif
#ifdef DMA_SMC_RETIRE
  DRPROF_ZONE_ENTER(DRZ_FLUSH);
  if (dma_smc_retire()) {
    dma_smc_retired++;
    DRPROF_ZONE_LEAVE(DRZ_FLUSH);
    return;
  }
  DRPROF_ZONE_LEAVE(DRZ_FLUSH);
#endif
  flush_translation_cache_ram();
}

void flush_translation_cache_rom(void)
{
  DRPH_SCOPE(DRPH_FLUSH);
  /* We flush the generated code except for everything below the watermark. */
  DRPROF_ZONE_ENTER(DRZ_FLUSH);
  flush_rom_total++;
#ifdef JIT_CODE_DISCIPLINE
  /* The ROM zone's addresses are about to hold a new generation of code
   * (state load, ROM load, cheats, a full cache, SMC partial activation). */
  jit_code_owner_changed(JOWN_ROM_FLUSH);
#endif
  last_rom_translation_ptr = &rom_translation_cache[rom_cache_watermark];
  rom_translation_ptr      = &rom_translation_cache[rom_cache_watermark];

  memset(rom_branch_hash, 0, sizeof(rom_branch_hash));
#ifdef VRAM_EXEC
  vram_code_forget();
#endif
#ifdef DISPATCH_CACHE
  dispatch_cache_clear();
#endif
#ifdef DRPROF_TWIN
  drprof_forget_rom(rom_translation_ptr);
#endif
  DRPROF_ZONE_LEAVE(DRZ_FLUSH);
}

#ifdef REWIND
/* Rewind measurement only (docs/REWIND.md): bytes of translated code in each
 * cache right now, so the frontend can say how much a restore made the
 * translator emit again. */
void gpsp_rewind_jit_used(u32 *rom, u32 *ram);
void gpsp_rewind_jit_used(u32 *rom, u32 *ram)
{
  *rom = (u32)(rom_translation_ptr - rom_translation_cache);
  *ram = (u32)(ram_translation_ptr - ram_translation_cache);
}
#endif

#ifdef XS_HEAP
/* One zeroed block for the SMC_SAME_SKIP / SMC_RETIRE_LOCAL tables, taken
 * once, before init_gamepak_buffer's 1 MiB loop (libretro.c retro_init);
 * init_dynarec_caches calls it too, as a no-op after the first.
 *
 * The pointer is file-scope so dynarec_heap_tables_release() can give the
 * block back.  Instance A keeps it across its own reloads (a GBA link
 * session's) and gives it back only in the in-process return's unload
 * (psp/main_psp.c), so the next game's retro_init finds a fresh boot's heap
 * and the heap census matches.  Instance B's .bss (gbab_xs_heap_blk
 * among it) is zeroed and lent to A's ROM cache between GBA link sessions
 * (fe_gbadual.c b_fresh / fe_gbadual_lend), so B MUST release the block
 * before it is unloaded: otherwise each session start finds a NULL pointer,
 * takes a fresh 70,656 B block and the previous one is orphaned -- the
 * 70,672 B/cycle sessmem_cycles leak on the PSP-1000 (docs/GBA-LINK-MIRROR.md
 * 19). */
static u8 *xs_heap_blk;
void dynarec_heap_tables_release(void)
{
  free(xs_heap_blk);
  xs_heap_blk = NULL;
#if defined(SMC_SAME_SKIP) && defined(SMC_PARTIAL_SINGLE)
  xs_val = NULL;
  xs_ok = xs_bad = NULL;
  xs_min = NULL;
#ifdef SMC_SAME_ADAPT
  xs_rec = NULL;
#endif
#endif
#if defined(SMC_RETIRE_LOCAL) && defined(SMC_RETIRE_WINDOW)
  smc_back_iw = smc_back_ew = NULL;
#endif
}
void dynarec_heap_tables_init(void)
{
  u8 *blk;
  u32 sz = 0;
  if (xs_heap_blk)
    return;
#if defined(SMC_SAME_SKIP) && defined(SMC_PARTIAL_SINGLE)
  sz += 0x8000 + 2 * (0x8000 / 16) + 0x8000 / 2;
#ifdef SMC_SAME_ADAPT
  sz += 2 * (0x10000 / 16);
#endif
#endif
#if defined(SMC_RETIRE_LOCAL) && defined(SMC_RETIRE_WINDOW)
  sz += (0x8000 >> SMC_LOCAL_SHIFT) * 2 + (0x40000 >> SMC_LOCAL_SHIFT) * 2;
#endif
  blk = malloc(sz ? sz : 64);
  if (!blk) {
    fprintf(stderr, "dynarec_heap_tables_init: %u bytes refused\n", (unsigned)sz);
    abort();
  }
  xs_heap_blk = blk;
  memset(blk, 0, sz ? sz : 64);
  {
    u8 *p = blk;
#if defined(SMC_SAME_SKIP) && defined(SMC_PARTIAL_SINGLE)
    xs_val = (u16 *)p; p += 0x8000;
    xs_ok = p;         p += 0x8000 / 16;
    xs_bad = p;        p += 0x8000 / 16;
    xs_min = (u16 *)p; p += 0x8000 / 2;
#ifdef SMC_SAME_ADAPT
    xs_rec = (u8 (*)[0x10000 / 16])p; p += 2 * (0x10000 / 16);
#endif
#endif
#if defined(SMC_RETIRE_LOCAL) && defined(SMC_RETIRE_WINDOW)
    smc_back_iw = (u16 *)p; p += (0x8000 >> SMC_LOCAL_SHIFT) * 2;
    smc_back_ew = (u16 *)p; p += (0x40000 >> SMC_LOCAL_SHIFT) * 2;
#endif
    (void)p;
  }
}
#endif
void init_dynarec_caches(void)
{
#ifdef XS_HEAP
  dynarec_heap_tables_init();
#endif
  /* Initialize caches so that we can start initalizing the emitter. */
#ifdef JIT_CODE_DISCIPLINE
  jit_code_owner_changed(JOWN_INIT);
#endif
  rom_translation_ptr = last_rom_translation_ptr = &rom_translation_cache[0];
  memset(rom_branch_hash, 0, sizeof(rom_branch_hash));
#ifdef VRAM_EXEC
  vram_code_forget();
#endif

  ram_translation_ptr = last_ram_translation_ptr = &ram_translation_cache[0];
#ifdef SMC_PARTIAL_SAFE
  smc_partial_active = 0;
#endif
#ifdef SMC_PARTIAL_SINGLE
  smc_wide_traps = 0;
#endif
#ifdef SMC_RETIRE_WINDOW
  smc_window_armed = 0;
#endif
#ifdef DISPATCH_CACHE
  dispatch_cache_clear();   /* bss zero would be key 0 -> entry NULL */
#endif
  memset(iwram, 0, 0x8000);
  memset(&ewram[0x40000], 0, 0x40000);

  ewram_code_min = 0;
  ewram_code_max = 0x40000;
  iwram_code_min = 0;
  iwram_code_max = 0x8000;
  /* NO smc_gates_reset() HERE.  See flush_dynarec_caches below: a reset keeps
   * the same ROM, and ROM load already resets the table. */
}

#if defined(RUNTIME_JIT_CACHE)
/* ---- startup-sized translation caches -----------------------------------
 *
 * ONE EBOOT, TWO BUDGETS.  A PSP-1000 has 32 MiB and runs at its limit: the
 * ROM page cache (init_gamepak_buffer) takes 1 MiB blocks until malloc fails,
 * and the frontend lives on what is left.  A 2000/3000/Go running the 64 MiB
 * layout stops at the ROM_BUFFER_SIZE cap with ~30 MiB unused -- the memory
 * that CFRU-class hacks (Unbound) need for a translation cache that does not
 * flush-storm.  So: the SMALL tier is static (.bss, exactly where the fixed
 * arrays always were), and the LARGE tier is one heap block taken here, in
 * retro_init, BEFORE init_gamepak_buffer, only when the heap can afford it.
 *
 * WHAT DECIDES IT: the heap, not the model number.  The quantity at stake is
 * the memory this process was actually given, and the model is only a proxy
 * for it -- a 2000 whose launcher did not grant MEMSIZE, or any 64 MiB
 * console running a 32 MiB layout, reports a big-memory model and has a small
 * heap.  So the test is one malloc of the large tier PLUS everything that
 * must still fit after it (JIT_LARGE_HEADROOM).  A PSP-1000's whole user
 * partition is ~24 MiB, below the ~31 MiB asked for, so the 1000 can never
 * pass it, whatever the model register says and however the binary grows.
 * (sceKernelGetModel is a kernel export; kuKernelGetModel needs a CFW bridge
 * PRX.  Neither is worth a boot-time import that can fail to resolve.)
 *
 * ON THE SMALL PATH THE HEAP IS UNTOUCHED: the only thing that happens is a
 * malloc that fails, which moves no break and allocates nothing.  The static
 * arrays are where they were, so the 1000's post-load budget is what it was.
 *
 * ON THE LARGE PATH the static SMALL arrays are simply left unused (2.4 MiB
 * of a 64 MiB console's ~30 MiB spare).  The block is never freed: it lives
 * for the process, like the static arrays it replaces, so retro_deinit /
 * retro_init cycles keep the first decision. */
u32 jit_cache_tier = JIT_CACHE_UNDECIDED;
u32 jit_cache_force_small = 0;
/* A frontend reloading the SAME game in the same process (GBA-LINK-MIRROR.md
 * 19, a link session's start and end) sets this: the tier is the one the
 * last load chose, without the probe.  The probe asks for one contiguous
 * block far larger than the tier; with the frontend's own allocations from
 * the first load still standing it can fail where the boot passed, and a
 * 64 MiB console would come back from a session on the SMALL tier. */
u32 jit_cache_replay = 0;
static u32 jit_cache_last_tier = JIT_CACHE_UNDECIDED;
const char *jit_cache_reason = "undecided";

/* What the heap must STILL hold once the large tier is taken, or the large
 * tier costs more than it buys: the full ROM page cache (ROM_BUFFER_SIZE
 * blocks of 1 MiB), the 1 MiB post-load reserve init_gamepak_buffer holds
 * across its loop, and 4 MiB for what the frontend allocates after the load
 * (frame buffer, sound ring, ME stages -- each a few hundred KiB). */
#define JIT_LARGE_HEADROOM  ((u32)(ROM_BUFFER_SIZE + 1 + 4) * 1024u * 1024u)
#define JIT_LARGE_BYTES     ((u32)(ROM_TRANSLATION_CACHE_SIZE_LARGE + \
                                   RAM_TRANSLATION_CACHE_SIZE_LARGE))
/* Cache-line alignment.  The emitter itself only needs 4 (the static arrays
 * are .align 2); 64 keeps every sync range line-exact.
 * LAYOUT_PIN (gpsp_config.h): align to the I-cache WAY instead and start the
 * cache JIT_PIN_OFFSET into it, so the emitted stubs land in the same sets on
 * every build, whatever the heap looked like before this call. */
#if defined(LAYOUT_PIN)
#define JIT_CACHE_ALIGN     ((u32)JIT_PIN_WAY)
#define JIT_PIN_SKIP        ((u32)JIT_PIN_OFFSET)
#else
#define JIT_CACHE_ALIGN     64u
#define JIT_PIN_SKIP        0u
#endif

#if defined(LAYOUT_PIN) && defined(GBA_LINK)
/* GBA_LINK (docs/GBA-LINK-MIRROR.md 17.5, 20): where THIS instance's
 * translation cache starts, mod JIT_PIN_WAY, on EITHER tier; -1 =
 * JIT_PIN_OFFSET like every other build (single player).  Set by the
 * frontend before retro_init: a cable session's start reloads instance A
 * with its session offset and loads B with its own (fe_host.c
 * gba_session_mem; B's copy is gbab_jit_pin_at), and the session's end
 * reloads A with -1.  SMALL: the static arrays carry one way of slack
 * (mips_stub.S), so the capacity is unchanged.  LARGE: the block is taken
 * one way larger.  Only the code's addresses move, which the guest cannot
 * see. */
s32 jit_pin_at = -1;
static void jit_small_rebase(void)
{
  u32 skip;
  if (jit_pin_at < 0)
    return;
  skip = ((u32)jit_pin_at - (u32)JIT_PIN_OFFSET) & (JIT_PIN_WAY - 1);
  skip &= ~63u;                       /* keep the zones line-exact */
  rom_translation_cache = rom_translation_cache_static + skip;
  ram_translation_cache = ram_translation_cache_static + skip;
  rom_translation_ptr = last_rom_translation_ptr = rom_translation_cache;
  ram_translation_ptr = last_ram_translation_ptr = ram_translation_cache;
}
/* The LARGE block's skip and size for this load. */
static u32 jit_large_skip(void)
{
  if (jit_pin_at < 0)
    return JIT_PIN_SKIP;
  return ((u32)jit_pin_at & (JIT_PIN_WAY - 1)) & ~63u;
}
static u32 jit_large_extra(void)
{
  return jit_pin_at < 0 ? JIT_PIN_SKIP : (u32)JIT_PIN_WAY;
}
#else
#define jit_small_rebase() do { } while (0)
#define jit_large_skip()   JIT_PIN_SKIP
#define jit_large_extra()  JIT_PIN_SKIP
#endif
/* The LARGE tier's block as allocated (what release frees). */
static u8 *jit_large_block;

void dynarec_select_translation_caches(void)
{
  /* VOLATILE, OR THERE IS NO PROBE.  A malloc whose result only reaches
   * free() is dead code to GCC: it deletes the pair and folds the NULL test
   * to "succeeded".  The first build of this function did exactly that (no
   * malloc call in the ELF, only the memalign), and in PPSSPP's 32 MiB layout
   * it chose the LARGE tier and left the ROM page cache 3 blocks.  That is
   * the PSP-1000 failure this function exists to prevent.  Same trap as
   * init_gamepak_buffer's post_load_reserve. */
  void * volatile probe;
  u8 *blk;

  if (jit_cache_tier != JIT_CACHE_UNDECIDED)
    return;

  /* Every early return below leaves the static SMALL tier in place. */
  jit_cache_tier = JIT_CACHE_SMALL;
  jit_small_rebase();
  if (gpsp_heap_census)
    gpsp_heap_census("pre_jit");

  if (jit_cache_force_small)
  {
    jit_cache_reason = "forced";
    return;
  }

  if (jit_cache_replay && jit_cache_last_tier == JIT_CACHE_SMALL)
  {
    jit_cache_reason = "replay";
    return;
  }
  if (!(jit_cache_replay && jit_cache_last_tier == JIT_CACHE_LARGE))
  {
    /* The decision.  Freed at once: newlib hands the top of the heap
     * straight back, so the block below lands where this probe was. */
    probe = malloc(JIT_LARGE_BYTES + jit_large_extra() + JIT_CACHE_ALIGN +
                   JIT_LARGE_HEADROOM);
    if (!probe)
    {
      jit_cache_reason = "heap_short";
      return;
    }
    free(probe);
  }

  blk = (u8 *)memalign(JIT_CACHE_ALIGN, JIT_LARGE_BYTES + jit_large_extra());
  if (!blk)
  {
    jit_cache_reason = "alloc_failed";
    return;
  }

#if defined(MIPS_ARCH)
  /* Generated code reaches the stubs and C handlers in the EBOOT's .text with
   * `j`/`jal`, which keep the top four bits of the PC: code and target must
   * share a 256 MiB segment.  The PSP's user memory is all in 0x08xxxxxx,
   * so this cannot fail on a real console -- which is exactly why it is
   * checked rather than assumed: if it ever does, a wrong jump is silent. */
  {
    uintptr_t text = (uintptr_t)&flush_translation_cache_rom;
    uintptr_t lo   = (uintptr_t)blk;
    uintptr_t hi   = lo + JIT_LARGE_BYTES + jit_large_extra() - 1;
    if ((lo >> 28) != (text >> 28) || (hi >> 28) != (text >> 28))
    {
      free(blk);
      jit_cache_reason = "segment";
      return;
    }
  }
#endif

  /* (blk itself is never freed after this point; JIT_PIN_SKIP is 0 unless
   * LAYOUT_PIN.) */
  jit_large_block            = blk;
  rom_translation_cache      = blk + jit_large_skip();
  ram_translation_cache      = rom_translation_cache +
                               ROM_TRANSLATION_CACHE_SIZE_LARGE;
  rom_translation_cache_size = ROM_TRANSLATION_CACHE_SIZE_LARGE;
  ram_translation_cache_size = RAM_TRANSLATION_CACHE_SIZE_LARGE;
  rom_translation_ptr = last_rom_translation_ptr = rom_translation_cache;
  ram_translation_ptr = last_ram_translation_ptr = ram_translation_cache;
  jit_cache_tier   = JIT_CACHE_LARGE;
  jit_cache_reason = "heap_ok";
#ifdef JIT_CODE_DISCIPLINE
  /* The zones move into the heap block (memory that was data until now) and
   * the SMALL array below becomes cart data: both directions of a lend. */
  jit_code_owner_changed(JOWN_TIER);
#endif

  /* The static SMALL ROM array is now idle for the life of the process (the
   * tier never changes back), so lend it to the ROM page cache: 2 MiB the
   * heap does not have to supply.  That is what lets a 32 MiB cart sit beside
   * this tier on a real PSP Go, whose heap is 5 MiB smaller than PPSSPP's
   * 64 MiB model (docs/ROM-RESIDENCY.md).  The RAM array (384 KiB) is not a
   * whole block and stays idle.  Only the ROM cache's DATA lands here; no
   * code is ever emitted into it again. */
  {
    extern u8 *gamepak_spare_pool;
    extern u32 gamepak_spare_pool_bytes;
    gamepak_spare_pool       = rom_translation_cache_static;
    gamepak_spare_pool_bytes = ROM_TRANSLATION_CACHE_SIZE;
  }
}

/* THE OTHER HALF, for a frontend that unloads a ROM and stays in the process:
 * the in-process return to the game list (psp/main_psp.c,
 * docs/INPROC-RETURN.md) and a GBA link session's reload
 * (docs/GBA-LINK-MIRROR.md 19).  The LARGE block above was taken in
 * retro_init BEFORE init_gamepak_buffer, so once retro_deinit has freed the
 * page cache, the frame buffer and the audio buffer it is the last allocation
 * of the load still standing.  Freeing it, pointing the caches back at the
 * static arrays, withdrawing the spare pool and putting the tier back to
 * UNDECIDED returns the heap to what it was before retro_init, and the next
 * retro_init decides afresh on the heap it finds (a session reload instead
 * replays the remembered tier: jit_cache_replay).
 *
 * Only after retro_deinit: the spare pool may hold page-cache blocks until
 * memory_term has dropped them.  A no-op on the SMALL tier, which owns no
 * memory. */
void dynarec_release_translation_caches(void)
{
  extern u8 *gamepak_spare_pool;
  extern u32 gamepak_spare_pool_bytes;
  if (jit_cache_tier == JIT_CACHE_LARGE)
  {
    free(jit_large_block);
    jit_large_block = NULL;
#ifdef JIT_CODE_DISCIPLINE
    /* The zones move back into the static arrays. */
    jit_code_owner_changed(JOWN_TIER);
#endif
  }
  rom_translation_cache      = rom_translation_cache_static;
  ram_translation_cache      = ram_translation_cache_static;
  rom_translation_cache_size = ROM_TRANSLATION_CACHE_SIZE;
  ram_translation_cache_size = RAM_TRANSLATION_CACHE_SIZE;
  rom_translation_ptr = last_rom_translation_ptr = rom_translation_cache;
  ram_translation_ptr = last_ram_translation_ptr = ram_translation_cache;
  gamepak_spare_pool       = NULL;
  gamepak_spare_pool_bytes = 0;
  jit_cache_last_tier = jit_cache_tier;
  jit_cache_tier   = JIT_CACHE_UNDECIDED;
  jit_cache_reason = "released";
}
#endif

/* WITHDRAWN: CLEARING THE GATE CANDIDATE TABLE HERE, AND IN
 * init_dynarec_caches.  This cost 2-16% of frame time on hardware.
 *
 * The proven defect was CROSS-ROM: a candidate saturated by one game promotes on
 * a single write from the next, because the stale remembered value counts as a
 * change.  That happens at ROM load, and gba_memory.c already calls
 * smc_gates_reset() there.  Extending it to savestate load, emulator reset,
 * cheat install and config change was my inference, and it is the expensive
 * kind of wrong.
 *
 * WHY IT COSTS SO MUCH.  Earning a gate needs SMC_GATE_MIN_SAMPLE observations
 * at ONE address, and smc_gate_earned runs once per SMC flush event -- not once
 * per store.  Measured on heart_soul_light, the hot mixer address 0300168c
 * produces about 12 events per 600 frames, so re-earning its gate from scratch
 * takes roughly 3200 frames.  Boot supplies those samples cheaply; a savestate
 * load at frame 30 threw them away and left the rest of the run in the no-gate
 * regime, which is the documented 457 KB/s re-translation behaviour.
 *
 * It shows up as MORE SMC EVENTS, not merely more CPU: hardware measured the
 * window count 33 -> 51, xlat 705 -> 831, and mean core time +21%.  The penalty
 * scales inversely with the mixer's write rate, which is why it was +16% on
 * heart_soul_light, +8% on unbound_double_high and +2% on unbound_rival_medium
 * -- the busier the mixer, the sooner the gate comes back.
 *
 * A savestate load does not change ROM, so the candidates remain evidence about
 * the right game.  What a state load DOES invalidate is smc_cand_val, the
 * remembered word at each address -- a real but much smaller hazard, and one
 * that cannot be closed by throwing away the hit counts that cost thousands of
 * frames to collect.  If it is ever worth closing, refresh the VALUES and keep
 * the counts. */
void flush_dynarec_caches(void)
{
  IRQCHK_RESET();
  /* Flush ROM and RAM caches. */
  flush_translation_cache_rom();
  ewram_code_min = 0;
  ewram_code_max = 0x40000;
  iwram_code_min = 0;
  iwram_code_max = 0x8000;
#ifdef SMC_REUSE
  smc_reuse_force_real = 1;   /* reset, state load, ROM load: start clean */
#endif
  flush_translation_cache_ram();
}

#ifdef JIT_CODE_DISCIPLINE
/* ---- JIT_CODE_DISCIPLINE: the writer API and the publish point -----------
 * docs/JIT-CODE-DISCIPLINE.md; the contract is at the top of jit_code.h. */
#define JIT_LINE   64u     /* Allegrex D and I line (measured: icprobe) */
#define JIT_SPANS  32u     /* spans between publishes; full = publish early */
#define JIT_KERNEL_RANGE_MAX 16384u  /* never pass the kernel a range this big */
#define JIT_KERNEL_CHUNK      8192u  /* D writeback piece size */

/* JIT_COST PUBINL (make JIT_COST=1; docs/JIT-COST.md): publish with the
 * cache-line operations themselves instead of two kernel calls.
 *
 * WHY.  The heavy SMC titles publish ~285 times a frame (Doom II, PSP-1000:
 * one span of ~18 lines per publish, 2.1 ms/frame, ~7.4 us each).  Each
 * publish was sceKernelDcacheWritebackRange + sceKernelIcacheInvalidateRange:
 * two syscalls whose bodies, for a range below the I-cache size, are exactly
 * a `cache 0x1A` loop (+ sync) and a `cache 0x08` loop over the lines
 * (uOFW src/kd/sysmem/start.S; docs/JIT-COHERENCY.md section 4).  The same
 * two operations already run in user mode on every PSP: emit_phand
 * (mips_emit.h) patches a `jal` and publishes its line with `cache 0x1A` +
 * `cache 0x08`, shipped since upstream gpSP.
 *
 * WHAT IS THE SAME.  The lines: every line of every recorded span, rounded
 * out exactly as before.  The order: all D write-backs, a sync, then the I
 * side, as the kernel calls ran.  The whole-I cases (ownership change, a span
 * of 16 KiB or more) still use sceKernelIcacheInvalidateAll, the path proven
 * on hardware (kernel-icache-16k-noop): a hit loop has no large-range path,
 * so nothing here can meet the firmware's >= 16 KiB no-op, but a whole
 * invalidate stays cheaper than 256+ line ops.  WHEN publishes happen is
 * untouched (every lookup still publishes before it returns a pointer), so
 * no code can run unpublished that did not before.
 *
 * Run-time switch jit_pub_inline (harness key of the same name): 0 = the
 * kernel calls, so one binary A/Bs both with the layout held fixed.
 *
 * TWIN.  qemu is coherent; a -DJITCOH_PSP_CACHE twin routes each line op to
 * the kernel-call stand-ins (one line each), so the coherency checker sees
 * exactly the lines this loop touches. */
#if defined(JIT_COST) && !defined(JIT_COST_NO_PUBINL)
#define JIT_PUB_INLINE 1
u32 jit_pub_inline = 1;
#if defined(PSP)
static inline void jit_line_dwb(u8 *p)
{ __asm__ volatile("cache 0x1A, 0(%0)" : : "r"(p) : "memory"); }
static inline void jit_line_iinv(u8 *p)
{ __asm__ volatile("cache 0x08, 0(%0)" : : "r"(p) : "memory"); }
static inline void jit_line_sync(void)
{ __asm__ volatile("sync" : : : "memory"); }
#elif defined(JITCOH_PSP_CACHE)
static inline void jit_line_dwb(u8 *p)  { sceKernelDcacheWritebackRange(p, JIT_LINE); }
static inline void jit_line_iinv(u8 *p) { sceKernelIcacheInvalidateRange(p, JIT_LINE); }
static inline void jit_line_sync(void)  { }
#endif
#if defined(PSP) || defined(JITCOH_PSP_CACHE)
/* [lo, hi) rounded out to lines: D write-back, sync, I invalidate.  Also the
 * harness probe's entry point (psp/icprobe.c), so the hardware check runs
 * this very code. */
void jit_pub_lines(u8 *lo, u8 *hi)
{
  u8 *p;
  lo = (u8 *)((uintptr_t)lo & ~(uintptr_t)(JIT_LINE - 1));
  for (p = lo; p < hi; p += JIT_LINE)
    jit_line_dwb(p);
  jit_line_sync();
  for (p = lo; p < hi; p += JIT_LINE)
    jit_line_iinv(p);
}
#endif
#endif
/* JIT_COST builds count the cache work a publish hands out: kernel calls
 * (kcalls) and user-mode line operations (lineops), so the twin -- which
 * cannot time either -- can compare the two models call for call. */
#ifdef JIT_COST
#define JIT_PUB_COUNT(field, n) (jit_code_stats.field += (n))
#else
#define JIT_PUB_COUNT(field, n) do { } while (0)
#endif

typedef struct { u8 *lo, *hi; } jit_span_type;
static jit_span_type jit_spans[JIT_SPANS];
u32 jit_dirty_n;
u32 jit_owner_pending;
u32 jit_stub_zone_sealed;   /* set by init_bios_hooks, cleared by any
                               ownership change */
jit_code_stats_type jit_code_stats;
#ifdef JIT_CODE_AB
#ifndef JIT_CODE_MODE_DEFAULT
#define JIT_CODE_MODE_DEFAULT JCM_DISCIPLINE   /* twin variants override */
#endif
u32 jit_code_mode = JIT_CODE_MODE_DEFAULT;   /* harness key jit_code_mode */
#define JIT_AB_LEGACY() (jit_code_mode < JCM_DISCIPLINE)
#else
#define JIT_AB_LEGACY() 0
#endif

#if defined(JIT_CODE_AUDIT) || defined(JIT_CODE_CHECK)
#include <stdio.h>
#include <stdlib.h>
u32 jit_check_violations;
static void jit_check_fail(const char *what, const void *a, const void *b)
{
  if (jit_check_violations++ < 32)
    fprintf(stderr, "JIT_CODE_CHECK VIOLATION: %s %p..%p (rom %p+%x wm %x ptr %p, "
            "ram %p+%x ptr %p)\n", what, a, b, (void *)rom_translation_cache,
            (unsigned)rom_translation_cache_size, (unsigned)rom_cache_watermark,
            (void *)rom_translation_ptr, (void *)ram_translation_cache,
            (unsigned)ram_translation_cache_size, (void *)ram_translation_ptr);
}

/* ZONES, asserted.  A recorded write must lie in the ROM or RAM allocation;
 * below the watermark only while the stubs are being rebuilt; in the RAM
 * allocation, never in the tag table that grows down from its top. */
static void jit_zone_check(u8 *lo, u8 *hi)
{
  u8 *rom_end = rom_translation_cache + rom_translation_cache_size;
  u8 *ram_end = ram_translation_cache + ram_translation_cache_size;
  u8 *tags = ram_end - (0x10000 - ram_block_tag) / 2 * sizeof(ramtag_type);
  if (((uintptr_t)rom_translation_cache | (uintptr_t)ram_translation_cache |
       rom_translation_cache_size | ram_translation_cache_size) & (JIT_LINE - 1))
    jit_check_fail("a zone does not own whole cache lines", rom_translation_cache,
                   ram_translation_cache);
  if (lo >= rom_translation_cache && hi <= rom_end) {
    if (jit_stub_zone_sealed &&
        lo < rom_translation_cache + rom_cache_watermark)
      jit_check_fail("write into the sealed STUB zone", lo, hi);
  } else if (lo >= ram_translation_cache && hi <= ram_end) {
    if (hi > tags)
      jit_check_fail("RAM code reaches the tag table", lo, hi);
  } else {
    jit_check_fail("code written outside every zone", lo, hi);
  }
}

/* A pointer about to leave a lookup: inside a zone's committed code, and
 * nothing recorded is still unpublished. */
void jit_code_check_entry_(u8 *p)
{
  if (!((p >= rom_translation_cache && p < rom_translation_ptr) ||
        (p >= ram_translation_cache && p < ram_translation_ptr)))
    jit_check_fail("lookup returned code outside the zones", p, p);
  if (jit_dirty_n || jit_owner_pending)
    jit_check_fail("lookup returned with unpublished code", p, p);
}
#define JIT_ZONE_CHECK(lo, hi) jit_zone_check((lo), (hi))
#else
#define JIT_ZONE_CHECK(lo, hi) do { } while (0)
#endif

#ifdef JIT_CODE_AUDIT
/* THE AUDIT (twin): proves the writer API is complete.  A shadow of each
 * zone's committed code is kept; at every publish (and once a frame, from
 * the host) every word that differs from the shadow must lie in a span being
 * published, be zone metadata (a ROM hash header), or be the patch handler's
 * jal-for-jal swap.  Anything else is a write that bypassed the API -- the
 * exact bug class this discipline exists to make impossible. */
static u8 *jit_shadow[2];
static u32 *jit_hdrmap;            /* 1 bit per ROM-zone word: hash header */
static u32 jit_shadow_ready;
u32 jit_audit_violations, jit_audit_checks, jit_audit_phand;

static void jit_audit_alloc(void)
{
  if (jit_shadow_ready)
    return;
  jit_shadow[0] = (u8 *)malloc(rom_translation_cache_size);
  jit_shadow[1] = (u8 *)malloc(ram_translation_cache_size);
  jit_hdrmap = (u32 *)calloc(rom_translation_cache_size / 4 / 32 + 1, 4);
  if (!jit_shadow[0] || !jit_shadow[1] || !jit_hdrmap)
    abort();
  memcpy(jit_shadow[0], rom_translation_cache, rom_translation_cache_size);
  memcpy(jit_shadow[1], ram_translation_cache, ram_translation_cache_size);
  jit_shadow_ready = 1;
}

void jit_code_meta_(u8 *lo, u8 *hi)
{
  u8 *p;
  if (lo < rom_translation_cache ||
      hi > rom_translation_cache + rom_translation_cache_size) {
    if (!(lo >= (u8 *)rom_branch_hash &&
          hi <= (u8 *)(rom_branch_hash + ROM_BRANCH_HASH_SIZE)))
      jit_check_fail("metadata outside the ROM zone", lo, hi);
    return;
  }
  jit_audit_alloc();
  for (p = lo; p < hi; p += 4) {
    u32 w = (u32)(p - rom_translation_cache) >> 2;
    jit_hdrmap[w >> 5] |= 1u << (w & 31);
  }
}

static int jit_in_spans(u8 *p)
{
  u32 i;
  for (i = 0; i < jit_dirty_n; i++)
    if (p >= jit_spans[i].lo && p < jit_spans[i].hi)
      return 1;
  return 0;
}

static int jit_is_stub_jal(u32 op, u8 *at)
{
  u32 tgt;
  if ((op >> 26) != 3)              /* jal */
    return 0;
  tgt = (((u32)(uintptr_t)at + 4) & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2);
  return tgt >= (u32)(uintptr_t)rom_translation_cache &&
         tgt < (u32)(uintptr_t)rom_translation_cache + rom_cache_watermark;
}

static void jit_audit_zone(u32 z, u8 *base, u8 *end)
{
  u32 *m = (u32 *)base, *sh = (u32 *)jit_shadow[z];
  u32 n = (u32)(end - base) >> 2, i, j;
  for (i = 0; i < n; i += 256) {
    u32 len = (n - i < 256) ? n - i : 256;
    if (!memcmp(m + i, sh + i, len * 4))
      continue;
    for (j = i; j < i + len; j++) {
      u8 *at;
      if (m[j] == sh[j])
        continue;
      at = base + j * 4;
      if (jit_in_spans(at))
        ;
      else if (z == 0 && (jit_hdrmap[j >> 5] & (1u << (j & 31))))
        ;
      else if (jit_is_stub_jal(sh[j], at) && jit_is_stub_jal(m[j], at))
        jit_audit_phand++;
      else if (jit_audit_violations++ < 32)
        fprintf(stderr, "JIT_CODE_AUDIT VIOLATION: unrecorded write in the %s "
                "zone at %p (+%x): %08x -> %08x\n", z ? "RAM" : "ROM",
                (void *)at, (unsigned)(at - base), (unsigned)sh[j],
                (unsigned)m[j]);
      sh[j] = m[j];
    }
  }
}

static void jit_audit(void)
{
  jit_audit_alloc();
  jit_audit_checks++;
  jit_audit_zone(0, rom_translation_cache, rom_translation_ptr);
  jit_audit_zone(1, ram_translation_cache, ram_translation_ptr);
}

/* Called by the twin host once a frame: catches a write that no later
 * publish would ever look at. */
void jit_code_audit_now(void)
{
  if (jit_shadow_ready)
    jit_audit();
}
#endif /* JIT_CODE_AUDIT */

/* Record [lo, hi) as written.  Rounded out to whole lines (the unit both
 * caches operate on) and merged into an overlapping or touching span; the
 * table is tiny because a translation writes one contiguous run per zone and
 * patches are rare. */
void jit_code_wrote_(u8 *lo, u8 *hi, u32 cls)
{
  u32 i;
  if (hi <= lo)
    return;
  if (JIT_AB_LEGACY()) {
    /* The original model: a patch synced its own range on the spot; an
     * emission waited for translate_icache_sync's [last, ptr). */
    if (cls == JW_PATCH)
      platform_cache_sync(lo, hi);
    return;
  }
  (void)cls;
  JIT_ZONE_CHECK(lo, hi);
  lo = (u8 *)((uintptr_t)lo & ~(uintptr_t)(JIT_LINE - 1));
  hi = (u8 *)(((uintptr_t)hi + JIT_LINE - 1) & ~(uintptr_t)(JIT_LINE - 1));
  for (i = jit_dirty_n; i-- > 0; ) {
    jit_span_type *s = &jit_spans[i];
    if (lo <= s->hi && hi >= s->lo) {
      if (lo < s->lo) s->lo = lo;
      if (hi > s->hi) s->hi = hi;
      return;
    }
  }
  if (jit_dirty_n == JIT_SPANS) {
    /* Publishing early is always safe: it only makes written code visible
     * sooner.  Nothing has run from these spans yet. */
    jit_code_stats.early++;
    jit_code_publish_();
  }
  jit_spans[jit_dirty_n].lo = lo;
  jit_spans[jit_dirty_n].hi = hi;
  jit_dirty_n++;
}

/* Code memory changed owner: whatever the I-cache holds for it may be a
 * previous generation's code (or never code at all).  The next publish
 * invalidates the whole I-cache -- before any code can run, since every
 * entry into translated code passes a publish. */
void jit_code_owner_changed(u32 why)
{
  if (why < JOWN_COUNT)
    jit_code_stats.owner[why]++;
  jit_stub_zone_sealed = 0;
  if (JIT_AB_LEGACY())
    return;
#ifdef JIT_CODE_AB
  if (jit_code_mode != JCM_NOOWNER)
#endif
  jit_owner_pending = 1;
#ifdef JIT_CODE_AUDIT
  if (why == JOWN_TIER || why == JOWN_INIT) {
    /* The zones moved or were re-initialised: start the shadow afresh. */
    if (jit_shadow_ready) {
      free(jit_shadow[0]); free(jit_shadow[1]); free(jit_hdrmap);
      jit_shadow_ready = 0;
    }
  } else if (why == JOWN_ROM_FLUSH && jit_shadow_ready) {
    /* Headers above the watermark are gone with the code. */
    u32 w = rom_cache_watermark >> 2;
    u32 words = rom_translation_cache_size >> 2, k;
    for (k = w; k < words && (k & 31); k++)
      jit_hdrmap[k >> 5] &= ~(1u << (k & 31));
    if (k < words)
      memset(&jit_hdrmap[k >> 5], 0, (words - k) / 8);
  }
#endif
}

/* THE PUBLISH POINT.  All recorded lines are written back from the D-cache
 * first, then the I-cache forgets them: line by line normally, or entirely
 * when code memory changed owner since the last publish. */
void jit_code_publish_(void)
{
  u32 i, n, k;
#ifdef XLAT_SPLIT_DIAG
  /* DIAGNOSTIC ONLY: publish time, us, into the rfux phase field. */
  u32 xsd_p0 = (core_phase_lvl >= CORE_PHASE_FINE) ? smc_prof_clock() : 0;
#endif

#ifdef JIT_CODE_AUDIT
  jit_audit();
#endif

  /* Sort by start and merge: spans that grew into each other while being
   * recorded are synced once. */
  n = jit_dirty_n;
  for (i = 1; i < n; i++) {
    jit_span_type t = jit_spans[i];
    k = i;
    while (k > 0 && jit_spans[k - 1].lo > t.lo) {
      jit_spans[k] = jit_spans[k - 1];
      k--;
    }
    jit_spans[k] = t;
  }
  for (i = 0, k = 0; i < n; i++) {
    if (k && jit_spans[i].lo <= jit_spans[k - 1].hi) {
      if (jit_spans[i].hi > jit_spans[k - 1].hi)
        jit_spans[k - 1].hi = jit_spans[i].hi;
    } else {
      jit_spans[k++] = jit_spans[i];
    }
  }
  n = k;

  /* JITCOH_PSP_CACHE: the twin's coherency checker (tools/jitcoh,
   * docs/JIT-COHERENCY.md) runs this PSP code against hooked stand-ins of
   * the four kernel calls, so it sees exactly what a PSP would do. */
#if defined(PSP) || defined(JITCOH_PSP_CACHE)
  if (platform_cache_paranoid) {
    /* harness `cache_paranoid = 1` still means: every sync is whole. */
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    jit_code_stats.whole_i++;
    JIT_PUB_COUNT(kcalls, 2);
  } else
#ifdef JIT_PUB_INLINE
  if (jit_pub_inline) {
    /* JIT_COST PUBINL (above): the same lines, no syscall. */
    u32 big = 0, lines = 0;
    for (i = 0; i < n; i++) {
      u8 *p;
      if ((u32)(jit_spans[i].hi - jit_spans[i].lo) >= JIT_KERNEL_RANGE_MAX)
        big = 1;
      for (p = jit_spans[i].lo; p < jit_spans[i].hi; p += JIT_LINE)
        jit_line_dwb(p);
      lines += (u32)(jit_spans[i].hi - jit_spans[i].lo) / JIT_LINE;
    }
    jit_line_sync();
    JIT_PUB_COUNT(lineops, lines);
    if (jit_owner_pending || big) {
      sceKernelIcacheInvalidateAll();
      jit_code_stats.whole_i++;
      JIT_PUB_COUNT(kcalls, 1);
      if (big && !jit_owner_pending)
        jit_code_stats.big_i++;
    } else {
      for (i = 0; i < n; i++) {
        u8 *p;
        for (p = jit_spans[i].lo; p < jit_spans[i].hi; p += JIT_LINE)
          jit_line_iinv(p);
      }
      JIT_PUB_COUNT(lineops, lines);
    }
  } else
#endif
  {
    /* NO KERNEL RANGE OF 16 KiB OR MORE (docs/JIT-COHERENCY.md section 8,
     * docs/JIT-CODE-DISCIPLINE.md 2.3).  The leading explanation of the
     * resident-ROM derail is that the kernel's ranged calls take a different
     * path at >= 16 KiB (the cache size), and only the old Thumb ROM publish
     * ever passed one (up to 83 KiB in a state-load frame).  So the D side
     * is written back in 8 KiB pieces, and an I span that large becomes the
     * whole invalidate -- which is what it would cost anyway. */
    u32 big = 0;
    for (i = 0; i < n; i++) {
      u8 *p = jit_spans[i].lo;
      if ((u32)(jit_spans[i].hi - p) >= JIT_KERNEL_RANGE_MAX)
        big = 1;
      while (p < jit_spans[i].hi) {
        u32 len = (u32)(jit_spans[i].hi - p);
        if (len > JIT_KERNEL_CHUNK)
          len = JIT_KERNEL_CHUNK;
        sceKernelDcacheWritebackRange(p, len);
        JIT_PUB_COUNT(kcalls, 1);
        p += len;
      }
    }
    if (jit_owner_pending || big) {
      sceKernelIcacheInvalidateAll();
      jit_code_stats.whole_i++;
      JIT_PUB_COUNT(kcalls, 1);
      if (big && !jit_owner_pending)
        jit_code_stats.big_i++;
    } else {
      for (i = 0; i < n; i++)
        sceKernelIcacheInvalidateRange(jit_spans[i].lo,
                                       jit_spans[i].hi - jit_spans[i].lo);
      JIT_PUB_COUNT(kcalls, n);
    }
  }
#else
  /* The twin and generic MIPS: the toolchain's clear-cache per span (what the
   * original path used).  qemu-user keeps translated code coherent by
   * itself, so the twin's whole invalidate is only counted; generic MIPS
   * hardware gets both zones cleared. */
  for (i = 0; i < n; i++) {
    __builtin___clear_cache((char *)jit_spans[i].lo, (char *)jit_spans[i].hi);
    /* count what the PSP path does with a span this big (see above) */
    if ((u32)(jit_spans[i].hi - jit_spans[i].lo) >= JIT_KERNEL_RANGE_MAX &&
        !jit_owner_pending) {
      jit_owner_pending = 1;
      jit_code_stats.big_i++;
    }
  }
  if (jit_owner_pending) {
#if !defined(DRPROF_MIPSEL_LINUX)
    __builtin___clear_cache((char *)rom_translation_cache,
                            (char *)rom_translation_cache +
                            rom_translation_cache_size);
    __builtin___clear_cache((char *)ram_translation_cache,
                            (char *)ram_translation_cache +
                            ram_translation_cache_size);
#endif
    jit_code_stats.whole_i++;
  }
#ifdef JIT_COST
  /* The PSP's cache work for this publish, counted as the PSP path above
   * would hand it out (the twin cannot time it). */
  {
    u32 lines = 0, dk = 0;
    for (i = 0; i < n; i++) {
      u32 len = (u32)(jit_spans[i].hi - jit_spans[i].lo);
      lines += len / JIT_LINE;
      dk += (len + JIT_KERNEL_CHUNK - 1) / JIT_KERNEL_CHUNK;
    }
#ifdef JIT_PUB_INLINE
    if (jit_pub_inline) {
      JIT_PUB_COUNT(lineops, jit_owner_pending ? lines : 2 * lines);
      JIT_PUB_COUNT(kcalls, jit_owner_pending ? 1 : 0);
    } else
#endif
    JIT_PUB_COUNT(kcalls, dk + (jit_owner_pending ? 1 : n));
  }
#endif
#endif

  if (n) {
    jit_code_stats.publishes++;
    jit_code_stats.spans += n;
    for (i = 0; i < n; i++)
      jit_code_stats.lines += (u32)(jit_spans[i].hi - jit_spans[i].lo) / JIT_LINE;
  }
#ifdef XLAT_SPLIT_DIAG
  if (xsd_p0) cph_rfux += smc_prof_clock() - xsd_p0;
#endif
  jit_dirty_n = 0;
  jit_owner_pending = 0;
}
#endif /* JIT_CODE_DISCIPLINE */

#if defined(SMC_RETIRE_LOCAL) && defined(SMC_RETIRE_LOCAL_CHECK)
static void smc_local_check(const u16 *tagp, u32 base, u32 first, u32 local,
                            u32 lo_off, u32 hi_off)
{
  u32 i, lo = base + lo_off, hi = base + hi_off;
  for (i = first; i < local; i++) {
    if (VALID_TAG(tagp[i])) {
      ramtag_type *te = get_ram_tag(tagp[i]);
      if ((SMC_RAM_LIVE_OFFSET(te, arm) &&
           (!te->blk_end_arm || te->blk_end_arm > lo)) ||
          (SMC_RAM_LIVE_OFFSET(te, thumb) &&
           (!te->blk_end_thumb || te->blk_end_thumb > lo))) {
        smc_local_violations++;
#ifdef DRPROF_TWIN
        fprintf(stderr, "SMC_RETIRE_LOCAL VIOLATION: live block at %08x "
                "(end arm %08x thumb %08x) overlaps [%08x,%08x) below the "
                "chunk bound %08x\n", base + (i << 1),
                (unsigned)te->blk_end_arm, (unsigned)te->blk_end_thumb,
                lo, hi, base + (local << 1));
        abort();
#endif
      }
    }
  }
}
#endif

#if defined(DRPROF_TWIN) && (defined(JIT_CODE_DISCIPLINE) || defined(JIT_SYNC_STATS))
#include <stdio.h>
/* The twin host prints this at exit (weak reference in dr_host.c). */
void jit_stats_report(FILE *f)
{
  /* twin only: flush counts by cause (main.c) */
  fprintf(f, "flushes ram=%u full=%u smc=%u dma=%u partial=%u rom=%u\n",
          (unsigned)flush_ram_total, (unsigned)flush_ram_full,
          (unsigned)flush_ram_smc, (unsigned)flush_ram_dma,
          (unsigned)flush_ram_partial, (unsigned)flush_rom_total);
#ifdef SMC_STM_CHAIN
  {
    extern u32 smc_chain_marks, smc_chain_same;
    fprintf(f, "stmchain marks=%u same=%u\n", (unsigned)smc_chain_marks,
            (unsigned)smc_chain_same);
  }
#endif
#if defined(DMA_SMC_ADAPT) || defined(SMC_WORK_STATS)
  { extern u32 smc_work_walk, smc_work_halves;
    fprintf(f, "smcwork walk=%u halves=%u\n", (unsigned)smc_work_walk,
            (unsigned)smc_work_halves); }
#endif
#ifdef DMA_SMC_ADAPT
  fprintf(f, "dmaadapt mode=%u switches=%u probes=%u\n",
          (unsigned)dma_rt_mode, (unsigned)dma_rt_switches,
          (unsigned)dma_rt_probes);
#endif
#ifdef SMC_STM_AHEAD
  fprintf(f, "stmahead bytes=%u\n", (unsigned)smc_stm_ahead_bytes);
#endif
#ifdef DMA_SMC_RETIRE
  fprintf(f, "dmaretire retired=%u\n", (unsigned)dma_smc_retired);
#endif
#ifdef SMC_FALLBACK_STATS
  {
    unsigned k;
    fprintf(f, "smcfb");
    for (k = 0; k < 16; k++)
      fprintf(f, " %u", (unsigned)smc_fallback_stats[k]);
    fprintf(f, "\n");
  }
#endif
#ifdef JIT_CODE_DISCIPLINE
  fprintf(f, "jitc publishes=%u spans=%u lines=%u whole_i=%u big_i=%u early=%u "
          "own_rom=%u own_ram=%u own_rebuild=%u own_init=%u own_tier=%u",
          (unsigned)jit_code_stats.publishes, (unsigned)jit_code_stats.spans,
          (unsigned)jit_code_stats.lines, (unsigned)jit_code_stats.whole_i,
          (unsigned)jit_code_stats.big_i, (unsigned)jit_code_stats.early,
          (unsigned)jit_code_stats.owner[JOWN_ROM_FLUSH],
          (unsigned)jit_code_stats.owner[JOWN_RAM_FLUSH],
          (unsigned)jit_code_stats.owner[JOWN_REBUILD],
          (unsigned)jit_code_stats.owner[JOWN_INIT],
          (unsigned)jit_code_stats.owner[JOWN_TIER]);
#ifdef JIT_COST
  fprintf(f, " kcalls=%u lineops=%u", (unsigned)jit_code_stats.kcalls,
          (unsigned)jit_code_stats.lineops);
#endif
#if defined(JIT_CODE_AUDIT) || defined(JIT_CODE_CHECK)
  fprintf(f, " check_violations=%u", (unsigned)jit_check_violations);
#endif
#ifdef JIT_CODE_AUDIT
  fprintf(f, " audit_checks=%u audit_violations=%u audit_phand=%u",
          (unsigned)jit_audit_checks, (unsigned)jit_audit_violations,
          (unsigned)jit_audit_phand);
#endif
  fprintf(f, "\n");
#else
  fprintf(f, "jitlegacy rom_pub=%u ram_pub=%u syncs=%u lines=%u\n",
          (unsigned)jit_legacy_stats.rom_pub, (unsigned)jit_legacy_stats.ram_pub,
          (unsigned)jit_legacy_stats.calls, (unsigned)jit_legacy_stats.lines);
#endif
#ifdef VRAM_EXEC
  fprintf(f, "vramx armed=%u activations=%u blocks=%u drops=%u slots=%u\n",
          (unsigned)vram_exec_active, (unsigned)vram_exec_activations,
          (unsigned)vram_exec_blocks, (unsigned)vram_exec_drops,
          (unsigned)vram_x_slot_n);
#endif
}
#endif

#ifdef DRPROF_TWIN
/* ---- DRPROF_TWIN: translation-time class map (docs/DYNAREC-PROFILE.md) ----
 * One u16 per emitted MIPS word, beside the caches (never inside them), so the
 * emitted code is byte-identical to a normal build.  The qemu plugin reads the
 * map lazily the first time it executes a translated TB. */
static u16 drprof_rom_map_s[ROM_TRANSLATION_CACHE_SIZE / 4];
static u16 drprof_ram_map_s[RAM_TRANSLATION_CACHE_SIZE / 4];
u16 *drprof_rom_map = drprof_rom_map_s;
u16 *drprof_ram_map = drprof_ram_map_s;
u8 *drprof_stub_end;
volatile u32 drprof_zone_arg;
volatile u32 drprof_xlat_pc;
volatile u32 drprof_blk_arg[8];

__attribute__((noinline, used)) void drprof_blk_mark(void)
{
  __asm__ volatile("" ::: "memory");
}

__attribute__((noinline, used)) void drprof_zone(void)
{
  __asm__ volatile("" ::: "memory");
}

/* noinline: inlined, its body ran inside translate_block_* and the twin
 * charged it to translation (~23 % of translate_block_arm, xlat-cost
 * 2026-10-28); out of line it is drprof_* bookkeeping, never counted, and
 * only the call remains (twin only: DRPROF_TWIN). */
__attribute__((noinline)) void drprof_note(u8 *host, u16 cls)
{
  if (host >= rom_translation_cache &&
      host < rom_translation_cache + rom_translation_cache_size)
    drprof_rom_map[(host - rom_translation_cache) >> 2] = cls;
  else if (host >= ram_translation_cache &&
           host < ram_translation_cache + ram_translation_cache_size)
    drprof_ram_map[(host - ram_translation_cache) >> 2] = cls;
}

void drprof_forget_ram(void)
{
  DRPROF_ZONE_ENTER(DRZ_TWIN);
  memset(drprof_ram_map_s, 0, sizeof(drprof_ram_map_s));
  DRPROF_ZONE_LEAVE(DRZ_TWIN);
}

void drprof_forget_rom(u8 *from)
{
  u32 w = (from - rom_translation_cache) >> 2;
  DRPROF_ZONE_ENTER(DRZ_TWIN);
  if (w < ROM_TRANSLATION_CACHE_SIZE / 4)
    memset(&drprof_rom_map_s[w], 0,
           (ROM_TRANSLATION_CACHE_SIZE / 4 - w) * sizeof(u16));
  DRPROF_ZONE_LEAVE(DRZ_TWIN);
}

u16 drprof_class_arm(u32 op, u32 fs)
{
  u32 cond = op >> 28, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
  u32 L = (op >> 20) & 1, S = (op >> 20) & 1;
  u16 f = 0, c = DRC_OTHER;
  (void)fs;
  if (cond != 0xE && cond != 0xF)
    f |= DRC_F_COND;
  switch ((op >> 25) & 7)
  {
    case 0:
      if ((op & 0x0FFFFFF0) == 0x012FFF10) { c = DRC_BX; f |= DRC_F_PCW; break; }
      if ((op & 0x0FC000F0) == 0x00000090 || (op & 0x0F8000F0) == 0x00800090)
      { c = DRC_MUL; if (S) f |= DRC_F_SBIT | DRC_F_FLAGS; break; }
      if ((op & 0x0FB00FF0) == 0x01000090) { c = DRC_SWP; break; }
      if ((op & 0x90) == 0x90)
      {
        if (L) c = rn == 15 ? DRC_LOAD_LIT : rn == 13 ? DRC_LOAD_SP : DRC_LOAD;
        else   c = rn == 13 ? DRC_STORE_SP : DRC_STORE;
        if (L && rd == 15) f |= DRC_F_PCW;
        break;
      }
      /* fall through: data processing, register operand */
    case 1:
      if (((op >> 23) & 3) == 2 && !S) { c = DRC_PSR; break; }
      {
        u32 opc = (op >> 21) & 15;
        c = (opc >= 8 && opc <= 11) ? DRC_CMP :
            (opc == 13 || opc == 15) ? DRC_MOV : DRC_ALU;
        /* ARM has no flag liveness: arm_dead_flag_eliminate() forces 0xF, so
         * every S-bit instruction generates its flags. */
        if (S) f |= DRC_F_SBIT | DRC_F_FLAGS;
        if (rd == 15 && c != DRC_CMP) f |= DRC_F_PCW;
      }
      break;
    case 2:
    case 3:
      if (((op >> 25) & 7) == 3 && (op & 0x10)) { c = DRC_OTHER; break; }
      if (L) c = rn == 15 ? DRC_LOAD_LIT : rn == 13 ? DRC_LOAD_SP : DRC_LOAD;
      else   c = rn == 13 ? DRC_STORE_SP : DRC_STORE;
      if (L && rd == 15) f |= DRC_F_PCW;
      break;
    case 4:
      c = L ? DRC_LDM : DRC_STM;
      if (rn == 13) f |= DRC_F_SPBASE;
      if (L && (op & 0x8000)) f |= DRC_F_PCW;
      break;
    case 5:
      c = (op >> 24) & 1 ? DRC_BL : DRC_B;
      f |= DRC_F_PCW;
      break;
    case 7:
      c = ((op >> 24) & 0xF) == 0xF ? DRC_SWI : DRC_OTHER;
      break;
    default:
      c = DRC_OTHER;
  }
  return c | f;
}

u16 drprof_class_thumb(u32 op, u32 fs)
{
  u32 hi = (op >> 8) & 0xFF;
  u16 f = DRC_F_THUMB, c = DRC_OTHER;
  u16 fl = (fs & 0xF) ? DRC_F_FLAGS : 0;
  if (hi <= 0x1F)      { c = DRC_ALU; f |= DRC_F_SBIT | fl; }
  else if (hi <= 0x27) { c = DRC_MOV; f |= DRC_F_SBIT | fl; }
  else if (hi <= 0x2F) { c = DRC_CMP; f |= DRC_F_SBIT | fl; }
  else if (hi <= 0x3F) { c = DRC_ALU; f |= DRC_F_SBIT | fl; }
  else if (hi <= 0x43)
  {
    u32 sub = (op >> 6) & 0xF;
    c = (sub == 8 || sub == 10 || sub == 11) ? DRC_CMP :
        sub == 13 ? DRC_MUL : sub == 15 ? DRC_MOV : DRC_ALU;
    f |= DRC_F_SBIT | fl;
  }
  else if (hi <= 0x47)
  {
    u32 o2 = (op >> 8) & 3, rd = (op & 7) | ((op >> 4) & 8);
    if (o2 == 3)      { c = DRC_BX; f |= DRC_F_PCW; }
    else if (o2 == 1) { c = DRC_CMP; f |= DRC_F_SBIT | fl; }
    else
    {
      c = o2 == 2 ? DRC_MOV : DRC_ALU;
      if (rd == 15) f |= DRC_F_PCW;
    }
  }
  else if (hi <= 0x4F) c = DRC_LOAD_LIT;
  else if (hi <= 0x5F) c = ((op >> 9) & 7) >= 3 ? DRC_LOAD : DRC_STORE;
  else if (hi <= 0x8F) c = (op & 0x0800) ? DRC_LOAD : DRC_STORE;
  else if (hi <= 0x9F) c = (op & 0x0800) ? DRC_LOAD_SP : DRC_STORE_SP;
  else if (hi <= 0xB0) c = DRC_ALU;
  else if (hi == 0xB4 || hi == 0xB5) { c = DRC_STM; f |= DRC_F_SPBASE; }
  else if (hi == 0xBC || hi == 0xBD)
  {
    c = DRC_LDM; f |= DRC_F_SPBASE;
    if (hi == 0xBD) f |= DRC_F_PCW;
  }
  else if (hi <= 0xBF) c = DRC_OTHER;
  else if (hi <= 0xC7) c = DRC_STM;
  else if (hi <= 0xCF) c = DRC_LDM;
  else if (hi <= 0xDD) { c = DRC_B; f |= DRC_F_COND | DRC_F_PCW; }
  else if (hi == 0xDF) c = DRC_SWI;
  else if (hi <= 0xDE) c = DRC_OTHER;
  else if (hi <= 0xE7) { c = DRC_B; f |= DRC_F_PCW; }
  else if (hi <= 0xEF) c = DRC_OTHER;
  else if (hi <= 0xF7) c = DRC_BL;
  else                 { c = DRC_BL; f |= DRC_F_PCW; }
  return c | f;
}
#endif /* DRPROF_TWIN */
