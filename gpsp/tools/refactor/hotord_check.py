#!/usr/bin/env python3
"""hotord_check.py -- T5 (builds/mainpsp-map/out/PLAN.md §5): is the pinned order
still true of this build?

    hotord_check.py --build DIR [--order F] [--ord F] [--sizes fail|warn|off] [--json OUT]
    hotord_check.py --elf ELF [--map MAP] [--order F] [--ord F] ...
    hotord_check.py --selftest DIR      seeded mutants on a real flavour dir (no build)

DIR is a flavours.sh flavour dir (gpsp_adhoc.elf, and gpsp_adhoc.map when the
sidecar ran).  --order defaults to psp/layout/hot_order.txt, --ord to
psp/layout/hot.ord of the commit the flavour dir was built from (its
flavour.json sourceCommit, read with git from this repo), else of THIS tree
(gpsp/, with a warning).  Check the build against the order it was linked
with: an ELF of another commit says nothing about this tree's order.

CHECKS, per hot_order.txt entry (docs/LAYOUT-PINNING.md "Keeping the pads"):
  match   the entry matches EXACTLY ONE input section.  With the map this is
          counted on the map itself: ld prints every hot.ord line and, under
          it, the sections it placed.  Without a map (regen.sh's case), it is
          the number of STT_FUNC symbols of that name in instance A (B's copies
          are renamed gbab_*), which is what -ffunction-sections' .text.<name>
          follows.  0 matches = the entry pins nothing; 2 = two static
          functions of one name, both pulled forward.
          GBA_LINK builds: B's line `*gbalink_core_b.o(.text.X)` must match one
          section too (map only).
  rename  an unmatched entry whose base name (clone suffixes .isra.N, .part.N,
          .constprop.N, .cold stripped) exists under ANOTHER name, or whose
          function now sits in a different input section (.text.unlikely.X,
          .text.startup.X, .text.hot.X): GCC re-cloned or re-sectioned it and
          the order silently lost it.
  size    the size hot_order.txt recorded vs the ELF's symbol size (the map's
          input-section size when they differ is reported too): "size" counts
          any difference, "rounding" the ones whose 32-byte-rounded size
          changed -- the rule: a function in hot.ord keeps its 32 B-rounded size.
  ord     hot.ord is exactly what gen_layout.py writes from hot_order.txt (a
          hand edit or a stale hot.ord is a failure).

Exit: 0 clean; 1 a match/rename/ord failure, or rounding breaks with
--sizes fail (the default); 2 usage.  --sizes warn reports sizes without
failing (what a pre-regen report wants); --sizes off skips them.
"""
import argparse
import json
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GPSP = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(GPSP, 'tools', 'layout'))
from elf32 import Elf  # noqa: E402
import sl_ldmap  # noqa: E402

B_OBJECT = 'gbalink_core_b.o'
B_PREFIX = 'gbab_'
B_PARTNER_PREFIX = 'gbab_fe_'
CLONE_RE = re.compile(r'(\.(isra|part|constprop|cold|lto_priv)(\.\d+)?)+$')
SPECIAL_SEC = re.compile(r'^\.text\.(unlikely|startup|hot|exit)\.')
PLACE_GAP = 16384     # baseline max gap between consecutive A entries: 3,508 B (Lab)


def base_name(n):
    return CLONE_RE.sub('', n)


def r32(n):
    return (n + 31) // 32 * 32


LATE_LINE = [None]   # hot_order.txt line of gen_layout's `# ---- late` marker, if any
LAST_LINE = [None]   # ... and of its `# ---- last` marker (rows after B's late copies)
BSCAN_LINE = [None]  # ... and of its `# ---- bscan` marker (rows in B's hot path, before B's update_scanline)


def read_order(path):
    """[(lineno, name, size or None, libmember or None, kind)]; kind: func|lib|asm|asmsec."""
    rows = []
    LATE_LINE[0] = None
    LAST_LINE[0] = None
    BSCAN_LINE[0] = None
    for k, l in enumerate(open(path), 1):
        if l.startswith('# ---- late') and LATE_LINE[0] is None:
            LATE_LINE[0] = k     # rows below link after B's whole hot path
        if l.startswith('# ---- last') and LAST_LINE[0] is None:
            LAST_LINE[0] = k     # rows below link after B's late copies (and the late pad)
        if l.startswith('# ---- bscan') and BSCAN_LINE[0] is None:
            BSCAN_LINE[0] = k    # rows below link in B's hot path, before B's update_scanline
        p = l.split('#', 1)[0].split()
        if not p:
            continue
        n = p[0]
        if n.startswith('@'):
            rows.append((k, n, None, None, 'asmsec' if '(' in n else 'asm'))
            continue
        size = int(p[2]) if len(p) > 2 and p[2].isdigit() else None
        lib = p[3] if len(p) > 3 else None
        rows.append((k, n, size, lib, 'lib' if lib else 'func'))
    return rows


def ord_lines_for(row):
    """The hot.ord line(s) gen_layout.write_ord emits for one order row (A, then B)."""
    _, n, _, lib, kind = row
    if kind == 'lib':
        return ['*:%s(.text .text.*)' % lib.split(':', 1)[1]], None
    if kind == 'func':
        return ['*(EXCLUDE_FILE(*%s) .text.%s)' % (B_OBJECT, n)], '*%s(.text.%s)' % (B_OBJECT, n)
    return [], None


class Build:
    def __init__(self, elf, mp=None):
        self.elf_path = elf
        self.e = Elf(elf)
        self.map = sl_ldmap.LdMap(mp) if mp else None
        self.b_sections = {}
        self._iv = []
        if self.map is not None:
            for i in self.map.inputs:
                if i.out == '.text' and i.size:
                    self._iv.append((i.addr, i.addr + i.size, i))
                if i.file.endswith(B_OBJECT) and i.out == '.text':
                    self.b_sections[i.name] = self.b_sections.get(i.name, 0) + 1
            self._iv.sort(key=lambda t: t[0])
        allf = sorted(self.e.funcs(), key=lambda x: x['value'])
        # Instance B (gbalink_core_b.o) renames its GLOBALS gbab_*, but its static
        # functions keep their names, so "update_scanline" can exist twice.  With a
        # map, B is told by the input file.  Without one (regen.sh's case), a
        # static is B's when the nearest marker globals BELOW and ABOVE it are both
        # gbab_* (B's code runs between its globals).  The partner's frontend
        # objects (gbab_fe_*: fe_autopilot_b.o, fe_gbadual.o) are linked among A's
        # frontend objects, not inside gbalink_core_b.o: they are not markers and
        # not B.  The selftest measures how often the two methods disagree.
        def marker(x):
            return x['bind'] != 0 and not x['name'].startswith(B_PARTNER_PREFIX)
        below, above = [None] * len(allf), [None] * len(allf)
        cur = None
        for k, x in enumerate(allf):
            if marker(x):
                cur = x['name']
            below[k] = cur
        cur = None
        for k in range(len(allf) - 1, -1, -1):
            if marker(allf[k]):
                cur = allf[k]['name']
            above[k] = cur
        self.funcs, self.b_funcs = {}, {}
        for k, x in enumerate(allf):
            if self.map is not None:
                inp = self.input_at(x['value'])
                in_b = inp is not None and inp.file.endswith(B_OBJECT)
            elif x['bind'] != 0:
                in_b = x['name'].startswith(B_PREFIX) and not x['name'].startswith(B_PARTNER_PREFIX)
            else:
                in_b = bool(below[k] and above[k] and below[k].startswith(B_PREFIX)
                            and above[k].startswith(B_PREFIX))
            (self.b_funcs if in_b else self.funcs).setdefault(x['name'], []).append(x)
        self.has_b = any(n.startswith(B_PREFIX) for n in list(self.funcs) + list(self.b_funcs))
        self.addr_names = {}
        for x in self.e.symbols:
            if x['type'] == 2:
                self.addr_names.setdefault(x['value'], []).append(x['name'])

    def input_at(self, addr):
        import bisect
        k = bisect.bisect_right(self._iv, (addr, 1 << 40)) - 1
        if k >= 0 and self._iv[k][0] <= addr < self._iv[k][1]:
            return self._iv[k][2]
        return None

    def a_funcs(self, name):
        return self.funcs.get(name, [])

    def section_of_func(self, sym):
        if self.map is None:
            return None
        return self.input_at(sym['value'])


def check(order_path, ord_path, build, sizes='fail', out=print, placement=True):
    rows = read_order(order_path)
    res = {'order': order_path, 'elf': build.elf_path, 'map': build.map.path if build.map else None,
           'entries': 0, 'unmatched': [], 'multi': [], 'multi_benign': [], 'renamed': [], 'size_diff': [],
           'rounding': [], 'b_unmatched': [], 'ord': 'not checked', 'notes': []}
    fail = 0
    # ord consistency: regenerate with gen_layout.write_ord and compare text
    if ord_path:
        try:
            import gen_layout
            fd, tmp = tempfile.mkstemp(suffix='.ord')
            os.close(fd)
            import io
            import contextlib
            with contextlib.redirect_stdout(io.StringIO()):
                gen_layout.write_ord(order_path, tmp)
            want = open(tmp, newline='').read().replace('\r\n', '\n')
            os.remove(tmp)
            have = open(ord_path, newline='').read().replace('\r\n', '\n')
            if want == have:
                res['ord'] = 'consistent'
            else:
                wl, hl = want.splitlines(), have.splitlines()
                extra = [x.strip() for x in hl if x not in wl][:5]
                missing = [x.strip() for x in wl if x not in hl][:5]
                res['ord'] = 'INCONSISTENT (missing %s; unexpected %s)' % (missing, extra)
                fail = 1
        except Exception as ex:
            res['ord'] = 'not checked (%s)' % ex
    funcs_names = set(build.funcs)
    for row in rows:
        k, n, size, lib, kind = row
        if kind in ('asm', 'asmsec'):
            if build.map is not None:
                obj = n[1:].split('(')[0]
                sec = n[n.index('(') + 1:-1] if '(' in n else '.text'
                if obj == B_OBJECT:
                    continue
                hits = [i for i in build.map.inputs if i.file.endswith('(%s)' % obj) and i.name == sec and i.size]
                if len(hits) != 1:
                    res['notes'].append('%s: %d non-empty %s sections of %s' % (n, len(hits), sec, obj))
            continue
        res['entries'] += 1
        a_lines, b_line = ord_lines_for(row)
        # ---- match count
        if build.map is not None:
            placed = []
            for pl in a_lines:
                placed += [i for i in build.map.patterns.get(pl, [])
                           if (i.size or kind == 'func') and (kind != 'func' or i.name == '.text.' + n)]
            if kind == 'lib':
                placed = [i for i in placed if i.size]
            nmatch = len(placed)
            how = 'map'
        else:
            nmatch = len(build.a_funcs(n))
            how = 'symtab'
        syms = build.a_funcs(n)
        if nmatch == 0:
            # rename / re-section?
            why = None
            if syms and build.map is not None:
                inp = build.section_of_func(syms[0])
                if inp is not None:
                    why = 'in input section %s, not .text.%s' % (inp.name, n)
            if why is None:
                cands = sorted(x for x in funcs_names if x != n and not x.startswith(B_PREFIX)
                               and base_name(x) == base_name(n))
                if cands:
                    why = 'renamed: now %s' % ', '.join(cands)
            if why:
                res['renamed'].append({'line': k, 'name': n, 'why': why})
                out('  RENAME   hot_order.txt:%-4d %-40s %s' % (k, n, why))
            else:
                res['unmatched'].append({'line': k, 'name': n, 'how': how})
                out('  NOMATCH  hot_order.txt:%-4d %-40s matches nothing (%s)' % (k, n, how))
            fail = 1
            continue
        if kind == 'func' and nmatch > 1:
            if build.map is not None:
                # classify every extra section: a live instance-A function (a real
                # ambiguity), instance B's partner copy outside gbalink_core_b.o
                # (EXCLUDE_FILE names only that object), or an overridden weak
                # default (no symbol at its address: dead bytes the order pulls in)
                live, extra = 0, []
                for i in placed:
                    names = build.addr_names.get(i.addr, [])
                    if any(x == n or base_name(x) == base_name(n) for x in names
                           if not x.startswith(B_PREFIX)):
                        live += 1
                    elif any(x.startswith(B_PREFIX) for x in names):
                        extra.append('B partner copy in %s' % i.file.split('/')[-1])
                    else:
                        extra.append('dead weak default in %s (%d B)' % (i.file.split('/')[-1], i.size))
                rec = {'line': k, 'name': n, 'count': nmatch, 'live': live, 'extra': extra}
                if live > 1:
                    res['multi'].append(rec)
                    out('  MULTI    hot_order.txt:%-4d %-40s matches %d live functions' % (k, n, live))
                    fail = 1
                else:
                    res['multi_benign'].append(rec)
                    out('  extra    hot_order.txt:%-4d %-40s also places: %s' % (k, n, '; '.join(extra)))
            else:
                res['multi'].append({'line': k, 'name': n, 'count': nmatch})
                out('  MULTI    hot_order.txt:%-4d %-40s %d functions of this name (symtab)' % (k, n, nmatch))
                fail = 1
        # ---- instance B's line (GBA_LINK): B's .text.X, when B has one, must be
        # placed by B's own line
        if kind == 'func' and build.map is not None:
            b_all = build.b_sections.get('.text.' + n, 0)
            nb = len(build.map.patterns.get(b_line, []))
            if b_all and nb != 1:
                res['b_unmatched'].append({'line': k, 'name': n, 'count': nb})
                out('  B-MATCH  hot_order.txt:%-4d %-40s B has %d .text.%s, its line places %d'
                    % (k, n, b_all, n, nb))
                fail = 1
        # ---- size
        if sizes != 'off' and size is not None and syms:
            have = syms[0]['size']
            msz = None
            if build.map is not None and kind == 'func':
                pl = build.map.patterns.get(a_lines[0], [])
                if len(pl) == 1:
                    msz = pl[0].size
            if have != size:
                rec = {'line': k, 'name': n, 'listed': size, 'elf': have, 'section': msz}
                res['size_diff'].append(rec)
                broke = r32(have) != r32(size)
                if broke:
                    res['rounding'].append(rec)
                out('  %s hot_order.txt:%-4d %-40s %5d -> %5d B%s%s' % (
                    'ROUNDING' if broke else 'size    ', k, n, size, have,
                    '  (32 B-rounded %d -> %d)' % (r32(size), r32(have)) if broke else '',
                    '  (map section %d B)' % msz if msz is not None and msz != have else ''))
    # ---- placement.  A matched NAME does not prove the line placed it: a function
    # re-sectioned to .text.unlikely.X / .text.startup.X keeps its symbol name, and
    # without a map (regen.sh --check-only) only its address shows it.  hot.ord
    # lays its entries out in order from the start of .text, so instance A's
    # entries must be in ascending address order (the longest ascending run is
    # kept; the rest are misplaced) and close together (a gap of more than
    # PLACE_GAP after the previous entry's end means it was linked elsewhere).
    res['misplaced'] = []
    seq = []
    for k, n, size, lib, kind in rows:
        if kind != 'func':
            continue
        f = build.a_funcs(n)
        if len(f) == 1:
            seq.append((k, n, f[0]['value'], f[0]['size']))
    if seq and placement:
        import bisect
        tails, tails_i, prev = [], [], [-1] * len(seq)
        for i, (_, _, a, _) in enumerate(seq):
            j = bisect.bisect_left(tails, a)
            if j == len(tails):
                tails.append(a); tails_i.append(i)
            else:
                tails[j] = a; tails_i[j] = i
            prev[i] = tails_i[j - 1] if j else -1
        keep, i = set(), tails_i[-1] if tails_i else -1
        while i >= 0:
            keep.add(i); i = prev[i]
        last = None
        for i, (k, n, a, sz) in enumerate(seq):
            why = None
            if i not in keep:
                why = 'at %#010x, out of hot.ord order' % a
            elif last is not None and a - (last[2] + last[3]) > PLACE_GAP and not any(
                    m is not None and last[0] < m < k
                    for m in (LATE_LINE[0], LAST_LINE[0], BSCAN_LINE[0])):
                # (the first bscan row follows B's hot core, the first late
                # row B's whole hot path, and the first last row B's late
                # copies and the late pad, by design)
                why = 'at %#010x, %d B after %s ends: not linked by its hot.ord line' % (
                    a, a - (last[2] + last[3]), last[1])
            if why:
                res['misplaced'].append({'line': k, 'name': n, 'why': why})
                out('  PLACE    hot_order.txt:%-4d %-40s %s' % (k, n, why))
                fail = 1
            else:
                last = (k, n, a, sz)
    if sizes == 'fail' and res['rounding']:
        fail = 1
    out('hot order: %d entries checked (%s); unmatched %d, multi-matched %d (+%d placing dead/partner bytes), '
        'renamed %d, misplaced %d, B-line %d; size differs %d, 32 B rounding broken %d; hot.ord %s'
        % (res['entries'], 'map' if build.map is not None else 'symtab only', len(res['unmatched']),
           len(res['multi']), len(res['multi_benign']), len(res['renamed']), len(res['misplaced']),
           len(res['b_unmatched']), len(res['size_diff']),
           len(res['rounding']), res['ord']))
    for nn in res['notes']:
        out('  note: %s' % nn)
    res['fail'] = fail
    return fail, res


ORDER_REL = ('psp/layout/hot_order.txt', 'psp/layout/hot.ord')


def _git_show(rev_path):
    """`git show REV:PATH` from this tree's repo, bytes or None.  From WSL on a Windows
    worktree plain git cannot read the .git file, so git.exe is tried too (flavours.sh's rule)."""
    import subprocess
    cmds = [['git', '-C', GPSP, 'show', rev_path]]
    try:
        w = subprocess.run(['wslpath', '-w', GPSP], capture_output=True, text=True).stdout.strip()
        if w:
            cmds.append(['git.exe', '-C', w, 'show', rev_path])
    except OSError:
        pass
    for c in cmds:
        try:
            r = subprocess.run(c, capture_output=True)
        except OSError:
            continue
        if r.returncode == 0:
            return r.stdout.replace(b'\r\n', b'\n') if c[0] == 'git.exe' else r.stdout
    return None


def order_of_build(d, tmp):
    """The (hot_order.txt, hot.ord) a flavours.sh flavour dir was LINKED with: taken from
    its flavour.json sourceCommit.  An ELF checked against another commit's order (the
    2026-11-07 T5 selftest failures on the 01067e74 baseline from a beta.5 tools tree:
    4-5 controls FAILED, unmatched 5, renamed 4) says nothing about either.
    -> (order, ord, note); (None, None, note) when the dir names no commit."""
    try:
        c = json.load(open(os.path.join(d, 'flavour.json'))).get('sourceCommit')
    except (OSError, ValueError):
        c = None
    if not c:
        return None, None, 'no flavour.json sourceCommit in %s' % d
    out = []
    for rel in ORDER_REL:
        b = _git_show('%s:gpsp/%s' % (c, rel))
        if b is None:
            return None, None, 'cannot read %s:gpsp/%s from this repo' % (c[:10], rel)
        p = os.path.join(tmp, os.path.basename(rel))
        open(p, 'wb').write(b)
        out.append(p)
    return out[0], out[1], 'order and hot.ord of the build\'s own commit %s' % c[:10]


def resolve_order(a, d):
    """Fill a.order / a.ord when the caller gave neither: the build's own commit's files,
    else THIS tree's (said so on stderr).  An explicit --order/--ord is never replaced."""
    if a.order is not None or a.ord is not None or not d:
        a.order = a.order or os.path.join(GPSP, ORDER_REL[0])
        a.ord = a.ord or os.path.join(GPSP, ORDER_REL[1])
        return
    o, r, note = order_of_build(d, tempfile.mkdtemp(prefix='hotord_ord_'))
    if o:
        a.order, a.ord = o, r
        print('hotord_check: %s' % note, file=sys.stderr)
    else:
        a.order, a.ord = os.path.join(GPSP, ORDER_REL[0]), os.path.join(GPSP, ORDER_REL[1])
        print('hotord_check: WARNING %s: using THIS tree\'s order (%s); it must be the one '
              'the ELF was linked with' % (note, GPSP), file=sys.stderr)


def resolve_build(a):
    if a.build:
        elf = os.path.join(a.build, 'gpsp_adhoc.elf')
        mp = os.path.join(a.build, 'gpsp_adhoc.map')
        return elf, (mp if os.path.isfile(mp) and not a.no_map else None)
    return a.elf, a.map


# --------------------------------------------------------------------------- selftest

def _set_size(line, old, new):
    """Replace the size column (3rd field) of one hot_order.txt line, keeping alignment."""
    p = line.split()
    assert p[2] == str(old), (line, old)
    i = line.index(p[1]) + len(p[1])
    j = line.index(p[2], i)
    w = j + len(p[2]) - i
    return line[:i] + ('%*d' % (w, new)) + line[j + len(p[2]):]


def selftest(d, order, ord_path):
    """Controls and mutants on a real flavour dir.  Each mutant edits a COPY of the
    order, the map, or the ELF's string table; nothing in the tree is touched."""
    import shutil
    elf = os.path.join(d, 'gpsp_adhoc.elf')
    mp = os.path.join(d, 'gpsp_adhoc.map')
    has_map = os.path.isfile(mp)
    t = tempfile.mkdtemp(prefix='hotord_')
    quiet = lambda *_: None
    fails = [0]

    def expect(label, rc, want_rc, cond=True):
        ok = (rc == want_rc) and cond
        print('  %-70s %s' % (label, 'ok' if ok else 'FAILED (rc %d)' % rc))
        fails[0] += not ok

    try:
        b_map = Build(elf, mp if has_map else None)
        b_sym = Build(elf, None)
        # C0: today's state, reported (sizes warn so only structure decides)
        rc, r0 = check(order, ord_path, b_map, 'warn', quiet)
        print('  today: %d entries, size differs %d, rounding broken %d, unmatched %d, renamed %d, multi %d, ord %s'
              % (r0['entries'], len(r0['size_diff']), len(r0['rounding']), len(r0['unmatched']),
                 len(r0['renamed']), len(r0['multi']), r0['ord']))
        expect('control: today, structure only (--sizes warn)', rc, 0)
        rc, _ = check(order, ord_path, b_sym, 'warn', quiet)
        expect('control: today, symtab only (no map), --sizes warn', rc, 0)
        rc, _ = check(order, ord_path, b_map, 'fail', quiet)
        expect('today with --sizes fail: rounding breaks fail (%d)' % len(r0['rounding']),
               rc, 1 if r0['rounding'] else 0)
        # C1: the two instance-B classifiers (map file vs symtab neighbour) agree
        if has_map:
            dis = []
            for nm_, xs in list(b_map.funcs.items()) + list(b_map.b_funcs.items()):
                for x in xs:
                    a_map = nm_ in b_map.funcs and x in b_map.funcs[nm_]
                    a_sym = any(y['value'] == x['value'] for y in b_sym.funcs.get(nm_, []))
                    if a_map != a_sym:
                        dis.append(nm_)
            n_all = sum(len(v) for v in b_map.funcs.values()) + sum(len(v) for v in b_map.b_funcs.values())
            order_names = {r[1] for r in read_order(order)}
            bad = sorted(set(dis) & order_names)
            print('  instance-B classification, symtab vs map: %d of %d functions disagree (%d in the order)'
                  % (len(dis), n_all, len(bad)))
            expect('control: symtab and map classify every function alike', 0, 0, not dis)
        # C2: an order whose sizes are refreshed from this ELF passes --sizes fail
        fresh = os.path.join(t, 'hot_order.fresh.txt')
        rows = read_order(order)
        ls = open(order).read().split('\n')
        for (k, n, size, lib, kind) in rows:
            f = b_sym.a_funcs(n)
            if size is not None and f:
                ls[k - 1] = _set_size(ls[k - 1], size, f[0]['size'])
        open(fresh, 'w', newline='\n').write('\n'.join(ls))
        rc, rf = check(fresh, ord_path, b_map, 'fail', quiet)
        expect('control: sizes refreshed from this ELF -> --sizes fail passes', rc, 0, not rf['size_diff'])
        # M1: lat_swap re-cloned, as the ELF's string table would show it
        data = open(elf, 'rb').read()
        old, new = b'\0lat_swap.isra.0\0', b'\0lat_swap.isra.1\0'
        cnt = data.count(old)
        me = os.path.join(t, 'renamed.elf')
        open(me, 'wb').write(data.replace(old, new))
        rc, rr = check(order, ord_path, Build(me, None), 'warn', quiet)
        expect('mutant: lat_swap.isra.0 -> .isra.1 in the ELF (symtab, %d strtab hit)' % cnt, rc, 1,
               any(x['name'] == 'lat_swap.isra.0' and 'lat_swap.isra.1' in x['why'] for x in rr['renamed']))
        if has_map:
            txt = open(mp, errors='replace').read()
            # M2: the same as ld would print it: the pattern places nothing and the
            # renamed section lands in the catch-all *(.text .text.*) run
            pat = ' *(EXCLUDE_FILE(*gbalink_core_b.o) .text.lat_swap.isra.0)\n'
            i0 = txt.index(pat) + len(pat)
            i1 = txt.index('\n *', i0) + 1
            moved = txt[i0:i1].replace('.text.lat_swap.isra.0', '.text.lat_swap.isra.1')
            body = txt[:i0] + txt[i1:]
            anchor = ' *(.text .stub .text.* .gnu.linkonce.t.*)\n'
            body = body.replace(anchor, anchor + moved, 1)
            mm = os.path.join(t, 'renamed.map')
            open(mm, 'w').write(body)
            rc, rr = check(order, ord_path, Build(me, mm), 'warn', quiet)
            expect('mutant: lat_swap re-cloned .isra.1, placed by the catch-all (map)', rc, 1,
                   any(x['name'] == 'lat_swap.isra.0' for x in rr['renamed']))
            # M3: ll_present emitted as .text.unlikely.ll_present (a cold attribute
            # without a section): the order's line places nothing
            pat = ' *(EXCLUDE_FILE(*gbalink_core_b.o) .text.ll_present)\n'
            i0 = txt.index(pat) + len(pat)
            i1 = txt.index('\n *', i0) + 1
            moved = txt[i0:i1].replace('.text.ll_present', '.text.unlikely.ll_present')
            body = txt[:i0] + txt[i1:]
            anchor = ' *(.text.unlikely .text.*_unlikely .text.unlikely.*)\n'
            body = body.replace(anchor, anchor + moved, 1)
            mm3 = os.path.join(t, 'unlikely.map')
            open(mm3, 'w').write(body)
            rc, rr = check(order, ord_path, Build(elf, mm3), 'warn', quiet)
            expect('mutant: ll_present re-sectioned .text.unlikely.ll_present (map)', rc, 1,
                   any(x['name'] == 'll_present' and 'unlikely' in x['why'] for x in rr['renamed']))
            # M4: two LIVE static functions of one name under one line: frame_end
            # exists twice in instance A (two TUs); add it to the order the way
            # gen_layout would, and place both sections under its line
            fe = b_map.a_funcs('frame_end')
            if len(fe) >= 2:
                mo = os.path.join(t, 'dup_order.txt')
                open(mo, 'w', newline='\n').write(open(order).read().replace(
                    '# ---- warm tail\n', '# ---- warm tail\nframe_end    0.000001    %d\n' % fe[0]['size'], 1))
                pat = ' *(EXCLUDE_FILE(*gbalink_core_b.o) .text.frame_end)\n'
                secs = ''.join(' .text.frame_end\n                0x%08x %#10x %s\n'
                               % (x['value'], x['size'], b_map.input_at(x['value']).file) for x in fe)
                anchor = ' *gpsp_libretro_psp1.a:mips_stub.o(.text.msr_fast)\n'
                mm4 = os.path.join(t, 'dup.map')
                open(mm4, 'w').write(txt.replace(anchor, pat + secs + anchor, 1))
                rc, rr = check(mo, None, Build(elf, mm4), 'warn', quiet)
                expect('mutant: frame_end in the order, 2 live functions (map)', rc, 1,
                       any(x['name'] == 'frame_end' for x in rr['multi']))
                rc, rr = check(mo, None, b_sym, 'warn', quiet)
                expect('mutant: frame_end in the order, 2 live functions (symtab)', rc, 1,
                       any(x['name'] == 'frame_end' for x in rr['multi']))
            else:
                print('  (no duplicated static function name in this ELF: M4 n/a)')
            # M5: B's update_gba no longer placed by B's own line
            mm5 = os.path.join(t, 'nob.map')
            open(mm5, 'w').write(txt.replace(' *gbalink_core_b.o(.text.update_gba)\n',
                                             ' *gbalink_core_b.o(.text.update_gbx)\n', 1))
            rc, rr = check(order, ord_path, Build(elf, mm5), 'warn', quiet)
            expect("mutant: B's .text.update_gba not placed by its own line (map)", rc, 1,
                   any(x['name'] == 'update_gba' for x in rr['b_unmatched']))
        else:
            print('  (no gpsp_adhoc.map in %s: map-mode mutants skipped)' % d)
        # M6: a listed size +32 (rounding break, fails) and within the rounding (passes)
        tgt = next(r for r in read_order(fresh) if r[1] == 'll_sample')
        small = next(dd for dd in (4, -4, 8, -8) if r32(tgt[2] + dd) == r32(tgt[2]))
        for delta, want_round in ((32, True), (small, False)):
            ls = open(fresh).read().split('\n')
            ls[tgt[0] - 1] = _set_size(ls[tgt[0] - 1], tgt[2], tgt[2] + delta)
            mo = os.path.join(t, 'size%d.txt' % delta)
            open(mo, 'w', newline='\n').write('\n'.join(ls))
            rc, rr = check(mo, None, b_sym, 'fail', quiet)
            got_round = any(x['name'] == 'll_sample' for x in rr['rounding'])
            got_size = any(x['name'] == 'll_sample' for x in rr['size_diff'])
            expect('mutant: ll_sample listed %+d B (%s)' % (delta, 'rounding break, fails' if want_round else
                                                          'within its 32 B rounding: reported, passes'),
                   rc, 1 if want_round else 0, got_size and got_round == want_round)
        # M7: hot.ord hand-edited (one line dropped)
        if ord_path:
            mo = os.path.join(t, 'hot.ord')
            ls = open(ord_path).read().split('\n')
            k = next(i for i, l in enumerate(ls) if '.text.lat_wait_step)' in l)
            open(mo, 'w', newline='\n').write('\n'.join(ls[:k] + ls[k + 1:]))
            rc, rr = check(order, mo, b_sym, 'warn', quiet)
            expect('mutant: hot.ord line for lat_wait_step deleted', rc, 1, rr['ord'].startswith('INCONSISTENT'))
        # M8: an order entry that names nothing
        mo = os.path.join(t, 'ghost.txt')
        open(mo, 'w', newline='\n').write(open(order).read().replace('ll_sample ', 'll_sampel ', 1))
        rc, rr = check(mo, None, b_sym, 'warn', quiet)
        expect('mutant: misspelt entry ll_sampel', rc, 1, any(x['name'] == 'll_sampel' for x in rr['unmatched']))
        # M9: a function re-sectioned (cold, no section) keeps its NAME; without a map
        # only its address shows the order lost it.  Move the symbol, as the link would.
        import struct
        st = b_sym.e.by_name['.symtab']
        strs = b_sym.e.sections[st['link']]
        main_a = b_sym.e.by_name['.text']['addr'] + b_sym.e.by_name['.text']['size'] // 2
        for victim, where, label in (('flag_interrupt', main_a, 'mid-order entry'),
                                     (None, main_a + 0x100000, 'LAST entry')):
            if victim is None:
                victim = [r[1] for r in rows if r[4] == 'func'][-1]
            mb = bytearray(data)
            hit = 0
            for q in range(st['size'] // 16):
                nm = struct.unpack_from('<I', mb, st['offset'] + 16 * q)[0]
                o = strs['offset'] + nm
                if bytes(mb[o:mb.index(b'\0', o)]) == victim.encode():
                    struct.pack_into('<I', mb, st['offset'] + 16 * q + 4, where)
                    hit += 1
            mp9 = os.path.join(t, 'moved.elf')
            open(mp9, 'wb').write(mb)
            rc, rr = check(order, ord_path, Build(mp9, None), 'warn', quiet)
            expect('mutant: %s %s moved out of the hot run, symtab only (%d sym)' % (label, victim, hit), rc, 1,
                   any(x['name'] == victim for x in rr['misplaced']))
    finally:
        shutil.rmtree(t, ignore_errors=True)
    print('selftest: %s' % ('PASS' if not fails[0] else '%d FAILED' % fails[0]))
    return 1 if fails[0] else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--build', help='flavours.sh flavour dir')
    ap.add_argument('--elf')
    ap.add_argument('--map')
    ap.add_argument('--no-map', action='store_true', help='ignore the dir\'s map (symtab mode)')
    ap.add_argument('--order', default=None,
                    help='default: the flavour dir\'s own commit (flavour.json), else this tree\'s')
    ap.add_argument('--ord', default=None, help='as --order')
    ap.add_argument('--no-ord', action='store_true', help='skip the hot.ord consistency check')
    ap.add_argument('--sizes', choices=('fail', 'warn', 'off'), default='fail')
    ap.add_argument('--no-placement', action='store_true',
                    help='skip the address-order check (an ELF NOT linked with this order, e.g. regen.sh --check)')
    ap.add_argument('--json')
    ap.add_argument('--selftest', metavar='DIR')
    a = ap.parse_args()
    resolve_order(a, a.selftest or a.build)
    ordp = None if a.no_ord else a.ord
    if a.selftest:
        return selftest(a.selftest, a.order, ordp)
    if not (a.build or a.elf):
        ap.error('--build DIR or --elf ELF')
    elf, mp = resolve_build(a)
    print('hotord_check: order %s\n              elf   %s%s' % (a.order, elf, '\n              map   %s' % mp if mp else ''))
    rc, res = check(a.order, ordp, Build(elf, mp), a.sizes, placement=not a.no_placement)
    if a.json:
        json.dump(res, open(a.json, 'w'), indent=1)
    return rc


if __name__ == '__main__':
    sys.exit(main())
