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
void vid_image_screen(const uint16_t *pix, int texw, int texh,
                      int srcw, int srch, int alpha)
{
   int ox, oy, dw, dh;
   dest_rect((unsigned)srcw, (unsigned)srch, &ox, &oy, &dw, &dh);
   vid_image(ox, oy, dw, dh, pix, texw, texh, srcw, srch, alpha);
}

void vid_image(int x, int y, int w, int h, const uint16_t *pix,
               int texw, int texh, int srcw, int srch, int alpha)
{
   tcvtx_t *v = (tcvtx_t *)sceGuGetMemory(2 * sizeof(tcvtx_t));
   unsigned int col = 0x00FFFFFFu | ((unsigned int)(alpha & 0xFF) << 24);
   sceGuEnable(GU_TEXTURE_2D);
   sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
   sceGuTexImage(0, texw, texh, texw, pix);
   sceGuTexFilter(GU_LINEAR, GU_LINEAR);
   sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
   v[0].u = 0;            v[0].v = 0;            v[0].color = col;
   v[0].x = (short)x;     v[0].y = (short)y;     v[0].z = 0;
   v[1].u = (short)srcw;  v[1].v = (short)srch;  v[1].color = col;
   v[1].x = (short)(x + w); v[1].y = (short)(y + h); v[1].z = 0;
   sceGuDrawArray(GU_SPRITES,
                  GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT |
                  GU_TRANSFORM_2D, 2, 0, v);
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
   sceGuScissor(x, y, x + w, y + h);
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

/* Black out BOTH display buffers with plain uncached stores.
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
   memset((void *)0x44000000u, 0, (size_t)FB_BYTES * 2u);
}

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
