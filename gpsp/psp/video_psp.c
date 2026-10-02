/* video_psp.c — GU video layer (see video_psp.h). */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspge.h>

#include <stdio.h>
#include <string.h>

#include "video_psp.h"
#include "font_8x16.h"
#include "font_ui.h"
#include "logo_ui.h"
#include "fe_util.h"
#include "fe_evt.h"
#include "ambient_look.h"

/* The other half of the pixel-format link guard; see gba_memory.c. */
#ifdef USE_PSP_RGB565_FORMAT
extern const char gpsp_core_pixfmt_psp5650;
__attribute__((used)) static const char *const gpsp_pixfmt_agreed =
   &gpsp_core_pixfmt_psp5650;
#else
extern const char gpsp_core_pixfmt_rgb565;
__attribute__((used)) static const char *const gpsp_pixfmt_agreed =
   &gpsp_core_pixfmt_rgb565;
#endif

#define FB_STRIDE 512
#define FB_BYTES  (FB_STRIDE * VID_SCR_H * 2)
#ifdef VID_TRIPLE
#define VID_NBUF  3u        /* display buffers; depth and staging sit above */
#else
#define VID_NBUF  2u
#endif

/* TWO display lists (ADR-0040).  One was enough while every list was synced
 * before the next one started; `gu_defer` lets a finished list keep running
 * on the GE while the CPU builds the next, so the buffer the GE is reading
 * must not be the buffer the CPU is writing.  Two is the exact requirement:
 * a frame issues at most a blit list and an OSD list before the pre-swap
 * flush, and gu_next_list() flushes rather than hand out a third. */
static unsigned int __attribute__((aligned(64))) gu_list[2][8192];
static int g_gu_cur;       /* index handed out by the last gu_next_list() */
static int g_gu_queued;    /* finished lists not yet synced, 0..2 */
static int g_gu_defer;     /* config.ini gu_defer; OFF unless asked */

typedef struct
{
   short u, v;
   short x, y, z;
} vtx_t;

typedef struct
{
   unsigned int color;
   short x, y, z;
} cvtx_t;

typedef struct
{
   short u, v;
   unsigned int color;
   short x, y, z;
} tcvtx_t;

/* Current GE draw-buffer offset in VRAM (toggles on every swap): lets the
 * GE readback dump the buffer that was just drawn, pre-swap. */
static unsigned g_draw_off;

#ifdef VID_TRIPLE
/* TRIPLE BUFFERING -- fixes tearing along the top of the screen.
 *
 * sceGuSwapBuffers() latches with PSP_DISPLAY_SETBUF_NEXTFRAME, so the flip
 * itself is atomic at vblank and cannot tear.  The tear comes from the frame
 * AFTER it: the new draw buffer is the one still on screen until that latch
 * happens.  The pacing loop in main_psp.c normally blocks in
 * sceDisplayWaitVblankStart() so a vblank always intervenes -- but when a
 * frame overruns its budget it takes the `g_fh_late` path and does NOT wait,
 * and the next GE blit then writes into the buffer being scanned out.  Blit
 * and beam both run top-to-bottom, the blit outruns the beam near the top,
 * and the seam lands a few rows down.  Vertical scrolling makes it obvious
 * because a horizontal seam separates two vertically-offset images.
 *
 * Three buffers provide space for the LCD's current frame, the pending
 * vblank flip, and a draw target. They are NOT a modulo-3 ring: fast-forward
 * and pacing catch-up can submit several frames before one vblank. vid_swap
 * must exclude both the current and pending display buffers when choosing
 * the next target. This needs no extra vblank wait on the normal path.
 *
 * VRAM cost: a third 512x272x2 = 272 KiB buffer.  Layout becomes
 *   0 / 272K / 544K  display, 816K depth, 1.09M staging, ending ~1.2 MiB of
 * the 2 MiB every PSP has (the 1000 included -- its 32 MiB limit is main
 * RAM, a different pool from sceGeEdram). */
static const unsigned g_fb_off[3] = { 0u, FB_BYTES, FB_BYTES * 2u };
static unsigned g_fb_cur;          /* index currently being DRAWN into */

/* Point the GE at the current draw buffer.  Must be issued inside a list, so
 * every sceGuStart in this file pairs with it. */
#define VID_GU_TARGET()    sceGuDrawBuffer(GU_PSM_5650, (void *)(uintptr_t)g_fb_off[g_fb_cur], FB_STRIDE)

/* HAS ANYTHING FILLED THE BUFFER WE ARE ABOUT TO SHOW?
 *
 * With two buffers, swapping with nothing drawn was invisible: you re-showed
 * the complete frame from two presents ago, which during fast-forward reads as
 * ordinary strobing.  With three buffers it is a bug.  The ring advances onto a
 * buffer whose newest content is three presents old, and buffer 2 is never
 * written at all before its first use -- so it shows BLACK.
 *
 * That is the "black bars flashing during FF" regression this fixes.  The
 * paced branch in main_psp.c calls vid_swap() unconditionally (correct when a
 * swap could only ever show a complete older frame), while every fast-forward
 * path deliberately skips the blit on most frames: g_blit_suppress for the
 * intermediate 1.5x/3x frames, the FF_PRESENT_MASK cadence for uncapped CPU
 * FF, and the post-only ME paths.  Skipped blit + unconditional swap = a
 * buffer nobody filled, on screen.
 *
 * A buffer counts as filled when something CLEARED it and drew a whole frame
 * into it: vid_draw_frame, vid_draw_prestaged, or vid_overlay_begin(1) (menus,
 * the wake overlay, the "connecting" frames).  vid_overlay_begin(0) is the OSD
 * drawing chips ON TOP of whatever is already there; it cannot make an undrawn
 * buffer presentable, so it deliberately does not set this.
 *
 * Scoped to VID_TRIPLE: in the double-buffered fallback an unconditional swap
 * is the shipped 2.0.3 behaviour and showed a complete frame, so it is left
 * exactly as it was. */
static int g_fb_filled;
#define VID_FB_FILLED()  do { g_fb_filled = 1; } while (0)
#else
#define VID_GU_TARGET()  do { } while (0)
#define VID_FB_FILLED()  do { } while (0)
#endif

static int g_scale  = VID_SCALE_1X;
static int g_filter = VID_FILTER_NEAREST;
/* The smoothing TRIANGLE's smoothed presets use: the last non-nearest filter
 * vid_set_mode was given.  BILINEAR until a player picks sharp, so the cycle
 * is unchanged for everyone who never does.  Invariant: g_filter is always
 * NEAREST or g_smooth. */
static int g_smooth = VID_FILTER_BILINEAR;

/* Where a w x h source lands, and which of its rows are drawn.  Only the
 * integer 2x mode crops (sy0 > 0, sh < h); every other mode draws all rows. */
typedef struct
{
   int ox, oy, dw, dh;   /* destination rect, screen px                   */
   int sy0, sh;          /* source rows [sy0, sy0 + sh)                   */
} vid_geo;

static void dest_rect(unsigned w, unsigned h,
                      int *ox, int *oy, int *dw, int *dh);

/* Ambient bars, defined at the end of this file. */
static int  amb_draw_bg(const vid_geo *g);
static void amb_snap_maybe(const uint16_t *src, int stride, unsigned w,
                           unsigned h, int uncached);

/* Sharp bilinear (docs/SHARP-BILINEAR.md), defined after the ambient bars:
 * its render target sits above their texture in VRAM. */
static int  sharp_draw(const uint16_t *tex, unsigned w, const vid_geo *g);
static int  picture_filter(unsigned w, const vid_geo *g);

/* Staging buffer for the GE texture upload.
 *
 * Historically the core output libretro RGB565 (R in bits 11-15) while the
 * GE's 5650 texture format is PSP channel order (R in bits 0-4), so this
 * buffer existed to hold a per-pixel R/B swap — uploading the core buffer
 * directly rendered with R and B swapped on hardware (2026-08-01 hw
 * baseline finding; PPSSPP's decode/encode round-trip masks it on screen,
 * which is why the harness check reads raw drawbuffer bytes instead).
 *
 * ADR-0039 moved that swap into the core's `convert_palette`, where it is
 * free (it was already swapping R and B to MAKE libretro RGB565 out of the
 * GBA's BGR555; it now just permutes differently).  Under
 * USE_PSP_RGB565_FORMAT the staging step is therefore a plain copy.
 *
 * THE BUFFER STILL EARNS ITS KEEP.  Binding the GE straight at the core's
 * framebuffer would drop the copy entirely, and it is tempting now that the
 * formats agree — but ADR-0034's hardware A/B already priced it: with the
 * texture in main RAM the GE took 1174 us, in VRAM 728 us.  A direct bind
 * puts the texture back in main RAM and hands the GE ~446 us to save the
 * CPU ~600, on top of needing a 76.8 KiB dcache writeback and a malloc'd
 * base with no alignment guarantee.  Measured, not assumed: keep the copy.
 *
 * One extra row, and GU_CLAMP wrap set at init.  The texel column just right
 * of the picture and the row just below it repeat the picture's edge
 * (stage_convert_rgb565), so a bilinear tap past the right or bottom edge
 * reads the edge pixel; GU_CLAMP does the same past the left and top.
 * These used to be "zeroed so edge taps read black" -- true for a 240-wide
 * GBA frame only: a 160x144 GB frame left columns 160-255 and rows 144-160
 * holding whatever was drawn there before, and the last column of the
 * picture was blended with it (docs/GB-PALETTE-FIXES.md). */
#define STAGE_STRIDE 256
#define STAGE_ROWS   161
#define STAGE_BYTES  (STAGE_STRIDE * STAGE_ROWS * 2)
static uint16_t __attribute__((aligned(64))) fb_staging_ram[STAGE_STRIDE * STAGE_ROWS];

/* ---- staging placement (ADR-0034) ----------------------------------------
 * `fb_staging` is what everything below writes and what the GE is pointed
 * at; `g_blit_mode` decides where it lives and whether a cache writeback is
 * needed.  The uncached and VRAM modes need NO writeback at all, which is
 * the whole point: mode 0 pushes 76.8 KiB into the D-cache and then walks
 * 1288 cache lines flushing it back out, so the pixels cross the bus twice
 * and the frame pays for a cache it never reads from.
 *
 * The GE is given the same pointer the CPU writes.  The mirror bits are
 * address-space decoration for the CPU; the GE resolves the physical page
 * either way, and vid_dump_ge() has been reading VRAM through 0x44000000
 * since Phase 2 on exactly that basis. */
static uint16_t *fb_staging = fb_staging_ram;
static int       g_blit_mode = VID_BLIT_CACHED;

/* Blit profile (ADR-0034).  Accumulated per frame, drained by the caller on
 * the heartbeat cadence — the whole reason all three modes ship is that the
 * choice between them can only be made from hardware numbers. */
static unsigned g_blit_frames;
static unsigned g_stage_us_tot, g_stage_us_max;
static unsigned g_gu_us_tot,    g_gu_us_max;
/* ADR-0038: the sceGuSync alone, carved out of `gu`.  See video_psp.h. */
static unsigned g_wait_us_tot,  g_wait_us_max;

/* Font atlas: 16x16 grid of 8x16 glyphs, GU_PSM_5551 (PSP order: A bit 15),
 * white-on-transparent so text tints via TFX_MODULATE vertex color. */
static uint16_t __attribute__((aligned(64))) font_tex[128 * 256];

static void stage_convert_rgb565(const uint16_t *src, unsigned w, unsigned h,
                                 size_t pitch_bytes)
{
   unsigned y;
#ifdef USE_PSP_RGB565_FORMAT
   /* ADR-0039: the core already emits the GE's channel order, so there is
    * nothing to convert and this is a straight row copy.  The rows are not
    * contiguous at either end (core pitch 480 B, staging stride 512 B), so
    * it stays a loop rather than one big copy. */
   if (w > 0 && w < STAGE_STRIDE && h > 0)
   {
      /* Plus the edge texels (see STAGE_STRIDE): column w repeats column
       * w-1, and row h, when there is room, repeats row h-1.  Read from the
       * cached source, never back from the (possibly uncached) staging. */
      unsigned rows = h < STAGE_ROWS ? h + 1 : h;
      for (y = 0; y < rows; y++)
      {
         const uint16_t *row = (const uint16_t *)((const uint8_t *)src +
                               (size_t)(y < h ? y : h - 1) * pitch_bytes);
         memcpy(&fb_staging[y * STAGE_STRIDE], row, (size_t)w * 2);
         fb_staging[y * STAGE_STRIDE + w] = row[w - 1];
      }
   }
   else
      for (y = 0; y < h; y++)
         memcpy(&fb_staging[y * STAGE_STRIDE],
                (const uint8_t *)src + (size_t)y * pitch_bytes,
                (size_t)w * 2);
#else
   unsigned x;
   for (y = 0; y < h; y++)
   {
      const uint32_t *s =
         (const uint32_t *)((const uint8_t *)src + (size_t)y * pitch_bytes);
      uint32_t *d = (uint32_t *)&fb_staging[y * STAGE_STRIDE];
      /* Two pixels per word: RGB565 -> PSP 5650 is an R/B field swap. */
      for (x = 0; x < w / 2; x++)
      {
         uint32_t v = s[x];
         d[x] = ((v & 0xF800F800u) >> 11) |
                 (v & 0x07E007E0u) |
                ((v & 0x001F001Fu) << 11);
      }
   }
   /* Edge texels, as above: column w repeats column w-1, row h row h-1. */
   if (w > 0 && h > 0 && w < STAGE_STRIDE)
   {
      for (y = 0; y < h; y++)
         fb_staging[y * STAGE_STRIDE + w] =
            fb_staging[y * STAGE_STRIDE + w - 1];
      if (h < STAGE_ROWS)
         memcpy(&fb_staging[h * STAGE_STRIDE],
                &fb_staging[(h - 1) * STAGE_STRIDE], (size_t)(w + 1) * 2);
   }
#endif
   /* Only mode 0 has a cache to flush.  In modes 1 and 2 the stores already
    * went to memory, so a writeback here would be pure cost — and worse, in
    * VRAM mode it would be a writeback over an address range the D-cache has
    * no lines for. */
   if (g_blit_mode == VID_BLIT_CACHED)
      sceKernelDcacheWritebackRange(fb_staging, STAGE_BYTES);
}

/* ---- staging placement + blit profile (ADR-0034) ------------------------ */

const char *vid_blit_mode_name(int mode)
{
   switch (mode)
   {
   case VID_BLIT_UNCACHED: return "uncached";
   case VID_BLIT_VRAM:     return "vram";
   default:                return "cached";
   }
}

int vid_blit_mode(void) { return g_blit_mode; }

int vid_set_blit_mode(int mode)
{
   uintptr_t base;

   if (mode < 0 || mode >= VID_BLIT_MODES)
      mode = VID_BLIT_CACHED;

   if (mode == VID_BLIT_VRAM)
   {
      /* Bump-allocate above the two display buffers and the depth buffer.
       * 2 MiB of VRAM against 3 x 512x272x2 = 816 KiB leaves ~1.2 MiB, so
       * 82 KiB fits with room to spare — but check rather than assert, and
       * fall back rather than scribble on the framebuffer if it ever moves. */
      uintptr_t off  = (uintptr_t)FB_BYTES * (VID_NBUF + 1u);
      unsigned  size = sceGeEdramGetSize();
      off = (off + 63u) & ~(uintptr_t)63u;
      if (!size || off + (uintptr_t)STAGE_BYTES > (uintptr_t)size)
         mode = VID_BLIT_CACHED;                 /* no room: stay in RAM */
      else
      {
         base = (uintptr_t)sceGeEdramGetAddr() + off;
         fb_staging = (uint16_t *)(0x40000000u | base);
      }
   }

   if (mode == VID_BLIT_UNCACHED)
      fb_staging = (uint16_t *)(0x40000000u | (uintptr_t)fb_staging_ram);
   else if (mode == VID_BLIT_CACHED)
      fb_staging = fb_staging_ram;

   /* Whichever way we are moving, the RAM buffer may hold dirty lines from
    * the previous mode.  Flush AND invalidate: a stale dirty line evicting
    * later over uncached writes is a corrupted frame that would look like a
    * GE bug, and the aliasing is invisible in the source. */
   sceKernelDcacheWritebackInvalidateRange(fb_staging_ram, STAGE_BYTES);

   g_blit_mode = mode;
   return mode;
}

void vid_blit_prof(unsigned *frames,
                   unsigned *stage_us, unsigned *stage_max,
                   unsigned *gu_us,    unsigned *gu_max,
                   unsigned *wait_us,  unsigned *wait_max)
{
   if (frames)    *frames    = g_blit_frames;
   if (stage_us)  *stage_us  = g_stage_us_tot;
   if (stage_max) *stage_max = g_stage_us_max;
   if (gu_us)     *gu_us     = g_gu_us_tot;
   if (gu_max)    *gu_max    = g_gu_us_max;
   if (wait_us)   *wait_us   = g_wait_us_tot;
   if (wait_max)  *wait_max  = g_wait_us_max;
   g_blit_frames = g_stage_us_tot = g_stage_us_max = 0;
   g_gu_us_tot   = g_gu_us_max   = 0;
   g_wait_us_tot = g_wait_us_max = 0;
}

/* ---- deferred GE sync (ADR-0040) ----------------------------------------
 *
 * The blit used to end with sceGuSync, which stops the CPU dead until the GE
 * has rasterised — 728 us of the 1800 us blit on a PSP-1000, doing nothing.
 * Meanwhile the main loop's very next act, a few microseconds later, is
 * sceDisplayWaitVblankStart: idling anyway.  Deferring the sync past the
 * vblank wait lets the GE work through time the frame was already spending.
 *
 * The saving is bounded by the idle, so it is SMALL at full speed (~700 us
 * of slack in a 16743 us budget) and LARGE during a clamped session, where
 * a 40 fps target leaves ~9 ms.  In-session is where the budget hurts, so
 * that is the right shape — but it does mean a solo-play A/B may show
 * almost nothing while a session A/B shows a lot.  Judge it in a session.
 *
 * OFF BY DEFAULT.  This is a concurrency change to the display path and the
 * test rig can prove it draws the right pixels but cannot price it.
 *
 * The invariant everything rests on: no buffer swap and no drawbuffer read
 * may happen with a list still in flight.  vid_swap() and vid_dump_ge()
 * flush first, unconditionally, defer or no defer. */

void vid_gu_flush(void)
{
   unsigned t0, d;

   if (!g_gu_queued)
      return;
   t0 = sceKernelGetSystemTimeLow();
   sceGuSync(0, 0);
   d  = sceKernelGetSystemTimeLow() - t0;
   /* Same accumulator the inline sync uses: `wait` means "CPU stalled on the
    * GE" wherever the stall happened, so the A/B is a straight comparison of
    * one number between two runs. */
   g_wait_us_tot += d;
   if (d > g_wait_us_max)
      g_wait_us_max = d;
   g_gu_queued = 0;
}

/* Hand out the list buffer the GE is definitely not reading. */
static unsigned int *gu_next_list(void)
{
   if (g_gu_queued >= 2)
      vid_gu_flush();
   g_gu_cur ^= 1;
   return gu_list[g_gu_cur];
}

/* Ends a list: either wait for it now (the historical behaviour) or leave it
 * running and count it against the two-buffer budget. */
static void gu_end_list(void)
{
   sceGuFinish();
   if (g_gu_defer)
      g_gu_queued++;
   else
      sceGuSync(0, 0);
}

int vid_gu_defer(void) { return g_gu_defer; }

void vid_set_gu_defer(int on)
{
   if (!on)
      vid_gu_flush();       /* never leave a list in flight across the change */
   g_gu_defer = on ? 1 : 0;
}

/* libretro RGB565 -> PSP ABGR8888 (for vertex colors / clears). */
static unsigned int rgb565_to_abgr(uint16_t c, int alpha)
{
   unsigned r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
   r = (r << 3) | (r >> 2);
   g = (g << 2) | (g >> 4);
   b = (b << 3) | (b >> 2);
   return ((unsigned)alpha << 24) | (b << 16) | (g << 8) | r;
}

static void font_atlas_build(void)
{
   int c, row, bit;
   for (c = 0; c < 256; c++)
   {
      int cx = (c & 15) * FE_FONT_W;
      int cy = (c >> 4) * FE_FONT_H;
      for (row = 0; row < FE_FONT_H; row++)
      {
         unsigned char bits = fe_font8x16[c * FE_FONT_H + row];
         uint16_t *d = &font_tex[(cy + row) * 128 + cx];
         for (bit = 0; bit < 8; bit++)
            d[bit] = (bits & (0x80 >> bit)) ? 0xFFFF : 0x0000;
      }
   }
   sceKernelDcacheWritebackRange(font_tex, sizeof(font_tex));
}

void vid_init(void)
{
   font_atlas_build();

   sceGuInit();
   sceGuStart(GU_DIRECT, gu_next_list());
   VID_GU_TARGET();
   sceGuDrawBuffer(GU_PSM_5650, (void *)0, FB_STRIDE);
   sceGuDispBuffer(VID_SCR_W, VID_SCR_H, (void *)(uintptr_t)FB_BYTES, FB_STRIDE);
   sceGuDepthBuffer((void *)(uintptr_t)(FB_BYTES * VID_NBUF), FB_STRIDE);
   sceGuOffset(2048 - (VID_SCR_W / 2), 2048 - (VID_SCR_H / 2));
   sceGuViewport(2048, 2048, VID_SCR_W, VID_SCR_H);
   sceGuScissor(0, 0, VID_SCR_W, VID_SCR_H);
   sceGuEnable(GU_SCISSOR_TEST);
   sceGuDisable(GU_DEPTH_TEST);
   sceGuDisable(GU_BLEND);
   /* sceGuInit() leaves ordered dithering ON.  That is right for a
    * photograph and wrong for flat UI surfaces: at RGB565 it stipples
    * every panel and gradient with a 4x4 grain that is plainly visible
    * on a PSP-3000 IPS panel.  The emulated frame is blitted 5650->5650
    * with GU_TFX_REPLACE, so it never dithered anyway -- nothing that
    * matters loses shading here. */
   sceGuDisable(GU_DITHER);
   sceGuEnable(GU_TEXTURE_2D);
   sceGuTexWrap(GU_CLAMP, GU_CLAMP);
   sceGuClearColor(0);
   sceGuFinish();
   sceGuSync(0, 0);
   sceDisplayWaitVblankStart();
   sceGuDisplay(GU_TRUE);

   g_draw_off = 0;
}

void vid_term(void)
{
   sceGuTerm();
}

void vid_set_mode(int scale, int filter)
{
   if (scale >= 0 && scale < VID_SCALE_MODES)
      g_scale = scale;
   /* Any other non-zero value is bilinear, as it always was. */
   g_filter = filter == VID_FILTER_SHARP ? VID_FILTER_SHARP
            : filter ? VID_FILTER_BILINEAR : VID_FILTER_NEAREST;
   if (g_filter != VID_FILTER_NEAREST)
      g_smooth = g_filter;
}

int vid_scale_mode(void) { return g_scale; }
int vid_filter(void)     { return g_filter; }

const char *vid_scale_name(int scale)
{
   switch (scale)
   {
   case VID_SCALE_FIT:     return "fit";
   case VID_SCALE_STRETCH: return "stretch";
   case VID_SCALE_INT2:    return "2x";
   default:                return "1x";
   }
}

const char *vid_filter_name(int filter)
{
   return filter == VID_FILTER_SHARP ? "sharp bilinear"
        : filter ? "bilinear" : "nearest";
}

const char *vid_cycle_preset(void)
{
   /* 1x/nearest -> fit/nearest -> fit/bilinear -> stretch/nearest ->
    * stretch/bilinear -> 1x/nearest.  2x is opt-in from Settings and not in
    * here (it crops the GBA picture top and bottom, which nobody cycling
    * for "bigger" should land on by accident); from 2x, i ends at 5 and the
    * cycle continues at fit. */
   static const struct { int s, f; const char *name; } preset[5] = {
      { VID_SCALE_1X,      VID_FILTER_NEAREST,  "1x"               },
      { VID_SCALE_FIT,     VID_FILTER_NEAREST,  "fit"              },
      { VID_SCALE_FIT,     VID_FILTER_BILINEAR, "fit bilinear"     },
      { VID_SCALE_STRETCH, VID_FILTER_NEAREST,  "stretch"          },
      { VID_SCALE_STRETCH, VID_FILTER_BILINEAR, "stretch bilinear" },
   };
   /* The smoothed presets take the player's own smoothing (g_smooth). */
   static const char *const sharp_name[5] = {
      "1x", "fit", "fit sharp", "stretch", "stretch sharp"
   };
   int i;
   for (i = 0; i < 5; i++)
      if (preset[i].s == g_scale &&
          (preset[i].f ? g_smooth : VID_FILTER_NEAREST) == g_filter)
         break;
   i = (i + 1) % 5;
   g_scale  = preset[i].s;
   g_filter = preset[i].f ? g_smooth : VID_FILTER_NEAREST;
   return g_smooth == VID_FILTER_SHARP ? sharp_name[i] : preset[i].name;
}

static void dest_geo(unsigned w, unsigned h, vid_geo *g)
{
   g->sy0 = 0;
   g->sh  = (int)h;
   switch (g_scale)
   {
   case VID_SCALE_INT2:
      /* Exact 2x: 2w x 2h, cropped to the screen's 272 rows by drawing only
       * the middle 136 source rows.  Cropping in the SOURCE (texture v) keeps
       * every vertex on screen and every output row exactly two source-row
       * copies -- no partial row at either edge.  A source wider than 240
       * cannot double into 480; draw it 1x rather than crop sideways. */
      if (2u * w > VID_SCR_W)
      {
         g->dw = (int)w;
         g->dh = (int)h;
         break;
      }
      if (2u * h > VID_SCR_H)
      {
         g->sh  = VID_SCR_H / 2;
         g->sy0 = ((int)h - g->sh) / 2;
      }
      g->dw = 2 * (int)w;
      g->dh = 2 * g->sh;
      break;
   default:
      {
         int ox, oy;
         dest_rect(w, h, &ox, &oy, &g->dw, &g->dh);
      }
      break;
   }
   g->ox = (VID_SCR_W - g->dw) / 2;
   g->oy = (VID_SCR_H - g->dh) / 2;
}

/* GU filter for the game picture: 2x is nearest whatever the setting says. */
static int game_filter(void)
{
   if (g_scale == VID_SCALE_INT2)
      return GU_NEAREST;
   return g_filter ? GU_LINEAR : GU_NEAREST;
}

static void dest_rect(unsigned w, unsigned h,
                      int *ox, int *oy, int *dw, int *dh)
{
   switch (g_scale)
   {
   case VID_SCALE_FIT:
      /* Fill height, preserve aspect: 240x160 -> 408x272 (1.7x). */
      *dh = VID_SCR_H;
      *dw = (int)((unsigned)VID_SCR_H * w / h);
      if (*dw > VID_SCR_W)
         *dw = VID_SCR_W;
      break;
   case VID_SCALE_STRETCH:
      *dw = VID_SCR_W;
      *dh = VID_SCR_H;
      break;
   default:
      *dw = (int)w;
      *dh = (int)h;
      break;
   }
   *ox = (VID_SCR_W - *dw) / 2;
   *oy = (VID_SCR_H - *dh) / 2;
}

/* The picture as it has always been drawn: the clear (or the ambient bars in
 * its place), then the staged texture at the destination rect in one pass,
 * filtered `filt`.  Both blit paths share it; the GE list it builds is the
 * one they built inline before sharp bilinear existed, command for command.
 * sharp_draw() is the only other way a game picture is drawn. */
static void game_draw(const uint16_t *tex, unsigned w, const vid_geo *g,
                      int filt)
{
   int x;

   /* Ambient bars REPLACE the clear: the background sprite covers every
    * pixel the clear would have, so the GE fills the same count either way. */
   if (!amb_draw_bg(g))
      sceGuClear(GU_COLOR_BUFFER_BIT);
   sceGuDisable(GU_BLEND);
   sceGuEnable(GU_TEXTURE_2D);
   sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
   sceGuTexImage(0, 256, 256, STAGE_STRIDE, tex);
   sceGuTexFilter(filt, filt);
   sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);

   /* 60-src-px strips (GE texture-cache friendliness); consecutive strips
    * share exact dest boundaries, so scaling leaves no seams. */
   for (x = 0; x < (int)w; x += 60)
   {
      int sw = ((int)w - x < 60) ? (int)w - x : 60;
      int dx0 = g->ox + x * g->dw / (int)w;
      int dx1 = g->ox + (x + sw) * g->dw / (int)w;
      vtx_t *v = (vtx_t *)sceGuGetMemory(2 * sizeof(vtx_t));
      v[0].u = (short)x;        v[0].v = (short)g->sy0;
      v[0].x = (short)dx0;      v[0].y = (short)g->oy;           v[0].z = 0;
      v[1].u = (short)(x + sw); v[1].v = (short)(g->sy0 + g->sh);
      v[1].x = (short)dx1;      v[1].y = (short)(g->oy + g->dh); v[1].z = 0;
      sceGuDrawArray(GU_SPRITES,
                     GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                     2, 0, v);
   }
}

void vid_draw_frame(const uint16_t *pix, unsigned w, unsigned h,
                    size_t pitch_bytes)
{
   int filt;
   unsigned t0, t1, t2, t3, d;
   vid_geo g;

   dest_geo(w, h, &g);
   filt = picture_filter(w, &g);

   /* ADR-0034: the two halves are priced separately because they answer
    * different questions.  `stage` is the CPU copy + (mode 0 only) the cache
    * writeback — the part the placement A/B moves.  `gu` is the list build
    * plus the sceGuSync that BLOCKS until the GE has finished rasterising,
    * which no placement change can shorten. */
   t0 = sceKernelGetSystemTimeLow();
   stage_convert_rgb565(pix, w, h, pitch_bytes);
   t1 = sceKernelGetSystemTimeLow();
   d  = t1 - t0;                        /* u32 subtraction: wrap-safe */
   g_stage_us_tot += d;
   if (d > g_stage_us_max)
      g_stage_us_max = d;

   sceGuStart(GU_DIRECT, gu_next_list());
   VID_GU_TARGET();
   VID_FB_FILLED();
   if (!sharp_draw(fb_staging, w, &g))
      game_draw(fb_staging, w, &g, filt);

   /* ADR-0038: bracket the sync SEPARATELY.  Everything above this point is
    * CPU work building the list; everything below is the CPU stopped dead
    * waiting on the GE.  Only the second number is recoverable, and until
    * ADR-0038 the profile summed them and called the total `gu`.
    *
    * Under gu_defer the wait is not here at all — gu_end_list() leaves the
    * list running and vid_gu_flush() charges whatever is left of it to the
    * same `wait` accumulator, after the vblank has already absorbed most of
    * it.  So `wait` stays comparable across the A/B and `gu` becomes almost
    * pure list-building. */
   t2 = sceKernelGetSystemTimeLow();
   gu_end_list();
   t3 = sceKernelGetSystemTimeLow();

   if (!g_gu_defer)
   {
      d = t3 - t2;
      g_wait_us_tot += d;
      if (d > g_wait_us_max)
         g_wait_us_max = d;
   }

   d = t3 - t1;
   g_gu_us_tot += d;
   if (d > g_gu_us_max)
      g_gu_us_max = d;
   g_blit_frames++;
   /* Outside the blit_prof brackets: a snapshot is a one-off, priced by its
    * own EVT line, and must not read as a blit regression. */
   amb_snap_maybe(pix, (int)(pitch_bytes / 2), w, h, 0);
}

/* ADR-0080: draw a frame that something else (the Media Engine) already
 * staged into a 256-texel-stride buffer in main RAM.  This is the GE-list
 * half of vid_draw_frame with the CPU copy amputated: 8 state calls, 4
 * sprite strips, one deferred-or-inline sync — ~53 us of CPU.  `staged`
 * must already be coherent in RAM (the ME writes it uncached).  Costs are
 * charged to the same gu/wait accumulators so `EVT blit_prof` remains the
 * before/after instrument (HANDOVER: do not break it); `stage` reads ~0
 * in ME mode, which is itself the signal that the offload is live. */
void vid_draw_prestaged(const uint16_t *staged, unsigned w, unsigned h)
{
   int filt;
   unsigned t1, t2, t3, d;
   vid_geo g;

   dest_geo(w, h, &g);
   filt = picture_filter(w, &g);
   t1 = sceKernelGetSystemTimeLow();

   sceGuStart(GU_DIRECT, gu_next_list());
   VID_GU_TARGET();
   VID_FB_FILLED();
   if (!sharp_draw(staged, w, &g))
      game_draw(staged, w, &g, filt);

   t2 = sceKernelGetSystemTimeLow();
   gu_end_list();
   t3 = sceKernelGetSystemTimeLow();
   if (!g_gu_defer)
   {
      d = t3 - t2;
      g_wait_us_tot += d;
      if (d > g_wait_us_max)
         g_wait_us_max = d;
   }
   d = t3 - t1;
   g_gu_us_tot += d;
   if (d > g_gu_us_max)
      g_gu_us_max = d;
   g_blit_frames++;
   /* The ME wrote `staged` behind the D-cache: read it uncached. */
   amb_snap_maybe(staged, STAGE_STRIDE, w, h, 1);
}

/* ---------------------------------------------------------------- overlay */

void vid_overlay_begin(int clear)
{
   sceGuStart(GU_DIRECT, gu_next_list());
   VID_GU_TARGET();
   if (clear)
   {
      /* A full-screen clear makes this buffer presentable on its own: menus
       * and the wake overlay own the whole frame.  A non-clearing overlay (the
       * OSD) does not -- see g_fb_filled. */
      VID_FB_FILLED();
      sceGuClear(GU_COLOR_BUFFER_BIT);
   }
   sceGuEnable(GU_BLEND);
   sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

void vid_rect(int x, int y, int w, int h, uint16_t rgb565, int alpha)
{
   cvtx_t *v = (cvtx_t *)sceGuGetMemory(2 * sizeof(cvtx_t));
   unsigned int col = rgb565_to_abgr(rgb565, alpha);
   sceGuDisable(GU_TEXTURE_2D);
   v[0].color = col; v[0].x = (short)x;       v[0].y = (short)y;       v[0].z = 0;
   v[1].color = col; v[1].x = (short)(x + w); v[1].y = (short)(y + h); v[1].z = 0;
   sceGuDrawArray(GU_SPRITES,
                  GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
}

/* Same idea, but the ALPHA is what ramps, at a constant colour.
 *
 * The marquee footer was a stack of four 8 px rects with alpha stepping
 * 40/85/130/175.  On the dark theme the steps hide in the artwork; on the
 * light one they are four visible bands of white across the bottom of the
 * picture.  The GE interpolates vertex alpha exactly as it interpolates
 * colour, so one strip gives a true per-pixel ramp for the same cost. */
void vid_gradient_a(int x, int y, int w, int h,
                    uint16_t rgb565, int a_top, int a_bot)
{
   cvtx_t *v = (cvtx_t *)sceGuGetMemory(4 * sizeof(cvtx_t));
   unsigned int ct = rgb565_to_abgr(rgb565, a_top);
   unsigned int cb = rgb565_to_abgr(rgb565, a_bot);
   sceGuDisable(GU_TEXTURE_2D);
   v[0].color = ct; v[0].x = (short)x;       v[0].y = (short)y;       v[0].z = 0;
   v[1].color = ct; v[1].x = (short)(x + w); v[1].y = (short)y;       v[1].z = 0;
   v[2].color = cb; v[2].x = (short)x;       v[2].y = (short)(y + h); v[2].z = 0;
   v[3].color = cb; v[3].x = (short)(x + w); v[3].y = (short)(y + h); v[3].z = 0;
   sceGuShadeModel(GU_SMOOTH);
   sceGuDrawArray(GU_TRIANGLE_STRIP,
                  GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, 0, v);
}

void vid_gradient(int x, int y, int w, int h,
                  uint16_t top565, uint16_t bot565, int alpha)
{
   /* GU_SPRITES is flat-shaded (the second vertex's colour wins), so a
    * gradient needs a real primitive the GE interpolates: one strip, four
    * vertices, two colours. */
   cvtx_t *v = (cvtx_t *)sceGuGetMemory(4 * sizeof(cvtx_t));
   unsigned int ct = rgb565_to_abgr(top565, alpha);
   unsigned int cb = rgb565_to_abgr(bot565, alpha);
   sceGuDisable(GU_TEXTURE_2D);
   v[0].color = ct; v[0].x = (short)x;       v[0].y = (short)y;       v[0].z = 0;
   v[1].color = ct; v[1].x = (short)(x + w); v[1].y = (short)y;       v[1].z = 0;
   v[2].color = cb; v[2].x = (short)x;       v[2].y = (short)(y + h); v[2].z = 0;
   v[3].color = cb; v[3].x = (short)(x + w); v[3].y = (short)(y + h); v[3].z = 0;
   sceGuShadeModel(GU_SMOOTH);
   sceGuDrawArray(GU_TRIANGLE_STRIP,
                  GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, 0, v);
}

/* `texw/texh` are the ALLOCATED texture dimensions (the GE only samples
 * power-of-two textures); `srcw/srch` is the sub-rect actually filled, so
 * portrait art can live in the top-left of a square allocation.  The old
 * signature hardcoded a 128x128 texture, which is why anything else drew
 * garbage. */
/* vid_image onto the same rect the emulator draws the game into, so a frame
 * shown behind the wake overlay lands exactly where the player last saw it --
 * letterboxed or stretched according to their own scale preset.
 *
 * Separate from vid_draw_prestaged because that one opens its OWN display
 * list (sceGuStart), which cannot be nested inside vid_overlay_begin's: the
 * nesting silently produced a list that drew nothing at all. */
/* The source sub-rect [0,srcw) x [v0,v1) of a texture, filtered `filt`. */
static void image_rows(int x, int y, int w, int h, const uint16_t *pix,
                       int texw, int texh, int srcw, int v0, int v1,
                       int alpha, int filt)
{
   tcvtx_t *v = (tcvtx_t *)sceGuGetMemory(2 * sizeof(tcvtx_t));
   unsigned int col = 0x00FFFFFFu | ((unsigned int)(alpha & 0xFF) << 24);
   sceGuEnable(GU_TEXTURE_2D);
   sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
   sceGuTexImage(0, texw, texh, texw, pix);
   sceGuTexFilter(filt, filt);
   sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
   v[0].u = 0;            v[0].v = (short)v0;    v[0].color = col;
   v[0].x = (short)x;     v[0].y = (short)y;     v[0].z = 0;
   v[1].u = (short)srcw;  v[1].v = (short)v1;    v[1].color = col;
   v[1].x = (short)(x + w); v[1].y = (short)(y + h); v[1].z = 0;
   sceGuDrawArray(GU_SPRITES,
                  GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT |
                  GU_TRANSFORM_2D, 2, 0, v);
}

void vid_image_screen(const uint16_t *pix, int texw, int texh,
                      int srcw, int srch, int alpha)
{
   /* Same geometry -- and the same 2x crop -- as the live picture. */
   vid_geo g;
   dest_geo((unsigned)srcw, (unsigned)srch, &g);
   if (alpha < 255)
   {
      image_rows(g.ox, g.oy, g.dw, g.dh, pix, texw, texh, srcw, g.sy0,
                 g.sy0 + g.sh, alpha, game_filter());
      return;
   }
   /* OPAQUE: the picture exactly as the player last saw it.  The in-game
    * menu (docs/UI-OVERLAY.md) sits on this, so it goes through the very
    * code the live frame does -- sharp_draw() or game_draw(): the same
    * filter, the same strips, the same ambient bars -- and opening the menu
    * does not visibly resample the game behind it.  (It was one GU_LINEAR
    * quad, which softened a nearest picture the moment the menu came up.)
    * The snapshot is a 256-stride texture like the staging buffer. */
   if (texw != STAGE_STRIDE || !sharp_draw(pix, (unsigned)srcw, &g))
   {
      if (texw == STAGE_STRIDE)
         game_draw(pix, (unsigned)srcw, &g, picture_filter((unsigned)srcw, &g));
      else
         image_rows(g.ox, g.oy, g.dw, g.dh, pix, texw, texh, srcw, g.sy0,
                    g.sy0 + g.sh, 255, game_filter());
   }
   sceGuEnable(GU_BLEND);
}

/* A texture drawn 1:1 with NEAREST sampling: every texel is one pixel. */
void vid_image_px(int x, int y, int w, int h, const uint16_t *pix,
                  int texw, int texh)
{
   image_rows(x, y, w, h, pix, texw, texh, w, 0, h, 255, GU_NEAREST);
}


void vid_image(int x, int y, int w, int h, const uint16_t *pix,
               int texw, int texh, int srcw, int srch, int alpha)
{
   image_rows(x, y, w, h, pix, texw, texh, srcw, 0, srch, alpha, GU_LINEAR);
}

/* ---- anti-aliased UI text ------------------------------------------------
 *
 * The 8x16 1-bit bitmap font that used to live here has no anti-aliasing and
 * a fixed 8 px advance, which is what made every label look chewed at the
 * edges.  RetroShell PSP solves this the way the PSP's own GE wants it
 * solved, and so do we now: Inter is baked on the HOST into an 8-bit
 * coverage atlas (tools/bake_font.py -> font_ui.h), uploaded as GU_PSM_T8,
 * and paired with a CLUT whose entry i is (i << 24) | 0x00FFFFFF -- so the
 * palette index IS the alpha and the glyph is pure coverage.  GU_TFX_MODULATE
 * then multiplies it by the vertex colour, which is where the text colour
 * comes from.  One sprite per glyph, same as before; the GE does the
 * blending it was always doing.
 *
 * Baking on the host rather than shipping a .ttf keeps the EBOOT
 * self-contained -- there is no font file for a user to lose, and no
 * runtime rasteriser.
 *
 * Two faces: `ui` (Inter Regular 15px) for everything, `hd` (Inter SemiBold
 * 19px) for screen titles.  Text is proportional now, so LAYOUT MUST ASK:
 * vid_text_w() replaces strlen(s) * FE_FONT_W everywhere.
 */

/* CLUT must be 16-byte aligned and written back before the GE reads it. */
static unsigned int fu_clut[256] __attribute__((aligned(16)));
static int fu_clut_ready;

static void fu_clut_build(void)
{
   int i;
   for (i = 0; i < 256; i++)
      fu_clut[i] = ((unsigned int)i << 24) | 0x00FFFFFFu;
   sceKernelDcacheWritebackRange(fu_clut, sizeof(fu_clut));
   sceKernelDcacheWritebackRange((void *)fu_ui_a, sizeof(fu_ui_a));
   sceKernelDcacheWritebackRange((void *)fu_hd_a, sizeof(fu_hd_a));
   fu_clut_ready = 1;
}

typedef struct {
   const fu_glyph      *g;
   const unsigned char *a;
   int                  aw, ah, asc;
} fu_face;

static fu_face fu_get(int hd)
{
   fu_face f;
   if (hd) {
      f.g = fu_hd_g; f.a = fu_hd_a;
      f.aw = FU_HD_W; f.ah = FU_HD_H; f.asc = FU_HD_ASC;
   } else {
      f.g = fu_ui_g; f.a = fu_ui_a;
      f.aw = FU_UI_W; f.ah = FU_UI_H; f.asc = FU_UI_ASC;
   }
   return f;
}

/* FU_LAST is past 0x7E: bytes 0x7F..FU_LAST are the button glyphs
 * (VID_GLYPH_*), contiguous with ASCII so the same subtraction indexes
 * them.  Anything beyond that range still draws as '?'. */
static int fu_measure(const fu_face *f, const char *s)
{
   int w = 0;
   for (; *s; s++)
   {
      unsigned char c = (unsigned char)*s;
      if (c < FU_FIRST || c > FU_LAST)
         c = '?';
      w += f->g[c - FU_FIRST].xadv;
   }
   return w;
}

int vid_text_w(const char *str)
{
   fu_face f = fu_get(0);
   return fu_measure(&f, str);
}

int vid_text_hd_w(const char *str)
{
   fu_face f = fu_get(1);
   return fu_measure(&f, str);
}

/* OPTICAL CENTRING (docs/UI-OVERLAY.md §10).  A line of UI text drawn at y
 * has a 16 px box, but its ink does not fill it: Inter's cap height is rows
 * y+4..y+14 (the 'H' glyph of the atlas: yoff 4, 11 rows; the baseline is
 * y+15), the x-height y+7..y+14, descenders to y+17.  Centring the 16 px box
 * in a band -- what every band did, give or take -- puts the letters 1.5 px
 * low in a 24 px band and on the floor of a 20 px one.  These centre the CAP
 * HEIGHT instead.  When the spare rows are odd the extra one goes BELOW, the
 * side the lowercase body and the descenders weigh down. */
static void fu_cap(int *top, int *h)
{
   const fu_glyph *g = &fu_ui_g['H' - FU_FIRST];
   *top = g->yoff;
   *h   = g->h;
}

int vid_band_y(int text_y, int band_h)
{
   int top, h;
   fu_cap(&top, &h);
   return text_y + top - (band_h - h) / 2;
}

int vid_text_y_in(int box_y, int box_h)
{
   int top, h;
   fu_cap(&top, &h);
   return box_y + (box_h - h) / 2 - top;
}

/* y is the TOP of the line box, as it was with the bitmap font, so every
 * existing call site keeps its coordinates. */
static void fu_draw(const fu_face *f, int x, int y, const char *str,
                    unsigned int col)
{
   int n = 0, i, cx = x;
   const char *s;
   tcvtx_t *v;

   if (!fu_clut_ready)
      fu_clut_build();

   for (s = str; *s; s++)
   {
      unsigned char c = (unsigned char)*s;
      if (c < FU_FIRST || c > FU_LAST)
         c = '?';
      if (f->g[c - FU_FIRST].w)
         n++;
      if (n >= 96)
         break;
   }
   if (!n)
      return;

   sceGuEnable(GU_TEXTURE_2D);
   sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
   sceGuClutLoad(256 / 8, fu_clut);
   sceGuTexMode(GU_PSM_T8, 0, 0, GU_FALSE);
   sceGuTexImage(0, f->aw, f->ah, f->aw, f->a);
   /* NEAREST: the atlas is baked at its display size, so any filtering can
    * only blur glyphs that are already correctly sampled 1:1. */
   sceGuTexFilter(GU_NEAREST, GU_NEAREST);
   sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);

   v = (tcvtx_t *)sceGuGetMemory(2 * n * sizeof(tcvtx_t));
   i = 0;
   for (s = str; *s; s++)
   {
      unsigned char c = (unsigned char)*s;
      const fu_glyph *g;
      int gx, gy;
      if (c < FU_FIRST || c > FU_LAST)
         c = '?';
      g = &f->g[c - FU_FIRST];
      if (!g->w)
      {
         cx += g->xadv;
         continue;
      }
      if (i >= n)
         break;
      gx = cx + g->xoff;
      /* yoff is measured from the TOP OF THE LINE BOX, not the baseline
       * (that is what PIL getbbox reports, and what the baker writes), so
       * it already carries the ascent.  Adding f->asc here shifted every
       * glyph down by a full ascent and pushed the footer off screen. */
      gy = y + g->yoff;
      v[i * 2 + 0].u = (short)g->x;          v[i * 2 + 0].v = (short)g->y;
      v[i * 2 + 0].color = col;
      v[i * 2 + 0].x = (short)gx;            v[i * 2 + 0].y = (short)gy;
      v[i * 2 + 0].z = 0;
      v[i * 2 + 1].u = (short)(g->x + g->w); v[i * 2 + 1].v = (short)(g->y + g->h);
      v[i * 2 + 1].color = col;
      v[i * 2 + 1].x = (short)(gx + g->w);   v[i * 2 + 1].y = (short)(gy + g->h);
      v[i * 2 + 1].z = 0;
      cx += g->xadv;
      i++;
   }
   sceGuDrawArray(GU_SPRITES,
                  GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT |
                  GU_TRANSFORM_2D, 2 * i, 0, v);
}

/* The wordmark, drawn exactly like a glyph: 8-bit coverage through the
 * alpha CLUT, tinted by the vertex colour.  So it follows the theme
 * accent for free -- white on the dark palette, black on the light one --
 * instead of needing a recoloured bitmap per theme. */
void vid_logo(int x, int y, uint16_t rgb565, int alpha)
{
   tcvtx_t *v;
   unsigned int col = rgb565_to_abgr(rgb565, alpha & 0xFF);
   if (!fu_clut_ready)
      fu_clut_build();
   sceKernelDcacheWritebackRange((void *)logo_a, sizeof(logo_a));
   sceGuEnable(GU_TEXTURE_2D);
   sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
   sceGuClutLoad(256 / 8, fu_clut);
   sceGuTexMode(GU_PSM_T8, 0, 0, GU_FALSE);
   sceGuTexImage(0, LOGO_TEX_W, LOGO_TEX_H, LOGO_TEX_W, logo_a);
   sceGuTexFilter(GU_LINEAR, GU_LINEAR);
   sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
   v = (tcvtx_t *)sceGuGetMemory(2 * sizeof(tcvtx_t));
   v[0].u = 0;              v[0].v = 0;
   v[0].color = col;        v[0].x = (short)x;  v[0].y = (short)y;
   v[0].z = 0;
   v[1].u = LOGO_W;         v[1].v = LOGO_H;
   v[1].color = col;        v[1].x = (short)(x + LOGO_W);
   v[1].y = (short)(y + LOGO_H); v[1].z = 0;
   sceGuDrawArray(GU_SPRITES,
                  GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT |
                  GU_TRANSFORM_2D, 2, 0, v);
}

void vid_text(int x, int y, const char *str, uint16_t rgb565)
{
   fu_face f = fu_get(0);
   fu_draw(&f, x, y, str, rgb565_to_abgr(rgb565, 255));
}

void vid_text_hd(int x, int y, const char *str, uint16_t rgb565)
{
   fu_face f = fu_get(1);
   fu_draw(&f, x, y, str, rgb565_to_abgr(rgb565, 255));
}

/* Restrict drawing to a rectangle.  The GE has a scissor; using it is how
 * text scrolls UNDER an edge.  Painting rectangles over the overflow
 * instead -- the first attempt -- puts opaque blocks on top of whatever
 * art is behind, which on the marquee is the whole point of the screen. */
void vid_clip(int x, int y, int w, int h)
{
   /* sceGuScissor takes a WIDTH and HEIGHT in this SDK (it sends
    * x + w - 1, y + h - 1; disassembled from libpspgu.a).  This passed the
    * end corner, which only came out right at x = y = 0: every clip with a
    * top edge ran on by its own y (the settings viewport, 74..246, cut at
    * 319 -- i.e. not at all) and every clip with a left edge by its x. */
   sceGuScissor(x, y, w, h);
}

void vid_clip_off(void)
{
   sceGuScissor(0, 0, VID_SCR_W, VID_SCR_H);
}

void vid_text_center(int y, const char *str, uint16_t rgb565)
{
   vid_text((VID_SCR_W - vid_text_w(str)) / 2, y, str, rgb565);
}

void vid_overlay_end(void)
{
   sceGuDisable(GU_BLEND);
   gu_end_list();
}

/* ------------------------------------------------------------- swap/dump */

/* Black out EVERY display buffer with plain uncached stores.
 *
 * For the suspend path only.  The LCD shows whatever is in VRAM the moment it
 * powers back on, which is the frame from before the sleep -- so a console
 * woken from standby flashes a stale frame of gameplay before the wake overlay
 * can draw.  Blanking at suspend replaces that with black, which is what a
 * sleeping device should look like anyway.
 *
 * Deliberately NOT a GE clear: this runs inside the power callback, where the
 * machine is already being taken away and issuing display lists is asking for
 * trouble.  Two memsets through the 0x44000000 uncached alias cannot race the
 * GE or leave a list unfinished. */
void vid_blank_all(void)
{
   /* VID_NBUF, not a hardcoded 2.  With triple buffering the old constant left
    * the THIRD buffer holding whatever was in VRAM, which is the other half of
    * the FF black-flash bug: an unwritten buffer 2 is exactly what the display
    * latched when the ring over-rotated.  See g_fb_filled. */
   memset((void *)0x44000000u, 0, (size_t)FB_BYTES * VID_NBUF);
}

void vid_swap(void)
{
#ifdef VID_TRIPLE
   void *display;
   uintptr_t edram = (uintptr_t)sceGeEdramGetAddr();
   int width, format;
   unsigned next;
#endif
   /* ADR-0040: swapping while the GE is still drawing shows a half-rendered
    * frame.  Unconditional, so the invariant does not depend on gu_defer. */
   vid_gu_flush();
#ifdef VID_TRIPLE
   /* NOTHING DREW THIS FRAME: hold what is on screen.  Do not present and do
    * not advance the ring -- rotating here is what put an unwritten buffer on
    * the display during fast-forward (see g_fb_filled).  Holding the current
    * frame is also the right look for a skipped frame, and it costs nothing:
    * no syscall, no flip. */
   if (!g_fb_filled)
      return;

   /* sceGuSwapBuffers() only knows two buffers, so drive the flip directly.
    * NEXTFRAME latches at vblank exactly as it did before -- the change is
    * WHICH buffer we start drawing into next, not when the flip happens. */
   if (sceDisplaySetFrameBuf((void *)(edram + g_fb_off[g_fb_cur]),
                         FB_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_565,
                         PSP_DISPLAY_SETBUF_NEXTFRAME) < 0)
      return;                         /* retain the complete frame for retry */

   /* Read AFTER submitting: an older pending frame can latch until the
    * SetFrameBuf above replaces it. From here the LCD can only keep the
    * current frame or advance to the one we just submitted. Excluding both
    * is safe even if vblank happens during/after GetFrameBuf. Querying the
    * NEXTFRAME address instead would protect only the pending frame.
    * Compare physical addresses because the SDK may return a VRAM alias. */
   next = (g_fb_cur + 1u) % VID_NBUF;
   if (sceDisplayGetFrameBuf(&display, &width, &format,
                            PSP_DISPLAY_SETBUF_IMMEDIATE) == 0)
   {
      while (next == g_fb_cur ||
             ((edram + g_fb_off[next]) & 0x1fffffffu) ==
             ((uintptr_t)display & 0x1fffffffu))
         next = (next + 1u) % VID_NBUF;
   }
   else
   {
      /* Without the current address, wait for our successful submission
       * to latch before reusing another buffer. Only the API-error path
       * waits; uncapped FF remains independent of the LCD refresh rate. */
      sceDisplayWaitVblankStart();
   }
   g_fb_cur    = next;
   g_draw_off  = g_fb_off[g_fb_cur];
   g_fb_filled = 0;                      /* the new target is empty again */
#else
   sceGuSwapBuffers();
   g_draw_off ^= FB_BYTES;
#endif
}

int vid_dump_ge(const char *path)
{
   const uint16_t *vram =
      (const uint16_t *)(0x44000000u + g_draw_off);   /* uncached VRAM */

   /* ADR-0040: the readback is the other operation that must not race the
    * GE.  Without this the colour test would sample a partly-drawn frame
    * under gu_defer and fail intermittently — which is the good outcome;
    * silently dumping torn pixels is the bad one. */
   vid_gu_flush();
   return fe_bmp_write_psp565(path, vram, VID_SCR_W, VID_SCR_H,
                              FB_STRIDE * 2);
}

/* ===========================================================================
 * AMBIENT BARS (docs/DISPLAY-FEATURES.md, look in ambient_look.h)
 *
 * THE TEXTURE LIVES IN VRAM, NOT IN RAM.  The 2 MiB of eDRAM hold the
 * display buffers, the depth buffer and the 82 KiB staging buffer -- 1.17 MiB
 * with triple buffering -- and nothing else, on every model.  The 32 KiB
 * texture goes above that.  A static array would have been simpler, but on a
 * PSP the BSS comes out of the same user partition as the heap
 * (PSP_HEAP_SIZE_KB(-1024) is "all of it but 1 MiB"), so 32 KiB of BSS is
 * 32 KiB less for the core's ROM-cache loop -- the PSP-1000 post-load budget
 * the core exhausts by design.  VRAM costs it nothing.  The offset does not
 * depend on blit_mode, so a staging fallback to RAM cannot move it.
 *
 *   [display x VID_NBUF][depth][staging 82 KiB][ambient 32 KiB]
 *
 * PER FRAME: the bars are drawn IN PLACE OF the clear the frame already
 * paid for -- one textured sprite per bar (2 at Fit, 4 at 1x, 0 at Stretch
 * and GBA 2x), GU_LINEAR, the scrim folded into the texture function as a
 * MODULATE colour.  Fewer pixels than the clear, no blending, no CPU work
 * beyond the vertices.  The bake is the only per-pixel CPU work, and it runs
 * once.
 * ======================================================================== */

#define AMB_TEX_W   128          /* power-of-two home for AMB_W x AMB_H */
#define AMB_TEX_H   128
#define AMB_TEX_BYTES (AMB_TEX_W * AMB_TEX_H * 2)

typedef char amb_w_fits[(AMB_W + 1 <= AMB_TEX_W && AMB_W % 4 == 0) ? 1 : -1];
typedef char amb_h_fits[(AMB_H + 1 <= AMB_TEX_H) ? 1 : -1];

enum { AMB_SRC_NONE = 0, AMB_SRC_ART = 1, AMB_SRC_SNAP = 2 };

static uint16_t *g_amb_tex;      /* uncached VRAM, NULL until first bake */
static int       g_amb_mode;     /* PCFG_AMB_* (0 off, 1 art, 2 art|game) */
static int       g_amb_src;      /* AMB_SRC_* of what g_amb_tex holds     */
static int       g_amb_scrim;    /* alpha of the black scrim for g_amb_src */
static uint16_t  g_amb_shade;    /* palette fallback, libretro RGB565      */
static int       g_amb_have_shade;
static unsigned  g_amb_frames;   /* presented frames, snapshot clock      */
static int       g_amb_snap_done;

typedef struct
{
   float u, v;
   float x, y, z;
} ftvtx_t;

/* Byte offset of the first free VRAM byte above the staging buffer. */
static uintptr_t vram_free_base(void)
{
   uintptr_t off = (uintptr_t)FB_BYTES * (VID_NBUF + 1u);
   off = (off + 63u) & ~(uintptr_t)63u;
   off += (uintptr_t)STAGE_BYTES;
   return (off + 63u) & ~(uintptr_t)63u;
}

/* Uncached pointer to [off, off+bytes) of VRAM, or NULL if it does not fit. */
static uint16_t *vram_at(uintptr_t off, uintptr_t bytes)
{
   unsigned size = sceGeEdramGetSize();
   if (!size || off + bytes > (uintptr_t)size)
      return NULL;
   return (uint16_t *)(0x40000000u |
                       ((uintptr_t)sceGeEdramGetAddr() + off));
}

void vid_ambient_mode(int mode)
{
   g_amb_mode = (mode >= 0 && mode <= 2) ? mode : 0;
   g_amb_frames = 0;             /* a newly enabled snapshot waits again */
}

void vid_ambient_fallback(uint16_t rgb565)
{
   g_amb_shade = rgb565;
   g_amb_have_shade = 1;
}

int vid_ambient_source(void)
{
   return g_amb_src;
}

void vid_ambient_resnap(void)
{
   /* Only a snapshot is ever retaken; art is the art. */
   if (g_amb_src == AMB_SRC_SNAP || (g_amb_src == AMB_SRC_NONE &&
                                     g_amb_snap_done))
   {
      g_amb_snap_done = 0;
      g_amb_frames = AMB_SNAP_AFTER - 1;   /* the next presented frame */
      if (g_amb_src == AMB_SRC_SNAP)
         g_amb_src = AMB_SRC_NONE;          /* palette until it lands */
   }
}

void vid_ambient_drop(void)
{
   if (g_amb_src == AMB_SRC_ART)
      g_amb_src = AMB_SRC_NONE;
}

int vid_ambient_image(int x, int y, int w, int h, int alpha)
{
   if (!g_amb_tex || g_amb_src != AMB_SRC_ART)
      return 0;
   vid_image(x, y, w, h, g_amb_tex, AMB_TEX_W, AMB_TEX_H, AMB_W, AMB_H,
             alpha);
   return 1;
}

/* One bar: screen rect [x0,x1) x [y0,y1), textured from the matching part
 * of the screen-sized texture, or a vertical palette ramp when tex == 0. */
static void amb_bar(int x0, int y0, int x1, int y1, int textured,
                    unsigned int top, unsigned int bot)
{
   if (x1 <= x0 || y1 <= y0)
      return;
   if (textured)
   {
      ftvtx_t *v = (ftvtx_t *)sceGuGetMemory(2 * sizeof(ftvtx_t));
      v[0].u = (float)x0 * AMB_W / VID_SCR_W;
      v[0].v = (float)y0 * AMB_H / VID_SCR_H;
      v[0].x = (float)x0; v[0].y = (float)y0; v[0].z = 0.0f;
      v[1].u = (float)x1 * AMB_W / VID_SCR_W;
      v[1].v = (float)y1 * AMB_H / VID_SCR_H;
      v[1].x = (float)x1; v[1].y = (float)y1; v[1].z = 0.0f;
      sceGuDrawArray(GU_SPRITES,
                     GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                     2, 0, v);
   }
   else
   {
      cvtx_t *v = (cvtx_t *)sceGuGetMemory(4 * sizeof(cvtx_t));
      v[0].color = top; v[0].x = (short)x0; v[0].y = (short)y0; v[0].z = 0;
      v[1].color = top; v[1].x = (short)x1; v[1].y = (short)y0; v[1].z = 0;
      v[2].color = bot; v[2].x = (short)x0; v[2].y = (short)y1; v[2].z = 0;
      v[3].color = bot; v[3].x = (short)x1; v[3].y = (short)y1; v[3].z = 0;
      sceGuDrawArray(GU_TRIANGLE_STRIP,
                     GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                     4, 0, v);
   }
}

/* `c` darkened as a black scrim of alpha `a` over it would leave it. */
static unsigned int amb_dim(uint16_t c, int a)
{
   unsigned int abgr = rgb565_to_abgr(c, 255);
   unsigned k = 255u - (unsigned)a;
   unsigned r = (abgr & 0xFF) * k / 255u;
   unsigned g = ((abgr >> 8) & 0xFF) * k / 255u;
   unsigned b = ((abgr >> 16) & 0xFF) * k / 255u;
   return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* Draw the bars around g, in place of the clear.  Returns 0 (the caller
 * clears as before) when ambient is off or the picture covers the screen. */
static int amb_draw_bg(const vid_geo *g)
{
   int textured, x0 = g->ox, x1 = g->ox + g->dw;
   int y0 = g->oy, y1 = g->oy + g->dh;
   unsigned int top = 0, bot = 0;

   if (!g_amb_mode)
      return 0;
   if (x0 <= 0 && y0 <= 0 && x1 >= VID_SCR_W && y1 >= VID_SCR_H)
      return 0;                  /* no bars: stretch, or 2x on a GBA */
   textured = g_amb_tex && (g_amb_src == AMB_SRC_ART ||
                            (g_amb_src == AMB_SRC_SNAP && g_amb_mode == 2));
   if (!textured && !g_amb_have_shade)
      return 0;
   if (x0 < 0) x0 = 0;
   if (y0 < 0) y0 = 0;
   if (x1 > VID_SCR_W) x1 = VID_SCR_W;
   if (y1 > VID_SCR_H) y1 = VID_SCR_H;

   sceGuDisable(GU_BLEND);
   if (textured)
   {
      unsigned k = 255u - (unsigned)g_amb_scrim;
      sceGuEnable(GU_TEXTURE_2D);
      sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
      sceGuTexImage(0, AMB_TEX_W, AMB_TEX_H, AMB_TEX_W, g_amb_tex);
      sceGuTexFilter(GU_LINEAR, GU_LINEAR);
      /* The scrim, for free: texel x (k,k,k) is the black-alpha blend. */
      sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
      sceGuColor(0xFF000000u | (k << 16) | (k << 8) | k);
   }
   else
   {
      sceGuDisable(GU_TEXTURE_2D);
      sceGuShadeModel(GU_SMOOTH);
      top = amb_dim(g_amb_shade, AMB_PAL_RAMP_TOP);
      bot = amb_dim(g_amb_shade, AMB_PAL_RAMP_BOT);
   }
   /* Full-height side bars, then the top and bottom between them.  A ramp
    * is interpolated over each bar's own height, so the top/bottom bars of a
    * 1x picture would restart it -- give them the colour at their rows. */
   amb_bar(0, 0, x0, VID_SCR_H, textured, top, bot);
   amb_bar(x1, 0, VID_SCR_W, VID_SCR_H, textured, top, bot);
   if (!textured)
   {
      unsigned int mid0 = amb_dim(g_amb_shade, AMB_PAL_RAMP_TOP +
                          (AMB_PAL_RAMP_BOT - AMB_PAL_RAMP_TOP) * y0 /
                          VID_SCR_H);
      unsigned int mid1 = amb_dim(g_amb_shade, AMB_PAL_RAMP_TOP +
                          (AMB_PAL_RAMP_BOT - AMB_PAL_RAMP_TOP) * y1 /
                          VID_SCR_H);
      amb_bar(x0, 0, x1, y0, 0, top, mid0);
      amb_bar(x0, y1, x1, VID_SCR_H, 0, mid1, bot);
   }
   else
   {
      amb_bar(x0, 0, x1, y0, 1, 0, 0);
      amb_bar(x0, y1, x1, VID_SCR_H, 1, 0, 0);
   }
   /* The game quad that follows sets its own texture state; restore what it
    * does not set. */
   sceGuColor(0xFFFFFFFFu);
   sceGuEnable(GU_TEXTURE_2D);
   return 1;
}

/* One separable box pass over an AMB_W x AMB_H x 3 byte image, edge-clamped,
 * radius r, along x or along y.  `line` holds one row or column. */
static void amb_box(unsigned char *img, unsigned char *line, int r, int along_x)
{
   int n_lines = along_x ? AMB_H : AMB_W;
   int len     = along_x ? AMB_W : AMB_H;
   int step    = along_x ? 3 : AMB_W * 3;
   int li, i, c, div = 2 * r + 1;

   for (li = 0; li < n_lines; li++)
   {
      unsigned char *base = img + (along_x ? li * AMB_W * 3 : li * 3);
      for (i = 0; i < len; i++)
         for (c = 0; c < 3; c++)
            line[i * 3 + c] = base[i * step + c];
      for (c = 0; c < 3; c++)
      {
         int sum = 0, k;
         for (k = -r; k <= r; k++)
         {
            int j = k < 0 ? 0 : (k >= len ? len - 1 : k);
            sum += line[j * 3 + c];
         }
         for (i = 0; i < len; i++)
         {
            int out = i - r, in = i + r + 1;
            base[i * step + c] = (unsigned char)(sum / div);
            if (out < 0) out = 0;
            if (in >= len) in = len - 1;
            sum += line[in * 3 + c] - line[out * 3 + c];
         }
      }
   }
}

static int amb_clamp255(int v)
{
   return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* THE BAKE.  Cover-crop `src` to the screen's 480:272, box-average it into
 * AMB_W x AMB_H (at most `max_taps` samples per axis per texel; the blur that
 * follows hides the rest), blur, grade, write VRAM.  The working image is
 * 24 KiB on the STACK (main thread, 256 KiB): a static would be BSS, which
 * is heap on a PSP (see the section comment).
 *
 * `luma_min` > 0 rejects a too-dark image (the snapshot's "is there a
 * picture yet" test) and returns 1 without touching the texture.
 * Returns 0 on success, -1 with no VRAM room. */
static int amb_bake(const uint16_t *src, int stride, int sw, int sh,
                    int max_taps, int luma_min, int libretro_order,
                    int extra_passes)
{
   unsigned char img[AMB_H * AMB_W * 3];
   unsigned char line[(AMB_W > AMB_H ? AMB_W : AMB_H) * 3];
   int cx = 0, cy = 0, cw = sw, ch = sh, tx, ty, p;
   unsigned luma = 0;
   uint16_t *tex;

   if (!src || sw <= 0 || sh <= 0)
      return -1;
   if (!g_amb_tex)
      g_amb_tex = vram_at(vram_free_base(), AMB_TEX_BYTES);
   tex = g_amb_tex;
   if (!tex)
      return -1;                 /* no VRAM room: ambient simply stays off */

   /* Cover crop: the widest/tallest 480:272 window, centred. */
   if ((long)sw * VID_SCR_H > (long)sh * VID_SCR_W)
   {
      cw = (int)((long)sh * VID_SCR_W / VID_SCR_H);
      cx = (sw - cw) / 2;
   }
   else
   {
      ch = (int)((long)sw * VID_SCR_H / VID_SCR_W);
      cy = (sh - ch) / 2;
   }
   if (cw < 1) cw = 1;
   if (ch < 1) ch = 1;

   for (ty = 0; ty < AMB_H; ty++)
   {
      int y0 = cy + ty * ch / AMB_H, y1 = cy + (ty + 1) * ch / AMB_H;
      int ys;
      if (y1 <= y0) y1 = y0 + 1;
      ys = (y1 - y0 + max_taps - 1) / max_taps;
      for (tx = 0; tx < AMB_W; tx++)
      {
         int x0 = cx + tx * cw / AMB_W, x1 = cx + (tx + 1) * cw / AMB_W;
         unsigned r = 0, gg = 0, b = 0, n = 0;
         int xs, x, y;
         if (x1 <= x0) x1 = x0 + 1;
         xs = (x1 - x0 + max_taps - 1) / max_taps;
         /* One tap per axis samples the middle of the block. */
         for (y = (max_taps == 1 ? (y0 + y1) / 2 : y0); y < y1; y += ys)
         {
            for (x = (max_taps == 1 ? (x0 + x1) / 2 : x0); x < x1; x += xs)
            {
               unsigned c = src[(long)y * stride + x];
               unsigned cr = c & 31, cg = (c >> 5) & 63, cb = (c >> 11) & 31;
               if (libretro_order)
               {
                  unsigned t = cr; cr = cb; cb = t;
               }
               r  += (cr << 3) | (cr >> 2);
               gg += (cg << 2) | (cg >> 4);
               b  += (cb << 3) | (cb >> 2);
               n++;
               if (max_taps == 1)
                  break;
            }
            if (max_taps == 1)
               break;
         }
         if (!n) n = 1;
         p = (ty * AMB_W + tx) * 3;
         img[p]     = (unsigned char)(r / n);
         img[p + 1] = (unsigned char)(gg / n);
         img[p + 2] = (unsigned char)(b / n);
         luma += (77u * img[p] + 150u * img[p + 1] + 29u * img[p + 2]) >> 8;
      }
   }
   luma /= (unsigned)(AMB_W * AMB_H);
   if (luma_min > 0 && (int)luma < luma_min)
      return 1;

   for (p = 0; p < AMB_BLUR_PASSES; p++)
   {
      amb_box(img, line, AMB_BLUR_RADIUS, 1);
      amb_box(img, line, AMB_BLUR_RADIUS, 0);
   }
   /* vid_ambient_bake_cover only: a small cover is nearly 1:1 with the
    * texture, so the "soft" pass alone leaves its lettering readable. */
   for (p = 0; p < extra_passes; p++)
   {
      amb_box(img, line, AMB_COVER_BLUR_RADIUS, 1);
      amb_box(img, line, AMB_COVER_BLUR_RADIUS, 0);
   }

   vid_gu_flush();               /* never rewrite a texture mid-draw */
   for (ty = 0; ty <= AMB_H; ty++)
   {
      /* Row AMB_H and column AMB_W repeat the edge: bilinear at the far
       * edge reads one texel past it. */
      int sy = ty < AMB_H ? ty : AMB_H - 1;
      for (tx = 0; tx <= AMB_W; tx++)
      {
         int sx = tx < AMB_W ? tx : AMB_W - 1;
         int r, gg, b, l;
         p = (sy * AMB_W + sx) * 3;
         r = img[p]; gg = img[p + 1]; b = img[p + 2];
         l = (77 * r + 150 * gg + 29 * b) >> 8;
         r  = l + ((r  - l) * AMB_SATURATION >> 8);
         gg = l + ((gg - l) * AMB_SATURATION >> 8);
         b  = l + ((b  - l) * AMB_SATURATION >> 8);
         r  += (AMB_TINT_R - r)  * AMB_TINT_AMOUNT >> 8;
         gg += (AMB_TINT_G - gg) * AMB_TINT_AMOUNT >> 8;
         b  += (AMB_TINT_B - b)  * AMB_TINT_AMOUNT >> 8;
         r = amb_clamp255(r); gg = amb_clamp255(gg); b = amb_clamp255(b);
         /* GE 5650 texel, PSP channel order: R in the low bits. */
         tex[ty * AMB_TEX_W + tx] =
            (uint16_t)((r >> 3) | ((gg >> 2) << 5) | ((b >> 3) << 11));
      }
   }
   return 0;
}

int vid_ambient_bake_art(const uint16_t *src, int stride, int w, int h,
                         int precomposed)
{
   unsigned t0 = sceKernelGetSystemTimeLow(), us;
   int rc = amb_bake(src, stride, w, h, 4, 0, 0, 0);
   us = sceKernelGetSystemTimeLow() - t0;
   g_amb_src = rc == 0 ? AMB_SRC_ART : AMB_SRC_NONE;
   g_amb_scrim = precomposed ? AMB_SCRIM_HERO : AMB_SCRIM_BOXART;
   fe_evt("ambient_bake src=art w=%d h=%d hero=%d rc=%d us=%u vram_off=%u",
          w, h, precomposed, rc, us, (unsigned)vram_free_base());
   FE_EVT_ONLY(us);
   return rc == 0 ? 0 : -1;
}

int vid_ambient_bake_cover(const uint16_t *src, int stride, int w, int h)
{
   unsigned t0 = sceKernelGetSystemTimeLow(), us;
   int rc = amb_bake(src, stride, w, h, 4, 0, 0, AMB_COVER_BLUR_PASSES);
   us = sceKernelGetSystemTimeLow() - t0;
   g_amb_src = rc == 0 ? AMB_SRC_ART : AMB_SRC_NONE;
   g_amb_scrim = AMB_SCRIM_BOXART;
   fe_evt("ambient_bake src=cover w=%d h=%d rc=%d us=%u", w, h, rc, us);
   FE_EVT_ONLY(us);
   return rc == 0 ? 0 : -1;
}

/* The snapshot fallback: called once per presented frame; does nothing
 * unless "art, else game" is on and there is no art.  One tap per texel, so
 * the read is ~8k pixels (uncached for an ME-written stage). */
static void amb_snap_maybe(const uint16_t *src, int stride, unsigned w,
                           unsigned h, int uncached)
{
   unsigned t0, us;
   int rc;
   if (g_amb_mode != 2 || g_amb_src != AMB_SRC_NONE || g_amb_snap_done || !src)
      return;
   g_amb_frames++;
   if (g_amb_frames < AMB_SNAP_AFTER ||
       (g_amb_frames - AMB_SNAP_AFTER) % AMB_SNAP_RETRY)
      return;
   if (g_amb_frames > AMB_SNAP_GIVE_UP)
   {
      g_amb_snap_done = 1;
      fe_evt("ambient_bake src=game rc=gave_up frame=%u", g_amb_frames);
      return;
   }
   if (uncached)
      src = (const uint16_t *)(0x40000000u | (uintptr_t)src);
   t0 = sceKernelGetSystemTimeLow();
#ifdef USE_PSP_RGB565_FORMAT
   rc = amb_bake(src, stride, (int)w, (int)h, 1, AMB_SNAP_MIN_LUMA, 0, 0);
#else
   /* Without ADR-0039 the core emits libretro order (R high); an ME stage
    * is already in the GE's order. */
   rc = amb_bake(src, stride, (int)w, (int)h, 1, AMB_SNAP_MIN_LUMA,
                 !uncached, 0);
#endif
   us = sceKernelGetSystemTimeLow() - t0;
   if (rc == 0)
   {
      g_amb_src = AMB_SRC_SNAP;
      g_amb_scrim = AMB_SCRIM_FRAME;
      g_amb_snap_done = 1;
   }
   else if (rc < 0)
      g_amb_snap_done = 1;       /* no VRAM: stop asking */
   fe_evt("ambient_bake src=game rc=%d frame=%u w=%u h=%u us=%u",
          rc, g_amb_frames, w, h, us);
   FE_EVT_ONLY(us);
}

/* ===========================================================================
 * SHARP BILINEAR (docs/SHARP-BILINEAR.md; VID_FILTER_SHARP, default off)
 *
 * The GE has no shaders, so the "sharp bilinear" look is built from two
 * fixed-function passes inside the blit's own display list:
 *
 *   1. PRESCALE.  Draw the staged frame NEAREST at an integer factor kx x ky
 *      into a render target in VRAM.  k = ceil(final scale) per axis, so the
 *      target is at least as large as the picture: 240x160 -> 480x320 at Fit
 *      and Stretch, 160x144 -> 320x288 (Fit) or 480x288 (Stretch).
 *   2. RESAMPLE.  Draw that target with GU_LINEAR at the final size.  Inside
 *      a k x k block every tap reads the same colour, so only the output
 *      pixel straddling a source-pixel edge is blended: crisp, square pixels
 *      with no nearest-neighbour shimmer (the uneven 1-2 px columns of a 1.7x
 *      nearest scale crawl when the picture scrolls).
 *
 * At an exact integer scale (1x, 2x) the passes would change nothing, so the
 * picture is drawn NEAREST through game_draw(), pixel-identical to "nearest".
 *
 * VRAM: [display x VID_NBUF][depth][staging][ambient 32 KiB][THIS]
 * 512 x SHARP_ROWS x 2 = 336 KiB, 8 KiB aligned, at offset 1,236,992 with
 * triple buffering; it ends at 1,581,056 of 2,097,152.  Taken on the first
 * sharp frame and never freed (VRAM is not shared with anything else).  If
 * it ever does not fit, sharp falls back to plain bilinear.
 *
 * The prescale replaces the full-screen clear with black sprites over the
 * bars only (or the ambient bars, which already do that), because the
 * picture covers everything else.  That pays back part of pass 1's fill.
 * ======================================================================== */

#define SHARP_STRIDE 512          /* render-target and texture row, texels */
#define SHARP_ROWS   336          /* >= 2 x 160 + 1 (GBA) and 2 x 144 + 1   */
#define SHARP_BYTES  (SHARP_STRIDE * SHARP_ROWS * 2)

/* GE drawing-region commands (pspsdk's private drawRegion uses these). */
#define GE_CMD_REGION1 21
#define GE_CMD_REGION2 22

static uintptr_t g_sharp_off;     /* VRAM byte offset of the target        */
static uint16_t *g_sharp_tex;     /* uncached pointer to it, NULL = none   */
static int       g_sharp_tried;

/* The integer prescale for picture geometry g of a w-wide source.
 * Returns 0 when the scale is already an integer on both axes (draw nearest),
 * 1 with kx and ky set when the two passes apply, -1 when the target would not
 * fit (draw bilinear).  Pure: the host geometry test calls it. */
static int sharp_factors(unsigned w, const vid_geo *g, int *kx, int *ky)
{
   int sw = (int)w, sh = g->sh, x, y;

   if (sw <= 0 || sh <= 0 || g->dw <= 0 || g->dh <= 0)
      return -1;
   if (g->dw % sw == 0 && g->dh % sh == 0)
      return 0;
   x = (g->dw + sw - 1) / sw;
   y = (g->dh + sh - 1) / sh;
   /* +1: the duplicated edge column and row (see sharp_draw). */
   if (x * sw + 1 > SHARP_STRIDE || y * sh + 1 > SHARP_ROWS)
      return -1;
   if (kx) *kx = x;
   if (ky) *ky = y;
   return 1;
}

/* The GU filter for the one-pass picture (game_draw).  2x is nearest
 * whatever the setting; sharp at an integer scale is nearest; sharp that
 * cannot prescale is bilinear. */
static int picture_filter(unsigned w, const vid_geo *g)
{
   if (g_filter == VID_FILTER_SHARP && g_scale != VID_SCALE_INT2)
      return sharp_factors(w, g, NULL, NULL) == 0 ? GU_NEAREST : GU_LINEAR;
   return game_filter();
}

static uint16_t *sharp_target(void)
{
   if (!g_sharp_tried)
   {
      uintptr_t off = vram_free_base() + (uintptr_t)AMB_TEX_BYTES;
      off = (off + 0x1FFFu) & ~(uintptr_t)0x1FFFu;
      g_sharp_tried = 1;
      g_sharp_off   = off;
      g_sharp_tex   = vram_at(off, SHARP_BYTES);
      fe_evt("sharp_target rc=%d vram_off=%u bytes=%u",
             g_sharp_tex ? 0 : -1, (unsigned)off, (unsigned)SHARP_BYTES);
   }
   return g_sharp_tex;
}

/* Black over [x0,x1) x [y0,y1), untextured. */
static void sharp_black(int x0, int y0, int x1, int y1)
{
   cvtx_t *v;
   if (x1 <= x0 || y1 <= y0)
      return;
   v = (cvtx_t *)sceGuGetMemory(2 * sizeof(cvtx_t));
   v[0].color = 0xFF000000u; v[0].x = (short)x0; v[0].y = (short)y0; v[0].z = 0;
   v[1].color = 0xFF000000u; v[1].x = (short)x1; v[1].y = (short)y1; v[1].z = 0;
   sceGuDrawArray(GU_SPRITES,
                  GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
}

/* One nearest/linear sprite: texels [u0,u1) x [v0,v1) onto [x0,x1) x [y0,y1). */
static void sharp_quad(int u0, int v0, int u1, int v1,
                       int x0, int y0, int x1, int y1)
{
   vtx_t *v = (vtx_t *)sceGuGetMemory(2 * sizeof(vtx_t));
   v[0].u = (short)u0; v[0].v = (short)v0;
   v[0].x = (short)x0; v[0].y = (short)y0; v[0].z = 0;
   v[1].u = (short)u1; v[1].v = (short)v1;
   v[1].x = (short)x1; v[1].y = (short)y1; v[1].z = 0;
   sceGuDrawArray(GU_SPRITES,
                  GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                  2, 0, v);
}

/* Draw the picture sharp-bilinear into the current list.  Returns 0 (and
 * draws nothing) when sharp is off or does not apply; the caller then draws
 * game_draw() exactly as before. */
static int sharp_draw(const uint16_t *tex, unsigned w, const vid_geo *g)
{
   int kx, ky, iw, ih, x, x0, y0, x1, y1;
   int last = g->sy0 + g->sh;     /* one past the last source row drawn */
   uint16_t *rt;

   if (g_filter != VID_FILTER_SHARP || g_scale == VID_SCALE_INT2)
      return 0;
   if (sharp_factors(w, g, &kx, &ky) != 1)
      return 0;
   rt = sharp_target();
   if (!rt)
      return 0;
   iw = kx * (int)w;
   ih = ky * g->sh;

   /* 0. Background: the ambient bars, else black bars -- never the
    *    full-screen clear, since the picture covers the rest. */
   if (!amb_draw_bg(g))
   {
      x0 = g->ox < 0 ? 0 : g->ox;
      y0 = g->oy < 0 ? 0 : g->oy;
      x1 = g->ox + g->dw > VID_SCR_W ? VID_SCR_W : g->ox + g->dw;
      y1 = g->oy + g->dh > VID_SCR_H ? VID_SCR_H : g->oy + g->dh;
      sceGuDisable(GU_BLEND);
      sceGuDisable(GU_TEXTURE_2D);
      sharp_black(0, 0, x0, VID_SCR_H);
      sharp_black(x1, 0, VID_SCR_W, VID_SCR_H);
      sharp_black(x0, 0, x1, y0);
      sharp_black(x0, y1, x1, VID_SCR_H);
   }

   /* 1. Prescale, nearest, into the render target.  Scissor AND drawing
    *    region are opened to the target: the screen's 480x272 would clip
    *    the 320-row GBA target. */
   sceGuDrawBufferList(GU_PSM_5650, (void *)g_sharp_off, SHARP_STRIDE);
   sceGuScissor(0, 0, iw + 1, ih + 1);
   sceGuSendCommandi(GE_CMD_REGION1, 0);
   sceGuSendCommandi(GE_CMD_REGION2, (ih << 10) | iw);
   /* The depth buffer is screen-sized (272 rows at stride 512) and the
    * staging texture sits directly after it, so a depth write on target row
    * 272+ would land in the texture being read.  Depth test is off, which
    * already means no depth writes; mask them anyway for this pass. */
   sceGuDepthMask(GU_TRUE);
   sceGuDisable(GU_BLEND);
   sceGuEnable(GU_TEXTURE_2D);
   sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
   sceGuTexImage(0, 256, 256, STAGE_STRIDE, tex);
   sceGuTexFilter(GU_NEAREST, GU_NEAREST);
   sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
   for (x = 0; x < (int)w; x += 60)
   {
      int sw = ((int)w - x < 60) ? (int)w - x : 60;
      sharp_quad(x, g->sy0, x + sw, last, x * kx, 0, (x + sw) * kx, ih);
   }
   /* The last column and row once more, one texel past the picture: a
    * bilinear tap at the far edge then reads the edge colour, whatever the
    * GE's sampling offset turns out to be, never stale VRAM. */
   sharp_quad((int)w - 1, g->sy0, (int)w, last, iw, 0, iw + 1, ih);
   sharp_quad(0, last - 1, (int)w, last, 0, ih, iw, ih + 1);
   sharp_quad((int)w - 1, last - 1, (int)w, last, iw, ih, iw + 1, ih + 1);

   /* 2. Back to the display buffer; resample the target, linear. */
   sceGuDrawBufferList(GU_PSM_5650, (void *)(uintptr_t)g_draw_off, FB_STRIDE);
   sceGuDepthMask(GU_FALSE);
   sceGuScissor(0, 0, VID_SCR_W, VID_SCR_H);
   sceGuSendCommandi(GE_CMD_REGION1, 0);
   sceGuSendCommandi(GE_CMD_REGION2, ((VID_SCR_H - 1) << 10) | (VID_SCR_W - 1));
   /* Pass 1's pixels must be in VRAM before pass 2 samples them, and the
    * texture cache may still hold LAST frame's target. */
   sceGuTexSync();
   sceGuTexImage(0, 512, 512, SHARP_STRIDE, rt);
   sceGuTexFlush();
   sceGuTexFilter(GU_LINEAR, GU_LINEAR);
   /* 64-texel strips; consecutive strips share exact dest boundaries. */
   for (x = 0; x < iw; x += 64)
   {
      int sw = (iw - x < 64) ? iw - x : 64;
      sharp_quad(x, 0, x + sw, ih,
                 g->ox + x * g->dw / iw, g->oy,
                 g->ox + (x + sw) * g->dw / iw, g->oy + g->dh);
   }
   return 1;
}
