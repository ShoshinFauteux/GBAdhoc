#!/usr/bin/env python3
"""read_party.py -- decode the party from a gen-3 .sav (trade oracle).

Reads the most-recent save slot (highest save_index among valid sections),
locates SaveBlock1 (sections 1..4, section 1 holds offset 0 of the struct),
and decodes the party.  The party lives at a DIFFERENT SaveBlock1 offset per
game (Phase-4 FR/LG work):

    Emerald (BPEE)          count u32 @ 0x0234, party @ 0x0238
    FireRed/LeafGreen       count u8  @ 0x0034, party @ 0x0038
    (BPRE/BPGE rev 1)

which is why `--game` exists; with no flag the layout is auto-detected by
trying both and keeping the one whose count is 1..6 with a non-zero
personality in slot 0.  Everything else -- the 14-sector/2-slot container,
the 0x08012025 signature, the 100-byte struct Pokemon -- is identical.
Gen-3 box data substructures are decrypted with
key = personality ^ otId and unshuffled by personality % 24 to recover the
species (Growth substruct word 0).

Output (one line per mon, machine-greppable):
  mon slot=N personality=0x%08x otid=0x%08x species=NNN level=NN

Usage: read_party.py FILE.sav [--game=emerald|frlg]
Layout facts verified against pret/pokeemerald and pret/pokefirered (save.h,
pokemon.h, global.h SaveBlock1) -- same layout model clone_sav.py validates
checksums against before it ever writes anything.
"""
import struct
import sys

SIG = 0x08012025
SECTION_SIZE = 0x1000
SLOT_SECTIONS = 14
MON_SIZE = 100
# SaveBlock1 offsets: (count offset, count width, party offset)
LAYOUTS = {
    "emerald": (0x234, 4, 0x238),
    "frlg":    (0x034, 1, 0x038),
}

# substructure shuffle order by personality % 24 (G=0 A=1 E=2 M=3)
ORDERS = ["GAEM", "GAME", "GEAM", "GEMA", "GMAE", "GMEA",
          "AGEM", "AGME", "AEGM", "AEMG", "AMGE", "AMEG",
          "EGAM", "EGMA", "EAGM", "EAMG", "EMGA", "EMAG",
          "MGAE", "MGEA", "MAGE", "MAEG", "MEGA", "MEAG"]


def latest_slot(buf):
    best = (-1, None)
    for slot in range(2):
        idxs = []
        ok = True
        for s in range(SLOT_SECTIONS):
            off = (slot * SLOT_SECTIONS + s) * SECTION_SIZE
            sec_id, ck, sig, idx = struct.unpack_from('<HHII', buf, off + 0xFF4)
            if sig != SIG:
                ok = False
                continue
            idxs.append(idx)
        if ok and idxs and min(idxs) == max(idxs) and idxs[0] > best[0]:
            best = (idxs[0], slot)
    if best[1] is None:
        sys.exit("no fully-valid save slot found")
    return best[1]


def section_map(buf, slot):
    m = {}
    for s in range(SLOT_SECTIONS):
        off = (slot * SLOT_SECTIONS + s) * SECTION_SIZE
        sec_id = struct.unpack_from('<H', buf, off + 0xFF4)[0]
        m[sec_id] = off
    return m


def read_count(sb1, name):
    coff, cw, poff = LAYOUTS[name]
    count = sb1[coff] if cw == 1 else struct.unpack_from('<I', sb1, coff)[0]
    return count, poff


def detect(sb1):
    for name in ("emerald", "frlg"):
        count, poff = read_count(sb1, name)
        if 1 <= count <= 6 and struct.unpack_from('<I', sb1, poff)[0] != 0:
            return name
    sys.exit("cannot tell which SaveBlock1 layout this .sav uses "
             "(pass --game=emerald or --game=frlg)")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    game = next((a.split("=", 1)[1] for a in sys.argv[1:]
                 if a.startswith("--game=")), None)
    buf = open(args[0], 'rb').read()
    slot = latest_slot(buf)
    secs = section_map(buf, slot)
    # SaveBlock1 = sections 1..4, each contributing 3968 data bytes
    sb1 = b''.join(buf[secs[1 + i]:secs[1 + i] + 3968] for i in range(4))
    if game is None:
        game = detect(sb1)
    elif game not in LAYOUTS:
        sys.exit(f"unknown --game={game}")
    count, PARTY_OFF = read_count(sb1, game)
    print(f"slot={slot} game={game} party_count={count}")
    for i in range(min(count, 6)):
        mon = sb1[PARTY_OFF + i * MON_SIZE: PARTY_OFF + (i + 1) * MON_SIZE]
        pers, otid = struct.unpack_from('<II', mon, 0)
        level = mon[84]
        key = pers ^ otid
        data = mon[32:80]
        words = list(struct.unpack('<12I', data))
        words = [w ^ key for w in words]
        order = ORDERS[pers % 24]
        gpos = order.index('G')
        growth = struct.pack('<3I', *words[gpos * 3:gpos * 3 + 3])
        species = struct.unpack_from('<H', growth, 0)[0]
        print(f"mon slot={i} personality=0x{pers:08x} otid=0x{otid:08x} "
              f"species={species} level={level}")


if __name__ == '__main__':
    main()
