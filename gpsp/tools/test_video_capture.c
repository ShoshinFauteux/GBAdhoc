/* Host check of the "As displayed" screenshot source in psp/video_psp.c
 * (vid_cap_frame / vid_cap_arm / vid_overlay_begin's grab), against the real
 * file with the triple-buffered swap.  Built like test_video_buffers.c (PSP
 * SDK headers, --gc-sections, host stubs for the GU and display calls).
 *
 * The eDRAM's UNCACHED alias, 0x44000000, is mapped here for real (mmap,
 * MAP_FIXED), so every VRAM read and the grab's copy run as on the PSP.  The
 * GE is simulated: a "blit" fills the draw buffer with a picture keyed on the
 * frame number and bumps g_blit_frames, exactly what vid_draw_prestaged's
 * caller sees; an "OSD" is vid_overlay_begin(0) followed by a stamp of
 * different pixels into the same buffer; a menu frame is
 * vid_overlay_begin(1) and a full repaint.
 *
 * Checks, each against the picture the test knows it put there:
 *   1. a fresh picture, not yet swapped, is read in place (the draw buffer);
 *   2. after the swap, with no overlay, the shown buffer is read;
 *   3. with an OSD on the newest picture nothing qualifies; armed, the next
 *      frame's OSD begin copies the clean picture first, and that copy (not
 *      the OSD-stamped buffer) is what vid_cap_frame then returns;
 *   4. a menu frame never qualifies, and its OSD does not grab;
 *   5. vid_blit_prof's counter reset keeps the answer ("clean" stays clean,
 *      "overlaid" stays overlaid);
 *   6. vid_cap_reset forgets a held copy;
 *   7. no eDRAM room: vid_cap_arm refuses (the caller saves the game
 *      picture instead). */
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#define VID_TRIPLE
#include "../psp/video_psp.c"

#define EDRAM_SIZE 0x200000u
static unsigned edram_size = EDRAM_SIZE;
static unsigned lcd = 1, pending = 1;

void *sceGeEdramGetAddr(void) { return (void *)(uintptr_t)0x04000000u; }
unsigned int sceGeEdramGetSize(void) { return edram_size; }
unsigned int sceKernelGetSystemTimeLow(void) { return 0; }
int sceGuSync(int mode, int what) { (void)mode; (void)what; return 0; }
int sceGuStart(int cid, void *list) { (void)cid; (void)list; return 0; }
int sceGuFinish(void) { return 0; }
void sceGuDrawBuffer(int psm, void *fbp, int fbw) { (void)psm; (void)fbp; (void)fbw; }
void sceGuClear(int flags) { (void)flags; }
void sceGuEnable(int s) { (void)s; }
void sceGuDisable(int s) { (void)s; }
void sceGuBlendFunc(int op, int src, int dest, unsigned int srcfix,
                    unsigned int destfix)
{ (void)op; (void)src; (void)dest; (void)srcfix; (void)destfix; }
int sceDisplaySetFrameBuf(void *addr, int width, int format, int sync)
{
   unsigned i;
   (void)width; (void)format; (void)sync;
   for (i = 0; i < VID_NBUF; i++)
      if ((uintptr_t)addr == 0x04000000u + g_fb_off[i])
         pending = i;
   return 0;
}
int sceDisplayGetFrameBuf(void **addr, int *width, int *format, int sync)
{
   (void)sync;
   *addr = (void *)(uintptr_t)(0x04000000u + g_fb_off[lcd]);
   *width = FB_STRIDE;
   *format = PSP_DISPLAY_PIXEL_FORMAT_565;
   return 0;
}
int sceDisplayWaitVblankStart(void) { lcd = pending; return 0; }

static uint16_t *buf(unsigned i)
{
   return (uint16_t *)(uintptr_t)(0x44000000u + g_fb_off[i]);
}
static uint16_t pic(unsigned frame, unsigned x, unsigned y)
{
   return (uint16_t)(frame * 7919u + y * 480u + x);
}
/* The GE draws game frame `frame` into the draw buffer. */
static void blit(unsigned frame)
{
   unsigned x, y;
   for (y = 0; y < VID_SCR_H; y++)
      for (x = 0; x < FB_STRIDE; x++)
         buf(g_fb_cur)[y * FB_STRIDE + x] = pic(frame, x, y);
   g_fb_filled = 1;
   g_blit_frames++;
}
/* An overlay (OSD chip: clear = 0, a menu: clear = 1) drawn on it. */
static void overlay(int clear)
{
   unsigned x;
   vid_overlay_begin(clear);
   for (x = 0; x < 64; x++)
      buf(g_fb_cur)[8 * FB_STRIDE + x] = 0xFFFF;   /* the chip's pixels */
   if (clear)
      g_fb_filled = 1;
}
static void swap(void)
{
   vid_swap();
   sceDisplayWaitVblankStart();
}
/* The shot equals game frame `frame`, with nothing of an overlay in it. */
static int is_frame(const uint16_t *p, size_t pitch_b, unsigned frame)
{
   unsigned x, y;
   for (y = 0; y < VID_SCR_H; y++)
      for (x = 0; x < VID_SCR_W; x++)
         if (p[y * (pitch_b / 2) + x] != pic(frame, x, y))
            return 0;
   return 1;
}

int main(void)
{
   const uint16_t *p;
   unsigned w = 0, h = 0;
   size_t pitch = 0;
   void *m = mmap((void *)(uintptr_t)0x44000000u, EDRAM_SIZE,
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
   assert(m == (void *)(uintptr_t)0x44000000u);
   g_fb_cur = 0;
   g_draw_off = 0;

   /* nothing drawn yet */
   assert(vid_cap_frame(&w, &h, &pitch) == NULL);

   /* 1. drawn, not yet swapped: the draw buffer itself */
   blit(1);
   p = vid_cap_frame(&w, &h, &pitch);
   assert(p == buf(g_fb_cur) && w == 480 && h == 272 && pitch == 1024);
   assert(is_frame(p, pitch, 1));

   /* 2. swapped without an overlay: the buffer on screen */
   {
      unsigned shown = g_fb_cur;
      swap();
      p = vid_cap_frame(&w, &h, &pitch);
      assert(p == buf(shown) && is_frame(p, pitch, 1));
   }

   /* 3. frame 2 with an OSD chip: nothing qualifies until armed + the next */
   blit(2);
   overlay(0);
   swap();
   assert(vid_cap_frame(&w, &h, &pitch) == NULL);
   assert(vid_cap_arm() == 1);
   blit(3);
   overlay(0);                 /* the grab happens here, before the chip */
   swap();
   blit(4);                    /* later frames do not disturb the copy */
   overlay(0);
   p = vid_cap_frame(&w, &h, &pitch);
   assert(p && pitch == 960 && is_frame(p, pitch, 3));
   /* the copy is in VRAM past the three display buffers, not one of them */
   assert(p != buf(0) && p != buf(1) && p != buf(2));
   assert((uintptr_t)p - 0x44000000u + 480u * 272u * 2u <= EDRAM_SIZE);

   /* 6. reset forgets it */
   vid_cap_reset();
   assert(vid_cap_frame(&w, &h, &pitch) == NULL);   /* frame 4 has a chip */

   /* 4. a menu frame (and its toasts) never qualifies and never grabs */
   swap();
   assert(vid_cap_arm() == 1);
   overlay(1);
   overlay(0);
   swap();
   assert(vid_cap_frame(&w, &h, &pitch) == NULL);
   /* the game comes back: its picture qualifies in place */
   blit(5);
   p = vid_cap_frame(&w, &h, &pitch);
   assert(p == buf(g_fb_cur) && is_frame(p, pitch, 5));
   vid_cap_reset();

   /* 5. the blit counter's reset (blit_prof) keeps the answer */
   vid_blit_prof(NULL, NULL, NULL, NULL, NULL, NULL, NULL);
   p = vid_cap_frame(&w, &h, &pitch);
   assert(p && is_frame(p, pitch, 5));            /* clean stays clean */
   overlay(0);
   vid_blit_prof(NULL, NULL, NULL, NULL, NULL, NULL, NULL);
   assert(vid_cap_frame(&w, &h, &pitch) == NULL); /* overlaid stays so */
   swap();
   assert(vid_cap_frame(&w, &h, &pitch) == NULL);

   /* 7. an eDRAM with no room for the copy: arm refuses */
   g_cap_buf = NULL;
   edram_size = 1u << 20;
   assert(vid_cap_arm() == 0);

   printf("test_video_capture: ok\n");
   return 0;
}
