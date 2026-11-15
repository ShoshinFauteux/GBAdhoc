#!/usr/bin/env python3
"""hot_budget.py ORDER NM [--strict] -- does the pinned HOT core still fit?

  ORDER  psp/layout/hot_order.txt (its header records the hot budget the
         order was generated for; the hot functions are the lines before
         "# ---- warm tail")
  NM     psp-nm -S -n of the ELF just linked

The hot order is generated for the function sizes of one build
(tools/layout/regen.sh).  When a hot function grows, everything linked behind
it moves and the hot core no longer fits the bytes it was budgeted
(docs/GBA-LINK-MIRROR.md 24: update_gba +688 bytes cost the link pair 8,400
I-cache misses a frame).  This prints the core's size today against the
sizes the order was generated for, and each function that grew; exit 1 with
--strict when it is over.  tools/build.sh runs it on every LAYOUT_PIN=1 build
(HOT_BUDGET_STRICT=1 to make an overrun fail the build).

THE UNIT IS THE SLOT, NOT THE BYTE.  The core is compiled with
-falign-functions=32 (Makefile, platform psp), so each hot function takes its
size rounded up to 32 B, and the next one starts at that boundary.  Growth
inside a function's last 32 B step moves nothing (beta.5: update_gba 4068 ->
4084 B, the same 4096 B slot; docs/LAYOUT-PINNING.md "Keeping the pads",
SCHED_HOT), so it is reported, not counted.  The verdict compares slots:
OVER means a function crossed a 32 B step and everything behind it moved.

When the first hot function and the head-hot one (the last line, of which
only the head was budgeted) are both in NM, the core's placed extent (the
head-hot function's address minus the first one's) is printed too and must
equal the slot sum: a mismatch means something else was linked into the core
(or the alignment changed) and counts as over.
"""
import re
import sys

ALIGN = 32   # -falign-functions=32 (Makefile, platform psp)


def slot(n):
    return (n + ALIGN - 1) // ALIGN * ALIGN


def main():
    order, nm = sys.argv[1], sys.argv[2]
    strict = '--strict' in sys.argv
    budget = None
    hot = []
    for l in open(order):
        m = re.search(r'hot budget (\d+) bytes', l)
        if m:
            budget = int(m.group(1))
        if l.startswith('# ---- warm tail'):
            break
        f = l.split('#', 1)[0].split()
        if len(f) >= 3 and not f[0].startswith('@'):
            hot.append((f[0], int(f[2])))
    size, addr = {}, {}
    for l in open(nm, errors='replace'):
        f = l.split()
        if len(f) == 4 and f[2] in 'TtWw' and f[3] not in size:
            size[f[3]] = int(f[1], 16)
            addr[f[3]] = int(f[0], 16)
    if budget is None or not hot:
        print('hot core: no budget recorded in %s' % order)
        return 0
    # the last hot function is the head-hot one (update_scanline_render):
    # only its head was budgeted, so it is compared by growth, not counted
    body = hot[:-1]
    then = sum(slot(s) for _, s in body)
    now = sum(slot(size.get(n, s)) for n, s in body)
    raw_then = sum(s for _, s in body)
    raw_now = sum(size.get(n, s) for n, s in body)
    grew = [(n, s, size[n]) for n, s in hot if n in size and size[n] != s]
    over = now - then
    extent = None
    first, head = hot[0][0], hot[-1][0]
    if first in addr and head in addr:
        extent = addr[head] - addr[first]
        if extent != now:
            over = max(over, extent - then, 1)
    print('hot core: %d bytes now in %d B slots, %d when the order was generated '
          '(raw %d / %d; generator budget %d): %s'
          % (now, ALIGN, then, raw_now, raw_then, budget,
             'OK' if over <= 0 else 'OVER by %d bytes' % over))
    if extent is not None:
        print('  placed extent %s .. %s: %d bytes (%s)'
              % (first, head, extent,
                 'matches the slots' if extent == now else 'DIFFERS from the slots'))
    for n, a, b in grew:
        moved = slot(a) != slot(b)
        print('  %-40s %6d -> %6d (%+d)%s' % (n, a, b, b - a,
              '' if moved else ', same %d B slot: nothing moves' % slot(b)))
    if over > 0:
        print('  regenerate the order for this build: tools/layout/regen.sh, then rebuild')
        return 1 if strict else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
