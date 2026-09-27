#!/usr/bin/env python3
"""prepare_rig.py -- build the rig's STAGE directories and golden set on the PC.
Writes nothing to any memory stick (that is setup_rig_cards.py, after the
owner handshake).  docs/RIG-DOUBLE-BATTLE.md §4-§5.

    python tools/rig/prepare_rig.py --build <dir with EBOOT.PBP + gbadhoc_me.prx>
        --variant growl|swift|frfr --backup <wireless-stick-backup dir>
        --out <builds/wireless-rig>

Produces <out>/<variant>/stage-A, stage-B (identical except `arm` intent and
rfu_shed_keep), the H0 controls stage-L (V1: arm A plus net_latency_ms 50 on
both sides) and stage-S (V2: arm A with the JOIN slowed by --slow-us per
frame), <out>/<variant>/golden, and MANIFEST.txt with every md5 and
the EBOOT CRC32 the scorer checks (I3).  CONFIG.INI per role is the OWNER'S
own (from the backup), so the rig runs the owner's settings -- profile, ME
mode, scale -- not a lab config.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
FIX = os.path.join(REPO, "testdata", "fixtures")
# At most 8 entries (rig_fail_probe).  H0 run 2+: the two link players'
# positions (x | y << 16, gObjectEvents[1] / [2] currentCoords) replace the
# menu cursor and the save-dialog callback, which only matter before the
# group forms, where the marks already pin the step.  recvQueue is read as a
# u16: count and the `full` latch.
FAIL_PROBE = ("0x030030F4:4,0x02023E8A:1,0x03005AEE:2,0x0203ADE8:1,0x02036E6C:4,"
              "0x03004FE0:4,0x02036E90:4,0x0202004F:2")
VARIANTS = {
    # variant: (host rom stem, join rom stem, golden args, turns, host src, join src)
    "growl": ("firered", "leafgreen", ["--move", "GROWL", "--scene-off", "--text-fast"], 20,
              "harness:host-firered.sav", "harness:join-leafgreen.sav"),
    "swift": ("firered", "leafgreen", ["--move", "SWIFT", "--ppups", "3", "--hp", "999"], 12,
              "harness:host-firered.sav", "harness:join-leafgreen.sav"),
    "frfr":  ("firered", "firered", ["--move", "GROWL", "--scene-off", "--text-fast"], 20,
              "harness:host-firered.sav", "backup:D-PSP-1000/GBADHOC/roms/Pokemon - FireRed Version (USA).sav"),
}


def md5(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()


def crc32(p):
    return "%08x" % (zlib.crc32(open(p, "rb").read()) & 0xFFFFFFFF)


# arm -> (rfu_shed_keep, {role: extra ini lines}).  L and S are H0's
# controls (docs/RIG-DOUBLE-BATTLE.md §4): each must MOVE F2M, or the rig
# cannot see what it is looking for.
#
# Q is B at a 57.00 session rate: the owner's CONFIG.INI forces 59.73
# (`net_session_fps_force = 1`, psp/main_psp.c: then `net_session_fps` from
# config.ini is the rate), and ADR-0073 measured 57.00 far kinder to a
# client that cannot hold 59.73.  CONFIG-only: one value, both consoles.
CFG_OVERRIDE = {"Q": {"net_session_fps": "57.00"}}


# H is B plus rfu_hold (hold, never discard).  B stages rfu_hold = 0 so that,
# on the same EBOOT, the ONLY difference between B and H is the hold.
def arms(slow_us):
    return {"A": (0, {}), "B": (2, {}), "Q": (2, {}),
            "H": (2, {"host": ["rfu_hold = 1"], "join": ["rfu_hold = 1"]}),
            "L": (0, {"host": ["net_latency_ms = 50"], "join": ["net_latency_ms = 50"]}),
            "S": (0, {"join": ["pace_slow_us = %d" % slow_us]})}


def override_cfg(src, dst, keys):
    """Copy the owner's CONFIG.INI byte for byte, replacing only the value of
    each key in `keys`.  Every key must appear EXACTLY once (the ini trap: a
    key that is absent or duplicated silently does something else)."""
    raw = open(src, "rb").read()
    lines = raw.split(b"\n")
    for k, v in keys.items():
        hits = [i for i, l in enumerate(lines) if l.split(b"=")[0].strip() == k.encode()]
        if len(hits) != 1:
            sys.exit("CONFIG override: %s appears %d times in %s" % (k, len(hits), src))
        cr = b"\r" if lines[hits[0]].endswith(b"\r") else b""
        lines[hits[0]] = ("%s = %s" % (k, v)).encode() + cr
    open(dst, "wb").write(b"\n".join(lines))


def harness_ini(role, rom, script, keep, extra=()):
    """An extra line REPLACES the base line for its key: the build reads the
    FIRST occurrence, and a duplicate is reported and voids the run (I4)."""
    keys = {l.split("=")[0].strip() for l in extra}
    base = [
        "# rig harness ini (prepare_rig.py) -- run_id and arm are appended by hw_loop",
        "script = %s" % script,
        "%s = 1" % role,
        "nick = %s" % role,
        "group = GPSP77",
        "rom = %s.gba" % rom,
        "rfu_shed_keep = %d" % keep,
        "rfu_hold = 0",
        "log_input = 0",
        "fail_probe = %s" % FAIL_PROBE,
        "watch_ram = 0x02036E6C:4,0x02036E90:4",
        "handoff = 1",
        "handoff_window_s = 90",
        "handoff_max_runs = 100000",
        "handoff_total_s = 900",
        "handoff_park_s = 0",
        "stall_watch_s = 20"]
    base = [l for l in base if l.startswith("#") or l.split("=")[0].strip() not in keys]
    return "\n".join(base + list(extra) + [""])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--variant", required=True, choices=sorted(VARIANTS))
    ap.add_argument("--backup", required=True)
    ap.add_argument("--harness-golden",
                    default=os.path.join(REPO, "..", "..", "harness-kit", "golden-saves-frlg"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--slow-us", type=int, default=2000,
                    help="stage-S join slowdown per frame (pace_slow_us)")
    a = ap.parse_args()

    hrom, jrom, gargs, turns, hsrc, jsrc = VARIANTS[a.variant]
    root = os.path.join(a.out, a.variant)
    if os.path.exists(root):
        shutil.rmtree(root)
    gold = os.path.join(root, "golden")
    os.makedirs(gold)

    def src(spec):
        kind, _, rel = spec.partition(":")
        return os.path.join(a.harness_golden if kind == "harness" else a.backup, rel)
    for role, stem, spec in (("host", hrom, hsrc), ("join", jrom, jsrc)):
        out = os.path.join(gold, "%s-%s.sav" % (role, stem))
        r = subprocess.run([sys.executable, os.path.join(HERE, "make_rig_golden.py"),
                            src(spec), out] + gargs, capture_output=True, text=True)
        if r.returncode:
            sys.exit("golden %s failed:\n%s%s" % (role, r.stdout, r.stderr))

    man = ["# prepare_rig.py variant=%s" % a.variant]
    for arm, (keep, extra) in sorted(arms(a.slow_us).items()):
        st = os.path.join(root, "stage-" + arm)
        os.makedirs(st)
        for f in ("EBOOT.PBP", "gbadhoc_me.prx"):
            shutil.copy2(os.path.join(a.build, f), os.path.join(st, f))
        for role, stem, drive in (("host", hrom, "F-PSP-3000"), ("join", jrom, "D-PSP-1000")):
            script = "frlg_battle_%s.inputs" % role
            body = open(os.path.join(FIX, script), encoding="utf-8").read()
            if turns != 30:
                body = body.replace("\nrepeat 30\n", "\nrepeat %d\n" % turns)
            open(os.path.join(st, script), "w", encoding="utf-8", newline="\n").write(body)
            open(os.path.join(st, "%s-.gpsp-harness.ini" % role), "w",
                 newline="\n").write(harness_ini(role, stem, script, keep,
                                                  extra.get(role, ())))
            cfg = os.path.join(a.backup, drive, "GBADHOC", "CONFIG.INI")
            if os.path.isfile(cfg):
                if arm in CFG_OVERRIDE:
                    override_cfg(cfg, os.path.join(st, "%s-CONFIG.INI" % role),
                                 CFG_OVERRIDE[arm])
                else:
                    shutil.copy2(cfg, os.path.join(st, "%s-CONFIG.INI" % role))
        for dp, _d, fs in os.walk(st):
            for f in sorted(fs):
                p = os.path.join(dp, f)
                man.append("%s  %s" % (md5(p), os.path.relpath(p, a.out)))
    for f in sorted(os.listdir(gold)):
        man.append("%s  %s" % (md5(os.path.join(gold, f)), os.path.relpath(os.path.join(gold, f), a.out)))
    man.append("eboot_crc32 %s" % crc32(os.path.join(a.build, "EBOOT.PBP")))
    man.append("turns %d" % turns)
    man.append("stage_S_join_pace_slow_us %d" % a.slow_us)
    open(os.path.join(root, "MANIFEST.txt"), "w").write("\n".join(man) + "\n")
    print("\n".join(man))
    return 0


if __name__ == "__main__":
    sys.exit(main())
