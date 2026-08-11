#include "fe_evt.h"

#include <stdio.h>
#include <string.h>

/* ADR-0067: the playable build implies no telemetry.  Derived HERE, in the
 * translation unit that uses it, because a `#define` in psp/main_psp.c is
 * invisible to this file — the first version of this change put it there and
 * produced a "telemetry-free" build that linked the whole logger. */
#if defined(GPSP_PLAYABLE) && !defined(GPSP_NO_TELEMETRY)
#define GPSP_NO_TELEMETRY 1
#endif

static FILE *evt_file    = NULL;
static int   evt_echo    = 0;

/* ADR-0021 log-I/O cost accounting. */
static unsigned long long (*evt_clock)(void);
static unsigned evt_lines, evt_us, evt_max_us;

/* ---- async line ring (ADR-0024) -----------------------------------------
 * Producer: whichever thread calls fe_evt/fe_log (the emulation thread).
 * Consumer: exactly one writer thread, via fe_evt_service().
 * Free-running byte indices; FE_EVT_RING is a power of two so the wrap is a
 * mask.  `evt_r` is only ever advanced by the consumer and only AFTER the
 * bytes have been written, so a producer that races it merely sees the ring
 * as fuller than it is — it can never overwrite unflushed bytes. */
#define FE_EVT_RING 16384
#define FE_EVT_MASK (FE_EVT_RING - 1)

static char evt_ring[FE_EVT_RING];
static volatile unsigned evt_w, evt_r;
static void (*evt_wake)(void);
static unsigned evt_drop, evt_hi;
static unsigned evt_io_us, evt_io_max_us;

void fe_evt_set_clock(unsigned long long (*now_us)(void))
{
   evt_clock = now_us;
}

void fe_evt_prof(unsigned *lines, unsigned *us, unsigned *max_us)
{
   if (lines)  *lines  = evt_lines;
   if (us)     *us     = evt_us;
   if (max_us) *max_us = evt_max_us;
}

void fe_evt_prof_io(unsigned *io_us, unsigned *io_max_us, unsigned *dropped,
                    unsigned *ring_hi)
{
   if (io_us)     *io_us     = evt_io_us;
   if (io_max_us) *io_max_us = evt_io_max_us;
   if (dropped)   *dropped   = evt_drop;
   if (ring_hi)   *ring_hi   = evt_hi;
}

/* ADR-0067: GPSP_NO_TELEMETRY — the PLAYABLE build has no event log at all.
 *
 * There is no runtime config that turns `fe_evt` off: `log_thread` and
 * `net_log_thread` choose HOW a line reaches the memory stick, never WHETHER
 * one is produced.  A build meant for someone to actually play should not be
 * opening a file on ms0:, formatting several hundred bytes a second into it,
 * or (worse) falling back to the `!evt_file` branch of emit(), which fwrites
 * and fflushes every line to stderr whether or not anything is listening.
 *
 * So this is a build flag, not a key: with it set `fe_evt_init` opens
 * nothing, `emit` returns immediately, and the ring, the writer thread and
 * the profiler all have nothing to do.  Call sites are untouched, so the
 * harness build is unaffected and the two cannot drift apart. */
int fe_evt_init(const char *path, int echo_stderr)
{
#ifdef GPSP_NO_TELEMETRY
   (void)path; (void)echo_stderr;
   return -1;
#else
   evt_echo = echo_stderr;
   if (evt_file)
   {
      fclose(evt_file);
      evt_file = NULL;
   }
   if (path && path[0])
      evt_file = fopen(path, "w");
   return evt_file ? 0 : -1;
#endif
}

/* Write everything the ring holds, then fflush once.  ONLY the writer
 * thread may call this while async mode is on; fe_evt_set_async(NULL) calls
 * it on the caller's thread once queueing has been switched off. */
int fe_evt_service(void)
{
#ifdef GPSP_NO_TELEMETRY
   /* ADR-0067: the playable build has no log file, so this was already inert
    * at runtime -- fe_evt_open() is compiled out, evt_file is permanently
    * NULL, and the guard below returned 0 on the first branch.  It was still
    * COMPILED, though, which linked fwrite/fflush and left a live stdio path
    * in a binary that is supposed to have none.  Caught by `psp-nm -u
    * fe_evt.o` still listing them after every token check passed -- the same
    * behavioural check that caught the GPSP_NO_TELEMETRY define landing in the
    * wrong translation unit.  Returning here removes the code, not just the
    * calls. */
   return 0;
#else
   unsigned long long t0;
   unsigned r = evt_r;
   unsigned w = evt_w;          /* snapshot: the producer may run on */
   unsigned n = w - r, wrote = 0;

   if (!n)
      return 0;
   if (!evt_file)
   {
      evt_r = w;                /* nowhere to put it; do not wedge the ring */
      return 0;
   }

   t0 = evt_clock ? evt_clock() : 0;
   while (n)
   {
      unsigned off   = r & FE_EVT_MASK;
      unsigned first = FE_EVT_RING - off;
      unsigned chunk = (n < first) ? n : first;
      if (fwrite(&evt_ring[off], 1, chunk, evt_file) != chunk)
         break;
      r     += chunk;
      n     -= chunk;
      wrote += chunk;
   }
   fflush(evt_file);
   evt_r = r;                   /* release only after the bytes are out */

   if (evt_clock)
   {
      unsigned d = (unsigned)(evt_clock() - t0);
      evt_io_us += d;
      if (d > evt_io_max_us)
         evt_io_max_us = d;
   }
   return (int)wrote;
#endif   /* GPSP_NO_TELEMETRY */
}

int fe_evt_pending(void)
{
   return (int)(evt_w - evt_r);
}

void fe_evt_set_async(void (*wake)(void))
{
   if (!wake)
   {
      evt_wake = NULL;          /* stop queueing FIRST... */
      fe_evt_service();         /* ...then drain here, single consumer */
      return;
   }
   /* Scope the emulation-thread maximum to the async era.  Boot logging is
    * deliberately synchronous, and on a memory stick those few lines carry
    * exactly the ~12 ms flush this change exists to remove — leaving them
    * in a free-running max would make every future field log read as if
    * nothing had improved.  `evt_us`/`evt_lines` stay cumulative because
    * sess_cost deltas them; only the max needs re-basing. */
   evt_max_us = 0;
   evt_wake = wake;
}

void fe_evt_close(void)
{
   evt_wake = NULL;
   fe_evt_service();
   if (evt_file)
   {
      fclose(evt_file);
      evt_file = NULL;
   }
}

/* Single producer.  Returns 0 if the line does not fit (dropped whole —
 * never half a line, so the log is always parseable). */
static int ring_push(const char *p, unsigned n)
{
   unsigned w    = evt_w;
   unsigned used = w - evt_r;
   unsigned i;

   if (used + n > FE_EVT_RING)
      return 0;
   for (i = 0; i < n; i++)
      evt_ring[(w + i) & FE_EVT_MASK] = p[i];
   evt_w = w + n;               /* publish only after the copy */
   if (used + n > evt_hi)
      evt_hi = used + n;
   return 1;
}

static void emit(const char *prefix, const char *fmt, va_list ap)
{
#ifdef GPSP_NO_TELEMETRY
   (void)prefix; (void)fmt; (void)ap;   /* ADR-0067, see fe_evt_init */
#else
   char line[512];
   unsigned long long t0 = evt_clock ? evt_clock() : 0;
   unsigned pl = (unsigned)strlen(prefix);
   unsigned n;
   int fn;

   memcpy(line, prefix, pl);
   fn = vsnprintf(line + pl, sizeof(line) - pl - 1, fmt, ap);
   if (fn < 0)
      fn = 0;
   if ((unsigned)fn > sizeof(line) - pl - 2)
      fn = (int)(sizeof(line) - pl - 2);
   n = pl + (unsigned)fn;
   line[n++] = '\n';

   if (evt_wake)
   {
      /* Async: the emulation thread pays a memcpy and a semaphore signal;
       * the memory stick is the writer thread's problem (ADR-0024). */
      if (!ring_push(line, n))
         evt_drop++;
      evt_wake();
   }
   else if (evt_file)
   {
      fwrite(line, 1, n, evt_file);
      fflush(evt_file);
   }
   if (evt_echo || !evt_file)
   {
      fwrite(line, 1, n, stderr);
      fflush(stderr);
   }

   evt_lines++;
   if (evt_clock)
   {
      unsigned d = (unsigned)(evt_clock() - t0);
      evt_us += d;
      if (d > evt_max_us)
         evt_max_us = d;
   }
#endif
}

void fe_evt(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   emit("EVT ", fmt, ap);
   va_end(ap);
}

void fe_log(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   emit("LOG ", fmt, ap);
   va_end(ap);
}
