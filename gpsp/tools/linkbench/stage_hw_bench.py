#!/usr/bin/env python3
"""stage_hw_bench.py --eboot DIR --rom AW2.gba --state AW2.st0 --sav AW2.sav
                     --bios gba_bios.bin --script cold.txt --out OUTDIR

Stages the single-console AW2 solo-cost bench (docs/GBA-LINK-FEASIBILITY.md
section 3) as self-contained PSP app folders.  Nothing is written to any
Memory Stick; the lead copies the folders.  Each folder carries its own ROM,
save and state COPIES, so the owner's GBADHOC saves are never touched.

  GBADHOC-AW2BENCH      XMB "AW2 BENCH (ME)"   ME renderer on (the default)
  GBADHOC-AW2BENCH-CPU  XMB "AW2 BENCH (CPU)"  ME off: renderer on the CPU

Every run writes log/shash.txt (per frame: c= core microseconds + the
guest-state hashes) and log/frontend.log (psp_model, jit tier, rom cache,
core_prof every 5 s), then exits to the XMB by itself (~2 min).
"""
import argparse, hashlib, shutil, sys, zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from relabel_pbp import relabel  # noqa: E402

ARMS = [("GBADHOC-AW2BENCH", "AW2 BENCH (ME)", "me", ["me_mode = 1"]),
        ("GBADHOC-AW2BENCH-CPU", "AW2 BENCH (CPU)", "cpu", ["me_mode = 0"])]

def md5(p): return hashlib.md5(Path(p).read_bytes()).hexdigest()

def main():
    ap = argparse.ArgumentParser()
    for k in ("eboot", "rom", "state", "sav", "bios", "script", "out"):
        ap.add_argument("--" + k, required=True)
    o = ap.parse_args()
    out = Path(o.out); out.mkdir(parents=True, exist_ok=True)
    man = []
    for folder, title, arm, keys in ARMS:
        d = out / folder
        if d.exists(): shutil.rmtree(d)
        (d / "roms").mkdir(parents=True); (d / "log").mkdir()
        pbp = relabel(Path(o.eboot, "EBOOT.PBP").read_bytes(), title)
        (d / "EBOOT.PBP").write_bytes(pbp)
        shutil.copy(Path(o.eboot, "gbadhoc_me.prx"), d)
        shutil.copy(o.bios, d / "gba_bios.bin")
        shutil.copy(o.rom, d / "roms" / "aw2.gba")
        shutil.copy(o.state, d / "roms" / "aw2.st0")
        shutil.copy(o.sav, d / "roms" / "aw2.sav")
        shutil.copy(o.script, d / "script.txt")
        ini = ["rom = aw2.gba", "load_state = 1", "script = script.txt",
               "shash = 1", "audio_oracle = 1", "heartbeat_s = 5",
               "autoexit_frames = 7000", f"run_id = aw2bench", f"arm = {arm}"] + keys
        (d / ".gpsp-harness.ini").write_text("\n".join(ini) + "\n")
        for f in sorted(p for p in d.rglob("*") if p.is_file()):
            man.append(f"{md5(f)}  {f.relative_to(out).as_posix()}")
        man.append(f"# {folder} EBOOT crc32 {zlib.crc32(pbp) & 0xffffffff:08x}")
    (out / "MANIFEST.txt").write_text("\n".join(man) + "\n")
    print("\n".join(man))

if __name__ == "__main__":
    main()
