#!/usr/bin/env python3
"""make_rc_hw_arms.py -- the 3.5.0 RC's "wireless, then Quit to game list"
arms for the hardware PAIR rig (PSP-3000 = host, PSP-1000 = join; one rig app
per console, PSP/GAME/GBADHOC-GBLINK, tools/rig/hw_loop.py's USB handoff).

    make_rc_hw_arms.py --host-build DIR --join-build DIR [--old-build DIR]
        --fx DIR --bios FILE --out RIG

  --host-build  GBA_LINK=1 harness64 (EBOOT.PBP, gbadhoc_me.prx, gbadhoc_home.prx)
  --join-build  GBA_LINK=1 harness (the PSP-1000's 32 MiB layout)
  --old-build   the 3.1.0 beta HARNESS (candidate-3.1-all 77aaabb, the 32 MiB
                layout): the adapter room's older partner (arm R).  It runs
                on the JOIN console, the PSP-1000 -- never stage a harness64
                (MEMSIZE=1) build there: the 1000 did not come back from one
                (2026-10-04, RC suite run 10)
  --fx          aw2.gba, aw2.st0, aw2.sav, msc.gba, msc.st0, em.gba, em.st0,
                em-host.sav, em-join.sav (~/gbamulti/{roms,fx} in WSL)

Every arm is driven by the PLAYER'S menus (harness ui_script, the same .ui
scripts the PPSSPP rig runs: tools/gbamirror/linkmenu + the RC's quit
scripts) and ends with the in-game menu's Quit to game list.  After any
radio use the quit must REFUSE the in-process return (EVT inproc_refused
why=wireless), flush, tear down and hand off; the console never reaches the
relaunch on the rig (handoff_run comes first), so the relaunched browser is
the owner checklist's item, not this rig's.  What each arm proves: the quit
after that wireless state is clean (no hang, no crash, .sav flushed, exit
line written) and the NEXT arm -- a fresh process -- links again.

Arms (order suggestion: N Q L S H W R N):
  N  cable session ended normally (AW2 battle, late mode); NO quit: link
     exit after the end (link_exit_after = 240).  The control, and the
     "second link session works" check when run after the others.
  Q  N, then both consoles quit to the game list from the menu.
  L  link lost: the join exits mid-battle (autoexit 2500 frames); the host
     sees "Link lost", carries on, quits.
  S  scan only: host console opens Wireless > Join (scan), backs out, quits;
     the join console idles (exits at frame 900).
  H  hosting that never got a partner: host hosts, quits while hosting; the
     join console idles.
  W  wrong game: host hosts AW2, join runs Super Circuit, sees the room on
     the scan list as another game, backs out, quits; host quits after.
  R  Wireless Adapter (RFU) room: host (RC) hosts Emerald's adapter room
     from the menu, keeps it open ~100 s (the consoles' relaunch skew can
     be a 90 s handoff window: at ~16 s the 1000 arrived to an empty air in
     the first run), then quits; the join is the 3.1.0 beta (--old-build,
     the 32 MiB harness) joining by harness join=1, or the RC if none given.
"""
import argparse
import hashlib
import os
import shutil
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

HANDOFF = """handoff = 1
handoff_window_s = 90
handoff_max_runs = 100000
handoff_total_s = 900
handoff_park_s = 0
stall_watch_s = 20
heartbeat_s = 5
me_mode = 1
log_input = 0
"""

CONFIG = "theme = 0\r\nui_shell = 0\r\ngroup = GPSP07\r\nconsole = 0\r\nmenu_button = home\r\n"

# Quit to game list: UP from Resume wraps to Exit, then Quit to game list
QUIT = """+120 menu
+10 press UP
+4 press UP
+4 press CROSS
"""

AW2 = "Advance Wars 2 - Black Hole Rising.gba"
MSC = "Mario Kart - Super Circuit (USA).gba"
EM = "Pokemon - Emerald Version.gba"


def ui(name):
    with open(os.path.join(HERE, name), encoding="utf-8") as fh:
        return fh.read()


def no_exit(text):
    return "".join(l for l in text.splitlines(True) if "exit 0" not in l)


def wr(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as fh:
        fh.write(text)


def cp(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def harness(rom, role_keys, extra=""):
    return ("rom = %s\nload_state = 1\n" % rom) + role_keys + extra + HANDOFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host-build", required=True)
    ap.add_argument("--join-build", required=True)
    ap.add_argument("--old-build")
    ap.add_argument("--fx", required=True)
    ap.add_argument("--bios", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    menu = ("gba_link_menu = 1\nlink_script_waits = 1\nui_script = ui_script.txt\n"
            "gba_dual_late = 1\n")
    mg = os.path.join(HERE, "..", "multigame", "scripts")
    aw2_host = no_exit(ui("aw2_host.ui"))
    aw2_join = no_exit(ui("aw2_join.ui"))
    # the join waits out hw_loop's launch skew (up to ~20 s) before scanning
    aw2_join_late = aw2_join.replace("+120 menu", "+1500 menu", 1)
    lost_host = "".join(l for l in aw2_host.splitlines(True)
                        if "wait ended" not in l and "toast-ended" not in l)
    arms = {
        "N": dict(rom=AW2, st="aw2", script=("aw2_host.txt", "aw2_join.txt"),
                  host=(menu + "link_exit_after = 240\ngblink_end_after = 120\n", aw2_host),
                  join=(menu + "link_exit_after = 240\n", aw2_join_late)),
        # link_exit_after: an autopilot run otherwise exits the moment the
        # session ends and its script is done, before the menu quit lands
        "Q": dict(rom=AW2, st="aw2", script=("aw2_host.txt", "aw2_join.txt"),
                  host=(menu + "gblink_end_after = 120\nlink_exit_after = 3000\n"
                        "autoexit_frames = 30000\n", aw2_host + QUIT),
                  join=(menu + "link_exit_after = 3000\nautoexit_frames = 30000\n",
                        aw2_join_late + QUIT)),
        "L": dict(rom=AW2, st="aw2", script=("aw2_host.txt", "aw2_join.txt"),
                  host=(menu + "link_exit_after = 3000\nautoexit_frames = 30000\n",
                        lost_host + "+1 wait ended\n+30 dump lost-toast\n" + QUIT),
                  join=(menu + "autoexit_frames = 4000\n", aw2_join_late)),
        "S": dict(rom=AW2, st="aw2", script=None,
                  host=(menu + "autoexit_frames = 6000\n",
                        "+120 menu\n+10 press DOWN\n+4 press DOWN\n+4 press CROSS\n"
                        "+10 press DOWN\n+4 press DOWN\n+4 press CROSS\n+120 dump scan-list\n"
                        "+4 press CIRCLE\n+6 press CIRCLE\n+6 press CIRCLE\n" + QUIT),
                  join=("autoexit_frames = 900\n", None)),
        "H": dict(rom=AW2, st="aw2", script=None,
                  host=(menu + "autoexit_frames = 6000\n",
                        no_exit(ui("host_wait.ui")).replace("+40 dump x-host-up",
                                                            "+600 dump x-host-up") + QUIT),
                  join=("autoexit_frames = 900\n", None)),
        "W": dict(rom=AW2, st="aw2", script=None, join_rom=MSC, join_st="msc",
                  host=(menu + "autoexit_frames = 6000\n",
                        no_exit(ui("host_wait.ui")).replace("+40 dump x-host-up",
                                                            "+2700 dump x-host-up") + QUIT),
                  join=(menu + "autoexit_frames = 6000\n",
                        no_exit(ui("wrong_join.ui")).replace("+120 menu", "+1500 menu", 1)
                        + QUIT)),
        "R": dict(rom=EM, st="em", script=None,
                  host=(menu + "autoexit_frames = 6000\n",
                        no_exit(ui("adapter_host.ui")).replace("+300 menu", "+6000 menu")
                        + QUIT),
                  join=("join = 1\ngroup = GPSP07\nautoexit_frames = 3000\n", None),
                  join_build=a.old_build),
    }
    manifest = []
    for name, d in arms.items():
        st = os.path.join(a.out, "stage-" + name)
        for role, build in (("host", a.host_build),
                            ("join", d.get("join_build") or a.join_build)):
            for f in ("EBOOT.PBP", "gbadhoc_me.prx", "gbadhoc_home.prx"):
                src = os.path.join(build, f)
                if os.path.exists(src):
                    cp(src, os.path.join(st, "%s-%s" % (role, f)))
            keys, script = d[role]
            rom = d.get("join_rom", d["rom"]) if role == "join" else d["rom"]
            stem = d.get("join_st", d["st"]) if role == "join" else d["st"]
            extra = "nick = %s\n" % role
            if d["script"]:
                extra += "script = link.txt\n"
                cp(os.path.join(mg, d["script"][0 if role == "host" else 1]),
                   os.path.join(st, "%s-link.txt" % role))
            wr(os.path.join(st, "%s-.gpsp-harness.ini" % role),
               "# RC 3.5.0 wireless-then-quit arm %s (%s)\n" % (name, role) +
               harness(rom, keys, extra))
            if script:
                wr(os.path.join(st, "%s-ui_script.txt" % role), script)
            wr(os.path.join(st, "%s-CONFIG.INI" % role), CONFIG)
            base = os.path.splitext(rom)[0]
            # hw_loop reads the role prefix from the BASENAME (roms/host-x)
            cp(os.path.join(a.fx, stem + ".gba"),
               os.path.join(st, "roms", "%s-%s" % (role, rom)))
            cp(os.path.join(a.fx, stem + ".st0"),
               os.path.join(st, "roms", "%s-%s.st0" % (role, base)))
            for sav in ("%s-%s.sav" % (stem, role), stem + ".sav"):
                if os.path.exists(os.path.join(a.fx, sav)):
                    cp(os.path.join(a.fx, sav),
                       os.path.join(st, "roms", "%s-%s.sav" % (role, base)))
                    break
        cp(a.bios, os.path.join(st, "gba_bios.bin"))
    for root, _dirs, files in os.walk(a.out):
        for f in sorted(files):
            p = os.path.join(root, f)
            with open(p, "rb") as fh:
                data = fh.read()
            rel = os.path.relpath(p, a.out)
            line = "%s  %s" % (hashlib.md5(data).hexdigest(), rel)
            if f.endswith("EBOOT.PBP"):
                line += "  crc32 %08x" % (zlib.crc32(data) & 0xFFFFFFFF)
            manifest.append(line)
    wr(os.path.join(a.out, "MANIFEST.txt"), "\n".join(sorted(manifest)) + "\n")
    print("arms:", ", ".join(sorted(arms)))


if __name__ == "__main__":
    main()
