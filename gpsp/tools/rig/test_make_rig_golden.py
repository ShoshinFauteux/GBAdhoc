#!/usr/bin/env python3
"""Round-trip test for make_rig_golden.py on a SYNTHETIC gen-3 FR/LG save
(no copyrighted save needed): build a valid 14-sector slot with a 2-mon party
whose substructures are encrypted and shuffled exactly as the game does, run
the tool, and require:
  - the edited mons decode with the requested move/PP/PP-ups/HP,
  - every mon checksum and sector checksum validates,
  - nothing outside the party (trainer sector, position) changed,
  - a corrupted input is REFUSED (no output written).
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "make_rig_golden.py")
sys.path.insert(0, HERE)
import make_rig_golden as m  # noqa: E402


def build_save():
    buf = bytearray(0x20000)
    for sid in range(14):
        off = sid * 0x1000
        if sid == 0:
            buf[off:off + 8] = b"\xbb\xbc\xbd\xff\xff\xff\xff\xff"
            struct.pack_into("<I", buf, off + 0x0A, 0x12345678)
        if sid == 1:
            struct.pack_into("<hh", buf, off, 10, 4)          # pos
            buf[off + 0x34] = 2                                # party count
            for i, (pers, otid, species) in enumerate(((0x0000002A, 0x11112222, 1),
                                                       (0x00000017, 0x11112222, 16))):
                mo = off + 0x38 + i * 100
                struct.pack_into("<II", buf, mo, pers, otid)
                order = m.ORDERS[pers % 24]
                dec = bytearray(48)
                g = order.index("G") * 12
                a = order.index("A") * 12
                struct.pack_into("<H", dec, g, species)
                struct.pack_into("<4H", dec, a, 33, 0, 0, 0)
                dec[a + 8] = 35
                struct.pack_into("<H", buf, mo + 0x1C, m.mon_ck(dec))
                key = pers ^ otid
                w = struct.unpack("<12I", bytes(dec))
                struct.pack_into("<12I", buf, mo + 0x20, *[x ^ key for x in w])
                buf[mo + 0x54] = 5
                struct.pack_into("<HH", buf, mo + 0x56, 20, 20)
        struct.pack_into("<HHII", buf, off + 0xFF4, sid,
                         m.sector_ck(buf[off:off + 3968]), m.SIG, 7)
    return buf


def main():
    with tempfile.TemporaryDirectory() as td:
        src, dst = os.path.join(td, "in.sav"), os.path.join(td, "out.sav")
        orig = build_save()
        open(src, "wb").write(orig)
        r = subprocess.run([sys.executable, TOOL, src, dst, "--move", "SWIFT",
                            "--ppups", "3", "--hp", "999", "--scene-off",
                            "--text-fast"],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        out = bytearray(open(dst, "rb").read())
        assert not m.verify_all(out)
        sb1 = m.sb1_offset(out, m.latest_slot(out))
        for i in range(2):
            d = m.describe(out, sb1 + 0x38 + i * 100)
            assert d["ck_ok"] and d["moves"] == (129, 0, 0, 0), d
            assert d["pp"][0] == 32 and d["ppbonuses"] == 3, d
            assert (d["hp"], d["maxhp"]) == (999, 999), d
            assert d["species"] == (1, 16)[i], d
        assert out[0x14] & 7 == 2 and out[0x15] & 4, "options not set"
        assert out[0:0x14] == orig[0:0x14], "trainer identity changed"
        assert out[0x16:0xFF4] == orig[0x16:0xFF4], "SaveBlock2 changed beyond options"
        assert out[0x1000:0x1034] == orig[0x1000:0x1034], "position changed"
        assert out[0x2000:] == orig[0x2000:], "other sectors changed"

        bad = bytearray(orig)
        bad[0x1000 + 0x38 + 0x25] ^= 0xFF                     # corrupt a mon
        open(src, "wb").write(bad)
        os.remove(dst)
        r = subprocess.run([sys.executable, TOOL, src, dst, "--move", "GROWL"],
                           capture_output=True, text=True)
        assert r.returncode != 0 and not os.path.exists(dst), "corrupt input accepted"
    print("make_rig_golden: round trip, isolation and refusal all pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
