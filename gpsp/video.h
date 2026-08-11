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
 * The complete per-frame input set the renderer needs, as PROVEN bit-exact by
 * the RENDER_REPLAY desktop oracle (~81k frames, 0 mismatch): a frame-start
 * snapshot of {affine reference seed, OAM_UPDATED} plus the per-line LCD
 * register file.  VRAM/OAM/palette are NOT copied here — they are stable
 * across a frame for ~98% of frames (RASTER_PROF) and the ME snapshots them
 * itself at post time, before emulation of the next frame resumes.
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
typedef struct
{
  u16 ioregs[160][ME_CAP_IOREGS];  /* per-line LCD register file            */
  s32 affine_seed[4];              /* BG2X, BG3X, BG2Y, BG3Y ref at line 0  */
  u32 oam_updated;                 /* reg[OAM_UPDATED] at line 0            */
} me_capture_frame;

extern u32 me_capture_mode;
extern me_capture_frame *me_capture_buf;   /* frontend-owned, double-buffered */

#endif
