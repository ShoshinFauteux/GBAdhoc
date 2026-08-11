/* video_psp.c — GU video layer (see video_psp.h). */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspge.h>

#include <stdio.h>
#include <string.h>

#include "video_psp.h"
#include "font_8x16.h"
#include "fe_util.h"

#define FB_STRIDE 512
#define FB_BYTES  (FB_STRIDE * VID_SCR_H * 2)

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

static int g_scale  = VID_SCALE_1X;
static int g_filter = VID_FILTER_NEAREST;

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
 * One extra row + zeroed right margin so bilinear edge taps read black, and
 * GU_CLAMP wrap set at init. */
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
      uintptr_t off  = (uintptr_t)FB_BYTES * 3u;
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
   sceGuDrawBuffer(GU_PSM_5650, (void *)0, FB_STRIDE);
   sceGuDispBuffer(VID_SCR_W, VID_SCR_H, (void *)(uintptr_t)FB_BYTES, FB_STRIDE);
   sceGuDepthBuffer((void *)(uintptr_t)(FB_BYTES * 2), FB_STRIDE);
   sceGuOffset(2048 - (VID_SCR_W / 2), 2048 - (VID_SCR_H / 2));
   sceGuViewport(2048, 2048, VID_SCR_W, VID_SCR_H);
   sceGuScissor(0, 0, VID_SCR_W, VID_SCR_H);
   sceGuEnable(GU_SCISSOR_TEST);
   sceGuDisable(GU_DEPTH_TEST);
   sceGuDisable(GU_BLEND);
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
   g_filter = filter ? VID_FILTER_BILINEAR : VID_FILTER_NEAREST;
}

int vid_scale_mode(void) { return g_scale; }
int vid_filter(void)     { return g_filter; }

const char *vid_scale_name(int scale)
{
   switch (scale)
   {
   case VID_SCALE_FIT:     return "fit";
   case VID_SCALE_STRETCH: return "stretch";
   default:                return "1x";
   }
}

const char *vid_filter_name(int filter)
{
   return filter ? "bilinear" : "nearest";
}

const char *vid_cycle_preset(void)
{
   /* 1x/nearest -> fit/nearest -> fit/bilinear -> stretch/nearest ->
    * stretch/bilinear -> 1x/nearest */
   static const struct { int s, f; const char *name; } preset[5] = {
      { VID_SCALE_1X,      VID_FILTER_NEAREST,  "1x"               },
      { VID_SCALE_FIT,     VID_FILTER_NEAREST,  "fit"              },
      { VID_SCALE_FIT,     VID_FILTER_BILINEAR, "fit bilinear"     },
      { VID_SCALE_STRETCH, VID_FILTER_NEAREST,  "stretch"          },
      { VID_SCALE_STRETCH, VID_FILTER_BILINEAR, "stretch bilinear" },
   };
   int i;
   for (i = 0; i < 5; i++)
      if (preset[i].s == g_scale && preset[i].f == g_filter)
         break;
   i = (i + 1) % 5;
   g_scale  = preset[i].s;
   g_filter = preset[i].f;
   return preset[i].name;
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

void vid_draw_frame(const uint16_t *pix, unsigned w, unsigned h,
                    size_t pitch_bytes)
{
   int x, ox, oy, dw, dh;
   int filt = g_filter ? GU_LINEAR : GU_NEAREST;
   unsigned t0, t1, t2, t3, d;

   dest_rect(w, h, &ox, &oy, &dw, &dh);

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
   sceGuClear(GU_COLOR_BUFFER_BIT);
   sceGuDisable(GU_BLEND);
   sceGuEnable(GU_TEXTURE_2D);
   sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
   sceGuTexImage(0, 256, 256, STAGE_STRIDE, fb_staging);
   sceGuTexFilter(filt, filt);
   sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);

   /* 60-src-px strips (GE texture-cache friendliness); consecutive strips
    * share exact dest boundaries, so scaling leaves no seams. */
   for (x = 0; x < (int)w; x += 60)
   {
      int sw = ((int)w - x < 60) ? (int)w - x : 60;
      int dx0 = ox + x * dw / (int)w;
      int dx1 = ox + (x + sw) * dw / (int)w;
      vtx_t *v = (vtx_t *)sceGuGetMemory(2 * sizeof(vtx_t));
      v[0].u = (short)x;        v[0].v = 0;
      v[0].x = (short)dx0;      v[0].y = (short)oy;        v[0].z = 0;
      v[1].u = (short)(x + sw); v[1].v = (short)h;
      v[1].x = (short)dx1;      v[1].y = (short)(oy + dh); v[1].z = 0;
      sceGuDrawArray(GU_SPRITES,
                     GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                     2, 0, v);
   }

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
   int x, ox, oy, dw, dh;
   int filt = g_filter ? GU_LINEAR : GU_NEAREST;
   unsigned t1, t2, t3, d;

   dest_rect(w, h, &ox, &oy, &dw, &dh);
   t1 = sceKernelGetSystemTimeLow();

   sceGuStart(GU_DIRECT, gu_next_list());
   sceGuClear(GU_COLOR_BUFFER_BIT);
   sceGuDisable(GU_BLEND);
   sceGuEnable(GU_TEXTURE_2D);
   sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
   sceGuTexImage(0, 256, 256, STAGE_STRIDE, staged);
   sceGuTexFilter(filt, filt);
   sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);

   for (x = 0; x < (int)w; x += 60)
   {
      int sw = ((int)w - x < 60) ? (int)w - x : 60;
      int dx0 = ox + x * dw / (int)w;
      int dx1 = ox + (x + sw) * dw / (int)w;
      vtx_t *v = (vtx_t *)sceGuGetMemory(2 * sizeof(vtx_t));
      v[0].u = (short)x;        v[0].v = 0;
      v[0].x = (short)dx0;      v[0].y = (short)oy;        v[0].z = 0;
      v[1].u = (short)(x + sw); v[1].v = (short)h;
      v[1].x = (short)dx1;      v[1].y = (short)(oy + dh); v[1].z = 0;
      sceGuDrawArray(GU_SPRITES,
                     GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                     2, 0, v);
   }

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
}

/* ---------------------------------------------------------------- overlay */

void vid_overlay_begin(int clear)
{
   sceGuStart(GU_DIRECT, gu_next_list());
   if (clear)
      sceGuClear(GU_COLOR_BUFFER_BIT);
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

void vid_image(int x, int y, int w, int h, const uint16_t *pix,
               int tw, int th, int alpha)
{
   tcvtx_t *v = (tcvtx_t *)sceGuGetMemory(2 * sizeof(tcvtx_t));
   unsigned int col = 0x00FFFFFFu | ((unsigned int)(alpha & 0xFF) << 24);
   sceGuEnable(GU_TEXTURE_2D);
   sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
   sceGuTexImage(0, 128, 128, 128, pix);
   sceGuTexFilter(GU_LINEAR, GU_LINEAR);
   sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
   v[0].u = 0;          v[0].v = 0;          v[0].color = col;
   v[0].x = (short)x;   v[0].y = (short)y;   v[0].z = 0;
   v[1].u = (short)tw;  v[1].v = (short)th;  v[1].color = col;
   v[1].x = (short)(x + w); v[1].y = (short)(y + h); v[1].z = 0;
   sceGuDrawArray(GU_SPRITES,
                  GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT |
                  GU_TRANSFORM_2D, 2, 0, v);
}

void vid_text(int x, int y, const char *str, uint16_t rgb565)
{
   unsigned int col = rgb565_to_abgr(rgb565, 255);
   int n = (int)strlen(str);
   int i, cx = x;
   tcvtx_t *v;

   if (n <= 0)
      return;
   if (n > 60)
      n = 60;

   sceGuEnable(GU_TEXTURE_2D);
   sceGuTexMode(GU_PSM_5551, 0, 0, GU_FALSE);
   sceGuTexImage(0, 128, 256, 128, font_tex);
   sceGuTexFilter(GU_NEAREST, GU_NEAREST);
   sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);

   v = (tcvtx_t *)sceGuGetMemory(2 * n * sizeof(tcvtx_t));
   for (i = 0; i < n; i++, cx += FE_FONT_W)
   {
      unsigned char c = (unsigned char)str[i];
      short u0 = (short)((c & 15) * FE_FONT_W);
      short v0 = (short)((c >> 4) * FE_FONT_H);
      v[i * 2 + 0].u = u0;                    v[i * 2 + 0].v = v0;
      v[i * 2 + 0].color = col;
      v[i * 2 + 0].x = (short)cx;             v[i * 2 + 0].y = (short)y;
      v[i * 2 + 0].z = 0;
      v[i * 2 + 1].u = (short)(u0 + FE_FONT_W);
      v[i * 2 + 1].v = (short)(v0 + FE_FONT_H);
      v[i * 2 + 1].color = col;
      v[i * 2 + 1].x = (short)(cx + FE_FONT_W);
      v[i * 2 + 1].y = (short)(y + FE_FONT_H);
      v[i * 2 + 1].z = 0;
   }
   sceGuDrawArray(GU_SPRITES,
                  GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT |
                  GU_TRANSFORM_2D, 2 * n, 0, v);
}

void vid_text_center(int y, const char *str, uint16_t rgb565)
{
   int w = (int)strlen(str) * FE_FONT_W;
   vid_text((VID_SCR_W - w) / 2, y, str, rgb565);
}

void vid_overlay_end(void)
{
   sceGuDisable(GU_BLEND);
   gu_end_list();
}

/* ------------------------------------------------------------- swap/dump */

void vid_swap(void)
{
   /* ADR-0040: swapping while the GE is still drawing shows a half-rendered
    * frame.  Unconditional, so the invariant does not depend on gu_defer. */
   vid_gu_flush();
   sceGuSwapBuffers();
   g_draw_off ^= FB_BYTES;
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
