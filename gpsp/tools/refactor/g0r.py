#!/usr/bin/env python3
"""g0r.py -- gate G0R (C5-0; builds/health-out/design/C-5.md §3 "Gates"): is a C-5 commit a
pure rename (or, for telemetry in the release, a pure deletion of telemetry stores)?

Input: two preprocessed streams of main_psp.c (`psp-gcc -E -P`, as pp_diff.sh makes them)
and the batch's rename map (gstruct.py's format).  pp_diff.sh --rename-map calls it per
flavour.

Both streams are tokenised (C tokens; whitespace and comments dropped, literals kept byte
for byte).  Then:
  1. NEW: every `VAR . FIELD` of the map reads as its OLD name (the rename in reverse).
  2. NEW: the map's struct definitions (`... struct TYPE { ... } ... ;` at file scope) are
     dropped.  REF: the file-scope declarations of the map's OLD names are dropped (a
     declaration naming a mapped and an unmapped object is a difference, not dropped).
  3. both: the definitions of the map's `telfn` functions are dropped in --mode deletions.
  4. with --tel, both: `do { S } while ( 0 )` whose S names a telemetry OLD reads as S
     (TEL(stmt) in a telemetry build); an empty `do { } while ( 0 )` is one marker token.
Mode equal (lab, harness; every flavour of a non-telemetry batch): the two token lists
must be identical.  Mode deletions (the release of a telemetry batch): every difference
must be REF tokens replaced by the empty-TEL marker (or deleted), and every `;`-separated
piece of those REF tokens must be a store to a telemetry OLD: `[++|--] OLD [index|.member]...
(= | op= | ++ | --) ...`.  Deleted pieces whose right side calls a function are LISTED
for the reviewer (sceDisplayGetVcount(), fe_host_mem_read(), net_now_us(): C-5.md §4),
not failed.

    g0r.py --map MAP [--tel] [--mode equal|deletions] REF.i NEW.i
    g0r.py --selftest
Exit 0 G0R ok, 1 a difference that is not a rename (or not a telemetry deletion), 2 usage.
"""
import difflib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gstruct  # noqa: E402

PUNCT = ['<<=', '>>=', '...', '->', '++', '--', '<<', '>>', '<=', '>=', '==', '!=', '&&', '||', '+=', '-=',
         '*=', '/=', '%=', '&=', '|=', '^=', '##']
TOKRE = re.compile(r'''
    (?P<ws>\s+|/\*.*?\*/|//[^\n]*|^\#[^\n]*)
  | (?P<str>[LuU8]*"(?:\\.|[^"\\\n])*"|[LuU]?'(?:\\.|[^'\\\n])*')
  | (?P<num>\.?[0-9](?:[eEpP][+-]|[A-Za-z0-9_.])*)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<p>%s|.)
''' % '|'.join(re.escape(p) for p in PUNCT), re.S | re.M | re.X)
ASSIGN = {'=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=', '++', '--'}
TELMARK = '@TEL@'
NOTCALL = {'sizeof', 'if', 'while', 'for', 'switch', 'return', '__typeof__', 'typeof', '__builtin_expect'}


def tokens(text):
    return [mt.group(0) for mt in TOKRE.finditer(text) if mt.lastgroup != 'ws']


def _match(t, i):
    """index of the bracket closing t[i]."""
    pair = {'(': ')', '[': ']', '{': '}'}
    o, c, d = t[i], pair[t[i]], 0
    for j in range(i, len(t)):
        if t[j] == o:
            d += 1
        elif t[j] == c:
            d -= 1
            if d == 0:
                return j
    raise ValueError('unbalanced %s at token %d' % (o, i))


def reverse_map(t, fields):
    rev = {(v, f): o for o, (v, f) in fields.items()}
    out, i, n = [], 0, len(t)
    while i < n:
        if i + 2 < n and t[i + 1] == '.' and (t[i], t[i + 2]) in rev and (i == 0 or t[i - 1] not in ('.', '->')):
            out.append(rev[(t[i], t[i + 2])])
            i += 3
            continue
        out.append(t[i])
        i += 1
    return out


def file_units(t):
    """[(a, b)] token spans of the file-scope units: a declaration through its `;`, a
    function definition through its body's `}`."""
    out, a, i, n = [], 0, 0, len(t)
    while i < n:
        x = t[i]
        if x in ('(', '['):
            i = _match(t, i) + 1
            continue
        if x == '{':
            j = _match(t, i)
            # a function body: `)` (or an attribute's `)`) before it and no `=` in the unit
            is_fn = t[i - 1] == ')' and '=' not in t[a:i]
            i = j + 1
            if is_fn:
                out.append((a, i))
                a = i
            continue
        if x == ';':
            out.append((a, i + 1))
            a = i + 1
        i += 1
    if a < n:
        out.append((a, n))
    return out


def declared(t, a, b):
    """names a file-scope declaration unit declares (bracket depth 0, attributes skipped)."""
    names, i = [], a
    while i < b:
        x = t[i]
        if x == '__attribute__' and i + 1 < b and t[i + 1] == '(':
            i = _match(t, i + 1) + 1
            continue
        if x in ('(', '['):
            i = _match(t, i) + 1
            continue
        if x == '{':
            i = _match(t, i) + 1
            continue
        if x == '=':
            # skip the initializer to the next top-level `,` or the end
            j = i + 1
            while j < b and t[j] not in (',', ';'):
                j = _match(t, j) + 1 if t[j] in ('(', '[', '{') else j + 1
            i = j
            continue
        if re.match(r'^[A-Za-z_]\w*$', x) and i + 1 < b and t[i + 1] in (';', ',', '=', '[', '__attribute__'):
            names.append(x)
        i += 1
    return names


def drop_units(t, pred):
    keep, dropped = [], []
    for a, b in file_units(t):
        if pred(t, a, b):
            dropped.append(' '.join(t[a:b])[:100])
        else:
            keep.extend(t[a:b])
    return keep, dropped


def fn_name(t, a, b):
    """the name of a function definition unit, else None."""
    if t[b - 1] != '}':
        return None
    j = a
    while j < b and t[j] != '{':
        j += 1
    k = j - 1
    # back over `)` of attributes / the parameter list to the name before the first `(`
    p = a
    while p < j and t[p] != '(':
        p += 1
    return t[p - 1] if p > a and p < j and re.match(r'^[A-Za-z_]\w*$', t[p - 1]) and k > p else None


def tel_normalise(t, telset):
    """`do { S } while ( 0 )` with S naming a telemetry OLD -> S (its `;` after `while (0)`
    dropped: S ends in its own); an empty one -> the marker (its `;` kept)."""
    out, i, n = [], 0, len(t)
    while i < n:
        if t[i] == 'do' and i + 1 < n and t[i + 1] == '{':
            j = _match(t, i + 1)
            if t[j + 1:j + 6] == ['while', '(', '0', ')', ';'] or t[j + 1:j + 5] == ['while', '(', '0', ')']:
                inner = t[i + 2:j]
                if not inner:
                    out.append(TELMARK)
                    i = j + 5
                    continue
                if telset & set(inner) and (inner[-1] == ';'):
                    out.extend(tel_normalise(inner, telset))
                    i = j + 5
                    if i < n and t[i] == ';':
                        i += 1
                    continue
        out.append(t[i])
        i += 1
    return out


def prepare(t, m, side, tel, mode):
    fields = m['fields']
    notes = []
    if side == 'new':
        t = reverse_map(t, fields)
        types = {st['type'] for st in m['structs'].values()}

        def is_block(tt, a, b):
            return any(tt[k] == 'struct' and tt[k + 1] in types and tt[k + 2] == '{' for k in range(a, min(b, a + 12) - 2))
        t, dr = drop_units(t, is_block)
        notes += ['new: struct definition dropped: %s' % x for x in dr]
    else:
        olds = set(fields)

        def is_decl(tt, a, b):
            if tt[b - 1] != ';':
                return False
            nm = declared(tt, a, b)
            return bool(nm) and set(nm) <= olds and 'extern' not in tt[a:b] and 'typedef' not in tt[a:b]
        t, dr = drop_units(t, is_decl)
        notes += ['ref: declaration dropped: %s' % x for x in dr]
    if mode == 'deletions' and m['telfns']:
        tf = set(m['telfns'])
        t, dr = drop_units(t, lambda tt, a, b: fn_name(tt, a, b) in tf)
        notes += ['%s: telfn definition dropped: %s' % (side, x[:60]) for x in dr]
    if tel:
        telset = {o for o, (v, _) in fields.items() if m['structs'][v]['kind'] == 'tel'}
        t = tel_normalise(t, telset)
    return t, notes


def tel_store(piece, telset):
    """is this `;`-free token list a store to a telemetry OLD?"""
    p = list(piece)
    if p and p[0] in ('++', '--'):
        p = p[1:]
    if not p or p[0] not in telset:
        return False
    i = 1
    while i < len(p):
        if p[i] == '[':
            i = _match(p, i) + 1
        elif p[i] in ('.', '->') and i + 1 < len(p):
            i += 2
        else:
            break
    if piece[0] in ('++', '--'):
        return i == len(p)
    return i < len(p) and p[i] in ASSIGN


def calls_in(piece):
    return sorted({piece[k] for k in range(len(piece) - 1) if piece[k + 1] == '('
                   and re.match(r'^[A-Za-z_]\w*$', piece[k]) and piece[k] not in NOTCALL})


def units(t):
    out, cur = [], []
    for x in t:
        cur.append(x)
        if x in (';', '{', '}'):
            out.append(tuple(cur))
            cur = []
    if cur:
        out.append(tuple(cur))
    return out


def compare(ref, new, m, tel=False, mode='equal'):
    """-> (ok, report lines)"""
    rep = []
    a, na = prepare(ref, m, 'ref', tel, mode)
    b, nb = prepare(new, m, 'new', tel, mode)
    rep += na + nb
    if a == b:
        rep.append('G0R: identical after the reverse rename (%d tokens)' % len(a))
        return True, rep
    telset = {o for o, (v, _) in m['fields'].items() if m['structs'][v]['kind'] == 'tel'}
    ua, ub = units(a), units(b)
    # popular units (`}`, `break ;`) are junk for the unit-level match (a whole TU is
    # ~10^5 units): matches still extend through them; the hunks are diffed by token below
    sm = difflib.SequenceMatcher(None, ua, ub, autojunk=True)
    bad, deleted, calls = [], 0, []
    for op, i1, i2, j1, j2 in sm.get_opcodes():
        if op == 'equal':
            continue
        ta = [x for u in ua[i1:i2] for x in u]
        tb = [x for u in ub[j1:j2] for x in u]
        sm2 = difflib.SequenceMatcher(None, ta, tb, autojunk=False)
        for op2, p1, p2, q1, q2 in sm2.get_opcodes():
            if op2 == 'equal':
                continue
            ra, rb = ta[p1:p2], tb[q1:q2]
            ctx = ' '.join(ta[max(0, p1 - 8):p1])
            if mode != 'deletions' or rb not in ([], [TELMARK]):
                bad.append('%s: ref [%s] -> new [%s]  (after: ...%s)'
                           % (op2, ' '.join(ra)[:120], ' '.join(rb)[:120], ctx[-80:]))
                continue
            pieces, cur = [], []
            for x in ra:
                if x == ';':
                    pieces.append(cur)
                    cur = []
                else:
                    cur.append(x)
            pieces.append(cur)
            for pc in pieces:
                if not pc:
                    continue
                if not tel_store(pc, telset):
                    bad.append('deleted a statement that is not a telemetry store: [%s]  (after: ...%s)'
                               % (' '.join(pc)[:120], ctx[-80:]))
                    continue
                deleted += 1
                cs = calls_in(pc)
                if cs:
                    calls.append('REVIEW deleted store calls %s: [%s]' % (', '.join(cs), ' '.join(pc)[:120]))
    rep += calls
    if bad:
        rep += ['G0R FAIL: %s' % x for x in bad[:40]]
        if len(bad) > 40:
            rep.append('G0R FAIL: ... %d more' % (len(bad) - 40))
        return False, rep
    rep.append('G0R: only telemetry stores deleted (%d statements, %d with a call to review)' % (deleted, len(calls)))
    return True, rep


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--map', required=True)
    ap.add_argument('--tel', action='store_true')
    ap.add_argument('--mode', choices=('equal', 'deletions'), default='equal')
    ap.add_argument('ref')
    ap.add_argument('new')
    a = ap.parse_args(argv)
    if a.mode == 'deletions' and not a.tel:
        print('g0r: --mode deletions needs --tel (a telemetry batch)', file=sys.stderr)
        return 2
    try:
        m = gstruct.read_map(open(a.map, encoding='utf-8').read())
    except gstruct.Refused as e:
        print('g0r: map: %s' % e, file=sys.stderr)
        return 2
    rd = lambda p: open(p, encoding='latin-1').read()
    ok, rep = compare(tokens(rd(a.ref)), tokens(rd(a.new)), m, a.tel, a.mode)
    for r in rep:
        print('    ' + r)
    return 0 if ok else 1


def selftest():
    sys.path.insert(0, os.path.join(HERE, 'tests'))
    import importlib
    return importlib.import_module('source-split_test_gstruct').run()


if __name__ == '__main__':
    if sys.argv[1:2] == ['--selftest']:
        sys.exit(selftest())
    sys.exit(main(sys.argv[1:]))
