#!/usr/bin/env python3
"""extract.py -- T7 (builds/mainpsp-map/out/PLAN.md §5, phase P1): marker
extraction for the host tests that read psp/main_psp.c (or, after the split,
its fragments) as text.

A region is delimited by two comments:

    /* @extract:begin NAME */  ...text...  /* @extract:end NAME */

extract(source, NAME) returns the text between them, with these rules (so a
region can be marked on its own lines OR inline, and still return exactly the
bytes the old str.index slicing returned):
  * a marker ALONE on its line (only blanks around it) is an own-line marker:
    a begin starts the text at the next line, an end stops it at the start of
    its own line (so the marker line adds nothing);
  * any other marker is inline: the text starts right after the begin comment
    / ends right before the end comment.  plant() refuses a position where an
    inline marker would end up alone on a line (it would read as own-line).
  * any other marker inside the region is removed (a whole marker line when it
    sits alone on its line, else just the comment), so regions may overlap
    or nest.
NAME: [A-Za-z0-9_.:-]+.  Each NAME must have exactly one begin and one end,
begin before end; anything else raises ExtractError (a test then fails loudly
instead of slicing the wrong text).

Library:  extract(src, name), names(src), strip_markers(src), plant(src, {name: (start, end)}),
          read_source(path)  (tests read main_psp.c through this: it expands the split's fragments)
plant() refuses a boundary inside a block comment or a string/char literal.
CLI:
  extract.py NAME [--src FILE ...]           print one region (default psp/main_psp.c; a
                                             split shell's fe/X.inc.c includes are expanded
                                             in place, read_source(); other FILEs appended)
  extract.py --list [--src FILE]             list the region names
  extract.py --check [--src FILE]            validate every marker pair (exit 1 on error)
  extract.py --enumerate [GPSP_DIR] [--gate] the host tests that read main_psp.c as text
                                             (all of GPSP_DIR, recursively; --gate also
                                             lists tools/refactor/, the gate tools)
"""
import os
import re
import sys

MARK = re.compile(r'/\* @extract:(begin|end) ([A-Za-z0-9_.:-]+) \*/')


class ExtractError(Exception):
    pass


def _scan(src):
    """-> list of (kind, name, start, end, line_start, line_end, alone)."""
    out = []
    for m in MARK.finditer(src):
        ls = src.rfind('\n', 0, m.start()) + 1
        le = src.find('\n', m.end())
        le = len(src) if le < 0 else le
        alone = src[ls:m.start()].strip() == '' and src[m.end():le].strip() == ''
        out.append((m.group(1), m.group(2), m.start(), m.end(), ls, le, alone))
    return out


def names(src):
    seen = []
    for k, n, *_ in _scan(src):
        if k == 'begin' and n not in seen:
            seen.append(n)
    return seen


def _pair(src, name, marks=None):
    marks = marks if marks is not None else _scan(src)
    b = [x for x in marks if x[0] == 'begin' and x[1] == name]
    e = [x for x in marks if x[0] == 'end' and x[1] == name]
    if len(b) != 1 or len(e) != 1:
        raise ExtractError('%s: %d begin and %d end markers (need exactly one each)' % (name, len(b), len(e)))
    if e[0][2] < b[0][3]:
        raise ExtractError('%s: end marker before begin marker' % name)
    return b[0], e[0]


def _region(src, b, e):
    # own-line marker (alone on its line): the region starts after its line /
    # ends at the start of its line; inline marker: right after / right before it
    start = (b[5] + 1 if b[5] < len(src) else b[5]) if b[6] else b[3]
    end = e[4] if e[6] else e[2]
    return start, end


def _strip(text):
    out = []
    pos = 0
    for k, n, s, en, ls, le, alone in _scan(text):
        if alone:
            out.append(text[pos:ls])
            pos = le + 1 if le < len(text) else le
        else:
            out.append(text[pos:s])
            pos = en
    out.append(text[pos:])
    return ''.join(out)


def extract(src, name):
    marks = _scan(src)
    b, e = _pair(src, name, marks)
    start, end = _region(src, b, e)
    if end < start:
        raise ExtractError('%s: empty or inverted region' % name)
    # remove the other markers inside [start, end); "alone" is judged on the
    # FULL text (a marker can look alone at the edge of a slice)
    out = []
    pos = start
    for k, n, s, en, ls, le, alone in marks:
        if s < start or en > end:
            continue
        if alone:
            a_, b_ = max(ls, start), min(le + 1, end)
        else:
            a_, b_ = s, en
        if a_ < pos:
            continue
        out.append(src[pos:a_])
        pos = b_
    out.append(src[pos:end])
    return ''.join(out)


def strip_markers(src):
    return _strip(src)


def check(src):
    errs = []
    marks = _scan(src)
    for n in sorted({x[1] for x in marks}):
        try:
            _pair(src, n, marks)
        except ExtractError as ex:
            errs.append(str(ex))
    return errs


def _opaque_spans(src):
    """[(start, end, kind)] of every block comment and string/char literal."""
    out = []
    i, n = 0, len(src)
    tok = re.compile(r'/\*|//|["\']')
    while True:
        m = tok.search(src, i)
        if not m:
            break
        i = m.start()
        if m.group() == '/*':
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append((i, j, 'block comment'))
        elif m.group() == '//':
            j = src.find('\n', i)
            j = n if j < 0 else j
        else:
            q, j = src[i], i + 1
            while j < n and src[j] != q and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            if j < n and src[j] == q:
                j += 1
                out.append((i, j, 'string or char literal'))
        i = max(j, i + 1)
    return out


def _inside_comment_or_literal(src, pos, spans=None):
    """'block comment' / 'string or char literal' when offset pos lies strictly
    inside one (at its first char or just after its last it is outside)."""
    for a, b, kind in (spans if spans is not None else _opaque_spans(src)):
        if a < pos < b:
            return kind
        if a >= pos:
            break
    return None


def plant(src, regions):
    """Insert markers so that extract(result, name) == src[start:end] for every
    {name: (start, end)}.  A boundary at a line start gets an own-line marker,
    anywhere else an inline one.  strip_markers(result) == src."""
    ins = {}   # pos -> list of (order, text)
    for name, (s, e) in regions.items():
        if not (0 <= s <= e <= len(src)):
            raise ExtractError('%s: bad span %r' % (name, (s, e)))
        at_ls = s == 0 or src[s - 1] == '\n'
        ins.setdefault(s, []).append((1, '/* @extract:begin %s */%s' % (name, '\n' if at_ls else '')))
        at_le = e == 0 or src[e - 1] == '\n'
        ins.setdefault(e, []).append((0, '/* @extract:end %s */%s' % (name, '\n' if at_le else '')))
    # a marker inside a block comment closes it early (its own `*/`), and one
    # inside a string literal changes .rodata: refuse both (review 2026-11-05)
    import bisect
    sp = _opaque_spans(src)
    sp_starts = [a for a, _, _ in sp]
    for pos in ins:
        k = bisect.bisect_left(sp_starts, pos) - 1
        why = _inside_comment_or_literal(src, pos, sp[k:k + 1] if k >= 0 else [])
        if why:
            raise ExtractError('cannot plant a marker at offset %d: it is inside a %s' % (pos, why))
    for pos, lst in ins.items():
        inl = [t for _, t in lst if not t.endswith('\n')]
        if inl:
            ls = src.rfind('\n', 0, pos) + 1
            le = src.find('\n', pos)
            le = len(src) if le < 0 else le
            if src[ls:pos].strip() == '' and src[pos:le].strip() == '' and len(inl) == len(lst):
                raise ExtractError('cannot plant an inline marker at offset %d: it would be alone on a blank line' % pos)
    out = []
    last = 0
    for pos in sorted(ins):
        out.append(src[last:pos])
        # ends first (they close regions that stop here), then begins; by name for determinism
        for _, t in sorted(ins[pos]):
            out.append(t)
        last = pos
    out.append(src[last:])
    return ''.join(out)


READ_RE = re.compile(r'main_psp\.c')
# read_source( is this module's own read: a test that reads main_psp.c through its
# @extract markers (P1) is still a reader
CALL_RE = re.compile(r'read_text\(|open\(|read_bytes\(|read_source\(')


# tools/refactor/ holds the gate tools and their self-tests: they read main_psp.c by
# design (through read_source / split.py inverse since P0a) and are not host tests
GATE_DIR = 'tools/refactor'
SELF = 'tools/tests/extract.py'


def _reads_main(text):
    lines = text.split('\n')
    for i, ln in enumerate(lines):
        if READ_RE.search(ln) and not ln.lstrip().startswith('#') and \
                any(CALL_RE.search(x) for x in lines[i:i + 3]):
            return True
    # a path variable read elsewhere: MAIN = ROOT / "psp" / "main_psp.c" ... MAIN.read_text()
    for var in re.findall(r'^\s*([A-Za-z_]\w*)\s*=[^\n]*main_psp\.c', text, re.M):
        if re.search(r'\b%s\s*\.\s*(read_text|read_bytes|open)\(|open\(\s*%s\b|'
                     r'read_source\(\s*(?:str\(\s*)?%s\b' % (var, var, var), text):
            return True
    return False


def enumerate_readers(gpsp, gate=False):
    """Python files that READ main_psp.c (a path mention plus a read call within 2
    lines, read_source() included, or a path variable read elsewhere), found by
    walking ALL of GPSP_DIR recursively (PLAN-P-beta5 P1: a reader added in any
    subdirectory is found).  Paths are relative to GPSP_DIR, '/'-separated, sorted.
    tools/refactor/ (the gate tools) is left out unless gate=True; this file is
    never listed.  C hand-mirrors that only cite main_psp.c in comments are not
    readers."""
    found = []
    for root, dirs, files in os.walk(gpsp):
        dirs[:] = sorted(d for d in dirs if d not in ('.git', '__pycache__'))
        rel = os.path.relpath(root, gpsp).replace('\\', '/')
        rel = '' if rel == '.' else rel + '/'
        if not gate and rel.startswith(GATE_DIR + '/'):
            dirs[:] = []
            continue
        for fn in files:
            if not fn.endswith('.py') or rel + fn == SELF:
                continue
            if _reads_main(open(os.path.join(root, fn), encoding='utf-8', errors='replace').read()):
                found.append(rel + fn)
    return sorted(found)


FRAG_INC = re.compile(r'^#include "fe/([A-Za-z0-9_.]+\.inc\.c)"\n', re.M)


def read_source(path):
    """main_psp.c as ONE text: after the split, the shell's `#include
    "fe/X.inc.c"` lines are expanded in place (split.py inverse), so a region
    is read in source order even when it crosses a fragment cut.  Concatenating
    the files instead (shell, then fe/*.inc.c alphabetically) splices
    unrelated fragments into such a region (review 2026-11-05)."""
    text = open(path, encoding='utf-8', newline='').read()
    base = os.path.dirname(os.path.abspath(path))

    def sub(m):
        p = os.path.join(base, 'fe', m.group(1))
        if not os.path.exists(p):
            raise ExtractError('%s: fragment %s missing' % (path, p))
        return open(p, encoding='utf-8', newline='').read()
    return FRAG_INC.sub(sub, text)


# Epoch C C1c (fe/teardown.inc.c): main()'s shared teardown is two calls.  Their
# parameters carry the names of main()'s locals and their bodies keep main()'s
# indentation, so pasting each body over its call line gives main()'s text as a
# test that reads the teardown expects it.
EPOCHC_TEARDOWN_CALLS = (("   session_end(have_variant);\n", "session_end"),
                         ("   process_end(exit_code, exit_reason);\n", "process_end"))


def _helper_body(source, name):
    m = re.search(r'^static [^\n]*\bvoid ' + name + r'\([^)]*\)\s*\{\n', source, re.M)
    if not m:
        raise ExtractError('epochc: %s() not defined' % name)
    end = source.index('\n}\n', m.end() - 1)
    return source[m.end():end + 1]


# Epoch C C2 (fe/boot.inc.c): main()'s boot is boot_run(), moved unchanged.  Its call
# statement is replaced by its body without the final `return -1;` (the body keeps
# main()'s indentation; its locals are declared at its top under main()'s names).
EPOCHC_BOOT_CALL = re.compile(r'^( +)if \(boot_run\(argc, argv, &bc\) >= 0\)\n +return 0;\n', re.M)


def _boot_body(source):
    m = re.search(r'^static [^\n]*\bint boot_run\([^)]*\)\s*\{\n', source, re.M)
    if not m:
        raise ExtractError('epochc: boot_run() not defined')
    end = source.index('\n}\n', m.end() - 1)
    body = source[m.end():end + 1]
    tail = '   return -1;\n'
    if not body.endswith(tail):
        raise ExtractError('epochc: boot_run() does not end in `return -1;`')
    return body[:-len(tail)]


def epochc_expand(main_text, source):
    """main()'s text with boot_run() and session_end()/process_end() calls replaced by
    their bodies (unchanged on a tree before Epoch C C1c / C2)."""
    if len(EPOCHC_BOOT_CALL.findall(main_text)) == 1:
        body = _boot_body(source)
        main_text = EPOCHC_BOOT_CALL.sub(lambda m: body, main_text)
    for call, name in EPOCHC_TEARDOWN_CALLS:
        if main_text.count(call) == 1:
            main_text = main_text.replace(call, _helper_body(source, name))
    m = EPOCHC_RESET_CALL.search(main_text)
    if m:
        main_text = main_text[:m.start()] + _reset_body(source, m.group(1)) + main_text[m.end():]
    return main_text


# Epoch C C3-1 (fe/session.inc.c): the in-process return's reset list is
# session_reset(&local.., boot values..).  Pasted back with `*name` read as `name`
# (each parameter carries the name of the main() local it is given) and main()'s
# indentation at the call.
EPOCHC_RESET_CALL = re.compile(r'^( +)session_reset\([^;]*\);\n', re.M)


def _reset_body(source, indent):
    m = re.search(r'^static [^\n]*\bvoid session_reset\(([^)]*)\)\s*\{\n', source, re.M)
    if not m:
        raise ExtractError('epochc: session_reset() not defined')
    ptr = re.findall(r'\*\s*(\w+)\s*(?:,|$)', m.group(1))
    end = source.index('\n}\n', m.end() - 1)
    body = source[m.end():end + 1]
    if ptr:
        body = re.sub(r'\*(%s)\b' % '|'.join(ptr), r'\1', body)
    return ''.join(indent + ln[3:] + '\n' if ln.startswith('   ') else ln + '\n'
                   for ln in body.split('\n')[:-1])


def epochc_early_helpers(source):
    """The XE_ flags and exit_early_begin/_end (Epoch C C1a/C1b), '' before them."""
    a = source.find('enum\n{\n   XE_EVT_FIRST')
    if a < 0:
        return ''
    m = re.search(r'^static [^\n]*\bvoid exit_early_end\([^)]*\)\s*\{\n', source[a:], re.M)
    if not m:
        raise ExtractError('epochc: exit_early_end() not defined')
    end = source.index('\n}\n', a + m.end() - 1)
    return source[a:end + 3]


def _read_sources(paths):
    """Each path through read_source; a fragment a shell already expanded is
    not added a second time."""
    done, out = set(), []
    for p in paths:
        ap = os.path.normcase(os.path.abspath(p))
        if ap in done:
            continue
        t = open(p, encoding='utf-8', newline='').read()
        for m in FRAG_INC.finditer(t):
            done.add(os.path.normcase(os.path.join(os.path.dirname(os.path.abspath(p)), 'fe', m.group(1))))
        out.append(read_source(p))
        done.add(ap)
    return ''.join(out)


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    gpsp = os.path.normpath(os.path.join(here, '..', '..'))
    src_files = []
    args = list(argv)
    if '--src' in args:
        i = args.index('--src')
        src_files = args[i + 1:]
        args = args[:i]
    if not src_files:
        src_files = [os.path.join(gpsp, 'psp', 'main_psp.c')]
    if args and args[0] == '--enumerate':
        gate = '--gate' in args
        rest = [x for x in args[1:] if x != '--gate']
        g = rest[0] if rest else gpsp
        r = enumerate_readers(g, gate)
        print('\n'.join(r))
        print('%d host test(s) read main_psp.c as text%s' % (len(r), ' (with the tools/refactor gate tools)' if gate else ''),
              file=sys.stderr)
        return 0
    src = _read_sources(src_files)
    if args and args[0] == '--list':
        print('\n'.join(names(src)))
        return 0
    if args and args[0] == '--check':
        errs = check(src)
        for e in errs:
            print('ERROR ' + e)
        print('%d region(s), %d error(s)' % (len(names(src)), len(errs)), file=sys.stderr)
        return 1 if errs else 0
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        sys.stdout.write(extract(src, args[0]))
    except ExtractError as e:
        print('extract: ' + str(e), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
