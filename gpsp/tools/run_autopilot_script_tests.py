#!/usr/bin/env python3
"""Exercise autopilot parser limits in release and GPSP_PERF_RIG profiles."""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check(ok: bool, message: str) -> None:
    if not ok:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: run_autopilot_script_tests.py RELEASE_TESTER RIG_TESTER")
    with tempfile.TemporaryDirectory(prefix="gbadhoc-autopilot-") as tmp:
        td = Path(tmp)
        for profile, exe_arg, limit in (("release", sys.argv[1], 256),
                                        ("perf_rig", sys.argv[2], 1024)):
            exe = Path(exe_arg).resolve()

            accepted = td / (profile + "_accepted.inputs")
            accepted.write_text("wait 1\n" * limit)
            result = subprocess.run([str(exe), str(accepted)], text=True,
                                    capture_output=True)
            check(result.returncode == 0,
                  f"{profile}: parser rejected exactly {limit} file steps: {result.stdout}{result.stderr}")
            check(f"accepted: {limit} steps of {limit}" in result.stdout,
                  f"{profile}: wrong reported file-step cap")

            rejected = td / (profile + "_rejected.inputs")
            rejected.write_text("wait 1\n" * (limit + 1))
            result = subprocess.run([str(exe), str(rejected)], text=True,
                                    capture_output=True)
            check(result.returncode != 0,
                  f"{profile}: parser accepted {limit + 1} file steps")

            state = td / (profile + "_state.inputs")
            state.write_text("state\n")
            result = subprocess.run([str(exe), str(state)], text=True,
                                    capture_output=True)
            check((result.returncode == 0) == (profile == "perf_rig"),
                  f"{profile}: state command profile gate is wrong")

            predicate = td / (profile + "_predicate.inputs")
            predicate.write_text("waitram 4 0x02000000 0 0 5\n")
            result = subprocess.run([str(exe), str(predicate)], text=True,
                                    capture_output=True)
            check(result.returncode == 0, f"{profile}: predicate script rejected")
            check("script input duration: 1..6 frames (predicate-dependent)" in result.stdout,
                  f"{profile}: predicate duration bounds are wrong: {result.stdout}")

            repeat = td / (profile + "_repeat.inputs")
            repeat.write_text("repeat 1000000\nwait 2\nendrepeat\n")
            result = subprocess.run([str(exe), str(repeat)], text=True,
                                    capture_output=True)
            check(result.returncode == 0, f"{profile}: repeat script rejected")
            check("accepted: 3 steps" in result.stdout and
                  "script input duration: 2000000 frames (fixed)" in result.stdout,
                  f"{profile}: repeat iterations were confused with file steps/duration")

            nested = td / (profile + "_nested.inputs")
            nested.write_text("repeat 2\nrepeat 2\nendrepeat\nendrepeat\n")
            result = subprocess.run([str(exe), str(nested)], text=True,
                                    capture_output=True)
            check(result.returncode != 0, f"{profile}: nested repeat was accepted")

            lb = td / (profile + "_logbytes.inputs")
            lb.write_text("logbytes fp 24 0x02023BFC\n")
            result = subprocess.run([str(exe), str(lb)], text=True,
                                    capture_output=True)
            check(result.returncode == 0, f"{profile}: logbytes 24 rejected")
            lb.write_text("logbytes fp 25 0x02023BFC\n")
            result = subprocess.run([str(exe), str(lb)], text=True,
                                    capture_output=True)
            check(result.returncode != 0, f"{profile}: logbytes 25 accepted")

            fixtures = sorted((ROOT / "testdata/fixtures").glob("*.inputs"))
            for fixture in fixtures:
                result = subprocess.run([str(exe), str(fixture)], text=True,
                                        capture_output=True)
                check(result.returncode == 0,
                      f"{profile}: rejected committed fixture {fixture.name}: {result.stdout}{result.stderr}")
            print(f"PASS {profile}: {limit}-step boundary, profile grammar, predicate bounds, "
                  f"repeat semantics, {len(fixtures)} committed fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
