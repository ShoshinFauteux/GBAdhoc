#!/usr/bin/env python3
"""epochc_exitmap.py -- the Epoch C exit inventory of a split main_psp.c tree.

Planning aid for builds/epochc-out/PLAN-EPOCHC-beta5.md (Epoch C, step C0).  It does NOT
replace T8 (exit_matrix.py, gate XS); it reuses T8's raw view for families A-C and adds
what T8 does not list as rows: the loop triggers (family D: every `g_running = 0` write
and the lab wall limit), the four teardown terminals, the exit macros' expansion count
and the out-of-band site, each with fragment:line, enclosing function, `#if` context and
the nearest reason/code.  Run it on each tree Epoch C starts from (the beta tip after
HW-1, HW-2 and the features land) and diff the TSVs: a new or vanished row is a plan
change, a moved line is not.

Usage:  python3 -I epochc_exitmap.py GPSP_ROOT OUT.tsv
        (GPSP_ROOT is the directory holding psp/ and tools/refactor/)
Exit:   0 ok; 2 usage or a tree T8's raw view cannot parse.
"""
import os
import re
import sys

ID_RE = re.compile(r'^(?:static\s+|inline\s+|volatile\s+)*[A-Za-z_][\w\s\*]*\b([A-Za-z_]\w*)\s*\(')
IF_RE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$')
TRIG_RE = re.compile(r'\bg_running\s*=\s*0\b|\blab_frame\s*\(')
REASON_RE = re.compile(r'exit_reason\s*=\s*("[^"]*"|[A-Za-z_]\w*)|\*why\s*=\s*("[^"]*")|reason=([a-z_]+)')
CODE_RE = re.compile(r'exit_code\s*=\s*([^;]+);|\*code\s*=\s*([^;]+);')
RELAUNCH_RE = re.compile(r'g_relaunch\s*=\s*(RELAUNCH_\w+)')


def tu_files(psp):
    """The shell and its fragments in TU (include) order."""
    shell = os.path.join(psp, 'main_psp.c')
    out = []
    with open(shell, encoding='utf-8', errors='replace') as f:
        lines = f.read().split('\n')
    out.append(('main_psp.c', lines))
    for ln in lines:
        m = re.match(r'\s*#\s*include\s+"(fe/[\w.]+\.inc\.c)"', ln)
        if m:
            with open(os.path.join(psp, m.group(1)), encoding='utf-8', errors='replace') as f:
                out.append((m.group(1), f.read().split('\n')))
    return out


def annotate(lines):
    """Per line: (#if context string, enclosing function)."""
    stack, ctx, fn, cur = [], [], [], ''
    for i, ln in enumerate(lines):
        m = IF_RE.match(ln)
        if m:
            k, rest = m.group(1), m.group(2).strip()
            rest = re.sub(r'/\*.*?\*/', '', rest).strip()
            if k in ('if', 'ifdef', 'ifndef'):
                stack.append(('!' if k == 'ifndef' else '') + (rest if k != 'if' else '(' + rest + ')'))
            elif k == 'elif' and stack:
                stack[-1] = '(' + rest + ')'
            elif k == 'else' and stack:
                stack[-1] = 'else:' + stack[-1]
            elif k == 'endif' and stack:
                stack.pop()
        if ln and not ln[0].isspace() and not ln.startswith(('#', '/', '*', '}', '{')) \
                and '(' in ln and not ln.rstrip().endswith(';') and '=' not in ln.split('(')[0]:
            mm = ID_RE.match(ln)
            if mm:
                cur = mm.group(1)
        ctx.append(' > '.join(stack) or '-')
        fn.append(cur)
    return ctx, fn


def near(lines, i, rx, back=3, fwd=1):
    for j in list(range(i, min(len(lines), i + fwd + 1))) + list(range(i - 1, max(-1, i - back - 1), -1)):
        m = rx.search(lines[j])
        if m:
            return next(g for g in m.groups() if g is not None).strip()
    return ''


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    root, outp = argv[1], argv[2]
    psp = os.path.join(root, 'psp')
    rows = []
    # families A-C (and T8's out-of-band row) from T8's raw view
    sys.path.insert(0, os.path.join(root, 'tools', 'refactor'))
    try:
        import exit_matrix as E
        import aec_cparse as C
        raw = E.View('raw', C.load_raw(os.path.join(psp, 'main_psp.c'), follow=True, alias=E.EXIT_ALIAS))
        st = E.write_view(raw, os.path.join(os.path.dirname(os.path.abspath(outp)), 't8raw'), 'raw')[0]
    except Exception as e:  # noqa: BLE001
        sys.stderr.write('T8 raw view failed: %r\n' % (e,))
        return 2
    files = tu_files(psp)
    ann = {fp: annotate(ls) for fp, ls in files}
    for r in st:
        rows.append(('A-C' if not r['id'].startswith('E:') else 'E', r['id'], r['func'],
                     str(r['line']) if r['func'] == 'main' else r['line'], r['code'], r['why'],
                     r['order'], r['flags'], r['seq'], '', ''))
    # family D triggers, the terminals, the macro sites
    n = 0
    for fp, ls in files:
        ctx, fn = ann[fp]
        for i, ln in enumerate(ls):
            code = ln.split('//')[0]
            if code.lstrip().startswith(('*', '/*')):
                continue
            loc = '%s:%d' % ('main_psp.c' if fp == 'main_psp.c' else fp, i + 1)
            if TRIG_RE.search(code) and 'volatile int g_running' not in code:
                n += 1
                rows.append(('D-trigger', 'T%02d' % n, fn[i], loc, near(ls, i, CODE_RE),
                             near(ls, i, REASON_RE), near(ls, i, RELAUNCH_RE, 8, 2) or '0', '', '',
                             ctx[i], ln.strip()[:90]))
            for pat, tid in ((r'goto\s+session_top', 'T-inproc'),
                             (r'relaunch_eboot\(g_relaunch', 'T-relaunch(refused|profile)'),
                             (r'relaunch_eboot\(0\)', 'B8-relaunch')):
                if re.search(pat, code):
                    rows.append(('terminal', tid, fn[i], loc, '', '', '', '', '', ctx[i], ln.strip()[:90]))
            for mac in ('FE_EXIT_GAME()', 'FE_HANDOFF('):
                if mac in code and '#define' not in code:
                    rows.append(('macro', mac.rstrip('(').rstrip('()'), fn[i], loc, '', '', '', '', '',
                                 ctx[i], ''))
    hdr = ('kind', 'id', 'function', 'at', 'code', 'why', 'order|relaunch', 'flags', 'seq',
           'if_context', 'text')
    with open(outp, 'w', newline='\n', encoding='utf-8') as f:
        f.write('\t'.join(hdr) + '\n')
        for r in rows:
            f.write('\t'.join(str(x).replace('\t', ' ') for x in r) + '\n')
    kinds = {}
    for r in rows:
        kinds[r[0]] = kinds.get(r[0], 0) + 1
    print('rows:', ', '.join('%s %d' % kv for kv in sorted(kinds.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
