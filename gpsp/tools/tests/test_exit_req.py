#!/usr/bin/env python3
"""HOME during a quit (PLAN HW-2 F1), run on the host against the production text.

Extracted from main_psp.c (read_source: the shell with its fe/ fragments expanded)
and compiled unchanged:
  * exit_cb (fe/power.a) and inproc_refusal (fe/inproc.b);
  * main()'s teardown, from `ui_set_wake_yield(NULL)` (the loop's end) to
    `FE_EXIT_GAME()`: session_end, the in-process return and process_end.  Its
    `goto session_top` becomes `return 1` (the list again), the end `return 0`.
Every call is a stub that records its name; one of them can "press HOME" (call
the real exit_cb from inside it, as cb_thread would at that moment).

The four windows of PLAN §0.2-3, plus controls:
  A  no HOME: the in-process return goes back to the list with g_running = 1;
  B  HOME before the second refusal check (here: in the ME teardown):
     refused why=home, no unload, no relaunch, sceKernelExitGame;
  C  HOME after the refusal check, inside the unload (heap_snap): the return
     completes but g_running stays 0, so the list leaves at once (its HOME exit);
  D  HOME during a refused return (census mismatch, HOME in its ME shutdown):
     exit, not relaunch;
  E  a refused return without HOME relaunches (control: F1 is HOME-only);
  F  a profile relaunch with HOME in the teardown: exit, not relaunch;
  G  HOME in the browser: ui_browser returns -1 and main's list exit logs
     `exit code=0 reason=home` and hands off (0, "home"), not `no_rom` code 2;
     a path too long (-1 with g_running set) stays `no_rom` code 2.
Then each part of the fix is removed from the production text (a mutant) and
must be caught.
"""
from pathlib import Path
import re
import subprocess
import tempfile

import extract
from test_psp_runtime import function

ROOT = Path(__file__).resolve().parents[2]
SOURCE = extract.read_source(str(ROOT / "psp/main_psp.c"))
UI = (ROOT / "psp/ui_psp.c").read_text(encoding="utf-8")

TD_BEGIN = "   ui_set_wake_yield(NULL);   /* no game loop: nothing to park */\n"
TD_END = "   FE_EXIT_GAME();\n   return 0;\n}"
B9_BEGIN = "      if (br != 0)\n      {\n"


def refusal(source):
    m = re.search(r"^static const char \*inproc_refusal\(int have_variant\)\n\{.*?^\}\n",
                  source, re.S | re.M)
    assert m, "inproc_refusal not found"
    return m.group(0)


def teardown(source):
    main = extract.extract(source, "fn:main")
    a = main.index(TD_BEGIN)
    b = main.index(TD_END, a)
    body = main[a:b + len("   FE_EXIT_GAME();\n")]
    assert body.count("goto session_top;") == 1, "the return's goto"
    return body.replace("goto session_top;", "return 1;")


def list_exit(source):
    """main's `if (br != 0)` block after ui_browser (B9)."""
    main = extract.extract(source, "fn:main")
    a = main.index(B9_BEGIN)
    b = main.index("\n      }\n", a)
    return main[a:b + len("\n      }\n")]


def production(source):
    return {"EXIT_CB": function(source, "exit_cb"),
            "REFUSAL": refusal(source),
            "TEARDOWN": teardown(source),
            "LISTEXIT": list_exit(source)}


STUB = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GPSP_NO_TELEMETRY 1
#define GPSP_INPROC_RETURN 1
#define RELAUNCH_PROFILE  1
#define RELAUNCH_GAMELIST 2
#define HARNESS_INI "harness.ini"

static char trace[8192];
static const char *home_at;          /* the stub that "presses HOME" */
static int quick_return = 1, census_same = 1, retain_ok = 1;
static int relaunched = -1, exited, handoff_code = -99;
static char handoff_why[32];

volatile int g_running = 1;
static volatile int g_exit_req;
static int exit_cb(int arg1, int arg2, void *common);
static void hit(const char *name)
{
   strcat(trace, name);
   strcat(trace, " ");
   if (home_at && strcmp(home_at, name) == 0)
   {
      home_at = NULL;
      exit_cb(0, 0, NULL);
   }
}

/* inproc_refusal's inputs */
static struct { int quick_return, sram_thread; int stick; } g_pcfg;
static int gbadhoc_net_touched, g_pwr_suspend;
extern unsigned badjump_recoveries __attribute__((weak));
static int me_host_was_dead(void) { return 0; }
static int me_host_handed_back(void) { return 0; }
#define INPROC_ME_UNLOAD_MUTANT() 0

/* main()'s locals and the frontend state the teardown resets */
static int g_relaunch, have_variant, inproc_n, exit_code, g_wake_have,
           g_wake_menu, ff_engaged, ff_toggled, ff_last_mode, ff_last_me,
           g_pad_swallow, chord_frames, inproc_menu_armed, g_ff_uncapped,
           g_ff_mult, g_ff_user, g_ff_gate, g_blit_suppress, g_drew,
           g_me_rend_cfg, me_rend_cfg_boot, g_me_video_cfg, g_me_video,
           sram_thread_boot, g_autoload_state, autoload_boot,
           autoload_state_slot, g_autoload_from_shelf, g_turbo, g_ff_gate_st,
           g_dump_chord_pending;
static unsigned prev_pad, g_wake_w, g_wake_h;
static const char *exit_reason;
static void *g_wake_frame, *g_last_pix, *g_me_stage[2];
typedef struct { unsigned arena, used, chunks, fordblks, free, largest,
                 kfree, kmax; } heap_snap_t;
static heap_snap_t snap_pre;

static void ui_set_wake_yield(void *p) { (void)p; hit("wake_yield"); }
static void sleep_quiesce_up(const char *w) { (void)w; hit("quiesce_up"); }
static void play_lock_drop(const char *w) { (void)w; hit("play_lock_drop"); }
static void pwr_hold(const char *w) { (void)w; hit("pwr_hold"); }
static void pwr_release(void) { hit("pwr_release"); }
static void ui_set_backdrop(void *f, int w, int h) { (void)f; (void)w; (void)h; }
static void ui_return_hold(void) { hit("return_hold"); }
static void ui_return_tick(void) { hit("return_tick"); }
static void stall_watch_stop(void) {}
static void home_host_stop(void) { hit("home_stop"); }
static void home_host_forget(void) { hit("home_forget"); }
static void drprof_stop(unsigned f) { (void)f; }
static unsigned fe_host_frame_count(void) { return 600; }
static void me_rend_teardown(const char *w) { (void)w; hit("me_rend_teardown"); }
static void me_video_teardown(const char *w, int x) { (void)w; (void)x; }
static int me_host_retain(unsigned us) { (void)us; hit("me_retain"); return retain_ok; }
static void me_host_shutdown(void) { hit("me_shutdown"); }
static int me_host_loaded(void) { return 1; }
static int me_host_retained(void) { return retain_ok; }
static void net_teardown(void) { hit("net_teardown"); }
static void mgift_stop(int x) { (void)x; }
static void emu_boost_stop(void) { hit("boost_stop"); }
static void emu_boost_start(void) { hit("boost_start"); }
static void io_thread_stop(void) { hit("io_stop"); }
static void ui_settings_flush(void) { hit("settings_flush"); }
static unsigned fe_evt_drops(void) { return 0; }
static unsigned long long net_now_us(void) { return 1000000ull; }
static void handoff_set_extra(const char *x) { (void)x; }
static void fe_evt(const char *fmt, ...)
{
   char line[160];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   if (!strncmp(line, "inproc_", 7) || !strncmp(line, "exit ", 5))
   {
      strcat(trace, "[");
      strcat(trace, line);
      strcat(trace, "] ");
   }
}
static void fe_host_shutdown(void) { hit("fe_host_shutdown"); }
static unsigned plat_audio_hash(void) { return 0; }
static unsigned plat_audio_sample_count(void) { return 0; }
static void dynarec_release_translation_caches(void) { hit("unload"); }
static void (*dynarec_heap_tables_release)(void);
static void me_rend_suspend(void) { hit("me_rend_suspend"); }
static void sceKernelDcacheWritebackInvalidateAll(void) {}
static void sceKernelIcacheInvalidateAll(void) {}
static void fe_host_session_reset(void) {}
static void osd_reset(void) {}
static void ui_close(void) { hit("ui_close"); }
static void vid_ambient_drop(void) {}
static void audio_stop(void) { hit("audio_stop"); }
static void audio_ring_reset(void) {}
static void audio_start(void) { hit("audio_start"); }
static void heap_snap(heap_snap_t *s) { hit("heap_snap"); memset(s, 0, sizeof(*s)); }
static void heap_trace_report(const char *w) { (void)w; }
static int heap_snap_same(const heap_snap_t *a, const heap_snap_t *b)
{ (void)a; (void)b; return census_same; }
static long fe_ini_get_int(const char *f, const char *k, long d)
{ (void)f; (void)k; return d; }
static void psp_clock_apply(const char *w) { (void)w; hit("clock_apply"); }
static void ctl_turbo_stop(int *t) { (void)t; }
static void ui_set_turbo_locked(int x) { (void)x; }
static void vid_term(void) { hit("vid_term"); }
static void gpsp_rtc_peek(long long *a, long long *b, int *c) { *a = *b = 0; *c = 0; }
static long long psp_local_wallclock(void) { return 0; }
static void core_health_evt(const char *w) { (void)w; }
static void evt_shutdown(void) { hit("evt_shutdown"); }
#define FE_EVT_ONLY(x) ((void)(x))
#define FE_HANDOFF(code, why) \
   (hit("handoff"), handoff_code = (code), \
    snprintf(handoff_why, sizeof(handoff_why), "%s", (why) ? (why) : "-"))
static void relaunch_eboot(int gamelist) { hit("relaunch"); relaunched = gamelist; }
#define FE_EXIT_GAME() (hit("exit_game"), exited = 1)

@EXIT_CB@

@REFUSAL@

static int teardown_tail(void)
{
@TEARDOWN@
   return 0;
}

/* main's list exit (B9), after ui_browser returned br */
static int list_exit(int br)
{
@LISTEXIT@
   return 1;
}

static void machine(int relaunch, const char *home)
{
   trace[0] = '\0';
   home_at = home;
   g_running = 0;           /* the loop has ended (quit or HOME) */
   g_exit_req = 0;
   g_relaunch = relaunch;
   g_pcfg.quick_return = quick_return;
   census_same = 1;
   retain_ok = 1;
   relaunched = -1;
   exited = 0;
   handoff_code = -99;
   handoff_why[0] = '\0';
   exit_code = 0;
   exit_reason = NULL;
}

#define HAS(s) (strstr(trace, (s)) != NULL)

int main(void)
{
   int r;

   /* A: the in-process return, no HOME. */
   machine(RELAUNCH_GAMELIST, NULL);
   r = teardown_tail();
   assert(r == 1 && g_running == 1 && !exited && relaunched == -1);
   assert(HAS("[inproc_return n=0]") && HAS("unload"));
   /* F3: the settings flush runs on every quit, after the writer stopped
    *     (ADR-0025, PLAN §3.2 D16b) and before the SRAM flush. */
   assert(HAS("io_stop settings_flush ") &&
          strstr(trace, "settings_flush") < strstr(trace, "fe_host_shutdown"));

   /* B: HOME before the second refusal check: refused "home", no unload, the
    *    ME shut down for the refused tail, and an exit -- not a relaunch. */
   machine(RELAUNCH_GAMELIST, "me_rend_teardown");
   r = teardown_tail();
   assert(r == 0 && HAS("[inproc_refused why=home]"));
   assert(!HAS("unload") && !HAS("inproc_return"));
   assert(exited && relaunched == -1 && !HAS("relaunch"));

   /* C: HOME inside the unload, after the refusal check: the return
    *    completes (the unload cannot be undone), but the list must not loop:
    *    g_running stays 0 and ui_browser returns at once. */
   machine(RELAUNCH_GAMELIST, "heap_snap");
   r = teardown_tail();
   assert(r == 1 && HAS("[inproc_return n=") && g_running == 0 && g_exit_req);
   list_exit(-1);                        /* ui_browser: while (g_running) */
   assert(exited && handoff_code == 0 && !strcmp(handoff_why, "home"));
   assert(HAS("[exit code=0 reason=home]") && !HAS("no_rom"));

   /* D: HOME during a refused return (the census refuses; HOME lands in the
    *    refused tail's ME shutdown): an exit, not the relaunch. */
   machine(RELAUNCH_GAMELIST, NULL);
   census_same = 0;
   home_at = "audio_start";              /* the unload's last step */
   r = teardown_tail();
   assert(r == 0 && HAS("[inproc_refused why=census]"));
   assert(exited && relaunched == -1);
   machine(RELAUNCH_GAMELIST, NULL);
   census_same = 0;
   home_at = "vid_term";                 /* process_end, after the refusal */
   r = teardown_tail();
   assert(r == 0 && exited && relaunched == -1);

   /* E: control -- a refused return without HOME relaunches to the list. */
   machine(RELAUNCH_GAMELIST, NULL);
   census_same = 0;
   r = teardown_tail();
   assert(r == 0 && relaunched == 1 && exited);

   /* F: a profile relaunch with HOME in the teardown: an exit. */
   machine(RELAUNCH_PROFILE, "net_teardown");
   r = teardown_tail();
   assert(r == 0 && exited && relaunched == -1);
   assert(HAS("io_stop settings_flush "));   /* F3 on a HOME exit too */
   machine(RELAUNCH_PROFILE, NULL);
   r = teardown_tail();
   assert(r == 0 && relaunched == 0);    /* control */

   /* G: the list's own exits.  HOME in the list: code 0, "home"; a ROM path
    *    too long (ui_browser -1 with the app still running): no_rom, code 2. */
   machine(0, NULL);
   exit_cb(0, 0, NULL);
   list_exit(-1);
   assert(exited && handoff_code == 0 && !strcmp(handoff_why, "home"));
   machine(0, NULL);
   g_running = 1;
   list_exit(-1);
   assert(exited && handoff_code == 2 && !strcmp(handoff_why, "no_rom"));
   assert(HAS("[exit code=2 reason=no_rom]"));

   puts("exit_req: return, HOME before/after the refusal, in a refused "
        "return, on a profile relaunch, in the list, path too long pass");
   return 0;
}
'''


def build(parts):
    text = STUB
    for k, v in parts.items():
        text = text.replace("@%s@" % k, v)
    return text


def run(parts, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-exitreq-") as folder:
        cfile = Path(folder) / "er.c"
        binary = Path(folder) / "er"
        cfile.write_text(build(parts))
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-unused-variable",
                        "-Wno-unused-but-set-variable", "-Wno-unused-parameter",
                        str(cfile), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, result.stdout, result.stderr))
        return result


# The list's loop must be the one that lets a HOME through (case C relies on it).
assert re.search(r"g_prev_pad = 0xFFFFFFFFu;\n   while \(g_running\)\n", UI), \
    "ui_browser_run's loop no longer tests g_running"

good = production(SOURCE)
print(run(good, True).stdout.strip())

MUTANTS = (
    ("exit_cb does not latch HOME", "EXIT_CB",
     "   g_exit_req = 1;\n", ""),
    ("inproc_refusal ignores HOME", "REFUSAL",
     "   if (g_exit_req)\n      return \"home\";\n", ""),
    ("the return sets g_running = 1 unconditionally", "TEARDOWN",
     "g_running = !g_exit_req;", "g_running = 1;"),
    ("HOME does not cancel the relaunch", "TEARDOWN",
     "   if (g_exit_req)\n      g_relaunch = 0;\n", ""),
    ("the list's HOME exit says no_rom", "LISTEXIT",
     "!g_running", "0"),
    ("the teardown does not flush the settings (F3)", "TEARDOWN",
     "   ui_settings_flush();\n", ""),
)
for what, part, old, new in MUTANTS:
    parts = dict(good)
    assert parts[part].count(old) == 1, "mutant site gone: " + what
    parts[part] = parts[part].replace(old, new)
    run(parts, False)
    print("  mutant caught: " + what)

# F10: ui_notice (ui_psp.c) ends on HOME, not after its 15/30 s.
NOTICE_STUB = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned Buttons; } SceCtrlData;
static struct { int active; } g_loading;
volatile int g_running = 1;
static unsigned now_us, frames, home_frame;
static unsigned sceKernelGetSystemTimeLow(void) { return now_us; }
static void sceDisplayWaitVblankStart(void)
{ now_us += 16683; if (++frames == home_frame) g_running = 0; }
static int sceCtrlPeekBufferPositive(SceCtrlData *p, int n) { (void)n; p->Buttons = 0; return 1; }
static void vid_overlay_begin(int x) { (void)x; }
static void vid_overlay_end(void) {}
static void vid_swap(void) {}
static void page(const char *a, const char *b) { (void)a; (void)b; }
static void vid_text_center(int y, const char *s, int c) { (void)y; (void)s; (void)c; }
#define C_WARN 0
#define C_ITEM 0
#define C_DIM 0
@NOTICE@
int main(void)
{
   home_frame = 30;
   ui_notice("T", "a", NULL, NULL, 30000);
   assert(frames == 30);                 /* HOME ends it the frame it lands */
   g_running = 1; frames = 0; now_us = 0; home_frame = 0;
   ui_notice("T", "a", NULL, NULL, 1000);
   assert(frames >= 59 && frames <= 61);  /* control: the hold without HOME */
   puts("ui_notice: HOME ends the notice; the hold otherwise");
   return 0;
}
'''


def notice(ui):
    m = re.search(r"^void ui_notice\(.*?^\}\n", ui, re.S | re.M)
    assert m, "ui_notice not found"
    return m.group(0)


def run_notice(text, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-notice-") as folder:
        cfile = Path(folder) / "n.c"
        binary = Path(folder) / "n"
        cfile.write_text(NOTICE_STUB.replace("@NOTICE@", text))
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Werror", str(cfile),
                        "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, result.stdout, result.stderr))
        return result


nt = notice(UI)
print(run_notice(nt, True).stdout.strip())
old_n = "      if (!g_running)\n         break;\n"
assert nt.count(old_n) == 1, "F10 site gone"
run_notice(nt.replace(old_n, ""), False)
print("  mutant caught: ui_notice ignores HOME")
print("PASS: test_exit_req (%d mutants)" % (len(MUTANTS) + 1))
