#!/usr/bin/env python3
"""The six player exits no fixture reaches, run on the host against the production text.

The Lab's fixtures end by autoexit, the autopilot or "Quit to game list".  These six
ways a PLAYER leaves a game had no test of any kind:

  X-1  the in-game menu's Exit                  -> a clean exit to the XMB
  X-2  START+SELECT held 1.5 s                  -> exit, reason user_abort
  X-3  a save file that cannot be read          -> a notice, then an exit that never
                                                   touches the .sav
  X-4  HOME in play                             -> a clean exit to the XMB
  X-5  a profile change (Media Engine on/off)   -> the settings saved, a clean
                                                   teardown, then the relaunch
  X-6  "Reset game" that fails (GB/GBC)         -> back to the game list

Each trigger is the production text, extracted from main_psp.c (the shell with its
fe/ fragments expanded) and compiled unchanged:
  * the in-game menu's `case UI_ACT_RESET:` .. `default:` (X-1, X-5, X-6);
  * the START+SELECT block of the frame loop (X-2);
  * the load-failed block (X-3);
  * exit_cb (X-4);
and each runs into main()'s real teardown, from `ui_set_wake_yield(NULL)` (the
loop's end) to `FE_EXIT_GAME()`, with inproc_refusal: every call is a stub that
records its name, `goto session_top` becomes `return 1` (the list again).

What is asserted, per path: the loop ends; the exit code and reason handed to the
log and to the handoff; that the save is flushed (fe_host_shutdown) with the writer
stopped, before the audio and the display go; that X-3 never reaches the save; that
only X-5 and a refused X-6 relaunch, and what they relaunch into.
Then each trigger is broken in the production text (a mutant) and must be caught.

The slices are found by anchor and the stubs cover the names HW-1 and HW-2 add
(ui_settings_save, ui_settings_flush, g_exit_req, exit_sim), so the test runs on the
tree it was written on (b7d0cafd) and on the trees with either branch merged.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import extract  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SOURCE = extract.read_source(str(ROOT / "psp/main_psp.c"))

TD_BEGIN = "   ui_set_wake_yield(NULL);   /* no game loop: nothing to park */\n"
TD_END = "   FE_EXIT_GAME();\n   return 0;\n}"


def function(source, name):
    match = re.search(r"^(?:static )?(?:inline )?(?:__attribute__\(\([^\n]*?\)\)\s*)?"
                      r"(?:int|void|const char \*)\s*" + name + r"\([^;]*?\)\s*\{",
                      source, re.M)
    assert match, "production function not found: " + name
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]', re.S)
    depth = 0
    for token in tokens.finditer(source, match.end() - 1):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():token.end()]
    raise AssertionError("unclosed function: " + name)


def between(text, begin, end, what, keep_end=True):
    assert text.count(begin) == 1, "%s: begin anchor found %d times" % (what, text.count(begin))
    a = text.index(begin)
    b = text.index(end, a)
    return text[a:b + (len(end) if keep_end else 0)]


# Epoch C C1 (fe/teardown.inc.c): the early exits call exit_early_begin/_end.  On such a
# tree the helpers' text (the XE_ enum through exit_early_end) is compiled with the rest,
# and the load-failed slice ends at its exit_early_end call instead of FE_EXIT_GAME().
HELPERS_BEGIN = "enum\n{\n   XE_EVT_FIRST"
LOADFAIL_ENDS = ("      FE_EXIT_GAME();\n", "\"load_failed\", 0);")


# Epoch C C1c: main's shared teardown is session_end(have_variant), the in-process
# return, then process_end(exit_code, exit_reason); both helpers join HELPERS.
TD_C1C = ("   session_end(have_variant);\n", "   process_end(exit_code, exit_reason);\n")


def helpers(source):
    if source.count(HELPERS_BEGIN) == 0:
        return ""
    a = source.index(HELPERS_BEGIN)
    last = "process_end" if "void process_end(" in source[a:] else "exit_early_end"
    end = function(source[a:], last)
    text = source[a:a + source[a:].index(end) + len(end)]
    # Epoch C C3-1: the in-process return's reset is session_reset (fe/session.inc.c)
    if "void session_reset(" in source:
        text += "\n" + function(source, "session_reset")
    return text


def production(source):
    # Epoch C C3-2b: the pass per game reads as the goto it replaced; C3-2c: the
    # session's phases as main()'s text
    main = extract.epochc_phases(extract.epochc_unloop(extract.extract(source, "fn:main"),
                                                       source), source)
    if TD_C1C[0] in main:
        td = between(main, TD_C1C[0], TD_C1C[1], "teardown")
    else:
        td = between(main, TD_BEGIN, TD_END, "teardown", keep_end=False) + "   FE_EXIT_GAME();\n"
    assert td.count("goto session_top;") == 1, "the return's goto"
    lf_begin = "      int refusal = fe_host_battery_refusal();\n"
    lf_end = [e for e in LOADFAIL_ENDS if e in main[main.index(lf_begin):]]
    assert lf_end, "load failed: no end anchor"
    lf_end = min(lf_end, key=lambda e: main.index(e, main.index(lf_begin)))
    return {
        "HELPERS": helpers(source),
        "EXIT_CB": function(source, "exit_cb"),
        "REFUSAL": function(source, "inproc_refusal"),
        "TEARDOWN": td.replace("goto session_top;", "return 1;"),
        # the in-game menu's actions that end the session
        "MENU": between(main, "         case UI_ACT_RESET:\n", "         default:\n            break;\n",
                        "menu cases"),
        # the frame loop's START+SELECT block (its braces included)
        "CHORD": between(main, "      {\n         static unsigned abort_held;\n",
                         "            abort_held = 0;\n      }\n", "abort chord"),
        # the boot's load-failed block
        "LOADFAIL": between(main, lf_begin, lf_end, "load failed"),
    }


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
#define PSP_CTRL_SELECT 0x0001
#define PSP_CTRL_START  0x0008
#define FE_CONSOLE_GBA 0
enum { UI_ACT_NONE, UI_ACT_RESET, UI_ACT_EXIT, UI_ACT_GAMELIST, UI_ACT_RELAUNCH };

static char trace[8192];
static int census_same = 1, retain_ok = 1, reset_rc, battery_refusal;
static int relaunched = -1, exited, handoff_code = -99, notice_ms;
static char handoff_why[32], notice_title[64];

volatile int g_running = 1;
static volatile int g_exit_req;      /* HW-2 F1 (unused before it) */
static int g_wake_lost_hold;         /* HW-1 H1: the ME-lost wake's hold, handed to the teardown */
static void hit(const char *name) { strcat(trace, name); strcat(trace, " "); }

/* inproc_refusal's inputs */
static struct { int quick_return, sram_thread, me_mode, console, standby; } g_pcfg;
static int gbadhoc_net_touched, g_pwr_suspend;
extern unsigned badjump_recoveries __attribute__((weak));
static int me_host_was_dead(void) { return 0; }
static int me_host_handed_back(void) { return 0; }
#define INPROC_ME_UNLOAD_MUTANT() 0

/* main()'s locals and the frontend state the triggers and the teardown touch */
static int g_relaunch, have_variant, inproc_n, exit_code, g_wake_have,
           g_wake_menu, ff_engaged, ff_toggled, ff_last_mode, ff_last_me,
           g_pad_swallow, chord_frames, inproc_menu_armed, g_ff_uncapped,
           g_ff_mult, g_ff_user, g_ff_gate, g_blit_suppress, g_drew,
           g_me_rend_cfg, me_rend_cfg_boot, g_me_video_cfg, g_me_video,
           sram_thread_boot, g_autoload_state, autoload_boot,
           autoload_state_slot, g_autoload_from_shelf, g_turbo, g_ff_gate_st,
           g_dump_chord_pending, g_link_menu;
static unsigned prev_pad, g_wake_w, g_wake_h, g_pad;
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
static void io_thread_stop(void) { hit("writer_stop"); }
static void ui_settings_flush(void) { hit("settings_flush"); }
static void ui_settings_save(void) { hit("cfg_save"); }
static void pcfg_save(void) { hit("cfg_save"); }
static void exit_sim(const char *w) { (void)w; }
static unsigned fe_evt_drops(void) { return 0; }
static unsigned long long net_now_us(void) { return 1000000ull; }
static void handoff_set_extra(const char *x) { (void)x; }
static void fe_evt(const char *fmt, ...)
{
   char line[200];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   if (!strncmp(line, "inproc_", 7) || !strncmp(line, "exit ", 5) || !strncmp(line, "abort ", 6))
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

/* the triggers' own calls */
static int psp_game_reset(void) { hit("game_reset"); return reset_rc; }
static void osd_toast(const char *fmt, ...) { (void)fmt; hit("toast"); }
static int fe_host_battery_refusal(void) { return battery_refusal; }
static void ui_loading_finish(int ok) { (void)ok; hit("loading_finish"); }
static void ui_notice(const char *t, const char *a, const char *b, const char *c, int ms)
{
   (void)a; (void)b; (void)c;
   hit("notice");
   snprintf(notice_title, sizeof(notice_title), "%s", t);
   notice_ms = ms;
}
/* HW-1: the boot's stop notice drops the boot hold while it waits */
static void boot_stop_notice(const char *t, const char *a, const char *b, const char *c, int ms)
{
   ui_notice(t, a, b, c, ms);
}

@EXIT_CB@

@REFUSAL@

/* Epoch C: the one exit sequence (fe/teardown.inc.c; empty before C1) */
@HELPERS@

static int teardown_tail(void)
{
@TEARDOWN@
   return 0;
}

/* the in-game menu's action switch (the cases that can end the session) */
static void menu_action(int act)
{
   switch (act)
   {
@MENU@
   }
}

/* one frame of the loop's START+SELECT watch */
static void chord_frame(void)
{
@CHORD@
}

/* the boot's "the game did not load" branch */
static int load_failed(void)
{
   {
@LOADFAIL@
   }
   return 0;
}

static void machine(void)
{
   trace[0] = '\0';
   g_running = 1;           /* in play */
   g_exit_req = 0;
   g_relaunch = 0;
   g_pcfg.quick_return = 1;
   g_pcfg.me_mode = 1;
   census_same = 1;
   retain_ok = 1;
   reset_rc = 0;
   relaunched = -1;
   exited = 0;
   handoff_code = -99;
   handoff_why[0] = '\0';
   notice_title[0] = '\0';
   notice_ms = 0;
   exit_code = 0;
   exit_reason = NULL;
   g_pad = 0;
}

#define HAS(s) (strstr(trace, (s)) != NULL)
#define BEFORE(a, b) (HAS(a) && HAS(b) && strstr(trace, (a)) < strstr(trace, (b)))
/* a clean end: the writer stopped, then the save flushed, then audio/display/log,
 * then the handoff -- and the exit last */
#define CLEAN_END() (BEFORE("writer_stop", "fe_host_shutdown") && BEFORE("fe_host_shutdown", "audio_stop") \
                     && BEFORE("audio_stop", "vid_term") && BEFORE("vid_term", "evt_shutdown") \
                     && BEFORE("evt_shutdown", "handoff") && BEFORE("handoff", "exit_game") && exited)

int main(void)
{
   int r, i;

   /* X-1: the in-game menu's Exit. */
   machine();
   menu_action(UI_ACT_EXIT);
   assert(g_running == 0 && g_relaunch == 0 && exit_reason == NULL);
   r = teardown_tail();
   assert(r == 0 && CLEAN_END() && relaunched == -1 && !HAS("relaunch"));
   assert(handoff_code == 0 && !strcmp(handoff_why, "-") && HAS("[exit code=0]"));
   assert(HAS("me_shutdown") && !HAS("unload") && !HAS("inproc_"));

   /* X-2: START+SELECT, held.  89 frames do nothing; a release starts over; the
    *      90th frame ends the run with its reason. */
   machine();
   g_pad = PSP_CTRL_START | PSP_CTRL_SELECT;
   for (i = 0; i < 89; i++)
      chord_frame();
   assert(g_running == 1 && exit_reason == NULL);
   g_pad = PSP_CTRL_START;                 /* one button let go */
   chord_frame();
   g_pad = PSP_CTRL_START | PSP_CTRL_SELECT;
   for (i = 0; i < 89; i++)
      chord_frame();
   assert(g_running == 1);
   chord_frame();
   assert(g_running == 0 && exit_reason && !strcmp(exit_reason, "user_abort") && g_relaunch == 0);
   r = teardown_tail();
   assert(r == 0 && CLEAN_END() && relaunched == -1);
   assert(handoff_code == 0 && !strcmp(handoff_why, "user_abort"));
   assert(HAS("[exit code=0 reason=user_abort]"));

   /* X-3: the save could not be read.  A notice the player can read, then an exit
    *      that never reaches the save: no writer stop/sync, no fe_host_shutdown. */
   machine();
   battery_refusal = 1;
   load_failed();
   assert(BEFORE("loading_finish", "notice") && !strcmp(notice_title, "SAVE FILE COULD NOT BE READ"));
   assert(notice_ms >= 15000);
   assert(!HAS("fe_host_shutdown") && !HAS("writer_stop") && !HAS("cfg_save"));
   assert(HAS("[exit code=1 reason=load_failed battery=1]"));
   assert(BEFORE("notice", "evt_shutdown") && BEFORE("evt_shutdown", "handoff") && BEFORE("handoff", "exit_game"));
   assert(HAS("audio_stop") && HAS("vid_term") && exited && relaunched == -1);
   assert(handoff_code == 1 && !strcmp(handoff_why, "load_failed"));
   machine();
   battery_refusal = 2;                    /* no memory for the save: its own notice */
   load_failed();
   assert(!strcmp(notice_title, "NOT ENOUGH MEMORY") && !HAS("fe_host_shutdown") && handoff_code == 1);
   machine();
   battery_refusal = 0;                    /* any other load failure: no notice, same exit */
   load_failed();
   assert(!HAS("notice") && !HAS("fe_host_shutdown") && handoff_code == 1 && exited);

   /* X-4: HOME in play (the exit callback), then the loop's teardown. */
   machine();
   exit_cb(0, 0, NULL);
   assert(g_running == 0);
   r = teardown_tail();
   assert(r == 0 && CLEAN_END() && relaunched == -1);
   assert(handoff_code == 0 && !strcmp(handoff_why, "-") && HAS("[exit code=0]"));
   assert(BEFORE("home_stop", "net_teardown"));     /* HOME goes back to the system first */

   /* X-5: a profile change.  Saved, a clean teardown, the ME stopped, and the
    *      relaunch is the LAST thing before the exit -- into the app, not the list. */
   machine();
   menu_action(UI_ACT_RELAUNCH);
   assert(g_running == 0 && g_relaunch == RELAUNCH_PROFILE && HAS("cfg_save"));
   assert(exit_reason && !strcmp(exit_reason, "profile_change"));
   r = teardown_tail();
   assert(r == 0 && CLEAN_END() && relaunched == 0);
   assert(BEFORE("handoff", "relaunch") && BEFORE("relaunch", "exit_game"));
   assert(HAS("me_shutdown") && !HAS("unload") && !HAS("me_retain"));
   assert(handoff_code == 0 && !strcmp(handoff_why, "profile_change"));
   assert(HAS("[exit code=0 reason=profile_change]"));

   /* X-6: "Reset game" failed: no machine is left, so back to the game list.
    *      In this process when the return is proven ... */
   machine();
   reset_rc = -1;
   menu_action(UI_ACT_RESET);
   assert(g_running == 0 && g_relaunch == RELAUNCH_GAMELIST && HAS("cfg_save") && HAS("toast"));
   assert(exit_reason && !strcmp(exit_reason, "reset_failed"));
   r = teardown_tail();
   assert(r == 1 && !exited && relaunched == -1 && HAS("[inproc_return n="));
   assert(BEFORE("writer_stop", "fe_host_shutdown") && BEFORE("fe_host_shutdown", "unload"));
   assert(BEFORE("unload", "pwr_release") && !HAS("me_shutdown"));
   /* ... and by the relaunch when it is not (the census refuses). */
   machine();
   reset_rc = -1;
   menu_action(UI_ACT_RESET);
   census_same = 0;
   r = teardown_tail();
   assert(r == 0 && CLEAN_END() && relaunched == 1 && HAS("[inproc_refused why=census]"));
   assert(handoff_code == 0 && !strcmp(handoff_why, "reset_failed"));
   /* controls: a reset that worked, or was refused, leaves the game running */
   for (i = 0; i >= -3; i--)
   {
      if (i == -1)
         continue;
      machine();
      reset_rc = i;
      menu_action(UI_ACT_RESET);
      assert(g_running == 1 && g_relaunch == 0 && exit_reason == NULL && HAS("toast"));
   }
   /* control: "Quit to game list" from the same switch */
   machine();
   menu_action(UI_ACT_GAMELIST);
   assert(g_running == 0 && g_relaunch == RELAUNCH_GAMELIST && !strcmp(exit_reason, "game_list"));

   puts("exit_paths: menu Exit, START+SELECT, unreadable save, HOME, profile "
        "change, reset failed pass");
   return 0;
}
'''


def build(parts):
    text = STUB
    for k, v in parts.items():
        text = text.replace("@%s@" % k, v)
    return text


def run(parts, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-exitpaths-") as folder:
        cfile = Path(folder) / "xp.c"
        binary = Path(folder) / "xp"
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


good = production(SOURCE)
print(run(good, True).stdout.strip())

MUTANTS = (
    ("X-1 the menu's Exit does not end the loop", "MENU",
     "         case UI_ACT_EXIT:\n            g_running = 0;\n", "         case UI_ACT_EXIT:\n"),
    ("X-2 the held chord does not end the loop", "CHORD",
     "               exit_reason = \"user_abort\";\n               g_running = 0;\n",
     "               exit_reason = \"user_abort\";\n"),
    ("X-2 the chord fires after one second", "CHORD",
     "if (++abort_held == 90)", "if (++abort_held == 60)"),
    ("X-2 the chord's reason is lost", "CHORD",
     "               exit_reason = \"user_abort\";\n", ""),
    ("X-3 the load-failed exit flushes the save it could not read", "LOADFAIL",
     ("      evt_shutdown();\n", "      exit_early_end("),
     ("      fe_host_shutdown();\n      evt_shutdown();\n", "      fe_host_shutdown();\n      exit_early_end(")),
    ("X-3 the unreadable-save notice is gone", "LOADFAIL",
     "      if (refusal == 1)\n", "      if (refusal == 99)\n"),
    ("X-3 the load-failed exit hands off as a pass", "LOADFAIL",
     ("FE_HANDOFF(1, \"load_failed\")", "exit_early_end(XE_EVT_FIRST, 1, \"load_failed\""),
     ("FE_HANDOFF(0, \"load_failed\")", "exit_early_end(XE_EVT_FIRST, 0, \"load_failed\"")),
    ("X-3 the load-failed exit goes without closing the display (Epoch C helper)", "HELPERS",
     ("   if (f & XE_EVT_FIRST)\n   {\n      audio_stop();\n      vid_term();\n   }\n",),
     ("   if (f & XE_EVT_FIRST)\n   {\n      audio_stop();\n   }\n",)),
    ("X-4 the exit callback does not end the loop", "EXIT_CB",
     "   g_running = 0;\n", ""),
    ("X-5 the profile change relaunches into the game list", "MENU",
     "            g_relaunch = RELAUNCH_PROFILE;\n", "            g_relaunch = RELAUNCH_GAMELIST;\n"),
    ("X-5 the profile change does not relaunch", "MENU",
     "            g_relaunch = RELAUNCH_PROFILE;\n", ""),
    ("X-6 a failed reset keeps the dead machine running", "MENU",
     "               exit_reason = \"reset_failed\";\n               g_running = 0;\n",
     "               exit_reason = \"reset_failed\";\n"),
    ("X-6 a failed reset exits to the XMB", "MENU",
     "               g_relaunch = RELAUNCH_GAMELIST;\n               exit_reason = \"reset_failed\";\n",
     "               exit_reason = \"reset_failed\";\n"),
    ("the teardown no longer flushes the save", ("TEARDOWN", "HELPERS"),
     "\n   fe_host_shutdown();\n", "\n"),
    ("the teardown flushes the save before the writer has stopped", ("TEARDOWN", "HELPERS"),
     "\n   io_thread_stop();   /* ADR-0025: no other thread may touch the .sav */\n", "\n"),
    ("the teardown relaunches before the handoff", ("TEARDOWN", "HELPERS"),
     "   FE_HANDOFF(exit_code, exit_reason);", "   ;"),
)
n = 0
for what, part, old, new in MUTANTS:
    # old/new may list the forms of a site before and after Epoch C C1: the form this
    # tree has is mutated; a helper-only mutant is skipped on a tree without helpers
    # (C1c: the teardown's text may sit in main's slice or in the helpers: either part)
    olds = old if isinstance(old, tuple) else (old,)
    news = new if isinstance(new, tuple) else (new,)
    cands = part if isinstance(part, tuple) else (part,)
    parts = dict(good)
    hit_ = [(p, o, w) for p in cands for o, w in zip(olds, news) if parts[p].count(o) == 1]
    if cands == ("HELPERS",) and "exit_early_end(" not in parts["LOADFAIL"]:
        print("  mutant skipped (the load-failed exit does not use the Epoch C helper here): " + what)
        continue
    assert hit_, "mutant site gone: %s" % what
    p, o, w = hit_[0]
    parts[p] = parts[p].replace(o, w)
    run(parts, False)
    n += 1
    print("  mutant caught: " + what)
print("PASS: test_exit_paths (%d mutants)" % n)
