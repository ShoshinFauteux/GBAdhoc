#!/usr/bin/env python3
"""frame_parity.py -- the per-frame code of the release and the Lab build must be the same code
at the same I-cache set (owner decision 2026-11-17).

The Lab runs only rig (lab) EBOOTs, never the release.  Its speed and picture-hash results prove
the shipped build only where the code that runs every frame is the same in both: the same bytes
(modulo where things were linked) at the same address mod 8 KiB (the I-cache set: 16 KiB, 2-way,
64 B lines).  Cold code (boot, menus, session setup and teardown, error paths) may differ.

The two builds were never fully identical (frame_parity.base lists what differed at 624af868:
telemetry and rig code in frame_loop, pace_frame, me_rend_frame ...; at 624af868 only 60 of
its ~380 compared members shared an I-cache set).  So the rule is
a RATCHET against the recorded baseline, function by function:

  same  at the baseline: release and lab must stay equivalent (func_diff's same-mod-address) and
        keep their offset difference mod 8 KiB (`d`, where it is held: below).
  diff  at the baseline: every difference between release and lab now must be one the baseline
        already had.  Each function's normalised instruction streams (address operands resolved
        to symbols, branch offsets dropped) are aligned by hunks(), a patience diff in pure
        Python (the hashes must not depend on the git installed); each non-equal hunk is a
        `chunk`, kept as a short hash.  The release/lab chunks now must be a sub-multiset of the
        recorded ones, and `d` must hold (below).  A change made alike in both builds leaves the chunks
        alone; a release-only (or lab-only) edit adds one.
  rel-only / lab-only at the baseline: still only there (or gone).
  rigonly: runs per frame only in a rig build (the release never executes it, e.g. the
        autopilot behind its gate); listed, not compared.

THE I-CACHE SET (`d`) is held where release and lab shared one at the baseline (d 0: the core's
hot path) and for every member inside the hot block in both builds (row pins make an offset
holdable there).  Elsewhere the two builds never shared a set: the frontend's per-frame code
outside the hot block (main_psp.o, ff_psp.o, video/osd/ui/ctl ...) sits among cold code whose size
differs per build, so the Lab build's copy was never at the release's set and any one-build cold
edit moves it.  There a changed `d` is a note, not a failure: there is no parity to lose, and
holding an arbitrary offset would fail every frontend change made alike in both builds.

A function that is new in the set must be the same code (`same`, or `layout`: data immediates
only) in both builds; a new member's `d` is held only in the hot block.  Fewer chunks than
recorded passes, with a note to re-record (the ratchet tightens only when someone records it).

THE PER-FRAME SET (tools/layout/frame_parity.spec):
  * the hot block: every function the hot order placed, i.e. every function symbol from _ftext up
    to __pin_from_frame_loop (psp/layout/hot_order.txt, both instances' rows, the asm stubs);
  * tools/layout/psp_frame_path.txt's rows (the PSP-only per-frame path, wherever it is linked);
  * the spec's `root` and `func` patterns (frame_loop, pace_frame, lat_*, me_rend_frame, ...);
  * every direct callee (static jal) of a `root`, unless a `cold` line names it;
  * the spec's `data` objects (compared as data, same-mod-address, offset difference held).
Patterns are fnmatch globs on the base name (`lat_swap.isra.0` matches `lat_swap*` and
`lat_swap`).

    frame_parity.py set     REL.elf LAB.elf [--spec F]          the set and why each is in it
    frame_parity.py measure REL.elf LAB.elf [--spec F] [-v]     release vs lab, every member
    frame_parity.py record  REL.elf LAB.elf [--spec F] [--out F] [--note TEXT]
    frame_parity.py check   REL.elf LAB.elf [--spec F] [--base F] [-v]
    frame_parity.py chunks  REL.elf LAB.elf NAME                one member's release/lab hunks
    frame_parity.py --selftest

Exit: 0 parity holds (check) / done, 1 a new divergence, 2 usage or an unreadable input.
"""
import argparse
import difflib
import fnmatch
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REFACTOR = os.path.join(HERE, '..', 'refactor')
sys.path.insert(0, REFACTOR)
from elf32 import Elf                       # noqa: E402
import ef_mips as M                         # noqa: E402

SPEC = os.path.join(HERE, 'frame_parity.spec')
BASE = os.path.join(HERE, 'frame_parity.base')
FRAME_PATH = os.path.join(HERE, 'psp_frame_path.txt')
WAY = 0x2000
BRANCH_OPS = {0x01, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17}
FAR_LO = 0x800        # |imm| from here on, off a non-stack pointer register: a %lo, not a field


# ------------------------------------------------------------------ inputs
def read_spec(path):
    """-> dict(kind -> [(pattern, reason)])"""
    out = {k: [] for k in ('root', 'func', 'cold', 'rigonly', 'data')}
    for n, ln in enumerate(open(path), 1):
        ln = ln.rstrip('\n')
        body = ln.split('#', 1)[0].strip()
        if not body:
            continue
        parts = body.split(None, 1)
        if parts[0] not in out or len(parts) < 2:
            raise SystemExit('frame_parity: %s:%d: bad line %r' % (path, n, ln))
        why = ln.split('#', 1)[1].strip() if '#' in ln else ''
        for pat in parts[1].split():
            out[parts[0]].append((pat, why))
    return out


def read_frame_path(path):
    names = []
    if os.path.exists(path):
        for ln in open(path):
            ln = ln.split('#', 1)[0].strip()
            if ln:
                names.append(ln.split()[0])
    return names


def matches(name, pats):
    b = M.base_name(name)
    c = M.canon(name)
    for p, why in pats:
        if fnmatch.fnmatchcase(name, p) or fnmatch.fnmatchcase(c, p) or fnmatch.fnmatchcase(b, p):
            return p, why
    return None


class Build:
    """One linked ELF: functions keyed as func_diff keys them (canonical name, STT_FILE for a
    local whose canonical name is shared, address ordinal)."""

    def __init__(self, path):
        self.path = path
        self.elf = Elf(path)
        self.space = M.Space(self.elf)
        self.abs = {s['name']: s['value'] for s in self.elf.symbols}
        self.funcs = self._keyed((2,))
        self.objs = self._keyed((1,))
        self.by_addr = {}
        for k, s in self.funcs.items():
            self.by_addr.setdefault(s['value'], k)
        self._fstarts = sorted((s['value'], s['size'], k) for k, s in self.funcs.items())
        self._fkeys = [x[0] for x in self._fstarts]

    def fn_token(self, a):
        """A code address as `function+offset` (any size: Space.token gives up on a function
        over 64 KiB, e.g. translate_block_arm), else ef_mips's token."""
        import bisect
        k = bisect.bisect_right(self._fkeys, a) - 1
        if k >= 0:
            v, sz, key = self._fstarts[k]
            if v <= a < v + sz:
                return '%s+%d' % (key, a - v)
        return self.space.token(a)

    def _keyed(self, kinds):
        syms = [s for s in self.elf.symbols if s['type'] in kinds and s['size'] > 0
                and s['shndx'] not in (0, 0xfff1)]
        groups = {}
        for s in syms:
            groups.setdefault(M.canon(s['name']), []).append(s)
        d = {}
        for cn, ss in groups.items():
            ss.sort(key=lambda s: s['value'])
            if len(ss) == 1:
                d[cn] = ss[0]
                continue
            per = {}
            for s in ss:
                per.setdefault(s['file'] or '', []).append(s)
            for f, fs in per.items():
                for k, s in enumerate(fs):
                    key = cn + ('@' + f if f else '') + ('#%d' % (k + 1) if len(fs) > 1 else '')
                    d[key] = s
        return d

    def hot_block(self):
        lo = self.abs.get('_ftext')
        hi = self.abs.get('__pin_from_frame_loop')
        if lo is None or hi is None:
            raise SystemExit('frame_parity: %s has no _ftext / __pin_from_frame_loop '
                             '(not a pinned PSP link?)' % self.path)
        return [k for k, s in self.funcs.items() if lo <= s['value'] < hi]

    def callees(self, key):
        s = self.funcs[key]
        code = self.elf.read(s['value'], s['size']) or b''
        out = []
        for i in range(len(code) // 4):
            w = struct.unpack_from('<I', code, 4 * i)[0]
            if w >> 26 == 3:                                        # jal
                t = ((s['value'] + 4 * i + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                k = self.by_addr.get(t)
                if k and k != key and k not in out:
                    out.append(k)
        return out

    def find(self, pats):
        return [k for k in self.funcs if matches(k.split('@', 1)[0].split('#', 1)[0], pats)]


# ------------------------------------------------------------------ the set
def frame_set(R, L, spec):
    """-> {key: (kind, why)}, kind in fn/data; why says how it got in."""
    out = {}
    cold, rig = spec['cold'], spec['rigonly']

    def add(key, why):
        if key not in out:
            out[key] = ('fn', why)

    for B in (R, L):
        for k in B.hot_block():
            add(k, 'hot block')
    fp = [(n, 'psp_frame_path.txt') for n in read_frame_path(FRAME_PATH)]
    for B in (R, L):
        for k in B.find(fp):
            add(k, 'psp_frame_path.txt')
        for k in B.find(spec['func']):
            add(k, 'spec func')
        for k in B.find(spec['root']):
            add(k, 'spec root')
    for B in (R, L):
        for k in B.find(spec['root']):
            for c in B.callees(k):
                nm = c.split('@', 1)[0].split('#', 1)[0]
                if matches(nm, cold):
                    continue
                add(c, 'callee of ' + k)
    for B in (R, L):
        for k in list(B.objs):
            if matches(k.split('@', 1)[0].split('#', 1)[0], spec['data']):
                out.setdefault(k, ('data', 'spec data'))
    final = {}
    for k, (kind, why) in out.items():
        nm = k.split('@', 1)[0].split('#', 1)[0]
        m = matches(nm, rig)
        final[k] = (kind, why, ('rigonly', m[1]) if m else None)
    return final


# ------------------------------------------------------------------ comparison
def data_class(B, a):
    """A DATA address, data-blind: only the kind of section it lands in.  Without relocations a
    %hi/%lo pair is matched to its lui by guesswork, and the release and the Lab never had the
    same data layout (telemetry .bss, the SMC profile, the hot-data guard), so a resolved name
    is often a neighbour's: it would turn every unrelated data change into a code 'hunk'.  What
    the check holds is the instruction stream and the call targets."""
    sec = B.elf.section_of(a)
    if sec is not None and sec['flags'] & 4 and a in B.by_addr:
        return 'fn:' + B.by_addr[a]          # a function pointer keeps its function's name
    return 'D'


def norm_words(B, s):
    """Normalised instruction stream of one function: call targets and %hi/%lo / $gp data
    operands resolved to symbol tokens (data_class), branch offsets dropped.  A lui no %lo user
    justifies stays a raw word, so a changed upper half is never hidden."""
    code = B.elf.read(s['value'], s['size']) or b''
    n = len(code) // 4
    w = struct.unpack('<%dI' % n, code[:4 * n])
    gp = B.abs.get('_gp')
    luis = {}
    for i, x in enumerate(w):
        if x >> 26 == 0x0F:
            luis.setdefault((x >> 16) & 31, []).append(i)
    lo_tok, justified = {}, set()
    for i, x in enumerate(w):
        op = x >> 26
        if op not in M.IMM_USERS:
            continue
        rs = (x >> 21) & 31
        if rs == 28 and gp is not None and op != 0x0D:
            full = M._lo_full(op, x, gp)
            if B.space.in_range(full):
                lo_tok[i] = data_class(B, full)
            continue
        cands = sorted(luis.get(rs, []), key=lambda k: (k > i, abs(i - k)))
        for la in cands:
            full = M._lo_full(op, x, (w[la] & 0xFFFF) << 16)
            if B.space.in_range(full):
                lo_tok[i] = data_class(B, full)
                justified.add(la)
                break
    out = []
    lo, hi = s['value'], s['value'] + s['size']
    for i, x in enumerate(w):
        op = x >> 26
        if op in (2, 3):
            t = ((s['value'] + 4 * i + 4) & 0xF0000000) | ((x & 0x3FFFFFF) << 2)
            if lo <= t < hi:                 # a jump inside this function: like a branch
                out.append('%s .self' % ('j' if op == 2 else 'jal'))
            else:
                out.append('%s %s' % ('j' if op == 2 else 'jal', B.fn_token(t)))
        elif op == 0x0F and (i in justified or B.space.in_range((x & 0xFFFF) << 16) or
                             B.space.in_range(((x & 0xFFFF) << 16) + 0xFFFF)):
            # the upper half of an address (its %lo elsewhere, or paired by a guess): data-blind
            out.append('lui r%d' % ((x >> 16) & 31))
        elif i in lo_tok:
            out.append('%08x @%s' % (x & 0xFFFF0000, lo_tok[i]))
        elif op in M.IMM_USERS and ((x >> 21) & 31) not in (0, 28, 29, 30) and \
                abs(((x & 0xFFFF) ^ 0x8000) - 0x8000) >= FAR_LO:
            # a far offset off a pointer register is the %lo of a global whose lui this function
            # does not hold (a callee-saved base, a hoisted pair): data layout, data-blind
            out.append('%08x @D?' % (x & 0xFFFF0000))
        elif op in BRANCH_OPS or (op in (0x11, 0x12) and ((x >> 21) & 31) == 8):
            out.append('%04x br' % (x >> 16))
        else:
            out.append('%08x' % x)
    return out, w


def layout_pair(x, y):
    """Two words that differ only where a different DATA layout makes them differ: the same lui
    (register) or the same load/store/addiu/ori (opcode, base, target) with another 16-bit
    immediate.  Calls are never layout pairs (another callee is another program)."""
    if x == y:
        return True
    op = x >> 26
    if op != y >> 26 or (x ^ y) & 0xFFFF0000:
        return False
    return op == 0x0F or op in M.IMM_USERS


SMALL = 2000000      # |a| * |b| under which an anchorless region is aligned with difflib


def _lis(pairs):
    """The longest subsequence of `pairs` (sorted by i) increasing in j; the first such in
    pair order (deterministic)."""
    import bisect
    tails, tails_j, prev = [], [], [None] * len(pairs)
    for k, (_, j) in enumerate(pairs):
        x = bisect.bisect_left(tails_j, j)
        if x > 0:
            prev[k] = tails[x - 1]
        if x == len(tails):
            tails.append(k)
            tails_j.append(j)
        else:
            tails[x] = k
            tails_j[x] = j
    out, k = [], tails[-1] if tails else None
    while k is not None:
        out.append(pairs[k])
        k = prev[k]
    return out[::-1]


def hunks(a, b):
    """[(i1, i2, j1, j2)] the non-equal regions of two word lists, by a patience diff written
    here (anchors: words unique in both sides of a region, their longest increasing chain;
    common prefix and suffix peeled; an anchorless region small enough goes to difflib with
    autojunk off, a larger one is one region).  Pure Python and deterministic: the hunks, and
    so the recorded hashes, do not depend on the git or diff installed (git 2.53 in WSL and
    2.54 on Windows split translate_block_arm's 90k words differently)."""
    out = []
    stack = [(0, len(a), 0, len(b))]
    while stack:
        alo, ahi, blo, bhi = stack.pop()
        while alo < ahi and blo < bhi and a[alo] == b[blo]:
            alo += 1
            blo += 1
        while alo < ahi and blo < bhi and a[ahi - 1] == b[bhi - 1]:
            ahi -= 1
            bhi -= 1
        if alo == ahi or blo == bhi:
            if alo < ahi or blo < bhi:
                out.append((alo, ahi, blo, bhi))
            continue
        ca, cb = {}, {}
        for i in range(alo, ahi):
            ca[a[i]] = ca.get(a[i], 0) + 1
        posb = {}
        for j in range(blo, bhi):
            cb[b[j]] = cb.get(b[j], 0) + 1
            posb[b[j]] = j
        pairs = [(i, posb[a[i]]) for i in range(alo, ahi)
                 if ca[a[i]] == 1 and cb.get(a[i]) == 1]
        if not pairs and (ahi - alo) * (bhi - blo) > SMALL:
            # no unique word: the rarest words that occur equally often on both sides, their
            # k-th occurrences paired (histogram diff's idea), so a big region still splits
            rare = sorted((n, w) for w, n in ca.items() if cb.get(w) == n)
            if rare:
                n0 = rare[0][0]
                ws = {w for n, w in rare if n == n0}
                occ_a, occ_b = {}, {}
                for i in range(alo, ahi):
                    if a[i] in ws:
                        occ_a.setdefault(a[i], []).append(i)
                for j in range(blo, bhi):
                    if b[j] in ws:
                        occ_b.setdefault(b[j], []).append(j)
                pairs = sorted(p for w in ws for p in zip(occ_a[w], occ_b[w]))
        if not pairs:
            if (ahi - alo) * (bhi - blo) <= SMALL:
                sm = difflib.SequenceMatcher(None, a[alo:ahi], b[blo:bhi], autojunk=False)
                for tag, i1, i2, j1, j2 in sm.get_opcodes():
                    if tag != 'equal':
                        out.append((alo + i1, alo + i2, blo + j1, blo + j2))
            else:
                out.append((alo, ahi, blo, bhi))
            continue
        chain = _lis(pairs)
        segs, pi, pj = [], alo, blo
        for i, j in chain:
            segs.append((pi, i, pj, j))
            pi, pj = i + 1, j + 1
        segs.append((pi, ahi, pj, bhi))
        stack.extend(reversed(segs))
    out.sort()
    # adjacent regions (an anchor between them was not equal context) stay separate hunks
    return out


def chunks(R, L, sr, sl):
    """-> (code hunks, number of layout-only hunks).  A hunk whose words pair up one for one as
    layout pairs (layout_pair) is the two builds' different data layout, not different code: it
    is counted, not hashed.  (So an edit that ONLY changes one load/store/addiu immediate in one
    build is not seen in a function that already differed; a baseline-`same` function is held by
    func_diff's exact rule instead.)"""
    (a, wa), (b, wb) = norm_words(R, sr), norm_words(L, sl)
    ops = hunks(a, b)
    res = []
    nlay = 0
    for i1, i2, j1, j2 in ops:
        if i2 - i1 == j2 - j1 and all(layout_pair(wa[i1 + k], wb[j1 + k]) for k in range(i2 - i1)):
            nlay += 1
            continue
        txt = '|'.join(a[i1:i2]) + ' <> ' + '|'.join(b[j1:j2])
        h = hashlib.sha1(txt.encode('latin-1')).hexdigest()[:12]
        res.append(dict(h=h, rel=(sr['value'] + 4 * i1, i2 - i1), lab=(sl['value'] + 4 * j1, j2 - j1),
                        a=a[i1:i2], b=b[j1:j2]))
    return res, nlay


def compare(R, L, key, kind):
    """-> dict(state, d, chunks)"""
    a, b = (R.funcs if kind == 'fn' else R.objs).get(key), (L.funcs if kind == 'fn' else L.objs).get(key)
    if a is None and b is None:
        return dict(state='absent')
    if a is None:
        return dict(state='lab-only')
    if b is None:
        return dict(state='rel-only')
    d = (b['value'] - a['value']) & (WAY - 1)
    ca, cb = R.elf.read(a['value'], a['size']), L.elf.read(b['value'], b['size'])
    if kind == 'data':
        if ca is None or cb is None:          # .bss: the size and place are the thing
            same = a['size'] == b['size']
        else:
            same = ca == cb or M.compare_data(R.space, L.space, ca, cb)
        return dict(state='same' if same else 'diff', d=d, chunks=[] if same else
                    [dict(h=hashlib.sha1((ca or b'') + b'<>' + (cb or b'')).hexdigest()[:12],
                          rel=(a['value'], a['size']), lab=(b['value'], b['size']), a=[], b=[])])
    if ca == cb and a['value'] == b['value']:
        return dict(state='same', d=d, chunks=[])
    r = M.compare_words(R.space, L.space, a['value'], b['value'], ca, cb, stop_first=True)
    if r['ok']:
        return dict(state='same', d=d, chunks=[])
    ch, nlay = chunks(R, L, a, b)
    return dict(state='diff' if ch else 'layout', d=d, chunks=ch, nlay=nlay,
                size=(a['size'], b['size']))


def in_hot_block(B, key):
    s = B.funcs.get(key)
    lo, hi = B.abs.get('_ftext'), B.abs.get('__pin_from_frame_loop')
    return bool(s and lo is not None and hi is not None and lo <= s['value'] < hi)


def measure(R, L, spec):
    fs = frame_set(R, L, spec)
    rows = {}
    for k in sorted(fs):
        kind, why, rig = fs[k]
        c = compare(R, L, k, kind)
        c.update(kind=kind, why=why, rig=rig,
                 hot=kind == 'fn' and in_hot_block(R, k) and in_hot_block(L, k))
        rows[k] = c
    return rows


# ------------------------------------------------------------------ baseline file
def fmt_row(k, c):
    kind = 'data' if c['kind'] == 'data' else 'fn'
    if c['rig']:
        return '%-4s %-48s rigonly' % (kind, k)
    if c['state'] in ('rel-only', 'lab-only', 'absent'):
        return '%-4s %-48s %s' % (kind, k, c['state'])
    s = '%-4s %-48s %-6s d=0x%04x' % (kind, k, c['state'], c['d'])
    if c.get('hot'):
        s += ' hot'
    if c['state'] == 'diff':
        s += ' n=%d c=%s' % (len(c['chunks']), ','.join(sorted(x['h'] for x in c['chunks'])))
    return s


def read_base(path):
    base = {}
    for ln in open(path):
        ln = ln.strip()
        if not ln or ln.startswith('#'):      # keys carry '#N' (address ordinals): no inline comments
            continue
        p = ln.split()
        kind, key, state = p[0], p[1], p[2]
        r = dict(kind=kind, state=state)
        for f in p[3:]:
            if f.startswith('d='):
                r['d'] = int(f[2:], 16)
            elif f.startswith('c='):
                r['c'] = f[2:].split(',') if f[2:] else []
        base[key] = r
    return base


def multiset_sub(cand, base):
    left = {}
    for h in base:
        left[h] = left.get(h, 0) + 1
    extra = []
    for h in cand:
        if left.get(h, 0):
            left[h] -= 1
        else:
            extra.append(h)
    return extra, sum(left.values())


def placement_held(c, b):
    """Where the I-cache set rule is held: members at the same set in release and lab at the
    baseline (d 0: the core's hot path), and every member inside the hot block (its row pins
    make an offset holdable there, so a one-build size change is a failure).  Elsewhere the
    two builds never shared a set: the frontend's per-frame code outside the hot block sits
    among cold code whose size differs per build (the Lab build's main_psp.o is ~26 KB
    larger), so any one-build cold edit moves it.  There the offset is reported, not held:
    there is no parity to lose, and holding the old offset would fail every frontend change."""
    return (b or {}).get('d', 0) == 0 or bool(c.get('hot'))


def check(rows, base):
    """-> (failures [str], notes [str])"""
    fails, notes = [], []
    for k, c in rows.items():
        b = base.get(k)
        if c['rig']:
            continue
        st = c['state']
        if b is None:
            if st in ('absent',):
                continue
            if st in ('same', 'layout'):
                if c['d'] != 0:
                    msg = ('%s: NEW in the per-frame set, release and lab at different I-cache '
                           'sets (d=0x%04x)' % (k, c['d']))
                    (fails if c.get('hot') else notes).append(msg)
                continue
            fails.append('%s: NEW in the per-frame set and not the same code in release and lab '
                         '(%s%s)' % (k, st, '' if 'd' not in c else ', d=0x%04x' % c['d']))
            continue
        bs = b['state']
        if bs == 'rigonly':
            continue
        if st in ('absent',):
            continue
        if bs in ('rel-only', 'lab-only'):
            if st not in (bs, 'same') or (st == 'same' and c['d'] != 0):
                fails.append('%s: was %s at the baseline, now %s' % (k, bs, st))
            elif st == 'same':
                notes.append('%s: was %s, now identical in both (re-record to hold it)' % (k, bs))
            continue
        if st in ('rel-only', 'lab-only'):
            fails.append('%s: in both builds at the baseline, now %s' % (k, st))
            continue
        if c['d'] != b.get('d', 0):
            if placement_held(c, b):
                fails.append('%s: release/lab offset mod 8 KiB was 0x%04x, now 0x%04x (an I-cache '
                             'set difference that the baseline did not have)'
                             % (k, b.get('d', 0), c['d']))
            else:
                notes.append('%s: release/lab offset mod 8 KiB 0x%04x -> 0x%04x (never the same '
                             'set; outside the hot block: reported, not held)'
                             % (k, b.get('d', 0), c['d']))
        if bs == 'same':
            if st != 'same':
                fails.append('%s: identical at the baseline, now release and lab differ (%s, %d code '
                             'hunk(s))' % (k, st, len(c['chunks'])))
            continue
        if st == 'same':
            notes.append('%s: was %s at the baseline, now identical (re-record to hold it)' % (k, bs))
            continue
        if bs == 'layout':
            if st != 'layout':
                fails.append('%s: differed only by data layout at the baseline, now by code too '
                             '(%d hunk(s))' % (k, len(c['chunks'])))
            continue
        # baseline diff
        extra, fewer = multiset_sub([x['h'] for x in c['chunks']], b.get('c', []))
        if extra:
            fails.append('%s: %d release/lab hunk(s) the baseline did not have: %s'
                         % (k, len(extra), ', '.join(extra[:12])))
        elif fewer:
            notes.append('%s: %d fewer release/lab hunk(s) than the baseline (re-record to hold it)'
                         % (k, fewer))
    for k, b in base.items():
        if k not in rows and b['state'] not in ('rigonly', 'absent'):
            notes.append('%s: in the baseline, no longer in the per-frame set' % k)
    return fails, notes


# ------------------------------------------------------------------ the delta rule (vs the parent)
def equiv(A, B, sa, sb):
    ca, cb = A.elf.read(sa['value'], sa['size']), B.elf.read(sb['value'], sb['size'])
    if ca == cb and sa['value'] == sb['value']:
        return True
    if ca is None or cb is None:
        return False
    return M.compare_words(A.space, B.space, sa['value'], sb['value'], ca, cb, stop_first=True)['ok']


def delta_hunks(A, B, sa, sb):
    """The change from one commit to the next in ONE flavour, as a sorted list of hunk texts
    (data-blind, layout-only hunks dropped).  Same-flavour neighbours share their register
    allocation, so a change made alike in both flavours gives the same texts."""
    (a, wa), (b, wb) = norm_words(A, sa), norm_words(B, sb)
    ops = hunks(a, b)
    out = []
    for i1, i2, j1, j2 in ops:
        if i2 - i1 == j2 - j1 and all(layout_pair(wa[i1 + k], wb[j1 + k]) for k in range(i2 - i1)):
            continue
        out.append('|'.join(a[i1:i2]) + ' <> ' + '|'.join(b[j1:j2]))
    return sorted(out)


def delta_check(R, L, RR, RL, rows, spec):
    """The commuting square: per member, the parent pair (RR, RL) -> this pair (R, L).  Passes
    when neither flavour changed, when both changed by the same hunks, or when the release/lab
    hunks now are a sub-multiset of the parent's; the offset difference mod 8 KiB must hold."""
    fails, notes, alike = [], [], set()
    for k, c in rows.items():
        if c['rig'] or c['kind'] != 'fn':
            continue
        nr, nl = R.funcs.get(k), L.funcs.get(k)
        rr, rl = RR.funcs.get(k), RL.funcs.get(k)
        if not (nr and nl and rr and rl):
            continue                       # new / gone in a flavour: the base rule judges it
        d_ref = (rl['value'] - rr['value']) & (WAY - 1)
        if c['d'] != d_ref and placement_held(c, {'d': d_ref}):
            fails.append('%s: release/lab offset mod 8 KiB 0x%04x at the parent, 0x%04x now '
                         '(release %04x -> %04x, lab %04x -> %04x)' % (
                             k, d_ref, c['d'], rr['value'] & 0x1fff, nr['value'] & 0x1fff,
                             rl['value'] & 0x1fff, nl['value'] & 0x1fff))
        sr, sl = equiv(RR, R, rr, nr), equiv(RL, L, rl, nl)
        if sr and sl:
            continue
        dr = [] if sr else delta_hunks(RR, R, rr, nr)
        dl = [] if sl else delta_hunks(RL, L, rl, nl)
        if dr == dl:
            if dr:
                alike.add(k)
                notes.append('%s: changed alike in release and lab (%d hunk(s))' % (k, len(dr)))
            continue
        ref_c = compare(RR, RL, k, 'fn')
        extra, _ = multiset_sub([x['h'] for x in c.get('chunks', [])],
                                [x['h'] for x in ref_c.get('chunks', [])])
        if not extra and (ref_c['state'] != 'same' or c['state'] == 'same'):
            if c['state'] != 'same' or ref_c['state'] != 'same':
                notes.append('%s: release and lab changed differently, but the release/lab '
                             'divergence did not grow' % k)
            continue
        fails.append('%s: changed differently in release (%d hunk(s)) and lab (%d hunk(s)) vs the '
                     'parent: a release-only or lab-only change of per-frame code' % (k, len(dr), len(dl)))
    return fails, notes, alike


# ------------------------------------------------------------------ commands
def elf_label(path):
    """runs/TAG/release/gpsp_adhoc.elf -> TAG/release, plus the ELF's sha256 (12)."""
    p = os.path.abspath(path)
    h = hashlib.sha256(open(p, 'rb').read()).hexdigest()[:12]
    return '%s/%s (elf %s)' % (os.path.basename(os.path.dirname(os.path.dirname(p))),
                               os.path.basename(os.path.dirname(p)), h)


def show_chunk(x, out):
    out.append('    hunk %s  release %08x +%d  |  lab %08x +%d' % (
        x['h'], x['rel'][0], x['rel'][1], x['lab'][0], x['lab'][1]))
    for i in range(max(len(x['a']), len(x['b']))):
        l = x['a'][i] if i < len(x['a']) else ''
        r = x['b'][i] if i < len(x['b']) else ''
        out.append('      %-44s | %s' % (l[:44], r[:60]))


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if '--selftest' in argv:
        import subprocess
        t = os.path.join(HERE, '..', 'tests', 'test_frame_parity.py')
        return subprocess.call([sys.executable, t])
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=('set', 'measure', 'record', 'check', 'chunks'))
    ap.add_argument('rel'); ap.add_argument('lab')
    ap.add_argument('name', nargs='?')
    ap.add_argument('--spec', default=SPEC)
    ap.add_argument('--base', default=BASE)
    ap.add_argument('--ref-rel', help='check: the parent commit\'s release ELF (the delta rule)')
    ap.add_argument('--ref-lab', help='check: the parent commit\'s lab ELF')
    ap.add_argument('--out')
    ap.add_argument('--note', default='')
    ap.add_argument('-v', action='store_true')
    o = ap.parse_args(argv)
    for p in (o.rel, o.lab, o.spec):
        if not os.path.exists(p):
            print('frame_parity: no such file: %s' % p)
            return 2
    spec = read_spec(o.spec)
    R, L = Build(o.rel), Build(o.lab)
    if o.cmd == 'set':
        fs = frame_set(R, L, spec)
        for k in sorted(fs):
            kind, why, rig = fs[k]
            print('%-4s %-48s %s%s' % (kind, k, why, '  [rigonly: %s]' % rig[1] if rig else ''))
        print('%d members' % len(fs))
        return 0
    if o.cmd == 'chunks':
        rows = measure(R, L, spec)
        c = rows.get(o.name)
        if c is None:
            print('frame_parity: %s is not in the per-frame set' % o.name)
            return 2
        out = [fmt_row(o.name, c)]
        for x in c.get('chunks', []):
            show_chunk(x, out)
        print('\n'.join(out))
        return 0
    rows = measure(R, L, spec)
    if o.cmd in ('measure', 'record'):
        lines = [fmt_row(k, c) for k, c in sorted(rows.items())]
        quiet = [fmt_row(k, c) for k, c in sorted(rows.items())
                 if c['rig'] or c['state'] != 'same' or c['d'] != 0]
        st = {}
        for c in rows.values():
            s = 'rigonly' if c['rig'] else c['state']
            st[s] = st.get(s, 0) + 1
        summary = ', '.join('%s %d' % kv for kv in sorted(st.items()))
        if o.cmd == 'measure':
            print('\n'.join(lines if o.v else quiet))
            print('per-frame set: %d (%s)' % (len(rows), summary))
            return 0
        hdr = ['# frame_parity.base -- GENERATED by tools/layout/frame_parity.py record; never hand-edit.',
               '# The release/lab parity baseline of the per-frame set (see the tool\'s docstring).',
               '# Re-recorded only with a reason in the commit (the ratchet: check fails on any new',
               '# release/lab difference, and passes with a note when one disappears).',
               '# release: %s' % elf_label(o.rel),
               '# lab:     %s' % elf_label(o.lab),
               '# %d members: %s' % (len(rows), summary)]
        if o.note:
            hdr += ['# ' + x for x in o.note.split('\n')]
        txt = '\n'.join(hdr + lines) + '\n'
        out = o.out or o.base
        with open(out, 'w', newline='\n') as fh:
            fh.write(txt)
        print('frame_parity: recorded %d members (%s) -> %s' % (len(rows), summary, out))
        return 0
    if not os.path.exists(o.base):
        print('frame_parity: FAIL: no baseline %s' % o.base)
        return 2
    base = read_base(o.base)
    fails, notes = check(rows, base)
    if o.ref_rel or o.ref_lab:
        if not (o.ref_rel and o.ref_lab and os.path.exists(o.ref_rel) and os.path.exists(o.ref_lab)):
            print('frame_parity: --ref-rel and --ref-lab name the parent build pair (both must exist)')
            return 2
        dfails, dnotes, alike = delta_check(R, L, Build(o.ref_rel), Build(o.ref_lab), rows, spec)
        notes += dnotes
        # a base-file failure on a function the parent pair shows was changed ALIKE in both
        # builds is a stale record, not a new divergence: it still fails, with that diagnosis
        fails = ['%s  [changed alike in release and lab vs the parent: re-record frame_parity.base '
                 'in this commit]' % f if f.split(':', 1)[0] in alike else f for f in fails]
        fails += dfails
    for n in notes:
        print('  note: ' + n)
    for f in fails:
        print('  FAIL: ' + f)
        if o.v:
            k = f.split(':', 1)[0]
            c = rows.get(k)
            if c and c.get('chunks'):
                b = base.get(k, {})
                have = list(b.get('c', []))
                out = []
                for x in c['chunks']:
                    if x['h'] in have:
                        have.remove(x['h'])
                    else:
                        show_chunk(x, out)
                print('\n'.join(out))
    print('frame_parity: %s (%d members, %d new divergence(s), %d note(s))' % (
        'PARITY HOLDS' if not fails else 'NEW RELEASE/LAB DIVERGENCE', len(rows), len(fails), len(notes)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
