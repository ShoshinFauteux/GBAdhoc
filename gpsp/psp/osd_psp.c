/* osd_psp.c — toast queue + status chips (see osd_psp.h). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "osd_psp.h"
#include "video_psp.h"
#include "font_8x16.h"

#define TOAST_MAX     4
#define TOAST_FRAMES  150   /* ~2.5 s per toast */
#define TOAST_LEN     56

#define COL_TEXT   0xFFFF   /* white          */
#define COL_PANEL  0x0000   /* black          */
#define COL_ACCENT 0x65BF   /* soft blue      */
#define COL_FF     0xFEA0   /* warm yellow    */

static char     toast_q[TOAST_MAX][TOAST_LEN];
static unsigned toast_head, toast_count;
static int      toast_timer;

static char chip_session_txt[32];
static char chip_ff_txt[16];
/* Wide enough for the full diagnostic line, e.g.
 * "24.0 fps (24)  s1700 f1% x48000 r0" (34 chars).  This was 24 and silently
 * truncated the profiler fields mid-string — the chip looked like it simply
 * had no x/r counters.  480px / 8px per glyph = 60 chars fit on screen. */
static char chip_fps_txt[64];

void osd_toast(const char *fmt, ...)
{
   va_list ap;
   unsigned slot;

   if (toast_count == TOAST_MAX)   /* full: drop the oldest */
   {
      toast_head = (toast_head + 1) % TOAST_MAX;
      toast_count--;
      toast_timer = TOAST_FRAMES;
   }
   slot = (toast_head + toast_count) % TOAST_MAX;
   va_start(ap, fmt);
   vsnprintf(toast_q[slot], TOAST_LEN, fmt, ap);
   va_end(ap);
   if (toast_count == 0)
      toast_timer = TOAST_FRAMES;
   toast_count++;
}

void osd_chip_session(const char *text)
{
   if (text && text[0])
      snprintf(chip_session_txt, sizeof(chip_session_txt), "%s", text);
   else
      chip_session_txt[0] = '\0';
}

void osd_chip_ff(const char *text)
{
   if (text && text[0])
      snprintf(chip_ff_txt, sizeof(chip_ff_txt), "%s", text);
   else
      chip_ff_txt[0] = '\0';
}

void osd_chip_fps(const char *text)
{
   if (text && text[0])
      snprintf(chip_fps_txt, sizeof(chip_fps_txt), "%s", text);
   else
      chip_fps_txt[0] = '\0';
}

void osd_draw(void)
{
   int any = toast_count || chip_session_txt[0] || chip_ff_txt[0] ||
             chip_fps_txt[0];
   if (!any)
      return;

   vid_overlay_begin(0);

   if (toast_count)
   {
      const char *msg = toast_q[toast_head];
      int tw = (int)strlen(msg) * FE_FONT_W;
      int px = (VID_SCR_W - tw) / 2 - 8;
      int pw = tw + 16;
      vid_rect(px, 6, pw, FE_FONT_H + 8, COL_PANEL, 200);
      vid_rect(px, 6, pw, 2, COL_ACCENT, 255);
      vid_text_center(10, msg, COL_TEXT);

      if (--toast_timer <= 0)
      {
         toast_head = (toast_head + 1) % TOAST_MAX;
         toast_count--;
         toast_timer = TOAST_FRAMES;
      }
   }

   if (chip_fps_txt[0])
   {
      /* Top-left, mirroring the FF chip's top-right — the two are designed
       * to be on screen together (FPS is most interesting DURING FF). */
      int tw = (int)strlen(chip_fps_txt) * FE_FONT_W;
      vid_rect(10, 6, tw + 12, FE_FONT_H + 4, COL_PANEL, 180);
      vid_text(16, 8, chip_fps_txt, COL_TEXT);
   }

   if (chip_ff_txt[0])
   {
      int tw = (int)strlen(chip_ff_txt) * FE_FONT_W;
      int px = VID_SCR_W - tw - 14;
      vid_rect(px - 4, 6, tw + 12, FE_FONT_H + 4, COL_PANEL, 180);
      vid_text(px + 2, 8, chip_ff_txt, COL_FF);
   }

   if (chip_session_txt[0])
   {
      int tw = (int)strlen(chip_session_txt) * FE_FONT_W;
      int px = VID_SCR_W - tw - 14;
      int py = VID_SCR_H - FE_FONT_H - 8;
      vid_rect(px - 4, py - 2, tw + 12, FE_FONT_H + 4, COL_PANEL, 180);
      vid_text(px + 2, py, chip_session_txt, COL_ACCENT);
   }

   vid_overlay_end();
}
