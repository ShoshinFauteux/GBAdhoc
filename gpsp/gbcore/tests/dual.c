/* Two TGB Dual instances in one process: isolation and headless fidelity.
 *
 * Instance A is the gbcore_* object; instance B is the same object with its
 * symbols renamed gbcoreb_* (tools/run_gb_tests.py builds it exactly as
 * psp/Makefile does).  For each ROM pair this proves, frame by frame:
 *
 *   1. B alone == A alone: the renamed copy is the same emulator
 *      (save-state image, picture, sound and sync hash identical).
 *   2. A and B interleaved one scanline at a time, no cable, both drawn:
 *      each machine's four hashes equal its single-instance run -- the two
 *      copies share no state.
 *   3. The same with one machine headless: the drawn one is still identical
 *      in all four, and the headless one's sync hash equals its drawn
 *      single-instance run's -- headless changes nothing the game can see.
 *   4. The sync hash notices a one-byte change and differs between ROMs,
 *      and the traces are alive (the state changes from frame to frame).
 *   5. The in-memory cable (gbcore_dual.c): a waiting peer takes the byte
 *      and returns its own; a peer that armed the external clock during
 *      the transfer and moved on (the Pokemon handshake) is clocked; an
 *      idle peer returns $FF; two machines handshaking in exact lockstep
 *      settle deterministically.
 *   6. Scanline batching (batch_lines): slot 0 running up to a frame ahead
 *      while the cable is quiet gives every cable case above the same bytes
 *      as strict alternation, for transfers and arms that begin at many
 *      different points of a batch.
 *
 * The committed ROMs are generated: a DMG and a CGB program that mix the
 * joypad into WRAM, cartridge RAM, VRAM and the sound registers from the
 * main loop, a timer interrupt and a VBlank interrupt.  With
 * GBADHOC_GB_ROMS=<dir> every .gb/.gbc there is run too, each paired with
 * the next, for GBADHOC_GB_FRAMES frames (default 3000) of scripted play.
 */
#include "../gbcore_dual.h"

#include <dirent.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

#define FNV_OFF 0xcbf29ce484222325ull
#define FNV_PRIME 0x100000001b3ull

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
  const uint8_t *p = (const uint8_t *)data;
  while (n--) { h ^= *p++; h *= FNV_PRIME; }
  return h;
}

/* ------------------------------------------------------------ the ROMs -- */

typedef struct {
  char name[64];
  uint8_t *data;
  size_t size;
  fe_console_t console;
} rom_t;

static void make_rom(rom_t *r, int color, uint8_t seed, uint8_t key)
{
  static const uint8_t logo[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
    0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
    0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
    0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
  };
  /* VBlank (0x40): DIV into the BG map.  Timer (0x50): count in SRAM. */
  static const uint8_t vblank[] = {
    0xF5,             /* PUSH AF          */
    0xF0, 0x04,       /* LDH A,(DIV)      */
    0xEA, 0x00, 0x98, /* LD ($9800),A     */
    0xF1,             /* POP AF           */
    0xD9              /* RETI             */
  };
  static const uint8_t timer[] = {
    0xF5,             /* PUSH AF          */
    0xFA, 0x02, 0xA0, /* LD A,($A002)     */
    0x3C,             /* INC A            */
    0xEA, 0x02, 0xA0, /* LD ($A002),A     */
    0xF1,             /* POP AF           */
    0xD9              /* RETI             */
  };
  uint8_t program[] = {
    0x31, 0xFE, 0xFF,       /* 150 LD SP,$FFFE             */
    0x3E, 0x0A,             /* 153 LD A,$0A                */
    0xEA, 0x00, 0x00,       /* 155 LD ($0000),A   RAM on   */
    0x3E, 0x80,             /* 158 LD A,$80                */
    0xE0, 0x26,             /* 15A LDH (NR52),A            */
    0x3E, 0x77,             /* 15C LD A,$77                */
    0xE0, 0x24,             /* 15E LDH (NR50),A            */
    0x3E, 0xFF,             /* 160 LD A,$FF                */
    0xE0, 0x25,             /* 162 LDH (NR51),A            */
    0x3E, 0x05,             /* 164 LD A,$05                */
    0xE0, 0x07,             /* 166 LDH (TAC),A             */
    0x3E, 0x05,             /* 168 LD A,$05  VBlank+timer  */
    0xE0, 0xFF,             /* 16A LDH (IE),A              */
    0xFB,                   /* 16C EI                      */
    0x21, 0x00, 0xC0,       /* 16D LD HL,$C000             */
    0x11, 0x00, 0x00,       /* 170 LD DE,seed (patched)    */
    0x3E, 0x10,             /* 173 loop: LD A,$10          */
    0xE0, 0x00,             /* 175 LDH (P1),A  buttons     */
    0xF0, 0x00,             /* 177 LDH A,(P1)              */
    0x83,                   /* 179 ADD A,E                 */
    0x07,                   /* 17A RLCA                    */
    0xEE, 0x00,             /* 17B XOR key (patched)       */
    0x5F,                   /* 17D LD E,A                  */
    0x22,                   /* 17E LD (HL+),A              */
    0xE0, 0x13,             /* 17F LDH (NR13),A            */
    0xEA, 0x00, 0xA0,       /* 181 LD ($A000),A            */
    0x7C,                   /* 184 LD A,H                  */
    0xFE, 0xE0,             /* 185 CP $E0                  */
    0x20, 0xEA,             /* 187 JR NZ,loop              */
    0x26, 0xC0,             /* 189 LD H,$C0                */
    0x3E, 0xF3,             /* 18B LD A,$F3                */
    0xE0, 0x12,             /* 18D LDH (NR12),A            */
    0x3E, 0x87,             /* 18F LD A,$87                */
    0xE0, 0x14,             /* 191 LDH (NR14),A  trigger   */
    0xFA, 0x01, 0xA0,       /* 193 LD A,($A001)            */
    0x3C,                   /* 196 INC A                   */
    0xEA, 0x01, 0xA0,       /* 197 LD ($A001),A            */
    0x18, 0xD7              /* 19A JR loop                 */
  };
  unsigned i;
  uint8_t check = 0;
  r->size = 0x8000;
  r->data = (uint8_t *)calloc(1, r->size);
  CHECK(r->data);
  r->console = color ? FE_CONSOLE_GBC : FE_CONSOLE_GB;
  snprintf(r->name, sizeof(r->name), "synthetic-%s-%02x", color ? "gbc" : "gb",
           seed);
  program[0x171 - 0x150] = seed;
  program[0x172 - 0x150] = (uint8_t)(seed * 7 + 1);
  program[0x17C - 0x150] = key;
  memcpy(r->data + 0x40, vblank, sizeof(vblank));
  memcpy(r->data + 0x50, timer, sizeof(timer));
  r->data[0x100] = 0xC3; r->data[0x101] = 0x50; r->data[0x102] = 0x01;
  memcpy(r->data + 0x104, logo, sizeof(logo));
  memcpy(r->data + 0x134, "DUAL TEST", 9);
  r->data[0x13D] = seed;                /* distinct headers */
  r->data[0x143] = color ? 0x80 : 0x00;
  r->data[0x147] = 0x03;                /* MBC1+RAM+BATTERY */
  r->data[0x149] = 0x02;                /* 8 KiB */
  for (i = 0x134; i <= 0x14C; i++) check = (uint8_t)(check - r->data[i] - 1);
  r->data[0x14D] = check;
  memcpy(r->data + 0x150, program, sizeof(program));
}

static int load_file(rom_t *r, const char *dir, const char *name)
{
  char path[1024];
  FILE *f;
  long n;
  const char *dot = strrchr(name, '.');
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  r->data = (uint8_t *)malloc((size_t)n);
  CHECK(r->data);
  CHECK(fread(r->data, 1, (size_t)n, f) == (size_t)n);
  fclose(f);
  r->size = (size_t)n;
  r->console = (dot && (dot[3] == 'c' || dot[3] == 'C')) ? FE_CONSOLE_GBC
                                                        : FE_CONSOLE_GB;
  snprintf(r->name, sizeof(r->name), "%s", name);
  return 0;
}

/* ---------------------------------------------------------- the traces -- */

typedef struct {
  uint64_t state, video, audio, sync;
} frame_hash_t;

typedef struct {
  frame_hash_t *f;
  unsigned n, cap;
  uint64_t video_acc, audio_acc;   /* this frame's, until recorded */
  int video_seen;
} trace_t;

static void on_video(void *userdata, const gbcore_video_frame_t *frame)
{
  trace_t *t = (trace_t *)userdata;
  unsigned y;
  uint64_t h = FNV_OFF;
  for (y = 0; y < frame->height; y++)
    h = fnv(h, (const uint8_t *)frame->pixels + y * frame->pitch_bytes,
            frame->width * 2);
  t->video_acc = h;
  t->video_seen = 1;
}

static void on_audio(void *userdata, const int16_t *s, size_t frames)
{
  trace_t *t = (trace_t *)userdata;
  t->audio_acc = fnv(t->audio_acc, s, frames * 4);
}

/* A deterministic button script: mostly START/A taps with some held
 * directions, differing per slot. */
static uint16_t script(int slot, uint64_t frame)
{
  uint64_t x = frame * 0x9E3779B97F4A7C15ull + (uint64_t)slot * 0x5851F42D;
  x ^= x >> 29;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 32;
  if ((frame / 8) % 5 == 0)
    return (uint16_t)(frame & 8 ? GBCORE_BUTTON_A : GBCORE_BUTTON_START);
  return (uint16_t)(x & 0xFF & ~(GBCORE_BUTTON_SELECT));
}

static uint8_t state_buf[1 << 20];

static void record(const gbcore_api_t *api, gbcore_t *core, trace_t *t,
                   int headless)
{
  frame_hash_t *h;
  long n = 0;
  if (t->n >= t->cap)
    return;             /* the other machine is still catching up */
  h = &t->f[t->n++];
  if (!headless)
  {
    n = api->state_save(core, state_buf, sizeof(state_buf));
    CHECK(n > 0);
  }
  h->state = headless ? 0 : fnv(FNV_OFF, state_buf, (size_t)n);
  h->video = t->video_seen ? t->video_acc : 0;
  h->audio = t->audio_acc;
  h->sync = api->sync_hash(core);
  t->video_seen = 0;
  t->audio_acc = FNV_OFF;
}

static time_t fixed_clock(void) { return 1700000000; }

static void trace_init(trace_t *t, unsigned frames)
{
  memset(t, 0, sizeof(*t));
  t->f = (frame_hash_t *)calloc(frames, sizeof(frame_hash_t));
  CHECK(t->f);
  t->cap = frames;
  t->audio_acc = FNV_OFF;
}

/* One machine on its own through instance `slot`'s API, playing the
 * button script of `player` (the slot it will occupy in the dual run),
 * after gbcore_power_on when `power` is set. */
static void run_single_ex(int slot, int player, const rom_t *rom,
                          unsigned frames, trace_t *t, int power,
                          int headless)
{
  const gbcore_api_t *api = gbdual_api(slot);
  gbcore_callbacks_t cb;
  gbcore_t *core;
  unsigned f;
  trace_init(t, frames);
  memset(&cb, 0, sizeof(cb));
  cb.userdata = t;
  cb.video = on_video;
  cb.audio_batch = on_audio;
  if (power)
    CHECK(api->power_on() == 0);
  api->set_wallclock(fixed_clock);
  api->set_palette(GBCORE_PALETTE_AUTO);
  core = api->create(NULL, rom->data, rom->size, rom->console, 32768, &cb);
  CHECK(core);
  if (headless)
    api->set_headless(core, 1);
  for (f = 0; f < frames; f++)
  {
    CHECK(api->run_frame(core, script(player, f)) == 0);
    record(api, core, t, headless);
  }
  api->shutdown(core);
}

/* The reference: from power-on, drawn. */
static void run_single(int slot, int player, const rom_t *rom,
                       unsigned frames, trace_t *t)
{
  run_single_ex(slot, player, rom, frames, t, 1, 0);
}

typedef struct {
  gbdual_t *dual;
  trace_t *t[2];
  int headless[2];
  unsigned frames;
} dual_ctx_t;

static int input(void *userdata, int slot, uint64_t frame, uint16_t *b)
{
  dual_ctx_t *c = (dual_ctx_t *)userdata;
  /* Record the frame that just ended before the next one begins. */
  if (frame > 0)
    record(gbdual_api(slot), gbdual_core(c->dual, slot), c->t[slot],
           c->headless[slot]);
  *b = script(slot, frame);
  return 1;
}

static void run_dual(const rom_t *a, const rom_t *b, int headless_a,
                     int headless_b, unsigned frames, trace_t *ta,
                     trace_t *tb)
{
  gbdual_config_t cfg;
  dual_ctx_t c;
  memset(&cfg, 0, sizeof(cfg));
  memset(&c, 0, sizeof(c));
  trace_init(ta, frames + 2);
  trace_init(tb, frames + 2);
  cfg.machine[0].rom_data = a->data;
  cfg.machine[0].rom_size = a->size;
  cfg.machine[0].console = a->console;
  cfg.machine[0].wallclock = fixed_clock;
  cfg.machine[0].headless = headless_a;
  cfg.machine[0].callbacks.userdata = ta;
  cfg.machine[0].callbacks.video = on_video;
  cfg.machine[0].callbacks.audio_batch = on_audio;
  cfg.machine[1] = cfg.machine[0];
  cfg.machine[1].rom_data = b->data;
  cfg.machine[1].rom_size = b->size;
  cfg.machine[1].console = b->console;
  cfg.machine[1].headless = headless_b;
  cfg.machine[1].callbacks.userdata = tb;
  cfg.audio_rate = 32768;
  cfg.link = 0;
  cfg.pace_slot = 0;
  c.dual = gbdual_create(&cfg);
  CHECK(c.dual);
  c.t[0] = ta;
  c.t[1] = tb;
  c.headless[0] = headless_a;
  c.headless[1] = headless_b;
  /* Until both have completed `frames` (and begun the next, which records
   * the last one). */
  while (gbdual_frame(c.dual, 0) <= frames || gbdual_frame(c.dual, 1) <= frames)
    CHECK(gbdual_advance(c.dual, input, &c) == 1);
  gbdual_destroy(c.dual);
}

static void compare(const char *what, const trace_t *ref, const trace_t *got,
                    unsigned frames, int sync_only)
{
  unsigned f;
  CHECK(got->n >= frames);
  for (f = 0; f < frames; f++)
  {
    const frame_hash_t *r = &ref->f[f], *g = &got->f[f];
    if (r->sync != g->sync ||
        (!sync_only && (r->state != g->state || r->video != g->video ||
                        r->audio != g->audio)))
    {
      fprintf(stderr, "FAIL %s: frame %u differs (state %d video %d audio %d "
              "sync %d)\n", what, f, r->state != g->state,
              r->video != g->video, r->audio != g->audio, r->sync != g->sync);
      exit(1);
    }
  }
}

static unsigned distinct_sync(const trace_t *t, unsigned frames)
{
  unsigned f, n = 0;
  for (f = 1; f < frames; f++)
    if (t->f[f].sync != t->f[f - 1].sync) n++;
  return n;
}

static void free_trace(trace_t *t) { free(t->f); t->f = NULL; }

static void pair(const rom_t *a, const rom_t *b, unsigned frames)
{
  trace_t ra, rb, rb_as_a, da, db;
  unsigned alive_a, alive_b;

  run_single(0, 0, a, frames, &ra);
  run_single(1, 1, b, frames, &rb);
  run_single(0, 1, b, frames, &rb_as_a);
  compare("instance B alone == instance A alone", &rb, &rb_as_a, frames, 0);
  free_trace(&rb_as_a);

  alive_a = distinct_sync(&ra, frames);
  alive_b = distinct_sync(&rb, frames);
  CHECK(alive_a > frames / 4 && alive_b > frames / 4);

  run_dual(a, b, 0, 0, frames, &da, &db);
  compare("dual A (both drawn)", &ra, &da, frames, 0);
  compare("dual B (both drawn)", &rb, &db, frames, 0);
  free_trace(&da); free_trace(&db);

  run_dual(a, b, 0, 1, frames, &da, &db);
  compare("dual A (B headless)", &ra, &da, frames, 0);
  compare("dual B headless, sync", &rb, &db, frames, 1);
  free_trace(&da); free_trace(&db);

  run_dual(a, b, 1, 0, frames, &da, &db);
  compare("dual A headless, sync", &ra, &da, frames, 1);
  compare("dual B (A headless)", &rb, &db, frames, 0);
  free_trace(&da); free_trace(&db);

  printf("dual isolation %s + %s: %u frames, 4 hashes/frame, identical to "
         "single runs (drawn and headless); state changed in %u/%u frames\n",
         a->name, b->name, frames, alive_a < alive_b ? alive_a : alive_b,
         frames - 1);
  free_trace(&ra); free_trace(&rb);
}

/* The sync hash sees a single byte of cartridge RAM, and is equal for two
 * machines in the same state. */
static void hash_sensitivity(const rom_t *r)
{
  const gbcore_api_t *a = gbdual_api(0), *b = gbdual_api(1);
  gbcore_t *ca, *cb;
  uint8_t ram[0x2000 + 64];
  unsigned f;
  CHECK(a->power_on() == 0 && b->power_on() == 0);
  a->set_wallclock(fixed_clock);
  b->set_wallclock(fixed_clock);
  ca = a->create(NULL, r->data, r->size, r->console, 32768, NULL);
  cb = b->create(NULL, r->data, r->size, r->console, 32768, NULL);
  CHECK(ca && cb);
  for (f = 0; f < 30; f++)
  {
    CHECK(a->run_frame(ca, script(0, f)) == 0);
    CHECK(b->run_frame(cb, script(0, f)) == 0);
    CHECK(a->sync_hash(ca) == b->sync_hash(cb));
  }
  CHECK(a->save_ram_read(ca, ram, sizeof(ram)) == 0);
  ram[0x1234] ^= 1;
  CHECK(a->save_ram_write(ca, ram, a->cart_ram_size(ca)) == 0);
  CHECK(a->sync_hash(ca) != b->sync_hash(cb));
  a->shutdown(ca);
  b->shutdown(cb);
  printf("sync hash: equal for equal machines, sees a 1-bit SRAM change\n");
}

/* gbcore_power_on erases history: a run after power-on is the run a fresh
 * process makes (checked in a forked child that has never touched the
 * core), in every hash including sound, whatever ran before it. */
static void power_on_is_fresh(const rom_t *x, const rom_t *y)
{
  const unsigned frames = 240;
  size_t bytes = frames * sizeof(frame_hash_t);
  frame_hash_t *shared;
  trace_t first, after;
  pid_t pid;
  int status;
  unsigned slot;

  shared = (frame_hash_t *)mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK(shared != MAP_FAILED);
  fflush(stdout);
  pid = fork();
  CHECK(pid >= 0);
  if (pid == 0)
  {
    /* Fresh program: no power_on, first use of instance B. */
    trace_t t;
    run_single_ex(1, 0, y, frames, &t, 0, 0);
    memcpy(shared, t.f, bytes);
    _exit(0);
  }
  CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
  for (slot = 0; slot < 2; slot++)
  {
    trace_t fresh, dirty;
    unsigned f;
    fresh.f = shared;
    fresh.n = frames;
    run_single_ex(slot, 0, x, frames, &first, 1, 0);   /* history */
    free_trace(&first);
    /* Without power-on the previous game leaks into this one... */
    run_single_ex(slot, 0, y, frames, &dirty, 0, 0);
    for (f = 0; f < frames; f++)
      if (dirty.f[f].sync != fresh.f[f].sync)
        break;
    CHECK(f < frames);
    printf("  instance %c: without power-on, game-visible state differs from "
           "a fresh process at frame %u\n", 'A' + slot, f);
    free_trace(&dirty);
    /* ...and with it, it does not. */
    run_single_ex(slot, 0, x, frames, &first, 1, 0);
    free_trace(&first);
    run_single_ex(slot, 0, y, frames, &after, 1, 0);
    compare("power_on == fresh process", &fresh, &after, frames, 0);
    free_trace(&after);
  }
  munmap(shared, bytes);
  printf("power_on: a run after power-on equals a fresh process's in every "
         "hash (%u frames, both instances, after another game ran)\n",
         frames);
}

static int is_rom(const char *n)
{
  const char *d = strrchr(n, '.');
  return d && (!strcmp(d, ".gb") || !strcmp(d, ".gbc") || !strcmp(d, ".GB") ||
               !strcmp(d, ".GBC"));
}

static int by_name(const void *x, const void *y)
{
  return strcmp(*(char *const *)x, *(char *const *)y);
}

/* ------------------------------------------------------ the cable rules -- */

/* A cartridge whose program (at 0x150) ends by storing SB at $A000 and a
 * done marker 0x42 at $A001 (MBC1+RAM+BATTERY, RAM enabled first). */
static void cable_rom(rom_t *r, const uint8_t *prog, size_t n, const char *name)
{
  static const uint8_t logo[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
    0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
    0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
    0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
  };
  static const uint8_t head[] = {
    0x31, 0xFE, 0xFF,                   /* LD SP,$FFFE          */
    0x3E, 0x0A, 0xEA, 0x00, 0x00        /* RAM on               */
  };
  uint8_t check = 0;
  unsigned i;
  r->size = 0x8000;
  r->data = (uint8_t *)calloc(1, r->size);
  CHECK(r->data);
  r->console = FE_CONSOLE_GB;
  snprintf(r->name, sizeof(r->name), "%s", name);
  r->data[0x100] = 0xC3; r->data[0x101] = 0x50; r->data[0x102] = 0x01;
  memcpy(r->data + 0x104, logo, sizeof(logo));
  memcpy(r->data + 0x134, name, strlen(name) < 11 ? strlen(name) : 11);
  r->data[0x147] = 0x03;
  r->data[0x149] = 0x02;
  for (i = 0x134; i <= 0x14C; i++) check = (uint8_t)(check - r->data[i] - 1);
  r->data[0x14D] = check;
  memcpy(r->data + 0x150, head, sizeof(head));
  memcpy(r->data + 0x150 + sizeof(head), prog, n);
}

/* Clocking side: SB=$5A, internal-clock transfer, wait, store SB. */
static const uint8_t prog_master[] = {
  0x3E, 0x5A, 0xE0, 0x01,               /* SB = $5A               */
  0x3E, 0x81, 0xE0, 0x02,               /* SC = internal, start   */
  0xF0, 0x02, 0xCB, 0x7F, 0x20, 0xFA,   /* wait SC bit 7 clear    */
  0xF0, 0x01, 0xEA, 0x00, 0xA0,         /* SB -> $A000            */
  0x3E, 0x42, 0xEA, 0x01, 0xA0,         /* done                   */
  0x18, 0xFE
};
/* Waits on the external clock the whole time (SB=$C3, SC=$80). */
static const uint8_t prog_waiting[] = {
  0x3E, 0xC3, 0xE0, 0x01,
  0x3E, 0x80, 0xE0, 0x02,
  0xF0, 0x02, 0xCB, 0x7F, 0x20, 0xFA,
  0xF0, 0x01, 0xEA, 0x00, 0xA0,
  0x3E, 0x42, 0xEA, 0x01, 0xA0,
  0x18, 0xFE
};
/* The Pokemon handshake's shape: arm the external clock with SB=$02 for
 * a few instructions, then start an internal attempt with SB=$01; repeat
 * until an attempt brings back something other than $FF. */
static const uint8_t prog_brief[] = {
  0x3E, 0x02, 0xE0, 0x01,               /* 0158 loop: SB = 2      */
  0x3E, 0x80, 0xE0, 0x02,               /* SC = external armed    */
  0x3E, 0x01, 0xE0, 0x01,               /* SB = 1                 */
  0x3E, 0x81, 0xE0, 0x02,               /* SC = internal start    */
  0xF0, 0x02, 0xCB, 0x7F, 0x20, 0xFA,   /* wait SC bit 7 clear    */
  0xF0, 0x01, 0xFE, 0xFF, 0x28, 0xE4,   /* SB==$FF: JR loop       */
  0xEA, 0x00, 0xA0,                     /* SB -> $A000            */
  0x3E, 0x42, 0xEA, 0x01, 0xA0,
  0x18, 0xFE
};
/* Never touches the port. */
static const uint8_t prog_idle[] = { 0x18, 0xFE };

static int cable_input(void *ud, int slot, uint64_t frame, uint16_t *b)
{
  (void)ud; (void)slot; (void)frame;
  *b = 0;
  return 1;
}

/* Runs the pair 20 frames; SB stored ($A000) and done marker ($A001) per
 * machine into out[2][2].  `wait` = iterations of a 28-cycle delay loop
 * each program runs first (0 = none), `batch`/`quiet` as gbdual_config_t. */
static void cable_run(const uint8_t *p0, size_t n0, const uint8_t *p1,
                      size_t n1, const unsigned wait[2], unsigned batch,
                      unsigned quiet, uint8_t out[2][2], uint64_t *batched)
{
  gbdual_config_t cfg;
  rom_t r[2];
  gbdual_t *d;
  uint8_t ram[2][0x2000];
  uint8_t prog[2][96];
  const uint8_t *src[2] = { p0, p1 };
  size_t len[2] = { n0, n1 };
  unsigned f, s;
  memset(&cfg, 0, sizeof(cfg));
  for (s = 0; s < 2; s++)
  {
    size_t k = 0;
    if (wait[s])
    {
      /* LD BC,n ; loop: DEC BC ; LD A,B ; OR C ; JR NZ,loop */
      prog[s][k++] = 0x01;
      prog[s][k++] = (uint8_t)wait[s];
      prog[s][k++] = (uint8_t)(wait[s] >> 8);
      prog[s][k++] = 0x0B; prog[s][k++] = 0x78; prog[s][k++] = 0xB1;
      prog[s][k++] = 0x20; prog[s][k++] = 0xFB;
    }
    CHECK(k + len[s] <= sizeof(prog[s]));
    memcpy(prog[s] + k, src[s], len[s]);
    /* relative jumps inside the program are unaffected by the prefix */
    cable_rom(&r[s], prog[s], k + len[s], s ? "CABLE B" : "CABLE A");
  }
  cfg.batch_lines = batch;
  cfg.quiet_frames = quiet;
  for (s = 0; s < 2; s++)
  {
    cfg.machine[s].rom_data = r[s].data;
    cfg.machine[s].rom_size = r[s].size;
    cfg.machine[s].console = r[s].console;
    cfg.machine[s].wallclock = fixed_clock;
  }
  cfg.audio_rate = 32768;
  cfg.link = 1;
  d = gbdual_create(&cfg);
  CHECK(d);
  for (f = 0; f < 20; f++)
    CHECK(gbdual_advance(d, cable_input, NULL) == 1);
  for (s = 0; s < 2; s++)
  {
    const gbcore_api_t *api = gbdual_api((int)s);
    CHECK(api->save_ram_read(gbdual_core(d, (int)s), ram[s], sizeof(ram[s])) == 0);
    out[s][0] = ram[s][0];
    out[s][1] = ram[s][1];
  }
  if (batched)
    *batched = gbdual_batch_cuts(d);
  gbdual_destroy(d);
  free(r[0].data);
  free(r[1].data);
}

static void cable_case(const char *what, const uint8_t *p0, size_t n0,
                       const uint8_t *p1, size_t n1, int s0_sb, int s1_sb)
{
  static const unsigned waits[][2] = {
    { 0, 0 }, { 6000, 0 }, { 0, 6000 }, { 6000, 6100 }, { 6100, 6000 },
    { 6000, 6600 }, { 7000, 6000 }, { 9000, 9013 }, { 9500, 9400 }
  };
  uint8_t ram[2][2], got[2][2];
  unsigned s, w, batched_cases = 0;
  unsigned none[2] = { 0, 0 };
  cable_run(p0, n0, p1, n1, none, 1, 0, ram, NULL);
  for (s = 0; s < 2; s++)
  {
    if ((s ? s1_sb : s0_sb) < 0)
      CHECK(ram[s][1] != 0x42);
    else
    {
      CHECK(ram[s][1] == 0x42);
      CHECK(ram[s][0] == (s ? s1_sb : s0_sb));
    }
  }
  printf("cable %s: slot 0 SB=%02x%s, slot 1 SB=%02x%s\n", what, ram[0][0],
         ram[0][1] == 0x42 ? "" : " (waiting)", ram[1][0],
         ram[1][1] == 0x42 ? "" : " (waiting)");
  /* The same case started at many points of a batch: slot 0 up to a whole
   * frame (154 scanlines) ahead while the cable is quiet must give exactly
   * the bytes strict alternation gives. */
  for (w = 0; w < sizeof(waits) / sizeof(waits[0]); w++)
  {
    uint8_t ref[2][2];
    uint64_t batched = 0;
    cable_run(p0, n0, p1, n1, waits[w], 1, 0, ref, NULL);
    cable_run(p0, n0, p1, n1, waits[w], 154, 1, got, &batched);
    if (memcmp(ref, got, sizeof(ref)))
    {
      fprintf(stderr, "FAIL cable %s batched, waits %u/%u: strict %02x/%02x "
              "%02x/%02x, batched %02x/%02x %02x/%02x\n", what, waits[w][0],
              waits[w][1], ref[0][0], ref[0][1], ref[1][0], ref[1][1],
              got[0][0], got[0][1], got[1][0], got[1][1]);
      exit(1);
    }
    if (batched)
      batched_cases++;
  }
  CHECK(batched_cases >= 5);   /* the serial events did cut batches */
  printf("cable %s: batched == strict at %u start points (%u began "
         "mid-batch)\n",
         what, (unsigned)(sizeof(waits) / sizeof(waits[0])), batched_cases);
}

static void cable_rules(void)
{
  /* TGB Dual's rule: a waiting peer takes the byte and gives its own. */
  cable_case("peer waiting", prog_master, sizeof(prog_master),
             prog_waiting, sizeof(prog_waiting), 0xC3, 0x5A);
  cable_case("peer waiting (clock on slot 1)", prog_waiting,
             sizeof(prog_waiting), prog_master, sizeof(prog_master), 0x5A,
             0xC3);
  /* Armed during the transfer and moved on: the handshake case. */
  cable_case("peer armed briefly", prog_master, sizeof(prog_master),
             prog_brief, sizeof(prog_brief), 0x02, 0x5A);
  /* Nobody listening: $FF, and the idle side is untouched. */
  cable_case("peer idle", prog_master, sizeof(prog_master), prog_idle,
             sizeof(prog_idle), 0xFF, -1);
  /* Both run the handshake in exact lockstep: slot 0 clocks, slot 1 is
   * clocked -- deterministic, where consoles would settle by clock phase. */
  cable_case("both handshaking in lockstep", prog_brief, sizeof(prog_brief),
             prog_brief, sizeof(prog_brief), 0x02, 0x01);
}

int main(void)
{
  rom_t dmg, cgb;
  const char *dir = getenv("GBADHOC_GB_ROMS");

  make_rom(&dmg, 0, 0x21, 0x5A);
  make_rom(&cgb, 1, 0x93, 0xC3);
  cable_rules();
  power_on_is_fresh(&dmg, &cgb);
  hash_sensitivity(&dmg);
  pair(&dmg, &cgb, 600);
  pair(&cgb, &dmg, 300);
  pair(&dmg, &dmg, 300);
  free(dmg.data);
  free(cgb.data);

  if (dir && *dir)
  {
    DIR *d = opendir(dir);
    char *names[64];
    unsigned n = 0, i, frames = 3000;
    struct dirent *e;
    const char *fr = getenv("GBADHOC_GB_FRAMES");
    if (fr) frames = (unsigned)atoi(fr);
    CHECK(d);
    while ((e = readdir(d)) && n < 64)
      if (is_rom(e->d_name))
        names[n++] = strdup(e->d_name);
    closedir(d);
    qsort(names, n, sizeof(names[0]), by_name);
    CHECK(n >= 1);
    for (i = 0; i < n; i++)
    {
      rom_t a, b;
      CHECK(load_file(&a, dir, names[i]) == 0);
      CHECK(load_file(&b, dir, names[(i + 1) % n]) == 0);
      pair(&a, &b, frames);
      free(a.data);
      free(b.data);
    }
    for (i = 0; i < n; i++) free(names[i]);
  }
  else
    printf("SKIP real-ROM isolation: GBADHOC_GB_ROMS is not set\n");
  return 0;
}
