#!/usr/bin/env python3
"""A sleep taken in the game list -- after a game was left -- parks the list.

The game list had no park point.  A switch slid there cost the power
callback its 250 ms, the callback gave the volatile partition back from its
own thread (the renderer's buffers, kept locked across an in-process return
for the next game) and returned, and the suspend then landed on a main
thread still running the list: drawing, reading box art off the stick, the
audio thread in sceAudioOutputBlocking.  A PSP-3001 was seen to cold-boot
on such a sleep.  The fix makes the list park like the game loop
(main_psp.c browser_park, ui_psp.c ui_browser).

Extracted, unchanged, and run on the host with real threads:
  from psp/main_psp.c  power_cb, sleep_park_hold (+ SLEEP_HOLD_NAPS),
                       browser_park, the quiesce bounds, and main()'s game
                       list call (the registration of the hook around
                       ui_browser, run as it stands);
  from psp/ui_psp.c    ui_set_browser_park and the list loop's park call.
Stubbed: the audio thread, the ME, the partition, the clock (real time).

The state is the one an in-process return leaves: the renderer off with its
buffers still in the volatile partition, the ME module unloaded, the audio
thread running, no play lock.  A switch is slid with the list drawing:

  1. the callback returns with the list PARKED and held (cb_done), the
     audio thread stopped and joined, the partition released, and NO game
     teardown (me_standby_down) -- g_pwr_slept stays 0, so the next game
     does not open on a wake overlay;
  2. while "asleep" (between the callback and RESUME_COMPLETE) the list
     draws nothing and is not mid-frame;
  3. the SUSPENDING that follows is answered at once (the sleep is held);
  4. after RESUME_COMPLETE the list runs again with sound, swallows what
     was held at the wake, and the hook is cleared when the list returns.

Then mutants of the production text, each of which must fail -- the first
is the fix's call site in main() reverted, i.e. the list as it was.
"""
from pathlib import Path
import re
import subprocess
import tempfile

from test_psp_runtime import function

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "psp/main_psp.c").read_text(encoding="utf-8")
UI = (ROOT / "psp/ui_psp.c").read_text(encoding="utf-8")


def define(src, name):
    m = re.search(r"^#define " + name + r"\b(?:.*\\\n)*.*$", src, re.M)
    assert m, name + " not found"
    return m.group(0)


def list_call(main_src):
    """main()'s game-list call, from the boot mark to the answer's check."""
    head = "         boot_timeline_emit();\n      }\n"
    i = main_src.find(head)
    assert i >= 0, "the game list's call in main() not found"
    i += len(head)
    j = main_src.find("\n      if (br > 0)", i)
    assert j > i, "the game list's answer check not found"
    body = main_src[i:j]
    assert "ui_browser(" in body, "ui_browser call not in the extracted span"
    return body


def park_call(ui_src):
    """The list loop's head, between its locals and its pad read: where the
    park call stands (and nothing else does)."""
    head = "      int rom;                        /* g_roms index of the cursor row */\n"
    i = ui_src.find(head)
    assert i >= 0 and ui_src.find(head, i + 1) < 0, "the list loop not found"
    i += len(head)
    j = ui_src.find("      sceCtrlPeekBufferPositive(&pd, 1);", i)
    assert j > i, "the list loop's pad read not found"
    return ui_src[i:j]


def setter(ui_src):
    m = re.search(r"^static int \(\*g_browser_park\)\(void\);\n"
                  r"void ui_set_browser_park\(.*$", ui_src, re.M)
    assert m, "ui_set_browser_park not found"
    return m.group(0)


STUB = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "sleep_q.h"
#define REWIND 1
#define PSP_POWER_CB_POWER_SWITCH 0x80000000u
#define PSP_POWER_CB_SUSPENDING 0x00010000u
#define PSP_POWER_CB_RESUMING 0x00020000u
#define PSP_POWER_CB_RESUME_COMPLETE 0x00040000u
#define PWR_PARK_WAIT_US 250000u
@BOUNDS@
#define FE_EVT_ONLY(x) ((void)(x))
#define fe_evt(...) ((void)0)
#define SLEEP_SIM_ASLEEP() 0
#define ROM_DIR "roms"
static struct { int standby; } g_pcfg = { 1 };
static volatile int g_running = 1;
static volatile int g_pwr_slept, g_pwr_rebuild, g_pwr_park_req, g_pwr_parked;
static volatile int g_pwr_park_ack, g_pwr_cb_done, g_pwr_sleep_pending;
static volatile int g_pwr_park_browser;
static volatile int g_pwr_suspend, g_pwr_resume, g_pwr_gb_flush_req;
static volatile unsigned g_pwr_park_t0;
static volatile int g_rw_pool_suspect, g_net_up;
static volatile int audio_running = 1, audio_stops, audio_starts;
static volatile int mer_vmem = 1, releases, standby_downs, lock_drops;
static volatile int frames, in_frame, swallowed;
static volatile unsigned g_prev_pad;

static void standby_note(const char *fmt, ...) { (void)fmt; }
static void home_host_disarm(void) {}
static void fe_host_sram_request_scan(void) {}
static void io_wake(void) {}
static void play_lock_drop(const char *who) { (void)who; lock_drops++; }
static void psp_rewind_pwr(void) {}
static int psp_rewind_pool_tenant(void) { return 0; }
static int me_host_loaded(void) { return 0; }     /* shut down at the exit */
static int me_host_idle(void) { return 1; }
static int me_rend_active(void) { return 0; }     /* torn down at the exit */
static int me_rend_release_vmem(void)
{
   int had = mer_vmem;
   mer_vmem = 0;
   releases++;
   return had;
}
static void me_standby_down(void)   /* the game's teardown, as production */
{
   standby_downs++;
   (void)me_rend_release_vmem();
   g_pwr_slept = 1;
}
static int sq_audio_stop(void *c)
{
   (void)c;
   if (!audio_running)
      return SQ_NONE;
   audio_running = 0;
   audio_stops++;
   return SQ_DONE;
}
static int sq_audio_start(void *c)
{
   (void)c;
   audio_running = 1;
   audio_starts++;
   return SQ_DONE;
}
static unsigned sceKernelGetSystemTimeLow(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (unsigned)(ts.tv_sec * 1000000u + ts.tv_nsec / 1000u);
}
static void sceKernelDelayThread(unsigned us) { usleep(us); }

@SETTER@

static int browser_loop(void)
{
   while (g_running)
   {
      g_prev_pad = 0;
@PARKCALL@
      if (g_prev_pad == 0xFFFFFFFFu)
         swallowed++;
      in_frame = 1;          /* a GE list in flight, an art read */
      frames++;
      usleep(1000);
      in_frame = 0;
      usleep(1000);
   }
   return -1;
}
#define ui_browser(a, b, c, d, e) \
   ((void)(a), (void)(b), (void)(c), (void)(d), (void)(e), browser_loop())

@HOLD@

@CALLBACK@

@PARK@

static void *list_thread(void *arg)
{
   static char rom_path[64];
   int browser_state_slot = 0, selected_console = 0, br;
   (void)arg;
@LISTCALL@
   (void)br;
   return NULL;
}

static void wait_frames(int n)
{
   int f0 = frames, i;
   for (i = 0; i < 2000 && frames < f0 + n; i++)
      usleep(1000);
   assert(frames >= f0 + n);
}

int main(void)
{
   pthread_t th;
   int f0;
   (void)g_net_up;   /* read only by the callback's fe_evt lines */
   pthread_create(&th, NULL, list_thread, NULL);
   wait_frames(10);

   /* 1. The switch, with the list drawing. */
   power_cb(0, PSP_POWER_CB_POWER_SWITCH, NULL);
   assert(g_pwr_parked == 1);
   assert(g_pwr_cb_done == 1 && g_pwr_park_req == 1);
   assert(audio_running == 0 && audio_stops == 1);
   assert(releases == 1 && mer_vmem == 0);
   assert(standby_downs == 0 && g_pwr_slept == 0);

   /* 2. Asleep: nothing drawn, nothing in flight. */
   f0 = frames;
   usleep(60000);
   assert(in_frame == 0);
   assert(frames == f0);

   /* 3. The SUSPENDING after it is no new sleep. */
   power_cb(0, PSP_POWER_CB_SUSPENDING, NULL);
   assert(standby_downs == 0 && releases == 1 && frames == f0);

   /* 4. The wake. */
   power_cb(0, PSP_POWER_CB_RESUMING, NULL);
   power_cb(0, PSP_POWER_CB_RESUME_COMPLETE, NULL);
   wait_frames(5);
   assert(g_pwr_parked == 0 && g_pwr_park_browser == 0);
   assert(audio_running == 1 && audio_starts == 1);
   assert(g_pwr_rebuild == 0 && g_pwr_slept == 0);
   assert(swallowed == 1);

   g_running = 0;
   pthread_join(th, NULL);
   assert(g_browser_park == NULL);
   puts("sleep from the game list: parked, sound joined, partition released, "
        "nothing drawn asleep, no wake overlay, sound back, hook cleared: pass");
   return 0;
}
'''


def build(main_src, ui_src):
    bounds = "\n".join(define(main_src, n) for n in
                       ("AUDIO_JOIN_US", "IO_JOIN_US", "SQ_SAV_LAND_US", "SQ_NET_TERM_US",
                        "PWR_QUIESCE_WAIT_US"))
    hold = define(main_src, "SLEEP_HOLD_NAPS") + "\n" + \
        function(main_src, "sleep_park_hold")
    return (STUB.replace("@BOUNDS@", bounds)
                .replace("@SETTER@", setter(ui_src))
                .replace("@PARKCALL@", park_call(ui_src))
                .replace("@HOLD@", hold)
                .replace("@CALLBACK@", function(main_src, "power_cb"))
                .replace("@PARK@", function(main_src, "browser_park"))
                .replace("@LISTCALL@", list_call(main_src)))


def run(main_src, ui_src, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-sleep-browser-") as d:
        cfile = Path(d) / "t.c"
        binary = Path(d) / "t"
        cfile.write_text(build(main_src, ui_src))
        subprocess.run(["cc", "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra",
                        "-Werror", "-Wno-unused-function", "-pthread",
                        "-I" + str(ROOT / "psp"), str(cfile), "-o",
                        str(binary)], check=True)
        r = subprocess.run([str(binary)], capture_output=True, text=True,
                           timeout=60)
        if (r.returncode == 0) != expect_pass:
            raise AssertionError("expected %s:\n%s%s" % (
                "pass" if expect_pass else "failure", r.stdout, r.stderr))
        return r


print(run(MAIN, UI, True).stdout.strip())


def mutant(src, old, new, label, needle, ui=False):
    assert src.count(old) == 1, label + ": production text not found"
    bad = src.replace(old, new, 1)
    r = run(MAIN if ui else bad, bad if ui else UI, False)
    assert needle in r.stderr, label + ": wrong assertion\n" + r.stderr
    print("mutant (%s): %s assertion failed" % (label, needle))


# The fix's call site in main(): without the registration the list runs as it
# did before -- no park, the callback times out and returns on a live list.
mutant(MAIN, "      ui_set_browser_park(browser_park);   /* the list parks too */\n",
       "", "list not registered = before the fix", "g_pwr_parked == 1")
# The list loop's own call removed.
mutant(UI, "      if (g_browser_park && g_browser_park())\n"
           "         g_prev_pad = 0xFFFFFFFFu;\n",
       "", "list loop never asks", "g_pwr_parked == 1", ui=True)
# The callback taking the list's park for the game's: the engine teardown
# runs, g_pwr_slept is set and the next game would open on a wake overlay.
mutant(MAIN, "else if (g_pwr_parked && !g_pwr_park_browser)",
       "else if (g_pwr_parked)", "game teardown for the list",
       "standby_downs == 0")
# The list parks but leaves the audio thread in its output.
mutant(MAIN, "   audio = sq_audio_stop(NULL);\n",
       "   audio = SQ_NONE;\n", "sound left running", "audio_running == 0")
# Sound not given back after the wake.
mutant(MAIN, "      (void)sq_audio_start(NULL);\n", "      (void)0;\n",
       "no sound after the wake", "audio_running == 1")
# The hook left registered after the list returns (a game loop would then
# have a stale hook in ui_psp.c).
mutant(MAIN, "      ui_set_browser_park(NULL);\n", "",
       "hook not cleared", "g_browser_park == NULL")
