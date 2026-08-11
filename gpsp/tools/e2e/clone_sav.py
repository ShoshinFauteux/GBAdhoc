#!/usr/bin/env python3
"""clone_sav.py -- clone a Pokemon gen-3 (Emerald/FR/LG) .sav with a new trainer identity.

Why: Union Room rosters with identical trainer name+TID are an untested edge case
(plan section 3.4); two-instance tests need distinct trainers on each side.

Usage:
  python3 clone_sav.py IN.sav OUT.sav --name "CLONEB" --tid 33333

Self-verifying: refuses to write unless every occupied section's existing checksum
validates under the assumed layout (i.e., our understanding of the format is
provably correct for THIS file), and re-validates the output before writing.

Layout (verified against pret/pokeemerald save.h/save.c and load-tested on the
supplied Emerald save):
  128 KiB file = 2 save slots x 14 sections x 4 KiB (+ Hall of Fame/extra after
  0x1C000, untouched). Section footer: u16 section_id @0xFF4, u16 checksum @0xFF6,
  u32 signature 0x08012025 @0xFF8, u32 save_index @0xFFC.
  Checksum: u32-wise sum over the section's DATA_SIZE bytes, folded
  ((sum>>16)+(sum&0xFFFF))&0xFFFF.
  Section 0 (Trainer Info): player name (8 bytes, gen-3 charset, 0xFF terminated)
  @0x00, trainer ID u32 @0x0A (low u16 = visible TID, high u16 = SID).
"""
import argparse
import struct
import sys

SIG = 0x08012025
SLOT_SECTIONS = 14
SECTION_SIZE = 0x1000
# Emerald per-section validated data sizes (pret: gSaveSectionDataSize)
DATA_SIZE = [3884, 3968, 3968, 3968, 3848, 3968, 3968,
             3968, 3968, 3968, 3968, 3968, 3968, 2000]

# Minimal gen-3 text encoding: A-Z, 0-9, space, terminator.
G3 = {**{chr(ord('A') + i): 0xBB + i for i in range(26)},
      **{chr(ord('0') + i): 0xA1 + i for i in range(10)},
      ' ': 0x00}


def encode_name(name):
    if not (1 <= len(name) <= 7):
        sys.exit("name must be 1-7 chars (A-Z, 0-9, space)")
    try:
        raw = bytes(G3[c] for c in name.upper())
    except KeyError as e:
        sys.exit(f"unsupported char in name: {e}")
    return raw + b'\xff' * (8 - len(raw))


def checksum(data):
    s = sum(struct.unpack_from(f'<{len(data)//4}I', data))
    return ((s >> 16) + (s & 0xFFFF)) & 0xFFFF


def sections(buf):
    """Yield (offset, section_id, stored_ck, live) for all 28 slot sections."""
    for i in range(2 * SLOT_SECTIONS):
        off = i * SECTION_SIZE
        sec_id, ck, sig, _idx = struct.unpack_from('<HHII', buf, off + 0xFF4)
        yield off, sec_id, ck, sig == SIG


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src'), ap.add_argument('dst')
    ap.add_argument('--name', required=True)
    ap.add_argument('--tid', type=int, required=True, help='visible TID 0-65535')
    ap.add_argument('--sid', type=int, default=None, help='secret ID (default: same as tid)')
    args = ap.parse_args()

    buf = bytearray(open(args.src, 'rb').read())
    if len(buf) < 2 * SLOT_SECTIONS * SECTION_SIZE:
        sys.exit(f"unexpected file size {len(buf)}")

    # Phase 1: prove the layout holds for every occupied section.
    occupied = [(o, sid_) for o, sid_, ck, live in sections(buf) if live]
    for off, sec_id in [(o, s) for o, s, ck, live in sections(buf) if live]:
        pass
    bad = []
    for off, sec_id, ck, live in sections(buf):
        if not live:
            continue
        if sec_id >= SLOT_SECTIONS:
            sys.exit(f"section id {sec_id} out of range at 0x{off:X}")
        calc = checksum(buf[off:off + DATA_SIZE[sec_id]])
        if calc != ck:
            bad.append((off, sec_id, ck, calc))
    if not occupied:
        sys.exit("no valid save sections found (empty/corrupt save?)")
    if bad:
        for off, sec_id, ck, calc in bad:
            print(f"  section {sec_id} @0x{off:X}: stored 0x{ck:04X} != calc 0x{calc:04X}")
        sys.exit("checksum model does not validate this file; refusing to edit")

    # Phase 2: rewrite trainer identity in every live section 0 (both slots).
    new_name = encode_name(args.name)
    sid = args.sid if args.sid is not None else args.tid
    tid_u32 = ((sid & 0xFFFF) << 16) | (args.tid & 0xFFFF)
    edited = 0
    for off, sec_id, ck, live in sections(buf):
        if not live or sec_id != 0:
            continue
        buf[off:off + 8] = new_name
        struct.pack_into('<I', buf, off + 0x0A, tid_u32)
        struct.pack_into('<H', buf, off + 0xFF6,
                         checksum(buf[off:off + DATA_SIZE[0]]))
        edited += 1

    # Phase 3: re-validate everything, then write.
    for off, sec_id, ck, live in sections(buf):
        if live and checksum(buf[off:off + DATA_SIZE[sec_id]]) != ck:
            sys.exit("post-edit validation failed; not writing output")
    open(args.dst, 'wb').write(buf)
    print(f"OK: wrote {args.dst}: trainer '{args.name.upper()}' "
          f"TID={args.tid} SID={sid} ({edited} trainer-info section(s) rewritten)")


if __name__ == '__main__':
    main()
