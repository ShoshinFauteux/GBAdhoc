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
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
WARN = ["-Wall", "-Wextra", "-Werror"]
SAN = ["-fsanitize=address,undefined", "-g", "-O1"]
CORE_FLAGS = ["-fgnu89-inline", "-fno-strict-aliasing", "-Wno-unused-parameter"]
CORE = ["apu.c", "cpu.c", "gb.c", "lcd.c", "mbc.c", "rom.c", "sgb.c"]
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
        glue = t / "gbcore_tgbdual.o"
        run(["gcc", "-std=gnu11", *WARN, *SAN, "-ffunction-sections",
             "-fdata-sections", "-Igbcore", "-Ifrontend-common", "-c",
             "gbcore/gbcore_tgbdual.c", "-o", str(glue)])

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
             *core_objs, "-lm"])
        run([str(t / "smoke")], asan)

        # The cable hook in the core: SC write -> callback -> SB/IF.
        run(["gcc", "-std=gnu11", *WARN, *SAN, "-Igbcore", "-o",
             str(t / "serial"), "gbcore/tests/serial.c", str(glue),
             *core_objs, "-lm"])
        run([str(t / "serial")], asan)

        # The production frontend dispatch, save path, save states and cable
        # glue.
        run(["gcc", "-std=gnu11", *WARN, *SAN, "-ffunction-sections",
             "-fdata-sections", *INC, "-o", str(t / "fe_gb_boot"),
             "tools/test_fe_gb_boot.c", "frontend-common/fe_host.c",
             "frontend-common/fe_util.c", "netdrv/gb_link.c", str(glue),
             *core_objs, "-Wl,--gc-sections", "-lm"])
        run([str(t / "fe_gb_boot")], asan)
    print("gb: all GB/GBC host tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
