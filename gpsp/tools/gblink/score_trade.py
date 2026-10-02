#!/usr/bin/env python3
"""score_trade.py -- did a Gen 1 / Gen 2 link trade happen, exactly?

    score_trade.py GEN A_BEFORE A_SLOT B_BEFORE B_SLOT A_AFTER B_AFTER [--json]

GEN is gen1 or gen2.  *_BEFORE are the battery saves each side started from,
*_SLOT the party slot (0-based) each side offered, *_AFTER the saves the
games wrote after the trade.  In both generations a trade removes the
offered Pokemon from the party and appends the received one, byte for byte
(the struct, its OT name and its nickname travel unchanged).  So the verdict
is exact, not a heuristic:

    A_AFTER party == A_BEFORE party minus slot A_SLOT, plus B_BEFORE[B_SLOT]
    B_AFTER party == B_BEFORE party minus slot B_SLOT, plus A_BEFORE[A_SLOT]

and both saves' checksums must hold.  Anything else -- no trade, a half
trade, a corrupted struct, a save not written -- is a FAIL, with the first
difference named.  Exit status 0 = PASS.

One byte is not carried: Gen 2 resets a traded Pokemon's friendship to
BASE_HAPPINESS (70), struct offset 27 (pokecrystal engine/link/link.asm),
so the expected incoming struct has it.
"""
from __future__ import annotations

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pksav import info  # noqa: E402


def key(m: dict) -> tuple:
    return (m['raw'], m['ot'], m['nick'])


GEN2_FRIENDSHIP, BASE_HAPPINESS = 27, 70


def expect(before: dict, slot: int, incoming: dict, gen: str) -> list[tuple]:
    mons = [key(m) for i, m in enumerate(before['party']) if i != slot]
    raw = bytearray(bytes.fromhex(incoming['raw']))
    if gen == 'gen2':
        raw[GEN2_FRIENDSHIP] = BASE_HAPPINESS
    return mons + [(raw.hex(), incoming['ot'], incoming['nick'])]


def check(label: str, got: dict, want: list[tuple], errors: list[str]) -> None:
    have = [key(m) for m in got['party']]
    if not got['checksum_ok']:
        errors.append('%s: save checksum does not hold' % label)
    if len(have) != len(want):
        errors.append('%s: party has %d Pokemon, expected %d'
                      % (label, len(have), len(want)))
    for i, (h, w) in enumerate(zip(have, want)):
        if h != w:
            errors.append('%s: party slot %d is %s/%s, expected %s/%s'
                          % (label, i, h[2], h[1], w[2], w[1]))
            break


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    gen, a0, sa, b0, sb, a1, b1 = args
    sa, sb = int(sa), int(sb)
    A0, B0 = info(open(a0, 'rb').read(), gen), info(open(b0, 'rb').read(), gen)
    A1, B1 = info(open(a1, 'rb').read(), gen), info(open(b1, 'rb').read(), gen)
    errors: list[str] = []
    if sa >= len(A0['party']) or sb >= len(B0['party']):
        errors.append('offered slot beyond the party')
    else:
        check('A', A1, expect(A0, sa, B0['party'][sb], gen), errors)
        check('B', B1, expect(B0, sb, A0['party'][sa], gen), errors)
    verdict = 'PASS' if not errors else 'FAIL'
    result = dict(verdict=verdict, gen=gen, errors=errors,
                  a_received=None if errors else A1['party'][-1]['nick'],
                  b_received=None if errors else B1['party'][-1]['nick'],
                  a_player=A1['player'], b_player=B1['player'])
    if '--json' in sys.argv:
        print(json.dumps(result))
    else:
        print('%s %s: %s' % (verdict, gen, '; '.join(errors) if errors else
              '%s received %s (OT %s), %s received %s (OT %s)' % (
                  A1['player'], A1['party'][-1]['nick'], A1['party'][-1]['ot'],
                  B1['player'], B1['party'][-1]['nick'], B1['party'][-1]['ot'])))
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
