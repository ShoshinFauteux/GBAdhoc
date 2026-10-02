#!/usr/bin/env python3
"""pksav.py -- read (and, for fixtures, edit) Gen 1 / Gen 2 Pokemon saves.

    pksav.py show FILE [gen1|gen2]          party, trainer, map, checksums
    pksav.py json FILE [gen1|gen2]          the same as JSON (for scorers)

Layouts are the English releases' (pret/pokered, pret/pokecrystal):

  Gen 1 (Red/Blue/Yellow), bank 1 of the 32 KiB SRAM:
    0x2598 player name (11)       0x2605 player ID (BE)
    0x260A current map, 0x260D Y, 0x260E X     (wCurMap/wYCoord/wXCoord)
    0x2F2C party: count, species[7], 6 x 44-byte mons, OT names, nicknames
    0x3523 checksum = ~sum(0x2598..0x3522) & 0xFF
  Gen 2 (Crystal, international):
    0x2009 player ID (BE), 0x200B player name
    0x2865 party: count, species[7], 6 x 48-byte mons, OT names, nicknames
    0x2D0D checksum = sum(0x2009..0x2B82) & 0xFFFF (LE); the backup copy
    at 0x1209..0x1D82 has its checksum at 0x1F0D.

A file may carry GBAdhoc's 48-byte RTC trailer (Crystal); it is ignored.
"""
from __future__ import annotations

import json
import sys

GEN1 = dict(name=0x2598, pid=0x2605, map=0x260A, y=0x260D, x=0x260E,
            party=0x2F2C, mon=44, lvl=0x21, csum_lo=0x2598, csum_hi=0x3522,
            csum=0x3523)
GEN2 = dict(name=0x200B, pid=0x2009, party=0x2865, mon=48, lvl=0x1F,
            csum_lo=0x2009, csum_hi=0x2B82, csum=0x2D0D)


def text(b: bytes) -> str:
    out = []
    for c in b:
        if c == 0x50:
            break
        if 0x80 <= c <= 0x99:
            out.append(chr(ord('A') + c - 0x80))
        elif 0xA0 <= c <= 0xB9:
            out.append(chr(ord('a') + c - 0xA0))
        elif 0xF6 <= c <= 0xFF:
            out.append(chr(ord('0') + c - 0xF6))
        elif c == 0x7F:
            out.append(' ')
        else:
            out.append('<%02x>' % c)
    return ''.join(out)


def encode(s: str, n: int = 11) -> bytes:
    out = bytearray()
    for ch in s:
        if 'A' <= ch <= 'Z':
            out.append(0x80 + ord(ch) - ord('A'))
        elif 'a' <= ch <= 'z':
            out.append(0xA0 + ord(ch) - ord('a'))
        elif '0' <= ch <= '9':
            out.append(0xF6 + ord(ch) - ord('0'))
        elif ch == ' ':
            out.append(0x7F)
        else:
            raise ValueError(ch)
    out.append(0x50)
    return bytes(out.ljust(n, b'\x50'))[:n]


def detect(d: bytes) -> str:
    return 'gen2' if checksum2(d) == int.from_bytes(d[0x2D0D:0x2D0F], 'little') else 'gen1'


def checksum1(d: bytes) -> int:
    return (~sum(d[0x2598:0x3523])) & 0xFF


def checksum2(d: bytes, lo: int = 0x2009, hi: int = 0x2B82) -> int:
    return sum(d[lo:hi + 1]) & 0xFFFF


def party(d: bytes, L: dict) -> list[dict]:
    p = L['party']
    n = d[p]
    mons = []
    base = p + 8
    ot = base + 6 * L['mon']
    nick = ot + 6 * 11
    for i in range(min(n, 6)):
        m = d[base + i * L['mon']: base + (i + 1) * L['mon']]
        mons.append(dict(species=d[p + 1 + i], level=m[L['lvl']],
                         ot_id=int.from_bytes(m[12:14] if L is GEN1 else m[6:8], 'big'),
                         ot=text(d[ot + i * 11: ot + (i + 1) * 11]),
                         nick=text(d[nick + i * 11: nick + (i + 1) * 11]),
                         raw=m.hex()))
    return mons


def info(d: bytes, gen: str | None = None) -> dict:
    gen = gen or detect(d)
    L = GEN1 if gen == 'gen1' else GEN2
    r = dict(gen=gen, player=text(d[L['name']:L['name'] + 11]),
             player_id=int.from_bytes(d[L['pid']:L['pid'] + 2], 'big'),
             party=party(d, L))
    if gen == 'gen1':
        r.update(map=d[GEN1['map']], y=d[GEN1['y']], x=d[GEN1['x']],
                 checksum_ok=checksum1(d) == d[0x3523])
    else:
        r.update(checksum_ok=checksum2(d) == int.from_bytes(d[0x2D0D:0x2D0F], 'little'),
                 backup_checksum_ok=checksum2(d, 0x1209, 0x1D82) ==
                 int.from_bytes(d[0x1F0D:0x1F0F], 'little'))
    return r


def fix_checksums(d: bytearray, gen: str) -> None:
    if gen == 'gen1':
        d[0x3523] = checksum1(d)
    else:
        d[0x2D0D:0x2D0F] = checksum2(d).to_bytes(2, 'little')


def rebrand(d: bytearray, gen: str, name: str, pid: int, nick: dict) -> None:
    """A second trainer from a copy of one save: new player name and ID, the
    same on every party Pokemon's OT, optional nicknames {slot: text}.
    Checksums are recomputed (Gen 2: the main copy, which the game loads
    when its checksum holds)."""
    L = GEN1 if gen == 'gen1' else GEN2
    d[L['name']:L['name'] + 11] = encode(name)
    d[L['pid']:L['pid'] + 2] = pid.to_bytes(2, 'big')
    p = L['party']
    base = p + 8
    ot = base + 6 * L['mon']
    nk = ot + 6 * 11
    idoff = 12 if gen == 'gen1' else 6
    for i in range(min(d[p], 6)):
        m = base + i * L['mon']
        d[m + idoff:m + idoff + 2] = pid.to_bytes(2, 'big')
        d[ot + i * 11: ot + (i + 1) * 11] = encode(name)
    for i, t in nick.items():
        d[nk + i * 11: nk + (i + 1) * 11] = encode(t)
    fix_checksums(d, gen)


def main() -> int:
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == 'rebrand':
        # rebrand IN OUT gen NAME ID [SLOT=NICK ...]
        out, gen, name, pid = sys.argv[3], sys.argv[4], sys.argv[5], int(sys.argv[6])
        nick = {int(k): v for k, v in (a.split('=', 1) for a in sys.argv[7:])}
        d = bytearray(open(path, 'rb').read())
        rebrand(d, gen, name, pid, nick)
        open(out, 'wb').write(bytes(d))
        return 0
    gen = sys.argv[3] if len(sys.argv) > 3 else None
    d = open(path, 'rb').read()
    r = info(d, gen)
    if cmd == 'json':
        print(json.dumps(r))
        return 0
    print('%s player=%s id=%05d%s checksum_ok=%s' % (
        r['gen'], r['player'], r['player_id'],
        ' map=0x%02x y=%d x=%d' % (r['map'], r['y'], r['x']) if 'map' in r else '',
        r['checksum_ok']))
    for i, m in enumerate(r['party']):
        print('  %d species=0x%02x L%-3d nick=%-10s ot=%s(%05d)' % (
            i, m['species'], m['level'], m['nick'], m['ot'], m['ot_id']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
