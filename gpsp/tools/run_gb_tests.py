#!/usr/bin/env python3
"""run_gb_tests.py -- every host test of the GB/GBC path, as one suite.

    python3 tools/run_gb_tests.py        (run_host_tests.py runs it as "gb")

Replaces run_fe_gb_boot_test.py and run_ui_console_tests.py, which nothing
invoked, and adds the three GB tests that had no runner at all.  It needs a
POSIX gcc (WSL on Windows -- run it through run_host_tests.py there).

Project code is built with -Wall -Wextra -Werror.  The vendored TGB Dual
core (gbcore/tgbdual/) is built with the flags psp/Makefile gives it (GNU89
inline, no strict aliasing, no unused-parameter warning) and -Werror too:
every other warning in it was fixed, see gbcore/tgbdual/LOCAL-PATCHES.md.
The core and the tests that drive it run under AddressSanitizer and
UBSan, which is what caught the upstream spare_oam underflow class of bug.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GB_BASELINE = "696ad10a21d92e83400d5fb45a2856e6032d2824"   # 3.0.0 source
WARN = ["-Wall", "-Wextra", "-Werror"]
SAN = ["-fsanitize=address,undefined", "-g", "-O1"]
CORE_FLAGS = ["-fgnu89-inline", "-fno-strict-aliasing", "-Wno-unused-parameter"]
CORE = ["apu.c", "cpu.c", "gb.c", "lcd.c", "mbc.c", "rom.c", "sgb.c"]
# Read-only tables every instance shares: linked once, outside the
# per-instance partial link (gbcore/tgbdual/tgb_shared.c).
SHARED = "tgb_shared.c"
INC = ["-Ifrontend-common", "-Inetdrv", "-Ilibretro/libretro-common/include"]


def run(cmd: list[str], env_extra: dict[str, str] | None = None) -> None:
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, env=env)
    if r.stdout.strip():
        print(r.stdout.rstrip())
    if r.returncode != 0:
        print(r.stderr.rstrip(), file=sys.stderr)
        raise SystemExit("FAIL: %s" % " ".join(cmd[:4]))


def build_instances(t: Path, objs: list[str]) -> tuple[str, str]:
    """Instance A (gbcore_*) and instance B (gbcoreb_*) from the same
    objects, the way psp/Makefile links them: `ld -r`, every global but the
    API made local, then the API renamed for the copy.  Renaming touches
    only symbols the object defines, so libc references are untouched."""
    joined, inst_a, inst_b = t / "gbcore_joined.o", t / "gbcore_a.o", t / "gbcore_b.o"
    run(["ld", "-r", "-T", "gbcore/gbcore_instance.ld", "-o", str(joined),
         *objs])
    run(["objcopy", "-w", "--keep-global-symbol=gbcore_*", str(joined),
         str(inst_a)])
    nm = subprocess.run(["nm", "-g", "--defined-only", str(inst_a)], cwd=REPO,
                        capture_output=True, text=True, check=True).stdout
    names = [line.split()[-1] for line in nm.splitlines() if line.strip()]
    if not names or any(not n.startswith("gbcore_") for n in names):
        raise SystemExit("FAIL: instance A exports %r" % names)
    syms = t / "gbcoreb.syms"
    syms.write_text("".join("%s gbcoreb_%s\n" % (n, n[len("gbcore_"):])
                            for n in names))
    run(["objcopy", "--redefine-syms=%s" % syms, str(inst_a), str(inst_b)])
    return str(inst_a), str(inst_b)


def build_trace(t: Path, root: Path, tag: str) -> Path:
    """gbcore/tests/bitident.c linked against the GB core under `root`
    (the repo, or the baseline copy), with psp/Makefile's core flags."""
    out = t / ("bitident-" + tag)
    obj = t / ("bitident-" + tag + "-o")
    obj.mkdir()
    objs = []
    names = CORE + ([SHARED] if (root / "gbcore/tgbdual" / SHARED).exists()
                    else [])
    for name in names:
        o = obj / (name[:-2] + ".o")
        run(["gcc", "-std=gnu11", "-O2", "-w", *CORE_FLAGS, "-c",
             str(root / "gbcore/tgbdual" / name), "-o", str(o)])
        objs.append(str(o))
    run(["gcc", "-std=gnu11", "-O2", "-w", "-I" + str(root / "gbcore"),
         "-I" + str(root / "frontend-common"), "-o", str(out),
         str(REPO / "gbcore/tests/bitident.c"),
         str(root / "gbcore/gbcore_tgbdual.c"), *objs, "-lm"])
    return out


def bit_identical(t: Path) -> None:
    """Single-player is bit-identical to the 3.0.0 release: picture, sound,
    save-state image and battery image, every frame, on the generated ROM
    and (GBADHOC_GB_ROMS) on every real one.  The baseline tree comes from
    run_host_tests.py (GBADHOC_BASELINE_DIR/gb) or from git here."""
    base = Path(os.environ.get("GBADHOC_BASELINE_DIR", "")) / "gb"
    if not (base / "gbcore/gbcore_tgbdual.c").exists():
        base = t / "baseline-gb"
        r = subprocess.run(["git", "archive", "--format=tar", GB_BASELINE,
                            "gbcore", "frontend-common/fe_console.h"],
                           cwd=REPO, capture_output=True)
        if r.returncode != 0:
            raise SystemExit("FAIL: bit-identity baseline %s unavailable: run "
                             "through tools/run_host_tests.py" % GB_BASELINE[:12])
        base.mkdir()
        subprocess.run(["tar", "-x", "-C", str(base)], input=r.stdout, check=True)
    # bitident.c includes "../gbcore.h": the baseline copy reads the
    # baseline header, which declares every function the trace calls.
    shutil.copy(REPO / "gbcore/tests/bitident.c", base / "gbcore/tests/bitident.c")
    old = build_trace(t, base, "release")
    new = build_trace(t, REPO, "current")
    frames = os.environ.get("GBADHOC_GB_FRAMES", "3000")
    roms_dir = os.environ.get("GBADHOC_GB_ROMS", "")
    roms = sorted(str(p) for p in Path(roms_dir).glob("*")
                  if p.suffix.lower() in (".gb", ".gbc")) if roms_dir else []
    runs = [["1200"]] + ([[frames, *roms]] if roms else [])
    lines = 0
    for args in runs:
        a = subprocess.run([str(old), *args], capture_output=True, text=True)
        b = subprocess.run([str(new), *args], capture_output=True, text=True)
        if a.returncode or b.returncode or a.stdout != b.stdout:
            for x, y in zip(a.stdout.splitlines(), b.stdout.splitlines()):
                if x != y:
                    print("release: " + x + "\ncurrent: " + y, file=sys.stderr)
                    break
            raise SystemExit("FAIL: single-player trace differs from 3.0.0")
        lines += a.stdout.count("\n")
    print("bit-identical to 3.0.0 (%s): %d frame records (picture, sound, "
          "state, battery), generated ROM%s" % (
              GB_BASELINE[:7], lines,
              " + %d real ROMs x %s frames" % (len(roms), frames) if roms
              else "; set GBADHOC_GB_ROMS for real ones"))


def main() -> int:
    asan = {"ASAN_OPTIONS": "detect_leaks=0"}
    with tempfile.TemporaryDirectory(prefix="gbadhoc-gb-") as tmp:
        t = Path(tmp)

        # Vendored core once, as objects shared by the tests that link it.
        core_objs = []
        for name in CORE:
            obj = t / (name[:-2] + ".o")
            run(["gcc", "-std=gnu11", *WARN, *SAN, *CORE_FLAGS,
                 "-ffunction-sections", "-fdata-sections", "-c",
                 "gbcore/tgbdual/" + name, "-o", str(obj)])
            core_objs.append(str(obj))
        shared = t / "tgb_shared.o"
        run(["gcc", "-std=gnu11", *WARN, *SAN, *CORE_FLAGS,
             "-ffunction-sections", "-fdata-sections", "-c",
             "gbcore/tgbdual/" + SHARED, "-o", str(shared)])
        glue = t / "gbcore_tgbdual.o"
        run(["gcc", "-std=gnu11", *WARN, *SAN, "-ffunction-sections",
             "-fdata-sections", "-Igbcore", "-Ifrontend-common", "-c",
             "gbcore/gbcore_tgbdual.c", "-o", str(glue)])

        # Two instances in one binary (gbcore_dual.h): the adapter and core
        # as one relocatable object with only gbcore_* global -- exactly
        # psp/Makefile's rule -- and a copy of it with those renamed
        # gbcoreb_*.  The dual test proves the copies share nothing.
        inst_a, inst_b = build_instances(t, [str(glue), *core_objs])
        run(["gcc", "-std=gnu11", *WARN, *SAN, "-Igbcore", "-o",
             str(t / "dual"), "gbcore/tests/dual.c", "gbcore/gbcore_dual.c",
             inst_a, inst_b, str(shared), "-lm"])
        run([str(t / "dual")], asan)

        bit_identical(t)

        # The link session protocol (fe_gblink): both consoles' complete
        # stacks in one process over a simulated network, on generated
        # cartridges -- a session, cartridge transfer, a damaged transfer
        # and an injected desync (tools/tests/gblink_session_tests.py).
        gbl = t / "gblink"
        run(["bash", "tools/gblink/build.sh", str(gbl)])
        run(["python3", "tools/tests/gblink_session_tests.py", str(gbl)])
        # The hardware rig's scorer: a clean pair of logs passes, and every
        # way a run can be broken scores FAIL or INVALID, never PASS.
        run(["python3", "tools/tests/test_gblink_score.py"])
        # The one tool that writes the sticks: never into a USB window it
        # cannot date (parked consoles), never the handoff, never a console
        # whose ROLE.TXT names another model.
        run(["python3", "tools/tests/test_gblink_setup.py"])
        # The I-cache geometry analyser (tools/cachemap): simulated caches of
        # known geometry are recovered exactly; a flat (PPSSPP) log is not.
        run(["python3", "tools/tests/test_icprobe_geometry.py"])

        # The autopilot commands the GB link scripts rely on (stepram,
        # ifram) against a walker whose buttons arrive late.
        run(["gcc", "-std=gnu99", *WARN, *SAN, "-DGPSP_PERF_RIG",
             "-Ifrontend-common", "-o", str(t / "ap_gb_steps"),
             "tools/tests/test_ap_gb_steps.c"])
        run([str(t / "ap_gb_steps")], asan)

        run(["gcc", "-std=gnu99", *WARN, *SAN, "-o", str(t / "gb_link"),
             "tools/test_gb_link.c", "netdrv/gb_link.c"])
        run([str(t / "gb_link")], asan)

        run(["gcc", "-std=gnu99", *WARN, *SAN, *INC, "-o", str(t / "gb_np"),
             "tools/test_gb_np_host.c", "frontend-common/netpacket_host.c"])
        run([str(t / "gb_np")], asan)

        run(["gcc", "-std=c99", *WARN, "-Ifrontend-common", "-o",
             str(t / "ui_filter"), "tools/test_ui_console_filter.c"])
        run([str(t / "ui_filter")])
        print("PASS console extension filter")

        run(["gcc", "-std=gnu11", *WARN, *SAN, "-Igbcore", "-o",
             str(t / "smoke"), "gbcore/tests/smoke.c", str(glue),
             *core_objs, str(shared), "-lm"])
        run([str(t / "smoke")], asan)

        # A state carries its DMG/SGB model: it loads after the DMG palette
        # (which picks the model at power-on) changed.  The 3.1 RC state
        # shelf quit to the XMB when it did not.
        run(["gcc", "-std=gnu11", *WARN, *SAN, "-Igbcore", "-o",
             str(t / "sgb_state"), "gbcore/tests/sgb_state.c", str(glue),
             *core_objs, str(shared), "-lm"])
        run([str(t / "sgb_state")], asan)

        # The Game Boy Color LCD model (docs/GB-PALETTE-FIXES.md): on by
        # default, off = 3.0.0's raw colours, switched live, DMG untouched.
        run(["gcc", "-std=gnu11", *WARN, *SAN, "-Igbcore", "-o",
             str(t / "cgb_lcd"), "gbcore/tests/cgb_lcd.c", str(glue),
             *core_objs, str(shared), "-lm"])
        run([str(t / "cgb_lcd")], asan)

        # The cable hook in the core: SC write -> callback -> SB/IF.
        run(["gcc", "-std=gnu11", *WARN, *SAN, "-Igbcore", "-o",
             str(t / "serial"), "gbcore/tests/serial.c", str(glue),
             *core_objs, str(shared), "-lm"])
        run([str(t / "serial")], asan)

        # The production frontend dispatch, save path, save states and cable
        # glue.
        run(["gcc", "-std=gnu11", *WARN, *SAN, "-ffunction-sections",
             "-fdata-sections", *INC, "-o", str(t / "fe_gb_boot"),
             "tools/test_fe_gb_boot.c", "frontend-common/fe_host.c",
             "frontend-common/fe_util.c", "netdrv/gb_link.c",
             "frontend-common/fe_gblink.c",
             "gbcore/gbcore_dual.c", inst_a, inst_b, str(shared),
             "-Wl,--gc-sections",
             "-lm"])
        run([str(t / "fe_gb_boot")], asan)
    print("gb: all GB/GBC host tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
