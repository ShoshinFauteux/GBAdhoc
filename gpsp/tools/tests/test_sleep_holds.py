#!/usr/bin/env python3
"""The power-hold windows around the renderer, a boot's notice, the online
cancel's screen and the harness's ME-quiet log (main_psp.c, as text and run).

1. THE RENDERER COMES BACK ON UNDER A HOLD.  power_cb's unparked branch gives
   the volatile partition back when the renderer is off, and rests on "every
   main-thread path that can turn the renderer ON runs under pwr_hold".  The
   ad hoc bring-ups suspended the renderer, let net_bringup take and drop its
   own hold, and only then resumed it: a switch slid during the bring-up was
   deferred to that drop and landed before me_rend_resume.  Every
   me_rend_suspend() call site of the TU is classified here:
     - wrapped: pwr_hold("net") on the line before it and pwr_release() on
       the line after its me_rend_resume(): net_bringup_rend_off, the one
       copy the silent bring-up, the GB link auto-start and the menu's ad
       hoc bring-up call, and the online bring-up.  Each wrapped run is
       compiled as it stands and executed with a net_bringup that takes and
       drops its own hold: the count must never reach zero before the
       renderer is back, and must be zero after.  The three callers must
       go through net_bringup_rend_off: a bare net_bringup() anywhere but
       there and the boot's is a fault.
     - covered by a hold its caller takes, each named below with the text
       that proves it.
   A new call site fails until it is one or the other.

2. A BOOT THAT STOPS ON A NOTICE (boot_stop_notice): the hold is not up
   while ui_notice waits on the player, it is up again for the exit, and
   main() shows no notice any other way after the boot's hold is taken.

3. THE ONLINE CANCEL (online_bringup_cancel): a new O press redraws the
   LINKING screen once with "Cancelling..." and no footer; an O held when
   the screen came up is not a press; later polls answer 1 and draw nothing.

4. THE HARNESS'S ME-QUIET LOG (sleep_sim_me_quiet): me_host_idle() sampled
   1 ms and 10 ms after the call, one EVT line and the same in standby.log.

5. NO MEDIA ENGINE IS NOT A LOST ONE (Adrenaline on a PS Vita).  A sleep on
   a console whose engine never came up -- the module not loaded, or loaded
   and dead because the unwind of a failed boot could not stop it -- runs
   me_standby_down as it stands: no sync asked, no pool dropped,
   g_pwr_me_lost left 0, so wake_service shows the Continue overlay
   (test_sleep_menu.py runs that side).  An engine that WAS up and fails
   its sync still sets it.

6. THE WAKE THAT ENDS THE SESSION SHOWS ITS NOTICE WITH NO HOLD
   (wake_me_lost_notice).  The notice loop cannot park, and power_cb drops
   the gameplay lock for a loop that does not; a hold taken for the notice
   was then the only lock left, and its release let the suspend start on the
   way to the teardown's save, before the teardown's hold.  With no hold the
   suspend lands inside the notice (renderer off, pool gone).  The function
   is compiled as it stands and run: it never touches the hold, it is up
   3 s with no button and at least 0.5 s with one; and wake_service takes
   or drops no hold between the notice and the loop's exit.

Then mutants of the production text, each of which must fail.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import extract
from test_psp_runtime import function, marked_function

ROOT = Path(__file__).resolve().parents[2]
MAIN = extract.read_source(str(ROOT / "psp/main_psp.c"))

CALL = re.compile(r"^[ \t]*me_rend_suspend\(\);", re.M)
WRAPPED_SITES = 2       # net_bringup_rend_off, ui_net_action's online bring-up
# the bring-ups made while a game runs: each calls net_bringup_rend_off
REND_OFF_CALLS = (
    "rc = net_bringup_rend_off(as_host, g_silent_group, g_silent_nick);",
    "rc = net_bringup_rend_off(g_gblink_role == 1, g_silent_group, g_pcfg.nick);",
    "rc = net_bringup_rend_off(is_host, group, g_pcfg.nick);",
)
# every other net_bringup( in the TU: its definition, the helper's call, the
# boot's (under the boot hold, a covered site below)
NET_BRINGUP_OTHER = (
    "static int net_bringup(int is_host, const char *group, const char *nick,",
    "rc = net_bringup(is_host, group, nick, 0);",
    "if (net_bringup(net_host, group, nick, net_probe) != 0)",
)
# me_rend_suspend() sites that are not wrapped themselves: (a line of text
# that identifies the site, searched between the previous site and this one
# or just after it; why it is under a hold; the check of that claim)
COVERED = (
    ("boot bring-up",
     "Bring the wireless session up around the booted core.",
     "main(): between the boot's pwr_hold and its release before the loop"),
    ("teardown",
     "The renderer's buffers: kept when they are the volatile",
     "main(): after pwr_hold(\"teardown\")"),
    ("gba_reload_note",
     "static void gba_reload_note(int phase, const char *what)",
     "fe_host's session reload callback; its callers hold \"session\""),
    ("mgift_start_io",
     "(The renderer goes and comes back under mgift_start's pwr_hold.)",
     "mgift_start: pwr_hold(\"mgift\") around its only call"),
)


def code_line_before(text, pos):
    """The line before the one `pos` starts, stripped."""
    start = text.rfind("\n", 0, pos)
    prev = text.rfind("\n", 0, start)
    return text[prev + 1:start].strip()


def classify(src):
    """-> (wrapped runs, covered labels, faults)."""
    wrapped, covered, faults = [], [], []
    for m in CALL.finditer(src):
        line_no = src.count("\n", 0, m.start()) + 1
        if code_line_before(src, m.start() + 1) == 'pwr_hold("net");':
            hold = src.rfind('pwr_hold("net");', 0, m.start())
            hold = src.rfind("\n", 0, hold) + 1
            res = src.find("me_rend_resume();", m.end())
            if res < 0:
                faults.append("line %d: wrapped suspend with no resume" % line_no)
                continue
            eol = src.find("\n", res)
            nxt = src[eol + 1:src.find("\n", eol + 1)].strip()
            if nxt != "pwr_release();":
                faults.append("line %d: me_rend_resume() is not followed by "
                              "pwr_release() (%r)" % (line_no, nxt))
                continue
            wrapped.append(src[hold:src.find("\n", eol + 1) + 1])
            continue
        window = src[max(0, m.start() - 1600):m.end() + 400]
        hit = [c for c in COVERED if c[1] in window]
        if len(hit) != 1:
            faults.append("line %d: me_rend_suspend() neither wrapped in "
                          "pwr_hold(\"net\") .. pwr_release() nor a known "
                          "covered site" % line_no)
            continue
        covered.append(hit[0][0])
    return wrapped, covered, faults


def covered_claims(src):
    """The text behind each COVERED reason."""
    faults = []
    main = marked_function(src, "main")
    boot_hold = main.rfind('pwr_hold("boot");')
    boot_rel = main.find("pwr_release();            /* the boot's hold")
    site = main.find("Bring the wireless session up around the booted core.")
    if not (0 <= boot_hold < site < boot_rel):
        faults.append("boot bring-up is not between the boot hold and its release")
    td = main.find('pwr_hold("teardown");')
    site = main.find("The renderer's buffers: kept when they are the volatile")
    if not (0 <= td < site):
        faults.append("the teardown's suspend is not after pwr_hold(\"teardown\")")
    start = function(src, "mgift_start")
    body = re.sub(r"\s+", " ", start[start.index("{"):])
    if body != '{ pwr_hold("mgift"); mgift_start_io(); pwr_release(); }':
        faults.append("mgift_start does not hold around mgift_start_io: " + body)
    if len(re.findall(r"\bmgift_start_io\(\);", src)) != 1:
        faults.append("mgift_start_io() has a caller besides mgift_start")
    return faults


RUN_HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned Buttons; } SceCtrlData;
#define PSP_CTRL_CIRCLE 0x2000
#define CABLE_PROTOCOL "c"
#define RL_MODE_CABLE 1
static struct { const char *nick; } g_pcfg;
static const char *g_silent_group = "g", *g_silent_nick = "n", *group = "g", *nick = "n";
static int g_gblink_role = 1, g_gba_cable, as_host, is_host, rc;
static unsigned g_ol_cancel_down; static int g_ol_cancelled;
static void *g_ol_cancel_lv; static struct { int x; } lv;
static int hold, min_hold_before_resume = 99, rend_on = 1, resumed_under, order_bad;
static void pwr_hold(const char *why) { (void)why; hold++; }
static void pwr_release(void) { hold--; }
static void me_rend_suspend(void) { if (hold <= 0) order_bad = 1; rend_on = 0; }
static void me_rend_resume(void) { resumed_under = hold; rend_on = 1; }
static void note(void) { if (!rend_on && hold < min_hold_before_resume) min_hold_before_resume = hold; }
/* the bring-up's own hold, as production's net_bringup: taken and dropped
 * inside, with the renderer still off when it returns */
static int net_bringup(int h, const char *g, const char *n, int p)
{ (void)h; (void)g; (void)n; (void)p; pwr_hold("net"); note(); pwr_release(); note(); return 0; }
static int online_bringup_cancel(void) { return 0; }
static void online_psp_cancel_hook(int (*c)(void)) { (void)c; }
static const char *online_build_id(void) { return "b"; }
static int online_psp_up(int h, const char *b, unsigned t)
{ (void)h; (void)b; (void)t; note(); return 0; }
static void sceCtrlPeekBufferPositive(SceCtrlData *p, int n) { (void)n; p->Buttons = 0; }
static void fe_np_set_gb_protocol(const char *p) { (void)p; }
int main(void)
{
   g_pcfg.nick = "n";
   @RUN@
   if (order_bad) { puts("FAIL: the renderer was suspended outside the hold"); return 1; }
   if (min_hold_before_resume < 1) { puts("FAIL: the hold count reached 0 with the renderer off"); return 1; }
   if (resumed_under < 1) { puts("FAIL: the renderer came back on with no hold up"); return 1; }
   if (hold != 0 || !rend_on) { puts("FAIL: the run left a hold up or the renderer off"); return 1; }
   puts("ok");
   return 0;
}
'''

NOTICE_HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int hold = 1, notice_hold = -1, notices; static const char *last_why = "";
static unsigned seen_ms; static const char *seen[4];
static void pwr_hold(const char *why) { hold++; last_why = why; }
static void pwr_release(void) { if (hold > 0) hold--; }
static void ui_notice(const char *t, const char *a, const char *b, const char *c, unsigned ms)
{ notices++; notice_hold = hold; seen[0] = t; seen[1] = a; seen[2] = b; seen[3] = c; seen_ms = ms; }
@FUNCTION@
int main(void)
{
   boot_stop_notice("T", "one", "two", NULL, 30000);
   if (notices != 1 || strcmp(seen[0], "T") || strcmp(seen[1], "one") ||
       strcmp(seen[2], "two") || seen[3] || seen_ms != 30000)
   { puts("FAIL: the notice is not the one asked for"); return 1; }
   if (notice_hold != 0) { puts("FAIL: the hold was up while the notice waited"); return 1; }
   if (hold != 1 || strcmp(last_why, "boot")) { puts("FAIL: the exit runs without the boot hold"); return 1; }
   puts("ok");
   return 0;
}
'''

LOST_HARNESS = r'''
#include <stdio.h>
#include <string.h>
typedef struct { unsigned Buttons; } SceCtrlData;
static unsigned now_us = 4000000000u;      /* the low word wraps in the run */
static unsigned pad_from_us = 0xFFFFFFFFu, start_us, frames, swaps, texts;
static int holds, releases, in_overlay, order_bad;
static void pwr_hold(const char *why) { (void)why; holds++; }
static void pwr_release(void) { releases++; }
static int scePowerLock(int t) { (void)t; holds++; return 0; }
static int scePowerUnlock(int t) { (void)t; releases++; return 0; }
static unsigned sceKernelGetSystemTimeLow(void) { return now_us; }
static void sceDisplayWaitVblankStart(void) { now_us += 16667u; frames++; }
static void sceCtrlPeekBufferPositive(SceCtrlData *p, int n)
{ (void)n; p->Buttons = (now_us - start_us >= pad_from_us) ? 0x4000u : 0; }
static void vid_overlay_begin(int dim) { (void)dim; if (in_overlay) order_bad = 1; in_overlay = 1; }
static void vid_text_center(int y, const char *t, unsigned c)
{ (void)y; (void)c; if (!in_overlay || !t || !*t) order_bad = 1; texts++; }
static void vid_overlay_end(void) { if (!in_overlay) order_bad = 1; in_overlay = 0; }
static void vid_swap(void) { if (in_overlay) order_bad = 1; swaps++; }
@FUNCTION@
static unsigned run(unsigned pad_from)
{
   pad_from_us = pad_from; start_us = now_us; frames = swaps = texts = 0;
   wake_me_lost_notice();
   return now_us - start_us;
}
int main(void)
{
   unsigned us = run(0xFFFFFFFFu);                  /* nobody presses */
   if (holds || releases) { puts("FAIL: the notice takes or drops a power hold"); return 1; }
   if (us < 3000000u || us > 3100000u) { printf("FAIL: untouched, the notice is up %u us", us); return 1; }
   if (order_bad || swaps != frames || texts != 2 * frames)
   { puts("FAIL: the notice is not drawn and presented every frame"); return 1; }
   us = run(0);                                     /* a key down from the wake on */
   if (us < 500000u || us > 600000u) { printf("FAIL: with a key down the notice is up %u us", us); return 1; }
   us = run(1500000u);                              /* a press at 1.5 s */
   if (us < 1500000u || us > 1600000u) { printf("FAIL: a press at 1.5 s ends it at %u us", us); return 1; }
   if (holds || releases) { puts("FAIL: the notice takes or drops a power hold"); return 1; }
   puts("ok");
   return 0;
}
'''

CANCEL_HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned Buttons; } SceCtrlData;
#define PSP_CTRL_CIRCLE 0x2000
typedef struct { const char *room, *l1, *l2, *l3, *footer; int busy, frac; } ui_linking_view;
static unsigned pad; static int draws, swaps, evts, order_bad;
static char drawn_l2[40], drawn_footer[40], drawn_l1[40];
static void sceCtrlPeekBufferPositive(SceCtrlData *p, int n) { (void)n; p->Buttons = pad | 0x40; }
#define fe_evt(...) (evts++)
static void ui_linking_draw(const ui_linking_view *v)
{ draws++; snprintf(drawn_l1, 40, "%s", v->l1); snprintf(drawn_l2, 40, "%s", v->l2);
  snprintf(drawn_footer, 40, "%s", v->footer ? v->footer : "(default)"); }
static void vid_swap(void) { if (swaps != draws - 1) order_bad = 1; swaps++; }
static unsigned g_ol_cancel_down; static int g_ol_cancelled;
static ui_linking_view *g_ol_cancel_lv;
@FUNCTION@
#define CHECK(c, what) do { if (!(c)) { puts("FAIL: " what); return 1; } } while (0)
int main(void)
{
   ui_linking_view lv; char l2[40] = "Finding the room (up to 15 s)...";
   memset(&lv, 0, sizeof(lv)); lv.l1 = "Joining Online"; lv.l2 = l2; lv.busy = 1;
   /* O was down when the screen came up (ui_net_action's seed): not a press */
   g_ol_cancel_lv = &lv; g_ol_cancel_down = PSP_CTRL_CIRCLE; g_ol_cancelled = 0; pad = PSP_CTRL_CIRCLE;
   CHECK(online_bringup_cancel() == 0 && !draws && !evts, "an O held from before is taken for a press");
   pad = 0;
   CHECK(online_bringup_cancel() == 0 && !draws, "no press, and something was drawn or cancelled");
   pad = PSP_CTRL_CIRCLE;
   CHECK(online_bringup_cancel() == 1 && g_ol_cancelled == 1 && evts == 1, "a new O press does not cancel");
   CHECK(draws == 1 && swaps == 1 && !order_bad, "the press does not draw and present the screen once");
   CHECK(!strcmp(drawn_l2, "Cancelling...") && !strcmp(drawn_footer, "") &&
         !strcmp(drawn_l1, "Joining Online"), "the screen drawn at the press does not say Cancelling... with no footer");
   CHECK(lv.busy == 1, "the ring stopped");
   pad = 0;
   CHECK(online_bringup_cancel() == 1 && draws == 1 && evts == 1, "a later poll draws again or forgets the cancel");
   pad = PSP_CTRL_CIRCLE;
   CHECK(online_bringup_cancel() == 1 && draws == 1 && swaps == 1, "a second press draws again");
   /* no screen registered: the cancel still works, nothing is drawn */
   g_ol_cancel_lv = NULL; g_ol_cancel_down = 0; g_ol_cancelled = 0; pad = PSP_CTRL_CIRCLE;
   CHECK(online_bringup_cancel() == 1 && draws == 1, "a cancel with no screen draws or fails");
   puts("ok");
   return 0;
}
'''

QUIET_HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static unsigned now_us; static int g_ss_me_sync_fail = 1, idle_calls, evts;
static unsigned idle_at[4]; static char line[160];
static int sceKernelDelayThread(unsigned us) { now_us += us; return 0; }
/* busy for the first 5 ms, idle after */
static int me_host_idle(void) { if (idle_calls < 4) idle_at[idle_calls] = now_us; idle_calls++; return now_us >= 5000; }
static int me_host_up(void) { return 1; }
static void fe_evt(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap); evts++; }
static char note[160]; static int notes;
static void standby_note(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vsnprintf(note, sizeof(note), fmt, ap); va_end(ap); notes++; }
@FUNCTION@
int main(void)
{
   sleep_sim_me_quiet();
   if (idle_calls != 2 || idle_at[0] != 1000 || idle_at[1] != 10000)
   { printf("FAIL: me_host_idle sampled %d times at %u, %u us\n", idle_calls, idle_at[0], idle_at[1]); return 1; }
   if (evts != 1 || strcmp(line, "standby me_quiet idle_1ms=0 idle_10ms=1 up=1 sim=1"))
   { printf("FAIL: the log line is %d x \"%s\"\n", evts, line); return 1; }
   if (notes != 1 || strcmp(note, "me_quiet idle_1ms=0 idle_10ms=1 up=1 sim=1"))
   { printf("FAIL: the standby.log line is %d x \"%s\"\n", notes, note); return 1; }
   puts("ok");
   return 0;
}
'''


NOME_HARNESS = r'''
#include <stdio.h>
#include <string.h>
#define fe_evt(...) ((void)0)
#define SLEEP_SIM_ME_SYNC_FAIL() 0
#define SLEEP_SIM_ME_QUIET() ((void)0)
#define MER_VM_UNLOCK() (unlocks++)
static struct { int standby; } g_pcfg = { 1 };
static int g_mer_vmem, g_me_rend, loaded, up, retained, unlocks, syncs, sync_ok;
static void *g_mer_desc, *g_mer_cap[2], *g_mer_stage[2], *g_mer_work;
static void *g_mer_sh_vram, *g_mer_sh_oam, *g_mer_sh_pal;
static int g_mer_sh_on, g_mer_ready, g_mer_pending, g_mer_last_presented;
static int g_pwr_had_host, g_pwr_slept, ring_r, ring_w;
static volatile int g_pwr_me_lost;
static void me_standby_step(const char *s) { (void)s; }
static void wake_snapshot(void) {}
static void vid_blank_all(void) {}
static int me_rend_active(void) { return g_me_rend; }
static void me_rend_teardown(const char *w) { (void)w; g_me_rend = 0; }
static int me_host_up(void) { return up; }
static int me_host_loaded(void) { return loaded; }
static int me_host_retained(void) { return retained; }
static int me_host_handed_back(void) { return 0; }
static void me_host_shutdown(void) { loaded = 0; }
static int me_host_sync(unsigned t) { (void)t; syncs++; return up && sync_ok ? 0 : -1; }
@FUNCTIONS@
static int sleep_as(int vmem, int ld, int u, int ok)
{
   g_mer_vmem = vmem; loaded = ld; up = u; sync_ok = ok; retained = 0;
   g_pwr_me_lost = 0; g_pwr_slept = 0; unlocks = syncs = 0;
   me_standby_down();
   return g_pwr_me_lost;
}
#define CHECK(c, what) do { if (!(c)) { puts("FAIL: " what); return 1; } } while (0)
int main(void)
{
   CHECK(!sleep_as(0, 0, 0, 0) && g_pwr_slept && !syncs && !unlocks,
         "no ME (module not loaded): the sleep is taken for a lost engine");
   CHECK(!sleep_as(0, 1, 0, 0) && g_pwr_slept && !syncs && !unlocks,
         "no ME (never booted, loaded and dead): the sleep is taken for a lost engine");
   CHECK(!sleep_as(1, 1, 1, 1) && syncs == 1 && unlocks == 1,
         "a healthy engine's sleep is not a plain confirmed release");
   CHECK(sleep_as(1, 1, 1, 0) == 1 && unlocks == 1,
         "an engine that was up and fails its sync is not marked lost");
   CHECK(sleep_as(1, 1, 0, 0) == 1 && unlocks == 1,
         "an engine the watchdog gave up on is not marked lost");
   puts("ok");
   return 0;
}
'''


def run_c(source):
    """-> (returncode, output)."""
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp) / "t.c", Path(tmp) / "t"
        c.write_text(source, encoding="utf-8")
        cc = subprocess.run(["gcc", "-std=gnu99", "-w", "-O1", str(c), "-o", str(exe)],
                            capture_output=True, text=True)
        if cc.returncode:
            return 100, cc.stdout + cc.stderr
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr


def check_all(src):
    """-> list of failures (empty = pass) for the TU text `src`."""
    fails = []
    wrapped, covered, faults = classify(src)
    fails += faults
    if len(wrapped) != WRAPPED_SITES:
        fails.append("%d wrapped me_rend_suspend() runs, expected %d" % (len(wrapped), WRAPPED_SITES))
    for call in REND_OFF_CALLS:
        if src.count(call) != 1:
            fails.append("a bring-up made in play does not go through "
                         "net_bringup_rend_off: " + call)
    bare = [ln.strip() for ln in src.splitlines()
            if re.search(r"\bnet_bringup\(", ln) and not ln.lstrip().startswith(("*", "/*"))]
    if sorted(bare) != sorted(NET_BRINGUP_OTHER):
        fails.append("net_bringup() is called outside net_bringup_rend_off and "
                     "the boot: %s" % sorted(set(bare) - set(NET_BRINGUP_OTHER)))
    if sorted(covered) != sorted(c[0] for c in COVERED):
        fails.append("covered sites are %s" % sorted(covered))
    fails += covered_claims(src)
    for i, run in enumerate(wrapped):
        rc, out = run_c(RUN_HARNESS.replace("@RUN@", run))
        if rc:
            fails.append("wrapped run %d: %s" % (i + 1, out.strip()))
    # 2. the boot's notice
    rc, out = run_c(NOTICE_HARNESS.replace("@FUNCTION@", extract.extract(src, "fn:boot_stop_notice")))
    if rc:
        fails.append("boot_stop_notice: " + out.strip())
    main = marked_function(src, "main")
    first_hold = main.find('pwr_hold("boot");')
    if first_hold < 0 or "ui_notice(" in main[first_hold:]:
        fails.append("main() calls ui_notice directly after the boot's hold is taken")
    if len(re.findall(r"\bboot_stop_notice\(", main)) != 3:
        fails.append("main() does not show its three boot notices through boot_stop_notice")
    # 3. the online cancel
    rc, out = run_c(CANCEL_HARNESS.replace("@FUNCTION@", marked_function(src, "online_bringup_cancel")))
    if rc:
        fails.append("online_bringup_cancel: " + out.strip())
    wiring = re.search(r"g_ol_cancel_lv = &lv;\s*online_psp_cancel_hook\(online_bringup_cancel\);"
                       r"\s*rc = online_psp_up\([^;]*\);\s*online_psp_cancel_hook\(NULL\);"
                       r"\s*g_ol_cancel_lv = NULL;", src)
    if not wiring:
        fails.append("ui_net_action does not give the cancel its LINKING screen for the bring-up only")
    # 4. the harness's ME-quiet log
    rc, out = run_c(QUIET_HARNESS.replace("@FUNCTION@", marked_function(src, "sleep_sim_me_quiet")))
    if rc:
        fails.append("sleep_sim_me_quiet: " + out.strip())
    # 5. no Media Engine is not a lost one
    fns = "\n".join((
        "static int me_rend_drop_vmem(void);",
        marked_function(src, "me_rend_release_vmem"),
        marked_function(src, "me_rend_drop_vmem"),
        marked_function(src, "me_standby_down")))
    rc, out = run_c(NOME_HARNESS.replace("@FUNCTIONS@", fns))
    if rc:
        fails.append("no ME: " + out.strip())
    down = marked_function(src, "me_standby_down")
    if not re.search(r"release_result = me_rend_drop_vmem\(\);\s*SLEEP_SIM_ME_QUIET\(\);", down):
        fails.append("me_standby_down does not take the quiet log right after the drop")
    # 6. the wake that ends the session: its notice, with no hold
    rc, out = run_c(LOST_HARNESS.replace("@FUNCTION@", extract.extract(src, "fn:wake_me_lost_notice")))
    if rc:
        fails.append("wake_me_lost_notice: " + out.strip())
    wake = extract.extract(src, "fn:wake_service")
    lost = re.search(r"\n   if \(g_wake_menu && g_pwr_me_lost\)\n   \{\n(.*?)\n   \}\n", wake, re.S)
    if not lost or not re.search(r"wake_me_lost_notice\(\);.*g_running = 0;\s*return WAKE_QUIT;",
                                 lost.group(1), re.S):
        fails.append("wake_service does not end the session after the ME-lost notice")
    elif re.search(r"\bpwr_(hold|release)\w*\(|\bscePower(Lock|Unlock)\(", lost.group(1)):
        fails.append("wake_service takes or drops a power hold on the ME-lost wake")
    return fails


MUTANTS = (
    ("net_bringup_rend_off: no outer hold",
     '   pwr_hold("net");\n   me_rend_suspend();\n   rc = net_bringup(is_host, group, nick, 0);',
     '   me_rend_suspend();\n   rc = net_bringup(is_host, group, nick, 0);'),
    ("net_bringup_rend_off: released before the renderer is back",
     '   rc = net_bringup(is_host, group, nick, 0);\n   me_rend_resume();\n   pwr_release();',
     '   rc = net_bringup(is_host, group, nick, 0);\n   pwr_release();\n   me_rend_resume();'),
    ("net_bringup_rend_off: the hold taken after the suspend",
     '   pwr_hold("net");\n   me_rend_suspend();\n   rc = net_bringup(is_host, group, nick, 0);',
     '   me_rend_suspend();\n   pwr_hold("net");\n   rc = net_bringup(is_host, group, nick, 0);'),
    ("silent_frame: the bring-up as it was",
     "      rc = net_bringup_rend_off(as_host, g_silent_group, g_silent_nick);",
     "      me_rend_suspend();\n      rc = net_bringup(as_host, g_silent_group, g_silent_nick, 0);\n"
     "      me_rend_resume();"),
    ("gblink_service: a bare net_bringup",
     "      rc = net_bringup_rend_off(g_gblink_role == 1, g_silent_group, g_pcfg.nick);",
     "      rc = net_bringup(g_gblink_role == 1, g_silent_group, g_pcfg.nick, 0);"),
    ("ui_net_action: the ad hoc bring-up as it was",
     "   rc = net_bringup_rend_off(is_host, group, g_pcfg.nick);",
     "   me_rend_suspend();\n   rc = net_bringup(is_host, group, g_pcfg.nick, 0);\n   me_rend_resume();"),
    ("a new unwrapped bring-up",
     "static void mgift_start(void)\n{",
     "static void mgift_again(void)\n{\n   me_rend_suspend();\n   me_rend_resume();\n}\n"
     "static void mgift_start(void)\n{"),
    ("mgift_start without its hold",
     '   pwr_hold("mgift");\n   mgift_start_io();', '   mgift_start_io();'),
    ("boot_stop_notice keeps the hold",
     "   pwr_release();\n   ui_notice(title, l1, l2, l3, hold_ms);",
     "   ui_notice(title, l1, l2, l3, hold_ms);"),
    ("boot_stop_notice does not take the hold back",
     '   ui_notice(title, l1, l2, l3, hold_ms);\n   pwr_hold("boot");',
     "   ui_notice(title, l1, l2, l3, hold_ms);"),
    ("main: a notice shown under the hold",
     '         boot_stop_notice("SAVE FILE COULD NOT BE READ",',
     '         ui_notice("SAVE FILE COULD NOT BE READ",'),
    ("cancel: nothing drawn",
     "         ui_linking_draw(g_ol_cancel_lv);\n         vid_swap();", ""),
    ("cancel: drawn but not presented",
     "         ui_linking_draw(g_ol_cancel_lv);\n         vid_swap();",
     "         ui_linking_draw(g_ol_cancel_lv);"),
    ("cancel: the old line stays",
     '         g_ol_cancel_lv->l2 = "Cancelling...";\n', ""),
    ("cancel: the footer still offers O",
     '         g_ol_cancel_lv->footer = "";\n', ""),
    ("cancel: the screen is never registered",
     "         g_ol_cancel_lv = &lv;\n", ""),
    ("no ME: the release asks any loaded module, pool or not",
     "   if (g_mer_vmem && me_host_loaded() && !me_host_retained() &&",
     "   if (me_host_loaded() && !me_host_retained() &&"),
    ("no ME: every sleep is a lost engine",
     "   if (release_result < 0)\n   {", "   if (release_result <= 0)\n   {"),
    ("quiet: one sample only",
     "   sceKernelDelayThread(9000);\n   idle_10ms = me_host_idle();",
     "   idle_10ms = idle_1ms;"),
    ("quiet: sampled at once",
     "   sceKernelDelayThread(1000);\n   idle_1ms = me_host_idle();",
     "   idle_1ms = me_host_idle();\n   sceKernelDelayThread(1000);"),
    ("quiet: the event log only (its writer is stopped by then)",
     '   standby_note("me_quiet idle_1ms=%d idle_10ms=%d up=%d sim=%d",\n'
     "                idle_1ms, idle_10ms, up, sim);\n", ""),
    ("lost notice: under a hold released at its end",
     "   SceCtrlData pd;\n   do\n   {\n      vid_overlay_begin(1);\n"
     '      vid_text_center(110, "The Media Engine stopped during sleep.", 0xFFFF);',
     '   SceCtrlData pd;\n   pwr_hold("wake");\n   do\n   {\n      vid_overlay_begin(1);\n'
     '      vid_text_center(110, "The Media Engine stopped during sleep.", 0xFFFF);'),
    ("lost notice: the hold dropped after it",
     "   } while ((unsigned)sceKernelGetSystemTimeLow() - t0 < 3000000u);\n}\n"
     "/* @extract:end fn:wake_me_lost_notice */",
     "   } while ((unsigned)sceKernelGetSystemTimeLow() - t0 < 3000000u);\n   pwr_release();\n}\n"
     "/* @extract:end fn:wake_me_lost_notice */"),
    ("lost wake: a hold around the notice, in wake_service",
     "      wake_me_lost_notice();\n      g_pwr_slept = 0;",
     '      pwr_hold("wake");\n      wake_me_lost_notice();\n      pwr_release();\n      g_pwr_slept = 0;'),
    ("lost notice: gone at the first frame",
     "   } while ((unsigned)sceKernelGetSystemTimeLow() - t0 < 3000000u);\n}\n"
     "/* @extract:end fn:wake_me_lost_notice */",
     "   } while (0);\n}\n/* @extract:end fn:wake_me_lost_notice */"),
    ("quiet: not taken on the failed sync",
     "      SLEEP_SIM_ME_QUIET();   /* harness: the engine, 1 ms and 10 ms on */\n", ""),
)


def main():
    fails = check_all(MAIN)
    for f in fails:
        print("FAIL: " + f)
    if fails:
        return 1
    print("renderer holds: %d wrapped runs executed, %d covered sites; boot notice; "
          "online cancel; ME-quiet log; no ME is not a lost ME; ME-lost notice: PASS"
          % (WRAPPED_SITES, len(COVERED)))
    for label, old, new in MUTANTS:
        if MAIN.count(old) != 1:
            print("FAIL: mutant %r no longer applies (%d matches)" % (label, MAIN.count(old)))
            return 1
        got = check_all(MAIN.replace(old, new))
        if not got:
            print("FAIL: mutant survived: " + label)
            return 1
        print("mutant (%s): failed as it must: %s" % (label, got[0][:90]))
    print("sleep holds: %d mutants fail" % len(MUTANTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
