#!/usr/bin/env python3
"""gen1_fixture_pokes.py -- the RAM a Gen 1 link fixture is built from.

    gen1_fixture_pokes.py SLOT FRAME SIDE > pokes.txt     (SIDE = a | b)

Pokemon Blue gives a new game nothing a Cable Club accepts (no Pokedex, no
Pokemon), and no Gen 1 save exists among the owner's backups (the Red save
on F: is blank).  tools/gblink/make_fixtures.sh therefore plays a new game
to Red's bedroom with the autopilot, then writes, at one frame, what the
opening hours of the game would have produced:

  - the player's name and ID; one party Pokemon (level 10; a Squirtle for
    side a, a Charmander for side b) with that trainer as its OT;
  - the Pallet Town events up to and including EVENT_GOT_POKEDEX (the
    Cable Club receptionist refuses without it);
  - a Fly warp to Viridian City (wDestinationMap + BIT_FLY_WARP): the game's
    own warp code loads the map and lands the player at the Pokemon Center
    door, exactly as flying there would.

The game itself then walks in and SAVES (START > SAVE), so the fixture is a
save file the game wrote, with its own checksum.  Addresses: pret/pokered
(English Red/Blue): wPlayerName D158, wPartyCount D163, wPartyMons D16B,
wPartyMonOT D273, wPartyMonNicks D2B5, wPlayerID D359, wDestinationMap
D71A, wStatusFlags6 D732, wEventFlags D747.
"""
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
from pksav import encode  # noqa: E402

SIDES = {
    'a': dict(name='LINKA', pid=11111, nick='SQUIRTLE',
              mon=[0xB1, 0x00, 0x1E, 0x0A, 0x00, 0x15, 0x15, 0x2D,
                   0x21, 0x27, 0x91, 0x00]),
    'b': dict(name='LINKB', pid=22222, nick='CHARMANDER',
              mon=[0xB0, 0x00, 0x1C, 0x0A, 0x00, 0x14, 0x14, 0x2D,
                   0x0A, 0x2D, 0x34, 0x00]),
}
TAIL = {  # exp .. stats, after the OT ID
    'a': bytes([0x00, 0x02, 0x30]) + bytes(10) + bytes([0xAA, 0xAA, 35, 30, 30, 0,
          10, 0x00, 0x1E, 0x00, 0x14, 0x00, 0x18, 0x00, 0x12, 0x00, 0x14]),
    'b': bytes([0x00, 0x02, 0x30]) + bytes(10) + bytes([0xAA, 0xAA, 35, 40, 25, 0,
          10, 0x00, 0x1C, 0x00, 0x14, 0x00, 0x12, 0x00, 0x18, 0x00, 0x14]),
}


def pokes(side: str) -> list[tuple[int, bytes]]:
    s = SIDES[side]
    pid = s['pid'].to_bytes(2, 'big')
    mon = bytes(s['mon']) + pid + TAIL[side]
    assert len(mon) == 44
    return [
        (0xD158, encode(s['name'])),
        (0xD359, pid),
        (0xD163, bytes([1, s['mon'][0], 0xFF])),
        (0xD16B, mon),
        (0xD273, encode(s['name'])),
        (0xD2B5, encode(s['nick'])),
        (0xD747, bytes([0x01])),          # EVENT_FOLLOWED_OAK_INTO_LAB
        (0xD74B, bytes([0x3F])),          # ... through EVENT_GOT_POKEDEX
        (0xD71A, bytes([0x01])),          # wDestinationMap = VIRIDIAN_CITY
        (0xD732, bytes([0x09])),          # wStatusFlags6: play-time bit (as the new game set it) + BIT_FLY_WARP
    ]


def main() -> int:
    slot, frame, side = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    print('# Gen 1 fixture side %s (tools/gblink/gen1_fixture_pokes.py)' % side)
    for addr, data in pokes(side):
        print('%d %d %04X %s' % (slot, frame, addr, data.hex()))
    return 0


if __name__ == '__main__':
    sys.exit(main())
