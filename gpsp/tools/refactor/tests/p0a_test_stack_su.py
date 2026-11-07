#!/usr/bin/env python3
"""p0a_test_stack_su.py -- T12 stack_depth P0a repair: .su statics matched by the .su
file's OBJECT stem, not by the record's source file (PLAN-P-beta5 §3 P0a T12).

  p0a_test_stack_su.py FLAVOUR_DIR CUTLIST

Builds a P4-simulated su/ tree: a copy of FLAVOUR_DIR/su in which every main_psp.su
record of a function the cutlist puts in a fragment names `fe/<frag>.inc.c` as its
source file (what -fstack-usage writes after the split, the code being unchanged).
  C1  stack_depth's unmatched count (and TOTAL) on the simulated .su equals the base's
  M1  the pre-P0a matcher (keyed on the record's source file) loses those statics:
      its unmatched count rises -- caught
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.dirname(HERE)
sys.path.insert(0, R)
import split  # noqa: E402

REC = re.compile(r'^(?P<file>[^:]+):(?P<line>\d+):(?P<col>\d+):(?P<name>\S+)(?P<rest>\s.*)$')


def simulate(su_dir, cutlist, out):
    shutil.copytree(su_dir, out)
    _, rows = split.read_cutlist(cutlist)
    frag = {r['name']: r['frag'] for r in rows if r['kind'] == 'F'}
    n = 0
    for root, _, files in os.walk(out):
        for fn in files:
            if fn != 'main_psp.su':
                continue
            p = os.path.join(root, fn)
            lines = open(p, encoding='utf-8', errors='replace').read().split('\n')
            for i, ln in enumerate(lines):
                m = REC.match(ln)
                if not m or os.path.basename(m.group('file')) != 'main_psp.c':
                    continue
                f = frag.get(m.group('name').split('.')[0])
                if f and not f.startswith('shell'):
                    lines[i] = 'fe/%s.inc.c:%s:%s:%s%s' % (f, m.group('line'), m.group('col'),
                                                            m.group('name'), m.group('rest'))
                    n += 1
            open(p, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    return n


def run(tool, elf, su):
    r = subprocess.run([sys.executable, tool, elf, '--su', su, '--resolve',
                        os.path.join(R, 'stack_resolve.txt'), '--tree', '0'], capture_output=True, text=True)
    um = re.search(r'\.su matched (\d+) records, (\d+) unmatched', r.stdout)
    tot = re.search(r'TOTAL .*= (\d+) B', r.stdout)
    return (int(um.group(1)), int(um.group(2))) if um else None, tot.group(1) if tot else None, r.returncode


def main(argv):
    if len(argv) != 2:
        print(__doc__); return 2
    fd, cut = argv
    elf = os.path.join(fd, 'gpsp_adhoc.elf')
    T = tempfile.mkdtemp(prefix='p0a_su')
    bad = 0
    try:
        n = simulate(os.path.join(fd, 'su'), cut, os.path.join(T, 'su'))
        tool = os.path.join(R, 'stack_depth.py')
        b = run(tool, elf, os.path.join(fd, 'su'))
        s = run(tool, elf, os.path.join(T, 'su'))
        ok = b[0] and s[0] and b[0][1] == s[0][1] and b[1] == s[1]
        print('  %s  C1 P4-simulated .su (%d main_psp.su records moved to fe/*.inc.c): unmatched %s -> %s, TOTAL %s -> %s'
              % ('ok  ' if ok else 'FAIL', n, b[0] and b[0][1], s[0] and s[0][1], b[1], s[1]))
        bad += not ok
        # the pre-P0a matcher: statics keyed by the record's own source file
        old = os.path.join(T, 'stack_depth_old.py')
        src = open(tool, encoding='utf-8').read()
        k = "cands = by_local.get((fn[:-3], name)) or by_global.get(name) or []"
        assert k in src
        src = src.replace(k, "cands = by_local.get((_stem(src), name)) or by_global.get(name) or []")
        open(old, 'w', encoding='utf-8').write(src)
        shutil.copy(os.path.join(R, 'elf32.py'), T)
        for dep in ('ef_mips.py', 'ef_dwarf.py'):
            if os.path.exists(os.path.join(R, dep)):
                shutil.copy(os.path.join(R, dep), T)
        ob = run(old, elf, os.path.join(fd, 'su'))
        os_ = run(old, elf, os.path.join(T, 'su'))
        caught = ob[0] and os_[0] and os_[0][1] > ob[0][1]
        print('  %s  M1 record-file matcher on the same .su: unmatched %s -> %s%s'
              % ('ok  ' if caught else 'FAIL', ob[0] and ob[0][1], os_[0] and os_[0][1], ': caught' if caught else ''))
        bad += not caught
    finally:
        shutil.rmtree(T, ignore_errors=True)
    print('p0a_test_stack_su: %s' % ('PASS' if not bad else '%d FAILED' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
