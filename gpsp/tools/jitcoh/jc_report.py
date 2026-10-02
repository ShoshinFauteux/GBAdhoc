#!/usr/bin/env python3
"""jc_report.py -- symbolise and summarise a jitcoh_plugin coh.txt.

    jc_report.py COH.TXT BIN [--events N] [--model HAZ_I,...] [--frames A:B]

BIN is the twin binary that produced it (for addr2line; run where
mipsel-linux-gnu-addr2line exists, e.g. inside the drprof-qemu image, or set
ADDR2LINE).  Prints, per model: stale executions, incidents (distinct word x
write version), and the writers ranked by incidents with their inlined
source chain and call stack; then the first N events in full.
"""
import argparse
import collections
import os
import subprocess
import sys


def parse(path):
    hdr, ev, V, Wr, S = [], [], {}, collections.defaultdict(list), {}
    Lo = collections.defaultdict(list)
    for ln in open(path):
        ln = ln.rstrip("\n")
        if ln.startswith("#"):
            hdr.append(ln)
        elif ln.startswith("E "):
            d = {}
            toks = ln.split()[1:]
            d["region"] = toks[2]
            for t in toks:
                if "=" in t:
                    k, v = t.split("=", 1)
                    d[k] = v
            ev.append(d)
        elif ln.startswith("V "):
            t = ln.split()
            V[t[1]] = dict(x.split("=") for x in t[2:])
        elif ln.startswith("W "):
            t = ln.split()
            Wr[t[1]].append(dict(x.split("=") for x in t[2:]))
        elif ln.startswith("L "):
            t = ln.split()
            Lo[t[1]].append(dict(x.split("=") for x in t[2:]))
        elif ln.startswith("S "):
            t = ln.split()
            S[t[1]] = t[2:]
    return hdr, ev, V, Wr, S, Lo


class Sym:
    def __init__(self, binary):
        self.bin = binary
        self.cache = {}
        self.a2l = os.environ.get("ADDR2LINE", "mipsel-linux-gnu-addr2line")

    def prime(self, addrs):
        todo = sorted({a for a in addrs if a and a not in self.cache})
        if not todo:
            return
        p = subprocess.run([self.a2l, "-a", "-f", "-i", "-C", "-e", self.bin] + todo,
                           capture_output=True, text=True)
        cur = None
        lines = p.stdout.splitlines()
        i = 0
        while i < len(lines):
            ln = lines[i]
            if ln.startswith("0x"):
                cur = "%08x" % int(ln, 16)
                self.cache[cur] = []
                i += 1
                continue
            fn = ln
            loc = lines[i + 1] if i + 1 < len(lines) else "?"
            loc = loc.split(" (discriminator")[0]
            loc = "/".join(loc.split("/")[-2:])
            self.cache[cur].append("%s@%s" % (fn, loc))
            i += 2

    def __call__(self, a):
        return self.cache.get(a, ["?"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("coh")
    ap.add_argument("bin")
    ap.add_argument("--events", type=int, default=12)
    ap.add_argument("--model", default="")
    ap.add_argument("--writers", type=int, default=12)
    ap.add_argument("--frames", default="")
    o = ap.parse_args()
    hdr, ev, V, Wr, S, Lo = parse(o.coh)
    models = [m for m in V if not o.model or m in o.model.split(",")]
    if o.frames:
        a, b = (int(x) for x in o.frames.split(":"))
        ev = [e for e in ev if a <= int(e["f"]) <= b]
    sym = Sym(o.bin)
    addrs = set()
    for m in models:
        for w in Wr[m][:o.writers]:
            addrs.add(w["wpc"])
            addrs.update(S.get(w["wstk"], []))
        for w in Lo[m][:o.writers]:
            addrs.add(w["lostra"])
            addrs.update(S.get(w["loststk"], []))
    for e in ev:
        addrs.update([e["wpc"], e["opc"], e["invra"], e.get("lostra", "0")])
        addrs.update(S.get(e["wstk"], []))
        addrs.update(S.get(e.get("loststk", "x"), []))
    sym.prime(addrs)
    for h in hdr:
        print(h)
    for m in models:
        print("\n== %s: stale executions %s, incidents %s" % (
            m, V[m]["stale_exec"], V[m]["incidents"]))
        # group writers by their source chain (the same macro in several
        # inlined copies is one writer)
        for w in Wr[m][:o.writers]:
            chain = sym(w["wpc"])
            stk = S.get(w["wstk"], [])
            print("  %5s  store %s  %s" % (w["incidents"], w["wpc"], " <- ".join(chain)))
            for a in stk[:6]:
                print("           called from %s %s" % (a, sym(a)[-1]))
        if Lo[m]:
            print("  -- ranged I invalidates that overlapped the line AFTER the write"
                  " but did not invalidate it (isem):")
        for w in Lo[m][:o.writers]:
            print("  %5s  kernel call from %s %s" % (w["incidents"], w["lostra"],
                                                  " <- ".join(sym(w["lostra"]))))
            for a in S.get(w["loststk"], [])[:6]:
                print("           called from %s %s" % (a, sym(a)[-1]))
        sel = [e for e in ev if e["v"] == m][:o.events]
        for e in sel:
            print("  EVENT host %s %s frame %s seg %s: executed %s, emitter wrote %s"
                  % (e["pc"], e["region"], e["f"], e["s"], e["got"], e["cur"]))
            print("     writer   store %s %s (frame %s, guest pc %s, store #%s)"
                  % (e["wpc"], sym(e["wpc"])[0], e["wf"], e["wgpc"], e["wst"]))
            for a in S.get(e["wstk"], [])[:5]:
                print("              <- %s %s" % (a, sym(a)[-1]))
            print("     previous store %s %s (frame %s, guest pc %s, value %s)"
                  % (e["opc"], sym(e["opc"])[0], e["of"], e["ogpc"], e["oval"]))
            print("     I line filled frame %s; last invalidate covering it: %s frame %s"
                  " store #%s ra %s %s range %s+%s; now store #%s"
                  % (e["fillf"], e["invk"], e["invf"], e["invst"], e["invra"],
                     sym(e["invra"])[-1] if e["invra"] != "00000000" else "",
                     e["inva"], e["invn"], e["st"]))
            if int(e.get("lostst", "0")) >= int(e["wst"]) and e.get("lostst", "0") != "0":
                print("     MISSED by ranged invalidate %s+%s (frame %s, store #%s) from %s %s"
                      % (e["losta"], e["lostn"], e["lostf"], e["lostst"], e["lostra"],
                         " <- ".join(sym(e["lostra"]))))
                for a in S.get(e["loststk"], [])[:4]:
                    print("              <- %s %s" % (a, sym(a)[-1]))


if __name__ == "__main__":
    sys.exit(main())
