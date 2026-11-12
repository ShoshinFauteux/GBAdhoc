#!/usr/bin/env python3
"""linkprof_report.py LOG... -- the DRPROF_HW time split of a GBA link
session or one-console bench (EVT linkprof, docs/GBA-LINK-MIRROR.md 16).

Per log: the share of the emulation thread's RUNNING samples in each
frontend phase (loop, machines, sync hash, cart check, net, session,
vblank wait), and inside the machines the per-instance split (A = the
player's machine, drawn; B = the partner's, headless) by core phase
(jit, update_gba, dispatch, flush, xlat, memc, sound, dma, serial, video,
irq, ...).  Converted to ms a frame with the samples' rate (1 kHz) and the
frames per window.  The first window (boot/load) is dropped.
"""
import re
import sys

FE = ["loop", "core", "hash", "crc", "net", "sess", "vwait", "-"]
PH = ["jit", "upd", "disp", "flush", "cmisc", "pcarg", "xlat", "memc", "sound",
      "dma", "serial", "video", "irq", "retro", "14", "outside"]


def parse(path):
    wins = []
    prev = None
    for ln in open(path, errors="replace"):
        if not ln.startswith("EVT linkprof "):
            continue
        d = dict(re.findall(r"(\w+)=(\S+)", ln))
        f = int(d["f"])
        w = dict(f=f, n=int(d["n"]), run=int(d["run"]), wait=int(d["wait"]),
                 fe=[int(x) for x in d["fe"].split(",")],
                 a=[int(x) for x in d["a"].split(",")],
                 b=[int(x) for x in d["b"].split(",")],
                 pair=int(d["pair"]), frames=f - prev if prev is not None else f)
        prev = f
        wins.append(w)
    return wins[1:] if len(wins) > 2 else wins


def parse_loop(path, first_f):
    """EVT looprof (section 17): the loop's own regions, windows after the
    first linkprof window's frame; EVT thrcpu: each thread's CPU per window."""
    names, tot = None, None
    thr = {}
    for ln in open(path, errors="replace"):
        if ln.startswith("EVT looprof "):
            d = dict(re.findall(r"(\w+)=(\S+)", ln))
            if int(d["f"]) <= first_f:
                continue
            v = [int(x) for x in d["lp"].split(",")]
            names = d["names"].split(",")
            tot = v if tot is None else [a + b for a, b in zip(tot, v)]
        elif ln.startswith("EVT thrcpu "):
            d = dict(re.findall(r"(\w+)=(\S+)", ln))
            if int(d["f"]) <= first_f:
                continue
            k = d["name"]
            t = thr.setdefault(k, [0, 0, d["prio"]])
            t[0] += int(d["us"])
            t[1] += int(d["wall"])
    return names, tot, thr


def report(path):
    wins = parse(path)
    if not wins:
        print("%s: no EVT linkprof" % path)
        return
    n = sum(w["n"] for w in wins)
    frames = sum(w["frames"] for w in wins) or 1
    # samples are ~1 ms apart: samples / frame ~= ms / frame
    def ms(x):
        return 1.0 * x / frames * (sum(w["n"] for w in wins) and
                                   1.0) * 1.0
    run = sum(w["run"] for w in wins)
    fe = [sum(w["fe"][i] for w in wins) for i in range(8)]
    a = [sum(w["a"][i] for w in wins) for i in range(16)]
    b = [sum(w["b"][i] for w in wins) for i in range(16)]
    pair = sum(w["pair"] for w in wins)
    wall_ms = 1.0 * n / frames
    print("== %s" % path)
    print("  %d windows, %d frames, %d samples (%.2f ms/frame wall at 1 kHz),"
          " running %.1f %%" % (len(wins), frames, n, wall_ms,
                                100.0 * run / max(n, 1)))
    print("  frontend phases, ms/frame: " + "  ".join(
        "%s %.2f" % (FE[i], fe[i] / frames) for i in range(7)))
    print("  inside the machines: A %.2f  B %.2f  pair loop %.2f ms/frame"
          % (sum(a) / frames, sum(b) / frames, pair / frames))
    print("  %-8s %7s %7s" % ("phase", "A ms", "B ms"))
    for i in range(16):
        if a[i] or b[i]:
            print("  %-8s %7.2f %7.2f" % (PH[i], a[i] / frames, b[i] / frames))
    first_f = wins[0]["f"] - wins[0]["frames"]
    names, lp, thr = parse_loop(path, first_f)
    if lp:
        print("  loop regions, ms/frame (sum %.2f):" % (sum(lp) / frames))
        print("    " + "  ".join("%s %.2f" % (n, x / frames)
                                 for n, x in zip(names, lp) if x))
    if thr:
        print("  threads' own CPU, ms/frame (runClocks):")
        for k, (us, wall, prio) in sorted(thr.items(), key=lambda kv: -kv[1][0]):
            if us * 1000 // max(frames, 1) >= 10:
                print("    %-18s prio %s  %6.2f" % (k, prio, us / 1000.0 / frames))


def main():
    for p in sys.argv[1:]:
        report(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
