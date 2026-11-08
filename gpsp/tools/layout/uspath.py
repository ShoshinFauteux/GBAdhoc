"""uspath.py ELF DIS NM [vcount...] : the PSP CPU's per-line path through
update_scanline in ME capture mode 1 (mini MIPS interpreter over the
disassembly; calls out of the function are recorded and skipped).
Prints the executed instruction addresses (one line per address, with the
number of the lines (vcounts) that executed it)."""
import re, struct, sys, collections

elf_p, dis_p, nm_p = sys.argv[1:4]
vcs = [int(x) for x in sys.argv[4:]] or [50]
data = open(elf_p, 'rb').read()
mem = {}
def load():
    e_phoff, = struct.unpack_from('<I', data, 28)
    e_phentsize, e_phnum = struct.unpack_from('<HH', data, 42)
    for i in range(e_phnum):
        p_type, off, va, pa, fsz, msz, fl, al = struct.unpack_from('<8I', data, e_phoff + i * e_phentsize)
        if p_type == 1:
            segs.append((va, data[off:off + fsz], msz))
segs = []
load()
over = {}
def rb(a):
    if a in over:
        return over[a]
    for va, b, msz in segs:
        if va <= a < va + msz:
            return b[a - va] if a - va < len(b) else 0
    return 0
def wb(a, v):
    over[a] = v & 0xff
def rd(a, n, signed=False):
    v = 0
    for i in range(n):
        v |= rb(a + i) << (8 * i)
    if signed and v >> (8 * n - 1):
        v -= 1 << (8 * n)
    return v & 0xffffffff
def wr(a, n, v):
    for i in range(n):
        wb(a + i, v >> (8 * i))
sym = {}
for l in open(nm_p, errors='replace'):
    p = l.split()
    if len(p) >= 3:
        sym.setdefault(p[-1], int(p[0], 16))
ins = {}
for l in open(dis_p):
    m = re.match(r'^ *([0-9a-f]+):\t(\S+)\s*(.*)$', l)
    if m:
        ins[int(m.group(1), 16)] = (m.group(2), m.group(3).split('<')[0].strip())
fn_lo = sym['update_scanline']; fn_hi = max(ins)
import os
FOLLOW = {sym[n] for n in os.environ.get('FOLLOW', '').split(',') if n in sym}
REG = {n: i for i, n in enumerate('zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp s8 ra'.split())}
REG['fp'] = 30
def S32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v >> 31 else v

def run(vc):
    global over
    over = {}
    SCR, LOG, SHADOW = 0x0b000000, 0x0b100000, 0x0b200000
    wr(sym['me_capture_mode'], 4, 1)
    wr(sym['me_capture_buf'], 4, SCR)
    wr(sym['me_vram_shadow'], 4, SHADOW)
    wr(SCR + 23060, 4, LOG); wr(SCR + 23064, 4, 12288)
    io = sym['io_registers']
    wr(io + 6, 2, vc)           # VCOUNT
    wr(io + 0, 2, 0x1f40)       # DISPCNT: mode 0, BG0-3 + OBJ on
    r = [0] * 32; hi = lo = 0
    r[29] = 0x0bf00000; r[31] = 0xdead0000; r[28] = sym.get('_gp', 0)
    pc = fn_lo; npc = None
    trace = []; calls = []
    steps = 0
    def g(n): return r[REG[n]]
    def s(n, v):
        if REG[n]:
            r[REG[n]] = v & 0xffffffff
    while steps < 20000:
        steps += 1
        if pc == 0xdead0000:
            return trace, calls
        if pc not in ins:
            raise SystemExit('left the function at %x' % pc)
        trace.append(pc)
        op, args = ins[pc]
        a = [x.strip() for x in args.split(',')] if args else []
        nxt = pc + 4
        br = None; likely = False; link = False
        def mem_ea(x):
            m = re.match(r'(-?\d+)\((\w+)\)', x)
            return (g(m.group(2)) + int(m.group(1))) & 0xffffffff
        def imm(x): return int(x, 0)
        if op == 'nop': pass
        elif op == 'lui': s(a[0], imm(a[1]) << 16)
        elif op == 'li': s(a[0], imm(a[1]))
        elif op == 'move': s(a[0], g(a[1]))
        elif op == 'addiu': s(a[0], g(a[1]) + imm(a[2]))
        elif op == 'addu': s(a[0], g(a[1]) + g(a[2]))
        elif op == 'subu': s(a[0], g(a[1]) - g(a[2]))
        elif op == 'and': s(a[0], g(a[1]) & g(a[2]))
        elif op == 'or': s(a[0], g(a[1]) | g(a[2]))
        elif op == 'andi': s(a[0], g(a[1]) & imm(a[2]))
        elif op == 'ori': s(a[0], g(a[1]) | imm(a[2]))
        elif op == 'sll': s(a[0], g(a[1]) << imm(a[2]))
        elif op == 'srl': s(a[0], g(a[1]) >> imm(a[2]))
        elif op == 'sltiu': s(a[0], int(g(a[1]) < (imm(a[2]) & 0xffffffff)))
        elif op == 'sltu': s(a[0], int(g(a[1]) < g(a[2])))
        elif op == 'movn':
            if g(a[2]): s(a[0], g(a[1]))
        elif op == 'ext':
            s(a[0], (g(a[1]) >> imm(a[2])) & ((1 << imm(a[3])) - 1))
        elif op == 'mult':
            p = S32(g(a[0])) * S32(g(a[1])); lo = p & 0xffffffff; hi = (p >> 32) & 0xffffffff
        elif op == 'divu':
            d = g(a[-1]); n = g(a[-2])
            if d: lo, hi = n // d, n % d
        elif op == 'mflo': s(a[0], lo)
        elif op == 'mfhi': s(a[0], hi)
        elif op in ('lw', 'lh', 'lhu', 'lb', 'lbu'):
            n = {'lw': 4, 'lh': 2, 'lhu': 2, 'lb': 1, 'lbu': 1}[op]
            s(a[0], rd(mem_ea(a[1]), n, op in ('lh', 'lb')))
        elif op in ('sw', 'sh', 'sb'):
            n = {'sw': 4, 'sh': 2, 'sb': 1}[op]
            wr(mem_ea(a[1]), n, g(a[0]))
        elif op in ('lwl', 'lwr', 'swl', 'swr'):
            ea = mem_ea(a[1])
            if op == 'lwr':   # with its lwl: an unaligned word, done here whole
                s(a[0], rd(ea, 4))
            elif op == 'swr':
                wr(ea, 4, g(a[0]))
        elif op == 'break': raise SystemExit('break at %x' % pc)
        elif op in ('b',): br = imm('0x' + a[0])
        elif op in ('beqz', 'bnez', 'beqzl', 'bnezl', 'beq', 'bne', 'beql', 'bnel'):
            likely = op.endswith('l') and op not in ('bnel', 'beql') or op in ('beql', 'bnel')
            likely = op in ('beqzl', 'bnezl', 'beql', 'bnel')
            base = op.rstrip('l') if likely else op
            if base in ('beqz', 'bnez'):
                c = (g(a[0]) == 0) == (base == 'beqz'); tgt = a[1]
            else:
                c = (g(a[0]) == g(a[1])) == (base == 'beq'); tgt = a[2]
            if c: br = int(tgt, 16)
            elif likely: nxt = pc + 8; likely = 'skip'
        elif op == 'jal':
            calls.append(a[0]); s('ra', pc + 8)
            br = int(a[0], 16) if int(a[0], 16) in FOLLOW else 'call'
        elif op == 'j':
            t = int(a[0], 16)
            if t in FOLLOW or fn_lo <= t < fn_lo + 0x2000 and t in ins and not t in sym.values():
                br = t
            else:             # a tail call out: recorded, then back to the caller
                calls.append(a[0]); br = g('ra')
        elif op == 'jr':
            br = g(a[0])
        else:
            raise SystemExit('op %s at %x' % (op, pc))
        if likely == 'skip':
            pc = nxt; continue
        if br is not None:
            # delay slot
            dpc = pc + 4
            trace.append(dpc)
            dop = ins[dpc]
            saved = pc
            pc = dpc
            # execute the slot by recursion-free trick: re-run loop body once
            ins_back = ins[dpc]
            op2, args2 = ins_back
            # minimal slot execution through the same dispatcher
            r_before = list(r)
            pc_slot_exec(dpc, r, ins, g, s, rd, wr)
            if br == 'call':
                s('v0', 0)
                pc = saved + 8
            else:
                pc = br
            continue
        pc = nxt
    raise SystemExit('too many steps')

def pc_slot_exec(dpc, r, ins, g, s, rd, wr):
    op, args = ins[dpc]
    a = [x.strip() for x in args.split(',')] if args else []
    def mem_ea(x):
        m = re.match(r'(-?\d+)\((\w+)\)', x)
        return (g(m.group(2)) + int(m.group(1))) & 0xffffffff
    def imm(x): return int(x, 0)
    if op == 'nop': return
    if op == 'lui': s(a[0], imm(a[1]) << 16)
    elif op == 'li': s(a[0], imm(a[1]))
    elif op == 'move': s(a[0], g(a[1]))
    elif op == 'addiu': s(a[0], g(a[1]) + imm(a[2]))
    elif op == 'addu': s(a[0], g(a[1]) + g(a[2]))
    elif op == 'subu': s(a[0], g(a[1]) - g(a[2]))
    elif op == 'and': s(a[0], g(a[1]) & g(a[2]))
    elif op == 'or': s(a[0], g(a[1]) | g(a[2]))
    elif op == 'andi': s(a[0], g(a[1]) & imm(a[2]))
    elif op == 'sll': s(a[0], g(a[1]) << imm(a[2]))
    elif op == 'srl': s(a[0], g(a[1]) >> imm(a[2]))
    elif op == 'sltiu': s(a[0], int(g(a[1]) < (imm(a[2]) & 0xffffffff)))
    elif op == 'sltu': s(a[0], int(g(a[1]) < g(a[2])))
    elif op == 'ext': s(a[0], (g(a[1]) >> imm(a[2])) & ((1 << imm(a[3])) - 1))
    elif op in ('lw', 'lh', 'lhu', 'lb', 'lbu'):
        n = {'lw': 4, 'lh': 2, 'lhu': 2, 'lb': 1, 'lbu': 1}[op]
        s(a[0], rd(mem_ea(a[1]), n, op in ('lh', 'lb')))
    elif op in ('sw', 'sh', 'sb'):
        n = {'sw': 4, 'sh': 2, 'sb': 1}[op]
        wr(mem_ea(a[1]), n, g(a[0]))
    elif op in ('lwr',): s(a[0], rd(mem_ea(a[1]), 4))
    elif op in ('swr',): wr(mem_ea(a[1]), 4, g(a[0]))
    elif op in ('lwl', 'swl'): pass
    else:
        raise SystemExit('slot op %s at %x' % (op, dpc))

agg = collections.Counter()
allcalls = collections.Counter()
for vc in vcs:
    t, c = run(vc)
    for x in set(t):
        agg[x] += 1
    for x in c:
        allcalls[x] += 1
    print('# vcount %d: %d instructions, %d lines, calls %s' % (
        vc, len(t), len(set(x >> 6 for x in t)), ' '.join(c)), file=sys.stderr)
for a in sorted(agg):
    print('%x %d' % (a, agg[a]))
