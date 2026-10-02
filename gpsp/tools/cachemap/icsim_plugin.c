/* icsim_plugin.c -- QEMU TCG plugin: the Allegrex instruction cache,
 * simulated on the twin's execution TRACE placed at PSP addresses
 * (docs/CACHE-MAP.md).
 *
 * The twin (tools/cachemap/twin_profile.sh) runs the GB link machines as
 * mipsel-linux code.  cachemap.py --emit-map writes, for every twin
 * instruction address the profile saw, the PSP address the same source
 * line has in a given layout (the linked ELF, or a candidate order it has
 * not been linked in yet).  This plugin replays every executed block's
 * instruction fetches, at those PSP addresses, through an LRU
 * set-associative cache -- one cache per layout, all in the same run -- so
 * layouts are compared on the same trace, and the misses per frame come out
 * without a console.
 *
 * Args: map=FILE[:FILE...] (one per layout; lines "twin_hex psp_hex group"),
 *       out=FILE, size=16384, ways=2, line=64, mark=HEX (per-frame marker:
 *       gbdual_advance), skip=N (frames before counting).
 * Output: per layout, accesses and misses in total and per group (the
 *       group letter from the map: A, B, S(hared), O(ther)), and frames. */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAXL 8
#define NG 5                         /* A B S O and unmapped */

typedef struct
{
   unsigned nsets, ways, lshift;
   uint32_t *tag;                    /* nsets * ways */
   uint64_t *age;
   uint64_t acc[NG], miss[NG];
   GHashTable *map;                  /* twin addr -> (psp line << 3 | group) */
   char *name;
} cache_t;

typedef struct
{
   unsigned n[MAXL];
   uint64_t *lines[MAXL];            /* per layout: (line<<3 | group) */
} tbrec;

static cache_t C[MAXL];
static unsigned nl;
static char *out_path;
static uint64_t mark_addr, skip_marks, marks;
static int armed = 1;
static uint64_t tick;

static int gidx(char g)
{
   switch (g)
   {
   case 'A': return 0;
   case 'B': return 1;
   case 'S': return 2;
   case 'O': return 3;
   }
   return 4;
}

static void access_line(cache_t *c, uint64_t lg)
{
   uint32_t line = (uint32_t)(lg >> 3);
   int g = (int)(lg & 7);
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
   if (!armed)
      return;
   for (k = 0; k < nl; k++)
      for (i = 0; i < r->n[k]; i++)
         access_line(&C[k], r->lines[k][i]);
}

static void on_mark(unsigned int vcpu, void *udata)
{
   (void)vcpu; (void)udata;
   marks++;
   if (!armed && marks >= skip_marks)
   {
      unsigned k;
      armed = 1;
      marks = 0;
      for (k = 0; k < nl; k++)
      {
         memset(C[k].acc, 0, sizeof(C[k].acc));
         memset(C[k].miss, 0, sizeof(C[k].miss));
      }
   }
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
   size_t n = qemu_plugin_tb_n_insns(tb), i;
   tbrec *r = g_new0(tbrec, 1);
   unsigned k;
   (void)id;
   for (k = 0; k < nl; k++)
   {
      uint64_t last = UINT64_MAX;
      r->lines[k] = g_new0(uint64_t, n);
      for (i = 0; i < n; i++)
      {
         struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
         uint64_t va = qemu_plugin_insn_vaddr(insn);
         gpointer v = g_hash_table_lookup(C[k].map, GSIZE_TO_POINTER(va));
         uint64_t lg;
         if (k == 0 && mark_addr && va == mark_addr)
            qemu_plugin_register_vcpu_insn_exec_cb(insn, on_mark,
                                                   QEMU_PLUGIN_CB_NO_REGS, NULL);
         if (!v)
            continue;               /* not in the map: not fetched from PSP code */
         lg = (uint64_t)GPOINTER_TO_SIZE(v);
         lg = ((lg >> 3) >> C[k].lshift << 3) | (lg & 7);
         if (lg != last)
            r->lines[k][r->n[k]++] = lg;
         last = lg;
      }
   }
   qemu_plugin_register_vcpu_tb_exec_cb(tb, on_tb, QEMU_PLUGIN_CB_NO_REGS, r);
}

static void at_exit(qemu_plugin_id_t id, void *p)
{
   FILE *fh = fopen(out_path, "w");
   unsigned k, g;
   static const char *gn[NG] = { "A", "B", "S", "O", "U" };
   (void)id; (void)p;
   if (!fh)
      return;
   fprintf(fh, "frames %" PRIu64 "\n", marks);
   for (k = 0; k < nl; k++)
   {
      uint64_t ta = 0, tm = 0;
      for (g = 0; g < NG; g++)
      {
         ta += C[k].acc[g];
         tm += C[k].miss[g];
      }
      fprintf(fh, "layout %s access %" PRIu64 " miss %" PRIu64, C[k].name, ta, tm);
      for (g = 0; g < NG; g++)
         fprintf(fh, " %s=%" PRIu64 "/%" PRIu64, gn[g], C[k].miss[g], C[k].acc[g]);
      fprintf(fh, "\n");
   }
   fclose(fh);
}

static int load_map(cache_t *c, const char *path)
{
   FILE *fh = fopen(path, "r");
   char buf[256];
   if (!fh)
      return -1;
   c->map = g_hash_table_new(g_direct_hash, g_direct_equal);
   while (fgets(buf, sizeof(buf), fh))
   {
      unsigned long long t, s;
      char g = 'O';
      if (sscanf(buf, "%llx %llx %c", &t, &s, &g) < 2)
         continue;
      g_hash_table_insert(c->map, GSIZE_TO_POINTER((gsize)t),
                          GSIZE_TO_POINTER((gsize)((s << 3) | (unsigned)gidx(g))));
   }
   fclose(fh);
   c->name = g_path_get_basename(path);
   return 0;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
   unsigned size = 16384, ways = 2, line = 64, k;
   char *maps = NULL;
   int i;
   (void)info;
   for (i = 0; i < argc; i++)
   {
      if (!strncmp(argv[i], "out=", 4)) out_path = g_strdup(argv[i] + 4);
      else if (!strncmp(argv[i], "map=", 4)) maps = g_strdup(argv[i] + 4);
      else if (!strncmp(argv[i], "size=", 5)) size = atoi(argv[i] + 5);
      else if (!strncmp(argv[i], "ways=", 5)) ways = atoi(argv[i] + 5);
      else if (!strncmp(argv[i], "line=", 5)) line = atoi(argv[i] + 5);
      else if (!strncmp(argv[i], "mark=", 5)) mark_addr = strtoull(argv[i] + 5, NULL, 16);
      else if (!strncmp(argv[i], "skip=", 5)) skip_marks = strtoull(argv[i] + 5, NULL, 10);
   }
   if (!out_path || !maps)
      return -1;
   {
      gchar **v = g_strsplit(maps, ":", MAXL);
      for (nl = 0; v[nl] && nl < MAXL; nl++)
      {
         cache_t *c = &C[nl];
         c->ways = ways;
         c->nsets = size / (ways * line);
         c->lshift = 0;
         while ((1u << c->lshift) < line)
            c->lshift++;
         c->tag = g_new0(uint32_t, c->nsets * ways);
         c->age = g_new0(uint64_t, c->nsets * ways);
         if (load_map(c, v[nl]) != 0)
            return -1;
      }
      g_strfreev(v);
   }
   for (k = 0; k < nl; k++)
      (void)k;
   if (mark_addr && skip_marks)
      armed = 0;
   qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
   qemu_plugin_register_atexit_cb(id, at_exit, NULL);
   return 0;
}
