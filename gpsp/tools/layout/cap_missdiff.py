"""cap_missdiff.py MISSES SIMTXT A B NM_A NM_B [JITA JITB] : per-symbol miss deltas B - A, I and D,
plus per-set deltas.  Lines are PSP addresses (unplaced data: twin addresses)."""
import sys, bisect, collections
mp, simp, A, B, nmA, nmB = sys.argv[1:7]
jit = {A: int(sys.argv[7], 16) if len(sys.argv) > 7 else 0, B: int(sys.argv[8], 16) if len(sys.argv) > 8 else 0}
def nm(p):
    rows = []
    for l in open(p, errors='replace'):
        x = l.split()
        if len(x) == 4:
            rows.append((int(x[0], 16), int(x[1], 16), x[3]))
        elif len(x) == 3 and len(x[0]) == 8 and x[1] not in 'aAU':
            rows.append((int(x[0], 16), 0, x[2]))
    rows.sort()
    return rows
S = {A: nm(nmA), B: nm(nmB)}
AD = {k: [r[0] for r in v] for k, v in S.items()}
def who(lay, a):
    if jit[lay] and jit[lay] <= a < jit[lay] + 0xb00000:
        return 'JIT+%x' % ((a - jit[lay]) & ~0x3ff) if a - jit[lay] < 0x6000 else 'JIT(blocks)'
    if a < 0x08800000 or a >= 0x0a000000:
        return 'unplaced'
    rows, ad = S[lay], AD[lay]
    i = bisect.bisect_right(ad, a + 63) - 1
    names = []
    while i >= 0 and rows[i][0] + max(rows[i][1], 4) > a and len(names) < 3:
        if rows[i][0] <= a + 63:
            names.append(rows[i][2])
        i -= 1
    if not names and i >= 0:
        names.append(rows[i][2] + '?')
    return names[-1] if names else '?'
frames = 1
M = {A: collections.defaultdict(collections.Counter), B: collections.defaultdict(collections.Counter)}
for l in open(mp):
    if l.startswith('#'):
        frames = int(l.split()[2]); continue
    lay, kind, a, n = l.split()
    if lay in M:
        M[lay][kind][int(a, 16)] = int(n)
print('frames', frames)
for l in open(simp):
    if l.split()[1:2] and l.split()[1] in (A, B):
        print(l.strip())
for kind in 'ID':
    agg = {A: collections.Counter(), B: collections.Counter()}
    sets = {A: collections.Counter(), B: collections.Counter()}
    for lay in (A, B):
        for a, n in M[lay][kind].items():
            agg[lay][who(lay, a)] += n
            sets[lay][(a >> 6) % 128] += n
    tA, tB = sum(agg[A].values()), sum(agg[B].values())
    print('\n%s misses/frame: %s %.1f  %s %.1f  delta %+.1f (%.4f ms at 215.3 ns)' % (
        kind, A, tA / frames, B, tB / frames, (tB - tA) / frames, (tB - tA) / frames * 215.3e-6))
    keys = set(agg[A]) | set(agg[B])
    d = sorted(keys, key=lambda k: -abs(agg[B][k] - agg[A][k]))
    for k in d[:25]:
        print('  %-44s %9.1f %9.1f %+9.1f' % (k[:44], agg[A][k] / frames, agg[B][k] / frames, (agg[B][k] - agg[A][k]) / frames))
    ds = sorted(range(128), key=lambda s: -abs(sets[B][s] - sets[A][s]))
    print('  sets (B-A):', ' '.join('%d:%+.0f' % (s, (sets[B][s] - sets[A][s]) / frames) for s in ds[:16]))
