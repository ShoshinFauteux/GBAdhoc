#!/usr/bin/env python3
"""elf32.py (from mainpsp-map scratch/x4_elf.py) -- minimal ELF32-LE reader (no pyelftools on this box): sections,
symbols with their STT_FILE owner (locals only; globals have none), bytes."""
import struct


class Elf:
    def __init__(self, path):
        self.path = path
        self.data = d = open(path, 'rb').read()
        assert d[:4] == b'\x7fELF' and d[4] == 1 and d[5] == 1, 'need ELF32 LE'
        (self.e_type, self.e_machine, _, self.e_entry, self.e_phoff, self.e_shoff,
         self.e_flags, _, self.e_phentsize, self.e_phnum, self.e_shentsize,
         self.e_shnum, self.e_shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', d, 16)
        self.sections = []
        for i in range(self.e_shnum):
            f = struct.unpack_from('<IIIIIIIIII', d, self.e_shoff + i * self.e_shentsize)
            self.sections.append(dict(idx=i, name_off=f[0], type=f[1], flags=f[2], addr=f[3],
                                      offset=f[4], size=f[5], link=f[6], info=f[7],
                                      align=f[8], entsize=f[9]))
        shs = self.sections[self.e_shstrndx]
        for s in self.sections:
            s['name'] = self._str(shs['offset'], s['name_off'])
        self.by_name = {s['name']: s for s in self.sections}
        self.phdrs = []
        for i in range(self.e_phnum):
            f = struct.unpack_from('<IIIIIIII', d, self.e_phoff + i * self.e_phentsize)
            self.phdrs.append(dict(type=f[0], offset=f[1], vaddr=f[2], paddr=f[3],
                                   filesz=f[4], memsz=f[5], flags=f[6], align=f[7]))
        self.symbols = []
        st = self.by_name.get('.symtab')
        if st:
            strs = self.sections[st['link']]
            cur_file = None
            for k in range(st['size'] // 16):
                nm, val, size, info, other, shndx = struct.unpack_from('<IIIBBH', d, st['offset'] + 16 * k)
                name = self._str(strs['offset'], nm)
                typ, bind = info & 0xf, info >> 4
                if typ == 4:          # STT_FILE
                    cur_file = name; continue
                sec = self.sections[shndx]['name'] if 0 < shndx < len(self.sections) else None
                self.symbols.append(dict(name=name, value=val, size=size, type=typ, bind=bind,
                                         shndx=shndx, section=sec,
                                         file=cur_file if bind == 0 else None))

    def _str(self, base, off):
        e = self.data.index(b'\0', base + off)
        return self.data[base + off:e].decode('latin-1')

    def section_of(self, addr):
        for s in self.sections:
            if s['flags'] & 2 and s['addr'] <= addr < s['addr'] + s['size']:
                return s
        return None

    def read(self, addr, size):
        s = self.section_of(addr)
        if s is None or s['type'] == 8:      # NOBITS
            return None
        o = s['offset'] + addr - s['addr']
        return self.data[o:o + size]

    def funcs(self):
        return [s for s in self.symbols if s['type'] == 2 and s['size'] > 0]

    def objects(self):
        return [s for s in self.symbols if s['type'] == 1]
