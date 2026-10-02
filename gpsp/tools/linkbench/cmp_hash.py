#!/usr/bin/env python3
"""cmp_hash.py A B [--from-a F] [--from-b F] [--seg last|N]

Compare two per-frame guest-state hash files (lb_host --hash, or the PSP
harness log/shash.txt) over the MEASURED segment: by default the frames after
the last state load (lb_host `s=` field).  For PSP shash files, which carry
no segment, pass --from-a/--from-b: the first frame after the reload.

Prints the number of identical frames, the first differing frame (index from
segment start) and which fields differ there, and the first divergence of
each field.  Exit 0 = identical over the common length, 1 = diverged.
"""
import argparse, sys

FIELDS = ["a", "n", "r", "pc", "i", "e", "io", "p", "o", "v"]

def load(path, seg, frm):
    rows = []
    for ln in open(path):
        if ln.startswith("#") or not ln.startswith("f="):
            continue
        d = dict(kv.split("=", 1) for kv in ln.split())
        rows.append(d)
    if frm is not None:
        return [r for r in rows if int(r["f"]) >= frm]
    if rows and "s" in rows[0]:
        s = max(int(r["s"]) for r in rows) if seg == "last" else int(seg)
        return [r for r in rows if int(r["s"]) == s]
    return rows

def markf(log, mark):
    f = None
    for ln in open(log, errors="replace"):
        if "ap_mark" in ln and f"text={mark} " in ln:
            f = int(ln.split(" f=")[1].split()[0])
    if f is None:
        raise SystemExit(f"mark {mark} not in {log}")
    return f

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a"); ap.add_argument("b")
    ap.add_argument("--from-a", type=int); ap.add_argument("--from-b", type=int)
    ap.add_argument("--seg", default="last")
    ap.add_argument("--log-a"); ap.add_argument("--log-b")
    ap.add_argument("--mark", help="align on the LAST `EVT ap_mark text=MARK f=N` "
                    "in --log-a/--log-b (frame N+1 onward)")
    ap.add_argument("--ignore", default="", help="comma list of fields to skip (e.g. a,n)")
    ap.add_argument("--quiet", action="store_true")
    o = ap.parse_args()
    ign = set(x for x in o.ignore.split(",") if x)
    if o.mark:
        o.from_a = markf(o.log_a, o.mark) + 1
        o.from_b = markf(o.log_b, o.mark) + 1
    A = load(o.a, o.seg, o.from_a); B = load(o.b, o.seg, o.from_b)
    n = min(len(A), len(B))
    first = {}
    firstany = None
    for i in range(n):
        for f in FIELDS:
            if f in ign or f not in A[i] or f not in B[i]:
                continue
            if A[i][f] != B[i][f] and f not in first:
                first[f] = i
                if firstany is None:
                    firstany = i
    if firstany is None:
        print(f"IDENTICAL {n} frames (a={len(A)} b={len(B)})")
        return 0
    diff = [f for f in FIELDS if f not in ign and A[firstany].get(f) != B[firstany].get(f)]
    print(f"DIVERGED at index {firstany} (A f={A[firstany]['f']}, B f={B[firstany]['f']}); "
          f"identical before it: {firstany}/{n}; fields: {','.join(diff)}")
    if not o.quiet:
        for f in sorted(first, key=first.get):
            print(f"  first {f:3s} diff at index {first[f]}")
    return 1

if __name__ == "__main__":
    sys.exit(main())
