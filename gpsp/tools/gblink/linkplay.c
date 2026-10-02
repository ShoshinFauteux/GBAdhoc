/* linkplay -- two Game Boys cabled together in one process, each driven by
 * an autopilot script (frontend-common/fe_autopilot.c grammar, GB addresses).
 *
 * This is the link session without the network: the machine pair of
 * gbcore/gbcore_dual.h, the in-memory cable, and each player's buttons
 * produced by a script that reads its OWN machine's RAM -- exactly what each
 * PSP does in a real session, where the script's buttons are its player's.
 * `delay=N` applies the session's input delay: buttons a script produces
 * at the start of frame f are pressed in frame f+N.
 *
 * Two autopilot engines: tools/gblink/build.sh links fe_autopilot.c twice,
 * the second copy's entry points renamed fe_autopilot_b_* and its host hooks
 * renamed ap_b_* (the same objcopy approach as the two GB cores).
 *
 *   linkplay rom0=A.gb rom1=B.gb [sav0=..] [sav1=..] [ap0=..] [ap1=..]
 *            [delay=N] [max=FRAMES] [rtc=SECONDS] [link=1] [headless=0|1|2]
 *            [dump=DIR] [dumpevery=N] [out0=..] [out1=..] [console0=gb|gbc]
 *            [console1=..] [palette=N] [after=FRAMES] [hashlog=FILE]
 *            [pokes=FILE] [st0=..] [st1=..] [stout0=..] [stout1=..]
 *            [batch=LINES] [quiet=FRAMES]   (gbdual_config_t.batch_lines)
 *
 * pokes=FILE: fixture building only -- lines "SLOT FRAME ADDR HEXBYTES"
 * write RAM at the start of that slot's frame (gbcore_poke).  st0/st1 load a
 * gbcore save state after power-on; stout0/stout1 write one at the end.
 *
 * Prints EVT lines (the engines' ap_*, plus lp_* of its own) to stdout.
 * Exit status: 0 when both scripts finished (or none was given and `max`
 * elapsed), 1 when a script failed, 2 on a usage or load error.
 */
#include "../../gbcore/gbcore_dual.h"
#include "../../frontend-common/fe_autopilot.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The second engine (renamed copy). */
int fe_autopilot_b_load(const char *path);
int fe_autopilot_b_active(void);
void fe_autopilot_b_frame(void);
int fe_autopilot_b_status(void);
int fe_autopilot_b_dump_pending(void);

#define MAXF (1u << 20)   /* frames per slot a session may run */

static gbdual_t *dual;
static uint32_t injected[2];          /* engine output, RETRO_DEVICE_ID bits */
static uint16_t *queue[2];            /* buttons by frame, after the delay */
static unsigned delay;
static const uint16_t *last_px[2];
static size_t last_pitch[2];
static const char *dump_dir;
static unsigned dump_every;
static int have_script[2];
static uint64_t cur_frame[2];

typedef struct { int slot; uint64_t frame; uint16_t addr; uint8_t n; uint8_t b[64]; } poke_t;
static poke_t pokes[256];
static unsigned npokes;

static int load_pokes(const char *path)
{
  char line[512];
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  while (fgets(line, sizeof(line), f) && npokes < 256)
  {
    poke_t *p = &pokes[npokes];
    char hex[300];
    unsigned slot, addr, i;
    unsigned long long fr;
    if (line[0] == '#' || sscanf(line, "%u %llu %x %299s", &slot, &fr, &addr, hex) != 4)
      continue;
    p->slot = (int)slot; p->frame = fr; p->addr = (uint16_t)addr;
    p->n = (uint8_t)(strlen(hex) / 2 > 64 ? 64 : strlen(hex) / 2);
    for (i = 0; i < p->n; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); p->b[i] = (uint8_t)v; }
    npokes++;
  }
  fclose(f);
  return 0;
}

/* ------------------------------------------------------------- logging -- */

static void vevt(int slot, const char *prefix, const char *fmt, va_list ap)
{
  printf("%s ", prefix);
  vprintf(fmt, ap);
  if (slot >= 0)
    printf(" slot=%d", slot);
  printf("\n");
  fflush(stdout);
}

void fe_evt(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vevt(0, "EVT", fmt, ap);
  va_end(ap);
}

void ap_b_evt(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vevt(1, "EVT", fmt, ap);
  va_end(ap);
}

static void lp_evt(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vevt(-1, "EVT", fmt, ap);
  va_end(ap);
}

void fe_log(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vevt(-1, "LOG", fmt, ap);
  va_end(ap);
}

unsigned long long fe_evt_now_us(void) { return 0; }

/* --------------------------------------------------------- engine hooks -- */

static int mem_read(int slot, uint32_t addr, void *out, unsigned len)
{
  if (addr > 0xFFFF)
    return -1;
  return gbdual_api(slot)->peek(gbdual_core(dual, slot), (uint16_t)addr, out,
                                len);
}

static uint32_t crc32_bytes(const uint8_t *p, size_t n)
{
  uint32_t c = 0xFFFFFFFFu;
  size_t i;
  int b;
  for (i = 0; i < n; i++)
  {
    c ^= p[i];
    for (b = 0; b < 8; b++)
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

/* waitsram: the cartridge RAM's CRC (the game's save landing). */
static uint32_t sram_crc(int slot)
{
  static uint8_t buf[0x20000 + 64];
  const gbcore_api_t *api = gbdual_api(slot);
  gbcore_t *c = gbdual_core(dual, slot);
  size_t n = api->cart_ram_size(c);
  if (!n || n > sizeof(buf) || api->save_ram_read(c, buf, sizeof(buf)) != 0)
    return 0;
  return crc32_bytes(buf, n);
}

int fe_host_mem_read(uint32_t a, void *o, unsigned n) { return mem_read(0, a, o, n); }
int ap_b_host_mem_read(uint32_t a, void *o, unsigned n) { return mem_read(1, a, o, n); }
void fe_host_input_inject(uint32_t m) { injected[0] = m; }
void ap_b_host_input_inject(uint32_t m) { injected[1] = m; }
uint32_t fe_host_sram_crc_now(void) { return sram_crc(0); }
uint32_t ap_b_host_sram_crc_now(void) { return sram_crc(1); }

static uint16_t gb_buttons(uint32_t m)
{
  uint16_t o = 0;
  if (m & (1u << 0)) o |= GBCORE_BUTTON_B;
  if (m & (1u << 2)) o |= GBCORE_BUTTON_SELECT;
  if (m & (1u << 3)) o |= GBCORE_BUTTON_START;
  if (m & (1u << 4)) o |= GBCORE_BUTTON_UP;
  if (m & (1u << 5)) o |= GBCORE_BUTTON_DOWN;
  if (m & (1u << 6)) o |= GBCORE_BUTTON_LEFT;
  if (m & (1u << 7)) o |= GBCORE_BUTTON_RIGHT;
  if (m & (1u << 8)) o |= GBCORE_BUTTON_A;
  return o;
}

/* ----------------------------------------------------------------- PNG -- */

static uint32_t png_crc(uint32_t c, const uint8_t *p, size_t n)
{
  size_t i;
  int b;
  c = ~c;
  for (i = 0; i < n; i++)
  {
    c ^= p[i];
    for (b = 0; b < 8; b++)
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
  uint8_t h[8];
  uint32_t c;
  put32(h, n);
  memcpy(h + 4, type, 4);
  fwrite(h, 1, 8, f);
  if (n) fwrite(data, 1, n, f);
  c = png_crc(0, h + 4, 4);
  c = png_crc(c, data, n);
  put32(h, c);
  fwrite(h, 1, 4, f);
}

/* 160x144 RGB, zlib "stored" blocks (one per row: 481 bytes). */
static void write_png(const char *path, const uint16_t *px, size_t pitch)
{
  static uint8_t z[144 * (5 + 481) + 64];
  uint8_t ihdr[13], row[481];
  uint32_t a = 1, b = 0, zn = 0;
  unsigned x, y;
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
  put32(ihdr, 160); put32(ihdr + 4, 144);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
  chunk(f, "IHDR", ihdr, 13);
  z[zn++] = 0x78; z[zn++] = 0x01;
  for (y = 0; y < 144; y++)
  {
    const uint16_t *r = (const uint16_t *)((const uint8_t *)px + y * pitch);
    unsigned i;
    row[0] = 0;
    for (x = 0; x < 160; x++)
    {
      uint16_t p = r[x];
      row[1 + x * 3] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
      row[2 + x * 3] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
      row[3 + x * 3] = (uint8_t)((p & 31) * 255 / 31);
    }
    z[zn++] = (uint8_t)(y == 143);
    z[zn++] = 481 & 0xFF; z[zn++] = 481 >> 8;
    z[zn++] = (uint8_t)~(481 & 0xFF); z[zn++] = (uint8_t)~(481 >> 8);
    memcpy(z + zn, row, 481);
    zn += 481;
    for (i = 0; i < 481; i++) { a = (a + row[i]) % 65521; b = (b + a) % 65521; }
  }
  put32(z + zn, (b << 16) | a);
  zn += 4;
  chunk(f, "IDAT", z, zn);
  chunk(f, "IEND", NULL, 0);
  fclose(f);
}

static void dump(int slot, uint64_t frame, const char *why)
{
  char path[1024];
  if (!dump_dir || !last_px[slot]) return;
  snprintf(path, sizeof(path), "%s/s%d_%06llu%s.png", dump_dir, slot,
           (unsigned long long)frame, why);
  write_png(path, last_px[slot], last_pitch[slot]);
}

static FILE *serial_log;
static void on_serial(void *ud, int slot, uint8_t sent, uint8_t got,
                      uint8_t peer_sc, uint64_t line)
{
  (void)ud;
  fprintf(serial_log, "%llu s%d out=%02x in=%02x peer_sc=%02x f=%llu/%llu\n",
          (unsigned long long)line, slot, sent, got, peer_sc,
          (unsigned long long)cur_frame[0], (unsigned long long)cur_frame[1]);
}

static uint64_t sc_from = ~0ull, sc_to;
static void on_line(void *ud, int slot, uint64_t line)
{
  (void)ud;
  if (cur_frame[0] >= sc_from && cur_frame[0] < sc_to)
    fprintf(serial_log, "L%llu s%d sc=%02x f=%llu\n", (unsigned long long)line,
            slot, gbdual_api(slot)->serial_control(gbdual_core(dual, slot)),
            (unsigned long long)cur_frame[slot]);
}

static void on_video(void *ud, const gbcore_video_frame_t *fr)
{
  int slot = (int)(intptr_t)ud;
  last_px[slot] = fr->pixels;
  last_pitch[slot] = fr->pitch_bytes;
}

/* --------------------------------------------------------------- input -- */

static int input(void *ud, int slot, uint64_t frame, uint16_t *buttons)
{
  (void)ud;
  cur_frame[slot] = frame;
  if (frame >= MAXF - delay)
    return 0;
  {
    unsigned i;
    for (i = 0; i < npokes; i++)
      if (pokes[i].slot == slot && pokes[i].frame == frame &&
          gbdual_api(slot)->poke(gbdual_core(dual, slot), pokes[i].addr,
                                 pokes[i].b, pokes[i].n) != 0)
        fe_log("poke refused slot=%d addr=%04x", slot, pokes[i].addr);
  }
  if (have_script[slot])
  {
    if (slot == 0)
    {
      fe_autopilot_frame();
      if (fe_autopilot_dump_pending()) dump(0, frame, "");
    }
    else
    {
      fe_autopilot_b_frame();
      if (fe_autopilot_b_dump_pending()) dump(1, frame, "");
    }
    queue[slot][frame + delay] = gb_buttons(injected[slot]);
  }
  if (dump_every && frame % dump_every == 0)
    dump(slot, frame, "_e");
  *buttons = queue[slot][frame];
  return 1;
}

/* ---------------------------------------------------------------- main -- */

static uint8_t *load_file(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  uint8_t *p;
  long n;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  p = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
  if (p && n > 0 && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
  fclose(f);
  *size = n > 0 ? (size_t)n : 0;
  return p;
}

static const char *arg(int argc, char **argv, const char *key, const char *def)
{
  size_t k = strlen(key);
  int i;
  for (i = 1; i < argc; i++)
    if (!strncmp(argv[i], key, k) && argv[i][k] == '=')
      return argv[i] + k + 1;
  return def;
}

static fe_console_t console_of(const char *rom, const char *forced)
{
  const char *dot = strrchr(rom, '.');
  if (forced) return strcmp(forced, "gbc") ? FE_CONSOLE_GB : FE_CONSOLE_GBC;
  return dot && (dot[3] == 'c' || dot[3] == 'C') ? FE_CONSOLE_GBC : FE_CONSOLE_GB;
}

static int save_out(int slot, const char *path)
{
  static uint8_t buf[0x20000 + 64];
  const gbcore_api_t *api = gbdual_api(slot);
  gbcore_t *c = gbdual_core(dual, slot);
  size_t n = api->save_ram_size(c);
  FILE *f;
  if (!path) return 0;
  if (!n || api->save_ram_read(c, buf, sizeof(buf)) != 0) return -1;
  f = fopen(path, "wb");
  if (!f || fwrite(buf, 1, n, f) != n) { if (f) fclose(f); return -1; }
  fclose(f);
  lp_evt("lp_save slot=%d bytes=%u crc=%08x cart_crc=%08x file=%s", slot,
         (unsigned)n, crc32_bytes(buf, n),
         crc32_bytes(buf, api->cart_ram_size(c)), path);
  return 0;
}

int main(int argc, char **argv)
{
  gbdual_config_t cfg;
  const char *rom[2], *sav[2], *ap[2], *out[2], *hashlog_path;
  uint8_t *romd[2], *savd[2] = { NULL, NULL };
  size_t roms[2], savs[2] = { 0, 0 };
  unsigned max, after, s;
  int headless, rc = 0, status[2] = { 1, 1 };
  FILE *hashlog = NULL;
  uint64_t f;

  rom[0] = arg(argc, argv, "rom0", NULL);
  rom[1] = arg(argc, argv, "rom1", NULL);
  sav[0] = arg(argc, argv, "sav0", NULL);
  sav[1] = arg(argc, argv, "sav1", NULL);
  ap[0] = arg(argc, argv, "ap0", NULL);
  ap[1] = arg(argc, argv, "ap1", NULL);
  out[0] = arg(argc, argv, "out0", NULL);
  out[1] = arg(argc, argv, "out1", NULL);
  hashlog_path = arg(argc, argv, "hashlog", NULL);
  delay = (unsigned)atoi(arg(argc, argv, "delay", "0"));
  max = (unsigned)atoi(arg(argc, argv, "max", "36000"));
  after = (unsigned)atoi(arg(argc, argv, "after", "0"));
  headless = atoi(arg(argc, argv, "headless", "0"));
  dump_dir = arg(argc, argv, "dump", NULL);
  dump_every = (unsigned)atoi(arg(argc, argv, "dumpevery", "0"));
  if (!rom[0] || !rom[1] || max >= MAXF - 64 || delay > 64)
  {
    fprintf(stderr, "usage: see tools/gblink/linkplay.c\n");
    return 2;
  }

  memset(&cfg, 0, sizeof(cfg));
  for (s = 0; s < 2; s++)
  {
    gbdual_machine_t *m = &cfg.machine[s];
    char key[16];
    romd[s] = load_file(rom[s], &roms[s]);
    if (!romd[s]) { fprintf(stderr, "cannot read %s\n", rom[s]); return 2; }
    if (sav[s])
    {
      savd[s] = load_file(sav[s], &savs[s]);
      if (!savd[s]) { fprintf(stderr, "cannot read %s\n", sav[s]); return 2; }
    }
    snprintf(key, sizeof(key), "console%u", s);
    m->rom_data = romd[s];
    m->rom_size = roms[s];
    m->console = console_of(rom[s], arg(argc, argv, key, NULL));
    m->palette = (unsigned)atoi(arg(argc, argv, "palette", "0"));
    m->headless = headless == (int)s + 1;
    m->save = savd[s];
    m->save_size = savs[s];
    m->callbacks.userdata = (void *)(intptr_t)s;
    m->callbacks.video = on_video;
    queue[s] = (uint16_t *)calloc(MAXF, sizeof(uint16_t));
  }
  cfg.audio_rate = 32768;
  cfg.link = atoi(arg(argc, argv, "link", "1"));
  cfg.pace_slot = 0;
  cfg.batch_lines = (unsigned)atoi(arg(argc, argv, "batch", "0"));
  cfg.quiet_frames = (unsigned)atoi(arg(argc, argv, "quiet", "0"));
  cfg.rtc_seed = strtoll(arg(argc, argv, "rtc", "1700000000"), NULL, 0);
  if (arg(argc, argv, "seriallog", NULL))
  {
    serial_log = fopen(arg(argc, argv, "seriallog", NULL), "w");
    cfg.serial_trace = serial_log ? on_serial : NULL;
    if (arg(argc, argv, "sctrace", NULL))
    {
      sc_from = strtoull(arg(argc, argv, "sctrace", NULL), NULL, 0);
      sc_to = sc_from + 2;
      cfg.line_trace = serial_log ? on_line : NULL;
    }
  }
  dual = gbdual_create(&cfg);
  if (!dual) { fprintf(stderr, "gbdual_create failed\n"); return 2; }
  if (arg(argc, argv, "pokes", NULL) && load_pokes(arg(argc, argv, "pokes", NULL)))
    return 2;
  for (s = 0; s < 2; s++)
  {
    char key[8];
    const char *st;
    snprintf(key, sizeof(key), "st%u", s);
    st = arg(argc, argv, key, NULL);
    if (st)
    {
      size_t n;
      uint8_t *d = load_file(st, &n);
      if (!d || gbdual_api(s)->state_load(gbdual_core(dual, s), d, n) != 0)
      { fprintf(stderr, "state %s refused\n", st); return 2; }
      free(d);
    }
  }
  if (ap[0]) { if (fe_autopilot_load(ap[0]) != 0) return 2; have_script[0] = 1; }
  if (ap[1]) { if (fe_autopilot_b_load(ap[1]) != 0) return 2; have_script[1] = 1; }
  if (hashlog_path) hashlog = fopen(hashlog_path, "w");
  lp_evt("lp_start rom0=%s rom1=%s delay=%u link=%d rtc=%lld", rom[0], rom[1],
         delay, cfg.link, (long long)cfg.rtc_seed);

  for (f = 0; f < max; f++)
  {
    if (gbdual_advance(dual, input, NULL) != 1) { rc = 2; break; }
    /* by slot 0's completed frames -- the numbering a session's HASH
     * messages and sesssim's hashlog use */
    if (hashlog && gbdual_frame(dual, 0) % 60 == 0)
      fprintf(hashlog, "%llu %016llx\n",
              (unsigned long long)gbdual_frame(dual, 0),
              (unsigned long long)gbdual_sync_hash(dual));
    status[0] = have_script[0] ? fe_autopilot_status() : 1;
    status[1] = have_script[1] ? fe_autopilot_b_status() : 1;
    if ((have_script[0] || have_script[1]) && status[0] != 0 && status[1] != 0)
      break;
  }
  /* Let the games finish what they were doing (a save in progress). */
  for (s = 0; s < after && rc == 0; s++)
    if (gbdual_advance(dual, input, NULL) != 1) rc = 2;
  dump(0, gbdual_frame(dual, 0), "_end");
  dump(1, gbdual_frame(dual, 1), "_end");
  lp_evt("lp_end frames=%llu/%llu status=%d/%d serial=%llu/%llu sync=%016llx",
         (unsigned long long)gbdual_frame(dual, 0),
         (unsigned long long)gbdual_frame(dual, 1), status[0], status[1],
         (unsigned long long)gbdual_serial_bytes(dual, 0),
         (unsigned long long)gbdual_serial_bytes(dual, 1),
         (unsigned long long)gbdual_sync_hash(dual));
  lp_evt("lp_stepping batch=%s quiet=%s batched_lines=%llu of %llu "
         "cuts=%llu serial_starts=%llu/%llu", arg(argc, argv, "batch", "1"),
         arg(argc, argv, "quiet", "60"),
         (unsigned long long)gbdual_batched_lines(dual),
         (unsigned long long)(gbdual_lines(dual) * 2),
         (unsigned long long)gbdual_batch_cuts(dual),
         (unsigned long long)gbdual_serial_starts(dual, 0),
         (unsigned long long)gbdual_serial_starts(dual, 1));
  if (save_out(0, out[0]) || save_out(1, out[1])) rc = 2;
  for (s = 0; s < 2; s++)
  {
    char key[8];
    const char *st;
    snprintf(key, sizeof(key), "stout%u", s);
    st = arg(argc, argv, key, NULL);
    if (st)
    {
      static uint8_t buf[1 << 20];
      long n = gbdual_api(s)->state_save(gbdual_core(dual, s), buf, sizeof(buf));
      FILE *f = fopen(st, "wb");
      if (n <= 0 || !f || fwrite(buf, 1, (size_t)n, f) != (size_t)n) rc = 2;
      if (f) fclose(f);
    }
  }
  if (hashlog) fclose(hashlog);
  gbdual_destroy(dual);
  if (rc) return rc;
  if (status[0] < 0 || status[1] < 0) return 1;
  if ((have_script[0] && status[0] != 1) || (have_script[1] && status[1] != 1))
    return 1;
  return 0;
}
