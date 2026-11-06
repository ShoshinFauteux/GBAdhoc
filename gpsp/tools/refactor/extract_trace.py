#!/usr/bin/env python3
"""extract_trace.py -- T7 support: record WHICH byte ranges of psp/main_psp.c
each host test takes today with its own str.index / str.find / regex slicing,
by running the test unchanged with main_psp.c's text handed out as a tracking
str.  The result is the ground truth tests/source-split_test_extract.py checks
extract.py against ("same text as the old str.index extraction", PLAN §5 T7).

    python3 extract_trace.py GPSP_DIR OUT.json [TEST ...]   (default: every reader
                                                            extract.py --enumerate finds)

Run where the tests run (WSL/Linux: they compile C with the host gcc).  Each
test runs in its own subprocess with a timeout; its pass/fail is recorded but
does not matter here.

What is recorded, as (start, end) offsets of the ORIGINAL file:
  * every slice s[i:j] of the file text, or of a slice of it (offsets compose);
  * the group-0 span of re.search / re.match / re.fullmatch / re.finditer /
    re.findall run directly on such a text.
Not recorded (documented limits): compiled-pattern methods (pattern.search),
str.split/str.partition pieces, and slices of strings derived by replace() or
concatenation (those are mutants built FROM the file, not extractions of it).
Slices that start at offset 0 are dropped: every one in these tests is the
prefix half of a mutant built as src[:i] + new + src[j:], not an extraction.
"""
import json
import os
import subprocess
import sys

CHILD = r'''
import json, os, pathlib, re, runpy, sys
TARGET = os.path.realpath(sys.argv[1]); TEST = sys.argv[2]; OUT = sys.argv[3]
REC = []
class T(str):
    def __new__(cls, s, base):
        o = str.__new__(cls, s); o._b = base; return o
    def __getitem__(self, k):
        r = str.__getitem__(self, k)
        if isinstance(k, slice) and k.step in (None, 1):
            a, b, _ = k.indices(len(self))
            if b > a:
                REC.append((self._b + a, self._b + b, 'slice'))
            return T(r, self._b + a)
        return r
_rt = pathlib.Path.read_text
def read_text(self, *a, **kw):
    s = _rt(self, *a, **kw)
    try:
        if os.path.realpath(str(self)) == TARGET:
            return T(s, 0)
    except Exception:
        pass
    return s
pathlib.Path.read_text = read_text
_open = open
def _wrap(fn, many=False):
    def w(pattern, string, *a, **kw):
        r = fn(pattern, string, *a, **kw)
        if isinstance(string, T):
            if many:
                r = list(r)
                for m in r:
                    if m.end() > m.start():
                        REC.append((string._b + m.start(), string._b + m.end(), 're'))
                return iter(r)
            if r is not None and r.end() > r.start():
                REC.append((string._b + r.start(), string._b + r.end(), 're'))
        return r
    return w
for nm in ('search', 'match', 'fullmatch'):
    setattr(re, nm, _wrap(getattr(re, nm)))
re.finditer = _wrap(re.finditer, many=True)
_fi = re.finditer
_fa = re.findall
def findall(pattern, string, *a, **kw):
    if isinstance(string, T):
        list(_fi(pattern, string, *a, **kw))
    return _fa(pattern, string, *a, **kw)
re.findall = findall
rc = 0
try:
    sys.argv = [TEST]; sys.path.insert(0, os.path.dirname(os.path.abspath(TEST)))
    runpy.run_path(TEST, run_name='__main__')
except SystemExit as e:
    rc = e.code if isinstance(e.code, int) else (0 if e.code is None else 1)
except BaseException as e:
    rc = 'exception: %s: %s' % (type(e).__name__, str(e)[:200])
json.dump({'rc': rc, 'rec': REC}, _open(OUT, 'w'))
'''


def main():
    gpsp = os.path.abspath(sys.argv[1])
    out = sys.argv[2]
    tests = sys.argv[3:]
    if not tests:
        sys.path.insert(0, os.path.join(gpsp, 'tools', 'tests'))
        import extract
        tests = extract.enumerate_readers(gpsp)
    target = os.path.join(gpsp, 'psp', 'main_psp.c')
    size = len(open(target, encoding='utf-8', newline='').read())
    res = {}
    child = os.path.join(os.path.dirname(os.path.abspath(out)), '.extract_trace_child.py')
    open(child, 'w').write(CHILD)
    for t in tests:
        tp = os.path.join(gpsp, t)
        tmp = out + '.part'
        try:
            p = subprocess.run([sys.executable, child, target, tp, tmp], cwd=os.path.dirname(tp),
                               capture_output=True, text=True, timeout=900)
            d = json.load(open(tmp))
            os.remove(tmp)
        except Exception as e:      # noqa: BLE001
            res[t] = {'rc': 'trace failed: %s' % e, 'regions': []}
            print('%-40s TRACE FAILED %s' % (t, e))
            continue
        regs = sorted({(a, b) for a, b, k in d['rec'] if a > 0 and b > a})
        res[t] = {'rc': d['rc'], 'regions': regs, 'dropped_prefix': sum(1 for a, b, k in d['rec'] if a == 0)}
        print('%-40s rc=%-4s %3d region(s)' % (t, d['rc'], len(regs)))
    os.remove(child)
    json.dump({'source': target, 'size': size, 'tests': res}, open(out, 'w'), indent=1)


if __name__ == '__main__':
    main()
