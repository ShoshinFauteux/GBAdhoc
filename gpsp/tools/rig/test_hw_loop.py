#!/usr/bin/env python3
"""Offline test of the rig's hw_loop fork against two FAKE consoles.

Each fake "card" is a temp directory with PSP/GAME/GBADHOC-RIG/{handoff,log};
a thread per console answers CMD.TXT the way usb_handoff.c does (run, write
frontend.log and RESULT.TXT).  Asserts, for an A/B campaign with --order ABBA:
  - arms alternate exactly as ordered, starting after the parked wake-up;
  - both staged .gpsp-harness.ini carry the SAME run_id and the right arm;
  - logs are named autoNNN-<arm>-<role>.log and pair by PC cycle;
  - every run has a meta.json with the card EBOOT CRC and RESULT.TXT fields;
  - a card whose ROLE.TXT disagrees is refused before anything is staged;
  - an --appdir other than GBADHOC-RIG is refused.
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "hw_loop.py")
APP = "GBADHOC-RIG"


def card(root, role):
    a = os.path.join(root, "PSP", "GAME", APP)
    os.makedirs(os.path.join(a, "handoff"), exist_ok=True)
    os.makedirs(os.path.join(a, "log"), exist_ok=True)
    open(os.path.join(a, "ROLE.TXT"), "w").write("%s PSP-test\n" % role)
    open(os.path.join(a, "handoff", "RESULT.TXT"), "w").write(
        "run=0\nexit=0\nreason=ok\nstatus=parked\n")
    return a


def fake_console(app, role, stop, seen):
    n = 0
    while not stop.is_set():
        cmd = os.path.join(app, "handoff", "CMD.TXT")
        if os.path.isfile(cmd):
            body = open(cmd).read()
            os.remove(cmd)
            if "RUN" not in body:
                # consumed, as the console does: record it for the asserts
                seen.append((role, "CMD", body.strip(), None))
                return
            n += 1
            ini = open(os.path.join(app, ".gpsp-harness.ini")).read()
            rid = re.search(r"run_id = (\S+)", ini)
            arm = re.search(r"arm = (\S+)", ini)
            seen.append((role, n, rid and rid.group(1), arm and arm.group(1)))
            mp = os.path.join(app, "MODEL.TXT")
            model = open(mp).read().strip() if os.path.isfile(mp) else "PSP-test"
            open(os.path.join(app, "log", "frontend.log"), "w").write(
                "EVT boot_ok\nEVT psp_model code=0 name=%s\nEVT rig run_id=%s arm=%s\n"
                "EVT exit code=0\n" % (model, rid and rid.group(1), arm and arm.group(1)))
            open(os.path.join(app, "handoff", "RESULT.TXT"), "w").write(
                "run=%d\nexit=0\nreason=ok\nstatus=ready\nframes=100\n"
                "evt_drop=0\n" % n)
        time.sleep(0.05)


def windowed_console(app, role, stop, W, phase, t0, rec):
    """Serves USB windows the way usb_handoff.c does: WINDOW.TXT gets a new
    token BEFORE each window, the window lasts W s whatever the PC does, and
    CMD.TXT is read only when it ends.  The first window is already open
    (of unknown age to the PC) when hw_loop starts.  Records, for every
    CMD.TXT the PC wrote, which window it landed in and how far into it."""
    n, run, k = 0, 0, -1
    while not stop.is_set():
        start = t0 + phase + k * W
        n += 1
        tok = "%d-%d" % (run, n)
        open(os.path.join(app, "handoff", "WINDOW.TXT"), "w").write(
            "token=%s\nseconds=%d\n" % (tok, W))
        rec.setdefault("windows", []).append((tok, start))
        cmd = os.path.join(app, "handoff", "CMD.TXT")
        seen = None
        while time.time() < start + W and not stop.is_set():
            if seen is None and os.path.isfile(cmd):
                seen = time.time() - start
                rec.setdefault("cmds", []).append((tok, seen))
            time.sleep(0.02)
        k += 1
        if os.path.isfile(cmd):
            body = open(cmd).read()
            os.remove(cmd)
            if "RUN" not in body:
                return
            run += 1
            ini = open(os.path.join(app, ".gpsp-harness.ini")).read()
            rid = re.search(r"run_id = (\S+)", ini)
            arm = re.search(r"arm = (\S+)", ini)
            open(os.path.join(app, "log", "frontend.log"), "w").write(
                "EVT boot_ok\nEVT psp_model code=0 name=PSP-test\n"
                "EVT rig run_id=%s arm=%s\nEVT exit code=0\n"
                % (rid and rid.group(1), arm and arm.group(1)))
            open(os.path.join(app, "handoff", "RESULT.TXT"), "w").write(
                "run=%d\nexit=0\nreason=ok\nstatus=ready\nframes=100\n"
                "evt_drop=0\n" % run)
            t0 = time.time() - phase - k * W     # the next window opens now


def window_case(td, arms, phase_join, W=4, m=1.5):
    h = os.path.join(td, "wH%s" % phase_join)
    j = os.path.join(td, "wJ%s" % phase_join)
    ah, aj = card(h, "host"), card(j, "join")
    stop = threading.Event()
    rec = {"host": {}, "join": {}}
    t0 = time.time()
    th = [threading.Thread(target=windowed_console,
                           args=(a, r, stop, W, ph, t0, rec[r]), daemon=True)
          for a, r, ph in ((ah, "host", 0.0), (aj, "join", phase_join))]
    for t_ in th:
        t_.start()
    time.sleep(0.3)
    r = subprocess.run([sys.executable, TOOL, "--host", h, "--join", j,
                        "--appdir", APP, "--arms", "A=%s,B=%s" % (arms["A"], arms["B"]),
                        "--order", "AB", "--runs", "2", "--logs",
                        os.path.join(td, "wlogs%s" % phase_join),
                        "--no-eject", "--poll", "0.05", "--timeout", "60",
                        "--window-min-left", str(m)],
                       capture_output=True, text=True, timeout=120)
    time.sleep(W + 0.5)
    stop.set()
    assert r.returncode == 0, r.stdout + r.stderr
    assert "unknown age" in r.stdout, r.stdout
    for role in ("host", "join"):
        first = rec[role]["windows"][0][0]
        cmds = rec[role].get("cmds", [])
        assert len(cmds) >= 3, (role, cmds, r.stdout)   # the wake + 2 runs
        for tok, into in cmds:
            assert tok != first, "%s: wrote into the window of unknown age" % role
            assert into <= W - m + 0.5, "%s: CMD %.2fs into a %ds window" % (
                role, into, W)
    names = os.listdir(os.path.join(td, "wlogs%s" % phase_join))
    assert not [n for n in names if n.startswith("auto") and "-x-" in n], names
    return r.stdout


def main():
    with tempfile.TemporaryDirectory() as td:
        h, j = os.path.join(td, "cardH"), os.path.join(td, "cardJ")
        ah, aj = card(h, "host"), card(j, "join")
        arms = {}
        for a in "AB":
            d = os.path.join(td, "arm" + a)
            os.makedirs(d)
            open(os.path.join(d, "EBOOT.PBP"), "wb").write(b"EBOOT-" + a.encode())
            for role in ("host", "join"):
                open(os.path.join(d, role + "-.gpsp-harness.ini"), "w").write(
                    "%s = 1\nrfu_shed_keep = %d\n" % (role, 0 if a == "A" else 2))
            arms[a] = d
        logs = os.path.join(td, "logs")
        stop = threading.Event()
        seen = []
        th = [threading.Thread(target=fake_console, args=(a, r, stop, seen), daemon=True)
              for a, r in ((ah, "host"), (aj, "join"))]
        for t in th:
            t.start()
        r = subprocess.run([sys.executable, TOOL, "--host", h, "--join", j,
                            "--appdir", APP, "--arms", "A=%s,B=%s" % (arms["A"], arms["B"]),
                            "--order", "ABBA", "--runs", "4", "--logs", logs,
                            "--no-eject", "--poll", "0.1", "--timeout", "60"],
                           capture_output=True, text=True, timeout=120)
        stop.set()
        assert r.returncode == 0, r.stdout + r.stderr
        runs = [(rid, arm) for role, n, rid, arm in seen if role == "host"]
        jruns = [(rid, arm) for role, n, rid, arm in seen if role == "join"]
        assert [a for _r, a in runs][:4] == list("ABBA"), (runs, r.stdout)
        assert runs[:4] == jruns[:4], "host/join run_id or arm differ"
        names = sorted(os.listdir(logs))
        for cyc, arm in ((1, "A"), (2, "B"), (3, "B"), (4, "A")):
            for role in ("host", "join"):
                assert "auto%03d-%s-%s.log" % (cyc, arm, role) in names, names
                m = json.load(open(os.path.join(logs, "auto%03d-%s-%s.meta.json"
                                                % (cyc, arm, role))))
                assert m["arm"] == arm and m["eboot_crc32"] and m["result"]["frames"] == "100"

        # the console's own psp_model disagreeing with ROLE.TXT: STOP, exit 2
        stop2 = threading.Event()
        open(os.path.join(aj, "MODEL.TXT"), "w").write("PSP-3000")
        for a in (ah, aj):
            c = os.path.join(a, "handoff", "CMD.TXT")
            if os.path.isfile(c):
                os.remove(c)          # phase 1's final STOP
            open(os.path.join(a, "handoff", "RESULT.TXT"), "w").write(
                "run=0\nexit=0\nreason=ok\nstatus=parked\n")
        seen2 = []
        th2 = [threading.Thread(target=fake_console, args=(a, r, stop2, seen2), daemon=True)
               for a, r in ((ah, "host"), (aj, "join"))]
        for t_ in th2:
            t_.start()
        r = subprocess.run([sys.executable, TOOL, "--host", h, "--join", j,
                            "--appdir", APP, "--runs", "3", "--logs", logs,
                            "--no-eject", "--poll", "0.1", "--timeout", "30"],
                           capture_output=True, text=True, timeout=120)
        stop2.set()
        assert r.returncode == 2 and "MODEL MISMATCH" in r.stdout, r.stdout
        # the join was told STOP (read off the card, or consumed by the fake
        # console already -- it deletes CMD.TXT exactly as usb_handoff.c does)
        for t_ in th2:
            t_.join(timeout=5)
        cmdp = os.path.join(aj, "handoff", "CMD.TXT")
        got = (open(cmdp).read().strip() if os.path.isfile(cmdp) else
               [c for r_, k, c, _ in seen2 if r_ == "join" and k == "CMD"])
        assert got in ("STOP", ["STOP"]), (got, r.stdout)

        # ROLE.TXT disagreeing with the role it is serviced as: refused
        open(os.path.join(aj, "ROLE.TXT"), "w").write("host PSP-test\n")
        r = subprocess.run([sys.executable, TOOL, "--host", h, "--join", j,
                            "--appdir", APP, "--runs", "1", "--logs", logs,
                            "--no-eject", "--timeout", "5"],
                           capture_output=True, text=True, timeout=60)
        assert r.returncode == 2 and "ROLE.TXT" in r.stdout, r.stdout
        # any other app dir: refused
        r = subprocess.run([sys.executable, TOOL, "--host", h, "--join", j,
                            "--appdir", "GBADHOC", "--runs", "1", "--logs", logs,
                            "--no-eject", "--timeout", "5"],
                           capture_output=True, text=True, timeout=60)
        assert r.returncode == 2 and "REFUSING" in r.stdout, r.stdout
        # USB windows (WINDOW.TXT): never into a window of unknown age,
        # never with less than --window-min-left of it to go -- and a
        # console whose window ran short while its partner's opened waits.
        window_case(td, arms, 3.0)
        out = window_case(td, arms, 1.2)
        assert "left (< " in out, out
    print("hw_loop rig fork: ABBA staging, run_id pairing, naming, meta, "
          "model/role/appdir guards, fresh-window staging all pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
