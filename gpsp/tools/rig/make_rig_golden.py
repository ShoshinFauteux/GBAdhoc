#!/usr/bin/env python3
"""make_rig_golden.py -- give every party mon in an FR/LG (or Emerald) save one
fixed move, so the double-battle rig's battle flow cannot depend on RNG.

    python tools/rig/make_rig_golden.py IN.sav OUT.sav --move GROWL
    python tools/rig/make_rig_golden.py IN.sav OUT.sav --move SWIFT --ppups 3 --hp 999
    python tools/rig/make_rig_golden.py SAV --show

Presets (docs/RIG-DOUBLE-BATTLE.md §1.3):
  GROWL (45)  40 PP, 100 acc, hits both foes, no damage   -> the lag rig
  SWIFT (129) 20 PP (+3 PP-ups = 32), never misses, hits both foes, damage
              -> the "realistic traffic" variant; pair with --hp 999 so no
              KO happens inside the 30-turn cap.

What is edited, and nothing else: in the MOST RECENT save slot's SaveBlock1
sector, each party mon's Attacks substructure (moves[4], pp[4]), the PP-up
bits in its Growth substructure, optionally hp/maxHP, the mon checksum, and
that sector's checksum.  Trainer identity, position, items, the other slot:
untouched.

SELF-VERIFYING, same rule as tools/e2e/clone_sav.py: refuses to write unless
(1) every live sector's existing checksum validates under the model and every
party mon's existing checksum validates, and (2) after editing, the output
re-decodes with the requested moves/PP and every checksum validates again.
"""
import argparse
import struct
import sys

SIG = 0x08012025
SECTION_SIZE = 0x1000
SLOT_SECTIONS = 14
SB1_SECTOR_DATA = 3968          # SaveBlock1 chunk 0 is a full sector
MON_SIZE = 100
LAYOUTS = {"emerald": (0x234, 4, 0x238), "frlg": (0x034, 1, 0x038)}
ORDERS = ["GAEM", "GAME", "GEAM", "GEMA", "GMAE", "GMEA",
          "AGEM", "AGME", "AEGM", "AEMG", "AMGE", "AMEG",
          "EGAM", "EGMA", "EAGM", "EAMG", "EMGA", "EMAG",
          "MGAE", "MGEA", "MAGE", "MAEG", "MEGA", "MEAG"]
MOVES = {"GROWL": (45, 40), "SWIFT": (129, 20), "TACKLE": (33, 35)}


def sector_ck(data):
    s = sum(struct.unpack_from('<%dI' % (len(data) // 4), data))
    return ((s >> 16) + (s & 0xFFFF)) & 0xFFFF


def live_sectors(buf):
    for i in range(2 * SLOT_SECTIONS):
        off = i * SECTION_SIZE
        sid, ck, sig, idx = struct.unpack_from('<HHII', buf, off + 0xFF4)
        if sig == SIG:
            yield off, sid, ck, idx, i // SLOT_SECTIONS


def latest_slot(buf):
    best = None
    for slot in (0, 1):
        idxs = [idx for _o, _s, _c, idx, sl in live_sectors(buf) if sl == slot]
        if len(idxs) == SLOT_SECTIONS and min(idxs) == max(idxs):
            if best is None or idxs[0] > best[0]:
                best = (idxs[0], slot)
    if best is None:
        sys.exit("no fully valid save slot")
    return best[1]


def sb1_offset(buf, slot):
    for off, sid, _ck, _idx, sl in live_sectors(buf):
        if sl == slot and sid == 1:
            return off
    sys.exit("SaveBlock1 sector not found")


def detect(buf, sb1):
    for name, (coff, cw, poff) in LAYOUTS.items():
        count = buf[sb1 + coff] if cw == 1 else struct.unpack_from('<I', buf, sb1 + coff)[0]
        if 1 <= count <= 6 and struct.unpack_from('<I', buf, sb1 + poff)[0]:
            return name, count, sb1 + poff
    sys.exit("cannot identify SaveBlock1 layout")


def mon_subs(buf, moff):
    """-> (key, order, decrypted 48 bytes)"""
    pers, otid = struct.unpack_from('<II', buf, moff)
    key = pers ^ otid
    raw = struct.unpack_from('<12I', buf, moff + 0x20)
    dec = struct.pack('<12I', *[w ^ key for w in raw])
    return key, ORDERS[pers % 24], bytearray(dec)


def mon_ck(dec):
    return sum(struct.unpack('<24H', bytes(dec))) & 0xFFFF


def describe(buf, moff):
    _key, order, dec = mon_subs(buf, moff)
    a = order.index('A') * 12
    g = order.index('G') * 12
    moves = struct.unpack_from('<4H', dec, a)
    pp = tuple(dec[a + 8:a + 12])
    species = struct.unpack_from('<H', dec, g)[0]
    ppb = dec[g + 8]
    hp, maxhp = struct.unpack_from('<HH', buf, moff + 0x56)
    ok = mon_ck(dec) == struct.unpack_from('<H', buf, moff + 0x1C)[0]
    return dict(species=species, moves=moves, pp=pp, ppbonuses=ppb, hp=hp,
                maxhp=maxhp, level=buf[moff + 0x54], ck_ok=ok)


def verify_all(buf):
    bad = []
    for off, sid, ck, _idx, _sl in live_sectors(buf):
        if sid >= SLOT_SECTIONS:
            bad.append("sector id %d" % sid)
        elif sector_ck(buf[off:off + SB1_SECTOR_DATA]) != ck:
            # every live sector must validate at the full sector size (the
            # unused tail is zero); a real mismatch is a corrupt input
            bad.append("sector %d @0x%x" % (sid, off))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--move", choices=sorted(MOVES))
    ap.add_argument("--ppups", type=int, default=0, choices=(0, 1, 2, 3))
    ap.add_argument("--hp", type=int, default=0, help="set hp and maxHP (1..999)")
    ap.add_argument("--scene-off", action="store_true",
                    help="options: battle animations OFF (SaveBlock2 +0x15 bit 2)")
    ap.add_argument("--text-fast", action="store_true",
                    help="options: text speed FAST (SaveBlock2 +0x14 bits 0-2 = 2)")
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()

    buf = bytearray(open(args.src, "rb").read())
    slot = latest_slot(buf)
    sb1 = sb1_offset(buf, slot)
    game, count, party = detect(buf, sb1)
    mons = [party + i * MON_SIZE for i in range(count)]

    bad = verify_all(buf)
    for i, m in enumerate(mons):
        if not describe(buf, m)["ck_ok"]:
            bad.append("mon %d checksum" % i)
    for i, m in enumerate(mons):
        print("  in  slot=%d game=%s mon=%d %s" % (slot, game, i, describe(buf, m)))
    if bad:
        sys.exit("input does not validate (%s); refusing" % ", ".join(bad))
    if args.show:
        return 0
    if not args.dst or not args.move:
        sys.exit("need DST and --move (or --show)")
    if args.hp and not 1 <= args.hp <= 999:
        sys.exit("--hp must be 1..999")

    move, base_pp = MOVES[args.move]
    pp = base_pp + (base_pp * 20 // 100) * args.ppups
    for m in mons:
        key, order, dec = mon_subs(buf, m)
        a = order.index('A') * 12
        g = order.index('G') * 12
        struct.pack_into('<4H', dec, a, move, 0, 0, 0)
        dec[a + 8:a + 12] = bytes((pp, 0, 0, 0))
        dec[g + 8] = args.ppups & 3          # PP-up bits for move slot 0 only
        struct.pack_into('<H', buf, m + 0x1C, mon_ck(dec))
        enc = struct.unpack('<12I', bytes(dec))
        struct.pack_into('<12I', buf, m + 0x20, *[w ^ key for w in enc])
        if args.hp:
            struct.pack_into('<HH', buf, m + 0x56, args.hp, args.hp)
        struct.pack_into('<I', buf, m + 0x50, 0)   # status: none
    struct.pack_into('<H', buf, sb1 + 0xFF6,
                     sector_ck(buf[sb1:sb1 + SB1_SECTOR_DATA]))
    if args.scene_off or args.text_fast:
        # SaveBlock2 (sector id 0 of the same slot): pokefirered global.h
        # +0x14 u16 optionsTextSpeed:3 | optionsWindowFrameType:5 | ... and
        # +0x15 bit 2 optionsBattleSceneOff.  Shorter turns, same link logic.
        sb2 = [o for o, sid, _c, _i, sl in live_sectors(buf)
               if sl == slot and sid == 0][0]
        if args.text_fast:
            buf[sb2 + 0x14] = (buf[sb2 + 0x14] & ~0x07) | 2
        if args.scene_off:
            buf[sb2 + 0x15] |= 0x04
        struct.pack_into('<H', buf, sb2 + 0xFF6,
                         sector_ck(buf[sb2:sb2 + SB1_SECTOR_DATA]))

    bad = verify_all(buf)
    for i, m in enumerate(mons):
        d = describe(buf, m)
        print("  out slot=%d game=%s mon=%d %s" % (slot, game, i, d))
        if not d["ck_ok"] or d["moves"] != (move, 0, 0, 0) or d["pp"][0] != pp:
            bad.append("mon %d did not re-decode as edited" % i)
        if args.hp and (d["hp"], d["maxhp"]) != (args.hp, args.hp):
            bad.append("mon %d hp" % i)
    if bad:
        sys.exit("post-edit validation failed (%s); NOT writing" % ", ".join(bad))
    open(args.dst, "wb").write(buf)
    print("OK: %s -> %s  move=%s pp=%d ppups=%d hp=%s scene_off=%d "
          "text_fast=%d (%d mons)"
          % (args.src, args.dst, args.move, pp, args.ppups, args.hp or "kept",
             args.scene_off, args.text_fast, count))
    return 0


if __name__ == "__main__":
    sys.exit(main())
