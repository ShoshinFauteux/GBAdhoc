#!/usr/bin/env python3
"""psp/list_marks.h (the 4.6.0-beta.4 list lag): runs
tools/tests/test_list_marks_cache.c against the real header, then against
mutants of it, each of which must fail."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HDR = (ROOT / "psp/list_marks.h").read_text(encoding="utf-8")
TEST = ROOT / "tools/tests/test_list_marks_cache.c"
CC = ["gcc", "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
      "-fsanitize=address,undefined"]


def run(header):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-lmc-") as d:
        Path(d, "list_marks.h").write_text(header, encoding="utf-8")
        shutil.copy(TEST, Path(d, "t.c"))
        exe = Path(d, "t")
        subprocess.run(CC + ["-I" + d, str(Path(d, "t.c")), "-o", str(exe)],
                       check=True)
        return subprocess.run([str(exe)], capture_output=True, text=True,
                              env={"ASAN_OPTIONS": "detect_leaks=0"},
                              timeout=60)


r = run(HDR)
if r.returncode:
    raise SystemExit("list marks cache FAILED\n" + r.stdout + r.stderr)
print(r.stdout.strip())

MUTANTS = (
    ("first file there does not answer", "if (yes || k == hi)", "if (k == hi)"),
    ("the cursor not first", "if (lm_open(io, rom, LM_ALL_K))\n   {",
     "if (0)\n   {"),
    ("progress kept across games", "if (p->rom != rom || p->probe < lo",
     "if (p->probe < lo"),
    ("forget leaves the sweep done", "   p->swept = 0;\n   if (p->rom",
     "   if (p->rom"),
    ("cheats asked without cheats",
     "if (!io->cheats && (want & LM_CHT_K)", "if (0 && (want & LM_CHT_K)"),
    ("budget ignored", "while (io->more(io->user, done))", "while (1)"),
    ("the legacy slot skipped", "else\n      p->probe = k + 1;",
     "else\n      p->probe = k + 2;"),
)
for label, old, new in MUTANTS:
    assert HDR.count(old) == 1, label
    bad = run(HDR.replace(old, new))
    if bad.returncode == 0:
        raise SystemExit("mutant survived: " + label)
    print("mutant (%s): failed as it must" % label)
print("list marks cache: %d mutants fail" % len(MUTANTS))
