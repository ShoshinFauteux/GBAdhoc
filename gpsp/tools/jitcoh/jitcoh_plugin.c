/* jitcoh_plugin.c -- QEMU TCG plugin: a DETERMINISTIC cache-coherency checker
 * for the GBAdhoc dynarec (docs/JIT-COHERENCY.md).
 *
 * The dynarec twin (tools/drprof, qemu-mipsel, the PSP's own MIPS emitter and
 * stubs) runs coherently: qemu fetches the bytes the emitter last stored.  A
 * PSP does not.  The Allegrex has a write-back data cache and an instruction
 * cache that does not snoop it, so a word the emitter writes reaches
 * instruction fetch only when
 *     (D) its data-cache line has been written back to memory -- by
 *         sceKernelDcacheWritebackRange / ...WritebackInvalidateAll, by the
 *         patch handler's `cache 0x1A`, or by eviction -- AND
 *     (I) no instruction-cache line still holds an older copy -- it was never
 *         fetched since the last invalidate (sceKernelIcacheInvalidateRange /
 *         ...InvalidateAll / `cache 0x08`), or it has been evicted.
 * This plugin models both caches beside the coherent run and flags every
 * executed translated-code word whose bytes, as the modelled hierarchy would
 * deliver them, differ from the bytes the emitter last wrote there.
 *
 * WHAT IT HOOKS (addresses from the twin binary's nm, passed by jc_run.sh).
 * QEMU 9.2 exposes no MIPS registers to plugins, so everything is read from
 * the stores the program makes (mem_get_value gives each stored value):
 *   - the core built -DJITCOH_PSP_CACHE compiles the PSP's own maintenance
 *     calls against four functions with the kernel's names and arguments
 *     (tools/drprof/dr_host.c) that post {addr, size, caller, op} to the
 *     mailbox `jitcoh_mbox`, op last; the op store applies the operation, so
 *     the model sees exactly the PSP's calls with the PSP's arguments;
 *   - `synci` (the twin's patch handler, where the PSP emits `cache 0x1A`
 *     then `cache 0x08` on the same address): D writeback line + I
 *     invalidate line, at the address of the patch store just before it in
 *     the same handler (same base register and offset; checked);
 *   - every store into the translation caches (value via mem_get_value);
 *   - drprof_xlat_pc (DRPROF_TWIN): the guest pc being translated;
 *   - every instruction fetch: per TB, line by line.
 *
 * MODELS, evaluated side by side on one run (one fetch stream):
 *   HAZ_I   I-cache never evicts; D coherent.  Flags exactly "the line was
 *           fetched since its last I-invalidate, then written, then the
 *           written word executed".  Layout-independent and conservative.
 *   HAZ_D   no I-cache; D-cache never evicts.  Flags a word executed while
 *           its write has never been written back (a missing writeback).
 *   HAZ_ID  both of the above at once (I never evicts, D never evicts).
 *   ACT_o<k> the real geometry: I 16 KiB 2-way 64 B LRU, with translated-code
 *           addresses moved k lines against the C code (a layout sweep: the
 *           bug is layout-sensitive); D-cache LRU 16 KiB 2-way 64 B
 *           (write-back, write-allocate) when dlru=1, else never-evict.
 *
 * For each flagged word (once per write version, per model) it prints an `E`
 * line: host pc, frame/segment, the bytes executed vs written, the writer
 * (store pc, interned call stack, guest pc of the translation in progress,
 * frame), the previous writer of the word, when the I line was filled and the
 * last invalidate that covered it.  tools/jitcoh/jc_report.py symbolises.
 *
 * Args: out=FILE  rom=ADDR:SIZE  ram=ADDR:SIZE  (translation caches, hex)
 *       mbox=                  jitcoh_mbox (the kernel-call mailbox)
 *       mark= ctl=             drprof_host_mark, drprof_ctl (frame/segment)
 *       xpc=                   drprof_xlat_pc (writer context)
 *       ioff=0.37.64           ACT instances: set offsets in lines, dot-separated (max 8)
 *       dlru=0|1               model the D-cache with LRU eviction
 *       maxev=N                E lines per model (default 4000)
 *       isize= iways= dsize= dways= line=   geometry (16384/2/16384/2/64)
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define LSH 6                    /* 64-byte lines (fixed: the Allegrex) */
#define WPL 16                   /* words per line */
#define MAXACT 8
#define SMAX 512
#define STK_K 8

/* ---------------------------------------------------------------- regions */
static uint64_t rom_lo, rom_n, ram_lo, ram_n;
static uint64_t jlo, jhi;        /* union of both caches (contiguous .jit) */
/* LAYOUT: the model works on MODEL addresses.  A translation-cache address a
 * is modelled at a + jshift (jshift=0x20 puts the twin's caches, which start
 * at ...f20, on the 64-byte line alignment the PSP's memalign(64) LARGE tier
 * has), so every line boundary the PSP sees is reproduced.  Everything else
 * (C code, stack, data) is modelled where it is. */
static uint64_t rjlo, rjhi, jshift;
static uint64_t jlo_m, jhi_m;   /* forward: the model range (== jlo/jhi) */
static inline uint64_t M(uint64_t a)
{
   if (a >= rjlo && a < rjhi)
      return a + jshift;
   if (a >= jlo_m && a < jhi_m)
      return a | 0x80000000ull;   /* not cache code: keep it out of the range */
   return a;
}
static inline uint64_t R(uint64_t m) { return (m >= jlo && m < jhi) ? m - jshift : m; }
/* RANGE SEMANTICS of the kernel's ranged calls (isem= for the I invalidate,
 * dsem= for the D writeback): which lines does (addr, size) touch?
 *   incl  every line overlapping [addr, addr+size)            (default)
 *   naive for (p = addr; p < addr+size; p += 64) line(p): misses the last
 *         line when addr is not line-aligned and the end spills into it
 *   floor for (p = addr & ~63; p < ((addr+size) & ~63); p += 64): misses a
 *         partial last line (and all of a range inside one line) */
enum { SEM_INCL, SEM_NAIVE, SEM_FLOOR, SEM_NONE };
/* THE FIRMWARE'S LARGE-RANGE PATHS (uOFW src/kd/sysmem/start.S, the PSP
 * kernel's own code).  sceKernelIcacheInvalidateRange runs the exact
 * `cache 0x8` line loop only for size < the I-cache size (16 KiB); at
 * 16 KiB or more it walks the tag RAM instead (`cache 0x0` + TAG_LO/TAG_HI
 * over 8 KiB of indices) and hit-invalidates lines whose reconstructed
 * address is in range.  sceKernelDcacheWritebackRange likewise switches to a
 * tag walk at 64 KiB.  ibig=noop / dbig=noop model "that path does nothing"
 * (the hypothesis under test); every large call is logged (`B` lines) either
 * way, with its caller. */
static int ibig_noop, dbig_noop;
static uint64_t n_ibig, n_dbig, ihist[33], dhist[33];
static int isem, dsem;
static uint64_t sem_lost_lines[2];   /* lines incl would touch, sem does not */
static int parse_sem(const char *v)
{
   return !strcmp(v, "naive") ? SEM_NAIVE : !strcmp(v, "floor") ? SEM_FLOOR : SEM_INCL;
}
/* lines [*l0, *l1] (inclusive) touched by (a, n) under semantics sem;
 * returns 0 when none */
static int sem_lines(int sem, uint64_t a, uint64_t n, uint64_t *l0, uint64_t *l1, int side)
{
   uint64_t i0 = a >> 6, i1 = (a + n - 1) >> 6;
   if (!n)
      return 0;
   *l0 = i0;
   if (sem == SEM_NONE)
   {
      sem_lost_lines[side] += i1 - i0 + 1;
      return 0;
   }
   if (sem == SEM_NAIVE)
      *l1 = i0 + ((n + 63) >> 6) - 1;
   else if (sem == SEM_FLOOR)
   {
      if (((a + n) >> 6) == i0)
      {
         sem_lost_lines[side] += i1 - i0 + 1;
         return 0;
      }
      *l1 = ((a + n) >> 6) - 1;
   }
   else
      *l1 = i1;
   if (*l1 > i1)
      *l1 = i1;
   sem_lost_lines[side] += (i1 - *l1);
   return 1;
}
static uint32_t nwords, nlines;

static inline int in_jit(uint64_t a) { return a >= jlo && a < jhi; }

/* --------------------------------------------------------- per-word state */
typedef struct
{
   uint32_t cur;     /* the bytes the emitter last wrote (= what qemu runs)   */
   uint32_t memN;    /* main memory, D-cache never evicting                   */
   uint32_t memL;    /* main memory, D-cache LRU (dlru=1)                     */
   uint32_t ivI;     /* HAZ_I  I-line copy                                    */
   uint32_t ivID;    /* HAZ_ID I-line copy                                    */
   uint32_t wpc, wstk, wgpc, wframe, wstamp;   /* last writer                 */
   uint32_t opc, ogpc, oframe, oval;           /* previous writer (value chg) */
   uint32_t cstamp;                            /* last store that CHANGED it */
} word_t;
static word_t *W;

typedef struct
{
   uint8_t vI, vID, dirtyN, inlist;
   uint32_t fill_stamp, fill_frame;           /* HAZ_I fill                   */
   uint32_t inv_stamp, inv_frame, inv_ra, inv_a, inv_n, inv_kind, inv_stk;
   /* the last ranged I invalidate whose range OVERLAPPED this line but that,
    * under isem, did not invalidate it */
   uint32_t lost_stamp, lost_frame, lost_ra, lost_stk, lost_a, lost_n;
} line_t;
static line_t *L;
static uint32_t *dirty_list, ndirty;           /* memN dirty lines            */

enum { K_NONE, K_IRANGE, K_IALL, K_SYNCI };
static const char *kind_name[] = { "none", "IcacheInvalidateRange",
   "IcacheInvalidateAll", "synci" };
static struct { uint32_t stamp, frame, ra, stk; } inv_all_rec;

/* -------------------------------------------------------- clocks/context */
static uint32_t stamp;           /* JIT stores so far (orders every event)    */
static uint32_t frame, seg;
static uint64_t ctl_addr, mark_addr;

/* ------------------------------------------------------ shadow call stack */
static uint32_t scall[SMAX], sret[SMAX];
static int sp;
static uint32_t sgen = 1, stk_cached_gen, stk_cached_id;
static uint32_t cur_gpc;         /* drprof_xlat_pc: 0 = not translating */
static uint64_t xpc_addr, mbox_addr;
static uint32_t mbox[4];          /* addr, size, caller, op */

typedef struct { uint32_t a[STK_K]; } stk_t;
static GArray *stks;             /* id -> stk_t */
static GHashTable *stk_ids;      /* stk_t bytes -> id+1 */

static guint stk_hash(gconstpointer p)
{
   const uint32_t *a = p;
   uint32_t h = 2166136261u;
   int i;
   for (i = 0; i < STK_K; i++)
      h = (h ^ a[i]) * 16777619u;
   return h;
}
static gboolean stk_eq(gconstpointer x, gconstpointer y)
{
   return !memcmp(x, y, sizeof(stk_t));
}

static uint32_t stack_id(void)
{
   stk_t k;
   int i;
   gpointer v;
   if (stk_cached_gen == sgen)
      return stk_cached_id;
   memset(&k, 0, sizeof(k));
   for (i = 0; i < STK_K && sp - 1 - i >= 0; i++)
      k.a[i] = scall[sp - 1 - i];
   v = g_hash_table_lookup(stk_ids, &k);
   if (!v)
   {
      stk_t *copy = g_new(stk_t, 1);
      *copy = k;
      g_array_append_val(stks, k);
      v = GUINT_TO_POINTER(stks->len);
      g_hash_table_insert(stk_ids, copy, v);
   }
   stk_cached_gen = sgen;
   stk_cached_id = GPOINTER_TO_UINT(v) - 1;
   return stk_cached_id;
}

static void on_call(unsigned int vcpu, void *ud)
{
   uint32_t site = (uint32_t)(uintptr_t)ud;
   (void)vcpu;
   if (sp == SMAX)
   {
      memmove(scall, scall + 128, (SMAX - 128) * 4);
      memmove(sret, sret + 128, (SMAX - 128) * 4);
      sp -= 128;
   }
   scall[sp] = site;
   sret[sp] = site + 8;
   sp++;
   sgen++;
}

static inline void stack_on_tb(uint32_t pc)
{
   int i, lim = sp - 12;
   if (lim < 0) lim = 0;
   for (i = sp - 1; i >= lim; i--)
      if (sret[i] == pc)
      {
         sp = i;
         sgen++;
         return;
      }
}

/* -------------------------------------------------------------- D models */
static int dlru;
static uint64_t n_dwr, n_iir, n_dwia, n_iia, n_synci, n_synci_unres, n_jstores,
                n_stores_bytes;
static uint32_t last_jstore_pc;   /* for synci */
static uint64_t last_jstore_va;   /* model address */
static uint64_t shadow_mismatch, shadow_checked;

/* memN: never evicts.  Only explicit writebacks reach memory. */
static inline void memN_dirty(uint32_t li)
{
   if (!L[li].dirtyN)
   {
      L[li].dirtyN = 1;
      if (!L[li].inlist)
      {
         L[li].inlist = 1;
         dirty_list[ndirty++] = li;
      }
   }
}
static inline void memN_wb_line(uint32_t li)
{
   if (L[li].dirtyN)
   {
      uint32_t w = li * WPL, k;
      for (k = 0; k < WPL; k++)
         W[w + k].memN = W[w + k].cur;
      L[li].dirtyN = 0;
   }
}

/* D LRU (dlru=1): every data access goes through it. */
typedef struct
{
   unsigned nsets, ways;
   uint32_t *tag;               /* line+1 */
   uint8_t *dirty;
   uint64_t *age;
} dcache_t;
static dcache_t DC;
static uint64_t dtick;
static unsigned dsize = 16384, dways = 2, isize = 16384, iways = 2;

static inline void memL_wb_line_addr(uint32_t line)
{
   uint64_t a = (uint64_t)line << LSH;
   if (in_jit(a))
   {
      uint32_t w = (uint32_t)((a - jlo) >> 2), k;
      for (k = 0; k < WPL; k++)
         W[w + k].memL = W[w + k].cur;
   }
}

static inline int dc_find(uint32_t line, unsigned *set_out)
{
   unsigned set = line % DC.nsets, w;
   *set_out = set;
   for (w = 0; w < DC.ways; w++)
      if (DC.tag[set * DC.ways + w] == line + 1)
         return (int)w;
   return -1;
}

static void dc_access(uint64_t addr, int store)
{
   uint32_t line = (uint32_t)(addr >> LSH);
   unsigned set, w, victim = 0;
   int hit = dc_find(line, &set);
   uint64_t oldest = UINT64_MAX;
   dtick++;
   if (hit >= 0)
   {
      DC.age[set * DC.ways + hit] = dtick;
      if (store) DC.dirty[set * DC.ways + hit] = 1;
      return;
   }
   for (w = 0; w < DC.ways; w++)
      if (DC.age[set * DC.ways + w] < oldest)
      {
         oldest = DC.age[set * DC.ways + w];
         victim = w;
      }
   w = set * DC.ways + victim;
   if (DC.tag[w] && DC.dirty[w])
      memL_wb_line_addr(DC.tag[w] - 1);
   DC.tag[w] = line + 1;
   DC.dirty[w] = (uint8_t)store;
   DC.age[w] = dtick;
}

static void dc_wb_line(uint32_t line)
{
   unsigned set;
   int w = dc_find(line, &set);
   if (w >= 0 && DC.dirty[set * DC.ways + w])
   {
      memL_wb_line_addr(line);
      DC.dirty[set * DC.ways + w] = 0;
   }
}

/* -------------------------------------------------------------- I models */
typedef struct
{
   int off;                     /* set offset (lines) for translated code */
   int jitonly;                 /* ioff token "<n>j": C code never evicts */
   unsigned nsets, ways;
   uint32_t *tag;               /* line+1 */
   uint64_t *age;
   uint32_t *snap;              /* WPL words per way, translated-code lines */
   uint64_t tick, acc, miss;
} icache_t;
static icache_t IC[MAXACT];
static int nact;

static inline unsigned ic_set(icache_t *c, uint32_t line)
{
   uint64_t a = (uint64_t)line << LSH;
   return in_jit(a) ? (unsigned)((line + c->off) % c->nsets) : line % c->nsets;
}

static inline int ic_find(icache_t *c, uint32_t line, unsigned set)
{
   unsigned w;
   for (w = 0; w < c->ways; w++)
      if (c->tag[set * c->ways + w] == line + 1)
         return (int)w;
   return -1;
}

/* returns the slot index (set*ways+way) holding the line after the access */
static inline unsigned ic_access(icache_t *c, uint32_t line, int *filled)
{
   unsigned set = ic_set(c, line), w, victim = 0, slot;
   int hit = ic_find(c, line, set);
   uint64_t oldest = UINT64_MAX;
   c->tick++;
   c->acc++;
   if (hit >= 0)
   {
      slot = set * c->ways + hit;
      c->age[slot] = c->tick;
      *filled = 0;
      return slot;
   }
   c->miss++;
   for (w = 0; w < c->ways; w++)
      if (c->age[set * c->ways + w] < oldest)
      {
         oldest = c->age[set * c->ways + w];
         victim = w;
      }
   slot = set * c->ways + victim;
   c->tag[slot] = line + 1;
   c->age[slot] = c->tick;
   *filled = 1;
   if (in_jit((uint64_t)line << LSH))
   {
      uint32_t w0 = (uint32_t)((((uint64_t)line << LSH) - jlo) >> 2), k;
      for (k = 0; k < WPL; k++)
         c->snap[slot * WPL + k] = dlru ? W[w0 + k].memL : W[w0 + k].memN;
   }
   return slot;
}

static void ic_inv_line(icache_t *c, uint32_t line)
{
   unsigned set = ic_set(c, line);
   int w = ic_find(c, line, set);
   if (w >= 0)
   {
      c->tag[set * c->ways + w] = 0;
      c->age[set * c->ways + w] = 0;
   }
}

/* ------------------------------------------------------------- reporting */
enum { V_I, V_D, V_ID, V_S, V_ACT0 };
#define NV (V_ACT0 + MAXACT)
static char vname[NV][24];
static int nv;
static uint64_t v_exec[NV], v_incid[NV], v_ev[NV];
static uint32_t *v_rep[NV];      /* per word: wstamp+1 last reported */
static GHashTable *v_bywriter[NV];   /* (wpc<<32 | wstk) -> count of incidents */
static GHashTable *v_bylost[NV];     /* (lost_ra<<32 | lost_stk) -> incidents
                                        whose line a ranged invalidate missed
                                        AFTER the write */
static uint64_t maxev = 4000;
static FILE *outf;

static const char *region_of(uint64_t a, uint64_t *off)   /* model address */
{
   if (a >= rom_lo && a < rom_lo + rom_n) { *off = a - rom_lo; return "rom"; }
   if (a >= ram_lo && a < ram_lo + ram_n) { *off = a - ram_lo; return "ram"; }
   *off = a - jlo;
   return "jit";
}

static void flag(int v, uint32_t wi, uint32_t got, uint32_t fill_frame)
{
   word_t *x = &W[wi];
   uint32_t li = wi / WPL;
   line_t *l = &L[li];
   uint64_t a = jlo + (uint64_t)wi * 4, off;   /* model address */
   const char *rg;
   v_exec[v]++;
   if (v_rep[v][wi] == x->wstamp + 1)
      return;
   v_rep[v][wi] = x->wstamp + 1;
   v_incid[v]++;
   {
      uint64_t key = ((uint64_t)x->wpc << 32) | x->wstk;
      gpointer old = g_hash_table_lookup(v_bywriter[v], (gpointer)(uintptr_t)key);
      g_hash_table_insert(v_bywriter[v], (gpointer)(uintptr_t)key,
                          GSIZE_TO_POINTER(GPOINTER_TO_SIZE(old) + 1));
      if (l->lost_stamp >= x->wstamp && l->lost_stamp)
      {
         key = ((uint64_t)l->lost_ra << 32) | l->lost_stk;
         old = g_hash_table_lookup(v_bylost[v], (gpointer)(uintptr_t)key);
         g_hash_table_insert(v_bylost[v], (gpointer)(uintptr_t)key,
                             GSIZE_TO_POINTER(GPOINTER_TO_SIZE(old) + 1));
      }
   }
   if (v_ev[v]++ >= maxev)
      return;
   rg = region_of(a, &off);
   {
      /* the last invalidate that covered this line: ranged/synci or all */
      uint32_t is = l->inv_stamp, ifr = l->inv_frame, ira = l->inv_ra,
               ik = l->inv_kind, ia = l->inv_a, in = l->inv_n, ist = l->inv_stk;
      if (inv_all_rec.stamp >= is && inv_all_rec.frame)
      {
         is = inv_all_rec.stamp; ifr = inv_all_rec.frame; ira = inv_all_rec.ra;
         ik = K_IALL; ia = 0; in = 0; ist = inv_all_rec.stk;
      }
      fprintf(outf, "E v=%s pc=%08" PRIx64 " %s+%" PRIx64 " f=%u s=%u cur=%08x got=%08x"
              " wpc=%08x wstk=%u wgpc=%08x wf=%u wst=%u opc=%08x ogpc=%08x of=%u oval=%08x"
              " fillf=%u invk=%s invf=%u invst=%u invra=%08x invstk=%u inva=%08x invn=%x"
              " lostst=%u lostf=%u lostra=%08x loststk=%u losta=%08x lostn=%x st=%u\n",
              vname[v], R(a), rg, off, frame, seg, x->cur, got, x->wpc, x->wstk,
              x->wgpc, x->wframe, x->wstamp, x->opc, x->ogpc, x->oframe, x->oval,
              fill_frame, kind_name[ik], ifr, is, ira, ist, ia, in,
              l->lost_stamp, l->lost_frame, l->lost_ra, l->lost_stk, l->lost_a, l->lost_n,
              stamp);
   }
}

/* ------------------------------------------------------------ cache ops */
static void op_inv_range(uint64_t a, uint64_t n, uint32_t ra, int kind, int sem)
{
   uint64_t l0, l1, ln;
   int k, any;
   if (!n)
      return;
   any = sem_lines(sem, a, n, &l0, &l1, 0);
   /* lines the range overlaps that the semantics leaves alone */
   for (ln = a >> LSH; ln <= (a + n - 1) >> LSH; ln++)
   {
      uint64_t la = ln << LSH;
      if ((!any || ln < l0 || ln > l1) && in_jit(la))
      {
         line_t *l = &L[(la - jlo) >> LSH];
         l->lost_stamp = stamp;
         l->lost_frame = frame;
         l->lost_ra = ra;
         l->lost_stk = stack_id();
         l->lost_a = (uint32_t)R(a);
         l->lost_n = (uint32_t)n;
      }
   }
   if (!any)
      return;
   for (ln = l0; ln <= l1; ln++)
   {
      uint64_t la = ln << LSH;
      for (k = 0; k < nact; k++)
         ic_inv_line(&IC[k], (uint32_t)ln);
      if (in_jit(la))
      {
         line_t *l = &L[(la - jlo) >> LSH];
         l->vI = l->vID = 0;
         l->inv_stamp = stamp;
         l->inv_frame = frame;
         l->inv_ra = ra;
         l->inv_a = (uint32_t)R(a);
         l->inv_n = (uint32_t)n;
         l->inv_kind = (uint32_t)kind;
         l->inv_stk = stack_id();
      }
   }
}

static void op_inv_all(uint32_t ra)
{
   uint32_t i;
   int k;
   for (i = 0; i < nlines; i++)
      L[i].vI = L[i].vID = 0;
   for (k = 0; k < nact; k++)
   {
      memset(IC[k].tag, 0, IC[k].nsets * IC[k].ways * 4);
      memset(IC[k].age, 0, IC[k].nsets * IC[k].ways * 8);
   }
   inv_all_rec.stamp = stamp;
   inv_all_rec.frame = frame ? frame : 1;
   inv_all_rec.ra = ra;
   inv_all_rec.stk = stack_id();
}

static void op_wb_range(uint64_t a, uint64_t n, int sem)
{
   uint64_t l0, l1, ln;
   if (!sem_lines(sem, a, n, &l0, &l1, 1))
      return;
   for (ln = l0; ln <= l1; ln++)
   {
      uint64_t la = ln << LSH;
      if (in_jit(la))
         memN_wb_line((uint32_t)((la - jlo) >> LSH));
      if (dlru)
         dc_wb_line((uint32_t)ln);
   }
}

static void op_wbinv_all(void)
{
   uint32_t i;
   for (i = 0; i < ndirty; i++)
   {
      memN_wb_line(dirty_list[i]);
      L[dirty_list[i]].inlist = 0;
   }
   ndirty = 0;
   if (dlru)
   {
      unsigned s;
      for (s = 0; s < DC.nsets * DC.ways; s++)
      {
         if (DC.tag[s] && DC.dirty[s])
            memL_wb_line_addr(DC.tag[s] - 1);
         DC.tag[s] = 0;
         DC.dirty[s] = 0;
      }
   }
}

static uint64_t n_biglog;
static void big_log(char side, uint32_t a, uint32_t n, uint32_t ra)
{
   if (n_biglog++ < 4000)
      fprintf(outf, "B %c a=%08x n=%x f=%u s=%u ra=%08x stk=%u st=%u\n", side, a, n,
              frame, seg, ra, stack_id(), stamp);
}

/* the mailbox op store: apply the PSP call the core just made */
static void mbox_op(uint32_t op)
{
   switch (op)
   {
   case 1:
      n_dwr++;
      dhist[mbox[1] ? 32 - __builtin_clz(mbox[1]) : 0]++;
      if (mbox[1] >= 65536)
      {
         n_dbig++;
         big_log('D', mbox[0], mbox[1], mbox[2]);
      }
      op_wb_range(M(mbox[0]), mbox[1], (dbig_noop && mbox[1] >= 65536) ? SEM_NONE : dsem);
      break;
   case 2:
      n_iir++;
      ihist[mbox[1] ? 32 - __builtin_clz(mbox[1]) : 0]++;
      if (mbox[1] >= 16384)
      {
         n_ibig++;
         big_log('I', mbox[0], mbox[1], mbox[2]);
      }
      op_inv_range(M(mbox[0]), mbox[1], mbox[2], K_IRANGE,
                   (ibig_noop && mbox[1] >= 16384) ? SEM_NONE : isem);
      break;
   case 3: n_dwia++; op_wbinv_all(); break;
   case 4: n_iia++;  op_inv_all(mbox[2]); break;
   }
}

/* synci: the patch handler's `sw t, off(ra)` then `synci off(ra)`.  udata is
 * that store instruction's address (resolved at translation); the store's
 * data address was captured when it executed. */
static void on_synci(unsigned int vcpu, void *ud)
{
   uint32_t spc = (uint32_t)(uintptr_t)ud;
   (void)vcpu;
   n_synci++;
   if (!spc || spc != last_jstore_pc)
   {
      n_synci_unres++;
      return;
   }
   op_wb_range(last_jstore_va & ~63ull, 64, SEM_INCL);
   op_inv_range(last_jstore_va & ~63ull, 64, spc, K_SYNCI, SEM_INCL);
}

static void on_mark(unsigned int vcpu, void *ud)
{
   uint8_t b[20];
   GByteArray *ba = g_byte_array_sized_new(20);
   (void)vcpu; (void)ud;
   if (qemu_plugin_read_memory_vaddr(ctl_addr, ba, 20) && ba->len >= 20)
   {
      memcpy(b, ba->data, 20);
      memcpy(&frame, b + 12, 4);
      memcpy(&seg, b + 16, 4);
   }
   g_byte_array_free(ba, TRUE);
}

/* --------------------------------------------------------------- stores */
static void on_mem(unsigned int vcpu, qemu_plugin_meminfo_t info,
                   uint64_t va, void *ud)
{
   unsigned sz;
   (void)vcpu;
   if (mbox_addr && va - mbox_addr < 16)
      ;                                  /* real address, below */
   else
      va = M(va);
   if (dlru)
      dc_access(va, qemu_plugin_mem_is_store(info));
   if (!qemu_plugin_mem_is_store(info))
      return;
   if (!in_jit(va))
   {
      if (va - mbox_addr < 16 && mbox_addr)
      {
         qemu_plugin_mem_value mv = qemu_plugin_mem_get_value(info);
         unsigned f = (unsigned)((va - mbox_addr) >> 2);
         mbox[f] = mv.data.u32;
         if (f == 3)
            mbox_op(mv.data.u32);
      }
      else if (va == xpc_addr && xpc_addr)
         cur_gpc = qemu_plugin_mem_get_value(info).data.u32;
      return;
   }
   last_jstore_pc = (uint32_t)(uintptr_t)ud;
   last_jstore_va = va;
   sz = 1u << qemu_plugin_mem_size_shift(info);
   {
      qemu_plugin_mem_value mv = qemu_plugin_mem_get_value(info);
      uint64_t val = 0;
      unsigned i;
      switch (mv.type)
      {
      case QEMU_PLUGIN_MEM_VALUE_U8:  val = mv.data.u8; break;
      case QEMU_PLUGIN_MEM_VALUE_U16: val = mv.data.u16; break;
      case QEMU_PLUGIN_MEM_VALUE_U32: val = mv.data.u32; break;
      case QEMU_PLUGIN_MEM_VALUE_U64: val = mv.data.u64; break;
      default: val = mv.data.u128.low; break;
      }
      n_stores_bytes += sz;
      /* byte-wise merge (handles sb/sh/sw/sdc1 and the byte stores swl/swr
       * become); one writer record per touched word */
      for (i = 0; i < sz; i++)
      {
         uint64_t ba = va + i;
         uint32_t wi, sh;
         word_t *x;
         uint32_t nv_;
         if (!in_jit(ba))
            continue;
         wi = (uint32_t)((ba - jlo) >> 2);
         sh = (uint32_t)(ba & 3) * 8;
         x = &W[wi];
         nv_ = (x->cur & ~(0xFFu << sh)) | ((uint32_t)((val >> (i * 8)) & 0xFF) << sh);
         if (i == 0 || (ba & 3) == 0)
         {
            stamp++;
            n_jstores++;
            if (nv_ != x->cur || x->wpc == 0)
            {
               x->opc = x->wpc; x->ogpc = x->wgpc; x->oframe = x->wframe;
               x->oval = x->cur;
               x->cstamp = stamp;
            }
            x->wpc = (uint32_t)(uintptr_t)ud;
            x->wstk = stack_id();
            x->wgpc = cur_gpc ? cur_gpc : 0xFFFFFFFFu;
            x->wframe = frame;
            x->wstamp = stamp;
            memN_dirty(wi / WPL);
         }
         x->cur = nv_;
      }
   }
}

/* ---------------------------------------------------------------- fetch */
typedef struct
{
   uint32_t pc;                 /* TB start (for the call-stack pops) */
   uint16_t nrun, jit;
   uint32_t *run;               /* jit: (word index << 5 | count) per line;
                                   other: line numbers */
} tbrec;

static void fetch_jit_run(uint32_t w0, unsigned cnt)
{
   uint32_t li = w0 / WPL, k;
   line_t *l = &L[li];
   uint32_t fillI;
   /* HAZ_I: infinite I-cache over a coherent D side */
   if (!l->vI)
   {
      uint32_t b = li * WPL;
      for (k = 0; k < WPL; k++)
         W[b + k].ivI = W[b + k].cur;
      l->vI = 1;
      l->fill_stamp = stamp;
      l->fill_frame = frame;
   }
   fillI = l->fill_frame;
   if (!l->vID)
   {
      uint32_t b = li * WPL;
      for (k = 0; k < WPL; k++)
         W[b + k].ivID = W[b + k].memN;
      l->vID = 1;
   }
   for (k = 0; k < cnt; k++)
   {
      word_t *x = &W[w0 + k];
      if (x->ivI != x->cur)
         flag(V_I, w0 + k, x->ivI, fillI);
      if (x->memN != x->cur)
         flag(V_D, w0 + k, x->memN, 0);
      if (x->ivID != x->cur)
         flag(V_ID, w0 + k, x->ivID, 0);
      /* HAZ_S: written after the line's last I invalidate, whether or not the
       * model saw the line fetched (any fill -- prefetch, a fetch down a
       * branch path not taken -- could have made it stale) */
      if (x->cstamp > l->inv_stamp && x->cstamp > inv_all_rec.stamp &&
          x->wpc && x->cur != x->oval)
         flag(V_S, w0 + k, x->oval, 0);
   }
   {
      int c;
      uint32_t line = (uint32_t)((jlo >> LSH) + li);
      for (c = 0; c < nact; c++)
      {
         int filled;
         unsigned slot = ic_access(&IC[c], line, &filled);
         uint32_t *sn = &IC[c].snap[slot * WPL];
         unsigned o = (w0 % WPL);
         for (k = 0; k < cnt; k++)
            if (sn[o + k] != W[w0 + k].cur)
               flag(V_ACT0 + c, w0 + k, sn[o + k], 0);
      }
   }
}

static uint32_t last_tb_pc;       /* for a crash: where the guest was */
static void on_tb(unsigned int vcpu, void *ud)
{
   tbrec *r = ud;
   unsigned i;
   (void)vcpu;
   last_tb_pc = r->pc;
   if (sp)
      stack_on_tb(r->pc);
   if (r->jit)
   {
      for (i = 0; i < r->nrun; i++)
         fetch_jit_run(r->run[i] >> 5, r->run[i] & 31);
   }
   else
   {
      int c;
      for (c = 0; c < nact; c++)
         for (i = 0; i < r->nrun && !IC[c].jitonly; i++)
         {
            int filled;
            ic_access(&IC[c], r->run[i], &filled);
         }
   }
}


static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
   size_t n = qemu_plugin_tb_n_insns(tb), i;
   tbrec *r = g_new0(tbrec, 1);
   uint64_t pc0 = qemu_plugin_tb_vaddr(tb);
   uint32_t last = UINT32_MAX;
   (void)id;
   r->pc = (uint32_t)pc0;                 /* real: the call-stack pops */
   r->jit = (uint16_t)in_jit(M(pc0));
   r->run = g_new0(uint32_t, n);
   for (i = 0; i < n; i++)
   {
      struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
      uint64_t va = qemu_plugin_insn_vaddr(insn);
      uint32_t op = 0;
      qemu_plugin_insn_data(insn, &op, 4);

      /* every store (and, with dlru, every load) */
      qemu_plugin_register_vcpu_mem_cb(insn, on_mem, QEMU_PLUGIN_CB_NO_REGS,
                                       dlru ? QEMU_PLUGIN_MEM_RW : QEMU_PLUGIN_MEM_W,
                                       (void *)(uintptr_t)va);
      /* calls: jal, jalr, bltzal/bgezal(+l) -- the writer back-traces */
      if ((op >> 26) == 3 || ((op >> 26) == 0 && (op & 0x3F) == 9) ||
          ((op >> 26) == 1 && (((op >> 16) & 0x1F) == 0x10 || ((op >> 16) & 0x1F) == 0x11 ||
                               ((op >> 16) & 0x1F) == 0x12 || ((op >> 16) & 0x1F) == 0x13)))
         qemu_plugin_register_vcpu_insn_exec_cb(insn, on_call, QEMU_PLUGIN_CB_NO_REGS,
                                                (void *)(uintptr_t)va);
      /* synci: REGIMM rt=0x1F.  Find the store with the same base register
       * and offset earlier in this TB (the patch store it publishes). */
      if ((op >> 26) == 1 && ((op >> 16) & 0x1F) == 0x1F)
      {
         uint32_t spc = 0;
         size_t j;
         for (j = i; j-- > 0;)
         {
            uint32_t o2 = 0;
            struct qemu_plugin_insn *in2 = qemu_plugin_tb_get_insn(tb, j);
            qemu_plugin_insn_data(in2, &o2, 4);
            if (((o2 >> 26) == 0x2b || (o2 >> 26) == 0x29 || (o2 >> 26) == 0x28) &&
                ((o2 >> 21) & 0x1F) == ((op >> 21) & 0x1F) &&
                (o2 & 0xFFFF) == (op & 0xFFFF))
            {
               spc = (uint32_t)qemu_plugin_insn_vaddr(in2);
               break;
            }
         }
         qemu_plugin_register_vcpu_insn_exec_cb(insn, on_synci, QEMU_PLUGIN_CB_NO_REGS,
                                                (void *)(uintptr_t)spc);
      }
      if (va == mark_addr)
         qemu_plugin_register_vcpu_insn_exec_cb(insn, on_mark, QEMU_PLUGIN_CB_NO_REGS, NULL);

      if (in_jit(M(va)))
      {
         uint32_t wi = (uint32_t)((M(va) - jlo) >> 2);
         shadow_checked++;
         if (W[wi].cur != op)
            shadow_mismatch++;
         if (r->nrun && (r->run[r->nrun - 1] >> 5) / WPL == wi / WPL &&
             (r->run[r->nrun - 1] >> 5) + (r->run[r->nrun - 1] & 31) == wi)
            r->run[r->nrun - 1]++;
         else
            r->run[r->nrun++] = (wi << 5) | 1;
      }
      else if (nact)
      {
         uint32_t line = (uint32_t)(va >> LSH);
         if (line != last)
            r->run[r->nrun++] = line;
         last = line;
      }
   }
   qemu_plugin_register_vcpu_tb_exec_cb(tb, on_tb, QEMU_PLUGIN_CB_NO_REGS, r);
}

/* ------------------------------------------------------------------ exit */
static gint cmp_desc(gconstpointer a, gconstpointer b)
{
   const uint64_t *x = a, *y = b;
   return x[1] < y[1] ? 1 : x[1] > y[1] ? -1 : 0;
}

static void at_exit(qemu_plugin_id_t id, void *p)
{
   int v, k;
   guint i;
   (void)id; (void)p;
   fprintf(outf, "# frames=%u seg=%u jit_store_words=%" PRIu64 " jit_store_bytes=%" PRIu64
           " DcacheWritebackRange=%" PRIu64 " IcacheInvalidateRange=%" PRIu64
           " DcacheWritebackInvalidateAll=%" PRIu64 " IcacheInvalidateAll=%" PRIu64
           " synci=%" PRIu64 " synci_unresolved=%" PRIu64 " shadow_checked=%" PRIu64
           " shadow_mismatch=%" PRIu64 " sem_lost_I=%" PRIu64 " sem_lost_D=%" PRIu64 "\n",
           frame, seg, n_jstores, n_stores_bytes, n_dwr, n_iir, n_dwia, n_iia, n_synci,
           n_synci_unres, shadow_checked, shadow_mismatch, sem_lost_lines[0],
           sem_lost_lines[1]);
   fprintf(outf, "# last_tb_pc=%08x xlat_pc=%08x stack=%u\n", last_tb_pc, cur_gpc,
           stack_id());
   fprintf(outf, "# big_ranges I(>=16KiB)=%" PRIu64 " D(>=64KiB)=%" PRIu64
           " ibig=%s dbig=%s\n# I range size log2 histogram:", n_ibig, n_dbig,
           ibig_noop ? "noop" : "ok", dbig_noop ? "noop" : "ok");
   for (k = 0; k < 33; k++)
      if (ihist[k])
         fprintf(outf, " <2^%d:%" PRIu64, k, ihist[k]);
   fprintf(outf, "\n# D range size log2 histogram:");
   for (k = 0; k < 33; k++)
      if (dhist[k])
         fprintf(outf, " <2^%d:%" PRIu64, k, dhist[k]);
   fprintf(outf, "\n");
   for (k = 0; k < nact; k++)
      fprintf(outf, "# icache %s acc=%" PRIu64 " miss=%" PRIu64 "\n", vname[V_ACT0 + k],
              IC[k].acc, IC[k].miss);
   for (v = 0; v < nv; v++)
   {
      GHashTableIter it;
      gpointer key, val;
      GArray *rows = g_array_new(FALSE, FALSE, sizeof(uint64_t) * 2);
      fprintf(outf, "V %s stale_exec=%" PRIu64 " incidents=%" PRIu64 "\n", vname[v],
              v_exec[v], v_incid[v]);
      g_hash_table_iter_init(&it, v_bywriter[v]);
      while (g_hash_table_iter_next(&it, &key, &val))
      {
         uint64_t row[2] = { (uint64_t)(uintptr_t)key, GPOINTER_TO_SIZE(val) };
         g_array_append_vals(rows, row, 1);
      }
      g_array_sort(rows, cmp_desc);
      for (i = 0; i < rows->len && i < 60; i++)
      {
         uint64_t *row = &g_array_index(rows, uint64_t, i * 2);
         fprintf(outf, "W %s wpc=%08x wstk=%u incidents=%" PRIu64 "\n", vname[v],
                 (uint32_t)(row[0] >> 32), (uint32_t)row[0], row[1]);
      }
      g_array_free(rows, TRUE);
      rows = g_array_new(FALSE, FALSE, sizeof(uint64_t) * 2);
      g_hash_table_iter_init(&it, v_bylost[v]);
      while (g_hash_table_iter_next(&it, &key, &val))
      {
         uint64_t row[2] = { (uint64_t)(uintptr_t)key, GPOINTER_TO_SIZE(val) };
         g_array_append_vals(rows, row, 1);
      }
      g_array_sort(rows, cmp_desc);
      for (i = 0; i < rows->len && i < 40; i++)
      {
         uint64_t *row = &g_array_index(rows, uint64_t, i * 2);
         fprintf(outf, "L %s lostra=%08x loststk=%u incidents=%" PRIu64 "\n", vname[v],
                 (uint32_t)(row[0] >> 32), (uint32_t)row[0], row[1]);
      }
      g_array_free(rows, TRUE);
   }
   for (i = 0; i < stks->len; i++)
   {
      stk_t *s = &g_array_index(stks, stk_t, i);
      fprintf(outf, "S %u", i);
      for (k = 0; k < STK_K && s->a[k]; k++)
         fprintf(outf, " %08x", s->a[k]);
      fprintf(outf, "\n");
   }
   fclose(outf);
}

static int parse_range(const char *s, uint64_t *lo, uint64_t *n)
{
   unsigned long long a, b;
   if (sscanf(s, "%llx:%llx", &a, &b) != 2)
      return -1;
   *lo = a;
   *n = b;
   return 0;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
   const char *out = NULL, *ioff = "";
   int i, k;
   (void)info;
   for (i = 0; i < argc; i++)
   {
      const char *a = argv[i];
#define HX(pfx, var) else if (!strncmp(a, pfx, strlen(pfx))) var = strtoull(a + strlen(pfx), NULL, 16)
      if (!strncmp(a, "out=", 4)) out = a + 4;
      else if (!strncmp(a, "rom=", 4)) parse_range(a + 4, &rom_lo, &rom_n);
      else if (!strncmp(a, "ram=", 4)) parse_range(a + 4, &ram_lo, &ram_n);
      HX("mbox=", mbox_addr); HX("mark=", mark_addr); HX("ctl=", ctl_addr);
      HX("xpc=", xpc_addr);
      else if (!strncmp(a, "ioff=", 5)) ioff = a + 5;
      else if (!strncmp(a, "dlru=", 5)) dlru = atoi(a + 5);
      else if (!strncmp(a, "jshift=", 7)) jshift = strtoull(a + 7, NULL, 16) & 63;
      else if (!strncmp(a, "isem=", 5)) isem = parse_sem(a + 5);
      else if (!strncmp(a, "dsem=", 5)) dsem = parse_sem(a + 5);
      else if (!strncmp(a, "ibig=", 5)) ibig_noop = !strcmp(a + 5, "noop");
      else if (!strncmp(a, "dbig=", 5)) dbig_noop = !strcmp(a + 5, "noop");
      else if (!strncmp(a, "maxev=", 6)) maxev = strtoull(a + 6, NULL, 10);
      else if (!strncmp(a, "isize=", 6)) isize = atoi(a + 6);
      else if (!strncmp(a, "iways=", 6)) iways = atoi(a + 6);
      else if (!strncmp(a, "dsize=", 6)) dsize = atoi(a + 6);
      else if (!strncmp(a, "dways=", 6)) dways = atoi(a + 6);
      else { fprintf(stderr, "jitcoh: bad arg %s\n", a); return -1; }
#undef HX
   }
   if (!out || !rom_n || !ram_n || !mbox_addr)
   {
      fprintf(stderr, "jitcoh: need out= rom= ram= mbox=\n");
      return -1;
   }
   rjlo = rom_lo < ram_lo ? rom_lo : ram_lo;
   rjhi = rom_lo + rom_n > ram_lo + ram_n ? rom_lo + rom_n : ram_lo + ram_n;
   rom_lo += jshift;
   ram_lo += jshift;
   jlo = rjlo + jshift;
   jhi = rjhi + jshift;
   jlo &= ~63ull;
   jhi = (jhi + 63) & ~63ull;
   jlo_m = jlo;
   jhi_m = jhi;
   nwords = (uint32_t)((jhi - jlo) / 4);
   nlines = nwords / WPL;
   W = g_new0(word_t, nwords);
   L = g_new0(line_t, nlines);
   dirty_list = g_new0(uint32_t, nlines);
   outf = fopen(out, "w");
   if (!outf)
      return -1;

   strcpy(vname[V_I], "HAZ_I");
   strcpy(vname[V_D], "HAZ_D");
   strcpy(vname[V_ID], "HAZ_ID");
   strcpy(vname[V_S], "HAZ_S");
   nv = V_ACT0;
   if (*ioff)
   {
      gchar **v = g_strsplit(ioff, ".", MAXACT);
      for (k = 0; v[k] && nact < MAXACT; k++)
      {
         icache_t *c = &IC[nact];
         c->off = atoi(v[k]);
         c->jitonly = strchr(v[k], 'j') != NULL;
         c->ways = iways;
         c->nsets = isize / (iways * 64);
         c->tag = g_new0(uint32_t, c->nsets * c->ways);
         c->age = g_new0(uint64_t, c->nsets * c->ways);
         c->snap = g_new0(uint32_t, c->nsets * c->ways * WPL);
         snprintf(vname[V_ACT0 + nact], sizeof(vname[0]), "ACT_o%d%s%s", c->off,
                  c->jitonly ? "j" : "", dlru ? "" : "_Dnever");
         nact++;
      }
      g_strfreev(v);
   }
   nv = V_ACT0 + nact;
   for (k = 0; k < nv; k++)
   {
      v_rep[k] = g_new0(uint32_t, nwords);
      v_bywriter[k] = g_hash_table_new(g_direct_hash, g_direct_equal);
      v_bylost[k] = g_hash_table_new(g_direct_hash, g_direct_equal);
   }
   if (dlru)
   {
      DC.ways = dways;
      DC.nsets = dsize / (dways * 64);
      DC.tag = g_new0(uint32_t, DC.nsets * DC.ways);
      DC.dirty = g_new0(uint8_t, DC.nsets * DC.ways);
      DC.age = g_new0(uint64_t, DC.nsets * DC.ways);
   }
   stks = g_array_new(FALSE, TRUE, sizeof(stk_t));
   stk_ids = g_hash_table_new(stk_hash, stk_eq);
   fprintf(outf, "# jitcoh rom=%" PRIx64 ":%" PRIx64 " ram=%" PRIx64 ":%" PRIx64
           " jshift=%" PRIx64 " isem=%d dsem=%d dlru=%d ioff=%s isize=%u iways=%u"
           " dsize=%u dways=%u line=64\n",
           rom_lo - jshift, rom_n, ram_lo - jshift, ram_n, jshift, isem, dsem, dlru,
           ioff, isize, iways, dsize, dways);
   qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
   qemu_plugin_register_atexit_cb(id, at_exit, NULL);
   return 0;
}
