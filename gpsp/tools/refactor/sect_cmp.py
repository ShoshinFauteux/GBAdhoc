#!/usr/bin/env python3
"""sect_cmp.py -- T2, gate G1 (gpsp/docs/design/RESTRUCTURE.md §0.1, §5).

    sect_cmp.py A B [--expect-sections N] [--json OUT]   A, B: ELF files or flavours.sh flavour dirs
    sect_cmp.py --runs R1 R2 [--expect-sections N]       every flavour present in both run dirs
    sect_cmp.py --list ELF|DIR                           the SHF_ALLOC sections G1 compares
    sect_cmp.py --selftest ELF|DIR                       seeded mutants on a real ELF (no build)

WHAT IT COMPARES (G1: "nothing loadable is masked").  For the two ELFs:
  * every SHF_ALLOC section, matched by name (and occurrence, for a duplicated
    name): type, flags, address, size, alignment, entsize, and every byte;
    SHT_NOBITS by name, type, flags, address, size and alignment (no bytes);
  * the ORDER of the SHF_ALLOC sections (by address), so a swap is a difference
    even if every section kept its own bytes;
  * e_entry and every program header (type, offset, vaddr, paddr, filesz,
    memsz, flags, align);
  * the file bytes of every PT_LOAD segment, so a byte that is loaded but lies
    in no SHF_ALLOC section (inter-section fill inside a segment) is compared too.
Only non-alloc sections (DWARF, .comment, .symtab, .strtab, .shstrtab,
.reginfo/.pdr when non-alloc) are ignored; they are listed as "informational".

THE BUILD LOG.  When A or B is a flavour dir (or --log FILE is given), its
build.log and logs/*.log are searched for psp-fixup-imports' "could not fixup
imports" warning (gpsp/docs/DEBUGGING.md): make exits 0 on it, the EBOOT
does not start.  Found -> exit 3, whatever the sections say.

ATTRIBUTION.  Each differing byte range is named by the function/object that
holds it (ELF symtab of A, then B), and by the input section and object when
the dir has gpsp_adhoc.map, so a G1 failure says WHERE, not only that.

Exit: 0 identical, 1 a loadable difference, 2 usage, 3 broken imports in a log,
4 --expect-sections not met.
"""
import glob
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf32 import Elf  # noqa: E402

SHF_ALLOC = 2
SHT_NOBITS = 8
PT_LOAD = 1
FIXUP_WARNING = 'could not fixup imports'
FLAVOURS = ('release', 'lab', 'harness')


# --------------------------------------------------------------------------- inputs

def resolve(p):
    """(elf path, map path or None, [log paths]) for an ELF or a flavour dir."""
    if os.path.isdir(p):
        elf = os.path.join(p, 'gpsp_adhoc.elf')
        if not os.path.isfile(elf):
            print('sect_cmp: no gpsp_adhoc.elf in %s' % p, file=sys.stderr)
            raise SystemExit(2)   # a missing input is a tool error, never "a difference" (1)
        mp = os.path.join(p, 'gpsp_adhoc.map')
        # logs/.build-*.log (the make output, where psp-fixup-imports prints) are
        # dotfiles: glob's '*' skips them, so list the dir instead
        ld = os.path.join(p, 'logs')
        logs = [x for x in [os.path.join(p, 'build.log')] +
                [os.path.join(ld, n) for n in (sorted(os.listdir(ld)) if os.path.isdir(ld) else [])]
                if os.path.isfile(x)]
        return elf, (mp if os.path.isfile(mp) else None), logs
    return p, None, []


def elf_from_bytes(data, label='<mem>'):
    """An Elf over in-memory bytes (mutants); elf32.Elf reads a path, so go via a temp file."""
    import tempfile
    fd, tmp = tempfile.mkstemp(suffix='.elf')
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
        e = Elf(tmp)
    finally:
        os.remove(tmp)
    e.path = label
    return e


def alloc_sections(e):
    return [s for s in e.sections if s['flags'] & SHF_ALLOC]


def keyed(e):
    """[(key, section)] with key = name, or name#k for the k-th duplicate."""
    seen = {}
    out = []
    for s in alloc_sections(e):
        k = seen.get(s['name'], 0)
        seen[s['name']] = k + 1
        out.append((s['name'] if k == 0 else '%s#%d' % (s['name'], k), s))
    return out


def scan_logs(logs):
    hits = []
    for p in logs:
        try:
            for n, l in enumerate(open(p, errors='replace'), 1):
                if FIXUP_WARNING in l:
                    hits.append('%s:%d: %s' % (p, n, l.strip()))
        except OSError:
            pass
    return hits


# --------------------------------------------------------------------------- attribution

class Namer:
    def __init__(self, elf, map_path=None):
        self.alloc = {s["name"] for s in elf.sections if s["flags"] & SHF_ALLOC}
        self.syms = sorted((s['value'], s['size'], s['name']) for s in elf.symbols
                           if s['type'] in (1, 2) and s['size'] > 0 and s['section'])
        self.map = None
        if map_path:
            try:
                import sl_ldmap
                self.map = sl_ldmap.LdMap(map_path)
            except Exception as ex:  # a map is a convenience, never a reason to fail G1
                print('  (map %s unreadable: %s)' % (map_path, ex))

    def name(self, addr):
        import bisect
        i = bisect.bisect_right(self.syms, (addr, 1 << 40, '')) - 1
        txt = None
        for j in range(i, max(-1, i - 64), -1):
            a, sz, n = self.syms[j]
            if a <= addr < a + sz:
                txt = '%s+%#x' % (n, addr - a)
                break
        if self.map is not None:
            inp = next((i for i in self.map.inputs if i.size and i.out in self.alloc
                        and i.addr <= addr < i.addr + i.size), None)
            if inp is not None:
                where = '%s %s' % (inp.name, inp.file.split('/')[-1])
                txt = '%s [%s]' % (txt, where) if txt else '[%s]' % where
        return txt or '?'


def ranges(a, b, base, limit=8):
    """Coalesced differing ranges [(addr, len)] (first `limit`), and the total count of
    differing bytes.  Compares 4 KiB chunks in C first, so a near-identical 3 MB
    section costs milliseconds."""
    out = []
    total = 0
    n = min(len(a), len(b))
    CH = 4096
    run_start = run_end = None
    for c in range(0, n, CH):
        if a[c:c + CH] == b[c:c + CH]:
            continue
        for k in range(c, min(c + CH, n)):
            if a[k] != b[k]:
                total += 1
                if run_end is not None and k <= run_end + 1:
                    run_end = k
                    continue
                if run_start is not None and len(out) < limit:
                    out.append((base + run_start, run_end - run_start + 1))
                run_start = run_end = k
    if run_start is not None and len(out) < limit:
        out.append((base + run_start, run_end - run_start + 1))
    return out, total


# --------------------------------------------------------------------------- compare

def compare(ea, eb, out=print, map_a=None, map_b=None, namer=True):
    """Return (n_differences, result dict).  ea, eb: Elf objects."""
    res = {'a': ea.path, 'b': eb.path, 'diffs': [], 'sections': 0, 'nobits': 0}
    diffs = res['diffs']

    def diff(kind, what, detail):
        diffs.append({'kind': kind, 'what': what, 'detail': detail})
        out('  DIFF %-9s %-24s %s' % (kind, what, detail))

    na_ = nb_ = None
    if ea.e_entry != eb.e_entry:
        diff('header', 'e_entry', '%#x != %#x' % (ea.e_entry, eb.e_entry))
    pf = ('type', 'offset', 'vaddr', 'paddr', 'filesz', 'memsz', 'flags', 'align')
    pha = [tuple(p[k] for k in pf) for p in ea.phdrs]
    phb = [tuple(p[k] for k in pf) for p in eb.phdrs]
    if len(pha) != len(phb):
        diff('phdr', 'count', '%d != %d' % (len(pha), len(phb)))
    for i, (x, y) in enumerate(zip(pha, phb)):
        if x != y:
            diff('phdr', 'phdr[%d]' % i, ' '.join('%s %#x!=%#x' % (k, u, v) for k, u, v in zip(pf, x, y) if u != v))

    ka, kb = keyed(ea), keyed(eb)
    da, db = dict(ka), dict(kb)
    res['sections'] = len(ka)
    res['nobits'] = sum(1 for _, s in ka if s['type'] == SHT_NOBITS)
    order_a = [k for k, s in sorted(ka, key=lambda t: (t[1]['addr'], t[1]['idx']))]
    order_b = [k for k, s in sorted(kb, key=lambda t: (t[1]['addr'], t[1]['idx']))]
    if order_a != order_b and set(order_a) == set(order_b):
        diff('order', 'SHF_ALLOC', 'address order %s != %s' % (order_a, order_b))
    for k in [k for k, _ in ka] + [k for k, _ in kb if k not in da]:
        x, y = da.get(k), db.get(k)
        if x is None or y is None:
            diff('presence', k, 'SHF_ALLOC only in %s' % ('B' if x is None else 'A'))
            continue
        hf = ('type', 'flags', 'addr', 'size', 'align', 'entsize')
        hx, hy = tuple(x[f] for f in hf), tuple(y[f] for f in hf)
        if hx != hy:
            diff('header', k, ' '.join('%s %#x!=%#x' % (f, u, v) for f, u, v in zip(hf, hx, hy) if u != v))
            if x['type'] == SHT_NOBITS or y['type'] == SHT_NOBITS or x['addr'] != y['addr']:
                continue
        if x['type'] == SHT_NOBITS:
            continue
        ba = ea.data[x['offset']:x['offset'] + x['size']]
        bb = eb.data[y['offset']:y['offset'] + y['size']]
        if ba != bb:
            rs, total = ranges(ba, bb, x['addr'])
            if namer and na_ is None:
                na_ = Namer(ea, map_a)
            det = '%d byte(s) differ in %d+ range(s)' % (total, len(rs))
            if len(ba) != len(bb):
                det += ' (and %d bytes of length)' % abs(len(ba) - len(bb))
            diff('bytes', k, det)
            for a, n in rs:
                out('            %#010x +%-5d %s' % (a, n, na_.name(a) if na_ else ''))

    # PT_LOAD file bytes: catches loaded bytes outside every SHF_ALLOC section
    loads_a = [p for p in ea.phdrs if p['type'] == PT_LOAD]
    loads_b = [p for p in eb.phdrs if p['type'] == PT_LOAD]
    for i, (p, q) in enumerate(zip(loads_a, loads_b)):
        sa = ea.data[p['offset']:p['offset'] + p['filesz']]
        sb = eb.data[q['offset']:q['offset'] + q['filesz']]
        if sa == sb:
            continue
        covered = [(s['offset'], s['offset'] + s['size']) for s in alloc_sections(ea) if s['type'] != SHT_NOBITS]
        rs, total = ranges(sa, sb, p['vaddr'], limit=10**6)
        outside = []
        for a, n in rs:
            for k in range(n):
                o = p['offset'] + (a - p['vaddr']) + k
                if not any(lo <= o < hi for lo, hi in covered):
                    outside.append((a + k, 1))
                    break
        if outside:
            diff('segment', 'PT_LOAD[%d]' % i, '%d differing range(s) outside every SHF_ALLOC section, first at %#x'
                 % (len(outside), outside[0][0]))
        elif not any(d_['kind'] in ('bytes', 'header', 'presence') for d_ in diffs):
            # loaded bytes differ, yet no section said so (B's section headers point
            # elsewhere than its segment): the segment is what the loader reads
            diff('segment', 'PT_LOAD[%d]' % i, '%d byte(s) differ in the loaded image, first at %#x, though every '
                 'SHF_ALLOC section compares equal' % (total, rs[0][0]))
    nonalloc = []
    xa = {s['name']: s for s in ea.sections if not s['flags'] & SHF_ALLOC and s['name']}
    xb = {s['name']: s for s in eb.sections if not s['flags'] & SHF_ALLOC and s['name']}
    for n in sorted(set(xa) | set(xb)):
        if n not in xa or n not in xb or \
                ea.data[xa[n]['offset']:xa[n]['offset'] + xa[n]['size']] != \
                eb.data[xb[n]['offset']:xb[n]['offset'] + xb[n]['size']]:
            nonalloc.append(n)
    res['nonalloc_differing'] = nonalloc
    out('  %d SHF_ALLOC sections (%d NOBITS) + %d PT_LOAD segments compared: %s'
        % (res['sections'], res['nobits'], len(loads_a),
           'IDENTICAL' if not diffs else '%d DIFFERENCE(S)' % len(diffs)))
    out('  non-alloc sections differing (ignored by G1): %s' % (', '.join(nonalloc) or 'none'))
    return len(diffs), res


def list_sections(elf):
    e = Elf(elf)
    al = alloc_sections(e)
    print('%-24s %-13s %-10s %-10s %10s %5s' % ('section', 'type', 'flags', 'addr', 'size', 'align'))
    for s in sorted(al, key=lambda s: (s['addr'], s['idx'])):
        tn = {1: 'PROGBITS', SHT_NOBITS: 'NOBITS', 0x7000002a: 'MIPS_ABIFLAGS'}.get(s['type'], '%#x' % s['type'])
        print('%-24s %-13s %#-10x %#010x %10d %5d' % (s['name'], tn, s['flags'], s['addr'], s['size'], s['align']))
    print('%d SHF_ALLOC sections: %d with file bytes, %d NOBITS'
          % (len(al), sum(1 for s in al if s['type'] != SHT_NOBITS), sum(1 for s in al if s['type'] == SHT_NOBITS)))
    return len(al)


def run_pair(a, b, label, expect=None):
    ea_p, ma, la = resolve(a)
    eb_p, mb, lb = resolve(b)
    print('[%s] A=%s\n%s B=%s' % (label, ea_p, ' ' * (len(label) + 2), eb_p))
    hits = scan_logs(la + lb)
    for h in hits:
        print('  BROKEN IMPORTS: %s' % h)
    n, res = compare(Elf(ea_p), Elf(eb_p), map_a=ma, map_b=mb)
    res['broken_imports'] = hits
    rc = 3 if hits else (1 if n else 0)
    if expect is not None and res['sections'] != expect:
        print('  EXPECTED %d SHF_ALLOC sections, found %d' % (expect, res['sections']))
        rc = rc or 4
    print('  G1 %s' % {0: 'ok', 1: 'FAIL (loadable difference)', 3: 'FAIL (could not fixup imports)',
                       4: 'FAIL (section count)'}[rc])
    res['rc'] = rc
    return rc, res


# --------------------------------------------------------------------------- selftest

def selftest(target):
    """Mutants on a real ELF: every one must be caught, every control must pass.
    Also checks that det_check.py (stage 1's quick tool) agrees on every case."""
    elf, mp, _ = resolve(target)
    data = open(elf, 'rb').read()
    base = Elf(elf)
    quiet = lambda *_: None
    fails = [0]
    try:
        import det_check
    except Exception:
        det_check = None

    def check(label, mutated, want_diff, kinds=None):
        eb = elf_from_bytes(mutated, 'mutant')
        n, res = compare(base, eb, out=quiet, namer=False)
        got = n > 0
        ok = got == want_diff
        if ok and kinds:
            ok = any(d['kind'] in kinds for d in res['diffs'])
        agree = ''
        if det_check is not None:
            dd = det_check.compare_elf(elf, elf, quiet, data, mutated)
            # det_check does not read segment fill or section order/alignment:
            # it may only MISS what sect_cmp catches, never the reverse.
            if (dd > 0) and not got:
                ok = False
                agree = ' (det_check found a difference sect_cmp missed!)'
            elif (dd > 0) != got:
                agree = ' (det_check: %s)' % ('diff' if dd else 'no diff')
        print('  %-66s %s%s' % (label, 'ok' if ok else 'FAILED', agree))
        fails[0] += not ok

    def shdr_set(b, s, field, value):
        struct.pack_into('<I', b, base.e_shoff + s['idx'] * base.e_shentsize + 4 * field, value)

    check('control: ELF vs itself', data, False)
    al = alloc_sections(base)
    print('  (%d SHF_ALLOC sections, %d NOBITS, in %s)' % (len(al), sum(s['type'] == SHT_NOBITS for s in al), elf))
    # 1. a one-byte flip at the first, middle and last byte of EVERY PROGBITS SHF_ALLOC section
    for s in al:
        if s['type'] == SHT_NOBITS or not s['size']:
            continue
        for where in sorted({0, s['size'] // 2, s['size'] - 1}):
            b = bytearray(data)
            b[s['offset'] + where] ^= 0x01
            check('mutant: byte %d of %s flipped' % (where, s['name']), bytes(b), True, {'bytes'})
    # the two PLAN names explicitly
    for nm in ('.sceStub.text', '.rodata.sceModuleInfo'):
        if nm not in base.by_name:
            print('  FAILED: %s missing from the ELF' % nm)
            fails[0] += 1
    # 2. NOBITS: size, address, alignment
    for s in al:
        if s['type'] != SHT_NOBITS:
            continue
        for field, val, what in ((5, s['size'] + 4, 'size +4'), (3, s['addr'] + 8, 'address +8'),
                                 (8, max(1, s['align']) * 2, 'alignment x2')):
            b = bytearray(data)
            shdr_set(b, s, field, val)
            check('mutant: NOBITS %s %s' % (s['name'], what), bytes(b), True)
    # 3. a PROGBITS section moved (address) with the same bytes
    s = base.by_name['.rodata.sceModuleInfo']
    b = bytearray(data)
    shdr_set(b, s, 3, s['addr'] + 4)
    check('mutant: .rodata.sceModuleInfo address +4, same bytes', bytes(b), True, {'header'})
    # 4. SHF_ALLOC dropped from a section: it vanishes from the alloc set
    s = base.by_name['.sceStub.text']
    b = bytearray(data)
    shdr_set(b, s, 2, s['flags'] & ~SHF_ALLOC)
    check('mutant: .sceStub.text loses SHF_ALLOC', bytes(b), True, {'presence'})
    # 5. a section renamed (one shstrtab byte)
    shs = base.sections[base.e_shstrndx]
    b = bytearray(data)
    b[shs['offset'] + base.by_name['.lib.stub']['name_off'] + 1] ^= 0x20     # .lib.stub -> .Lib.stub
    check('mutant: .lib.stub renamed .Lib.stub', bytes(b), True, {'presence'})
    # 6. e_entry, a program header
    b = bytearray(data)
    struct.pack_into('<I', b, 24, base.e_entry + 4)
    check('mutant: e_entry +4', bytes(b), True, {'header'})
    b = bytearray(data)
    struct.pack_into('<I', b, base.e_phoff + 0x14, base.phdrs[0]['memsz'] + 16)
    check('mutant: phdr[0].memsz +16', bytes(b), True, {'phdr'})
    # 7. a loaded byte in no SHF_ALLOC section (fill between sections inside a PT_LOAD)
    gap = None
    cov = sorted((x['offset'], x['offset'] + x['size']) for x in al if x['type'] != SHT_NOBITS)
    for p in (p for p in base.phdrs if p['type'] == PT_LOAD):
        cur = p['offset']
        for lo, hi in cov:
            if hi <= cur or lo >= p['offset'] + p['filesz']:
                continue
            if lo > cur:
                gap = cur
                break
            cur = max(cur, hi)
        if gap is None and cur < p['offset'] + p['filesz']:
            gap = cur
        if gap is not None:
            break
    if gap is not None:
        b = bytearray(data)
        b[gap] ^= 0xff
        check('mutant: loaded fill byte at file offset %#x (in no section)' % gap, bytes(b), True, {'segment'})
    else:
        print('  (no fill byte inside a PT_LOAD outside every section in this ELF: segment mutant n/a)')
    # 7b. a loaded byte changed while the section header is redirected to an
    # unchanged copy of the old bytes: only the segment compare can see it
    s = base.by_name['.lib.ent.top']
    old = data[s['offset']:s['offset'] + s['size']]
    alt = data.find(old, s['offset'] + s['size'])
    if alt > 0:
        b = bytearray(data)
        b[s['offset']] ^= 0x01
        shdr_set(b, s, 4, alt)
        check('mutant: .lib.ent.top byte flipped, sh_offset redirected to its old bytes', bytes(b), True,
              {'segment'})
    # 8. controls: non-alloc changes are ignored
    for nm in ('.comment', '.debug_info', '.debug_line', '.symtab', '.strtab'):
        if nm in base.by_name and base.by_name[nm]['size']:
            b = bytearray(data)
            b[base.by_name[nm]['offset']] ^= 0x01
            check('control: byte flipped in %s (non-alloc, ignored)' % nm, bytes(b), False)
    # 9. the build-log check
    import tempfile
    d = tempfile.mkdtemp()
    try:
        lp = os.path.join(d, 'build.log')
        open(lp, 'w').write('psp-fixup-imports gpsp_adhoc.elf\nError, could not fixup imports, stubs out of order.\n')
        hit = bool(scan_logs([lp]))
        print('  %-66s %s' % ('mutant: "could not fixup imports" in a build log', 'ok' if hit else 'FAILED'))
        fails[0] += not hit
        # where flavours.sh really puts it: the make output, logs/.build-eboot.log (a dotfile)
        fd_ = os.path.join(d, 'flav')
        os.makedirs(os.path.join(fd_, 'logs'))
        open(os.path.join(fd_, 'gpsp_adhoc.elf'), 'wb').write(data[:64])
        open(os.path.join(fd_, 'build.log'), 'w').write('build ok\n')
        open(os.path.join(fd_, 'logs', '.build-eboot.log'), 'w').write(
            'psp-fixup-imports gpsp_adhoc.elf\nError, could not fixup imports, stubs out of order.\n')
        hit = bool(scan_logs(resolve(fd_)[2]))
        print('  %-66s %s' % ("mutant: the warning in a flavour dir's logs/.build-eboot.log", 'ok' if hit else 'FAILED'))
        fails[0] += not hit
        open(lp, 'w').write('psp-fixup-imports gpsp_adhoc.elf\n')
        clean = not scan_logs([lp])
        print('  %-66s %s' % ('control: a clean build log', 'ok' if clean else 'FAILED'))
        fails[0] += not clean
    finally:
        import shutil
        shutil.rmtree(d, ignore_errors=True)
    print('selftest: %s' % ('PASS' if not fails[0] else '%d FAILED' % fails[0]))
    return 1 if fails[0] else 0


# --------------------------------------------------------------------------- main

def main(argv):
    expect = None
    js = None
    args = []
    i = 0
    while i < len(argv):
        if argv[i] == '--expect-sections':
            expect = int(argv[i + 1]); i += 2
        elif argv[i] == '--json':
            js = argv[i + 1]; i += 2
        else:
            args.append(argv[i]); i += 1
    if len(args) == 2 and args[0] == '--selftest':
        return selftest(args[1])
    if len(args) == 2 and args[0] == '--list':
        n = list_sections(resolve(args[1])[0])
        return 4 if expect is not None and n != expect else 0
    results = []
    if len(args) == 3 and args[0] == '--runs':
        import runs                      # flavours discovered, never a fixed list
        fl, one = runs.pair(args[1], args[2])
        if not fl:
            print('no flavour in both run dirs')
            return 2
        rc = 0
        # a flavour built in one run only is not "identical": never skip it silently
        for f in one:
            print('[%s] FAIL: gpsp_adhoc.elf in only one of the run dirs' % f)
            results.append({'flavour': f, 'rc': 1, 'missing_in_one_run': True})
            rc = 1
        for f in fl:
            r, res = run_pair(os.path.join(args[1], f), os.path.join(args[2], f), f, expect)
            res['flavour'] = f
            results.append(res)
            rc = max(rc, r)
        print('G1 over %s: %s' % (', '.join(fl), 'ok' if rc == 0 else 'FAIL'))
    elif len(args) == 2:
        rc, res = run_pair(args[0], args[1], os.path.basename(args[1].rstrip('/\\')) or 'pair', expect)
        results.append(res)
    else:
        print(__doc__)
        return 2
    if js:
        json.dump(results, open(js, 'w'), indent=1)
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
