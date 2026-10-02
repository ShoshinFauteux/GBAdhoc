/* Copyright (c) 2026 GBAdhoc contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Two Game Boys in one process, for GB/GBC link play.
 *
 * Each console in a link session runs BOTH players' Game Boys: slot 0 is
 * the host's game and slot 1 the guest's, on both consoles, so the two
 * consoles perform exactly the same computation.  Only the player's own
 * slot is drawn and heard; the other runs headless.  The two machines'
 * serial ports are wired to each other in memory, so the link cable itself
 * never touches the network -- only each player's buttons do.
 *
 * Slot 0 is TGB Dual instance A (gbcore_*), slot 1 instance B (gbcoreb_*).
 *
 * Time.  The machines advance one scanline each, alternately (slot 0 first),
 * as TGB Dual itself ran two Game Boys.  Each machine keeps its own frame
 * count and asks for its buttons when it starts a frame; a machine whose
 * buttons are not known yet stops the whole pair right there (a stall), and
 * the next gbdual_advance resumes from exactly that point.  The order of
 * every emulated operation is therefore fixed by the inputs alone, never by
 * when they arrived.
 */
#ifndef GBADHOC_GBCORE_DUAL_H
#define GBADHOC_GBCORE_DUAL_H

#include "gbcore.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GBDUAL_SLOTS 2

typedef struct gbdual gbdual_t;

typedef struct gbdual_machine {
  /* Exactly one of rom_path, rom_data and rom_owned.  rom_owned is a
   * buffer from gbcore_rom_alloc holding rom_size bytes; the machine takes
   * it over (no copy) and frees it, even if gbdual_create fails. */
  const char *rom_path;
  const void *rom_data;
  uint8_t *rom_owned;
  size_t rom_size;
  fe_console_t console;
  unsigned palette;              /* GBCORE_PALETTE_*: decides SGB mode */
  time_t (*wallclock)(void);     /* gbcore_set_wallclock; NULL = time() */
  int headless;                  /* gbcore_set_headless */
  /* Battery image to start from (gbcore_save_ram_write), or NULL. */
  const void *save;
  size_t save_size;
  /* Video/audio of this machine (ignored when headless).  serial_* are
   * owned by the pair and must be NULL. */
  gbcore_callbacks_t callbacks;
} gbdual_machine_t;

typedef struct gbdual_config {
  gbdual_machine_t machine[GBDUAL_SLOTS];
  unsigned audio_rate;
  /* 1 = the serial ports are cabled together; 0 = no cable. */
  int link;
  /* gbdual_advance returns each time this slot finishes a frame. */
  int pace_slot;
  /* Scanlines slot 0 runs before slot 1 catches up, while the cable is
   * QUIET; 0/1 = always one (strict alternation).  Quiet = neither machine
   * has a transfer requested or armed (SC bit 7) and no serial event for
   * quiet_frames x 154 scanlines (default 60 frames).  Any serial event (a
   * transfer or an arm) ends slot 0's turn after that scanline, slot 1
   * catches up, and the pair alternates line by line until quiet again --
   * so every transfer after the first of a burst runs exactly as with
   * strict alternation, and the first one sees its peer at most
   * batch_lines scanlines apart (two real Game Boys are never in phase
   * either).  Why: on the PSP, two copies of the core's code alternating
   * every scanline cost more than the second machine's own work (hw1). */
  unsigned batch_lines;
  unsigned quiet_frames;
  /* Nonzero: both cartridge clocks run on EMULATED time -- this many
   * seconds since 1970 at power-on, plus each machine's own emulated
   * frames -- instead of the machines' wallclock functions, so two consoles
   * agree on every MBC3 clock read (Gold/Silver/Crystal).  A link session
   * uses the host's clock at session start for both. */
  int64_t rtc_seed;
  /* Optional: every byte over the cable -- the clocking slot, the byte it
   * sent, the byte it got, the peer's SC at that moment (bit 7 clear = the
   * peer was not waiting, so the clocking side read 0xFF), and the scanline
   * pair it happened in. */
  void (*serial_trace)(void *userdata, int slot, uint8_t sent,
                       uint8_t received, uint8_t peer_sc, uint64_t line);
  void *serial_trace_userdata;
  /* Optional, diagnostics: called after every scanline of either machine. */
  void (*line_trace)(void *userdata, int slot, uint64_t line);
  /* Optional: a machine finished its frame `frame` (0-based count of
   * frames completed, so the first call has frame 1).  Called at the same
   * point of the interleave on every console, so what it reads there is
   * the same everywhere (the link session's hash checks and end point). */
  void (*frame_hook)(void *userdata, int slot, uint64_t frame);
  void *frame_hook_userdata;
} gbdual_config_t;

/* Buttons (GBCORE_BUTTON_*) for `slot` in its frame `frame` (0-based).
 * Return 1 with *buttons filled, or 0 when they are not known yet. */
typedef int (*gbdual_input_fn)(void *userdata, int slot, uint64_t frame,
                               uint16_t *buttons);

gbdual_t *gbdual_create(const gbdual_config_t *config);
void gbdual_destroy(gbdual_t *dual);

/* Advance until the pace slot finishes a frame (returns 1) or a machine
 * needs buttons the input function does not have yet (returns 0; call
 * again later, nothing is lost).  -1 on error. */
int gbdual_advance(gbdual_t *dual, gbdual_input_fn input, void *userdata);

/* Frames `slot` has completed. */
uint64_t gbdual_frame(const gbdual_t *dual, int slot);
/* Scanline pairs run since creation (a pair = one line of each machine). */
uint64_t gbdual_lines(const gbdual_t *dual);
/* Serial bytes exchanged over the in-memory cable, by the clocking slot. */
uint64_t gbdual_serial_bytes(const gbdual_t *dual, int slot);
/* Serial starts (SC writes with bit 7) a machine made, and the scanlines
 * the pair ran in batches (the rest ran strictly alternating). */
uint64_t gbdual_serial_starts(const gbdual_t *dual, int slot);
uint64_t gbdual_batched_lines(const gbdual_t *dual);
/* Batches a serial event cut short while slot 1 was behind. */
uint64_t gbdual_batch_cuts(const gbdual_t *dual);

gbcore_t *gbdual_core(const gbdual_t *dual, int slot);
const gbcore_api_t *gbdual_api(int slot);

/* Both machines' gbcore_sync_hash, combined (valid between gbdual_advance
 * calls: every machine is between lines then). */
uint64_t gbdual_sync_hash(gbdual_t *dual);

#ifdef __cplusplus
}
#endif
#endif
