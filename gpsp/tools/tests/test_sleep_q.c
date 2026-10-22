/* test_sleep_q.c -- the sleep quiesce sequence (psp/sleep_q.c), against the
 * production file.
 *
 *   1. Down order is TYL's: audio, writer, files, home, net.  Up is the
 *      reverse, audio LAST, and restarts only what down really stopped.
 *   2. The .sav / event-log handles are closed only once the writer is gone
 *      (joined, or terminated after a timed-out join); the ROM handle too.
 *   3. Idempotent: a second down while down does nothing; up without down
 *      does nothing. */
#include <stdio.h>
#include <string.h>

#include "sleep_q.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- a fake machine --------------------------------------------------- */
static char trace[512];
static char notes[4096];
static int  rc_audio_stop, rc_writer_stop, rc_home_stop, rc_net_term;
static int  sav_open, rom_open, evt_open, writer_live;

static void t(const char *s)
{
   strncat(trace, s, sizeof(trace) - strlen(trace) - 1);
   strncat(trace, " ", sizeof(trace) - strlen(trace) - 1);
}
static int f_audio_stop(void *c) { (void)c; t("A-"); return rc_audio_stop; }
static int f_audio_start(void *c) { (void)c; t("A+"); return 0; }
static int f_writer_stop(void *c)
{
   (void)c;
   t("W-");
   writer_live = 0;
   return rc_writer_stop;
}
static int f_writer_start(void *c) { (void)c; t("W+"); writer_live = 1; return 0; }
static int f_files_close(void *c, char *d, size_t n)
{
   int closed = 0;
   (void)c;
   t("F-");
   CHECK(!writer_live);         /* the writer owns the .sav while it runs */
   closed = sav_open + rom_open + evt_open;
   snprintf(d, n, "sav=%d rom=%d evt=%d", sav_open, rom_open, evt_open);
   sav_open = rom_open = evt_open = 0;
   return closed;
}
static int f_files_reopen(void *c, char *d, size_t n)
{
   (void)c;
   t("F+");
   snprintf(d, n, "evt=1");
   return 1;
}
static int f_home_stop(void *c) { (void)c; t("H-"); return rc_home_stop; }
static int f_home_rearm(void *c) { (void)c; t("H+"); return 0; }
static int f_net_term(void *c) { (void)c; t("N-"); return rc_net_term; }
static void f_note(void *c, const char *line)
{
   (void)c;
   strncat(notes, line, sizeof(notes) - strlen(notes) - 2);
   strcat(notes, "\n");
}

static const sleep_q_ops ops = {
   NULL, f_audio_stop, f_audio_start, f_writer_stop, f_writer_start,
   f_files_close, f_files_reopen, f_home_stop, f_home_rearm, f_net_term,
   f_note
};

static void reset(void)
{
   trace[0] = notes[0] = '\0';
   rc_audio_stop = rc_writer_stop = rc_home_stop = rc_net_term = 0;
   sav_open = rom_open = evt_open = 1;
   writer_live = 1;
}

static void test_order(void)
{
   sleep_q q;
   memset(&q, 0, sizeof(q));
   reset();
   CHECK(sleep_q_down(&q, &ops) == 5);
   CHECK(strcmp(trace, "A- W- F- H- N- ") == 0);
   CHECK(strstr(notes, "q audio_stop rc=0\n") != NULL);
   CHECK(strstr(notes, "q writer_stop rc=0\n") != NULL);
   CHECK(strstr(notes, "q fd_close n=3 sav=1 rom=1 evt=1\n") != NULL);
   CHECK(strstr(notes, "q home_stop rc=0\n") != NULL);
   CHECK(strstr(notes, "q net_term rc=0\n") != NULL);
   /* idempotent while down */
   CHECK(sleep_q_down(&q, &ops) == 0);
   CHECK(strcmp(trace, "A- W- F- H- N- ") == 0);
   trace[0] = '\0';
   CHECK(sleep_q_up(&q, &ops) == 3);   /* home, writer, audio (files: 1 = none) */
   /* reverse; the radio is not restarted; sound last */
   CHECK(strcmp(trace, "H+ F+ W+ A+ ") == 0);
   CHECK(strstr(notes, "q net_up rc=1 (session ended)") != NULL);
   CHECK(strstr(notes, "q audio_start rc=0") != NULL);
   trace[0] = '\0';
   CHECK(sleep_q_up(&q, &ops) == 0);   /* not down: nothing */
   CHECK(trace[0] == '\0');
}

static void test_only_restart_what_stopped(void)
{
   sleep_q q;
   memset(&q, 0, sizeof(q));
   reset();
   rc_audio_stop = -3;          /* join timed out: terminated */
   rc_writer_stop = -5;         /* the same */
   rc_home_stop = SQ_NONE;      /* module never loaded */
   rc_net_term = SQ_NONE;       /* no session */
   CHECK(sleep_q_down(&q, &ops) == 3);   /* audio, writer, files */
   CHECK(sav_open == 0);        /* a terminated writer is gone too */
   CHECK(strstr(notes, "q writer_stop rc=-5") != NULL);
   trace[0] = '\0';
   sleep_q_up(&q, &ops);
   /* A terminated thread is gone all the same: it is started again, or the
    * game stays silent and its saves synchronous for the session. */
   CHECK(strcmp(trace, "F+ W+ A+ ") == 0);
}

int main(void)
{
   test_order();
   test_only_restart_what_stopped();
   if (failures)
   {
      fprintf(stderr, "sleep_q: %d of %d checks FAILED\n", failures, checks);
      return 1;
   }
   printf("sleep_q: %d checks passed (order, restart-only-what-stopped, "
          "handle ownership)\n", checks);
   return 0;
}
