/* lb_host.c -- GBA link-cable feasibility bench, HOST side.
 *
 * Measurement tool only (docs/GBA-LINK-FEASIBILITY.md).  Links the unix
 * libretro core (x86 dynarec) exactly like the SDL twin, but headless and
 * unpaced, and writes two per-frame files:
 *
 *   --hash FILE   the guest-state oracle, same fields as the PSP harness
 *                 `shash` (r0-r15/CPSR, IWRAM/EWRAM data, I/O, palette, OAM,
 *                 VRAM, rolling audio hash).  Two runs fed the same state and
 *                 inputs must print identical lines after the last state load.
 *   --bw FILE     thin-client bandwidth: what a host would have to send a
 *                 client each frame so the client's renderer (the ME renderer
 *                 input set: VRAM, OAM, palette, the per-line LCD register
 *                 capture, affine seed) is up to date, before and after cheap
 *                 compression, at frame-skip k = 1,2,3,4,6.
 *
 * Inputs come from an autopilot script (fe_autopilot grammar; only frame-
 * stamped press/hold/wait are used, so the input sequence is independent of
 * guest state).  A script `state` step reloads the savestate, which is how a
 * WARM arm (translation history first) and a COLD arm (reload immediately)
 * are made to run the same measured segment.
 *
 * Not part of any PSP build.  The optional -DLINKBENCH counters in sound.c are
 * compiled only by tools/linkbench/Makefile.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "fe_host.h"
#include "fe_evt.h"
#include "fe_util.h"
#include "fe_autopilot.h"

/* ---- core globals (C linkage; see gba_memory.h, video.h, cpu.h) -------- */
extern unsigned char iwram[], ewram[], vram[];
extern unsigned short palette_ram[], oam_ram[], io_registers[];
extern unsigned int reg[64];
extern unsigned char vram_clean[96];
extern unsigned int me_capture_mode;
typedef struct
{
   unsigned short ioregs[160][64];
   int affine_seed[4];
   unsigned int oam_updated;
} lb_capture_frame;                 /* == me_capture_frame (video.h) */
extern lb_capture_frame *me_capture_buf;
#ifdef LINKBENCH
extern unsigned int lb_fifo_bytes[2];   /* sound.c, LINKBENCH only */
#endif

/* ---- helpers -------------------------------------------------------------- */
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
static unsigned g_frame_samples;
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
   g_frame_samples += (unsigned)n;
}
static uint32_t no_input(void) { return 0; }

/* ---- LZ4 block format, greedy, 64 KiB window --------------------------- *
 * A real encoder (the matching decoder below round-trips every frame in
 * --selftest), so the byte counts are what an LZ4 stream would carry.      */
#define LZ4_HBITS 14
static uint32_t lz4_htab[1u << LZ4_HBITS];
static size_t lz4_put_len(uint8_t *o, size_t len)
{
   size_t n = 0;
   while (len >= 255) { o[n++] = 255; len -= 255; }
   o[n++] = (uint8_t)len;
   return n;
}
static size_t lz4_compress(const uint8_t *s, size_t n, uint8_t *o)
{
   size_t ip = 0, anchor = 0, op = 0;
   memset(lz4_htab, 0xff, sizeof(lz4_htab));
   if (n >= 13)
   {
      size_t limit = n - 12;
      while (ip < limit)
      {
         uint32_t seq, h, ref;
         memcpy(&seq, s + ip, 4);
         h = (seq * 2654435761u) >> (32 - LZ4_HBITS);
         ref = lz4_htab[h];
         lz4_htab[h] = (uint32_t)ip;
         if (ref != 0xffffffffu && ip - ref <= 65535 &&
             !memcmp(s + ref, s + ip, 4))
         {
            size_t ml = 4, lit = ip - anchor;
            uint8_t *tok = o + op++;
            while (ip + ml < n - 5 && s[ref + ml] == s[ip + ml])
               ml++;
            *tok = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
            if (lit >= 15) op += lz4_put_len(o + op, lit - 15);
            memcpy(o + op, s + anchor, lit); op += lit;
            o[op++] = (uint8_t)((ip - ref) & 0xff);
            o[op++] = (uint8_t)((ip - ref) >> 8);
            *tok |= (uint8_t)(ml - 4 >= 15 ? 15 : ml - 4);
            if (ml - 4 >= 15) op += lz4_put_len(o + op, ml - 4 - 15);
            ip += ml;
            anchor = ip;
            continue;
         }
         ip++;
      }
   }
   {  /* last literals */
      size_t lit = n - anchor;
      o[op++] = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
      if (lit >= 15) op += lz4_put_len(o + op, lit - 15);
      memcpy(o + op, s + anchor, lit); op += lit;
   }
   return op;
}
static size_t lz4_decompress(const uint8_t *i, size_t n, uint8_t *o)
{
   size_t ip = 0, op = 0;
   while (ip < n)
   {
      uint8_t t = i[ip++];
      size_t lit = t >> 4, ml;
      if (lit == 15) { uint8_t b; do { b = i[ip++]; lit += b; } while (b == 255); }
      memcpy(o + op, i + ip, lit); ip += lit; op += lit;
      if (ip >= n) break;
      {
         size_t off = i[ip] | (i[ip + 1] << 8), k;
         ip += 2;
         ml = t & 15;
         if (ml == 15) { uint8_t b; do { b = i[ip++]; ml += b; } while (b == 255); }
         ml += 4;
         for (k = 0; k < ml; k++, op++) o[op] = o[op - off];
      }
   }
   return op;
}

/* Zero-run patch format: the cheapest thing a client can apply in place.
 * Records of {u32 offset:20 | len:12} + len XOR bytes; runs separated by
 * fewer than 4 unchanged bytes are merged (a header costs 4).  Written to
 * `o` (may be NULL to size only). */
static size_t rle_encode(const uint8_t *x, size_t n, uint8_t *o)
{
   size_t i = 0, out = 0;
   while (i < n)
   {
      size_t j, gap;
      if (!x[i]) { i++; continue; }
      j = i;
      for (;;)
      {
         while (j < n && x[j]) j++;
         gap = 0;
         while (j + gap < n && !x[j + gap] && gap < 4) gap++;
         if (gap < 4 && j + gap < n) { j += gap; continue; }
         break;
      }
      while (i < j)
      {
         size_t len = j - i > 4095 ? 4095 : j - i;
         if (o)
         {
            uint32_t h = (uint32_t)(i << 12) | (uint32_t)len;
            memcpy(o + out, &h, 4);
            memcpy(o + out + 4, x + i, len);
         }
         out += 4 + len;
         i += len;
      }
   }
   return out;
}

/* ---- thin-client payload accounting ------------------------------------ */
#define VRAM_SZ   (96 * 1024)
#define CAP_SZ    (160 * 64 * 2)
#define PAY_SZ    (VRAM_SZ + 1024 + 1024 + CAP_SZ + 20)
#define NSKIP     5
static const unsigned k_skip[NSKIP] = { 1, 2, 3, 4, 6 };

typedef struct
{
   uint8_t shadow[PAY_SZ];   /* what the client holds */
   int valid;
} lb_shadow;

static lb_shadow g_sh[NSKIP];
/* Adaptive link model: a link of B KB/s (1 KB = 1000 B) minus that frame's
 * ADPCM audio (FIFO bytes / 2).  The host sends a new frame only when the
 * previous one has drained; the delta then covers every change since the
 * last SENT frame (a partial-update / frame-skip scheme).  16 B per packet
 * of framing is charged. */
#define NBUD 3
static const unsigned k_bud[NBUD] = { 100, 200, 300 };
static lb_shadow g_bsh[NBUD];
static double g_backlog[NBUD];
static lb_capture_frame g_cap;
static uint8_t g_cur[PAY_SZ], g_x[PAY_SZ], g_pages[PAY_SZ];
static uint8_t g_z[2 * PAY_SZ + 1024], g_back[2 * PAY_SZ], g_rle[2 * PAY_SZ];
static int g_selftest;

static void build_payload(uint8_t *p)
{
   memcpy(p, vram, VRAM_SZ);
   memcpy(p + VRAM_SZ, oam_ram, 1024);
   memcpy(p + VRAM_SZ + 1024, palette_ram, 1024);
   memcpy(p + VRAM_SZ + 2048, g_cap.ioregs, CAP_SZ);
   memcpy(p + VRAM_SZ + 2048 + CAP_SZ, g_cap.affine_seed, 16);
   memcpy(p + VRAM_SZ + 2048 + CAP_SZ + 16, &g_cap.oam_updated, 4);
}

static size_t deflate_size(const uint8_t *s, size_t n, int level)
{
   uLongf zl = sizeof(g_z);
   if (compress2(g_z, &zl, s, n, level) != Z_OK)
      return (size_t)-1;
   return zl;
}

typedef struct
{
   unsigned vchg, oamchg, palchg, caplines;
   size_t raw_pages, xor_nz, rle, lz4x, lz4p, defl1;
} lb_cost;

/* Cost of bringing shadow `sh` up to g_cur, then updating it. */
static void cost_delta(lb_shadow *sh, lb_cost *c)
{
   size_t i, np = 0;
   unsigned p, l;
   memset(c, 0, sizeof(*c));
   if (!sh->valid)
      memset(sh->shadow, 0, PAY_SZ);
   for (i = 0; i < PAY_SZ; i++)
   {
      g_x[i] = g_cur[i] ^ sh->shadow[i];
      if (g_x[i]) c->xor_nz++;
   }
   /* ME granularity: whole 1 KiB VRAM pages, whole OAM/palette, per-line
    * capture rows (128 B) + the 20-byte seed. */
   for (p = 0; p < 96; p++)
      if (memcmp(g_cur + p * 1024, sh->shadow + p * 1024, 1024))
      {
         c->vchg++;
         memcpy(g_pages + np, g_cur + p * 1024, 1024); np += 1024;
      }
   if (memcmp(g_cur + VRAM_SZ, sh->shadow + VRAM_SZ, 1024))
   { c->oamchg = 1; memcpy(g_pages + np, g_cur + VRAM_SZ, 1024); np += 1024; }
   if (memcmp(g_cur + VRAM_SZ + 1024, sh->shadow + VRAM_SZ + 1024, 1024))
   { c->palchg = 1; memcpy(g_pages + np, g_cur + VRAM_SZ + 1024, 1024); np += 1024; }
   for (l = 0; l < 160; l++)
   {
      size_t off = VRAM_SZ + 2048 + l * 128;
      if (memcmp(g_cur + off, sh->shadow + off, 128))
      { c->caplines++; memcpy(g_pages + np, g_cur + off, 128); np += 128; }
   }
   memcpy(g_pages + np, g_cur + VRAM_SZ + 2048 + CAP_SZ, 20); np += 20;
   c->raw_pages = np;
   c->rle  = rle_encode(g_x, PAY_SZ, g_rle);
   /* zero-run patch, then LZ4 over the patch stream */
   c->lz4x = lz4_compress(g_rle, c->rle, g_z);
   if (g_selftest)
   {
      size_t back = lz4_decompress(g_z, c->lz4x, g_back);
      if (back != c->rle || memcmp(g_back, g_rle, c->rle))
      { fprintf(stderr, "lz4 selftest FAILED\n"); exit(9); }
   }
   c->lz4p = lz4_compress(g_pages, np, g_z);
   c->defl1 = deflate_size(g_x, PAY_SZ, 1);
   memcpy(sh->shadow, g_cur, PAY_SZ);
   sh->valid = 1;
}

/* ---- main ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
   const char *rom = NULL, *bios = ".", *save = NULL, *state = NULL;
   const char *script = NULL, *hash_path = NULL, *bw_path = NULL;
   const char *log_path = "lb_host.log", *dump_dir = NULL, *dumplist = NULL;
   long load_at = 30, max_frames = 100000, dump_every = 0, save_at = 0;
   const char *save_to = NULL;
   FILE *hf = NULL, *bf = NULL;
   fe_host_config cfg;
   unsigned seg = 0, f;
   int i;

   for (i = 1; i < argc; i++)
   {
#define ARG(n) (!strcmp(argv[i], n) && i + 1 < argc)
      if (ARG("--rom")) rom = argv[++i];
      else if (ARG("--bios-dir")) bios = argv[++i];
      else if (ARG("--save")) save = argv[++i];
      else if (ARG("--state")) state = argv[++i];
      else if (ARG("--script")) script = argv[++i];
      else if (ARG("--hash")) hash_path = argv[++i];
      else if (ARG("--bw")) bw_path = argv[++i];
      else if (ARG("--log")) log_path = argv[++i];
      else if (ARG("--load-at")) load_at = strtol(argv[++i], NULL, 0);
      else if (ARG("--frames")) max_frames = strtol(argv[++i], NULL, 0);
      else if (ARG("--dump-dir")) dump_dir = argv[++i];
      else if (ARG("--dump-every")) dump_every = strtol(argv[++i], NULL, 0);
      else if (ARG("--dump-at")) dumplist = argv[++i];
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
      else if (ARG("--save-at")) save_at = strtol(argv[++i], NULL, 0);
      else if (ARG("--save-to")) save_to = argv[++i];
      else if (!strcmp(argv[i], "--selftest")) g_selftest = 1;
      else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
#undef ARG
   }
   if (!rom) { fprintf(stderr, "--rom required\n"); return 2; }

   fe_evt_init(log_path, 0);
   memset(&cfg, 0, sizeof(cfg));
   cfg.rom_path = rom;
   cfg.system_dir = bios;
   cfg.save_path = save;
   cfg.video_frame = v_frame;
   cfg.audio_frames = a_frames;
   cfg.input_bitmask = no_input;
   if (fe_host_boot(&cfg) != 0) { fprintf(stderr, "boot failed\n"); return 3; }
   /* capture AND render: the renderer input set is recorded, the frame is
    * still drawn (so dumps work).  Does not touch emulation state. */
   me_capture_buf = (void *)&g_cap;
   me_capture_mode = 2;

   if (script && fe_autopilot_load(script) != 0) return 4;
   if (hash_path) { hf = fopen(hash_path, "w"); if (hf) fprintf(hf, "# lbhash v1 rom=%s\n", rom); }
   if (bw_path)
   {
      int k;
      bf = fopen(bw_path, "w");
      if (bf)
      {
         fprintf(bf, "f,seg,vmark,samples,fifoA,fifoB,psgchg");
         for (k = 0; k < NSKIP; k++)
            fprintf(bf, ",k%u_vchg,k%u_oam,k%u_pal,k%u_cap,k%u_raw,k%u_nz,k%u_rle,"
                    "k%u_rlz4,k%u_lz4p,k%u_defl1",
                    k_skip[k], k_skip[k], k_skip[k], k_skip[k], k_skip[k],
                    k_skip[k], k_skip[k], k_skip[k], k_skip[k], k_skip[k]);
         fprintf(bf, ",key_lz4,key_defl1");
         for (k = 0; k < NBUD; k++)
            fprintf(bf, ",b%u_sent,b%u_bytes,b%u_backlog", k_bud[k], k_bud[k],
                    k_bud[k]);
         fprintf(bf, "\n");
      }
   }

   {
      uint8_t psg_prev[0x50];
      memset(psg_prev, 0, sizeof(psg_prev));
      for (f = 0; f < (unsigned)max_frames; f++)
      {
         unsigned vmark = 0, p, psgchg = 0, k;
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
            /* per-segment audio oracle: restart the rolling hash at a load */
            g_ahash = 2166136261u;
            g_asamples = 0;
            for (k = 0; k < NSKIP; k++) g_sh[k].valid = 0;
            for (k = 0; k < NBUD; k++) { g_bsh[k].valid = 0; g_backlog[k] = 0; }
         }
         memset(vram_clean, 1, 96);
#ifdef LINKBENCH
         lb_fifo_bytes[0] = lb_fifo_bytes[1] = 0;
#endif
         g_frame_samples = 0;
         fe_autopilot_frame();
         fe_host_run_frame();

         if (hf)
            fprintf(hf, "f=%u s=%u a=%08x n=%lu r=%08x pc=%08x i=%08x e=%08x "
                    "io=%08x p=%08x o=%08x v=%08x\n",
                    fe_host_frame_count(), seg, g_ahash, g_asamples,
                    fnv32w(reg, 17 * 4), reg[15],
                    fnv32w(iwram + 0x8000, 0x8000), fnv32w(ewram, 0x40000),
                    fnv32w(io_registers, 0x400), fnv32w(palette_ram, 0x400),
                    fnv32w(oam_ram, 0x400), fnv32w(vram, VRAM_SZ));
         if (bf)
         {
            const uint8_t *io = (const uint8_t *)io_registers;
            for (p = 0; p < 96; p++) if (!vram_clean[p]) vmark++;
            /* PSG + sound control 0x60..0x8F and wave RAM 0x90..0x9F */
            for (p = 0; p < 0x40; p++)
               if (io[0x60 + p] != psg_prev[p]) psgchg++;
            memcpy(psg_prev, io + 0x60, 0x40);
            build_payload(g_cur);
            fprintf(bf, "%u,%u,%u,%u,", fe_host_frame_count(), seg, vmark,
                    g_frame_samples);
#ifdef LINKBENCH
            fprintf(bf, "%u,%u,", lb_fifo_bytes[0], lb_fifo_bytes[1]);
#else
            fprintf(bf, "-1,-1,");
#endif
            fprintf(bf, "%u", psgchg);
            for (k = 0; k < NSKIP; k++)
            {
               lb_cost c;
               if (f % k_skip[k] == 0 || reloaded)
               {
                  cost_delta(&g_sh[k], &c);
                  fprintf(bf, ",%u,%u,%u,%u,%zu,%zu,%zu,%zu,%zu,%zu",
                          c.vchg, c.oamchg, c.palchg, c.caplines, c.raw_pages,
                          c.xor_nz, c.rle, c.lz4x, c.lz4p, c.defl1);
               }
               else
                  fprintf(bf, ",,,,,,,,,,");
            }
            if (f % 600 == 0 || reloaded)
               fprintf(bf, ",%zu,%zu", lz4_compress(g_cur, PAY_SZ, g_z),
                       deflate_size(g_cur, PAY_SZ, 1));
            else
               fprintf(bf, ",,");
            for (k = 0; k < NBUD; k++)
            {
#ifdef LINKBENCH
               double audio = (lb_fifo_bytes[0] + lb_fifo_bytes[1]) / 2.0;
#else
               double audio = 0;
#endif
               double drain = k_bud[k] * 1000.0 / 60.0 - audio;
               unsigned sent = 0;
               size_t bytes = 0;
               g_backlog[k] -= drain;
               if (g_backlog[k] < 0)
                  g_backlog[k] = 0;
               if (g_backlog[k] <= 0)
               {
                  lb_cost c;
                  cost_delta(&g_bsh[k], &c);
                  bytes = c.lz4x + 16;
                  g_backlog[k] += bytes;
                  sent = 1;
               }
               fprintf(bf, ",%u,%zu,%.0f", sent, bytes, g_backlog[k]);
            }
            fprintf(bf, "\n");
         }
         if (dump_dir && g_pix)
         {
            int want = dump_every && (fe_host_frame_count() % dump_every) == 0;
            if (dumplist)
            {
               char key[32];
               snprintf(key, sizeof(key), ",%u,", fe_host_frame_count());
               {
                  char buf[4096];
                  snprintf(buf, sizeof(buf), ",%s,", dumplist);
                  if (strstr(buf, key)) want = 1;
               }
            }
            if (fe_autopilot_dump_pending()) want = 1;
            if (want)
            {
               char path[512];
               snprintf(path, sizeof(path), "%s/f%06u.bmp", dump_dir,
                        fe_host_frame_count());
               fe_bmp_write_rgb565(path, g_pix, 240, 160, g_pitch);
            }
         }
         if (save_to && save_at && fe_host_frame_count() == (unsigned)save_at)
            fe_host_state_save(save_to);
         if (script && fe_autopilot_status() != 0)
            break;
      }
   }
   if (hf) fclose(hf);
   if (bf) fclose(bf);
   fprintf(stderr, "done frames=%u seg=%u ahash=%08x samples=%lu ap=%d\n",
           fe_host_frame_count(), seg, g_ahash, g_asamples,
           script ? fe_autopilot_status() : 0);
   fe_host_shutdown();
   fe_evt_close();
   return 0;
}
