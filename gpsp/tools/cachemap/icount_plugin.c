/* icount_plugin.c -- QEMU TCG plugin: how many times each instruction ADDRESS
 * executed (docs/CACHE-MAP.md).  The cache map's execution profile.
 *
 * Every translation block gets a counter; at exit each block's count is added
 * to every instruction address in it.  Exact (no sampling) and without an
 * observer effect: the program runs the same instructions it would without
 * the plugin.
 *
 * Args: out=FILE (required), skip=N (ignore everything until the Nth
 * execution of the address given by mark=HEX -- e.g. a per-frame marker, so
 * boot and state loading are not profiled; both optional).
 * Output: one "addr count" line per executed address (hex, decimal). */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct
{
   uint64_t count;
   unsigned n;
   uint64_t vaddr[];
} tbrec;

static GPtrArray *recs;
static char *out_path;
static uint64_t mark_addr, skip_marks, marks_seen;
static int armed = 1;

static void on_tb(unsigned int vcpu, void *udata)
{
   (void)vcpu;
   if (armed)
      ((tbrec *)udata)->count++;
}

static void on_mark(unsigned int vcpu, void *udata)
{
   (void)vcpu; (void)udata;
   if (++marks_seen == skip_marks)
   {
      unsigned i;
      for (i = 0; i < recs->len; i++)   /* forget the warm-up */
         ((tbrec *)g_ptr_array_index(recs, i))->count = 0;
      armed = 1;
   }
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
   size_t n = qemu_plugin_tb_n_insns(tb), i;
   tbrec *r = g_malloc0(sizeof(*r) + n * sizeof(uint64_t));
   (void)id;
   r->n = (unsigned)n;
   for (i = 0; i < n; i++)
   {
      struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
      r->vaddr[i] = qemu_plugin_insn_vaddr(insn);
      if (mark_addr && r->vaddr[i] == mark_addr)
         qemu_plugin_register_vcpu_insn_exec_cb(insn, on_mark,
                                                QEMU_PLUGIN_CB_NO_REGS, NULL);
   }
   g_ptr_array_add(recs, r);
   qemu_plugin_register_vcpu_tb_exec_cb(tb, on_tb, QEMU_PLUGIN_CB_NO_REGS, r);
}

static void at_exit(qemu_plugin_id_t id, void *p)
{
   GHashTable *h = g_hash_table_new(g_int64_hash, g_int64_equal);
   GHashTableIter it;
   gpointer k, v;
   unsigned i, j;
   FILE *fh;
   (void)id; (void)p;
   for (i = 0; i < recs->len; i++)
   {
      tbrec *r = g_ptr_array_index(recs, i);
      if (!r->count)
         continue;
      for (j = 0; j < r->n; j++)
      {
         uint64_t *c = g_hash_table_lookup(h, &r->vaddr[j]);
         if (!c)
         {
            c = g_new0(uint64_t, 2);
            c[0] = r->vaddr[j];
            g_hash_table_insert(h, &c[0], c);
         }
         c[1] += r->count;
      }
   }
   fh = fopen(out_path, "w");
   if (!fh)
      return;
   fprintf(fh, "# icount marks_seen=%" PRIu64 " skip=%" PRIu64 "\n",
           marks_seen, skip_marks);
   g_hash_table_iter_init(&it, h);
   while (g_hash_table_iter_next(&it, &k, &v))
   {
      uint64_t *c = v;
      fprintf(fh, "%" PRIx64 " %" PRIu64 "\n", c[0], c[1]);
   }
   fclose(fh);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
   int i;
   (void)info;
   for (i = 0; i < argc; i++)
   {
      if (!strncmp(argv[i], "out=", 4))
         out_path = g_strdup(argv[i] + 4);
      else if (!strncmp(argv[i], "mark=", 5))
         mark_addr = strtoull(argv[i] + 5, NULL, 16);
      else if (!strncmp(argv[i], "skip=", 5))
         skip_marks = strtoull(argv[i] + 5, NULL, 10);
   }
   if (!out_path)
      return -1;
   if (mark_addr && skip_marks)
      armed = 0;
   recs = g_ptr_array_new();
   qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
   qemu_plugin_register_atexit_cb(id, at_exit, NULL);
   return 0;
}
