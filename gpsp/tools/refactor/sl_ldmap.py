#!/usr/bin/env python3
"""sl_ldmap.py -- GNU ld link-map reader (the -Wl,-Map= file flavours_sidecar.sh
writes next to every flavour's ELF).  Shared by sect_cmp.py (T2), hotord_check.py
(T5) and layout_report.py (T10); group "sections-layout".

    m = LdMap(path)
    m.inputs      [Input(out, name, addr, size, file, pattern)]  in map order
    m.outputs     {output section name: (addr, size)}
    m.symbols     [(addr, name, Input)]   symbols the map prints (globals only)
    m.patterns    {pattern text: [Input, ...]}   the script line each input matched

`pattern` is the linker-script line that placed the input section, as ld prints
it (` *(EXCLUDE_FILE(*gbalink_core_b.o) .text.update_gba)`, stripped).  With
--section-ordering-file those are hot.ord's own lines, so a hot.ord entry's
matches can be counted directly.  `file` is the object as ld names it:
`main_psp.o`, `../gpsp_libretro_psp1.a(mips_stub.o)`, `/tmp/ccXXXXXX.ltrans0...`
(LTO temporaries: random names, never key on them).

Only the part after "Linker script and memory map" is read (the archive-member
and discarded-section lists come before it)."""
import re
from collections import namedtuple

Input = namedtuple('Input', 'out name addr size file pattern')

_HEX = r'0x[0-9a-fA-F]+'
_OUT = re.compile(r'^(\.[^\s]+|[A-Za-z_][^\s]*)(?:\s+(%s)\s+(%s))?\s*$' % (_HEX, _HEX))
_IN_FULL = re.compile(r'^ (\.[^\s]+|COMMON)\s+(%s)\s+(%s)\s+(\S.*?)\s*$' % (_HEX, _HEX))
_IN_NAME = re.compile(r'^ (\.[^\s]+|COMMON)\s*$')
_CONT = re.compile(r'^\s+(%s)\s+(%s)\s+(\S.*?)\s*$' % (_HEX, _HEX))
_SYM = re.compile(r'^\s+(%s)\s+([A-Za-z_.$][^\s]*)\s*$' % _HEX)
_FILL = re.compile(r'^ \*fill\*')


class LdMap:
    def __init__(self, path):
        self.path = path
        self.inputs = []
        self.outputs = {}
        self.symbols = []
        self.patterns = {}
        self._parse(open(path, errors='replace').read().splitlines())

    def _parse(self, lines):
        try:
            start = next(i for i, l in enumerate(lines) if l.startswith('Linker script and memory map'))
        except StopIteration:
            raise ValueError('%s: not a GNU ld map (no "Linker script and memory map")' % self.path)
        out = None
        pending_out = None
        pending_in = None
        pattern = None
        last_in = None
        for l in lines[start + 1:]:
            if not l.strip():
                continue
            if pending_in is not None:
                m = _CONT.match(l)
                pending, pending_in = pending_in, None
                if m:
                    last_in = self._add(out, pending, int(m.group(1), 16), int(m.group(2), 16),
                                        m.group(3), pattern)
                    continue
            if pending_out is not None:
                m = re.match(r'^\s+(%s)\s+(%s)' % (_HEX, _HEX), l)
                name, pending_out = pending_out, None
                if m:
                    self.outputs.setdefault(name, (int(m.group(1), 16), int(m.group(2), 16)))
                    continue
            if l[0] not in ' \t':
                m = _OUT.match(l)
                if m and m.group(1).startswith('.'):
                    out = m.group(1)
                    pattern = None
                    if m.group(2):
                        self.outputs.setdefault(out, (int(m.group(2), 16), int(m.group(3), 16)))
                    else:
                        pending_out = out
                continue
            if _FILL.match(l):
                continue
            if l.startswith(' *'):
                pattern = l.strip()
                self.patterns.setdefault(pattern, [])
                continue
            m = _IN_FULL.match(l)
            if m:
                last_in = self._add(out, m.group(1), int(m.group(2), 16), int(m.group(3), 16),
                                    m.group(4), pattern)
                continue
            m = _IN_NAME.match(l)
            if m:
                pending_in = m.group(1)
                continue
            m = _SYM.match(l)
            if m and last_in is not None:
                self.symbols.append((int(m.group(1), 16), m.group(2), last_in))

    def _add(self, out, name, addr, size, file, pattern):
        i = Input(out, name, addr, size, file.strip(), pattern)
        self.inputs.append(i)
        if pattern is not None:
            self.patterns.setdefault(pattern, []).append(i)
        return i

    # ---- helpers
    def inputs_of(self, obj_re, out=None):
        r = re.compile(obj_re)
        return [i for i in self.inputs if r.search(i.file) and (out is None or i.out == out)]

    def input_at(self, addr, out=None):
        """The non-empty input section holding addr (first match)."""
        for i in self.inputs:
            if i.size and i.addr <= addr < i.addr + i.size and (out is None or i.out == out):
                return i
        return None

    def objects(self):
        return sorted({i.file for i in self.inputs})


def member(file):
    """'../gpsp_libretro_psp1.a(mips_stub.o)' -> ('gpsp_libretro_psp1.a', 'mips_stub.o');
    'main_psp.o' -> ('', 'main_psp.o')."""
    m = re.match(r'^(.*?)([^/\\]+\.a)\(([^)]+)\)$', file)
    if m:
        return m.group(2), m.group(3)
    return '', re.split(r'[/\\]', file)[-1]


if __name__ == '__main__':
    import sys
    m = LdMap(sys.argv[1])
    print('%d output sections, %d input sections, %d patterns, %d map symbols'
          % (len(m.outputs), len(m.inputs), len(m.patterns), len(m.symbols)))
