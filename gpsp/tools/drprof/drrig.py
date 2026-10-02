#!/usr/bin/env python3
"""drrig.py -- the dynarec-profile hardware bench as ONE rig app on ONE console,
driven by the USB handoff loop: launched once from the XMB, never N apps.

    python tools/drprof/drrig.py stage   --out STAGE --builds OUT --fx FX --bios BIOS
    python tools/drprof/drrig.py plan    --stage STAGE
    python tools/drprof/drrig.py install --stage STAGE --drive E: --backup DIR
    python tools/drprof/drrig.py run     --stage STAGE --drive E: --logs LOGS

Modelled on tools/rig/resrig.py (proven on the PSP Go, 21 cycles, 2026-09-26)
and built on the same harness-kit/hw_loop.py primitives (parse_result, flush,
eject, copy_retry, card_root), which it imports and does not change.

THE DIFFERENCE FROM RESRIG.  resrig's arms differ only in ini keys; here they
differ in the EBOOT (shipped dynarec / prototypes / time-split sampler) and in
the fixture (AW2 tour / H&S heavy battle).  So each cycle stages, while the
console is parked in its handoff window:
    EBOOT.PBP            <- the arm's build (all three relabelled "DRPROF RIG")
    .gpsp-harness.ini    <- base keys + the arm's rom/script/sampler keys
    roms/*.sav, *.st0    <- golden, restored before every run
Both ROMs, both scripts, the BIOS and the (identical) ME PRX stay installed.
Every staged file is re-read and md5-checked after the copy.

VOLUME.  The in-app USB export is the MEMORY STICK driver (usbstorms.prx): on
a PSP Go the rig must live on the M2 card, NOT on internal storage (ef0).  The
2026-09-29 attempt was installed on ef0; the run completed, the export served
the M2 card, and the PC watched a volume that never changed.  EBOOTs that
carry the guard (main_psp.c) refuse the handoff on ef0 at boot (EVT handoff_refused
and an on-screen notice) instead of parking invisibly.

SAFETY (as resrig).  Writes only under <drive>/PSP/GAME/GBADHOC-DRPROF.
`install` to a drive letter refuses without tools/rig/backup_sticks.py's
MANIFEST.verified for that volume serial.  A missing RESULT.TXT is a result
(froze/hung): logged, then the loop waits for the owner's relaunch.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.environ.get("RESRIG_HWLOOP_DIR") or os.path.normpath(
    os.path.join(HERE, "..", "..", "..", "..", "harness-kit"))
sys.path.insert(0, KIT)
sys.path.insert(0, os.path.join(HERE, ".."))
import hw_loop  # noqa: E402
from relabel_pbp import relabel  # noqa: E402

APPDIR = "GBADHOC-DRPROF"
TITLE = "DRPROF RIG"
ROUNDS = 2

# build dir (under --builds) per EBOOT kind
BUILDS = {"base": "build-A", "proto": "build-B", "samp": "build-S"}
FIXTURES = {
    "A": {"rom": "aw2.gba", "script": "aw2.txt", "autoexit": "7000",
          "files": {"aw2.gba": "aw2.gba", "aw2.sav": "aw2.sav", "aw2.st0": "aw2.st0"},
          "script_src": "aw2_psp_script.txt"},
    "H": {"rom": "hns.gba", "script": "hns.txt", "autoexit": "3200",
          "files": {"hns.gba": "hns.gba", "hns.sav": "hns.sav",
                    "hns.st0": "heart_soul_heavy.st0"},
          "script_src": "battle.txt"},
}
# arm = (name, fixture letter, build kind, extra harness keys)
ARM_TABLE = [
    ("A-BASE", "A", "base", {}), ("A-PROTO", "A", "proto", {}), ("A-SAMP", "A", "samp", {}),
    ("H-BASE", "H", "base", {}), ("H-PROTO", "H", "proto", {}), ("H-SAMP", "H", "samp", {}),
    # hnsdiag (2026-09-30): the Go's H-BASE derailed deterministically at the
    # first frame after the state load (pc stuck at BIOS 0x186c, RAM frozen)
    # while H-PROTO/H-SAMP matched PPSSPP.  Each arm varies ONE thing on the
    # SAME base EBOOT.  All keys are read by main_psp.c (rom_diag echoes them).
    ("H-BASE-PARA", "H", "base", {"cache_paranoid": "1"}),   # whole-cache syncs
    ("H-BASE-SCAN", "H", "base", {"jit_coherency_scan": "30"}),  # EVT jit_coh
    ("H-BASE-PAGED", "H", "base", {"rom_cap": "15"}),        # ROM paged, not resident
    # hnsdiag2 (2026-09-30): PARA and PAGED both fix H-BASE.  Same EBOOT, no
    # rebuild (a rebuild moves the layout the bug depends on):
    ("H-BASE-STUBS", "H", "base", {"swap_stubs": "1"}),      # resident data, paged-ROM stubs
    ("H-BASE-BAL", "H", "base", {"rom_ballast_kb": "512"}),  # heap blocks land 512 KiB higher
    ("H-BASE-CAP32", "H", "base", {"rom_cap": "32"}),        # resident by the cap, no probe
    # cachesync (2026-09-30): the per-site diagnostic EBOOT (claude/hns-cachesync
    # 63aba20).  D-BASE must still die, or the rebuild moved the layout away.
    ("D-BASE", "H", "diag", {}),
    ("D-PARA", "H", "diag", {"cache_paranoid": "1"}),
    ("D-S0", "H", "diag", {"cache_paranoid_mask": "1"}),       # ROM translate sync
    ("D-S4", "H", "diag", {"cache_paranoid_mask": "16"}),      # emitter stubs sync
    ("D-RAM", "H", "diag", {"cache_paranoid_mask": "494"}),    # RAM/SMC sites 1-3,5-8
    ("D-LOAD", "H", "diag", {"sync_after_load": "1"}),         # one sync after the state load
    ("D-DONLY", "H", "diag", {"cache_paranoid_mask": "66047"}),  # all sites, whole D only
    ("D-IONLY", "H", "diag", {"cache_paranoid_mask": "131583"}), # all sites, whole I only
    # canary (2026-09-30): D-BASE survived the rebuild, so detect the CAUSE:
    # a resident ROM page changing (stray write) whether or not the game dies.
    ("C-BASE", "H", "canary", {"rom_canary": "60"}),
    ("C-STUBS", "H", "canary", {"rom_canary": "60", "swap_stubs": "1"}),
    ("C-PARA", "H", "canary", {"rom_canary": "60", "cache_paranoid": "1"}),
    # hnsdiag3 (2026-09-30): the ORIGINAL failing EBOOT.  Every slower-CPU arm
    # fixed it, and the ME ran in every failure: is it a race with the ME?
    ("H-BASE-NOME", "H", "base", {"me_mode": "0"}),
    ("H-BASE-B128", "H", "base", {"rom_ballast_kb": "128"}),   # still resident, heap shifted
    ("H-BASE-SMALL", "H", "base", {"jit_small": "1"}),         # small JIT tier: nothing lent
    # patch (2026-09-30): the ORIGINAL EBOOT with ONE inlined sync site's
    # paranoid check made unconditional (hns-patch/eboots/PATCHES.json); no
    # relayout.  P-ALL patches all nine = cache_paranoid=1 (the mechanism check).
    ("P-A", "H", "pA", {}),
    ("P-ALL", "H", "pALL", {}),
    ("P-B", "H", "pB", {}),
    ("P-C", "H", "pC", {}),
    ("P-D", "H", "pD", {}),
    ("P-E", "H", "pE", {}),
    ("P-F", "H", "pF", {}),
    ("P-G", "H", "pG", {}),
    ("P-H", "H", "pH", {}),
    ("P-EI", "H", "pEI", {}),   # site E: ranged D writeback + WHOLE I invalidate
    ("P-ED", "H", "pED", {}),   # site E: ranged I invalidate + WHOLE D writeback
    # sweep (2026-09-30): the per-site diag EBOOT survives at ballast 0; find a
    # heap layout where it dies, then narrow the sync sites THERE.
    ("S-B0", "H", "diag", {"rom_ballast_kb": "0"}),
    ("S-B32", "H", "diag", {"rom_ballast_kb": "32"}),
    ("S-B64", "H", "diag", {"rom_ballast_kb": "64"}),
    ("S-B96", "H", "diag", {"rom_ballast_kb": "96"}),
    ("S-B128", "H", "diag", {"rom_ballast_kb": "128"}),
    ("S-B160", "H", "diag", {"rom_ballast_kb": "160"}),
    ("S-B192", "H", "diag", {"rom_ballast_kb": "192"}),
    ("S-B224", "H", "diag", {"rom_ballast_kb": "224"}),
    ("S-B256", "H", "diag", {"rom_ballast_kb": "256"}),
    ("S-B320", "H", "diag", {"rom_ballast_kb": "320"}),
]
PLANS = {
    # 12 cycles, ABBA: the dynarec-profile bench
    "bench": ["A-BASE", "A-PROTO", "A-SAMP", "H-BASE", "H-PROTO", "H-SAMP"],
    # 10 cycles, ABBA: why does the shipped dynarec derail H&S on the Go?
    "hnsdiag": ["H-BASE", "H-BASE-PARA", "H-BASE-SCAN", "H-BASE-PAGED", "H-SAMP"],
    # 10 cycles, ABBA: which part of residency?
    "hnsdiag2": ["H-BASE", "H-BASE-STUBS", "H-BASE-BAL", "H-BASE-CAP32", "H-BASE-PARA"],
    "cachesync": ["D-BASE", "D-PARA", "D-S0", "D-S4", "D-RAM", "D-LOAD", "D-DONLY", "D-IONLY"],
    "canary": ["C-BASE", "C-STUBS", "C-PARA"],
    "hnsdiag3": ["H-BASE", "H-BASE-NOME", "H-BASE-B128"],
    "hnsdiag4": ["H-BASE", "H-BASE-SMALL"],
    "patchGo": ["H-BASE", "P-ALL", "P-A", "P-B", "P-C"],
    "patch3000": ["H-BASE", "P-D", "P-E", "P-F", "P-G", "P-H"],
    # order_for(rounds=1): ONE pass, key answers first
    "patchGo2": ["H-BASE", "P-E", "P-EI", "P-ED", "P-D", "P-F", "P-ALL", "P-B", "P-C", "P-A"],
    "patch3000b": ["H-BASE", "P-EI", "P-ED", "H-BASE", "P-E", "P-D", "P-F", "H-BASE"],
    "sweepA": ["S-B0", "S-B32", "S-B64", "S-B96", "S-B128"],
    "sweepB": ["S-B160", "S-B192", "S-B224", "S-B256", "S-B320"],
}
ARMS = [(n, f, k) for n, f, k, _x in ARM_TABLE]

BASE_INI = [
    ("load_state", "1"),
    ("shash", "1"),
    ("audio_oracle", "1"),
    ("heartbeat_s", "5"),
    ("me_mode", "1"),
    ("log_input", "0"),
    ("handoff", "1"),
    ("handoff_window_s", "90"),
    ("handoff_park_s", "0"),
    ("handoff_max_runs", "100000"),
    ("handoff_total_s", "900"),
]


def log(msg):
    hw_loop.log(msg)


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for blk in iter(lambda: fh.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def crc32(path):
    c = 0
    with open(path, "rb") as fh:
        for blk in iter(lambda: fh.read(1 << 20), b""):
            c = zlib.crc32(blk, c)
    return "%08x" % (c & 0xFFFFFFFF)


def arm_def(arm):
    for name, fx, kind, _x in ARM_TABLE:
        if name == arm:
            return fx, kind
    raise KeyError(arm)


def arm_extra(arm):
    for name, _f, _k, extra in ARM_TABLE:
        if name == arm:
            return extra
    raise KeyError(arm)


def order_for(rounds=ROUNDS, plan="bench"):
    """Round 1 = every arm once; the repeat round runs them in REVERSE
    (ABBA), so no arm is always first after a fixture switch or always last."""
    names = PLANS[plan]
    out = []
    for r in range(rounds):
        out.extend(names if r % 2 == 0 else names[::-1])
    return out


def render_ini(arm, run_id):
    fx, kind = arm_def(arm)
    f = FIXTURES[fx]
    lines = ["# drrig: dynarec-profile hardware bench (docs/DYNAREC-PROFILE.md)"]
    extra = arm_extra(arm)
    # an arm's key REPLACES the base key (duplicate keys: the reader takes one)
    lines += ["%s = %s" % kv for kv in BASE_INI if kv[0] not in extra]
    lines += ["rom = %s" % f["rom"], "script = %s" % f["script"],
              "autoexit_frames = %s" % f["autoexit"]]
    if kind == "samp":
        lines.append("drprof_us = 1000")
    lines += ["%s = %s" % kv for kv in sorted(arm_extra(arm).items())]
    lines += ["run_id = %s" % run_id, "arm = %s" % arm]
    return "\n".join(lines) + "\n"


def staged_keys(ini_text):
    out = {}
    for line in ini_text.splitlines():
        if line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip()
    return out


# --------------------------------------------------------------- stage ----
def cmd_stage(a):
    if os.path.exists(a.out) and os.listdir(a.out):
        print("REFUSING: %s is not empty" % a.out)
        return 2
    base = os.path.join(a.out, "base")
    eb = os.path.join(a.out, "eboots")
    os.makedirs(os.path.join(base, "roms"))
    os.makedirs(eb)
    prx = None
    for kind, bdir in BUILDS.items():
        src = os.path.join(a.builds, bdir)
        with open(os.path.join(src, "EBOOT.PBP"), "rb") as fh:
            pbp = relabel(fh.read(), TITLE)
        with open(os.path.join(eb, kind + ".PBP"), "wb") as fh:
            fh.write(pbp)
        p = md5(os.path.join(src, "gbadhoc_me.prx"))
        if prx and p != prx:
            print("REFUSING: the three builds carry different ME PRXs")
            return 2
        prx = p
    shutil.copy2(os.path.join(a.builds, "build-A", "gbadhoc_me.prx"),
                 os.path.join(base, "gbadhoc_me.prx"))
    shutil.copy2(a.bios, os.path.join(base, "gba_bios.bin"))
    for fxl, f in FIXTURES.items():
        for dst, src in f["files"].items():
            shutil.copy2(os.path.join(a.fx, src), os.path.join(base, "roms", dst))
        shutil.copy2(os.path.join(a.fx, f["script_src"]), os.path.join(base, f["script"]))
    first = order_for(plan=a.plan)[0]
    shutil.copy2(os.path.join(eb, arm_def(first)[1] + ".PBP"),
                 os.path.join(base, "EBOOT.PBP"))
    man = {"created": time.strftime("%Y-%m-%d %H:%M:%S"), "appdir": APPDIR,
           "plan": a.plan, "arms": PLANS[a.plan],
           "order": order_for(plan=a.plan),
           "base_ini": BASE_INI, "builds": BUILDS, "files": {}, "eboots": {}}
    for cur, _d, files in os.walk(base):
        for n in files:
            p = os.path.join(cur, n)
            rel = os.path.relpath(p, base).replace(os.sep, "/")
            if rel == "EBOOT.PBP":
                continue          # swapped per cycle; tracked under "eboots"
            man["files"][rel] = {"md5": md5(p), "crc32": crc32(p),
                                 "size": os.path.getsize(p)}
    for kind in BUILDS:
        p = os.path.join(eb, kind + ".PBP")
        man["eboots"][kind] = {"md5": md5(p), "crc32": crc32(p)}
    with open(os.path.join(a.out, "MANIFEST.json"), "w") as fh:
        json.dump(man, fh, indent=1)
    print("staged %d files + %d EBOOTs -> %s" % (len(man["files"]), len(BUILDS), a.out))
    for k, v in man["eboots"].items():
        print("  %-6s crc32=%s md5=%s" % (k, v["crc32"], v["md5"]))
    return 0


def load_manifest(stage):
    with open(os.path.join(stage, "MANIFEST.json")) as fh:
        return json.load(fh)


def verify_stage(stage, man):
    for rel, want in man["files"].items():
        p = os.path.join(stage, "base", rel)
        if not os.path.isfile(p) or md5(p) != want["md5"]:
            print("STAGE CHANGED since it was made: %s" % rel)
            return False
    for kind, want in man["eboots"].items():
        p = os.path.join(stage, "eboots", kind + ".PBP")
        if not os.path.isfile(p) or md5(p) != want["md5"]:
            print("STAGE CHANGED since it was made: eboots/%s.PBP" % kind)
            return False
    return True


def cmd_plan(a):
    man = load_manifest(a.stage)
    for i, arm in enumerate(man["order"], 1):
        fx, kind = arm_def(arm)
        print("cycle %2d  %-13s fixture=%s build=%s eboot=%s %s" % (
            i, arm, FIXTURES[fx]["rom"], kind, man["eboots"][kind]["crc32"],
            arm_extra(arm) or ""))
    return 0


# ------------------------------------------------------------- install ----
def volume_serial(drive):
    try:
        out = subprocess.run(["cmd", "/c", "vol", drive], capture_output=True,
                             text=True, timeout=20).stdout
        for line in out.splitlines():
            if "Serial Number" in line:
                return line.split()[-1]
    except Exception:
        pass
    return "unknown"


def backup_ok(backup, serial):
    if not os.path.isfile(os.path.join(backup, "MANIFEST.verified")):
        return False, "no MANIFEST.verified in %s" % backup
    try:
        with open(os.path.join(backup, "MANIFEST.txt"), encoding="utf-8") as fh:
            body = fh.read()
    except OSError as e:
        return False, str(e)
    if "serial=%s" % serial not in body:
        return False, "backup manifest has no file from volume %s" % serial
    return True, "backup verified for volume %s" % serial


def user_files(drive, cap=1):
    """Count regular files on the volume (stops at `cap`), ignoring Windows'
    own System Volume Information and the empty folder skeleton a PSP
    creates (PSP/GAME, PSP/SAVEDATA ... with nothing in them)."""
    n = 0
    for cur, dirs, files in os.walk(drive + os.sep):
        dirs[:] = [d for d in dirs if d.lower() not in (
            "system volume information", "$recycle.bin")]
        n += len(files)
        if n >= cap:
            return n
    return n


def stage_arm(stage, man, root, arm, run_id):
    """EBOOT + ini for one arm, verified by re-reading the card."""
    kind = arm_def(arm)[1]
    src = os.path.join(stage, "eboots", kind + ".PBP")
    dst = os.path.join(root, "EBOOT.PBP")
    if not hw_loop.copy_retry(src, dst, why="eboot"):
        return None
    if md5(dst) != man["eboots"][kind]["md5"]:
        log("  EBOOT VERIFY FAILED for %s" % arm)
        return None
    text = render_ini(arm, run_id)
    with open(os.path.join(root, ".gpsp-harness.ini"), "w", encoding="utf-8",
              newline="\n") as fh:
        fh.write(text)
    return text


def cmd_install(a):
    man = load_manifest(a.stage)
    if not verify_stage(a.stage, man):
        return 2
    is_card = len(a.drive) == 2 and a.drive[1] == ":"
    if is_card and not os.path.isdir(a.drive + os.sep):
        print("REFUSING: %s is not mounted" % a.drive)
        return 2
    if is_card:
        serial = volume_serial(a.drive)
        ok, why = backup_ok(a.backup, serial)
        if not ok:
            # A volume with NO user files has nothing to back up (the Go's
            # fresh M2 card, 2026-09-30): allowed, and said so in RIG.TXT.
            nfiles = user_files(a.drive)
            if nfiles == 0:
                why = "volume %s holds no user files: nothing to back up" % serial
            else:
                print("REFUSING to write to %s: %s (and the volume holds %d user "
                      "file(s))" % (a.drive, why, nfiles))
                return 2
    else:
        serial, why = "dir", "directory target (dry run): backup check skipped"
    log(why)
    root = hw_loop.card_root(a.drive)
    if os.path.exists(root) and not a.reinstall:
        print("REFUSING: %s exists (pass --reinstall to overwrite the rig "
              "folder; nothing outside it is touched)" % root)
        return 2
    base = os.path.join(a.stage, "base")
    for cur, _d, files in os.walk(base):
        rel = os.path.relpath(cur, base)
        out = root if rel == "." else os.path.join(root, rel)
        os.makedirs(out, exist_ok=True)
        for n in files:
            if not hw_loop.copy_retry(os.path.join(cur, n), os.path.join(out, n),
                                      why="install"):
                return 1
    for sub in ("handoff", "log"):
        d = os.path.join(root, sub)
        os.makedirs(d, exist_ok=True)
        for n in os.listdir(d):          # a previous attempt's leftovers
            try:
                os.remove(os.path.join(d, n))
            except OSError:
                pass
    run_id = "1-%d" % int(time.time())
    if stage_arm(a.stage, man, root, man["order"][0], run_id) is None:
        return 1
    with open(os.path.join(root, "RIG.TXT"), "w") as fh:
        fh.write("drrig install %s serial=%s first=%s run_id=%s\n"
                 % (time.strftime("%Y-%m-%d %H:%M:%S"), serial,
                    man["order"][0], run_id))
    for rel, want in man["files"].items():
        if md5(os.path.join(root, rel)) != want["md5"]:
            print("INSTALL VERIFY FAILED: %s" % rel)
            return 1
    if is_card:
        hw_loop.flush(a.drive)
    log("installed %d files + EBOOT to %s (verified); cycle 1 = %s run_id=%s"
        % (len(man["files"]), root, man["order"][0], run_id))
    return 0


# ----------------------------------------------------------------- run ----
def collect(root, dest, cycle, arm, run_id, man, drive, res, ini_text, froze):
    os.makedirs(dest, exist_ok=True)
    got = {}
    for sub in ("log", "handoff"):
        d = os.path.join(root, sub)
        if not os.path.isdir(d):
            continue
        for n in sorted(os.listdir(d)):
            p = os.path.join(d, n)
            if not os.path.isfile(p) or n == "CMD.TXT":
                continue
            out = os.path.join(dest, n if sub == "log" else "handoff-" + n)
            if hw_loop.copy_retry(p, out, why="collect"):
                got[os.path.basename(out)] = md5(out)
                if sub == "log":
                    try:
                        os.remove(p)
                    except OSError:
                        pass
    eboot = os.path.join(root, "EBOOT.PBP")
    kind = arm_def(arm)[1] if arm in [x[0] for x in ARMS] else None
    meta = {"drive": drive, "role": "solo", "arm": arm, "run_id": run_id,
            "cycle": cycle, "result": res or "unknown",
            "eboot_crc32": crc32(eboot) if os.path.isfile(eboot) else "unknown",
            "eboot_expected_crc32": man["eboots"][kind]["crc32"] if kind else "unknown",
            "staged_ini": staged_keys(ini_text), "collected": got,
            "t": time.strftime("%Y-%m-%d %H:%M:%S"), "froze_before": froze}
    with open(os.path.join(dest, "meta.json"), "w") as fh:
        json.dump(meta, fh, indent=1)
    return meta


def restore_golden(stage, root):
    src = os.path.join(stage, "base", "roms")
    done = []
    for n in sorted(os.listdir(src)):
        if n.lower().endswith((".sav", ".st0")):
            if hw_loop.copy_retry(os.path.join(src, n), os.path.join(root, "roms", n),
                                  why="golden"):
                done.append(n)
    return done


def cmd_run(a):
    man = load_manifest(a.stage)
    if not verify_stage(a.stage, man):
        return 2
    order = man["order"]
    root = hw_loop.card_root(a.drive)
    os.makedirs(a.logs, exist_ok=True)
    state_p = os.path.join(a.logs, "RIGSTATE.json")
    st = {"next": 0, "cycles": []}
    if os.path.isfile(state_p):
        with open(state_p) as fh:
            st = json.load(fh)
    real = len(a.drive) == 2 and a.drive[1] == ":"
    log("drrig: %d cycles, %s, logs -> %s (resuming at %d)"
        % (len(order), root, a.logs, st["next"] + 1))
    while st["next"] < len(order):
        i = st["next"]
        arm = order[i]
        res_p = os.path.join(root, "handoff", "RESULT.TXT")
        ini_p = os.path.join(root, ".gpsp-harness.ini")
        log("cycle %d/%d: waiting for %s (RESULT.TXT)" % (i + 1, len(order), arm))
        t0, warned = time.time(), False
        while hw_loop.parse_result(res_p) is None:
            if not warned and time.time() - t0 > a.freeze_s:
                log("  NO RESULT after %ds -- cycle %d (%s) froze or hung. Owner: "
                    "hold POWER to switch off, power on, relaunch DRPROF RIG. "
                    "The loop keeps waiting." % (a.freeze_s, i + 1, arm))
                with open(os.path.join(a.logs, "FREEZE-c%03d-%s.txt" % (i + 1, arm)),
                          "w") as fh:
                    fh.write("cycle=%d arm=%s no RESULT.TXT after %ds at %s\n"
                             % (i + 1, arm, a.freeze_s, time.strftime("%Y-%m-%d %H:%M:%S")))
                warned = True
            if time.time() - t0 > a.timeout:
                log("TIMEOUT waiting for cycle %d -- stopping (resume later with "
                    "the same command)" % (i + 1))
                return 1
            time.sleep(a.poll)
        res = hw_loop.parse_result(res_p)
        try:
            with open(ini_p, encoding="utf-8") as fh:
                ini_text = fh.read()
        except OSError:
            ini_text = ""
        keys = staged_keys(ini_text)
        run_id = keys.get("run_id", "unknown")
        if keys.get("arm") != arm:
            log("  WARNING: card ini says arm=%s, plan says %s -- collecting under "
                "the card's value" % (keys.get("arm"), arm))
            arm = keys.get("arm", "unknown")
        if res.get("status") == "parked" and not warned:
            log("  console was PARKED -- sending RUN for %s (not counted)" % arm)
            more = True
        else:
            dest = os.path.join(a.logs, "auto%03d-%s-solo" % (i + 1, arm))
            meta = collect(root, dest, i + 1, arm, run_id, man, a.drive, res,
                           ini_text, warned)
            log("  collected %d files -> %s  status=%s exit=%s reason=%s frames=%s "
                "evt_drop=%s" % (len(meta["collected"]), os.path.basename(dest),
                                 res.get("status"), res.get("exit"), res.get("reason"),
                                 res.get("frames", "unknown"),
                                 res.get("evt_drop", "unknown")))
            st["cycles"].append({"cycle": i + 1, "arm": arm, "run_id": run_id,
                                 "froze": warned, "dir": os.path.basename(dest)})
            st["next"] = i + 1
            log("  golden restored: %s" % ", ".join(restore_golden(a.stage, root)))
            more = st["next"] < len(order)
            if more:
                nxt = order[st["next"]]
                nrid = "%d-%d" % (st["next"] + 1, int(time.time()))
                if stage_arm(a.stage, man, root, nxt, nrid) is None:
                    log("STAGING FAILED for %s -- stopping with the console parked"
                        % nxt)
                    return 1
                log("  staged cycle %d: %s (%s) run_id=%s"
                    % (st["next"] + 1, nxt, arm_def(nxt)[1], nrid))
        with open(os.path.join(root, "handoff", "CMD.TXT"), "w") as fh:
            fh.write("RUN\n" if more else "STOP\n")
        try:
            os.remove(res_p)
        except OSError:
            pass
        with open(state_p, "w") as fh:
            json.dump(st, fh, indent=1)
        if real:
            hw_loop.eject(a.drive, attempts=3)
    log("all %d cycles collected. Report: python tools/drprof/hw_report.py --rig %s"
        % (len(order), a.logs))
    return 0


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("stage")
    s.add_argument("--out", required=True)
    s.add_argument("--builds", required=True, help="dir holding build-A/B/S")
    s.add_argument("--fx", required=True, help="fixture dir (roms, states, scripts)")
    s.add_argument("--bios", required=True)
    s.add_argument("--plan", default="bench", choices=sorted(PLANS))
    p = sp.add_parser("plan")
    p.add_argument("--stage", required=True)
    for name in ("install", "run"):
        q = sp.add_parser(name)
        q.add_argument("--stage", required=True)
        q.add_argument("--drive", required=True,
                       help="the console's MEMORY STICK drive letter, or a "
                            "directory standing in for one (PPSSPP dry run)")
        q.add_argument("--appdir", default=APPDIR)
        if name == "install":
            q.add_argument("--backup", default="")
            q.add_argument("--reinstall", action="store_true")
        else:
            q.add_argument("--logs", required=True)
            q.add_argument("--freeze-s", type=int, default=600)
            q.add_argument("--timeout", type=int, default=4 * 3600)
            q.add_argument("--poll", type=float, default=2.0)
    a = ap.parse_args()
    if getattr(a, "appdir", APPDIR) != APPDIR:
        print("REFUSING: --appdir must be exactly %s" % APPDIR)
        return 2
    hw_loop.APPDIR = APPDIR
    return {"stage": cmd_stage, "plan": cmd_plan, "install": cmd_install,
            "run": cmd_run}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
