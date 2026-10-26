#!/usr/bin/env python3
"""Exercise the production PSP power callback with controlled event sequences."""
from pathlib import Path
import subprocess
import tempfile

import re

from test_psp_runtime import function

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "psp/main_psp.c").read_text()
callback = function(source, "power_cb")


def bounds(src):
    """The production quiesce budget and the step bounds it is made of."""
    out = []
    for name in ("AUDIO_JOIN_US", "IO_JOIN_US", "SQ_SAV_LAND_US",
                 "SQ_NET_TERM_US", "PWR_QUIESCE_WAIT_US"):
        m = re.search(r"^#define " + name + r"\b(?:.*\\\n)*.*$", src, re.M)
        assert m, name + " not found"
        out.append(m.group(0))
    return "\n".join(out)

stub = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#define PSP_POWER_CB_POWER_SWITCH 0x80000000u
#define PSP_POWER_CB_SUSPENDING 0x00010000u
#define PSP_POWER_CB_RESUMING 0x00020000u
#define PSP_POWER_CB_RESUME_COMPLETE 0x00040000u
#define PWR_PARK_WAIT_US 250000u
@BOUNDS@
#define FE_EVT_ONLY(x) ((void)(x))
#define fe_evt(...) ((void)0)
static struct { int standby; } g_pcfg = { 1 };
static int g_pwr_slept, g_pwr_parked = 1, g_pwr_park_req;
static int g_pwr_park_ack, g_pwr_cb_done, g_pwr_sleep_pending;
static int g_pwr_suspend, g_pwr_resume, g_pwr_rebuild, g_pwr_gb_flush_req;
static int down_calls, notes, scan_calls, wakes, disarms, delays;
static int lock_drops;
static unsigned clock_us;
static int ack_in, park_in;   /* the loop: acknowledges, then parks, after n naps */
static void home_host_disarm(void) { disarms++; }
static void fe_host_sram_request_scan(void) { scan_calls++; }
static void standby_note(const char *fmt, ...) { (void)fmt; notes++; }
static void io_wake(void) { wakes++; }
static void play_lock_drop(const char *who) { (void)who; lock_drops++; }
static void sceKernelDelayThread(unsigned us)
{
    clock_us += us; delays++;
    if (g_pwr_park_req && ack_in && --ack_in == 0)
        g_pwr_park_ack = 1;
    else if (g_pwr_park_ack && park_in && --park_in == 0)
        g_pwr_parked = 1;
}
static unsigned sceKernelGetSystemTimeLow(void) { return ++clock_us; }
static int me_host_loaded(void) { return 0; }
static int me_host_idle(void) { return 1; }
static int me_rend_active(void) { return 1; }
static int me_rend_release_vmem(void) { return 1; }
static int stub_retain;   /* me_standby_down kept the partition: slept stays 0 */
static void me_standby_down(void) { down_calls++; if (!stub_retain) g_pwr_slept = 1; }
@CALLBACK@
int main(void)
{
    unsigned t0;
    int d0;
    power_cb(0, PSP_POWER_CB_POWER_SWITCH, NULL);
    assert(down_calls == 1 && g_pwr_slept == 1);
    assert(scan_calls == 1 && g_pwr_suspend == 1);
    /* The parked loop is NOT released; it is told the callback is done,
     * and it (not the callback) drops the power lock. */
    assert(g_pwr_park_req == 1 && g_pwr_cb_done == 1);
    assert(g_pwr_sleep_pending == 1 && lock_drops == 0);
    /* The SUSPENDING that follows returns at once: no delay, no teardown. */
    d0 = delays;
    power_cb(0, PSP_POWER_CB_SUSPENDING, NULL);
    assert(down_calls == 1 && scan_calls == 1); /* duplicate event */
    assert(delays == d0);
    power_cb(0, PSP_POWER_CB_RESUMING, NULL);
    power_cb(0, PSP_POWER_CB_RESUME_COMPLETE, NULL);
    assert(g_pwr_rebuild == 1 && g_pwr_resume == 1);
    assert(g_pwr_park_req == 0 && g_pwr_cb_done == 0);   /* hold released */
    assert(g_pwr_sleep_pending == 0);
    /* Slid again on the wake overlay, before Continue: g_pwr_slept is still
     * 1 (the engine stays down until the overlay's answer).  The loop must
     * park and quiesce again -- the overlay yields to the park -- but the
     * engine is not torn down twice, and the wake asks for the overlay. */
    g_pwr_rebuild = 0;
    power_cb(0, PSP_POWER_CB_POWER_SWITCH, NULL);
    assert(scan_calls == 2 && g_pwr_suspend == 2);   /* again, from the overlay */
    assert(down_calls == 1);                         /* already down */
    assert(g_pwr_park_req == 1 && g_pwr_cb_done == 1 && lock_drops == 0);
    d0 = delays;
    power_cb(0, PSP_POWER_CB_SUSPENDING, NULL);
    assert(delays == d0 && scan_calls == 2);
    power_cb(0, PSP_POWER_CB_RESUME_COMPLETE, NULL);
    assert(g_pwr_rebuild == 1 && g_pwr_park_req == 0 && g_pwr_cb_done == 0);
    g_pwr_slept = 0;
    power_cb(0, PSP_POWER_CB_SUSPENDING, NULL);
    assert(down_calls == 2); /* inactivity-driven suspend still works */
    power_cb(0, PSP_POWER_CB_RESUME_COMPLETE, NULL);
    assert(notes >= 5 && wakes >= 3 && disarms == 3);

    /* A teardown that could not release the partition leaves slept = 0;
     * the held sleep must still answer the SUSPENDING at once. */
    stub_retain = 1;
    g_pwr_slept = 0;
    power_cb(0, PSP_POWER_CB_POWER_SWITCH, NULL);
    assert(down_calls == 3 && g_pwr_slept == 0 && g_pwr_cb_done == 1);
    d0 = delays;
    power_cb(0, PSP_POWER_CB_SUSPENDING, NULL);
    assert(down_calls == 3 && delays == d0);   /* held: no second pass */
    power_cb(0, PSP_POWER_CB_RESUME_COMPLETE, NULL);
    stub_retain = 0;

    /* No park point (browser, a menu): the callback waits the old 250 ms,
     * not the quiesce budget, releases nothing it does not own and drops
     * the gameplay lock itself. */
    g_pwr_parked = 0;
    g_pwr_slept = 0;
    t0 = clock_us;
    power_cb(0, PSP_POWER_CB_POWER_SWITCH, NULL);
    assert(clock_us - t0 < PWR_PARK_WAIT_US + 60000u + 80000u + 20000u);
    assert(down_calls == 3 && lock_drops == 1);
    assert(g_pwr_park_req == 0 && g_pwr_cb_done == 0);
    power_cb(0, PSP_POWER_CB_RESUME_COMPLETE, NULL);

    /* The budget covers the quiesce's own bounds: the audio and writer
     * joins, the .sav the writer may leave, and the radio's teardown. */
    assert(PWR_QUIESCE_WAIT_US >=
           AUDIO_JOIN_US + IO_JOIN_US + SQ_SAV_LAND_US + SQ_NET_TERM_US);

    /* Acknowledged but the quiesce never finishes: bounded by the budget.
     * The loop is mid-quiesce (in a join, a .sav write, the radio), so the
     * callback leaves the gameplay lock to it: dropped once it parks, never
     * with it inside a driver. */
    g_pwr_park_ack = 1;
    t0 = clock_us;
    power_cb(0, PSP_POWER_CB_POWER_SWITCH, NULL);
    assert(clock_us - t0 >= PWR_PARK_WAIT_US + PWR_QUIESCE_WAIT_US);
    assert(clock_us - t0 < PWR_PARK_WAIT_US + PWR_QUIESCE_WAIT_US + 200000u);
    assert(down_calls == 3 && g_pwr_park_req == 0 && g_pwr_cb_done == 0);
    assert(lock_drops == 1);                  /* not by the callback */
    /* The loop parks at last and keeps the lock: nothing was torn down
     * (sleep_park_hold).  The SUSPENDING that follows is no repeat, and
     * finds it parked: the teardown runs now, and the loop is held. */
    g_pwr_parked = 1;
    d0 = down_calls;
    power_cb(0, PSP_POWER_CB_SUSPENDING, NULL);
    assert(down_calls == d0 + 1 && g_pwr_cb_done == 1 && g_pwr_park_req == 1);
    assert(lock_drops == 1);
    power_cb(0, PSP_POWER_CB_RESUME_COMPLETE, NULL);
    g_pwr_parked = 0;

    /* Acknowledged at once, parked once a quiesce longer than the first
     * stage is done: the callback waits for it, tears down, holds the loop,
     * and returns as soon as it is parked. */
    g_pwr_park_ack = 0;
    g_pwr_slept = 0;
    ack_in = 3;
    park_in = 200;                            /* 6 ms, then 400 ms more */
    t0 = clock_us;
    power_cb(0, PSP_POWER_CB_POWER_SWITCH, NULL);
    assert(down_calls == 5 && g_pwr_cb_done == 1 && g_pwr_park_req == 1);
    assert(clock_us - t0 < 60000u + 6000u + 400000u + 80000u + 20000u);
    assert(lock_drops == 1);                  /* the parked loop drops it */
    puts("power callback: switch-only, duplicate, resume, again-from-overlay, "
         "suspending-only, hold, no-park, quiesce-timeout, teardown-after-"
         "give-up and slow-quiesce pass");
    return 0;
}
'''


def run(body, expect_pass, src=source):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-power-") as folder:
        cfile = Path(folder) / "power.c"
        binary = Path(folder) / "power"
        cfile.write_text(stub.replace("@BOUNDS@", bounds(src))
                             .replace("@CALLBACK@", body))
        subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                        str(cfile), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError(result.stdout + result.stderr)
        return result


print(run(callback, True).stdout.strip())
# One wrong input flag must make the test fail in the real callback's path.
mutant = callback.replace(
    "(PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_SUSPENDING)) &&",
    "PSP_POWER_CB_SUSPENDING) &&", 1)
assert mutant != callback
bad = run(mutant, False)
assert "down_calls == 1" in bad.stderr, bad.stderr
print("mutant (manual-switch branch removed): down_calls assertion failed")

# The dedupe must cover a sleep whose teardown is done but held: without
# g_pwr_cb_done in it, the SUSPENDING after the switch would wait again.
mutant = callback.replace("!(g_pcfg.standby && g_pwr_cb_done)", "1", 1)
assert mutant != callback
bad = run(mutant, False)
assert "down_calls == 1 && scan_calls == 1" in bad.stderr, bad.stderr
print("mutant (no dedupe): duplicate-event assertion failed")

# ...and only that: deduping on g_pwr_slept as well skips the park for a
# second sleep taken from the wake overlay, which then suspends with the
# overlay drawing and the audio thread and the writer live.
mutant = callback.replace("!(g_pcfg.standby && g_pwr_cb_done)",
                          "!(g_pcfg.standby && (g_pwr_slept || g_pwr_cb_done))", 1)
assert mutant != callback
bad = run(mutant, False)
assert "scan_calls == 2 && g_pwr_suspend == 2" in bad.stderr, bad.stderr
print("mutant (dedupe on slept): sleep-from-overlay assertion failed")

# The engine torn down by the first sleep is not torn down again by the second.
mutant = callback.replace("if (g_pwr_parked && g_pwr_slept)", "if (0)", 1)
assert mutant != callback
bad = run(mutant, False)
assert re.search(r"[(`]down_calls == 1[)']", bad.stderr), bad.stderr
print("mutant (second teardown from the overlay): already-down assertion failed")

# Stage 2 of the wait must depend on the acknowledgement: a loop with no
# park point must not cost the callback the whole quiesce budget.
mutant = callback.replace("if (g_pwr_park_ack)\n            while", "while", 1)
assert mutant != callback
bad = run(mutant, False)
assert "PWR_PARK_WAIT_US + 60000u" in bad.stderr, bad.stderr
print("mutant (quiesce wait without acknowledgement): no-park timing failed")

# Stage 2 must end when the loop has parked, not run out the budget on every
# sleep that has a park point.
mutant = callback.replace("if (g_pwr_park_ack)\n            while (!g_pwr_parked &&",
                          "if (g_pwr_park_ack)\n            while (", 1)
assert mutant != callback
bad = run(mutant, False)
assert "60000u + 6000u + 400000u" in bad.stderr, bad.stderr
print("mutant (stage 2 ignores the park): slow-quiesce timing failed")

# A loop that acknowledged is still quiescing when the budget runs out: the
# callback must not drop the lock under it.
mutant = callback.replace("if (!g_pwr_parked && !g_pwr_park_ack)",
                          "if (!g_pwr_parked)", 1)
assert mutant != callback
bad = run(mutant, False)
assert re.search(r"[(`]lock_drops == 1[)']", bad.stderr), bad.stderr
print("mutant (callback drops the lock mid-quiesce): lock assertion failed")

# The old budget (1.5 s) is shorter than the writer's join alone.
m = re.search(r"^#define PWR_QUIESCE_WAIT_US\b(?:.*\\\n)*.*$", source, re.M)
bad_src = source.replace(m.group(0), "#define PWR_QUIESCE_WAIT_US 1500000u", 1)
bad = run(callback, False, bad_src)
assert "AUDIO_JOIN_US + IO_JOIN_US" in bad.stderr, bad.stderr
print("mutant (budget below the quiesce's bounds): budget assertion failed")

# A callback that gave up tore nothing down: the SUSPENDING after it must not
# be taken for a repeat, or the loop -- which keeps the lock until a teardown
# has run -- waits out its hold and the slide is lost.
mutant = callback.replace("      if (held)\n         g_pwr_cb_done = 1;",
                          "      g_pwr_cb_done = 1;", 1)
assert mutant != callback
bad = run(mutant, False)
assert "g_pwr_cb_done == 0" in bad.stderr, bad.stderr
print("mutant (give-up marked done): cb_done assertion failed")
