#!/usr/bin/env python3
"""mirhand.py -- the mirrig bench (tools/gbamirror/mirrig.py) taken over by a
console that is ALREADY PARKED in another rig's handoff loop, with no owner
launch: freezerig's window-aware backup / install / run loop (freezerig.py,
from a rig branch that is not in this repo, imported
unchanged, like hnsrig.py does), mirrig's arms and ini text.

    python mirhand.py stage   --mirstage MIRRIG_STAGE --out STAGE [--plan p3000]
    python mirhand.py backup  --drive F: --out BACKUP              (freezerig's)
    python mirhand.py install --stage STAGE --drive F: --backup BACKUP --window-s 300
    python mirhand.py run     --stage STAGE --drive F: --logs LOGS --window-s 300
    python mirhand.py report  --logs LOGS --out MIRRIG_LAYOUT      (then mir_report.py --rig)

The parked app relaunches the EBOOT in ITS OWN folder (default GBADHOC-
DRPROF), so `install` replaces that folder's contents (all backed up) and keeps
handoff/RESULT.TXT, WINDOW.TXT, RUNS.TXT; every write happens inside a handoff
window this script saw open; CMD.TXT=RUN is written only after the card
re-reads correct; the plan ends with CMD.TXT=STOP (the console parks again in
the same folder).  The handoff protocol of the mirror EBOOT is the same code
(psp/usb_handoff.c, 1e855d2) as the parked rig's.
"""
import argparse
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mirrig  # noqa: E402

FRZ = os.environ.get("FREEZERIG_DIR") or os.path.normpath(os.path.join(
    HERE, "..", "..", "..", "freeze-rig-worktree", "tools", "rig", "freezerig"))
sys.path.insert(0, FRZ)
import freezerig as fr  # noqa: E402

HANG_S = 1500          # longest arm (M-BATTLE) ~5 min on a PSP; boot slack
PLANS = dict(mirrig.PLANS)
# the 3000 today: the battle first (the headline), then the DUAL/SOLO/10K
# cost trio and the own-ROM arm, ABBA
PLANS["p3000"] = ["M-BATTLE", "M-DUAL", "M-SOLO", "M-DUAL10K", "M-OWN"]
PLANS["dry"] = ["M-DUAL", "M-SOLO"]      # mirhand_dry.sh
# the 3000, second session: the lookahead sweep
PLANS["p3000b"] = ["M-BATTLE140K", "M-DUAL140K", "M-DUAL50K", "M-BATTLE50K"]
# ...superseded after run 4: cost without the per-frame two-machine hash
PLANS["p3000c"] = ["M-BATTLE140K", "M-DUAL-NH", "M-DUAL140K-NH", "M-DUAL50K-NH",
                   "M-BATTLE140K-NH"]
# the hang (docs/GBA-LINK-MIRROR.md section 13): the arm that hung, against
# the same arm with every cache sync whole (both cores), ABBA (--rounds 4)
PLANS["hang"] = ["M-BATTLE140K-NH", "M-BATTLE140K-NH-PAR"]


def render_ini(man, arm, run_id, window_s=None, dry_cycles=None):
    text = mirrig.render_ini(arm, run_id)
    if window_s:
        text = text.replace("handoff_window_s = 90",
                            "handoff_window_s = %d" % window_s)
    return text


fr.render_ini = render_ini
fr.freeze_deadline_s = lambda man, arm, window_s: window_s + HANG_S

# a folder another rig installed can lack our fixtures: copy any missing or
# wrong base file before the arm, in the same window (hnsrig.py's guard)
_stage_arm = fr.stage_arm


def _stage_arm_with_base(stage, man, root, arm, run_id, window_s=None, dry_cycles=None):
    for rel, want in man["files"].items():
        dst = os.path.join(root, rel)
        if os.path.isfile(dst) and fr.md5(dst) == want["md5"]:
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not fr.hw_loop.copy_retry(os.path.join(stage, "base", rel), dst, why="base"):
            return None
        if fr.md5(dst) != want["md5"]:
            fr.log("  VERIFY FAILED: base file %s" % rel)
            return None
        fr.log("  installed missing base file %s" % rel)
    return _stage_arm(stage, man, root, arm, run_id, window_s, dry_cycles)


fr.stage_arm = _stage_arm_with_base


def cmd_stage(a):
    mm = mirrig.load_manifest(a.mirstage)
    if not mirrig.verify_stage(a.mirstage, mm):
        return 2
    if os.path.exists(a.out) and os.listdir(a.out):
        print("REFUSING: %s is not empty" % a.out)
        return 2
    names = PLANS[a.plan] if a.plan else mm["arms"]
    order = []
    for r in range(a.rounds):
        order.extend(names if r % 2 == 0 else names[::-1])
    src = os.path.join(a.mirstage, "base")
    base = os.path.join(a.out, "base")
    armd = os.path.join(a.out, "arms", "link")
    os.makedirs(armd)
    for rel in mm["files"]:
        if rel in ("EBOOT.PBP", "gbadhoc_me.prx"):
            shutil.copy2(os.path.join(src, rel), os.path.join(armd, rel))
            continue
        os.makedirs(os.path.dirname(os.path.join(base, rel)) or base, exist_ok=True)
        shutil.copy2(os.path.join(src, rel), os.path.join(base, rel))
    builds = {"link": {n: {"md5": fr.md5(os.path.join(armd, n)),
                           "crc32": fr.crc32(os.path.join(armd, n))}
                       for n in ("EBOOT.PBP", "gbadhoc_me.prx")}}
    arms = {arm: {"build": "link", "cycles": 0,
                  "what": "gba link mirror %s %s" % (arm, mirrig.arm_extra(arm))}
            for arm in names}
    man = {"created": fr.now(), "rig": "mirhand", "mirrig_stage": os.path.abspath(a.mirstage),
           "arms": arms, "order": order, "base_ini": [], "script_frames": {},
           "builds": builds, "files": {}}
    for cur, _d, files in os.walk(base):
        for n in files:
            p = os.path.join(cur, n)
            rel = os.path.relpath(p, base).replace(os.sep, "/")
            man["files"][rel] = {"md5": fr.md5(p), "crc32": fr.crc32(p),
                                 "size": os.path.getsize(p)}
    with open(os.path.join(a.out, "MANIFEST.json"), "w") as fh:
        json.dump(man, fh, indent=1)
    print("staged %d base files + EBOOT crc32=%s; order %s"
          % (len(man["files"]), builds["link"]["EBOOT.PBP"]["crc32"], order))
    return 0


def cmd_report(a):
    """freezerig's runNNN-<arm>-<run_id> dirs -> mirrig's autoNNN-<arm>-solo,
    which mir_report.py --rig reads."""
    with open(os.path.join(a.logs, "RIGSTATE.json")) as fh:
        st = json.load(fh)
    os.makedirs(a.out, exist_ok=True)
    for r in st["runs"]:
        dst = os.path.join(a.out, "auto%03d-%s-solo" % (r["n"], r["arm"]))
        if not os.path.isdir(dst):
            shutil.copytree(os.path.join(a.logs, r["dir"]), dst)
        print("run %d %s %s" % (r["n"], r["arm"], r["verdict"]))
    print("%d runs -> %s" % (len(st["runs"]), a.out))
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] in ("install", "run", "backup"):
        return fr.main()
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("stage")
    s.add_argument("--mirstage", required=True, help="a mirrig.py stage")
    s.add_argument("--out", required=True)
    s.add_argument("--plan", default="p3000", choices=sorted(PLANS))
    s.add_argument("--rounds", type=int, default=2, help="ABBA rounds")
    r = sp.add_parser("report")
    r.add_argument("--logs", required=True)
    r.add_argument("--out", required=True)
    a = ap.parse_args()
    return {"stage": cmd_stage, "report": cmd_report}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
