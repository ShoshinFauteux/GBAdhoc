#!/usr/bin/env python3
"""setup_rig_cards.py -- create PSP/GAME/GBADHOC-RIG on each memory stick.
RUN ONLY AFTER THE OWNER HANDSHAKE (docs/RIG-DOUBLE-BATTLE.md §5): this is the
first thing that WRITES to a stick.

    python tools/rig/setup_rig_cards.py --backup-verified <backup dir>
        --variant-dir <builds/wireless-rig/growl> --arm A
        host=F:=PSP-3000 join=D:=PSP-1000

What it writes, and nothing else, under PSP/GAME/GBADHOC-RIG/ on each card:
  EBOOT.PBP, gbadhoc_me.prx, the fixture, .gpsp-harness.ini, CONFIG.INI
  (the arm's stage dir, role-resolved), roms/<stem>.gba copied from the SAME
  card's GBADHOC/roms (md5 checked against the canonical rev-1 image),
  roms/<stem>.sav (the golden), gba_bios.bin (from the card's GBADHOC/),
  ROLE.TXT ("<role> <model>"), handoff/ and log/.
It refuses if the backup has no MANIFEST.verified, if a card already has a
GBADHOC-RIG whose ROLE.TXT names another role, or if a ROM md5 is wrong.
The owner's PSP/GAME/GBADHOC is only ever READ.
"""
import argparse
import hashlib
import os
import shutil
import sys

APP = "GBADHOC-RIG"
CANON = {"firered": ("Pokemon - FireRed Version (USA).gba", "51901a6e40661b3914aa333c802e24e8"),
         "leafgreen": ("Pokemon - LeafGreen Version (USA).gba", "9d33a02159e018d09073e700e1fd10fd")}


def md5(p):
    h = hashlib.md5()
    with open(p, "rb") as fh:
        for b in iter(lambda: fh.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--backup-verified", required=True)
    ap.add_argument("--variant-dir", required=True)
    ap.add_argument("--arm", default="A")
    ap.add_argument("cards", nargs=2, help="role=DRIVE:=MODEL")
    a = ap.parse_args()
    if not os.path.isfile(os.path.join(a.backup_verified, "MANIFEST.verified")):
        sys.exit("REFUSING: no verified backup at %s" % a.backup_verified)
    stage = os.path.join(a.variant_dir, "stage-" + a.arm)
    gold = os.path.join(a.variant_dir, "golden")
    for spec in a.cards:
        role, _, rest = spec.partition("=")
        drive, _, model = rest.partition("=")
        drive = drive.rstrip("\\/")
        owner = os.path.join(drive + os.sep, "PSP", "GAME", "GBADHOC")
        rig = os.path.join(drive + os.sep, "PSP", "GAME", APP)
        rolefile = os.path.join(rig, "ROLE.TXT")
        if os.path.isfile(rolefile) and not open(rolefile).read().startswith(role):
            sys.exit("REFUSING: %s already has %s for another role" % (drive, APP))
        golds = [g for g in os.listdir(gold) if g.startswith(role + "-")]
        if len(golds) != 1:
            sys.exit("REFUSING: need exactly one %s- golden save" % role)
        stem = golds[0][len(role) + 1:-4]
        name, want = CANON[stem]
        rom_src = os.path.join(owner, "roms", name)
        if md5(rom_src) != want:
            sys.exit("REFUSING: %s is not the canonical rev-1 image" % rom_src)
        for d in ("roms", "log", "handoff"):
            os.makedirs(os.path.join(rig, d), exist_ok=True)
        for f in os.listdir(stage):
            src = os.path.join(stage, f)
            if f.startswith(("host-", "join-")):
                if not f.startswith(role + "-"):
                    continue
                f = f[len(role) + 1:]
            shutil.copy2(src, os.path.join(rig, f))
        with open(os.path.join(rig, ".gpsp-harness.ini"), "a") as fh:
            fh.write("\nrun_id = setup\narm = %s\n" % a.arm)
        shutil.copy2(rom_src, os.path.join(rig, "roms", stem + ".gba"))
        shutil.copy2(os.path.join(gold, golds[0]), os.path.join(rig, "roms", stem + ".sav"))
        shutil.copy2(os.path.join(owner, "gba_bios.bin"), os.path.join(rig, "gba_bios.bin"))
        open(rolefile, "w").write("%s %s\n" % (role, model))
        print("%s: %s ready (role=%s model=%s rom=%s arm=%s)" % (drive, APP, role, model, stem, a.arm))
    return 0


if __name__ == "__main__":
    sys.exit(main())
