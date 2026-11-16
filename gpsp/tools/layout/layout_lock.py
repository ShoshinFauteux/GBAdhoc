#!/usr/bin/env python3
"""layout_lock.py -- hold the pinned layout to tools/layout/layout.lock.

The release layout rests on linker pins (`pin` lines, which psp/pins.ld feeds
to the link: the regions after main, code-health C3-3; .rodata, C3-4; the hot
order's rows, C3-5) and the link order that puts each where it is
(docs/LAYOUT-PINNING.md, "Pins", "Row pins").  Nothing else catches it when a change moves one of them: hotord_check
checks the hot functions' order, not the holes, and a pad that silently stops
working only shows up as a slower frame on hardware.  This tool compares a
build's `psp-nm -S -n` listing with the lock and fails on any difference.

    layout_lock.py check FLAVOUR NM [--lock FILE]
    layout_lock.py write FLAVOUR NM [--lock FILE]     re-record the values (not pins)
    layout_lock.py pins  FLAVOUR NM                    the manifest's layoutPins line
    layout_lock.py stats FLAVOUR NM [--ref NM]          each pin's gap, slack used vs REF
    layout_lock.py regen FLAVOUR (--spend-page PIN | --free-page PIN | --move PIN=0xADDR)
    layout_lock.py strict KEY=VALUE...                 1 if the lock is enforced
    layout_lock.py verdict KIND FLAVOUR RC STRICT [LAYOUT_LOCK]
                                                       manifest result + action

`strict` and `verdict` are tools/build.sh's policy (critique-9 SF3), here so
it is tested: which arms must match the lock, and what a check's exit code
means for the build and for the manifest's layoutLock (make_release.sh ships
only `release:ok`).

FLAVOUR is a [section] of the lock: `release` (tools/build.sh release,
RFU_SCHED=1), `lab` (tools/build.sh lab), or a rig profile's (pins only).

Lock lines (one per line, `#` comments):
    at   SYM[#N] 0xMOD     the symbol's address mod 8192
    size SYM[#N] BYTES     the symbol's size (main: a size change moves the
                           frame loop's window, so it must be re-checked)
    gap  SYM[#N] SYM2[#M] BYTES   address(SYM2) - address(SYM)
    tail SYM[#N] BYTES     next symbol's address - (address + size): a hole
    pin  NAME 0xADDR       an INPUT of the link: the region after pin NAME
                           starts at ADDR (absolute); checked against the
                           __pin_at_NAME symbol the link defines

PINS AND THE OVERRUN POLICY (critique C-3 point 1).  A pin always reaches its
offset mod 8 KiB.  If the code before it grows past ADDR, the gap wraps and
the region lands 8 KiB higher: a PSP-1000 heap page spent.  A strict link
(the release always) fails on that, and `write` never touches pins.  Only
`regen` does, and only with an explicit choice: --spend-page (accept the page:
that pin and every later one +8 KiB), --free-page (the reverse, after a
shrink), --move (a new offset: an I-cache placement change, hardware ABBA).
It prints what the commit message has to say.

SYM#N is the N-th symbol of that name in address order (0 = first), so
instance A's and B's same-named file statics are not mixed up.  Only symbols
at or above 0x08800000 count (absolute and SDK symbols are skipped).

`write` keeps the keys and comments and replaces only the values, so the lock
says what is held and the build says where it is.  Run it only after a
re-pin that was checked (loop_diff, moddiff, hotord_check) and say so in the
commit.

(`solve`, the first-order pad solver, went with the last pads in code-health
C3-5: every hold is a pin, and a pin is moved only by `regen`.)
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
    return {"at": 1, "size": 1, "gap": 2, "tail": 1, "pin": 1}.get(kind)  # (no `pad` since C3-5)


def pin_landed(by, name):
    """Where the link put pin NAME: the absolute __pin_at_NAME symbol that
    psp/gbadhoc.ld / psp/groups.ld define, and the gap the pin filled."""
    at, _, _ = sym(by, "__pin_at_" + name)
    try:
        frm, _, _ = sym(by, "__pin_from_" + name)
    except KeyError:
        frm = None
    return at, (at - frm if frm is not None else None)


def pin_why(want, got):
    """What a pin that landed off its lock address means (the overrun policy,
    critique C-3 point 1): whole pages are a wrap or a freed page, anything
    else an offset change."""
    d = got - want
    if d and d % PAGE == 0:
        n = d // PAGE
        return ("wrapped: %+d KiB, %d PSP-1000 heap page(s) spent; regen --spend-page" % (n * 8, n)
                if n > 0 else
                "%d KiB lower: %d heap page(s) freed; regen --free-page" % (-n * 8, -n))
    return "a different offset (%+d B): an I-cache placement change; regen --move" % d


def cmd_check(lock, flavour, by, text_addrs, quiet=False):
    bad = 0
    n = 0
    for i, kind, args, raw in lock[flavour]:
        vi = value_arg(kind)
        if vi is None:
            continue
        n += 1
        want = args[vi]
        if kind == "pin":
            try:
                at, gap = pin_landed(by, args[0])
            except KeyError as e:
                print("  MISSING  pin   %-44s %s" % (args[0], e))
                bad += 1
                continue
            if at != int(want, 16):
                bad += 1
                print("  MOVED    pin   %-44s lock %s, build 0x%08x: %s"
                      % (args[0], want, at, pin_why(int(want, 16), at)))
            elif not quiet:
                print("  ok       pin   %-44s 0x%08x (gap %s B)" % (args[0], at, gap))
            continue
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
        if kind == "pin":
            # a pin is an INPUT of the link: only `regen` changes it, by an
            # explicit choice (critique C-3 point 1)
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


def cmd_pins(lock, flavour, by):
    """One line for the manifest's layoutPins: the flavour and the gap each pin
    filled (the bytes the region before it can still grow without a wrap)."""
    out = [flavour]
    for i, kind, args, raw in lock[flavour]:
        if kind == "pin":
            try:
                at, gap = pin_landed(by, args[0])
                out.append("%s=%s" % (args[0], gap if at == int(args[1], 16) else "MOVED"))
            except KeyError:
                out.append("%s=MISSING" % args[0])
    print(" ".join(out))
    return 0


def cmd_stats(lock, flavour, by, text_addrs, ref):
    """The success measure's numbers (code-health C3-6): for every pin, the gap
    it filled (the slack left before it) and, with a reference build, how much
    of the slack a change used; then every other lock entry that differs.  A
    pin that MOVED is a regen decision; a differing `at`/`size` is a checked
    value a change moved (re-record with `write` once it is accepted)."""
    rby = None
    if ref:
        rby, _ = load_nm(ref)
    used = 0
    print("layout.lock [%s] pins: gap now%s" % (flavour, ", reference, slack used" if rby else ""))
    for i, kind, args, raw in lock[flavour]:
        if kind != "pin":
            continue
        try:
            at, gap = pin_landed(by, args[0])
        except KeyError:
            print("  %-22s MISSING" % args[0])
            continue
        moved = at != int(args[1], 16)
        line = "  %-22s %6s" % (args[0], "MOVED" if moved else gap)
        if rby is not None:
            try:
                _, rgap = pin_landed(rby, args[0])
                line += " %6s %+7d" % (rgap, rgap - gap)
                used += rgap - gap
            except KeyError:
                line += "      -"
        print(line)
    if rby is not None:
        print("  slack used in all: %+d B" % used)
    n = 0
    for i, kind, args, raw in lock[flavour]:
        vi = value_arg(kind)
        if vi is None or kind == "pin":
            continue
        try:
            got = measure(by, text_addrs, kind, args)
        except KeyError:
            print("  MISSING  %-5s %s" % (kind, " ".join(args[:vi])))
            n += 1
            continue
        same = (int(args[vi], 16) == int(got, 16)) if kind == "at" else (int(args[vi]) == int(got))
        if not same:
            print("  differs  %-5s %-34s lock %s, build %s" % (kind, " ".join(args[:vi]), args[vi], got))
            n += 1
    print("  %d checked value(s) differ" % n)
    return 0


def cmd_regen(path, lines, lock, flavour, spend, free, move):
    """Change pins, and only by an explicit choice (critique C-3 point 1: a
    regen never decides silently).  --spend-page NAME: NAME and every pin
    after it +8 KiB (the region before NAME outgrew it; one PSP-1000 heap
    page).  --free-page NAME: -8 KiB from NAME on (a page given back).
    --move NAME=0xADDR: NAME alone at a new address, i.e. a new offset mod
    8 KiB: an I-cache placement change, a hardware event."""
    # in address order: "every later pin" is every pin linked after it
    pins = sorted([(i, args[0], int(args[1], 16), raw)
                   for i, kind, args, raw in lock[flavour] if kind == "pin"], key=lambda x: x[2])
    if not pins:
        print("layout.lock [%s] has no pins" % flavour)
        return 2
    chosen = [x for x in (spend, free, move) if x]
    if len(chosen) != 1:
        for i, n, a, raw in pins:
            print("  pin %-10s 0x%08x" % (n, a))
        print("regen never decides: name exactly one of --spend-page NAME, "
              "--free-page NAME, --move NAME=0xADDR")
        return 2
    names = [n for i, n, a, raw in pins]
    target = (move.split("=", 1)[0] if move else chosen[0])
    if target not in names:
        print("layout.lock [%s] has no pin %s (pins: %s)" % (flavour, target, " ".join(names)))
        return 2
    k = names.index(target)
    new = {}
    if spend or free:
        d = PAGE if spend else -PAGE
        for i, n, a, raw in pins[k:]:
            new[i] = a + d
    else:
        try:
            v = int(move.split("=", 1)[1], 16)
        except (IndexError, ValueError):
            print("--move wants NAME=0xADDR")
            return 2
        if v < 0x08800000:
            print("--move %s: 0x%08x is not an EBOOT address" % (target, v))
            return 2
        new[pins[k][0]] = v
    for i, n, a, raw in pins:
        if i not in new:
            continue
        toks = re.split(r"(\s+)", raw)
        words = [j for j, t in enumerate(toks) if t and not t.isspace()]
        toks[words[2]] = "0x%08x" % new[i]
        lines[i] = "".join(toks)
        print("  pin %-10s 0x%08x -> 0x%08x" % (n, a, new[i]))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    if spend:
        print("page spent: the commit message must say so, with the PSP-1000 boot-heap census")
    elif free:
        print("page freed: the commit message must say so, with the PSP-1000 boot-heap census")
    else:
        print("OFF changed: an I-cache placement change; the commit needs a hardware ABBA")
    return 0


# The switches a strict arm must have at their release values.  ME_LOWLAT=2 is
# the source default (main_psp.c PLAY_ME_LOWLAT), so unset/empty/2 is the
# same program; INPUT_FRESH and MGIFT default to 1 (unset = the default).
STRICT_ON = ("RFU_SCHED", "GBA_LINK", "REWIND", "ONLINE", "CHEATS", "LAYOUT_PIN")
STRICT_DEFAULT_ON = ("INPUT_FRESH", "MGIFT")
STRICT_EMPTY = ("PSP_EXTRA", "CORE_EXTRA", "FE_EXTRA")


def strictness(env):
    """-> (strict, reasons): must a release / committed-lab-pads arm with
    these tools/build.sh variables match the lock?  The rules of the shell it
    replaces (build.sh before round 10), plus MGIFT."""
    why = []
    for k in STRICT_EMPTY:
        if env.get(k, ""):
            why.append("%s set" % k)
    if env.get("ME_LOWLAT", "") not in ("", "2"):
        why.append("ME_LOWLAT=%s" % env["ME_LOWLAT"])
    for k in STRICT_ON:
        if env.get(k, "") != "1":
            why.append("%s=%s" % (k, env.get(k, "")))
    for k in STRICT_DEFAULT_ON:
        if env.get(k, "") not in ("", "1"):
            why.append("%s=%s" % (k, env[k]))
    lean = env.get("GBALINK_LEAN_OUTLINE", "")
    if lean and lean != "0":
        why.append("GBALINK_LEAN_OUTLINE=%s" % lean)
    return (not why), why


def verdict(kind, flavour, rc, strict, layout_lock=""):
    """-> (manifest layoutLock, action): action is ok | note | die."""
    if rc == 0:
        return flavour + ":ok", "ok"
    if rc != 1:
        # psp-nm failed or no such section: never "differs"
        return flavour + ":unchecked", ("die" if strict else "note")
    # LAYOUT_LOCK=0 lets a lab build through while a re-pin is in progress;
    # a release never.
    if strict and (kind == "release" or (layout_lock or "1") != "0"):
        return flavour + ":differs", "die"
    return flavour + ":differs", "note"


def policy_main(argv):
    if argv[0] == "strict":
        env = {}
        for kv in argv[1:]:
            k, sep, v = kv.partition("=")
            if not sep:
                print("strict: %r is not KEY=VALUE" % kv)
                return 2
            env[k] = v
        ok, why = strictness(env)
        print("1" if ok else "0 " + ", ".join(why))
        return 0
    if len(argv) not in (5, 6):
        print("usage: layout_lock.py verdict KIND FLAVOUR RC STRICT [LAYOUT_LOCK]")
        return 2
    res, act = verdict(argv[1], argv[2], int(argv[3]), argv[4] == "1",
                       argv[5] if len(argv) == 6 else "")
    print(res, act)
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] in ("strict", "verdict"):
        return policy_main(sys.argv[1:])
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("cmd", choices=["check", "write", "pins", "regen", "stats"])
    ap.add_argument("--ref", help="stats: the reference build's psp-nm -S -n")
    ap.add_argument("flavour")
    ap.add_argument("nm", nargs="?")
    ap.add_argument("--lock", default=DEFAULT_LOCK)
    ap.add_argument("-q", "--quiet", action="store_true", help="check: print only differences")
    ap.add_argument("--spend-page", metavar="PIN", help="regen: PIN and later +8 KiB")
    ap.add_argument("--free-page", metavar="PIN", help="regen: PIN and later -8 KiB")
    ap.add_argument("--move", metavar="PIN=0xADDR", help="regen: PIN alone to ADDR")
    a = ap.parse_args()
    lock, lines = parse_lock(a.lock)
    if a.flavour not in lock:
        print("layout.lock has no [%s] section" % a.flavour)
        return 2
    if a.cmd == "regen":
        return cmd_regen(a.lock, lines, lock, a.flavour, a.spend_page, a.free_page, a.move)
    if not a.nm:
        print("%s needs NM (psp-nm -S -n of the ELF)" % a.cmd)
        return 2
    by, text_addrs = load_nm(a.nm)
    if not by:
        print("%s: no symbols (is it `psp-nm -S -n` output?)" % a.nm)
        return 2
    if a.cmd == "check":
        return cmd_check(lock, a.flavour, by, text_addrs, a.quiet)
    if a.cmd == "write":
        return cmd_write(a.lock, lines, lock, a.flavour, by, text_addrs)
    if a.cmd == "stats":
        return cmd_stats(lock, a.flavour, by, text_addrs, a.ref)
    return cmd_pins(lock, a.flavour, by)


if __name__ == "__main__":
    sys.exit(main())
