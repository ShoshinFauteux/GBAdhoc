#!/usr/bin/env python3
"""A sleep taken from an in-game sub-screen wakes to a working display.

A sleep taken with the START+SELECT menu open used to wake to a black
screen: the menu's branch of the game loop ends its iteration with
`continue`, ABOVE the wake overlay + rebuild, so g_pwr_rebuild stayed set,
g_wake_hold suppressed every swap onto the blanked screen, and the renderer
was never rebuilt.  The SELECT+TRIANGLE pause screen had no park point at
all.

Extracted, unchanged, from psp/main_psp.c: the game loop's in-game-menu
branch and its in-game present's wake call (both brace-matched out of
main()), wake_service, loop_park, loop_unpark, wake_overlay_yield and the
g_wake_hold macro; from psp/ui_psp.c: the wake overlay
(ui_wake_menu) with its yield hook and ui_swallow_pad.  Stubbed: the
callback's half (the hold simulates teardown + RESUME_COMPLETE), drawing, pad.

  1. Menu open, sleep, wake: the overlay comes up, Continue rebuilds once,
     the menu is still open and SWAPS again, and the answer's X (still held)
     is not read by the menu as a press.
  2. The same, answered `Quit to game list`: the loop leaves for the browser,
     nothing rebuilt.
  3. The SELECT+TRIANGLE pause screen (the overlay, modal, renderer live):
     a switch slid there makes the overlay LEAVE without parking; the loop
     goes round and parks at its own top; the overlay is back as the wake
     overlay, swallows what is held at the wake, and Continue rebuilds.
  4. Nothing pending: wake_service is a no-op.
  5. The pause screen yields but no sleep follows: the pause screen is back.
  6. Wiring in main(): the hook is registered before the game loop, the
     in-game present still runs wake_service, the loop top still parks.

Then mutants of the production text, each of which must fail.
"""
from pathlib import Path
import re
import subprocess
import tempfile

from test_psp_runtime import function

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "psp/main_psp.c").read_text(encoding="utf-8")
UI = (ROOT / "psp/ui_psp.c").read_text(encoding="utf-8")

TOKENS = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
                    r"'(?:\\.|[^'\\])*'|[{}]", re.S)


def braced(source, start):
    depth = 0
    for token in TOKENS.finditer(source, start):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:token.end()]
    raise AssertionError("unclosed block")


MENU_HEAD = "      if (ui_active())\n      {\n         /* Core paused"


def menu_block(main_src):
    i = main_src.find(MENU_HEAD)
    assert i >= 0, "the game loop's in-game menu branch not found"
    return braced(main_src, i)


PRESENT_HEAD = "      {\n         int w = wake_service();\n         if (w)\n"


def present_block(main_src):
    i = main_src.find(PRESENT_HEAD)
    assert i >= 0, "the in-game present's wake call not found"
    return braced(main_src, i)


def static_fn(source, signature):
    i = source.find(signature + "\n{")
    assert i >= 0, signature + " not found"
    return braced(source, i)


def production(main_src, ui_src):
    parts = []
    for pattern in (r"^#define g_wake_hold .*$", r"^#define WAKE_QUIT .*$",
                    r"^#define WAKE_YIELD .*$"):
        m = re.search(pattern, main_src, re.M)
        assert m, pattern + " not found"
        parts.append(m.group(0))
    m = re.search(r"^static int \(\*g_wake_yield\)\(void\);$", ui_src, re.M)
    assert m, "g_wake_yield not found"
    parts.append(m.group(0))
    for name in ("ui_set_wake_yield", "ui_swallow_pad", "ui_wake_menu"):
        parts.append(function(ui_src, name))
    for name in ("loop_park", "loop_unpark", "wake_overlay_yield"):
        parts.append(function(main_src, name))
    ws = function(main_src, "wake_service")
    peek = "sceCtrlPeekBufferPositive(&pd, 1);\n         pad_swallow_arm"
    if peek in ws:
        ws = ws.replace(peek, "answer_pad_read(&pd);\n         pad_swallow_arm")
    parts.append(ws)
    parts.append("static int menu_iteration(void)\n{\n   do\n   {\n" +
                 menu_block(main_src) +
                 "\n      return 1;   /* fell through: menu closed */\n"
                 "   } while (0);\n   return 0;   /* the branch's continue */\n}")
    # One game-loop iteration out of the menu: top, pad read, the in-game
    # present's wake call (extracted), then the core reads g_pad.
    parts.append("static int game_iteration(void)\n{\n   do\n   {\n"
                 "      loop_top();\n"
                 "      game_frames++;\n"
                 "      g_pad = game_pad[game_frames & 63];\n" +
                 present_block(main_src) +
                 "\n      core_seen[game_frames & 63] = g_pad;  /* retro_run */\n"
                 "      core_frames++;\n"
                 "      return 1;\n"
                 "   } while (0);\n   return 0;   /* the present's continue */\n}")
    return "\n\n".join(parts)


STUB = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define FE_EVT_ONLY(x) ((void)(x))
#define fe_evt(...) ((void)0)
#define LOOP_PHASE(n) ((void)(n))
#define UISCRIPT_DUMP() ((void)0)
#define PSP_CTRL_UP     0x0010u
#define PSP_CTRL_DOWN   0x0040u
#define PSP_CTRL_CROSS  0x4000u
#define PSP_CTRL_CIRCLE 0x2000u
#define UI_WAKE_YIELD 2
#define VID_SCR_W 480
#define VID_SCR_H 272
#define FE_FONT_H 16
#define C_BG_BOT 0
#define C_TITLE 1
#define C_DIM 2
#define C_ACCENT 3
#define C_SEL 4
#define C_ITEM 5
#define PSP_FILE_PATH_CAP 256
#define ROOM_LABEL_CAP 32
#define RELAUNCH_GAMELIST 2
#define RELAUNCH_PROFILE 1
#define FE_CONSOLE_GBA 0
#define LK_NONE 0
#define SQ_TRACE(s) do { strncat(trace, s " ", sizeof(trace) - strlen(trace) - 1); } while (0)
typedef struct { unsigned Buttons; } SceCtrlData;
typedef struct { int x; } theme;
typedef enum { UI_ACT_NONE, UI_ACT_RESUME, UI_ACT_SAVESTATE, UI_ACT_LOADSTATE,
               UI_ACT_NET_HOST, UI_ACT_NET_JOIN, UI_ACT_NET_DISCONNECT,
               UI_ACT_NET_MGIFT, UI_ACT_NET_MGIFT_STOP, UI_ACT_EXIT,
               UI_ACT_GAMELIST, UI_ACT_RELAUNCH, UI_ACT_RESET } ui_action;
static const theme THM_LIGHT, THM_DARK;
static const theme *g_thm;
static char trace[1024];
static struct { int theme, console, me_mode, osd_wireless; const char *last_rom; } g_pcfg;
static volatile int g_running = 1;
static volatile int g_pwr_slept, g_pwr_rebuild, g_pwr_park_req, g_pwr_parked;
static volatile int g_pwr_park_ack, g_pwr_sleep_pending;
static int g_wake_menu, g_wake_have = 1, g_relaunch;
static uint16_t g_wake_frame_buf[16];
static uint16_t *g_wake_frame = g_wake_frame_buf;
static unsigned g_wake_w = 240, g_wake_h = 160;
static unsigned ring_r, ring_w;
static struct { int live; } g_turbo;
static struct { int down; } g_sq;
static const char *exit_reason;
static unsigned g_pad, g_prev_pad;
static int g_wake_shot;
static char state_path[PSP_FILE_PATH_CAP], g_link_room[ROOM_LABEL_CAP];
static const char *g_session_info = "";
static int g_link_wait, g_link_menu, g_link_kind, g_net_up, g_mg_on;
static int menu_active = 1, menu_frames, menu_presses;
static int rend_up = 1, rebuilds, blanks;
static int ov_frames, ov_swaps, ov_answer_frame, screen_swaps;
static int ov_switch_at;           /* overlay frame whose hook sees a switch */
static int switch_fired;
static int hold_sleeps = 1;        /* 0: the park ends with no sleep */
static unsigned ov_pad[64];        /* the pad, per overlay frame (1-based) */
static unsigned game_pad[64];      /* the pad, per game iteration (1-based) */
static unsigned core_seen[64];     /* g_pad as the core read it */
static int game_frames, core_frames;
/* PR #16's pad swallow, which wake_service arms on Continue (main_psp.c:
 * pad_swallow_arm, g_wake_answered); the swallow itself is test_menu_press's. */
static unsigned swallow_armed;
static int g_wake_answered;
static void pad_swallow_arm(unsigned held) { swallow_armed |= held; }
/* The answer's pad, read once more after the overlay: as held at the
 * answer, and not an overlay frame (production calls the same peek). */
static void answer_pad_read(SceCtrlData *pd) { pd->Buttons = ov_pad[ov_frames]; }
static void loop_top(void);

/* ---- the callback's half, simulated inside the hold ------------------- */
static void sleep_quiesce_down(void) { SQ_TRACE("qdown"); g_sq.down = 1; }
static void sleep_park_hold(void)
{
   SQ_TRACE("hold");
   assert(g_pwr_parked && g_pwr_park_ack);
   /* power_cb: parked -> me_standby_down (renderer down, VRAM blanked) */
   if (hold_sleeps) { g_pwr_slept = 1; rend_up = 0; blanks++; }
   /* RESUME_COMPLETE */
   g_pwr_park_req = 0; g_pwr_sleep_pending = 0;
   g_pwr_rebuild = g_pwr_slept;
}
static void sleep_quiesce_up(const char *why) { (void)why; SQ_TRACE("qup"); g_sq.down = 0; }
static void me_standby_up(void)
{
   SQ_TRACE("rebuild");
   rebuilds++; g_pwr_slept = 0; g_pwr_rebuild = 0; rend_up = 1;
}
static void pwr_hold(const char *w) { (void)w; }
static void pwr_release(void) {}
static void home_host_disarm(void) {}
static void ctl_turbo_stop(void *t) { (void)t; }
static int me_rend_active(void) { return rend_up; }
static int me_rend_vmem_held(void) { return rend_up; }
static unsigned sceKernelGetSystemTimeLow(void) { return 0; }

/* ---- drawing and the pad ---------------------------------------------- */
static void vid_overlay_begin(int c) { (void)c; }
static void vid_overlay_end(void) {}
static void vid_image_screen(const uint16_t *f, int a, int b, int w, int h, int al)
{ (void)f; (void)a; (void)b; (void)w; (void)h; (void)al; }
static void vid_rect(int x, int y, int w, int h, int c, int a)
{ (void)x; (void)y; (void)w; (void)h; (void)c; (void)a; }
static void vid_gradient_a(int x, int y, int w, int h, int c, int a, int b)
{ (void)x; (void)y; (void)w; (void)h; (void)c; (void)a; (void)b; }
static void vid_logo(int x, int y, int c, int a) { (void)x; (void)y; (void)c; (void)a; }
static void vid_text(int x, int y, const char *t, int c) { (void)x; (void)y; (void)t; (void)c; }
static int vid_text_w(const char *t) { return (int)strlen(t) * 8; }
static int vid_band_y(int y, int h) { (void)h; return y; }
static void footer(const char *t) { (void)t; }
static void demo_dump_named(const char *n) { (void)n; }
static void sceDisplayWaitVblankStart(void) {}
static int in_overlay;
static void vid_swap(void)
{
   if (in_overlay) ov_swaps++;
   else screen_swaps++;
}
static void sceCtrlPeekBufferPositive(SceCtrlData *pd, int n)
{
   (void)n;
   ov_frames++;
   assert(ov_frames < 64 || !"the overlay never answered");
   pd->Buttons = ov_pad[ov_frames];
   if (pd->Buttons & PSP_CTRL_CROSS) ov_answer_frame = ov_frames;
}

/* ---- the menu's surroundings ------------------------------------------ */
static int ui_frame_edges(unsigned pad)
{
   unsigned edges = pad & ~g_prev_pad;
   g_prev_pad = pad;
   return (int)edges;
}
static ui_action ui_frame(unsigned pad, int s, const char *i)
{
   (void)s; (void)i;
   menu_frames++;
   if (ui_frame_edges(pad) & PSP_CTRL_CROSS) menu_presses++;
   return UI_ACT_NONE;
}
static int ui_active(void) { return menu_active; }
static void ui_set_turbo_locked(int l) { (void)l; }
static int link_session_any(void) { return 0; }
static void link_ui_view(void) {}
static int link_locks_states(void) { return 0; }
static int state_save_slot(const char *p, unsigned s) { (void)p; (void)s; return 0; }
static int ui_state_slot(void) { return 1; }
static int ui_state_save_done(int rc) { (void)rc; return 1; }
static int ui_state_load_done(int rc) { (void)rc; return 1; }
static int psp_state_path_for_slot(char *o, size_t n, const char *p, unsigned s)
{ (void)o; (void)n; (void)p; (void)s; return -1; }
static int psp_state_load(const char *p, const char *w) { (void)p; (void)w; return -1; }
static int psp_game_reset(void) { return -2; }
static void osd_toast(const char *f, ...) { (void)f; }
static void ui_net_action(int h) { (void)h; }
static int fe_host_gblink_active(void) { return 0; }
static void fe_host_gblink_request_end(void) {}
static void net_teardown(void) {}
static void mgift_start(void) {}
static void mgift_stop(int q) { (void)q; }
static void pcfg_save(void) {}
static void osd_chip_session(const char *s) { (void)s; }
static void osd_draw(void) {}
static void link_chip(void) {}

@FUNCTIONS@

/* The game loop's top, as main() runs it (no sleep simulator). */
static void loop_top(void)
{
   (void)loop_park();
   loop_unpark("resume");
}

static void machine(void)
{
   trace[0] = '\0';
   g_running = 1; menu_active = 1; rend_up = 1;
   g_pwr_slept = g_pwr_rebuild = g_pwr_park_req = g_pwr_parked = 0;
   g_pwr_park_ack = g_pwr_sleep_pending = 0;
   g_wake_menu = 0; g_wake_have = 1; g_relaunch = 0; g_sq.down = 0;
   exit_reason = NULL; g_pad = 0; g_prev_pad = 0;
   menu_frames = menu_presses = rebuilds = blanks = 0;
   ov_frames = ov_swaps = ov_answer_frame = screen_swaps = 0;
   ov_switch_at = 0; switch_fired = 0; hold_sleeps = 1;
   swallow_armed = 0; g_wake_answered = 0;
   memset(ov_pad, 0, sizeof(ov_pad));
   memset(game_pad, 0, sizeof(game_pad));
   memset(core_seen, 0xAA, sizeof(core_seen));
   game_frames = core_frames = 0;
   ui_set_wake_yield(wake_overlay_yield);
}

/* The overlay's hook, with the switch slid (once) between two of its frames. */
static int hook_with_switch(void)
{
   int r, now = 0;
   if (!switch_fired && ov_frames + 1 == ov_switch_at)
   {
      switch_fired = now = 1;
      g_pwr_park_req = 1;
      g_pwr_sleep_pending = 1;
   }
   r = wake_overlay_yield();
   assert(!now || strcmp(trace, "") == 0 || !"no park inside the overlay");
   return r;
}

int main(void)
{
   /* 1. Menu open, sleep, wake, Continue. */
   machine();
   assert(menu_iteration() == 0);             /* the menu, live */
   assert(screen_swaps == 1 && menu_frames == 1);
   g_pwr_park_req = 1; g_pwr_sleep_pending = 1;   /* the switch */
   loop_top();
   assert(strcmp(trace, "qdown hold qup ") == 0);
   assert(g_pwr_rebuild == 1 && blanks == 1 && !rend_up);
   ov_pad[3] = PSP_CTRL_CROSS;                /* Continue */
   {
      /* the overlay runs from inside the menu branch */
      int r;
      in_overlay = 1;
      r = menu_iteration();
      in_overlay = 0;
      assert(r == 0);
   }
   assert(ov_answer_frame == 3);              /* the overlay came up */
   assert(rebuilds == 1 && rend_up);          /* ...and Continue rebuilt */
   assert(strstr(trace, "qup rebuild") != NULL);
   assert(!g_pwr_slept && !g_pwr_rebuild && !g_wake_menu);
   assert(g_running && ui_active() && exit_reason == NULL);
   assert(swallow_armed == PSP_CTRL_CROSS && g_pad == 0);   /* PR #16 */
   g_pad = PSP_CTRL_CROSS;                    /* the answer, still held */
   assert(menu_iteration() == 0);
   assert(screen_swaps == 2);                 /* the menu is on screen again */
   assert(menu_presses == 0);                 /* ...and X was not a press */
   g_pad = 0;
   assert(menu_iteration() == 0);
   g_pad = PSP_CTRL_CROSS;                    /* a real press still is one */
   assert(menu_iteration() == 0);
   assert(menu_presses == 1 && screen_swaps == 4 && rebuilds == 1);

   /* 2. The same sleep, answered `Quit to game list`. */
   machine();
   g_pwr_park_req = 1; g_pwr_sleep_pending = 1;
   loop_top();
   ov_pad[2] = PSP_CTRL_DOWN;
   ov_pad[4] = PSP_CTRL_CROSS;
   in_overlay = 1;
   assert(menu_iteration() == 0);
   in_overlay = 0;
   assert(!g_running && g_relaunch == RELAUNCH_GAMELIST);
   assert(exit_reason && strcmp(exit_reason, "game_list") == 0);
   assert(rebuilds == 0 && !g_pwr_slept && !g_pwr_rebuild);

   /* 3. The pause screen: modal, renderer live, a switch slid there. */
   machine();
   menu_active = 0;
   ui_set_wake_yield(hook_with_switch);
   g_wake_menu = 1;                           /* SELECT+TRIANGLE */
   ov_switch_at = 2;                          /* slid after its 1st frame */
   in_overlay = 1;
   assert(game_iteration() == 0);             /* the overlay left... */
   in_overlay = 0;
   assert(strcmp(trace, "") == 0);            /* ...without parking in it */
   assert(ov_frames == 1 && ov_swaps == 1);   /* nothing drawn after the ask */
   assert(core_frames == 0 && exit_reason == NULL && g_running);
   assert(g_wake_menu == 1 && g_pwr_park_req == 1);   /* back after the park */
   ov_pad[2] = PSP_CTRL_CROSS;                /* held as the console wakes */
   ov_pad[3] = PSP_CTRL_CROSS;
   ov_pad[4] = 0;
   ov_pad[5] = PSP_CTRL_CROSS;                /* the real answer */
   game_pad[2] = PSP_CTRL_UP;                 /* read before the overlay */
   in_overlay = 1;
   assert(game_iteration() == 1);             /* the loop's top parked */
   in_overlay = 0;
   assert(strcmp(trace, "qdown hold qup rebuild ") == 0);
   assert(blanks == 1 && rebuilds == 1 && rend_up && !g_sq.down);
   assert(ov_frames == 5);                    /* the wake's X was swallowed */
   assert(swallow_armed == PSP_CTRL_CROSS);   /* PR #16: the answer's X ... */
   assert(core_seen[game_frames & 63] == 0);  /* ...and not the core's pad */
   assert(g_wake_answered == 1);              /* the caller drops its edges */
   assert(g_running && !g_pwr_slept && !g_pwr_rebuild && !g_wake_menu);

   /* 4. Nothing pending: nothing happens. */
   machine();
   assert(wake_service() == 0 && ov_frames == 0 && rebuilds == 0);
   loop_top();
   assert(trace[0] == '\0');

   /* 5. The pause screen yields, the park ends with no sleep: it is back. */
   machine();
   menu_active = 0;
   hold_sleeps = 0;
   ui_set_wake_yield(hook_with_switch);
   g_wake_menu = 1;
   ov_switch_at = 2;
   in_overlay = 1;
   assert(game_iteration() == 0 && trace[0] == '\0');
   ov_pad[3] = PSP_CTRL_CROSS;
   assert(game_iteration() == 1);
   in_overlay = 0;
   assert(strcmp(trace, "qdown hold qup ") == 0 && rebuilds == 0);
   assert(ov_answer_frame == 3 && !g_wake_menu && g_running);

   puts("sleep from the menu / pause screen / play: overlay, one rebuild, "
        "the pause screen parks at the loop top, menu swaps again, held X "
        "swallowed by the menu, quit, no-op pass");
   return 0;
}
'''


def run(functions, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-sleepmenu-") as folder:
        cfile = Path(folder) / "sm.c"
        binary = Path(folder) / "sm"
        cfile.write_text(STUB.replace("@FUNCTIONS@", functions))
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-unused-variable",
                        "-Wno-unused-but-set-variable",
                        str(cfile), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, result.stdout, result.stderr))
        return result


def wiring(main_src):
    """main(): hook registered before the game loop; the loop top parks;
    the in-game present still runs the wake.  Returns a list of failures."""
    bad = []
    body = main_src[main_src.index("int main(int argc, char *argv[])"):]
    reg = body.find("ui_set_wake_yield(wake_overlay_yield);")
    loop = body.find("   while (g_running)\n   {\n      unsigned frames;")
    if reg < 0 or loop < 0 or reg > loop:
        bad.append("hook not registered before the game loop")
    if not re.search(r"\(void\)loop_park\(\);\s*loop_unpark\(\"resume\"\);",
                     body):
        bad.append("loop top no longer parks")
    if len(re.findall(r"\bloop_park\(\)", main_src)) != 1:   # the loop top
        bad.append("loop_park called from somewhere other than the loop top")
    present = body.find("      FE_LP_SET(FELP_L_PRES0);\n      me_video_present();")
    ingame = body.rfind(PRESENT_HEAD, 0, present)
    if present < 0 or ingame < 0 or ingame < body.find(MENU_HEAD):
        bad.append("in-game present no longer runs wake_service")
    return bad


good = production(MAIN, UI)
print(run(good, True).stdout.strip())
assert wiring(MAIN) == [], wiring(MAIN)
print("wiring: hook registered, loop top is the only park, in-game wake "
      "intact")

MENU_CALL = """         if (g_pwr_rebuild || g_wake_menu)
         {
            int w = wake_service();
            if (w == WAKE_QUIT)
               exit_reason = "game_list";
            else if (!w)
               ui_swallow_pad();
            continue;
         }
"""
YIELD_FN = "static int wake_overlay_yield(void)\n{\n   return g_pwr_park_req;\n}"
MUTANTS = (
    # (label, file, old, new, expected text in stderr)
    ("the menu branch skips the wake",
     "main", MENU_CALL, "", "ov_answer_frame == 3"),
    ("the menu reads the overlay's X as its own press",
     "main", "            else if (!w)\n               ui_swallow_pad();\n", "",
     "menu_presses == 0"),
    ("Continue does not rebuild",
     "main", "         me_standby_up();\n         pwr_release();",
     "         pwr_release();", "rebuilds == 1"),
    ("the overlay parks from inside itself",
     "main", YIELD_FN,
     "static int wake_overlay_yield(void)\n{\n   (void)loop_park();\n"
     "   loop_unpark(\"overlay\");\n   return 0;\n}",
     "no park inside the overlay"),
    ("the overlay never yields",
     "ui", "      if (g_wake_yield && g_wake_yield())\n         return UI_WAKE_YIELD;\n",
     "", "the overlay never answered"),
    ("a yield is not reopened after the park",
     "main", "         g_wake_menu = 1;\n         fe_evt(\"wake_menu yield",
     "         fe_evt(\"wake_menu yield", "g_wake_menu == 1"),
    ("the present reads a yield as a quit",
     "main", "            if (w == WAKE_QUIT)\n               exit_reason = \"game_list\";\n            continue;",
     "            exit_reason = \"game_list\";\n            continue;",
     "exit_reason == NULL"),
    ("the present runs the frame after a yield",
     "main", "            if (w == WAKE_QUIT)\n               exit_reason = \"game_list\";\n            continue;",
     "            if (w == WAKE_QUIT)\n            {\n               exit_reason = \"game_list\";\n               continue;\n            }",
     "game_iteration() == 0"),
    ("Continue does not swallow the answer's X (PR #16)",
     "main", "         pad_swallow_arm(pd.Buttons);\n         g_pad = 0;\n",
     "         g_pad = 0;\n", "swallow_armed == PSP_CTRL_CROSS"),
    ("Continue leaves the pad read before the overlay to the core",
     "main", "         pad_swallow_arm(pd.Buttons);\n         g_pad = 0;\n",
     "         pad_swallow_arm(pd.Buttons);\n", "core_seen[game_frames & 63] == 0"),
    ("the overlay reads the wake's held X as the answer",
     "ui", "      if (first)\n         { edges = 0; first = 0; }\n",
     "      first = 0;\n", "ov_frames == 5"),
)
for label, which, old, new, expect in MUTANTS:
    src = MAIN if which == "main" else UI
    bad_src = src.replace(old, new, 1)
    assert bad_src != src, label
    funcs = production(bad_src, UI) if which == "main" else \
        production(MAIN, bad_src)
    bad = run(funcs, False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant (%s): failed as it must" % label)

for label, old, new in (
        ("hook never registered",
         "   ui_set_wake_yield(wake_overlay_yield);", "   /* none */"),
        ("in-game present loses the wake",
         PRESENT_HEAD, "      {\n         int w = 0;\n         if (w)\n"),
        ("a second park point inside the overlay",
         YIELD_FN, YIELD_FN.replace("   return g_pwr_park_req;",
                                    "   return loop_park();"))):
    bad_src = MAIN.replace(old, new, 1)
    assert bad_src != MAIN, label
    assert wiring(bad_src), label
    print("mutant (%s): failed as it must" % label)
