#!/usr/bin/env python3
"""drop_test.py — read the ADR-0081 identical-input drop test.

The fixture oscillates UP/DOWN within one tile on BOTH consoles and logs the
avatar's y (dt_y0..8) and gRfu.sendQueue.count (dt_sq0..8, Gate A) after each
press.  Every press MUST toggle y by 1; a press that leaves y unchanged was
DROPPED by the game (input eaten).  Same inputs both roles -> comparing the
drop counts is a controlled experiment: host is the control, join the test.

Usage: drop_test.py <log>...
"""
import re, sys, glob

def read(path):
    y = {}; sq = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"ap_val name=dt_y(\d+) val=0x([0-9a-fA-F]+)", line)
        if m: y[int(m.group(1))] = int(m.group(2), 16)
        m = re.search(r"ap_val name=dt_sq(\d+) val=0x([0-9a-fA-F]+)", line)
        if m: sq[int(m.group(1))] = int(m.group(2), 16)
    return y, sq

def analyse(path):
    y, sq = read(path)
    if 0 not in y:
        return None
    n = max(k for k in y) if y else 0
    landed = dropped = 0
    drops = []
    for i in range(1, n + 1):
        if i not in y or (i - 1) not in y:
            continue
        moved = (y[i] != y[i - 1])
        if moved:
            landed += 1
        else:
            dropped += 1
            drops.append((i, sq.get(i, "?")))
    return dict(n=n, landed=landed, dropped=dropped, drops=drops,
                yseq=[y.get(i) for i in range(0, n + 1)])

def main():
    files = []
    for a in sys.argv[1:]:
        files += glob.glob(a)
    tot = {"host": [0, 0], "join": [0, 0]}   # [landed, dropped]
    for p in sorted(files):
        r = analyse(p)
        if not r:
            continue
        role = "host" if "host" in p else ("join" if "join" in p else "?")
        base = p.split("/")[-1].split("\\")[-1]
        drops = " ".join("press%d(sq=%s)" % (i, s) for i, s in r["drops"])
        print("%-22s %-4s landed=%d/%d dropped=%d   y=%s   %s" % (
            base, role, r["landed"], r["landed"] + r["dropped"],
            r["dropped"], ",".join(str(v) for v in r["yseq"]),
            ("DROPS: " + drops) if drops else ""))
        if role in tot:
            tot[role][0] += r["landed"]; tot[role][1] += r["dropped"]
    print("\n=== totals (same inputs both roles) ===")
    for role in ("host", "join"):
        l, d = tot[role]
        n = l + d
        print("  %-4s landed=%d/%d  dropped=%d  (%.0f%% dropped)" % (
            role, l, n, d, 100.0 * d / n if n else 0))

if __name__ == "__main__":
    main()
