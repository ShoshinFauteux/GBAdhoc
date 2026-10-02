/* drprof_plugin.c -- QEMU TCG plugin: exact dynamic MIPS-instruction profile
 * of the GBAdhoc dynarec twin (docs/DYNAREC-PROFILE.md).
 *
 * Runs under qemu-mipsel (linux-user) with dr_host built for
 * platform=drprof-mipsel -DDRPROF_TWIN.  Every guest(=PSP-side) MIPS
 * instruction executed is attributed, with no sampling error, to:
 *
 *   translated code   (rom/ram translation cache, outside the stub area) by
 *                     ROLE of the MIPS instruction (ALU, flag write, cycle
 *                     update/check, reg[] access, memory-stub call, dispatch
 *                     jump, ...) and by the GUEST INSTRUCTION CLASS that
 *                     emitted it (the core's class map, drprof.h);
 *   the stub area     the emitter's memory handlers, per word offset (the
 *                     analysis script names them from dr_host's stub map);
 *   C / asm code      per symbol, unless inside a zone (translation, flush),
 *                     where it is charged to the zone instead.
 *
 * Output (out=FILE):
 *   F <frame> <seg> <total> <c0> ... <cN-1>        one line per frame
 *   S <scene> <bucket> <count>                      bucket deltas per scene
 * The per-frame coarse categories are fixed (CAT_* below, printed in the
 * header).  Symbol categories come from symcat=FILE (generated from nm by
 * dr_profile.sh).
 *
 * Args: out=, symcat=, ctl=<hex addr of drprof_ctl>, zarg=<hex addr of
 * drprof_zone_arg>.
 *
 * The instrumentation lives entirely in QEMU: the profiled program executes
 * the same MIPS instructions it would without the plugin, so there is no
 * observer effect on the counts.  (The twin build adds only translation-time
 * bookkeeping and two empty marker functions, which are counted as HOST.) */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

enum
{
   CAT_JIT = 0,     /* translated code: guest work                        */
   CAT_JMEM,        /* translated code: memory-access call setup + jal    */
   CAT_STUB,        /* emitter memory stubs (region handlers, patchers)   */
   CAT_MEMC,        /* C memory slow paths (io, backup, open bus, gamepak) */
   CAT_DISP,        /* dispatch: indirect branch stubs + block lookup     */
   CAT_JDISP,       /* translated code: jumps into the dispatcher + args  */
   CAT_CYC,         /* translated code: cycle update / budget check       */
   CAT_UPD,         /* mips_update_gba + update_gba (event scheduler)     */
   CAT_XLAT,        /* translation zone                                   */
   CAT_FLUSH,       /* flush / SMC zone                                   */
   CAT_VIDEO,       /* renderer                                           */
   CAT_SOUND,       /* sound                                              */
   CAT_CORE,        /* other core: dma, timers, irq, bios hle, cpu mode   */
   CAT_LIBC,        /* libc (memcpy/memset...)                            */
   CAT_HOST,        /* dr_host / frontend-common / markers                */
   CAT_OTHER,       /* unattributed                                       */
   NCAT
};
static const char *cat_name[NCAT] = {
   "jit", "jmem", "stub", "memc", "disp", "jdisp", "cyc", "upd", "xlat",
   "flush", "video", "sound", "core", "libc", "host", "other"
};

/* roles of a translated MIPS instruction */
enum
{
   R_NOP, R_ALU, R_TEMP, R_ARG, R_FLAG, R_CYC, R_CYCCHK, R_REGMEM,
   R_MEMDIR, R_CALLMEM, R_CALLOTH, R_DISPJ, R_LINKJ, R_BRANCH, NROLE
};
static const char *role_name[NROLE] = {
   "nop", "alu", "temp", "arg", "flag", "cyc", "cycchk", "regmem",
   "memdir", "callmem", "calloth", "dispj", "linkj", "branch"
};

/* ---- buckets --------------------------------------------------------------- */
static GHashTable *bk_ids;     /* name -> id+1 */
static GPtrArray *bk_names;
static GArray *bk_cat;         /* u8 */
static GArray *bk_cnt;         /* u64 */
static GArray *bk_prev;        /* u64 */

static uint32_t bucket(const char *name, int cat)
{
   gpointer v = g_hash_table_lookup(bk_ids, name);
   uint64_t z = 0;
   uint8_t c = (uint8_t)cat;
   uint32_t id;
   if (v)
      return GPOINTER_TO_UINT(v) - 1;
   id = bk_names->len;
   g_ptr_array_add(bk_names, g_strdup(name));
   g_array_append_val(bk_cat, c);
   g_array_append_val(bk_cnt, z);
   g_array_append_val(bk_prev, z);
   g_hash_table_insert(bk_ids, g_strdup(name), GUINT_TO_POINTER(id + 1));
   return id;
}

/* ---- symbols (from symcat) ------------------------------------------------- */
typedef struct { uint32_t lo, hi; char *name; int cat; } sym_t;
static GArray *syms;           /* sorted by lo */
static GHashTable *symcat_by_name;

static int sym_cmp(const void *a, const void *b)
{
   const sym_t *x = a, *y = b;
   return x->lo < y->lo ? -1 : x->lo > y->lo;
}

static const sym_t *sym_find(uint32_t a)
{
   int lo = 0, hi = (int)syms->len - 1;
   while (lo <= hi)
   {
      int mid = (lo + hi) / 2;
      const sym_t *s = &g_array_index(syms, sym_t, mid);
      if (a < s->lo) hi = mid - 1;
      else if (a >= s->hi) lo = mid + 1;
      else return s;
   }
   return NULL;
}

/* the translation caches, from the static symbols (usable before the host
 * has published drprof_ctl) */
static uint32_t csym_lo[2], csym_hi[2];
static int in_cache_sym(uint32_t a)
{
   return (a >= csym_lo[0] && a < csym_hi[0]) || (a >= csym_lo[1] && a < csym_hi[1]);
}

/* ---- run state ------------------------------------------------------------ */
static FILE *out;
static char hot_sym[128];      /* hot=<symbol>: per-address counts for it */
static uint64_t ctl_addr, zarg_addr;
static uint32_t rom_base, rom_size, ram_base, ram_size, stub_end;
static uint32_t rom_map, ram_map;
static int geom_ok;
static int zstack[32], zdepth;
static int running;             /* between START and STOP */
static uint64_t fcat[NCAT], ftot;
static char scene[64] = "prestart";
static uint32_t zone_bucket[3];

typedef struct
{
   uint32_t id;
   uint32_t n;
} bcount;

typedef struct tbrec
{
   uint64_t vaddr;
   uint32_t n;
   uint32_t *words;
   int kind;            /* 0 C/asm, 1 translated, 2 stub area, 3 marker-mark, 4 marker-zone */
   int classified;
   bcount *list;        /* attribution for one execution */
   uint32_t nlist;
   uint32_t cat_n[NCAT];
   uint32_t zb[4];      /* per-zone "zone:<z>:<symbol>" bucket, 0 = not made */
} tbrec;

static GHashTable *tb_dedup;

static guint tbkey_hash(gconstpointer k)
{
   const tbrec *r = k;
   uint32_t h = (uint32_t)r->vaddr * 2654435761u ^ r->n;
   uint32_t i;
   for (i = 0; i < r->n; i++)
      h = (h ^ r->words[i]) * 16777619u;
   return h;
}
static gboolean tbkey_eq(gconstpointer a, gconstpointer b)
{
   const tbrec *x = a, *y = b;
   return x->vaddr == y->vaddr && x->n == y->n &&
          !memcmp(x->words, y->words, x->n * 4);
}

static int read_u32s(uint64_t addr, uint32_t *dst, size_t n)
{
   GByteArray *ba = g_byte_array_new();
   int ok = qemu_plugin_read_memory_vaddr(addr, ba, n * 4);
   if (ok && ba->len >= n * 4)
      memcpy(dst, ba->data, n * 4);
   g_byte_array_unref(ba);
   return ok;
}

static void read_geom(void)
{
   uint32_t c[12];
   if (!read_u32s(ctl_addr, c, 12) || c[0] != 0x46505244u)
      return;
   rom_base = c[5]; rom_size = c[6]; ram_base = c[7]; ram_size = c[8];
   stub_end = c[9]; rom_map = c[10]; ram_map = c[11];
   geom_ok = 1;
   fprintf(out, "# geom rom=%08x+%x ram=%08x+%x stub_end=%08x\n",
           rom_base, rom_size, ram_base, ram_size, stub_end);
}

static int in_rom(uint32_t a) { return a >= rom_base && a < rom_base + rom_size; }
static int in_ram(uint32_t a) { return a >= ram_base && a < ram_base + ram_size; }
static int in_stub(uint32_t a) { return a >= rom_base && a < stub_end; }

/* ---- classification ------------------------------------------------------- */
static int writes_reg(uint32_t w)
{
   uint32_t op = w >> 26, fn = w & 63;
   if (op == 0)
   {
      if (fn == 8 || fn == 9 || fn == 12 || fn == 13 || fn == 24 || fn == 25 ||
          fn == 26 || fn == 27 || fn == 17 || fn == 19)
         return fn == 9 ? (int)((w >> 11) & 31) : -1;
      return (w >> 11) & 31;
   }
   if (op == 0x1C || op == 0x1F)   /* special2 (mul/clz) / special3 (ext/ins/seb) */
      return (op == 0x1F && (fn == 0x20)) ? (int)((w >> 11) & 31)
           : (op == 0x1F) ? (int)((w >> 16) & 31) : (int)((w >> 11) & 31);
   if ((op >= 8 && op <= 15) || (op >= 32 && op <= 38))
      return (w >> 16) & 31;
   return -1;
}

static int is_branch(uint32_t w)
{
   uint32_t op = w >> 26;
   return op == 1 || (op >= 4 && op <= 7) || (op >= 20 && op <= 23) ||
          (op == 0 && ((w & 63) == 8 || (w & 63) == 9));
}

static const char *stub_asm_disp[] = {
   "mips_indirect_branch_arm", "mips_indirect_branch_thumb",
   "mips_indirect_branch_dual", NULL
};

static int role_of(uint32_t w, uint32_t pc, uint32_t *calltarget)
{
   uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
   int wr;
   *calltarget = 0;
   if (w == 0)
      return R_NOP;
   if (op == 3 || op == 2)
   {
      uint32_t t = ((pc + 4) & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2);
      const sym_t *s;
      *calltarget = t;
      if (in_stub(t))
         return op == 3 ? R_CALLMEM : R_CALLMEM;   /* smc/io trampolines too */
      s = sym_find(t);
      if (s)
      {
         int i;
         for (i = 0; stub_asm_disp[i]; i++)
            if (!strcmp(s->name, stub_asm_disp[i]))
               return R_DISPJ;
         if (!strcmp(s->name, "mips_update_gba"))
            return R_CYCCHK;
      }
      if (in_rom(t) || in_ram(t))
         return op == 3 ? R_CALLOTH : R_LINKJ;
      return op == 3 ? R_CALLOTH : R_DISPJ;
   }
   if (op == 1 && (rt == 0x10 || rt == 0x11))
   {
      uint32_t t = pc + 4 + ((int32_t)(int16_t)(w & 0xFFFF) << 2);
      *calltarget = t;
      if (rs == 17)
         return R_CYCCHK;
      if (in_stub(t))
         return R_CALLMEM;
      return R_CALLOTH;
   }
   if (op >= 32 && op <= 46)
      return rs == 16 ? R_REGMEM : R_MEMDIR;
   if (is_branch(w))
      return R_BRANCH;
   wr = writes_reg(w);
   if (wr == 17) return R_CYC;
   if (wr >= 20 && wr <= 23) return R_FLAG;
   if (wr >= 4 && wr <= 6) return R_ARG;
   if (wr == 1 || wr == 2) return R_TEMP;
   return R_ALU;
}

static const char *gcls_name(uint32_t c)
{
   static const char *n[] = { "none", "alu", "mov", "cmp", "mul", "psr",
      "load", "load_sp", "load_lit", "store", "store_sp", "ldm", "stm",
      "b", "bl", "bx", "swi", "swp", "other" };
   uint32_t k = c & 63;
   if (k == 60) return "prologue";
   if (k == 61) return "cycle_upd";
   if (k == 62) return "tail";
   if (k == 63) return "cheat";
   return k < sizeof(n) / sizeof(n[0]) ? n[k] : "bad";
}

/* role -> coarse category for translated code */
static int role_cat(int role)
{
   switch (role)
   {
   case R_CALLMEM: return CAT_JMEM;
   case R_DISPJ:   return CAT_JDISP;
   case R_CYC:
   case R_CYCCHK:  return CAT_CYC;
   default:        return CAT_JIT;
   }
}

static void list_add(GArray *l, uint32_t id, uint32_t c)
{
   guint i;
   bcount b = { id, c };
   for (i = 0; i < l->len; i++)
      if (g_array_index(l, bcount, i).id == id)
      {
         g_array_index(l, bcount, i).n += c;
         return;
      }
   g_array_append_val(l, b);
}

/* Translated code: needs the class map, so done at first execution. */
static void classify_jit(tbrec *r)
{
   GArray *l = g_array_new(FALSE, FALSE, sizeof(bcount));
   uint32_t base = in_rom((uint32_t)r->vaddr) ? rom_base : ram_base;
   uint32_t map = in_rom((uint32_t)r->vaddr) ? rom_map : ram_map;
   uint32_t idx = ((uint32_t)r->vaddr - base) >> 2;
   uint32_t back = idx < 256 ? idx : 256, i;
   uint16_t *m = g_new0(uint16_t, back + r->n + 2);
   GByteArray *ba = g_byte_array_new();
   uint32_t owner = 0;
   char nm[96];
   if (qemu_plugin_read_memory_vaddr(map + (idx - back) * 2, ba,
                                     (back + r->n) * 2) &&
       ba->len >= (back + r->n) * 2)
      memcpy(m, ba->data, (back + r->n) * 2);
   g_byte_array_unref(ba);
   for (i = 0; i < back; i++)
      if (m[i])
         owner = m[i];
   for (i = 0; i < r->n; i++)
   {
      uint32_t w = r->words[i], pc = (uint32_t)r->vaddr + i * 4, tgt;
      uint16_t e = m[back + i];
      int role;
      if (e)
      {
         owner = e;
         if ((e & 63) < 60)
         {
            /* guest instruction dispatched: count it, by full class key */
            snprintf(nm, sizeof(nm), "gi:%s:%s%s%s%s%s%s", gcls_name(e),
                     (e & 0x0200) ? "t" : "a", (e & 0x0040) ? "S" : "",
                     (e & 0x0080) ? "F" : "", (e & 0x0100) ? "C" : "",
                     (e & 0x0400) ? "P" : "", (e & 0x0800) ? "K" : "");
            list_add(l, bucket(nm, CAT_OTHER), 1);   /* cat ignored for gi */
         }
      }
      role = role_of(w, pc, &tgt);
      snprintf(nm, sizeof(nm), "jit:%s:%s:%s", owner ? gcls_name(owner) : "unowned",
               (owner & 0x0200) ? "t" : "a", role_name[role]);
      list_add(l, bucket(nm, role_cat(role)), 1);
      if (role == R_CALLMEM && in_stub(tgt))
      {
         snprintf(nm, sizeof(nm), "mc:%s:%x", owner ? gcls_name(owner) : "unowned",
                  tgt - rom_base);
         list_add(l, bucket(nm, CAT_OTHER), 1);
      }
   }
   g_free(m);
   r->nlist = l->len;
   r->list = (bcount *)g_array_free(l, FALSE);
   memset(r->cat_n, 0, sizeof(r->cat_n));
   for (i = 0; i < r->nlist; i++)
   {
      const char *bn = g_ptr_array_index(bk_names, r->list[i].id);
      if (!strncmp(bn, "jit:", 4))
         r->cat_n[g_array_index(bk_cat, uint8_t, r->list[i].id)] += r->list[i].n;
   }
   r->classified = 1;
}

static void classify_static(tbrec *r, const char *symname)
{
   GArray *l = g_array_new(FALSE, FALSE, sizeof(bcount));
   uint32_t i;
   char nm[160];
   memset(r->cat_n, 0, sizeof(r->cat_n));
   if (r->kind == 2)
   {
      for (i = 0; i < r->n; i++)
      {
         snprintf(nm, sizeof(nm), "stub:%x",
                  (uint32_t)r->vaddr + i * 4 - rom_base);
         list_add(l, bucket(nm, CAT_STUB), 1);
      }
      r->cat_n[CAT_STUB] = r->n;
   }
   else
   {
      int cat = CAT_OTHER;
      gpointer v;
      if (!symname)
         symname = "?";
      v = g_hash_table_lookup(symcat_by_name, symname);
      if (v)
         cat = GPOINTER_TO_INT(v) - 1;
      snprintf(nm, sizeof(nm), "sym:%s", symname);
      list_add(l, bucket(nm, cat), r->n);
      r->cat_n[cat] = r->n;
      if (hot_sym[0] && !strcmp(symname, hot_sym))
      {
         /* "hot:<addr>" per instruction (NOT in the category sums) */
         for (i = 0; i < r->n; i++)
         {
            snprintf(nm, sizeof(nm), "hot:%08x", (uint32_t)r->vaddr + i * 4);
            list_add(l, bucket(nm, CAT_OTHER), 1);
         }
      }
      {
         /* call counts: executions of a TB that starts at a symbol */
         const sym_t *s = sym_find((uint32_t)r->vaddr);
         if (s && s->lo == (uint32_t)r->vaddr)
         {
            snprintf(nm, sizeof(nm), "ent:%s", s->name);
            list_add(l, bucket(nm, CAT_OTHER), 1);
         }
      }
   }
   r->nlist = l->len;
   r->list = (bcount *)g_array_free(l, FALSE);
   r->classified = 1;
}

/* ---- marks ---------------------------------------------------------------- */
static void dump_scene(void)
{
   guint i;
   for (i = 0; i < bk_names->len; i++)
   {
      uint64_t c = g_array_index(bk_cnt, uint64_t, i);
      uint64_t p = g_array_index(bk_prev, uint64_t, i);
      if (c != p)
         fprintf(out, "S %s %s %" PRIu64 "\n", scene,
                 (char *)g_ptr_array_index(bk_names, i), c - p);
      g_array_index(bk_prev, uint64_t, i) = c;
   }
}

static void on_mark(unsigned int vcpu, void *ud)
{
   uint32_t c[16];
   char name[49];
   (void)vcpu; (void)ud;
   if (!read_u32s(ctl_addr, c, 16))
      return;
   if (!geom_ok)
      read_geom();
   {
      GByteArray *ba = g_byte_array_new();
      if (qemu_plugin_read_memory_vaddr(ctl_addr + 48, ba, 48))
      {
         memcpy(name, ba->data, 48);
         name[48] = 0;
      }
      else
         strcpy(name, "?");
      g_byte_array_unref(ba);
   }
   switch (c[2])
   {
   case 1:   /* frame */
      if (running)
      {
         int k;
         fprintf(out, "F %u %u %" PRIu64, c[3], c[4], ftot);
         for (k = 0; k < NCAT; k++)
            fprintf(out, " %" PRIu64, fcat[k]);
         fputc('\n', out);
      }
      memset(fcat, 0, sizeof(fcat));
      ftot = 0;
      break;
   case 2:   /* scene */
      if (running)
         dump_scene();
      snprintf(scene, sizeof(scene), "%s", name);
      fprintf(out, "M %u %s\n", c[3], scene);
      break;
   case 3:   /* start */
      dump_scene();          /* discard boot into "prestart" */
      snprintf(scene, sizeof(scene), "%s", "start");
      running = 1;
      memset(fcat, 0, sizeof(fcat));
      ftot = 0;
      break;
   case 4:   /* stop */
      dump_scene();
      running = 0;
      break;
   }
}

static void on_zone(unsigned int vcpu, void *ud)
{
   uint32_t a;
   (void)vcpu; (void)ud;
   if (!read_u32s(zarg_addr, &a, 1))
      return;
   if (a & 1)
   {
      if (zdepth < 32)
         zstack[zdepth] = (int)(a >> 1);
      zdepth++;
   }
   else if (zdepth > 0)
      zdepth--;
}

static void on_tb(unsigned int vcpu, void *ud)
{
   tbrec *r = ud;
   uint32_t i;
   (void)vcpu;
   if (!r->classified)
   {
      /* kind 1 = somewhere in the translation caches: split stub area from
       * translated code now, in vCPU context, where guest memory is readable */
      if (!geom_ok)
         read_geom();
      if (geom_ok && in_stub((uint32_t)r->vaddr))
      {
         r->kind = 2;
         classify_static(r, NULL);
      }
      else if (geom_ok && (in_rom((uint32_t)r->vaddr) || in_ram((uint32_t)r->vaddr)))
         classify_jit(r);
      else
      {
         r->kind = 0;
         classify_static(r, "?");
      }
   }
   if (r->kind >= 3)
   {
      /* markers and twin bookkeeping cost nothing; the bookkeeping's CALL
       * COUNTS are kept (drprof_note entries = guest instructions translated) */
      if (r->kind == 5 && r->nlist > 1)
         g_array_index(bk_cnt, uint64_t, r->list[1].id) += 1;
      return;
   }
   /* a zone captures everything that is not translated/stub code */
   if (zdepth > 0 && r->kind == 0)
   {
      int z = zstack[zdepth > 32 ? 31 : zdepth - 1];
      if (z == 3)
         return;          /* DRZ_TWIN: the twin's own map clears */
      int cat = z == 1 ? CAT_XLAT : z == 2 ? CAT_FLUSH : CAT_OTHER;
      int zi = z == 1 ? 1 : z == 2 ? 2 : 0;
      uint32_t id = zone_bucket[zi];
      g_array_index(bk_cnt, uint64_t, id) += r->n;
      if (!r->zb[zi] && r->nlist)
      {
         /* which function inside the zone: "zone:xlat:<symbol>" */
         char nm[200];
         snprintf(nm, sizeof(nm), "%s:%s",
                  (char *)g_ptr_array_index(bk_names, id),
                  (char *)g_ptr_array_index(bk_names, r->list[0].id) + 4);
         r->zb[zi] = bucket(nm, cat) + 1;
      }
      if (r->zb[zi])
         g_array_index(bk_cnt, uint64_t, r->zb[zi] - 1) += r->n;
      /* call counts (ent:) and hot: addresses inside zones too; list[0] is
       * the symbol bucket, which the zone replaces */
      for (i = 1; i < r->nlist; i++)
         g_array_index(bk_cnt, uint64_t, r->list[i].id) += r->list[i].n;
      fcat[cat] += r->n;
      ftot += r->n;
      return;
   }
   for (i = 0; i < r->nlist; i++)
      g_array_index(bk_cnt, uint64_t, r->list[i].id) += r->list[i].n;
   for (i = 0; i < NCAT; i++)
      fcat[i] += r->cat_n[i];
   ftot += r->n;
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
   size_t n = qemu_plugin_tb_n_insns(tb), i;
   uint64_t va = qemu_plugin_tb_vaddr(tb);
   tbrec key, *r;
   uint32_t *words = g_new(uint32_t, n);
   const char *sym = NULL;
   (void)id;
   for (i = 0; i < n; i++)
   {
      struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
      uint32_t w = 0;
      qemu_plugin_insn_data(in, &w, 4);
      words[i] = w;
      if (i == 0)
         sym = qemu_plugin_insn_symbol(in);
   }
   key.vaddr = va; key.n = (uint32_t)n; key.words = words;
   r = g_hash_table_lookup(tb_dedup, &key);
   if (!r)
   {
      r = g_new0(tbrec, 1);
      r->vaddr = va; r->n = (uint32_t)n; r->words = words;
      if (sym && !strcmp(sym, "drprof_host_mark"))
         r->kind = 3;
      else if (sym && !strcmp(sym, "drprof_zone"))
         r->kind = 4;
      else if (in_cache_sym((uint32_t)va))
         r->kind = 1;     /* classified at first execution */
      else
      {
         const sym_t *s = sym_find((uint32_t)va);
         r->kind = 0;
         if (s)
            sym = s->name;
         else
            r->kind = 1;        /* no symbol: decided at first execution */
         if (s && !strncmp(s->name, "drprof_", 7))
            r->kind = 5;        /* twin bookkeeping: never counted */
      }
      if (r->kind == 0)
         classify_static(r, sym);
      if (r->kind >= 3)
         classify_static(r, sym);   /* counted as a symbol (host) */
      g_hash_table_insert(tb_dedup, r, r);
   }
   else
      g_free(words);

   if (r->kind == 3)
      qemu_plugin_register_vcpu_tb_exec_cb(tb, on_mark, QEMU_PLUGIN_CB_NO_REGS, r);
   else if (r->kind == 4)
      qemu_plugin_register_vcpu_tb_exec_cb(tb, on_zone, QEMU_PLUGIN_CB_NO_REGS, r);
   qemu_plugin_register_vcpu_tb_exec_cb(tb, on_tb, QEMU_PLUGIN_CB_NO_REGS, r);
}

static void at_exit(qemu_plugin_id_t id, void *p)
{
   (void)id; (void)p;
   dump_scene();
   fprintf(out, "# end\n");
   fclose(out);
}

static int load_symcat(const char *path)
{
   FILE *f = fopen(path, "r");
   char line[512];
   if (!f)
      return -1;
   while (fgets(line, sizeof(line), f))
   {
      unsigned lo, sz;
      int cat;
      char name[400];
      if (sscanf(line, "%x %x %d %399s", &lo, &sz, &cat, name) == 4)
      {
         sym_t s = { lo, lo + (sz ? sz : 4), g_strdup(name), cat };
         if (!strcmp(name, "rom_translation_cache"))
         { csym_lo[0] = lo; csym_hi[0] = lo + sz; }
         else if (!strcmp(name, "ram_translation_cache"))
         { csym_lo[1] = lo; csym_hi[1] = lo + sz; }
         else
            g_array_append_val(syms, s);
         g_hash_table_insert(symcat_by_name, g_strdup(name),
                             GINT_TO_POINTER(cat + 1));
      }
   }
   fclose(f);
   g_array_sort(syms, sym_cmp);
   return 0;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
   const char *outp = "drprof.out", *symcat = NULL;
   int i;
   (void)info;
   bk_ids = g_hash_table_new(g_str_hash, g_str_equal);
   bk_names = g_ptr_array_new();
   bk_cat = g_array_new(FALSE, FALSE, 1);
   bk_cnt = g_array_new(FALSE, FALSE, 8);
   bk_prev = g_array_new(FALSE, FALSE, 8);
   syms = g_array_new(FALSE, FALSE, sizeof(sym_t));
   symcat_by_name = g_hash_table_new(g_str_hash, g_str_equal);
   tb_dedup = g_hash_table_new(tbkey_hash, tbkey_eq);
   for (i = 0; i < argc; i++)
   {
      if (!strncmp(argv[i], "out=", 4)) outp = argv[i] + 4;
      else if (!strncmp(argv[i], "symcat=", 7)) symcat = argv[i] + 7;
      else if (!strncmp(argv[i], "ctl=", 4)) ctl_addr = strtoull(argv[i] + 4, NULL, 16);
      else if (!strncmp(argv[i], "zarg=", 5)) zarg_addr = strtoull(argv[i] + 5, NULL, 16);
      else if (!strncmp(argv[i], "hot=", 4)) snprintf(hot_sym, sizeof(hot_sym), "%s", argv[i] + 4);
   }
   if (!symcat || load_symcat(symcat) != 0 || !ctl_addr || !zarg_addr)
   {
      fprintf(stderr, "drprof: need symcat=, ctl=, zarg=\n");
      return -1;
   }
   out = fopen(outp, "w");
   if (!out)
      return -1;
   fprintf(out, "# drprof v1 cats");
   for (i = 0; i < NCAT; i++)
      fprintf(out, " %s", cat_name[i]);
   fputc('\n', out);
   zone_bucket[0] = bucket("zone:other", CAT_OTHER);
   zone_bucket[1] = bucket("zone:xlat", CAT_XLAT);
   zone_bucket[2] = bucket("zone:flush", CAT_FLUSH);
   qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
   qemu_plugin_register_atexit_cb(id, at_exit, NULL);
   return 0;
}
