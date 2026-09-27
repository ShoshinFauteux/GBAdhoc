#!/usr/bin/env python3
"""hw_loop.py -- PC half of the unattended two-console hardware loop (ADR-0053).

RIG FORK (docs/RIG-DOUBLE-BATTLE.md §6).  Forked from harness-kit/hw_loop.py
(md5 ab657d09) so the shared kit is never edited under another campaign.
Added, and nothing else changed in behaviour:
  --arms A=<dir>,B=<dir> --order ABBA   arm round-robin: each cycle stages
        the NEXT arm's directory; the arm and a run_id are appended to both
        staged .gpsp-harness.ini files, so every console log names its arm.
  logs are named autoNNN-<arm>-<role>.log (PC cycle, never console counters)
  autoNNN-<arm>-<role>.meta.json: drive, role, arm, run_id, card EBOOT
        crc32/md5, PRX md5, RESULT.TXT (incl. frames=/evt_drop=/t_ms=)
  frontend.prev.log (a relaunch-preserved log) is collected too
  ROLE.TXT on each card must name the role it is serviced as
  --appdir must be GBADHOC-RIG (the owner's own install is never touched)
Liveness is RESULT.TXT, never log growth.

The console finishes a run, writes handoff/RESULT.TXT, and exposes its memory
stick over USB.  This script notices the volume, collects the log, stages the
next build, writes handoff/CMD.TXT, and ejects.  The console sees the eject,
reads the command, and relaunches itself.  Nobody touches the PSP.

Run it with the cards' drive letters:

    python hw_loop.py --host D: --join E: --stage build/next --runs 10

STAGING.  --stage is a directory whose contents are copied onto BOTH cards
before the relaunch: EBOOT.PBP, CONFIG.INI, .gpsp-harness.ini, whatever.  Files
may be role-qualified with a `host-` or `join-` prefix to go to one card only
(e.g. `join-CONFIG.INI`), which is how you set rfu_rx_cap on the client alone.
An empty or missing --stage means "same build again", which is a legitimate
experiment and not an error.

WHAT IT REFUSES TO DO.  It never deletes a save.  It never writes to a card
that has no handoff/RESULT.TXT (that is somebody's PSP, not a rig console).  It
stops the chain the moment a run reports a nonzero exit unless --keep-going.
"""
import argparse
import hashlib
import json
import zlib
import os
import re
import shutil
import subprocess
import sys
import time


def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def parse_result(path):
    """handoff/RESULT.TXT -> dict. Absent or malformed reads as None, never
    as a default-shaped success -- a missing result must not look like a pass."""
    try:
        with open(path, "r", errors="replace") as fh:
            body = fh.read()
    except OSError:
        return None
    out = {}
    for line in body.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out if "status" in out else None


def _ps(script, timeout=60):
    """Run a PowerShell snippet, returning (rc, stdout). Never raises."""
    try:
        p = subprocess.run(["powershell", "-NoProfile", "-NonInteractive",
                            "-Command", script], timeout=timeout,
                           capture_output=True, text=True)
        return p.returncode, (p.stdout or "").strip()
    except Exception as e:                      # noqa: BLE001 - report, never raise
        return 1, "exception: %s" % e


def flush(drive):
    """Force Windows' write cache out to the card.

    THIS IS THE LOAD-BEARING STEP, not the eject.  CMD.TXT is written while the
    card is exported over USB; if it is still sitting in the PC's cache when the
    console tears USB down and reads ms0 itself, the console sees NO FILE and
    re-arms for another window.  That is precisely what happened: the log said
    `no CMD.TXT -- backing off to a 300s window` while the PC could plainly see
    CMD.TXT, because the PC was reading its own cache.  Same class of bug as the
    16 MB ROM that "verified" twice and was not on the card.
    """
    rc, out = _ps("Write-VolumeCache -DriveLetter %s" % drive[0])
    if rc != 0:
        log("  FLUSH FAILED on %s (%s) -- the console may not see CMD.TXT"
            % (drive, out or "no output"))
    return rc == 0


def eject(drive, verify_s=20, attempts=3):
    """Release the volume so the console's sceUsbGetState sees the drop and
    stops waiting.  Without this every run pays the full handoff window.

    VERIFIES.  The old version fired InvokeVerb("Eject") into the dark, caught
    every exception, and returned True regardless -- so a silently refused eject
    (a busy handle, an antivirus scan, a shell window open on the card) was
    indistinguishable from a working one, and the loop sat waiting for a run
    that was never commanded.  A still-mounted volume IS the failure signal.
    """
    flush(drive)

    # RETRY, because the refusal is transient by nature.
    #
    # The eject is refused on ~4 of 5 cycles, costing ~2 minutes of every ~7
    # minute cycle (the console falls back to its full handoff window). The
    # cause is a handle still open on the volume at the instant we ask -- we
    # have just finished copying a log, up to 20 BMPs and two saves off it, and
    # Windows does not release directory handles the moment `shutil.copy2`
    # returns. Antivirus scanning the freshly-written copies can hold them too.
    #
    # So this does not CHANGE the eject, it just asks again after giving the
    # volume a moment to go quiet. Deliberately conservative:
    #   * behaviour with attempts=1 is byte-for-byte what it was before,
    #   * a refusal is still only ever logged, never fatal,
    #   * CMD.TXT is already flushed above, so even total failure costs time
    #     and never data or a stalled loop.
    # Revert = pass --eject-retries 1 (no code change needed).
    ps = ('$d=(New-Object -comObject Shell.Application).Namespace(17)'
          '.ParseName("%s"); if ($d) { $d.InvokeVerb("Eject") }' % drive)
    per_try = max(2.0, verify_s / float(max(1, attempts)))
    for attempt in range(1, max(1, attempts) + 1):
        _ps(ps, timeout=30)
        deadline = time.time() + per_try
        while time.time() < deadline:
            if not os.path.isdir(drive + os.sep):
                log("  %s ejected%s" % (drive,
                    "" if attempt == 1 else " (attempt %d)" % attempt))
                return True
            time.sleep(0.5)
        if attempt < attempts:
            time.sleep(1.5)      # let whatever holds it finish and close

    log("  EJECT DID NOT TAKE on %s -- still mounted after %d attempt(s) over "
        "%ds. The console will fall back to its handoff window; CMD.TXT was "
        "flushed, so it should still pick the command up."
        % (drive, attempts, verify_s))
    return False


# Files that decide which console is host and which is client.  Staging one of
# these WITHOUT a role prefix copies the same role onto both cards -- two hosts,
# no session, and a pile of logs that look like a networking bug.  Refused.
ROLE_CRITICAL = (".gpsp-harness.ini",)


RIG_APPDIR = "GBADHOC-RIG"
_ARM_OF_RUN = {}          # PC cycle -> arm letter that ran in it
_NEXT_ARM = [None]        # arm staged for the run about to start
_MODEL_MISMATCH = []      # (drive, role, logged model, ROLE.TXT model)


def check_model(drive, role, log_path):
    """ROLES BY CONTENT: the console writes `EVT psp_model ... name=PSP-3000`
    into every log.  ROLE.TXT says which model this card's role belongs to.
    A disagreement means the drive letters drifted or the cards were swapped;
    the loop stops before staging another run onto the wrong console."""
    try:
        role_txt = open(os.path.join(card_root(drive), "ROLE.TXT"),
                        encoding="utf-8", errors="replace").read().split()
        want = role_txt[1] if len(role_txt) > 1 else None
        got = None
        for line in open(log_path, encoding="utf-8", errors="replace"):
            m = re.match(r"EVT psp_model .*name=(\S+)", line)
            if m:
                got = m.group(1)
                break
    except OSError:
        return
    if want and got and want != got:
        _MODEL_MISMATCH.append((drive, role, got, want))


def _crc32_md5(path):
    crc, h = 0, hashlib.md5()
    try:
        with open(path, "rb") as fh:
            for blk in iter(lambda: fh.read(1 << 20), b""):
                crc = zlib.crc32(blk, crc)
                h.update(blk)
    except OSError:
        return None, None
    return "%08x" % (crc & 0xFFFFFFFF), h.hexdigest()


def check_role_file(drive, role):
    """ROLE.TXT is written once at rig setup, only after the console's own
    psp_model line has been read off that card.  A drive letter that drifted
    (they do) shows up here, before anything is staged."""
    p = os.path.join(card_root(drive), "ROLE.TXT")
    try:
        txt = open(p, encoding="utf-8", errors="replace").read().strip()
    except OSError:
        return False, "no ROLE.TXT"
    if not txt.lower().startswith(role):
        return False, "ROLE.TXT says %r" % txt
    return True, txt


def stage_arm(arm_dirs, arm, dest_root, role, run_id):
    """Stage one arm and stamp its harness ini with arm + run_id."""
    staged = stage_files(arm_dirs[arm], dest_root, role)
    ini = os.path.join(dest_root, ".gpsp-harness.ini")
    if os.path.isfile(ini):
        with open(ini, "a", encoding="utf-8") as fh:
            fh.write("\nrun_id = %s\narm = %s\n" % (run_id, arm))
        staged.append(".gpsp-harness.ini+run_id=%s,arm=%s" % (run_id, arm))
    else:
        log("  %s: arm %s staged NO .gpsp-harness.ini -- the run cannot name "
            "its arm" % (role, arm))
    return staged


def stage_files(stage_dir, dest_root, role):
    """Mirror stage_dir onto the card, honouring host-/join- prefixes.

    Walks recursively, because the things worth staging are not all in the app
    root: the save lives in roms/EMERALD.SAV.  A file's BASENAME carries the
    prefix, so `stage/roms/join-EMERALD.SAV` lands as `roms/EMERALD.SAV` on the
    client only.  Returns the relative paths written."""
    if not stage_dir or not os.path.isdir(stage_dir):
        return []
    copied = []
    for cur, _dirs, files in os.walk(stage_dir):
        rel_dir = os.path.relpath(cur, stage_dir)
        rel_dir = "" if rel_dir == "." else rel_dir
        for name in sorted(files):
            target = name
            for pfx in ("host-", "join-"):
                if name.startswith(pfx):
                    if pfx[:-1] != role:
                        target = None
                    else:
                        target = name[len(pfx):]
                    break
            if target is None:
                continue
            if target == name and target in ROLE_CRITICAL:
                log("  REFUSED to stage unprefixed %s -- it sets the console's "
                    "role and would make both cards the same. Rename it "
                    "host-%s / join-%s." % (target, target, target))
                continue
            dst_dir = os.path.join(dest_root, rel_dir) if rel_dir else dest_root
            os.makedirs(dst_dir, exist_ok=True)
            if not copy_retry(os.path.join(cur, name),
                              os.path.join(dst_dir, target), why="stage"):
                continue
            rel = os.path.join(rel_dir, target) if rel_dir else target
            copied.append("%s@%s" % (rel, _md5_8(os.path.join(cur, name))))
    return copied


def _md5_8(path):
    """First 8 hex of a file's md5, for the `staged:` log line.

    WHY: the log used to record staged FILENAMES only -- never content -- so it
    could not distinguish "staged fixture v2" from "staged v5", and the only
    timestamp in it that looks like a run boundary (`run N: waiting`) is printed
    AFTER staging.  During the IDLEPOLL campaign that combination misdated three
    runs: each time a .inputs file was rewritten a few seconds after the loop had
    already copied the previous version onto the cards, and the log gave no way
    to tell.  Run 9 was filed as fixture v3 while the card held v2 and only an
    opcode mismatch in its ap_fail line caught it.

    With this, every run is self-identifying from its own log line -- the same
    principle ab_compare.py's config_of() already relies on: filenames lie,
    content does not.  Cost is one read of each staged file per run, against a
    copy of the same file that just happened."""
    h = hashlib.md5()
    try:
        with open(path, "rb") as fh:
            for blk in iter(lambda: fh.read(1 << 20), b""):
                h.update(blk)
    except OSError:
        return "????????"
    return h.hexdigest()[:8]


def copy_retry(src, dst, attempts=5, why=""):
    """shutil.copy2 that survives a transient Windows file lock.

    A 12-run batch died on `PermissionError: [WinError 32] ... being used by
    another process` while restoring a golden save. On this rig the cards and
    the golden directory both sit under OneDrive, and a sync pass, an antivirus
    scan or a not-yet-closed handle can hold a file for a moment. That is a
    HICCUP, not a reason to abandon a night of runs -- especially since every
    caller here is idempotent and the next cycle would have retried anyway.

    Retries with a short backoff, then gives up LOUDLY and returns False. It
    never raises: a copy failure must degrade one cycle, not kill the loop."""
    for attempt in range(1, attempts + 1):
        try:
            shutil.copy2(src, dst)
            if attempt > 1:
                log("  copied %s on attempt %d" % (os.path.basename(dst), attempt))
            return True
        except (OSError, PermissionError) as e:
            if attempt == attempts:
                log("  COPY FAILED after %d attempts: %s -> %s (%s)%s"
                    % (attempts, os.path.basename(src), dst, e,
                       " [%s]" % why if why else ""))
                return False
            time.sleep(0.6 * attempt)
    return False


def restore_save(golden_dir, dest_root, role):
    """Put the save back to a known state before the next run.

    THE TRADE MUTATES THE SAVE.  Run 2 begins with the mons already swapped, so
    a position-driven input script walks into a different menu and 'ten
    identical runs' measures ten different game states.  Without this, repeated
    runs are not repeated -- which is the entire premise of the loop.

    Restored AFTER the run's own SRAM flush (fe_host_shutdown) and BEFORE the
    relaunch, which is exactly the window where the PC owns the filesystem."""
    if not golden_dir or not os.path.isdir(golden_dir):
        return []
    done = []
    for name in sorted(os.listdir(golden_dir)):
        src = os.path.join(golden_dir, name)
        if not os.path.isfile(src):
            continue
        for pfx in ("host-", "join-"):
            if name.startswith(pfx):
                if pfx[:-1] != role:
                    break
                dst = os.path.join(dest_root, "roms", name[len(pfx):])
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if copy_retry(src, dst, why="golden restore"):
                    done.append(os.path.basename(dst))
                break
    return done


def party_of(sav, game=None):
    """Personalities of the party in a .sav, via the existing trade oracle.

    Returns None if the oracle cannot read it -- which must NOT be treated as
    'unchanged'.  A save we failed to decode is an unknown, not a pass."""
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "e2e", "read_party.py")
    if not os.path.isfile(tool) or not os.path.isfile(sav):
        return None
    try:
        cmd = [sys.executable, tool, sav]
        if game:
            cmd.append("--game=" + game)
        # game=None lets read_party.py auto-detect the SaveBlock1 layout.
        out = subprocess.run(cmd, capture_output=True, timeout=60, text=True)
    except Exception:                                   # noqa: BLE001
        return None
    if out.returncode != 0:
        return None
    pers = re.findall(r"personality=(0x[0-9a-fA-F]+)", out.stdout)
    return pers or None


def verify_trade(golden_sav, post_sav, role, run, game=None):
    """Did the party actually CHANGE? `exit=0` only proves the script finished.

    This is deliberately a weak oracle -- it asserts the party is not identical
    to the one the run started from, which is what distinguishes 'a trade
    happened' from 'the console idled and exited cleanly'.  The full swap check
    (who received whose mon) lives in run_trade_test_psp.sh and needs both
    consoles' pre-state; this needs only the golden baseline, so it can run
    unattended on every link of the chain."""
    before = party_of(golden_sav, game)
    after = party_of(post_sav, game)
    if before is None or after is None:
        return None, "oracle could not read the save(s)"
    if before == after:
        return False, "party identical to the starting save -- no trade occurred"
    return True, "party changed (%d mons -> %d)" % (len(before), len(after))


# The PSP application directory on the card.  This was hardcoded as
# "gpsp-adhoc" in two places while stage_cards.ps1 and push_scripts.ps1 both
# wrote to "gpsp-harness" -- the directory the consoles actually run from.  The
# collector therefore polled a path whose handoff/ dir is permanently empty and
# never saw a finished run, so every "unattended" chain silently did nothing
# while completed runs sat unread on the cards.  One name, one place, and the
# CLI can override it.
DEFAULT_APPDIR = "GBADHOC-RIG"
APPDIR = DEFAULT_APPDIR


def card_root(drive):
    return os.path.join(drive + os.sep, "PSP", "GAME", APPDIR)


def collect(drive, role, run, logs_dir, stage_dir, more_runs, golden_dir=None,
            game=None,
            verify=False, arm_dirs=None, next_run_id=None):
    """`run` is the PC's loop index and is NOT used to name anything: it restarts
    at 1 every invocation, so a second batch silently overwrote the first
    batch's logs.  The console's own run counter (RESULT.TXT `run=`) is the
    authoritative, monotonic identity and is what files are named by."""
    root = card_root(drive)
    hand = os.path.join(root, "handoff")
    # Belt and braces to the readiness-poll fix above: if the result is present
    # but still incomplete, WAIT for it rather than returning None. Returning
    # None here skips the log copy and skips writing CMD.TXT, which parks a
    # console that had in fact finished a run and throws its log away.
    for _attempt in range(15):
        res = parse_result(os.path.join(hand, "RESULT.TXT"))
        if res is not None:
            break
        time.sleep(1.0)
    if res is None:
        log("  %s %-4s RESULT.TXT unreadable after 15s -- skipping this cycle"
            % (drive, role))
        return None

    # NAME BY THE PC'S CYCLE, NOT THE CONSOLE'S OWN RUN COUNTER.
    #
    # Each console keeps its own RUNS.TXT and they DRIFT: observed host=42
    # while join=41 in the same cycle, after the host's counter jumped by two.
    # Naming logs from that counter means the two halves of one physical run
    # get DIFFERENT filenames, so autoNNN-host.log and autoNNN-join.log stop
    # describing the same run -- and any analysis that pairs them by number
    # silently compares two different runs.  The PC services both consoles in
    # one cycle by construction, so its own counter is the only number that
    # is guaranteed to identify a pair.
    #
    # The console's own run number is kept in the log line below and inside
    # RESULT.TXT, so nothing is lost.
    tag = str(run)
    console_run = res.get("run", "?")
    arm = _ARM_OF_RUN.get(run, "x")
    stem = "auto%03d-%s-%s" % (int(tag), arm, role)
    src_log = os.path.join(root, "log", "frontend.log")
    # A relaunch-preserved log (console copied an uncollected frontend.log)
    prev = os.path.join(root, "log", "frontend.prev.log")
    if os.path.isfile(prev):
        if copy_retry(prev, os.path.join(logs_dir, stem + ".prev.log"),
                      why="collect prev log"):
            try:
                os.remove(prev)
            except OSError:
                pass
            log("  %s %-4s collected frontend.prev.log (an earlier run's "
                "uncollected log)" % (drive, role))
    crc, md5 = _crc32_md5(os.path.join(root, "EBOOT.PBP"))
    _pc, prx_md5 = _crc32_md5(os.path.join(root, "gbadhoc_me.prx"))
    meta = {"cycle": run, "drive": drive, "role": role, "arm": arm,
            "console_run": console_run, "eboot_crc32": crc, "eboot_md5": md5,
            "prx_md5": prx_md5, "result": res,
            "collected": time.strftime("%Y-%m-%d %H:%M:%S")}
    try:
        with open(os.path.join(logs_dir, stem + ".meta.json"), "w") as fh:
            json.dump(meta, fh, indent=1)
    except OSError as e:
        log("  %s %-4s META WRITE FAILED: %s" % (drive, role, e))
    if os.path.isfile(src_log):
        dst = os.path.join(logs_dir, stem + ".log")
        if copy_retry(src_log, dst, why="collect log"):
            check_model(drive, role, dst)
        if not os.path.isfile(dst):
            # Do NOT delete what we failed to copy. Leaving it on the card means
            # the next cycle can still collect it; deleting it would destroy the
            # only evidence of a run because of a transient lock.
            log("  %s %-4s log copy FAILED -- left on the card for the next cycle"
                % (drive, role))
        else:
            try:
                os.remove(src_log)      # next run starts clean, like the manual flow
            except OSError:
                pass
            log("  %s %-4s log -> %s (console run=%s) (%d bytes)"
                % (drive, role, os.path.basename(dst), console_run,
                   os.path.getsize(dst)))
    else:
        log("  %s %-4s NO LOG -- run produced nothing to read" % (drive, role))

    # COLLECT THE SCREENSHOTS TOO.  `dump` writes <appdir>/log/frame_NNNNNN.bmp
    # (main_psp.c), and this function used to copy frontend.log and the saves
    # and nothing else -- so every BMP stayed on the card until the next run's
    # housekeeping wiped it.  A screenshot is often the only artifact that says
    # WHICH menu entry the cursor was on, which is precisely the question the
    # log cannot answer.  Same naming rule as the log: the console's run number.
    shots = []
    logdir = os.path.join(root, "log")
    if os.path.isdir(logdir):
        for name in sorted(os.listdir(logdir)):
            if name.lower().endswith(".bmp"):
                dst = os.path.join(logs_dir,
                                   "auto%03d-%s-%s" % (int(tag), role, name))
                if not copy_retry(os.path.join(logdir, name), dst,
                                  why="collect screenshot"):
                    continue
                try:
                    os.remove(os.path.join(logdir, name))
                except OSError:
                    pass
                shots.append(os.path.basename(dst))
    if shots:
        log("  %s %-4s %d screenshot(s): %s"
            % (drive, role, len(shots), ", ".join(shots)))

    # PRESERVE THE OUTCOME BEFORE ERASING IT.
    #
    # The golden restore below is what makes runs repeatable -- and it would
    # overwrite the only evidence that the trade actually happened.  The party
    # oracle (tools/e2e/read_party.py) decodes the .sav to prove the mons
    # really swapped; `exit=0` proves only that the input script ran to the end.
    # A chain that reports ten successes without a single verified trade is the
    # vacuous-gate failure at scale, so the post-run save comes off the card
    # first and lives beside the log.
    saved = []
    romdir = os.path.join(root, "roms")
    if os.path.isdir(romdir):
        for name in sorted(os.listdir(romdir)):
            if name.lower().endswith(".sav"):
                dst = os.path.join(logs_dir, "auto%03d-%s-%s" % (int(tag), role, name))
                if not copy_retry(os.path.join(romdir, name), dst,
                                  why="collect save"):
                    continue
                saved.append(os.path.basename(dst))
    if saved:
        log("  %s %-4s post-run save kept: %s" % (drive, role, ", ".join(saved)))

    if verify and golden_dir and saved:
        # The golden save is whatever <role>-*.sav the golden dir holds --
        # hardcoding EMERALD silently mis-verified any other game.
        gold = None
        for gname in sorted(os.listdir(golden_dir)):
            if (gname.lower().startswith(role + "-")
                    and gname.lower().endswith(".sav")):
                gold = os.path.join(golden_dir, gname)
                break
        ok, why = (None, "no %s-*.sav in --golden" % role) if not gold else             verify_trade(gold, os.path.join(logs_dir, saved[0]), role, run, game)
        if ok is True:
            log("  %s %-4s TRADE VERIFIED: %s" % (drive, role, why))
        elif ok is False:
            log("  %s %-4s TRADE NOT VERIFIED: %s" % (drive, role, why))
        else:
            log("  %s %-4s trade unverifiable: %s" % (drive, role, why))

    restored = restore_save(golden_dir, root, role)
    if restored:
        log("  %s %-4s save restored: %s" % (drive, role, ", ".join(restored)))
    elif golden_dir:
        log("  %s %-4s NO SAVE RESTORED -- runs will drift apart" % (drive, role))

    if arm_dirs:
        staged = stage_arm(arm_dirs, _NEXT_ARM[0], root, role, next_run_id)
    else:
        staged = stage_files(stage_dir, root, role)
    if staged:
        log("  %s %-4s staged: %s" % (drive, role, ", ".join(staged)))

    if _MODEL_MISMATCH:
        more_runs = False           # park the console; never run the wrong role
    with open(os.path.join(hand, "CMD.TXT"), "w") as fh:
        fh.write("RUN\n" if more_runs else "STOP\n")
    # The console keys off RESULT.TXT being gone; leaving it would let a
    # crashed console look like a finished one on the next poll.
    try:
        os.remove(os.path.join(hand, "RESULT.TXT"))
    except OSError:
        pass
    return res


def main():
    ap = argparse.ArgumentParser()
    # SOLO MODE: pass "" for either console to drop it from the cycle.  A
    # render-only experiment (frame hashes, ME timings) needs no peer, and
    # when one console is wedged the other is still perfectly good for it --
    # the alternative, pointing both roles at one drive, collects that card
    # twice per cycle and races its own log copy.
    ap.add_argument("--host", default="D:",
                    help='drive of the HOST console, or "" to run without one')
    ap.add_argument("--join", default="E:",
                    help='drive of the JOIN console, or "" to run without one')
    ap.add_argument("--runs", type=int, default=5,
                    help="how many runs to service, then send STOP. Use 0 for "
                         "--forever: never send STOP, keep the consoles alive.")
    ap.add_argument("--forever", action="store_true",
                    help="service runs indefinitely and NEVER send STOP. The "
                         "consoles stay plugged in and in the loop; work is "
                         "driven by dropping files into --stage between runs. "
                         "This is the normal mode -- a chain that terminates "
                         "leaves the consoles at the XMB needing a human.")
    ap.add_argument("--stage", default=None)
    ap.add_argument("--golden", default=None,
                    help="directory of host-/join- prefixed .SAV files restored "
                         "into roms/ before every run. Without this the trade "
                         "mutates the save and runs are not repeatable.")
    ap.add_argument("--logs", default=os.path.join(
        os.path.expanduser("~"), "OneDrive", "Desktop", "Logs"))
    ap.add_argument("--game", default=None,
                    choices=("emerald", "frlg"),
                    help="SaveBlock1 layout the party oracle should assume. "
                         "Omit to auto-detect. Emerald and FR/LG put the party "
                         "count and array at different offsets, so decoding a "
                         "FR/LG save with Emerald's layout yields nonsense that "
                         "reads as 'party unchanged' -- i.e. a silent NOT "
                         "VERIFIED on a trade that did happen.")
    ap.add_argument("--verify", action="store_true",
                    help="after each run, decode the post-run save and require "
                         "the party to differ from the golden baseline. Without "
                         "this a chain reports successes that prove only that "
                         "the input script reached its last line.")
    ap.add_argument("--keep-going", action="store_true",
                    help="continue the chain even after a nonzero exit")
    ap.add_argument("--i-know-no-eject-is-unsafe-on-hardware",
                    dest="no_eject_ack", action="store_true",
                    help=argparse.SUPPRESS)
    ap.add_argument("--no-eject", action="store_true",
                    help="do not release the volume; the console then waits out "
                         "its full handoff window. Use when the cards are "
                         "mounted for other work and must not be yanked.")
    ap.add_argument("--appdir", default=DEFAULT_APPDIR,
                    help="PSP/GAME/<dir> the consoles run from. Must match "
                         "stage_cards.ps1 and push_scripts.ps1 (default: %s)"
                         % DEFAULT_APPDIR)
    ap.add_argument("--eject-retries", dest="eject_retries", type=int, default=3,
                    help="eject attempts per cycle before giving up. 1 restores "
                         "the previous single-shot behaviour exactly (default: 3)")
    ap.add_argument("--arms", default=None,
                    help="A=<stageDir>,B=<stageDir>: one stage dir per arm")
    ap.add_argument("--order", default="ABBA",
                    help="arm letters, cycled (default ABBA)")
    ap.add_argument("--allow-appdir", action="store_true",
                    help="permit an --appdir other than %s" % RIG_APPDIR)
    ap.add_argument("--poll", type=float, default=2.0)
    ap.add_argument("--timeout", type=float, default=1800,
                    help="seconds to wait for BOTH consoles before giving up")
    args = ap.parse_args()

    global APPDIR
    APPDIR = args.appdir
    if APPDIR != RIG_APPDIR and not args.allow_appdir:
        print("REFUSING: --appdir %s -- the rig only ever writes to PSP/GAME/%s,"
              " so the owner's own install cannot be touched." % (APPDIR, RIG_APPDIR))
        return 2
    arm_dirs = None
    if args.arms:
        arm_dirs = {}
        for part in args.arms.split(","):
            k, _, v = part.partition("=")
            if not k or not os.path.isdir(v):
                print("REFUSING: bad --arms entry %r" % part)
                return 2
            arm_dirs[k.strip()] = v
        if not args.order or any(c not in arm_dirs for c in args.order):
            print("REFUSING: --order %r names an arm not in --arms" % args.order)
            return 2
        if args.stage:
            print("REFUSING: --stage and --arms together; an arm IS a stage")
            return 2

    # A wrong app dir is silent: handoff/RESULT.TXT simply never appears and the
    # loop waits out its full --timeout looking like patient, working software.
    # That is exactly how the gpsp-adhoc/gpsp-harness split went unnoticed, so
    # say something up front rather than time out in half an hour.
    #
    # But an UNMOUNTED card is not an error: starting the collector while a run
    # is already in flight is the normal way to attach to one, and the volume
    # only appears when the console re-exports it.  So only refuse when the card
    # is demonstrably present and the app dir is demonstrably not on it.
    drives = [(d, r) for d, r in ((args.host, "host"), (args.join, "join")) if d]
    if not drives:
        print("REFUSING: both --host and --join are empty; nothing to service.")
        return 2
    for drive, role in drives:
        if not os.path.isdir(os.path.join(drive + os.sep, "PSP", "GAME")):
            log("  %s (%s) not mounted yet -- will wait for it" % (drive, role))
            continue
        ok, why = check_role_file(drive, role)
        if not ok:
            print("REFUSING: %s serviced as %s but %s" % (drive, role, why))
            return 2
        if not os.path.isdir(os.path.join(card_root(drive), "handoff")):
            print("REFUSING: %s (%s) is mounted but has no PSP/GAME/%s/handoff"
                  % (drive, role, APPDIR))
            print("The consoles run from PSP/GAME/<appdir>; pass --appdir if")
            print("this rig uses a different directory.")
            return 2

    # --no-eject on REAL HARDWARE is a data-loss hazard, not a convenience.
    # The console re-arms USB on a timer and sceUsbDeactivate yanks the device;
    # without an eject, anything Windows still has buffered is lost and FAT
    # metadata can be left inconsistent.  Ejecting flushes and unmounts, so the
    # PC is provably idle by the time the window expires.  The flag exists for
    # the rig, where the "volume" is a directory and there is nothing to eject.
    real_drives = any(len(d) == 2 and d[1] == ":" for d, _ in drives)
    if args.no_eject and real_drives and not args.no_eject_ack:
        print("REFUSING: --no-eject with real drive letters (%s)."
              % ", ".join(d for d, _ in drives))
        print("The console re-arms USB on a timer; without an eject a buffered")
        print("write is lost when it deactivates, and that has corrupted a card")
        print("on this project before. Drop --no-eject, or pass")
        print("--i-know-no-eject-is-unsafe-on-hardware if you truly mean it.")
        return 2

    os.makedirs(args.logs, exist_ok=True)
    log("watching %s for handoff/RESULT.TXT"
        % " and ".join("%s (%s)" % (d, r) for d, r in drives))
    if len(drives) == 1:
        log("SOLO MODE: one console. No wireless session is possible, so this "
            "services render-only experiments.")
    log("logs -> %s   stage -> %s   runs -> %d"
        % (args.logs, args.stage or "(none: same build)", args.runs))
    if not args.golden:
        log("WARNING: no --golden. The trade mutates the save, so runs will")
        log("         diverge and are NOT a repeated measurement.")

    forever = args.forever or args.runs == 0
    if forever:
        log("FOREVER mode: consoles will never be told to STOP.")
        log("Drop files into --stage between runs to change what they do.")
    run = 0
    while True:
        run += 1
        if not forever and run > args.runs:
            break
        # Both consoles must present a result before either is released:
        # releasing one early restarts it against a peer that is still parked
        # in USB mode, and the session it tries to open cannot succeed.
        log("run %d%s: waiting for both consoles..."
            % (run, "" if forever else "/%d" % args.runs))
        deadline = time.time() + args.timeout
        ready = {}
        # SAY SO WHILE BLOCKED.
        #
        # Both consoles must present a RESULT.TXT before either is released,
        # and a console only writes one after its main loop exits -- so a
        # console that crashes or hangs never hands off and this loop waits
        # out the whole --timeout in total silence.  Observed: "D: (host) is
        # up" logged, then nothing for two hours, with the stall detectable
        # only from the log file's mtime.  A stall that looks identical to
        # normal operation is the expensive kind, so name the console we are
        # still waiting for, on a cadence too slow to spam the log.
        waiting_since = time.time()
        next_nag = waiting_since + 300
        while time.time() < deadline and len(ready) < len(drives):
            now_t = time.time()
            if now_t >= next_nag:
                missing = ", ".join("%s (%s)" % (d, r)
                                    for d, r in drives if r not in ready)
                log("  still waiting %d min for %s -- a console that crashed "
                    "or hung never writes RESULT.TXT and will not appear"
                    % (int((now_t - waiting_since) / 60), missing or "?"))
                next_nag = now_t + 300
            for drive, role in drives:
                if role in ready:
                    continue
                p = os.path.join(card_root(drive), "handoff", "RESULT.TXT")
                # PARSE, DO NOT MERELY STAT.  Treating "the file exists" as
                # "the console is ready" reads a RESULT.TXT the console is
                # still writing: parse_result then returns None, collect()
                # bails before it copies anything AND before it writes
                # CMD.TXT, and the console parks -- deleting the frontend.log
                # of a run that had already completed.  Observed three times in
                # twenty minutes on E:, costing one join log outright and then
                # desyncing the pair so the next run had no peer.  The file is
                # complete when it has a status= line; until then keep polling.
                if parse_result(p) is not None:
                    ready[role] = True
                    log("  %s (%s) is up" % (drive, role))
            time.sleep(args.poll)

        if len(ready) < len(drives):
            log("TIMEOUT: only %d/%d consoles appeared. Stopping."
                % (len(ready), len(drives)))
            return 1

        # In forever mode every command is RUN.  STOP is what ends the chain
        # and drops the consoles back to the XMB, which is exactly what the
        # loop exists to avoid.
        #
        # PEEK BEFORE DECIDING.  `more` is written into CMD.TXT by collect(),
        # but whether this cycle is a real run is only known after reading
        # RESULT.TXT.  Computing it blind meant `--runs 1` against two PARKED
        # consoles wrote STOP -- parking them again without ever running once,
        # and the loop then exited reporting success.  A cycle that merely wakes
        # parked consoles must always command RUN.
        peek = {}
        for drive, role in drives:
            peek[role] = parse_result(
                os.path.join(card_root(drive), "handoff", "RESULT.TXT"))
        # REPORT COUNTER DRIFT.
        #
        # The consoles keep independent RUNS.TXT counters. They are supposed
        # to advance together because this loop services both in one cycle,
        # but they do drift (observed host=42 / join=41), and the drift is
        # invisible unless someone diffs two log filenames. Log naming no
        # longer depends on it, so drift is now cosmetic -- but it is also
        # the clearest signal that one console ran, crashed, or relaunched
        # without its peer, which is worth knowing about immediately.
        _rn = {r: (v or {}).get("run") for r, v in peek.items()}
        if len(_rn) == 2 and all(_rn.values()):
            try:
                _h, _j = int(_rn["host"]), int(_rn["join"])
                if _h != _j:
                    log("  NOTE: console run counters differ (host=%d join=%d, "
                        "delta %d) -- one console advanced without its peer. "
                        "Logs are paired by PC cycle, so this does not affect "
                        "analysis." % (_h, _j, _h - _j))
            except (TypeError, ValueError):
                pass
        waking = bool(peek) and all(
            v and v.get("status") == "parked" for v in peek.values())
        more = True if (forever or waking) else (run < args.runs)
        results = {}
        # The arm for the NEXT run.  Parked consoles (the first cycle) ran no
        # arm; every other cycle's arm is what was staged last time.
        if not waking and run not in _ARM_OF_RUN:
            _ARM_OF_RUN[run] = _NEXT_ARM[0] or "x"
        nxt = None
        next_run_id = None
        if arm_dirs:
            k = sum(1 for v in _ARM_OF_RUN.values() if v != "x")
            nxt = args.order[k % len(args.order)]
            _NEXT_ARM[0] = nxt
            next_run_id = "%d-%d" % (run + 1, int(time.time()))
            log("  next run: arm %s run_id %s" % (nxt, next_run_id))
        for drive, role in drives:
            ok, why = check_role_file(drive, role)
            if not ok:
                log("  ROLE MISMATCH on %s (%s): %s -- stopping" % (drive, role, why))
                return 2
            results[role] = collect(drive, role, run, args.logs,
                                    args.stage, more, golden_dir=args.golden,
                                    game=args.game, verify=args.verify,
                                    arm_dirs=arm_dirs, next_run_id=next_run_id)
        if _MODEL_MISMATCH:
            for d, r, got, want in _MODEL_MISMATCH:
                log("  MODEL MISMATCH on %s (%s): log says %s, ROLE.TXT says %s "
                    "-- STOPPING before another run" % (d, r, got, want))
            return 2
        if args.no_eject:
            log("  --no-eject: consoles will wait out their handoff window")
        else:
            for drive, _role in drives:
                eject(drive, attempts=args.eject_retries)

        bad = [r for r, v in results.items()
               if v and v.get("exit", "0") not in ("0", "")]
        parked = [r for r, v in results.items()
                  if v and v.get("status") == "parked"]
        if len(parked) == len(drives):
            # BOTH consoles were merely parked -- nothing ran.  Counting this
            # as a run made "--runs 2" deliver one actual run, because the
            # first iteration was spent waking consoles that were already idle.
            log("  both consoles were PARKED, not fresh from a run -- "
                "woke them, not counting this as run %d" % run)
            run -= 1
            continue
        if parked:
            log("  %s was PARKED (waiting for work), not fresh from a run"
                % ", ".join(parked))
        for role, v in results.items():
            if v:
                log("  %-4s run=%s exit=%s reason=%s"
                    % (role, v.get("run", "?"), v.get("exit", "?"),
                       v.get("reason", "?")))
        if bad and not args.keep_going and not forever:
            log("nonzero exit on %s -- stopping (use --keep-going to continue)"
                % ", ".join(bad))
            return 2
        if bad and forever:
            log("nonzero exit on %s -- continuing (forever mode)"
                % ", ".join(bad))
        if not more:
            log("chain complete: %d runs" % args.runs)

    return 0


if __name__ == "__main__":
    sys.exit(main())
