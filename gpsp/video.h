/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef VIDEO_H
#define VIDEO_H

void update_scanline(void);
void video_reload_counters(void);

extern s32 affine_reference_x[2];
extern s32 affine_reference_y[2];

extern u16* gba_screen_pixels;

/* ---- ME RENDERER CAPTURE (the production path for the second-core render) --
 *
 * Captured replay inputs are a frame-start snapshot of {affine reference
 * seed, OAM_UPDATED} plus the per-line LCD register file (and, with the two
 * flags below, the per-line affine reference and a mid-frame memory log;
 * docs/ME-MIDFRAME.md).  VRAM/OAM/palette are not copied here; the ME
 * snapshots them at post time.  The desktop
 * RENDER_REPLAY oracle reproduced the CPU renderer for ~81k tested frames,
 * but that does not establish hardware equivalence: the hardware comparison
 * recorded deterministic image divergence when frame snapshots differ from
 * per-scanline state.  See docs/ME-RENDERER-DIVERGENCE.md for the measured
 * results and the still-unproven cause.
 *
 * 64 halfwords per line covers the whole LCD block the renderer reads
 * (0x00..0x2A, VCOUNT at 0x03 included); the renderer reads nothing above
 * 0x3F.  160 x 64 x 2 = 20.5 KB per frame.
 *
 * me_capture_mode: 0 = off (normal rendering).
 *                  1 = capture AND SKIP the render — the ME renders (this is
 *                      where the main CPU gets its ~6 ms/frame back).
 *                  2 = capture AND render — validation only (desktop oracle
 *                      compares a replay-from-capture against the live frame).
 */
#define ME_CAP_IOREGS 64

/* ME_AFFINE_LINES: capture the affine reference point PER LINE, not only at
 * line 0 (docs/ME-MIDFRAME.md).
 *
 * The line log carries BG2X/BG2Y/BG3X/BG3Y as REGISTERS, but the renderer
 * draws from the internal reference counters, which a register write RELOADS
 * (write_io_register16) and every line then steps by PB/PD.  The engine used
 * to start from the line-0 seed and step on its own, so a reference written
 * mid-frame -- by an HBlank IRQ or HBlank DMA, the classic "mode 7" road or
 * floor -- never reached it: the whole frame was projected from line 0.
 * With this on, capture mode 1 keeps stepping the counters exactly as the CPU
 * renderer does and records the value each line is drawn with; the replay
 * installs it before every line.  On a frame with no mid-frame reload the
 * recorded values ARE the ones the engine would have stepped to, so the
 * output is bit-identical to the old contract.
 *
 * Appended at the END of the struct: every older field keeps its offset. */
#ifndef ME_AFFINE_LINES
#define ME_AFFINE_LINES 1
#endif

/* ME_MIDFRAME_LOG: graphics memory written while lines 0-159 draw
 * (docs/ME-MIDFRAME.md).
 *
 * The engine copies VRAM/OAM/palette ONCE, at the vcount-160 post, and draws
 * all 160 lines from that copy.  A game that rewrites any of them between two
 * lines -- an HBlank IRQ or HBlank DMA palette gradient, a tile rewritten per
 * line, sprites moved mid-frame, a bitmap page redrawn while it is on screen
 * -- was drawn with the line-159 state everywhere.
 *
 * With this on, the capture also carries an UNDO/REDO log: for every changed
 * halfword (palette, OAM) or word (VRAM), the line it takes effect on and its
 * value before and after.  The engine walks the log backwards over its
 * snapshot (which is the line-160 state) to recover the line-0 state, then
 * forwards, applying each line's entries before drawing that line.  After the
 * last entry its copy is the line-160 state again, which is what its
 * persistent VRAM mirror must hold for the next frame's dirty-page patch.
 * A log that overflows is dropped whole (log_n = 0): a partial log would break
 * that invariant.  Detection, per scanline, from flags the store paths set:
 *   OAM      reg[OAM_UPDATED] (the store stubs have always set it)
 *   palette  reg[PAL_UPDATED] (cpu.h; one store added to the palette stubs)
 *   VRAM     the ME's dirty-page map, split at line 0.  Scanning it every
 *            line would cost tens of us a frame, so it is scanned per line
 *            only while "active": a frame whose visible lines wrote VRAM
 *            that some line reads turns it on for ME_LOG_HOLD frames, and
 *            such an effect is exact from its second frame on.
 * A frame with none of this costs two flag loads a line, a 2 KiB baseline
 * copy and two 96-byte map passes. */
#ifndef ME_MIDFRAME_LOG
#define ME_MIDFRAME_LOG 1
#endif
#define ME_LOG_HOLD 60              /* frames tracking stays on after a hit  */

typedef struct
{
  u32 tag;       /* line (bits 0-7) | region << 8 | index << 10             */
  u32 old_v;     /* value before the write (line-0 side)                    */
  u32 new_v;     /* value from `line` on                                    */
} me_log_entry;
#define ME_LOG_PAL    0            /* index: palette_ram_converted halfword  */
#define ME_LOG_OAM    1            /* index: oam_ram halfword                */
#define ME_LOG_VRAM   2            /* index: vram word                       */
#define ME_LOG_OAMREP 3            /* no data: re-sort OBJs at this line     */

typedef struct
{
  u16 ioregs[160][ME_CAP_IOREGS];  /* per-line LCD register file            */
  s32 affine_seed[4];              /* BG2X, BG3X, BG2Y, BG3Y ref at line 0  */
  u32 oam_updated;                 /* reg[OAM_UPDATED] at line 0            */
#if ME_AFFINE_LINES
  s32 affine_line[160][4];         /* same order, the value line N draws at */
#endif
#if ME_MIDFRAME_LOG
  uintptr_t log_addr;              /* frontend: this capture's entry buffer  */
  u32 log_cap;                     /* frontend: its size in entries, 0 = off */
  u32 log_n;                       /* core: entries valid for THIS frame     */
  u32 log_flags;                   /* core: ME_LOGF_* (diagnostic)           */
#endif
} me_capture_frame;
#define ME_LOGF_OVERFLOW  1u       /* log dropped: more than log_cap entries */
#define ME_LOGF_PAL       2u       /* palette entries this frame             */
#define ME_LOGF_VRAM_ON   4u       /* per-line VRAM tracking was active      */

#if ME_MIDFRAME_LOG
/* Frontend-owned, 96 KiB: the core's copy of VRAM as the log last saw it,
 * which is where the log's old values come from.  NULL = VRAM tracking off
 * (palette and OAM are still logged). */
extern u8 *me_vram_shadow;
/* The frontend's "the ME has copied the dirty pages, clear the map" step.
 * Must go through the core so page invalidation for the shadow is not lost. */
void me_vram_map_consumed(void);
/* vcount 160, before gpsp_visible_done_hook: the line-160 check, the map
 * merge and the hysteresis update.  main.c calls it every frame. */
void me_capture_visible_end(void);
void me_capture_visible_resume(void);
/* What the recorder did, cumulative (host CPU cost is proportional to it):
 * page_diffs/page_copies are 1 KiB passes, base_copies 1 KiB copies,
 * map_scans 96-byte map scans (one per tracked line). */
typedef struct
{
  u32 frames, tracked_frames, pal_checks, oam_checks, map_scans;
  u32 page_diffs, page_copies, base_copies, entries, overflows;
} me_log_stats_t;
extern me_log_stats_t me_log_stats;
/* Frontend: give capture `cap` an entry buffer (NULL/0 = logging off for it). */
void me_capture_log_setup(void *cap, void *entries, u32 n_entries);
#endif

extern u32 me_capture_mode;
extern me_capture_frame *me_capture_buf;   /* frontend-owned, double-buffered */

/* Replay lines 0-159 of a capture through update_scanline().  The ONE loop
 * the Media Engine (psp/me/me_render_glue.cc) and the desktop models
 * (ME_CAP_VALIDATE, ME_TIMING_SIM) share, so the desktop tests the engine's
 * own code.  The caller has already installed the seed, reg[OAM_UPDATED],
 * gba_screen_pixels and the graphics memory.  `reloads`, if not NULL,
 * receives the number of lines whose recorded reference differs from where
 * the previous line's step left it (0 without ME_AFFINE_LINES).  `use_log`:
 * apply the ME_MIDFRAME_LOG entries (only valid when the graphics memory is
 * the line-160 state, i.e. a vcount-160 post; the ME glue's host zeroes
 * log_n for any other post). */
void me_replay_lines(const me_capture_frame *cap, int *reloads, int use_log);

#endif
