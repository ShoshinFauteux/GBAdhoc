#!/usr/bin/env python3
"""make_rc_stages.py -- hw_loop arm directories for the 3.5.0 RC's
"Quit to game list" realism arms (one console each, no radio), on top of the
in-process soak's (make_stages.py, docs/INPROC-SOAK.md).

    python make_rc_stages.py --out <dir> --fx <fixtures> --builds <dir> \
        [--backup1000 <fresh scoped backup> --backup3000 <fresh scoped backup>]

<builds> holds harness/ (PSP-1000, 32 MiB) and harness64/ (PSP-3000/Go), each
with EBOOT.PBP, gbadhoc_me.prx, gbadhoc_home.prx.  <fx> holds the soak's
fixtures (make_stages.GAMES) plus aw2_psp_script.txt, battle.txt,
emerald_save.inputs and emerald_shiny.sav (testdata's "Pokemon Emerald All
Shiny Fixed.sav", the save emerald_save.inputs was written for).  Z (restore the rig) is written only when both backups
are given; they must be FRESH (freezerig backup --scope ... taken just before
the run), never an older card image.

Every soak file lives in roms/soak/ (s_ names), as in make_stages.py.

Arms (each one hw_loop run; the second game of each run boots in-process, so
its census and its first 300 frames are the check):
  I   install: the RC EBOOT + prx + soak ROMs, then 20 returns (S1's order).
  L1  long play: the AW2 scene tour (aw2_psp_script, quit at frame 5900, ~1.6 min)
      then the harness quit -> Emerald from its state, 300 frames.
  L2  long play: the Heart & Soul heavy battle (battle.txt, 3200 frames;
      PAGED on the PSP-1000) then quit -> Red.
  L3  battery save: Emerald boots from its .sav, the in-game SAVE writes the
      flash (emerald_save.inputs; the write lands at frame 629), quit at
      frame 640 -> Crystal (the script itself ends the run at 1180).
  F   fast-forward held (ff = 1) through 20 returns: every quit lands mid-FF.
  SQ  a state save on frame 299, the quit on frame 300 (20 returns).
  GA  GB -> GBA -> GB -> GBA alternation, 40 returns.
  SS  the Save States page: slot 1 saved at frame 100 (state + .thumb +
      .shot), the page opened, its .shot read by the dwell, back to the menu,
      the menu's own Quit to game list (ui_script) -> Emerald.
  ME  the Media Engine renderer busy at every quit: 100 returns at an odd
      frame count (157) with me_mode = 1, so the quit lands at varying points
      of the ME's frame (PPSSPP cannot run the ME at all).
  Z   (with --backup*) the rig's pre-suite files back; staged with STOP.
"""
import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_stages as ms  # noqa: E402

ORDER = ["s_aw2.gba", "s_red.gb", "s_emerald.gba", "s_crystal.gbc", "s_hns.gba"]


def rl(order):
    return "inproc_roms = %s\n" % "|".join("soak/" + g for g in order)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--fx", required=True)
    ap.add_argument("--builds", required=True)
    ap.add_argument("--backup1000")
    ap.add_argument("--backup3000")
    a = ap.parse_args()
    for model, role, build, backup in (("1000", "join", "harness", a.backup1000),
                                       ("3000", "host", "harness64", a.backup3000)):
        root = os.path.join(a.out, model)
        if os.path.isdir(root):
            shutil.rmtree(root)
        bd = os.path.join(a.builds, build)

        def arm(name, desc, keys, extra=None, files=None):
            d = os.path.join(root, name)
            ms.arm(d, role, model, name, "RC 3.5.0 quit realism: " + desc, keys,
                   a.fx, extra)
            for src, dst in (files or {}).items():
                ms.cp(src, os.path.join(d, dst))
            return d

        d = arm("I", "install + 20 returns aw2>red>emerald>crystal>hns",
                rl(ORDER) + "inproc_cycles = 20\ninproc_frames = 300\n"
                "autoexit_frames = 301\nload_state = 1\n")
        for f in ("EBOOT.PBP", "gbadhoc_me.prx", "gbadhoc_home.prx"):
            ms.cp(os.path.join(bd, f), os.path.join(d, f))
        for soakname, rom, _s, _t in ms.GAMES:
            ms.cp(os.path.join(a.fx, rom), os.path.join(d, "roms", "soak", soakname))
        arm("L1", "AW2 tour to frame 5900, quit -> emerald",
            rl(["s_aw2.gba", "s_emerald.gba"]) + "inproc_cycles = 1\n"
            "inproc_frames = 5900\nautoexit_frames = 6100\nload_state = 1\n"
            "script = soak_aw2_tour.txt\n",
            files={os.path.join(a.fx, "aw2_psp_script.txt"): "soak_aw2_tour.txt"})
        arm("L2", "H&S heavy battle 3200 frames, quit -> red",
            rl(["s_hns.gba", "s_red.gb"]) + "inproc_cycles = 1\n"
            "inproc_frames = 3200\nautoexit_frames = 3201\nload_state = 1\n"
            "script = soak_hns_battle.txt\n",
            files={os.path.join(a.fx, "battle.txt"): "soak_hns_battle.txt"})
        arm("L3", "Emerald in-game SAVE (flash write at f629), quit at 640 -> crystal",
            rl(["s_emerald.gba", "s_crystal.gbc"]) + "inproc_cycles = 1\n"
            "inproc_frames = 640\nautoexit_frames = 4001\n"
            "script = soak_em_save.txt\n",
            files={os.path.join(a.fx, "emerald_save.inputs"): "soak_em_save.txt",
                   # the save the script was written for (testdata "All Shiny
                   # Fixed"): it stands in the overworld; the soak's 3000 save
                   # does not open the start menu at the script's step 13
                   os.path.join(a.fx, "emerald_shiny.sav"):
                       os.path.join("roms", "soak", "s_emerald.sav")})
        arm("F", "ff = 1 through 20 returns",
            rl(["s_aw2.gba", "s_crystal.gbc", "s_hns.gba", "s_red.gb"]) +
            "inproc_cycles = 20\ninproc_frames = 600\nautoexit_frames = 601\n"
            "load_state = 1\nff = 1\n")
        arm("SQ", "state save at 299, quit at 300, 20 returns",
            rl(["s_aw2.gba", "s_red.gb", "s_emerald.gba", "s_crystal.gbc"]) +
            "inproc_cycles = 20\ninproc_frames = 300\nautoexit_frames = 301\n"
            "load_state = 1\nstate_save_at = 299\n")
        arm("GA", "GB/GBA alternation, 40 returns",
            rl(["s_red.gb", "s_aw2.gba", "s_crystal.gbc", "s_emerald.gba"]) +
            "inproc_cycles = 40\ninproc_frames = 300\nautoexit_frames = 301\n"
            "load_state = 1\n")
        arm("SS", "Save States page .shot read, menu quit -> emerald",
            rl(["s_aw2.gba", "s_emerald.gba"]) + "inproc_cycles = 1\n"
            "inproc_frames = 99999\nautoexit_frames = 600\nload_state = 1\n"
            "state_save_at = 100\nui_script = soak_ss_quit.ui\n",
            extra={"soak_ss_quit.ui": SS_UI})
        arm("ME", "ME busy at every quit: 100 returns at 157 frames",
            rl(ORDER) + "inproc_cycles = 100\ninproc_frames = 157\n"
            "autoexit_frames = 158\nload_state = 1\n")
        if backup:
            z = os.path.join(root, "Z")
            app = os.path.join(backup, "files", "PSP", "GAME", "GBADHOC-GBLINK")
            for f in ("EBOOT.PBP", "gbadhoc_me.prx", "CONFIG.INI"):
                ms.cp(os.path.join(app, f), os.path.join(z, f))
            with open(os.path.join(app, ".gpsp-harness.ini"), encoding="utf-8") as fh:
                lines = fh.read().splitlines()
            keep = [l for l in lines if not l.startswith(("run_id =", "arm ="))]
            while keep and not keep[-1].strip():
                keep.pop()
            ms.wr(os.path.join(z, role + "-.gpsp-harness.ini"), "\n".join(keep) + "\n")
            g = os.path.join(root, "golden")
            os.makedirs(g, exist_ok=True)
            for f in sorted(os.listdir(os.path.join(app, "roms"))):
                if f.endswith((".sav", ".sav.bak")):
                    ms.cp(os.path.join(app, "roms", f), os.path.join(g, role + "-" + f))
        print("%s: %s" % (model, ", ".join(sorted(os.listdir(root)))))


# The menu's rows since 3.5: Resume, Save states, Wireless, Settings,
# Quit to game list, Exit.  Leaving the Save States page puts the cursor on
# Resume; UP wraps to Exit, and UP again is Quit to game list.
SS_UI = """# Save States page: slot 1 (saved at frame 100, with .shot), dwell on it,
# back out, Quit to game list from the menu (RC 3.5.0 quit realism).
+200 menu
+10 press DOWN
+4 press CROSS
+40 press DOWN
+30 press UP
+40 press CIRCLE
+10 press UP
+4 press UP
+6 press CROSS
"""

if __name__ == "__main__":
    main()
