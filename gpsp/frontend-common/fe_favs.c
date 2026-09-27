/* fe_favs.c — see fe_favs.h. */
#include <string.h>

#include "fe_favs.h"

unsigned fe_favs_hash(const char *s)
{
   unsigned h = 2166136261u;
   for (; *s; s++)
   {
      h ^= (unsigned char)*s;
      h *= 16777619u;
   }
   return h;
}

void fe_favs_clear(fe_favs *f)
{
   f->n = 0;
   f->used = 0;
}

int fe_favs_find_h(const fe_favs *f, const char *rel, unsigned h)
{
   unsigned i;
   for (i = 0; i < f->n; i++)
      if (f->hash[i] == h && strcmp(f->pool + f->off[i], rel) == 0)
         return (int)i;
   return -1;
}

int fe_favs_find(const fe_favs *f, const char *rel)
{
   return fe_favs_find_h(f, rel, fe_favs_hash(rel));
}

/* Append a name that is known not to be present.  0, or -1 if it does not
 * fit -- the list is then exactly as it was. */
static int favs_push(fe_favs *f, const char *s, size_t len, unsigned h)
{
   if (f->n >= FE_FAVS_MAX || f->used + len + 1 > FE_FAVS_POOL || len == 0)
      return -1;
   memmove(f->pool + f->used, s, len);      /* s may alias the pool */
   f->pool[f->used + len] = '\0';
   f->off[f->n]  = (unsigned short)f->used;
   f->hash[f->n] = h;
   f->used += (unsigned)(len + 1);
   f->n++;
   return 0;
}

unsigned fe_favs_parse_pool(fe_favs *f, size_t len)
{
   size_t i = 0;
   int truncated = 0;
   /* One byte past the text is always inside the pool, so a span can be
    * NUL-terminated for hashing.  A file that overflows the pool loses its
    * tail, and a line cut in half by that is not a name: drop it. */
   if (len >= FE_FAVS_POOL)
   {
      len = FE_FAVS_POOL - 1;
      truncated = 1;
   }
   f->n = 0;
   f->used = 0;
   /* The compacted names are written at f->used, which never overtakes the
    * read cursor: every kept line is at least as long as its trimmed form
    * plus the newline that separated it. */
   while (i < len)
   {
      size_t s = i, e;
      char   saved;
      unsigned h;
      while (i < len && f->pool[i] != '\n')
         i++;
      e = i;
      if (i < len)
         i++;                               /* past the LF */
      else if (truncated)
         break;                             /* a cut line, not a name */
      while (s < e && (f->pool[s] == ' ' || f->pool[s] == '\t'))
         s++;
      while (e > s && (f->pool[e - 1] == ' ' || f->pool[e - 1] == '\t' ||
                       f->pool[e - 1] == '\r'))
         e--;
      if (e == s || f->pool[s] == '#')
         continue;
      /* Hash and look up on the raw span: it needs a terminator, and the
       * byte after it is the LF we have already consumed (or the end). */
      saved = f->pool[e];
      f->pool[e] = '\0';
      h = fe_favs_hash(f->pool + s);
      if (fe_favs_find_h(f, f->pool + s, h) < 0)
         favs_push(f, f->pool + s, e - s, h);
      if (e >= f->used)                     /* not inside the compacted names */
         f->pool[e] = saved;
   }
   return f->n;
}

int fe_favs_add(fe_favs *f, const char *rel)
{
   unsigned h;
   if (!rel || !rel[0])
      return -1;
   h = fe_favs_hash(rel);
   if (fe_favs_find_h(f, rel, h) >= 0)
      return 0;
   return favs_push(f, rel, strlen(rel), h);
}

int fe_favs_remove(fe_favs *f, const char *rel)
{
   int      i = fe_favs_find(f, rel);
   unsigned k, at, len;
   if (i < 0)
      return -1;
   at  = f->off[i];
   len = (unsigned)strlen(f->pool + at) + 1;
   memmove(f->pool + at, f->pool + at + len, f->used - (at + len));
   f->used -= len;
   for (k = (unsigned)i + 1; k < f->n; k++)
   {
      f->off[k - 1]  = (unsigned short)(f->off[k] > at ? f->off[k] - len
                                                       : f->off[k]);
      f->hash[k - 1] = f->hash[k];
   }
   for (k = 0; k < (unsigned)i; k++)
      if (f->off[k] > at)
         f->off[k] = (unsigned short)(f->off[k] - len);
   f->n--;
   return 0;
}

size_t fe_favs_serialise(const fe_favs *f, char *out, size_t cap)
{
   size_t need = 0, w = 0;
   unsigned i;
   for (i = 0; i < f->n; i++)
   {
      const char *s = f->pool + f->off[i];
      size_t len = strlen(s), j;
      for (j = 0; j <= len; j++)
      {
         char c = j < len ? s[j] : '\n';
         if (w + 1 < cap)
            out[w++] = c;
      }
      need += len + 1;
   }
   if (cap)
      out[w] = '\0';
   return need;
}
