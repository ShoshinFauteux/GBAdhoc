/* shot_name.h -- screenshot file names (GitHub issue #13):
 *
 *     <game title>_<YYYY-MM-DD>_<HH-MM-SS>.png      (or .bmp)
 *     <game title>_<YYYY-MM-DD>_<HH-MM-SS>_2.png    (a second shot that second)
 *
 * The title is the ROM's file name without its folder or extension, made
 * safe for FAT and for the XMB's Photo list: letters with accents become
 * plain ASCII letters (text_fold.h -- a name the firmware has to convert is
 * exactly the kind that went missing in issue #19), the characters FAT
 * refuses (\ / : * ? " < > |) and control characters become '_', runs of
 * them collapse to one, and leading/trailing spaces, dots and '_' go (FAT
 * drops a trailing dot or space silently, which would change the name under
 * us).  At most SHOT_TITLE_MAX bytes.  An empty result is "GBAdhoc".
 *
 * Pure C; tools/tests/test_shot_name.c runs it on the host. */
#ifndef PSP_SHOT_NAME_H
#define PSP_SHOT_NAME_H

#include <stdio.h>
#include <string.h>
#include "text_fold.h"

#define SHOT_TITLE_MAX 64

static inline int shot_bad_char(unsigned char c)
{
   return c < 0x20 || c == 0x7F || c >= 0x80 || strchr("\\/:*?\"<>|", c);
}

static inline void shot_title_from_rom(const char *rom, char *out, size_t cap)
{
   char folded[SHOT_TITLE_MAX * 4 + 1];
   const char *base, *dot;
   char stem[sizeof(folded)];
   size_t n, i, k = 0;
   int last_bad = 0;

   if (!out || cap < 2)
      return;
   out[0] = '\0';
   if (!rom)
      rom = "";
   base = strrchr(rom, '/');
   base = base ? base + 1 : rom;
   {
      const char *bs = strrchr(base, '\\');
      if (bs)
         base = bs + 1;
   }
   dot = strrchr(base, '.');
   n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
   if (n > sizeof(stem) - 1)
      n = sizeof(stem) - 1;
   memcpy(stem, base, n);
   stem[n] = '\0';
   utf8_trim_partial(stem);
   utf8_fold_copy(folded, sizeof(folded), stem);
   for (i = 0; folded[i] && k + 1 < cap && k < SHOT_TITLE_MAX; i++)
   {
      unsigned char c = (unsigned char)folded[i];
      if (shot_bad_char(c))
      {
         if (!last_bad && k > 0)
            out[k++] = '_';
         last_bad = 1;
         continue;
      }
      last_bad = 0;
      out[k++] = (char)c;
   }
   while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == '.' ||
                    out[k - 1] == '_'))
      k--;
   out[k] = '\0';
   for (i = 0; out[i] == ' ' || out[i] == '.' || out[i] == '_'; i++)
      ;
   if (i)
      memmove(out, out + i, k - i + 1);
   if (!out[0])
      snprintf(out, cap, "GBAdhoc");
}

/* The file name (no folder).  seq <= 1: none; 2, 3, ...: "_2", "_3".
 * 0 on success, -1 if it did not fit. */
static inline int shot_file_name(char *out, size_t cap, const char *title,
                                 int year, int mon, int day, int hour,
                                 int min, int sec, unsigned seq,
                                 const char *ext)
{
   char sfx[16] = "";
   int n;
   if (seq > 1)
      snprintf(sfx, sizeof(sfx), "_%u", seq);
   n = snprintf(out, cap, "%s_%04d-%02d-%02d_%02d-%02d-%02d%s.%s", title,
                year, mon, day, hour, min, sec, sfx, ext);
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

#endif
