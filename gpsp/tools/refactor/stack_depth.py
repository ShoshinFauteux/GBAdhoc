#!/usr/bin/env python3
"""stack_depth.py (T12, gate ST) -- worst-case stack depth of the cb_thread
callbacks against cb_thread's 4,000 B stack (main_psp.c setup_exit_callback,
`sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0, ...)`).

    stack_depth.py ELF [--su DIR] [--roots power_cb,exit_cb] [--base cb_thread]
                   [--limit 4000] [--budget 3200] [--resolve FILE] [--json OUT]
                   [--tree N]
    stack_depth.py --selftest ELF [--su DIR]

What it does
  * Call graph from the LINKED ELF (what actually runs, after inlining, cloning
    and LTO), by decoding every STT_FUNC body:
      jal / bal / bgezal         direct call      (summed)
      j / b<cond> out of the fn  tail call        (summed as a call: upper bound)
      jalr                       INDIRECT call    (flagged, never summed as 0)
      jr rX (rX != ra)           indirect jump    (switch table or indirect tail
                                                   call; flagged separately)
  * Frame of every function = its prologue `addiu sp,sp,-N` (largest single
    decrement; `lui/ori + subu sp,sp,rX` resolved, an unresolved `subu sp` is a
    DYNAMIC frame and flagged).  This covers newlib and the SDK (no .su).
  * With --su DIR, every `-fstack-usage` record (flavours_sidecar.sh writes them
    under <flavour>/su) is matched to its ELF symbol (file + name for statics)
    and the frame used is max(.su, prologue).  Every disagreement is listed:
    for our objects the two must agree, so a mismatch means the scanner (or the
    match) is wrong.  `dynamic` .su qualifiers are flagged.
  * Syscall stubs (.sceStub.text) are leaves with 0 user frame (the kernel runs
    on its own stack).  A `...CB` syscall reachable from a callback is flagged:
    callbacks can be dispatched inside it, i.e. re-entered on the same stack.
  * Recursion (a cycle) is flagged; the cycle edge is not followed.
  * Depth(f) = frame(f) + max(depth(callee)); total = frame(base) + max over
    roots (callbacks run inside cb_thread's sceKernelSleepThreadCB, and
    cb_thread also calls power_cb directly).

Verdict: PASS when total <= budget and nothing is unresolved; FAIL when total >
budget; INCOMPLETE when total <= budget but an indirect call, an unresolved jr,
a dynamic frame, recursion or an unknown call target remains (the figure is
then a LOWER bound).  Resolve indirect sites with --resolve FILE, lines
`caller_symbol  callee[,callee...]   # evidence`, or `caller_symbol  -` for a
jr that is a proven switch table (target inside the function).

What it does NOT prove: kernel-side stack used when the kernel dispatches a
callback into user mode (not modelled; it is added on the same stack only if
the firmware pushes a user-mode frame, unknown), interrupt handlers (they run
on the kernel's stack), and paths through function pointers it was not told
about.  Exit codes: 0 PASS, 1 FAIL, 3 INCOMPLETE, 2 usage.
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf32 import Elf  # noqa: E402

SP, RA, AT = 29, 31, 1
STUB_SEC = '.sceStub.text'


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


class Func:
    __slots__ = ('name', 'addr', 'size', 'file', 'bind', 'frame_pro', 'frame_su',
                 'su_qual', 'dynamic', 'calls', 'tails', 'jalr', 'jr', 'unknown',
                 'syscall', 'su_src', 'jr_switch', 'lw_def', 'luis', 'addr_refs',
                 'multi_dec', 'jr_bad')

    def __init__(self, name, addr, size, file, bind):
        self.name, self.addr, self.size, self.file, self.bind = name, addr, size, file, bind
        self.frame_pro = 0
        self.frame_su = None
        self.su_qual = None
        self.su_src = None
        self.dynamic = []          # addresses of unresolved subu sp
        self.calls = {}            # callee addr -> first site
        self.tails = {}
        self.jalr = []             # sites
        self.jr = []
        self.unknown = []          # (site, target) with no function there
        self.syscall = False
        self.jr_switch = None
        self.lw_def = {}
        self.luis = set()
        self.addr_refs = set()     # function addresses built with lui+addiu/ori
        self.multi_dec = []        # a second, different addiu sp,sp,-N
        self.jr_bad = []           # jr sites not proven to be table dispatch

    @property
    def frame(self):
        return max(self.frame_pro, self.frame_su or 0)


def load_funcs(elf):
    """{addr: Func}, one per address (prefer global names, then the longest
    size), from STT_FUNC symbols with a size, plus zero-size STT_FUNC/NOTYPE
    labels in .sceStub.text (stubs are 8 B each)."""
    by = {}
    starts = sorted(set(x['value'] for x in elf.symbols if x['value']))
    import bisect
    for s in elf.symbols:
        if not s['section'] or s['value'] == 0:
            continue
        sec = elf.by_name.get(s['section'])
        if not sec or not sec['flags'] & 4:          # SHF_EXECINSTR
            continue
        size = s['size']
        if s['type'] != 2 and s['section'] != STUB_SEC:
            continue
        if size == 0:
            if s['section'] == STUB_SEC:
                size = 8
            elif s['bind'] != 0 or s['type'] == 2:
                # an asm label (e.g. _fini in .fini): to the next symbol
                i = bisect.bisect_right(starts, s['value'])
                end = min(starts[i] if i < len(starts) else s['value'] + 4,
                          sec['addr'] + sec['size'], s['value'] + 0x1000)
                size = end - s['value']
                if size <= 0:
                    continue
            else:
                continue
        f = Func(s['name'], s['value'], size, s['file'], s['bind'])
        old = by.get(s['value'])
        if old is None or (old.bind == 0 and s['bind'] != 0) or \
                (old.bind == s['bind'] and size > old.size):
            by[s['value']] = f
    for f in by.values():
        if elf.section_of(f.addr) and elf.section_of(f.addr)['name'] == STUB_SEC:
            f.syscall = True
    return by


def _dests(w):
    """GPRs an instruction may write (conservative; used to drop tracked
    constants).  Loads, ALU ops, SPECIAL2/3 (mul, ext/ins, seb/seh), COP moves."""
    op = w >> 26
    rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    if op in (0x00, 0x1c):
        return (rd,)
    if op == 0x1f:
        return (rt, rd)
    if 0x08 <= op <= 0x0f or 0x20 <= op <= 0x27 or op in (0x30, 0x38):
        return (rt,)
    if op in (0x10, 0x11, 0x12) and rs in (0, 2, 3):
        return (rt,)
    if op == 0x03 or (op == 0x01 and rt in (0x10, 0x11, 0x12, 0x13)):
        return (RA,)
    return ()


def _is_cti(w):
    """branch or jump (has a delay slot)"""
    op = w >> 26
    return op in (0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17) or         (op == 0x01 and (w >> 16) & 31 in (0, 1, 2, 3, 0x10, 0x11, 0x12, 0x13)) or         (op == 0 and (w & 0x3f) in (0x08, 0x09))


def scan(elf, funcs):
    addrs = sorted(funcs)
    for a in addrs:
        f = funcs[a]
        if f.syscall:
            continue
        code = elf.read(f.addr, f.size)
        if not code:
            continue
        n = len(code) // 4
        words = struct.unpack_from('<%dI' % n, code)
        # constants are tracked only along straight-line code: forget them at
        # every intra-function branch target and after every branch/jump (its
        # delay slot included), so a value from another path is never used
        forget = set()
        for i, w in enumerate(words):
            op = w >> 26
            if _is_cti(w):
                forget.add(i + 2)
            if op in (0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17) or                     (op == 0x01 and (w >> 16) & 31 in (0, 1, 2, 3)):
                t = (f.addr + 4 * i + 4 + (s16(w & 0xffff) << 2)) & 0xffffffff
                if f.addr <= t < f.addr + f.size:
                    forget.add((t - f.addr) // 4)
            elif op == 0x02:
                t = ((f.addr + 4 * i + 4) & 0xf0000000) | ((w & 0x03ffffff) << 2)
                if f.addr <= t < f.addr + f.size:
                    forget.add((t - f.addr) // 4)
        regval = {}                 # reg -> known constant (lui/ori/addiu $0)
        f.lw_def = {}               # jr site -> (base reg value or None, lw imm) of its target reg
        last_def = {}               # reg -> index of the instruction that last wrote it
        luis = set()
        decs = set()
        for i, w in enumerate(words):
            if i in forget:
                regval.clear()
                last_def.clear()
            pc = f.addr + 4 * i
            op = w >> 26
            rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            imm = w & 0xffff
            if op == 0x09 and rs == SP and rt == SP and s16(imm) < 0:     # addiu sp,sp,-N
                # several DIFFERENT decrements (e.g. __ssvfiscanf_r: -416 in
                # the prologue, -8 later) may nest: sum the distinct values
                # (upper bound; a shrink-wrapped duplicate prologue repeats
                # the same N and is counted once)
                if -s16(imm) not in decs:
                    if decs:
                        f.multi_dec.append(pc)
                    decs.add(-s16(imm))
                f.frame_pro = max(f.frame_pro, sum(decs))
            elif op == 0 and (w & 0x3f) == 0x23 and rd == SP and rs == SP:  # subu sp,sp,rt
                if rt in regval and 0 < regval[rt] < 0x100000:
                    f.frame_pro = max(f.frame_pro, regval[rt])
                else:
                    f.dynamic.append(pc)
            elif op == 0 and (w & 0x3f) == 0x21 and rd == SP and rs == SP:  # addu sp,sp,rt (neg const)
                if rt in regval and regval[rt] >= 0x80000000:
                    f.frame_pro = max(f.frame_pro, 0x100000000 - regval[rt])
                elif rt not in regval:
                    f.dynamic.append(pc)
            # calls / jumps
            if op == 0x03:                                   # jal
                t = ((pc + 4) & 0xf0000000) | ((w & 0x03ffffff) << 2)
                edge(funcs, f, t, pc, call=True)
            elif op == 0x02:                                 # j
                t = ((pc + 4) & 0xf0000000) | ((w & 0x03ffffff) << 2)
                if not f.addr <= t < f.addr + f.size:
                    edge(funcs, f, t, pc, call=False)
            elif op == 0x01 and rt in (0x10, 0x11, 0x12, 0x13):  # bltzal/bgezal(l), bal
                t = (pc + 4 + (s16(imm) << 2)) & 0xffffffff
                edge(funcs, f, t, pc, call=True)
            elif op in (0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17) or                     (op == 0x01 and rt in (0, 1, 2, 3)):     # conditional branches
                t = (pc + 4 + (s16(imm) << 2)) & 0xffffffff
                if not f.addr <= t < f.addr + f.size:
                    edge(funcs, f, t, pc, call=False)
            elif op == 0 and (w & 0x3f) == 0x09:             # jalr
                f.jalr.append(pc)
            elif op == 0 and (w & 0x3f) == 0x08 and rs != RA:  # jr rX
                f.jr.append(pc)
                j = last_def.get(rs)
                if j is not None and words[j] >> 26 == 0x23:    # lw rX, imm(rB)
                    f.lw_def[pc] = s16(words[j] & 0xffff)
            # constant tracking (large frames, callback addresses); every
            # other write to a register drops what was known about it
            if op == 0x0f:                                   # lui
                regval[rt] = (imm << 16) & 0xffffffff
            elif op == 0x0d and (rs in regval or rs == 0):   # ori
                regval[rt] = (regval[rs] if rs else 0) | imm
            elif op == 0x09 and rt != SP and (rs in regval or rs == 0):  # addiu / li
                regval[rt] = ((regval[rs] if rs else 0) + s16(imm)) & 0xffffffff
            else:
                for r in _dests(w):
                    regval.pop(r, None)
            if op in (0x0f, 0x0d, 0x09) and rt in regval and rt != SP:
                luis.add(regval[rt])
                if op != 0x0f and regval[rt] in funcs:
                    f.addr_refs.add(regval[rt])
            for r in _dests(w):
                last_def[r] = i
        f.luis = luis


def edge(funcs, f, t, pc, call):
    g = funcs.get(t)
    if g is None:
        f.unknown.append((pc, t))
        return
    (f.calls if call else f.tails).setdefault(t, pc)


def find_jump_tables(elf, funcs):
    """classify every `jr rX` (rX != ra) site.  A site is switch dispatch only
    when (1) a table of >= 2 consecutive words pointing strictly INSIDE the
    same function (not at its entry, so a {f, f} pointer pair does not count)
    exists in a non-exec alloc section (GCC's .rodata jump tables), and (2) the
    jr's register was last written by `lw rX, lo(rB)` where some constant the
    function builds (lui %hi, or lui+addiu) plus lo is the start of, or lies
    inside, one of those tables.  Any other jr in the function (an indirect
    tail call `jr t9` next to a switch, say) stays flagged in f.jr_bad.
    f.jr_switch = first table address or None."""
    lo_hi = sorted((f.addr, f.addr + f.size, f) for f in funcs.values() if f.jr)
    if not lo_hi:
        return
    import bisect
    starts = [x[0] for x in lo_hi]
    tables = {}                 # owner addr -> [(start, end)]
    for sec in elf.sections:
        if not sec['flags'] & 2 or sec['flags'] & 4 or sec['type'] == 8 or sec['size'] < 8:
            continue
        d = elf.data[sec['offset']:sec['offset'] + sec['size']]
        n = len(d) // 4
        ws = struct.unpack_from('<%dI' % n, d)
        prev, run0 = None, 0
        for i, w in enumerate(ws + (0,)):
            j = bisect.bisect_right(starts, w) - 1
            owner = lo_hi[j][2] if j >= 0 and lo_hi[j][0] < w < lo_hi[j][1] else None
            if owner is not None and owner is prev:
                continue
            if prev is not None and i - run0 >= 2:
                tables.setdefault(prev.addr, []).append((sec['addr'] + 4 * run0, sec['addr'] + 4 * i))
            prev, run0 = owner, i
    for f in funcs.values():
        f.jr_bad = []
        tl = tables.get(f.addr, [])
        f.jr_switch = tl[0][0] if tl else None
        for pc in f.jr:
            lo = f.lw_def.get(pc)
            ok = lo is not None and any(
                ts <= ((c + lo) & 0xffffffff) < te for c in f.luis for ts, te in tl)
            if not ok:
                f.jr_bad.append(pc)


SU_RE = re.compile(r'^(?P<file>[^:]+):(?P<line>\d+):(?P<col>\d+):(?P<name>\S+)\s+(?P<n>\d+)\s+(?P<q>\S+)')


def load_su(su_dir, funcs):
    """match .su records to ELF functions; returns (matched, unmatched_records)"""
    by_global = {}
    by_local = {}
    for f in funcs.values():
        if f.bind != 0:
            by_global.setdefault(f.name, []).append(f)
        else:
            by_local.setdefault((os.path.basename(f.file or ''), f.name), []).append(f)
    matched, unmatched = 0, []
    for root, _, files in os.walk(su_dir):
        for fn in files:
            if not fn.endswith('.su'):
                continue
            p = os.path.join(root, fn)
            for line in open(p, errors='replace'):
                m = SU_RE.match(line.strip())
                if not m:
                    continue
                name, n, q = m.group('name'), int(m.group('n')), m.group('q')
                src = os.path.basename(m.group('file'))
                cands = by_local.get((src, name)) or by_global.get(name) or []
                if len(cands) != 1:
                    unmatched.append((os.path.relpath(p, su_dir), name, n, len(cands)))
                    continue
                f = cands[0]
                f.frame_su = max(f.frame_su or 0, n)
                f.su_qual = q
                f.su_src = '%s:%s' % (src, m.group('line'))
                matched += 1
    return matched, unmatched


def load_resolve(path, funcs):
    """caller -> list of callee addrs ([] = no call through that site);
    plus the set of pruned function names (`!prune NAME`), plus a list of
    stale-resolution problems:
      * `sites=N` (required on every caller line) must equal the caller's
        jalr + jr(rX != ra) count in this ELF: a NEW indirect site in a reviewed
        caller is then not silently covered by the old review;
      * a listed callee absent from an ELF that has the caller is an error
        (a renamed clone, e.g. foo -> foo.part.0, would otherwise drop out of
        the max silently) unless it is written `?name` (flavour-dependent)."""
    by_name = {}
    for f in funcs.values():
        by_name.setdefault(f.name, []).append(f)
    res = {}
    prune = set()
    stale = []
    if not path:
        return res, prune, stale
    for line in open(path):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        caller, callees = line.split(None, 1)
        if caller == '!prune':
            prune.add(callees.strip())
            continue
        if caller == '!unroll':
            # `!unroll CALLER CALLEE [-PRUNED ...]`: that cycle edge is taken
            # once; the -names are not followed inside the second pass
            toks = callees.split()
            prune.add(('unroll', toks[0], toks[1]) + tuple(t[1:] for t in toks[2:] if t.startswith('-')))
            continue
        toks = callees.split()
        sites = None
        for t in toks[1:]:
            if t.startswith('sites='):
                sites = int(t[6:])
        callees = toks[0]
        if caller not in by_name:
            continue                # not in this flavour's ELF (its sites then stay unresolved under any new name)
        # a name may be several statics (e.g. the core and its gbab_ twin);
        # the line applies to every copy, so every copy must match the count
        have = '/'.join(str(len(cf.jalr) + len(cf.jr)) for cf in by_name[caller])
        if sites is None:
            stale.append((caller, 'no sites=N on the resolve line', have))
        elif any(len(cf.jalr) + len(cf.jr) != sites for cf in by_name[caller]):
            stale.append((caller, 'sites=%d reviewed, %s in this ELF' % (sites, have), have))
        out = []
        if callees.strip() != '-':
            for c in callees.split(','):
                c = c.strip()
                opt = c.startswith('?')
                c = c.lstrip('?')
                g = by_name.get(c)
                if not g:
                    if not opt:
                        stale.append((caller, 'callee %s absent from this ELF' % c, have))
                    continue
                if len(g) != 1:
                    raise SystemExit('resolve: callee %s is ambiguous in the ELF' % c)
                out.append(g[0].addr)
        res[caller] = out
    return res, prune, stale


def analyse(funcs, roots, base, resolve, prune=(), with_base=True):
    by_name = {}
    for f in funcs.values():
        by_name.setdefault(f.name, []).append(f)
    unroll = {(x[1], x[2]): set(x[3:]) for x in prune if isinstance(x, tuple)}
    names_pruned = {x for x in prune if isinstance(x, str)}
    memo = {}
    issues = {'jalr': [], 'jr': [], 'dynamic': [], 'recursion': [], 'unknown': [],
              'cb_syscall': [], 'su_dynamic': [], 'pruned': [], 'unrolled': []}
    seen_issue = set()

    def note(kind, item):
        if (kind, item) not in seen_issue:
            seen_issue.add((kind, item))
            issues[kind].append(item)

    def depth(a, stack):
        if a in memo:
            return memo[a]
        f = funcs[a]
        if f.name in names_pruned:
            note('pruned', f.name)
            memo[a] = (0, [a])
            return memo[a]
        stack.append(a)
        best, path = 0, []
        succ = dict(f.calls)
        for t, pc in f.tails.items():
            succ.setdefault(t, pc)
        if f.name in resolve:
            for t in resolve[f.name]:
                succ.setdefault(t, 'resolved')
        else:
            for pc in f.jalr:
                note('jalr', (f.name, pc))
            for pc in f.jr_bad:
                note('jr', (f.name, pc))
        for pc in f.dynamic:
            note('dynamic', (f.name, pc))
        if f.su_qual and f.su_qual.startswith('dynamic') and f.su_qual != 'dynamic,bounded':
            note('su_dynamic', (f.name, f.su_qual))
        for pc, t in f.unknown:
            note('unknown', (f.name, pc, t))
        if f.syscall and f.name.endswith('CB'):
            note('cb_syscall', f.name)
        for t in sorted(succ):
            if t in stack:
                if (f.name, funcs[t].name) in unroll:
                    # bounded recursion: walk the callee once more with this
                    # caller pruned, so the cycle is taken exactly once
                    note('unrolled', (f.name, funcs[t].name))
                    sub = analyse(funcs, [funcs[t].name], base, resolve,
                                  set(prune) | {f.name} | unroll[(f.name, funcs[t].name)],
                                  with_base=False)[0][funcs[t].name]
                    if sub[0] > best:
                        best, path = sub[0], sub[1]
                    continue
                note('recursion', (f.name, funcs[t].name))
                continue
            d, p = depth(t, stack)
            if d > best:
                best, path = d, p
        stack.pop()
        r = (f.frame + best, [a] + path)
        memo[a] = r
        return r

    out = {}
    for r in roots:
        fs = by_name.get(r)
        if not fs or len(fs) != 1:
            raise SystemExit('root %s is %s in the ELF' % (r, 'absent' if not fs else 'ambiguous'))
        out[r] = depth(fs[0].addr, [])
    bf = by_name.get(base)
    if not bf or len(bf) != 1:
        raise SystemExit('base %s not found' % base)
    # the base thread's own direct calls run on the same stack too (cb_thread
    # calls power_cb directly; any other callee it gains must be counted)
    if with_base:
        out['<' + base + '>'] = (depth(bf[0].addr, []), )
    return out, bf[0], issues, memo


def run(elf_path, su_dir=None, roots=('power_cb', 'exit_cb'), base='cb_thread',
        limit=4000, budget=3200, resolve_path=None):
    elf = Elf(elf_path)
    funcs = load_funcs(elf)
    scan(elf, funcs)
    su_matched, su_unmatched = (0, [])
    if su_dir:
        su_matched, su_unmatched = load_su(su_dir, funcs)
    find_jump_tables(elf, funcs)
    resolve, prune, stale = load_resolve(resolve_path, funcs)
    raw = analyse(funcs, roots, base, resolve)
    res, basef, issues, memo = analyse(funcs, roots, base, resolve, prune)
    bkey = '<' + base + '>'
    base_self = res.pop(bkey)[0]
    raw_self = raw[0].pop(bkey)[0]
    raw_total = max(raw[1].frame + max(d for d, _ in raw[0].values()), raw_self[0])
    worst_root = max(res, key=lambda r: res[r][0])
    total = basef.frame + res[worst_root][0]
    if base_self[0] > total:
        # the deepest path is a direct call chain of the base, not a callback
        worst_root = bkey
        res[bkey] = (base_self[0] - basef.frame, base_self[1][1:])
        total = base_self[0]
    issues['resolve_stale'] = [s_ for s_ in stale if s_[0] in resolve and
                               any(funcs[a].name == s_[0] for a in memo)]
    # every function whose address the base builds (lui+addiu/ori) must be a
    # root or a direct callee: an unlisted one is a callback the bound ignores
    cbs = {funcs[a].name for a in basef.addr_refs if a in funcs} - set(roots) -         {funcs[a].name for a in basef.calls}
    issues['cb_unlisted'] = sorted(cbs)
    issues['multi_frame'] = [(funcs[a].name, pc) for a in memo for pc in funcs[a].multi_dec]
    mism = sorted((f.name, f.frame_su, f.frame_pro) for f in funcs.values()
                  if f.frame_su is not None and f.frame_su != f.frame_pro)
    unresolved = sum(len(v) for k, v in issues.items() if k not in ('cb_syscall', 'pruned', 'unrolled', 'multi_frame'))
    # (resolve_stale and cb_unlisted count as unresolved; multi_frame is a
    # note: those frames are already summed as an upper bound)
    if total > budget:
        verdict = 'FAIL'
    elif unresolved:
        verdict = 'INCOMPLETE'
    else:
        verdict = 'PASS'
    return dict(elf=elf_path, funcs=funcs, memo=memo, res=res, base=basef,
                total=total, worst_root=worst_root, limit=limit, budget=budget,
                issues=issues, verdict=verdict, su_matched=su_matched,
                su_unmatched=su_unmatched, mism=mism, resolve=resolve, prune=prune,
                raw_total=raw_total, raw=raw[0], switches=sorted(
                    f.name for f in funcs.values() if f.jr and f.jr_switch and f.addr in memo))


def frame_of(funcs, name):
    fs = [f for f in funcs.values() if f.name == name]
    return fs[0] if len(fs) == 1 else None


def report(r, tree=0, out=sys.stdout):
    funcs = r['funcs']
    w = out.write
    w('stack_depth: %s\n' % r['elf'])
    w('  base %s frame %d B (pro %d, su %s)\n' % (r['base'].name, r['base'].frame,
      r['base'].frame_pro, r['base'].frame_su))
    for root, (d, path) in sorted(r['res'].items()):
        w('  root %-10s depth %5d B   worst path: %s\n' % (root, d, ' > '.join(
            '%s(%d)' % (funcs[a].name, funcs[a].frame) for a in path)))
    w('  TOTAL %s + worst root (%s) = %d B   budget %d   limit %d   headroom to limit %d\n'
      % (r['base'].name, r['worst_root'], r['total'], r['budget'], r['limit'],
         r['limit'] - r['total']))
    if r['prune']:
        rd, rp = max(r['raw'].values())
        w('  RAW static bound (no prunes, cycles cut, no unroll) = %d B, via: %s\n' % (r['raw_total'], ' > '.join(
            funcs[a].name for a in rp)))
    if r['switches']:
        w('  jr classified as switch dispatch (jump table found): %s\n' % ', '.join(r['switches']))
    reach = [funcs[a] for a in r['memo']]
    w('  reachable functions: %d (%d syscall stubs); .su matched %d records, %d unmatched\n'
      % (len(reach), sum(1 for f in reach if f.syscall), r['su_matched'], len(r['su_unmatched'])))
    rm = [m for m in r['mism'] if any(f.name == m[0] for f in reach)]
    w('  .su vs prologue disagreements: %d in the whole ELF, %d on reachable functions\n'
      % (len(r['mism']), len(rm)))
    for m in rm[:20]:
        w('    MISMATCH %s su=%s prologue=%s\n' % m)
    for k, v in r['issues'].items():
        if not v:
            continue
        w('  %-11s %d: %s\n' % (k.upper(), len(v), ', '.join(
            ' '.join(('%08x' % x) if isinstance(x, int) else str(x) for x in (it if isinstance(it, tuple) else (it,)))
            for it in v[:12]) + (' ...' if len(v) > 12 else '')))
    if tree:
        w('  largest frames on the reachable graph:\n')
        for f in sorted(reach, key=lambda f: -f.frame)[:tree]:
            w('    %6d  %-40s pro=%-5d su=%-5s %s\n' % (f.frame, f.name, f.frame_pro,
              f.frame_su, f.su_src or ('lib/asm' if f.frame_su is None else '')))
    w('  verdict: %s\n' % r['verdict'])


def to_json(r):
    funcs = r['funcs']
    return dict(elf=r['elf'], total=r['total'], raw_total=r['raw_total'],
                pruned=sorted(str(x) for x in r['prune']), budget=r['budget'], limit=r['limit'],
                verdict=r['verdict'], base=dict(name=r['base'].name, frame=r['base'].frame),
                roots={k: dict(depth=d, path=[funcs[a].name for a in p]) for k, (d, p) in r['res'].items()},
                issues={k: [list(x) if isinstance(x, tuple) else x for x in v] for k, v in r['issues'].items()},
                su_matched=r['su_matched'], su_unmatched=len(r['su_unmatched']),
                mismatches=r['mism'],
                frames={funcs[a].name: funcs[a].frame for a in r['memo']})


# ------------------------------------------------------------------ selftest
def _patch(src, dst, elf, addr, word):
    s = elf.section_of(addr)
    off = s['offset'] + addr - s['addr']
    d = bytearray(open(src, 'rb').read())
    struct.pack_into('<I', d, off, word)
    open(dst, 'wb').write(bytes(d))


def selftest(elf_path, su_dir=None, resolve_path=None):
    import tempfile
    fails = 0
    tmp = tempfile.mkdtemp(prefix='stackdepth.')

    def chk(cond, label):
        nonlocal fails
        print('  %-5s %s' % ('ok' if cond else 'FAIL', label))
        if not cond:
            fails += 1

    base = run(elf_path, su_dir, resolve_path=resolve_path)
    funcs, elf = base['funcs'], Elf(elf_path)
    print('control: total %d B, verdict %s' % (base['total'], base['verdict']))
    for lib in ('vsnprintf', 'fopen', 'fprintf', 'snprintf', 'fclose'):
        f = frame_of(funcs, lib)
        if f is None:
            chk(False, 'library function %s present' % lib)
            continue
        d = base['memo'].get(f.addr)
        if d is None:
            # not reachable from the callbacks in this flavour: compute directly
            d = analyse(funcs, [lib], base['base'].name, base['resolve'], base['prune'])[0][lib]
        print('        %-10s own frame %4d B (prologue), subtree depth %5d B, su=%s'
              % (lib, f.frame_pro, d[0], f.frame_su))
    for lib in ('vsnprintf', 'fopen'):
        f = frame_of(funcs, lib)
        if f is None:
            continue
        d = base['memo'].get(f.addr) or analyse(funcs, [lib], base['base'].name, base['resolve'], base['prune'])[0][lib]
        # PLAN T verify: "its prologue scan finds a non-zero frame for vsnprintf and fopen"
        chk(d[0] > 0 and (f.frame_pro > 0 or any(funcs[a].frame_pro > 0 for a in d[1])),
            '%s: prologue scan gives a non-zero frame (own %d, subtree %d)' % (lib, f.frame_pro, d[0]))
    if su_dir:
        ours = [f for f in funcs.values() if f.frame_su is not None]
        agree = sum(1 for f in ours if f.frame_su == f.frame_pro)
        print('        .su cross-check: %d/%d matched functions agree with the prologue scan'
              % (agree, len(ours)))
        chk(len(ours) > 100 and agree / len(ours) > 0.97, 'prologue scan agrees with -fstack-usage on >97% of matched functions')

    # M1: inflate the frame of the deepest non-root function on the worst path
    path = base['res'][base['worst_root']][1]
    victim = None
    for a in path[1:]:
        f = funcs[a]
        if f.frame_pro > 0 and not f.syscall:
            victim = f
            break
    if victim:
        code = elf.read(victim.addr, victim.size)
        for i in range(len(code) // 4):
            w = struct.unpack_from('<I', code, 4 * i)[0]
            if w >> 26 == 0x09 and (w >> 21) & 31 == SP and (w >> 16) & 31 == SP and s16(w & 0xffff) == -victim.frame_pro:
                bump = max(2000, base['budget'] - base['total'] + 8)
                bump = (bump + 7) & ~7
                newn = victim.frame_pro + bump
                mut = os.path.join(tmp, 'm1.elf')
                _patch(elf_path, mut, elf, victim.addr + 4 * i, (w & 0xffff0000) | ((-newn) & 0xffff))
                r = run(mut, None, resolve_path=resolve_path)  # no su: prologue alone must see it
                b0 = run(elf_path, None, resolve_path=resolve_path)
                chk(r['total'] == b0['total'] + bump and r['total'] > r['budget'] and r['verdict'] == 'FAIL',
                    'M1 %s frame +%d B in the ELF: total %d -> %d, verdict %s'
                    % (victim.name, bump, b0['total'], r['total'], r['verdict']))
                break
    else:
        chk(False, 'M1 no frame on the worst path to inflate')

    # M2: turn the root's first jal into jalr $t9 (an indirect call must be flagged)
    root = frame_of(funcs, 'power_cb')
    code = elf.read(root.addr, root.size)
    for i in range(len(code) // 4):
        w = struct.unpack_from('<I', code, 4 * i)[0]
        if w >> 26 == 0x03:
            mut = os.path.join(tmp, 'm2.elf')
            _patch(elf_path, mut, elf, root.addr + 4 * i, (25 << 21) | (31 << 11) | 0x09)
            r = run(mut, su_dir, resolve_path=resolve_path)
            hit = any(n == 'power_cb' and pc == root.addr + 4 * i for n, pc in r['issues']['jalr'])
            chk(hit and r['verdict'] in ('INCOMPLETE', 'FAIL'),
                'M2 jal->jalr in power_cb at %08x flagged INDIRECT, verdict %s' % (root.addr + 4 * i, r['verdict']))
            break

    # M3: exit_cb gains a call into standby_note's tree (or the deepest function)
    ex = frame_of(funcs, 'exit_cb')
    tgt = frame_of(funcs, 'standby_note') or funcs[path[1]] if len(path) > 1 else None
    if ex and tgt:
        code = elf.read(ex.addr, ex.size)
        # replace the first non-jr instruction that is not a delay slot with jal tgt
        for i in range(len(code) // 4):
            w = struct.unpack_from('<I', code, 4 * i)[0]
            if not (w >> 26 == 0 and (w & 0x3f) == 0x08):
                mut = os.path.join(tmp, 'm3.elf')
                _patch(elf_path, mut, elf, ex.addr + 4 * i, (0x03 << 26) | ((tgt.addr >> 2) & 0x03ffffff))
                r = run(mut, su_dir, resolve_path=resolve_path)
                d0 = base['res']['exit_cb'][0]
                d1 = r['res']['exit_cb'][0]
                chk(d1 > d0 and tgt.name in [r['funcs'][a].name for a in r['res']['exit_cb'][1]],
                    'M3 exit_cb calls %s: exit_cb depth %d -> %d' % (tgt.name, d0, d1))
                break

    # M4: recursion -- a callee on the worst path calls power_cb back
    if victim:
        code = elf.read(victim.addr, victim.size)
        for i in range(len(code) // 4):
            w = struct.unpack_from('<I', code, 4 * i)[0]
            if w >> 26 == 0x03:
                mut = os.path.join(tmp, 'm4.elf')
                _patch(elf_path, mut, elf, victim.addr + 4 * i, (0x03 << 26) | ((root.addr >> 2) & 0x03ffffff))
                r = run(mut, su_dir, resolve_path=resolve_path)
                chk(any(b == 'power_cb' for a, b in r['issues']['recursion']) and r['verdict'] != 'PASS',
                    'M4 %s -> power_cb cycle flagged RECURSION, verdict %s' % (victim.name, r['verdict']))
                break

    # M5: a .su record that disagrees with the ELF is reported
    if su_dir:
        import shutil
        sd = os.path.join(tmp, 'su')
        shutil.copytree(su_dir, sd)
        target = None
        for root_d, _, files in os.walk(sd):
            for fn in files:
                if fn == 'main_psp.su':
                    target = os.path.join(root_d, fn)
        if target:
            txt = open(target).read()
            txt2, n = re.subn(r'(:power_cb\t)(\d+)', lambda m: m.group(1) + str(int(m.group(2)) + 48), txt)
            open(target, 'w').write(txt2)
            r = run(elf_path, sd, resolve_path=resolve_path)
            chk(n == 1 and any(m[0] == 'power_cb' for m in r['mism']) and
                r['res']['power_cb'][0] == base['res']['power_cb'][0] + 48,
                'M5 power_cb .su +48 B: reported as MISMATCH and max() used (%d -> %d)'
                % (base['res']['power_cb'][0], r['res']['power_cb'][0]))
    # M6: a large frame through lui/ori/subu sp is resolved, not missed
    #     (synthetic: decode a 3-word sequence directly)
    fake = Func('fake', 0x1000, 12, None, 1)
    words = [(0x0f << 26) | (AT << 16) | 0x0001,             # lui at,1
             (0x0d << 26) | (AT << 21) | (AT << 16) | 0x1000,  # ori at,at,0x1000
             (SP << 21) | (AT << 16) | (SP << 11) | 0x23]    # subu sp,sp,at

    class _E:
        def read(self, a, n):
            return struct.pack('<3I', *words)

        def section_of(self, a):
            return None
    scan(_E(), {0x1000: fake})
    chk(fake.frame_pro == 0x11000 and not fake.dynamic, 'M6 lui/ori/subu frame resolved to %d B' % fake.frame_pro)
    fake2 = Func('fake2', 0x1000, 12, None, 1)
    words[2] = (SP << 21) | (2 << 16) | (SP << 11) | 0x23     # subu sp,sp,v0 (unknown)
    scan(_E(), {0x1000: fake2})
    chk(bool(fake2.dynamic), 'M6b subu sp,sp,<unknown reg> flagged DYNAMIC')
    # ---- verifier mutants (adversarial pass): each was a false PASS before
    def words_of(fx):
        c = elf.read(fx.addr, fx.size)
        return [struct.unpack_from('<I', c, 4 * i)[0] for i in range(len(c) // 4)]
    reach = {funcs[a].name for a in base['memo']}
    bf = base['base']
    # M7: the base thread gains a direct call to a deep function
    deep = frame_of(funcs, 'fopen')
    bw = words_of(bf)
    for i, w in enumerate(bw):
        t = ((bf.addr + 4 * i + 4) & 0xf0000000) | ((w & 0x03ffffff) << 2)
        if w >> 26 == 0x03 and t in funcs and funcs[t].syscall and deep:
            mut = os.path.join(tmp, 'm7.elf')
            _patch(elf_path, mut, elf, bf.addr + 4 * i, (0x03 << 26) | ((deep.addr >> 2) & 0x03ffffff))
            r = run(mut, su_dir, resolve_path=resolve_path)
            want = bf.frame + analyse(funcs, ['fopen'], bf.name, base['resolve'], base['prune'])[0]['fopen'][0]
            chk(r['total'] >= want and r['total'] >= base['total'],
                'M7 %s calls fopen directly: total %d -> %d (>= %d)' % (bf.name, base['total'], r['total'], want))
            break
    # M8: a reviewed (resolved) caller gains a new indirect call site
    m8 = None
    for name in base['resolve']:
        if name in reach and resolve_path:
            fx = frame_of(funcs, name)
            if fx is None:
                continue
            for i, w in enumerate(words_of(fx)):
                if w >> 26 == 0x03:
                    m8 = (fx, i)
                    break
        if m8:
            break
    if m8:
        fx, i = m8
        mut = os.path.join(tmp, 'm8.elf')
        _patch(elf_path, mut, elf, fx.addr + 4 * i, (25 << 21) | (31 << 11) | 0x09)
        r = run(mut, su_dir, resolve_path=resolve_path)
        chk(r['verdict'] != 'PASS' and any(x[0] == fx.name for x in r['issues']['resolve_stale']),
            'M8 new jalr in reviewed caller %s: RESOLVE_STALE, verdict %s' % (fx.name, r['verdict']))
    else:
        print('        M8 skipped: no reachable reviewed caller has a jal to turn into jalr in this flavour')
    # M9: a function with a proven jump table gains an indirect TAIL call (jr t9)
    sw = [frame_of(funcs, n) for n in base['switches'] if n not in base['resolve']]
    sw = [x for x in sw if x]
    if sw:
        fx = sw[0]
        for i, w in enumerate(words_of(fx)):
            if w >> 26 == 0x03:
                mut = os.path.join(tmp, 'm9.elf')
                _patch(elf_path, mut, elf, fx.addr + 4 * i, (25 << 21) | 0x08)
                r = run(mut, su_dir, resolve_path=resolve_path)
                chk(any(n == fx.name for n, _ in r['issues']['jr']) and r['verdict'] != 'PASS',
                    'M9 jr t9 added to switch function %s: flagged JR, verdict %s' % (fx.name, r['verdict']))
                break
    else:
        print('        M9 skipped: no unreviewed switch function is reachable in this flavour')
    # M10: a resolved callee is renamed (clone suffix) in the symbol table
    m10 = None
    for name, cal in base['resolve'].items():
        if name in reach:
            for a in cal:
                if sum(1 for g in funcs.values() if g.name == funcs[a].name) == 1:
                    m10 = (name, funcs[a].name)
                    break
        if m10:
            break
    if m10:
        d = bytearray(open(elf_path, 'rb').read())
        st = elf.by_name['.symtab']
        strs = elf.sections[st['link']]
        key = m10[1].encode() + bytes(1)
        for k in range(st['size'] // 16):
            o = strs['offset'] + struct.unpack_from('<I', d, st['offset'] + 16 * k)[0]
            if bytes(d[o:o + len(key)]) == key:
                d[o + len(key) - 2] ^= 0x01
        mut = os.path.join(tmp, 'm10.elf')
        open(mut, 'wb').write(bytes(d))
        r = run(mut, su_dir, resolve_path=resolve_path)
        chk(r['verdict'] != 'PASS' and any(x[0] == m10[0] for x in r['issues']['resolve_stale']),
            'M10 resolved callee %s of %s renamed: RESOLVE_STALE, verdict %s' % (m10[1], m10[0], r['verdict']))
    # M11: the base registers a callback that is not a root
    ex = frame_of(funcs, 'exit_cb')
    done = False
    for i, w in enumerate(bw):
        if ex and w >> 26 in (0x09, 0x0d) and (w & 0xffff) == (ex.addr & 0xffff):
            for a in sorted(funcs):
                if a != ex.addr and (a + 0x8000) >> 16 == (ex.addr + 0x8000) >> 16 and not funcs[a].syscall                         and (w >> 26 == 0x09 or a >> 16 == ex.addr >> 16):
                    mut = os.path.join(tmp, 'm11.elf')
                    _patch(elf_path, mut, elf, bf.addr + 4 * i, (w & 0xffff0000) | (a & 0xffff))
                    r = run(mut, su_dir, resolve_path=resolve_path)
                    chk(funcs[a].name in r['issues']['cb_unlisted'] and r['verdict'] != 'PASS',
                        'M11 %s registers %s instead of exit_cb: CB_UNLISTED, verdict %s'
                        % (bf.name, funcs[a].name, r['verdict']))
                    done = True
                    break
        if done:
            break
    # M12: a tracked constant overwritten by an untracked op is not reused
    for label, mid in (('addiu v0,a0,16', (0x09 << 26) | (4 << 21) | (2 << 16) | 16),
                       ('seb v0,a0', (0x1f << 26) | (4 << 16) | (2 << 11) | (0x10 << 6) | 0x20),
                       ('lwl v0,0(a0)', (0x22 << 26) | (4 << 21) | (2 << 16))):
        words[:] = [(0x0f << 26) | (2 << 16) | 1, mid, (SP << 21) | (2 << 16) | (SP << 11) | 0x23]
        fk = Func('fk', 0x1000, 12, None, 1)
        scan(_E(), {0x1000: fk})
        chk(fk.dynamic and fk.frame_pro == 0, 'M12 lui v0,1; %s; subu sp,sp,v0 -> DYNAMIC (not 65536)' % label)
    # M13: a second, different decrement is summed, not maxed
    words[:] = [(0x09 << 26) | (SP << 21) | (SP << 16) | ((-416) & 0xffff), 0,
                (0x09 << 26) | (SP << 21) | (SP << 16) | ((-8) & 0xffff)]
    fk = Func('fk', 0x1000, 12, None, 1)
    scan(_E(), {0x1000: fk})
    chk(fk.frame_pro == 424 and fk.multi_dec, 'M13 addiu sp,-416 ... addiu sp,-8 -> frame %d (424)' % fk.frame_pro)
    import shutil
    shutil.rmtree(tmp, ignore_errors=True)
    print('selftest: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('elf')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--su')
    ap.add_argument('--roots', default='power_cb,exit_cb')
    ap.add_argument('--base', default='cb_thread')
    ap.add_argument('--limit', type=int, default=4000)
    ap.add_argument('--budget', type=int, default=3200)
    ap.add_argument('--resolve')
    ap.add_argument('--json')
    ap.add_argument('--tree', type=int, default=15)
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest(a.elf, a.su, a.resolve)
    r = run(a.elf, a.su, a.roots.split(','), a.base, a.limit, a.budget, a.resolve)
    report(r, a.tree)
    if a.json:
        json.dump(to_json(r), open(a.json, 'w'), indent=1)
    return {'PASS': 0, 'FAIL': 1, 'INCOMPLETE': 3}[r['verdict']]


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
