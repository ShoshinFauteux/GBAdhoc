#!/usr/bin/env python3
"""arrival_gap.py — the input-eating PROXY (user's idea, 2026-08-09 night).

If the client's inputs are being eaten, she takes materially longer to walk
to her chair and reach the trade menu than the host does — not milliseconds,
whole seconds, because the mash-autopilot must re-issue each eaten press.
So time the autopilot's own milestones and compare host vs client.

The metric is CLOCK-INDEPENDENT: each console has its own boot-relative
microsecond clock (EVT heartbeat t_us=), so absolute times are not
comparable across consoles.  We measure the ELAPSED interval WITHIN each
console between two milestones and compare those durations.

Frame->wallclock: interpolate over the `EVT heartbeat frames=N t_us=T`
anchors.  Milestone frame: the running frame counter last seen (ap_sync
frame= or heartbeat frames=) before the `EVT ap_mark text=NAME` line.

Usage:
  arrival_gap.py <log>...                     # per-log intervals
  arrival_gap.py --pair <host.log> <join.log> # side-by-side + gap
"""
import re, sys, glob

FROM_MARK = "in_trade_center"
TO_MARKS  = ["seated", "trade_selected", "trade_attempted"]

def parse(path):
    hb = []            # (frame, us) anchors
    cur = 0            # running frame counter (last seen)
    marks = {}         # name -> frame at first occurrence
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"heartbeat frames=(\d+) t_us=(\d+)", line)
        if m:
            cur = int(m.group(1)); hb.append((cur, int(m.group(2)))); continue
        m = re.search(r"ap_sync .*frame=(\d+)", line)
        if m:
            cur = int(m.group(1)); continue
        m = re.search(r"ap_mark text=(\w+)", line)
        if m and m.group(1) not in marks:
            marks[m.group(1)] = cur
    return hb, marks

def f2us(hb, frame):
    if not hb: return None
    if frame <= hb[0][0]:
        return hb[0][1]
    for i in range(1, len(hb)):
        (f0,t0),(f1,t1) = hb[i-1], hb[i]
        if frame <= f1:
            if f1 == f0: return t1
            return t0 + (t1-t0)*(frame-f0)/(f1-f0)
    # past the last anchor: extrapolate at the last known rate
    (f0,t0),(f1,t1) = hb[-2], hb[-1]
    if f1==f0: return t1
    return t1 + (t1-t0)*(frame-f1)/(f1-f0)

def interval_s(path):
    """seconds from FROM_MARK to each TO_MARK; None if a mark is missing."""
    hb, marks = parse(path)
    if FROM_MARK not in marks: return None
    base = f2us(hb, marks[FROM_MARK])
    out = {}
    for tm in TO_MARKS:
        if tm in marks and base is not None:
            t = f2us(hb, marks[tm])
            out[tm] = (t - base)/1e6 if t is not None else None
        else:
            out[tm] = None
    return out

def role_of(path):
    return "host" if "host" in path else ("join" if "join" in path else "?")

def main():
    args = sys.argv[1:]
    if args and args[0] == "--pair":
        h, j = args[1], args[2]
        ih, ij = interval_s(h), interval_s(j)
        print("milestone            host(s)  join(s)   gap(join-host)")
        for tm in TO_MARKS:
            a = ih.get(tm) if ih else None
            b = ij.get(tm) if ij else None
            g = (b-a) if (a is not None and b is not None) else None
            print("  %-18s %7s %8s %10s" % (
                tm,
                "%.2f"%a if a is not None else "  -",
                "%.2f"%b if b is not None else "  -",
                "%+.2f"%g if g is not None else "  -"))
        return
    # aggregate mode: intervals per log, then host vs join medians
    files = []
    for a in args: files += glob.glob(a)
    rows = {"host": {tm: [] for tm in TO_MARKS}, "join": {tm: [] for tm in TO_MARKS}}
    for p in sorted(files):
        iv = interval_s(p)
        if not iv: continue
        r = role_of(p)
        for tm in TO_MARKS:
            if iv.get(tm) is not None:
                rows[r][tm].append(iv[tm])
    def med(xs):
        xs=sorted(xs); n=len(xs)
        return xs[n//2] if n else None
    print("=== in_trade_center -> milestone, median seconds (n) ===")
    print("milestone            host           join           join-host")
    for tm in TO_MARKS:
        h, j = rows["host"][tm], rows["join"][tm]
        mh, mj = med(h), med(j)
        gap = (mj-mh) if (mh is not None and mj is not None) else None
        print("  %-18s %-14s %-14s %s" % (
            tm,
            "%.2f (n=%d)"%(mh,len(h)) if mh is not None else "-",
            "%.2f (n=%d)"%(mj,len(j)) if mj is not None else "-",
            "%+.2fs"%gap if gap is not None else "-"))

if __name__ == "__main__":
    main()
