#!/usr/bin/env python3
"""layout_lock.py -- hold the pinned layout to tools/layout/layout.lock.

The release layout rests on five pads (psp/Makefile LAYOUT_PAD_*), two 32 B
holes in the warm tail and the link order that puts each where it is
(docs/LAYOUT-PINNING.md, "Keeping the pads").  Nothing else catches it when a
change moves one of them: hotord_check checks the hot functions' order, not
the holes, and a pad that silently stops working only shows up as a slower
frame on hardware.  This tool compares a build's `psp-nm -S -n` listing with
the lock and fails on any difference.

    layout_lock.py check FLAVOUR NM [--lock FILE]
    layout_lock.py write FLAVOUR NM [--lock FILE]     re-record the values
    layout_lock.py solve FLAVOUR NM [--lock FILE] [--pads FILE|--makefile FILE]

FLAVOUR is a [section] of the lock: `release` (tools/build.sh release,
RFU_SCHED=1) or `lab` (tools/build.sh lab with tools/layout/lab-pads.env).

Lock lines (one per line, `#` comments):
    at   SYM[#N] 0xMOD     the symbol's address mod 8192
    size SYM[#N] BYTES     the symbol's size (main: a size change moves the
                           frame loop's window, so it must be re-checked)
    gap  SYM[#N] SYM2[#M] BYTES   address(SYM2) - address(SYM)
    tail SYM[#N] BYTES     next symbol's address - (address + size): a hole
    pad  VAR PADSYM ANCHOR[#N]    the pad VAR, whose section holds PADSYM,
                           places ANCHOR (used by `solve`)

SYM#N is the N-th symbol of that name in address order (0 = first), so
instance A's and B's same-named file statics are not mixed up.  Only symbols
at or above 0x08800000 count (absolute and SDK symbols are skipped).

`write` keeps the keys and comments and replaces only the values, so the lock
says what is held and the build says where it is.  Run it only after a
re-pin that was checked (loop_diff, moddiff, hotord_check) and say so in the
commit.

`solve` is a first-order pad solver: for each `pad` line in link order it
computes how far its anchor is from the locked offset, after the shift the
earlier pads' changes cause, and prints new values (rounded to 4 B, kept in
0..8191).  Alignment of the next input section, `.rodata`'s 16-64 B steps and
`.data.pin` are not modelled, so relink with the printed values and `check`.
It refuses (exit 3, no values) while a `gap` or `tail` differs: a hole is not
a pad, and a pad that compensates for a missing hole moves the hole's
neighbours instead.
"""
import argparse
import collections
import os
import re
import sys

PAGE = 8192
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LOCK = os.path.join(HERE, "layout.lock")


def load_nm(path):
    """name -> list of (addr, size) in address order; plus a sorted list of
    sized-or-not symbol addresses for `tail`."""
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) < 3 or len(p[0]) != 8:
                continue
            try:
                a = int(p[0], 16)
            except ValueError:
                continue
            if a < 0x08800000:
                continue
            if len(p) >= 4:
                try:
                    s = int(p[1], 16)
                except ValueError:
                    s = 0
            else:
                s = 0
            rows.append((a, s, p[-2], p[-1]))
    rows.sort(key=lambda r: (r[0], r[3]))
    by = collections.defaultdict(list)
    for a, s, t, n in rows:
        by[n].append((a, s, t))
    text_addrs = sorted({a for a, s, t, n in rows if t in "Tt"})
    return by, text_addrs


def sym(by, key):
    name, _, occ = key.partition("#")
    occ = int(occ) if occ else 0
    lst = by.get(name, [])
    if occ >= len(lst):
        raise KeyError("%s: %d copies in the ELF" % (key, len(lst)))
    return lst[occ]


def parse_lock(path):
    """{flavour: [(lineno, kind, args, raw)]}, and the raw lines."""
    out = collections.defaultdict(list)
    cur = None
    lines = open(path, encoding="utf-8").read().splitlines()
    for i, raw in enumerate(lines):
        # a `SYM#N` is not a comment: strip only " #" comments
        body = re.split(r"\s#", raw, maxsplit=1)[0].strip()
        if not body or body.startswith("#"):
            continue
        m = re.match(r"^\[(\w+)\]$", body)
        if m:
            cur = m.group(1)
            continue
        if cur is None:
            raise SystemExit("%s:%d: entry before any [flavour]" % (path, i + 1))
        p = body.split()
        out[cur].append((i, p[0], p[1:], raw))
    return out, lines


def measure(by, text_addrs, kind, args):
    """The value the ELF has for one lock entry, as the lock spells it."""
    if kind == "at":
        a, _, _ = sym(by, args[0])
        return "0x%04x" % (a % PAGE)
    if kind == "size":
        _, s, _ = sym(by, args[0])
        return str(s)
    if kind == "gap":
        a, _, _ = sym(by, args[0])
        b, _, _ = sym(by, args[1])
        return str(b - a)
    if kind == "tail":
        a, s, _ = sym(by, args[0])
        import bisect
        i = bisect.bisect_right(text_addrs, a + s - 1)
        nxt = text_addrs[i] if i < len(text_addrs) else a + s
        return str(nxt - (a + s))
    return None


def value_arg(kind):
    return {"at": 1, "size": 1, "gap": 2, "tail": 1}.get(kind)


def cmd_check(lock, flavour, by, text_addrs, quiet=False):
    bad = 0
    n = 0
    for i, kind, args, raw in lock[flavour]:
        vi = value_arg(kind)
        if vi is None:
            continue
        n += 1
        want = args[vi]
        try:
            got = measure(by, text_addrs, kind, args)
        except KeyError as e:
            print("  MISSING  %-5s %-44s %s" % (kind, " ".join(args[:vi]), e))
            bad += 1
            continue
        if kind == "at":
            ok = int(want, 16) == int(got, 16)
        else:
            ok = int(want) == int(got)
        if not ok:
            bad += 1
            print("  MOVED    %-5s %-44s lock %s, build %s" % (kind, " ".join(args[:vi]), want, got))
        elif not quiet:
            print("  ok       %-5s %-44s %s" % (kind, " ".join(args[:vi]), got))
    print("layout.lock [%s]: %d entries, %d differ" % (flavour, n, bad))
    return 1 if bad else 0


def cmd_write(path, lines, lock, flavour, by, text_addrs):
    for i, kind, args, raw in lock[flavour]:
        vi = value_arg(kind)
        if vi is None:
            continue
        got = measure(by, text_addrs, kind, args)
        old = args[vi]
        # replace the value token only, keeping spacing and any comment
        toks = re.split(r"(\s+)", raw)
        words = [j for j, t in enumerate(toks) if t and not t.isspace()]
        toks[words[vi + 1]] = got
        lines[i] = "".join(toks)
        if old != got:
            print("  %-5s %-44s %s -> %s" % (kind, " ".join(args[:vi]), old, got))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote [%s] in %s" % (flavour, path))
    return 0


def read_pads(pads_file, makefile):
    vals = {}
    if pads_file:
        for line in open(pads_file, encoding="utf-8"):
            m = re.match(r"^\s*(LAYOUT_PAD_\w+)=(\d+)", line)
            if m:
                vals[m.group(1)] = int(m.group(2))
    if makefile:
        for line in open(makefile, encoding="latin-1"):
            m = re.match(r"^(LAYOUT_PAD_\w+)\s*\?=\s*(\d+)", line)
            if m and m.group(1) not in vals:
                vals[m.group(1)] = int(m.group(2))
    return vals


def cmd_solve(lock, flavour, by, text_addrs, pads):
    # A hole (`gap`/`tail`) is not a pad variable: a closed or resized hole
    # moves everything after it, and a pad change that "fixes" the anchor
    # would move the hole's neighbours instead (critique-8 SF2: on arm L the
    # missing warm hole made solve suggest CORE +64, which would move A's
    # thumb lookup again).  Fix the holes first; only then solve the pads.
    holes = 0
    for i, kind, args, raw in lock[flavour]:
        if kind not in ("gap", "tail"):
            continue
        vi = value_arg(kind)
        try:
            got = measure(by, text_addrs, kind, args)
        except KeyError as e:
            print("  HOLE MISSING %-5s %-40s %s" % (kind, " ".join(args[:vi]), e))
            holes += 1
            continue
        if int(got) != int(args[vi]):
            print("  HOLE MOVED   %-5s %-40s lock %s, build %s" % (kind, " ".join(args[:vi]), args[vi], got))
            holes += 1
    if holes:
        print("solve: %d hole(s) differ; restore them (psp/layout_pad_warm.S, "
              "psp/gbalink_b_stubs.c) before solving the pads -- no pad values suggested" % holes)
        return 3
    want = {}
    for i, kind, args, raw in lock[flavour]:
        if kind == "at":
            want[args[0]] = int(args[1], 16)
    plist = []
    for i, kind, args, raw in lock[flavour]:
        if kind == "pad":
            var, padsym, anchor = args[:3]
            a, _, _ = sym(by, padsym)
            plist.append((a, var, padsym, anchor))
    plist.sort()
    shift = 0
    rc = 0
    for a, var, padsym, anchor in plist:
        if anchor not in want:
            print("  %s: anchor %s has no `at` line" % (var, anchor))
            rc = 1
            continue
        cur = sym(by, anchor)[0] % PAGE
        d = (want[anchor] - (cur + shift)) % PAGE
        if d > PAGE // 2:
            d -= PAGE
        d = int(round(d / 4.0)) * 4
        old = pads.get(var)
        if old is None:
            print("  %s: current value unknown (--pads/--makefile)" % var)
            rc = 1
            continue
        new = (old + d) % PAGE
        shift += (new - old)
        print("  %-24s %5d -> %5d  (%+d; anchor %s at 0x%04x, locked 0x%04x)"
              % (var, old, new, new - old, anchor, cur, want[anchor]))
    print("relink with these values, then `layout_lock.py check`: alignment of "
          "the next section and .rodata's steps are not modelled")
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("cmd", choices=["check", "write", "solve"])
    ap.add_argument("flavour")
    ap.add_argument("nm")
    ap.add_argument("--lock", default=DEFAULT_LOCK)
    ap.add_argument("--pads", help="KEY=VALUE file (tools/layout/lab-pads.env)")
    ap.add_argument("--makefile", help="psp/Makefile, for the release pads")
    ap.add_argument("-q", "--quiet", action="store_true", help="check: print only differences")
    a = ap.parse_args()
    lock, lines = parse_lock(a.lock)
    if a.flavour not in lock:
        print("layout.lock has no [%s] section" % a.flavour)
        return 2
    by, text_addrs = load_nm(a.nm)
    if not by:
        print("%s: no symbols (is it `psp-nm -S -n` output?)" % a.nm)
        return 2
    if a.cmd == "check":
        return cmd_check(lock, a.flavour, by, text_addrs, a.quiet)
    if a.cmd == "write":
        return cmd_write(a.lock, lines, lock, a.flavour, by, text_addrs)
    return cmd_solve(lock, a.flavour, by, text_addrs, read_pads(a.pads, a.makefile))


if __name__ == "__main__":
    sys.exit(main())
