#!/usr/bin/env python3
"""test_resrig_summary.py -- the summarizer must refuse what it cannot vouch for.

Builds synthetic resrig run directories (a clean control, then one corruption
per invariant) and requires the verdict each deserves.  A summarizer that
passes a run with the wrong arm, a foreign binary, an un-echoed key or a
diverged guest state is worse than none -- that is what this proves it does
not do.  No PSP, no PPSSPP: pure files.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SUMMARY = os.path.join(HERE, "resrig_summary.py")
CRC, PRX = "2948398c", "0" * 32
FRAMES = 700


def shash_line(f, tweak=False):
    return ("f=%d a=%08x n=%d r=%08x pc=08000000 i=%08x e=%08x io=0 p=0 o=0 v=0"
            % (f, f * 7, f * 549, f, (f * 13) ^ (1 if tweak else 0), f * 17))


def make_run(root, name, arm, staged, *, log_arm=None, crc=CRC, echo=None,
             unknown=False, rom_diag=None, blocks="15", diverge_at=None,
             complete=True, shash=True, hb_stuck=False, froze_prev=None):
    d = os.path.join(root, name)
    os.makedirs(d)
    rid = staged["run_id"]
    echo = staged if echo is None else echo
    rd = {"rom_cap": "0", "swap_stubs": "0", "cache_paranoid": "0",
          "jit_coh_every": "0", "ballast_kb": "0"}
    rd.update(rom_diag or {})
    lines = ["EVT boot_ok", "EVT psp_model code=4 name=PSP-Go",
             "EVT rig run_id=%s arm=%s" % (rid, log_arm or arm),
             "EVT build eboot_crc=%s size=1" % crc,
             "EVT me_mode on=%s src=config" % staged.get("me_mode", "1"),
             "EVT rom_diag " + " ".join("%s=%s" % kv for kv in rd.items()),
             "EVT rom_cache_cap blocks=%s reason=x" % blocks]
    lines += ["EVT cfg src=harness key=%s raw=%s" % kv for kv in echo.items()]
    if unknown:
        lines.append("EVT cfg_unknown src=harness key=rom_capp raw=32")
    lines.append("EVT rom_cache blocks=%s rom=32768KB resident=%s lo=09100000 "
                 "hi=0b100000 plan=default" % (blocks, "1" if blocks == "32" else "0"))
    lines += ["EVT core_prof core=1/2/3 win=0/0/%d/0/0 wspike=0" % i for i in range(3)]
    if complete:
        lines += ["EVT evt_drop total=0", "EVT audio_hash cfeacd06 samples=1",
                  "EVT exit code=0"]
    with open(os.path.join(d, "frontend.log"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    if shash:
        last = FRAMES if complete else 400
        with open(os.path.join(d, "shash.txt"), "w") as fh:
            fh.write("# shash v2 run_id=%s arm=%s eboot_crc=%s model=4\n"
                     % (rid, arm, crc))
            for f in range(1, last + 1):
                fh.write(shash_line(f, tweak=diverge_at is not None and f >= diverge_at) + "\n")
    with open(os.path.join(d, "heartbeat.txt"), "w") as fh:
        for s in range(1, 8):
            fr = 400 if (hb_stuck and s > 3) else s * 100
            fh.write("hb s=%d t_ms=1 iter=%d phase=2 frame=%d evt_drop=0 shash=1 "
                     "run_id=%s arm=%s eboot_crc=%s model=4\n"
                     % (s * 5, fr, fr, rid, arm, crc))
    if froze_prev is not None:
        # A frozen first attempt: its oracle (diverged at froze_prev, stopped
        # at 650) survives as shash.prev.txt, and its heartbeat lines precede
        # the retry's in the appended heartbeat.txt (t_ms restarts at boot).
        with open(os.path.join(d, "shash.prev.txt"), "w") as fh:
            fh.write("# shash v2 run_id=%s arm=%s eboot_crc=%s model=4\n"
                     % (rid, arm, crc))
            for f in range(1, 651):
                fh.write(shash_line(f, tweak=f >= froze_prev) + "\n")
        with open(os.path.join(d, "heartbeat.txt")) as fh:
            retry = fh.read()
        with open(os.path.join(d, "heartbeat.txt"), "w") as fh:
            for s_ in range(1, 6):
                fh.write("hb s=%d t_ms=%d iter=650 phase=2 frame=650 evt_drop=0 "
                         "shash=1 run_id=%s arm=%s eboot_crc=%s model=4\n"
                         % (s_ * 5, 90000 + s_, rid, arm, crc))
            fh.write(retry)
    meta = {"arm": arm, "run_id": rid, "cycle": 1, "eboot_crc32": crc,
            "prx_md5": PRX, "staged_ini": staged,
            "froze_before": froze_prev is not None,
            "result": {"status": "ready", "exit": "0", "reason": "ok",
                       "frames": str(FRAMES), "evt_drop": "0"} if complete else "unknown"}
    with open(os.path.join(d, "meta.json"), "w") as fh:
        json.dump(meta, fh)


def staged(arm, rid, **keys):
    s = {"shash": "1", "heartbeat_s": "5"}
    s.update(keys)
    s.update({"run_id": rid, "arm": arm})
    return s


def run_summary(logs, stage, golden):
    out = os.path.join(logs, "out.json")
    p = subprocess.run([sys.executable, SUMMARY, "--logs", logs, "--stage", stage,
                        "--golden", golden, "--golden-model", "PSP-Go",
                        "--json", out], capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout, p.stderr)
        raise SystemExit("summarizer exited %d" % p.returncode)
    return {r["dir"]: r for r in json.load(open(out))}


def main():
    t = tempfile.mkdtemp(prefix="resrig_t_")
    stage = os.path.join(t, "stage")
    os.makedirs(stage)
    json.dump({"files": {"EBOOT.PBP": {"crc32": CRC, "md5": "x"},
                         "gbadhoc_me.prx": {"md5": PRX}}},
              open(os.path.join(stage, "MANIFEST.json"), "w"))
    golden = os.path.join(t, "golden")
    make_run(golden, "auto001-CTRL-solo", "CTRL", staged("CTRL", "1-1"))
    logs = os.path.join(t, "logs")
    res32 = dict(rom_cap="32")
    cases = [
        ("auto001-CTRL-solo", "CTRL", staged("CTRL", "1-9"), {}, "PASS"),
        ("auto002-RES-solo", "RES", staged("RES", "2-9", **res32),
         dict(rom_diag={"rom_cap": "32"}, blocks="32"), "PASS"),
        ("auto003-RES-solo", "RES", staged("RES", "3-9", **res32),
         dict(rom_diag={"rom_cap": "32"}, blocks="32", diverge_at=512),
         "DIVERGED f=512 i"),
        ("auto004-RES-solo", "RES", staged("RES", "4-9", **res32),
         dict(rom_diag={"rom_cap": "32"}, blocks="32", log_arm="CTRL"), "INVALID"),
        ("auto005-RES-solo", "RES", staged("RES", "5-9", **res32),
         dict(rom_diag={"rom_cap": "32"}, blocks="32", crc="deadbeef"), "INVALID"),
        ("auto006-RES-solo", "RES", staged("RES", "6-9", **res32),
         dict(rom_diag={"rom_cap": "32"}, blocks="32", unknown=True), "INVALID"),
        ("auto007-RES-solo", "RES", staged("RES", "7-9", **res32),
         dict(rom_diag={"rom_cap": "0"}, blocks="15"), "INVALID"),
        ("auto008-RES-solo", "RES", staged("RES", "8-9", **res32),
         dict(rom_diag={"rom_cap": "32"}, blocks="32",
              echo=staged("RES", "8-9")), "INVALID"),
        ("auto009-RES-solo", "RES", staged("RES", "9-9", **res32),
         dict(rom_diag={"rom_cap": "32"}, blocks="32", complete=False,
              hb_stuck=True), "FROZE/CRASH"),
        ("auto010-CTRL-solo", "CTRL", staged("CTRL", "10-9"),
         dict(shash=False), "INVALID"),
        ("auto011-RES-solo", "RES", staged("RES", "11-9", **res32),
         dict(rom_diag={"rom_cap": "32"}, blocks="32", froze_prev=600),
         "FROZE (attempt 1: diverged f=600 i); retry: PASS"),
    ]
    for name, arm, st, kw, _ in cases:
        make_run(logs, name, arm, st, **kw)
    got = run_summary(logs, stage, golden)
    bad = 0
    for name, _a, _s, _k, want in cases:
        v = got[name]["verdict"]
        ok = v.startswith(want)
        why = got[name]["invalid"]
        # An INVALID must be invalid for the RIGHT reason, not by accident.
        need = {"auto004-RES-solo": "I2 log rig", "auto005-RES-solo": "I3 eboot",
                "auto006-RES-solo": "I4 cfg_unknown", "auto007-RES-solo": "I4 applied rom_cap",
                "auto008-RES-solo": "I4 rom_cap staged=32 echoed=none",
                "auto010-CTRL-solo": "I2 shash header"}.get(name)
        if need and not any(w.startswith(need) for w in why):
            ok = False
        bad += not ok
        print("%s %-22s want %-18s got %s%s" % ("ok  " if ok else "FAIL", name, want, v,
              ("  [" + "; ".join(why) + "]") if why else ""))
    hb = got["auto009-RES-solo"]["heartbeat"]
    if "STUCK" not in hb:
        print("FAIL heartbeat of a frozen run does not say STUCK: %s" % hb)
        bad += 1
    if got["auto010-CTRL-solo"]["shash_lines"] != "unknown":
        print("FAIL a missing shash.txt must read unknown")
        bad += 1
    print("%d/%d checks failed" % (bad, len(cases) + 2) if bad else
          "all %d checks passed" % (len(cases) + 2))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
