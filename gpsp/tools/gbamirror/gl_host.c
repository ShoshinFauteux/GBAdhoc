/* gl_host.c -- desktop prototype of the GBA link "mirror": two gpSP cores in
 * one process, cabled together (gbalink_core.c / gbalink/), each driven by
 * its own frame-stamped input script.  Measurement and validation tool;
 * not part of any PSP build.  docs/GBA-LINK-MIRROR.md.
 *
 * Instance A is the core as linked; instance B is the same relocatable with
 * every global renamed gbab_* (tools/gbamirror/Makefile).  --swap puts slot 0
 * (the parent end of the cable) on instance B and slot 1 on A; the per-slot
 * traces must not change.
 *
 *   gl_host --rom R --bios-dir B --save0 S0 --save1 S1 --state0 T0 --state1 T1
 *           --script0 F0 --script1 F1 --frames N [--lookahead C] [--swap]
 *           [--hash OUT] [--dump-dir D] [--dump-every N] [--load-at F]
 *           [--solo] [--yield-every C] [--save-state P --save-state-at F]
 *
 *   --rom1 R         instance B loads its own, different cartridge (a
 *                    two-ROM pair such as Ruby + Sapphire; not --shared-rom)
 *   --save-state P   write slot 0's machine (retro_serialize, the PSP .st0
 *                    format) after frame F; F = 0 is right after the
 *                    --load-at boot frames, before any --state0 is loaded
 *                    (the multigame fixtures' power-on states)
 *   --save-out0/1 P  write that slot's battery image (128 KiB) at exit
 *   --ram-dump D:F0:F1:S  both slots' EWRAM+IWRAM every S frames, F0..F1
 *   --sio-trace A:B  print both ports' SIO registers at every frame end
 *                    from frame A to B
 *   SIO census       at exit, per slot: which SIO mode (normal8/normal32/
 *                    multi/uart/gpio/joybus) the port sat in, in frames,
 *                    and the first frame of each
 *
 *   --hash OUT       one line per completed frame per slot:
 *                    "s=<slot> f=<frame> r=.. i=.. e=.. io=.. p=.. o=.. v=.."
 *   --fast           the cable fast path on in both cores (gbalink_fast):
 *                    a pure speed switch, --hash must be byte-identical
 *   --fast2          its checking mode (gbalink_fast = 2): the full path
 *                    decides, the fast path's answers are compared with it;
 *                    "fastchk:" prints the count and the first disagreement
 *                    and the exit code is 8 if there was one
 *   --stress SEED    TEST: random run horizons inside the stepping rule's
 *                    limits and random extra pumps (gbalink_pair_config
 *                    stress_seed); SEED | 0x80000000 = the pumps only, which
 *                    must leave --hash unchanged.  Deterministic per seed.
 *   --solo           no link: slot 0 alone, stepped by retro_run (the
 *                    reference for the yield-invisibility test)
 *   --yield-every C  solo through the link machinery with only slot 0 and a
 *                    horizon every C cycles (the cable is attached to
 *                    nothing): must hash like --solo
 *   --ap0 F --ap1 F  drive the slots with the AUTOPILOT engine instead of
 *                    the blind reader: RAM predicates (waitram, mash, ifram,
 *                    logram ...) read each slot's own machine; EVT lines go
 *                    to --evt FILE.  Inputs are then asked for lazily, at
 *                    each frame start (gbalink_pair_config.input).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>

#include "libretro.h"
#include "../../gbalink.h"
#include "../../gbalink/gbalink_pair.h"
#include "../../frontend-common/fe_evt.h"

/* the two autopilot engines (Makefile: ap_a.o, ap_b.o) */
int  fe_autopilot_load(const char *path);
void fe_autopilot_frame(void);
int  fe_autopilot_status(void);
int  fe_autopilot_dump_pending(void);
int  gbab_fe_autopilot_load(const char *path);
void gbab_fe_autopilot_frame(void);
int  gbab_fe_autopilot_status(void);
int  gbab_fe_autopilot_dump_pending(void);

/* ---- the two instances' libretro entry points ---------------------------- */
#define CORE_DECLS(P)                                                         \
   void P##retro_set_environment(retro_environment_t);                        \
   void P##retro_set_video_refresh(retro_video_refresh_t);                    \
   void P##retro_set_audio_sample(retro_audio_sample_t);                      \
   void P##retro_set_audio_sample_batch(retro_audio_sample_batch_t);          \
   void P##retro_set_input_poll(retro_input_poll_t);                          \
   void P##retro_set_input_state(retro_input_state_t);                        \
   void P##retro_init(void);                                                  \
   bool P##retro_load_game(const struct retro_game_info *);                   \
   size_t P##retro_serialize_size(void);                                         bool P##retro_serialize(void *, size_t);                                   \
   bool P##retro_unserialize(const void *, size_t);                           \
   void *P##retro_get_memory_data(unsigned);                                  \
   void P##retro_run(void);                                                   \
   void P##gbalink_inst_get_ops(gbalink_inst_ops *);
CORE_DECLS()
CORE_DECLS(gbab_)

typedef struct
{
   void (*set_environment)(retro_environment_t);
   void (*set_video_refresh)(retro_video_refresh_t);
   void (*set_audio_sample)(retro_audio_sample_t);
   void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*set_input_poll)(retro_input_poll_t);
   void (*set_input_state)(retro_input_state_t);
   void (*init)(void);
   bool (*load_game)(const struct retro_game_info *);
   size_t (*serialize_size)(void);
   bool (*serialize)(void *, size_t);
   bool (*unserialize)(const void *, size_t);
   void *(*get_memory_data)(unsigned);
   void (*run)(void);
   void (*get_ops)(gbalink_inst_ops *);
} core_api;

#define CORE_API(P) { P##retro_set_environment, P##retro_set_video_refresh,   \
   P##retro_set_audio_sample, P##retro_set_audio_sample_batch,                \
   P##retro_set_input_poll, P##retro_set_input_state, P##retro_init,          \
   P##retro_load_game, P##retro_serialize_size, P##retro_serialize,              P##retro_unserialize,                                                      \
   P##retro_get_memory_data, P##retro_run, P##gbalink_inst_get_ops }

/* shared ROM (gba_memory.c, GBA_LINK) */
extern uint32_t gamepak_buffer_cap;
extern const gbalink_rom_share *gbab_gamepak_share_in;
extern void (*gamepak_evict_hook)(uint32_t);
void gamepak_share_export(gbalink_rom_share *);
void gbab_gamepak_share_unmap(uint32_t);
extern uint32_t gamepak_page_loads, gbab_gamepak_page_loads;
static gbalink_rom_share g_share;

static const core_api api_inst[2] = { CORE_API(), CORE_API(gbab_) };

/* SIO mode census (tools/gbamirror/multigame): each instance's SIOCNT/RCNT
 * sampled at every frame end -- which mode, and in normal mode which width,
 * the game leaves the port in.  Read straight from each core's I/O array
 * (instance B's is gbab_io_registers). */
extern uint16_t io_registers[512];
extern uint16_t gbab_io_registers[512];
enum { SIOM_N8, SIOM_N32, SIOM_MULTI, SIOM_UART, SIOM_GPIO, SIOM_JOY, SIOM_N };
static const char *siom_name[SIOM_N] = { "normal8", "normal32", "multi",
                                          "uart", "gpio", "joybus" };
static uint32_t g_siom[2][SIOM_N];
static uint32_t g_siom_first[2][SIOM_N];
static uint32_t g_sioxf[2][SIOM_N], g_sio_last[2][2];
static uint32_t g_sio_trace0 = 1, g_sio_trace1 = 0;   /* --sio-trace F0:F1 */
static gbalink_inst_ops g_ops[2];   /* by slot */
static void sio_census(unsigned slot, unsigned inst, uint32_t frame)
{
   const uint16_t *io = inst ? gbab_io_registers : io_registers;
   uint16_t sc = io[0x128 / 2], rc = io[0x134 / 2];
   int m;
   if (rc & 0x8000)
      m = (rc & 0x4000) ? SIOM_JOY : SIOM_GPIO;
   else if (sc & 0x2000)
      m = (sc & 0x1000) ? SIOM_UART : SIOM_MULTI;
   else
      m = (sc & 0x1000) ? SIOM_N32 : SIOM_N8;
   if (!g_siom[slot][m]++)
      g_siom_first[slot][m] = frame;
   if (frame >= g_sio_trace0 && frame <= g_sio_trace1)
      printf("siotrace s=%u f=%u cnt=%04x rcnt=%04x m=%04x,%04x,%04x,%04x "
             "send=%04x d32=%04x%04x ie=%04x if=%04x\n", slot, frame, sc, rc,
             io[0x120 / 2], io[0x122 / 2], io[0x124 / 2], io[0x126 / 2],
             io[0x12A / 2], io[0x122 / 2], io[0x120 / 2], io[0x200 / 2],
             io[0x202 / 2]);
   /* transfers completed this frame, by the width the port is set to */
   if (g_ops[slot].get_stats)
   {
      gbalink_inst_stats st;
      g_ops[slot].get_stats(&st);
      g_sioxf[slot][SIOM_MULTI] += st.mp_xfers - g_sio_last[slot][0];
      g_sioxf[slot][(sc & 0x1000) ? SIOM_N32 : SIOM_N8] +=
         st.n_xfers - g_sio_last[slot][1];
      g_sio_last[slot][0] = st.mp_xfers;
      g_sio_last[slot][1] = st.n_xfers;
   }
}
static const char *g_save_state_path;
static const char *g_save_out[2];
/* --ram-dump DIR:F0:F1:STEP -- each slot's EWRAM (256 KiB) + IWRAM (32 KiB)
 * at every STEP-th frame end from F0 to F1, as DIR/r<slot>_<frame>.bin: RAM
 * searches for a fixture's predicates (multigame/README in GBA-LINK-MIRROR 16) */
static char g_ramdir[400];
static long g_ram0 = -1, g_ram1 = -1, g_ramstep = 1;
static void ram_dump(unsigned slot, uint32_t frame)
{
   static uint8_t buf[0x48000];
   char path[512];
   FILE *f;
   if (!g_ramdir[0] || (long)frame < g_ram0 || (long)frame > g_ram1 ||
       ((long)frame - g_ram0) % g_ramstep || !g_ops[slot].peek)
      return;
   if (g_ops[slot].peek(0x02000000, buf, 0x40000) ||
       g_ops[slot].peek(0x03000000, buf + 0x40000, 0x8000))
      return;
   snprintf(path, sizeof(path), "%s/r%u_%06u.bin", g_ramdir, slot, frame);
   if ((f = fopen(path, "wb")))
   {
      fwrite(buf, 1, sizeof(buf), f);
      fclose(f);
   }
}
static long g_save_state_at = -1;
static void save_state_now(unsigned inst)
{
   const core_api *c = &api_inst[inst];
   size_t n = c->serialize_size();
   void *b = malloc(n);
   FILE *f;
   if (!b || !c->serialize(b, n) || !(f = fopen(g_save_state_path, "wb")))
   {
      fprintf(stderr, "save state %s failed\n", g_save_state_path);
      exit(8);
   }
   fwrite(b, 1, n, f);
   fclose(f);
   free(b);
   printf("saved state %s (%zu bytes)\n", g_save_state_path, n);
}

/* ---- per-instance frontend state ----------------------------------------- */
static const char *g_bios_dir = ".";
static const uint16_t *g_pix[2];
static size_t g_pitch[2];
static uint16_t g_solo_keys[2];

static bool env_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
   case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
   case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *(const char **)data = g_bios_dir;
      return true;
   case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;
   case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
      return true;
   default:
      return false;
   }
}

static void video0(const void *d, unsigned w, unsigned h, size_t p)
{ (void)w; (void)h; if (d) { g_pix[0] = d; g_pitch[0] = p; } }
static void video1(const void *d, unsigned w, unsigned h, size_t p)
{ (void)w; (void)h; if (d) { g_pix[1] = d; g_pitch[1] = p; } }
static void asample(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t abatch(const int16_t *d, size_t n) { (void)d; return n; }
static void ipoll(void) { }
/* Solo mode reads buttons through the callback; GBA bit order -> retropad
 * bitmask (the core maps back).  Order: A B SELECT START RIGHT LEFT UP DOWN
 * R L. */
static const unsigned gba_to_rp[10] = {
   RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
   RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
   RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT,
   RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
   RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_L };
static int16_t istate_k(unsigned inst, unsigned id)
{
   uint16_t k = g_solo_keys[inst];
   int16_t m = 0;
   unsigned i;
   for (i = 0; i < 10; i++)
      if (k & (1u << i))
         m |= (int16_t)(1 << gba_to_rp[i]);
   if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
      return m;
   return (m >> id) & 1;
}
static int16_t istate0(unsigned port, unsigned dev, unsigned idx, unsigned id)
{ (void)dev; (void)idx; return port ? 0 : istate_k(0, id); }
static int16_t istate1(unsigned port, unsigned dev, unsigned idx, unsigned id)
{ (void)dev; (void)idx; return port ? 0 : istate_k(1, id); }

/* ---- tiny input-script reader (the blind subset of fe_autopilot) ---------- */
typedef struct { uint16_t *keys; uint8_t *dump; char **mark; unsigned n; } script_t;

static uint16_t parse_btns(const char *s)
{
   static const char *nm[10] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT",
                                 "UP", "DOWN", "R", "L" };
   uint16_t k = 0;
   char buf[128], *tok, *save = NULL;
   snprintf(buf, sizeof(buf), "%s", s);
   for (tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save))
   {
      unsigned i;
      for (i = 0; i < 10; i++)
         if (!strcmp(tok, nm[i]))
            k |= (uint16_t)(1u << i);
   }
   return k;
}

static void sc_push(script_t *sc, uint16_t k, unsigned n, unsigned cap)
{
   while (n-- && sc->n < cap)
      sc->keys[sc->n++] = k;
}

static int load_script(const char *path, script_t *sc, unsigned cap)
{
   FILE *f = fopen(path, "r");
   char line[256];
   char rep[64][256];
   int in_rep = 0, nrep = 0, reps = 0;
   sc->keys = calloc(cap, sizeof(uint16_t));
   sc->dump = calloc(cap, 1);
   sc->mark = calloc(cap, sizeof(char *));
   sc->n = 0;
   if (!f)
      return -1;
   for (;;)
   {
      const char *lines[64];
      int nl = 0, it, li;
      char *p;
      if (!fgets(line, sizeof(line), f))
         break;
      if ((p = strpbrk(line, "#;\r\n")))
         *p = 0;
      if (!line[0])
         continue;
      if (!strncmp(line, "repeat", 6))
      {
         in_rep = 1; nrep = 0; reps = atoi(line + 6);
         continue;
      }
      if (in_rep && strncmp(line, "endrepeat", 9))
      {
         if (nrep < 64)
            snprintf(rep[nrep++], 256, "%s", line);
         continue;
      }
      if (in_rep)
      {
         in_rep = 0;
         for (li = 0; li < nrep; li++)
            lines[nl++] = rep[li];
      }
      else
      {
         lines[0] = line; nl = 1; reps = 1;
      }
      for (it = 0; it < reps; it++)
         for (li = 0; li < nl; li++)
         {
            char cmd[32], a1[128];
            int n2 = 0;
            a1[0] = 0;
            if (sscanf(lines[li], "%31s %127s %d", cmd, a1, &n2) < 1)
               continue;
            if (!strcmp(cmd, "wait"))
               sc_push(sc, 0, (unsigned)atoi(a1), cap);
            else if (!strcmp(cmd, "press"))
            {
               sc_push(sc, parse_btns(a1), n2 > 0 ? (unsigned)n2 : 2, cap);
               sc_push(sc, 0, 2, cap);
            }
            else if (!strcmp(cmd, "hold"))
               sc_push(sc, parse_btns(a1), (unsigned)n2, cap);
            else if (!strcmp(cmd, "dump"))
            {
               if (sc->n < cap)
                  sc->dump[sc->n] = 1;
            }
            else if (!strcmp(cmd, "evt"))
            {
               if (sc->n < cap)
                  sc->mark[sc->n] = strdup(a1);
            }
         }
      reps = 1;
   }
   fclose(f);
   return 0;
}

/* ---- DRPROF_TWIN: the PAIR under the dynarec twin's instruction counter ----
 * (tools/drprof/Makefile `pair`, docs/GBA-LINK-MIRROR.md 23).  Built for
 * platform=drprof-mipsel and run under qemu-mipsel with drprof_plugin.so:
 * instance A's translation caches are published in drprof_ctl (same block
 * and marks as tools/drprof/dr_host.c), so A's emitted code is split by role
 * and class; instance B is the renamed copy, counted per gbab_* symbol
 * (dr_symcat.py gives each the category of the symbol it was renamed from;
 * its caches are the buckets gbab_rom/ram_translation_cache).  Scenes are cut
 * at slot-0 frames given by --scenes "0:boot,2600:pre,3000:fight,...". */
#ifdef DRPROF_TWIN
extern unsigned char rom_translation_cache[], ram_translation_cache[];
extern unsigned char *drprof_stub_end;
extern unsigned short *drprof_rom_map, *drprof_ram_map;
extern unsigned int tmemld[11][16], tmemst[4][16];
struct drprof_ctl_s
{
   uint32_t magic, version, kind, frame, seg;
   uint32_t rom_base, rom_size, ram_base, ram_size, stub_end;
   uint32_t rom_map, ram_map;
   char     name[48];
   /* v2 (dr_host.c; machine A only): per-word guest pc / opcode / block maps */
   uint32_t rom_gpc, ram_gpc, rom_gop, ram_gop, rom_blk, ram_blk;
};
struct drprof_ctl_s drprof_ctl;
__attribute__((noinline, used)) void drprof_host_mark(void)
{
   __asm__ volatile("" ::: "memory");
}
static void dr_mark(uint32_t kind, const char *name)
{
   drprof_ctl.kind = kind;
   if (name)
      snprintf(drprof_ctl.name, sizeof(drprof_ctl.name), "%s", name);
   drprof_host_mark();
}
static struct { uint32_t f; char name[40]; } dr_sc[16];
static unsigned dr_nsc, dr_isc;
static const char *dr_stubmap;
static void dr_scenes_parse(const char *s)
{
   while (*s && dr_nsc < 16)
   {
      char *e;
      unsigned k = 0;
      dr_sc[dr_nsc].f = (uint32_t)strtoul(s, &e, 0);
      if (*e != ':')
         break;
      for (s = e + 1; *s && *s != ',' && k < 39; s++)
         dr_sc[dr_nsc].name[k++] = *s;
      dr_sc[dr_nsc++].name[k] = 0;
      if (*s == ',')
         s++;
   }
}
/* The geometry must be published before ANY emulated instruction runs: the
 * plugin classifies each qemu translation block once, at its first
 * execution, and a block of instance A's cache met before the geometry is
 * known stays "?" for the whole run. */
static void dr_geom(void)
{
   drprof_ctl.magic = 0x46505244u;
   drprof_ctl.version = 2;
   drprof_ctl.rom_base = (uint32_t)(uintptr_t)rom_translation_cache;
   drprof_ctl.ram_base = (uint32_t)(uintptr_t)ram_translation_cache;
   drprof_ctl.rom_size = 10u * 1024 * 1024;
   {
      extern uint32_t drprof_ram_cache_bytes;
      drprof_ctl.ram_size = drprof_ram_cache_bytes;
   }
   {
      extern uint32_t *drprof_rom_gpc, *drprof_ram_gpc, *drprof_rom_gop,
                      *drprof_ram_gop, *drprof_rom_blk, *drprof_ram_blk;
      drprof_ctl.rom_gpc = (uint32_t)(uintptr_t)drprof_rom_gpc;
      drprof_ctl.ram_gpc = (uint32_t)(uintptr_t)drprof_ram_gpc;
      drprof_ctl.rom_gop = (uint32_t)(uintptr_t)drprof_rom_gop;
      drprof_ctl.ram_gop = (uint32_t)(uintptr_t)drprof_ram_gop;
      drprof_ctl.rom_blk = (uint32_t)(uintptr_t)drprof_rom_blk;
      drprof_ctl.ram_blk = (uint32_t)(uintptr_t)drprof_ram_blk;
   }
   drprof_ctl.stub_end = (uint32_t)(uintptr_t)drprof_stub_end;
   drprof_ctl.rom_map = (uint32_t)(uintptr_t)drprof_rom_map;
   drprof_ctl.ram_map = (uint32_t)(uintptr_t)drprof_ram_map;
   if (dr_stubmap)
   {
      static const char *rg[16] = { "bios", "open1", "ewram", "iwram", "io",
         "pal", "vram", "oam", "rom8", "rom9", "romA", "romB", "romC",
         "eeprom", "backup", "openF" };
      static const char *ld[11] = { "ld_u8", "ld_s8", "ld_u16", "ld_u16u1",
         "ld_s16", "ld_s16u1", "ld_u32", "ld_u32u1", "ld_u32u2", "ld_u32u3",
         "ld_a32" };
      static const char *st[4] = { "st_u8", "st_u16", "st_u32", "st_a32" };
      FILE *sm = fopen(dr_stubmap, "w");
      unsigned o, r;
      if (sm)
      {
         for (o = 0; o < 11; o++)
            for (r = 0; r < 16; r++)
               fprintf(sm, "%08x %s_%s\n", tmemld[o][r] -
                       (unsigned)(uintptr_t)rom_translation_cache, ld[o], rg[r]);
         for (o = 0; o < 4; o++)
            for (r = 0; r < 16; r++)
               fprintf(sm, "%08x %s_%s\n", tmemst[o][r] -
                       (unsigned)(uintptr_t)rom_translation_cache, st[o], rg[r]);
         fprintf(sm, "%08x stub_end\n",
                 (unsigned)((uintptr_t)drprof_stub_end -
                            (uintptr_t)rom_translation_cache));
         fclose(sm);
      }
   }
}
static void dr_begin(void)
{
   dr_mark(3 /* DRM_START */, "start");
}
static void dr_frame(uint32_t frame)
{
   drprof_ctl.frame = frame;
   while (dr_isc < dr_nsc && frame >= dr_sc[dr_isc].f)
      dr_mark(2 /* DRM_SCENE */, dr_sc[dr_isc++].name);
   dr_mark(1 /* DRM_FRAME */, NULL);
}
#endif

/* ---- autopilot host hooks, one set per slot -------------------------------- */
static uint32_t g_ap_mask[2];      /* retropad mask the engine injected */
static int g_use_ap;

void fe_host_input_inject(uint32_t m) { g_ap_mask[0] = m; }
void gbab_fe_host_input_inject(uint32_t m) { g_ap_mask[1] = m; }
int fe_host_mem_read(uint32_t a, void *o, unsigned n)
{ return g_ops[0].peek ? g_ops[0].peek(a, o, n) : -1; }
int gbab_fe_host_mem_read(uint32_t a, void *o, unsigned n)
{ return g_ops[1].peek ? g_ops[1].peek(a, o, n) : -1; }
uint32_t fe_host_sram_crc_now(void)
{ return g_ops[0].sram_crc ? g_ops[0].sram_crc() : 0; }
uint32_t gbab_fe_host_sram_crc_now(void)
{ return g_ops[1].sram_crc ? g_ops[1].sram_crc() : 0; }

static uint16_t keys_from_retro(uint32_t pad)
{
   uint16_t k = 0;
   unsigned i;
   for (i = 0; i < 10; i++)
      if (pad & (1u << gba_to_rp[i]))
         k |= (uint16_t)(1u << i);
   return k;
}

static int g_ap_dump[2];

/* gbalink_pair_config.input: run that slot's engine for the frame about to
 * start (it reads the RAM the previous frame left, like the PSP harness). */
static int ap_input(void *user, unsigned slot, uint32_t frame, uint16_t *keys)
{
   (void)user; (void)frame;
   if (slot == 0)
   {
      fe_autopilot_frame();
      g_ap_dump[0] |= fe_autopilot_dump_pending();
   }
   else
   {
      gbab_fe_autopilot_frame();
      g_ap_dump[1] |= gbab_fe_autopilot_dump_pending();
   }
   *keys = keys_from_retro(g_ap_mask[slot]);
   return 1;
}

/* ---- output -------------------------------------------------------------- */
static FILE *g_hash;
static const char *g_dump_dir;
static long g_dump_every;
static script_t g_sc[2];
static unsigned g_inst_of_slot[2];

static void write_bmp(const char *path, const uint16_t *pix, size_t pitch)
{
   FILE *f = fopen(path, "wb");
   unsigned w = 240, h = 160, y, x;
   uint32_t row = w * 3, sz = 54 + row * h;
   uint8_t hd[54];
   if (!f || !pix)
   {
      if (f) fclose(f);
      return;
   }
   memset(hd, 0, sizeof(hd));
   hd[0] = 'B'; hd[1] = 'M';
   memcpy(hd + 2, &sz, 4);
   hd[10] = 54; hd[14] = 40;
   memcpy(hd + 18, &w, 4);
   memcpy(hd + 22, &h, 4);
   hd[26] = 1; hd[28] = 24;
   fwrite(hd, 1, 54, f);
   for (y = 0; y < h; y++)
   {
      const uint16_t *r = (const uint16_t *)((const uint8_t *)pix +
                                             (h - 1 - y) * pitch);
      for (x = 0; x < w; x++)
      {
         uint16_t c = r[x];
         uint8_t px[3];
         px[2] = (uint8_t)(((c >> 11) & 31) << 3);
         px[1] = (uint8_t)(((c >> 5) & 63) << 2);
         px[0] = (uint8_t)((c & 31) << 3);
         fwrite(px, 1, 3, f);
      }
   }
   fclose(f);
}

static int g_sdiag;   /* --sdiag: B's clock and counters beside each S line */
static void emit_hash(unsigned slot, uint32_t frame)
{
   uint32_t h[8];
   g_ops[slot].hash(h);
   /* the session's sync hash (fe_gblink.c pair_sync_hash): slot 1's half at
    * its own boundary, the frame before slot 0's */
   if (g_hash && slot == 1 && (frame + 1) % 60 == 0)
      fprintf(g_hash, "S f=%u h=%08x\n", frame + 1, h[7]);
   /* the 4.5.0 sync hash: the OTHER slot as slot 0's frame ends (depends on
    * how slot 1's run was sliced) */
   if (g_hash && slot == 0 && frame % 60 == 0)
   {
      uint32_t o[8];
      g_ops[1].hash(o);
      fprintf(g_hash, "O f=%u r=%08x i=%08x e=%08x io=%08x p=%08x o=%08x v=%08x\n",
              frame, o[0], o[1], o[2], o[3], o[4], o[5], o[6]);
      if (g_sdiag)
      {
         extern uint32_t gbab_reg[];
         extern uint16_t gbab_io_registers[];
         extern int32_t gbab_video_count, gbab_execute_cycles;
         fprintf(g_hash, "D f=%u now=%llu pc=%08x halt=%u vc=%u ds=%04x if=%04x "
                 "vcnt=%d ex=%d\n", frame,
                 (unsigned long long)g_ops[1].now(), gbab_reg[15],
                 gbab_reg[18 /* CPU_HALT_STATE */], gbab_io_registers[0x06 / 2],
                 gbab_io_registers[0x04 / 2], gbab_io_registers[0x202 / 2],
                 gbab_video_count, gbab_execute_cycles);
      }
   }
   if (g_hash)
      fprintf(g_hash, "s=%u f=%u r=%08x i=%08x e=%08x io=%08x p=%08x o=%08x "
              "v=%08x\n", slot, frame, h[0], h[1], h[2], h[3], h[4], h[5], h[6]);
}

static void frame_out(unsigned slot, uint32_t frame)
{
   unsigned inst = g_inst_of_slot[slot];
   uint32_t idx = frame - 1;   /* script index of the frame just finished */
   emit_hash(slot, frame);
   sio_census(slot, inst, frame);
   ram_dump(slot, frame);
   if (slot == 0 && g_save_state_path && (long)frame == g_save_state_at)
      save_state_now(inst);
   if (idx < g_sc[slot].n && g_sc[slot].mark[idx])
      printf("mark s=%u f=%u %s\n", slot, frame, g_sc[slot].mark[idx]);
   if (g_dump_dir && ((idx < g_sc[slot].n && g_sc[slot].dump[idx]) ||
                      g_ap_dump[slot] ||
                      (g_dump_every && frame % g_dump_every == 0)))
   {
      char path[512];
      snprintf(path, sizeof(path), "%s/s%u_%06u.bmp", g_dump_dir, slot, frame);
      write_bmp(path, g_pix[inst], g_pitch[inst]);
      g_ap_dump[slot] = 0;
   }
}

static void on_frame(void *user, unsigned slot, uint32_t frame)
{
   (void)user;
   frame_out(slot, frame);
}

static void *read_file(const char *path, size_t *n)
{
   FILE *f = fopen(path, "rb");
   void *b;
   long sz;
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   b = malloc(sz ? sz : 1);
   *n = fread(b, 1, sz, f);
   fclose(f);
   return b;
}

static double now_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
   const char *rom = NULL, *save[2] = { 0 }, *state[2] = { 0 },
              *script[2] = { 0 }, *hash = NULL, *ap[2] = { 0 },
              *evt = NULL, *rom1 = NULL;
   long frames = 1000, load_at = 30, yield_every = 0;
   uint64_t lookahead = 0;          /* 0 = GBALINK_DEFAULT_LOOKAHEAD */
   int swap = 0, solo = 0, shared_rom = 0, i, s;
   uint32_t stress = 0;
   int headless1 = 0;
   int fast_bad = 0;
   long rom_cap = 32;
   gbalink_pair pair;
   gbalink_pair_config cfg;
   double t0;

   for (i = 1; i < argc; i++)
   {
#define ARG(x) (!strcmp(argv[i], x) && i + 1 < argc)
      if (ARG("--rom")) rom = argv[++i];
      else if (ARG("--rom1")) rom1 = argv[++i];   /* instance B's own cart */
      else if (ARG("--bios-dir")) g_bios_dir = argv[++i];
      else if (ARG("--save0")) save[0] = argv[++i];
      else if (ARG("--save1")) save[1] = argv[++i];
      else if (ARG("--state0")) state[0] = argv[++i];
      else if (ARG("--state1")) state[1] = argv[++i];
      else if (ARG("--script0")) script[0] = argv[++i];
      else if (ARG("--script1")) script[1] = argv[++i];
      else if (ARG("--ap0")) ap[0] = argv[++i];
      else if (ARG("--ap1")) ap[1] = argv[++i];
      else if (ARG("--evt")) evt = argv[++i];
      else if (ARG("--frames")) frames = strtol(argv[++i], NULL, 0);
      else if (ARG("--lookahead")) lookahead = strtoull(argv[++i], NULL, 0);
      else if (ARG("--hash")) hash = argv[++i];
      else if (ARG("--dump-dir")) g_dump_dir = argv[++i];
      else if (ARG("--dump-every")) g_dump_every = strtol(argv[++i], NULL, 0);
      else if (ARG("--load-at")) load_at = strtol(argv[++i], NULL, 0);
      else if (ARG("--yield-every")) yield_every = strtol(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--swap")) swap = 1;
      else if (!strcmp(argv[i], "--shared-rom")) shared_rom = 1;
      else if (ARG("--rom-cap")) rom_cap = strtol(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--solo")) solo = 1;
      /* the cable fast path in both cores (gbalink_core.c gbalink_fast,
       * docs/GBA-LINK-MIRROR.md 21): --hash must not change */
      else if (!strcmp(argv[i], "--fast") || !strcmp(argv[i], "--fast2") ||
               !strcmp(argv[i], "--fast3"))
      {
         /* --fast3: the fast path plus the lean inline hooks (section 23) */
         extern uint32_t gbalink_fast, gbab_gbalink_fast;
         gbalink_fast = gbab_gbalink_fast =
            argv[i][6] ? (uint32_t)(argv[i][6] - '0') : 1;
      }
      else if (ARG("--stress")) stress = (uint32_t)strtoul(argv[++i], NULL, 0);
      /* slot 1 never draws and mixes no sound, as the partner machine on a
       * console (the profile's machine B) */
      else if (!strcmp(argv[i], "--headless1")) headless1 = 1;
      /* ...and its sound ring is not read out (gba_link_headless_audio = 2) */
      else if (!strcmp(argv[i], "--headless1q"))
      {
         extern uint32_t gbalink_headless_audio, gbab_gbalink_headless_audio;
         headless1 = 1;
         gbalink_headless_audio = gbab_gbalink_headless_audio = 2;
      }
      /* the joining console's view: slot 0 is the partner (headless, quiet)
       * and slot 1 the local machine.  The session's sync hash must not
       * care which console computed it. */
      else if (!strcmp(argv[i], "--sdiag")) g_sdiag = 1;
      else if (!strcmp(argv[i], "--headless0q"))
      {
         extern uint32_t gbalink_headless_audio, gbab_gbalink_headless_audio;
         headless1 = -1;
         gbalink_headless_audio = gbab_gbalink_headless_audio = 2;
      }
#ifdef DRPROF_TWIN
      else if (ARG("--scenes")) dr_scenes_parse(argv[++i]);
      else if (ARG("--stubmap")) dr_stubmap = argv[++i];
#endif
      else if (ARG("--save-state")) g_save_state_path = argv[++i];
      else if (ARG("--save-out0")) g_save_out[0] = argv[++i];
      else if (ARG("--ram-dump"))
      {
         char *c;
         snprintf(g_ramdir, sizeof(g_ramdir), "%s", argv[++i]);
         if ((c = strchr(g_ramdir, ':')))
         {
            *c++ = 0;
            sscanf(c, "%ld:%ld:%ld", &g_ram0, &g_ram1, &g_ramstep);
            if (g_ramstep < 1)
               g_ramstep = 1;
         }
      }
      else if (ARG("--save-out1")) g_save_out[1] = argv[++i];
      else if (ARG("--sio-trace"))
      {
         char *e;
         g_sio_trace0 = (uint32_t)strtoul(argv[++i], &e, 0);
         g_sio_trace1 = *e == ':' ? (uint32_t)strtoul(e + 1, NULL, 0) : g_sio_trace0;
      }
      else if (ARG("--save-state-at")) g_save_state_at = strtol(argv[++i], NULL, 0);
      else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
   }
   if (!rom) { fprintf(stderr, "--rom required\n"); return 2; }
   if (hash && !(g_hash = fopen(hash, "w"))) return 3;

   g_inst_of_slot[0] = swap ? 1 : 0;
   g_inst_of_slot[1] = swap ? 0 : 1;

   for (s = 0; s < 2; s++)
   {
      if (script[s] && load_script(script[s], &g_sc[s], (unsigned)frames + 16))
      {
         fprintf(stderr, "script %s\n", script[s]);
         return 3;
      }
      if (!script[s])
         load_script("/dev/null", &g_sc[s], (unsigned)frames + 16);
   }

   /* Boot both instances, load the same cartridge, each its own save, run
    * load_at frames, then load each its state (the feasibility "cold" arm). */
   gamepak_buffer_cap = (uint32_t)rom_cap;   /* instance A's ROM pool */
   for (i = 0; i < 2; i++)
   {
      const core_api *c = &api_inst[i];
      struct retro_game_info gi;
      if (i == 1 && shared_rom)
      {
         /* B borrows A's ROM pool (gbalink.h); A's evictions unmap in B. */
         gamepak_share_export(&g_share);
         gbab_gamepak_share_in = &g_share;
         gamepak_evict_hook = gbab_gamepak_share_unmap;
      }
      c->set_environment(env_cb);
      c->set_video_refresh(i ? video1 : video0);
      c->set_audio_sample(asample);
      c->set_audio_sample_batch(abatch);
      c->set_input_poll(ipoll);
      c->set_input_state(i ? istate1 : istate0);
      c->init();
      memset(&gi, 0, sizeof(gi));
      gi.path = (i == 1 && rom1) ? rom1 : rom;
      if (!c->load_game(&gi))
      {
         fprintf(stderr, "load_game failed (instance %d)\n", i);
         return 4;
      }
   }
   for (s = 0; s < 2; s++)
   {
      const core_api *c = &api_inst[g_inst_of_slot[s]];
      size_t n = 0;
      void *b;
      if (save[s] && (b = read_file(save[s], &n)))
      {
         memcpy(c->get_memory_data(RETRO_MEMORY_SAVE_RAM), b,
                n > 0x20000 ? 0x20000 : n);
         free(b);
      }
   }
#ifdef DRPROF_TWIN
   dr_geom();
#endif
   for (i = 0; i < load_at; i++)
   {
      api_inst[0].run();
      api_inst[1].run();
   }
   /* --save-state-at 0: the machine as a session would start it -- power-on
    * plus load_at frames with the battery in (multigame/mkstate.sh) */
   if (g_save_state_path && g_save_state_at == 0)
      save_state_now(0);
   for (s = 0; s < 2; s++)
   {
      const core_api *c = &api_inst[g_inst_of_slot[s]];
      size_t n = 0;
      void *b;
      if (!state[s])
         continue;
      b = read_file(state[s], &n);
      if (!b || n != c->serialize_size() || !c->unserialize(b, n))
      {
         fprintf(stderr, "state %s failed (n=%zu want %zu)\n", state[s], n,
                 c->serialize_size());
         return 5;
      }
      free(b);
   }
   for (s = 0; s < 2; s++)
   {
      api_inst[g_inst_of_slot[s]].get_ops(&g_ops[s]);
      g_ops[s].name = g_inst_of_slot[s] ? "B" : "A";
   }

   if (ap[0] || ap[1])
   {
      g_use_ap = 1;
      if (evt)
         fe_evt_init(evt, 0);
      if (!ap[0] || !ap[1] || fe_autopilot_load(ap[0]) != 0 ||
          gbab_fe_autopilot_load(ap[1]) != 0)
      {
         fprintf(stderr, "--ap0 and --ap1 both, loadable\n");
         return 3;
      }
   }

   t0 = now_s();
   if (solo)
   {
      /* Reference: slot 0 by retro_run, no link code involved. */
      const core_api *c = &api_inst[g_inst_of_slot[0]];
      long f;
      for (f = 0; f < frames; f++)
      {
         g_solo_keys[g_inst_of_slot[0]] = f < (long)g_sc[0].n ? g_sc[0].keys[f] : 0;
         c->run();
         frame_out(0, (uint32_t)f + 1);
      }
   }
   else if (yield_every)
   {
      /* Slot 0 alone through the link run loop, stopped every C cycles.  The
       * cable is attached to nothing (slot 1 never begins). */
      long f = 0;
      gbalink_bus_reset();
      g_ops[0].begin(0, 0);
      while (f < frames)
      {
         int r;
         if (g_ops[0].need_begin())
            g_ops[0].frame_begin(f < (long)g_sc[0].n ? g_sc[0].keys[f] : 0);
         r = g_ops[0].run(g_ops[0].now() + (uint64_t)yield_every);
         if (r == GBALINK_RUN_FRAME)
            frame_out(0, (uint32_t)++f);
         else if (r != GBALINK_RUN_HORIZON)
         {
            fprintf(stderr, "run=%d at frame %ld\n", r, f);
            return 6;
         }
      }
   }
   else
   {
      uint32_t fed[2] = { 0, 0 };
      memset(&cfg, 0, sizeof(cfg));
      cfg.lookahead = lookahead;
      cfg.local_slot = headless1 < 0 ? 1 : 0;
      cfg.on_frame = on_frame;
      cfg.stress_seed = stress;
      cfg.headless[0] = headless1 < 0;
      cfg.headless[1] = headless1 > 0;
      if (g_use_ap)
         cfg.input = ap_input;
      /* both drawn on the desktop, so dumps work; rendering is not guest
       * state */
      gbalink_pair_init(&pair, &g_ops[0], &g_ops[1], &cfg);
#ifdef DRPROF_TWIN
      dr_begin();
#endif
      while (pair.frames[0] < (uint32_t)frames)
      {
         int r;
#ifdef DRPROF_TWIN
         dr_frame(pair.frames[0]);
#endif
         /* The input ring holds 256 frames: keep it 64 ahead of each slot
          * (a session feeds it from the network the same way). */
         for (s = 0; s < 2 && !g_use_ap; s++)
            for (; fed[s] < pair.frames[s] + 64; fed[s]++)
               gbalink_pair_input(&pair, s, fed[s], fed[s] < g_sc[s].n ?
                                  g_sc[s].keys[fed[s]] : 0);
         r = gbalink_pair_step(&pair);
         if (r == GBALINK_PAIR_STALL)
         {
            fprintf(stderr, "unexpected stall at %u/%u\n", pair.frames[0],
                    pair.frames[1]);
            return 7;
         }
         if (r == GBALINK_PAIR_ERROR)
         {
            fprintf(stderr, "pair error %d at frame %u/%u\n", pair.error,
                    pair.frames[0], pair.frames[1]);
            return 7;
         }
      }
      {
         gbalink_inst_stats st[2];
         gbalink_bus_stats bs;
         g_ops[0].get_stats(&st[0]);
         g_ops[1].get_stats(&st[1]);
         gbalink_bus_get_stats(&bs);
         for (s = 0; s < 2; s++)
            printf("slot %d (%s): frames=%u runs=%u blocked=%u yh=%u yb=%u "
                   "ev_in=%u late=%u late_max=%u mp=%u n=%u\n", s,
                   g_ops[s].name, pair.frames[s], pair.st.runs[s],
                   pair.st.blocked[s], st[s].yields_horizon,
                   st[s].yields_blocked, st[s].ev_in, st[s].ev_late,
                   st[s].late_max, st[s].mp_xfers, st[s].n_xfers);
         printf("bus: posted=%u/%u depth_max=%u overflow=%u switches=%u\n",
                bs.posted[0], bs.posted[1], bs.depth_max, bs.overflow,
                pair.st.switches);
         /* progress guard (docs/GBA-LINK-MIRROR.md section 13) */
         printf("guard: idle_runs=%u step_runs_max=%u peer_frames_max=%u\n",
                pair.st.idle_runs, pair.st.step_runs_max,
                pair.st.step_peer_frames_max);
         /* the fast path's checking mode and the per-run call guard
          * (docs/GBA-LINK-MIRROR.md section 22) */
         printf("fastchk: checks=%u/%u mismatch=%u/%u run_calls_max=%u/%u\n",
                st[0].fast_checks, st[1].fast_checks, st[0].fast_mismatch,
                st[1].fast_mismatch, st[0].run_calls_max,
                st[1].run_calls_max);
         for (s = 0; s < 2; s++)
            if (st[s].fast_mismatch)
            {
               fast_bad = 1;
               printf("fastchk: FIRST slot=%d where=%u cached=%llu true=%llu "
                      "now=%llu\n", s, st[s].fm_where,
                      (unsigned long long)st[s].fm_cached,
                      (unsigned long long)st[s].fm_true,
                      (unsigned long long)st[s].fm_now);
            }
      }
   }
   for (s = 0; s < 2; s++)
   {
      int m;
      printf("sio s=%d:", s);
      for (m = 0; m < SIOM_N; m++)
         if (g_siom[s][m])
            printf(" %s=%u(from f%u,xfers=%u)", siom_name[m], g_siom[s][m],
                   g_siom_first[s][m], g_sioxf[s][m]);
      printf("\n");
   }
   if (g_use_ap)
      printf("autopilot: slot0 status=%d slot1 status=%d (1 = done, 0 = "
             "running, -1 = failed)\n", fe_autopilot_status(),
             gbab_fe_autopilot_status());
   fe_evt_close();
   printf("rom: shared=%d cap=%ld page_loads A=%u B=%u\n", shared_rom,
          rom_cap, gamepak_page_loads, gbab_gamepak_page_loads);
   printf("done frames=%ld wall=%.2fs\n", frames, now_s() - t0);
   for (s = 0; s < 2; s++)
   {
      /* --save-out0/1: the slot's battery image at exit (a fixture's
       * battery made by a script, multigame/mksave.sh) */
      FILE *f;
      if (!g_save_out[s] || !(f = fopen(g_save_out[s], "wb")))
         continue;
      fwrite(api_inst[g_inst_of_slot[s]].get_memory_data(RETRO_MEMORY_SAVE_RAM),
             1, 0x20000, f);
      fclose(f);
      printf("battery s=%d -> %s\n", s, g_save_out[s]);
   }
   if (g_hash)
      fclose(g_hash);
   return fast_bad ? 8 : 0;
}
