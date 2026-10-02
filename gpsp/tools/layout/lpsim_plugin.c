/* lpsim_plugin.c -- QEMU TCG plugin: the Allegrex instruction cache under
 * many candidate PSP code LAYOUTS at once, on the dynarec twin's execution
 * (docs/LAYOUT-PINNING.md).
 *
 * The drprof twin (tools/drprof, built by tools/layout/twin_build.sh) runs the
 * PSP's own translator as mipsel-linux code.  Its instruction addresses mean
 * nothing on a PSP, so every fetch is PLACED before it reaches the simulated
 * cache:
 *   - static code (C and asm): through a per-layout map file, "twin_hex
 *     psp_hex" per executed twin instruction (tools/layout/lp_map.py writes it
 *     from the PSP ELF of that layout, or from a layout that was never linked);
 *     twin instructions with no PSP twin (the host harness, the renderer --
 *     which runs on the Media Engine on a PSP -- and libc the PSP does not
 *     have) are not fetched;
 *   - translated code: everything in jit=LO:HI (the twin's ROM+RAM
 *     translation caches, which are contiguous in both tiers) goes to the
 *     layout's PSP JIT base + (va - LO).  The emitted code and the order it is
 *     emitted in are the PSP's (drprof's oracle), so only the base differs;
 *   - PSP-only per-frame code the twin cannot run (main loop, ME host, GE,
 *     audio): a per-layout list of PSP line addresses fetched once at every
 *     frame marker.
 * Every layout sees the SAME trace, so differences between layouts are the
 * layouts, not the run.
 *
 * Args: out=FILE, layouts=FILE (lines: "name map_file jit_base_hex
 *       frame_file|-"), jit=LO:HI (hex), mark=HEX (per-frame marker, the
 *       twin's drprof_host_mark), skip=N (frames before counting),
 *       size=16384 ways=2 line=64, counts=FILE (optional: per-address
 *       execution counts, as tools/cachemap/icount_plugin.c writes them).
 * Output: per layout, accesses and misses per group (J translated, S static,
 *       F per-frame PSP path) and the frame count. */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAXL 160
enum { GJ, GS, GF, NG };

typedef struct
{
   unsigned nsets, ways;
   uint32_t *tag;                    /* nsets * ways, line + 1 (0 = empty) */
   uint64_t *age;
   uint64_t acc[NG], miss[NG];
   GHashTable *map;                  /* twin addr -> psp addr (shared) */
   uint32_t jit_line;                /* PSP JIT base >> lshift */
   uint32_t *frame;                  /* per-frame PSP lines */
   unsigned nframe;
   char *name;
} cache_t;

typedef struct
{
   int jit;                          /* translated code: lines are offsets */
   unsigned nj;
   uint32_t *jl;                     /* jit line offsets */
   unsigned n[MAXL];
   uint32_t *lines[MAXL];            /* static: per layout */
   uint64_t count;                   /* executions (counts=) */
   unsigned ninsn;
   uint64_t *vaddr;
} tbrec;

static cache_t C[MAXL];
static unsigned nl, lshift;
static char *out_path, *counts_path;
static uint64_t mark_addr, skip_marks, marks, jit_lo, jit_hi;
static int armed = 1;
static uint64_t tick;
static GPtrArray *recs;
static GMutex lock;

static inline void access_line(cache_t *c, uint32_t line, int g)
{
   unsigned set = line % c->nsets, w, victim = 0;
   uint32_t *t = c->tag + set * c->ways;
   uint64_t *a = c->age + set * c->ways;
   uint64_t oldest = UINT64_MAX;
   c->acc[g]++;
   tick++;
   for (w = 0; w < c->ways; w++)
      if (t[w] == line + 1)
      {
         a[w] = tick;
         return;
      }
   c->miss[g]++;
   for (w = 0; w < c->ways; w++)
      if (a[w] < oldest)
      {
         oldest = a[w];
         victim = w;
      }
   t[victim] = line + 1;
   a[victim] = tick;
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
            access_line(&C[k], C[k].jit_line + r->jl[i], GJ);
      return;
   }
   for (k = 0; k < nl; k++)
      for (i = 0; i < r->n[k]; i++)
         access_line(&C[k], r->lines[k][i], GS);
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
      for (k = 0; k < nl; k++)
      {
         memset(C[k].acc, 0, sizeof(C[k].acc));
         memset(C[k].miss, 0, sizeof(C[k].miss));
      }
      if (counts_path)
         for (i = 0; i < recs->len; i++)
            ((tbrec *)g_ptr_array_index(recs, i))->count = 0;
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
   if (jit_hi && va0 >= jit_lo && va0 < jit_hi)
   {
      uint32_t last = UINT32_MAX;
      r->jit = 1;
      r->jl = g_new0(uint32_t, n);
      for (i = 0; i < n; i++)
      {
         uint64_t va = qemu_plugin_insn_vaddr(qemu_plugin_tb_get_insn(tb, i));
         uint32_t l = (uint32_t)((va - jit_lo) >> lshift);
         if (l != last)
            r->jl[r->nj++] = l;
         last = l;
      }
   }
   else
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

static void at_exit(qemu_plugin_id_t id, void *p)
{
   FILE *fh = fopen(out_path, "w");
   unsigned k, g, i, j;
   static const char *gn[NG] = { "J", "S", "F" };
   (void)id; (void)p;
   if (fh)
   {
      fprintf(fh, "frames %" PRIu64 "\n", marks);
      for (k = 0; k < nl; k++)
      {
         uint64_t ta = 0, tm = 0;
         for (g = 0; g < NG; g++)
         {
            ta += C[k].acc[g];
            tm += C[k].miss[g];
         }
         fprintf(fh, "layout %s access %" PRIu64 " miss %" PRIu64, C[k].name,
                 ta, tm);
         for (g = 0; g < NG; g++)
            fprintf(fh, " %s=%" PRIu64 "/%" PRIu64, gn[g], C[k].miss[g],
                    C[k].acc[g]);
         fprintf(fh, "\n");
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
   char *lay = NULL, buf[1024];
   GHashTable *maps = g_hash_table_new(g_str_hash, g_str_equal);
   FILE *fh;
   int i;
   (void)info;
   for (i = 0; i < argc; i++)
   {
      if (!strncmp(argv[i], "out=", 4)) out_path = g_strdup(argv[i] + 4);
      else if (!strncmp(argv[i], "counts=", 7)) counts_path = g_strdup(argv[i] + 7);
      else if (!strncmp(argv[i], "layouts=", 8)) lay = g_strdup(argv[i] + 8);
      else if (!strncmp(argv[i], "size=", 5)) size = atoi(argv[i] + 5);
      else if (!strncmp(argv[i], "ways=", 5)) ways = atoi(argv[i] + 5);
      else if (!strncmp(argv[i], "line=", 5)) line = atoi(argv[i] + 5);
      else if (!strncmp(argv[i], "mark=", 5)) mark_addr = strtoull(argv[i] + 5, NULL, 16);
      else if (!strncmp(argv[i], "skip=", 5)) skip_marks = strtoull(argv[i] + 5, NULL, 10);
      else if (!strncmp(argv[i], "jit=", 4))
      {
         char *e;
         jit_lo = strtoull(argv[i] + 4, &e, 16);
         if (*e == ':')
            jit_hi = strtoull(e + 1, NULL, 16);
      }
   }
   if (!out_path || !lay || !(fh = fopen(lay, "r")))
      return -1;
   while ((1u << lshift) < line)
      lshift++;
   while (nl < MAXL && fgets(buf, sizeof(buf), fh))
   {
      char name[256], map[512], frame[512];
      unsigned long long jb;
      cache_t *c = &C[nl];
      if (buf[0] == '#' || sscanf(buf, "%255s %511s %llx %511s", name, map,
                                  &jb, frame) != 4)
         continue;
      if (jb & (line - 1))
         return -1;                 /* JIT bases are line-aligned (memalign) */
      c->ways = ways;
      c->nsets = size / (ways * line);
      c->tag = g_new0(uint32_t, c->nsets * ways);
      c->age = g_new0(uint64_t, c->nsets * ways);
      c->map = load_map(maps, map);
      c->jit_line = (uint32_t)(jb >> lshift);
      c->name = g_strdup(name);
      if (!c->map || load_frame(c, frame) != 0)
         return -1;
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
