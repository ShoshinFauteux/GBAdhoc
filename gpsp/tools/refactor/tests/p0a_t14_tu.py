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
  p0a_t14_tu.py index FE_PRIV_H HEADER... [--src GPSP]
      fe_priv.h's core-ABI index says what it is (P-review r2, N4): "PLAN-P-beta5 §5.2's set ...
      the names T14 asserts (in any flavour) plus six it cannot".  Checked against the generated
      headers (one per flavour): every name any HEADER asserts is in the index; the index's names
      that no HEADER asserts are exactly the six its "The six of this index that are UNCHECKED"
      sentence names, and each of them is in an UNCHECKED list.  P-review r3 (B6): the text from
      "main()'s" to the end of the comment names exactly the copy sites in no T14 flavour, i.e.
      `abi_asserts.py unbuilt` of GPSP with these HEADERs (the copy names it mentions == the names
      of the sites unbuilt lists).  GPSP defaults to the tree FE_PRIV_H sits in (GPSP/psp/fe/);
      a fe_priv.h elsewhere (a mutant copy) needs --src.  Exit 1 otherwise, 2 usage.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import abi_asserts  # noqa: E402  (unbuilt, raw_sites)


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


IDX_BEGIN = "The index below is PLAN-P-beta5 §5.2's set"
IDX_END = 'T14 (tools/refactor/abi_asserts.py) asserts'
SIX = 'The six of this index that are'


def header_names(header):
    """(asserted names, UNCHECKED names) of one generated fe_abi_asserts.h"""
    h = open(header, encoding='utf-8').read()
    asserted = set(re.findall(r'__typeof__\(&(\w+)\)', h))
    i = h.find('/* UNCHECKED')
    if i < 0:
        raise SystemExit('%s: no UNCHECKED list' % header)
    return asserted, set(re.findall(r'^ \*\s+\S+:\d+\s+(\w+)\s', h[i:], re.M))


def index(fe_priv, headers, gpsp=None):
    t = open(fe_priv, encoding='utf-8').read()
    a, b = t.find(IDX_BEGIN), t.find(IDX_END)
    s = t.find(SIX, b)
    e = t.find("main()'s", s)
    z = t.find('*/', e)
    if a < 0 or b < a or s < 0 or e < s or z < e:
        print('index: %s has no "%s" ... "%s" ... "%s" ... "main()\'s" ... */ comment' % (fe_priv, IDX_BEGIN, IDX_END, SIX))
        return 1
    if gpsp is None:
        p = os.path.abspath(fe_priv).replace('\\', '/')
        if not p.endswith('/psp/fe/fe_priv.h'):
            print('index: %s is not GPSP/psp/fe/fe_priv.h: give --src GPSP' % fe_priv)
            return 2
        gpsp = p[:-len('/psp/fe/fe_priv.h')]
    asserted, unchecked = set(), set()
    for hd in headers:
        x, u = header_names(hd)
        asserted |= x
        unchecked |= u
    known = asserted | unchecked
    idx = set(re.findall(r'\b\w+\b', t[a:b])) & known
    six = set(re.findall(r'\b\w+\b', t[s:e])) & known
    rc = 0
    miss = sorted(asserted - idx)
    print('index %s: %d names (%d asserted in some flavour, %d not); %d headers assert %d names, list %d UNCHECKED'
          % (fe_priv, len(idx), len(idx & asserted), len(idx - asserted), len(headers), len(asserted), len(unchecked)))
    if miss:
        print('  FAIL asserted but not in the index: %s' % ', '.join(miss))
        rc = 1
    if idx - asserted != six:
        print('  FAIL the index names that no flavour asserts %s != the "six" sentence %s'
              % (sorted(idx - asserted), sorted(six)))
        rc = 1
    if len(six) != 6 or not six <= unchecked:
        print('  FAIL the "six" sentence names %d names, %s not in an UNCHECKED list' % (len(six), sorted(six - unchecked)))
        rc = 1
    if not rc:
        print('  ok   every asserted name is in the index; the six it cannot assert: %s' % ', '.join(sorted(six)))
        print('  ok   UNCHECKED names the index does not list: %d' % len(unchecked - idx))
    # B6: the "main()'s ..." text names the sites no T14 flavour compiles, all of them and only them
    try:
        unc, st = abi_asserts.unbuilt(gpsp, headers)
    except (abi_asserts.Unbuilt, ValueError) as x:
        print('  FAIL abi_asserts unbuilt on %s refused: %s' % (gpsp, x))
        return 1
    copy_names = {n for _, n, _, _ in abi_asserts.raw_sites(gpsp)}
    said = set(re.findall(r'\b\w+\b', t[e:z])) & copy_names
    want = {n for _, n, _, _ in unc}
    if said != want:
        print('  FAIL the "main()\'s" text names %s; the sites in no T14 flavour (abi_asserts unbuilt) are %s'
              % (sorted(said), ', '.join('%s %s' % (c, n) for c, n, _, _ in unc)))
        rc = 1
    else:
        print('  ok   the "main()\'s" text names the %d copy sites in no T14 flavour: %s'
              % (len(unc), ', '.join('%s %s' % (c, n) for c, n, _, _ in unc)))
    return rc


def main(a):
    if len(a) >= 3 and a[0] == 'index':
        src = None
        if '--src' in a:
            k = a.index('--src')
            if k + 1 >= len(a):
                print(__doc__)
                return 2
            src, a = a[k + 1], a[:k] + a[k + 2:]
        return index(a[1], a[2:], src)
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
