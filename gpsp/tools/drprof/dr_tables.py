#!/usr/bin/env python3
"""dr_tables.py RUNROOT [--cmp RUNROOT2]

The tables in docs/DYNAREC-PROFILE.md, regenerated from a dr_suite.sh run
(RUNROOT/<fixture>/prof.txt).  --cmp adds the before/after table for a
variant suite (e.g. runs/dcache).  All numbers: dynamic MIPS instructions on
the twin (qemu), per emulated frame.
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dr_analyze as A  # noqa: E402

# fixture -> scenes that form the measured window
WINDOW = {
    "aw2": None,                      # every scene after the first mark
    "hns_heavy": ["sample_begin"],
    "hns_light": ["sample_begin"],
    "ub_rival": ["sample_begin"],
    "ub_double": ["sample_begin"],
    "ub_ow": ["ow_walk"],
    "em_battle": ["battle"],
}
LABEL = {
    "aw2": "AW2 scene tour", "hns_heavy": "H&S battle, heavy music",
    "hns_light": "H&S battle, light music", "ub_rival": "Unbound rival battle",
    "ub_double": "Unbound double battle", "ub_ow": "Unbound overworld walk",
    "em_battle": "Emerald wild battle",
}
ORDER = ["aw2", "hns_heavy", "hns_light", "ub_rival", "ub_double", "ub_ow", "em_battle"]

GROUPS = collections.OrderedDict([
    ("sound", ("sound_", "render_gbc_sound", "sound_timer")),
    ("serial/link", ("update_serial", "serial", "rfu_")),
    ("dma", ("dma_",)),
    ("irq", ("check_and_raise", "check_interrupt")),
])


def window(run, fx):
    cats, frames, marks, SB, stubs = A.load(run)
    scenes = [m[1] for m in sorted(marks)]
    want = WINDOW.get(fx) or [s for s in dict.fromkeys(scenes) if s not in ("prestart", "start")]
    rows, B = [], collections.Counter()
    for s in dict.fromkeys(scenes):
        if s in want:
            rows += A.scene_frames(frames, marks, s)
            B.update(SB.get(s, {}))
    return rows, B, stubs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--cmp")
    o = ap.parse_args()
    res, raw = {}, {}
    for fx in ORDER:
        d = os.path.join(o.root, fx)
        if not os.path.exists(os.path.join(d, "prof.txt")):
            continue
        rows, B, stubs = window(d, fx)
        res[fx] = A.analyse_scene(fx, rows, B, stubs, False)
        raw[fx] = (rows, B)

    print("### Split of main-CPU instructions (twin, per frame, renderer on the ME)\n")
    keys = list(A.TASK.keys())
    print("| fixture | frames | core instr/frame mean | p95 | " + " | ".join(k[2:] for k in keys) + " |")
    print("|---|---:|---:|---:|" + "---:|" * len(keys))
    for fx, r in res.items():
        print("| %s | %d | %.0f | %.0f | " % (LABEL[fx], r["frames"], r["core_mean"], r["core_p95"])
              + " | ".join("%.1f%%" % r["task"][k] for k in keys) + " |")

    print("\n### Inside the categories (percent of core instructions unless noted)\n")
    print("| fixture | jit roles: guest work | mem: call setup+jal | mem: stubs | mem: C slow paths "
          "| dispatches/frame | instr/dispatch | cycle code in blocks | update_gba+stub "
          "| translate | flush/SMC | SMC flushes/frame |")
    print("|---|" + "---:|" * 11)
    for fx, r in res.items():
        rows, B = raw[fx]
        n = len(rows)
        c = r["cats"]
        disp_n = sum(B.get("ent:" + s, 0) for s in (
            "mips_indirect_branch_arm", "mips_indirect_branch_thumb",
            "mips_indirect_branch_dual", "lookup_pc")) / n
        disp_i = (c.get("disp", 0) + c.get("jdisp", 0)) / 100 * r["core_mean"]
        smc = (B.get("ent:smc_write", 0) + B.get("ent:flush_translation_cache_ram_dma", 0) / 2) / n
        print("| %s | %.1f%% | %.1f%% | %.1f%% | %.1f%% | %.0f | %.0f | %.1f%% | %.1f%% | %.1f%% | %.1f%% | %.1f |" % (
            LABEL[fx], c.get("jit", 0), c.get("jmem", 0), c.get("stub", 0), c.get("memc", 0),
            disp_n, disp_i / disp_n if disp_n else 0, c.get("cyc", 0), c.get("upd", 0),
            c.get("xlat", 0), c.get("flush", 0), smc))

    print("\n### 'Other core' by subsystem (percent of core instructions)\n")
    print("| fixture | " + " | ".join(GROUPS) + " | libc | rest (mode switches, psr, bios, timers) |")
    print("|---|" + "---:|" * (len(GROUPS) + 2))
    for fx, r in res.items():
        rows, B = raw[fx]
        tot = r["core_mean"] * len(rows)
        g = collections.Counter()
        for k, v in B.items():
            if not k.startswith("sym:"):
                continue
            name = k[4:]
            for gname, pre in GROUPS.items():
                if name.startswith(pre) or any(p in name for p in pre if len(p) > 6):
                    g[gname] += v
                    break
        other = r["task"]["6 other core"]
        libc = r["cats"].get("libc", 0)
        grp = {k: 100.0 * v / tot for k, v in g.items()}
        rest = other - libc - sum(grp.get(k, 0) for k in GROUPS)
        print("| %s | " % LABEL[fx] + " | ".join("%.1f%%" % grp.get(k, 0) for k in GROUPS)
              + " | %.1f%% | %.1f%% |" % (libc, rest))

    print("\n### Emitted code (translated blocks only)\n")
    print("| fixture | guest instr/frame | MIPS/guest | ALU | flag writes | arg/temp setup | "
          "cycle upd+chk | mem calls | SP fast path ld/st | branches | block links | "
          "dispatch jumps | nops | reg[] ld/st |")
    print("|---|" + "---:|" * 13)
    for fx, r in res.items():
        rr = r["jit_roles"]
        print("| %s | %.0f | %.2f | %.1f%% | %.1f%% | %.1f%% | %.1f%% | %.1f%% | %.1f%% | %.1f%% | %.1f%% | %.1f%% | %.1f%% | %.1f%% |" % (
            LABEL[fx], r["guest_insns"] / r["frames"], r["mips_per_guest"], rr.get("alu", 0),
            rr.get("flag", 0), rr.get("arg", 0) + rr.get("temp", 0),
            rr.get("cyc", 0) + rr.get("cycchk", 0), rr.get("callmem", 0) + rr.get("calloth", 0),
            rr.get("memdir", 0), rr.get("branch", 0), rr.get("linkj", 0), rr.get("dispj", 0),
            rr.get("nop", 0), rr.get("regmem", 0)))

    print("\n### Flags\n")
    print("| fixture | ARM share of guest instr | ARM S-bit | ARM flag code generated | "
          "Thumb flag-setting | Thumb flag code generated | flag writes / JIT instr |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for fx, r in res.items():
        f = r["flags"]
        tot = f.get("arm", 0) + f.get("thumb", 0)
        print("| %s | %.0f%% | %.1f%% | %.1f%% of S | %.1f%% | %.1f%% of those | %.1f%% |" % (
            LABEL[fx], 100.0 * f.get("arm", 0) / tot if tot else 0,
            100.0 * f.get("arm_S", 0) / f["arm"] if f.get("arm") else 0,
            100.0 * f.get("arm_SF", 0) / f["arm_S"] if f.get("arm_S") else 0,
            100.0 * f.get("thumb_S", 0) / f["thumb"] if f.get("thumb") else 0,
            100.0 * f.get("thumb_SF", 0) / f["thumb_S"] if f.get("thumb_S") else 0,
            r["flag_mips_share"]))

    print("\n### MIPS instructions per guest instruction, by guest class (AW2 / Unbound rival)\n")
    for fx in ("aw2", "ub_rival"):
        if fx not in res:
            continue
        print("%s:\n" % LABEL[fx])
        print("| class | share | MIPS/guest | main roles (per guest instr) |")
        print("|---|---:|---:|---|")
        for cls, v in list(res[fx]["per_class"].items())[:14]:
            roles = ", ".join("%s %.2f" % (k, x) for k, x in sorted(v["roles"].items(), key=lambda y: -y[1])[:5])
            print("| %s | %.1f%% | %.2f | %s |" % (cls, v["share"], v["mips_per"], roles))
        print()

    print("### Memory calls by target region (calls/frame)\n")
    print("| fixture | calls/frame | IWRAM | EWRAM | ROM | I/O | VRAM/OAM/PAL | backup | patch-handler instr/frame |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for fx, r in res.items():
        n = r["frames"]
        reg = collections.Counter()
        for k, v in r["mem_calls_cls_region"].items():
            reg[k.split("->")[1]] += v
        tot = r["mem_calls"]

        def s(*keys):
            return sum(v for k, v in reg.items() if k.startswith(keys))
        print("| %s | %.0f | %.0f%% | %.0f%% | %.0f%% | %.0f%% | %.0f%% | %.0f%% | %.0f |" % (
            LABEL[fx], tot / n, 100.0 * s("iwram") / tot, 100.0 * s("ewram") / tot,
            100.0 * s("rom") / tot, 100.0 * s("io") / tot, 100.0 * s("vram", "oam", "pal") / tot,
            100.0 * s("backup", "eeprom") / tot, r["patch_handler_insns"] / n))
    print("\n(mem_calls_cls_region only lists the top 40 class->region pairs; small remainders omitted)")

    # the hardware sampler's phases (drprof.h DRPH_*), reproduced on the twin
    # so hardware TIME shares can be divided by twin INSTRUCTION shares
    # (= relative cycles per instruction of each phase on the real CPU)
    ASM_JIT = ("mips_indirect_branch", "lookup_pc", "mips_update_gba", "execute_",
               "write_io_epilogue", "smc_write", "return_to_main", "cpu_sleep_loop",
               "mips_cheat_hook")
    PHASE_OF = [("disp", ("block_lookup_",)), ("upd", ("update_gba",)),
                ("sound", ("sound_", "render_gbc_sound")), ("dma", ("dma_",)),
                ("serial", ("update_serial", "serial", "rfu_")),
                ("irq", ("check_and_raise",)),
                ("memc", ("read_backup", "write_backup", "read_eeprom", "write_eeprom",
                          "write_io_register", "write_gpio", "load_gamepak_page",
                          "read_memory"))]
    print("\n### Twin instruction shares in the hardware sampler's phases\n")
    print("(for dividing hardware time shares by; renderer excluded as on the ME)\n")
    print("| fixture | jit (incl. stubs, asm glue) | disp | upd | xlat | flush | memc | sound | dma | serial | irq | other |")
    print("|---|" + "---:|" * 11)
    for fx, r in res.items():
        rows, B = raw[fx]
        ph = collections.Counter()
        for k, v in B.items():
            if k.startswith("jit:"):
                ph["jit"] += v
            elif k.startswith("stub:"):
                ph["jit"] += v
            elif k in ("zone:xlat", "zone:flush"):
                ph[k[5:]] += v
            elif k.startswith("sym:"):
                name = k[4:]
                if name.startswith(ASM_JIT):
                    ph["jit"] += v
                    continue
                for pn, pre in PHASE_OF:
                    if name.startswith(pre):
                        ph[pn] += v
                        break
                else:
                    ph["_sym"] += v
        tot = r["core_mean"] * len(rows)
        other = tot - sum(v for k, v in ph.items() if k != "_sym")
        cells = [ph[k] for k in ("jit", "disp", "upd", "xlat", "flush", "memc", "sound", "dma", "serial", "irq")]
        print("| %s | " % LABEL[fx] + " | ".join("%.1f%%" % (100.0 * c / tot) for c in cells)
              + " | %.1f%% |" % (100.0 * other / tot))

    if o.cmp:
        print("\n### Variant vs base (core instructions per frame)\n")
        print("| fixture | base mean | variant mean | change | base p95 | variant p95 | change | dispatch share base -> variant |")
        print("|---|---:|---:|---:|---:|---:|---:|---|")
        for fx in res:
            d = os.path.join(o.cmp, fx)
            if not os.path.exists(os.path.join(d, "prof.txt")):
                continue
            rows, B, stubs = window(d, fx)
            v = A.analyse_scene(fx, rows, B, stubs, False)
            b = res[fx]
            print("| %s | %.0f | %.0f | %+.1f%% | %.0f | %.0f | %+.1f%% | %.1f%% -> %.1f%% |" % (
                LABEL[fx], b["core_mean"], v["core_mean"], 100 * (v["core_mean"] / b["core_mean"] - 1),
                b["core_p95"], v["core_p95"], 100 * (v["core_p95"] / b["core_p95"] - 1),
                b["task"]["3 dispatch"], v["task"]["3 dispatch"]))


if __name__ == "__main__":
    main()
