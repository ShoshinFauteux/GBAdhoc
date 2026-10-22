#!/usr/bin/env python3
"""The button that answers an in-game UI is not the game's.

Closing the START+SELECT menu with Resume left its X held as the game got
the pad back, and the next pad read handed it to the core (Pokemon Crystal
read it as B).  The same for O (Resume), X on a state load, and the X or O
that answers the SELECT+TRIANGLE pause screen / wake overlay.

Taken as written from psp/main_psp.c: g_pad_swallow, pad_swallow_arm,
pad_swallow; from main(): the pad read's swallow, the menu branch's closing
arm and the HOME close; the overlay's Continue, found by its
`wake_menu choice=continue` event.  They are spliced into a hand-written
model of the loop's order (pad read, shortcuts, HOME, menu, the rewind
level, overlay, core).  Stubbed: the menu (closes on an X or O edge), the
overlay (answers on an X edge), ctl_shortcuts, the core, turbo and rewind
(which read g_pad, pad_new, ctl_fired and g_rw_held as the core runs).

  1. Menu, Resume with X: the X still held is not seen by the core until
     released; another button pressed meanwhile is; a new X is.
  2. The same with O.
  3. Menu closed by HOME with a button held: not the game's, this iteration
     (read before the close: nor its edges, shortcuts or rewind level) or
     the next, until released; a button first pressed after HOME is.
  4. Pause screen / wake overlay, Continue: this iteration's pad (read
     before the overlay), its edges, shortcuts and rewind level, and the
     answer's X still held, are not the core's; a button first pressed
     after the answer is.
  5. Nothing answered: the pad reaches the core untouched.
  6. A slow menu action (Host/Join, a state load): a button first pressed
     while it ran is the game's; only what was held at the answer is not.
  7. A button still swallowed when another close arms stays swallowed.
  8. Wiring: the swallow is at the pad read, before anything reads
     g_pad; the menu branch arms as it closes, with the pad it answered;
     HOME and the overlay's Continue arm with what is held and clear the
     iteration's pad and what was read from it; a game left for the game
     list in-process starts with nothing swallowed.

Then mutants of the production text, each of which must fail.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "psp/main_psp.c").read_text(encoding="utf-8")

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


def static_fn(source, signature):
    i = source.find(signature + "\n{")
    assert i >= 0, signature + " not found"
    return braced(source, i)


READ = re.compile(r"^      g_pad = pad_swallow\(g_pad\);.*\n", re.M)
MENU_HEAD = "      if (ui_active())\n      {\n         /* Core paused"
CLOSE = re.compile(r"\n( *if \(!ui_active\(\)\)\n *pad_swallow_arm\([^;\n]*\);\n)\s*$")
HOME_HEAD = "      if (home_pressed && ui_active() && ui_home_press())\n"
CONTINUE = 'fe_evt("wake_menu choice=continue'


def main_body(src):
    return src[src.index("int main(int argc, char *argv[])"):]


def home_close(src):
    """The HOME close statement (with its block, when it has one)."""
    i = src.find(HOME_HEAD)
    if i < 0:
        return ""
    rest = src[i + len(HOME_HEAD):]
    if rest.startswith("      {"):
        return HOME_HEAD + braced(rest, 0) + "\n"
    return HOME_HEAD + rest[:rest.index("\n") + 1]


def overlay_continue(src):
    """What the overlay's Continue runs between clearing g_wake_have and
    the leaving check: the arm and the cleared pad."""
    i = src.find(CONTINUE)
    if i < 0:
        return ""
    j = src.find("g_wake_have = 0;", i)
    k = src.find("if (!g_running)", j)
    return src[j + len("g_wake_have = 0;"):k] if 0 <= j < k else ""


def menu_close(src):
    """The menu branch's last statements before its final continue."""
    i = src.find(MENU_HEAD)
    if i < 0:
        return ""
    block = braced(src, i)
    end = block.rstrip()[:-1].rstrip()          # drop the closing brace
    assert end.endswith("continue;"), "menu branch no longer ends in continue"
    m = CLOSE.search(end[:-len("continue;")])
    return m.group(1) if m else ""


def inproc_reset(src):
    """The in-process return's per-game reset, before goto session_top."""
    i = src.find('fe_evt("inproc_return')
    j = src.find("goto session_top;", i)
    return src[i:j] if 0 <= i < j else ""


def production(src):
    parts = []
    m = re.search(r"^static unsigned g_pad_swallow;$", src, re.M)
    assert m, "g_pad_swallow not found"
    parts.append(m.group(0))
    parts.append(static_fn(src, "static void pad_swallow_arm(unsigned held)"))
    parts.append(static_fn(src, "static unsigned pad_swallow(unsigned pad)"))
    body = main_body(src)
    m = READ.search(body)
    read = m.group(0) if m else ""
    parts.append(
        "static int iteration(void)\n{\n   do\n   {\n"
        "      frame++;\n"
        "      g_pad = pads[frame];\n" + read +
        "      pad_new = g_pad & ~prev_pad;\n"
        "      ctl_fired = ctl_shortcuts(g_pad, pad_new);\n"
        "      prev_pad = g_pad;\n" + home_close(body) +
        "      if (ui_active())\n      {\n"
        "         ui_frame(g_pad);\n" + menu_close(body) +
        "         continue;\n      }\n"
        "      g_rw_held = (ctl_fired & FIRE_REWIND) != 0;\n"
        "      if (overlay_up)\n      {\n"
        "         overlay_up = 0;\n"
        "         ui_wake_menu();\n" + overlay_continue(src) + "\n"
        "      }\n"
        "      core_seen[frame] = g_pad;   /* retro_run */\n"
        "      new_seen[frame] = pad_new;  /* turbo_frame */\n"
        "      fired_seen[frame] = ctl_fired;\n"
        "      rw_seen[frame] = g_rw_held; /* psp_rewind_tick */\n"
        "      return 1;\n"
        "   } while (0);\n"
        "   core_seen[frame] = NOT_RUN;\n   return 0;\n}")
    return "\n\n".join(parts)


STUB = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define PSP_CTRL_SELECT   0x0001u
#define PSP_CTRL_RIGHT    0x0020u
#define PSP_CTRL_UP       0x0010u
#define PSP_CTRL_TRIANGLE 0x1000u
#define PSP_CTRL_CIRCLE   0x2000u
#define PSP_CTRL_CROSS    0x4000u
#define NOT_RUN 0xDEADu
#define REWIND 1
#define FIRE_REWIND PSP_CTRL_TRIANGLE   /* the rewind level, from the pad */
typedef struct { unsigned Buttons; } SceCtrlData;
static SceCtrlData pd;
static unsigned g_pad, prev_pad, pad_new, ctl_fired;
static int g_rw_held;
static int home_pressed, menu_open, overlay_up;
static unsigned pads[64];          /* the pad, per game iteration (1-based) */
static unsigned core_seen[64];     /* g_pad as the core read it */
static unsigned new_seen[64];      /* pad_new, as turbo read it */
static unsigned fired_seen[64];    /* ctl_fired after the overlay */
static int rw_seen[64];            /* g_rw_held, as the rewind tick read it */
static int frame;
static unsigned menu_prev;
static unsigned ov_pad[64];        /* the overlay's own reads (1-based) */
static int ov_frames;

/* The shortcuts: the held pad (levels) and its edges, apart. */
static unsigned ctl_shortcuts(unsigned pad, unsigned edges)
{
   return pad | edges << 16;
}
/* The menu: closes on an X or O edge (Resume, a state load, Host/Join). */
static int ui_active(void) { return menu_open; }
static void ui_frame(unsigned pad)
{
   unsigned edges = pad & ~menu_prev;
   menu_prev = pad;
   if (edges & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE))
      menu_open = 0;
}
/* HOME on the menu's top page closes it, like Resume. */
static int ui_home_press(void) { menu_open = 0; return 1; }
/* The overlay: modal, its own pad reads, answered on an X edge. */
static void ui_wake_menu(void)
{
   unsigned prev = 0;
   for (;;)
   {
      unsigned now;
      assert(ov_frames + 1 < 64 || !"the overlay never answered");
      now = ov_pad[++ov_frames];
      if (now & ~prev & PSP_CTRL_CROSS)
         return;
      prev = now;
   }
}
/* The pad as it is now: as it was at the overlay's answer. */
static void sceCtrlPeekBufferPositive(SceCtrlData *d, int n)
{
   (void)n;
   d->Buttons = ov_pad[ov_frames];
}

@FUNCTIONS@

static void machine(void)
{
   g_pad = prev_pad = pad_new = ctl_fired = 0;
   g_rw_held = 0;
   home_pressed = menu_open = overlay_up = 0;
   memset(pads, 0, sizeof(pads));
   memset(ov_pad, 0, sizeof(ov_pad));
   memset(core_seen, 0xAA, sizeof(core_seen));
   memset(new_seen, 0xAA, sizeof(new_seen));
   memset(fired_seen, 0xAA, sizeof(fired_seen));
   memset(rw_seen, 0xAA, sizeof(rw_seen));
   frame = ov_frames = 0;
   menu_prev = 0xFFFFFFFFu;        /* ui_open swallows the opening chord */
   g_pad_swallow = 0;
}

static void menu_closed_by(unsigned button)
{
   machine();
   menu_open = 1;
   pads[1] = 0;
   pads[2] = button;                          /* Resume */
   pads[3] = button;                          /* still held */
   pads[4] = button | PSP_CTRL_UP;            /* + a new press */
   pads[5] = 0;                               /* released */
   pads[6] = button;                          /* a real press */
   assert(iteration() == 0 && iteration() == 0 && !menu_open);
   assert(iteration() == 1 && core_seen[3] == 0);   /* the answer: not B */
   assert(iteration() == 1 && core_seen[4] == PSP_CTRL_UP);
   assert(iteration() == 1 && core_seen[5] == 0);
   assert(iteration() == 1 && core_seen[6] == button);
}

int main(void)
{
   /* 1, 2. Resume with X, with O. */
   menu_closed_by(PSP_CTRL_CROSS);
   menu_closed_by(PSP_CTRL_CIRCLE);

   /* 3. HOME closes the menu with RIGHT and TRIANGLE (rewind) held. */
   machine();
   menu_open = 1;
   pads[1] = PSP_CTRL_RIGHT | PSP_CTRL_TRIANGLE;
   pads[2] = PSP_CTRL_RIGHT | PSP_CTRL_TRIANGLE | PSP_CTRL_UP;  /* + new */
   pads[4] = PSP_CTRL_RIGHT;
   home_pressed = 1;
   assert(iteration() == 1 && !menu_open && !home_pressed);
   assert(core_seen[1] == 0);                 /* read before the close */
   assert(new_seen[1] == 0);                  /* ...nor its edges (turbo) */
   assert(fired_seen[1] == 0);                /* ...nor its shortcuts */
   assert(rw_seen[1] == 0);                   /* ...nor the rewind level */
   assert(iteration() == 1 && core_seen[2] == PSP_CTRL_UP);
   assert(iteration() == 1 && core_seen[3] == 0);
   assert(iteration() == 1 && core_seen[4] == PSP_CTRL_RIGHT);

   /* 4. The pause screen (SELECT+TRIANGLE, the rewind button), Continue. */
   machine();
   pads[1] = PSP_CTRL_SELECT | PSP_CTRL_TRIANGLE;   /* read before it */
   overlay_up = 1;
   ov_pad[3] = PSP_CTRL_CROSS;                /* Continue */
   pads[2] = PSP_CTRL_CROSS | PSP_CTRL_UP;    /* still held, + new */
   pads[3] = 0;
   pads[4] = PSP_CTRL_CROSS;                  /* a real press */
   assert(iteration() == 1 && ov_frames == 3);
   assert(core_seen[1] == 0);                 /* the stale pad: not run */
   assert(new_seen[1] == 0);                  /* ...nor its edges (turbo) */
   assert(fired_seen[1] == 0);                /* ...nor its shortcuts */
   assert(rw_seen[1] == 0);                   /* ...nor the rewind level */
   assert(iteration() == 1 && core_seen[2] == PSP_CTRL_UP);  /* X: not B */
   assert(iteration() == 1 && core_seen[3] == 0);
   assert(iteration() == 1 && core_seen[4] == PSP_CTRL_CROSS);

   /* 5. Nothing answered: the game's pad, untouched. */
   machine();
   pads[1] = PSP_CTRL_CROSS;
   pads[2] = PSP_CTRL_CROSS | PSP_CTRL_UP;
   assert(iteration() == 1 && core_seen[1] == PSP_CTRL_CROSS);
   assert(iteration() == 1 && core_seen[2] == (PSP_CTRL_CROSS | PSP_CTRL_UP));

   /* 6. Host/Join with X: the action runs before the arm, and RIGHT is
    *    first pressed while it does.  Only the X is not the game's. */
   machine();
   menu_open = 1;
   pads[1] = 0;
   pads[2] = PSP_CTRL_CROSS;                  /* Host: up to 15 s */
   pads[3] = PSP_CTRL_CROSS | PSP_CTRL_RIGHT; /* pressed during the wait */
   assert(iteration() == 0 && iteration() == 0 && !menu_open);
   assert(iteration() == 1 && core_seen[3] == PSP_CTRL_RIGHT);

   /* 7. Resume with X; X still held (swallowed) as the menu opens again;
    *    O closes it.  The X is still not the game's. */
   machine();
   menu_open = 1;
   pads[1] = 0;
   pads[2] = PSP_CTRL_CROSS;                  /* Resume */
   pads[3] = PSP_CTRL_CROSS;                  /* swallowed */
   pads[4] = PSP_CTRL_CROSS;                  /* the menu again */
   pads[5] = PSP_CTRL_CROSS | PSP_CTRL_CIRCLE;   /* O: Resume */
   pads[6] = PSP_CTRL_CROSS | PSP_CTRL_CIRCLE;
   assert(iteration() == 0 && iteration() == 0 && !menu_open);
   assert(iteration() == 1 && core_seen[3] == 0);
   menu_open = 1;
   menu_prev = 0xFFFFFFFFu;                   /* ui_open */
   assert(iteration() == 0 && iteration() == 0 && !menu_open);
   assert(iteration() == 1 && core_seen[6] == 0);

   puts("menu Resume (X, O), HOME close, pause screen Continue: the answer "
        "is not the game's until released; play untouched");
   return 0;
}
'''


def run(functions, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-menupress-") as folder:
        cfile = Path(folder) / "mp.c"
        binary = Path(folder) / "mp"
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


def wiring(src):
    """main(): the swallow at the pad read, before the first use of g_pad;
    the menu branch arms as it closes; HOME and the overlay's Continue arm
    and clear the pad.  Returns a list of failures."""
    bad = []
    body = main_body(src)
    raw = body.find("      g_pad = pd.Buttons & ~CTL_STICK_ALL;")
    m = READ.search(body, max(raw, 0))
    swallow = m.start() if m else -1
    first_use = body.find("      pad_new = g_pad & ~prev_pad;", raw)
    if raw < 0 or swallow < 0 or first_use < 0 or swallow > first_use:
        bad.append("the pad read does not go through the swallow")
    if len(READ.findall(body)) != 1:
        bad.append("the swallow is not applied exactly once")
    if not menu_close(body):
        bad.append("the menu branch does not arm as it closes")
    elif "pad_swallow_arm(g_pad);" not in menu_close(body):
        bad.append("the menu branch does not arm with the pad it answered")
    for name, text, arm, names in (
            ("HOME close", home_close(body), "pad_swallow_arm(g_pad);",
             ("g_pad", "pad_new", "ctl_fired")),
            ("overlay Continue", overlay_continue(src),
             "sceCtrlPeekBufferPositive(&pd, 1);",
             ("g_pad", "pad_new", "ctl_fired", "g_rw_held"))):
        i = text.find(arm)
        if i < 0 or (name == "overlay Continue" and
                     "pad_swallow_arm(pd.Buttons);" not in text[i:]):
            bad.append("%s does not arm with what is held" % name)
        for var in names:
            m = re.search(r"\b%s = (?:\w+ = )*0;" % var, text)
            if not m or m.start() < i:
                bad.append("%s does not clear %s after the arm" % (name, var))
    if not re.search(r"^ *g_pad_swallow = 0;", inproc_reset(src), re.M):
        bad.append("the in-process return keeps the last game's swallow")
    return bad


good = production(MAIN)
print(run(good, True).stdout.strip())
assert wiring(MAIN) == [], wiring(MAIN)
print("wiring: swallow at the pad read, menu close, HOME and overlay arm, "
      "in-process return resets")


def in_site(start, end):
    """A mutation confined to one site: start..end, both found in order."""
    def site(pattern, repl):
        def mutate(src):
            i = src.find(start)
            j = src.find(end, i + len(start))
            assert 0 <= i < j, (start, end)
            return src[:i] + re.sub(pattern, repl, src[i:j], count=1) + src[j:]
        return mutate
    return site


at_home = in_site(HOME_HEAD, "\n      }\n")
at_continue = in_site(CONTINUE, "if (!g_running)")
at_close = in_site("         if (!ui_active())\n            pad_swallow_arm(",
                   "continue;")

MUTANTS = (
    # (label, old, new, expected text in stderr, wiring must see it too)
    ("Resume's X reaches the core",
     at_close(r"pad_swallow_arm\([^;]*\);", "(void)0;"), "", "core_seen[3] == 0",
     True),
    ("a released button is not freed",
     "   g_pad_swallow &= pad;", "   /* g_pad_swallow &= pad; */",
     "core_seen[6] == button", False),
    ("the swallow hides new presses too",
     "   return pad & ~g_pad_swallow;", "   return g_pad_swallow ? 0 : pad;",
     "core_seen[4] == PSP_CTRL_UP", False),
    ("the pad read skips the swallow",
     READ.search(MAIN).group(0), "", "core_seen[3] == 0", True),
    ("HOME's close leaves the pad live",
     at_home(r"pad_swallow_arm\(g_pad\);", "(void)0;"), "",
     "core_seen[2] == PSP_CTRL_UP", True),
    ("HOME's iteration runs on its stale pad",
     at_home(r"\bg_pad = ", ""), "", "core_seen[1] == 0", True),
    ("HOME's iteration fires turbo on its stale edges",
     at_home(r"\bpad_new = ", ""), "", "new_seen[1] == 0", True),
    ("HOME's iteration acts on its stale shortcuts",
     at_home(r"\bctl_fired = ", ""), "", "fired_seen[1] == 0", True),
    ("HOME arms with everything, held or not",
     at_home(r"pad_swallow_arm\(g_pad\)", "pad_swallow_arm(0xFFFFFFFFu)"),
     "", "core_seen[2] == PSP_CTRL_UP", True),
    ("the overlay's answer reaches the core",
     at_continue(r"pad_swallow_arm\(pd\.Buttons\);", "(void)0;"), "",
     "core_seen[2] == PSP_CTRL_UP", True),
    ("the overlay's iteration runs on its stale pad",
     at_continue(r"\bg_pad = ", ""), "", "core_seen[1] == 0", True),
    ("the overlay's iteration fires turbo on its stale edges",
     at_continue(r"\bpad_new = ", ""), "", "new_seen[1] == 0", True),
    ("the overlay's iteration keeps its stale shortcuts",
     at_continue(r"\bctl_fired = ", ""), "", "fired_seen[1] == 0", True),
    ("the overlay's iteration rewinds on its stale level",
     at_continue(r"\bg_rw_held = 0;", ""), "", "rw_seen[1] == 0", True),
    ("the overlay arms with everything, held or not",
     at_continue(r"pad_swallow_arm\(pd\.Buttons\)",
                 "pad_swallow_arm(0xFFFFFFFFu)"),
     "", "core_seen[2] == PSP_CTRL_UP", True),
    ("the menu arms with everything, held or not: Host's wait swallows",
     at_close(r"pad_swallow_arm\(g_pad\)", "pad_swallow_arm(0xFFFFFFFFu)"),
     "", "core_seen[3] == PSP_CTRL_RIGHT", True),
    ("an arm forgets what is still swallowed",
     "   g_pad_swallow |= held;", "   g_pad_swallow = held;",
     "core_seen[6] == 0", False),
)
for label, old, new, expect, wired in MUTANTS:
    if callable(old):
        bad_src = old(MAIN)
    elif hasattr(old, "sub"):
        bad_src = old.sub(new, MAIN, 1)
    else:
        bad_src = MAIN.replace(old, new, 1)
    assert bad_src != MAIN, label
    bad = run(production(bad_src), False)
    assert expect in bad.stderr, (label, bad.stderr)
    assert bool(wiring(bad_src)) == wired, (label, wiring(bad_src))
    print("mutant (%s): failed as it must" % label)

# Wiring only: the next game's first read is not a model this loop has.
bad_src = in_site('fe_evt("inproc_return', "goto session_top;")(
    r"\n *g_pad_swallow = 0;[^\n]*", "")(MAIN)
assert bad_src != MAIN
assert wiring(bad_src) == ["the in-process return keeps the last game's "
                           "swallow"], wiring(bad_src)
print("mutant (the in-process return keeps the swallow): wiring sees it")
