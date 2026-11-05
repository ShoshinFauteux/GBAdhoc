/* prx_libc.c -- the little libc gbadhoc_online.prx needs (see prx_libc.h). */
#include "online_link.h"

#include <string.h>

extern const online_host *g_ol_host;

void *ol_calloc(size_t n, size_t sz) { return g_ol_host->calloc_(n, sz); }
void  ol_free(void *p) { if (p) g_ol_host->free_(p); }

int ol_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap)
{
   return g_ol_host->vsnprintf_(out, cap, fmt, ap);
}

int ol_snprintf(char *out, size_t cap, const char *fmt, ...)
{
   va_list ap;
   int n;
   va_start(ap, fmt);
   n = g_ol_host->vsnprintf_(out, cap, fmt, ap);
   va_end(ap);
   return n;
}

int ol_sprintf(char *out, const char *fmt, ...)
{
   va_list ap;
   int n;
   va_start(ap, fmt);
   n = g_ol_host->vsnprintf_(out, 64, fmt, ap);    /* only ipfmt: "a.b.c.d:p" */
   va_end(ap);
   return n;
}

/* gcc emits calls to these for struct copies and clears, as well as the
 * explicit ones in transport_relay.c. */
void *memcpy(void *d, const void *s, size_t n)
{
   unsigned char *a = (unsigned char *)d;
   const unsigned char *b = (const unsigned char *)s;
   while (n--)
      *a++ = *b++;
   return d;
}

void *memmove(void *d, const void *s, size_t n)
{
   unsigned char *a = (unsigned char *)d;
   const unsigned char *b = (const unsigned char *)s;
   if (a < b)
      while (n--)
         *a++ = *b++;
   else
      while (n--)
         a[n] = b[n];
   return d;
}

void *memset(void *d, int c, size_t n)
{
   unsigned char *a = (unsigned char *)d;
   while (n--)
      *a++ = (unsigned char)c;
   return d;
}

int memcmp(const void *x, const void *y, size_t n)
{
   const unsigned char *a = (const unsigned char *)x, *b = (const unsigned char *)y;
   for (; n; n--, a++, b++)
      if (*a != *b)
         return *a < *b ? -1 : 1;
   return 0;
}

size_t strlen(const char *s)
{
   const char *p = s;
   while (*p)
      p++;
   return (size_t)(p - s);
}

char *strchr(const char *s, int c)
{
   for (;; s++)
   {
      if (*s == (char)c)
         return (char *)s;
      if (!*s)
         return NULL;
   }
}
