#!/usr/bin/env python3
"""ef_dwarf.py -- minimal DWARF 2-5 reader for the phase-T ELF tools
(func_diff.py T1, loop_diff.py T1b, inline_census.py T13).

No pyelftools on this box.  Reads only what the tools need:

  * .debug_line programs, versions 2-5 (the pspdev ELF has 396 v3 tables and
    4 v5 ones; scratch/x4_dwline.py only decoded v2-4 headers):
        line_rows(elf, want)  -> [(addr, file, line) | (None, None, None)]
        attribute(rows, lo, hi) -> {(file, line): bytes} inside [lo, hi)
  * .debug_info, versions 2-5 (this ELF: all 400 CUs are v5), with
    .debug_abbrev, .debug_str, .debug_line_str, .debug_rnglists (v5) and
    .debug_ranges (v2-4):
        Info(elf).cu_named('main_psp.c') -> CU
        CU.dies, DIE.tag / .attrs / .parent / .children
        CU.inlined()   -> one record per DW_TAG_inlined_subroutine
        CU.subprograms() -> one record per DW_TAG_subprogram

Forms that need .debug_addr / .debug_str_offsets (strx*, addrx*, rnglistx)
raise NotImplementedError: GCC does not emit them without -gsplit-dwarf, and a
silent wrong answer is worse than a crash here.
"""
import struct

# tags
TAG_CU, TAG_SUBPROGRAM, TAG_INLINED, TAG_LEXBLOCK = 0x11, 0x2e, 0x1d, 0x0b
# attributes
AT_name, AT_low_pc, AT_high_pc, AT_stmt_list = 0x03, 0x11, 0x12, 0x10
AT_inline, AT_abstract_origin, AT_specification = 0x20, 0x31, 0x47
AT_decl_file, AT_decl_line, AT_external, AT_declaration = 0x3a, 0x3b, 0x3f, 0x3c
AT_ranges, AT_call_file, AT_call_line, AT_entry_pc = 0x55, 0x58, 0x59, 0x52
AT_linkage_name, AT_mips_linkage_name = 0x6e, 0x2007
AT_comp_dir = 0x1b


def uleb(d, o):
    r = s = 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if b < 0x80:
            return r, o


def sleb(d, o):
    r = s = 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if b < 0x80:
            if b & 0x40:
                r -= 1 << s
            return r, o


def cstr(d, o):
    e = d.index(b'\0', o)
    return d[o:e].decode('latin-1'), e + 1


def _secstr(elf, name, off):
    s = elf.by_name.get(name)
    if s is None:
        raise ValueError('no %s section' % name)
    return cstr(elf.data, s['offset'] + off)[0]


# ---------------------------------------------------------------- line tables

def _v5_entries(elf, d, p, addr_size=4):
    """DWARF 5 directory/file entry list -> (list of dict(content->value), p)."""
    nfmt = d[p]; p += 1
    fmt = []
    for _ in range(nfmt):
        ct, p = uleb(d, p); fm, p = uleb(d, p); fmt.append((ct, fm))
    cnt, p = uleb(d, p)
    out = []
    for _ in range(cnt):
        ent = {}
        for ct, fm in fmt:
            v, p = _form_value(elf, d, p, fm, addr_size, None, 5)
            ent[ct] = v
        out.append(ent)
    return out, p


def line_header(elf, off):
    """Parse the line-program header at .debug_line+off.
    Returns dict(version, files, dirs, prog, end, min_inst, line_base,
    line_range, op_base, std_len, def_stmt).  files[] is indexed the way the
    program's DW_LNS_set_file operands are (1-based for v2-4, 0-based for v5)."""
    sec = elf.by_name['.debug_line']
    d = elf.data
    o = sec['offset'] + off
    ulen = struct.unpack_from('<I', d, o)[0]
    if ulen == 0xffffffff:
        raise NotImplementedError('64-bit DWARF')
    uend = o + 4 + ulen
    ver = struct.unpack_from('<H', d, o + 4)[0]
    p = o + 6
    if ver >= 5:
        p += 2                                 # address_size, seg_sel_size
    hlen = struct.unpack_from('<I', d, p)[0]; p += 4
    prog = p + hlen
    min_inst = d[p]; p += 1
    if ver >= 4:
        p += 1                                 # max_ops_per_inst
    def_stmt = d[p]; p += 1
    line_base = struct.unpack_from('<b', d, p)[0]; p += 1
    line_range = d[p]; p += 1
    op_base = d[p]; p += 1
    std_len = list(d[p:p + op_base - 1]); p += op_base - 1
    if ver >= 5:
        dents, p = _v5_entries(elf, d, p)
        dirs = [e.get(1, '') for e in dents]            # DW_LNCT_path
        fents, p = _v5_entries(elf, d, p)
        files = []
        for e in fents:
            nm = e.get(1, '')
            di = e.get(2, 0)                           # DW_LNCT_directory_index
            files.append(nm if nm.startswith('/') or not dirs or di >= len(dirs)
                         else (dirs[di].rstrip('/') + '/' + nm if di else nm))
    else:
        dirs = ['']
        while d[p]:
            s, p = cstr(d, p); dirs.append(s)
        p += 1
        files = ['']
        while d[p]:
            s, p = cstr(d, p); _, p = uleb(d, p); _, p = uleb(d, p); _, p = uleb(d, p)
            files.append(s)
        p += 1
    return dict(version=ver, files=files, dirs=dirs, prog=prog, end=uend,
                min_inst=min_inst, line_base=line_base, line_range=line_range,
                op_base=op_base, std_len=std_len, def_stmt=def_stmt, start=o)


def _run_program(d, h):
    out = []
    files = list(h['files'])
    f0 = 1 if h['version'] < 5 else 1   # DWARF5: register file starts at 1 too
    addr = 0; f = f0; line = 1
    p = h['prog']; uend = h['end']
    op_base, line_range, line_base, min_inst = h['op_base'], h['line_range'], h['line_base'], h['min_inst']

    def fname(k):
        return files[k] if 0 <= k < len(files) else '?'
    while p < uend:
        op = d[p]; p += 1
        if op >= op_base:
            adj = op - op_base
            addr += (adj // line_range) * min_inst
            line += line_base + adj % line_range
            out.append((addr, fname(f), line))
        elif op == 0:
            n, p = uleb(d, p); sub = d[p]; q = p + 1
            if sub == 1:
                out.append((addr, None, None))      # end_sequence row (its address ends the last row)
                out.append((None, None, None))
                addr = 0; f = f0; line = 1
            elif sub == 2:
                addr = struct.unpack_from('<I', d, q)[0]
            elif sub == 3:
                s, _ = cstr(d, q); files.append(s)
            p += n
        elif op == 1:
            out.append((addr, fname(f), line))
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
            for _ in range(h['std_len'][op - 1]):
                _, p = uleb(d, p)
    return out


def line_rows(elf, want):
    """Every row of every line program whose file table names `want`
    (substring).  A (None, None, None) row separates sequences; the row just
    before it is the sequence's end address (file None)."""
    sec = elf.by_name['.debug_line']
    d = elf.data
    o = sec['offset']; end = o + sec['size']
    out = []
    while o < end:
        h = line_header(elf, o - sec['offset'])
        if any(want in f for f in h['files']):
            out.extend(_run_program(d, h))
        o = h['end']
    return out


def attribute(rows, lo, hi):
    """bytes per (file, line) inside [lo, hi): each row owns until the next."""
    acc = {}
    for i in range(len(rows) - 1):
        a, f, l = rows[i]
        b = rows[i + 1][0]
        if a is None or b is None or f is None:
            continue
        s, e = max(a, lo), min(b, hi)
        if e > s:
            acc[(f, l)] = acc.get((f, l), 0) + e - s
    return acc


def row_spans(rows):
    """[(start, end, file, line)] for every row with a successor in sequence."""
    out = []
    for i in range(len(rows) - 1):
        a, f, l = rows[i]
        b = rows[i + 1][0]
        if a is None or b is None or f is None or b <= a:
            continue
        out.append((a, b, f, l))
    return out


# ---------------------------------------------------------------- .debug_info

def _form_value(elf, d, p, form, asz, cu, ver, implicit=None):
    """Decode one attribute value; returns (value, new_p).  References
    (ref1/2/4/8/udata) come back as ('ref', absolute .debug_info offset)."""
    if form == 0x01: return struct.unpack_from('<I', d, p)[0] if asz == 4 else struct.unpack_from('<Q', d, p)[0], p + asz
    if form == 0x0b: return d[p], p + 1
    if form == 0x05: return struct.unpack_from('<H', d, p)[0], p + 2
    if form == 0x06: return struct.unpack_from('<I', d, p)[0], p + 4
    if form == 0x07: return struct.unpack_from('<Q', d, p)[0], p + 8
    if form == 0x1e: return d[p:p + 16], p + 16
    if form == 0x0d: return sleb(d, p)
    if form == 0x0f: return uleb(d, p)
    if form == 0x08: return cstr(d, p)
    if form == 0x0e: return _secstr(elf, '.debug_str', struct.unpack_from('<I', d, p)[0]), p + 4
    if form == 0x1f: return _secstr(elf, '.debug_line_str', struct.unpack_from('<I', d, p)[0]), p + 4
    if form == 0x0c: return d[p], p + 1
    if form == 0x19: return 1, p
    if form == 0x17: return ('sec', struct.unpack_from('<I', d, p)[0]), p + 4
    if form == 0x0a: n = d[p]; return d[p + 1:p + 1 + n], p + 1 + n
    if form == 0x03: n = struct.unpack_from('<H', d, p)[0]; return d[p + 2:p + 2 + n], p + 2 + n
    if form == 0x04: n = struct.unpack_from('<I', d, p)[0]; return d[p + 4:p + 4 + n], p + 4 + n
    if form in (0x09, 0x18):
        n, q = uleb(d, p); return d[q:q + n], q + n
    base = cu.offset if cu is not None else 0
    if form == 0x11: return ('ref', base + d[p]), p + 1
    if form == 0x12: return ('ref', base + struct.unpack_from('<H', d, p)[0]), p + 2
    if form == 0x13: return ('ref', base + struct.unpack_from('<I', d, p)[0]), p + 4
    if form == 0x14: return ('ref', base + struct.unpack_from('<Q', d, p)[0]), p + 8
    if form == 0x15:
        v, q = uleb(d, p); return ('ref', base + v), q
    if form == 0x10:
        sz = 4 if ver >= 3 else asz
        v = struct.unpack_from('<I', d, p)[0]
        return ('refaddr', v), p + sz
    if form == 0x20: return d[p:p + 8], p + 8
    if form == 0x21: return implicit, p
    if form == 0x16:
        f2, q = uleb(d, p); return _form_value(elf, d, q, f2, asz, cu, ver)
    if form in (0x1c, 0x1d): return None, p + 4
    if form == 0x24: return None, p + 8
    if form == 0x22:
        return uleb(d, p)      # loclistx: we never read locations
    raise NotImplementedError('DWARF form 0x%x (needs .debug_addr/.debug_str_offsets)' % form)


class DIE:
    __slots__ = ('offset', 'tag', 'attrs', 'parent', 'children', 'cu')

    def __init__(self, offset, tag, attrs, parent, cu):
        self.offset, self.tag, self.attrs, self.parent, self.cu = offset, tag, attrs, parent, cu
        self.children = []

    def get(self, at, default=None):
        return self.attrs.get(at, default)


class CU:
    def __init__(self, info, offset, ver, asz, abbrev_off, die_off, end):
        self.info, self.offset, self.version, self.addr_size = info, offset, ver, asz
        self.abbrev_off, self.die_off, self.end = abbrev_off, die_off, end
        self._dies = None
        self._top = None
        self._files = None

    # the CU DIE alone (cheap): name, low_pc, stmt_list
    def top(self):
        if self._top is None:
            self._top = self.info._parse(self, only_first=True)[0]
        return self._top

    @property
    def name(self):
        return self.top().get(AT_name, '')

    @property
    def dies(self):
        if self._dies is None:
            self._dies = self.info._parse(self)
            self.by_off = {x.offset: x for x in self._dies}
        return self._dies

    def files(self):
        """decl_file / call_file index -> name, from this CU's line table."""
        if self._files is None:
            sl = self.top().get(AT_stmt_list)
            if isinstance(sl, tuple): sl = sl[1]
            self._files = line_header(self.info.elf, sl)['files'] if sl is not None else []
        return self._files

    def file(self, k):
        fs = self.files()
        return fs[k] if k is not None and 0 <= k < len(fs) else None

    def deref(self, v):
        if isinstance(v, tuple) and v[0] in ('ref', 'refaddr'):
            self.dies
            x = self.by_off.get(v[1])
            if x is None:
                x = self.info.die_at(v[1])
            return x
        return None

    # -- names and code ranges -------------------------------------------
    def origin(self, die):
        """follow abstract_origin / specification to the DIE with the name."""
        seen = 0
        x = die
        while x is not None and seen < 8:
            if AT_name in x.attrs:
                return x
            nxt = x.get(AT_abstract_origin) or x.get(AT_specification)
            x = x.cu.deref(nxt) if nxt is not None else None
            seen += 1
        return die

    def name_of(self, die):
        o = self.origin(die)
        return o.get(AT_name)

    def ranges(self, die):
        """[(lo, hi)] of a DIE's code (low/high_pc or DW_AT_ranges)."""
        lo = die.get(AT_low_pc)
        hi = die.get(AT_high_pc)
        if lo is not None and hi is not None:
            # high_pc is an offset when its form was a constant class.  The
            # parser marks address-class values by leaving them > lo.
            if die.attrs.get('_hi_is_off', False):
                hi = lo + hi
            return [(lo, hi)] if hi > lo else []
        r = die.get(AT_ranges)
        if r is None:
            return []
        off = r[1] if isinstance(r, tuple) else r
        base = self.top().get(AT_low_pc, 0) or 0
        return self.info.read_ranges(off, base, self.version)

    def inlined(self):
        """One record per DW_TAG_inlined_subroutine in this CU."""
        out = []
        for x in self.dies:
            if x.tag != TAG_INLINED:
                continue
            org = self.origin(x)
            # the enclosing concrete function (out-of-line code) and the
            # immediate inline parent (an inlined_subroutine, if nested)
            p = x.parent; inl_parent = None; depth = 0
            while p is not None and p.tag != TAG_SUBPROGRAM:
                if p.tag == TAG_INLINED:
                    depth += 1
                    if inl_parent is None: inl_parent = p
                p = p.parent
            out.append(dict(
                die=x, name=org.get(AT_name), decl_line=org.get(AT_decl_line),
                decl_file=org.cu.file(org.get(AT_decl_file)),
                call_line=x.get(AT_call_line), call_file=self.file(x.get(AT_call_file)),
                ranges=self.ranges(x), container=p,
                container_name=self.name_of(p) if p is not None else None,
                container_ranges=self.ranges(p) if p is not None else [],
                inline_parent=inl_parent,
                inline_parent_name=self.name_of(inl_parent) if inl_parent is not None else None,
                depth=depth))
        return out

    def subprograms(self):
        out = []
        for x in self.dies:
            if x.tag != TAG_SUBPROGRAM:
                continue
            org = self.origin(x)
            out.append(dict(die=x, name=org.get(AT_name), decl_line=org.get(AT_decl_line),
                            decl_file=org.cu.file(org.get(AT_decl_file)),
                            inline=org.get(AT_inline), external=bool(org.get(AT_external)),
                            declaration=bool(x.get(AT_declaration)),
                            abstract=x.get(AT_inline) is not None and not self.ranges(x),
                            concrete=bool(self.ranges(x)), ranges=self.ranges(x),
                            linkage=x.get(AT_linkage_name) or org.get(AT_linkage_name)))
        return out


class Info:
    def __init__(self, elf):
        self.elf = elf
        sec = elf.by_name.get('.debug_info')
        if sec is None:
            raise ValueError('%s has no .debug_info (built without -g?)' % elf.path)
        self.sec = sec
        self.abbrev = elf.by_name['.debug_abbrev']
        self._abbr_cache = {}
        self.cus = []
        d = elf.data
        o = sec['offset']; end = o + sec['size']
        while o < end:
            ulen = struct.unpack_from('<I', d, o)[0]
            if ulen == 0xffffffff:
                raise NotImplementedError('64-bit DWARF')
            ver = struct.unpack_from('<H', d, o + 4)[0]
            if ver >= 5:
                utype, asz = d[o + 6], d[o + 7]
                aoff = struct.unpack_from('<I', d, o + 8)[0]
                hdr = 12
                if utype in (2, 6): hdr += 12            # type units / split: skip sig+off
                elif utype in (4, 5): hdr += 8
            else:
                aoff = struct.unpack_from('<I', d, o + 6)[0]
                asz = d[o + 10]; hdr = 11
            self.cus.append(CU(self, o, ver, asz, aoff, o + hdr, o + 4 + ulen))
            o += 4 + ulen

    def cu_named(self, suffix):
        hits = [c for c in self.cus if c.name == suffix or c.name.endswith('/' + suffix)]
        if not hits:
            hits = [c for c in self.cus if c.name.endswith(suffix)]
        if len(hits) != 1:
            raise LookupError('%d CUs named %r' % (len(hits), suffix))
        return hits[0]

    def die_at(self, off):
        for c in self.cus:
            if c.offset <= off < c.end:
                c.dies
                return c.by_off.get(off)
        return None

    def _abbrevs(self, aoff):
        if aoff in self._abbr_cache:
            return self._abbr_cache[aoff]
        d = self.elf.data
        p = self.abbrev['offset'] + aoff
        tab = {}
        while True:
            code, p = uleb(d, p)
            if code == 0:
                break
            tag, p = uleb(d, p)
            has_kids = d[p]; p += 1
            specs = []
            while True:
                at, p = uleb(d, p); fm, p = uleb(d, p)
                imp = None
                if fm == 0x21:
                    imp, p = sleb(d, p)
                if at == 0 and fm == 0:
                    break
                specs.append((at, fm, imp))
            tab[code] = (tag, has_kids, specs)
        self._abbr_cache[aoff] = tab
        return tab

    def _parse(self, cu, only_first=False):
        d = self.elf.data
        tab = self._abbrevs(cu.abbrev_off)
        p = cu.die_off
        out = []
        stack = [None]
        while p < cu.end:
            off = p
            code, p = uleb(d, p)
            if code == 0:
                if len(stack) > 1:
                    stack.pop()
                continue
            tag, kids, specs = tab[code]
            attrs = {}
            for at, fm, imp in specs:
                v, p = _form_value(self.elf, d, p, fm, cu.addr_size, cu, cu.version, imp)
                attrs[at] = v
                if at == AT_high_pc and fm not in (0x01, 0x1b, 0x29, 0x2a, 0x2b, 0x2c):
                    attrs['_hi_is_off'] = True
            die = DIE(off, tag, attrs, stack[-1], cu)
            if stack[-1] is not None:
                stack[-1].children.append(die)
            out.append(die)
            if only_first:
                return out
            if kids:
                stack.append(die)
        return out

    def read_ranges(self, off, base, ver):
        d = self.elf.data
        out = []
        if ver >= 5:
            sec = self.elf.by_name['.debug_rnglists']
            p = sec['offset'] + off
            while True:
                k = d[p]; p += 1
                if k == 0:           # end_of_list
                    break
                if k == 4:           # offset_pair
                    a, p = uleb(d, p); b, p = uleb(d, p); out.append((base + a, base + b))
                elif k == 5:         # base_address
                    base = struct.unpack_from('<I', d, p)[0]; p += 4
                elif k == 6:         # start_end
                    a, b = struct.unpack_from('<II', d, p); p += 8; out.append((a, b))
                elif k == 7:         # start_length
                    a = struct.unpack_from('<I', d, p)[0]; p += 4
                    n, p = uleb(d, p); out.append((a, a + n))
                else:
                    raise NotImplementedError('DW_RLE 0x%x needs .debug_addr' % k)
        else:
            sec = self.elf.by_name['.debug_ranges']
            p = sec['offset'] + off
            while True:
                a, b = struct.unpack_from('<II', d, p); p += 8
                if a == 0 and b == 0:
                    break
                if a == 0xffffffff:
                    base = b; continue
                out.append((base + a, base + b))
        return [(a, b) for a, b in out if b > a]
