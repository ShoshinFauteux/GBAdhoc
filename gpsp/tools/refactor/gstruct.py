#!/usr/bin/env python3
"""gstruct.py -- C5-0 (builds/health-out/design/C-5.md §2.1, §3): move a reviewed set of
main_psp.c file-scope globals into one module struct, by a RENAME MAP.  Called as
`globals.py struct MAP --src INVERSE -o OUT [--tel]`; g0r.py (gate G0R) reads the same map.

THE MAP (one file per batch, reviewed in the commit that applies it):

    # comment
    struct VAR TYPE KIND [SECTION]   a struct object: `static struct TYPE {...} VAR;`
                                     KIND  st   state: members WITHOUT an initializer (.bss)
                                           cfg  defaults: members WITH an initializer (.data)
                                           tel  telemetry: no initializer; the block is
                                                wrapped in #ifdef GPSP_KEEP_TELEMETRY
                                     SECTION  the members' common section(...) (e.g. .sbss),
                                              or nothing
    OLD VAR.FIELD                    the global OLD becomes member FIELD of VAR
    telfn NAME                       (g0r.py only) a function the telemetry batch compiles
                                     out of the release whole (lat_census): its body is
                                     not compared in the release
    telobj NAME                      (g0r.py only) a telemetry object that stays a standalone
                                     definition under #ifdef GPSP_KEEP_TELEMETRY (one a
                                     .bss struct cannot hold: an initializer, ~0u min
                                     trackers); its stores count as telemetry stores

WHAT `struct` DOES on a single-file source (`split.py inverse` output; split it again
after): each OLD's definition line(s) are deleted; the struct block is written where the
struct's first member (in source order) was defined; every code token OLD becomes
VAR.FIELD.  A member keeps its declaration text: type, `volatile`, array dims, its
trailing comment.  `static` and the section attribute move to the object.  For cfg the
initializers become a designated initializer list in the same order.

IT REFUSES (exit 1, nothing written), per C-5.md §2.1-2.2 and the rename rules of
globals.py rename (the same checks, applied to every OLD):
  - OLD is not a file-scope definition of the file, is defined twice (an #if arm each), or
    is also declared extern in the file
  - OLD is exported ABI / a weak-override hook (globals.EXPORTED, gpsp_*), or not static
  - OLD is const, or a macro-global (a #define of the same name in another arm)
  - OLD carries an attribute other than section(...) (aligned: a struct cannot keep it per
    member), or its section differs from the struct's SECTION
  - st/tel member WITH an initializer, cfg member WITHOUT one (never move bytes between
    .bss and .data: C-5.md §2.1)
  - the members' #if contexts differ (the block inherits the first member's)
  - OLD's declaration shares a line with another item or declares several names
  - OLD is shadowed by a local/parameter, used in a preprocessor line, used after `.`/`->`
  - OLD is used inside fe_evt / LAB_HOOK / LAB_FFP / FE_LP_SET arguments, unless --tel
    (then the lab and harness G0R are the check: C-5.md §3 C5-0)
  - VAR or TYPE already names something, or a map row is malformed

    gstruct.py --selftest        tests/source-split_test_gstruct.py
"""
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import csrc  # noqa: E402

KINDS = ('st', 'cfg', 'tel')
SEC_ATTR = re.compile(r'__attribute__\s*\(\(\s*(?:section|__section__)\s*\(\s*"[^"]*"\s*\)\s*\)\)')


class Refused(Exception):
    pass


def read_map(text):
    """-> dict(structs=OrderedDict(var -> dict(type, kind, section)),
               fields=OrderedDict(old -> (var, field)), telfns=[names], telobjs=[names])."""
    structs, fields, telfns, telobjs = collections.OrderedDict(), collections.OrderedDict(), [], []
    for k, ln in enumerate(text.split('\n'), 1):
        s = ln.split('#', 1)[0].strip()
        if not s:
            continue
        w = s.split()
        if w[0] == 'struct':
            if len(w) not in (4, 5) or w[3] not in KINDS:
                raise Refused('map line %d: want `struct VAR TYPE st|cfg|tel [SECTION]`: %r' % (k, ln))
            if w[1] in structs:
                raise Refused('map line %d: struct %s twice' % (k, w[1]))
            if any(st['type'] == w[2] for st in structs.values()):
                raise Refused('map line %d: type %s twice' % (k, w[2]))
            structs[w[1]] = dict(type=w[2], kind=w[3], section=w[4] if len(w) == 5 else None)
        elif w[0] == 'telfn':
            if len(w) != 2:
                raise Refused('map line %d: want `telfn NAME`' % k)
            telfns.append(w[1])
        elif w[0] == 'telobj':
            if len(w) != 2:
                raise Refused('map line %d: want `telobj NAME`' % k)
            telobjs.append(w[1])
        else:
            m = re.match(r'^([A-Za-z_]\w*)\.([A-Za-z_]\w*)$', w[1]) if len(w) == 2 else None
            if not m or not re.match(r'^[A-Za-z_]\w*$', w[0]):
                raise Refused('map line %d: want `OLD VAR.FIELD`: %r' % (k, ln))
            if w[0] in fields:
                raise Refused('map line %d: %s mapped twice' % (k, w[0]))
            if m.group(1) not in structs:
                raise Refused('map line %d: struct %s is not declared above' % (k, m.group(1)))
            if any(f == (m.group(1), m.group(2)) for f in fields.values()):
                raise Refused('map line %d: member %s.%s twice' % (k, m.group(1), m.group(2)))
            fields[w[0]] = (m.group(1), m.group(2))
    for var in structs:
        if not any(v == var for v, _ in fields.values()):
            raise Refused('struct %s has no member' % var)
    return dict(structs=structs, fields=fields, telfns=telfns, telobjs=telobjs)


def _top_eq(s):
    """index of the initializer's `=` at bracket depth 0 (not ==, <=, ...), or -1."""
    d = 0
    for i, c in enumerate(s):
        if c in '([{':
            d += 1
        elif c in ')]}':
            d -= 1
        elif c == '=' and d == 0 and s[i + 1:i + 2] != '=' and s[i - 1:i] not in ('=', '!', '<', '>'):
            return i
    return -1


def apply(text, m, root=None, tel=False):
    """-> (new_text, report lines).  Raises Refused (see the module doc)."""
    sys.path.insert(0, HERE)
    import globals as G
    d = csrc.parse(text, root)
    clean = d['clean']
    if len(clean) != len(text):
        raise Refused('internal: csrc clean view is not position-aligned with the source')
    lstart = [0]
    for ln in text.split('\n'):
        lstart.append(lstart[-1] + len(ln) + 1)
    structs, fields = m['structs'], m['fields']
    for var, st in structs.items():
        for nm in (var, st['type']):
            if re.search(r'\b%s\b' % re.escape(nm), clean) or nm in d['defines']:
                raise Refused('%s already names something in this file' % nm)
    defs = {}
    for old, (var, fld) in fields.items():
        gl = [v for v in d['vars'] if v['name'] == old]
        own = [v for v in gl if not v['extern']]
        if not own:
            raise Refused('%s is not a file-scope global defined in this file' % old)
        if len(own) > 1:
            raise Refused('%s is defined %d times (an #if arm each): not one object' % (old, len(own)))
        if len(own) != len(gl):
            raise Refused('%s is also declared extern in this file' % old)
        v = own[0]
        st = structs[var]
        if old in G.EXPORTED or old.startswith('gpsp_'):
            raise Refused('%s is exported ABI / a weak-override hook: never a member' % old)
        if not v['static']:
            raise Refused('%s is not static (external linkage): not a member' % old)
        if v['const']:
            raise Refused('%s is const: a const table stays a standalone object (C-5.md §2.2)' % old)
        if old in d['defines']:
            raise Refused('%s is a macro-global (#define %s in another arm): stays standalone' % (old, old))
        if v.get('agg'):
            raise Refused('%s has an anonymous aggregate type: not a member' % old)
        for a in v['attrs']:
            if not re.search(r'\b(?:section|__section__)\s*\(', a) or re.search(r'aligned|packed|used|weak', a):
                raise Refused('%s carries attribute %s: only section(...) can move to the object' % (old, a.strip()))
        if v['section'] != st['section']:
            raise Refused('%s is in section %s, struct %s in %s' % (old, v['section'], var, st['section']))
        if st['kind'] == 'cfg' and not v['init']:
            raise Refused('%s has no initializer: a cfg member must have one (.data stays .data)' % old)
        if st['kind'] != 'cfg' and v['init']:
            raise Refused('%s has an initializer: it belongs in a cfg struct, not %s (%s)' % (old, var, st['kind']))
        if old in d['shadow']:
            raise Refused('%s is shadowed by a local or parameter in %s'
                          % (old, ', '.join(sorted(set(d['shadow'][old])))))
        others = [it for it in d['items'] if it.get('name') != old
                  and not (it['end'] < v['line'] or it['start'] > v['end'])]
        if others:
            raise Refused('%s: its declaration (L:%d-%d) shares a line with another item' % (old, v['line'], v['end']))
        if sum(1 for x in d['vars'] if x['line'] == v['line']) != 1:
            raise Refused('%s: L:%d declares several names' % (old, v['line']))
        defs[old] = v
    # one #if context per struct
    for var in structs:
        conds = {defs[o]['cond'] for o, (vv, _) in fields.items() if vv == var}
        if len(conds) != 1:
            raise Refused('struct %s: its members are in different #if contexts: %s' % (var, sorted(conds)))
    bc = csrc.blank_comments_strings(text).split('\n')
    for old in fields:
        in_pp = [k + 1 for k, ln in enumerate(bc) if d['pp_line'][k + 1] and re.search(r'\b%s\b' % re.escape(old), ln)]
        if in_pp:
            raise Refused('%s occurs in a preprocessor directive: L:%s' % (old, ','.join(map(str, in_pp))))
    # every code token, minus the definitions themselves
    dspan = {old: (lstart[v['line'] - 1], lstart[v['end']]) for old, v in defs.items()}
    # A trailing comment that goes on over the next lines belongs to the declaration: its
    # lines go with it (tier C C5-3: g_clock_fake's three-line comment; without this the
    # continuation lines stayed behind, an unopened comment that hid the rest of the file).
    for old, (a, b) in list(dspan.items()):
        seg = text[a:b]
        o = seg.rfind('/*')
        if o >= 0 and seg.find('*/', o) < 0:
            c = text.find('*/', b)
            if c < 0:
                raise Refused('%s: its trailing comment is never closed' % old)
            nl = text.find('\n', c)
            dspan[old] = (a, len(text) if nl < 0 else nl + 1)
    edits = []          # (start, end, replacement)
    nuse = collections.Counter()
    for old, (var, fld) in fields.items():
        a0, b0 = dspan[old]
        for mt in re.finditer(r'\b%s\b' % re.escape(old), clean):
            p = mt.start()
            if a0 <= p < b0:
                continue
            if re.search(r'(\.|->)\s*$', clean[max(0, p - 4):p]):
                raise Refused('%s is used after `.`/`->` at L:%d (a member of the same name)' % (old, d['lineof'](p)))
            if not tel:
                for (sa, sb, kind) in d['spans']:
                    if sa <= p <= sb:
                        raise Refused('%s occurs inside %s(...) at L:%d, which the release compiles out '
                                      '(r6 B3): use --tel and the lab/harness G0R' % (old, kind, d['lineof'](p)))
            edits.append((p, mt.end(), '%s.%s' % (var, fld)))
            nuse[old] += 1
    # the members and the blocks
    members = collections.defaultdict(list)
    inits = collections.defaultdict(list)
    for old, (var, fld) in fields.items():
        v = defs[old]
        a, b = dspan[old]
        seg, cseg = text[a:b], clean[a:b]
        k = cseg.rfind(';')
        if k < 0:
            raise Refused('%s: no `;` in its declaration' % old)
        code, comment = seg[:k], seg[k + 1:].strip()
        code = SEC_ATTR.sub(' ', code)
        code = re.sub(r'^\s*static\s+', '', code)
        e = _top_eq(code)
        init = None
        if e >= 0:
            code, init = code[:e], code[e + 1:].strip()
        code = ' '.join(code.split())
        code2, n = re.subn(r'\b%s\b' % re.escape(old), fld, code)
        if n != 1:
            raise Refused('%s: its declarator is not found once in %r' % (old, code))
        head = '   %s;' % code2
        if '\n' in comment:         # a multi-line trailing comment: re-aligned under its start
            cl = comment.split('\n')
            comment = cl[0] + ''.join('\n' + ' ' * (len(head) + 4) + x.strip() for x in cl[1:])
        members[var].append((v['line'], '%s%s' % (head, ('   ' + comment) if comment else '')))
        if init is not None:
            inits[var].append('   .%s = %s,' % (fld, ' '.join(init.split())))
    first = {}
    for old, (var, fld) in fields.items():
        if var not in first or defs[old]['line'] < defs[first[var]]['line']:
            first[var] = old
    for var, st in structs.items():
        body = [x for _, x in sorted(members[var])]
        tail = ' __attribute__((section("%s")))' % st['section'] if st['section'] else ''
        blk = ['static struct %s' % st['type'], '{'] + body
        if st['kind'] == 'cfg':
            blk += ['} %s%s =' % (var, tail), '{'] + inits[var] + ['};']
        else:
            blk += ['} %s%s;' % (var, tail)]
        if st['kind'] == 'tel':
            blk = ['#ifdef GPSP_KEEP_TELEMETRY   /* C-5 telemetry: absent from the release */'] + blk + ['#endif']
        for old, (vv, _) in fields.items():
            if vv != var:
                continue
            a, b = dspan[old]
            edits.append((a, b, '\n'.join(blk) + '\n' if old == first[var] else ''))
    edits.sort()
    for x, y in zip(edits, edits[1:]):
        if x[1] > y[0]:
            raise Refused('internal: overlapping edits at offset %d' % y[0])
    out, last = [], 0
    for a, b, r in edits:
        out.append(text[last:a])
        out.append(r)
        last = b
    out.append(text[last:])
    rep = ['struct %s (%s, %s%s): %d members, %d uses renamed'
           % (var, st['type'], st['kind'], ', ' + st['section'] if st['section'] else '',
              len(members[var]), sum(nuse[o] for o, (vv, _) in fields.items() if vv == var))
           for var, st in structs.items()]
    return ''.join(out), rep


def main(argv):
    """globals.py struct MAP --src SRC -o OUT [--tel] [--root GPSP]"""
    import argparse
    ap = argparse.ArgumentParser(prog='globals.py struct')
    ap.add_argument('map')
    ap.add_argument('--src', required=True)
    ap.add_argument('--root', default=None)
    ap.add_argument('--tel', action='store_true')
    ap.add_argument('-o', required=True)
    a = ap.parse_args(argv)
    if os.path.abspath(a.o) == os.path.abspath(a.src):
        print('REFUSED: -o is the source file; struct writes a copy only', file=sys.stderr)
        return 1
    text = open(a.src, encoding='utf-8', newline='').read()
    if re.search(r'^\s*#\s*include\s+"fe/[^"]+\.inc\.c"', text, re.M):
        print('REFUSED: %s is a split shell; run struct on `split.py inverse` output' % a.src, file=sys.stderr)
        return 1
    try:
        m = read_map(open(a.map, encoding='utf-8').read())
        t2, rep = apply(text, m, a.root, a.tel)
    except Refused as e:
        print('REFUSED: ' + str(e), file=sys.stderr)
        return 1
    open(a.o, 'w', encoding='utf-8', newline='').write(t2)
    for r in rep:
        print(r)
    print('-> %s' % a.o)
    return 0


if __name__ == '__main__':
    if sys.argv[1:2] == ['--selftest']:
        sys.path.insert(0, os.path.join(HERE, 'tests'))
        import importlib
        sys.exit(importlib.import_module('source-split_test_gstruct').run())
    sys.exit(main(sys.argv[1:]))
