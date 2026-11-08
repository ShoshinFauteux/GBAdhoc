#!/usr/bin/env python3
"""split.py -- T4 (builds/mainpsp-map/out/PLAN.md §5, phase P4): the unity split
of psp/main_psp.c into fragments, driven by a generated cutlist.txt.

  split.py cover   [--src main_psp.c]
        crit_cover.awk reproduced: functions in no / several §1.2 ranges, and
        after the plan's assignments (expect 5/69 -> 0/0 on 01067e74).
  split.py gen     [--src main_psp.c] [--carry OLD_CUTLIST] -o cutlist.txt
        generate the cutlist: one row per function and per global (name ->
        module -> fragment -> anchor) plus one FRAG row per fragment (its
        first line as the anchor).  Without --carry, function modules come
        from modules.py (the §1.2 line ranges + the plan's name list; only
        valid on 01067e74's line numbers).  With --carry, every name keeps the
        module OLD_CUTLIST gave it and new names are REFUSED (listed), never
        guessed from stale line numbers; the review files them with
        --assign KEY=MODULE (repeatable).
  split.py split   --src main_psp.c --cutlist cutlist.txt --out DIR
        cut at the anchors (never at line numbers) into DIR/psp/main_psp.c
        (the shell: shell fragments inline + one `#include "fe/X.inc.c"` per
        other fragment, in source order) and DIR/psp/fe/X.inc.c.  REFUSES
        (exit 1, nothing written) on: an item of the source unlisted, listed
        twice, or listed but absent; an anchor not found / out of order /
        not on a cuttable line, or on an own-line `@extract:end` marker
        (gen keeps those with the unit above); a fragment whose #if/#endif do not balance;
        an item that does not lie in the fragment its row names; a `../`
        include inside a fragment; a result whose --inverse is not
        byte-identical to the source.
  split.py inverse --split DIR -o main_psp.c
        expand the shell's fragment includes; byte-identical to the source.
  split.py replay  --split DIR --cutlist cutlist.txt --patch P --out DIR2
        inverse, apply P (a branch's diff of main_psp.c) with `git apply`,
        and split again with the same cutlist (so a new or renamed item is
        refused, never silently filed).
  split.py --selftest   [--src main_psp.c]     mutants, no compiler needed

Exit: 0 ok, 1 refused / check failed, 2 usage.
The fragment directory is psp/fe/ (PLAN §1.2).  This tool never touches the
real psp/main_psp.c: --out must not be the source's own tree.
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import csrc      # noqa: E402
import modules   # noqa: E402

INC_RE = re.compile(r'^#include "fe/([A-Za-z0-9_.]+)\.inc\.c"\n$')
# An own-line `/* @extract:end NAME */` (tools/tests/extract.py; PLAN-P-beta5 P1) closes
# a region whose text lies ABOVE it, so it stays with the unit above: gen never starts a
# unit (or a fragment) with one, and split refuses a FRAG anchor on one.  Without the
# rule the end marker after a fragment's last function opened the next fragment (its
# region then crossed the cut, and the blank line after it moved across too).
END_MARK_LINE = re.compile(r'^[ \t]*/\* @extract:end [A-Za-z0-9_.:-]+ \*/\s*$')


class Refused(Exception):
    pass


# ----------------------------------------------------------------- the model
def esc(s):
    return s.replace('\\', '\\\\').replace('\t', '\\t')


def unesc(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            out.append({'t': '\t', '\\': '\\'}.get(s[i + 1], s[i + 1]))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)


class Model:
    """main_psp.c as lines + items + cuttable lines."""

    def __init__(self, text):
        self.text = text
        self.lines = text.splitlines(keepends=True)       # lines[0] is line 1
        self.p = csrc.parse(text, None, want_refs=False)
        p = self.p
        n = len(self.lines)
        # items: F (function definitions) and G (variable definitions); keyed name#occ
        self.items = []
        occ = collections.Counter()
        for it in p['items']:
            if it['kind'] == 'F':
                k = 'F'
            elif it['kind'] == 'V':
                k = 'G'
            else:
                continue
            occ[(k, it['name'])] += 1
            self.items.append({'kind': k, 'name': it['name'], 'occ': occ[(k, it['name'])],
                               'key': '%s:%s#%d' % (k, it['name'], occ[(k, it['name'])]),
                               'start': it['start'], 'end': it['end']})
        # lines inside a multi-line item (any kind), a block comment or a pp continuation
        self.uncuttable = [False] * (n + 2)
        for it in p['items']:
            for L in range(it['start'] + 1, it['end'] + 1):
                self.uncuttable[L] = True
        off = 0
        starts = []
        for ln in self.lines:
            starts.append(off)
            off += len(ln)
        import bisect
        for a, b in csrc.comment_spans(text):
            k = bisect.bisect_right(starts, a)          # first line starting after a
            while k < n and starts[k] < b:
                self.uncuttable[k + 1] = True
                k += 1
        for L in range(1, n + 1):
            if p['pp_line'][L] and p['pp_kw'][L] is None:
                self.uncuttable[L] = True        # continuation of a directive
        self.line_occ = {}
        seen = collections.Counter()
        for i, ln in enumerate(self.lines, 1):
            seen[ln] += 1
            self.line_occ[i] = seen[ln]
        self.line_count = seen

    def cuttable(self, L):
        return (1 <= L <= len(self.lines) and self.p['pp_depth'][L] == 0 and not self.uncuttable[L])

    def unit_start_ok(self, L):
        """gen may start a unit at L: cuttable, not blank, not an own-line end marker."""
        ln = self.lines[L - 1] if 1 <= L <= len(self.lines) else ''
        return self.cuttable(L) and ln.strip() != '' and not END_MARK_LINE.match(ln)

    def anchor_of(self, L):
        return (self.line_occ[L], self.lines[L - 1].rstrip('\n'))

    def resolve(self, occ, text):
        k = 0
        for i, ln in enumerate(self.lines, 1):
            if ln.rstrip('\n') == text:
                k += 1
                if k == occ:
                    return i
        return None


# ----------------------------------------------------------------- cutlist io
def read_cutlist(path):
    frags, rows = [], []
    for no, ln in enumerate(open(path, encoding='utf-8'), 1):
        ln = ln.rstrip('\n')
        if not ln or ln.startswith('#'):
            continue
        f = ln.split('\t')
        if f[0] == 'FRAG':
            if len(f) != 5:
                raise Refused('cutlist:%d: FRAG row needs 5 fields' % no)
            frags.append({'frag': f[1], 'module': f[2], 'occ': int(f[3]), 'anchor': unesc(f[4]), 'row': no})
        elif f[0] in ('F', 'G'):
            if len(f) != 6:
                raise Refused('cutlist:%d: item row needs 6 fields' % no)
            rows.append({'kind': f[0], 'key': f[0] + ':' + f[1], 'name': f[1].split('#')[0], 'module': f[2],
                         'frag': f[3], 'occ': int(f[4]), 'anchor': unesc(f[5]), 'row': no})
        else:
            raise Refused('cutlist:%d: unknown row type %r' % (no, f[0]))
    return frags, rows


def write_cutlist(path, frags, rows, header):
    with open(path, 'w', encoding='utf-8', newline='\n') as fo:
        for h in header:
            fo.write('# ' + h + '\n')
        fo.write('#\n# FRAG\tfragment\tmodule\tocc\tanchor (the fragment\'s first line, exact; occ = which identical line)\n')
        for f in frags:
            fo.write('FRAG\t%s\t%s\t%d\t%s\n' % (f['frag'], f['module'], f['occ'], esc(f['anchor'])))
        fo.write('#\n# F|G\tname#occ\tmodule (owner)\tfragment (where it sits)\tocc\tanchor (its first line)\n')
        for r in rows:
            fo.write('%s\t%s\t%s\t%s\t%d\t%s\n' % (r['kind'], r['key'].split(':', 1)[1], r['module'], r['frag'],
                                                   r['occ'], esc(r['anchor'])))


# ----------------------------------------------------------------- gen
def gen(m, carry=None, assign=None):
    """-> (frags, rows, report).  Raises Refused on unassignable items."""
    items = m.items
    fmods = {}
    unresolved = []
    if carry is not None:
        _, crow = carry
        cm = {r['key']: r['module'] for r in crow}
        for k_, v_ in (assign or {}).items():      # review decisions for new items
            if v_ not in modules.MODULE_ORDER:
                raise Refused('--assign %s: unknown module %r' % (k_, v_))
            cm[k_] = v_
        new = [it['key'] for it in items if it['key'] not in cm]
        if new:
            raise Refused('carry: %d item(s) not in the old cutlist (assign them by review): %s'
                          % (len(new), ', '.join(new)))
        for it in items:
            fmods[it['key']] = cm[it['key']]
    else:
        for it in items:
            if it['kind'] == 'F':
                mod, how = modules.function_module(it['name'], it['start'])
                if mod is None:
                    unresolved.append('%s (L:%d, %s)' % (it['key'], it['start'], how))
                fmods[it['key']] = mod
            else:
                mod, how = modules.owner_of({'name': it['name'], 'extern': False})
                if mod is None:
                    unresolved.append('%s (L:%d, %s)' % (it['key'], it['start'], how))
                fmods[it['key']] = mod
        if unresolved:
            raise Refused('%d item(s) with no module: %s' % (len(unresolved), '; '.join(unresolved)))

    # units: maximal runs of items with no cuttable line between them
    # the head (module info, the #include list) is its own shell unit: cut at the
    # first cuttable line after the last #include that precedes the first item
    units = []
    cutline = {0: 1}      # unit index -> first line of the unit
    last_inc = 0
    for L in range(1, items[0]['start']):
        if m.p['pp_kw'][L] == 'include':
            last_inc = L
    head_cut = None
    if last_inc:
        for L in range(last_inc + 1, items[0]['start'] + 1):
            if m.unit_start_ok(L):
                head_cut = L
                break
    if head_cut and head_cut > 1:
        units.append([])
        cutline[1] = head_cut
    cur = [items[0]]
    for a, b in zip(items, items[1:]):
        cands = [L for L in range(a['end'] + 1, b['start'] + 1) if m.unit_start_ok(L)]
        # the shell's head range (L:1-160: module info, thread attributes, pads)
        # keeps its glue: prefer a cut past it when the gap allows one
        late = [L for L in cands if L > modules.SHELL_RANGES[0][1]]
        cand = (late or cands or [None])[0]
        if cand is None:
            cur.append(b)
        else:
            units.append(cur)
            cutline[len(units)] = cand
            cur = [b]
    units.append(cur)

    def unit_module(u):
        w = collections.Counter()
        for it in u:
            if it['kind'] == 'F':
                w[fmods[it['key']]] += it['end'] - it['start'] + 1
        if not w:
            return None
        return sorted(w.items(), key=lambda kv: (-kv[1], modules.MODULE_ORDER.index(kv[0])
                                                 if kv[0] in modules.MODULE_ORDER else 99))[0][0]
    umod = [unit_module(u) for u in units]
    # a unit holding a `../` include cannot leave psp/ (it would resolve from
    # psp/fe/): it stays in the shell, and its functions are reported as pinned
    for i in range(len(units)):
        a_ = cutline[i]
        b_ = cutline[i + 1] - 1 if i + 1 < len(units) else len(m.lines)
        if any(re.match(r'\s*#\s*include\s*"\.\.?/', m.lines[L - 1]) for L in range(a_, b_ + 1)):
            umod[i] = 'shell'
    # globals-only units: inside the shell's head range -> shell; else the next function unit's module
    for i, u in enumerate(units):
        if not u:
            umod[i] = 'shell'          # the head
        elif umod[i] is None and all(it['end'] <= modules.SHELL_RANGES[0][1] for it in u) and carry is None:
            umod[i] = 'shell'
    if carry is not None:
        # carried: a globals-only unit keeps the fragment module its first item had
        old_frag_mod = {f['frag']: f['module'] for f in carry[0]}
        oldfrag = {r['key']: r['frag'] for r in carry[1]}
        for i, u in enumerate(units):
            if umod[i] is None:
                umod[i] = old_frag_mod.get(oldfrag.get(u[0]['key']))
    nxt = None
    for i in range(len(units) - 1, -1, -1):
        if umod[i] is None:
            umod[i] = nxt
        else:
            nxt = umod[i]
    prev = 'shell'
    for i in range(len(units)):
        if umod[i] is None:
            umod[i] = prev
        prev = umod[i]

    # fragments: merge consecutive units of one module
    fr = []     # [module, first_line, [items]]
    for i, u in enumerate(units):
        if fr and fr[-1][0] == umod[i]:
            fr[-1][2].extend(u)
        else:
            fr.append([umod[i], cutline[i], list(u)])
    per = collections.Counter(f[0] for f in fr)
    seen = collections.Counter()
    frags, where = [], {}
    for mod, first, its in fr:
        seen[mod] += 1
        if per[mod] == 1:
            name = mod
        else:
            k = seen[mod] - 1
            name = '%s.%s' % (mod, chr(ord('a') + k) if k < 26 else 'z%02d' % k)
        occ, anc = m.anchor_of(first)
        frags.append({'frag': name, 'module': mod, 'occ': occ, 'anchor': anc, 'first': first, 'n': len(its)})
        for it in its:
            where[it['key']] = name
    rows = []
    pinned = []
    for it in items:
        occ, anc = m.anchor_of(it['start'])
        mod = fmods[it['key']]
        rows.append({'kind': it['kind'], 'key': it['key'], 'name': it['name'], 'module': mod,
                     'frag': where[it['key']], 'occ': occ, 'anchor': anc})
        fmod = next(f['module'] for f in frags if f['frag'] == where[it['key']])
        if it['kind'] == 'F' and fmod != mod:
            pinned.append((it['key'], it['start'], mod, where[it['key']]))
    rep = {'units': len(units), 'frags': len(frags), 'per_module': dict(per), 'pinned': pinned,
           'functions': sum(1 for it in items if it['kind'] == 'F'),
           'globals': sum(1 for it in items if it['kind'] == 'G')}
    return frags, rows, rep


# ----------------------------------------------------------------- split
def do_split(m, frags, rows):
    """-> {fragname: text}, shell text, report.  Raises Refused."""
    errs = []
    # 1. items: unlisted / doubly listed / absent
    src_keys = collections.Counter(it['key'] for it in m.items)
    row_keys = collections.Counter(r['key'] for r in rows)
    dup = sorted(k for k, c in row_keys.items() if c > 1)
    unl = sorted(k for k in src_keys if k not in row_keys)
    absent = sorted(k for k in row_keys if k not in src_keys)
    if dup:
        errs.append('listed twice: ' + ', '.join(dup))
    if unl:
        errs.append('unlisted (in the source, not in the cutlist): ' + ', '.join(unl))
    if absent:
        errs.append('listed but not in the source: ' + ', '.join(absent))
    # module names are checked: a typo ('powr') used to pass silently, and a
    # FRAG typo of 'shell' turned shell glue into a fragment (review 2026-11-05)
    badm = sorted({'%s %s' % (r['key'], r['module']) for r in rows if r['module'] not in modules.MODULE_ORDER}
                  | {'FRAG %s %s' % (f['frag'], f['module']) for f in frags if f['module'] not in modules.MODULE_ORDER})
    if badm:
        errs.append('unknown module: ' + ', '.join(badm))
    badf = sorted('FRAG %s (module %s)' % (f['frag'], f['module']) for f in frags
                  if f['frag'].split('.')[0] != f['module'])
    if badf:
        errs.append('fragment name does not start with its module: ' + ', '.join(badf))
    fnames = [f['frag'] for f in frags]
    if len(set(fnames)) != len(fnames):
        errs.append('fragment named twice: ' + ', '.join(sorted(k for k, c in collections.Counter(fnames).items() if c > 1)))
    if errs:
        raise Refused('; '.join(errs))
    # 2. fragment anchors
    cuts = []
    for f in frags:
        L = m.resolve(f['occ'], f['anchor'])
        if L is None:
            errs.append('FRAG %s: anchor not found (occ %d): %r' % (f['frag'], f['occ'], f['anchor']))
            continue
        if cuts and L <= cuts[-1][0]:
            errs.append('FRAG %s: anchor L:%d is not after the previous fragment (L:%d)' % (f['frag'], L, cuts[-1][0]))
        if L != 1 and not m.cuttable(L):
            why = ('inside an open #if (depth %d)' % m.p['pp_depth'][L]) if m.p['pp_depth'][L] else 'inside an item, comment or directive'
            errs.append('FRAG %s: cut at L:%d is %s' % (f['frag'], L, why))
        elif END_MARK_LINE.match(m.lines[L - 1]):
            errs.append('FRAG %s: anchor L:%d is an own-line @extract:end marker (it closes the region above and '
                        'stays with the previous fragment; regenerate the cutlist with split.py gen)' % (f['frag'], L))
        cuts.append((L, f))
    if errs:
        raise Refused('; '.join(errs))
    if not cuts or cuts[0][0] != 1:
        raise Refused('the first FRAG must anchor line 1')
    bounds = []
    for i, (L, f) in enumerate(cuts):
        end = cuts[i + 1][0] - 1 if i + 1 < len(cuts) else len(m.lines)
        bounds.append((L, end, f))
    # 3. #if balance per fragment (independent of the cut rule above)
    for L, end, f in bounds:
        d = 0
        for k in range(L, end + 1):
            kw = m.p['pp_kw'][k]
            if kw in ('if', 'ifdef', 'ifndef'):
                d += 1
            elif kw == 'endif':
                d -= 1
            elif kw in ('else', 'elif') and d == 0:
                d = -1
            if d < 0:
                break
        if d != 0:
            errs.append('FRAG %s (L:%d-%d): unbalanced #if/#endif (net %+d)' % (f['frag'], L, end, d))
    if m.p['final_depth'] != 0:
        errs.append('the source itself ends inside an open #if (depth %d)' % m.p['final_depth'])
    # 4. every item in its listed fragment, its anchor current
    rk = {r['key']: r for r in rows}
    for it in m.items:
        r = rk[it['key']]
        holder = [f for (L, end, f) in bounds if L <= it['start'] and it['end'] <= end]
        if not holder:
            errs.append('%s (L:%d-%d) straddles a cut' % (it['key'], it['start'], it['end']))
            continue
        if holder[0]['frag'] != r['frag']:
            errs.append('%s sits in %s, the cutlist says %s' % (it['key'], holder[0]['frag'], r['frag']))
        occ, anc = m.anchor_of(it['start'])
        if anc != r['anchor']:
            errs.append('%s: stale anchor (the line now reads %r)' % (it['key'], anc))
    # 5. includes a fragment would resolve differently
    for L, end, f in bounds:
        if f['module'] == 'shell':
            continue
        for k in range(L, end + 1):
            mm = re.match(r'\s*#\s*include\s*"([^"]+)"', m.lines[k - 1])
            if mm and (mm.group(1).startswith('../') or mm.group(1).startswith('./')):
                errs.append('FRAG %s L:%d: relative include %r would resolve from psp/fe/' % (f['frag'], k, mm.group(1)))
    if errs:
        raise Refused('; '.join(errs))
    out = {}
    shell = []
    for L, end, f in bounds:
        body = ''.join(m.lines[L - 1:end])
        if f['module'] == 'shell':
            shell.append(body)
        else:
            out[f['frag']] = body
            shell.append('#include "fe/%s.inc.c"\n' % f['frag'])
    shell_text = ''.join(shell)
    if inverse_text(shell_text, out) != m.text:
        raise Refused('internal: the inverse of the split is not byte-identical to the source')
    return out, shell_text, {'fragments': len(bounds), 'files': len(out), 'shell_lines': shell_text.count('\n')}


def inverse_text(shell_text, frags):
    out = []
    for ln in shell_text.splitlines(keepends=True):
        mm = INC_RE.match(ln)
        if mm:
            if mm.group(1) not in frags:
                raise Refused('inverse: fragment %s.inc.c missing' % mm.group(1))
            out.append(frags[mm.group(1)])
        else:
            out.append(ln)
    return ''.join(out)


def write_split(outdir, frags, shell_text):
    psp = os.path.join(outdir, 'psp')
    fe = os.path.join(psp, 'fe')
    if os.path.isdir(fe):
        shutil.rmtree(fe)
    os.makedirs(fe, exist_ok=True)
    for k, v in frags.items():
        with open(os.path.join(fe, k + '.inc.c'), 'w', encoding='utf-8', newline='') as fo:
            fo.write(v)
    with open(os.path.join(psp, 'main_psp.c'), 'w', encoding='utf-8', newline='') as fo:
        fo.write(shell_text)


def read_split(d):
    psp = os.path.join(d, 'psp')
    shell = open(os.path.join(psp, 'main_psp.c'), encoding='utf-8', newline='').read()
    frags = {}
    fe = os.path.join(psp, 'fe')
    for fn in sorted(os.listdir(fe)):
        if fn.endswith('.inc.c'):
            frags[fn[:-6]] = open(os.path.join(fe, fn), encoding='utf-8', newline='').read()
    return shell, frags


def guard_out(outdir, src):
    so = os.path.realpath(os.path.dirname(os.path.abspath(src)))
    oo = os.path.realpath(os.path.join(os.path.abspath(outdir), 'psp'))
    if so == oo:
        raise Refused('--out would overwrite the source tree (%s); split is dry-run only' % so)


def read_src(p):
    return open(p, encoding='utf-8', newline='').read()


# ----------------------------------------------------------------- replay
def replay(split_dir, cutlist, patch, outdir):
    shell, fr = read_split(split_dir)
    text = inverse_text(shell, fr)
    ptxt = open(patch, encoding='utf-8', newline='').read()
    mm = re.search(r'^\+\+\+ b/(\S*main_psp\.c)\s*$', ptxt, re.M)
    if not mm:
        raise Refused('replay: the patch touches no main_psp.c')
    others = [x for x in re.findall(r'^\+\+\+ b/(\S+)', ptxt, re.M) if not x.endswith('main_psp.c')]
    rel = mm.group(1)
    tmp = tempfile.mkdtemp(prefix='split-replay-')
    try:
        dst = os.path.join(tmp, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'w', encoding='utf-8', newline='') as fo:
            fo.write(text)
        r = subprocess.run(['git', '-c', 'core.autocrlf=false', '-c', 'core.eol=lf', 'apply', '--include=' + rel, os.path.abspath(patch)], cwd=tmp,
                           capture_output=True, text=True)
        if r.returncode:
            raise Refused('replay: git apply failed: ' + r.stderr.strip())
        new = read_src(dst)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    m = Model(new)
    frags, rows = read_cutlist(cutlist)
    out, shell_text, rep = do_split(m, frags, rows)
    write_split(outdir, out, shell_text)
    rep['ignored_files'] = others
    return rep


# ----------------------------------------------------------------- cli
def default_src():
    return os.path.normpath(os.path.join(HERE, '..', '..', 'psp', 'main_psp.c'))


def cmd_cover(a):
    m = Model(read_src(a.src))
    none, multi = modules.cover(m.p['funcs'])
    print('cover (PLAN §1.2 ranges, crit_cover.awk; main excluded): %d functions, %d in no range, %d in 2-3 ranges'
          % (len(m.p['funcs']) - 1, len(none), len(multi)))
    for f in none:
        print('  NONE  L:%-6d %s' % (f['start'], f['name']))
    if a.verbose:
        for f, h in multi:
            print('  MULTI L:%-6d %-28s %s' % (f['start'], f['name'], ','.join(h)))
    how = collections.Counter()
    un = []
    for f in m.p['funcs']:
        mod, h = modules.function_module(f['name'], f['start'])
        how[h] += 1
        if mod is None:
            un.append(f['name'])
    stale = sorted(n for n in modules.NAMES if n not in {f['name'] for f in m.p['funcs']})
    lm = sum(1 for f, h in multi if set(h) >= {'link', 'misc'})
    print('after the plan\'s assignments: %d unassigned, 0 doubly assigned (one module per function by construction); '
          'by name %d, by single range %d, by the link/misc rule %d (the plan says 27 link/misc: %d)'
          % (len(un), how['name'], how['range'], how['rule:link/misc'], lm))
    if stale:
        print('  stale plan names (not functions in this source): ' + ', '.join(stale))
    return 0 if not un and not stale else 1


def cmd_gen(a):
    m = Model(read_src(a.src))
    carry = read_cutlist(a.carry) if a.carry else None
    assign = {}
    for x in a.assign or []:
        k_, _, v_ = x.partition('=')
        assign[k_ if ':' in k_ else 'F:%s#1' % k_] = v_
    if assign and not carry:
        raise Refused('--assign needs --carry')
    frags, rows, rep = gen(m, carry, assign)
    import hashlib
    sha = hashlib.sha256(m.text.encode('utf-8')).hexdigest()
    hdr = ['cutlist.txt -- GENERATED by gpsp/tools/refactor/split.py gen (T4, PLAN.md §5).  Regenerate, never hand-edit',
           'except to move a row in the cutlist review (then split.py split re-checks everything).',
           'source: %s  sha256 %s  (%d lines)' % (a.label or os.path.basename(a.src), sha, len(m.lines)),
           'modules: %s' % ('carried from ' + a.carry if a.carry else 'modules.py (PLAN §1.2 ranges + name list)'),
           '%d functions, %d globals, %d fragments (%d units), %d function(s) pinned into another module\'s fragment by #if'
           % (rep['functions'], rep['globals'], rep['frags'], rep['units'], len(rep['pinned']))]
    write_cutlist(a.o, frags, rows, hdr)
    print('\n'.join(hdr[2:]))
    print('fragments per module: ' + ', '.join('%s %d' % (k, v) for k, v in sorted(rep['per_module'].items())))
    for p in rep['pinned']:
        print('  pinned: %s L:%d owner %s sits in %s' % p)
    # the same cutlist must split this source
    do_split(m, *read_cutlist(a.o))
    print('wrote %s; split check OK' % a.o)
    return 0


def cmd_split(a):
    guard_out(a.out, a.src)
    m = Model(read_src(a.src))
    frags, rows = read_cutlist(a.cutlist)
    out, shell_text, rep = do_split(m, frags, rows)
    write_split(a.out, out, shell_text)
    print('split OK: %d fragments (%d files in psp/fe/, shell %d lines) -> %s' % (rep['fragments'], rep['files'],
                                                                                 rep['shell_lines'], a.out))
    return 0


def cmd_inverse(a):
    shell, fr = read_split(a.split)
    t = inverse_text(shell, fr)
    with open(a.o, 'w', encoding='utf-8', newline='') as fo:
        fo.write(t)
    print('inverse: %d lines -> %s' % (t.count('\n'), a.o))
    return 0


def cmd_replay(a):
    rep = replay(a.split, a.cutlist, a.patch, a.out)
    print('replay OK: %d fragments -> %s%s' % (rep['fragments'], a.out,
                                             ('; other files in the patch ignored: ' + ', '.join(rep['ignored_files']))
                                             if rep['ignored_files'] else ''))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--src', dest='top_src', default=default_src())
    sub = ap.add_subparsers(dest='cmd')
    c = sub.add_parser('cover')
    c.add_argument('--src', default=None)
    c.add_argument('-v', '--verbose', action='store_true')
    g = sub.add_parser('gen')
    g.add_argument('--src', default=None)
    g.add_argument('--carry')
    g.add_argument('--assign', action='append', help='KEY=MODULE for an item new since the carried cutlist '
                   '(KEY like F:name#1 or G:name#1; a bare name means F:name#1)')
    g.add_argument('--label')
    g.add_argument('-o', required=True)
    s = sub.add_parser('split')
    s.add_argument('--src', default=None)
    s.add_argument('--cutlist', required=True)
    s.add_argument('--out', required=True)
    i = sub.add_parser('inverse')
    i.add_argument('--split', required=True)
    i.add_argument('-o', required=True)
    r = sub.add_parser('replay')
    r.add_argument('--split', required=True)
    r.add_argument('--cutlist', required=True)
    r.add_argument('--patch', required=True)
    r.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    if getattr(a, 'src', None) is None:
        a.src = a.top_src
    if a.selftest:
        sys.path.insert(0, os.path.join(HERE, 'tests'))
        import importlib
        t = importlib.import_module('source-split_test_split')
        return t.run(a.src)
    try:
        if a.cmd == 'cover':
            return cmd_cover(a)
        if a.cmd == 'gen':
            return cmd_gen(a)
        if a.cmd == 'split':
            return cmd_split(a)
        if a.cmd == 'inverse':
            return cmd_inverse(a)
        if a.cmd == 'replay':
            return cmd_replay(a)
    except Refused as e:
        print('REFUSED: ' + str(e), file=sys.stderr)
        return 1
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
