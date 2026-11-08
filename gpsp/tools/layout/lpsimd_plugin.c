/* lpsim_plugin.c -- QEMU TCG plugin: the Allegrex instruction AND data caches
 * under many candidate PSP LAYOUTS at once, on the dynarec twin's execution
 * (docs/LAYOUT-PINNING.md, section 9 for the data side).
 *
 * The drprof twin (tools/drprof, built by tools/layout/twin_build.sh) runs the
 * PSP's own translator as mipsel-linux code.  Its addresses mean nothing on a
 * PSP, so every fetch and every data access is PLACED first:
 *   - static code (C and asm): through a per-layout map file, "twin_hex
 *     psp_hex" per executed twin instruction (tools/layout/lp_map.py writes it
 *     from the PSP ELF of that layout); twin instructions with no PSP twin
 *     (the host harness, libc the PSP does not have) are not fetched, and
 *     their data accesses are not made;
 *   - translated code: everything in jit=LO:HI goes to the layout's PSP JIT
 *     base + (va - LO);
 *   - PSP-only per-frame code the twin cannot run (main loop, ME host, GE,
 *     audio): a per-layout list of PSP line addresses fetched once at every
 *     frame marker;
 *   - DATA (optional, a layout line's "D=FILE"): ranges "twin_lo twin_hi
 *     psp_lo" (lp_map.py dmap: every named object by name, the translation
 *     cache to the JIT base).  An address in no range keeps its twin address
 *     in every layout (heap, stack): it competes for the same sets in all of
 *     them, so it cannot make a difference between two layouts by itself.
 * Every layout sees the SAME trace, so differences between layouts are the
 * layouts, not the run.
 *
 * Args: out=FILE, layouts=FILE (lines: "name map_file jit_base_hex
 *       frame_file|- [jit_base2_hex ...] [D=dmap_file]"), jit=LO:HI (hex),
 *       mark=HEX (per-frame marker, the twin's drprof_host_mark), skip=N
 *       (frames before counting), size=16384 ways=2 line=64 (both caches),
 *       counts=FILE (per-address execution counts), misses=FILE (per layout,
 *       misses per PSP line, I and D).
 * Output: per layout, accesses and misses per group (J translated, S static,
 *       F per-frame PSP path; DJ/DS data accesses made by translated/static
 *       code) and the frame count. */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAXL 64
#define MAXJ 4
enum { GJ, GS, GF, NG };
enum { DJ, DS, NDG };

typedef struct
{
   unsigned nsets, ways;
   uint32_t *tag;                    /* nsets * ways, line + 1 (0 = empty) */
   uint64_t *age;
} cache_geom;

typedef struct
{
   cache_geom ic, dc;
   uint64_t acc[NG], miss[NG];
   uint64_t dacc[NDG], dmiss[NDG];
   GHashTable *map;                  /* twin addr -> psp addr (shared) */
   uint32_t jit_line[MAXJ];          /* PSP JIT base >> lshift, per range */
   uint32_t jit_base0;
   uint32_t *frame;                  /* per-frame PSP lines */
   unsigned nframe;
   int32_t *ddelta;                  /* per 4-byte granule of [dlo,dhi) */
   GHashTable *imiss, *dmissl;       /* misses=: PSP line -> n */
   char *name;
} cache_t;

typedef struct
{
   int jit;
   unsigned nj;
   uint32_t *jl;
   unsigned n[MAXL];
   uint32_t *lines[MAXL];
   uint64_t count;
   unsigned ninsn;
   uint64_t *vaddr;
} tbrec;

static cache_t C[MAXL];
static unsigned nl, lshift;
static char *out_path, *counts_path, *misses_path;
static uint64_t mark_addr, skip_marks, marks, jit_lo[MAXJ], jit_hi[MAXJ];
static unsigned njit;
static int armed = 1, dsim;
static uint64_t tick, dtick;
static uint64_t dlo = UINT64_MAX, dhi;      /* data placement window */
static GPtrArray *recs;
static GMutex lock;

static void geom_init(cache_geom *g, unsigned size, unsigned ways, unsigned line)
{
   g->ways = ways;
   g->nsets = size / (ways * line);
   g->tag = g_new0(uint32_t, g->nsets * ways);
   g->age = g_new0(uint64_t, g->nsets * ways);
}

static inline int lru_access(cache_geom *c, uint32_t line, uint64_t *clk)
{
   unsigned set = line % c->nsets, w, victim = 0;
   uint32_t *t = c->tag + set * c->ways;
   uint64_t *a = c->age + set * c->ways;
   uint64_t oldest = UINT64_MAX;
   (*clk)++;
   for (w = 0; w < c->ways; w++)
      if (t[w] == line + 1)
      {
         a[w] = *clk;
         return 0;
      }
   for (w = 0; w < c->ways; w++)
      if (a[w] < oldest)
      {
         oldest = a[w];
         victim = w;
      }
   t[victim] = line + 1;
   a[victim] = *clk;
   return 1;
}

static inline void bump(GHashTable *h, uint32_t line)
{
   gpointer k;
   if (!h)
      return;
   k = GSIZE_TO_POINTER((gsize)line);
   g_hash_table_insert(h, k, GSIZE_TO_POINTER(
      GPOINTER_TO_SIZE(g_hash_table_lookup(h, k)) + 1));
}

static inline void access_line(cache_t *c, uint32_t line, int g)
{
   c->acc[g]++;
   if (lru_access(&c->ic, line, &tick))
   {
      c->miss[g]++;
      bump(c->imiss, line);
   }
}

static void on_tb(unsigned int vcpu, void *udata)
{
   tbrec *r = udata;
   unsigned k, i;
   (void)vcpu;
   r->count++;
   if (!armed)
      return;
   if (r->jit)
   {
      for (k = 0; k < nl; k++)
         for (i = 0; i < r->nj; i++)
            access_line(&C[k], C[k].jit_line[r->jit - 1] + r->jl[i], GJ);
      return;
   }
   for (k = 0; k < nl; k++)
      for (i = 0; i < r->n[k]; i++)
         access_line(&C[k], r->lines[k][i], GS);
}

static void on_mem(unsigned int vcpu, qemu_plugin_meminfo_t info,
                   uint64_t va, void *udata)
{
   unsigned k, g = (unsigned)(uintptr_t)udata;
   (void)vcpu; (void)info;
   if (!armed)
      return;
   for (k = 0; k < nl; k++)
   {
      cache_t *c = &C[k];
      uint64_t pa = va;
      uint32_t line;
      if (va >= jit_lo[0] && va < jit_hi[0])
         pa = c->jit_base0 + (va - jit_lo[0]);
      else if (c->ddelta && va >= dlo && va < dhi)
         pa = (uint64_t)((int64_t)va + c->ddelta[(va - dlo) >> 2]);
      line = (uint32_t)(pa >> lshift);
      c->dacc[g]++;
      if (lru_access(&c->dc, line, &dtick))
      {
         c->dmiss[g]++;
         bump(c->dmissl, line);
      }
   }
}

static void reset_counts(void)
{
   unsigned k, i;
   for (k = 0; k < nl; k++)
   {
      memset(C[k].acc, 0, sizeof(C[k].acc));
      memset(C[k].miss, 0, sizeof(C[k].miss));
      memset(C[k].dacc, 0, sizeof(C[k].dacc));
      memset(C[k].dmiss, 0, sizeof(C[k].dmiss));
      if (C[k].imiss)
         g_hash_table_remove_all(C[k].imiss);
      if (C[k].dmissl)
         g_hash_table_remove_all(C[k].dmissl);
   }
   if (counts_path)
      for (i = 0; i < recs->len; i++)
         ((tbrec *)g_ptr_array_index(recs, i))->count = 0;
}

static void on_mark(unsigned int vcpu, void *udata)
{
   unsigned k, i;
   (void)vcpu; (void)udata;
   marks++;
   if (!armed)
   {
      if (marks < skip_marks)
         return;
      armed = 1;
      marks = 0;
      reset_counts();
      return;
   }
   for (k = 0; k < nl; k++)
      for (i = 0; i < C[k].nframe; i++)
         access_line(&C[k], C[k].frame[i], GF);
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
   size_t n = qemu_plugin_tb_n_insns(tb), i;
   tbrec *r = g_new0(tbrec, 1);
   uint64_t va0 = qemu_plugin_insn_vaddr(qemu_plugin_tb_get_insn(tb, 0));
   unsigned k;
   (void)id;
   r->ninsn = n;
   if (counts_path)
   {
      r->vaddr = g_new0(uint64_t, n);
      for (i = 0; i < n; i++)
         r->vaddr[i] = qemu_plugin_insn_vaddr(qemu_plugin_tb_get_insn(tb, i));
   }
   for (k = 0; k < njit; k++)
      if (va0 >= jit_lo[k] && va0 < jit_hi[k])
         break;
   if (k < njit)
   {
      uint32_t last = UINT32_MAX;
      unsigned jr = k;
      r->jit = 1 + (int)jr;
      r->jl = g_new0(uint32_t, n);
      for (i = 0; i < n; i++)
      {
         struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
         uint64_t va = qemu_plugin_insn_vaddr(insn);
         uint32_t l = (uint32_t)((va - jit_lo[jr]) >> lshift);
         if (l != last)
            r->jl[r->nj++] = l;
         last = l;
         if (dsim)
            qemu_plugin_register_vcpu_mem_cb(insn, on_mem, QEMU_PLUGIN_CB_NO_REGS,
                                             QEMU_PLUGIN_MEM_RW,
                                             (void *)(uintptr_t)DJ);
      }
   }
   else
   {
      for (k = 0; k < nl; k++)
      {
         uint32_t last = UINT32_MAX;
         r->lines[k] = g_new0(uint32_t, n);
         for (i = 0; i < n; i++)
         {
            uint64_t va = qemu_plugin_insn_vaddr(qemu_plugin_tb_get_insn(tb, i));
            gpointer v = g_hash_table_lookup(C[k].map, GSIZE_TO_POINTER(va));
            uint32_t l;
            if (!v)
               continue;            /* no PSP twin: not fetched on a PSP */
            l = (uint32_t)(GPOINTER_TO_SIZE(v) >> lshift);
            if (l != last)
               r->lines[k][r->n[k]++] = l;
            last = l;
         }
      }
      /* data: only what a PSP instruction would do (layout 0's map decides:
       * the layouts are ELFs of one source, so they place the same names) */
      if (dsim && nl)
         for (i = 0; i < n; i++)
         {
            struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
            if (g_hash_table_lookup(C[0].map,
                    GSIZE_TO_POINTER(qemu_plugin_insn_vaddr(insn))))
               qemu_plugin_register_vcpu_mem_cb(insn, on_mem,
                                                QEMU_PLUGIN_CB_NO_REGS,
                                                QEMU_PLUGIN_MEM_RW,
                                                (void *)(uintptr_t)DS);
         }
   }
   for (i = 0; i < n && mark_addr; i++)
   {
      struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
      if (qemu_plugin_insn_vaddr(insn) == mark_addr)
         qemu_plugin_register_vcpu_insn_exec_cb(insn, on_mark,
                                                QEMU_PLUGIN_CB_NO_REGS, NULL);
   }
   if (counts_path)
   {
      g_mutex_lock(&lock);
      g_ptr_array_add(recs, r);
      g_mutex_unlock(&lock);
   }
   qemu_plugin_register_vcpu_tb_exec_cb(tb, on_tb, QEMU_PLUGIN_CB_NO_REGS, r);
}

static void dump_hash(FILE *fh, const char *lay, const char *kind, GHashTable *h)
{
   GHashTableIter it;
   gpointer key, val;
   if (!h)
      return;
   g_hash_table_iter_init(&it, h);
   while (g_hash_table_iter_next(&it, &key, &val))
      fprintf(fh, "%s %s %" PRIx64 " %" PRIu64 "\n", lay, kind,
              (uint64_t)GPOINTER_TO_SIZE(key) << lshift,
              (uint64_t)GPOINTER_TO_SIZE(val));
}

static void at_exit(qemu_plugin_id_t id, void *p)
{
   FILE *fh = fopen(out_path, "w");
   unsigned k, g, i, j;
   static const char *gn[NG] = { "J", "S", "F" };
   static const char *dn[NDG] = { "DJ", "DS" };
   (void)id; (void)p;
   if (fh)
   {
      fprintf(fh, "frames %" PRIu64 "\n", marks);
      for (k = 0; k < nl; k++)
      {
         uint64_t ta = 0, tm = 0, da = 0, dm = 0;
         for (g = 0; g < NG; g++)
         {
            ta += C[k].acc[g];
            tm += C[k].miss[g];
         }
         for (g = 0; g < NDG; g++)
         {
            da += C[k].dacc[g];
            dm += C[k].dmiss[g];
         }
         fprintf(fh, "layout %s access %" PRIu64 " miss %" PRIu64, C[k].name,
                 ta, tm);
         for (g = 0; g < NG; g++)
            fprintf(fh, " %s=%" PRIu64 "/%" PRIu64, gn[g], C[k].miss[g],
                    C[k].acc[g]);
         if (dsim)
         {
            fprintf(fh, " daccess %" PRIu64 " dmiss %" PRIu64, da, dm);
            for (g = 0; g < NDG; g++)
               fprintf(fh, " %s=%" PRIu64 "/%" PRIu64, dn[g], C[k].dmiss[g],
                       C[k].dacc[g]);
         }
         fprintf(fh, "\n");
      }
      fclose(fh);
   }
   if (misses_path && (fh = fopen(misses_path, "w")))
   {
      fprintf(fh, "# frames %" PRIu64 "\n", marks);
      for (k = 0; k < nl; k++)
      {
         dump_hash(fh, C[k].name, "I", C[k].imiss);
         dump_hash(fh, C[k].name, "D", C[k].dmissl);
      }
      fclose(fh);
   }
   if (counts_path && (fh = fopen(counts_path, "w")))
   {
      GHashTable *agg = g_hash_table_new(g_direct_hash, g_direct_equal);
      GHashTableIter it;
      gpointer key, val;
      for (i = 0; i < recs->len; i++)
      {
         tbrec *r = g_ptr_array_index(recs, i);
         if (!r->count)
            continue;
         for (j = 0; j < r->ninsn; j++)
         {
            gpointer kk = GSIZE_TO_POINTER(r->vaddr[j]);
            uint64_t *c = g_hash_table_lookup(agg, kk);
            if (!c)
            {
               c = g_new0(uint64_t, 1);
               g_hash_table_insert(agg, kk, c);
            }
            *c += r->count;
         }
      }
      fprintf(fh, "# frames %" PRIu64 "\n", marks);
      g_hash_table_iter_init(&it, agg);
      while (g_hash_table_iter_next(&it, &key, &val))
         fprintf(fh, "%" PRIx64 " %" PRIu64 "\n",
                 (uint64_t)GPOINTER_TO_SIZE(key), *(uint64_t *)val);
      fclose(fh);
   }
}

static GHashTable *load_map(GHashTable *cache, const char *path)
{
   GHashTable *m = g_hash_table_lookup(cache, path);
   FILE *fh;
   char buf[256];
   if (m)
      return m;
   m = g_hash_table_new(g_direct_hash, g_direct_equal);
   if (strcmp(path, "-") && (fh = fopen(path, "r")))
   {
      while (fgets(buf, sizeof(buf), fh))
      {
         unsigned long long t, s;
         if (sscanf(buf, "%llx %llx", &t, &s) == 2)
            g_hash_table_insert(m, GSIZE_TO_POINTER((gsize)t),
                                GSIZE_TO_POINTER((gsize)s));
      }
      fclose(fh);
   }
   else if (strcmp(path, "-"))
      return NULL;
   g_hash_table_insert(cache, g_strdup(path), m);
   return m;
}

/* "twin_lo twin_hi psp_lo" ranges; the first line "window LO HI" (hex) is the
 * twin's whole static data range (one granule table per layout). */
static int load_dmap(cache_t *c, const char *path)
{
   FILE *fh = fopen(path, "r");
   char buf[256];
   unsigned long long lo, hi, to;
   if (!fh)
      return -1;
   while (fgets(buf, sizeof(buf), fh))
   {
      if (sscanf(buf, "window %llx %llx", &lo, &hi) == 2)
      {
         if (dlo == UINT64_MAX)
         {
            dlo = lo & ~3ull;
            dhi = hi;
         }
         c->ddelta = g_new0(int32_t, (dhi - dlo) / 4 + 1);
         continue;
      }
      if (!c->ddelta || sscanf(buf, "%llx %llx %llx", &lo, &hi, &to) != 3)
         continue;
      if (lo < dlo || hi > dhi || hi <= lo)
         continue;
      for (; lo < hi; lo += 4, to += 4)
         c->ddelta[(lo - dlo) >> 2] = (int32_t)((int64_t)to - (int64_t)lo);
   }
   fclose(fh);
   return c->ddelta ? 0 : -1;
}

static int load_frame(cache_t *c, const char *path)
{
   FILE *fh;
   char buf[128];
   unsigned cap = 1024;
   if (!strcmp(path, "-"))
      return 0;
   if (!(fh = fopen(path, "r")))
      return -1;
   c->frame = g_new0(uint32_t, cap);
   while (fgets(buf, sizeof(buf), fh))
   {
      unsigned long long a;
      if (sscanf(buf, "%llx", &a) != 1)
         continue;
      if (c->nframe == cap)
         c->frame = g_renew(uint32_t, c->frame, cap *= 2);
      c->frame[c->nframe++] = (uint32_t)(a >> lshift);
   }
   fclose(fh);
   return 0;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
   unsigned size = 16384, ways = 2, line = 64;
   char *lay = NULL, buf[2048];
   GHashTable *maps = g_hash_table_new(g_str_hash, g_str_equal);
   FILE *fh;
   int i;
   (void)info;
   for (i = 0; i < argc; i++)
   {
      if (!strncmp(argv[i], "out=", 4)) out_path = g_strdup(argv[i] + 4);
      else if (!strncmp(argv[i], "counts=", 7)) counts_path = g_strdup(argv[i] + 7);
      else if (!strncmp(argv[i], "misses=", 7)) misses_path = g_strdup(argv[i] + 7);
      else if (!strncmp(argv[i], "layouts=", 8)) lay = g_strdup(argv[i] + 8);
      else if (!strncmp(argv[i], "size=", 5)) size = atoi(argv[i] + 5);
      else if (!strncmp(argv[i], "ways=", 5)) ways = atoi(argv[i] + 5);
      else if (!strncmp(argv[i], "line=", 5)) line = atoi(argv[i] + 5);
      else if (!strncmp(argv[i], "mark=", 5)) mark_addr = strtoull(argv[i] + 5, NULL, 16);
      else if (!strncmp(argv[i], "skip=", 5)) skip_marks = strtoull(argv[i] + 5, NULL, 10);
      else if (!strncmp(argv[i], "jit=", 4) && njit < MAXJ)
      {
         char *e;
         jit_lo[njit] = strtoull(argv[i] + 4, &e, 16);
         if (*e == ':')
            jit_hi[njit] = strtoull(e + 1, NULL, 16);
         if (jit_hi[njit])
            njit++;
      }
   }
   if (!out_path || !lay || !(fh = fopen(lay, "r")))
      return -1;
   while ((1u << lshift) < line)
      lshift++;
   while (nl < MAXL && fgets(buf, sizeof(buf), fh))
   {
      char name[256], map[512], frame[512], *dp;
      unsigned long long jb, jx[MAXJ - 1] = { 0, 0, 0 };
      cache_t *c = &C[nl];
      int nf, q;
      if (buf[0] == '#' ||
          (nf = sscanf(buf, "%255s %511s %llx %511s %llx %llx %llx", name, map,
                       &jb, frame, &jx[0], &jx[1], &jx[2])) < 4)
         continue;
      if (jb & (line - 1))
         return -1;
      for (q = 0; q < MAXJ - 1; q++)
         c->jit_line[q + 1] = (uint32_t)((q < nf - 4 ? jx[q] : jb) >> lshift);
      geom_init(&c->ic, size, ways, line);
      geom_init(&c->dc, size, ways, line);
      c->map = load_map(maps, map);
      c->jit_line[0] = (uint32_t)(jb >> lshift);
      c->jit_base0 = (uint32_t)jb;
      c->name = g_strdup(name);
      if (!c->map || load_frame(c, frame) != 0)
         return -1;
      if ((dp = strstr(buf, " D=")))
      {
         char dpath[512];
         if (sscanf(dp + 3, "%511s", dpath) != 1 || load_dmap(c, dpath) != 0)
            return -1;
         dsim = 1;
      }
      if (misses_path)
      {
         c->imiss = g_hash_table_new(g_direct_hash, g_direct_equal);
         c->dmissl = g_hash_table_new(g_direct_hash, g_direct_equal);
      }
      nl++;
   }
   fclose(fh);
   recs = g_ptr_array_new();
   if (mark_addr && skip_marks)
      armed = 0;
   qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
   qemu_plugin_register_atexit_cb(id, at_exit, NULL);
   return 0;
}
