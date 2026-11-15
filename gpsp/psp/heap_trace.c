/* heap_trace.c -- which allocations outlive a game (docs/INPROC-RETURN.md).
 *
 * Diagnostic builds only: `tools/build.sh harness` with PSP_EXTRA=
 * "HEAP_TRACE=1" links newlib's allocator through the wrappers below
 * (psp/Makefile: -Wl,--wrap=...), and every live allocation is kept in a
 * table with its size, the code address that asked for it and a sequence
 * number.  heap_trace_mark() remembers "now"; heap_trace_report() prints
 * every allocation made since the mark that is still live -- after a game
 * has been unloaded, that list IS the unload's residue, by name.
 *
 * Two layers: the public entry points (malloc, free, ...) record the caller
 * of malloc(); the reentrant ones (_malloc_r, ...) also catch newlib's own
 * internal allocations (stdio buffers, dtoa's Bigints, the tm buffer...),
 * whose caller is then the libc function itself.  Everything runs under the
 * (recursive) malloc lock.  Only HEAP_TRACE=1 compiles and links it. */
#ifdef GPSP_HEAP_TRACE
#include <malloc.h>
#include <reent.h>
#include <stddef.h>
#include <stdint.h>
#include "fe_evt.h"

#define HT_SLOTS 4096
static struct { void *p; unsigned n, ra, seq; } g_ht[HT_SLOTS];
static unsigned g_ht_seq, g_ht_mark, g_ht_lost;
static unsigned g_ht_outer_ra;   /* caller of malloc() & co, 0 = internal */
/* Wrapped allocator calls in progress.  newlib builds calloc, memalign and
 * realloc in objects of their own that reach _malloc_r through the wrapper
 * too; only the outermost call records, or one block is counted twice. */
static unsigned g_ht_depth;

static void ht_add(void *p, size_t n, unsigned ra)
{
   unsigned i;
   if (!p)
      return;
   for (i = 0; i < HT_SLOTS; i++)
      if (!g_ht[i].p)
      {
         g_ht[i].p = p;
         g_ht[i].n = (unsigned)n;
         g_ht[i].ra = g_ht_outer_ra ? g_ht_outer_ra : ra;
         g_ht[i].seq = ++g_ht_seq;
         return;
      }
   g_ht_lost++;
}

static void ht_del(void *p)
{
   unsigned i;
   if (!p)
      return;
   for (i = 0; i < HT_SLOTS; i++)
      if (g_ht[i].p == p)
      {
         g_ht[i].p = NULL;
         return;
      }
}

void *__real__malloc_r(struct _reent *, size_t);
void  __real__free_r(struct _reent *, void *);
void *__real__realloc_r(struct _reent *, void *, size_t);
void *__real__calloc_r(struct _reent *, size_t, size_t);
void *__real__memalign_r(struct _reent *, size_t, size_t);

void *__wrap__malloc_r(struct _reent *r, size_t n)
{
   void *p;
   __malloc_lock(r);
   g_ht_depth++;
   p = __real__malloc_r(r, n);
   g_ht_depth--;
   if (!g_ht_depth)
      ht_add(p, n, (unsigned)__builtin_return_address(0));
   __malloc_unlock(r);
   return p;
}

void __wrap__free_r(struct _reent *r, void *q)
{
   __malloc_lock(r);
   ht_del(q);
   __real__free_r(r, q);
   __malloc_unlock(r);
}

void *__wrap__realloc_r(struct _reent *r, void *q, size_t n)
{
   void *p;
   __malloc_lock(r);
   g_ht_depth++;
   p = __real__realloc_r(r, q, n);
   g_ht_depth--;
   if (!g_ht_depth)
   {
      if (p || !n)
         ht_del(q);
      ht_add(p, n, (unsigned)__builtin_return_address(0));
   }
   __malloc_unlock(r);
   return p;
}

void *__wrap__calloc_r(struct _reent *r, size_t a, size_t b)
{
   void *p;
   __malloc_lock(r);
   g_ht_depth++;
   p = __real__calloc_r(r, a, b);
   g_ht_depth--;
   if (!g_ht_depth)
      ht_add(p, a * b, (unsigned)__builtin_return_address(0));
   __malloc_unlock(r);
   return p;
}

void *__wrap__memalign_r(struct _reent *r, size_t al, size_t n)
{
   void *p;
   __malloc_lock(r);
   g_ht_depth++;
   p = __real__memalign_r(r, al, n);
   g_ht_depth--;
   if (!g_ht_depth)
      ht_add(p, n, (unsigned)__builtin_return_address(0));
   __malloc_unlock(r);
   return p;
}

/* The public layer: who called malloc(). */
#define OUTER(call)                                                    \
   do {                                                                \
      __malloc_lock(_REENT);                                           \
      g_ht_outer_ra = (unsigned)__builtin_return_address(0);           \
      call;                                                            \
      g_ht_outer_ra = 0;                                               \
      __malloc_unlock(_REENT);                                         \
   } while (0)

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__real_memalign(size_t, size_t);
void *__wrap_malloc(size_t n)            { void *p; OUTER(p = __real_malloc(n)); return p; }
void *__wrap_calloc(size_t a, size_t b)  { void *p; OUTER(p = __real_calloc(a, b)); return p; }
void *__wrap_realloc(void *q, size_t n)  { void *p; OUTER(p = __real_realloc(q, n)); return p; }
void *__wrap_memalign(size_t a, size_t n){ void *p; OUTER(p = __real_memalign(a, n)); return p; }

void heap_trace_mark(void)
{
   __malloc_lock(_REENT);
   g_ht_mark = g_ht_seq;
   __malloc_unlock(_REENT);
}

/* Back to "everything since the process started". */
void heap_trace_mark0(void)
{
   g_ht_mark = 0;
}

/* Every allocation since the mark that is still live (table order). */
void heap_trace_report(const char *where)
{
   unsigned i, n = 0, bytes = 0, top = g_ht_seq;
   __malloc_lock(_REENT);
   for (i = 0; i < HT_SLOTS; i++)
         if (g_ht[i].p && g_ht[i].seq > g_ht_mark && g_ht[i].seq <= top)
         {
            fe_evt("heap_trace at=%s seq=%u p=%p n=%u ra=%08x", where,
                   g_ht[i].seq - g_ht_mark, g_ht[i].p, g_ht[i].n, g_ht[i].ra);
            n++;
            bytes += g_ht[i].n;
         }
   fe_evt("heap_trace_done at=%s live=%u bytes=%u lost=%u", where, n, bytes,
          g_ht_lost);
   __malloc_unlock(_REENT);
}
#else
/* Not linked without HEAP_TRACE=1 (psp/Makefile, code-health B-4); the
 * callers' psp/heap_trace.h compiles the calls to nothing. */
typedef int heap_trace_off_unit;   /* not an empty translation unit */
#endif
