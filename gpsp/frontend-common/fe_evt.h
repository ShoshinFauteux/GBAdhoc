/* fe_evt.h — structured EVT logger (plan §7.1).
 *
 * The EVT log is a PRODUCT FEATURE, not scaffolding: the validation harness
 * greps these lines from ms0:/... (PSP) or a host path (desktop).  Every EVT
 * line is a single fflush()ed line of the form "EVT <name>[ key=value...]".
 * Keep names/keys grep-stable; changing one is an interface change.
 */
#ifndef FE_EVT_H
#define FE_EVT_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the log file (append=0 truncates). echo_stderr also mirrors every
 * line to stderr (desktop convenience; harmless no-op audience on PSP).
 * Returns 0 on success; on failure logging degrades to stderr-only. */
int  fe_evt_init(const char *path, int echo_stderr);
void fe_evt_close(void);

/* Structured event: emits "EVT " + formatted line + "\n", then fflush. */
/* ADR-0067b: with telemetry compiled out, a call site should not survive at
 * all.  The stub function version still cost a call and full varargs setup at
 * every site, and kept every format string in .rodata -- and it still
 * EVALUATED the arguments, which for `fe_evt("sram_load crc=%08x", sram_crc())`
 * means CRCing 128 KB for a line nobody will ever read.
 *
 * Verified safe before doing this: no fe_evt call site in the tree mutates
 * state in its arguments, so discarding them changes nothing.
 *
 * Derived in the HEADER, not in fe_evt.c, because callers need to agree with
 * the implementation about which one they are getting -- putting this
 * derivation where only the implementation could see it is the exact bug
 * ADR-0067 records (a "telemetry-free" build that linked the whole logger). */
#if defined(GPSP_PLAYABLE) && !defined(GPSP_NO_TELEMETRY) &&     !defined(GPSP_KEEP_TELEMETRY)
#define GPSP_NO_TELEMETRY 1
#endif

#ifdef GPSP_NO_TELEMETRY
#define fe_evt(...)   ((void)0)
#else
void fe_evt(const char *fmt, ...);
#endif

/* Free-form info line (not an EVT marker): "LOG " prefix, also flushed. */
void fe_log(const char *fmt, ...);

/* ---- log I/O cost accounting (ADR-0021) ---------------------------------
 * Every line is fflush()ed, and on a PSP that flush lands on a memory stick.
 * At the normal ~1 line per 5-10 s that is noise, but a driver that starts
 * logging per packet turns it into stutter — and nothing measured it. Wire a
 * clock with fe_evt_set_clock() (NULL disables; zero overhead when unset)
 * and fe_evt_prof() returns free-running totals: lines written and the total
 * microseconds spent in emit().
 *
 * NOTE (ADR-0024): once fe_evt_set_async() is installed, `us`/`max_us` are
 * what the CALLING (emulation) thread paid — format + ring copy + signal.
 * The memory-stick cost moves to fe_evt_prof_io(). */
void fe_evt_set_clock(unsigned long long (*now_us)(void));
void fe_evt_prof(unsigned *lines, unsigned *us, unsigned *max_us);

/* ---- asynchronous writer (ADR-0024) -------------------------------------
 * The field measured `evt` maxima of ~12.0-12.5 ms on BOTH consoles: one
 * fflush to a memory stick, on the emulation thread, inside a 16.7 ms vblank
 * period.  With an async sink installed, fe_evt/fe_log only format the line
 * into an internal ring and call wake(); a platform writer thread then calls
 * fe_evt_service() to do the fwrite+fflush.
 *
 * Contract — exactly one consumer at a time:
 *   fe_evt_set_async(wake)  install; the platform must have its thread up.
 *   fe_evt_service()        called ONLY by the writer thread while async is
 *                           on.  Returns bytes written.
 *   fe_evt_set_async(NULL)  stop your writer thread FIRST, then call this:
 *                           it disables queueing and drains on your thread.
 * A line that does not fit the ring is dropped WHOLE (never truncated, so
 * the log stays parseable) and counted in fe_evt_prof_io()'s `dropped`.
 * Installing a sink re-bases fe_evt_prof()'s `max_us` to zero: boot logging
 * is synchronous by design and its flushes would otherwise dominate a
 * free-running maximum forever.
 * fe_evt_close() implies fe_evt_set_async(NULL). */
void fe_evt_set_async(void (*wake)(void));
int  fe_evt_service(void);
int  fe_evt_pending(void);
void fe_evt_prof_io(unsigned *io_us, unsigned *io_max_us, unsigned *dropped,
                    unsigned *ring_hi);

#ifdef __cplusplus
}
#endif

#endif /* FE_EVT_H */
