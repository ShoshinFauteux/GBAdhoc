/* Single-player GB/GBC trace, for proving a change leaves it bit-identical.
 *
 * tools/run_gb_tests.py builds this program twice -- against the current
 * gbcore/ and against the 3.0.0 release's (extracted by run_host_tests.py
 * into GBADHOC_BASELINE_DIR/gb) -- runs both on the same ROMs and requires
 * byte-identical output.  It uses only the API the release already had,
 * through the single-player entry point gbcore_run_frame, so it answers the
 * question for the path players actually run.
 *
 * Per frame it prints the hash of the picture, the sound, the save-state
 * image and the battery image.  The run covers: scripted buttons, a serial
 * peer that answers every transfer (the ad-hoc cable path), a stretch of
 * fast-forward frameskip (drawing skipped on alternate frames), and a save
 * state taken and loaded back mid-run.
 *
 *   bitident <frames> <rom> [<rom>...]      (no ROMs: the generated one)
 */
#include "../gbcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GB-PALETTE-FIXES.md: the Game Boy Color LCD model is the one deliberate
 * change to the picture since 3.0.0.  The trace turns it off where it
 * exists (weak: the 3.0.0 tree has no such function), so everything else
 * is still proved bit-identical; tools/run_gb_tests.py checks the model
 * itself separately (cgb_lcd). */
void gbcore_set_color_correction(int on) __attribute__((weak));

#define FNV_OFF 0xcbf29ce484222325ull
#define FNV_PRIME 0x100000001b3ull

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
  const uint8_t *p = (const uint8_t *)data;
  while (n--) { h ^= *p++; h *= FNV_PRIME; }
  return h;
}

static uint64_t video_h, audio_h;
static unsigned serial_calls;

static void on_video(void *u, const gbcore_video_frame_t *f)
{
  unsigned y;
  (void)u;
  video_h = FNV_OFF;
  for (y = 0; y < f->height; y++)
    video_h = fnv(video_h, (const uint8_t *)f->pixels + y * f->pitch_bytes,
                  f->width * 2);
}

static void on_audio(void *u, const int16_t *s, size_t n)
{
  (void)u;
  audio_h = fnv(audio_h, s, n * 4);
}

static int on_serial(void *u, uint8_t out, int internal, uint8_t *in)
{
  (void)u;
  serial_calls++;
  *in = (uint8_t)(out * 5 + 3 + internal);
  return 1;
}

static time_t fixed_clock(void) { return 1700000000; }

static uint16_t script(uint64_t frame)
{
  uint64_t x = frame * 0x9E3779B97F4A7C15ull;
  x ^= x >> 29;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 32;
  if ((frame / 8) % 5 == 0)
    return (uint16_t)(frame & 8 ? GBCORE_BUTTON_A : GBCORE_BUTTON_START);
  return (uint16_t)(x & 0xFF & ~GBCORE_BUTTON_SELECT);
}

/* A generated DMG ROM that keeps a serial transfer going (smoke.c's). */
static void make_rom(uint8_t *rom)
{
  static const uint8_t logo[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
    0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
    0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
    0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
  };
  static const uint8_t program[] = {
    0x31, 0xFE, 0xFF, 0x3E, 0x91, 0xE0, 0x40,     /* SP, LCD on        */
    0xF0, 0x01, 0x3C, 0xE0, 0x01,                 /* loop: SB++        */
    0x3E, 0x81, 0xE0, 0x02,                       /* SC = internal     */
    0xF0, 0x02, 0xCB, 0x7F, 0x20, 0xFA,           /* wait SC bit 7     */
    0xF0, 0x01, 0xEA, 0x00, 0xC0,                 /* SB -> WRAM        */
    0x18, 0xEA                                    /* JR loop           */
  };
  uint8_t check = 0;
  unsigned i;
  memset(rom, 0, 0x8000);
  rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;
  memcpy(rom + 0x104, logo, sizeof(logo));
  memcpy(rom + 0x134, "BITIDENT", 8);
  for (i = 0x134; i <= 0x14C; i++) check = (uint8_t)(check - rom[i] - 1);
  rom[0x14D] = check;
  memcpy(rom + 0x150, program, sizeof(program));
}

static uint8_t state[1 << 20], save[0x40000];

static int trace(const char *name, const void *data, size_t size,
                 fe_console_t console, unsigned frames)
{
  gbcore_callbacks_t cb;
  gbcore_t *core;
  unsigned f;
  memset(&cb, 0, sizeof(cb));
  cb.video = on_video;
  cb.audio_batch = on_audio;
  cb.serial_transfer = on_serial;
  gbcore_set_wallclock(fixed_clock);
  gbcore_set_palette(GBCORE_PALETTE_AUTO);
  if (gbcore_set_color_correction)
    gbcore_set_color_correction(0);
  core = gbcore_create(NULL, data, size, console, 32768, &cb);
  if (!core)
  {
    printf("%s: create failed\n", name);
    return 1;
  }
  serial_calls = 0;
  for (f = 0; f < frames; f++)
  {
    long n;
    size_t sn;
    audio_h = FNV_OFF;
    /* Frameskip stretch: the middle fifth, drawing every other frame. */
    gbcore_set_skip_render(core, f > frames * 2 / 5 && f < frames * 3 / 5 &&
                                 (f & 1));
    if (gbcore_run_frame(core, script(f)) != 0)
    {
      printf("%s: frame %u failed\n", name, f);
      return 1;
    }
    n = gbcore_state_save(core, state, sizeof(state));
    if (f == frames / 2 && n > 0 &&
        gbcore_state_load(core, state, (size_t)n) != 0)
    {
      printf("%s: state load failed\n", name);
      return 1;
    }
    sn = gbcore_save_ram_size(core);
    if (sn > sizeof(save) || gbcore_save_ram_read(core, save, sizeof(save)))
      sn = 0;
    printf("%s %u v=%016llx a=%016llx s=%016llx b=%016llx\n", name, f,
           (unsigned long long)video_h, (unsigned long long)audio_h,
           (unsigned long long)(n > 0 ? fnv(FNV_OFF, state, (size_t)n) : 0),
           (unsigned long long)fnv(FNV_OFF, save, sn));
  }
  printf("%s serial_calls=%u frames=%llu\n", name, serial_calls,
         (unsigned long long)gbcore_frame_number(core));
  gbcore_shutdown(core);
  return 0;
}

static uint8_t *load(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  uint8_t *p;
  long n;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  p = (uint8_t *)malloc((size_t)n);
  if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
  fclose(f);
  *size = (size_t)n;
  return p;
}

int main(int argc, char **argv)
{
  unsigned frames = argc > 1 ? (unsigned)atoi(argv[1]) : 600;
  int i, rc = 0;
  if (argc <= 2)
  {
    static uint8_t rom[0x8000];
    make_rom(rom);
    rc |= trace("generated.gb", rom, sizeof(rom), FE_CONSOLE_GB, frames);
    rc |= trace("generated-cgb-hw", rom, sizeof(rom), FE_CONSOLE_GBC, frames);
  }
  for (i = 2; i < argc; i++)
  {
    size_t size = 0;
    uint8_t *rom = load(argv[i], &size);
    const char *dot = strrchr(argv[i], '.');
    const char *base = strrchr(argv[i], '/');
    if (!rom) { printf("%s: unreadable\n", argv[i]); rc = 1; continue; }
    rc |= trace(base ? base + 1 : argv[i], rom, size,
                dot && (dot[3] == 'c' || dot[3] == 'C') ? FE_CONSOLE_GBC
                                                        : FE_CONSOLE_GB,
                frames);
    free(rom);
  }
  return rc;
}
