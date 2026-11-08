"""cap_orders.py C4.nm C4.uspath OUTDIR NAME=ORDER ... : never-linked layouts that
re-link the blocks after the hot core (from 0x08806220, 32-byte aligned) in
ORDER, e.g. "cap,ml,us,render,W" or "us,cap,ml,W" (render then goes cold, at
the end) or with a pad "P192,us,cap,ml,W".  Blocks keep their internal layout
from the c4 link.  Prints each layout's block addresses mod 8 KiB."""
import os, sys
src, usp, outd = sys.argv[1:4]
lines = open(src, errors='replace').read().splitlines()
def first(name):
    for l in lines:
        p = l.split()
        if len(p) >= 3 and p[-1] == name and p[-2] in 'tT':
            return int(p[0], 16), (int(p[1], 16) if len(p) == 4 else 0)
def span(a, b):
    lo = first(a)[0]; e = first(b); return lo, e[0] + e[1]
B = {'us': span('update_scanline', 'update_scanline'),
     'render': span('update_scanline_render', 'update_scanline_render'),
     'cap': span('me_capture_line', 'me_capture_line'),
     'ml': span('_ZL12ml_line_bodym', '_ZL12ml_relevancePKtPm.isra.0'),
     'W': span('msr_fast_store', 'translate_block_arm')}
os.makedirs(outd, exist_ok=True)
for spec in sys.argv[4:]:
    name, order = spec.split('=', 1)
    at = 0x08806220
    delta = {}
    for tok in order.split(','):
        if tok.startswith('P'):
            at += int(tok[1:]); continue
        lo, hi = B[tok]
        at = (at + 31) & ~31
        delta[tok] = at - lo
        at += hi - lo
    if 'render' not in delta:
        lo, hi = B['render']; delta['render'] = 0x0a100000 - lo
    out = []
    for l in lines:
        q = l.split()
        if len(q) >= 3 and len(q[0]) == 8 and q[-2] in 'tTwW':
            a = int(q[0], 16)
            for k, (lo, hi) in B.items():
                if lo <= a < hi and k in delta:
                    q[0] = '%08x' % (a + delta[k]); l = ' '.join(q); break
        out.append(l)
    open(os.path.join(outd, name + '.nm'), 'w').write('\n'.join(out) + '\n')
    with open(os.path.join(outd, name + '.uspath'), 'w') as fh:
        for l in open(usp):
            a, n = l.split(); a = int(a, 16)
            for k, (lo, hi) in B.items():
                if lo <= a < hi and k in delta:
                    a += delta[k]; break
            fh.write('%x %s\n' % (a, n))
    print(name, order, ' '.join('%s@%04x' % (k, (B[k][0] + d) % 8192) for k, d in delta.items()))
