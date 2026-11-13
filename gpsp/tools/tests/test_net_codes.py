#!/usr/bin/env python3
"""The wireless bring-up's failure codes (PLAN HW-2 r3-B1), on the host.

net_bringup_io (fe/link.c) is extracted unchanged from main_psp.c (read_source)
and compiled with stubs against the real netdrv/transport_adhoc.h.  Every way
the ladder can fail must return the code that names it, and never -1
(ADHOC_ERR_WLAN_OFF) unless the switch really is off:

  ad hoc:  the transport's own code (WLAN off, connect timeout) passes through;
           netdrv's start failing is ADHOC_ERR_NP_START, the transport undone;
  online:  online_psp_up failing is ADHOC_ERR_ONLINE (it returns -1 itself on
           three paths); netdrv failing after it is ADHOC_ERR_NP_START, the
           online path taken down.
Then ui_net_action's toasts: each code has its own, and ADHOC_ERR_ONLINE does
not fall into "WLAN switch is OFF".  Mutants of the old `return -1` are caught.
"""
from pathlib import Path
import re
import subprocess
import tempfile

import extract

ROOT = Path(__file__).resolve().parents[2]
SOURCE = extract.read_source(str(ROOT / "psp/main_psp.c"))


def bringup(source):
    m = re.search(r"^static int net_bringup_io\(int is_host.*?^   return 0;\n\}\n", source,
                  re.S | re.M)
    assert m, "net_bringup_io not found"
    return m.group(0)


def net_action_toasts(source):
    m = re.search(r"^   if \(rc == ADHOC_ERR_WLAN_OFF\)\n      osd_toast\(\"WLAN switch is OFF\"\);\n"
                  r".*?\n\}\n", source, re.S | re.M)
    assert m, "ui_net_action's toasts not found"
    return m.group(0)


STUB = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "transport_adhoc.h"

#define FE_CONSOLE_GBA 0
#define PACE_MODE_FIXED 0
#define PACE_MODE_ADAPTIVE 1
#define PACE_MODE_BACKPRESSURE 3
#define PACE_NOMINAL_X100 5973
#define PACE_FLOOR_X100 5000
#define PACE_FIXED_RAMP_X100_PER_S 100
#define FE_EVT_ONLY(x) ((void)(x))
static struct { int console, net_tx_thread, net_session_fps_x100,
                net_pace_match, net_session_fps_snap; } g_pcfg;
static int g_gba_cable, g_gblink_role;
static const char *g_save_path_for_backup;
static int online, online_rc, np_rc, init_rc;
static int online_downs, terms;
static void backup_save(const char *p) { (void)p; }
void adhoc_transport_set_tx_thread(int on) { (void)on; }
int adhoc_transport_tx_thread_active(void) { return 0; }
static int online_psp_enabled(void) { return online; }
static int online_psp_up(int h, const char *b, unsigned t)
{ (void)h; (void)b; (void)t; return online_rc; }
static void online_psp_down(void) { online_downs++; }
static const char *online_psp_why(void) { return "no AP"; }
static const char *online_build_id(void) { return "test"; }
static void fe_evt(const char *f, ...) { (void)f; }
static int net_start_np(int h, const char *g, const char *n, int p)
{ (void)h; (void)g; (void)n; (void)p; return np_rc; }
static int sceKernelTotalFreeMemSize(void) { return 0; }
static int sceKernelMaxFreeMemSize(void) { return 0; }
int adhoc_transport_init(const char *g, uint32_t c) { (void)g; (void)c; return init_rc; }
int adhoc_transport_init_join(const char *g, uint32_t c, uint32_t ms)
{ (void)g; (void)c; (void)ms; return init_rc; }
void adhoc_transport_join_info(int *t, uint32_t *ms, int *ch, uint8_t b[6])
{ *t = 0; *ms = 0; *ch = 0; memset(b, 0, 6); }
void adhoc_transport_term(void) { terms++; }
static void net_error_evt(const char *w, int rc) { (void)w; (void)rc; }
static void log_adhoc_up(const char *g) { (void)g; }
static void skip_policy_session_begin(void) {}
static unsigned pace_snap_x100(unsigned r, unsigned *vb) { *vb = 1; return r; }
static void pace_session_begin(void) {}

@BRINGUP@

static char toast[96];
#define osd_toast(...) snprintf(toast, sizeof(toast), __VA_ARGS__)
#define GBADHOC_ONLINE 1
static const char s_online_fmt[] = "Online: %s";
static char g_np_start_why[] = "no memory";
const char *adhoc_transport_stage(void) { return "net_init"; }
static void toasts(int rc)
{
   const char *room = "Room 7";
   toast[0] = '\0';
@TOASTS@

static int case_(int on, int orc, int nrc, int irc, int join)
{
   online = on; online_rc = orc; np_rc = nrc; init_rc = irc;
   online_downs = terms = 0;
   return net_bringup_io(!join, "GBLNK7", "nick", 0);
}

int main(void)
{
   /* ad hoc */
   assert(case_(0, 0, 0, ADHOC_OK, 0) == 0);
   assert(case_(0, 0, 0, ADHOC_ERR_WLAN_OFF, 0) == ADHOC_ERR_WLAN_OFF);
   assert(case_(0, 0, 0, ADHOC_ERR_CONNECT_TIMEOUT, 1) == ADHOC_ERR_CONNECT_TIMEOUT);
   assert(case_(0, 0, ADHOC_ERR_NP_START, ADHOC_OK, 0) == ADHOC_ERR_NP_START &&
          terms == 1);
   /* online */
   assert(case_(1, 0, 0, 0, 0) == 0);
   assert(case_(1, -1, 0, 0, 0) == ADHOC_ERR_ONLINE && online_downs == 0);
   assert(case_(1, 0, ADHOC_ERR_NP_START, 0, 1) == ADHOC_ERR_NP_START &&
          online_downs == 1);
   /* the player reads what failed */
   toasts(ADHOC_ERR_WLAN_OFF);
   assert(!strcmp(toast, "WLAN switch is OFF"));
   toasts(ADHOC_ERR_ONLINE);
   assert(!strcmp(toast, "Online: no AP"));
   toasts(ADHOC_ERR_NP_START);
   assert(!strcmp(toast, "Radio OK, netdrv failed: no memory"));
   toasts(ADHOC_ERR_NO_GROUP);
   assert(strstr(toast, "not found") != NULL);
   puts("net codes: ad hoc, online, netdrv and the toasts keep their codes");
   return 0;
}
'''


def run(parts, expect_pass):
    text = STUB
    for k, v in parts.items():
        text = text.replace("@%s@" % k, v)
    with tempfile.TemporaryDirectory(prefix="gbadhoc-netcodes-") as folder:
        cfile = Path(folder) / "nc.c"
        binary = Path(folder) / "nc"
        cfile.write_text(text)
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Werror", "-Wno-unused-function", "-Wno-unused-const-variable",
                        "-I" + str(ROOT / "netdrv"), str(cfile), "-o", str(binary)],
                       check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, result.stdout, result.stderr))
        return result


good = {"BRINGUP": bringup(SOURCE), "TOASTS": net_action_toasts(SOURCE)}
print(run(good, True).stdout.strip())
MUTANTS = (
    ("an online failure returns -1", "BRINGUP",
     "return ADHOC_ERR_ONLINE;", "return -1;"),
    ("netdrv's failure returns -1 (ad hoc)", "BRINGUP",
     "      adhoc_transport_term();\n      return rc;", "      adhoc_transport_term();\n      return -1;"),
    ("netdrv's failure returns -1 (online)", "BRINGUP",
     "         return rc;                 /* ADHOC_ERR_NP_START */",
     "         return -1;"),
    ("ADHOC_ERR_ONLINE has no toast of its own", "TOASTS",
     "   else if (rc == ADHOC_ERR_ONLINE)\n      osd_toast(s_online_fmt, online_psp_why());\n", ""),
)
for what, part, old, new in MUTANTS:
    parts = dict(good)
    assert parts[part].count(old) == 1, "mutant site gone: " + what
    parts[part] = parts[part].replace(old, new)
    run(parts, False)
    print("  mutant caught: " + what)


# ---- F4: the cable-restart early return takes an online bring-up down ------
# ui_net_action's `g_link_kind == LK_CABLE && !g_gba_cable_ok` block, extracted
# unchanged.  An Online session (ol) has the module, the AP and the room up by
# then: all three go, under the power hold every radio teardown outside the
# loop's park point runs under.  An ad hoc one (ol == 0) touches nothing.
def cable_restart(source):
    m = re.search(r"^   if \(g_link_kind == LK_CABLE && !g_gba_cable_ok\)\n   \{\n.*?^   \}\n", source,
                  re.S | re.M)
    assert m, "ui_net_action's cable-restart return not found"
    return m.group(0)


F4_STUB = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GBADHOC_ONLINE 1
enum { LK_NONE, LK_CABLE };
static int g_link_kind, g_gba_cable_ok, hold;
static char trace[96], toast[96];
static void note(const char *s) { strcat(trace, s); strcat(trace, " "); }
static void pwr_hold(const char *why) { assert(!strcmp(why, "net")); hold++; note("hold"); }
static void pwr_release(void) { hold--; note("release"); }
static void online_psp_down(void) { note(hold > 0 ? "down" : "DOWN-UNHELD"); }
static void online_psp_clear(void) { note("clear"); }
#define osd_toast(...) snprintf(toast, sizeof(toast), __VA_ARGS__)
static void action(int ol)
{
@F4@
   note("linking");
}
int main(void)
{
   g_link_kind = LK_CABLE; g_gba_cable_ok = 0; trace[0] = '\0';
   action(1);
   assert(!strcmp(trace, "hold down clear release ") && hold == 0);
   assert(g_link_kind == LK_NONE && strstr(toast, "restart") != NULL);
   g_link_kind = LK_CABLE; trace[0] = '\0';
   action(0);
   assert(!strcmp(trace, ""));
   g_link_kind = LK_CABLE; g_gba_cable_ok = 1; trace[0] = '\0';
   action(1);
   assert(!strcmp(trace, "linking "));
   puts("net codes: the cable-restart return takes the online path down, held");
   return 0;
}
'''


def run_f4(block, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-netcodes-") as folder:
        cfile = Path(folder) / "f4.c"
        binary = Path(folder) / "f4"
        cfile.write_text(F4_STUB.replace("@F4@", block))
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Werror", "-Wno-unused-function",
                        str(cfile), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, result.stdout, result.stderr))
        return result


f4 = cable_restart(SOURCE)
print(run_f4(f4, True).stdout.strip())
F4_MUTANTS = (
    ("the cable-restart return leaves the online path up",
     "         online_psp_down();\n         online_psp_clear();\n", ""),
    ("the online path is taken down without the power hold",
     "         pwr_hold(\"net\");\n", ""),
    ("the hold is never released",
     "         pwr_release();\n", ""),
)
for what, old, new in F4_MUTANTS:
    assert f4.count(old) == 1, "mutant site gone: " + what
    run_f4(f4.replace(old, new), False)
    print("  mutant caught: " + what)
print("PASS: test_net_codes (%d mutants)" % (len(MUTANTS) + len(F4_MUTANTS)))
