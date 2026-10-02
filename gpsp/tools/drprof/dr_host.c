/* dr_host.c -- headless host for the dynarec profiling twin and the
 * differential oracle (docs/DYNAREC-PROFILE.md).
 *
 * Links the libretro core exactly like tools/linkbench/lb_host.c, but carries
 * only what the dynarec study needs:
 *
 *   --hash FILE   the per-frame guest-state oracle, SAME fields and SAME FNV
 *                 as the PSP harness `shash` and lb_host (r0-r15/CPSR, IWRAM,
 *                 EWRAM, I/O, palette, OAM, VRAM, rolling audio hash).  Two
 *                 translator variants fed the same state and inputs must print
 *                 identical lines -- tools/drprof/dr_oracle.py compares them.
 *   profiling     when built for the twin (platform=drprof-mipsel,
 *                 -DDRPROF_TWIN) it publishes the translation-cache geometry in
 *                 `drprof_ctl` and calls drprof_host_mark() once per frame and
 *                 at each script `evt`, which the qemu plugin
 *                 (drprof_plugin.c) uses to cut per-frame and per-scene rows.
 *
 * Inputs come from an autopilot script (fe_autopilot grammar).  A script
 * `state` step reloads the savestate; --load-at N loads it at frame N.
 *
 * Not part of any PSP build. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fe_host.h"
#include "fe_evt.h"
#include "fe_util.h"
#include "fe_autopilot.h"

extern unsigned char iwram[], ewram[], vram[];
extern unsigned short palette_ram[], oam_ram[], io_registers[];
extern unsigned int reg[64];
extern unsigned char vram_clean[96];

#ifdef DRPROF_TWIN
extern unsigned int tmemld[11][16], tmemst[4][16];
/* the twin is a static Linux build: the caches are ARRAYS (mips_stub.S
 * .jit section), not the RUNTIME_JIT_CACHE pointers the PSP uses */
extern unsigned char rom_translation_cache[], ram_translation_cache[];
extern unsigned char *drprof_stub_end;
extern unsigned short *drprof_rom_map, *drprof_ram_map;
#endif

/* ---- profiling control block (read by the qemu plugin) -------------------
 * Layout is part of the plugin contract: plugin reads it raw, little-endian,
 * as u32 words.  Bump DRPROF_CTL_VERSION on any change. */
#define DRPROF_CTL_VERSION 1
struct drprof_ctl_s
{
   uint32_t magic;        /* 'DRPF' */
   uint32_t version;
   uint32_t kind;         /* DRM_* of the current mark */
   uint32_t frame;        /* frames completed */
   uint32_t seg;          /* savestate segment */
   uint32_t rom_base, rom_size, ram_base, ram_size, stub_end;
   uint32_t rom_map, ram_map;
   char     name[48];     /* scene name for DRM_SCENE */
};
struct drprof_ctl_s drprof_ctl;

__attribute__((noinline, used)) void drprof_host_mark(void)
{
   __asm__ volatile("" ::: "memory");
}

/* ---- the PSP kernel's cache calls, for a core built -DJITCOH_PSP_CACHE ----
 * (tools/jitcoh, docs/JIT-COHERENCY.md).  qemu-mipsel is cache-coherent, so
 * these change nothing in the run.  Each one publishes its arguments and its
 * caller in a mailbox and writes the operation code LAST; the coherency
 * checker plugin (jitcoh_plugin.c) watches the stores to the mailbox (a QEMU
 * plugin cannot read MIPS registers) and applies the Allegrex semantics to
 * its cache model on the `op` store -- one model operation per PSP call,
 * with the PSP call's own arguments.  Unreferenced (and harmless) in a core
 * built without the define. */
struct jitcoh_mbox_s
{
   volatile uint32_t addr, size, caller, op;   /* op: JC_OP_* (written last) */
};
enum { JC_OP_DWB_RANGE = 1, JC_OP_IINV_RANGE = 2, JC_OP_DWBINV_ALL = 3,
       JC_OP_IINV_ALL = 4 };
struct jitcoh_mbox_s jitcoh_mbox;

#define JC_POST(o, p, n)                                                     \
   do {                                                                      \
      jitcoh_mbox.addr = (uint32_t)(uintptr_t)(p);                           \
      jitcoh_mbox.size = (uint32_t)(n);                                      \
      jitcoh_mbox.caller = (uint32_t)(uintptr_t)__builtin_return_address(0); \
      jitcoh_mbox.op = (o);                                                  \
   } while (0)

__attribute__((noipa, used)) void sceKernelDcacheWritebackRange(const void *p,
                                                                unsigned int n)
{
   JC_POST(JC_OP_DWB_RANGE, p, n);
}
__attribute__((noipa, used)) void sceKernelIcacheInvalidateRange(const void *p,
                                                                 unsigned int n)
{
   JC_POST(JC_OP_IINV_RANGE, p, n);
}
__attribute__((noipa, used)) void sceKernelDcacheWritebackInvalidateAll(void)
{
   JC_POST(JC_OP_DWBINV_ALL, 0, 0);
}
__attribute__((noipa, used)) void sceKernelIcacheInvalidateAll(void)
{
   JC_POST(JC_OP_IINV_ALL, 0, 0);
}

static void mark(uint32_t kind, const char *name)
{
   drprof_ctl.kind = kind;
   if (name)
      snprintf(drprof_ctl.name, sizeof(drprof_ctl.name), "%s", name);
   drprof_host_mark();
}

static uint32_t fnv32w(const void *p, size_t n)   /* == shash_fnv (main_psp.c) */
{
   const uint32_t *w = (const uint32_t *)p;
   uint32_t h = 2166136261u;
   size_t i;
   for (i = 0; i < n / 4; i++)
      h = (h ^ w[i]) * 16777619u;
   return h;
}

static uint32_t g_ahash = 2166136261u;
static unsigned long g_asamples;
static const uint16_t *g_pix;
static size_t g_pitch;

static void v_frame(const uint16_t *p, unsigned w, unsigned h, size_t pitch)
{
   (void)w; (void)h;
   if (p) { g_pix = p; g_pitch = pitch; }
}
static void a_frames(const int16_t *lr, size_t n)
{
   size_t i;
   for (i = 0; i < n * 2; i++)
      g_ahash = (g_ahash ^ (uint16_t)lr[i]) * 16777619u;
   g_asamples += n;
}
static uint32_t no_input(void) { return 0; }

/* JIT_CODE_DISCIPLINE / JIT_SYNC_STATS variants (docs/JIT-CODE-DISCIPLINE.md):
 * code-sync statistics at exit, and the writer-API audit once a frame.  Weak,
 * so every other variant links without them. */
extern void jit_stats_report(FILE *f) __attribute__((weak));
extern void jit_code_audit_now(void) __attribute__((weak));

/* The GBA RTC (Unbound, Emerald) reads the host clock; a profiling/oracle
 * run must not depend on when it was started.  gba_memory.c's documented
 * hook: pinned to 2026-09-01 12:00:00 "local". */
#include <time.h>
extern time_t (*gpsp_wallclock)(void);
static time_t pinned_clock(void) { return (time_t)1788264000; }

int main(int argc, char **argv)
{
   const char *rom = NULL, *bios = ".", *save = NULL, *state = NULL;
   const char *script = NULL, *hash_path = NULL, *log_path = "dr_host.log";
   const char *dump_dir = NULL, *dumplist = NULL, *stubmap = NULL;
   long load_at = 30, max_frames = 100000, prof_from = 0;
   FILE *hf = NULL;
   fe_host_config cfg;
   unsigned seg = 0, f;
   int i, started = 0;

   for (i = 1; i < argc; i++)
   {
#define ARG(n) (!strcmp(argv[i], n) && i + 1 < argc)
      if (ARG("--rom")) rom = argv[++i];
      else if (ARG("--bios-dir")) bios = argv[++i];
      else if (ARG("--save")) save = argv[++i];
      else if (ARG("--state")) state = argv[++i];
      else if (ARG("--script")) script = argv[++i];
      else if (ARG("--hash")) hash_path = argv[++i];
      else if (ARG("--log")) log_path = argv[++i];
      else if (ARG("--load-at")) load_at = strtol(argv[++i], NULL, 0);
      else if (ARG("--frames")) max_frames = strtol(argv[++i], NULL, 0);
      else if (ARG("--prof-from")) prof_from = strtol(argv[++i], NULL, 0);
      else if (ARG("--dump-dir")) dump_dir = argv[++i];
      else if (ARG("--dump-at")) dumplist = argv[++i];
      else if (ARG("--stubmap")) stubmap = argv[++i];
      else if (ARG("--option"))
      {
         char kv[256], *eq;
         snprintf(kv, sizeof(kv), "%s", argv[++i]);
         eq = strchr(kv, '=');
         if (!eq) return 2;
         *eq = 0;
         if (fe_host_option_set(kv, eq + 1) != 0)
         { fprintf(stderr, "unknown option %s\n", kv); return 2; }
      }
      else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
#undef ARG
   }
   if (!rom) { fprintf(stderr, "--rom required\n"); return 2; }

   gpsp_wallclock = pinned_clock;
   fe_evt_init(log_path, 0);
   memset(&cfg, 0, sizeof(cfg));
   cfg.rom_path = rom;
   cfg.system_dir = bios;
   cfg.save_path = save;
   cfg.video_frame = v_frame;
   cfg.audio_frames = a_frames;
   cfg.input_bitmask = no_input;
   if (fe_host_boot(&cfg) != 0) { fprintf(stderr, "boot failed\n"); return 3; }

   drprof_ctl.magic = 0x46505244u;   /* "DRPF" */
   drprof_ctl.version = DRPROF_CTL_VERSION;
#ifdef DRPROF_TWIN
   drprof_ctl.rom_base = (uint32_t)(uintptr_t)rom_translation_cache;
   drprof_ctl.ram_base = (uint32_t)(uintptr_t)ram_translation_cache;
   drprof_ctl.rom_size = 10u * 1024 * 1024;     /* gpsp_config.h, large tier */
   drprof_ctl.ram_size = 512u * 1024;
   drprof_ctl.stub_end = (uint32_t)(uintptr_t)drprof_stub_end;
   drprof_ctl.rom_map = (uint32_t)(uintptr_t)drprof_rom_map;
   drprof_ctl.ram_map = (uint32_t)(uintptr_t)drprof_ram_map;
#endif

#ifdef DRPROF_TWIN
   /* Name the emitter's handler stubs for the analysis (offsets from the
    * start of the rom translation cache, where init_emitter puts them). */
   if (stubmap)
   {
      static const char *rg[16] = { "bios", "open1", "ewram", "iwram", "io",
         "pal", "vram", "oam", "rom8", "rom9", "romA", "romB", "romC",
         "eeprom", "backup", "openF" };
      static const char *ld[11] = { "ld_u8", "ld_s8", "ld_u16", "ld_u16u1",
         "ld_s16", "ld_s16u1", "ld_u32", "ld_u32u1", "ld_u32u2", "ld_u32u3",
         "ld_a32" };
      static const char *st[4] = { "st_u8", "st_u16", "st_u32", "st_a32" };
      FILE *sm = fopen(stubmap, "w");
      unsigned o, r;
      if (sm)
      {
         static const char *ph[10] = { "ld_u8", "ld_s8", "ld_u16", "ld_s16",
            "ld_u32", "ld_a32", "st_u8", "st_u16", "st_u32", "st_a32" };
         for (o = 0; o < 10; o++)
            fprintf(sm, "%x patch_handler:%s\n", o * 64, ph[o]);
         fprintf(sm, "%x tramp:smc_write\n", 640);
         fprintf(sm, "%x tramp:io_epilogue\n", 648);
         fprintf(sm, "%x tramp:sp_ewram\n", 656);
         for (o = 0; o < 11; o++)
            for (r = 0; r < 16; r++)
               fprintf(sm, "%x %s:%s\n",
                       (unsigned)(tmemld[o][r] - (uintptr_t)rom_translation_cache),
                       ld[o], rg[r]);
         for (o = 0; o < 4; o++)
            for (r = 0; r < 16; r++)
               fprintf(sm, "%x %s:%s\n",
                       (unsigned)(tmemst[o][r] - (uintptr_t)rom_translation_cache),
                       st[o], rg[r]);
         fprintf(sm, "%x end_of_stubs\n",
                 (unsigned)((uintptr_t)drprof_stub_end - (uintptr_t)rom_translation_cache));
         fclose(sm);
      }
   }
#else
   (void)stubmap;
#endif

   if (script && fe_autopilot_load(script) != 0) return 4;
   if (hash_path)
   {
      hf = fopen(hash_path, "w");
      if (hf) fprintf(hf, "# drhash v1 rom=%s\n", rom);
   }

   for (f = 0; f < (unsigned)max_frames; f++)
   {
      const char *m;
      int reloaded = 0;
      if (state && load_at > 0 && f == (unsigned)load_at)
      {
         if (fe_host_state_load(state) != 0) return 5;
         seg++; reloaded = 1;
      }
      if (state && fe_autopilot_state_pending())
      {
         if (fe_host_state_load(state) != 0) return 5;
         seg++; reloaded = 1;
      }
      if (reloaded)
      {
         g_ahash = 2166136261u;
         g_asamples = 0;
      }
      if (!started && (long)f >= prof_from)
      {
         started = 1;
         drprof_ctl.seg = seg;
         drprof_ctl.frame = fe_host_frame_count();
         mark(3 /* DRM_START */, "start");
      }
      memset(vram_clean, 1, 96);
      fe_autopilot_frame();
      m = fe_autopilot_take_mark();
      if (m && started)
      {
         drprof_ctl.frame = fe_host_frame_count();
         mark(2 /* DRM_SCENE */, m);
      }
      fe_host_run_frame();
      if (jit_code_audit_now)
         jit_code_audit_now();
      if (started)
      {
         drprof_ctl.frame = fe_host_frame_count();
         drprof_ctl.seg = seg;
         mark(1 /* DRM_FRAME */, NULL);
      }

      if (hf)
         fprintf(hf, "f=%u s=%u a=%08x n=%lu r=%08x pc=%08x i=%08x e=%08x "
                 "io=%08x p=%08x o=%08x v=%08x\n",
                 fe_host_frame_count(), seg, g_ahash, g_asamples,
                 fnv32w(reg, 17 * 4), reg[15],
                 fnv32w(iwram + 0x8000, 0x8000), fnv32w(ewram, 0x40000),
                 fnv32w(io_registers, 0x400), fnv32w(palette_ram, 0x400),
                 fnv32w(oam_ram, 0x400), fnv32w(vram, 96 * 1024));
      if (dump_dir && g_pix)
      {
         int want = fe_autopilot_dump_pending();
         if (dumplist)
         {
            char key[32], buf[4096];
            snprintf(key, sizeof(key), ",%u,", fe_host_frame_count());
            snprintf(buf, sizeof(buf), ",%s,", dumplist);
            if (strstr(buf, key)) want = 1;
         }
         if (want)
         {
            char path[512];
            snprintf(path, sizeof(path), "%s/f%06u.bmp", dump_dir,
                     fe_host_frame_count());
            fe_bmp_write_rgb565(path, g_pix, 240, 160, g_pitch);
         }
      }
      if (script && fe_autopilot_status() != 0)
         break;
   }
   if (started)
      mark(4 /* DRM_STOP */, "stop");
   if (hf) fclose(hf);
   if (jit_stats_report)
      jit_stats_report(stderr);
   fprintf(stderr, "done frames=%u seg=%u ahash=%08x samples=%lu ap=%d\n",
           fe_host_frame_count(), seg, g_ahash, g_asamples,
           script ? fe_autopilot_status() : 0);
   fe_host_shutdown();
   fe_evt_close();
   return 0;
}
