#!/usr/bin/env python3
"""test_hot_budget.py -- tools/layout/hot_budget.py against the committed order.

On a synthetic nm built from psp/layout/hot_order.txt's hot core (each
function at its 32 B slot, as -falign-functions=32 places it):
* the generated sizes are OK;
* growth inside a function's last 32 B step (beta.5: update_gba 4068 -> 4084)
  is OK and reported as "nothing moves";
* growth across a step (update_gba -> 4100) is OVER by 32 and --strict exits 1;
* the head-hot last function growing is reported, not counted;
* a placed extent that differs from the slot sum (something linked into the
  core) is OVER even when every size matches.
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "layout", "hot_budget.py")
ORDER = os.path.join(ROOT, "psp", "layout", "hot_order.txt")
BASE = 0x08804b00

fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails += 1


def hot_rows():
    rows = []
    for l in open(ORDER):
        if l.startswith('# ---- warm tail'):
            break
        f = l.split('#', 1)[0].split()
        if len(f) >= 3 and not f[0].startswith('@'):
            rows.append((f[0], int(f[2])))
    return rows


def write_nm(d, sizes, gap_after=None):
    a = BASE
    lines = []
    for n, s in sizes:
        lines.append('%08x %08x T %s' % (a, s, n))
        a += (s + 31) // 32 * 32
        if n == gap_after:
            a += 32
    p = os.path.join(d, 'elf.nm')
    with open(p, 'w', newline='\n') as fh:
        fh.write('\n'.join(lines) + '\n')
    return p


def run(nm, strict=True):
    args = [sys.executable, TOOL, ORDER, nm] + (['--strict'] if strict else [])
    p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True)
    return p.returncode, p.stdout


def resized(rows, name, new):
    return [(n, new if n == name else s) for n, s in rows]


def main():
    rows = hot_rows()
    names = [n for n, _ in rows]
    check(len(rows) >= 3 and 'update_gba' in names, 'order has a hot core with update_gba')
    gen = dict(rows)['update_gba']
    in_slot = (gen + 31) // 32 * 32
    with tempfile.TemporaryDirectory() as d:
        rc, out = run(write_nm(d, rows))
        check(rc == 0 and ': OK' in out and 'matches the slots' in out, 'generated sizes: OK')

        if in_slot > gen:
            rc, out = run(write_nm(d, resized(rows, 'update_gba', in_slot)))
            check(rc == 0 and ': OK' in out and 'nothing moves' in out,
                  'update_gba %d -> %d (same slot): OK, nothing moves' % (gen, in_slot))

        rc, out = run(write_nm(d, resized(rows, 'update_gba', in_slot + 4)))
        check(rc == 1 and 'OVER by 32 bytes' in out,
              'update_gba -> %d (next slot): OVER by 32, --strict exits 1' % (in_slot + 4))
        rc, _ = run(write_nm(d, resized(rows, 'update_gba', in_slot + 4)), strict=False)
        check(rc == 0, 'without --strict an overrun is a report (exit 0)')

        head, hs = rows[-1]
        rc, out = run(write_nm(d, resized(rows, head, hs + 640)))
        check(rc == 0 and ': OK' in out and head in out,
              'head-hot %s growing is reported, not counted' % head)

        rc, out = run(write_nm(d, rows, gap_after=names[1]))
        check(rc == 1 and 'DIFFERS from the slots' in out,
              'a 32 B gap inside the core with unchanged sizes: OVER')
    print('%d failure(s)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
