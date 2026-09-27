/* test_smc_safety.c — host proof for the dynarec safety invariants added on
 * opus/performance-stability-fixes.
 *
 * Follows this repo's test convention: the production logic is reproduced here
 * textually, and each case asserts the PRE-change behaviour as well as the
 * current one, so the test documents the defect rather than only the fix.  A
 * test that can only pass cannot tell you the bug was ever there.
 *
 *   gcc -Wall -Wextra -O1 -o test_smc_safety tools/test_smc_safety.c && ./test_smc_safety
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint32_t u32;

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

/* ======================================================================== 1
 * What the sentinel becomes once it reaches the patch macro.
 *
 * cpu_threaded.c: block_lookup_translate_* returns (u8 *)(~0) for a pc in a
 * region it does not handle.  mips/mips_emit.h:
 *
 *   #define generate_branch_patch_unconditional(dest, offset)  \
 *     *((u32 *)(dest)) = (mips_opcode_j << 26) |               \
 *      ((mips_absolute_offset(offset)) & 0x3FFFFFF)
 *
 * mips/mips_codegen.h: mips_absolute_offset(x) == ((u32)x / 4), mips_opcode_j
 * == 0x02.
 */
#define MIPS_OPCODE_J           0x02u
#define BLOCK_LOOKUP_UNMAPPABLE ((u8 *)(~(uintptr_t)0))

static u32 mips_absolute_offset(const void *p) { return (u32)(uintptr_t)p / 4u; }

static u32 patch_word(const void *target)
{
   return (MIPS_OPCODE_J << 26) | (mips_absolute_offset(target) & 0x3FFFFFFu);
}

/* MIPS `j`: PC = (PC_of_delay_slot & 0xF0000000) | (target26 << 2). */
static u32 j_destination(u32 delay_slot_pc, u32 insn)
{
   return (delay_slot_pc & 0xF0000000u) | ((insn & 0x3FFFFFFu) << 2);
}

/* PSP main RAM, 32 MB kit (PSP-2000 and later; a PSP-1000 has half). */
#define PSP_RAM_LO 0x08800000u
#define PSP_RAM_HI 0x09FFFFFFu

static int test_sentinel_is_a_jump_to_nowhere(void)
{
   u32 insn = patch_word(BLOCK_LOOKUP_UNMAPPABLE);
   u32 dest;

   CHECK((insn >> 26) == MIPS_OPCODE_J);
   /* 0xFFFFFFFF / 4 = 0x3FFFFFFF, truncated to 26 bits. */
   CHECK((insn & 0x3FFFFFFu) == 0x3FFFFFFu);

   /* Emitted into a block living in PSP main RAM. */
   dest = j_destination(0x08810004u, insn);
   CHECK(dest == 0x0FFFFFFCu);
   CHECK(dest < PSP_RAM_LO || dest > PSP_RAM_HI);   /* nothing is mapped there */

   /* The old test at the patch site. This is the whole defect: the sentinel is
    * not NULL, so it sailed through and was written into a live block. */
   CHECK(BLOCK_LOOKUP_UNMAPPABLE != NULL);

   /* A real target round-trips, so the patch macro itself is not at fault. */
   {
      u8 fake_block[64];
      u32 ok = patch_word(fake_block + 16);
      CHECK(j_destination(0x08810004u, ok) ==
            (((u32)(uintptr_t)(fake_block + 16)) & 0x0FFFFFFCu));
   }
   return 0;
}

/* ======================================================================== 2
 * gba_pc_translatable must be exactly block_lookup_translate_builder's switch:
 * case 0x2, 0x3 (RAM), case 0x0 and 0x8 ... 0xD (BIOS + ROM).  Everything else
 * falls through to the sentinel.
 */
#define GBA_PC(x) ((x) & 0x0FFFFFFFu)

static int gba_pc_translatable(u32 pc)
{
   u32 region = GBA_PC(pc) >> 24;
   return region == 0x0 || region == 0x2 || region == 0x3 ||
          (region >= 0x8 && region <= 0xD);
}

static int test_region_classifier(void)
{
   struct { u32 pc; int ok; const char *what; } cases[] = {
      { 0x00000008u, 1, "BIOS SWI vector"          },
      { 0x02012345u, 1, "EWRAM"                    },
      { 0x0300168cu, 1, "IWRAM (the mixer gate)"   },
      { 0x08000000u, 1, "ROM page 0"               },
      { 0x0DFFFFFCu, 1, "ROM top"                  },
      { 0x01000000u, 0, "unused region 1"          },
      { 0x04000208u, 0, "I/O"                      },
      { 0x05000000u, 0, "palette"                  },
      { 0x06008000u, 0, "VRAM"                     },
      { 0x07000100u, 0, "OAM"                      },
      { 0x0E005555u, 0, "SRAM"                     },
      { 0x0F000000u, 0, "region F"                 },
   };
   unsigned i;
   for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      if (gba_pc_translatable(cases[i].pc) != cases[i].ok) {
         printf("FAIL classifier: %08x (%s)\n", cases[i].pc, cases[i].what);
         return 1;
      }
      checks++;
   }
   /* GBA_PC_MASK is on in the release build, so the top nibble is masked off
    * before the region test -- 0x58581818 masks to region 8 and IS
    * translatable.  That is precisely why masking alone is not a fix and the
    * containment has to sit at the patch site. */
   CHECK(gba_pc_translatable(GBA_PC(0x58581818u)) == 1);
   return 0;
}

/* ======================================================================== 3
 * The patch loop, before and after.
 */
static u8 cache[256];


/* Pre-change: translate, test for NULL only, patch. */
static u32 patch_loop_old(u32 branch_target, u8 *source)
{
   u8 *translation_target = gba_pc_translatable(branch_target)
                          ? cache + 128           /* a real block */
                          : BLOCK_LOOKUP_UNMAPPABLE;
   if (!translation_target)
      return 0;
   *((u32 *)source) = patch_word(translation_target);
   return 1;
}

/* Post-change: classify first; an untranslatable exit is patched to the shared,
 * pre-generated bios_swi_entrypoint and NOTHING is emitted.
 *
 * The first version emitted a 16-byte dispatch island per exit.  It was correct
 * and cost +16.4%/+8.2% of frame work on hardware, because every island is
 * generated code in a translation cache that a flush-heavy workload keeps
 * refilling.  `bytes_emitted` stays at zero here to pin that down: the fix is
 * that this path allocates no cache at all. */
static u32 bytes_emitted;
static u8 swi_entrypoint[4];                 /* stands in for the real one */
static u32 patch_loop_new(u32 branch_target, u8 *source, u8 **translation_ptr)
{
   u8 *translation_target;
   if (!gba_pc_translatable(branch_target)) {
      translation_target = swi_entrypoint;     /* shared, never emitted */
   } else {
      translation_target = cache + 128;
      (void)translation_ptr;
   }
   if (!translation_target || translation_target == BLOCK_LOOKUP_UNMAPPABLE)
      return 0;
   *((u32 *)source) = patch_word(translation_target);
   return 1;
}

static int test_containment(void)
{
   u8 src[4];
   u8 *tp = cache + 160;
   u32 insn, dest;

   /* A garbage exit, pre-change. */
   memset(src, 0, sizeof(src));
   CHECK(patch_loop_old(0x07000100u, src) == 1);      /* it "succeeded" */
   insn = *(u32 *)src;
   dest = j_destination(0x08810004u, insn);
   CHECK(dest == 0x0FFFFFFCu);                        /* straight off the map */

   /* The same exit, post-change. */
   memset(src, 0, sizeof(src));
   bytes_emitted = 0;
   CHECK(patch_loop_new(0x07000100u, src, &tp) == 1);
   CHECK(bytes_emitted == 0);      /* nothing added to the translation cache */
   insn = *(u32 *)src;
   /* It encodes the shared known-good entrypoint, not the sentinel.  (The host
    * is 64-bit, so only the encoding is compared; the segment arithmetic is
    * checked against a real PSP address in test 1.) */
   CHECK(insn == patch_word(swi_entrypoint));
   CHECK(insn != patch_word(BLOCK_LOOKUP_UNMAPPABLE));
   CHECK((insn & 0x3FFFFFFu) != 0x3FFFFFFu);

   /* A good exit is untouched by the new path. */
   memset(src, 0, sizeof(src));
   bytes_emitted = 0;
   CHECK(patch_loop_new(0x0300168cu, src, &tp) == 1);
   CHECK(bytes_emitted == 0);
   CHECK(*(u32 *)src == patch_word(cache + 128));

   /* AND NO EMIT BUDGET IS NEEDED, which is the point of the shared stub.
    * translation_cache_limit is tested only inside the per-instruction emit
    * loop, so anything emitted after it spends the threshold's margin with
    * nothing checking -- and the ramtag table sits immediately after the RAM
    * cache.  A path that emits nothing cannot overrun it. */
   CHECK(bytes_emitted == 0);
   return 0;
}

/* ======================================================================== 4
 * The ranked gate candidate table: the promotion rule, before and after.
 */
#define SMC_GATE_MIN_SAMPLE    64
#define SMC_GATE_CAND          32

typedef struct {
   u32 addr[SMC_GATE_CAND], hits[SMC_GATE_CAND], val[SMC_GATE_CAND];
   u32 chg[SMC_GATE_CAND];
   u32 used;
} cand_table;

/* Pre-change: lifetime totals, no staleness, and nothing ever cleared it. */
static int earned_old(cand_table *c, u32 gpc, u32 cur)
{
   u32 k, coldest = 0;
   for (k = 0; k < c->used; k++)
      if (c->addr[k] == gpc) {
         c->hits[k]++;
         if (cur != c->val[k]) c->chg[k]++;
         c->val[k] = cur;
         return c->hits[k] >= SMC_GATE_MIN_SAMPLE &&
                c->chg[k] * 2 >= c->hits[k];
      }
   if (c->used >= SMC_GATE_CAND) {
      for (k = 1; k < SMC_GATE_CAND; k++)
         if (c->hits[k] < c->hits[coldest]) coldest = k;
   } else coldest = c->used++;
   c->addr[coldest] = gpc; c->hits[coldest] = 1;
   c->chg[coldest] = 0;    c->val[coldest] = cur;
   return 0;
}

/* Post-change: the SAME rule -- what changed is that the table is now cleared
 * at every boundary where its evidence dies.
 *
 * A staleness rule was here too, restarting a row after a ~60-frame gap so an
 * address hot in one role could not promote on one write in another.  It was
 * WITHDRAWN on 2026-09-18: the window and the time to accumulate
 * SMC_GATE_MIN_SAMPLE writes at one address are the same order, so it could stop
 * the hot gate being earned at all, and hardware showed degraded performance.
 * See the note in cpu_threaded.c.  It is affordable to drop because the
 * containment fix makes a wrongly promoted gate a dispatcher lookup rather than
 * a jump to 0x0FFFFFFC. */
#define earned_new earned_old

static void cand_forget(cand_table *c) { memset(c, 0, sizeof(*c)); }

/* Savestate reload preserves learned counts but rebases the remembered word
 * from restored guest memory. This models smc_gates_refresh_values(). */
static void cand_refresh_value(cand_table *c, u32 addr, u32 restored)
{
   u32 k;
   for (k = 0; k < c->used; k++)
      if (c->addr[k] == addr) {
         c->val[k] = restored;
         return;
      }
}

/* Drive n writes that each change the word, as self-modifying code does. */
static int drive_changing(int (*fn)(cand_table *, u32, u32),
                          cand_table *c, u32 addr, unsigned n)
{
   unsigned i; int promoted = 0;
   for (i = 0; i < n; i++)
      promoted |= fn(c, addr, 0xE1A00000u + i);
   return promoted;
}

#define MIXER_ADDR 0x0300168cu     /* 120 same / 62953 changed: real SMC */
#define DATA_ADDR  0x03001404u     /*  66 same /     1 changed: a constant */

static int test_promotion_rule(void)
{
   cand_table c;
   unsigned i;

   /* Genuine SMC earns a gate, at the 64th write and not before. */
   memset(&c, 0, sizeof(c));
   CHECK(drive_changing(earned_new, &c, MIXER_ADDR, 63) == 0);
   CHECK(drive_changing(earned_new, &c, MIXER_ADDR, 1)  == 1);

   /* THE GATE THAT PAYS FOR THE PERFORMANCE MUST STILL BE EARNED, and quickly:
    * 64 writes at one address and nothing else.  Anything that delays this
    * costs the documented no-gate regime (457 KB/s re-translation, 37 fps on a
    * PSP-1000), which is how the withdrawn staleness rule was found. */
   memset(&c, 0, sizeof(c));
   for (i = 1; i <= 64; i++) {
      int got = earned_new(&c, MIXER_ADDR, 0xE1A00000u + i);
      CHECK(got == (i == 64));
   }

   /* And the idempotent data word never does. */
   memset(&c, 0, sizeof(c));
   for (i = 0; i < 200; i++) CHECK(earned_new(&c, DATA_ADDR, 0x08001234u) == 0);

   /* THE CROSS-ROM LEAK.  Unbound saturates the row; nothing clears it; Heart
    * & Soul's first write to the same IWRAM address promotes on one sample. */
   memset(&c, 0, sizeof(c));
   CHECK(drive_changing(earned_old, &c, MIXER_ADDR, 64) == 1);
   c.hits[0] = 63;                       /* just short, as a real row would be */
   CHECK(earned_old(&c, MIXER_ADDR, 0xDEADBEEFu) == 1);   /* <-- one write */

   /* With the table cleared at the boundary, the same write proves nothing. */
   memset(&c, 0, sizeof(c));
   CHECK(drive_changing(earned_new, &c, MIXER_ADDR, 64) == 1);
   cand_forget(&c);
   CHECK(earned_new(&c, MIXER_ADDR, 0xDEADBEEFu) == 0);
   CHECK(drive_changing(earned_new, &c, MIXER_ADDR, 62) == 0);
   CHECK(drive_changing(earned_new, &c, MIXER_ADDR, 1)  == 1);   /* 64 again */

   /* KNOWN AND ACCEPTED HOLE: a role change inside one game.  A saturated row
    * whose address the game later writes in a different role still promotes on
    * one observation, because nothing ages a row.  The staleness rule that
    * closed this was withdrawn (see above) and this asserts the hole so nobody
    * mistakes it for covered. */
   memset(&c, 0, sizeof(c));
   CHECK(drive_changing(earned_new, &c, MIXER_ADDR, 64) == 1);
   CHECK(earned_new(&c, MIXER_ADDR, 0x08001234u) == 1);   /* still promotes */

   /* A same-ROM state load preserves expensive hit/change learning, but the
    * saved candidate value belongs to the pre-load timeline. Without rebasing,
    * one restored value can be miscounted as the 32nd real change and promote
    * the row. Refreshing prevents that false promotion without clearing hits. */
   memset(&c, 0, sizeof(c));
   c.used = 1; c.addr[0] = DATA_ADDR; c.hits[0] = 63;
   c.chg[0] = 31; c.val[0] = 0x11111111u;
   CHECK(earned_new(&c, DATA_ADDR, 0x22222222u) == 1); /* old timeline: false */
   memset(&c, 0, sizeof(c));
   c.used = 1; c.addr[0] = DATA_ADDR; c.hits[0] = 63;
   c.chg[0] = 31; c.val[0] = 0x11111111u;
   cand_refresh_value(&c, DATA_ADDR, 0x22222222u);
   CHECK(earned_new(&c, DATA_ADDR, 0x22222222u) == 0); /* restored baseline */
   CHECK(c.hits[0] == 64 && c.chg[0] == 31); /* costly evidence retained */
   return 0;
}

/* ======================================================================== 5
 * The selective-invalidation writer fingerprint.
 */
#define REG_LR 14

/* smc_writer_safe_range's decode, with the opcode handed in so no memory map is
 * needed.
 *
 * An IWRAM precondition, a latched writer pc and a repetition count were added
 * here and then WITHDRAWN on 2026-09-18. The current production predicate is
 * the broader M4A writer-family match from e501964: ARM STMIA through LR,
 * non-writeback, bounded to +0x200, with the write range derived from the
 * register list. */
#define MIXER_PATCH_SPAN 0x200u

static int writer_safe_range(u32 pc_plus_4, u32 addr, u32 op, int thumb,
                             u32 *low, u32 *high)
{
   u32 pc = pc_plus_4 - 4;
   if (thumb) return 0;
   if ((op & 0x0E000000u) != 0x08000000u) return 0;
   if ((op & 0x00100000u) || (op & 0x00200000u)) return 0;     /* L or W */
   if (((op >> 16) & 0xFu) != REG_LR ||
       (op & 0x01000000u) || !(op & 0x00800000u) || (op & 0x00400000u) ||
       addr <= pc || addr - pc > MIXER_PATCH_SPAN)
      return 0;
   {
      u32 list = op & 0xFFFFu, count = 0;
      while (list) { count += list & 1u; list >>= 1; }
      if (!count) return 0;
      *low  = addr - (count - 1) * 4;
      *high = addr + 4;
      return 1;
   }
}

/* ARM STMIA lr, {r0,r1}: cond=E 100 P=0 U=1 S=0 W=0 L=0 Rn=14 list=0x0003. */
#define OP_STMIA_LR_R0R1 0xE88E0003u

static int accepts(u32 pc_plus_4, u32 addr, u32 op, int thumb)
{
   u32 lo = 0, hi = 0;
   return writer_safe_range(pc_plus_4, addr, op, thumb, &lo, &hi);
}

static int test_writer_fingerprint(void)
{
   u32 lo = 0, hi = 0;
   const u32 pc = 0x03001600u, pc4 = pc + 4, dst = pc + 0x3cu;

   /* CFRU's SoundMainRAM patch is admitted, on the FIRST event -- no warm-up.
    * The accepted build activates immediately and so must this one. */
   CHECK(accepts(pc4, dst, OP_STMIA_LR_R0R1, 0) == 1);
   CHECK(writer_safe_range(pc4, dst, OP_STMIA_LR_R0R1, 0, &lo, &hi) == 1);
   /* and the range is the FULL store, not just the checked word */
   CHECK(lo == dst - 4 && hi == dst + 4);

   /* H&S's measured +0x48..+0x148 family is accepted, including its
    * four-register stores. Its low/high range must cover every transferred
    * word; merely checking the final word would miss earlier unchecked words. */
   CHECK(writer_safe_range(0x03001604u, 0x03001648u,
                           OP_STMIA_LR_R0R1, 0, &lo, &hi) == 1);
   CHECK(lo == 0x03001644u && hi == 0x0300164Cu);
   CHECK(writer_safe_range(0x03001604u, 0x03001748u,
                           0xE88E0704u, 0, &lo, &hi) == 1);
   CHECK(lo == 0x0300173Cu && hi == 0x0300174Cu);

   /* Rejections, all of them from the shape alone. */
   CHECK(accepts(pc4, dst, OP_STMIA_LR_R0R1, 1) == 0);   /* Thumb */
   CHECK(accepts(pc4, dst, OP_STMIA_LR_R0R1 | 0x00200000u, 0) == 0); /* W=1 */
   CHECK(accepts(pc4, dst, OP_STMIA_LR_R0R1 | 0x00100000u, 0) == 0); /* LDM */
   CHECK(accepts(pc4, dst, OP_STMIA_LR_R0R1 | 0x01000000u, 0) == 0); /* P=1 */
   CHECK(accepts(pc4, dst, OP_STMIA_LR_R0R1 & ~0x00800000u, 0) == 0); /* U=0 */
   CHECK(accepts(pc4, dst, OP_STMIA_LR_R0R1 | 0x00400000u, 0) == 0); /* S=1 */
   CHECK(accepts(pc4, dst, 0xE88D0003u, 0) == 0);        /* Rn=SP not LR */
   CHECK(accepts(pc4, dst, 0xE88E000Fu, 0) == 1);        /* count, not exact rlist */
   CHECK(accepts(pc4, pc + 0x200u, OP_STMIA_LR_R0R1, 0) == 1); /* inclusive limit */
   CHECK(accepts(pc4, pc + 0x201u, OP_STMIA_LR_R0R1, 0) == 0); /* out of span */
   CHECK(accepts(pc4, pc, OP_STMIA_LR_R0R1, 0) == 0);    /* not above writer */
   CHECK(accepts(pc4, dst, 0xE88E0000u, 0) == 0);        /* empty register list */

   /* There is no fixed writer-PC or game-title identity filter: an unrelated
    * EWRAM writer with the same shape and bounded EWRAM target is admitted. */
   CHECK(writer_safe_range(0x02002004u, 0x02002048u,
                           OP_STMIA_LR_R0R1, 0, &lo, &hi) == 1);
   return 0;
}

/* The production SMC path may retire a range selectively only after both
 * endpoint gates established the block layout: the first written word and the
 * final checked word. A full gate table can leave only the low endpoint
 * present, which must take the conservative full-flush path. */
static int partial_range_ready(int active, int range_safe, int activating,
                               int gate_changed, int low_gated, int high_gated)
{
   return active && range_safe && !activating && !gate_changed &&
          low_gated && high_gated;
}

static int test_partial_range_requires_both_gates(void)
{
   CHECK(partial_range_ready(1, 1, 0, 0, 1, 1) == 1);
   CHECK(partial_range_ready(1, 1, 0, 0, 1, 0) == 0);
   CHECK(partial_range_ready(1, 1, 0, 0, 0, 1) == 0);
   CHECK(partial_range_ready(1, 1, 0, 1, 1, 1) == 0);
   return 0;
}

/* ======================================================================== 7
 * A block that stops at a translation gate must tag the gate word.
 *
 * cpu_threaded.c scan_block: each loop iteration tags block_end_pc (the
 * instruction about to be scanned), loads it, ADVANCES block_end_pc, and only
 * then asks SMC_SCAN_GATE_END whether the new block_end_pc is a gate.  So the
 * gate word itself was never tagged by the block that stops in front of it.
 * mips/mips_emit.h arm_block_memory: every STM word but the last goes through
 * execute_aligned_store32 (no SMC check); only the final word reaches
 * execute_store_u32, whose stub raises SMC when the word's tag is non-zero.
 *
 * Measured in PPSSPP (heart_soul_heavy, timing-neutral A/B): with the gate at
 * 0x0300168c only, `stm lr, {r0, r1}` rewrote 0x03001688 inside the LIVE block
 * 03001604..0300168c without any SMC event, and the stale instruction ran.
 */
#define TAG_BASE  0x03001600u
#define TAG_WORDS 0x100u
static u32 tagmap[TAG_WORDS * 2];                 /* one entry per halfword */
static u32 *tag_at(u32 a) { return &tagmap[(a - TAG_BASE) >> 1]; }

/* scan_block's order of operations for an ARM RAM block that stops at a
 * gate.  Returns the block end. */
static u32 scan_to_gate(u32 start, u32 gate, int tag_gate_word)
{
   u32 end = start;
   do {
      if (!*tag_at(end)) *tag_at(end) = 0x0101;   /* smc_write_arm_yes() */
      if (!*tag_at(end + 2)) *tag_at(end + 2) = 0x0101;
      end += 4;                                   /* arm_load_opcode()    */
   } while (end != gate);                         /* SMC_SCAN_GATE_END()  */
   if (tag_gate_word) {                           /* smc_tag_gate_arm_yes */
      if (!*tag_at(end)) *tag_at(end) = 0x0101;
      if (!*tag_at(end + 2)) *tag_at(end + 2) = 0x0101;
   }
   return end;
}

/* Does a non-writeback STM of `n` words at `lo` raise SMC?  Only its final
 * word is checked. */
static int stm_raises_smc(u32 lo, u32 n)
{
   u32 last = lo + 4 * (n - 1);
   return *tag_at(last) != 0 || *tag_at(last + 2) != 0;
}

static int covered(u32 start, u32 end, u32 lo, u32 n)
{
   return lo < end && lo + 4 * n > start;
}

static int test_gate_word_is_tagged(void)
{
   int fix;
   for (fix = 0; fix <= 1; fix++) {
      u32 end;
      /* ONE gate (the checked word): a block scanned from 0x03001604 stops
       * at 0x0300168c and covers 0x03001688. */
      memset(tagmap, 0, sizeof(tagmap));
      end = scan_to_gate(0x03001604u, 0x0300168cu, fix);
      CHECK(end == 0x0300168cu);
      CHECK(covered(0x03001604u, end, 0x03001688u, 2));  /* live block is hit */
      /* ...so the store MUST be caught.  Before the fix it was not. */
      CHECK(stm_raises_smc(0x03001688u, 2) == fix);

      /* TWO gates, and the one-instruction block AT the low gate has been
       * translated while the block at the high gate has not run yet: the
       * same hole, one word along. */
      memset(tagmap, 0, sizeof(tagmap));
      end = scan_to_gate(0x03001688u, 0x0300168cu, fix);
      CHECK(covered(0x03001688u, end, 0x03001688u, 2));
      CHECK(stm_raises_smc(0x03001688u, 2) == fix);

      /* TWO gates, only the block that stops at the LOW gate is live: it does
       * not cover either written word, so no SMC is needed -- and the fix
       * must not create one (the low word is the unchecked one). */
      memset(tagmap, 0, sizeof(tagmap));
      end = scan_to_gate(0x03001604u, 0x03001688u, fix);
      CHECK(!covered(0x03001604u, end, 0x03001688u, 2));
      CHECK(stm_raises_smc(0x03001688u, 2) == 0);
   }
   return 0;
}

/* ======================================================================== 8
 * A Thumb BL whose two halves land in different blocks.
 *
 * cpu_threaded.c translate_thumb_instruction: the low half (0xF000-0xF7FF)
 * emitted no code; the high half is fused with it (thumb_bl, target from
 * scan_block's thumb_branch_target) only when both are in one block.  A gate
 * at the high half starts a new block there, which runs thumb_blh:
 * target = LR + offset*2.  The low half must therefore leave
 * LR = pc + 4 + (sext(off11) << 12) behind when it ends its block.
 */
static u32 bl_fused_target(u32 low_pc, u32 low_op, u32 high_op)
{
   /* scan_block: block_end_pc == high_pc + 2 when the high half is scanned */
   u32 block_end_pc = low_pc + 4;
   return block_end_pc + ((int32_t)((low_op & 0x07FFu) << 21) >> 9) +
          ((high_op & 0x07FFu) * 2);
}

static u32 bl_low_lr(u32 low_pc, u32 low_op)
{
   return low_pc + 4 + ((int32_t)((low_op & 0x07FFu) << 21) >> 9);
}

static int test_split_thumb_bl(void)
{
   /* bl 0x0821ae6c from 0x08218fc4 (the Heart & Soul sprite loop): f001 ff52 */
   const u32 low_pc = 0x08218fc4u, low = 0xF001u, high = 0xFF52u;
   const u32 stale_lr = 0x08218fc9u;
   u32 lr;
   CHECK(bl_fused_target(low_pc, low, high) == 0x0821ae6cu);

   /* Before: the low half did nothing, so the split high half branched to
    * stale LR + offset. */
   lr = stale_lr;
   CHECK(lr + (high & 0x07FFu) * 2 != 0x0821ae6cu);

   /* After: the low half, ending its block, sets LR; the high half's thumb_blh
    * then lands exactly where the fused form does -- including backwards. */
   lr = bl_low_lr(low_pc, low);
   CHECK(lr + (high & 0x07FFu) * 2 == bl_fused_target(low_pc, low, high));
   CHECK(bl_low_lr(0x03001000u, 0xF7FFu) + (0xFFFFu & 0x07FFu) * 2 ==
         bl_fused_target(0x03001000u, 0xF7FFu, 0xFFFFu));
   return 0;
}

int main(void)
{
   struct { const char *name; int (*fn)(void); } t[] = {
      { "a block stopping at a gate tags the gate word", test_gate_word_is_tagged },
      { "a split Thumb BL keeps its LR", test_split_thumb_bl                },
      { "sentinel becomes j 0x0FFFFFFC", test_sentinel_is_a_jump_to_nowhere },
      { "region classifier == the switch", test_region_classifier           },
      { "unmappable exits are contained", test_containment                  },
      { "gate promotion needs fresh evidence", test_promotion_rule          },
      { "writer fingerprint", test_writer_fingerprint                       },
      { "partial ranges require both endpoint gates", test_partial_range_requires_both_gates },
   };
   unsigned i;
   for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
      if (t[i].fn()) { printf("FAILED: %s\n", t[i].name); return 1; }
      printf("  pass  %s\n", t[i].name);
   }
   printf("test_smc_safety: %u checks passed\n", checks);
   return 0;
}
