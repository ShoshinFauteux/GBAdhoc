"""Splice the sound driver blob into the bundled BIOS image (bios/splice_sound_driver.sh runs this).

    python3 -I splice_sound_driver.py BIOS.bin BLOB.bin NM.txt

Every byte below SD_BASE stays as it was: only the 0x2800-0x3FFF tail (zero in the image the
driver was first spliced into) and eight words of swi_branch_table change, so the other SWIs and
the boot code cannot move.  Re-running it on an already spliced image replaces the old blob.
"""
import struct
import sys

SD_BASE = 0x2800
TABLE = 0xB0              # swi_branch_table in open_gba_bios.bin
SWI_INVALID = 0x15C       # its bare `bx lr`
ENTRIES = {0x19: "sd_SoundBias", 0x1A: "sd_SoundDriverInit", 0x1B: "sd_SoundDriverMode",
           0x1C: "sd_SoundDriverMain", 0x1D: "sd_SoundDriverVSync", 0x1E: "sd_SoundChannelClear",
           0x28: "sd_VSyncOff", 0x29: "sd_VSyncOn"}


def main(bios_path, blob_path, nm_path):
    bios = bytearray(open(bios_path, "rb").read())
    blob = open(blob_path, "rb").read()
    assert len(bios) == 0x4000, "the BIOS image must be 16 KiB"
    assert bios[SWI_INVALID:SWI_INVALID + 4] == bytes.fromhex("1eff2fe1"), "swi_Invalid moved"
    if SD_BASE + len(blob) > 0x4000:
        raise SystemExit("blob too big: %#x bytes, %#x free" % (len(blob), 0x4000 - SD_BASE))
    syms = {}
    for line in open(nm_path):
        f = line.split()
        if len(f) == 3:
            syms[f[2]] = int(f[0], 16)
    for swi, name in ENTRIES.items():
        old = struct.unpack_from("<I", bios, TABLE + 4 * swi)[0]
        if old != SWI_INVALID and not SD_BASE <= old < 0x4000:
            raise SystemExit("table entry %#x is %#x: not swi_Invalid nor a previous splice" % (swi, old))
        addr = syms[name]
        assert SD_BASE <= addr < SD_BASE + len(blob) and addr % 4 == 0, (name, hex(addr))
        struct.pack_into("<I", bios, TABLE + 4 * swi, addr)
    if any(bios[0x2439:SD_BASE]):
        raise SystemExit("bytes between the old image end and SD_BASE are not zero")
    bios[SD_BASE:] = bytes(0x4000 - SD_BASE)
    bios[SD_BASE:SD_BASE + len(blob)] = blob
    open(bios_path, "wb").write(bios)
    print("spliced %d bytes at %#x (%d free); table:" % (len(blob), SD_BASE, 0x4000 - SD_BASE - len(blob)))
    for swi, name in sorted(ENTRIES.items()):
        print("  %02x %-22s %#06x" % (swi, name, syms[name]))


if __name__ == "__main__":
    main(*sys.argv[1:4])
