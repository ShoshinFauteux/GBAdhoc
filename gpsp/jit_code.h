/* jit_code.h -- the dynarec's code-memory discipline (JIT_CODE_DISCIPLINE).
 *
 * docs/JIT-CODE-DISCIPLINE.md is the design; this is the contract in brief.
 *
 * ZONES.  Translated code lives in exactly three places, and nowhere else:
 *   STUB  [rom_translation_cache, rom_cache_watermark): the emitter's stubs and
 *         the pre-generated BIOS SWI entry.  Written by init_emitter /
 *         init_bios_hooks, then immutable until the next rebuild.
 *   ROM   [rom_cache_watermark, rom_translation_ptr): translations of ROM and
 *         BIOS code.  Each block is preceded by an 8-byte hash header (zone
 *         METADATA: never executed, never shares a byte with code).
 *   RAM   [ram_translation_cache, ram_translation_ptr): translations of
 *         IWRAM/EWRAM code.  The RAM tag table grows down from the top of the
 *         same allocation (METADATA; checked never to meet the code).
 *
 * ONE WRITER API.  Every write of executable bytes is recorded:
 *   JIT_CODE_EMITTED(lo, hi)   a block's whole emission, at commit;
 *   JIT_CODE_WROTE(lo, hi)     a patch of code (thunks, retirement, links);
 *   JIT_PATCH_BRANCH(src, dst) the branch patch, recorded;
 *   JIT_CODE_META(lo, hi)      metadata inside a zone (audit builds only).
 * Records are rounded out to whole 64-byte cache lines and coalesced.  No
 * call site chooses a sync range any more.
 *
 * ONE PUBLISH POINT.  jit_code_publish(): D-cache writeback of every recorded
 * line, then I-cache invalidate of the same lines -- or of the WHOLE I-cache
 * if code memory changed owner since the last publish -- then the record is
 * cleared.  It runs before a block pointer leaves the lookups, after any
 * patch of existing code, and at the end of init_bios_hooks.  Publishing is
 * idempotent and cheap when nothing is pending, so an extra call is never
 * wrong; a missing one is what the audit build catches.
 *
 * OWNERSHIP CHANGES.  jit_code_owner_changed(why): ROM/RAM translation-cache
 * flush, emitter rebuild, tier selection (the spare-pool lend), and through
 * those the state load and ROM load.  Memory that held other code (or data)
 * a moment ago may still have I-cache lines; the next publish invalidates the
 * whole I-cache.  These are rare, so the whole invalidate is cheap.
 *
 * THE ONE WRITER OUTSIDE THE API is emitted code: the memory-region patch
 * handler (mips_emit.h emit_phand) rewrites the `jal` of a memory access in
 * place and syncs that one line itself (cache 0x1A + cache 0x08).  It only
 * ever swaps one stub call for another stub call, and every stub checks the
 * region again, so even a stale fetch of the old `jal` re-enters the patch
 * handler and computes the right result.  The audit build recognises it.
 *
 * OFF (the default): every macro below expands to the original code, and
 * translate_icache_sync / platform_cache_sync keep their old behaviour. */
#ifndef JIT_CODE_H
#define JIT_CODE_H

#ifdef JIT_CODE_DISCIPLINE

#if !defined(MIPS_ARCH)
#error "JIT_CODE_DISCIPLINE is implemented for the MIPS dynarec only"
#endif

/* Why code memory changed owner (statistics and the audit's log). */
enum
{
  JOWN_ROM_FLUSH = 0,   /* flush_translation_cache_rom                       */
  JOWN_RAM_FLUSH,       /* flush_translation_cache_ram (full)                */
  JOWN_REBUILD,         /* init_emitter: stubs re-emitted                    */
  JOWN_INIT,            /* init_dynarec_caches                               */
  JOWN_TIER,            /* dynarec_select_translation_caches (zones move;    */
                        /* the idle SMALL array is lent to the ROM as data)  */
  JOWN_COUNT
};

/* Pending work.  Read by the inline fast path of jit_code_publish(). */
extern u32 jit_dirty_n;        /* recorded spans not yet published          */
extern u32 jit_owner_pending;  /* whole-I owed at the next publish          */
extern u32 jit_stub_zone_sealed; /* STUB zone immutable until next rebuild */

/* What kind of write a record is.  The discipline treats both alike; the
 * class only matters to JIT_CODE_AB's legacy modes, which must reproduce the
 * original syncs exactly: EMIT writes were covered by the translation's
 * [last, ptr) sync, PATCH writes were synced on the spot. */
enum { JW_EMIT = 0, JW_PATCH = 1 };

void jit_code_wrote_(u8 *lo, u8 *hi, u32 cls);
void jit_code_publish_(void);
void jit_code_owner_changed(u32 why);

static inline void jit_code_publish(void)
{
  if (jit_dirty_n | jit_owner_pending)
    jit_code_publish_();
}

#define JIT_CODE_WROTE(lo, hi)   jit_code_wrote_((u8 *)(lo), (u8 *)(hi), JW_PATCH)
#define JIT_CODE_EMITTED(lo, hi) jit_code_wrote_((u8 *)(lo), (u8 *)(hi), JW_EMIT)
/* A block link written while translating (eager exit resolution). */
#define JIT_PATCH_BRANCH(src, dst) do {                                        \
  u8 *jpb_src_ = (u8 *)(src);                                                 \
  generate_branch_patch_unconditional(jpb_src_, (dst));                       \
  jit_code_wrote_(jpb_src_, jpb_src_ + 4, JW_EMIT);                           \
} while (0)

/* JIT_CODE_AB (harness only): one binary, the sync model chosen at startup
 * (harness key `jit_code_mode`), so a hardware A/B holds the code layout
 * fixed -- the bug this exists for comes and goes with layout.
 *   0 LEGACY     the original ranged syncs (no 8b48b48 fix)
 *   1 ROMWHOLE   the original + 8b48b48 (whole I on every ROM publish)
 *   2 DISCIPLINE this model (the default)
 *   3 NOOWNER    this model without the whole I on ownership changes: the
 *                mechanism probe (if 3 derails where 2 does not, the stale
 *                lines are a previous owner's) */
#ifdef JIT_CODE_AB
enum { JCM_LEGACY = 0, JCM_ROMWHOLE = 1, JCM_DISCIPLINE = 2, JCM_NOOWNER = 3 };
extern u32 jit_code_mode;
#endif

/* Statistics: always kept in discipline builds (a few adds per publish). */
typedef struct
{
  u32 publishes;        /* non-empty publishes                               */
  u32 spans;            /* spans published                                   */
  u32 lines;            /* 64-byte lines written back (and, if ranged, I-inv) */
  u32 whole_i;          /* whole I-cache invalidates                         */
  u32 early;            /* publishes forced by a full span table             */
  u32 owner[JOWN_COUNT];
  u32 big_i;            /* whole-I forced by a span >= 16 KiB (kept last:
                           the frontend reads the first ten words) */
} jit_code_stats_type;
extern jit_code_stats_type jit_code_stats;

/* JIT_CODE_CHECK (implied by JIT_CODE_AUDIT): zone asserts on every record
 * and on every pointer a lookup returns.  Debug/twin builds only. */
#if defined(JIT_CODE_AUDIT) || defined(JIT_CODE_CHECK)
void jit_code_check_entry_(u8 *p);
#define JIT_CODE_CHECK_ENTRY(p) jit_code_check_entry_((u8 *)(p))
extern u32 jit_check_violations;
#else
#define JIT_CODE_CHECK_ENTRY(p) do { } while (0)
#endif

#ifdef JIT_CODE_AUDIT
/* Twin/debug only: every publish compares the zones' code against a shadow
 * and reports any byte that changed without being recorded. */
void jit_code_meta_(u8 *lo, u8 *hi);
#define JIT_CODE_META(lo, hi) jit_code_meta_((u8 *)(lo), (u8 *)(hi))
extern u32 jit_audit_violations;
extern u32 jit_audit_checks;
extern u32 jit_audit_phand;
#else
#define JIT_CODE_META(lo, hi) do { } while (0)
#endif

#else /* !JIT_CODE_DISCIPLINE: the original code, unchanged */

#define JIT_CODE_WROTE(lo, hi)     do { } while (0)
#define JIT_CODE_EMITTED(lo, hi)   do { } while (0)
#define JIT_CODE_CHECK_ENTRY(p)    do { } while (0)
#define JIT_CODE_META(lo, hi)      do { } while (0)
#define JIT_PATCH_BRANCH(src, dst) generate_branch_patch_unconditional(src, dst)

#endif /* JIT_CODE_DISCIPLINE */

#endif /* JIT_CODE_H */
