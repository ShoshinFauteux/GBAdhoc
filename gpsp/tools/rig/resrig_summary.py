#!/usr/bin/env python3
"""resrig_summary.py -- judge a resrig campaign, refusing anything it cannot vouch for.

    python tools/rig/resrig_summary.py --logs LOGS --stage STAGE \
        [--golden DRYRUN_LOGS] [--model PSP-Go] [--from 31] [--json OUT]

Every run directory (autoNNN-<arm>-solo/) is checked against the invariants
below.  A run that breaks one is INVALID, with the reason, and is never scored.
A metric whose source line is missing prints `unknown` -- never 0, never "did
not happen".

  I1 complete   boot_ok, `EVT exit`, RESULT.TXT status=ready.  Otherwise the
                run is FROZE/CRASH (a result, judged from heartbeat.txt).
  I2 identity   run_id and arm agree across the staged ini (meta.json), the
                log's `rig` line, the shash.txt header and every heartbeat line.
  I3 binary     `build eboot_crc` == meta eboot_crc32 == the stage manifest;
                the card's PRX md5 == the manifest's.
  I4 config     every staged harness key echoed `cfg src=harness key= raw=`
                with the staged value; zero cfg_unknown; the APPLIED values
                (rom_diag, rom_cache_cap, rom_cache, me_mode) are what the arm
                asked for.
  I6 log loss   evt_gap markers, `evt_drop total`, RESULT evt_drop: reported
                per run.  The oracle (shash.txt) does not go through the ring,
                so drops thin only the SMC census, whose missing windows read
                `unknown`.
  I8 device     `psp_model` is the console this rig is for (--model).

OUTCOME.  Liveness comes from RESULT.TXT frames= and heartbeat.txt (written by
a 0x10 thread with raw sceIo), never from how long frontend.log is.  The oracle
is shash.txt: from --from (the first frame after the savestate load; frames
before it carry the RTC's wall-clock seconds and differ by design) every line
must equal the reference -- the PPSSPP golden when --golden is given, and the
same campaign's CTRL runs as a second reference.  The SMC census (`core_prof
win=` per 600 frames) is compared window by window, as in the 2026-09-16
analysis.
"""
import argparse
import json
import os
import re
import sys

ARM_DEFAULTS = {"rom_cap": "0", "swap_stubs": "0", "cache_paranoid": "0",
                "jit_coherency_scan": "0", "rom_ballast_kb": "0"}


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return None


def kv_line(text, prefix):
    """The LAST `EVT <prefix> k=v ...` line as a dict, or None."""
    out = None
    if text is None:
        return None
    for m in re.finditer(r"^EVT %s (.*)$" % re.escape(prefix), text, re.M):
        out = dict(re.findall(r"(\w+)=(\S+)", m.group(1)))
    return out


def shash_lines(path):
    """{frame: line} and the header, or (None, None) when absent."""
    body = read(path)
    if body is None:
        return None, None
    header, lines = None, {}
    for ln in body.splitlines():
        if ln.startswith("#"):
            header = ln
            continue
        m = re.match(r"f=(\d+) (.*)$", ln)
        if m:
            lines[int(m.group(1))] = m.group(2)
    return header, lines


def smc_windows(text):
    """core_prof win=rom/full/smc/dma/page per 600-frame window."""
    if text is None:
        return None
    return re.findall(r"EVT core_prof core=\S+ win=(\S+)", text)


def field_diff(a, b):
    fa = dict(x.split("=", 1) for x in a.split())
    fb = dict(x.split("=", 1) for x in b.split())
    return [k for k in fa if fa.get(k) != fb.get(k)]


def compare(lines, ref, start):
    """First frame >= start where the run and the reference disagree."""
    if lines is None or ref is None:
        return {"verdict": "unknown", "why": "shash missing"}
    common = sorted(f for f in ref if f >= start and f in lines)
    for f in common:
        if lines[f] != ref[f]:
            return {"verdict": "DIVERGED", "frame": f,
                    "fields": field_diff(lines[f], ref[f]),
                    "compared": common.index(f)}
    last_ref = max((f for f in ref if f >= start), default=None)
    last = max(lines) if lines else None
    if last_ref is not None and (last is None or last < last_ref):
        return {"verdict": "IDENTICAL_UNTIL_END_OF_RUN", "through": last,
                "compared": len(common), "ref_last": last_ref}
    return {"verdict": "IDENTICAL", "through": last, "compared": len(common)}


def judge(run_dir, man, model, start):
    inv, notes = [], []
    meta = json.loads(read(os.path.join(run_dir, "meta.json")) or "{}")
    log = read(os.path.join(run_dir, "frontend.log"))
    arm, rid = meta.get("arm", "unknown"), meta.get("run_id", "unknown")
    staged = meta.get("staged_ini", {})
    res = meta.get("result") if isinstance(meta.get("result"), dict) else {}
    r = {"dir": os.path.basename(run_dir), "arm": arm, "run_id": rid,
         "cycle": meta.get("cycle")}

    # I1 -- complete?
    complete = (log is not None and "EVT boot_ok" in log and
                re.search(r"^EVT exit ", log or "", re.M) is not None and
                res.get("status") == "ready")
    r["frames"] = res.get("frames", "unknown")
    r["exit"] = "%s/%s" % (res.get("exit", "unknown"), res.get("reason", "unknown"))

    # I2 -- identity everywhere
    rig = kv_line(log, "rig")
    header, lines = shash_lines(os.path.join(run_dir, "shash.txt"))
    hb = read(os.path.join(run_dir, "heartbeat.txt"))
    if not rig or rig.get("run_id") != rid or rig.get("arm") != arm:
        inv.append("I2 log rig line %s != staged %s/%s" % (rig, rid, arm))
    if header is None or ("run_id=%s " % rid) not in header + " " or \
            ("arm=%s " % arm) not in header + " ":
        inv.append("I2 shash header %r != %s/%s" % (header, rid, arm))
    if hb:
        bad = [l for l in hb.splitlines() if l.strip() and
               ("run_id=%s " % rid not in l or "arm=%s " % arm not in l)]
        if bad:
            inv.append("I2 %d heartbeat line(s) from another run" % len(bad))

    # I3 -- the binary
    want_crc = man["files"]["EBOOT.PBP"]["crc32"]
    build = kv_line(log, "build")
    got = (build or {}).get("eboot_crc", "unknown")
    if not (got == meta.get("eboot_crc32") == want_crc):
        inv.append("I3 eboot crc log=%s card=%s manifest=%s"
                   % (got, meta.get("eboot_crc32"), want_crc))
    if meta.get("prx_md5") != man["files"]["gbadhoc_me.prx"]["md5"]:
        inv.append("I3 prx md5 card=%s manifest=%s"
                   % (meta.get("prx_md5"), man["files"]["gbadhoc_me.prx"]["md5"]))
    if header and ("eboot_crc=%s" % want_crc) not in header:
        inv.append("I3 shash header binary %r" % header)

    # I4 -- config echoed, nothing unknown, applied as asked
    if log is not None:
        echoed = dict(re.findall(r"^EVT cfg src=harness key=(\S+) raw=(.*)$",
                                 log, re.M))
        for k, v in staged.items():
            if echoed.get(k) != v:
                inv.append("I4 %s staged=%s echoed=%s" % (k, v, echoed.get(k, "none")))
        unk = re.findall(r"^EVT cfg_unknown (.*)$", log, re.M)
        if unk:
            inv.append("I4 cfg_unknown: %s" % "; ".join(unk))
        keys = dict(ARM_DEFAULTS)
        keys.update({k: v for k, v in staged.items() if k in ARM_DEFAULTS})
        diag = kv_line(log, "rom_diag") or {}
        applied = {"rom_cap": diag.get("rom_cap"),
                   "swap_stubs": diag.get("swap_stubs"),
                   "cache_paranoid": diag.get("cache_paranoid"),
                   "jit_coherency_scan": diag.get("jit_coh_every"),
                   "rom_ballast_kb": diag.get("ballast_kb")}
        for k, v in keys.items():
            if applied[k] != v:
                inv.append("I4 applied %s=%s, staged %s" % (k, applied[k], v))
        want_blocks = "32" if keys["rom_cap"] == "32" else "15"
        cap = kv_line(log, "rom_cache_cap") or {}
        rc = kv_line(log, "rom_cache") or {}
        if cap.get("blocks") != want_blocks or rc.get("blocks") != want_blocks:
            inv.append("I4 rom cache cap=%s blocks=%s, want %s"
                       % (cap.get("blocks"), rc.get("blocks"), want_blocks))
        if rc.get("resident") != ("1" if want_blocks == "32" else "0"):
            inv.append("I4 resident=%s with %s blocks" % (rc.get("resident"),
                                                         want_blocks))
        if keys["rom_ballast_kb"] != "0" and int(rc.get("lo", "0"), 16) < 0x0A000000:
            inv.append("I4 ballast did not move the ROM above 0x0A000000 (lo=%s)"
                       % rc.get("lo"))
        want_me = staged.get("me_mode", "1")
        mm = kv_line(log, "me_mode") or {}
        if mm.get("on") != want_me:
            inv.append("I4 me_mode on=%s, want %s" % (mm.get("on"), want_me))
        r["rom"] = "%s blk %s-%s" % (rc.get("blocks", "?"), rc.get("lo", "?"),
                                     rc.get("hi", "?"))
        r["me"] = (kv_line(log, "me_init") or {}).get("state", "unknown")

    # I6 -- log loss, made visible
    gaps = [int(x) for x in re.findall(r"^EVT evt_gap dropped=(\d+)", log or "", re.M)]
    tot = kv_line(log, "evt_drop")
    r["evt_drop"] = "log=%s result=%s gaps=%d(%d)" % (
        (tot or {}).get("total", "unknown"), res.get("evt_drop", "unknown"),
        len(gaps), sum(gaps))

    # I8 -- the device
    pm = kv_line(log, "psp_model") or {}
    r["model"] = pm.get("name", "unknown")
    if model and r["model"] != model:
        inv.append("I8 psp_model=%s, rig is for %s" % (r["model"], model))

    # Liveness, from the heartbeat, not the log.  heartbeat.txt is APPENDED
    # across boots, so after a freeze + relaunch it holds both attempts; a
    # boot is where t_ms (time since power-on) goes backwards.
    allhb = [dict(re.findall(r"(\w+)=(\S+)", l)) for l in (hb or "").splitlines()
             if l.startswith("hb ")]
    boots, cur = [], []
    for b in allhb:
        if cur and int(b.get("t_ms", 0)) < int(cur[-1].get("t_ms", 0)):
            boots.append(cur)
            cur = []
        cur.append(b)
    if cur:
        boots.append(cur)
    hbl = boots[-1] if boots else []
    if hbl:
        last = hbl[-1]
        moving = len(hbl) > 1 and hbl[-1].get("frame") != hbl[-2].get("frame")
        r["heartbeat"] = "%d beats, last s=%s frame=%s iter=%s phase=%s%s" % (
            len(hbl), last.get("s"), last.get("frame"), last.get("iter"),
            last.get("phase"), "" if moving or complete else " (frame STUCK)")
    else:
        r["heartbeat"] = "unknown"

    # THE FROZEN ATTEMPT.  A cycle whose first attempt produced no RESULT.TXT
    # (meta froze_before, or a preserved .prev file) is reported as a freeze
    # WITH its own evidence -- never hidden behind the retry's clean result.
    p_header, p_lines = shash_lines(os.path.join(run_dir, "shash.prev.txt"))
    froze_before = bool(meta.get("froze_before")) or p_lines is not None or         os.path.isfile(os.path.join(run_dir, "frontend.prev.log"))
    r["froze_before"] = froze_before
    if froze_before:
        first = boots[0] if len(boots) > 1 else []
        r["prior_attempt"] = {
            "shash_lines": len(p_lines) if p_lines is not None else "unknown",
            "last_frame": max(p_lines) if p_lines else "unknown",
            "heartbeat": ("%d beats, last frame=%s iter=%s phase=%s" % (
                len(first), first[-1].get("frame"), first[-1].get("iter"),
                first[-1].get("phase")) if first else "unknown"),
            "identity": p_header or "unknown"}
        if p_header and (("run_id=%s " % rid) not in p_header + " "):
            inv.append("I2 shash.prev.txt belongs to another run: %r" % p_header)
    r["_prev_lines"] = p_lines
    r["shash_lines"] = len(lines) if lines is not None else "unknown"
    r["smc"] = smc_windows(log)
    r["audio_hash"] = (re.findall(r"^EVT audio_hash (\S+)", log or "", re.M)
                       or ["unknown"])[-1]
    r["jit_coh"] = re.findall(r"^EVT jit_coh (.*)$", log or "", re.M)[-1:] or []
    r["complete"] = complete
    r["invalid"] = inv
    r["_lines"] = lines
    return r


def load_ref(dirs, man, model, start):
    refs = []
    for d in dirs:
        rr = judge(d, man, model, start)
        if not rr["invalid"] and rr["complete"]:
            refs.append(rr)
    return refs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", required=True)
    ap.add_argument("--stage", required=True)
    ap.add_argument("--golden", help="a resrig logs dir from the PPSSPP dry run")
    ap.add_argument("--model", default="PSP-Go")
    ap.add_argument("--golden-model", default="PSP-2000")
    ap.add_argument("--from", dest="start", type=int, default=31)
    ap.add_argument("--json")
    a = ap.parse_args()
    with open(os.path.join(a.stage, "MANIFEST.json")) as fh:
        man = json.load(fh)

    def runs_in(d):
        return sorted(os.path.join(d, n) for n in os.listdir(d)
                      if n.startswith("auto") and os.path.isdir(os.path.join(d, n)))

    golden = None
    if a.golden:
        g = [x for x in load_ref(runs_in(a.golden), man, a.golden_model, a.start)
             if x["arm"] == "CTRL"]
        if not g:
            print("REFUSING: --golden has no valid, complete CTRL run")
            return 2
        golden = g[0]
    runs = [judge(d, man, a.model, a.start) for d in runs_in(a.logs)]
    ctrl = [x for x in runs if x["arm"] == "CTRL" and not x["invalid"] and x["complete"]]

    print("resrig summary: %d runs, stage EBOOT crc %s, oracle from frame %d"
          % (len(runs), man["files"]["EBOOT.PBP"]["crc32"], a.start))
    print("reference: %s; CTRL runs on this device: %d"
          % ("PPSSPP golden %s" % golden["dir"] if golden else "none (no --golden)",
             len(ctrl)))
    out = []
    # Taken before the loop: it pops each run's lines, CTRL's included.
    ctrl_lines = ctrl[0]["_lines"] if ctrl else None
    golden_lines = golden["_lines"] if golden else None
    for r in runs:
        lines = r.pop("_lines")
        if r["invalid"]:
            verdict = "INVALID"
        else:
            vs_g = compare(lines, golden_lines, a.start) if golden else None
            vs_c = compare(lines, ctrl_lines, a.start) \
                if ctrl and ctrl[0]["dir"] != r["dir"] else None
            r["vs_golden"], r["vs_ctrl"] = vs_g, vs_c
            if golden and r["smc"] is not None:
                g = golden["smc"] or []
                n = min(len(g), len(r["smc"]))
                r["smc_vs_golden"] = ("same %d windows" % n
                                      if r["smc"][:n] == g[:n] else
                                      "DIFFERS at window %d" % next(
                                          i for i in range(n) if r["smc"][i] != g[i]))
                if len(r["smc"]) < len(g):
                    r["smc_vs_golden"] += ", %d window(s) unknown" % (len(g) - len(r["smc"]))
            primary = vs_g or vs_c or {"verdict": "unknown"}
            if not r["complete"]:
                verdict = "FROZE/CRASH"
            elif primary["verdict"] == "DIVERGED":
                verdict = "DIVERGED f=%d %s" % (primary["frame"],
                                                ",".join(primary["fields"]))
            elif primary["verdict"] == "IDENTICAL":
                verdict = "PASS"
            else:
                verdict = primary["verdict"]
        prev = r.pop("_prev_lines", None)
        if r.get("froze_before"):
            pv = compare(prev, golden_lines or ctrl_lines, a.start)                 if (golden_lines or ctrl_lines) else {"verdict": "unknown"}
            r["prior_vs_reference"] = pv
            if pv.get("verdict") == "DIVERGED":
                what = "diverged f=%d %s" % (pv["frame"], ",".join(pv["fields"]))
            elif prev:
                what = "identical through f=%s" % max(prev)
            else:
                what = "oracle unknown"
            verdict = "FROZE (attempt 1: %s); retry: %s" % (what, verdict)
        r["verdict"] = verdict
        out.append(r)
        print("\n%s  arm=%s run_id=%s  %s" % (r["dir"], r["arm"], r["run_id"], verdict))
        for k in ("prior_attempt", "prior_vs_reference",
                  "model", "rom", "me", "frames", "exit", "shash_lines",
                  "heartbeat", "evt_drop", "audio_hash", "smc_vs_golden",
                  "vs_golden", "vs_ctrl", "jit_coh"):
            if k in r:
                print("   %-14s %s" % (k, r[k] if r[k] not in (None, [], "") else "unknown"))
        for i in r["invalid"]:
            print("   INVALID  %s" % i)
    print("\nPER ARM (valid runs only)")
    arms = []
    for r in out:
        if r["arm"] not in arms:
            arms.append(r["arm"])
    for arm in arms:
        rs = [r for r in out if r["arm"] == arm]
        print("  %-13s n=%d  %s" % (arm, len(rs), "; ".join(r["verdict"] for r in rs)))
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(out, fh, indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
