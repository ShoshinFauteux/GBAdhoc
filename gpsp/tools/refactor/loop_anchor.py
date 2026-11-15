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
  2. --tree DIR (explicit): DIR/psp/main_psp.c of the source tree the ELF was
     built from (a gpsp/ dir, or a checkout or export holding gpsp/); the
     callers' own source-tree options (layout_report.py --src / --ref-src,
     loop_lpsim.py --tree / --tree-b) pass it here (P-tidy F3: they used to
     read the window from 3./4. whatever tree they were told);
  3. the ELF's flavours.sh record: <elf dir>/flavour.json sourceCommit ->
     `git show <commit>:gpsp/psp/main_psp.c` in the repo these tools live in;
  4. this tool's own tree's psp/main_psp.c, named as such in `where`.
and then CHECKED: main's DW_AT_decl_line in the ELF must equal the source's
`int main(` line, else LookupError (a stale or wrong source would silently
measure other code; that is how the hard-coded windows failed).

FUNCTION MODE (tier C, C-1 commit 1; health-out/design/C-1.md, CRITIQUE C-1): once C-1
commit 4 lands, the loop is the file-scope function `frame_loop` (main_psp.c; later moved
whole, as an ordinary split unit, into fe/loop.inc.c).  When the source DEFINES
`frame_loop(` -- or the ELF has a concrete `frame_loop` -- the window is that function:
lines from its definition line to its closing brace, in the file that defines it
(r['linefile']: main_psp.c, or the fragment's basename), r['func'] = 'frame_loop', and the
check is frame_loop's DW_AT_decl_line (and decl_file) against the source.  Every caller
takes func/linefile from the result (loop_diff, loop_lpsim, layout_report).  Both shapes
at once -- frame_loop defined AND main still holding `while (g_running)`, or an ELF with
frame_loop whose source has none (or the reverse) -- is a LookupError: a tool must never
silently measure the old window.

    loop_anchor.py MAIN_C            print the resolved lines of a source file
    loop_anchor.py --elf ELF [--main-c P | --tree DIR]   resolve for an ELF (with the check)
    loop_anchor.py --selftest
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# FLAV_REPO: the repo when these tools run from a copy (run_all on a tree export),
# as flavours.sh and tests/p0a_b5.sh take it
REPO = os.environ.get('FLAV_REPO') or os.path.normpath(os.path.join(HERE, '..', '..', '..'))
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


FUNC = 'frame_loop'


def find_def(text, name=FUNC):
    """-> (first_line, close_line, name_line) of the file-scope DEFINITION of NAME (brace
    depth 0, a body `{` before the next `;`: a prototype has none), else None.  name_line
    is what DW_AT_decl_line records."""
    clean = clean_c(text)
    lineof = lambda off: text.count('\n', 0, off) + 1
    for mt in re.finditer(r'\b%s\s*\(' % re.escape(name), clean):
        p = mt.start()
        ls = clean.rfind('\n', 0, p) + 1
        # brace depth 0 here (not a call inside a function)
        if clean.count('{', 0, p) != clean.count('}', 0, p):
            continue
        d, j = 0, mt.end() - 1
        while j < len(clean):
            if clean[j] == '(':
                d += 1
            elif clean[j] == ')':
                d -= 1
                if d == 0:
                    break
            j += 1
        k = j + 1
        while k < len(clean) and clean[k] not in ';{':
            k += 1
        if k >= len(clean) or clean[k] != '{':
            continue                    # a prototype
        # the definition's first line: back over the declaration's own lines (attributes,
        # return type) to the line after the previous `;`, `}` or preprocessor line
        st = ls
        while st > 0:
            prev = clean.rfind('\n', 0, st - 1) + 1
            pl = clean[prev:st - 1].strip()
            if not pl or pl.endswith((';', '}', '{')) or pl.startswith('#'):
                break
            st = prev
        return lineof(st), lineof(_close(clean, k)), lineof(p)
    return None


def resolve_fn(text, where_file='main_psp.c'):
    """function mode: the window is frame_loop's definition through its closing brace."""
    fd = find_def(text)
    if fd is None:
        raise LookupError('no file-scope definition of %s() in %s' % (FUNC, where_file))
    first, close, nameline = fd
    cl = clean_c(text).split('\n')
    code = lambda a, b: frozenset(i for i in range(a, b + 1) if 0 < i <= len(cl) and cl[i - 1].strip())
    return dict(func=FUNC, linefile=where_file, fn_line=nameline, lo=first, hi=close, loop0=first,
                loop_close=close, tail_end=close, session_top=None, code=code)


def has_main_loop(text):
    """does main() still hold the `while (g_running)` loop (the pre-C-1 shape)?"""
    try:
        resolve(text)
        return True
    except LookupError:
        return False


def resolve(text):
    """-> dict(main, session_top, loop0, loop_close, tail_end, lo, hi) (1-based lines)."""
    clean = clean_c(text)
    lineof = lambda off: text.count('\n', 0, off) + 1
    m = re.compile(r'^int main\s*\(', re.M).search(clean)
    if not m:
        raise LookupError('no `int main(` definition')
    mend = _close(clean, m.end())
    # Epoch C C3-2b: the pass per game `   for (;;)` stands where `session_top:` stood
    st = re.compile(r'^(?:session_top:|   for \(;;\)\s*\n\s*\{)', re.M).search(clean, m.end(), mend)
    if not st:
        raise LookupError('no `session_top:` label (or pass `for (;;)`) inside main')
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
    # WSL on a Windows worktree: plain git cannot follow the .git file.  git.exe through
    # WSL interop fails transiently under load (2026-11-07): retried, like flavours.sh
    import time
    try:
        w = subprocess.run(['wslpath', '-w', REPO], capture_output=True, text=True).stdout.strip()
        for _ in range(6 if w else 0):
            r = subprocess.run(['git.exe', '-C', w, 'show', '%s:%s' % (commit, path)], capture_output=True)
            if r.returncode == 0:
                return r.stdout.decode('utf-8', 'replace').replace('\r\n', '\n')
            time.sleep(5)
    except OSError:
        pass
    return None


def tree_main(tree):
    """psp/main_psp.c of a source tree: TREE is a gpsp/ dir (TREE/psp/main_psp.c) or a checkout
    or export that holds one (TREE/gpsp/psp/main_psp.c).  LookupError when it has neither."""
    for p in (os.path.join(tree, 'psp', 'main_psp.c'), os.path.join(tree, 'gpsp', 'psp', 'main_psp.c')):
        if os.path.isfile(p):
            return p
    raise LookupError('source tree %s has no psp/main_psp.c (nor gpsp/psp/main_psp.c): pass the gpsp/ '
                      'tree the ELF was built from' % tree)


def source_for(elf_path, main_c=None, tree=None):
    """-> (text, where) of the main_psp.c the ELF was (claimed to be) built from."""
    if main_c:
        return open(main_c, encoding='utf-8', errors='replace').read().replace('\r\n', '\n'), main_c
    if tree:
        p = tree_main(tree)
        return open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n'), p
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
    return (open(TREE_MAIN, encoding='utf-8', errors='replace').read().replace('\r\n', '\n'),
            '%s (this tool\'s own tree: no --main-c/--tree, and no flavour.json commit this repo shows)' % TREE_MAIN)


def main_decl_line(elf_path, func='main', srcfile='main_psp.c'):
    sys.path.insert(0, HERE)
    from elf32 import Elf
    import ef_dwarf as D
    cu = D.Info(Elf(elf_path)).cu_named(srcfile)
    sp = [s for s in cu.subprograms() if s['name'] == func and s['concrete']]
    return sp[0]['decl_line'] if sp else None


def source_reader(elf_path, main_c=None, tree=None):
    """-> read(rel): the text of psp/REL (e.g. 'fe/loop.inc.c') from the same source
    source_for reads main_psp.c from (a path's directory, or the flavour.json commit)."""
    if main_c:
        base = os.path.dirname(os.path.abspath(main_c))
    elif tree:
        base = os.path.dirname(tree_main(tree))
    else:
        base = None
        fj = os.path.join(os.path.dirname(os.path.abspath(elf_path)), 'flavour.json')
        if os.path.exists(fj):
            try:
                c = json.load(open(fj)).get('sourceCommit')
            except ValueError:
                c = None
            if c and _git_show(c, 'gpsp/psp/main_psp.c') is not None:
                return lambda rel: _git_show(c, 'gpsp/psp/' + rel)
        if base is None:
            base = os.path.dirname(TREE_MAIN)

    def rd(rel):
        p = os.path.join(base, rel)
        return open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n') if os.path.isfile(p) else None
    return rd


def func_decl(elf_path, func=FUNC, srcfile='main_psp.c'):
    """-> (decl_line, decl_file basename) of a concrete FUNC in main_psp.o's CU, else None."""
    sys.path.insert(0, HERE)
    from elf32 import Elf
    import ef_dwarf as D
    cu = D.Info(Elf(elf_path)).cu_named(srcfile)
    sp = [s for s in cu.subprograms() if s['name'] == func and s['concrete']]
    return (sp[0]['decl_line'], os.path.basename(sp[0]['decl_file'] or '')) if sp else None


def _includes(text):
    inc = {}
    for k, ln in enumerate(text.split('\n'), 1):
        m = re.match(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+\.inc\.c)"', ln)
        if m:
            inc[m.group(1).split('/')[-1]] = (k, m.group(1))
    return inc


def resolve_tree(text, read=None, where='main_psp.c'):
    """-> the window of a source tree, in whichever shape it has (see FUNCTION MODE):
    function mode when main_psp.c or one of its fragments defines frame_loop, else main's
    anchored loop.  LookupError for both shapes at once."""
    inc = _includes(text)
    fn_text, fn_file = None, None
    if find_def(text) is not None:
        fn_text, fn_file = text, 'main_psp.c'
    elif read is not None:
        # module 18's fragments only (fe/loop*.inc.c: where C-1 moves frame_loop whole);
        # reading all 44 through `git show` per ELF would cost seconds for nothing
        for base, (_, rel) in inc.items():
            if not base.startswith('loop'):
                continue
            ft = read(rel)
            if ft is not None and find_def(ft) is not None:
                fn_text, fn_file = ft, base
                break
    old = has_main_loop(text)
    if fn_text is not None and old:
        raise LookupError('%s defines %s() (in %s) AND main() still holds `while (g_running)`: both '
                          'loop shapes at once; refusing to pick a window' % (where, FUNC, fn_file))
    if fn_text is not None:
        r = resolve_fn(fn_text, fn_file)
        mm = re.compile(r'^int main\s*\(', re.M).search(clean_c(text))
        r['main'] = text.count('\n', 0, mm.start()) + 1 if mm else None
    else:
        r = resolve(text)
        r.update(func='main', linefile='main_psp.c')
    r['includes'] = {b: k for b, (k, _) in inc.items()}
    return r


def for_elf(elf_path, main_c=None, decl_line=None, tree=None, fn_decl='elf'):
    """resolve the window for ELF; LookupError when the source is not the ELF's.
    main_c / tree: the explicit source (a main_psp.c, or the gpsp/ tree holding it).
    fn_decl: frame_loop's (decl_line, decl_file) in the ELF, or None for an ELF without
    it; 'elf' (default) reads it from the ELF's DWARF."""
    text, where = source_for(elf_path, main_c, tree)
    r = resolve_tree(text, source_reader(elf_path, main_c, tree), where)
    fd = func_decl(elf_path) if fn_decl == 'elf' else fn_decl
    if r['func'] == FUNC:
        if fd is None:
            raise LookupError('source %s defines %s() but the ELF has no %s: not the source this ELF was '
                              'built from (pass --main-c / --tree)' % (where, FUNC, FUNC))
        if (fd[0], fd[1]) != (r['fn_line'], r['linefile']):
            raise LookupError('source %s has %s() at %s:%s but the ELF says %s:%s (DW_AT_decl_line/file): '
                              'not the source this ELF was built from' % (where, FUNC, r['linefile'],
                                                                          r['fn_line'], fd[1], fd[0]))
    else:
        if fd is not None:
            raise LookupError('the ELF has %s() (%s:%s) but source %s still holds the loop in main(): '
                              'not the source this ELF was built from' % (FUNC, fd[1], fd[0], where))
        dl = decl_line if decl_line is not None else main_decl_line(elf_path)
        if dl != r['main']:
            raise LookupError('source %s has `int main(` at line %s but the ELF says main is at '
                              'line %s (DW_AT_decl_line): not the source this ELF was built from '
                              '(pass --main-c)' % (where, r['main'], dl))
    r['where'] = where
    return r


def describe(r):
    if r.get('func') == FUNC:
        return ('loop window by function: %s() lines %d-%d of %s (decl L:%d; main at L:%s; source %s)'
                % (FUNC, r['lo'], r['hi'], r['linefile'], r['fn_line'], r.get('main'), r.get('where', '?')))
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
    r3 = resolve(base.replace('session_top:\n', '   for (;;)\n   {\n').replace('   z();\n', '   }\n   z();\n'))
    t((r3['session_top'], r3['lo'], r3['hi']) == (3, 6, 15),
      'toy (Epoch C C3-2b): the pass `for (;;)` stands for session_top: (%d-%d)' % (r3['lo'], r3['hi']))
    r2 = resolve(base.replace('if (g_perf_rig)', 'if (other)'))
    t(r2['hi'] == r2['loop_close'] == 8, 'mutant: no perf_rig tail block -> window ends at the loop brace')
    try:
        check_explicit(dict(lo=16678, hi=18402), 16368, 18090, 'x'); t(False, 'old window accepted')
    except LookupError:
        t(True, 'the old hard-coded 16368-18090 on fbe9da63 fails loudly')
    # --tree (P-tidy F3): the source is read from the tree given, in either layout, before
    # any flavour.json commit or this tool's own tree; a tree without main_psp.c is refused
    import shutil
    import tempfile
    d = tempfile.mkdtemp(prefix='loop_anchor.')
    try:
        for lay in ('psp', os.path.join('gpsp', 'psp')):
            os.makedirs(os.path.join(d, lay), exist_ok=True)
        open(os.path.join(d, 'gpsp', 'psp', 'main_psp.c'), 'w').write(base)
        txt, where = source_for(os.path.join(d, 'none.elf'), tree=d)
        t(txt == base and where == os.path.join(d, 'gpsp', 'psp', 'main_psp.c'),
          '--tree CHECKOUT (holding gpsp/): its gpsp/psp/main_psp.c is read')
        open(os.path.join(d, 'gpsp', 'psp', 'main_psp.c'), 'w').write('/* shifted */\n' + base)
        txt, where = source_for(os.path.join(d, 'none.elf'), tree=os.path.join(d, 'gpsp'))
        t(resolve(txt)['main'] == 2 and where == os.path.join(d, 'gpsp', 'psp', 'main_psp.c'),
          '--tree GPSP_DIR: its psp/main_psp.c is read (main at L:%d)' % resolve(txt)['main'])
        try:
            source_for(os.path.join(d, 'none.elf'), tree=os.path.join(d, 'psp')); t(False, 'a tree without main_psp.c accepted')
        except LookupError:
            t(True, 'mutant: a --tree without psp/main_psp.c -> LookupError')
        # FUNCTION MODE (C-1 commit 1): the loop as the file-scope function frame_loop
        fl = ('static __attribute__((noinline, noclone, section(".text.startup.fe_frame_loop")))\n'
              'void frame_loop(loop_t *L, const session_ctx *S)\n{\n   int exit_code = L->exit_code;\n'
              '   while (g_running)\n   {\n      if (S->a) { b("}"); }\n   }\n   L->exit_code = exit_code;\n}\n')
        proto = 'static void frame_loop(loop_t *L, const session_ctx *S);\n'
        newmain = 'int main(int a)\n{\n   for (;;)\n   {\n      frame_loop(&L, &S);\n   }\n   z();\n}\n'
        src = '/* c1 */\n' + proto + fl + newmain
        mc = os.path.join(d, 'gpsp', 'psp', 'main_psp.c')
        open(mc, 'w').write(src)
        r = resolve_tree(src)
        t((r['func'], r['linefile'], r['lo'], r['fn_line'], r['hi'], r['main']) == ('frame_loop', 'main_psp.c', 3, 4, 12, 13),
          'F1 frame_loop in main_psp.c: window = its definition (L:%s-%s, decl L:%s), a prototype ignored'
          % (r['lo'], r['hi'], r['fn_line']))
        r = for_elf(os.path.join(d, 'none.elf'), main_c=mc, fn_decl=(4, 'main_psp.c'))
        t(r['func'] == 'frame_loop' and 'by function' in describe(r), 'F2 for_elf: decl line/file match -> function mode')
        for fd, why in ((None, 'the ELF has no frame_loop'), ((5, 'main_psp.c'), 'decl line differs'),
                        ((4, 'loop.inc.c'), 'decl file differs')):
            try:
                for_elf(os.path.join(d, 'none.elf'), main_c=mc, fn_decl=fd); t(False, 'F3 accepted: ' + why)
            except LookupError:
                t(True, 'F3 mutant: %s -> LookupError' % why)
        open(mc, 'w').write(base)
        try:
            for_elf(os.path.join(d, 'none.elf'), main_c=mc, decl_line=1, fn_decl=(4, 'main_psp.c'))
            t(False, 'F4 an ELF with frame_loop measured with main\'s old window')
        except LookupError:
            t(True, 'F4 mutant: the ELF has frame_loop, the source still loops in main -> LookupError')
        r = for_elf(os.path.join(d, 'none.elf'), main_c=mc, decl_line=1, fn_decl=None)
        t(r['func'] == 'main' and r['linefile'] == 'main_psp.c' and (r['lo'], r['hi']) == (5, 14),
          'F5 no frame_loop anywhere: the old anchored window, func main')
        try:
            resolve_tree(fl + base); t(False, 'F6 both shapes accepted')
        except LookupError:
            t(True, 'F6 mutant: frame_loop defined AND main still holds while (g_running) -> LookupError')
        os.makedirs(os.path.join(d, 'gpsp', 'psp', 'fe'), exist_ok=True)
        open(os.path.join(d, 'gpsp', 'psp', 'fe', 'loop.inc.c'), 'w').write('/* module 18 */\n' + fl)
        open(mc, 'w').write(proto + '#include "fe/loop.inc.c"\n' + newmain)
        r = for_elf(os.path.join(d, 'none.elf'), tree=os.path.join(d, 'gpsp'), fn_decl=(3, 'loop.inc.c'))
        t((r['func'], r['linefile'], r['lo'], r['hi'], r['includes'].get('loop.inc.c')) == ('frame_loop', 'loop.inc.c', 2, 11, 2),
          'F7 frame_loop moved whole into fe/loop.inc.c: window in the fragment (L:%s-%s)' % (r['lo'], r['hi']))
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print('loop_anchor selftest: %s' % ('PASS' if not bad else '%d FAILED' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['--selftest']:
        sys.exit(selftest())
    if a[:1] == ['--elf']:
        mc = a[a.index('--main-c') + 1] if '--main-c' in a else None
        tr = a[a.index('--tree') + 1] if '--tree' in a else None
        try:
            print(describe(for_elf(a[1], mc, tree=tr)))
        except LookupError as ex:
            print('loop_anchor: %s' % ex); sys.exit(2)
        sys.exit(0)
    if len(a) == 1:
        r = resolve(open(a[0], encoding='utf-8', errors='replace').read().replace('\r\n', '\n'))
        print(describe(r)); sys.exit(0)
    print(__doc__); sys.exit(2)
