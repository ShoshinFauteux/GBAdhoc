#!/usr/bin/env python3
"""prepare_gbalink_hw_rig.py -- stage the GBA link HARDWARE pair rig
(docs/GBA-LINK-MIRROR.md section 15): PSP-3000 = HOST, PSP-1000 = JOIN, one
rig app per console (PSP/GAME/GBADHOC-GBLINK, hw_loop.py's USB handoff).

    prepare_gbalink_hw_rig.py --host-build DIR --join-build DIR
        --fake-stage DIR --golden DIR --rom FILE --bios FILE --out RIG

  --host-build   GBA_LINK=1 tools/build.sh harness64 output (EBOOT.PBP,
                 gbadhoc_me.prx): the 3000's binary
  --join-build   GBA_LINK=1 tools/build.sh harness output: the 1000's
                 (32 MiB layout; MEMSIZE does nothing on a 1000 anyway)
  --fake-stage   the AW2 link-cable FAKE baseline's stage
                 (a local stage directory, not in the repo): one core,
                 upstream's AW serial fake over RFU-style netplay
  --golden       host-/join-aw2.sav + .st0 (restored into roms/ every cycle)
  --arm NAME=HOSTDIR,JOINDIR[,KEY=VAL;KEY=VAL...]   (repeatable) replaces the
                 default B/A arms with your own: each a mirror arm with its
                 own pair of builds and extra harness keys (hw2 onward:
                 profiling and one-change-at-a-time A/Bs)
  --no-fake      leave out arm F
  --game-arm NAME=HOSTDIR,JOINDIR,ROM,STATE,HOSTSCRIPT,JOINSCRIPT[,KEY=VAL;...]
                 (repeatable) a mirror arm on another game: ROM and STATE
                 (the fixture's .st0, staged as roms/<rom base>.st0 every
                 cycle, so no golden is needed) and each role's autopilot
                 script (staged as link.txt); the AW2 cart, golden and
                 battle scripts still ride along unused.  E.g. the multigame
                 rig's Super Circuit race (fixtures/msc.fixture: no battery,
                 gblink_end_after = 1500, autoexit_frames = 18000).

Arms (each a stage-<arm>/ directory hw_loop mirrors onto both cards; files
prefixed host-/join- go to one card only; roms/aw2.gba and gba_bios.bin are
staged in every arm so the cards need no separate setup):

  B  mirror session, AFTER binary with every smoothness change OFF
     (gblink_absorb / gblink_nack / gblink_audio_cushion / _follow / _soft
     = 0, gblink_hash_every = 60): the claude/gbalink-session behaviour with
     the new telemetry (EVT gblink_win: stalls, late frames, audio
     starvation, hash cost, pg= ROM page loads)
  A  mirror session, AFTER binary, defaults (absorb, nack, audio cushion /
     fade / follow, hash every 240)
  F  the AW2 link-cable fake (baseline), as built: the bar to compare with

Every arm plays the same RAM-gated 10-round AW2 battle (the baseline's
scripts, md5-identical), delay 4, no per-frame hash (it costs ~10 ms a
frame on a PSP and would swamp the comparison).  All three EBOOTs are
relabelled "GBALINK rig" so the XMB names the rig, never the owner's build.
MANIFEST.txt lists every staged file's md5 and each arm's per-role EBOOT
crc32 (score_gbalink_hw.py checks the logs against it).
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
APPDIR = "GBADHOC-GBLINK"

COMMON = """rom = aw2.gba
script = battle.txt
load_state = 1
shash = 1
heartbeat_s = 5
me_mode = 1
log_input = 0
gba_dual = 1
gblink_role = {role}
group = GBAHW1
nick = {role}
gblink_delay = 4
gblink_end_after = 120
gblink_win_ms = 10000
autoexit_frames = 30000
stall_watch_s = 15
freeze_rig_recover = 1
handoff = 1
handoff_window_s = 90
handoff_max_runs = 100000
handoff_total_s = 900
handoff_park_s = 0
"""
OFF = """gblink_absorb = 0
gblink_nack = 0
gblink_audio_cushion = 0
gblink_audio_follow = 0
gblink_audio_soft = 0
gblink_hash_every = 60
"""
ARMS = {
    "B": ("mirror, AFTER binary, every smoothness change OFF (= before)", OFF),
    "A": ("mirror, AFTER binary, defaults (the ported fixes)", ""),
}


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for blk in iter(lambda: fh.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def crc32(data):
    return "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)


def fake(a, st, out, manifest, common_files):

    shutil.copytree(a.fake_stage, st)
    common_files(st)
    raw = open(os.path.join(st, "EBOOT.PBP"), "rb").read()
    src = md5(os.path.join(st, "EBOOT.PBP"))
    pbp = relabel(raw, TITLE)
    with open(os.path.join(st, "EBOOT.PBP"), "wb") as fh:
        fh.write(pbp)
    for role in ("host", "join"):
        manifest.append("# arm F %s eboot crc32 %s (relabelled from md5 %s)"
                        % (role, crc32(pbp), src))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host-build")
    ap.add_argument("--join-build")
    ap.add_argument("--fake-stage", required=True,
                    help="the AW fake stage (its battle scripts and "
                         "CONFIG.INI are used by every arm)")
    ap.add_argument("--golden", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--bios", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--arm", action="append", default=[])
    ap.add_argument("--no-fake", action="store_true")
    ap.add_argument("--game-arm", action="append", default=[])
    a = ap.parse_args()
    custom = []
    games = {}
    for spec in a.game_arm:
        name, _, rest = spec.partition("=")
        parts = rest.split(",", 6)
        if len(parts) < 6 or not name:
            ap.error("bad --game-arm %r" % spec)
        keys = parts[6] if len(parts) == 7 else ""
        games[name] = dict(rom=parts[2], state=parts[3], scripts={
            "host": parts[4], "join": parts[5]})
        base = os.path.basename(parts[2])
        a.arm.append("%s=%s,%s,rom=%s;script=link.txt%s" % (
            name, parts[0], parts[1], base, (";" + keys) if keys else ""))
    for spec in a.arm:
        name, _, rest = spec.partition("=")
        parts = rest.split(",", 2)
        if len(parts) < 2 or not name:
            ap.error("bad --arm %r" % spec)
        extra = ""
        if len(parts) == 3 and parts[2]:
            extra = "".join("%s\n" % kv.strip().replace("=", " = ", 1)
                            for kv in parts[2].split(";") if kv.strip())
        custom.append((name, parts[0], parts[1], extra))

    out = a.out
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    manifest = ["GBA link hardware pair rig (tools/gbamirror/"
                "prepare_gbalink_hw_rig.py)",
                "host = PSP-3000 (harness64), join = PSP-1000 (harness); "
                "app folder PSP/GAME/%s" % APPDIR]
    def load(d):
        raw = open(os.path.join(d, "EBOOT.PBP"), "rb").read()
        return (relabel(raw, TITLE), os.path.join(d, "gbadhoc_me.prx"),
                md5(os.path.join(d, "EBOOT.PBP")))
    arms = []
    if custom:
        for name, hd, jd, extra in custom:
            arms.append((name, "mirror %s (%s / %s) %s" % (
                name, os.path.basename(hd.rstrip("/\\")),
                os.path.basename(jd.rstrip("/\\")),
                extra.strip().replace("\n", "; ") or "defaults"),
                extra, {"host": load(hd), "join": load(jd)}))
    else:
        both = {"host": load(a.host_build), "join": load(a.join_build)}
        for arm, (why, extra) in ARMS.items():
            arms.append((arm, why, extra, both))
    scripts = {r: os.path.join(a.fake_stage, "%s-battle.txt" % r)
               for r in ("host", "join")}

    def common_files(st):
        os.makedirs(os.path.join(st, "roms"))
        shutil.copy2(a.rom, os.path.join(st, "roms", "aw2.gba"))
        shutil.copy2(a.bios, os.path.join(st, "gba_bios.bin"))

    for arm, why, extra, eb in arms:
        st = os.path.join(out, "stage-" + arm)
        os.makedirs(st)
        common_files(st)
        shutil.copy2(os.path.join(a.fake_stage, "CONFIG.INI"),
                     os.path.join(st, "CONFIG.INI"))
        g = games.get(arm)
        if g:
            rb = os.path.basename(g["rom"])
            shutil.copy2(g["rom"], os.path.join(st, "roms", rb))
            shutil.copy2(g["state"], os.path.join(
                st, "roms", os.path.splitext(rb)[0] + ".st0"))
            for role in ("host", "join"):
                shutil.copy2(g["scripts"][role],
                             os.path.join(st, role + "-link.txt"))
        for role in ("host", "join"):
            pbp, prx, src_md5 = eb[role]
            with open(os.path.join(st, role + "-EBOOT.PBP"), "wb") as fh:
                fh.write(pbp)
            shutil.copy2(prx, os.path.join(st, role + "-gbadhoc_me.prx"))
            shutil.copy2(scripts[role],
                         os.path.join(st, role + "-battle.txt"))
            with open(os.path.join(st, role + "-.gpsp-harness.ini"), "w",
                      newline="\n") as fh:
                fh.write("# GBA link HW rig (claude/gbalink-hw), %s, arm %s: "
                         "%s.\n# run_id and arm are appended by hw_loop.\n"
                         % (role, arm, why))
                # an ini key read twice keeps the FIRST value: an arm's own
                # key replaces the common line instead of following it
                own = {ln.split("=")[0].strip() for ln in extra.splitlines()
                       if "=" in ln}
                fh.write("".join(
                    ln + "\n" for ln in COMMON.format(role=role).splitlines()
                    if ln.split("=")[0].strip() not in own))
                fh.write(extra)
            manifest.append("# arm %s %s eboot crc32 %s (relabelled from md5 "
                            "%s)" % (arm, role, crc32(pbp), src_md5))
    # F: the fake baseline, as staged by its own tool, plus the cart + BIOS
    st = os.path.join(out, "stage-F")
    if a.no_fake:
        st = None
    if st:
        fake(a, st, out, manifest, common_files)
    g = os.path.join(out, "golden")
    os.makedirs(g)
    for n in sorted(os.listdir(a.golden)):
        if n.startswith(("host-aw2.", "join-aw2.")):
            shutil.copy2(os.path.join(a.golden, n), os.path.join(g, n))
    for cur, _d, files in os.walk(out):
        for n in sorted(files):
            p = os.path.join(cur, n)
            manifest.append("%s  %s" % (md5(p), os.path.relpath(p, out)))
    with open(os.path.join(out, "MANIFEST.txt"), "w", newline="\n") as fh:
        fh.write("\n".join(manifest) + "\n")
    print("\n".join(manifest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
