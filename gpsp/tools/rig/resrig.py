#!/usr/bin/env python3
"""resrig.py -- the ROM-residency hardware A/B, unattended, on ONE console.

    python tools/rig/resrig.py stage   --out STAGE --eboot E --prx P --job JOB \
                                       --rom ROM --bios BIOS
    python tools/rig/resrig.py plan    --stage STAGE
    python tools/rig/resrig.py install --stage STAGE --drive G: --backup DIR
    python tools/rig/resrig.py run     --stage STAGE --drive G: --logs LOGS

WHAT IT IS.  A thin wrapper round harness-kit/hw_loop.py's primitives
(parse_result, flush, eject, copy_retry, card_root): it imports them and
changes none of them.  It exists instead of a patch to hw_loop.py because the
wireless rig is adding --arms/--order there (claude/wireless); this keeps the
same conventions -- run_id = <cycle>-<unix>, `arm =`, a meta.json sidecar with
{drive, role, arm, run_id, eboot_crc32, eboot_md5, prx_md5, cycle,
console_run} -- so it can be retired onto that code once it lands.

THE EXPERIMENT (docs/ROM-RESIDENCY.md).  One harness64 EBOOT, the 2026-09-16
Unbound double-battle job, seven arms that differ only in harness ini keys,
every arm checked emulation-neutral in PPSSPP.  Interleaved: round r runs every
arm once, the order rotated by r, so no arm always follows the same one.

SAFETY.
  * Writes only under <drive>/PSP/GAME/GBADHOC-RESRIG.  Any other --appdir is
    refused; nothing else on the card is read or written.
  * `install` refuses unless --backup holds MANIFEST.verified from
    tools/rig/backup_sticks.py for that drive's volume serial.
  * Never deletes a save.  The golden .sav/.st0 are restored before every run
    (the job must start from the same state), after the post-run copy is kept.
  * A missing RESULT.TXT is a result (the console froze or hung), logged with
    the arm and run_id that were running; the loop keeps waiting for the owner
    to power-cycle and relaunch, then collects both attempts.
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
# harness-kit/hw_loop.py is the solo-mode copy (ca53cab); the tree's own
# tools/hw_loop.py predates it.  RESRIG_HWLOOP_DIR overrides.
KIT = os.environ.get("RESRIG_HWLOOP_DIR") or os.path.normpath(
    os.path.join(HERE, "..", "..", "..", "..", "harness-kit"))
sys.path.insert(0, KIT)
import hw_loop  # noqa: E402  (parse_result, flush, eject, copy_retry, card_root)

APPDIR = "GBADHOC-RESRIG"
ROM_NAME = "Pokemon Unbound.gba"
ROUNDS = 3

# One variable per arm.  Keys are harness-ini keys; each is echoed on the
# console (cfg / rom_diag / rom_cache_cap / me_mode lines) and checked by
# resrig_summary.py.  CTRL is the paged control every other arm is judged by.
ARMS = [
    ("CTRL",         {}),
    ("RES",          {"rom_cap": "32"}),
    ("RES-SWAP",     {"rom_cap": "32", "swap_stubs": "1"}),
    ("RES-PARANOID", {"rom_cap": "32", "cache_paranoid": "1"}),
    ("BALLAST",      {"rom_ballast_kb": "17408"}),
    ("RES-MEOFF",    {"rom_cap": "32", "me_mode": "0"}),
    ("RES-SCAN",     {"rom_cap": "32", "jit_coherency_scan": "60"}),
]

# The 2026-09-16 job's harness keys that still mean something to this build,
# plus the rig's instruments.  handoff_* drive the USB loop; they do not touch
# emulation.  log_input = 0: the job's 900 presses would otherwise be 900
# event lines competing with the SMC census for the ring.
BASE_INI = [
    ("rom", ROM_NAME),
    ("load_state", "1"),
    ("script", "battle.inputs"),
    ("autoexit_frames", "4500"),
    ("perf_rig", "1"),
    ("perf_job", "resrig"),
    ("perf_fixture", "unbound_double_high"),
    ("perf_from", "300"),
    ("perf_to", "3900"),
    ("perf_timeout_s", "600"),
    ("core_phase", "0"),
    ("ff", "0"),
    ("host", "0"),
    ("join", "0"),
    ("preempt_prof", "0"),
    ("log_thread", "1"),
    ("sram_thread", "1"),
    ("me_sameframe", "0"),
    ("me_shadow", "0"),
    ("audio_oracle", "1"),
    ("shash", "1"),
    ("heartbeat_s", "5"),
    ("log_input", "0"),
    ("handoff", "1"),
    ("handoff_window_s", "90"),
    ("handoff_park_s", "0"),
    ("handoff_max_runs", "100000"),
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


def order_for(rounds=ROUNDS):
    """Round r = every arm once, rotated by r: CTRL is never always first and
    no arm always follows the same neighbour."""
    names = [a for a, _ in ARMS]
    out = []
    for r in range(rounds):
        k = (r * 3) % len(names)
        out.extend(names[k:] + names[:k])
    return out


def arm_keys(arm):
    for name, keys in ARMS:
        if name == arm:
            return keys
    raise KeyError(arm)


def render_ini(arm, run_id):
    lines = ["# resrig: ROM-residency hardware A/B (docs/ROM-RESIDENCY.md)",
             "# Falsifier: RES diverging from the golden shash while CTRL",
             "# matches it = residency still breaks this tree on hardware."]
    lines += ["%s = %s" % kv for kv in BASE_INI]
    lines += ["%s = %s" % kv for kv in sorted(arm_keys(arm).items())]
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
    base = os.path.join(a.out, "base")
    if os.path.exists(a.out) and os.listdir(a.out):
        print("REFUSING: %s is not empty" % a.out)
        return 2
    os.makedirs(os.path.join(base, "roms"))
    shutil.copy2(a.eboot, os.path.join(base, "EBOOT.PBP"))
    shutil.copy2(a.prx, os.path.join(base, "gbadhoc_me.prx"))
    shutil.copy2(a.bios, os.path.join(base, "gba_bios.bin"))
    shutil.copy2(a.rom, os.path.join(base, "roms", ROM_NAME))
    shutil.copy2(os.path.join(a.job, "battle.inputs"),
                 os.path.join(base, "battle.inputs"))
    for name in os.listdir(os.path.join(a.job, "roms")):
        shutil.copy2(os.path.join(a.job, "roms", name),
                     os.path.join(base, "roms", name))
    # CONFIG.INI: the job's, minus keys this build does not read (they were
    # dead then too -- e.g. me_sameframe is harness-only).  Pruned list comes
    # from the PPSSPP cfg audit, passed as --drop-config.
    drop = set(k for k in (a.drop_config or "").split(",") if k)
    with open(os.path.join(a.job, "CONFIG.INI"), encoding="utf-8") as fh:
        cfg = [l for l in fh.read().splitlines()
               if not (("=" in l and not l.startswith("#"))
                       and l.split("=", 1)[0].strip() in drop)]
    with open(os.path.join(base, "CONFIG.INI"), "w", encoding="utf-8",
              newline="\n") as fh:
        fh.write("\n".join(cfg) + "\n")
    man = {"created": time.strftime("%Y-%m-%d %H:%M:%S"),
           "appdir": APPDIR, "arms": [a_ for a_, _ in ARMS],
           "order": order_for(), "base_ini": BASE_INI,
           "arm_keys": dict(ARMS), "config_dropped": sorted(drop),
           "files": {}}
    for cur, _d, files in os.walk(base):
        for n in files:
            p = os.path.join(cur, n)
            rel = os.path.relpath(p, base).replace(os.sep, "/")
            man["files"][rel] = {"md5": md5(p), "crc32": crc32(p),
                                 "size": os.path.getsize(p)}
    with open(os.path.join(a.out, "MANIFEST.json"), "w") as fh:
        json.dump(man, fh, indent=1)
    print("staged %d files -> %s" % (len(man["files"]), a.out))
    print("EBOOT crc32=%s md5=%s" % (man["files"]["EBOOT.PBP"]["crc32"],
                                     man["files"]["EBOOT.PBP"]["md5"]))
    return 0


def load_manifest(stage):
    with open(os.path.join(stage, "MANIFEST.json")) as fh:
        return json.load(fh)


def verify_stage(stage, man):
    base = os.path.join(stage, "base")
    for rel, want in man["files"].items():
        p = os.path.join(base, rel)
        if not os.path.isfile(p) or md5(p) != want["md5"]:
            print("STAGE CHANGED since it was made: %s" % rel)
            return False
    return True


def cmd_plan(a):
    man = load_manifest(a.stage)
    for i, arm in enumerate(man["order"], 1):
        print("cycle %2d  %-13s %s" % (i, arm, man["arm_keys"][arm] or "(control)"))
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


def copy_tree(src, dst):
    for cur, _d, files in os.walk(src):
        rel = os.path.relpath(cur, src)
        out = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(out, exist_ok=True)
        for n in files:
            if not hw_loop.copy_retry(os.path.join(cur, n), os.path.join(out, n),
                                      why="install"):
                return False
    return True


def write_ini(root, arm, run_id):
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
    if is_card:
        serial = volume_serial(a.drive)
        ok, why = backup_ok(a.backup, serial)
        if not ok:
            print("REFUSING to write to %s: %s" % (a.drive, why))
            return 2
    else:
        # A directory standing in for a card (PPSSPP memstick): nothing of the
        # owner's is there, so no backup is required.
        serial, why = "dir", "directory target (dry run): backup check skipped"
    log(why)
    root = hw_loop.card_root(a.drive)
    if os.path.exists(root) and not a.reinstall:
        print("REFUSING: %s exists (pass --reinstall to overwrite the rig "
              "folder; nothing outside it is touched)" % root)
        return 2
    if not copy_tree(os.path.join(a.stage, "base"), root):
        return 1
    os.makedirs(os.path.join(root, "handoff"), exist_ok=True)
    os.makedirs(os.path.join(root, "log"), exist_ok=True)
    run_id = "1-%d" % int(time.time())
    write_ini(root, man["order"][0], run_id)
    with open(os.path.join(root, "RIG.TXT"), "w") as fh:
        fh.write("resrig install %s serial=%s first=%s run_id=%s\n"
                 % (time.strftime("%Y-%m-%d %H:%M:%S"), serial,
                    man["order"][0], run_id))
    # Re-read what landed, not what we meant to write.
    for rel, want in man["files"].items():
        if md5(os.path.join(root, rel)) != want["md5"]:
            print("INSTALL VERIFY FAILED: %s" % rel)
            return 1
    if is_card:
        hw_loop.flush(a.drive)
    log("installed %d files to %s (verified); cycle 1 = %s run_id=%s"
        % (len(man["files"]), root, man["order"][0], run_id))
    return 0


# ----------------------------------------------------------------- run ----
def collect(root, dest, cycle, arm, run_id, man, drive, res, ini_text,
            froze=False):
    """Copy EVERYTHING the run left in log/ and handoff/, plus a meta sidecar.
    Files are removed from the card only after they are copied."""
    os.makedirs(dest, exist_ok=True)
    got = {}
    for sub in ("log", "handoff"):
        d = os.path.join(root, sub)
        if not os.path.isdir(d):
            continue
        for n in sorted(os.listdir(d)):
            p = os.path.join(d, n)
            if not os.path.isfile(p) or n in ("CMD.TXT",):
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
    prx = os.path.join(root, "gbadhoc_me.prx")
    meta = {
        "drive": drive, "volume_serial": volume_serial(drive)
        if len(drive) == 2 else "dir", "role": "solo", "arm": arm,
        "run_id": run_id, "cycle": cycle,
        "console_run": (res or {}).get("run", "unknown"),
        "result": res or "unknown",
        "eboot_crc32": crc32(eboot) if os.path.isfile(eboot) else "unknown",
        "eboot_md5": md5(eboot) if os.path.isfile(eboot) else "unknown",
        "prx_md5": md5(prx) if os.path.isfile(prx) else "unknown",
        "staged_ini": staged_keys(ini_text),
        "manifest_eboot_crc32": man["files"]["EBOOT.PBP"]["crc32"],
        "collected": got, "t": time.strftime("%Y-%m-%d %H:%M:%S"),
        # True when this cycle's first attempt produced no RESULT.TXT within
        # --freeze-s: the files below then hold the RETRY, and the frozen
        # attempt's evidence is frontend.prev.log / shash.prev.txt / the
        # first boot's lines in heartbeat.txt.
        "froze_before": froze,
    }
    with open(os.path.join(dest, "meta.json"), "w") as fh:
        json.dump(meta, fh, indent=1)
    return meta


def restore_golden(stage, root):
    """The job's .sav/.st0 back to the staged bytes before every run."""
    src = os.path.join(stage, "base", "roms")
    done = []
    for n in sorted(os.listdir(src)):
        if n.lower().endswith((".sav", ".st0")):
            if hw_loop.copy_retry(os.path.join(src, n),
                                  os.path.join(root, "roms", n), why="golden"):
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

    def wrong_card():
        """Drive letters drift between sessions: the card must be the one we
        installed (and backed up), identified by content, not by letter."""
        if not real:
            return None
        try:
            with open(os.path.join(root, "RIG.TXT")) as fh:
                rig = fh.read()
        except OSError:
            return "no RIG.TXT on %s" % root
        serial = volume_serial(a.drive)
        if "serial=%s " % serial not in rig:
            return "%s (serial %s) is not the card resrig installed (%s)" % (
                a.drive, serial, rig.strip())
        return None
    log("resrig: %d cycles, %s, logs -> %s (resuming at %d)"
        % (len(order), root, a.logs, st["next"] + 1))
    while st["next"] < len(order):
        i = st["next"]
        arm = order[i]
        res_p = os.path.join(root, "handoff", "RESULT.TXT")
        ini_p = os.path.join(root, ".gpsp-harness.ini")
        log("cycle %d/%d: waiting for %s (RESULT.TXT)" % (i + 1, len(order), arm))
        t0 = time.time()
        warned = False
        while hw_loop.parse_result(res_p) is None:
            if not warned and time.time() - t0 > a.freeze_s:
                # No RESULT within the budget: the run froze, hung or crashed.
                # That IS the measurement -- record it, then keep waiting for
                # the owner's power-cycle + relaunch.
                log("  NO RESULT after %ds -- cycle %d (%s) froze or hung. "
                    "Owner: hold POWER to switch off, power on, relaunch "
                    "GBAdhoc RESRIG from the XMB. The loop keeps waiting."
                    % (a.freeze_s, i + 1, arm))
                with open(os.path.join(a.logs, "FREEZE-c%03d-%s.txt" % (i + 1, arm)),
                          "w") as fh:
                    fh.write("cycle=%d arm=%s no RESULT.TXT after %ds at %s\n"
                             % (i + 1, arm, a.freeze_s,
                                time.strftime("%Y-%m-%d %H:%M:%S")))
                warned = True
            if time.time() - t0 > a.timeout:
                log("TIMEOUT waiting for cycle %d -- stopping (resume later "
                    "with the same command)" % (i + 1))
                return 1
            time.sleep(a.poll)
        res = hw_loop.parse_result(res_p)
        why = wrong_card()
        if why:
            print("REFUSING: %s" % why)
            return 2
        try:
            with open(ini_p, encoding="utf-8") as fh:
                ini_text = fh.read()
        except OSError:
            ini_text = ""
        run_id = staged_keys(ini_text).get("run_id", "unknown")
        if staged_keys(ini_text).get("arm") != arm:
            log("  WARNING: card ini says arm=%s, plan says %s -- collecting "
                "under the card's value" % (staged_keys(ini_text).get("arm"), arm))
            arm = staged_keys(ini_text).get("arm", "unknown")
        if res.get("status") == "parked" and not warned:
            # Woken, not fresh from a run: command RUN with the staged arm.
            log("  console was PARKED -- sending RUN for %s (not counted)" % arm)
            more = True
        else:
            dest = os.path.join(a.logs, "auto%03d-%s-solo" % (i + 1, arm))
            meta = collect(root, dest, i + 1, arm, run_id, man, a.drive, res,
                           ini_text, froze=warned)
            log("  collected %d files -> %s  status=%s exit=%s reason=%s "
                "frames=%s evt_drop=%s"
                % (len(meta["collected"]), os.path.basename(dest),
                   res.get("status"), res.get("exit"), res.get("reason"),
                   res.get("frames", "unknown"), res.get("evt_drop", "unknown")))
            st["cycles"].append({"cycle": i + 1, "arm": arm, "run_id": run_id,
                                 "froze": warned, "dir": os.path.basename(dest)})
            st["next"] = i + 1
            restored = restore_golden(a.stage, root)
            log("  golden restored: %s" % ", ".join(restored))
            more = st["next"] < len(order)
            if more:
                nxt = order[st["next"]]
                nrid = "%d-%d" % (st["next"] + 1, int(time.time()))
                write_ini(root, nxt, nrid)
                log("  staged cycle %d: %s run_id=%s" % (st["next"] + 1, nxt, nrid))
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
    log("all %d cycles collected. Summarize with tools/rig/resrig_summary.py"
        % len(order))
    return 0


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("stage")
    s.add_argument("--out", required=True)
    s.add_argument("--eboot", required=True)
    s.add_argument("--prx", required=True)
    s.add_argument("--job", required=True, help="the 09-16 job directory")
    s.add_argument("--rom", required=True)
    s.add_argument("--bios", required=True)
    s.add_argument("--drop-config", default="",
                   help="comma list of CONFIG.INI keys this build does not read")
    p = sp.add_parser("plan")
    p.add_argument("--stage", required=True)
    for name in ("install", "run"):
        q = sp.add_parser(name)
        q.add_argument("--stage", required=True)
        q.add_argument("--drive", required=True,
                       help="the console's drive letter (G:), or a directory "
                            "standing in for one (PPSSPP dry run)")
        q.add_argument("--appdir", default=APPDIR)
        if name == "install":
            q.add_argument("--backup", default="",
                           help="backup_sticks.py output for this card "
                                "(required for a drive letter)")
            q.add_argument("--reinstall", action="store_true")
        else:
            q.add_argument("--logs", required=True)
            q.add_argument("--freeze-s", type=int, default=600,
                           help="no RESULT.TXT this long = froze/hung")
            q.add_argument("--timeout", type=int, default=6 * 3600)
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
