/* heap_trace.h -- which allocations outlive a game (psp/heap_trace.c,
 * docs/INPROC-RETURN.md).
 *
 * Diagnostic builds only: PSP_EXTRA="HEAP_TRACE=1" defines GPSP_HEAP_TRACE,
 * links heap_trace.o and wraps newlib's allocator (psp/Makefile).  Every other
 * build (the release and the lab among them) does not link the object since
 * code-health B-4, and these calls compile to nothing. */
#ifndef HEAP_TRACE_H
#define HEAP_TRACE_H

#ifdef GPSP_HEAP_TRACE
void heap_trace_mark(void);                  /* remember "now" */
void heap_trace_mark0(void);                 /* "everything live" */
void heap_trace_report(const char *where);   /* what is live since the mark */
#else
#define heap_trace_mark()      ((void)0)
#define heap_trace_mark0()     ((void)0)
#define heap_trace_report(w)   ((void)(w))
#endif

#endif
