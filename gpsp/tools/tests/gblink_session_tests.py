#!/usr/bin/env python3
"""gblink_session_tests.py BIN_DIR -- link-session protocol tests that need
no commercial cartridge (run_gb_tests.py runs them as part of "gb").

Two generated cartridges (a DMG and a CGB program that keep WRAM, cartridge
RAM and the sound registers busy and read the joypad), each console a full
fe_gblink stack (sesssim), a simulated network with latency and jitter:

  run        both consoles end DONE, every periodic hash matched, both saves
             committed and each console's equal to the other's copy
  transfer   different cartridges: each is sent in memory, SHA-1 checked
  corrupt    one ROM chunk damaged in flight: the receiver refuses the
             cartridge (SHA-1), the session fails, NOTHING is written
  desync     one byte of one console's copy flipped (HRAM, which the
             program never rewrites, so the difference persists): detected
             at the next hash, both fail, nothing is written
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

LOGO = bytes([
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
    0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
    0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E])


def rom(color: bool, seed: int, key: int, size: int) -> bytes:
    """gbcore/tests/dual.c's busy program, as a file."""
    r = bytearray(size)
    r[0x40:0x48] = bytes([0xF5, 0xF0, 0x04, 0xEA, 0x00, 0x98, 0xF1, 0xD9])
    r[0x50:0x5A] = bytes([0xF5, 0xFA, 0x02, 0xA0, 0x3C, 0xEA, 0x02, 0xA0, 0xF1, 0xD9])
    r[0x100:0x103] = bytes([0xC3, 0x50, 0x01])
    r[0x104:0x134] = LOGO
    r[0x134:0x13D] = b'LINK TEST'
    r[0x13D] = seed
    r[0x143] = 0x80 if color else 0
    r[0x147] = 0x03
    r[0x148] = {0x8000: 0, 0x10000: 1, 0x20000: 2}[size]
    r[0x149] = 0x02
    c = 0
    for i in range(0x134, 0x14D):
        c = (c - r[i] - 1) & 0xFF
    r[0x14D] = c
    prog = [0x31, 0xFE, 0xFF, 0x3E, 0x0A, 0xEA, 0x00, 0x00, 0x3E, 0x80, 0xE0,
            0x26, 0x3E, 0x77, 0xE0, 0x24, 0x3E, 0xFF, 0xE0, 0x25, 0x3E, 0x05,
            0xE0, 0x07, 0x3E, 0x05, 0xE0, 0xFF, 0xFB, 0x21, 0x00, 0xC0, 0x11,
            seed, (seed * 7 + 1) & 0xFF, 0x3E, 0x10, 0xE0, 0x00, 0xF0, 0x00,
            0x83, 0x07, 0xEE, key, 0x5F, 0x22, 0xE0, 0x13, 0xEA, 0x00, 0xA0,
            0x7C, 0xFE, 0xE0, 0x20, 0xEA, 0x26, 0xC0, 0x3E, 0xF3, 0xE0, 0x12,
            0x3E, 0x87, 0xE0, 0x14, 0xFA, 0x01, 0xA0, 0x3C, 0xEA, 0x01, 0xA0,
            0x18, 0xD7]
    r[0x150:0x150 + len(prog)] = bytes(prog)
    return bytes(r)


def run(sim: Path, t: Path, tag: str, *args: str) -> tuple[int, str]:
    r = subprocess.run([str(sim), *args, "out_h=%s/%s-h.sav" % (t, tag),
                        "out_g=%s/%s-g.sav" % (t, tag)],
                       capture_output=True, text=True, timeout=600)
    return r.returncode, r.stdout


def check(ok: bool, what: str, out: str) -> None:
    if not ok:
        print(out[-3000:])
        raise SystemExit("FAIL gblink session: " + what)


def main() -> int:
    sim = Path(sys.argv[1]) / "sesssim"
    with tempfile.TemporaryDirectory(prefix="gblink-") as tmp:
        t = Path(tmp)
        (t / "dmg.gb").write_bytes(rom(False, 0x21, 0x5A, 0x8000))
        (t / "cgb.gbc").write_bytes(rom(True, 0x93, 0xC3, 0x20000))
        # a battery image each (cartridge RAM only)
        (t / "a.sav").write_bytes(bytes(range(256)) * 32)
        (t / "b.sav").write_bytes(bytes(255 - (i & 255) for i in range(8192)))
        common = ["sav_h=%s/a.sav" % t, "sav_g=%s/b.sav" % t, "delay=3",
                  "quit_after=900", "max=20000"]

        rc, out = run(sim, t, "run", "rom_h=%s/dmg.gb" % t,
                      "rom_g=%s/dmg.gb" % t, "lat=2", "jitter=5", *common)
        check(rc == 0 and out.count("gblink_done") == 2 and
              (t / "run-h.sav").exists() and (t / "run-g.sav").exists(),
              "a plain session did not end DONE on both consoles", out)
        print("gblink session: run ok (%s)" % [l for l in out.splitlines()
              if "gblink_done" in l][0].split(" console=")[0][4:])

        rc, out = run(sim, t, "xfer", "rom_h=%s/cgb.gbc" % t,
                      "rom_g=%s/dmg.gb" % t, "lat=3", "jitter=3", *common)
        check(rc == 0 and out.count("gblink_rom_received") == 2 and
              "ok=1" in out and out.count("gblink_done") == 2,
              "cartridge transfer session failed", out)
        print("gblink session: both cartridges transferred, SHA-1 checked, DONE")

        # Scanline batching decided by the host (CONFIG) for both consoles.
        rc, out = run(sim, t, "batch", "rom_h=%s/dmg.gb" % t,
                      "rom_g=%s/dmg.gb" % t, "lat=2", "jitter=3", "batch=154",
                      *common)
        check(rc == 0 and out.count("gblink_done") == 2 and
              out.count("batch=154") >= 2,
              "a batched session did not end DONE on both consoles", out)
        print("gblink session: host-chosen scanline batching (154) on both "
              "consoles: DONE, hashes matched")

        # The bulk lane (large unreliable datagrams, selective resends):
        # the same session over a lossy, reordering network, one datagram
        # damaged on the way.  (Saves are not compared with the ordered
        # run's: this cartridge writes its RAM every frame, so the end frame,
        # which follows the network's timing, changes them.)
        rc, out = run(sim, t, "bulk", "rom_h=%s/cgb.gbc" % t,
                      "rom_g=%s/dmg.gb" % t, "lat=3", "jitter=3",
                      "bulk=1037", "bulk_loss=8", "corrupt_bulk=30", *common)
        bad = [l for l in out.splitlines() if "gblink_rom_received" in l]
        check(rc == 0 and len(bad) == 2 and all("ok=1" in l for l in bad) and
              all("chunk=1024" in l for l in bad) and
              "bulk_datagram_corrupted" in out and
              any(" bad=" in l and " bad=0" not in l for l in bad +
                  [l for l in out.splitlines() if "result console" in l]) and
              out.count("gblink_done") == 2 and
              (t / "bulk-h.sav").exists() and (t / "bulk-g.sav").exists(),
              "bulk-lane transfer (8% loss, a damaged datagram) failed", out)
        print("gblink session: bulk lane, 8% loss + reordering + a damaged "
              "datagram (CRC refused, resent): SHA-1 checked, DONE")

        rc, out = run(sim, t, "bulkcap", "rom_h=%s/cgb.gbc" % t,
                      "rom_g=%s/dmg.gb" % t, "lat=2", "bulk=1037",
                      "bulk_cap=1", *common)
        check(rc == 0 and out.count("gblink_done") == 2,
              "bulk lane over a network that carries 1 datagram a frame", out)
        print("gblink session: bulk lane throttled to 1 datagram/frame by the "
              "network: rate adapted, DONE")

        # A LIVE link: each console links from its running game (a save
        # state), and the session equals the one-process pair started from
        # the same states, hash for hash.
        lp = Path(sys.argv[1]) / "linkplay"
        subprocess.run([str(lp), "rom0=%s/dmg.gb" % t, "rom1=%s/cgb.gbc" % t,
                        "sav0=%s/a.sav" % t, "sav1=%s/b.sav" % t, "link=0",
                        "max=250", "stout0=%s/a.st" % t, "stout1=%s/b.st" % t],
                       capture_output=True, text=True, check=True)
        rc, out = run(sim, t, "live", "rom_h=%s/dmg.gb" % t,
                      "rom_g=%s/cgb.gbc" % t, "st_h=%s/a.st" % t,
                      "st_g=%s/b.st" % t, "lat=2", "jitter=3", "bulk=1037",
                      "hashlog=%s/live.hashes" % t, *common)
        check(rc == 0 and out.count("gblink_done") == 2 and
              "own_state=0" not in out,
              "a live-link session did not end DONE on both consoles", out)
        r = subprocess.run([str(lp), "rom0=%s/dmg.gb" % t, "rom1=%s/cgb.gbc" % t,
                            "sav0=%s/a.sav" % t, "sav1=%s/b.sav" % t,
                            "st0=%s/a.st" % t, "st1=%s/b.st" % t, "delay=3",
                            "rtc=1790500000", "max=1200",
                            "hashlog=%s/ref.hashes" % t],
                           capture_output=True, text=True)
        live = dict(l.split() for l in open(t / "live.hashes"))
        ref = dict(l.split() for l in open(t / "ref.hashes"))
        both = [k for k in live if k in ref]
        check(len(both) >= 10 and all(live[k] == ref[k] for k in both),
              "live link differs from the one-process pair from the same "
              "states (%d frames compared)" % len(both), out)
        print("gblink session: live link from save states == one-process "
              "pair from the same states (%d sync hashes)" % len(both))

        # Redundant inputs: an ordered channel that delivers 5 % of its
        # messages 6 frames late stalls the game; with each input repeated in
        # the next 8 unreliable datagrams (which lose 5 % too) it almost never
        # does, and the machines run identically (sync hashes compared frame
        # by frame: the end frame itself follows wall time).
        res = {}
        for copies in (0, 8):
            rc, out = run(sim, t, "red%d" % copies, "rom_h=%s/dmg.gb" % t,
                          "rom_g=%s/dmg.gb" % t, "lat=1", "bulk=1037",
                          "bulk_loss=5", "arq_loss=5", "arq_rto=6",
                          "input_copies=%d" % copies,
                          "hashlog=%s/red%d.hashes" % (t, copies), *common)
            check(rc == 0 and out.count("gblink_done") == 2,
                  "redundancy run (copies %d) failed" % copies, out)
            res[copies] = (
                [int(x) for x in re.findall(r" stalls=(\d+) streak_max", out)
                 [-2:]],
                dict(l.split() for l in open(t / ("red%d.hashes" % copies))))
        both = [k for k in res[0][1] if k in res[8][1]]
        check(min(res[0][0]) > 0 and 20 * max(res[8][0]) <= min(res[0][0]) and
              len(both) >= 10 and all(res[0][1][k] == res[8][1][k]
                                      for k in both),
              "redundant inputs: stalls %s without, %s with" %
              (res[0][0], res[8][0]), out)
        print("gblink session: lossy ordered channel -- stalls %s without "
              "redundant inputs, %s with; %d sync hashes identical" %
              (res[0][0], res[8][0], len(both)))

        rc, out = run(sim, t, "corrupt", "rom_h=%s/cgb.gbc" % t,
                      "rom_g=%s/dmg.gb" % t, "lat=2", "corrupt_rom=40", *common)
        check(rc != 0 and "rom_hash_mismatch" in out and
              not (t / "corrupt-h.sav").exists() and
              not (t / "corrupt-g.sav").exists(),
              "a damaged cartridge was not refused", out)
        print("gblink session: damaged cartridge refused by SHA-1, nothing written")

        rc, out = run(sim, t, "desync", "rom_h=%s/dmg.gb" % t,
                      "rom_g=%s/dmg.gb" % t, "lat=2", "desync_at=300",
                      "desync_addr=0xFF90", *common)
        check(rc != 0 and "gblink_desync" in out and
              not (t / "desync-h.sav").exists() and
              not (t / "desync-g.sav").exists(),
              "an injected desync was not caught", out)
        print("gblink session: injected desync caught (%s), nothing written" %
              [l for l in out.splitlines() if "gblink_desync" in l][0]
              .split(" own=")[0][4:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
