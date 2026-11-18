#!/usr/bin/env python3
"""dwline.py (copied from the restructure survey's x4_dwline.py, not in the repo; unchanged logic) -- DWARF 2/3 .debug_line decoder (the pspdev ELF emits v3).
rows(elf, want_file_substr) -> list of (addr, file, line) for every row of
CUs whose file table names want_file_substr.  CLI:
  x4_dwline.py <elf> <file-substr> <func-lo-hex> <func-hi-hex> [srclo srchi]
prints the bytes attributed to each source-line bucket of [lo,hi)."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from elf32 import Elf


def uleb(d, o):
    r = s = 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if b < 0x80: return r, o


def sleb(d, o):
    r = s = 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if b < 0x80:
            if b & 0x40: r -= 1 << s
            return r, o


def cstr(d, o):
    e = d.index(b'\0', o)
    return d[o:e].decode('latin-1'), e + 1


def rows(elf, want):
    sec = elf.by_name['.debug_line']
    d = elf.data
    o = sec['offset']; end = o + sec['size']
    out = []
    while o < end:
        ulen = struct.unpack_from('<I', d, o)[0]; uend = o + 4 + ulen
        ver = struct.unpack_from('<H', d, o + 4)[0]
        hlen = struct.unpack_from('<I', d, o + 6)[0]
        p = o + 10
        min_inst = d[p]; p += 1
        if ver >= 4: p += 1
        def_stmt = d[p]; p += 1
        line_base = struct.unpack_from('<b', d, p)[0]; p += 1
        line_range = d[p]; p += 1
        op_base = d[p]; p += 1
        std_len = list(d[p:p + op_base - 1]); p += op_base - 1
        dirs = ['']
        while d[p]:
            s, p = cstr(d, p); dirs.append(s)
        p += 1
        files = ['']
        while d[p]:
            s, p = cstr(d, p); _, p = uleb(d, p); _, p = uleb(d, p); _, p = uleb(d, p)
            files.append(s)
        p += 1
        prog = o + 10 + hlen
        if not any(want in f for f in files):
            o = uend; continue
        addr = 0; f = 1; line = 1
        p = prog
        while p < uend:
            op = d[p]; p += 1
            if op >= op_base:
                adj = op - op_base
                addr += (adj // line_range) * min_inst
                line += line_base + adj % line_range
                out.append((addr, files[f] if f < len(files) else '?', line))
            elif op == 0:
                n, p = uleb(d, p); sub = d[p]; q = p + 1
                if sub == 1:
                    addr = 0; f = 1; line = 1
                    out.append((None, None, None))     # end of sequence
                elif sub == 2:
                    addr = struct.unpack_from('<I', d, q)[0]
                elif sub == 3:
                    s, q2 = cstr(d, q); files.append(s)
                p += n
            elif op == 1:
                out.append((addr, files[f] if f < len(files) else '?', line))
            elif op == 2:
                v, p = uleb(d, p); addr += v * min_inst
            elif op == 3:
                v, p = sleb(d, p); line += v
            elif op == 4:
                f, p = uleb(d, p)
            elif op == 5:
                _, p = uleb(d, p)
            elif op in (6, 7, 10, 11):
                pass
            elif op == 8:
                addr += ((255 - op_base) // line_range) * min_inst
            elif op == 9:
                addr += struct.unpack_from('<H', d, p)[0]; p += 2
            elif op == 12:
                _, p = uleb(d, p)
            else:
                for _ in range(std_len[op - 1]): _, p = uleb(d, p)
        o = uend
    return out


def attribute(rws, lo, hi):
    """bytes per (file,line) inside [lo,hi): each row owns until the next row."""
    seq = [r for r in rws]
    acc = {}
    for i in range(len(seq) - 1):
        a, f, l = seq[i]
        b = seq[i + 1][0]
        if a is None or b is None: continue
        s, e = max(a, lo), min(b, hi)
        if e > s:
            acc[(f, l)] = acc.get((f, l), 0) + e - s
    return acc


if __name__ == '__main__':
    e = Elf(sys.argv[1])
    rws = rows(e, sys.argv[2])
    lo, hi = int(sys.argv[3], 16), int(sys.argv[4], 16)
    acc = attribute(rws, lo, hi)
    tot = sum(acc.values())
    print('attributed bytes', tot, 'of', hi - lo)
    byfile = {}
    for (f, l), n in acc.items(): byfile[f] = byfile.get(f, 0) + n
    for f, n in sorted(byfile.items(), key=lambda x: -x[1])[:12]: print('  file', f, n)
    if len(sys.argv) > 6:
        buckets = [int(x) for x in sys.argv[5:]]
        res = [0] * (len(buckets) + 1)
        for (f, l), n in acc.items():
            if not f.endswith(sys.argv[2]): continue
            k = 0
            while k < len(buckets) and l >= buckets[k]: k += 1
            res[k] += n
        for k, n in enumerate(res):
            print('  bucket <%s: %d' % (buckets[k] if k < len(buckets) else 'end', n))
