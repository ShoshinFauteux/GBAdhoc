/* prx_libc.h -- force-included into every gbadhoc_online.prx object.
 *
 * The module links no libc (-nostdlib): newlib would give it a heap of its
 * own, carved from the same 1 MiB of partition the net modules need.  Memory
 * and printf-style formatting go to the EBOOT through online_host (see
 * online_link.h); the handful of string routines are in prx_libc.c.  The
 * names below are renamed before any libc header declares them, so those
 * declarations simply declare the ol_* versions. */
#ifndef PRX_LIBC_H
#define PRX_LIBC_H

#include <stddef.h>
#include <stdarg.h>

#define calloc    ol_calloc
#define free      ol_free
#define vsnprintf ol_vsnprintf
#define snprintf  ol_snprintf
#define sprintf   ol_sprintf

void *ol_calloc(size_t n, size_t sz);
void  ol_free(void *p);
int   ol_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);
int   ol_snprintf(char *out, size_t cap, const char *fmt, ...);
int   ol_sprintf(char *out, const char *fmt, ...);

#endif
