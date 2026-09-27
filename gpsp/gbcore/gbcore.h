/* Copyright (c) 2026 GBAdhoc contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Frontend-neutral GB/GBC execution API.  Implementation: TGB Dual (Hii) as
 * ported to the PSP by MasterBoy (Brunni), GPL v2 or later; see
 * gbcore/tgbdual/README.md and gbcore_tgbdual.c.
 *
 * The core is a single global machine: at most one gbcore_t exists at a
 * time (gbcore_create refuses a second).
 */
#ifndef GBADHOC_GBCORE_H
#define GBADHOC_GBCORE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "../frontend-common/fe_console.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gbcore gbcore_t;

enum {
  GBCORE_BUTTON_RIGHT  = 1u << 0,
  GBCORE_BUTTON_LEFT   = 1u << 1,
  GBCORE_BUTTON_UP     = 1u << 2,
  GBCORE_BUTTON_DOWN   = 1u << 3,
  GBCORE_BUTTON_A      = 1u << 4,
  GBCORE_BUTTON_B      = 1u << 5,
  GBCORE_BUTTON_SELECT = 1u << 6,
  GBCORE_BUTTON_START  = 1u << 7
};

typedef struct gbcore_video_frame {
  /* 16-bit pixels in the frontend's RGB565 layout: libretro order (red in
   * the high bits), or the PSP GE's 5650 order when the build defines
   * USE_PSP_RGB565_FORMAT -- the same layout the GBA path hands the
   * frontend, so no conversion pass is needed.  The memory belongs to the
   * core and stays valid, unchanged, until the next gbcore_run_frame or
   * gbcore_shutdown. */
  const uint16_t *pixels;
  unsigned width;
  unsigned height;
  unsigned pitch_bytes;
} gbcore_video_frame_t;

typedef struct gbcore_callbacks {
  void *userdata;
  void (*video)(void *userdata, const gbcore_video_frame_t *frame);
  /* One int16 stereo batch is delivered per emulated video frame. `frames`
   * counts stereo pairs, not scalar samples. */
  void (*audio_batch)(void *userdata, const int16_t *interleaved,
                      size_t frames);
  /* Optional complete-byte cable bridge. It runs synchronously at the SC
   * write that starts a transfer; a frontend may pause emulation while it
   * exchanges one byte. Return 1 and fill `received` when the transfer was
   * handled, or 0 to leave disconnected-cable behaviour in effect (an
   * internal-clock transfer then completes with 0xFF after eight bit times;
   * an external-clock one waits, as on hardware).  For an internal-clock
   * transfer the received byte reaches SB eight bit times later; for an
   * external-clock one it reaches SB, with the serial interrupt, at once. */
  int (*serial_transfer)(void *userdata, uint8_t outgoing, int internal_clock,
                         uint8_t *received);
} gbcore_callbacks_t;

/* Creates and loads a GB or GBC ROM. FE_CONSOLE_GB runs DMG hardware (with
 * Super Game Boy colours for SGB-aware games when the palette is
 * GBCORE_PALETTE_AUTO); FE_CONSOLE_GBC runs CGB hardware. ROM bytes are
 * copied by the core. Exactly one of rom_path and rom_data may be supplied.
 * Returns NULL on failure, including when another core instance exists. */
gbcore_t *gbcore_create(const char *rom_path,
                        const void *rom_data,
                        size_t rom_size,
                        fe_console_t console,
                        unsigned audio_rate,
                        const gbcore_callbacks_t *callbacks);

/* Execute one emulated video frame with the current frontend button mask. */
int gbcore_run_frame(gbcore_t *core, uint16_t buttons);
/* Skip (1) or resume (0) drawing.  A skipped frame is fully emulated and
 * delivers audio, but the video callback receives the previous image
 * (pixels unchanged).  For fast-forward's intermediate frames. */
void gbcore_set_skip_render(gbcore_t *core, int skip);
void gbcore_shutdown(gbcore_t *core);

unsigned gbcore_sample_rate(const gbcore_t *core);
unsigned gbcore_frame_width(const gbcore_t *core);
unsigned gbcore_frame_height(const gbcore_t *core);
uint64_t gbcore_frame_number(const gbcore_t *core);
int gbcore_rom_title(gbcore_t *core, char *destination, size_t capacity);

/* Battery image: cartridge RAM followed, for MBC3 clock carts, by the common
 * 48-byte RTC trailer (VBA-M/BGB/SameBoy: five 32-bit live registers, five
 * latched, 64-bit little-endian UNIX time) stamped with the wall clock at
 * the moment of the read -- so the image changes every second even when the
 * game wrote nothing.  Use gbcore_cart_ram_size() bytes for change
 * detection. */
size_t gbcore_save_ram_size(gbcore_t *core);
size_t gbcore_cart_ram_size(gbcore_t *core);
int gbcore_save_ram_read(gbcore_t *core, void *destination, size_t capacity);
/* Accepts cart RAM alone or cart RAM plus a 44- or 48-byte clock trailer;
 * any other trailer, or one stamped in the future or before 1997, resets
 * the clock (the RAM is kept either way). */
int gbcore_save_ram_write(gbcore_t *core, const void *source, size_t size);

/* Save states.  The image is self-describing (magic, version, console and
 * cartridge identity) and is refused by gbcore_state_load unless it was
 * made by this core for the same cartridge on the same console.
 * gbcore_state_save returns the byte count written or -1. */
size_t gbcore_state_size(gbcore_t *core);
long gbcore_state_save(gbcore_t *core, void *destination, size_t capacity);
int gbcore_state_load(gbcore_t *core, const void *source, size_t size);

/* Wall clock for cartridge RTCs, as seconds since 1970. NULL (the default)
 * means C time(). Required on the PSP, whose time() is roughly uptime: with
 * it every MBC3 save looked like it came from the future or from 1970 and
 * the game's clock was reset on every boot. Set before gbcore_create(). */
void gbcore_set_wallclock(time_t (*wallclock)(void));

/* DMG palette.  AUTO shows Super Game Boy colours for games that support
 * the SGB (decided when the game boots) and GREY otherwise; the others are
 * plain four-shade palettes.  Colour-only changes apply to a running game
 * at once; switching an SGB game in or out of SGB colours takes effect the
 * next time it boots.  Unknown ids fall back to AUTO. */
enum {
  GBCORE_PALETTE_AUTO = 0,
  GBCORE_PALETTE_GREY,
  GBCORE_PALETTE_GREEN,
  GBCORE_PALETTE_POCKET,
  GBCORE_PALETTE_COUNT
};
void gbcore_set_palette(unsigned palette);
const char *gbcore_palette_name(unsigned palette);

#ifdef __cplusplus
}
#endif
#endif
