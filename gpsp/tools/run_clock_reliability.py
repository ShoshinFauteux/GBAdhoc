#!/usr/bin/env python3
"""Focused production RTC/rewind/link regressions; run under Linux/WSL."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ap = argparse.ArgumentParser()
ap.add_argument("--out", type=Path, default=Path("audit/reliability/clock-rewind"))
args = ap.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
base = ["gcc", "-std=gnu99", "-O1", "-g", "-ffunction-sections",
        "-fdata-sections", "-fsanitize=address,undefined", "-DINLINE=inline",
        "-I.", "-Ilibretro", "-Ilibretro/libretro-common/include", "-Ifrontend-common"]
suites = [
    ("rewind_rtc", ["-w", "-DREWIND", "tools/tests/test_rewind_rtc.c",
                    "frontend-common/fe_rewind.c", "frontend-common/fe_rewind_pack.c"]),
    ("gbadual_clock", ["-Wall", "-Wextra", "-Werror", "tools/tests/test_gbadual_clock.c"]),
    ("gbadual_bfresh", ["-Wall", "-Wextra", "-Werror", "tools/tests/test_gbadual_bfresh.c"]),
    ("rtc_protocol", ["-w", "tools/tests/test_rtc_protocol.c"]),
    ("rtc_clock", ["-Wall", "-Wextra", "-Werror", "tools/tests/test_rtc_clock.c"]),
]
results = []
with tempfile.TemporaryDirectory(prefix="gbadhoc-clock-tests-") as tmp:
    for name, inputs in suites:
        exe = str(Path(tmp) / name)
        command = base + inputs + ["-Wl,--gc-sections", "-o", exe]
        t0 = time.monotonic()
        built = subprocess.run(command, cwd=root, text=True, capture_output=True)
        ran = None
        if built.returncode == 0:
            ran = subprocess.run([exe], cwd=root, text=True, capture_output=True,
                                 env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"},
                                 timeout=120)
        log = "$ " + " ".join(command) + "\n" + built.stdout + built.stderr
        if ran is not None:
            log += "\n$ " + exe + "\n" + ran.stdout + ran.stderr
        (out / (name + ".log")).write_text(log)
        ok = built.returncode == 0 and ran is not None and ran.returncode == 0
        results.append({"name": name, "passed": ok, "command": command,
                        "seconds": round(time.monotonic() - t0, 3)})
        print(name, "PASS" if ok else "FAIL", flush=True)
(out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
raise SystemExit(0 if all(r["passed"] for r in results) else 1)
