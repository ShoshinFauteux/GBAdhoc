/* Host regression for the real PSP swap code, with an asynchronous LCD.
 * Compile with the PSP SDK headers and --gc-sections; unused GU/UI code is
 * discarded, so only the display and sync calls below need host stubs.
 * See docs/FF-ARTIFACT-FIX.md for the command and hardware follow-up. */
#include <assert.h>
#include <stdio.h>
#define VID_TRIPLE
#include "../psp/video_psp.c"

static unsigned lcd, pending;
static unsigned latch_schedule, swaps, queries, waits, syncs;
static int fail_set, fail_get;
static unsigned alias;

static void latch(void) { lcd = pending; }
static void *fb(unsigned i)
{
   return (void *)(uintptr_t)(0x04000000u + g_fb_off[i]);
}

void *sceGeEdramGetAddr(void) { return (void *)(uintptr_t)0x04000000u; }
unsigned int sceKernelGetSystemTimeLow(void) { return 0; }
int sceGuSync(int mode, int what)
{
   assert(mode == 0 && what == 0);
   syncs++;
   return 0;
}
int sceDisplaySetFrameBuf(void *addr, int width, int format, int sync)
{
   unsigned i;
   assert(width == FB_STRIDE && format == PSP_DISPLAY_PIXEL_FORMAT_565);
   assert(sync == PSP_DISPLAY_SETBUF_NEXTFRAME);
   assert(g_gu_queued == 0); /* GE must finish before the LCD can latch. */
   swaps++;
   if (latch_schedule & 1) latch();
   if (fail_set) return -1;
   for (i = 0; i < VID_NBUF && fb(i) != addr; i++) {}
   assert(i < VID_NBUF);
   pending = i;
   if (latch_schedule & 2) latch();
   return 0;
}
int sceDisplayGetFrameBuf(void **addr, int *width, int *format, int sync)
{
   assert(sync == PSP_DISPLAY_SETBUF_IMMEDIATE);
   queries++;
   if (fail_get) return -1;
   if (latch_schedule & 4) latch();
   *addr = (void *)((uintptr_t)fb(lcd) | alias);
   *width = FB_STRIDE;
   *format = PSP_DISPLAY_PIXEL_FORMAT_565;
   if (latch_schedule & 8) latch();
   return 0;
}
int sceDisplayWaitVblankStart(void)
{
   waits++;
   latch();
   return 0;
}

static void reset(void)
{
   lcd = pending = 1; /* vid_init: draw 0, display 1 */
   g_fb_cur = g_draw_off = 0;
   g_fb_filled = g_gu_queued = 0;
   swaps = queries = waits = syncs = 0;
   fail_set = fail_get = 0;
   latch_schedule = alias = 0;
}

static void check_draw_target(void)
{
   assert(g_fb_cur < VID_NBUF);
   assert(g_fb_cur != lcd);
   assert(g_fb_cur != pending);
   assert(g_draw_off == g_fb_off[g_fb_cur]);
}

int main(void)
{
   unsigned schedule, i, use_alias;
   /* No latch at all reproduces FF. Every placement of a latch inside the
    * syscalls covers the race at vblank, including cached/uncached aliases. */
   for (use_alias = 0; use_alias < 2; use_alias++)
      for (schedule = 0; schedule < 32; schedule++)
      {
         reset();
         alias = use_alias ? 0x40000000u : 0;
         latch_schedule = schedule;
         for (i = 0; i < 1000; i++)
         {
            unsigned old_swaps = swaps;
            check_draw_target();
            g_fb_filled = (i % 7 != 0); /* skipped frames must hold */
            g_gu_queued = i % 3;       /* zero, one or two GPU lists */
            vid_swap();
            assert(swaps == old_swaps + (i % 7 != 0));
            assert(g_fb_filled == 0);
            check_draw_target();
            if (schedule & 16) latch();
            check_draw_target();
         }
         assert(waits == 0); /* safety must not cap FF to LCD speed */
      }

   reset();
   g_fb_filled = 1;
   fail_set = 1;
   vid_swap();
   assert(g_fb_cur == 0 && g_fb_filled == 1);
   assert(queries == 0 && waits == 0);
   check_draw_target();

   fail_set = 0;
   fail_get = 1;
   vid_swap();
   assert(waits == 1); /* fallback waits until the submitted buffer latches */
   assert(g_fb_filled == 0);
   check_draw_target();
   puts("PASS: 64,000 swap schedules, skipped frames, aliases and API errors");
   return 0;
}
