#!/usr/bin/env python3
"""det_check.py -- section-level identity of two builds (the core of G1).

    det_check.py A B            A, B = gpsp_adhoc.elf files or flavours.sh flavour dirs
    det_check.py --runs R1 R2   every flavour in either run dir (discovered; one-sided = FAIL)
    det_check.py --selftest ELF mutants on a real ELF (no build needed)

Compares EVERY SHF_ALLOC section by name, address, size, type, flags and bytes;
SHT_NOBITS by name, address and size only.  Nothing loadable is masked.  The
program headers (PT_LOAD layout) and e_entry are compared too.  Non-alloc
sections (DWARF, .comment, .symtab) are only reported, never a failure.  For
flavour dirs it also compares EBOOT.PBP, the .map and the .su set, and reports
which differ (informational: the ELF verdict is the gate).

This is the quick determinism check of stage 1.  T2 sect_cmp.py is the gate
tool proper; this one must agree with it on identical/different.

Exit: 0 identical, 1 a loadable difference, 2 usage.
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf32 import Elf  # noqa: E402

SHF_ALLOC = 2
SHT_NOBITS = 8


def sec_bytes(e, s):
    return e.data[s['offset']:s['offset'] + s['size']]


def alloc_sections(e):
    return [s for s in e.sections if s['flags'] & SHF_ALLOC]


def compare_elf(pa, pb, out=print, data_a=None, data_b=None):
    """Return the number of loadable differences; print a report."""
    a = Elf(pa) if data_a is None else _elf_from(pa, data_a)
    b = Elf(pb) if data_b is None else _elf_from(pb, data_b)
    diffs = 0
    if a.e_entry != b.e_entry:
        out('  DIFF e_entry %#x != %#x' % (a.e_entry, b.e_entry)); diffs += 1
    pha = [(p['type'], p['vaddr'], p['filesz'], p['memsz'], p['flags']) for p in a.phdrs]
    phb = [(p['type'], p['vaddr'], p['filesz'], p['memsz'], p['flags']) for p in b.phdrs]
    if pha != phb:
        out('  DIFF program headers: %s != %s' % (pha, phb)); diffs += 1
    sa = {s['name']: s for s in alloc_sections(a)}
    sb = {s['name']: s for s in alloc_sections(b)}
    names = [s['name'] for s in alloc_sections(a)] + \
            [n for n in (s['name'] for s in alloc_sections(b)) if n not in sa]
    for n in names:
        x, y = sa.get(n), sb.get(n)
        if x is None or y is None:
            out('  DIFF %-26s only in %s' % (n, 'B' if x is None else 'A')); diffs += 1; continue
        hdr_a = (x['type'], x['flags'], x['addr'], x['size'])
        hdr_b = (y['type'], y['flags'], y['addr'], y['size'])
        if hdr_a != hdr_b:
            out('  DIFF %-26s header type/flags/addr/size %s != %s' % (n, hdr_a, hdr_b)); diffs += 1
            continue
        if x['type'] == SHT_NOBITS:
            continue
        ba, bb = sec_bytes(a, x), sec_bytes(b, y)
        if ba != bb:
            first = next(i for i in range(len(ba)) if ba[i] != bb[i])
            nd = sum(1 for i in range(len(ba)) if ba[i] != bb[i])
            out('  DIFF %-26s %d byte(s) differ, first at %#x (+%#x)' % (n, nd, x['addr'] + first, first))
            diffs += 1
    nonalloc = []
    na = {s['name']: s for s in a.sections if not s['flags'] & SHF_ALLOC and s['name']}
    nb = {s['name']: s for s in b.sections if not s['flags'] & SHF_ALLOC and s['name']}
    for n in sorted(set(na) | set(nb)):
        if n not in na or n not in nb or sec_bytes(a, na[n]) != sec_bytes(b, nb[n]):
            nonalloc.append(n)
    out('  %d SHF_ALLOC sections (%d NOBITS) compared: %s' % (
        len(sa), sum(1 for s in sa.values() if s['type'] == SHT_NOBITS),
        'IDENTICAL' if diffs == 0 else '%d DIFFERENT' % diffs))
    out('  non-alloc sections differing (informational): %s' % (', '.join(nonalloc) or 'none'))
    return diffs


def _elf_from(path, data):
    """An Elf over in-memory bytes (for the mutants)."""
    e = Elf.__new__(Elf)
    tmp = path + '.detcheck.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    try:
        e.__init__(tmp)
    finally:
        os.remove(tmp)
    return e


def sha(p):
    if not os.path.isfile(p):
        return None
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def map_norm(p):
    import re
    return re.sub(rb'/tmp/cc[A-Za-z0-9]{6}\.', b'/tmp/ccXXXXXX.', open(p, 'rb').read())


def su_digest(d):
    root = os.path.join(d, 'su')
    if not os.path.isdir(root):
        return None, 0
    h = hashlib.sha256(); n = 0
    for dp, _, fs in sorted(os.walk(root)):
        for f in sorted(fs):
            p = os.path.join(dp, f)
            h.update(os.path.relpath(p, root).replace('\\', '/').encode())
            h.update(open(p, 'rb').read()); n += 1
    return h.hexdigest(), n


def compare_dirs(da, db, label):
    print('[%s] %s  vs  %s' % (label, da, db))
    ea, eb = os.path.join(da, 'gpsp_adhoc.elf'), os.path.join(db, 'gpsp_adhoc.elf')
    d = compare_elf(ea, eb)
    print('  elf sha256      %s %s' % (sha(ea)[:16], 'same' if sha(ea) == sha(eb) else 'DIFFERENT ' + sha(eb)[:16]))
    for f in ('EBOOT.PBP', 'gpsp_adhoc.map', 'gbadhoc_me.prx', 'gbadhoc_home.prx', 'gbadhoc_online.prx'):
        x, y = sha(os.path.join(da, f)), sha(os.path.join(db, f))
        if x is None and y is None:
            continue
        if f == 'gpsp_adhoc.map' and x and y and x != y:
            # LTO names its ltrans temporaries /tmp/ccXXXXXX.ltransN.ltrans.o at
            # random; that is the only difference seen between two clean builds
            na_, nb_ = map_norm(os.path.join(da, f)), map_norm(os.path.join(db, f))
            print('  %-15s %s' % (f, 'same modulo /tmp/cc*.ltrans names' if na_ == nb_ else
                                  'DIFFERENT (also after normalising /tmp/cc* names)'))
            continue
        print('  %-15s %s' % (f, 'same' if x == y else 'DIFFERENT (A %s, B %s)' % (x and x[:12], y and y[:12])))
    (xa, na), (xb, nb) = su_digest(da), su_digest(db)
    if na or nb:
        print('  .su files       %d / %d, %s' % (na, nb, 'same' if xa == xb else 'DIFFERENT'))
    return d


def selftest(elf_path):
    """Self-test on a real ELF: identity, then mutants that must be caught."""
    data = open(elf_path, 'rb').read()
    e = Elf(elf_path)
    quiet = lambda *_: None
    fails = 0

    def expect(name, got_diff, want_diff):
        nonlocal fails
        ok = (got_diff > 0) == want_diff
        print('  %-55s %s' % (name, 'ok' if ok else 'FAILED'))
        fails += not ok

    expect('identical: ELF vs itself', compare_elf(elf_path, elf_path, quiet), False)
    alloc = alloc_sections(e)
    print('  (%d SHF_ALLOC sections in %s)' % (len(alloc), os.path.basename(elf_path)))

    def flip(secname, at=0):
        s = e.by_name[secname]
        b = bytearray(data); b[s['offset'] + at] ^= 0x01
        return bytes(b)

    def setfield(secname, field_index, value):
        s = e.by_name[secname]
        b = bytearray(data)
        struct.pack_into('<I', b, e.e_shoff + s['idx'] * e.e_shentsize + 4 * field_index, value)
        return bytes(b)

    for name in ('.sceStub.text', '.rodata.sceModuleInfo', '.text', '.lib.stub'):
        if name in e.by_name:
            expect('mutant: one byte flipped in %s' % name,
                   compare_elf(elf_path, elf_path, quiet, data, flip(name)), True)
        else:
            print('  (no %s section in this ELF)' % name)
    s_last = [s for s in alloc if s['type'] != SHT_NOBITS][-1]['name']
    expect('mutant: last byte of the last PROGBITS (%s)' % s_last,
           compare_elf(elf_path, elf_path, quiet, data, flip(s_last, e.by_name[s_last]['size'] - 1)), True)
    nob = [s for s in alloc if s['type'] == SHT_NOBITS]
    if nob:
        n = nob[0]
        expect('mutant: NOBITS %s size +4' % n['name'],
               compare_elf(elf_path, elf_path, quiet, data, setfield(n['name'], 5, n['size'] + 4)), True)
        expect('mutant: NOBITS %s address +8' % n['name'],
               compare_elf(elf_path, elf_path, quiet, data, setfield(n['name'], 3, n['addr'] + 8)), True)
    na = [s for s in e.sections if not s['flags'] & SHF_ALLOC and s['size'] and s['type'] != SHT_NOBITS
          and s['name'].startswith(('.comment', '.debug'))]
    if na:
        expect('non-mutant: byte flipped in %s (non-alloc, ignored)' % na[0]['name'],
               compare_elf(elf_path, elf_path, quiet, data, flip(na[0]['name'])), False)
    print('selftest: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


def main(argv):
    if len(argv) == 2 and argv[0] == '--selftest':
        return selftest(argv[1])
    if len(argv) == 3 and argv[0] == '--runs':
        r1, r2 = argv[1], argv[2]
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import runs
        fl, one = runs.pair(r1, r2)      # discovered: release-stable and any other flavour too
        if not fl:
            print('no flavour in both run dirs'); return 2
        for f in one:
            print('[%s] FAIL: gpsp_adhoc.elf in only one of the run dirs' % f)
        bad = len(one) + sum(1 for f in fl if compare_dirs(os.path.join(r1, f), os.path.join(r2, f), f))
        print('determinism: %s' % ('IDENTICAL in %s' % ', '.join(fl) if not bad else '%d flavour(s) DIFFER' % bad))
        return 1 if bad else 0
    if len(argv) == 2:
        a, b = argv
        if os.path.isdir(a) and os.path.isdir(b):
            return 1 if compare_dirs(a, b, os.path.basename(b.rstrip('/\\'))) else 0
        return 1 if compare_elf(a, b) else 0
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
