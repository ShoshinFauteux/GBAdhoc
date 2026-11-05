/* Copyright (c) 2026 GBAdhoc contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Frontend-neutral GB/GBC execution API.  Implementation: TGB Dual (Hii) as
 * ported to the PSP by MasterBoy (Brunni), GPL v2 or later; see
 * gbcore/tgbdual/README.md and gbcore_tgbdual.c.
 *
 * The core is a single global machine: at most one gbcore_t exists at a
 * time per INSTANCE (gbcore_create refuses a second).  The build links a
 * second instance, whose API is the same functions renamed gbcore_* ->
 * gbcoreb_*: one relocatable object, two copies, every other symbol local
 * (psp/Makefile, tools/run_gb_tests.py).  The two instances share no state
 * at all, so a link session runs two Game Boys (gbcore_dual.h).  Code that
 * must work with either instance goes through gbcore_api_t below.
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
  /* Optional in-memory cable (two-instance link sessions).  It runs when an
   * internal-clock transfer COMPLETES -- eight bit times after the SC write
   * -- which is when a real cable has finished shifting the peer's byte in.
   * Return 1 with the peer's byte in `received` (0xFF when the peer is not
   * waiting on the external clock), or 0 for the ordinary completion (the
   * byte serial_transfer supplied, else 0xFF).  The peer side is completed
   * by gbcore_serial_clock_in.  A session that uses this leaves
   * serial_transfer NULL. */
  int (*serial_clocked)(void *userdata, uint8_t outgoing, uint8_t *received);
} gbcore_callbacks_t;

/* Takes the instance's one process-lifetime allocation now: the pristine
 * copy of the core's initialised data that every create restores (a
 * small malloc, otherwise made by the first create).  A frontend that
 * unloads and reloads games in one process calls it at boot, so no game
 * leaves anything behind (docs/INPROC-RETURN.md).  Idempotent. */
void gbcore_prepare(void);

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

/* A ROM buffer as gbcore_create would build it for `rom_size` bytes (0xFF
 * filled, at least 1 MiB, a power of two), for a caller that fills bytes
 * [0, rom_size) itself -- a ROM received over a link -- and then passes it
 * to gbcore_create_owned, which takes ownership (freed at shutdown, or at
 * once if the create fails).  No second copy of the cartridge is made. */
uint8_t *gbcore_rom_alloc(size_t rom_size, size_t *alloc);
gbcore_t *gbcore_create_owned(uint8_t *rom_buffer, size_t rom_size,
                              fe_console_t console, unsigned audio_rate,
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
 * detection.  The stamp is what the wall-clock hook returns; the format
 * defines it as UTC, so that is what the hook should give. */
size_t gbcore_save_ram_size(gbcore_t *core);
size_t gbcore_cart_ram_size(gbcore_t *core);
int gbcore_save_ram_read(gbcore_t *core, void *destination, size_t capacity);
/* Accepts cart RAM alone or cart RAM plus a 44- or 48-byte clock trailer.
 * The RAM is always kept.  The clock resumes from the trailer's registers
 * plus the wall-clock time since its stamp; a stamp from the future adds
 * nothing (the registers stand), and a halted clock stays where it was.  No
 * trailer, a trailer of any other size, or a stamp before 1997 starts the
 * clock from zero. */
int gbcore_save_ram_write(gbcore_t *core, const void *source, size_t size);

/* Save states.  The image is self-describing (magic, version, console and
 * cartridge identity) and is refused by gbcore_state_load unless it was
 * made by this core for the same cartridge on the same console.
 * gbcore_state_save returns the byte count written or -1. */
size_t gbcore_state_size(gbcore_t *core);
long gbcore_state_save(gbcore_t *core, void *destination, size_t capacity);
int gbcore_state_load(gbcore_t *core, const void *source, size_t size);

/* Wall clock for cartridge RTCs, as seconds since 1970 (UTC: it stamps the
 * battery image). NULL (the default) means C time(). Required on the PSP,
 * whose time() is roughly uptime: with it every MBC3 save looked like it
 * came from the future or from 1970 and the game's clock was reset on every
 * boot. Set before gbcore_create(). A hook that cannot read its clock
 * returns 0, and the cartridge clock holds until it can. */
void gbcore_set_wallclock(time_t (*wallclock)(void));

/* DMG palette.  AUTO shows Super Game Boy colours for games that support
 * the SGB (decided when the game boots) and GREY otherwise; the others are
 * plain four-shade palettes.  Colour-only changes apply to a running game
 * at once; switching an SGB game in or out of SGB colours takes effect the
 * next time it boots.  Unknown ids fall back to AUTO.
 *
 * GBC_AUTO and the twelve GBC_* after it are the Game Boy Color's colours
 * for DMG games (issue #34).  GBC_AUTO is the BG/OBJ0/OBJ1 combination the
 * GBC boot ROM picks from the cartridge header (Nintendo licensee, title
 * sum, 4th letter: gbcore/tgbdual/_cgb_dmg_pal.h); the others are the
 * palettes a player gets by holding a d-pad direction (+A/B) at boot.  Like
 * GREY they boot the game as a plain DMG and change only the display, never
 * the machine, its state or its timing.  They apply wherever a DMG palette
 * is shown (DMG games on FE_CONSOLE_GB and FE_CONSOLE_GBC), never to CGB
 * games, and go through the GBC LCD model (gbcore_set_color_correction). */
enum {
  GBCORE_PALETTE_AUTO = 0,
  GBCORE_PALETTE_GREY,
  GBCORE_PALETTE_GREEN,
  GBCORE_PALETTE_POCKET,
  GBCORE_PALETTE_GBC_AUTO,
  GBCORE_PALETTE_GBC_UP, GBCORE_PALETTE_GBC_UP_A, GBCORE_PALETTE_GBC_UP_B,
  GBCORE_PALETTE_GBC_LEFT, GBCORE_PALETTE_GBC_LEFT_A,
  GBCORE_PALETTE_GBC_LEFT_B,
  GBCORE_PALETTE_GBC_DOWN, GBCORE_PALETTE_GBC_DOWN_A,
  GBCORE_PALETTE_GBC_DOWN_B,
  GBCORE_PALETTE_GBC_RIGHT, GBCORE_PALETTE_GBC_RIGHT_A,
  GBCORE_PALETTE_GBC_RIGHT_B,
  GBCORE_PALETTE_COUNT
};
/* What a GB link session tells the peer about this side's palette.  The
 * peer runs this machine headless, so only the model the palette implies
 * matters to it (AUTO: SGB for SGB carts; anything else: DMG).  The GBC
 * palettes go out as GREY, which a build without them reads the same way:
 * mixed builds still boot the same machine. */
#define GBCORE_PALETTE_WIRE(p) \
  ((p) >= GBCORE_PALETTE_GBC_AUTO && (p) < GBCORE_PALETTE_COUNT \
   ? GBCORE_PALETTE_GREY : (p))
void gbcore_set_palette(unsigned palette);
const char *gbcore_palette_name(unsigned palette);

/* Game Boy Color colours.  On (the default), a CGB palette entry is shown
 * as the GBC's own LCD shows it: its response curve lifts the mid-tones and
 * its green picks up some blue (SameBoy's measured model, "Modern -
 * Balanced").  Off, it is the plain 5-to-8-bit expansion, which on a modern
 * display makes CGB games too dark and too saturated
 * (docs/GB-PALETTE-FIXES.md).  DMG palettes (gbcore_set_palette) other
 * than the GBC ones, and Super Game Boy colours, are not affected.  One setting for the whole program
 * (both instances), applied at once and kept across gbcore_power_on. */
void gbcore_set_color_correction(int on);
int gbcore_color_correction(void);

/* ------------------------------------------------------ link sessions --
 * Line stepping, for two instances interleaved one scanline at a time.
 * gbcore_run_frame(core, b) is exactly gbcore_frame_begin(core, b) and then
 * gbcore_run_line(core) until it returns 1.
 *
 * gbcore_frame_begin latches the buttons for the frame (and raises the
 * joypad interrupt on a new press).  gbcore_run_line runs one scanline and
 * returns 1 when that line ended the frame -- VBlank started, or 154 lines
 * with the LCD off -- after delivering the frame's video and audio; 0 when
 * the frame goes on; -1 on misuse. */
int gbcore_frame_begin(gbcore_t *core, uint16_t buttons);
int gbcore_run_line(gbcore_t *core);

/* Headless: emulate everything the game can observe (CPU, memory, timers,
 * LY/STAT, interrupts, DMA, the serial port, the sound registers and their
 * length/envelope/sweep state) but draw no pixels and synthesise no audio.
 * No video or audio callback is made, and the frame buffer is released.
 * For the partner's Game Boy in a link session.  gbcore_sync_hash is the
 * same headless or not. */
void gbcore_set_headless(gbcore_t *core, int headless);

/* A 64-bit hash of every piece of state the game can observe or that
 * decides what it does next, excluding what exists only to draw or to
 * synthesise sound.  Two machines that agree on it are running the same
 * game from the same point.  Valid between frames. */
uint64_t gbcore_sync_hash(gbcore_t *core);
/* 1 = the hash before claude/gblink-perf (64-bit FNV-1a by byte, ~5x the
 * cost on a PSP); 0 = the word hash (default).  Both consoles of a session
 * must agree: harness A/B only (gblink_hash_legacy).  Set after
 * gbcore_power_on, which resets it. */
void gbcore_set_sync_hash_legacy(int legacy);

/* The far end of the in-memory cable: a peer's internal clock shifted
 * `master_byte` into this machine.  If this machine is waiting on the
 * external clock (SC bit 7 set, bit 0 clear) the byte lands in SB, the
 * transfer completes with the serial interrupt, and the byte SB held is
 * returned; otherwise nothing changes and 0xFF is returned (an idle
 * cable).  Valid between lines. */
uint8_t gbcore_serial_clock_in(gbcore_t *core, uint8_t master_byte);
/* The far end of the cable when this machine armed the external clock
 * during the peer's transfer but has since switched away (gbcore_dual.c):
 * the peer's clock still shifted `master_byte` in.  SB takes it, the
 * transfer request clears, any transfer of its own that this machine
 * started since is cancelled, and the serial interrupt is raised. */
void gbcore_serial_deliver(gbcore_t *core, uint8_t master_byte);
/* SC as the game last wrote it (bit 7 transfer requested, bit 1 fast
 * clock on CGB, bit 0 internal clock), for link diagnostics. */
uint8_t gbcore_serial_control(gbcore_t *core);

/* Returns every static byte of this instance -- the core's globals, file
 * and function statics, and the adapter's settings -- to what a freshly
 * loaded program holds, so two consoles start a link session from the same
 * machine whatever either played before.  (gb_reset does not clear the
 * sound registers NR10-NR51, the CGB registers FF6C/FF72-FF75 or the
 * synthesiser's phases; a game reads the first two.)  Only with no core
 * of this instance active; 0 on success.  Needs the partial link through
 * gbcore/gbcore_instance.ld (-1 without it).  Call gbcore_set_wallclock
 * and gbcore_set_palette after it. */
int gbcore_power_on(void);

/* Reads `len` bytes of the Game Boy's address space starting at `addr`
 * without side effects: ROM, VRAM, cartridge RAM (when mapped), WRAM and its
 * echo, OAM and HRAM, as the CPU would see them now.  I/O registers
 * (FF00-FF7F, FFFF) and unmapped cartridge RAM are refused.  0 on success,
 * -1 if any byte is refused.  For scripted play (autopilot RAM predicates)
 * and tests. */
int gbcore_peek(gbcore_t *core, uint16_t addr, void *out, unsigned len);
/* The writing counterpart, for building test fixtures only (a link
 * session never pokes): VRAM, mapped cartridge RAM, WRAM, OAM and HRAM;
 * ROM and I/O are refused.  Nothing is written unless every byte is
 * accepted. */
int gbcore_poke(gbcore_t *core, uint16_t addr, const void *data,
                unsigned len);

/* One instance's whole API, so session code can drive either. */
typedef struct gbcore_api {
  gbcore_t *(*create)(const char *rom_path, const void *rom_data,
                      size_t rom_size, fe_console_t console,
                      unsigned audio_rate, const gbcore_callbacks_t *callbacks);
  void (*shutdown)(gbcore_t *core);
  int (*run_frame)(gbcore_t *core, uint16_t buttons);
  int (*frame_begin)(gbcore_t *core, uint16_t buttons);
  int (*run_line)(gbcore_t *core);
  void (*set_skip_render)(gbcore_t *core, int skip);
  void (*set_headless)(gbcore_t *core, int headless);
  uint64_t (*sync_hash)(gbcore_t *core);
  uint8_t (*serial_clock_in)(gbcore_t *core, uint8_t master_byte);
  uint64_t (*frame_number)(const gbcore_t *core);
  int (*rom_title)(gbcore_t *core, char *destination, size_t capacity);
  size_t (*save_ram_size)(gbcore_t *core);
  size_t (*cart_ram_size)(gbcore_t *core);
  int (*save_ram_read)(gbcore_t *core, void *destination, size_t capacity);
  int (*save_ram_write)(gbcore_t *core, const void *source, size_t size);
  size_t (*state_size)(gbcore_t *core);
  long (*state_save)(gbcore_t *core, void *destination, size_t capacity);
  int (*state_load)(gbcore_t *core, const void *source, size_t size);
  void (*set_wallclock)(time_t (*wallclock)(void));
  void (*set_palette)(unsigned palette);
  int (*power_on)(void);
  int (*peek)(gbcore_t *core, uint16_t addr, void *out, unsigned len);
  int (*poke)(gbcore_t *core, uint16_t addr, const void *data, unsigned len);
  uint8_t (*serial_control)(gbcore_t *core);
  gbcore_t *(*create_owned)(uint8_t *rom_buffer, size_t rom_size,
                            fe_console_t console, unsigned audio_rate,
                            const gbcore_callbacks_t *callbacks);
  void (*serial_deliver)(gbcore_t *core, uint8_t master_byte);
  void (*set_sync_hash_legacy)(int legacy);
} gbcore_api_t;

/* Instance A: the gbcore_* functions above. */
extern const gbcore_api_t gbcore_api;
/* Instance B: the same object with its symbols renamed gbcore_* ->
 * gbcoreb_*.  Present only in builds that link the second copy. */
extern const gbcore_api_t gbcoreb_api;

#ifdef __cplusplus
}
#endif
#endif
