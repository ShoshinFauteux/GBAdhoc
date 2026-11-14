#!/usr/bin/env python3
"""epochc_exitmap.py -- the Epoch C exit inventory of a split main_psp.c tree.

Planning aid for builds/epochc-out/PLAN-EPOCHC-beta5.md (Epoch C, step C0).  It does NOT
replace T8 (exit_matrix.py, gate XS); it reuses T8's raw view for families A-C and adds
what T8 does not list as rows:
  D-trigger    every `g_running = 0` write and the lab wall limit (family D's triggers),
               with the settings save made just before it and any latch set beside it
  D-step       the shared loop teardown, one row per call or guard, with PLAN §3.2's step
               id and the guarding conditions (T8's raw step table)
  terminal     the four teardown terminals
  macro        each use of the exit macros (FE_EXIT_GAME / FE_HANDOFF)
  oob          exits OUTSIDE main_psp.c's unit: every raw sceKernelExitGame() of the other
               psp/*.c files (lab_harness.c's lab_boot / lab_window / lab_relaunch_and_die,
               usb_handoff.c).  They never run pwr_release_all; that is safe only while
               they run before the first pwr_hold (plan §1.2, "out of band")
  browser-ret  why the game list returns non-zero to main (ui_psp.c: ui_browser,
               ui_browser_run, browser_settings): the causes behind sites B8 (relaunch)
               and B9 (no_rom / home), which T8 sees as one site each
each with file:line, enclosing function, `#if` context and the nearest reason/code.
Families A-C also carry the hold a site exits with and the text of its log lines (T8's
`hold` and `text` columns).

Run it on each tree Epoch C starts from (the beta tip after HW-1, HW-2 and the features
land) and diff the TSVs: a new or vanished row is a plan change, a moved line is not.

Usage:  python3 -I epochc_exitmap.py GPSP_ROOT OUT.tsv
            (GPSP_ROOT is the directory holding psp/ and tools/refactor/)
        python3 -I epochc_exitmap.py --diff OLD.tsv NEW.tsv [-o OUT.txt]
            rows compared by what they are (id, function, statement), never by line
        python3 -I epochc_exitmap.py --selftest
Exit:   0 ok (--diff: 0 also when rows differ; the differences are the output);
        2 usage or a tree T8's raw view cannot parse.
"""
import os
import re
import sys

ID_RE = re.compile(r'^(?:static\s+|inline\s+|volatile\s+)*[A-Za-z_][\w\s\*]*\b([A-Za-z_]\w*)\s*\(')
IF_RE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$')
TRIG_RE = re.compile(r'\bg_running\s*=\s*0\b|\blab_frame\s*\(')
REASON_RE = re.compile(r'exit_reason\s*=\s*("[^"]*"|[A-Za-z_]\w*)|\*why\s*=\s*("[^"]*")'
                       r'|g_link_fatal_why\s*=\s*("[^"]*")|reason=([a-z_]+)')
CODE_RE = re.compile(r'exit_code\s*=\s*([^;]+);|\*code\s*=\s*([^;]+);|g_link_fatal_code\s*=\s*([^;]+);')
SAVE_RE = re.compile(r'\b(pcfg_save|ui_settings_save|ui_settings_flush)\s*\(')
LATCH_RE = re.compile(r'\b(g_exit_req)\s*=\s*1\b')      # HW-2 F1: the sticky HOME latch
RELAUNCH_RE = re.compile(r'g_relaunch\s*=\s*(RELAUNCH_\w+)')
BROWSER_FNS = ('browser_settings', 'ui_browser_run', 'ui_browser')
HDR = ('kind', 'id', 'function', 'at', 'code', 'why', 'order|relaunch', 'flags', 'seq',
       'if_context', 'text', 'hold', 'evt_text')


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


def code_of(ln):
    """the line without its comments (a `//` or a whole `/* .. */` on the line)."""
    return re.sub(r'/\*.*?\*/', ' ', ln).split('//')[0]


def oob_rows(psp):
    """Raw sceKernelExitGame() calls of the psp/*.c files that are not main_psp.c's unit."""
    rows = []
    for fn in sorted(os.listdir(psp)):
        if not fn.endswith('.c') or fn == 'main_psp.c':
            continue
        with open(os.path.join(psp, fn), encoding='utf-8', errors='replace') as f:
            ls = f.read().split('\n')
        ctx, fns = annotate(ls)
        in_c = False
        k = 0
        for i, ln in enumerate(ls):
            s = ln
            if in_c:
                if '*/' not in s:
                    continue
                s = s[s.index('*/') + 2:]
                in_c = False
            c = code_of(s)
            if '/*' in c:
                c, in_c = c[:c.index('/*')], True
            if re.search(r'\bsceKernelExitGame\s*\(', c):
                k += 1
                held = any('pwr_release_all' in x for x in ls[max(0, i - 6):i + 1])
                rows.append(('oob', '%s#%d' % (fn[:-2], k), fns[i], '%s:%d' % (fn, i + 1), '', '', '',
                             'PRA' if held else 'no-PRA', '', ctx[i], ln.strip()[:90], '', ''))
    return rows


def browser_rows(psp):
    """Why the game list hands main a non-zero result (B8: 1 = relaunch; B9: -1)."""
    p = os.path.join(psp, 'ui_psp.c')
    if not os.path.exists(p):
        return []
    with open(p, encoding='utf-8', errors='replace') as f:
        ls = f.read().split('\n')
    ctx, fns = annotate(ls)
    rows, n = [], {}
    for i, ln in enumerate(ls):
        fn = fns[i]
        if fn not in BROWSER_FNS:
            continue
        c = code_of(ln).strip()
        m = re.match(r'^(?:.*\)\s*)?return\s+([^;]+);', c)
        what = None
        if m and m.group(1).strip() not in ('0',):
            what = 'returns ' + m.group(1).strip()
        elif fn == 'browser_settings' and re.search(r'\bwhile\s*\(\s*g_running\b', c):
            what = 'loop ends on !g_running (HOME)'
        elif re.match(r'^while\s*\(\s*g_running\s*\)', c):
            what = 'loop ends on !g_running (HOME)'
        if what is None:
            continue
        # the cause: a warning shown to the player just above (its text), else the `if`
        # that encloses the return; the search stops at another path's `return`
        cause = ''
        if what.startswith('returns'):
            ind = len(ln) - len(ln.lstrip())
            for j in range(i - 1, max(-1, i - 13), -1):
                cj = code_of(ls[j])
                if re.match(r'\s*return\b', cj):
                    break
                m2 = re.search(r'"([^"]+)"\s*,\s*C_WARN', cj)
                if m2:
                    cause = 'after the notice "%s"' % m2.group(1)
                    break
                if cj.strip().startswith('if ') and len(cj) - len(cj.lstrip()) < ind:
                    cause = cj.strip()
                    break
        n[fn] = n.get(fn, 0) + 1
        rows.append(('browser-ret', '%s#%d' % (fn, n[fn]), fn, 'ui_psp.c:%d' % (i + 1), '', what, '',
                     '', '', ctx[i], (cause or c)[:90], '', ''))
    return rows


def build_rows(root, outp):
    psp = os.path.join(root, 'psp')
    rows = []
    # families A-C (and T8's out-of-band row) from T8's raw view
    # T8 from THIS tool tree, whatever tree is read: one tool judges every side of a diff
    # (GPSP_ROOT's own exit_matrix.py may predate the hold and text columns)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import exit_matrix as E
    import aec_cparse as C
    raw = E.View('raw', C.load_raw(os.path.join(psp, 'main_psp.c'), follow=True, alias=E.EXIT_ALIAS))
    st = E.write_view(raw, os.path.join(os.path.dirname(os.path.abspath(outp)), 't8raw'), 'raw')[0]
    files = tu_files(psp)
    ann = {fp: annotate(ls) for fp, ls in files}
    for r in st:
        rows.append(('A-C' if not r['id'].startswith('E:') else 'E', r['id'], r['func'],
                     str(r['line']) if r['func'] == 'main' else r['line'], r['code'], r['why'],
                     r['order'], ','.join(r['flags']), ' '.join(r['seq']), '', '',
                     r.get('hold', ''), getattr(E, 'TEXT_SEP', ' ;; ').join(r.get('text', []))))
    # the shared teardown, step by step (T8's raw step table)
    for k, r in enumerate(E.step_rows(raw)[0]):
        rows.append(('D-step', r['step'], r['phase'], str(r['line']), '', '', '', r['kind'], '',
                     ' && '.join(r['conds'])[:160],
                     ('%s(%s)' % (r['name'], r['args']) if r['kind'] == 'call' else
                      '%s %s' % (r['name'], r['args']))[:120], '', ''))
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
                latch = near(ls, i, LATCH_RE, 2, 2)
                rows.append(('D-trigger', 'T%02d' % n, fn[i], loc, near(ls, i, CODE_RE),
                             near(ls, i, REASON_RE), near(ls, i, RELAUNCH_RE, 8, 2) or '0',
                             'latch:' + latch if latch else '',
                             'saves:' + near(ls, i, SAVE_RE, 6, 0) if near(ls, i, SAVE_RE, 6, 0) else '',
                             ctx[i], ln.strip()[:90], '', ''))
            for pat, tid in ((r'goto\s+session_top|if \(!inproc_try_return\(', 'T-inproc'),
                             (r'relaunch_eboot\(g_relaunch', 'T-relaunch(refused|profile)'),
                             (r'relaunch_eboot\(0\)', 'B8-relaunch')):
                if re.search(pat, code):
                    rows.append(('terminal', tid, fn[i], loc, '', '', '', '', '', ctx[i],
                                 ln.strip()[:90], '', ''))
            for mac in ('FE_EXIT_GAME()', 'FE_HANDOFF('):
                if mac in code and '#define' not in code:
                    rows.append(('macro', mac.rstrip('(').rstrip('()'), fn[i], loc, '', '', '', '', '',
                                 ctx[i], '', '', ''))
    rows += oob_rows(psp)
    rows += browser_rows(psp)
    return rows


# ------------------------------------------------------------------ diff
def load(path):
    with open(path, encoding='utf-8') as f:
        hdr = f.readline().rstrip('\n').split('\t')
        out = []
        for ln in f:
            if ln.strip():
                p = ln.rstrip('\n').split('\t')
                out.append(dict(zip(hdr, p + [''] * (len(hdr) - len(p)))))
        return hdr, out


def row_key(r):
    k = r['kind']
    if k in ('A-C', 'E', 'terminal'):
        return (k, r['id'])
    if k == 'D-trigger':
        # the statement, not its T number (a new trigger renumbers the ones after it)
        return (k, r['function'], re.sub(r'\s+', ' ', r['text']))
    if k == 'macro':
        return (k, r['id'], r['function'], r['if_context'])
    if k == 'D-step':
        return (k, r['function'], re.sub(r'\s+', ' ', r['text']), r['if_context'])
    if k == 'oob':
        return (k, r['function'], re.sub(r'\s+', ' ', r['text']), r['if_context'])
    return (k, r['function'], r['why'], re.sub(r'\s+', ' ', r['text']))


def diff(old_p, new_p):
    ho, old = load(old_p)
    hn, new = load(new_p)

    def index(rows):
        d = {}
        for r in rows:
            d.setdefault(row_key(r), []).append(r)
        return d
    a, b = index(old), index(new)
    out = []
    cmpf = [c for c in ('code', 'why', 'order|relaunch', 'flags', 'seq', 'if_context', 'hold', 'evt_text')
            if c in ho and c in hn]
    skipped = [c for c in ('hold', 'evt_text') if (c in ho) != (c in hn)]
    moved = same = 0
    for k in sorted(set(a) | set(b), key=lambda x: tuple(str(y) for y in x)):
        ra, rb = a.get(k, []), b.get(k, [])
        if len(ra) != len(rb):
            if not ra:
                for r in rb:
                    out.append('+ NEW      %-11s %-28s %-24s %s | %s %s' % (
                        r['kind'], r['id'], r['function'], r['at'], r['why'] or r['code'], r['text'][:70]))
            elif not rb:
                for r in ra:
                    out.append('- VANISHED %-11s %-28s %-24s %s | %s %s' % (
                        r['kind'], r['id'], r['function'], r['at'], r['why'] or r['code'], r['text'][:70]))
            else:
                out.append('~ COUNT    %-11s %-28s %-24s %d -> %d uses' % (
                    k[0], k[1], k[2] if len(k) > 2 else '', len(ra), len(rb)))
                if k[0] != 'macro':
                    for tag, rr in (('was', ra), ('now', rb)):
                        for r in rr:
                            out.append('      %s: %-8s %-26s code=%s why=%s relaunch=%s %s' % (
                                tag, r['id'], r['at'], r['code'] or '-', r['why'] or '-',
                                r['order|relaunch'] or '-', r['if_context']))
            continue
        for x, y in zip(ra, rb):
            ch = [c for c in cmpf if x.get(c, '') != y.get(c, '')]
            if ch:
                out.append('~ CHANGED  %-11s %-28s %-24s %s -> %s' % (
                    x['kind'], y['id'], y['function'], x['at'], y['at']))
                for c in ch:
                    if c == 'evt_text':
                        # only the calls that differ (the lists are long)
                        xa, ya = x[c].split(' ;; '), y[c].split(' ;; ')
                        for t in xa:
                            if t not in ya:
                                out.append('      text gone:     %s' % t[:300])
                        for t in ya:
                            if t not in xa:
                                out.append('      text new:      %s' % t[:300])
                        continue
                    out.append('      %-14s %s\n      %-14s %s' % (c + ' was:', x.get(c, '')[:400],
                                                                  c + ' now:', y.get(c, '')[:400]))
            elif x['at'] != y['at'] or x['id'] != y['id']:
                moved += 1
            else:
                same += 1
    # the teardown's ORDER: steps both sides have (once each), in the other order
    def seq(rows):
        ks = [row_key(r) for r in rows if r['kind'] == 'D-step']
        return [k for k in ks if ks.count(k) == 1]
    so, sn = seq(old), seq(new)
    pos = {k: i for i, k in enumerate(sn)}
    common = [k for k in so if k in pos]
    by = {row_key(r): r for r in new if r['kind'] == 'D-step'}
    for x, y in zip(common, common[1:]):
        if pos[x] > pos[y]:
            out.append('~ ORDER    D-step      %-28s %s now runs BEFORE %s (line %s)' % (
                by[y]['function'], by[y]['text'][:50], by[x]['text'][:50], by[y]['at']))
    kinds = {}
    for r in new:
        kinds[r['kind']] = kinds.get(r['kind'], 0) + 1
    head = ['exit map diff: %s -> %s' % (os.path.basename(old_p), os.path.basename(new_p)),
            'rows: %d -> %d (%s)' % (len(old), len(new), ', '.join('%s %d' % kv for kv in sorted(kinds.items()))),
            '%d new, %d vanished, %d changed, %d count changes, %d order changes; %d only moved, '
            '%d identical' % (
                sum(x.startswith('+') for x in out), sum(x.startswith('-') for x in out),
                sum(x.startswith('~ CHANGED') for x in out), sum(x.startswith('~ COUNT') for x in out),
                sum(x.startswith('~ ORDER') for x in out), moved, same)]
    if skipped:
        head.append('(columns only one side has, not compared: %s)' % ', '.join(skipped))
    return '\n'.join(head + [''] + out) + '\n'


def write(rows, outp):
    with open(outp, 'w', newline='\n', encoding='utf-8') as f:
        f.write('\t'.join(HDR) + '\n')
        for r in rows:
            f.write('\t'.join(str(x).replace('\t', ' ') for x in r) + '\n')


def selftest():
    import tempfile
    d = tempfile.mkdtemp(prefix='exitmap')
    base = [('A-C', 'B9', 'main', '1820', '2', '"no_rom"', 'ORDER_EVT_FIRST', '', 'EVT ES AUD VID HO EXIT',
             '', '', '-', 'fe_evt("exit code=2 reason=no_rom")'),
            ('D-trigger', 'T07', 'main', 'main_psp.c:3544', '', 'reset_failed', 'RELAUNCH_GAMELIST', '', '',
             '-', 'g_running = 0;', '', ''),
            ('D-trigger', 'T08', 'main', 'main_psp.c:3553', '', '', '0', '', '', '-', 'g_running  = 0;', '', ''),
            ('D-step', 'D3', 'session_end', '4511', '', '', '', 'call', '', '', 'play_lock_drop("loop_exit")', '', ''),
            ('D-step', 'D4', 'session_end', '4512', '', '', '', 'call', '', '', 'pwr_hold("teardown")', '', ''),
            ('macro', 'FE_EXIT_GAME', 'main', 'main_psp.c:10', '', '', '', '', '', '-', '', '', ''),
            ('macro', 'FE_EXIT_GAME', 'main', 'main_psp.c:20', '', '', '', '', '', '-', '', '', '')]
    new = [list(r) for r in base]
    new[3], new[4] = new[4], new[3]                  # HW-1 F2: the hold first
    new[3][3], new[4][3] = '4529', '4530'
    new[0][3] = '1844'                               # moved only
    new[0][5] = 'home ? "home" : "no_rom"'           # ... and changed
    new[0][12] = 'fe_evt("exit code=%d reason=%s", home ? 0 : 2, home ? "home" : "no_rom")'
    new[1][1], new[1][3] = 'T08', 'main_psp.c:3600'  # renumbered and moved: not a change
    new[2][1], new[2][10] = 'T09', 'g_running = 0;'
    new.insert(1, ['D-trigger', 'T07', 'wake_service', 'fe/power.e.inc.c:171', '', 'me_lost',
                   'RELAUNCH_GAMELIST', '', '', '-', 'g_running = 0; /* ME lost */', '', ''])
    new.pop()                                        # one macro use fewer
    write(base, os.path.join(d, 'a.tsv'))
    write(new, os.path.join(d, 'b.tsv'))
    t = diff(os.path.join(d, 'a.tsv'), os.path.join(d, 'b.tsv'))
    ok = ('1 new, 0 vanished, 1 changed, 1 count changes, 1 order changes; 4 only moved, 0 identical' in t
          and '~ ORDER    D-step      session_end                  pwr_hold("teardown") now runs BEFORE '
              'play_lock_drop("loop_exit")' in t
          and '+ NEW      D-trigger   T07' in t and 'wake_service' in t
          and '~ CHANGED  A-C         B9' in t and 'why was:' in t and 'text new:      fe_evt("exit code=%d' in t
          and '2 -> 1 uses' in t)
    print('epochc_exitmap selftest: %s' % ('ok' if ok else 'FAILED\n' + t))
    return 0 if ok else 1


def main(argv):
    if '--selftest' in argv:
        return selftest()
    if len(argv) >= 4 and argv[1] == '--diff':
        t = diff(argv[2], argv[3])
        sys.stdout.write(t)
        if '-o' in argv:
            with open(argv[argv.index('-o') + 1], 'w', newline='\n', encoding='utf-8') as f:
                f.write(t)
        return 0
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    root, outp = argv[1], argv[2]
    try:
        rows = build_rows(root, outp)
    except Exception as e:  # noqa: BLE001
        sys.stderr.write('T8 raw view failed: %r\n' % (e,))
        return 2
    write(rows, outp)
    kinds = {}
    for r in rows:
        kinds[r[0]] = kinds.get(r[0], 0) + 1
    print('rows:', ', '.join('%s %d' % kv for kv in sorted(kinds.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
