#!/usr/bin/env python3
"""sl_dwline.py -- DWARF 2-4 .debug_line row decoder, vendored unchanged in logic
from the restructure survey's x4_dwline.py (not in the repo; the x4 basis that gave
"220 lines / 115 sets" for main's loop), reading through elf32.Elf.  Group
"sections-layout"; layout_report.py uses it for main's loop set map BY LINE.
T1b loop_diff.py owns the by-address window with inlined ranges.

rows(elf, want_file_substr) -> [(addr, file, line)] for every CU whose file
table names want_file_substr; (None, None, None) marks an end of sequence."""
import struct


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


