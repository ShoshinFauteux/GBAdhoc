#!/usr/bin/env python3
"""stage_slot.py -- stage the layout-pinning hardware bench as the hardware
queue's SLOT step (docs/LAYOUT-PINNING.md section 7).

    python tools/layout/stage_slot.py --console 3000 --builds builds/layout-pin-out/all

Reuses builds/hw-queue unchanged where it can: hwqueue.make_drstage writes the
drrig-format stage (base fixtures from the queue's own pinned sources,
relabelled EBOOTs, MANIFEST.json), then `rigshim.py hnsrig stage` turns it into
the hnsrig stage the queue runs at stages/slot-<console>/.  The arms are
builds/hw-queue/slot_arms/layout_pin.py (loaded by queue_arms.py), and so is
the order (ABBA).  Builds (harness64, each dir: EBOOT.PBP, gbadhoc_me.prx,
build-manifest.json):

    lpoff     ref-h64      claude/candidate-3.1-all fa89f4e (LAYOUT_PIN=0)
    lppin     h64-pin      LAYOUT_PIN=1
    lppinpad  h64-pinpad   LAYOUT_PIN=1 + LAYOUT_PAD_TEXT=2600 LAYOUT_PAD_BSS=1216
    lpoffpad  h64-offpad   LAYOUT_PIN=0 + the same pad

Never touches a console or a drive.
"""
import argparse
import importlib.util
import json
import os
import subprocess
import sys

KINDS = {"lpoff": "ref-h64", "lppin": "h64-pin", "lppinpad": "h64-pinpad",
         "lpoffpad": "h64-offpad"}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--console", default="3000")
    ap.add_argument("--builds", required=True)
    ap.add_argument("--hwqueue", default=None, help="builds/hw-queue")
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    hq = a.hwqueue or os.path.normpath(os.path.join(here, "..", "..", "..", "hw-queue"))
    sys.path.insert(0, hq)
    import hwqueue
    spec = importlib.util.spec_from_file_location(
        "lp_slot", os.path.join(hq, "slot_arms", "layout_pin.py"))
    arms = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(arms)
    src = {}
    for k, d in KINDS.items():
        d = os.path.join(a.builds, d)
        pin = json.load(open(os.path.join(d, "build-manifest.json")))["eboot"]["md5"]
        src[k] = (os.path.join(d, "EBOOT.PBP"), os.path.join(d, "gbadhoc_me.prx"), True, pin)
    hwqueue.kind_sources = lambda: src
    drrig, _h = hwqueue.drrig_mod()
    dr = os.path.join(hq, "stages", "_dr-slot-%s" % a.console)
    out = os.path.join(hq, "stages", "slot-%s" % a.console)
    for p in (dr, out):
        if os.path.exists(p):
            sys.exit("REFUSING: %s exists (remove it to restage)" % p)
    hwqueue.make_drstage(dr, arms.ORDER, drrig)
    rc = subprocess.call([sys.executable, os.path.join(hq, "rigshim.py"), "hnsrig",
                          "stage", "--drstage", dr, "--out", out])
    if rc == 0:
        m = json.load(open(os.path.join(out, "MANIFEST.json")))
        print("staged %s: %d runs, order %s" % (out, len(m["order"]), " ".join(m["order"])))
    return rc


if __name__ == "__main__":
    sys.exit(main())
