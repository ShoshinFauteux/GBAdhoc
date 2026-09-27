#!/usr/bin/env python3
"""backup_sticks.py -- copy every save, state and config off the PSP memory
sticks BEFORE the rig touches them, and prove the copy.

    python tools/rig/backup_sticks.py --out <dir> D:=PSP-1000 F:=PSP-3000

READ-ONLY on the sticks.  For every PSP/GAME/<app>/ it copies:
    roms/*.sav, roms/*.sav.bak, roms/*.st<N>, roms/*.srm
    CONFIG.INI / config.ini, .gpsp-harness.ini, variant.ini
into <out>/<letter>-<label>/<app>/..., then RE-READS both the stick file and
the copy and writes MANIFEST.txt (md5, size, source path) plus
MANIFEST.verified once every entry matches.  Exit 0 only when every file
verified; anything else is a loud nonzero exit and no .verified marker.

The label (PSP-1000 / PSP-3000) is what the operator SAYS the stick is; the
stick itself carries no model.  The rig's own run log records the console's
model (psp_model line), which is how roles are verified by content later.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time

KEEP_EXT = (".sav", ".sav.bak", ".srm")
KEEP_NAMES = ("config.ini", ".gpsp-harness.ini", "variant.ini")


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for blk in iter(lambda: fh.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def wanted(name):
    low = name.lower()
    if low in KEEP_NAMES:
        return True
    if low.endswith(KEEP_EXT):
        return True
    # savestates: .st0 .. .st9 (and .st for older builds)
    base, dot, ext = low.rpartition(".")
    return bool(dot) and (ext == "st" or (len(ext) == 3 and ext[:2] == "st"
                                           and ext[2].isdigit()))


def volume_serial(letter):
    try:
        out = subprocess.run(["cmd", "/c", "vol", letter], capture_output=True,
                             text=True, timeout=20).stdout
        for line in out.splitlines():
            if "Serial Number" in line:
                return line.split()[-1]
    except Exception:
        pass
    return "unknown"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("sticks", nargs="+", help="LETTER:=LABEL, e.g. D:=PSP-1000")
    args = ap.parse_args()

    rows, bad = [], 0
    for spec in args.sticks:
        drive, _, label = spec.partition("=")
        drive = drive.rstrip("\\/")
        games = os.path.join(drive + os.sep, "PSP", "GAME")
        if not os.path.isdir(games):
            print("MISSING: %s has no PSP/GAME" % drive)
            return 2
        dest_root = os.path.join(args.out, "%s-%s" % (drive[0], label or "unlabelled"))
        serial = volume_serial(drive)
        for app in sorted(os.listdir(games)):
            app_dir = os.path.join(games, app)
            if not os.path.isdir(app_dir):
                continue
            for cur, _dirs, files in os.walk(app_dir):
                # only the app root and its roms/ -- never walk a whole install
                rel = os.path.relpath(cur, app_dir)
                if rel not in (".", "roms"):
                    continue
                for name in sorted(files):
                    if not wanted(name):
                        continue
                    src = os.path.join(cur, name)
                    dst = os.path.join(dest_root, app, "" if rel == "." else rel, name)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copy2(src, dst)
                    a, b = md5(src), md5(dst)
                    ok = (a == b and os.path.getsize(src) == os.path.getsize(dst))
                    bad += 0 if ok else 1
                    rows.append("%s  %9d  %s  %s  serial=%s%s" % (
                        b, os.path.getsize(dst), src,
                        os.path.relpath(dst, args.out), serial,
                        "" if ok else "  MISMATCH"))
    man = os.path.join(args.out, "MANIFEST.txt")
    os.makedirs(args.out, exist_ok=True)
    with open(man, "w", encoding="utf-8") as fh:
        fh.write("# backup_sticks.py %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
        fh.write("# md5  size  source  copy  volume\n")
        fh.write("\n".join(rows) + "\n")
    print("%d files, %d mismatches -> %s" % (len(rows), bad, man))
    if bad or not rows:
        return 1
    with open(os.path.join(args.out, "MANIFEST.verified"), "w") as fh:
        fh.write("%d files verified (source re-read == copy) %s\n"
                 % (len(rows), time.strftime("%Y-%m-%d %H:%M:%S")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
