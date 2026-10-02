#!/usr/bin/env python3
"""dr_analyze.py RUNDIR [RUNDIR...] [--json OUT] [--scenes a,b] [--me]

Turn a drprof plugin profile (RUNDIR/prof.txt, stubmap.txt) into the tables
docs/DYNAREC-PROFILE.md is built from.  All numbers are DYNAMIC MIPS
INSTRUCTION COUNTS on the twin (qemu), not time: label them that way.

"Core" instructions exclude the host (dr_host hashing, frontend) and, unless
--cpu-render, the renderer: on the PSP the renderer runs on the Media Engine
(player default), so the main CPU never executes it.

Task categories (the six in the study brief):
  1 translated   emitted code doing guest work (ALU, flags, reg[] traffic,
                 in-block branches, direct block links)
  2 memory       emitted memory-call setup (address/PC args + jal) + the
                 emitter's region stubs + C slow paths (I/O, backup, gamepak)
  3 dispatch     jumps to the dispatcher + mips_indirect_branch_* + lookup_pc
                 + block_lookup_* (hash/tag lookups, excluding translation)
  4 cycles/upd   emitted cycle updates and budget checks + mips_update_gba +
                 update_gba (the event scheduler itself)
  5 translate    translation zone + flush/SMC zone
  6 other core   DMA, timers, IRQ, sound, BIOS HLE, serial, libc
"""
import argparse
import bisect
import collections
import json
import math
import os
import re
import sys

TASK = collections.OrderedDict([
    ("1 translated", ["jit"]),
    ("2 memory", ["jmem", "stub", "memc"]),
    ("3 dispatch", ["disp", "jdisp"]),
    ("4 cycles/update_gba", ["cyc", "upd"]),
    ("5 translate+flush", ["xlat", "flush"]),
    ("6 other core", ["sound", "core", "libc"]),
])

ROLE_TASK = {  # translated-code roles -> where they belong
    "callmem": "2 memory", "dispj": "3 dispatch", "cyc": "4 cycles/update_gba",
    "cycchk": "4 cycles/update_gba",
}


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def quant(xs, q):
    if not xs:
        return 0
    xs = sorted(xs)
    k = min(len(xs) - 1, max(0, int(math.ceil(q * len(xs))) - 1))
    return xs[k]


def load(run):
    cats = None
    frames = []           # (frame, seg, total, {cat: n})
    marks = []            # (frame, scene)
    scene_b = collections.defaultdict(collections.Counter)
    for ln in open(os.path.join(run, "prof.txt"), errors="replace"):
        if ln.startswith("# drprof"):
            cats = ln.split("cats", 1)[1].split()
        elif ln.startswith("F "):
            f = ln.split()
            vals = list(map(int, f[4:]))
            frames.append((int(f[1]), int(f[2]), int(f[3]), dict(zip(cats, vals))))
        elif ln.startswith("M "):
            f = ln.split(None, 2)
            marks.append((int(f[1]), f[2].strip()))
        elif ln.startswith("S "):
            f = ln.split()
            scene_b[f[1]][f[2]] += int(f[3])
    stubs = []
    sp = os.path.join(run, "stubmap.txt")
    if os.path.exists(sp):
        for ln in open(sp):
            o, n = ln.split()
            stubs.append((int(o, 16), n))
        stubs.sort()
    return cats, frames, marks, scene_b, stubs


def stub_name(stubs, off):
    if not stubs:
        return "stub@%x" % off
    keys = [s[0] for s in stubs]
    i = bisect.bisect_right(keys, off) - 1
    if i < 0:
        return "stub@%x" % off
    # several table entries share one handler (regions 8-11, ignore stores);
    # report the first name at that exact offset
    o = stubs[i][0]
    names = [n for (k, n) in stubs if k == o]
    return names[0] if len(names) == 1 else names[0] + "(+%d)" % (len(names) - 1)


def core_of(row, cpu_render):
    _, _, tot, c = row
    x = tot - c.get("host", 0) - c.get("other", 0)
    if not cpu_render:
        x -= c.get("video", 0)
    return x


def scene_frames(frames, marks, scene):
    """frames belonging to a scene: from its mark to the next mark"""
    bounds = sorted(marks)
    out = []
    for i, (f0, name) in enumerate(bounds):
        if name != scene:
            continue
        f1 = bounds[i + 1][0] if i + 1 < len(bounds) else 1 << 30
        out += [r for r in frames if f0 < r[0] <= f1]
    return out


def analyse_scene(name, rows, B, stubs, cpu_render):
    res = {"scene": name, "frames": len(rows)}
    if not rows:
        return res
    core = [core_of(r, cpu_render) for r in rows]
    res["core_mean"] = sum(core) / len(core)
    res["core_p50"] = quant(core, 0.5)
    res["core_p95"] = quant(core, 0.95)
    res["core_max"] = max(core)
    res["video_mean"] = sum(r[3].get("video", 0) for r in rows) / len(rows)
    cat_tot = collections.Counter()
    for r in rows:
        for k, v in r[3].items():
            cat_tot[k] += v
    core_tot = sum(core)
    res["cats"] = {k: pct(v, core_tot) for k, v in cat_tot.items()
                   if k not in ("host", "other") and (cpu_render or k != "video")}
    task = collections.OrderedDict()
    for t, cs in TASK.items():
        task[t] = pct(sum(cat_tot[c] for c in cs), core_tot)
    if cpu_render:
        task["(renderer)"] = pct(cat_tot["video"], core_tot)
    res["task"] = task

    # ---- emitted code ------------------------------------------------------
    gi = collections.Counter()
    gi_flags = collections.Counter()
    jit_role = collections.Counter()
    jit_cls = collections.Counter()
    jit_cls_role = collections.Counter()
    for k, v in B.items():
        if k.startswith("gi:"):
            _, cls, fl = k.split(":")
            gi[cls] += v
            gi_flags[(cls, fl)] += v
        elif k.startswith("jit:"):
            _, cls, isa, role = k.split(":")
            jit_role[role] += v
            jit_cls[cls] += v
            jit_cls_role[(cls, role)] += v
    ngi = sum(gi.values())
    njit = sum(jit_role.values())
    res["guest_insns"] = ngi
    res["jit_insns"] = njit
    res["mips_per_guest"] = njit / ngi if ngi else 0
    res["jit_roles"] = {r: pct(v, njit) for r, v in jit_role.most_common()}
    res["per_class"] = {}
    for cls, n in gi.most_common():
        res["per_class"][cls] = {
            "guest": n, "share": pct(n, ngi),
            "mips_per": jit_cls[cls] / n if n else 0,
            "roles": {r: jit_cls_role[(cls, r)] / n for r in
                      ("alu", "flag", "arg", "temp", "regmem", "memdir", "callmem",
                       "cyc", "cycchk", "branch", "linkj", "dispj", "nop", "calloth")
                      if jit_cls_role[(cls, r)]},
        }
    pseudo = {c: jit_cls[c] for c in ("prologue", "cycle_upd", "tail", "unowned", "cheat")}
    res["pseudo_mips"] = {k: pct(v, njit) for k, v in pseudo.items() if v}
    # flags: guest instructions that write flags (S), and how many actually
    # generated flag code (F).  ARM has no liveness pass: S implies F.
    fl = collections.Counter()
    for (cls, f), v in gi_flags.items():
        isa = "thumb" if f.startswith("t") else "arm"
        if "S" in f:
            fl[isa + "_S"] += v
            if "F" in f:
                fl[isa + "_SF"] += v
        if "C" in f:
            fl[isa + "_cond"] += v
        fl[isa] += v
    res["flags"] = dict(fl)
    res["flag_mips_share"] = pct(jit_role["flag"], njit)
    res["regmem_per_guest"] = jit_role["regmem"] / ngi if ngi else 0

    # ---- memory stubs --------------------------------------------------------
    stub_ins = collections.Counter()
    stub_by = collections.Counter()
    for k, v in B.items():
        if k.startswith("stub:"):
            off = int(k[5:], 16)
            stub_ins[off] = v
    for off, v in stub_ins.items():
        stub_by[stub_name(stubs, off)] += v
    res["stub_top"] = stub_by.most_common(25)
    # calls: mc:<class>:<target offset> counts jal executions per target stub
    calls = collections.Counter()
    calls_cls_region = collections.Counter()
    for k, v in B.items():
        if k.startswith("mc:"):
            _, cls, off = k.split(":")
            nmx = stub_name(stubs, int(off, 16))
            calls[nmx] += v
            reg = nmx.split(":")[1].split("(")[0] if ":" in nmx else nmx
            calls_cls_region[(cls, reg)] += v
    res["mem_calls"] = sum(calls.values())
    res["mem_calls_top"] = calls.most_common(25)
    res["mem_calls_cls_region"] = {"%s->%s" % k: v for k, v in calls_cls_region.most_common(40)}
    patch = sum(v for k, v in stub_by.items() if k.startswith("patch_handler"))
    res["patch_handler_insns"] = patch

    # ---- dispatch / calls ------------------------------------------------------
    ent = {k[4:]: v for k, v in B.items() if k.startswith("ent:")}
    res["entries_per_frame"] = {k: v / len(rows) for k, v in
                                sorted(ent.items(), key=lambda x: -x[1])[:30]}
    sym = collections.Counter({k[4:]: v for k, v in B.items() if k.startswith("sym:")})
    res["top_syms_per_frame"] = [(k, v / len(rows)) for k, v in sym.most_common(40)]
    res["zones_per_frame"] = {k: v / len(rows) for k, v in B.items() if k.startswith("zone:")}
    return res


def fmt_report(runname, results, cpu_render):
    o = []
    o.append("## %s\n" % runname)
    o.append("Dynamic MIPS instructions per frame on the twin (qemu, exact counts; "
             "NOT time). Core excludes host%s.\n" % ("" if cpu_render else " and renderer (ME on)"))
    o.append("| scene | frames | core mean | p50 | p95 | max | renderer mean |")
    o.append("|---|---:|---:|---:|---:|---:|---:|")
    for r in results:
        if not r.get("frames"):
            continue
        o.append("| %s | %d | %.0f | %d | %d | %d | %.0f |" % (
            r["scene"], r["frames"], r["core_mean"], r["core_p50"], r["core_p95"],
            r["core_max"], r["video_mean"]))
    o.append("")
    keys = list(TASK.keys()) + (["(renderer)"] if cpu_render else [])
    o.append("| scene | " + " | ".join(keys) + " |")
    o.append("|---|" + "---:|" * len(keys))
    for r in results:
        if not r.get("frames"):
            continue
        o.append("| %s | " % r["scene"] + " | ".join("%.1f%%" % r["task"][k] for k in keys) + " |")
    o.append("")
    return "\n".join(o)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--json")
    ap.add_argument("--cpu-render", action="store_true")
    ap.add_argument("--skip", default="prestart,start",
                    help="scenes to leave out of the ALL row")
    o = ap.parse_args()
    alljs = {}
    for run in o.runs:
        cats, frames, marks, SB, stubs = load(run)
        scenes = [m[1] for m in sorted(marks)]
        results = []
        skip = set(o.skip.split(","))
        agg_rows, agg_B = [], collections.Counter()
        for sc in dict.fromkeys(scenes):
            rows = scene_frames(frames, marks, sc)
            r = analyse_scene(sc, rows, SB.get(sc, {}), stubs, o.cpu_render)
            results.append(r)
            if sc not in skip:
                agg_rows += rows
                agg_B.update(SB.get(sc, {}))
        results.append(analyse_scene("ALL", agg_rows, agg_B, stubs, o.cpu_render))
        print(fmt_report(os.path.basename(run.rstrip("/")), results, o.cpu_render))
        alljs[os.path.basename(run.rstrip("/"))] = results
    if o.json:
        json.dump(alljs, open(o.json, "w"), indent=1)


if __name__ == "__main__":
    main()
