#!/usr/bin/env python3
"""hw_report.py LOGROOT [--label NAME]

Analyse one console's run of the dynarec-profile hardware bench
(tools/drprof/drrig.py; use --rig on its --logs dir).  Legacy layout: LOGROOT holds one subfolder per app, named like the app
folder (GBADHOC-DRPROF1 .. 6), each with that app's log/ contents
(shash.txt, frontend.log).

Prints, per fixture (AW2 tour: apps 1-3; H&S heavy battle: apps 4-6):

  1. core cost per scene (retro_run microseconds, shash c=): base vs
     prototypes (DISPATCH_CACHE + SMC_RETIRE_WINDOW) vs sampler -- median, p95, mean;
  2. the HARDWARE ORACLE: base vs proto must be hash-identical on every frame
     after the first scene mark, all fields including audio (same console,
     same audio path); base vs sampler likewise;
  3. the sampler's time split: EVT drprof windows (phase histogram of the
     samples taken while the emulation thread was running), per scene and
     overall, and the sampler's own cost (sampler mean c= vs base mean c=).

Works unchanged on PPSSPP logs (ppsspp_stage_smoke.sh) -- label those as
emulator numbers.
"""
import argparse
import statistics as st
import sys
from pathlib import Path

PH = ["jit", "upd", "disp", "flush", "cmisc", "-5", "xlat", "memc", "sound",
      "dma", "serial", "video", "irq", "retro", "-14", "outside"]
FIX = [("AW2 scene tour", ["GBADHOC-DRPROF1", "GBADHOC-DRPROF2", "GBADHOC-DRPROF3"]),
       ("H&S heavy-music battle", ["GBADHOC-DRPROF4", "GBADHOC-DRPROF5", "GBADHOC-DRPROF6"])]
ARMS = ["base", "proto", "sampler"]
FIELDS = ["a", "n", "r", "pc", "i", "e", "io", "p", "o", "v"]


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))] if v else 0


def load(d):
    d = Path(d)
    if not (d / "shash.txt").exists():
        return None
    log = (d / "frontend.log").read_text(errors="replace").splitlines()
    info, marks, windows = {}, [], []
    for ln in log:
        for k in ("psp_model", "jit_cache", "me_mode", "exit"):
            if ln.startswith("EVT " + k) and k not in info:
                info[k] = ln[4:]
        if "ap_mark" in ln:
            marks.append((int(ln.split(" f=")[1].split()[0]),
                          ln.split("text=")[1].split()[0]))
        if ln.startswith("EVT drprof f="):
            kv = dict(x.split("=", 1) for x in ln[4:].split()[1:])
            windows.append((int(kv["f"]), int(kv["n"]), int(kv["run"]),
                            [int(x) for x in kv["ph"].split(",")]))
    rows = {}
    for ln in (d / "shash.txt").read_text().splitlines():
        if ln.startswith("f="):
            kv = dict(x.split("=", 1) for x in ln.split())
            rows[int(kv["f"])] = kv
    return dict(info=info, marks=sorted(marks), windows=windows, rows=rows)


def scenes(run):
    """(name, first, last) frame ranges from the scene marks"""
    m = run["marks"]
    last = max(run["rows"]) if run["rows"] else 0
    out = []
    for i, (f0, name) in enumerate(m):
        f1 = m[i + 1][0] if i + 1 < len(m) else last
        out.append((name, f0 + 1, f1))
    return out


def cost(run, f0, f1):
    return [int(run["rows"][f]["c"]) for f in range(f0, f1 + 1)
            if f in run["rows"] and "c" in run["rows"][f]]


def oracle(a, b):
    if not a["marks"] or not b["marks"]:
        return "no scene marks"
    fa, fb = a["marks"][0][0] + 1, b["marks"][0][0] + 1
    n = 0
    while (fa + n) in a["rows"] and (fb + n) in b["rows"]:
        ra, rb = a["rows"][fa + n], b["rows"][fb + n]
        diff = [f for f in FIELDS if ra.get(f) != rb.get(f)]
        if diff:
            return "DIVERGED after %d identical frames at f=%d: %s" % (n, fa + n, ",".join(diff))
        n += 1
    return "IDENTICAL %d frames (all fields incl. audio)" % n


def split(windows, f0=None, f1=None):
    tot = [0] * 16
    n = run = 0
    for (f, nn, rr, ph) in windows:
        if f0 is not None and not (f0 <= f <= f1 + 299):
            continue
        n += nn
        run += rr
        for i in range(16):
            tot[i] += ph[i]
    return n, run, tot


def frozen_from(run, first):
    """first frame from which EWRAM never changes again (the game's logic is
    dead: a hung or derailed guest can still churn IWRAM/I/O in an interrupt
    loop, as the Go's H-BASE did at BIOS 0x186c), or None"""
    fr = sorted(f for f in run["rows"] if f >= first)
    if len(fr) < 120:
        return None
    last = run["rows"][fr[-1]]
    k = len(fr) - 1
    while k > 0 and run["rows"][fr[k - 1]].get("e") == last.get("e"):
        k -= 1
    return fr[k] if len(fr) - k >= 120 else None


def diverge(a, b, first):
    n = 0
    f = first
    while f in a["rows"] and f in b["rows"]:
        ra, rb = a["rows"][f], b["rows"][f]
        d = [x for x in FIELDS if ra.get(x) != rb.get(x)]
        if d:
            return "DIVERGED at f=%d (%s) after %d identical" % (f, ",".join(d), n)
        n += 1
        f += 1
    return "IDENTICAL %d frames" % n


def cycle_table(root, ref_arm):
    """One line per collected cycle: identity, cost, liveness, oracle.  The
    oracle starts at the first frame after the savestate load (f=31: the
    harness autoloads at frame 30), not at the first scene mark."""
    dirs = sorted(root.glob("auto*-solo"))
    runs = {d: load(d) for d in dirs}
    refs = {}
    for d in dirs:
        arm = d.name.split("-", 1)[1].rsplit("-solo", 1)[0]
        fx = arm.split("-")[0]
        want = ref_arm if fx == ref_arm.split("-")[0] else fx + "-BASE"
        if arm == want and fx not in refs and runs[d]:
            refs[fx] = (d.name, runs[d])
    print("\n## Per cycle (oracle from f=31 vs the first %s run of the fixture)"
          % "/".join(sorted(set(r[0].split("-", 1)[1].rsplit("-solo", 1)[0]
                                   for r in refs.values()))))
    print("  %-26s %8s %9s %9s  %-18s %s" % ("cycle", "frames", "mean ms",
                                            "p95 ms", "guest", "oracle"))
    for d in dirs:
        r = runs[d]
        if not r:
            print("  %-26s (no shash.txt)" % d.name)
            continue
        arm = d.name.split("-", 1)[1].rsplit("-solo", 1)[0]
        fx = arm.split("-")[0]
        c = [int(x["c"]) for f, x in r["rows"].items() if f >= 31 and "c" in x]
        fz = frozen_from(r, 31)
        ref = refs.get(fx)
        orc = "-" if not ref else ("(reference)" if ref[0] == d.name
                                   else diverge(r, ref[1], 31))
        coh = [l for l in (d / "frontend.log").read_text(errors="replace").splitlines()
               if l.startswith("EVT jit_coh")]
        extra = ""
        if coh:
            bad = [l for l in coh if " n=0 " not in l]
            extra = "  jit_coh lines=%d nonzero=%d%s" % (len(coh), len(bad),
                                                       (" first: " + bad[0][:90]) if bad else "")
        print("  %-26s %8d %9.2f %9.2f  %-18s %s%s" % (
            d.name, len(c), st.mean(c) / 1000 if c else 0, pct(c, 95) / 1000,
            ("EWRAM DEAD from f=%d" % fz) if fz else "progressing", orc, extra))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="per-app log folders (old six-app layout), "
                    "or with --rig the drrig.py --logs directory")
    ap.add_argument("--rig", action="store_true",
                    help="root is a drrig.py logs dir (autoNNN-<arm>-solo)")
    ap.add_argument("--label", default="")
    ap.add_argument("--ref", default="H-SAMP",
                    help="--rig: arm whose FIRST run is the per-cycle oracle "
                         "reference for its fixture (default H-SAMP, which matched "
                         "PPSSPP on the Go 2026-09-30); AW2 uses A-BASE")
    o = ap.parse_args()
    root = Path(o.root)
    print("# dynarec-profile hardware bench: %s %s" % (root, o.label))
    groups = []
    if o.rig:
        cycle_table(root, o.ref)
        # round k of fixture F = the k-th BASE/PROTO/SAMP run of F, by cycle
        seen = {}
        for d in sorted(root.glob("auto*-solo")):
            parts = d.name.split("-")          # autoNNN, A, BASE, solo
            if len(parts) != 4:
                continue
            fx, kind = parts[1], parts[2]
            seen.setdefault((fx, kind), []).append(d)
        for fx, fixname in (("A", "AW2 scene tour"), ("H", "H&S heavy-music battle")):
            n = max([len(seen.get((fx, k), [])) for k in ("BASE", "PROTO", "SAMP")] + [0])
            for r in range(n):
                dirs = [seen.get((fx, k), [])[r] if r < len(seen.get((fx, k), [])) else None
                        for k in ("BASE", "PROTO", "SAMP")]
                groups.append(("%s, round %d (%s)" % (fixname, r + 1, ", ".join(
                    x.name for x in dirs if x)), dirs))
    else:
        groups = [(fixname, [root / f for f in folders]) for fixname, folders in FIX]
    for fixname, folders in groups:
        runs = [load(f) if f else None for f in folders]
        if not any(runs):
            continue
        print("\n## %s" % fixname)
        for arm, r in zip(ARMS, runs):
            if r:
                print("  %-8s %s | %s | %s" % (arm, r["info"].get("psp_model", "?"),
                                               r["info"].get("jit_cache", "?"),
                                               r["info"].get("exit", "?")))
        base = runs[0]
        if not base:
            continue
        print("\n  %-22s %6s | %-19s | %-19s | %-19s" % (
            "scene (ms/frame)", "frames", "base med/p95/mean", "proto med/p95/mean",
            "sampler med/p95/mean"))
        allc = [[], [], []]
        for name, f0, f1 in scenes(base):
            cells = []
            for k, r in enumerate(runs):
                if not r:
                    cells.append("-")
                    continue
                # align by the same mark in each run
                mk = [m for m in r["marks"] if m[1] == name]
                if not mk:
                    cells.append("-")
                    continue
                off = mk[-1][0] - (f0 - 1)
                v = cost(r, f0 + off, f1 + off)
                allc[k] += v
                cells.append("%5.2f %5.2f %5.2f" % (st.median(v) / 1000, pct(v, 95) / 1000,
                                                    st.mean(v) / 1000) if v else "-")
            print("  %-22s %6d | %-19s | %-19s | %-19s" % (name[:22], f1 - f0 + 1, *cells))
        cells = ["%5.2f %5.2f %5.2f" % (st.median(v) / 1000, pct(v, 95) / 1000, st.mean(v) / 1000)
                 if v else "-" for v in allc]
        print("  %-22s %6s | %-19s | %-19s | %-19s" % ("ALL", "", *cells))
        if allc[0] and allc[1]:
            print("  proto vs base:  mean %+.1f%%, p95 %+.1f%%" % (
                100 * (st.mean(allc[1]) / st.mean(allc[0]) - 1),
                100 * (pct(allc[1], 95) / pct(allc[0], 95) - 1)))
        if allc[0] and allc[2]:
            print("  sampler cost:   mean %+.1f%% (sampler vs base)" % (
                100 * (st.mean(allc[2]) / st.mean(allc[0]) - 1)))
        print("\n  oracle base vs proto  : %s" % (oracle(base, runs[1]) if runs[1] else "-"))
        print("  oracle base vs sampler: %s" % (oracle(base, runs[2]) if runs[2] else "-"))
        s = runs[2]
        if s and s["windows"]:
            n, run, tot = split(s["windows"])
            core = sum(tot[i] for i in range(16) if PH[i] not in ("outside",))
            print("\n  sampler: %d samples, %d with the emulation thread running (%.0f%%)"
                  % (n, run, 100.0 * run / n if n else 0))
            print("  time split of CORE samples (retro_run; 'outside' excluded):")
            for i in sorted(range(16), key=lambda i: -tot[i]):
                if tot[i] and PH[i] != "outside":
                    print("    %-8s %6.1f%%  (%d)" % (PH[i], 100.0 * tot[i] / core, tot[i]))
            print("  per scene (window = 300 frames ending at f; approximate):")
            for name, f0, f1 in scenes(s):
                nn, rr, t = split(s["windows"], f0, f1)
                c = sum(t[i] for i in range(16) if PH[i] != "outside")
                if not c:
                    continue
                top = sorted(range(16), key=lambda i: -t[i])
                print("    %-20s " % name[:20] + "  ".join(
                    "%s %.0f%%" % (PH[i], 100.0 * t[i] / c) for i in top[:6]
                    if t[i] and PH[i] != "outside"))


if __name__ == "__main__":
    main()
