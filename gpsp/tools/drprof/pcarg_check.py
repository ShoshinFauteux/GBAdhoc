#!/usr/bin/env python3
"""pcarg_check.py -- PCARG_SIDE's twin checks (docs/PCARG-SIDE.md section 8).

The release build carries no runtime check of the side table: "the table stays sorted" and "every entry is a
call" are counted only under DRPROF_TWIN (pcarg_bad, mips_emit.h), and the Lab gate's own twins are plain
(TWIN=0), so they never print it.  This script is where that check runs.  Run it for every change to
mips/mips_emit.h, mips/mips_stub.S or cpu_threaded.c's translator, before the gate.

  python -I gpsp/tools/drprof/pcarg_check.py --lab-backend LAB/backend --base-manifest BASE/build-manifest.json \
         --cand COMMIT --cand-manifest CAND/build-manifest.json [--out DIR] [--fixtures a,b] [--far-max 63]

Arms, all with -DRAM_CACHE_KB=384 (the PSP's RAM cache), built and run by the Lab's gate_twin (WSL + docker):
  base   the base manifest's source commit, plain twin
  pad    COMMIT + PCARG_SIDE_PAD=1, plain twin: every block keeps the base's size, so the per-frame state and
         framebuffer hashes must equal base's (the exactness oracle)
  far    COMMIT + PCARG_SIDE_PAD=1 + PCARG_NEAR_MAX=N, plain twin: every plain site more than N bytes into its
         block takes the out-of-range fallback (the old form).  The forced sites still emit the base's addiu, so
         the hashes must equal base's too
  padc   pad's defines, TWIN=1 (the same emitted code plus DRPROF_TWIN's bookkeeping): bad == 0, far == 0, new > 0
  farc   far's defines, TWIN=1: bad == 0, far > 0

SMC_REUSE's call-site log (cpu_threaded.c smc_reuse_log_site) must see every jal the site emitters write: a site
missing from a block's record stays learnt when the block is revived.  Only an SMC_REUSE_DEBUG build counts that
(sitebad: records whose log disagrees with a scan of the body), so three more arms, all TWIN=1 + SMC_REUSE_DEBUG=1:
  based  the base commit: the reference `smc_reuse:` counters
  padd   pad's defines: the same blocks as base, so sitebad, records and unlearnt must EQUAL based's
  candd  COMMIT as shipped (no pad): sitebad no higher than based's, records > 0

Exit 0 only when every case passes.  Writes pcarg_check.json to --out (default: the current directory)."""
import argparse
import concurrent.futures as cf
import json
import os
import re
import shlex
import subprocess
import sys

DEFAULT_FIXTURES = "kf_payback,kf_ice_nine,kf_doom_ii,kf_hot_wheels_stunt_track_challenge"


def log(s):
    print(s, flush=True)


def build_counted(gt, commit, flags):
    """gate_twin.build's recipe with TWIN=1 (DRPROF_TWIN bookkeeping); reuses an existing binary."""
    defs, _ = gt.core_defs(commit, flags)
    v = gt.variant_name(commit, defs, False) + "-t1"
    home = gt.home()
    _, out = gt.wsl("test -x %s/bin/dr_host_%s && echo have" % (home, v), 60)
    if "have" in out:
        return v
    log("  twin build %s (TWIN=1)" % v)
    os.makedirs(gt.PATCHES, exist_ok=True)
    tar = os.path.join(gt.PATCHES, "src-%s.tar" % v)
    spec = [commit, "gpsp"] + [":(exclude)%s" % e for e in gt.EXCLUDE]
    subprocess.run(["git", "-C", gt.REPO, "-c", "core.autocrlf=false", "archive", "--format=tar", "-o", tar] + spec,
                   check=True)
    inner = "\n".join([
        "set -e", "cd /w/src-%s/gpsp/tools/drprof" % v,
        "make VARIANT=%s VDEFS=%s TWIN=1 2>&1 | grep -E ' error|Error [0-9]' || true" % (
            shlex.quote(v), shlex.quote(defs)),
        "test -x bin/dr_host_%s" % v, "cp bin/dr_host_%s /w/bin/" % v, "cd / && rm -rf /w/src-%s" % v, ""])
    gt.put("jobs/build-%s.in.sh" % v, inner)
    src = "%s/src-%s" % (home, v)
    rc, out = gt.wsl("\n".join([
        "set -e", "rm -rf %s; mkdir -p %s" % (src, src), "tar -xf %s -C %s" % (shlex.quote(gt.wpath(tar)), src),
        "docker run --rm -v %s:/w %s bash /w/jobs/build-%s.in.sh" % (home, gt.IMAGE, v)]), 3600)
    os.remove(tar)
    gt.wsl("rm -rf %s" % src, 300)
    if rc:
        log(out[-1500:])
        sys.exit(2)
    return v


def counters(text):
    """the last `PCARG sites ...` line of a TWIN=1 run -> {name: int}"""
    lines = [ln for ln in text.splitlines() if ln.startswith("PCARG sites")]
    if not lines:
        return None
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", lines[-1])}


def reuse_counters(text):
    """the `smc_reuse: paused=.. sitebad=.. ...` line of a TWIN=1 run -> {name: int}"""
    lines = [ln for ln in text.splitlines() if ln.startswith("smc_reuse: paused=")]
    if not lines:
        return None
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", lines[-1])}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lab-backend", required=True, help="the Lab tree's backend directory (analysis/gate_twin.py)")
    ap.add_argument("--base-manifest", required=True)
    ap.add_argument("--cand", required=True, help="candidate source commit")
    ap.add_argument("--cand-manifest", required=True, help="a lab build-manifest.json with the candidate's coreFlags")
    ap.add_argument("--out", default=".")
    ap.add_argument("--fixtures", default=DEFAULT_FIXTURES)
    ap.add_argument("--far-max", type=int, default=63)
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()

    sys.path.insert(0, os.path.abspath(a.lab_backend))
    from analysis import gate_twin as gt
    from analysis import gate_ppsspp as gp

    bm = json.load(open(a.base_manifest))
    cm = json.load(open(a.cand_manifest))
    extra = {}
    plain_defs = gt.core_defs

    def core_defs(commit, flags):
        d, u = plain_defs(commit, flags)
        return (d + " -DRAM_CACHE_KB=384 " + extra.get("defs", "")).strip(), u

    gt.core_defs = core_defs
    pad = "-DPCARG_SIDE_PAD=1"
    far = "%s -DPCARG_NEAR_MAX=%d" % (pad, a.far_max)

    def plain(label, commit, flags, defs):
        extra["defs"] = defs
        r = gt.build({"label": label, "commit": commit, "core_flags": flags}, False, log)
        if not r.get("ok"):
            log("build %s failed: %s" % (label, (r.get("error") or "")[-1500:]))
            sys.exit(2)
        return r["variant"]

    def counted(defs, commit=None, flags=None):
        extra["defs"] = defs
        return build_counted(gt, commit or a.cand, flags or cm["coreFlags"])

    dbg = "-DSMC_REUSE_DEBUG=1"

    v = {"base": plain("base", bm["sourceCommit"], bm["coreFlags"], ""),
         "pad": plain("pad", a.cand, cm["coreFlags"], pad),
         "far": plain("far", a.cand, cm["coreFlags"], far),
         "padc": counted(pad), "farc": counted(far),
         "based": counted(dbg, bm["sourceCommit"], bm["coreFlags"]),
         "padd": counted(pad + " " + dbg), "candd": counted(dbg)}
    log("twins: %s" % v)

    fxs, _ = gp.bench().load()
    byid = {f["id"]: f for f in fxs}
    jobs = [(n, gt.stage_fixture(byid[n])) for n in a.fixtures.split(",") if n]

    def one(job):
        name, fx = job
        frames = fx["frames"]
        runs = {k: gt.run(v[k], fx, "pcargcheck/%s-%s" % (name, k), frames) for k in ("base", "pad", "far")}
        res = {"frames": frames, "fail": []}
        base_hash = gt.read_text(runs["base"]["dir"] + "/hash.txt")
        for k in ("pad", "far"):
            hc = gt.hash_compare(base_hash, gt.read_text(runs[k]["dir"] + "/hash.txt"), expect=frames)
            res[k] = {"identical": bool(hc.get("identical")), "common": hc.get("common"),
                      "first_diff": hc.get("first_diff"), "rc": runs[k]["rc"]}
            if not hc.get("identical") or hc.get("common") != frames or runs[k]["rc"] or runs["base"]["rc"]:
                res["fail"].append("%s: hashes differ from base (first f%s, %s common, rc %s/%s)" % (
                    k, hc.get("first_diff"), hc.get("common"), runs["base"]["rc"], runs[k]["rc"]))
        for k in ("padc", "farc"):
            r = gt.run(v[k], fx, "pcargcheck/%s-%s" % (name, k), frames, hash_=False)
            c = counters(gt.read_text(r["dir"] + "/run.out"))
            res[k] = c
            if c is None or r["rc"]:
                res["fail"].append("%s: no PCARG counter line (rc %s)" % (k, r["rc"]))
                continue
            if c.get("bad", 1) != 0:
                res["fail"].append("%s: bad=%s" % (k, c.get("bad")))
            if c.get("new", 0) == 0:
                res["fail"].append("%s: no new-form site" % k)
            if k == "padc" and c.get("far", 1) != 0:
                res["fail"].append("padc: far=%s without the test knob" % c.get("far"))
            if k == "farc" and c.get("far", 0) == 0:
                res["fail"].append("farc: the knob forced no site")
        ru = {}
        for k in ("based", "padd", "candd"):
            r = gt.run(v[k], fx, "pcargcheck/%s-%s" % (name, k), frames, hash_=False)
            c = reuse_counters(gt.read_text(r["dir"] + "/run.out"))
            if c is None or r["rc"]:
                res["fail"].append("%s: no smc_reuse counter line (rc %s)" % (k, r["rc"]))
                continue
            ru[k] = c
            res[k] = {n: c.get(n) for n in ("sitebad", "records", "unlearnt", "hits")}
        if len(ru) == 3:
            b, p, c = ru["based"], ru["padd"], ru["candd"]
            for n in ("sitebad", "records", "unlearnt"):
                if p.get(n) != b.get(n):
                    res["fail"].append("padd: %s=%s, base has %s (same blocks: must be equal)" % (n, p.get(n), b.get(n)))
            if c.get("sitebad", 1) > b.get("sitebad", 0):
                res["fail"].append("candd: sitebad=%s of %s records, base has %s of %s" % (
                    c.get("sitebad"), c.get("records"), b.get("sitebad"), b.get("records")))
            if b.get("records", 0) and not c.get("records", 0):
                res["fail"].append("candd: no record at all (base has %s)" % b.get("records"))
        return name, res

    out = {"cand": a.cand, "base": bm["sourceCommit"], "far_max": a.far_max, "twins": v, "cases": {}}
    bad = 0
    os.makedirs(a.out, exist_ok=True)
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for name, res in ex.map(one, jobs):
            out["cases"][name] = res
            bad += 1 if res["fail"] else 0
            log("%-38s %s pad=%s far=%s | padc %s | farc %s | based %s | padd %s | candd %s" % (
                name, "FAIL" if res["fail"] else "PASS",
                "identical" if res["pad"]["identical"] else "DIFF f%s" % res["pad"]["first_diff"],
                "identical" if res["far"]["identical"] else "DIFF f%s" % res["far"]["first_diff"],
                res.get("padc"), res.get("farc"), res.get("based"), res.get("padd"), res.get("candd")))
            for f in res["fail"]:
                log("    " + f)
            json.dump(out, open(os.path.join(a.out, "pcarg_check.json"), "w"), indent=1)
    log("pcarg_check: %s (%d of %d cases failed)" % ("FAIL" if bad else "PASS", bad, len(jobs)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
