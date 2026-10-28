/* text_fold.h -- file names that are not ASCII (GitHub issue #19).
 *
 * TWO SEPARATE JOBS, KEPT SEPARATE ON PURPOSE:
 *
 *  1. DISPLAY.  The UI fonts are baked ASCII plus the PSP button glyphs at
 *     0x7F..0x84 (font_ui.h).  A file name is UTF-8 (PPSSPP; the firmware's
 *     FAT driver for long names), so "Pokémon" arrives as 'P','o','k',0xC3,
 *     0xA9,... -- and 0xA9 is not a glyph, while 0x80..0x84 continuation
 *     bytes WOULD draw as button glyphs.  utf8_fold_next() reads one whole
 *     UTF-8 sequence and gives back ONE display byte: the Latin letter
 *     without its accent (é -> e, ñ -> n, ü -> u, ß -> s), or '?' for
 *     anything with no close ASCII letter (Japanese, symbols).  A lone byte
 *     that is not a valid sequence is passed through unchanged, so the
 *     single-byte button glyphs keep working exactly as before.  The fold is
 *     for DRAWING ONLY: the path, the save names and box art keys always use
 *     the real bytes.
 *
 *  2. FINDING AND OPENING.  fat_rom_pick() decides, for one directory entry,
 *     which name to OPEN the ROM by and which to SHOW.  The long name is
 *     preferred; if it does not carry the console's extension (a long name
 *     the firmware could not convert may arrive cut short), or it is not
 *     ASCII and does not stat, the FAT 8.3 short name the firmware reports
 *     through SceIoDirent.d_private is used to open it instead -- and the
 *     long name, when there is a usable one, is still what the list shows.
 *
 * Pure C, no SDK: tools/tests/test_text_fold.c runs it on the host. */
#ifndef PSP_TEXT_FOLD_H
#define PSP_TEXT_FOLD_H

#include <stddef.h>
#include <string.h>

/* U+00C0..U+00FF, one ASCII letter each ('?' where there is none). */
static const char TEXT_FOLD_LATIN1[64 + 1] =
   "AAAAAAACEEEEIIII"      /* C0..CF  À Á Â Ã Ä Å Æ Ç È É Ê Ë Ì Í Î Ï */
   "DNOOOOOxOUUUUYPs"      /* D0..DF  Ð Ñ Ò Ó Ô Õ Ö × Ø Ù Ú Û Ü Ý Þ ß */
   "aaaaaaaceeeeiiii"      /* E0..EF */
   "dnooooo?ouuuuypy";     /* F0..FF  ð ñ ò ó ô õ ö ÷ ø ù ú û ü ý þ ÿ */

/* U+0100..U+017F (Latin Extended-A): base letter of each code point. */
static const char TEXT_FOLD_EXTA[128 + 1] =
   "AaAaAaCcCcCcCcDd"      /* 0100 */
   "DdEeEeEeEeEeGgGg"      /* 0110 */
   "GgGgHhHhIiIiIiIi"      /* 0120 */
   "IiJjJjKkkLlLlLlL"      /* 0130 */
   "lLlNnNnNnnNnOoOo"      /* 0140 */
   "OoOoRrRrRrSsSsSs"      /* 0150 */
   "SsTtTtTtUuUuUuUu"      /* 0160 */
   "UuUuWwYyYZzZzZzs";     /* 0170 */

/* Decode one UTF-8 sequence at *s (strictly: no overlongs, no surrogates,
 * nothing above U+10FFFF).  Returns the code point and advances *s past it;
 * returns -1 and leaves *s alone if the bytes there are not one. */
static inline long utf8_decode(const unsigned char **s)
{
   const unsigned char *p = *s;
   unsigned c = p[0];
   long cp;
   int n, i;
   if (c < 0xC2 || c > 0xF4)
      return -1;
   n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
   cp = c & (0x3F >> n);
   for (i = 1; i <= n; i++)
   {
      if ((p[i] & 0xC0) != 0x80)
         return -1;
      cp = (cp << 6) | (p[i] & 0x3F);
   }
   if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
       (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
      return -1;
   *s = p + n + 1;
   return cp;
}

/* One display byte for the text at *s, advancing *s (by at least one byte;
 * never past the terminating NUL, which the caller tests for). */
static inline unsigned char utf8_fold_next(const char **sp)
{
   const unsigned char *s = (const unsigned char *)*sp;
   long cp = utf8_decode(&s);
   unsigned char out;
   if (cp < 0)
   {
      out = (unsigned char)**sp;     /* a lone byte: drawn as it always was */
      *sp = *sp + 1;
      return out;
   }
   if (cp >= 0xC0 && cp <= 0xFF)
      out = (unsigned char)TEXT_FOLD_LATIN1[cp - 0xC0];
   else if (cp >= 0x100 && cp <= 0x17F)
      out = (unsigned char)TEXT_FOLD_EXTA[cp - 0x100];
   else if (cp == 0xA0 || cp == 0x3000)         /* no-break / ideographic */
      out = ' ';
   else if (cp == 0x2018 || cp == 0x2019)       /* curly single quotes   */
      out = '\'';
   else if (cp == 0x201C || cp == 0x201D)       /* curly double quotes   */
      out = '"';
   else if (cp == 0x2013 || cp == 0x2014)       /* en / em dash          */
      out = '-';
   else if (cp >= 0xFF01 && cp <= 0xFF5E)       /* full-width ASCII      */
      out = (unsigned char)(cp - 0xFF01 + '!');
   else
      out = '?';
   *sp = (const char *)s;
   return out;
}

/* The whole string folded into out (at most cap-1 bytes + NUL). */
static inline void utf8_fold_copy(char *out, size_t cap, const char *in)
{
   size_t n = 0;
   if (!cap)
      return;
   while (in && *in && n + 1 < cap)
      out[n++] = (char)utf8_fold_next(&in);
   out[n] = '\0';
}

/* After a byte-wise cut (a title clipped to fit): drop a trailing UTF-8
 * sequence that the cut left incomplete, so it does not draw as '?'. */
static inline void utf8_trim_partial(char *s)
{
   size_t n = strlen(s), i = n, k = 0;
   while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80 && k < 3)
   {
      i--;
      k++;
   }
   if (i > 0)
   {
      unsigned char c = (unsigned char)s[i - 1];
      int need = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : -1;
      if (c >= 0xC2 && c <= 0xF4 && (int)k < need)
         s[i - 1] = '\0';
   }
}

static inline int text_is_ascii(const char *s)
{
   for (; s && *s; s++)
      if ((unsigned char)*s >= 0x80)
         return 0;
   return 1;
}

/* WHERE THE FIRMWARE PUT THE NAMES IN d_private.  It has two layouts, chosen
 * by the SDK version the EBOOT declares (JPCSP / PPSSPP __IoDread):
 *   old (SDK <= 3.07, or none declared -- this app): [0..12] 8.3 name,
 *       [13..] long name; the size word at [0..3] is not read.
 *   new: [0..3] size (>= 1044), [4..19] 8.3 name, [20..] long name.
 * Reading the new layout from an old-layout block drops the first 7
 * characters of every long name (issue #19's fix did, on hardware: names
 * shown cut and ROMs that would not open).  Which one is in use is decided
 * from the entries themselves: the layout whose long name equals d_name (the
 * firmware's own long name for an ASCII entry) wins and is remembered in
 * *layout for entries that cannot tell (non-ASCII, where d_name is
 * mangled).  *layout: 0 unknown (treated as old), 13 old, 20 new.  `raw` is
 * the whole d_private block, at least FAT_PRIV_BYTES long, NUL-terminated
 * by the caller at FAT_PRIV_BYTES - 1.  Copies the names out NUL-bounded. */
#define FAT_PRIV_BYTES (20 + 1024 + 1)
static inline void fat_private_names(const unsigned char *raw,
                                     const char *d_name, int *layout,
                                     char *long_out, size_t long_cap,
                                     char *short_out, size_t short_cap)
{
   const char *l13 = (const char *)raw + 13, *l20 = (const char *)raw + 20;
   int lay = *layout;
   size_t n;
   if (d_name && d_name[0] && text_is_ascii(d_name))
   {
      if (strcmp(l13, d_name) == 0)
         lay = *layout = 13;
      else if (strcmp(l20, d_name) == 0)
         lay = *layout = 20;
   }
   if (lay != 20)
      lay = 13;
   n = strlen((const char *)raw + lay);
   if (n >= long_cap)
      n = long_cap - 1;
   memcpy(long_out, raw + lay, n);
   long_out[n] = '\0';
   /* the 8.3 name: [0..12] old, [4..19] new; at most 12 characters.  In
    * the old layout [0..3] still holds the size word when the firmware
    * filled only the long name (PPSSPP does), so a name with any byte
    * outside printable ASCII is not one: no short name. */
   n = 0;
   while (n < 12 && n + 1 < short_cap && raw[(lay == 20 ? 4 : 0) + n])
   {
      unsigned char c = raw[(lay == 20 ? 4 : 0) + n];
      if (c < 0x20 || c > 0x7E)
      {
         n = 0;
         break;
      }
      short_out[n] = (char)c;
      n++;
   }
   short_out[n] = '\0';
}

/* Which name a directory entry is opened by, and which is shown.
 *
 *   d_name     SceIoDirent.d_name
 *   long_name  the FAT long name from d_private ("" if not reported)
 *   short_name the FAT 8.3 name from d_private ("" if not reported)
 *   matches    does a name carry the console's ROM extension
 *   opens      does rel_dir/<name> stat (only asked for non-ASCII names)
 *
 * Returns 1 and sets *open_name / *show_name, or 0: not a ROM. */
typedef int (*fat_match_fn)(const char *name, void *ctx);
typedef int (*fat_opens_fn)(const char *name, void *ctx);

static inline int fat_rom_pick(const char *d_name, const char *long_name,
                               const char *short_name, fat_match_fn matches,
                               fat_opens_fn opens, void *ctx,
                               const char **open_name, const char **show_name)
{
   const char *cand[2];
   const char *shown = NULL;
   int i;
   cand[0] = long_name && long_name[0] ? long_name : d_name;
   cand[1] = d_name;
   for (i = 0; i < 2; i++)
   {
      const char *c = cand[i];
      if (!c || !c[0] || !matches(c, ctx))
         continue;
      if (!shown)
         shown = c;
      if (text_is_ascii(c) || opens(c, ctx))
      {
         *open_name = c;
         *show_name = c;
         return 1;
      }
   }
   if (short_name && short_name[0] && matches(short_name, ctx))
   {
      *open_name = short_name;
      *show_name = shown ? shown : short_name;
      return 1;
   }
   /* A non-ASCII long name that did not stat, and no short name: list it
    * anyway (the stat is the only thing that disagreed; the open may not). */
   if (shown)
   {
      *open_name = shown;
      *show_name = shown;
      return 1;
   }
   return 0;
}

#endif
