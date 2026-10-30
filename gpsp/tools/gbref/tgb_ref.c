/* tgb_ref.c -- headless HOST runner of GBAdhoc's GB/GBC core (TGB Dual),
 * the twin of GBAdhoc-Lab compat/ref/mgba_ref.c so the two can be compared
 * frame for frame on the same input script.
 *
 *   tgb_ref --rom FILE [--console gb|gbc] [--frames N] [--script FILE]
 *           [--dump-at f1,f2,..] [--dump-every N] [--dump-dir DIR]
 *           [--hash FILE] [--cc 0|1] [--palette N]
 *           [--peek ADDR:LEN[,..]] [--peek-every N]
 *           [--state-in FILE] [--state-out FILE --state-out-at F]
 *           [--time] [--statehash FILE]
 *
 * --statehash FILE: per frame "f=N s=HASH" -- FNV-1a 64 over the machine's
 * whole save state after the frame, for exactness checks of core changes
 * that must not move any state (docs/GB-CORE-PERF.md).
 *
 * Same script subset and timing as mgba_ref (wait / press BTNS [N] /
 * hold BTNS N / repeat N .. endrepeat / evt / dump; press = hold N (default
 * 2) then release 2), frames counted from 1, keys for frame f applied
 * before frame f runs, BMP names frame_NNNNNN.bmp.  --hash writes the same
 * RGB555 FNV-1a as mgba_ref.  --cc 0 (the default here) turns the CGB LCD
 * colour model off so pictures compare with mGBA's plain expansion.
 * Host-only development tool: never linked into any PSP build. */
#include "../../gbcore/gbcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

enum { OP_WAIT, OP_HOLD, OP_DUMP, OP_EVT };
struct step { int op; unsigned keys; int n; char text[64]; };
static struct step *steps; static int nsteps, capsteps;

static void push(struct step s)
{
  if (nsteps == capsteps) { capsteps = capsteps ? capsteps * 2 : 256; steps = realloc(steps, capsteps * sizeof(*steps)); }
  steps[nsteps++] = s;
}

static int keybit(const char *k)
{
  static const struct { const char *n; unsigned b; } t[] = {
    {"A", GBCORE_BUTTON_A}, {"B", GBCORE_BUTTON_B}, {"SELECT", GBCORE_BUTTON_SELECT},
    {"START", GBCORE_BUTTON_START}, {"RIGHT", GBCORE_BUTTON_RIGHT}, {"LEFT", GBCORE_BUTTON_LEFT},
    {"UP", GBCORE_BUTTON_UP}, {"DOWN", GBCORE_BUTTON_DOWN}, {"R", 0}, {"L", 0} };
  unsigned i;
  for (i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcasecmp(k, t[i].n)) return (int)t[i].b;
  return -1;
}

static int parse_btns(char *s, unsigned *out)
{
  char *t;
  *out = 0;
  for (t = strtok(s, "+"); t; t = strtok(NULL, "+")) { int b = keybit(t); if (b < 0) return -1; *out |= (unsigned)b; }
  return 0;
}

static int load_script(const char *path)
{
  FILE *fp = fopen(path, "r");
  char line[256];
  int rep_start = -1, rep_n = 0, ln = 0;
  if (!fp) { perror(path); return -1; }
  while (fgets(line, sizeof line, fp))
  {
    char *c = strpbrk(line, "#;\r\n");
    char cmd[32] = "", a1[64] = "", a2[64] = "";
    struct step s;
    int n;
    if (c) *c = 0;
    ln++;
    n = sscanf(line, "%31s %63s %63s", cmd, a1, a2);
    if (n <= 0) continue;
    memset(&s, 0, sizeof s);
    if (!strcmp(cmd, "wait")) { s.op = OP_WAIT; s.n = (int)strtol(a1, NULL, 0); push(s); }
    else if (!strcmp(cmd, "press") || !strcmp(cmd, "hold"))
    {
      char tmp[64]; snprintf(tmp, sizeof tmp, "%s", a1);
      if (parse_btns(tmp, &s.keys)) { fprintf(stderr, "%s:%d bad buttons %s\n", path, ln, a1); return -1; }
      s.op = OP_HOLD; s.n = n >= 3 ? (int)strtol(a2, NULL, 0) : 2; push(s);
      if (!strcmp(cmd, "press")) { struct step r = { OP_WAIT, 0, 2, "" }; push(r); }
    }
    else if (!strcmp(cmd, "repeat")) { rep_start = nsteps; rep_n = (int)strtol(a1, NULL, 0); }
    else if (!strcmp(cmd, "endrepeat"))
    {
      int len, k, j;
      if (rep_start < 0) { fprintf(stderr, "%s:%d endrepeat without repeat\n", path, ln); return -1; }
      len = nsteps - rep_start;
      for (k = 1; k < rep_n; k++) for (j = 0; j < len; j++) push(steps[rep_start + j]);
      rep_start = -1;
    }
    else if (!strcmp(cmd, "dump")) { s.op = OP_DUMP; push(s); }
    else if (!strcmp(cmd, "evt")) { s.op = OP_EVT; snprintf(s.text, sizeof s.text, "%s", a1); push(s); }
    else { fprintf(stderr, "%s:%d unsupported command '%s'\n", path, ln, cmd); return -1; }
  }
  fclose(fp);
  return 0;
}

static int cur, cur_left = -1, want_dump;
static unsigned script_frame(unsigned f)
{
  while (cur < nsteps)
  {
    struct step *s = &steps[cur];
    if (s->op == OP_DUMP) { want_dump = 1; cur++; continue; }
    if (s->op == OP_EVT) { printf("EVT ap_mark text=%s f=%u\n", s->text, f); cur++; continue; }
    if (cur_left < 0) cur_left = s->n;
    if (cur_left == 0) { cur++; cur_left = -1; continue; }
    cur_left--;
    return s->op == OP_HOLD ? s->keys : 0;
  }
  return 0;
}

static int in_list(const char *list, unsigned f)
{
  const char *p = list;
  if (!list) return 0;
  while (*p) { char *e; unsigned long v = strtoul(p, &e, 0); if (v == f) return 1; if (*e != ',') break; p = e + 1; }
  return 0;
}

/* the last delivered frame, as 8-bit RGB */
static unsigned W = 160, H = 144;
static unsigned char rgb[256 * 256 * 3];
static void on_video(void *u, const gbcore_video_frame_t *fr)
{
  unsigned x, y;
  (void)u;
  W = fr->width; H = fr->height;
  for (y = 0; y < H; y++)
  {
    const uint16_t *row = (const uint16_t *)((const uint8_t *)fr->pixels + y * fr->pitch_bytes);
    for (x = 0; x < W; x++)
    {
      uint16_t v = row[x];             /* RGB565, red high (host build) */
      unsigned char *o = rgb + (y * W + x) * 3;
      o[0] = (unsigned char)(((v >> 11) & 31) << 3);
      o[1] = (unsigned char)(((v >> 5) & 63) << 2);
      o[2] = (unsigned char)((v & 31) << 3);
    }
  }
}
static void on_audio(void *u, const int16_t *s, size_t n) { (void)u; (void)s; (void)n; }
static time_t fixed_clock(void) { return 1700000000; }

static void write_bmp(const char *path)
{
  FILE *fp = fopen(path, "wb");
  unsigned row = (W * 3 + 3) & ~3u, size = 54 + row * H, x;
  int y;
  unsigned char hd[54] = { 'B', 'M' }, *buf;
  if (!fp) { perror(path); return; }
#define W32(o, v) do { hd[o] = (v) & 255; hd[o+1] = ((v) >> 8) & 255; hd[o+2] = ((v) >> 16) & 255; hd[o+3] = ((v) >> 24) & 255; } while (0)
  W32(2, size); W32(10, 54); W32(14, 40); W32(18, W); W32(22, H); hd[26] = 1; hd[28] = 24; W32(34, row * H);
  fwrite(hd, 1, 54, fp);
  buf = calloc(1, row);
  for (y = (int)H - 1; y >= 0; y--)
  {
    for (x = 0; x < W; x++)
    {
      const unsigned char *p = rgb + (y * W + x) * 3;
      buf[x * 3 + 0] = p[2]; buf[x * 3 + 1] = p[1]; buf[x * 3 + 2] = p[0];
    }
    fwrite(buf, 1, row, fp);
  }
  free(buf);
  fclose(fp);
}

static unsigned frame_hash(void)
{
  unsigned h = 2166136261u, i;
  for (i = 0; i < W * H; i++)
  {
    const unsigned char *p = rgb + i * 3;
    unsigned v = (p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10);
    h = (h ^ (v & 255)) * 16777619u;
    h = (h ^ (v >> 8)) * 16777619u;
  }
  return h;
}

static unsigned char *slurp(const char *path, size_t *n)
{
  FILE *f = fopen(path, "rb");
  unsigned char *p;
  long sz;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
  p = malloc((size_t)sz);
  if (p && fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); p = NULL; }
  fclose(f);
  *n = (size_t)sz;
  return p;
}

static void print_peek(gbcore_t *core, const char *spec, unsigned f)
{
  char tmp[256], *t;
  snprintf(tmp, sizeof tmp, "%s", spec);
  for (t = strtok(tmp, ","); t; t = strtok(NULL, ","))
  {
    unsigned long addr = strtoul(t, NULL, 0), len = 1, i;
    unsigned char b[64];
    char *c = strchr(t, ':');
    if (c) len = strtoul(c + 1, NULL, 0);
    if (len > sizeof b) len = sizeof b;
    printf("MEM f=%u addr=%04lx hex=", f, addr);
    if (gbcore_peek(core, (uint16_t)addr, b, (unsigned)len)) printf("ERR");
    else for (i = 0; i < len; i++) printf("%02x", b[i]);
    printf("\n");
  }
}

int main(int argc, char **argv)
{
  const char *rom = NULL, *script = NULL, *dump_at = NULL, *dump_dir = ".", *hash_path = NULL,
             *peek = NULL, *state_in = NULL, *state_out = NULL, *con = NULL,
             *statehash_path = NULL;
  FILE *shf = NULL;
  unsigned frames = 600, dump_every = 0, peek_every = 0, state_out_at = 0, f;
  int cc = 0, palette = GBCORE_PALETTE_AUTO, timing = 0, i;
  gbcore_callbacks_t cb;
  gbcore_t *core;
  fe_console_t console;
  FILE *hf;
  struct timespec t0, t1;
  for (i = 1; i < argc; i++)
  {
#define ARG(s) (!strcmp(argv[i], s) && i + 1 < argc)
    if (ARG("--rom")) rom = argv[++i];
    else if (ARG("--console")) con = argv[++i];
    else if (ARG("--frames")) frames = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (ARG("--script")) script = argv[++i];
    else if (ARG("--dump-at")) dump_at = argv[++i];
    else if (ARG("--dump-every")) dump_every = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (ARG("--dump-dir")) dump_dir = argv[++i];
    else if (ARG("--hash")) hash_path = argv[++i];
    else if (ARG("--statehash")) statehash_path = argv[++i];
    else if (ARG("--cc")) cc = atoi(argv[++i]);
    else if (ARG("--palette")) palette = atoi(argv[++i]);
    else if (ARG("--peek")) peek = argv[++i];
    else if (ARG("--peek-every")) peek_every = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (ARG("--state-in")) state_in = argv[++i];
    else if (ARG("--state-out")) state_out = argv[++i];
    else if (ARG("--state-out-at")) state_out_at = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--time")) timing = 1;
    else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
#undef ARG
  }
  if (!rom) { fprintf(stderr, "--rom required\n"); return 2; }
  if (script && load_script(script)) return 2;
  if (con) console = !strcmp(con, "gbc") ? FE_CONSOLE_GBC : FE_CONSOLE_GB;
  else { const char *d = strrchr(rom, '.'); console = (d && !strcasecmp(d, ".gbc")) ? FE_CONSOLE_GBC : FE_CONSOLE_GB; }
  memset(&cb, 0, sizeof cb);
  cb.video = on_video;
  cb.audio_batch = on_audio;
  gbcore_set_wallclock(fixed_clock);
  gbcore_set_palette((unsigned)palette);
  gbcore_set_color_correction(cc);
  core = gbcore_create(rom, NULL, 0, console, 32768, &cb);
  if (!core) { fprintf(stderr, "cannot load %s\n", rom); return 3; }
  if (state_in)
  {
    size_t n; unsigned char *b = slurp(state_in, &n);
    if (!b || gbcore_state_load(core, b, n)) { fprintf(stderr, "state load failed %s\n", state_in); return 3; }
    free(b);
  }
  printf("EVT boot rom=%s console=%s\n", rom, console == FE_CONSOLE_GBC ? "gbc" : "gb");
  hf = hash_path ? fopen(hash_path, "w") : NULL;
  shf = statehash_path ? fopen(statehash_path, "w") : NULL;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (f = 1; f <= frames; f++)
  {
    if (gbcore_run_frame(core, (uint16_t)script_frame(f))) { fprintf(stderr, "frame %u failed\n", f); return 4; }
    if (hf) fprintf(hf, "f=%u h=%08x\n", f, frame_hash());
    if (shf)
    {
      static unsigned char sb[1 << 20];
      long n = gbcore_state_save(core, sb, sizeof sb), k;
      unsigned long long h = 0xCBF29CE484222325ull;
      for (k = 0; k < n; k++) { h ^= sb[k]; h *= 0x100000001B3ull; }
      fprintf(shf, "f=%u s=%016llx\n", f, h);
    }
    if (want_dump || in_list(dump_at, f) || (dump_every && f % dump_every == 0))
    {
      char p[512]; snprintf(p, sizeof p, "%s/frame_%06u.bmp", dump_dir, f);
      write_bmp(p); printf("EVT frame_dump file=%s\n", p); want_dump = 0;
    }
    if (peek && ((peek_every && f % peek_every == 0) || f == frames)) print_peek(core, peek, f);
    if (state_out && f == state_out_at)
    {
      static unsigned char st[1 << 20];
      long n = gbcore_state_save(core, st, sizeof st);
      FILE *o = fopen(state_out, "wb");
      if (n > 0 && o) { fwrite(st, 1, (size_t)n, o); printf("EVT state_out f=%u bytes=%ld\n", f, n); }
      if (o) fclose(o);
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  if (hf) fclose(hf);
  if (shf) fclose(shf);
  if (timing)
    printf("EVT time frames=%u us_per_frame=%.1f\n", frames,
           ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3) / frames);
  printf("EVT exit code=0 frames=%u\n", frames);
  gbcore_shutdown(core);
  return 0;
}
