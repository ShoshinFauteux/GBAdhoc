#!/usr/bin/env python3
"""run_gba_state_checks.py -- the GBA core's savestate validators against their real writers
(host suite gba_state in tools/run_host_tests.py).

    python3 tools/tests/run_gba_state_checks.py [--out DIR] [--baseline]

Each test serialises real state with the production writer, checks that the validator accepts
it and the reader restores it, then corrupts one field at a time and requires a refusal:
  test_gba_cpu_state.c      cpu.cc: every legal CPU mode (and the bank switch), bad modes and a
                            bad halt state refused
  test_gba_memory_state.c   gba_memory.c: EEPROM phase/address bounds at every serial phase,
                            VCOUNT/DISPSTAT consistency
  test_gba_timer_state.c    main.c: timer reload bounds (0 and 0x10001 refused, 1 and 0x10000 kept)
  test_gba_sound_safety.c   sound.c: five out-of-range PSG fields refused; a noise-width write
                            without a retrigger takes effect (real MMIO write + mixer)
  test_savestate_preflight_loops.c  the whole preflight: its REWIND arm here (the plain arm is
                            suite savestate_preflight_loops)
Two arms: plain, and -DREWIND (the shipped default, REWIND ?= 1 in both makefiles).

--out DIR keeps every command's log and state-results.json there (default: nothing kept).
--baseline also proves the gaps of v4.0.0 (needs a git that can read the repo and the v4.0.0
tag; docs/audit/gba-core.md).
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
OUT = None
if "--out" in sys.argv:
    OUT = Path(sys.argv[sys.argv.index("--out") + 1])
    OUT.mkdir(parents=True, exist_ok=True)
ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
FLAGS = ["-O1", "-g", "-w", "-DINLINE=inline",
         "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
         "-I.", "-Ilibretro/libretro-common/include", "-Ifrontend-common"]
CFLAGS = ["-std=gnu99"] + FLAGS
LINK = ["-Wl,--gc-sections", "-Wl,--allow-multiple-definition"]
records = []


def execute(name, cmd, expected=0, marker=None):
    cmd = list(map(str, cmd))
    p = subprocess.run(cmd, text=True, capture_output=True, env=ENV)
    clean = expected != 0 or not any(s in p.stderr for s in ("runtime error:", "Sanitizer"))
    ok = p.returncode == expected and clean and (marker is None or marker in p.stdout + p.stderr)
    records.append(dict(name=name, command=cmd, exit=p.returncode, expected=expected, passed=ok))
    if OUT:
        (OUT / (name + ".log")).write_text(p.stdout + p.stderr)
        (OUT / "state-results.json").write_text(json.dumps(records, indent=2) + "\n")
    print(("PASS " if ok else "FAIL ") + name, flush=True)
    if not ok:
        print(p.stdout + p.stderr)
        raise SystemExit(1)


def build(directory, name, sources, extra=()):
    exe = directory / name
    execute(name + "-build", ["gcc", *CFLAGS, *extra, *sources, *LINK, "-o", exe])
    return exe


def build_cpu(directory, name, cpu_cc, extra=()):
    """cpu.cc is C++: compiled with g++, the test with gcc, linked by g++."""
    obj, tobj, exe = directory / (name + "-cpu.o"), directory / (name + ".o"), directory / name
    execute(name + "-build-cpu", ["g++", *FLAGS, *extra, "-c", cpu_cc, "-o", obj])
    execute(name + "-build", ["gcc", *CFLAGS, *extra, "-c", ROOT / "tools/tests/test_gba_cpu_state.c",
                              "-o", tobj])
    execute(name + "-link", ["g++", *FLAGS, tobj, obj, "-Wl,--gc-sections", "-o", exe])
    return exe


with tempfile.TemporaryDirectory(prefix="gba-state-") as directory:
    t = Path(directory)
    preflight = [ROOT / "tools/test_savestate_preflight_loops.c",
                 *[ROOT / n for n in ("input.c", "main.c", "savestate.c", "gba_memory.c", "sound.c")]]
    if "--baseline" in sys.argv:
        old = t / "v4"; old.mkdir()
        for name in ("input.c", "main.c", "savestate.c", "gba_memory.c", "sound.c"):
            blob = subprocess.check_output(["git", "show", "v4.0.0:gpsp/" + name])
            (old / name).write_bytes(blob)
        exe = build(t, "types-baseline", [preflight[0], *[old / p.name for p in preflight[1:]]],
                    ["-DEXPECT_UNSAFE_TYPES"])
        execute("types-baseline", [exe, "types"], marker="BASELINE accepts all 22")
        exe = build(t, "timer-baseline", [ROOT / "tools/tests/test_gba_timer_state.c", old / "main.c"],
                    ["-DEXPECT_UNSAFE"])
        execute("timer-baseline", [exe], marker="reload=0 accepted=1")
        test = (ROOT / "tools/tests/test_gba_memory_state.c").read_text()
        test = test.replace('"../../gba_memory.c"', '"' + str(old / "gba_memory.c") + '"')
        test = test.replace('"../../savestate.c"', '"' + str(ROOT / "savestate.c") + '"')
        (t / "memory-old-test.c").write_text(test)
        exe = build(t, "memory-baseline", [t / "memory-old-test.c"], ["-DEXPECT_UNSAFE"])
        execute("memory-baseline-overrun", [exe, "consume"], 1, "AddressSanitizer: global-buffer-overflow")
    for rewind in (False, True):
        suffix = "-rewind" if rewind else ""
        extra = ["-DREWIND"] if rewind else []
        exe = build(t, "memory" + suffix, [ROOT / "tools/tests/test_gba_memory_state.c"], extra)
        execute("memory" + suffix, [exe])
        exe = build(t, "timer" + suffix,
                    [ROOT / "tools/tests/test_gba_timer_state.c", ROOT / "main.c"], extra)
        execute("timer" + suffix, [exe])
        exe = build(t, "sound" + suffix, [ROOT / "tools/tests/test_gba_sound_safety.c"], extra)
        execute("sound" + suffix, [exe])
        exe = build_cpu(t, "cpu" + suffix, ROOT / "cpu.cc", extra)
        execute("cpu" + suffix, [exe])
        if rewind:   # the plain arm is suite savestate_preflight_loops
            exe = build(t, "preflight" + suffix, preflight, extra)
            execute("preflight" + suffix, [exe])
print("GBA state checks: %d steps passed (cpu, memory, timer, sound, preflight; plain and REWIND)"
      % len(records))
