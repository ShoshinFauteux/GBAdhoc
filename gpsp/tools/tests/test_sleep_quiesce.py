#!/usr/bin/env python3
"""The production sleep quiesce wiring in psp/main_psp.c, run on the host.

Extracts, unchanged, the park-side functions (play_lock_take/drop, the sq_*
operations, g_sq_ops, sleep_quiesce_down/up, sleep_park_hold) and links them
with the real psp/sleep_q.c against stubs that record every call.  Checked:

  1. TYL's lifecycle: the gameplay power lock is held through play; at the
     park the audio thread is stopped and joined FIRST, then the writer, then
     the handles, the HOME module and the radio; the lock is dropped while
     the loop is held, i.e. BEFORE the resume; after it, everything comes
     back in reverse with sound last.
  2. A hold that no suspend follows ends on its own and asks for the wake
     overlay; a loop the callback gave up on keeps the lock until a
     teardown has run (the SUSPENDING that follows), or ends on its own; a
     loop released before the wake lets the suspend through on its way
     out.
  3. The lock is never re-taken while a sleep is pending, is held over the
     wake overlay (which yields to the park), and is taken once, not per
     frame.
  4. standby = 0: the park does nothing at all.
  5. The teardown and the rewind (psp_rewind_pool_lost, psp_rewind_pwr):
     one in the volatile pool is ended; a heap-backed one is kept, unless
     the machine stands inside an old save with the loop parked -- then its
     hold would keep the suspend off, so it is ended as in the pool and its
     hold let go.

Then mutants of the production text, each of which must fail.
"""
from pathlib import Path
import re
import subprocess
import tempfile

from test_psp_runtime import function

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "psp/main_psp.c").read_text(encoding="utf-8")

NAMES = ("play_lock_take", "play_lock_drop", "sq_audio_stop", "sq_audio_start",
         "sq_writer_stop", "sq_writer_start", "sq_files_close",
         "sq_files_reopen", "sq_home_stop", "sq_home_rearm", "sq_net_term",
         "sq_note", "sleep_quiesce_down", "sleep_quiesce_up",
         "sleep_park_hold", "loop_unpark")


def production(source):
    parts = []
    for name in NAMES:
        parts.append(function(source, name))
        if name == "sq_note":
            m = re.search(r"^static const sleep_q_ops g_sq_ops = \{.*?\};",
                          source, re.S | re.M)
            assert m, "g_sq_ops not found"
            parts.append(m.group(0))
        if name == "sleep_quiesce_up":
            m = re.search(r"^#define SLEEP_HOLD_NAPS .*$", source, re.M)
            assert m, "SLEEP_HOLD_NAPS not found"
            parts.append(m.group(0))
    return "\n\n".join(parts)


STUB = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sleep_q.h"
#define GPSP_PLAYABLE 1
#define REWIND 1
#define FE_EVT_ONLY(x) ((void)(x))
#define fe_evt(...) ((void)0)
#define SLEEP_SIM_ASLEEP() 0
#define SQ_TRACE(s) do { strncat(trace, s " ", sizeof(trace) - strlen(trace) - 1); } while (0)
typedef struct RFILE RFILE;
static char trace[1024], notes[4096];
static struct { int standby; } g_pcfg;
static int g_wake_hold, g_running = 1, g_pwr_slept, g_pwr_rebuild;
static volatile int g_pwr_park_req, g_pwr_cb_done, g_pwr_sleep_pending;
static int locks, unlocks, unlocked_while_held, holding;
static int audio_thid = -1, audio_running, audio_output_error;
static unsigned ring_r, ring_w;
static int g_io_thid = -1, sav_open, evt_open, home_mod, g_net_up;
static RFILE *gamepak_file_large;
static int naps, resume_at_nap, teardown_at_nap;
static int rw_hold, rw_unvetted;   /* the rewind's own power hold */
static sleep_q g_sq;
static volatile int g_pwr_play_lock;
static void standby_note(const char *fmt, ...)
{
   char line[256];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   strncat(notes, line, sizeof(notes) - strlen(notes) - 2);
   strcat(notes, "\n");
}
static int scePowerLock(int u) { (void)u; locks++; SQ_TRACE("lock"); return 0; }
static int scePowerUnlock(int u)
{
   (void)u; unlocks++; SQ_TRACE("unlock");
   if (holding && g_pwr_park_req) unlocked_while_held = 1;
   return 0;
}
static void sceKernelDelayThread(unsigned us)
{
   (void)us;
   assert(naps < 100000);                     /* held forever */
   if (++naps == teardown_at_nap)
   {  /* the SUSPENDING after a give-up: it finds the loop parked */
      SQ_TRACE("teardown");
      g_pwr_park_req = 1; g_pwr_cb_done = 1; g_pwr_slept = 1;
   }
   if (naps == resume_at_nap)
   {  /* the callback's RESUME_COMPLETE */
      SQ_TRACE("resume");
      g_pwr_park_req = 0; g_pwr_cb_done = 0; g_pwr_sleep_pending = 0;
   }
}
static unsigned sceKernelGetSystemTimeLow(void) { return 1000u * (unsigned)naps; }
static int audio_stop(void) { SQ_TRACE("audio_stop"); audio_running = 0; audio_thid = -1; return 0; }
static void audio_start(void) { SQ_TRACE("audio_start"); audio_running = 1; audio_thid = 7; }
static int io_thread_stop(void)
{
   SQ_TRACE("io_stop");
   if (g_io_thid < 0) return 1;
   g_io_thid = -1; return 0;
}
static void io_thread_start(void) { SQ_TRACE("io_start"); g_io_thid = 9; }
static int fe_host_sram_flush(int force) { (void)force; SQ_TRACE("sav_scan"); return 0; }
static void fe_host_sram_sync(void) { SQ_TRACE("sav_sync"); }
static int fe_host_sram_sleep_close(void) { int w = sav_open; SQ_TRACE("sav_close"); sav_open = 0; return w; }
static void filestream_close(RFILE *f) { (void)f; SQ_TRACE("rom_close"); }
static int fe_evt_is_open(void) { return evt_open; }
static void fe_evt_close(void) { SQ_TRACE("evt_close"); evt_open = 0; }
static int home_host_loaded(void) { return home_mod; }
static void home_host_stop(void) { SQ_TRACE("home_stop"); home_mod = 0; }
static void home_host_forget(void) { SQ_TRACE("home_forget"); }
static void net_teardown_io(void) { SQ_TRACE("net_term"); g_net_up = 0; }
static void psp_rewind_pwr(void)
{  /* as production: the hold follows fe_host_rewind_unvetted() */
   if (rw_hold && !rw_unvetted) { rw_hold = 0; SQ_TRACE("rw_release"); }
}

@FUNCTIONS@

static void machine(void)
{
   trace[0] = notes[0] = '\0';
   memset(&g_sq, 0, sizeof(g_sq));
   g_pcfg.standby = 1;
   audio_thid = 7; audio_running = 1; g_io_thid = 9;
   sav_open = 1; evt_open = 0; home_mod = 1; g_net_up = 1;
   gamepak_file_large = (RFILE *)&naps;
   naps = 0; resume_at_nap = teardown_at_nap = 0; locks = unlocks = 0;
   unlocked_while_held = holding = 0;
   g_pwr_play_lock = 0; g_wake_hold = 0;
   g_pwr_park_req = g_pwr_cb_done = g_pwr_sleep_pending = 0;
   g_pwr_slept = g_pwr_rebuild = 0;
   rw_hold = rw_unvetted = 0;
}

int main(void)
{
   /* 1. TYL's lifecycle, end to end. */
   machine();
   play_lock_take();                          /* the loop's top, in play */
   assert(locks == 1 && g_pwr_play_lock == 1);
   g_pwr_sleep_pending = 1; g_pwr_park_req = 1;   /* the switch */
   sleep_quiesce_down();                      /* the park point */
   /* The writer gone, a last scan and drain on this thread: SRAM written
    * since the callback's scan request reaches the stick before the
    * suspend, with or without a writer thread. */
   assert(strstr(trace, "lock audio_stop io_stop sav_scan sav_sync sav_close "
                        "rom_close home_stop net_term ") != NULL);
   assert(gamepak_file_large == NULL && sav_open == 0);
   assert(strstr(notes, "q audio_stop rc=0") && strstr(notes, "q fd_close n=2"));
   assert(strstr(notes, "q home_stop rc=0") && strstr(notes, "q net_term rc=0"));
   g_pwr_cb_done = 1; g_pwr_slept = 1;        /* the callback's part */
   resume_at_nap = 3;
   holding = 1;
   sleep_park_hold();
   holding = 0;
   assert(unlocks == 1 && unlocked_while_held);   /* the suspend went through
                                                   * with the loop held */
   assert(strstr(trace, "unlock resume") != NULL);
   assert(naps == 3 && g_pwr_park_req == 0);
   /* The wake overlay holds the lock too: it yields to the park, so a
    * second sleep slid on it waits for the park like one slid in play. */
   g_wake_hold = 1;
   play_lock_take();
   assert(locks == 2 && g_pwr_play_lock == 1);
   trace[0] = '\0';
   sleep_quiesce_up("resume");
   assert(strcmp(trace, "home_forget io_start audio_start ") == 0);
   assert(audio_running == 1 && g_io_thid == 9 && !g_sq.down);
   g_wake_hold = 0;
   play_lock_take();                          /* Continue: play again */
   assert(locks == 2 && g_pwr_play_lock == 1);   /* still the one lock */

   /* 2a. A switch that no suspend follows: the hold ends by itself and the
    *     wake overlay is asked for, so the rebuild still happens. */
   machine();
   play_lock_take();
   g_pwr_sleep_pending = 1; g_pwr_park_req = 1;
   sleep_quiesce_down();
   g_pwr_cb_done = 1; g_pwr_slept = 1;
   sleep_park_hold();
   assert(naps == SLEEP_HOLD_NAPS && g_pwr_park_req == 0);
   assert(g_pwr_sleep_pending == 0 && g_pwr_rebuild == 1 && unlocks == 1);
   assert(strstr(notes, "q hold_timeout"));

   /* 2b. Given up on before it parked (a quiesce that overran the
    *     callback's budget): that callback tore nothing down, so the loop,
    *     parked at last, keeps the lock -- dropped now, the suspend would
    *     land with the renderer up -- until the SUSPENDING that follows
    *     finds it parked and tears down.  Then it drops it, stays held
    *     through the suspend, and the quiesce is undone only once the wake
    *     has come. */
   machine();
   play_lock_take();
   g_pwr_sleep_pending = 1; g_pwr_park_req = 1;
   sleep_quiesce_down();
   g_pwr_park_req = 0;                        /* the callback gave up */
   teardown_at_nap = 3;
   resume_at_nap = 6;
   holding = 1;
   sleep_park_hold();
   holding = 0;
   assert(unlocks == 1 && unlocked_while_held);
   assert(strstr(trace, "teardown unlock resume") != NULL);
   assert(naps == 6 && g_sq.down);
   loop_unpark("resume");
   assert(!g_sq.down && audio_running == 1 && g_io_thid == 9);

   /* 2c. ...and if nothing follows, it ends by itself and puts the game
    *     back with the lock still held: nothing was torn down, so no
    *     overlay, and no suspend let through with the renderer up. */
   machine();
   play_lock_take();
   g_pwr_sleep_pending = 1; g_pwr_park_req = 1;
   sleep_quiesce_down();
   g_pwr_park_req = 0;
   sleep_park_hold();
   assert(naps == SLEEP_HOLD_NAPS && unlocks == 0 && g_pwr_play_lock == 1);
   assert(g_pwr_sleep_pending == 0 && g_pwr_rebuild == 0);
   loop_unpark("resume");
   assert(!g_sq.down && audio_running == 1 && g_io_thid == 9);

   /* 2d. The quiesce is not undone while the sleep is still pending (the
    *     loop let go before the wake). */
   machine();
   g_pwr_sleep_pending = 1; g_pwr_park_req = 1;
   sleep_quiesce_down();
   trace[0] = '\0';
   loop_unpark("resume");
   assert(g_sq.down && trace[0] == '\0');     /* pending: not yet */
   g_pwr_sleep_pending = 0;
   loop_unpark("resume");
   assert(!g_sq.down && strstr(trace, "audio_start") != NULL);

   /* 3. Never re-taken while pending. */
   machine();
   g_pwr_sleep_pending = 1;
   play_lock_take();
   assert(locks == 0);
   g_pwr_sleep_pending = 0;
   play_lock_take();
   play_lock_take();
   assert(locks == 1);                        /* once, not per frame */

   /* 3b. A rewind standing inside an old save holds the switch with its
    *     own lock (psp_rewind_pwr).  The callback's teardown suspends the
    *     rewind, which ends that state; the held loop must let that hold
    *     go too, or the suspend waits for it and the hold times out with
    *     no sleep. */
   machine();
   play_lock_take();
   rw_hold = 1; rw_unvetted = 1;
   g_pwr_sleep_pending = 1; g_pwr_park_req = 1;
   sleep_quiesce_down();
   g_pwr_cb_done = 1; g_pwr_slept = 1;
   rw_unvetted = 0;                           /* fe_host_rewind_suspend */
   resume_at_nap = 3;
   sleep_park_hold();
   assert(rw_hold == 0 && strstr(trace, "rw_release") != NULL);
   assert(strstr(trace, "resume") > strstr(trace, "rw_release"));

   /* 4. standby = 0: the park does nothing at all. */
   machine();
   g_pcfg.standby = 0;
   sleep_quiesce_down();
   assert(trace[0] == '\0' && !g_sq.down);

    puts("sleep quiesce: TYL lifecycle, hold timeout, overrun hold, "
        "unpark gating, lock gating, rewind hold, standby = 0 pass");
   return 0;
}
'''


def run(functions, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-sleepq-") as folder:
        cfile = Path(folder) / "sq.c"
        binary = Path(folder) / "sq"
        cfile.write_text(STUB.replace("@FUNCTIONS@", functions))
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-I" + str(ROOT / "psp"),
                        str(cfile), str(ROOT / "psp/sleep_q.c"),
                        "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, result.stdout, result.stderr))
        return result


good = production(SOURCE)
print(run(good, True).stdout.strip())

MUTANTS = (
    ("the held loop never drops the gameplay lock",
     '         play_lock_drop("park");\n', "",
     "unlocked_while_held"),
    ("the park lands no SRAM written since the callback's scan",
     "   (void)fe_host_sram_flush(0);\n", "",
     "sav_scan sav_sync"),
    ("the park scans but leaves the blocks undrained",
     "   fe_host_sram_sync();\n   return rc;", "   return rc;",
     "sav_scan sav_sync"),
    ("the paged ROM handle is left open",
     "gamepak_file_large = NULL;\n      rom = 1;", "rom = 1;",
     "gamepak_file_large == NULL"),
    ("the park quiesces with standby = 0",
     "if (g_sq.down || !g_pcfg.standby)", "if (g_sq.down)",
     "trace[0] == '\\0'"),
    ("the lock is re-taken while a sleep is pending",
     "g_pwr_play_lock || g_pwr_sleep_pending ||", "g_pwr_play_lock ||",
     "locks =="),
    ("the loop let go before the wake does not hold",
     "while ((g_pwr_park_req || g_pwr_sleep_pending || SLEEP_SIM_ASLEEP())",
     "while ((g_pwr_park_req || SLEEP_SIM_ASLEEP())",
     "unlocks == 1 && unlocked_while_held"),
    ("the quiesce is undone while the sleep is pending",
     "if (g_sq.down && !g_pwr_sleep_pending)", "if (g_sq.down)",
     "g_sq.down && trace[0]"),
    ("a loop given up on drops the lock with nothing torn down",
     "      if (g_pwr_cb_done)\n      {\n         play_lock_drop(\"park\");",
     "      if (cb_over)\n      {\n         play_lock_drop(\"park\");",
     "unlocks == 1 && unlocked_while_held"),
    ("a loop given up on is held until the wake, however long",
     "int cb_over = g_pwr_cb_done || !g_pwr_park_req;",
     "int cb_over = g_pwr_cb_done;",
     "naps < 100000"),
    ("the held loop keeps the rewind's hold",
     "         psp_rewind_pwr();\n", "",
     "rw_hold == 0"),
    ("the wake overlay runs without the lock",
     "g_pwr_sleep_pending || g_pwr_park_req)",
     "g_pwr_sleep_pending || g_pwr_park_req || g_wake_hold)",
     "locks == 2 && g_pwr_play_lock == 1"),
)
for label, old, new, expect in MUTANTS:
    bad_src = SOURCE.replace(old, new, 1)
    assert bad_src != SOURCE, label
    bad = run(production(bad_src), False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant (%s): failed as it must" % label)


# 5. The teardown and the rewind.
RW_STUB = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define REWIND 1
static volatile int g_pwr_parked;
static int g_rw_on, g_rw_pool, g_rw_asleep, g_rw_showing;
static const uint16_t *g_rw_show;
static uint16_t *g_rw_slot, pic[4];
static int unvetted, suspends, holds;
static void fe_host_rewind_suspend(void) { suspends++; }
static int fe_host_rewind_unvetted(void) { return unvetted; }
static void pwr_hold(const char *why) { (void)why; holds++; }
static void pwr_release(void) { holds--; }

@FUNCTIONS@

static void standing(int pool, int unv)
{
   g_rw_on = 1; g_rw_pool = pool; g_rw_asleep = 0; unvetted = unv;
   g_rw_showing = 1; g_rw_show = pic; g_rw_slot = pic;
   suspends = 0;
   psp_rewind_pwr();               /* the loop, before the switch */
}

int main(void)
{
   /* In the pool: the pool goes, and so does the history. */
   standing(1, 0);
   g_pwr_parked = 1;
   psp_rewind_pool_lost();
   assert(suspends == 1 && !g_rw_on && g_rw_asleep && !g_rw_pool);
   assert(!g_rw_showing && !g_rw_show && !g_rw_slot);

   /* On the heap, standing on a vetted frame: it survives the sleep. */
   standing(0, 0);
   psp_rewind_pool_lost();
   assert(suspends == 0 && g_rw_on && !g_rw_asleep && g_rw_showing);

   /* On the heap, inside an old save, the loop parked: its hold would keep
    * the suspend off, so it is ended as in the pool, and the held loop's
    * next look lets the hold go. */
   standing(0, 1);
   assert(holds == 1);
   psp_rewind_pool_lost();
   assert(suspends == 1 && !g_rw_on && g_rw_asleep);
   assert(!g_rw_showing && !g_rw_show && !g_rw_slot);
   psp_rewind_pwr();               /* sleep_park_hold */
   assert(holds == 0);

   /* ...but not with the loop running (the callback's unparked branch):
    * the frame may be inside the rewind this very moment. */
   g_pwr_parked = 0;
   standing(0, 1);
   psp_rewind_pool_lost();
   assert(suspends == 0 && g_rw_on && holds == 1);

   puts("rewind teardown: pool ended, heap kept, heap unvetted ended when "
        "parked, not when running pass");
   return 0;
}
"""


def rw_production(source):
    m = re.search(r"^static int g_rw_pwr;$", source, re.M)
    assert m, "g_rw_pwr not found"
    return "\n\n".join((m.group(0), function(source, "psp_rewind_pwr"),
                        function(source, "psp_rewind_pool_lost")))


def rw_run(functions, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-rwsleep-") as folder:
        cfile = Path(folder) / "rw.c"
        binary = Path(folder) / "rw"
        cfile.write_text(RW_STUB.replace("@FUNCTIONS@", functions))
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", str(cfile), "-o", str(binary)],
                       check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, result.stdout, result.stderr))
        return result


print(rw_run(rw_production(SOURCE), True).stdout.strip())

RW_MUTANTS = (
    ("a heap-backed rewind inside an old save is left standing",
     "   if (!g_rw_pool &&\n       !(g_pwr_parked && g_rw_on && fe_host_rewind_unvetted()))",
     "   if (!g_rw_pool)",
     "suspends == 1 && !g_rw_on && g_rw_asleep"),
    ("a heap-backed rewind is ended with the loop running",
     "!(g_pwr_parked && g_rw_on && fe_host_rewind_unvetted())",
     "!(g_rw_on && fe_host_rewind_unvetted())",
     "suspends == 0 && g_rw_on && holds == 1"),
    ("every heap-backed rewind is ended",
     "!(g_pwr_parked && g_rw_on && fe_host_rewind_unvetted())",
     "!(g_pwr_parked && g_rw_on)",
     "suspends == 0 && g_rw_on && !g_rw_asleep"),
)
for label, old, new, expect in RW_MUTANTS:
    bad_src = SOURCE.replace(old, new, 1)
    assert bad_src != SOURCE, label
    bad = rw_run(rw_production(bad_src), False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant (%s): failed as it must" % label)
