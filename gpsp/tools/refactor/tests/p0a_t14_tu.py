#!/usr/bin/env python3
"""p0a_t14_tu.py -- T14 helpers for a split main_psp.c (P-review 2026-11-08, used by p0a_b5.sh t14).

  p0a_t14_tu.py grep REGEX GPSP
      every line of main_psp.c's TU that matches REGEX, in TU order (the shell with each
      `#include "fe/X.inc.c"` expanded in place), one cite per line: main_psp.c:N or fe/X.inc.c:N.
      On a single-file tree this is `grep -n` of psp/main_psp.c.
  p0a_t14_tu.py cites HEADER GPSP
      every cite a generated fe_abi_asserts.h makes is RIGHT on GPSP: each `/* FILE:N (kind) */`
      line before a typedef abi_fe_t_NAME, each UNCHECKED `FILE:N NAME` row, and each constant's
      `/* FILE:N */` must name a line of that file holding NAME (a #define of it for constants).
      Exit 1 on any wrong cite or none checked.  A header that cites fragment lines as
      main_psp.c:N (the pre-review tool) fails on a split tree.
"""
import os
import re
import sys


def tu(gpsp):
    psp = os.path.join(gpsp, 'psp')
    out = []
    for i, ln in enumerate(open(os.path.join(psp, 'main_psp.c'), encoding='utf-8', errors='replace').read().split('\n'), 1):
        m = re.match(r'^#include "(fe/[A-Za-z0-9_.]+\.inc\.c)"\s*$', ln)
        if m:
            fp = os.path.join(psp, m.group(1))
            out += [(m.group(1), j, x) for j, x in
                    enumerate(open(fp, encoding='utf-8', errors='replace').read().split('\n'), 1)]
        else:
            out.append(('main_psp.c', i, ln))
    return out


def line_of(gpsp, cite):
    f, n = cite.rsplit(':', 1)
    p = os.path.join(gpsp, f) if f.startswith('tools/') else os.path.join(gpsp, 'psp', f)
    if not os.path.exists(p):
        return None
    ls = open(p, encoding='utf-8', errors='replace').read().split('\n')
    n = int(n)
    return ls[n - 1] if 1 <= n <= len(ls) else None


CITE = r'((?:main_psp\.c|fe/[A-Za-z0-9_.]+|tools/[A-Za-z0-9_./]+):\d+)'


def cites(header, gpsp):
    hl = open(header, encoding='utf-8').read().split('\n')
    n_ok, n_mac, bad = 0, 0, []

    def check(c, name, define=False):
        nonlocal n_ok, n_mac
        t = line_of(gpsp, c)
        pat = r'#\s*define\s+%s\b' % re.escape(name) if define else r'\b%s\b' % re.escape(name)
        if t is not None and re.search(pat, t):
            n_ok += 1
        elif t is not None and not define and re.match(r'^\s*[A-Z_][A-Z0-9_]*\s*\(', t):
            # the copy comes from a macro expanded at that line (PSP_MODULE_INFO declares the
            # __lib_* externs): the -E linemarker names the line of the macro call
            n_ok += 1
            n_mac += 1
        else:
            bad.append('%s %s: that line is %r' % (c, name, t))
    for k, ln in enumerate(hl):
        m = re.match(r'^/\* %s \((extern|block|proto)\)' % CITE, ln)
        if m and k + 1 < len(hl):
            mm = re.search(r'\babi_fe_t_(\w+?)(?:__\d+)?\b', hl[k + 1])
            if mm:
                check(m.group(1), mm.group(1))
            else:
                bad.append('%s: no typedef after the cite' % m.group(1))
            continue
        m = re.match(r'^ \*   %s\s+(\w+)\s' % CITE, ln)
        if m:
            check(m.group(1), m.group(2))
            continue
        m = re.match(r'^#define ABI_(?:FE|FET|TEST)_(\w+) .*/\* %s \*/$' % CITE, ln)
        if m:
            check(m.group(2), m.group(1), define=True)
    print('cites %s on %s: %d right (%d of them a macro call at the cited line), %d wrong'
          % (header, gpsp, n_ok, n_mac, len(bad)))
    for b in bad[:20]:
        print('  WRONG ' + b)
    return 0 if n_ok and not bad else 1


def main(a):
    if len(a) == 3 and a[0] == 'grep':
        rx = re.compile(a[1])
        for f, i, ln in tu(a[2]):
            if rx.search(ln):
                print('%s:%d' % (f, i))
        return 0
    if len(a) == 3 and a[0] == 'cites':
        return cites(a[1], a[2])
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
