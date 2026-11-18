#!/usr/bin/env python3
"""prepare_gbalink_solo_rig.py -- the GBA link mirror's ONE-CONSOLE bench as
an unattended hw_loop rig (docs/GBA-LINK-MIRROR.md section 16): both AW2
machines on one console (gba_dual = 1), the player's machine driven by the
battle's host script, the partner's by its autopilot script, no radio.
It is the same two-machine computation a link session runs on each console,
so it profiles and A/Bs the CORE cost on one console -- the PSP-1000, which
paces the pair -- with a bit-exact golden (gba_dual_hash: both machines,
every frame, = the PPSSPP / PSP-3000 golden M-BATTLE140K).

    prepare_gbalink_solo_rig.py --bench DIR --golden-hash DIR --out RIG
        --role join --arm NAME=BUILDDIR[,KEY=VAL;KEY=VAL...] ...

  --bench        a local bench directory, not in the repo (battle_host.txt,
                 battle_join.txt, gba_bios.bin, roms/aw2.*)
  --golden-hash  the golden run's directory (gbadual_hash.txt), copied into
                 the rig for the scorer
  --role         the card's ROLE.TXT role (hw_loop prefixes: join- on the
                 PSP-1000 of the 3000+1000 pair)
Files are staged with the role prefix; the golden save/state are restored
into roms/ every cycle.  MANIFEST.txt: md5s and "# arm X <role> eboot crc32".
"""
import argparse
import hashlib
import os
import shutil
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from relabel_pbp import relabel  # noqa: E402

TITLE = "GBALINK rig"
COMMON = """rom = aw2.gba
load_state = 1
shash = 0
heartbeat_s = 5
me_mode = 1
log_input = 0
autoexit_frames = 17000
gba_dual = 1
gba_dual_autopilot = battle_join.txt
gba_dual_hash = 0
gba_dual_lookahead = 140448
script = battle_host.txt
stall_watch_s = 15
freeze_rig_recover = 1
handoff = 1
handoff_window_s = 90
handoff_max_runs = 100000
handoff_total_s = 900
handoff_park_s = 0
"""


def md5(p):
    h = hashlib.md5()
    with open(p, "rb") as fh:
        for b in iter(lambda: fh.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", required=True)
    ap.add_argument("--golden-hash", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--role", default="join", choices=("host", "join"))
    ap.add_argument("--arm", action="append", required=True)
    a = ap.parse_args()
    if os.path.exists(a.out):
        shutil.rmtree(a.out)
    os.makedirs(a.out)
    r = a.role
    man = ["GBA link one-console bench rig (prepare_gbalink_solo_rig.py), "
           "role %s, app folder PSP/GAME/GBADHOC-GBLINK" % r]
    for spec in a.arm:
        name, _, rest = spec.partition("=")
        parts = rest.split(",", 1)
        bd = parts[0]
        extra = ""
        if len(parts) == 2:
            extra = "".join("%s\n" % kv.strip().replace("=", " = ", 1)
                            for kv in parts[1].split(";") if kv.strip())
        own = {ln.split("=")[0].strip() for ln in extra.splitlines()}
        st = os.path.join(a.out, "stage-" + name)
        os.makedirs(os.path.join(st, "roms"))
        shutil.copy2(os.path.join(a.bench, "roms", "aw2.gba"),
                     os.path.join(st, "roms", "aw2.gba"))
        for f in ("gba_bios.bin", "battle_host.txt", "battle_join.txt"):
            shutil.copy2(os.path.join(a.bench, f), os.path.join(st, f))
        pbp = relabel(open(os.path.join(bd, "EBOOT.PBP"), "rb").read(), TITLE)
        with open(os.path.join(st, r + "-EBOOT.PBP"), "wb") as fh:
            fh.write(pbp)
        shutil.copy2(os.path.join(bd, "gbadhoc_me.prx"),
                     os.path.join(st, r + "-gbadhoc_me.prx"))
        with open(os.path.join(st, r + "-.gpsp-harness.ini"), "w",
                  newline="\n") as fh:
            fh.write("# GBA link one-console bench (claude/gbalink-hw), arm "
                     "%s: %s %s\n# run_id and arm are appended by hw_loop.\n"
                     % (name, os.path.basename(bd.rstrip("/\\")),
                        extra.strip().replace("\n", "; ") or "defaults"))
            fh.write("".join(ln + "\n" for ln in COMMON.splitlines()
                             if ln.split("=")[0].strip() not in own))
            fh.write(extra)
        man.append("# arm %s %s eboot crc32 %08x (relabelled from md5 %s)" % (
            name, r, zlib.crc32(pbp) & 0xFFFFFFFF,
            md5(os.path.join(bd, "EBOOT.PBP"))))
    g = os.path.join(a.out, "golden")
    os.makedirs(g)
    for ext in ("sav", "st0"):
        shutil.copy2(os.path.join(a.bench, "roms", "aw2." + ext),
                     os.path.join(g, "%s-aw2.%s" % (r, ext)))
    shutil.copy2(os.path.join(a.golden_hash, "gbadual_hash.txt"),
                 os.path.join(a.out, "golden-gbadual_hash.txt"))
    for cur, _d, files in os.walk(a.out):
        for n in sorted(files):
            p = os.path.join(cur, n)
            man.append("%s  %s" % (md5(p), os.path.relpath(p, a.out)))
    with open(os.path.join(a.out, "MANIFEST.txt"), "w", newline="\n") as fh:
        fh.write("\n".join(man) + "\n")
    print("\n".join(l for l in man if l.startswith("#") or "stage" not in l))
    return 0


if __name__ == "__main__":
    sys.exit(main())
