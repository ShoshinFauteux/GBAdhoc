#!/usr/bin/env python3
"""ef_mips.py -- address-normalising comparison of Allegrex (MIPS32 + VFPU)
code between two linked ELFs.  Shared by func_diff.py (T1) and loop_diff.py
(T1b).  From scratch/x4_func_diff.py (the M prototype, 5,163/140/5,022/1 and
3/3 mutants), with the gaps x4 listed closed:

  * VFPU memory ops lv.s/lv.q/lvl.q/lvr.q/sv.s/sv.q/svl.q/svr.q (opcodes 0x32,
    0x36, 0x35, 0x3a, 0x3e, 0x3d; 14-bit word offset in bits 15..2) are %lo
    users, so a layout shift under them is no longer a false DIFFERENT.
  * every WEAK %hi/%lo pairing (the hi reached the lo through a move or a
    spill, so the pairing is a guess) is returned as a site, and the caller
    can make weak sites fatal (--strict-weak).
  * compare_words() can run past the first difference and return every
    differing word (loop_diff.py needs all of them, by address).

The rule (unchanged from the prototype).  Identical words are never
reinterpreted.  A differing word is accepted only if it is
  j/jal   same opcode, both targets resolve to the same token;
  lui     same opcode and rt, and a later %lo user justifies it;
  %lo use same upper 16 bits (op, rs, rt) and the full address, built from a
          lui on the base register (tier 1) or any lui of the body (WEAK,
          tier 2), resolves to the same token in both ELFs.
Tokens: a sized symbol -> name+off with clone/static number suffixes dropped
(`lat_swap.isra.0` -> `lat_swap.isra`, `home_told.12` -> `home_told`); an
anonymous C string -> its text; another anonymous constant -> its bytes up to
32 B, with code pointers in it resolved (switch tables).

Verify pass (2026-11-05, adversarial single-word mutant sweeps over every
function, builds/mainpsp-tools-out/scratch-elf-func-verify/):
  * tier 1 (lui on the base register itself) accepted ~3.5 % of immediate
    mutants on the 304/144 pair (e.g. `addiu v0,v0,1` -> `+5` in
    render_scanline_objs): an unrelated `lui v0` put both addresses in a
    zero-filled anonymous gap, and `ro:` tokens compared content only.
    Anonymous-constant tokens now carry their anchor (Space.anchor).
  * canonical names collide (hint.14..17@ui_psp.c, pipe_fetch.constprop.0/.1,
    CSWTCH.*): a reference re-pointed to a sibling resolved alike.  A shared
    canonical name now gets @STT_FILE and, within a file, #address-ordinal.
  * a WEAK pairing landing in two different 1-char strings (`")"`) was
    accepted: short strings must also match by anchor (weak_unsafe).
"""
import bisect
import re
import struct

NUMSUFFIX = re.compile(r'(\.\d+)+(?=(\.|$))')
CLONE_KIND = re.compile(r'\.(isra|part|constprop|cold|lto_priv|local)\b')


def canon(name):
    """lat_swap.isra.0 -> lat_swap.isra ; home_told.12 -> home_told"""
    return NUMSUFFIX.sub('', name)


def base_name(name):
    """lat_swap.isra.0 -> lat_swap ; power_request_poll.part.0 -> power_request_poll"""
    return re.sub(r'\..*$', '', name)


LOADSTORE = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E,
             0x30, 0x31, 0x38, 0x39}
VFPU_MEM = {0x32, 0x35, 0x36, 0x3A, 0x3D, 0x3E}      # lv.s lvl/r.q lv.q sv.s svl/r.q sv.q
IMM_USERS = LOADSTORE | VFPU_MEM | {0x09, 0x0D, 0x08}   # + addiu, ori, addi
STORES = {0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x38, 0x39} | VFPU_MEM
# A WEAK pairing (hi from a lui on another register) is a guess, so it may
# only match on a token that names something: anonymous-constant tokens
# (`ro:`, a content hash: zero-filled .rodata makes many addresses look alike)
# and unresolvable ones are not evidence.  Found by the %lo+4 mutant
# (tests/elf-func_test_func_diff.py): the prototype accepted
# audio_ring+0 -> audio_ring+4 through `ro:00000000 x8` on another lui.
WEAK_UNSAFE = ('ro:', 'ABS?')
# Refusing WEAK whenever a same-register lui resolved to a different token
# was tried and REJECTED: it made 11 false DIFFERENTs on the clean 304/144
# pair (the register is reused; the real hi comes through a move).
STRICT_TIER1 = False


def weak_unsafe(SA, SB, fa, fb, tok):
    """True when a token is too common to justify a WEAK guess: anonymous
    constants, unresolvable addresses, and a C string shorter than 4 chars
    unless it is also the same string by place (anchor).  Found by the
    verify sweep: a +64 mutant in gbab_load_gamepak was accepted through an
    unrelated lui landing in two different `")"` strings."""
    if tok.startswith(WEAK_UNSAFE):
        return True
    if tok.startswith('str:') and len(tok) < 4 + 2 + 4:      # str:'abc'
        return SA.anchor(fa) != SB.anchor(fb)
    return False


class Space:
    """Address -> token resolver for one ELF."""

    def __init__(self, elf):
        self.elf = elf
        syms = [s for s in elf.symbols if s['size'] > 0 and s['type'] in (1, 2)
                and s['shndx'] not in (0, 0xfff1)]
        syms.sort(key=lambda s: (s['value'], -s['size']))
        self.syms = syms
        self.addrs = [s['value'] for s in syms]
        # Token names.  The canonical name drops clone/static numbers, so
        # several symbols can share it (hint.14..hint.17@ui_psp.c,
        # pipe_fetch.constprop.0/.1, CSWTCH.*): a load re-pointed from one to
        # another resolved to the same token (verify mutants).  A shared name
        # gets its STT_FILE and, inside one file, its address ordinal.
        groups = {}
        for s in sorted(syms, key=lambda s: s['value']):
            groups.setdefault(canon(s['name']), []).append(s)
        self.tname = {}
        for cn, ss in groups.items():
            if len(ss) == 1:
                self.tname[id(ss[0])] = cn
                continue
            per = {}
            for s in ss:
                per.setdefault(s['file'], []).append(s)
            for f, fs in per.items():
                for k, s in enumerate(fs):
                    self.tname[id(s)] = '%s@%s%s' % (cn, f or '', '#%d' % (k + 1) if len(fs) > 1 else '')
        self.alloc = [s for s in elf.sections if s['flags'] & 2 and s['size'] > 0 and s['addr']]
        self.lo = min(s['addr'] for s in self.alloc)
        self.hi = max(s['addr'] + s['size'] for s in self.alloc)
        self._cache = {}

    def in_range(self, a):
        return self.lo <= a <= self.hi

    def token(self, a):
        t = self._cache.get(a)
        if t is None:
            t = self._cache[a] = self._token(a)
        return t

    def _token(self, a):
        k = bisect.bisect_right(self.addrs, a) - 1
        # a smaller symbol may shadow a bigger one that covers a: walk back
        j = k
        while j >= 0 and a - self.addrs[j] < 0x10000:
            s = self.syms[j]
            if s['value'] <= a < s['value'] + s['size']:
                return '%s+%d' % (self.tname[id(s)], a - s['value'])
            j -= 1
        if k >= 0:
            s = self.syms[k]
            if a == s['value'] + s['size'] and s['type'] == 1:
                return '%s+%d' % (self.tname[id(s)], a - s['value'])
        sec = self.elf.section_of(a)
        if sec is None:
            return 'ABS?%s' % self.near(a)
        if sec['type'] == 8:                       # NOBITS, no symbol
            return '%s+%d' % (sec['name'], a - sec['addr'])
        raw = self.elf.read(a, 64) or b''
        z = raw.find(b'\0')
        if z > 0 and all(32 <= c < 127 or c in (9, 10, 13) for c in raw[:z]):
            return 'str:%r' % raw[:z].decode('latin-1')
        k2 = bisect.bisect_right(self.addrs, a)
        lim = 32
        if k2 < len(self.addrs):
            lim = max(1, min(lim, self.addrs[k2] - a))
        lim = min(lim, sec['addr'] + sec['size'] - a, len(raw))
        parts = []
        for q in range(0, lim - lim % 4, 4):
            v = struct.unpack_from('<I', raw, q)[0]
            s2 = self.elf.section_of(v) if v & 3 == 0 and self.lo <= v <= self.hi else None
            if s2 is not None and s2['flags'] & 4:  # into code
                kk = bisect.bisect_right(self.addrs, v) - 1
                parts.append('&%s+%d' % (self.tname[id(self.syms[kk])], v - self.addrs[kk]))
            else:
                parts.append(raw[q:q + 4].hex())
        if lim % 4:
            parts.append(raw[lim - lim % 4:lim].hex())
        # Anchored: an anonymous constant is also named by its place (the
        # nearest sized symbol before it in its section, or the section
        # start).  Content alone let two addresses in one zero-filled gap
        # share a token, and tier 1 accepted e.g. `addiu v0,v0,1` -> `+5`
        # through an unrelated `lui v0` (verify sweep, docs/elf-func.md).
        return 'ro:@' + self.anchor(a, sec) + ':' + ','.join(parts)

    def anchor(self, a, sec=None):
        sec = sec or self.elf.section_of(a)
        kp = bisect.bisect_right(self.addrs, a) - 1
        if sec is None:
            return hex(a)
        if kp >= 0 and self.addrs[kp] >= sec['addr']:
            return '%s+%d' % (self.tname[id(self.syms[kp])], a - self.addrs[kp])
        return '%s+%d' % (sec['name'], a - sec['addr'])

    def near(self, a):
        k = bisect.bisect_right(self.addrs, a) - 1
        if k < 0:
            return hex(a)
        s = self.syms[k]
        return '%s+%d' % (self.tname[id(s)], a - s['value'])


def _lo_full(op, w, hv):
    imm = w & 0xFFFF
    if op == 0x0D:
        return hv | imm
    if op in VFPU_MEM:
        imm &= 0xFFFC
    return (hv + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF


def compare_words(SA, SB, aa, ab, ca, cb, stop_first=True):
    """Compare two code blobs (aa/ab = their addresses).
    Returns dict(ok, bad=[(word_index, reason)], weak=[word_index],
                 naddr=<address words resolved>).
    With stop_first the scan ends at the first bad word (func_diff); without
    it every word is judged (loop_diff)."""
    res = dict(ok=True, bad=[], weak=[], naddr=0)
    if len(ca) != len(cb):
        res['ok'] = False
        res['bad'].append((min(len(ca), len(cb)) // 4, 'size %d vs %d' % (len(ca), len(cb))))
        if stop_first:
            return res
    n = min(len(ca), len(cb)) // 4
    wa = struct.unpack('<%dI' % n, ca[:4 * n])
    wb = struct.unpack('<%dI' % n, cb[:4 * n])
    luis_a = {}
    for i in range(n):
        if wa[i] >> 26 == 0x0F:
            luis_a.setdefault((wa[i] >> 16) & 31, []).append(i)
    pend = set(); used = set()

    def bad(i, why):
        res['ok'] = False
        res['bad'].append((i, why))
        return stop_first

    for i in range(n):
        x, y = wa[i], wb[i]
        if x == y:
            continue
        op = x >> 26
        if op != y >> 26:
            if bad(i, 'opcode %08x vs %08x' % (x, y)): return res
            continue
        if op in (2, 3):
            ta = SA.token(((aa + 4 * i + 4) & 0xF0000000) | ((x & 0x3FFFFFF) << 2))
            tb = SB.token(((ab + 4 * i + 4) & 0xF0000000) | ((y & 0x3FFFFFF) << 2))
            if ta != tb:
                if bad(i, 'call %s vs %s' % (ta, tb)): return res
                continue
            res['naddr'] += 1
            continue
        if op == 0x0F:
            if (x ^ y) & 0xFFFF0000:
                if bad(i, 'lui reg %08x vs %08x' % (x, y)): return res
                continue
            pend.add(i)
            continue
        if op in IMM_USERS and not ((x ^ y) & 0xFFFF0000) and \
                (op not in VFPU_MEM or not ((x ^ y) & 3)):
            rs = (x >> 21) & 31
            ok = False; weak_mode = False
            cands = sorted(luis_a.get(rs, []), key=lambda k: (k > i, abs(i - k)))
            weak = sorted((k for r, ks in luis_a.items() if r != rs for k in ks),
                          key=lambda k: (k > i, abs(i - k)))
            tier1_refutes = False
            for la in cands + ['WEAK'] + weak:
                if la == 'WEAK':
                    # A lui on the base register itself resolved both sides
                    # in range but to DIFFERENT tokens: that is evidence of
                    # a real change; never let a guess (WEAK) overrule it.
                    if tier1_refutes and STRICT_TIER1:
                        break
                    weak_mode = True
                    continue
                if not ((wb[la] >> 26) == 0x0F and ((wb[la] >> 16) & 31) == ((wa[la] >> 16) & 31)):
                    continue
                fa = _lo_full(op, x, (wa[la] & 0xFFFF) << 16)
                fb = _lo_full(op, y, (wb[la] & 0xFFFF) << 16)
                if not (SA.in_range(fa) and SB.in_range(fb)):
                    continue
                ta, tb = SA.token(fa), SB.token(fb)
                if ta == tb and not (weak_mode and weak_unsafe(SA, SB, fa, fb, ta)):
                    ok = True; used.add(la)
                    break
                if not weak_mode:
                    tier1_refutes = True
            if ok:
                res['naddr'] += 1
                if weak_mode:
                    res['weak'].append(i)
                continue
            if bad(i, 'imm %08x vs %08x' % (x, y)): return res
            continue
        if bad(i, 'word %08x vs %08x' % (x, y)): return res
    left = sorted(pend - used)
    for i in left:
        res['ok'] = False
        res['bad'].append((i, 'lui never justified by a lo'))
        if stop_first:
            break
    res['bad'].sort()
    return res


def compare_data(SA, SB, ra, rb):
    """Data object bytes equal modulo pointer words that resolve alike."""
    if len(ra) != len(rb):
        return False
    for k in range(0, len(ra) - 3, 4):
        x = struct.unpack_from('<I', ra, k)[0]; y = struct.unpack_from('<I', rb, k)[0]
        if x == y:
            continue
        if not (SA.elf.section_of(x) and SB.elf.section_of(y) and SA.token(x) == SB.token(y)):
            return False
    return ra[len(ra) & ~3:] == rb[len(rb) & ~3:]


def icache_set(addr):
    """PSP Allegrex I-cache: 16 KiB, 2-way, 64 B lines -> 128 sets per 8 KiB way."""
    return (addr >> 6) & 127


def anon_gaps(space):
    """Anonymous data of one ELF: every byte range of an allocated, non-code
    PROGBITS section that no sized symbol covers (string literals, switch
    tables, float constants).  -> {anchor: (addr, bytes)}; the anchor is the
    token name of the sized symbol before the gap (or the section start), so
    a uniform layout shift keeps the keys."""
    e = space.elf
    out = {}
    for sec in e.sections:
        if not (sec['flags'] & 2) or sec['flags'] & 4 or sec['type'] != 1 or not sec['size'] or not sec['addr']:
            continue
        lo, hi = sec['addr'], sec['addr'] + sec['size']
        cur, anchor = lo, sec['name']
        syms = [s for s in space.syms if lo <= s['value'] < hi]
        for s in syms + [None]:
            end = s['value'] if s is not None else hi
            if end > cur:
                raw = e.read(cur, end - cur)
                if raw and raw.strip(b'\0'):
                    out['%s+%d' % (anchor, 0) if cur == lo else anchor] = (cur, raw)
            if s is not None and s['value'] + s['size'] > cur:
                cur = s['value'] + s['size']
                anchor = 'after:' + space.tname[id(s)]
    return out


def compare_anon(SA, SB, aa, ra, ab, rb):
    """compare_data for a gap that starts at an arbitrary address: pointer
    words are aligned to the ADDRESS, not to the gap start."""
    if len(ra) != len(rb) or (aa - ab) & 3:
        return ra == rb
    p = (-aa) & 3
    if ra[:p] != rb[:p]:
        return False
    return compare_data(SA, SB, ra[p:], rb[p:])
