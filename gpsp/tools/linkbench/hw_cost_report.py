#!/usr/bin/env python3
"""hw_cost_report.py RUNDIR [RUNDIR...]

Per-scene core cost from a harness run (PSP hardware or PPSSPP): RUNDIR holds
shash.txt (v3: per-frame c= microseconds of retro_run) and frontend.log (the
autopilot `ap_mark` scene boundaries, psp_model, jit tier, me_mode).  Uses the
LAST occurrence of each mark (the measured tour after the `state` reload).
"""
import statistics as st, sys
from pathlib import Path

def pct(v, p):
    v = sorted(v); return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))] if v else 0

def report(d):
    d = Path(d)
    log = (d / "frontend.log").read_text(errors="replace").splitlines()
    info = {k: "" for k in ("psp_model", "jit_cache", "me_mode", "clock")}
    marks = {}
    for ln in log:
        for k in info:
            if ln.startswith(f"EVT {k}") and not info[k]:
                info[k] = ln[4:]
        if "ap_mark" in ln:
            name = ln.split("text=")[1].split()[0]
            marks[name] = int(ln.split(" f=")[1].split()[0])
    order = sorted(marks.items(), key=lambda x: x[1])
    cost = {}
    for ln in (d / "shash.txt").read_text().splitlines():
        if ln.startswith("f="):
            kv = dict(x.split("=", 1) for x in ln.split())
            if "c" in kv:
                cost[int(kv["f"])] = int(kv["c"])
    print(f"== {d}\n   " + " | ".join(v for v in info.values() if v))
    print(f"   {'scene':20s} {'frames':>6s} {'med ms':>7s} {'p95 ms':>7s} {'max ms':>7s}")
    allv = []
    for i, (name, f0) in enumerate(order):
        if name == "end": continue
        f1 = order[i + 1][1] if i + 1 < len(order) else max(cost) + 1
        v = [cost[f] for f in range(f0 + 1, f1 + 1) if f in cost]
        if not v: continue
        allv += v
        print(f"   {name:20s} {len(v):6d} {st.median(v)/1000:7.2f} {pct(v,95)/1000:7.2f} {max(v)/1000:7.2f}")
    if allv:
        print(f"   {'ALL':20s} {len(allv):6d} {st.median(allv)/1000:7.2f} {pct(allv,95)/1000:7.2f} "
              f"{max(allv)/1000:7.2f}   mean {st.mean(allv)/1000:.2f}")

for a in sys.argv[1:]:
    report(a)
