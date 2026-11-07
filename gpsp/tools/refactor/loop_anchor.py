#!/usr/bin/env python3
"""loop_anchor.py -- main()'s frame-loop window in psp/main_psp.c, found by ANCHOR,
not by line number (PLAN-P-beta5 §3 P0a, T1b/T10/T16).

The window the phase-T tools measure (x4's "16368-18090" on 01067e74) is:
  start  the `   while (g_running)` that follows `session_top:` inside `main`
  end    the brace closing the `if (g_perf_rig) { ... }` block right after the
         loop (the loop's own close brace, then `LAB_FFP(lab_ffpace_finish();)`,
         then that block: the old 18090 tail rule); without such a block, the
         loop's own close brace.
On 01067e74 this resolves to exactly 16368-18090 (the self-test proves it).

The SOURCE must be the one the ELF was built from.  Found in this order:
  1. --main-c PATH (explicit);
  2. the ELF's flavours.sh record: <elf dir>/flavour.json sourceCommit ->
     `git show <commit>:gpsp/psp/main_psp.c` in the repo these tools live in;
  3. this tree's psp/main_psp.c.
and then CHECKED: main's DW_AT_decl_line in the ELF must equal the source's
`int main(` line, else LookupError (a stale or wrong source would silently
measure other code; that is how the hard-coded windows failed).

    loop_anchor.py MAIN_C            print the resolved lines of a source file
    loop_anchor.py --elf ELF [--main-c P]   resolve for an ELF (with the check)
    loop_anchor.py --selftest
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
TREE_MAIN = os.path.normpath(os.path.join(HERE, '..', '..', 'psp', 'main_psp.c'))


def clean_c(text):
    """comments, string and char literals blanked (newlines kept), so braces count."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


def _close(clean, j):
    """index of the brace closing the '{' at or after j."""
    j = clean.index('{', j)
    dep = 0
    while True:
        ch = clean[j]
        if ch == '{':
            dep += 1
        elif ch == '}':
            dep -= 1
            if dep == 0:
                return j
        j += 1


def resolve(text):
    """-> dict(main, session_top, loop0, loop_close, tail_end, lo, hi) (1-based lines)."""
    clean = clean_c(text)
    lineof = lambda off: text.count('\n', 0, off) + 1
    m = re.compile(r'^int main\s*\(', re.M).search(clean)
    if not m:
        raise LookupError('no `int main(` definition')
    mend = _close(clean, m.end())
    st = re.compile(r'^session_top:', re.M).search(clean, m.end(), mend)
    if not st:
        raise LookupError('no `session_top:` label inside main')
    lw = re.compile(r'^   while \(g_running\)\s*\n\s*\{', re.M).search(clean, st.end(), mend)
    if not lw:
        raise LookupError('no `while (g_running)` after session_top: inside main')
    lc = _close(clean, lw.start())
    tail = lc
    # the old tail rule: blank lines and LAB_FFP(...) lines, then `if (g_perf_rig) {...}`
    rest = clean[lc + 1:]
    t = re.match(r'(?:[ \t]*\n|[ \t]*LAB_FFP\([^\n]*\n)*([ \t]*)if \(g_perf_rig\)\s*\{', rest)
    if t:
        tail = _close(clean, lc + 1 + t.end() - 1)
    cl = clean.split('\n')
    code = lambda a, b: frozenset(i for i in range(a, b + 1) if 0 < i <= len(cl) and cl[i - 1].strip())
    return dict(main=lineof(m.start()), session_top=lineof(st.start()), loop0=lineof(lw.start()),
                loop_close=lineof(lc), tail_end=lineof(tail), lo=lineof(lw.start()), hi=lineof(tail),
                code=code)


def _git_show(commit, path):
    for cmd in (['git', '-C', REPO, 'show', '%s:%s' % (commit, path)],):
        try:
            r = subprocess.run(cmd, capture_output=True)
            if r.returncode == 0:
                return r.stdout.decode('utf-8', 'replace').replace('\r\n', '\n')
        except OSError:
            pass
    # WSL on a Windows worktree: plain git cannot follow the .git file
    try:
        w = subprocess.run(['wslpath', '-w', REPO], capture_output=True, text=True).stdout.strip()
        if w:
            r = subprocess.run(['git.exe', '-C', w, 'show', '%s:%s' % (commit, path)], capture_output=True)
            if r.returncode == 0:
                return r.stdout.decode('utf-8', 'replace').replace('\r\n', '\n')
    except OSError:
        pass
    return None


def source_for(elf_path, main_c=None):
    """-> (text, where) of the main_psp.c the ELF was (claimed to be) built from."""
    if main_c:
        return open(main_c, encoding='utf-8', errors='replace').read().replace('\r\n', '\n'), main_c
    fj = os.path.join(os.path.dirname(os.path.abspath(elf_path)), 'flavour.json')
    if os.path.exists(fj):
        try:
            c = json.load(open(fj)).get('sourceCommit')
        except ValueError:
            c = None
        if c:
            t = _git_show(c, 'gpsp/psp/main_psp.c')
            if t is not None:
                return t, '%s:gpsp/psp/main_psp.c' % c[:10]
    return open(TREE_MAIN, encoding='utf-8', errors='replace').read().replace('\r\n', '\n'), TREE_MAIN


def main_decl_line(elf_path, func='main', srcfile='main_psp.c'):
    sys.path.insert(0, HERE)
    from elf32 import Elf
    import ef_dwarf as D
    cu = D.Info(Elf(elf_path)).cu_named(srcfile)
    sp = [s for s in cu.subprograms() if s['name'] == func and s['concrete']]
    return sp[0]['decl_line'] if sp else None


def for_elf(elf_path, main_c=None, decl_line=None):
    """resolve the window for ELF; LookupError when the source is not the ELF's."""
    text, where = source_for(elf_path, main_c)
    r = resolve(text)
    dl = decl_line if decl_line is not None else main_decl_line(elf_path)
    if dl != r['main']:
        raise LookupError('source %s has `int main(` at line %s but the ELF says main is at '
                          'line %s (DW_AT_decl_line): not the source this ELF was built from '
                          '(pass --main-c)' % (where, r['main'], dl))
    r['where'] = where
    # a split shell (P4): where each fragment is #included, so a tool can order a
    # fragment's definitions against main (loop_diff's "defined before main")
    r['includes'] = {}
    for k, ln in enumerate(text.split('\n'), 1):
        m = re.match(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+\.inc\.c)"', ln)
        if m:
            r['includes'][m.group(1).split('/')[-1]] = k
    return r


def describe(r):
    return ('loop window by anchor: lines %d-%d (while (g_running) L:%d after session_top: L:%d, '
            'loop closes L:%d, tail to L:%d; main at L:%d; source %s)'
            % (r['lo'], r['hi'], r['loop0'], r['session_top'], r['loop_close'], r['tail_end'],
               r['main'], r.get('where', '?')))


def check_explicit(r, lo, hi, tool):
    """an explicit --lines range must be the anchored one (the old hard-coded
    window run on another tree fails loudly instead of measuring other code)."""
    same = (lo, hi) == (r['lo'], r['hi']) or ('code' in r and r['code'](lo, hi) == r['code'](r['lo'], r['hi']))
    if not same:
        raise LookupError('%s: explicit loop lines %d-%d are not the anchored window %d-%d of this '
                          'ELF\'s source (%s); a hard-coded window measures other code on this tree'
                          % (tool, lo, hi, r['lo'], r['hi'], r.get('where', '?')))


def selftest():
    bad = 0

    def t(c, m):
        nonlocal bad
        print('  %s  %s' % ('ok  ' if c else 'FAIL', m)); bad += 0 if c else 1
    # x4's 16368-18090 on 01067e74 ends one line past the tail block's brace (18089) on a
    # comment line: the same code lines, so --lines 16368-18090 is accepted there
    want = {'01067e74': (13837, 16368, 18089), '7ffe4236': (14109, 16655, 18372),
            'fbe9da63': (14132, 16678, 18401)}
    for c, (mn, lo, hi) in want.items():
        txt = _git_show(c, 'gpsp/psp/main_psp.c')
        if txt is None:
            print('  skip  %s not in this repo' % c); continue
        r = resolve(txt)
        if c == '01067e74':
            try:
                check_explicit(r, 16368, 18090, 'selftest'); t(True, '01067e74: x4 16368-18090 = the anchored code lines')
            except LookupError as ex:
                t(False, str(ex))
        t((r['main'], r['lo'], r['hi']) == (mn, lo, hi),
          '%s: main L:%d, window %d-%d (want %d, %d-%d)' % (c, r['main'], r['lo'], r['hi'], mn, lo, hi))
    # mutants
    base = 'int main(int a)\n{\nsession_top:\n   x();\n   while (g_running)\n   {\n      if (a) { b("}"); }\n   }\n' \
           '   LAB_FFP(f();)\n\n   if (g_perf_rig)\n   {\n      /* } */ y();\n   }\n   z();\n}\n'
    r = resolve(base)
    t((r['lo'], r['loop_close'], r['hi']) == (5, 8, 14), 'toy: braces in strings/comments ignored, tail rule (%d-%d)' % (r['lo'], r['hi']))
    try:
        resolve(base.replace('session_top:', 'top:')); t(False, 'mutant: no session_top must raise')
    except LookupError:
        t(True, 'mutant: no session_top: label -> LookupError')
    r2 = resolve(base.replace('if (g_perf_rig)', 'if (other)'))
    t(r2['hi'] == r2['loop_close'] == 8, 'mutant: no perf_rig tail block -> window ends at the loop brace')
    try:
        check_explicit(dict(lo=16678, hi=18402), 16368, 18090, 'x'); t(False, 'old window accepted')
    except LookupError:
        t(True, 'the old hard-coded 16368-18090 on fbe9da63 fails loudly')
    print('loop_anchor selftest: %s' % ('PASS' if not bad else '%d FAILED' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['--selftest']:
        sys.exit(selftest())
    if a[:1] == ['--elf']:
        mc = a[a.index('--main-c') + 1] if '--main-c' in a else None
        try:
            print(describe(for_elf(a[1], mc)))
        except LookupError as ex:
            print('loop_anchor: %s' % ex); sys.exit(2)
        sys.exit(0)
    if len(a) == 1:
        r = resolve(open(a[0], encoding='utf-8', errors='replace').read().replace('\r\n', '\n'))
        print(describe(r)); sys.exit(0)
    print(__doc__); sys.exit(2)
