/* video_psp.h — GU video layer for the native PSP frontend (Phase 2).
 *
 * Owns: GU init/term, the RGB565→PSP-5650 staging conversion (the hw
 * color-order fix — see stage_convert in video_psp.c), scaling modes +
 * filter (plan §8 Settings/Video), the pre-swap GE drawbuffer readback
 * (color regression check), and overlay primitives (rects + 8x16 text)
 * used by the OSD/toast layer and the menu UI.
 *
 * All calls from the main/emu thread only.
 */
#ifndef VIDEO_PSP_H
#define VIDEO_PSP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VID_SCR_W 480
#define VID_SCR_H 272

enum
{
   VID_SCALE_1X      = 0,  /* 240x160 centered (hw-confirmed v1 default) */
   VID_SCALE_FIT     = 1,  /* aspect-preserving fill: 408x272 centered   */
   VID_SCALE_STRETCH = 2,  /* fullscreen 480x272                          */
   VID_SCALE_MODES   = 3
};

enum
{
   VID_FILTER_NEAREST  = 0,
   VID_FILTER_BILINEAR = 1
};

void vid_init(void);
void vid_term(void);

void vid_set_mode(int scale, int filter);
int  vid_scale_mode(void);
int  vid_filter(void);
const char *vid_scale_name(int scale);
const char *vid_filter_name(int filter);

/* Cycle the runtime presets (Triangle, plan §8): 1x → fit → fit+bilinear
 * → stretch → stretch+bilinear → 1x. Returns the new preset's name. */
const char *vid_cycle_preset(void);

/* Blit one libretro-RGB565 GBA frame through the GU path at the current
 * scale/filter. Converts channel order into the staging buffer. */
void vid_draw_frame(const uint16_t *pix, unsigned w, unsigned h,
                    size_t pitch_bytes);

/* ADR-0080: draw a frame the Media Engine already staged into a
 * 256-texel-stride main-RAM buffer — the list-build half of
 * vid_draw_frame with the CPU copy removed.  `staged` must be coherent
 * in RAM (ME writes it uncached). */
void vid_draw_prestaged(const uint16_t *staged, unsigned w, unsigned h);

/* ---- staging-buffer placement (ADR-0034, settled by hardware) ------------
 * The blit costs ~2.5 ms/frame on hardware and unlike the rest of the frame
 * budget it IS our code.  All three placements ship and `config.ini
 * blit_mode` picks one with no rebuild; the DEFAULT is 2 (VRAM), decided by
 * measurement on a PSP-1000 (us/frame, 6-8 windows each, ~6 us spread):
 *
 *              stage    gu    tot
 *   0 cached    1337   1176   2513
 *   1 uncached  1041   1174   2215
 *   2 vram      1071    728   1800
 *
 * **Read the `gu` column.**  We predicted these modes would move `stage`
 * only and leave the GE alone; they do not.  VRAM's win is mostly the GE
 * reading its source texture out of local memory (1176 -> 728, -38 %),
 * which dwarfs the fact that its CPU-side writes are marginally SLOWER than
 * mode 1's (1071 vs 1041).  The rig predicted none of this: it reported all
 * three identical to the microsecond. */
enum
{
   /* Cached RAM staging + sceKernelDcacheWritebackRange over 82 KiB every
    * frame.  The original path; kept for the A/B. */
   VID_BLIT_CACHED   = 0,
   /* Same RAM buffer written through the uncached mirror (0x40000000|addr):
    * stores go straight to memory, so there is no cache to write back and
    * the 76.8 KiB never crosses the bus twice.  Best CPU-side number of the
    * three, and still 415 us/frame worse overall than VRAM. */
   VID_BLIT_UNCACHED = 1,
   /* Staging in VRAM (sceGeEdramGetAddr + a bump above the display and
    * depth buffers), written uncached.  No writeback, and — the part that
    * actually won it — the GE fetches the texture from local memory.
    * DEFAULT.  Falls back to VID_BLIT_CACHED if the bump does not fit. */
   VID_BLIT_VRAM     = 2,
   VID_BLIT_MODES    = 3
};

/* Selects the staging placement.  Safe before or after vid_init(); falls
 * back to VID_BLIT_CACHED if VRAM has no room.  Returns the mode actually
 * in force. */
int  vid_set_blit_mode(int mode);
int  vid_blit_mode(void);
const char *vid_blit_mode_name(int mode);

/* Blit cost since the last call, in microseconds, then reset.  `stage` is
 * the RGB565->5650 conversion (plus the cache writeback in mode 0); `gu` is
 * the GE list build and the sceGuSync that waits for it.
 *
 * `wait` SPLITS `gu` (ADR-0038).  It is the sceGuSync alone — the part of
 * the frame where the CPU is stopped doing nothing while the GE rasterises,
 * and therefore the only part a deferred-sync scheme could ever recover.
 * `gu - wait` is list building, which is real CPU work and stays whatever we
 * do.  These were one number until now, which is precisely why nobody could
 * say whether deferring the sync was worth 700 us or 70; measure first. */
void vid_blit_prof(unsigned *frames,
                   unsigned *stage_us, unsigned *stage_max,
                   unsigned *gu_us,    unsigned *gu_max,
                   unsigned *wait_us,  unsigned *wait_max);

/* Overlay pass (OSD chips/toasts over the game, or full menu screens):
 * begin(clear=1) starts from a cleared black frame (menu screens);
 * begin(clear=0) draws on top of whatever is already in the drawbuffer.
 * Coordinates are screen pixels; colors are libretro RGB565 + alpha. */
void vid_overlay_begin(int clear);
void vid_rect(int x, int y, int w, int h, uint16_t rgb565, int alpha);
void vid_text(int x, int y, const char *str, uint16_t rgb565);
void vid_text_center(int y, const char *str, uint16_t rgb565);
/* Vertical gradient fill (top color -> bottom color), same pass/rules as
 * vid_rect.  Drawn as one triangle strip so the GE interpolates per-pixel. */
void vid_gradient(int x, int y, int w, int h,
                  uint16_t top565, uint16_t bot565, int alpha);
/* Draw the top-left tw x th texels of a 128x128 RGB565 (PSP channel order)
 * buffer, scaled to w x h at x,y.  `pix` must be 16-byte aligned and the
 * caller must have written the texels back to memory (writeback the cache)
 * before the overlay pass ends — the UI box-art loader does this once at
 * load.  alpha 255 = opaque.  Used by the game-gallery browser. */
void vid_image(int x, int y, int w, int h, const uint16_t *pix,
               int tw, int th, int alpha);
void vid_overlay_end(void);

/* ---- deferred GE sync (ADR-0040, `config.ini gu_defer`, DEFAULT OFF) -----
 * ON leaves a finished display list running on the GE instead of blocking
 * on it, and waits at the last safe moment — after the main loop's vblank
 * wait, immediately before the swap — so the GE rasterises through idle
 * time the frame was spending anyway.  The recoverable amount is exactly
 * the loop's idle: small at full speed, ~9 ms during a 40 fps clamped
 * session.  A/B it IN A SESSION and read `wait=` in blit_prof.
 *
 * Off by default: it is a concurrency change to the display path, and the
 * test rig can prove it draws correctly but cannot price it. */
int  vid_gu_defer(void);
void vid_set_gu_defer(int on);

/* Wait for any in-flight display list.  Callers only need this before
 * touching the drawbuffer behind the GU's back; vid_swap() and
 * vid_dump_ge() already do it. */
void vid_gu_flush(void);

/* Swap buffers (tracks which VRAM buffer is being drawn).  Flushes first. */
void vid_swap(void);

/* GE drawbuffer readback (color-order regression check): writes the buffer
 * that was just drawn (pre-swap) as a 24bpp BMP, decoding VRAM bytes with
 * the REAL GE 5650 layout. Returns 0 on success. */
int vid_dump_ge(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_PSP_H */
