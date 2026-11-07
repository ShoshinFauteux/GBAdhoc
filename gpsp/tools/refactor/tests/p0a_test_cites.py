#!/usr/bin/env python3
"""p0a_test_cites.py -- P0a T6 (PLAN-P-beta5 §3): strong_syms.spec's `file:line` cites
point at lines that hold what they cite, on the tree they are written for (fbe9da63).

    p0a_test_cites.py SPEC GPSP_TREE

  - a row's own cite (`# main_psp.c:N` after the row) must name the row's symbol;
  - a cite in a comment block names a symbol of the rows that follow it (up to the next
    blank line), and a cite in a block that says "weak stub" must be an
    `__attribute__((weak))` line;
  - every hook row has its weak stub cited (a cite whose line names it).
Exit 0 all cites hold, 1 any stale cite, 2 usage.
"""
import os
import re
import sys

def resolve(tree, f):
    for c in (os.path.join(tree, f), os.path.join(tree, 'psp', f)):
        if os.path.isfile(c):
            return c
    return None


def cites(text):
    """[(file, lo, hi)] -- `rfu.c:96`, `mgift_net.c:147-148`."""
    out = []
    for m in re.finditer(r'\b([\w./]+\.(?:c|h|inc)):(\d+)(?:-(\d+))?', text):
        lo = int(m.group(2))
        out.append((m.group(1), lo, int(m.group(3) or lo)))
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    spec, tree = sys.argv[1], sys.argv[2]
    lines = open(spec, encoding='utf-8').read().split('\n')
    rows = []          # (index, name, kind)
    for i, l in enumerate(lines):
        body = l.split('#', 1)[0].strip()
        if body:
            p = body.split()
            rows.append((i, p[0], p[1]))
    cache = {}

    def text(f, lo, hi):
        path = resolve(tree, f)
        if path is None:
            return None
        if path not in cache:
            cache[path] = open(path, encoding='utf-8', errors='replace').read().split('\n')
        L = cache[path]
        return '\n'.join(L[lo - 1:hi]) if 0 < lo <= hi <= len(L) else ''

    fails, n = 0, 0
    stub_named = set()
    block, block_txt = [], ''
    for i, l in enumerate(lines):
        if not l.strip():
            block, block_txt = [], ''
            continue
        body, _, com = l.partition('#')
        if body.strip():
            name = body.split()[0]
            want, weak = [name], False
        else:
            block.append(i)
            block_txt += ' ' + com
            # the rows this comment block describes: up to the next blank line
            want = []
            for j in range(i + 1, len(lines)):
                if not lines[j].strip():
                    break
                b = lines[j].split('#', 1)[0].strip()
                if b:
                    want.append(b.split()[0])
            weak = 'weak stub' in block_txt
        for f, lo, hi in cites(com):
            n += 1
            t = text(f, lo, hi)
            got = [w for w in want if t and re.search(r'\b%s\b' % re.escape(w), t)]
            ok = bool(got) and (not weak or '__attribute__((weak))' in t)
            if ok and weak:
                stub_named.update(got)
            label = '%s:%d%s' % (f, lo, '-%d' % hi if hi != lo else '')
            if ok:
                print('  ok    %-22s names %s' % (label, ', '.join(got)))
            else:
                fails += 1
                print('  FAIL  %-22s should name %s%s; the tree has: %s' % (
                    label, '/'.join(want) or '?', ' in a weak stub' if weak else '',
                    'no such file' if t is None else (t.strip().split('\n')[0][:90] or 'nothing')))
    for i, name, kind in rows:
        if kind == 'hook' and name not in stub_named:
            fails += 1
            print('  FAIL  hook %s: no cite of its weak stub' % name)
    print('p0a_test_cites: %d cites, %s' % (n, 'all hold' if not fails else '%d STALE' % fails))
    return 1 if fails or not n else 0


if __name__ == '__main__':
    sys.exit(main())
