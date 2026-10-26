/* sleep_q.c -- see sleep_q.h.  Pure C: the host test links this file. */
#include <stdio.h>
#include <string.h>

#include "sleep_q.h"

static void sq_note(const sleep_q_ops *ops, const char *line)
{
   if (ops->note)
      ops->note(ops->ctx, line);
}

static void sq_notef_rc(const sleep_q_ops *ops, const char *what, int rc)
{
   char line[96];
   snprintf(line, sizeof(line), "q %s rc=%d", what, rc);
   sq_note(ops, line);
}

int sleep_q_down(sleep_q *q, const sleep_q_ops *ops)
{
   int n = 0, rc;
   char detail[64] = "";
   char line[112];

   if (!q || !ops || q->down)
      return 0;
   q->down = 1;
   q->stopped = 0;

   /* 1. Sound.  TYL before_pause: StopSoundThread (join + delete).  The
    *    channel is released by the thread on its way out.  Terminated
    *    (rc < 0, the join timed out) the thread is gone all the same (the
    *    stop releases its channel), and is started again on the way up. */
   rc = ops->audio_stop(ops->ctx);
   sq_notef_rc(ops, "audio_stop", rc);
   if (rc == SQ_DONE || rc < 0)
   {
      q->stopped |= SQ_STEP_AUDIO;
      n++;
   }

   /* 2. The writer.  It is stopped, then the .sav is landed on this thread
    *    (a last scan and drain), and the sink stays synchronous.  Terminated
    *    (rc < 0) still means the thread is gone: restarted, as joined. */
   rc = ops->writer_stop(ops->ctx);
   sq_notef_rc(ops, "writer_stop", rc);
   if (rc == SQ_DONE || rc < 0)
   {
      q->stopped |= SQ_STEP_WRITER;
      n++;
   }

   /* 3. Long-lived handles.  The .sav and the event log belong to the
    *    writer while it runs, which is why this comes after it. */
   rc = ops->files_close(ops->ctx, detail, sizeof(detail));
   snprintf(line, sizeof(line), "q fd_close n=%d %s", rc, detail);
   sq_note(ops, line);
   if (rc > 0)
   {
      q->stopped |= SQ_STEP_FILES;
      n++;
   }

   /* 4. The HOME poller (a kernel module thread polling every 16 ms). */
   rc = ops->home_stop(ops->ctx);
   sq_notef_rc(ops, "home_stop", rc);
   if (rc == SQ_DONE)
   {
      q->stopped |= SQ_STEP_HOME;
      n++;
   }

   /* 5. The radio.  A session does not survive a sleep anyway; ending it
    *    here unloads the adhoc modules before the WLAN chip loses power. */
   rc = ops->net_term(ops->ctx);
   sq_notef_rc(ops, "net_term", rc);
   if (rc == SQ_DONE)
   {
      q->stopped |= SQ_STEP_NET;
      n++;
   }

   return n;
}

int sleep_q_up(sleep_q *q, const sleep_q_ops *ops)
{
   int n = 0, rc;
   char detail[64] = "";
   char line[112];

   if (!q || !ops || !q->down)
      return 0;
   q->down = 0;

   /* The radio is not brought back: the session it carried is over (the
    * partner noticed the silence long ago).  The player starts a new one. */
   if (q->stopped & SQ_STEP_NET)
      sq_note(ops, "q net_up rc=1 (session ended)");

   if (q->stopped & SQ_STEP_HOME)
   {
      rc = ops->home_rearm(ops->ctx);
      sq_notef_rc(ops, "home_rearm", rc);
      if (rc == SQ_DONE)
         n++;
   }

   /* Every handle we closed reopens itself on first use (the .sav writer
    * and the ROM page loader both reopen by path); only the event log needs
    * a hand.  Ask even when nothing was closed: it is cheap and logs. */
   rc = ops->files_reopen(ops->ctx, detail, sizeof(detail));
   snprintf(line, sizeof(line), "q fd_reopen rc=%d %s", rc, detail);
   sq_note(ops, line);
   if (rc == SQ_DONE)
      n++;

   if (q->stopped & SQ_STEP_WRITER)
   {
      rc = ops->writer_start(ops->ctx);
      sq_notef_rc(ops, "writer_start", rc);
      if (rc == SQ_DONE)
         n++;
   }

   /* Sound last, as TYL's after_pause: nothing is queued for it to play
    * that predates the sleep. */
   if (q->stopped & SQ_STEP_AUDIO)
   {
      rc = ops->audio_start(ops->ctx);
      sq_notef_rc(ops, "audio_start", rc);
      if (rc == SQ_DONE)
         n++;
   }

   q->stopped = 0;
   return n;
}
