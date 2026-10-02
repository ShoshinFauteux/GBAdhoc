/* Copyright (c) 2026 GBAdhoc contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TGB Dual adapter: GBAdhoc's gbcore.h API over the vendored core in
 * tgbdual/ (Hii's TGB Dual as ported to the PSP by MasterBoy; GPL v2 or
 * later, see tgbdual/README.md).  It replaces MasterBoy's frontend glue
 * (psp/gameboy_render.c, psp/SMS.c, loadrom.c): input, the MBC3 clock, the
 * frame loop, sound rendering, battery images and save states.
 *
 * The core is one global machine, so there is at most one gbcore_t per
 * instance.  A build may link this object twice, the second copy with its
 * gbcore_* symbols renamed gbcoreb_* (gbcore.h); everything else in it is
 * local, so the copies share nothing.
 */
#include "gbcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tgbdual/gb.h"

/* Largest cartridge any supported mapper addresses (MBC5: 512 x 16 KiB). */
#define GBCORE_ROM_MAX (8u * 1024u * 1024u)
/* The ROM buffer is at least this large and padded with 0xFF: MBC2 and
 * MMM01 select banks without masking them to the cartridge size, so a small
 * image must still have readable memory behind every bank they can name. */
#define GBCORE_ROM_MIN_ALLOC 0x100000u
/* Cartridge RAM backing store.  MBC5 can select 16 banks of 8 KiB even when
 * the header declares less; the core does not mask them. */
#define GBCORE_SRAM_ALLOC 0x20000u
#define GB_CLOCK_HZ 4194304u
#define GB_LINE_CLOCKS 456u
#define GB_FRAME_LINES 154u
#define RTC_TRAILER_48 48u
#define RTC_TRAILER_44 44u
#define RTC_SPAN (512 * 86400)        /* the 9-bit day counter wraps here */
#define RTC_EPOCH_MIN 852076800       /* 1997-01-01: older stamps are junk */

#define STATE_MAGIC "GBAT"
#define STATE_VERSION 1u
#define STATE_HEADER 40u
#define STATE_EXT 96u

struct gbcore {
  gbcore_callbacks_t callbacks;
  fe_console_t console;
  uint8_t *rom;
  uint8_t *sram;
  int16_t *audio;
  size_t audio_capacity;       /* stereo pairs */
  uint64_t audio_acc;          /* fractional samples, in GB clocks * rate */
  unsigned sample_rate;
  uint64_t frame_number;
  uint16_t prev_buttons;
  int skip_render;
  int headless;                /* gbcore_set_headless */
  unsigned lines;              /* scanlines run in the current frame */
  uint8_t header[0x1C];        /* ROM 0x134..0x14F: identity for states */
};

static gbcore_t *active;
static time_t (*gbcore_wallclock)(void);
static unsigned palette_id = GBCORE_PALETTE_AUTO;

/* ------------------------------------------------------------ power on --
 * gbcore_power_on returns every static byte of this instance to its state
 * in a freshly loaded program.  gbcore/gbcore_instance.ld gathers the
 * instance's writable data and marks it; the initialised part (.data) is
 * copied aside at the instance's first call, before anything can have
 * changed it, and the zero-initialised part (.bss) is simply cleared.  A
 * build that links the adapter without that script has no marks, and
 * gbcore_power_on fails. */
extern char tgb_image_data_start[] __attribute__((weak));
extern char tgb_image_data_end[] __attribute__((weak));
extern char tgb_image_bss_start[] __attribute__((weak));
extern char tgb_image_bss_end[] __attribute__((weak));

/* Outside the cleared range (the script places .bss.gbcore_keep first). */
static struct {
  uint8_t *data;
  size_t size;
  int taken;
  int failed;
} image __attribute__((section(".bss.gbcore_keep")));

/* Byte loops the sanitizer leaves alone: the ranges include the padding
 * AddressSanitizer puts between instrumented globals. */
__attribute__((no_sanitize_address))
static void image_copy(volatile uint8_t *dst, const volatile uint8_t *src,
                       size_t n)
{
  while (n--)
    *dst++ = *src++;
}

__attribute__((no_sanitize_address))
static void image_zero(volatile uint8_t *dst, size_t n)
{
  while (n--)
    *dst++ = 0;
}

static void image_keep(void)
{
  size_t n;
  if (image.taken)
    return;
  image.taken = 1;
  if (!tgb_image_data_start || !tgb_image_data_end ||
      !tgb_image_bss_start || !tgb_image_bss_end)
  {
    image.failed = 1;
    return;
  }
  n = (size_t)(tgb_image_data_end - tgb_image_data_start);
  image.data = n ? (uint8_t *)malloc(n) : NULL;
  if (n && !image.data)
  {
    image.failed = 1;
    return;
  }
  image_copy(image.data, (const volatile uint8_t *)tgb_image_data_start, n);
  image.size = n;
}

/* Core-visible state owned by the adapter (tgb_port.h). */
int pad_state;
word tgb_dmg_palette[12];

/* ------------------------------------------------------------- palettes -- */

#define BGR555(rgb) ((word)((((rgb) >> 19) & 0x1f) | \
                            ((((rgb) >> 11) & 0x1f) << 5) | \
                            ((((rgb) >> 3) & 0x1f) << 10)))

static const uint32_t palette_rgb[GBCORE_PALETTE_COUNT][4] = {
  /* AUTO outside SGB: MasterBoy's default greys. */
  { 0xFFFFFF, 0xA8A8A8, 0x585858, 0x000000 },
  { 0xFFFFFF, 0xA8A8A8, 0x585858, 0x000000 },
  /* The original DMG's green, as BGB and most emulators render it. */
  { 0xE0F8D0, 0x88C070, 0x346856, 0x081820 },
  /* Game Boy Pocket: a warm, slightly yellow grey. */
  { 0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F },
};

static const char *const palette_names[GBCORE_PALETTE_COUNT] = {
  "Auto", "Grey", "DMG green", "Pocket"
};

static void palette_apply(void)
{
  unsigned i;
  const uint32_t *p = palette_rgb[palette_id];
  for (i = 0; i < 12; i++)
    tgb_dmg_palette[i] = BGR555(p[i & 3]);
  gb_invalidate_palette(0);
}

void gbcore_set_palette(unsigned palette)
{
  image_keep();
  palette_id = palette < GBCORE_PALETTE_COUNT ? palette : GBCORE_PALETTE_AUTO;
  palette_apply();
}

const char *gbcore_palette_name(unsigned palette)
{
  return palette < GBCORE_PALETTE_COUNT ? palette_names[palette] : "?";
}

void gbcore_set_color_correction(int on)
{
  tgb_cgb_lcd = on ? 1 : 0;
  /* Every CGB entry is reconverted before the next line is drawn. */
  gb_invalidate_all_colors();
}

int gbcore_color_correction(void)
{
  return tgb_cgb_lcd;
}

/* ---------------------------------------------------------- wall clock -- */

void gbcore_set_wallclock(time_t (*wallclock)(void))
{
  image_keep();
  gbcore_wallclock = wallclock;
}

static int64_t wall_now(void)
{
  return (int64_t)(gbcore_wallclock ? gbcore_wallclock() : time(NULL));
}

/* MBC3 clock.  MasterBoy kept a bare offset from time(); this adds the halt
 * and day-carry bits the cartridge has, which Pokemon Gold/Silver/Crystal
 * use when the player sets the clock.  Running: count = now - base.
 * Halted: count is frozen in halted_count. */
static struct {
  int64_t base;
  int64_t halted_count;
  int halted;
  int carry;
} rtc;

static int64_t rtc_count(void)
{
  int64_t now = wall_now();
  int64_t c = rtc.halted ? rtc.halted_count : now - rtc.base;
  if (c < 0)
  {
    /* The wall clock went backwards (the user changed it): hold at zero
     * rather than report a negative time. */
    c = 0;
    if (!rtc.halted)
      rtc.base = now;
  }
  if (c >= RTC_SPAN)
  {
    rtc.carry = 1;
    c %= RTC_SPAN;
    if (rtc.halted)
      rtc.halted_count = c;
    else
      rtc.base = now - c;
  }
  return c;
}

static void rtc_set_count(int64_t c)
{
  if (rtc.halted)
    rtc.halted_count = c;
  else
    rtc.base = wall_now() - c;
}

static void rtc_reset(void)
{
  rtc.halted = 0;
  rtc.carry = 0;
  rtc.halted_count = 0;
  rtc.base = wall_now();
}

static byte rtc_reg(int64_t c, int type)
{
  int64_t days = c / 86400;
  switch (type)
  {
    case 8:  return (byte)(c % 60);
    case 9:  return (byte)((c / 60) % 60);
    case 10: return (byte)((c / 3600) % 24);
    case 11: return (byte)(days & 0xff);
    case 12: return (byte)(((days >> 8) & 1) | (rtc.halted ? 0x40 : 0) |
                           (rtc.carry ? 0x80 : 0));
  }
  return 0;
}

byte gbe_getTime(int type)
{
  return rtc_reg(rtc_count(), type);
}

void gbe_setTime(int type, byte dat)
{
  int64_t c = rtc_count();
  int64_t s = c % 60, m = (c / 60) % 60, h = (c / 3600) % 24, d = c / 86400;
  switch (type)
  {
    case 8:  s = dat % 60; break;
    case 9:  m = dat % 60; break;
    case 10: h = dat % 24; break;
    case 11: d = (d & 0x100) | dat; break;
    case 12:
      d = (d & 0xff) | ((int64_t)(dat & 1) << 8);
      rtc.carry = (dat >> 7) & 1;
      break;
    default: return;
  }
  c = d * 86400 + h * 3600 + m * 60 + s;
  if (type == 12 && ((dat >> 6) & 1) != rtc.halted)
  {
    if (dat & 0x40)
    {
      rtc.halted = 1;
      rtc.halted_count = c;
      return;
    }
    rtc.halted = 0;
  }
  rtc_set_count(c);
}

/* ------------------------------------------- remaining MasterBoy glue -- */

void gbe_init(void)
{
  gbe_reset();
}

void gbe_reset(void)
{
  pad_state = 0;
}

word gbe_getSensor(char x_y)
{
  (void)x_y;
  return 2047;   /* MBC7 tilt sensor at rest (Kirby Tilt 'n' Tumble) */
}

void gbe_setBibrate(char bibrate)
{
  (void)bibrate; /* no rumble */
}

/* Hardware model for the loaded cartridge.  FE_CONSOLE_GBC is CGB hardware:
 * colour games in colour, DMG games with the DMG palette.  FE_CONSOLE_GB is
 * DMG hardware, upgraded to Super Game Boy (colour palettes, no border) for
 * SGB-aware games when the palette is AUTO. */
void set_gb_type(void)
{
  struct rom_info *ri = rom_get_info();
  if (!active || !rom_get_loaded())
    return;
  if (active->console == FE_CONSOLE_GBC)
  {
    ri->gb_type = 3;
    now_gb_mode = org_gbtype == 3 ? 3 : 1;
    lcd_set_mpal(PAL_STANDARD);
  }
  else if (sgb_mode && palette_id == GBCORE_PALETTE_AUTO)
  {
    ri->gb_type = 2;
    now_gb_mode = 2;
    lcd_set_mpal(PAL_SGB);
  }
  else
  {
    ri->gb_type = 1;
    now_gb_mode = 1;
    lcd_set_mpal(PAL_STANDARD);
  }
}

/* An internal-clock transfer completes (cpu_exec).  The in-memory cable
 * decides the byte now; otherwise the one chosen at the SC write stands. */
byte tgb_port_serial_complete(byte outgoing, byte fallback)
{
  uint8_t in = 0xFF;
  if (!active || !active->callbacks.serial_clocked)
    return fallback;
  if (!active->callbacks.serial_clocked(active->callbacks.userdata, outgoing,
                                        &in))
    return fallback;
  return in;
}

uint8_t gbcore_serial_clock_in(gbcore_t *core, uint8_t master_byte)
{
  if (!core || core != active)
    return 0xFF;
  return cpu_seri_send(master_byte);
}

void gbcore_serial_deliver(gbcore_t *core, uint8_t master_byte)
{
  if (!core || core != active)
    return;
  g_regs.SB = master_byte;
  g_regs.SC &= 0x7f;
  seri_occer = 0x7fffffff;
  cpu_irq(INT_SERIAL);
}

uint8_t gbcore_serial_control(gbcore_t *core)
{
  return core && core == active ? g_regs.SC : 0;
}

int tgb_port_serial(byte outgoing, int internal_clock, byte *received)
{
  uint8_t in = 0xFF;
  if (!active || !active->callbacks.serial_transfer)
    return 0;
  if (!active->callbacks.serial_transfer(active->callbacks.userdata, outgoing,
                                         internal_clock ? 1 : 0, &in))
    return 0;
  *received = in;
  return 1;
}

/* --------------------------------------------------------------- create -- */

static uint8_t *load_rom(const char *rom_path, const void *rom_data,
                         size_t rom_size, size_t *out_size)
{
  uint8_t *buffer;
  size_t size = rom_size, alloc = GBCORE_ROM_MIN_ALLOC, need;
  unsigned code;
  FILE *f = NULL;

  if (rom_path)
  {
    long length = -1;
    f = fopen(rom_path, "rb");
    if (!f)
      return NULL;
    if (fseek(f, 0, SEEK_END) == 0)
      length = ftell(f);
    if (length < 0x150 || (unsigned long)length > GBCORE_ROM_MAX ||
        fseek(f, 0, SEEK_SET) != 0)
    {
      fclose(f);
      return NULL;
    }
    size = (size_t)length;
  }
  else if (size < 0x150 || size > GBCORE_ROM_MAX)
    return NULL;

  /* Room for every bank the header's size code can select, and for the
   * whole file, rounded up to a power of two (the core masks with it). */
  need = 0x8000;
  while (need < size)
    need <<= 1;
  if (need > alloc)
    alloc = need;
  buffer = (uint8_t *)malloc(alloc);
  if (!buffer)
  {
    if (f) fclose(f);
    return NULL;
  }
  memset(buffer, 0xFF, alloc);
  if (f)
  {
    size_t n = fread(buffer, 1, size, f);
    fclose(f);
    if (n != size)
    {
      free(buffer);
      return NULL;
    }
  }
  else
    memcpy(buffer, rom_data, size);

  code = buffer[0x148];
  if (code <= 8 && (0x8000u << code) > alloc)
  {
    /* Header claims more than the file holds (and more than the minimum). */
    uint8_t *bigger = (uint8_t *)realloc(buffer, 0x8000u << code);
    if (!bigger)
    {
      free(buffer);
      return NULL;
    }
    memset(bigger + alloc, 0xFF, (0x8000u << code) - alloc);
    buffer = bigger;
  }
  *out_size = size;
  return buffer;
}

/* The buffer load_rom would build for a ROM of `rom_size` bytes, 0xFF
 * filled, for a caller that fills it itself (a ROM received over a link)
 * and hands it to gbcore_create_owned: no second copy. */
uint8_t *gbcore_rom_alloc(size_t rom_size, size_t *alloc_out)
{
  size_t need = 0x8000, alloc = GBCORE_ROM_MIN_ALLOC;
  uint8_t *b;
  if (rom_size < 0x150 || rom_size > GBCORE_ROM_MAX)
    return NULL;
  while (need < rom_size)
    need <<= 1;
  if (need > alloc)
    alloc = need;
  b = (uint8_t *)malloc(alloc);
  if (b)
    memset(b, 0xFF, alloc);
  if (alloc_out)
    *alloc_out = alloc;
  return b;
}

static gbcore_t *create_common(const char *rom_path, const void *rom_data,
                               size_t rom_size, uint8_t *owned,
                               fe_console_t console, unsigned audio_rate,
                               const gbcore_callbacks_t *callbacks);

gbcore_t *gbcore_create(const char *rom_path,
                        const void *rom_data,
                        size_t rom_size,
                        fe_console_t console,
                        unsigned audio_rate,
                        const gbcore_callbacks_t *callbacks)
{
  if ((rom_path != NULL) == (rom_data != NULL) ||
      (rom_path != NULL && rom_size != 0))
    return NULL;
  return create_common(rom_path, rom_data, rom_size, NULL, console,
                       audio_rate, callbacks);
}

gbcore_t *gbcore_create_owned(uint8_t *rom_buffer, size_t rom_size,
                              fe_console_t console, unsigned audio_rate,
                              const gbcore_callbacks_t *callbacks)
{
  if (!rom_buffer)
    return NULL;
  if (rom_size < 0x150 || rom_size > GBCORE_ROM_MAX)
  {
    free(rom_buffer);
    return NULL;
  }
  return create_common(NULL, NULL, rom_size, rom_buffer, console, audio_rate,
                       callbacks);
}

static gbcore_t *create_common(const char *rom_path, const void *rom_data,
                               size_t rom_size, uint8_t *owned,
                               fe_console_t console, unsigned audio_rate,
                               const gbcore_callbacks_t *callbacks)
{
  gbcore_t *core;
  size_t size = 0;

  image_keep();
  if (active ||
      (console != FE_CONSOLE_GB && console != FE_CONSOLE_GBC) ||
      audio_rate < 8000 || audio_rate > 192000)
  {
    free(owned);
    return NULL;
  }

  core = (gbcore_t *)calloc(1, sizeof(*core));
  if (!core)
  {
    free(owned);
    return NULL;
  }
  if (callbacks)
    core->callbacks = *callbacks;
  core->console = console;
  core->sample_rate = audio_rate;
  /* Up to two frames' worth: a frame that re-synchronises with an LCD
   * switch-on can run past 154 lines (gbcore_run_frame). */
  core->audio_capacity = (size_t)audio_rate / 29 + 64;
  core->audio = (int16_t *)malloc(core->audio_capacity * 2 * sizeof(int16_t));
  core->sram = (uint8_t *)malloc(GBCORE_SRAM_ALLOC);
  if (owned)
  {
    /* The buffer came from gbcore_rom_alloc(rom_size): grow it exactly as
     * load_rom does when the header claims more than the file holds. */
    unsigned code = owned[0x148];
    size_t need = 0x8000, alloc = GBCORE_ROM_MIN_ALLOC;
    while (need < rom_size)
      need <<= 1;
    if (need > alloc)
      alloc = need;
    size = rom_size;
    core->rom = owned;
    if (code <= 8 && (0x8000u << code) > alloc)
    {
      uint8_t *bigger = (uint8_t *)realloc(owned, 0x8000u << code);
      if (!bigger)
      {
        free(owned);
        core->rom = NULL;
      }
      else
      {
        memset(bigger + alloc, 0xFF, (0x8000u << code) - alloc);
        core->rom = bigger;
      }
    }
  }
  else
    core->rom = load_rom(rom_path, rom_data, rom_size, &size);
  vframe = (word *)malloc(VFRAME_SIZE);
  snd_write_que = (struct apu_que *)malloc(SND_QUE_SIZE *
                                           sizeof(struct apu_que));
  if (!core->audio || !core->sram || !core->rom || !vframe || !snd_write_que)
    goto fail;
  /* Blank cartridge RAM reads 0xFF, as the SameBoy core this replaced left
   * it: a game's "no save" detection sees the same thing it did before. */
  memset(core->sram, 0xFF, GBCORE_SRAM_ALLOC);
  memcpy(core->header, core->rom + 0x134, sizeof(core->header));

  active = core;
  snd_rate = (int)audio_rate;
  palette_apply();
  rtc_reset();
  gb_init();
  if (!rom_load_rom(core->rom, (int)size, core->sram, 0))
    goto fail;
  gb_reset();
  return core;

fail:
  gbcore_shutdown(core);
  return NULL;
}

void gbcore_shutdown(gbcore_t *core)
{
  if (!core)
    return;
  if (core == active)
  {
    active = NULL;
    free(vframe);
    vframe = NULL;
    free(snd_write_que);
    snd_write_que = NULL;
    snd_que_count = 0;
  }
  else if (!active)
  {
    /* A create that failed before it became active owns these. */
    free(vframe);
    vframe = NULL;
    free(snd_write_que);
    snd_write_que = NULL;
  }
  free(core->rom);
  free(core->sram);
  free(core->audio);
  free(core);
}

/* ------------------------------------------------------------ run frame -- */

static int pad_bits(uint16_t buttons)
{
  int p = 0;
  if (buttons & GBCORE_BUTTON_A)      p |= 1;
  if (buttons & GBCORE_BUTTON_B)      p |= 2;
  if (buttons & GBCORE_BUTTON_SELECT) p |= 4;
  if (buttons & GBCORE_BUTTON_START)  p |= 8;
  if (buttons & GBCORE_BUTTON_DOWN)   p |= 16;
  if (buttons & GBCORE_BUTTON_UP)     p |= 32;
  if (buttons & GBCORE_BUTTON_LEFT)   p |= 64;
  if (buttons & GBCORE_BUTTON_RIGHT)  p |= 128;
  return p;
}

/* The core's clocks are ints that only grow: total_clock wraps after ~8.5
 * minutes of single-speed play, and the serial deadline and sound queue
 * compare against it.  At a frame boundary the sound queue is empty, so
 * shifting everything back to zero keeps every comparison in range. */
static void rebase_clocks(void)
{
  int base = total_clock;
  total_clock = 0;
  snd_bef_clock -= base;
  if (seri_occer != 0x7fffffff)
    seri_occer -= base;
}

void gbcore_set_skip_render(gbcore_t *core, int skip)
{
  if (core)
    core->skip_render = skip ? 1 : 0;
}

void gbcore_set_headless(gbcore_t *core, int headless)
{
  if (!core || core != active)
    return;
  headless = headless ? 1 : 0;
  if (headless == core->headless)
    return;
  if (headless)
  {
    /* Nothing draws into it again (every lcd_render and fill is under
     * gbSkip or checks for NULL); 128 KiB back to the heap. */
    free(vframe);
    vframe = NULL;
  }
  else if (!vframe)
  {
    vframe = (word *)malloc(VFRAME_SIZE);
    if (!vframe)
      return;                 /* stays headless */
    memset(vframe, 0, VFRAME_SIZE);
  }
  core->headless = headless;
  gbSkip = core->skip_render || headless;
}

int gbcore_frame_begin(gbcore_t *core, uint16_t buttons)
{
  uint16_t pressed;

  if (!core || core != active)
    return -1;

  pad_state = pad_bits(buttons);
  /* The joypad interrupt, which the core never raised: a newly pressed
   * button is how a game wakes from STOP. */
  pressed = (uint16_t)(buttons & ~core->prev_buttons);
  core->prev_buttons = buttons;
  if (pressed)
    cpu_irq(INT_PAD);

  gbSkip = core->skip_render || core->headless;
  core->lines = 0;
  return 0;
}

static void frame_end(gbcore_t *core)
{
  size_t samples;

  core->audio_acc += (uint64_t)core->lines * GB_LINE_CLOCKS *
                     core->sample_rate;
  samples = (size_t)(core->audio_acc / GB_CLOCK_HZ);
  core->audio_acc %= GB_CLOCK_HZ;
  if (samples > core->audio_capacity)
    samples = core->audio_capacity;
  if (core->headless)
  {
    /* Drop the frame's sound-register writes unheard.  They changed the
     * register side of the APU when they were made (apu_write ->
     * snd_process); the queue only feeds the synthesiser, whose copy of
     * the state nothing in the game can read. */
    snd_que_count = 0;
    snd_bef_clock = total_clock;
    samples = 0;
  }
  else if (samples)
    snd_render_orig(core->audio, (int)samples);
  rebase_clocks();
  core->frame_number++;

  if (core->headless)
    return;
  if (core->callbacks.video)
  {
    gbcore_video_frame_t frame;
    frame.pixels = vframe + GUARD_LINE;
    frame.width = 160;
    frame.height = 144;
    frame.pitch_bytes = SIZE_LINE * sizeof(word);
    core->callbacks.video(core->callbacks.userdata, &frame);
  }
  if (core->callbacks.audio_batch && samples)
    core->callbacks.audio_batch(core->callbacks.userdata, core->audio,
                                samples);
}

/* A frame ends when VBlank starts, so the image is lines 0-143 of one
 * frame (MasterBoy's loop ended at LY 0, after drawing the next frame's
 * first line).  With the LCD off there is no VBlank: 154 lines. */
int gbcore_run_line(gbcore_t *core)
{
  if (!core || core != active)
    return -1;
  gb_run();
  core->lines++;
  if (g_regs.LCDC & 0x80)
  {
    if (g_regs.LY != 144 && core->lines < 2 * GB_FRAME_LINES)
      return 0;
  }
  else if (core->lines < GB_FRAME_LINES)
    return 0;
  frame_end(core);
  return 1;
}

int gbcore_run_frame(gbcore_t *core, uint16_t buttons)
{
  int r;
  if (gbcore_frame_begin(core, buttons) != 0)
    return -1;
  while ((r = gbcore_run_line(core)) == 0)
    ;
  return r < 0 ? -1 : 0;
}

unsigned gbcore_sample_rate(const gbcore_t *core)
{
  return core ? core->sample_rate : 0;
}

unsigned gbcore_frame_width(const gbcore_t *core)
{
  return core ? 160 : 0;
}

unsigned gbcore_frame_height(const gbcore_t *core)
{
  return core ? 144 : 0;
}

uint64_t gbcore_frame_number(const gbcore_t *core)
{
  return core ? core->frame_number : 0;
}

/* The header title, 0x134-0x143, printable ASCII up to the first other
 * byte.  Newer cartridges end the field with the CGB flag (0x80/0xC0). */
int gbcore_rom_title(gbcore_t *core, char *destination, size_t capacity)
{
  size_t i;
  if (!core || !destination || capacity < 17)
    return -1;
  for (i = 0; i < 16; i++)
  {
    uint8_t c = core->header[i];
    if (c < 0x20 || c > 0x7E)
      break;
    destination[i] = (char)c;
  }
  destination[i] = '\0';
  while (i > 0 && destination[i - 1] == ' ')
    destination[--i] = '\0';
  return 0;
}

/* -------------------------------------------------------- battery save -- */

static int cart_has_rtc(void)
{
  int t = rom_get_info()->cart_type;
  return t == 0x0F || t == 0x10;          /* MBC3+TIMER(+RAM)+BATTERY */
}

/* Sized exactly as SameBoy sized it (mbc.c GB_configure_cart), so the
 * battery files the previous GB core wrote line up byte for byte. */
size_t gbcore_cart_ram_size(gbcore_t *core)
{
  static const size_t sizes[6] = { 0, 0x800, 0x2000, 0x8000, 0x20000,
                                   0x10000 };
  int t;
  if (!core || core != active)
    return 0;
  t = rom_get_info()->cart_type;
  if (t == 0x05 || t == 0x06)
    return 0x200;                         /* MBC2: 512 x 4 bits on chip */
  if (t == 0x22)
    return 0x100;                         /* MBC7 EEPROM */
  return rom_get_info()->ram_size <= 5 ? sizes[rom_get_info()->ram_size] : 0;
}

size_t gbcore_save_ram_size(gbcore_t *core)
{
  size_t ram = gbcore_cart_ram_size(core);
  int clock;
  if (!core || core != active || !rom_has_battery())
    return 0;
  clock = cart_has_rtc();
  if (!ram && !clock)
    return 0;
  return ram + (clock ? RTC_TRAILER_48 : 0);
}

static void put_le32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

int gbcore_save_ram_read(gbcore_t *core, void *destination, size_t capacity)
{
  size_t required = gbcore_save_ram_size(core);
  size_t ram = gbcore_cart_ram_size(core);
  uint8_t *out = (uint8_t *)destination;
  if (!required)
    return 0;
  if (!destination || capacity < required)
    return -1;
  memcpy(out, core->sram, ram);
  if (cart_has_rtc())
  {
    /* VBA-M / BGB 48-byte layout: live registers, latched registers,
     * 64-bit little-endian UNIX time of this snapshot. */
    int64_t now = wall_now();
    int64_t c = rtc_count();
    uint8_t *t = out + ram;
    int i;
    for (i = 0; i < 5; i++)
      put_le32(t + i * 4, rtc_reg(c, 8 + i));
    put_le32(t + 20, mbc3_sec);
    put_le32(t + 24, mbc3_min);
    put_le32(t + 28, mbc3_hour);
    put_le32(t + 32, mbc3_dayl);
    put_le32(t + 36, mbc3_dayh);
    put_le32(t + 40, (uint32_t)((uint64_t)now & 0xffffffffu));
    put_le32(t + 44, (uint32_t)((uint64_t)now >> 32));
  }
  return 0;
}

int gbcore_save_ram_write(gbcore_t *core, const void *source, size_t size)
{
  size_t ram = gbcore_cart_ram_size(core);
  const uint8_t *in = (const uint8_t *)source;
  if (!core || core != active || (!source && size))
    return -1;
  if (!size)
    return 0;
  memcpy(core->sram, in, size < ram ? size : ram);
  if (!cart_has_rtc())
    return 0;
  rtc_reset();
  if (size == ram + RTC_TRAILER_48 || size == ram + RTC_TRAILER_44)
  {
    const uint8_t *t = in + ram;
    int64_t stamp = get_le32(t + 40);
    int64_t now = wall_now();
    int64_t c;
    byte high = (byte)get_le32(t + 16);
    if (size == ram + RTC_TRAILER_48)
      stamp |= (int64_t)get_le32(t + 44) << 32;
    if (stamp > now || stamp < RTC_EPOCH_MIN)
      return 0;                           /* not real clock data: reset */
    c = (int64_t)(get_le32(t) % 60) + (int64_t)(get_le32(t + 4) % 60) * 60 +
        (int64_t)(get_le32(t + 8) % 24) * 3600 +
        ((int64_t)(get_le32(t + 12) & 0xff) | ((int64_t)(high & 1) << 8)) *
           86400;
    rtc.carry = (high >> 7) & 1;
    if (high & 0x40)
    {
      rtc.halted = 1;
      rtc.halted_count = c;
    }
    else
    {
      c += now - stamp;                   /* the time the PSP was off */
      if (c >= RTC_SPAN)
      {
        rtc.carry = 1;
        c %= RTC_SPAN;
      }
      rtc.base = now - c;
    }
    mbc3_sec = (byte)get_le32(t + 20);
    mbc3_min = (byte)get_le32(t + 24);
    mbc3_hour = (byte)get_le32(t + 28);
    mbc3_dayl = (byte)get_le32(t + 32);
    mbc3_dayh = (byte)get_le32(t + 36);
  }
  return 0;
}

/* ----------------------------------------------------------- savestate -- */

/* Layout, all integers little-endian:
 *   0  "GBAT"                 magic
 *   4  u16 version            STATE_VERSION
 *   6  u8  console            fe_console_t the state was made on
 *   7  u8  gb_type            core hardware mode (1 DMG, 2 SGB, 3 CGB)
 *   8  0x1C bytes             ROM 0x134-0x14F (title, flags, checksums)
 *  36  u32 core_len           length of TGB Dual's own block
 *  40  core block             gb_save_state()
 *  ..  STATE_EXT bytes        what gb_save_state omits (below)
 * The extension carries the CPU clock phase, interrupt pipeline, HDMA
 * stall, window line and serial deadline, which TGB's own format leaves
 * out; without them a loaded state resumes a few cycles off. */

typedef struct {
  uint8_t *p;
  const uint8_t *q;
} cursor_t;

static void w32(cursor_t *c, int32_t v)
{
  put_le32(c->p, (uint32_t)v);
  c->p += 4;
}

static int32_t r32(cursor_t *c)
{
  int32_t v = (int32_t)get_le32(c->q);
  c->q += 4;
  return v;
}

static void ext_save(uint8_t *p)
{
  cursor_t c;
  c.p = p;
  memset(p, 0, STATE_EXT);
  w32(&c, div_clock);
  w32(&c, rest_clock);
  w32(&c, sys_clock);
  w32(&c, total_clock);
  w32(&c, seri_occer);
  w32(&c, gdma_rest);
  w32(&c, last_int);
  w32(&c, now_win_line);
  w32(&c, re_render);
  *c.p++ = seri_rx;
  *c.p++ = (uint8_t)int_disable_next;
  *c.p++ = _ff6c;
  *c.p++ = _ff72;
  *c.p++ = _ff73;
  *c.p++ = _ff74;
  *c.p++ = _ff75;
  *c.p++ = (uint8_t)(rtc.halted | (rtc.carry << 1));
  memcpy(c.p, ext_mem, 16);
  c.p += 16;
  memcpy(c.p, spare_oam, 0x18);
}

static void ext_load(const uint8_t *q)
{
  cursor_t c;
  c.q = q;
  div_clock = r32(&c);
  rest_clock = r32(&c);
  sys_clock = r32(&c);
  total_clock = r32(&c);
  seri_occer = r32(&c);
  gdma_rest = r32(&c);
  last_int = r32(&c);
  now_win_line = r32(&c);
  re_render = r32(&c);
  seri_rx = *c.q++;
  int_disable_next = (char)*c.q++;
  _ff6c = *c.q++;
  _ff72 = *c.q++;
  _ff73 = *c.q++;
  _ff74 = *c.q++;
  _ff75 = *c.q++;
  c.q++;                /* RTC flags: the clock follows the wall, not states */
  memcpy(ext_mem, c.q, 16);
  c.q += 16;
  memcpy(spare_oam, c.q, 0x18);
}

size_t gbcore_state_size(gbcore_t *core)
{
  if (!core || core != active)
    return 0;
  return STATE_HEADER + gb_save_state(NULL) + STATE_EXT;
}

long gbcore_state_save(gbcore_t *core, void *destination, size_t capacity)
{
  size_t need = gbcore_state_size(core), core_len;
  uint8_t *out = (uint8_t *)destination;
  if (!need || !destination || capacity < need)
    return -1;
  core_len = need - STATE_HEADER - STATE_EXT;
  memset(out, 0, STATE_HEADER);
  memcpy(out, STATE_MAGIC, 4);
  out[4] = (uint8_t)STATE_VERSION;
  out[5] = (uint8_t)(STATE_VERSION >> 8);
  out[6] = (uint8_t)core->console;
  out[7] = (uint8_t)rom_get_info()->gb_type;
  memcpy(out + 8, core->header, sizeof(core->header));
  put_le32(out + 36, (uint32_t)core_len);
  if (gb_save_state(out + STATE_HEADER) != core_len)
    return -1;
  ext_save(out + STATE_HEADER + core_len);
  return (long)need;
}

/* Put the machine in hardware model `type` (1 DMG, 2 SGB) exactly as
 * set_gb_type() would have at power-on. */
static void gb_model_apply(int type)
{
  rom_get_info()->gb_type = type;
  now_gb_mode = type;
  lcd_set_mpal(type == 2 ? PAL_SGB : PAL_STANDARD);
}

int gbcore_state_load(gbcore_t *core, const void *source, size_t size)
{
  const uint8_t *in = (const uint8_t *)source;
  size_t core_len;
  int model, saved_model;
  if (!core || core != active || !source || size < STATE_HEADER + STATE_EXT)
    return -1;
  if (memcmp(in, STATE_MAGIC, 4) != 0 ||
      (in[4] | (in[5] << 8)) != STATE_VERSION ||
      in[6] != (uint8_t)core->console ||
      memcmp(in + 8, core->header, sizeof(core->header)) != 0)
    return -1;
  /* The first int of TGB's block is the hardware mode it restores, and the
   * header repeats it; they must agree. */
  saved_model = (int)get_le32(in + STATE_HEADER);
  if (in[7] != (uint8_t)saved_model)
    return -1;
  /* A STATE IS A WHOLE MACHINE, MODEL INCLUDED.  On FE_CONSOLE_GB the model
   * is DMG or Super Game Boy, and set_gb_type() picks it at power-on from the
   * DMG palette setting: Auto boots an SGB-aware cart as an SGB, any explicit
   * palette boots it as a DMG.  So changing the palette (a display setting)
   * made every earlier state of such a game unloadable -- the browser shelf
   * then quit to the XMB, and the menu said "No state to load".  The game
   * code saved in the state has already detected the hardware it was booted
   * on, so the only correct restore is onto that same hardware: adopt the
   * saved model (the next boot goes back to what the palette chooses).  Only
   * DMG <-> SGB, and SGB only for a cart that declares SGB support; CGB
   * states never meet a DMG/SGB core (the console byte above). */
  model = rom_get_info()->gb_type;
  if (saved_model != model)
  {
    if (core->console != FE_CONSOLE_GB ||
        (saved_model != 1 && saved_model != 2) ||
        (model != 1 && model != 2) ||
        (saved_model == 2 && !sgb_mode))
      return -1;
    gb_model_apply(saved_model);
  }
  core_len = gb_save_state(NULL);
  if (get_le32(in + 36) != core_len ||
      size != STATE_HEADER + core_len + STATE_EXT)
  {
    if (saved_model != model)
      gb_model_apply(model);
    return -1;
  }
  if (gb_restore_state(in + STATE_HEADER, core_len) != 0)
    return -1;
  ext_load(in + STATE_HEADER + core_len);
  /* The sound queue belongs to the frame the state interrupted, not to the
   * one it restores; the volume latch must be re-read from NR50. */
  snd_que_count = 0;
  snd_bef_clock = total_clock;
  snd_stat_upd = 1;
  rebase_clocks();
  cpu_irq_check();
  return 0;
}

int gbcore_power_on(void)
{
  image_keep();
  if (active || image.failed)
    return -1;
  image_copy((volatile uint8_t *)tgb_image_data_start, image.data,
             image.size);
  image_zero((volatile uint8_t *)tgb_image_bss_start,
             (size_t)(tgb_image_bss_end - tgb_image_bss_start));
  return 0;
}

int gbcore_peek(gbcore_t *core, uint16_t addr, void *out, unsigned len)
{
  uint8_t *o = (uint8_t *)out;
  unsigned i;
  if (!core || core != active || !out)
    return -1;
  for (i = 0; i < len; i++)
  {
    unsigned a = (unsigned)addr + i;
    if (a > 0xFFFE)
      return -1;
    if (a < 0x4000)
      o[i] = get_rom()[a];
    else if (a < 0x8000)
      o[i] = mbc_get_rom()[a];
    else if (a < 0xA000)
      o[i] = vram_bank[a & 0x1FFF];
    else if (a < 0xC000)
    {
      if (!mbc_is_ext_ram())
        return -1;
      o[i] = mbc_get_sram()[a & 0x1FFF];
    }
    else if (a < 0xFE00)
      o[i] = (a & 0x1000) ? ram_bank[a & 0x0FFF] : cpu_get_ram()[a & 0x0FFF];
    else if (a < 0xFEA0)
      o[i] = cpu_get_oam()[a - 0xFE00];
    else if (a >= 0xFF80)
      o[i] = cpu_get_stack()[a - 0xFF80];
    else
      return -1;
  }
  return 0;
}

static uint8_t *poke_ptr(unsigned a)
{
  if (a >= 0x8000 && a < 0xA000)
    return &vram_bank[a & 0x1FFF];
  if (a >= 0xA000 && a < 0xC000)
    return mbc_is_ext_ram() ? &mbc_get_sram()[a & 0x1FFF] : NULL;
  if (a >= 0xC000 && a < 0xFE00)
    return (a & 0x1000) ? &ram_bank[a & 0x0FFF] : &cpu_get_ram()[a & 0x0FFF];
  if (a >= 0xFE00 && a < 0xFEA0)
    return &cpu_get_oam()[a - 0xFE00];
  if (a >= 0xFF80 && a < 0xFFFF)
    return &cpu_get_stack()[a - 0xFF80];
  return NULL;
}

int gbcore_poke(gbcore_t *core, uint16_t addr, const void *data, unsigned len)
{
  const uint8_t *d = (const uint8_t *)data;
  unsigned i;
  if (!core || core != active || !data)
    return -1;
  for (i = 0; i < len; i++)
    if (!poke_ptr((unsigned)addr + i))
      return -1;
  for (i = 0; i < len; i++)
    *poke_ptr((unsigned)addr + i) = d[i];
  return 0;
}

/* ----------------------------------------------------------- sync hash -- */

/* FNV-1a, 64-bit, over bytes: portable and endian-independent, and cheap
 * enough at the rate a link session checks (a few hundred KiB a second). */
#define SYNC_FNV_OFFSET 0xcbf29ce484222325ull
#define SYNC_FNV_PRIME  0x100000001b3ull

static uint64_t sync_bytes(uint64_t h, const void *data, size_t size)
{
  const uint8_t *p = (const uint8_t *)data;
  size_t i;
  for (i = 0; i < size; i++)
  {
    h ^= p[i];
    h *= SYNC_FNV_PRIME;
  }
  return h;
}

static uint64_t sync_int(uint64_t h, int64_t v)
{
  uint8_t b[8];
  unsigned i;
  for (i = 0; i < 8; i++)
    b[i] = (uint8_t)((uint64_t)v >> (8 * i));
  return sync_bytes(h, b, sizeof(b));
}

/* What goes in: everything gb_save_state and ext_save carry that the game
 * can observe or that decides its next instruction -- memory, registers,
 * clocks, DMA, the serial port, the cartridge (RAM, banks, MBC3 clock),
 * the register side of the APU, SGB packet state -- plus the adapter's
 * input latch and clock.  What stays out: the synthesiser's copy of the
 * APU (apu_get_stat_gen, advanced only when sound is rendered) and the
 * renderer's window-line counter (now_win_line), both of which differ
 * between a drawn and a headless machine and neither of which the game
 * can read. */
uint64_t gbcore_sync_hash(gbcore_t *core)
{
  static const int tbl_ram[] = { 1, 1, 1, 4, 16, 8 };
  struct rom_info *ri;
  struct cpu_regs *r;
  uint64_t h = SYNC_FNV_OFFSET;
  int cpu_dat[16];
  int banks;

  if (!core || core != active)
    return 0;
  ri = rom_get_info();
  h = sync_int(h, ri->gb_type);
  h = sync_int(h, now_gb_mode);
  h = sync_int(h, sgb_mode);
  h = sync_bytes(h, cpu_get_ram(), ri->gb_type >= 3 ? 0x2000 * 4 : 0x2000);
  h = sync_bytes(h, cpu_get_vram(), ri->gb_type >= 3 ? 0x2000 * 2 : 0x2000);
  h = sync_bytes(h, cpu_get_oam(), 0xA0);
  h = sync_bytes(h, cpu_get_stack(), 0x80);
  h = sync_bytes(h, spare_oam, 0x18);
  h = sync_bytes(h, ext_mem, 16);
  banks = ri->ram_size <= 5 ? tbl_ram[ri->ram_size] : 1;
  h = sync_bytes(h, get_sram(), (size_t)banks * 0x2000);

  r = cpu_get_c_regs();
  h = sync_int(h, r->AF.w);
  h = sync_int(h, r->BC.w);
  h = sync_int(h, r->DE.w);
  h = sync_int(h, r->HL.w);
  h = sync_int(h, r->SP);
  h = sync_int(h, r->PC);
  h = sync_int(h, r->I);
  h = sync_bytes(h, &g_regs, sizeof(g_regs));
  h = sync_bytes(h, &cg_regs, sizeof(cg_regs));
  h = sync_bytes(h, lcd_get_pal(0), sizeof(word) * 16 * 4);

  memset(cpu_dat, 0, sizeof(cpu_dat));
  cpu_save_state(cpu_dat);                /* banks, speed, HDMA */
  cpu_save_state_ex(cpu_dat + 8);         /* div/rest/sys/total clocks */
  h = sync_bytes(h, cpu_dat, sizeof(cpu_dat));
  h = sync_int(h, halt);
  h = sync_int(h, b_dma_first);
  h = sync_int(h, gdma_rest);
  h = sync_int(h, last_int);
  h = sync_int(h, int_disable_next);
  h = sync_int(h, int_invoke_next);
  h = sync_int(h, seri_occer);
  h = sync_int(h, seri_rx);
  h = sync_int(h, re_render);
  h = sync_int(h, _ff6c);
  h = sync_int(h, _ff72);
  h = sync_int(h, _ff73);
  h = sync_int(h, _ff74);
  h = sync_int(h, _ff75);

  h = sync_int(h, mbc_get_state());
  h = sync_int(h, (int64_t)(mbc_get_rom() - get_rom()));
  h = sync_int(h, (int64_t)(mbc_get_sram() - get_sram()));
  h = sync_int(h, mbc_is_ext_ram());
  h = sync_int(h, mbc3_latch);
  h = sync_int(h, mbc3_sec);
  h = sync_int(h, mbc3_min);
  h = sync_int(h, mbc3_hour);
  h = sync_int(h, mbc3_dayl);
  h = sync_int(h, mbc3_dayh);
  h = sync_int(h, mbc3_timer);

  h = sync_bytes(h, apu_get_stat_cpu(), sizeof(struct apu_stat));
  h = sync_bytes(h, apu_get_mem(), 0x30);

  if (now_gb_mode == 2)
  {
    h = sync_int(h, bit_received);
    h = sync_int(h, bits_received);
    h = sync_int(h, packets_received);
    h = sync_int(h, sgb_state);
    h = sync_int(h, sgb_index);
    h = sync_int(h, sgb_multiplayer);
    h = sync_int(h, sgb_fourplayers);
    h = sync_int(h, sgb_nextcontrol);
    h = sync_int(h, sgb_readingcontrol);
    h = sync_int(h, sgb_mask);
    h = sync_bytes(h, sgb_palette, sizeof(unsigned short) * 8 * 16);
    h = sync_bytes(h, sgb_palette_memory, sizeof(unsigned short) * 512 * 4);
    h = sync_bytes(h, sgb_buffer, 7 * 16);
    h = sync_bytes(h, sgb_ATF, 18 * 20);
    h = sync_bytes(h, sgb_ATF_list, 45 * 20 * 18);
  }

  h = sync_int(h, rtc.base);
  h = sync_int(h, rtc.halted_count);
  h = sync_int(h, rtc.halted);
  h = sync_int(h, rtc.carry);
  h = sync_int(h, pad_state);
  h = sync_int(h, core->prev_buttons);
  return h;
}

/* ------------------------------------------------------ instance table -- */

const gbcore_api_t gbcore_api = {
  gbcore_create,
  gbcore_shutdown,
  gbcore_run_frame,
  gbcore_frame_begin,
  gbcore_run_line,
  gbcore_set_skip_render,
  gbcore_set_headless,
  gbcore_sync_hash,
  gbcore_serial_clock_in,
  gbcore_frame_number,
  gbcore_rom_title,
  gbcore_save_ram_size,
  gbcore_cart_ram_size,
  gbcore_save_ram_read,
  gbcore_save_ram_write,
  gbcore_state_size,
  gbcore_state_save,
  gbcore_state_load,
  gbcore_set_wallclock,
  gbcore_set_palette,
  gbcore_power_on,
  gbcore_peek,
  gbcore_poke,
  gbcore_serial_control,
  gbcore_create_owned,
  gbcore_serial_deliver,
};
