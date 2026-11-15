#!/usr/bin/env python3
"""csrc.py -- the shared C-source model of psp/main_psp.c for the phase-T tools
(split.py T4, globals.py T9).  Host Python only, no compiler.

Basis: builds/mainpsp-map/out/scratch/census.py (x1, validated against the ELF in
x1 Verification), ported into a function and FIXED as PLAN §5 T9 asks:

  * census.py skipped every declaration carrying `__attribute__((...))`: its
    prototype test `\\w\\s*\\([^*]` matched `__attribute__((` and its declarator
    regex stripped `__attribute__((aligned(64)))` non-greedily, leaving a `)`.
    That lost 23 file-scope definitions (x1 Verification: g_vc_work 2247,
    g_all_clean 3566, the .sbss/.sdata pins 3148-3171 and 13740-13741).
    Here every `__attribute__((...))` is removed with BALANCED parentheses
    before the declarator parse and kept in `attrs` (raw text, so
    section(".sbss") keeps its string).

Everything else (comment/string blanking, the #if stack per line, the top-level
brace walk, reference classification, call graph) is census.py's logic.

parse(text, root=None) -> dict with
  funcs   [{name,start,body,end,s,e,static,head}]           (1-based lines)
  vars    [{name,line,end,extern,static,volatile,const,init,decl,attrs,cond,...}]
  items   every top-level item, in order: {kind,name,start,end}
          kind F function definition, V variable definition, X extern
          declaration, P prototype / macro statement, T typedef/tag, O other
  line_cond[i], pp_depth[i] (#if depth BEFORE line i), pp_line[i] (bool)
  refs, calls, macro_calls, fnref, mcalls, shadow  (as census.json)
"""
import bisect
import os
import re
import glob as _glob


def blank_comments_strings(raw):
    """Replace comments and string/char literal bodies by spaces, keep newlines
    (so every offset and line number is unchanged)."""
    out = []
    i, n = 0, len(raw)
    while i < n:
        c = raw[i]
        if raw.startswith('/*', i):
            j = raw.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in raw[i:j]))
            i = j
        elif raw.startswith('//', i):
            j = raw.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif c == '"' or c == "'":
            q = c
            j = i + 1
            while j < n and raw[j] != q and raw[j] != '\n':
                if raw[j] == '\\':
                    j += 1
                j += 1
            if j < n and raw[j] == q:
                j += 1
                seg = raw[i:j]
                out.append(q + ''.join('\n' if ch == '\n' else ' ' for ch in seg[1:-1]) + q)
            else:
                # unterminated on its line (an apostrophe in `#error can't`,
                # in #if 0 text, or a quote a marker split off): blank to the
                # end of the line and KEEP the newline.  The old code replaced
                # that newline by the quote, so every later line number was
                # off by one (adversarial review 2026-11-05)
                out.append(''.join('\n' if ch == '\n' else ' ' for ch in raw[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    s = ''.join(out)
    assert len(s) == len(raw)
    assert s.count('\n') == raw.count('\n'), 'blank_comments_strings lost a line break'
    return s


def comment_spans(raw):
    """[(start, end)] of every comment (block and line), strings respected."""
    spans = []
    i, n = 0, len(raw)
    while i < n:
        c = raw[i]
        if raw.startswith('/*', i):
            j = raw.find('*/', i + 2)
            j = n if j < 0 else j + 2
            spans.append((i, j))
            i = j
        elif raw.startswith('//', i):
            j = raw.find('\n', i)
            j = n if j < 0 else j
            spans.append((i, j))
            i = j
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and raw[j] != c and raw[j] != '\n':
                if raw[j] == '\\':
                    j += 1
                j += 1
            i = j + 1
        else:
            i += 1
    return spans


def strip_attrs(clean, raw=None):
    """Remove every `__attribute__ (( ... ))` (balanced) from `clean`; return
    (text, [attr texts]).  Offsets are NOT preserved (used on statement text)."""
    attrs = []
    out = []
    i = 0
    for m in re.finditer(r'__attribute__\s*\(', clean):
        if m.start() < i:
            continue
        j = m.end() - 1
        d = 0
        while j < len(clean):
            if clean[j] == '(':
                d += 1
            elif clean[j] == ')':
                d -= 1
                if d == 0:
                    break
            j += 1
        out.append(clean[i:m.start()])
        attrs.append((raw if raw is not None else clean)[m.start():j + 1])
        out.append(' ')
        i = j + 1
    out.append(clean[i:])
    return ''.join(out), attrs


SIMPLE = {'char': 1, 'signed char': 1, 'unsigned char': 1, 'u8': 1, 's8': 1, 'uint8_t': 1, 'int8_t': 1, 'bool': 1, '_Bool': 1,
          'short': 2, 'unsigned short': 2, 'u16': 2, 's16': 2, 'uint16_t': 2, 'int16_t': 2,
          'int': 4, 'unsigned': 4, 'unsigned int': 4, 'signed': 4, 'long': 4, 'unsigned long': 4, 'u32': 4, 's32': 4,
          'uint32_t': 4, 'int32_t': 4, 'float': 4, 'SceUID': 4, 'size_t': 4, 'SceSize': 4, 'uintptr_t': 4, 'intptr_t': 4,
          'long long': 8, 'unsigned long long': 8, 'u64': 8, 's64': 8, 'uint64_t': 8, 'int64_t': 8, 'double': 8,
          'SceUInt64': 8, 'time_t': 4, 'sig_atomic_t': 4}
QUAL = r'\b(static|volatile|const|extern|register|__thread|inline)\b'


def split_top(s, ch=','):
    parts, d, cur = [], 0, []
    for c in s:
        if c in '([{':
            d += 1
        elif c in ')]}':
            d -= 1
        if c == ch and d == 0:
            parts.append(''.join(cur))
            cur = []
        else:
            cur.append(c)
    parts.append(''.join(cur))
    return parts


def parse(raw, root=None, want_refs=True):
    clean = blank_comments_strings(raw)
    cl = clean.split('\n')
    nlines = len(cl)
    # ---------- preprocessor: cond stack and depth per line; blank pp lines ----------
    cond = []
    line_cond = [None] * (nlines + 2)
    pp_depth = [0] * (nlines + 2)
    pp_line = [False] * (nlines + 2)
    pp_kw = [None] * (nlines + 2)
    defines = {}
    k = 0
    while k < nlines:
        s = cl[k].strip()
        line_cond[k + 1] = ' && '.join(cond)
        pp_depth[k + 1] = len(cond)
        if s.startswith('#'):
            full = s
            first = k
            pp_line[k + 1] = True
            while cl[k].rstrip().endswith('\\') and k + 1 < nlines:
                cl[k] = ' ' * len(cl[k])
                k += 1
                full += ' ' + cl[k].strip()
                line_cond[k + 1] = ' && '.join(cond)
                pp_depth[k + 1] = len(cond)
                pp_line[k + 1] = True
            cl[k] = ' ' * len(cl[k])
            d = re.match(r'#\s*(\w+)\s*(.*)', full)
            if d:
                kw, rest = d.group(1), d.group(2).replace('\\', ' ').strip()
                pp_kw[first + 1] = kw
                if kw in ('if', 'ifdef', 'ifndef'):
                    cond.append({'if': rest, 'ifdef': 'defined(' + rest + ')', 'ifndef': '!defined(' + rest + ')'}[kw])
                elif kw == 'elif':
                    if cond:
                        cond[-1] = '!(' + cond[-1] + ') && ' + rest
                elif kw == 'else':
                    if cond:
                        cond[-1] = '!(' + cond[-1] + ')'
                elif kw == 'endif':
                    if cond:
                        cond.pop()
                elif kw == 'define':
                    m = re.match(r'(\w+)(\s+(.*))?$', rest)
                    if m and '(' not in rest[:len(m.group(1)) + 1]:
                        defines.setdefault(m.group(1), (m.group(3) or '').strip())
        k += 1
    final_depth = len(cond)
    clean2 = '\n'.join(cl)
    if root:
        for hp in (_glob.glob(os.path.join(root, 'psp', '*.h')) + _glob.glob(os.path.join(root, '*.h'))
                   + _glob.glob(os.path.join(root, 'psp', 'me', '*.h'))):
            for hl in open(hp, encoding='utf-8', errors='replace'):
                mh = re.match(r'\s*#\s*define\s+(\w+)\s+([^/\n]*)', hl)
                if mh:
                    defines.setdefault(mh.group(1), mh.group(2).strip())

    nl = [m.start() for m in re.finditer('\n', clean2)]

    def lineof(pos):
        return bisect.bisect_left(nl, pos) + 1

    # ---------- top-level walk (census.py §3) ----------
    items = []
    depth = 0
    par = 0
    stmt_start = 0
    i = 0
    n = len(clean2)
    pending_kind = None
    body_open = None
    while i < n:
        c = clean2[i]
        if c == '(':
            par += 1
        elif c == ')':
            par -= 1
        elif c == '{':
            if depth == 0 and par == 0:
                head = clean2[stmt_start:i]
                hs, _ = strip_attrs(head.strip())
                hs = hs.strip()
                if '=' in head:
                    pending_kind = 'init'
                elif re.search(r'\)\s*$', hs, re.S) and not re.match(r'\s*(typedef\s+)?(struct|union|enum)\b[^()]*$', hs, re.S):
                    pending_kind = 'func'
                    body_open = i
                else:
                    pending_kind = 'agg'
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0 and pending_kind == 'func':
                items.append(('func', stmt_start, i + 1, clean2[stmt_start:body_open], body_open))
                stmt_start = i + 1
                pending_kind = None
        elif c == ';' and depth == 0 and par == 0:
            items.append(('stmt', stmt_start, i + 1, clean2[stmt_start:i], None))
            stmt_start = i + 1
            pending_kind = None
        i += 1
    tail_open = clean2[stmt_start:].strip() != ''

    def first_code(s, text):
        return s + len(text) - len(text.lstrip())

    # ---------- functions ----------
    funcs = []
    for kind, s, e, text, bo in items:
        if kind != 'func':
            continue
        t0, _ = strip_attrs(text.strip())
        m = re.search(r'(\w+)\s*\([^()]*(\([^()]*\)[^()]*)*\)\s*$', t0.strip(), re.S)
        name = m.group(1) if m else '?'
        funcs.append({'name': name, 'start': lineof(first_code(s, text)), 'body': lineof(bo), 'end': lineof(e - 1),
                      's': bo, 'e': e, 'ps': first_code(s, text),
                      'static': bool(re.search(r'\bstatic\b', text)), 'head': ' '.join(text.split())})
    fstarts = [f['s'] for f in funcs]

    def func_at(pos):
        k2 = bisect.bisect_right(fstarts, pos) - 1
        if k2 >= 0 and funcs[k2]['s'] <= pos < funcs[k2]['e']:
            return funcs[k2]['name']
        return None

    # ---------- declarations ----------
    vars_ = []
    all_items = []
    for kind, s, e, text, bo in items:
        st_line = lineof(first_code(s, text))
        en_line = lineof(e - 1)
        if kind == 'func':
            all_items.append({'kind': 'F', 'name': funcs[len([x for x in all_items if x['kind'] == 'F'])]['name'],
                              'start': st_line, 'end': en_line})
            continue
        if not text.strip():
            all_items.append({'kind': 'O', 'name': None, 'start': st_line, 'end': en_line})
            continue
        p0 = first_code(s, text)
        raw_stmt = raw[p0:e - 1]
        clean_stmt = clean2[p0:e - 1]
        t_noattr, attrs = strip_attrs(clean_stmt, raw_stmt)
        t = ' '.join(t_noattr.split())
        rec = {'start': st_line, 'end': en_line}
        if t.startswith('typedef') or re.match(r'^(static\s+)?(struct|union|enum)\s+\w+\s*(\{.*\})?\s*$', t):
            all_items.append(dict(rec, kind='T', name=None))
            continue
        if re.match(r'^(static\s+)?_Static_assert|^asm\b|^__asm__', t):
            all_items.append(dict(rec, kind='O', name=None))
            continue
        is_extern = bool(re.match(r'^extern\b', t))
        aggbody = None
        m = re.match(r'^(.*?\b(struct|union|enum)\b\s*\w*\s*)\{(.*)\}(.*)$', t)
        if m and '=' not in m.group(1):
            typ = m.group(1).strip()
            # the aggregate body ends at the brace MATCHING the first `{`, not
            # at the last `}` of the statement: `static const struct {...}
            # g_fxs_gba[] = { {...}, ... };` (L:6773) has an initializer whose
            # last `}` the greedy match took, which left no declarator and lost
            # the definition (adversarial review 2026-11-05)
            ob = m.end(1)
            dpt, cb = 0, None
            for q in range(ob, len(t)):
                if t[q] == '{':
                    dpt += 1
                elif t[q] == '}':
                    dpt -= 1
                    if dpt == 0:
                        cb = q
                        break
            aggbody = t[ob + 1:cb] if cb is not None else m.group(3)
            decls = t[cb + 1:] if cb is not None else m.group(4)
        else:
            parts = split_top(t)
            first = parts[0]
            f0 = first.split('=')[0]
            fm = re.search(r'\(\s*\*\s*(\w+)\s*((?:\[[^\]]*\])*)\s*\)\s*\(', f0)
            if not fm and re.search(r'\w\s*\(', f0):
                nm_p = re.search(r'(\w+)\s*\(', f0)
                all_items.append(dict(rec, kind='P', name=nm_p.group(1) if nm_p else None))
                continue
            if fm:
                base = f0[:fm.start()]
                v = {'name': fm.group(1), 'type': ' '.join(base.split()) + ' (*)(...)' + (fm.group(2) or ''),
                     'line': st_line, 'end': en_line, 'extern': is_extern,
                     'static': bool(re.search(r'\bstatic\b', base)), 'volatile': 'volatile' in base,
                     'const': False, 'init': '=' in first, 'decl': t[:220], 'attrs': attrs, 'cond': '',
                     'size': None}
                vars_.append(v)
                all_items.append(dict(rec, kind='X' if is_extern else 'V', name=v['name']))
                continue
            mm = re.match(r'^((?:(?:static|volatile|const|extern|unsigned|signed|long|short|struct|union|enum|register)\s+)*[\w]+)((?:\s|\*).*)$', f0.strip())
            if not mm:
                all_items.append(dict(rec, kind='O', name=None))
                continue
            typ = mm.group(1)
            lead = t[:len(t) - len(t.lstrip())]
            decls = ','.join([first.lstrip()[len(typ):]] + parts[1:])
        base_type = ' '.join(re.sub(QUAL, ' ', typ).split())
        got = False
        for dcl in split_top(decls):
            dpart = dcl.split('=')[0] if '=' in dcl else dcl
            hasinit = '=' in dcl
            nm = re.search(r'([A-Za-z_]\w*)\s*((?:\[[^\]]*\]\s*)*)$', dpart.strip())
            if not nm:
                continue
            name = nm.group(1)
            arr = nm.group(2) or ''
            if name in ('void',):
                continue
            stars = re.sub(r'\[[^\]]*\]', '', dpart).count('*')
            if stars:
                esz = 4
            else:
                esz = SIMPLE.get(base_type)
            dims = re.findall(r'\[([^\]]*)\]', arr)
            cnt = 1
            for dm in dims:
                if dm.strip().isdigit():
                    cnt *= int(dm.strip())
                else:
                    cnt = None
                    break
            v = {'name': name, 'type': (base_type + ' ' + '*' * stars).strip() + arr.replace(' ', ''),
                 'line': st_line, 'end': en_line, 'extern': is_extern,
                 'static': bool(re.search(r'\bstatic\b', typ)), 'volatile': 'volatile' in typ or 'volatile' in dpart,
                 'const': bool(re.search(r'\bconst\b', typ)) and not stars, 'init': hasinit, 'decl': t[:220],
                 'attrs': attrs, 'cond': '', 'size': (esz * cnt) if (esz and cnt) else None,
                 'agg': aggbody is not None}
            vars_.append(v)
            all_items.append(dict(rec, kind='X' if is_extern else 'V', name=name))
            got = True
        if not got:
            all_items.append(dict(rec, kind='O', name=None))

    for v in vars_:
        v['cond'] = line_cond[v['line']] or ''
        v['section'] = None
        for a in v['attrs']:
            sm = re.search(r'section\s*\(\s*"([^"]*)"', a)
            if sm:
                v['section'] = sm.group(1)
        v['attributed'] = bool(v['attrs'])

    res = {'raw': raw, 'clean': clean2, 'nlines': nlines, 'line_cond': line_cond, 'pp_depth': pp_depth,
           'pp_line': pp_line, 'pp_kw': pp_kw, 'final_depth': final_depth, 'tail_open': tail_open,
           'funcs': funcs, 'vars': vars_, 'items': all_items, 'defines': defines, 'lineof': lineof,
           'func_at': func_at}
    if not want_refs:
        return res

    # ---------- references (census.py §6) ----------
    gnames = {}
    for v in vars_:
        gnames.setdefault(v['name'], []).append(v)
    tok = re.compile(r'\b[A-Za-z_]\w*\b')
    refs = {nm: {'r': {}, 'w': {}, 'a': {}, 'lines': []} for nm in gnames}
    fnames = {f['name'] for f in funcs}
    ASSIGN = re.compile(r'\s*(=(?!=)|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<=|>>=|\+\+|--)')
    WRITECALL = re.compile(r'(memset|memcpy|memmove|strcpy|strncpy|snprintf|sprintf|vsnprintf|strcat|strncat|fe_strlcpy|strlcpy|sceIoRead|fread|sceIoGetstat|sceRtcGetCurrentClockLocalTime|sceCtrlReadBufferPositive|sceCtrlPeekBufferPositive|copy_name|fe_copy)\s*\(\s*$')
    fn_pos_ref = {}
    spans = []
    for mx in re.finditer(r'\b(LAB_HOOK|LAB_FFP|fe_evt|FE_LP_SET)\s*\(', clean2):
        j = mx.end() - 1
        d = 0
        while j < n:
            if clean2[j] == '(':
                d += 1
            elif clean2[j] == ')':
                d -= 1
                if d == 0:
                    break
            j += 1
        spans.append((mx.start(), j, mx.group(1)))
    span_starts = [x[0] for x in spans]

    def span_at(pos):
        k2 = bisect.bisect_right(span_starts, pos) - 1
        while k2 >= 0:
            if spans[k2][0] <= pos <= spans[k2][1]:
                return spans[k2][2]
            k2 -= 1
            if k2 >= 0 and pos - spans[k2][0] > 20000:
                break
        return None

    for m in tok.finditer(clean2):
        w = m.group(0)
        if w in gnames:
            p = m.start()
            ln = lineof(p)
            fn = func_at(p)
            if fn is None:
                continue
            j = m.end()
            while True:
                if re.match(r'\s*\[', clean2[j:j + 2]):
                    d = 0
                    while j < n:
                        if clean2[j] == '[':
                            d += 1
                        elif clean2[j] == ']':
                            d -= 1
                            if d == 0:
                                j += 1
                                break
                        j += 1
                    continue
                mm2 = re.match(r'\s*(\.|->)\s*\w+', clean2[j:j + 80])
                if mm2:
                    j += mm2.end()
                    continue
                break
            before = clean2[max(0, p - 60):p]
            a = ASSIGN.match(clean2[j:j + 4])
            isw = bool(a) or bool(re.search(r'(\+\+|--)\s*$', before))
            isaddr = bool(re.search(r'(?<![&\w)\]])&\s*$', before))
            iscallw = bool(WRITECALL.search(before)) and not isw
            rec = refs[w]
            if isw or iscallw:
                rec['w'][fn] = rec['w'].get(fn, 0) + 1
                if a and a.group(1) not in ('=',) or (not a and isw):
                    rec['r'][fn] = rec['r'].get(fn, 0) + 1
            elif isaddr:
                rec['a'][fn] = rec['a'].get(fn, 0) + 1
            else:
                rec['r'][fn] = rec['r'].get(fn, 0) + 1
            kind = 'w' if (isw or iscallw) else ('a' if isaddr else 'r')
            if a and a.group(1) != '=':
                kind = 'rw'
            rec['lines'].append((ln, fn, kind, span_at(p)))
        elif w in fnames:
            p = m.start()
            after = clean2[m.end():m.end() + 30]
            if not re.match(r'\s*\(', after):
                fn = func_at(p)
                fn_pos_ref.setdefault(w, []).append((lineof(p), fn))

    TYPEW = r'(?:int|unsigned|u32|u16|u8|s32|s16|s8|char|short|long|float|double|uint\d+_t|int\d+_t|size_t|SceUID|bool|const\s+char|struct\s+\w+|\w+_t)'
    shadow = {}
    for f in funcs:
        body = clean2[f['s']:f['e']]
        for nm in gnames:
            if nm in body and re.search(TYPEW + r'\s*\**\s*' + re.escape(nm) + r'\s*[=;,\[)]', body):
                shadow.setdefault(nm, []).append(f['name'])

    calls = {f['name']: set() for f in funcs}
    for f in funcs:
        body = clean2[f['s']:f['e']]
        for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', body):
            if m.group(1) in fnames and m.group(1) != f['name']:
                calls[f['name']].add(m.group(1))
    macro_calls = {}
    for dn, dv in defines.items():
        fs = set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', dv)) & fnames
        if fs:
            macro_calls[dn] = fs
    for f in funcs:
        body = clean2[f['s']:f['e']]
        for dn, fs in macro_calls.items():
            if re.search(r'\b' + dn + r'\b', body):
                calls[f['name']] |= fs
    mcalls = []
    # main()'s calls by line (globals.py phases them); tier C C-1: frame_loop's body is
    # main()'s frame loop moved out, so its calls are main()'s too (frame_loop itself,
    # called from main, is not a callee of interest: it is the loop)
    for mf in [f for f in funcs if f['name'] in ('main', 'frame_loop')]:
        for mm3 in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', clean2[mf['s']:mf['e']]):
            nm2 = mm3.group(1)
            pos2 = mf['s'] + mm3.start()
            if nm2 == 'frame_loop':
                continue
            if nm2 in fnames or nm2 in macro_calls:
                mcalls.append((lineof(pos2), nm2, span_at(pos2)))
    for nm2, lst in fn_pos_ref.items():
        for (l2, f2) in lst:
            if f2 in ('main', 'frame_loop'):
                mcalls.append((l2, nm2, 'value'))
    res.update({'refs': refs, 'calls': {k2: sorted(v) for k2, v in calls.items()}, 'macro_calls': {k2: sorted(v) for k2, v in macro_calls.items()},
                'fnref': fn_pos_ref, 'mcalls': mcalls, 'shadow': shadow, 'spans': spans})
    return res
