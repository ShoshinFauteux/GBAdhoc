#!/usr/bin/env python3
"""ef_srcscan.py -- port of builds/mainpsp-map/out/scratch/x4_src.py (the M
basis of x4's 335/280/55 census) for inline_census.py (T13).

Scans a C file for top-level function definitions (line ranges) and runs a
mini preprocessor over it: #if/#ifdef/#elif/#else stacks are evaluated against
a flavour's define set (the CONFIGS table below, from tools/build.sh FE_DEFS +
psp/Makefile flags + fe_features.h + the fe_evt.h GPSP_NO_TELEMETRY
derivation), and #defines made inside the file are followed.  Headers are NOT
read: a condition on a macro that only a header defines evaluates as
undefined.  It is a scanner, not a compiler; T3 pp_diff.sh (psp-gcc -E) is the
authority on what is compiled.

Changes from x4_src.py: a `harness` configuration (the lab set without
LAB_HARNESS, tools/build.sh `harness` profile); importable API
(scan(path, flavours)); no other behaviour change, so x4's counts reproduce.
"""
import json, re, sys

# Configurations (tools/build.sh FE_DEFS + psp/Makefile flags + fe_features.h
# + fe_evt.h derivation).  Values are ints; absent = undefined.
BASE = dict(PSP=1, USE_PSP_RGB565_FORMAT=None)
RELEASE = dict(GPSP_PLAYABLE=1, GPSP_PROFILE_RELEASE=1, GPSP_NO_TELEMETRY=1,
               REWIND=1, GBA_LINK=1, GBADHOC_ONLINE=1, RFU_SCHED=1,
               GPSP_CHEATS=1, VID_TRIPLE=1, PLAY_INPUT_FRESH=1,
               PLAY_FF_FLIP_PACE=7, LAYOUT_PAD_BYTES=304,
               LAYOUT_PAD_RO_BYTES=0, ND_MAX_PAYLOAD=144)
LAB = dict(GPSP_PLAYABLE=1, GPSP_KEEP_TELEMETRY=1, GPSP_PERF_RIG=1,
           GPSP_PROFILE_HARNESS=1, REWIND=1, GBA_LINK=1, GBADHOC_ONLINE=1,
           RFU_SCHED=1, GPSP_CHEATS=1, VID_TRIPLE=1, PLAY_INPUT_FRESH=1,
           PLAY_FF_FLIP_PACE=7, LAYOUT_PAD_BYTES=5904,
           LAYOUT_PAD_RO_BYTES=2864, LAB_HARNESS=1, ND_MAX_PAYLOAD=144)
CONFIGS = dict(release=RELEASE, lab=LAB)

KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "defined",
            "__attribute__", "__asm__", "asm", "typeof", "__typeof__"}


def strip_code(text):
    """Blank comments and string/char literals, keep newlines and offsets."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        elif c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == '\\': j += 1
                if j < n and text[j] == '\n': break
                j += 1
            j = min(j + 1, n)
            out.append(c + re.sub(r'[^\n]', ' ', text[i + 1:j - 1]) + c); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


def eval_cond(expr, defs):
    e = re.sub(r'defined\s*\(\s*(\w+)\s*\)', lambda m: '1' if m.group(1) in defs else '0', expr)
    e = re.sub(r'defined\s+(\w+)', lambda m: '1' if m.group(1) in defs else '0', e)
    def ident(m):
        w = m.group(0)
        if w in defs:
            v = defs[w]
            return str(v) if isinstance(v, int) else '1'
        return '0'
    e = re.sub(r'\b[A-Za-z_]\w*\b', ident, e)
    e = re.sub(r'(\d+)[uUlL]+\b', r'\1', e)
    e = e.replace('&&', ' and ').replace('||', ' or ')
    e = re.sub(r'!(?!=)', ' not ', e)
    try:
        return bool(eval(e, {}, {}))
    except Exception:
        return None


# x4_src.py read only the FIRST physical line of a directive.  A `#if a || \`
# continued on the next line was then evaluated as a syntax error -> False,
# so whole #if blocks were taken as compiled out: in lab/harness the stall
# watchdog block (main_psp.c:10198 `#if !defined(GPSP_PLAYABLE) ||
# defined(GPSP_KEEP_TELEMETRY) || \`) lost 7 functions the ELF does contain.
# Found by inline_census.py (scan says dead/compiled out, ELF has symbols).
# Set False to get x4_src.py's exact behaviour.
JOIN_CONTINUATIONS = True
BSL = chr(92)


def join_continuations(lines):
    """A directive continued with a trailing backslash becomes one logical
    line on its first physical line; the continuation lines become blank."""
    out = list(lines)
    i = 0
    while i < len(out):
        if out[i].lstrip().startswith('#') and out[i].rstrip().endswith(BSL):
            j = i
            acc = out[i].rstrip()[:-1]
            while out[j].rstrip().endswith(BSL) and j + 1 < len(out):
                j += 1
                acc += ' ' + out[j].rstrip().rstrip(BSL)
                out[j] = ''
            out[i] = acc
            i = j
        i += 1
    return out


def preprocess(lines, cfg):
    """Return per-line (active, cond_stack_text) for configuration cfg."""
    if JOIN_CONTINUATIONS:
        lines = join_continuations(lines)
    defs = dict(BASE); defs.update(cfg)
    defs = {k: v for k, v in defs.items()}
    stack = []   # entries: [parent_active, this_branch_active, any_taken, text]
    active = True
    res = []
    for ln in lines:
        s = ln.strip()
        m = re.match(r'#\s*(\w+)\s*(.*)', s)
        d = m.group(1) if m else None
        rest = m.group(2) if m else ''
        rest = re.sub(r'/\*.*?\*/', '', rest); rest = re.sub(r'//.*', '', rest).strip()
        if d in ('if', 'ifdef', 'ifndef'):
            if d == 'ifdef': v = rest.split()[0] in defs; txt = 'defined(%s)' % rest.split()[0]
            elif d == 'ifndef': v = rest.split()[0] not in defs; txt = '!defined(%s)' % rest.split()[0]
            else:
                v = eval_cond(rest, defs); txt = rest
                if v is None: v = False; txt += ' [?]'
            stack.append([active, active and v, v, txt])
            active = active and v
            res.append((active, [x[3] for x in stack])); continue
        if d == 'elif' and stack:
            top = stack[-1]
            v = eval_cond(rest, defs)
            if v is None: v = False
            take = (not top[2]) and v
            top[1] = top[0] and take; top[2] = top[2] or v; top[3] = 'elif ' + rest
            active = top[1]
            res.append((active, [x[3] for x in stack])); continue
        if d == 'else' and stack:
            top = stack[-1]
            top[1] = top[0] and not top[2]; top[2] = True; top[3] = 'else of ' + top[3]
            active = top[1]
            res.append((active, [x[3] for x in stack])); continue
        if d == 'endif' and stack:
            res.append((active, [x[3] for x in stack]))
            top = stack.pop(); active = top[0]; continue
        if active and d == 'define':
            mm = re.match(r'(\w+)(\([^)]*\))?\s*(.*)', rest)
            if mm and not mm.group(2):
                val = mm.group(3).strip()
                try: defs[mm.group(1)] = int(val, 0)
                except ValueError: defs[mm.group(1)] = 1 if val == '' else val
            elif mm: defs[mm.group(1)] = 1
        if active and d == 'undef':
            defs.pop(rest.split()[0], None)
        res.append((active, [x[3] for x in stack]))
    return res


def find_functions(code):
    """Top-level function definitions: (name, start_line, open_line, end_line)."""
    lines = code.split('\n')
    # offsets of line starts
    starts = [0]
    for ln in lines: starts.append(starts[-1] + len(ln) + 1)
    import bisect
    def line_of(off): return bisect.bisect_right(starts, off)  # 1-based
    funcs = []
    depth = 0
    seg_start = 0          # start offset of the current top-level declaration
    i, n = 0, len(code)
    # blank preprocessor lines so they do not glue to declarations
    pp = re.compile(r'^[ \t]*#.*?(?<!\\)$', re.M | re.S)
    code2 = list(code)
    for m in re.finditer(r'^[ \t]*#(?:[^\n]*\\\n)*[^\n]*', code, re.M):
        for k in range(m.start(), m.end()):
            if code2[k] != '\n': code2[k] = ' '
    code = ''.join(code2)
    while i < n:
        c = code[i]
        if c == '{':
            if depth == 0:
                decl = code[seg_start:i]
                name = None
                if '=' not in decl.split('(')[0] and '(' in decl and not re.search(r'=\s*$', decl.strip()):
                    # walk identifiers followed by '('
                    j = 0; dd = decl
                    for mm in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', dd):
                        if mm.group(1) in KEYWORDS: continue
                        name = mm.group(1); break
                    if re.match(r'\s*(typedef|struct|union|enum)\b', decl) and not re.search(r'\)\s*$', decl.strip()):
                        name = None
                    if not re.search(r'\)\s*(__attribute__\s*\(\(.*\)\)\s*)?$', decl.strip(), re.S):
                        name = None
                if name:
                    # start line: first non-blank line of decl
                    k = seg_start
                    while k < i and code[k].isspace(): k += 1
                    funcs.append([name, line_of(k), line_of(i), None])
                    cur = funcs[-1]
                else:
                    cur = None
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                if cur is not None:
                    cur[3] = line_of(i)
                    cur = None
                seg_start = i + 1
        elif c == ';' and depth == 0:
            seg_start = i + 1
        i += 1
    return funcs


HARNESS = {k: v for k, v in LAB.items() if k != 'LAB_HARNESS'}
CONFIGS['harness'] = HARNESS


# PLAN §1.1 modules are fragments `psp/fe/<name>.inc.c` #included into
# main_psp.c: one CU, one STT_FILE.  Their definitions must be scanned as part
# of main_psp.c, or a function moved into a fragment vanishes from the census
# (an inlined-only one leaves no trace at all, so a change in its inlining
# between two split trees was a false PASS).  A quoted #include of a .c /
# .inc file that exists relative to the including file is expanded in place,
# recursively; headers are still not read.  Found by the verify pass
# (move-across-fragments mutant, tests/elf-func_test_inline_census.py).
# main_psp.c at 01067e74 has no such include, so x4's counts are unchanged.
INCLUDE_FRAGMENTS = True
_FRAG = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+\.(?:c|inc))"')


def expand_fragments(path, depth=0, seen=None):
    """-> (text, origin): origin[i] = (file, line) of expanded line i+1.  The
    #include line itself stays (blank) so line counts of the shell hold."""
    import os
    seen = set() if seen is None else seen
    src = open(path, encoding='utf-8', errors='replace').read()
    out, origin = [], []
    for k, ln in enumerate(src.split('\n')):
        m = _FRAG.match(ln) if INCLUDE_FRAGMENTS and depth < 8 else None
        q = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(path)), m.group(1))) if m else None
        if q and os.path.exists(q) and q not in seen:
            seen.add(q)
            out.append(''); origin.append((path, k + 1))
            t, o = expand_fragments(q, depth + 1, seen)
            out.extend(t.split('\n')); origin.extend(o)
            continue
        out.append(ln); origin.append((path, k + 1))
    return '\n'.join(out), origin


def scan(path, flavours=('release', 'lab', 'harness')):
    """-> dict(functions=[dict(name, start, open, end, lines, <flavour>=dict(
    active_at_open, live_lines), cond, src_file, src_line)], summary)"""
    src, origin = expand_fragments(path)
    raw_lines = src.split('\n')
    funcs = find_functions(strip_code(src))
    pre = {k: preprocess(raw_lines, CONFIGS[k]) for k in flavours}
    out = []
    for name, s, o, e in funcs:
        rec = dict(name=name, start=s, open=o, end=e, lines=(e - s + 1) if e else None)
        for k, p in pre.items():
            rec[k] = dict(active_at_open=p[o - 1][0],
                          live_lines=sum(p[x - 1][0] for x in range(s, (e or s) + 1)))
        rec['cond'] = pre[flavours[0]][o - 1][1]
        rec['src_file'], rec['src_line'] = origin[s - 1]
        out.append(rec)
    summ = {k: dict(live_lines=sum(1 for a, _ in p if a), total=len(p)) for k, p in pre.items()}
    return dict(functions=out, summary=summ)
