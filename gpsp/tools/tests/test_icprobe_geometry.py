#!/usr/bin/env python3
"""The I-cache geometry analyser against probe logs from SIMULATED caches.

The probe's chains (psp/icprobe.c: K blocks at stride S, warm; 2048 blocks at
small strides, cold) are replayed through an LRU set-associative cache of a
known geometry, with a hit and a miss cost and +-10 % deterministic noise, and
written as the log lines the console writes -- with the fixed per-pass
loop-back cost that makes ns per block fall as 1/K (seen in PPSSPP).  The analyser must recover every
geometry exactly, and must report NO_GEOMETRY for a flat log (PPSSPP, which
models no cache) -- never a guessed one."""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, '..', 'cachemap', 'icprobe_geometry.py')
STRIDES = (64, 1024, 2048, 4096, 8192, 16384, 32768, 65536)
COLD = (16, 32, 64, 128, 256)
BUF = 1 << 20


class LRU:
    def __init__(self, size, ways, line):
        self.ways, self.line = ways, line
        self.nsets = size // (ways * line)
        self.sets = [[] for _ in range(self.nsets)]

    def fetch(self, addr):
        ln = addr // self.line
        s = self.sets[ln % self.nsets]
        if ln in s:
            s.remove(ln)
            s.insert(0, ln)
            return 0
        s.insert(0, ln)
        del s[self.ways:]
        return 1


def block_addrs(i, k, s):
    """Instruction addresses of block i (2 words, the last block 7)."""
    n = 7 if i == k - 1 else 2
    return [i * s + 4 * w for w in range(n)]


def log_for(geom, hit=3.0, miss=60.0, seed=1, loop=9.0):
    rnd = random.Random(seed)
    lines = ['EVT icprobe_begin base=0x9000000 visits=240000 reps=7']
    for s in STRIDES:
        for k in range(1, 13):
            if k * s > BUF:
                break
            if geom is None:
                ns = hit + loop / k      # PPSSPP: the loop-back cost only
            else:
                c = LRU(*geom)
                for _ in range(3):              # warm
                    for i in range(k):
                        for a in block_addrs(i, k, s):
                            c.fetch(a)
                m = 0
                for i in range(k):
                    for a in block_addrs(i, k, s):
                        m += c.fetch(a)
                ns = hit + loop / k + miss * m / k
            ns *= rnd.uniform(0.9, 1.1)
            lines.append('EVT icprobe_warm s=%d k=%d ns10=%d' % (s, k, ns * 10))
    for s in COLD:
        if geom is None:
            ns = hit
        else:
            c = LRU(*geom)
            m = sum(c.fetch(a) for i in range(2048) for a in block_addrs(i, 2048, s))
            ns = hit + miss * m / 2048.0
        ns *= rnd.uniform(0.95, 1.05)
        lines.append('EVT icprobe_cold s=%d k=2048 ns10=%d' % (s, ns * 10))
    lines.append('EVT icprobe_end')
    return '\n'.join(lines) + '\n'


def run(text):
    with tempfile.NamedTemporaryFile('w', suffix='.log', delete=False) as fh:
        fh.write(text)
        p = fh.name
    try:
        r = subprocess.run([sys.executable, TOOL, p], capture_output=True,
                           text=True)
    finally:
        os.remove(p)
    return r.returncode, r.stdout


def main():
    bad = 0
    for geom in ((16384, 2, 64), (16384, 4, 64), (8192, 2, 32),
                 (32768, 2, 64), (16384, 2, 128)):
        for seed in (1, 2, 3):
            rc, out = run(log_for(geom, seed=seed))
            want = 'I-cache: %d B = %d-way x %d B per way, %d B lines' % (
                geom[0], geom[1], geom[0] // geom[1], geom[2])
            if rc != 0 or want not in out:
                bad += 1
                print('FAIL icprobe geometry %s seed %d:\n%s' % (geom, seed, out))
    rc, out = run(log_for(None))
    if rc != 2 or 'NO_GEOMETRY' not in out:
        bad += 1
        print('FAIL icprobe flat log (PPSSPP) must be NO_GEOMETRY:\n%s' % out)
    rc, out = run('EVT boot_ok\n')
    if rc == 0:
        bad += 1
        print('FAIL icprobe empty log passed:\n%s' % out)
    if bad:
        return 1
    print('icprobe geometry: 5 geometries x 3 noise seeds recovered exactly; '
          'a flat log is NO_GEOMETRY; an empty one is not a pass')
    return 0


if __name__ == '__main__':
    sys.exit(main())
