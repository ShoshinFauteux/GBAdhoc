#!/usr/bin/env python3
"""check_flags.py -- fail when the build flags drift.

    tools/check_flags.py                 source checks, a second or two, no docker
    tools/check_flags.py --compile       plus: what the compilers resolve (docker)
    tools/check_flags.py --build DIR     plus: a release build's manifest
    tools/check_flags.py --update        rewrite tools/flags.lock after a change
                                         you meant; review it in the same commit

Exit 0 clean, 1 drift, 2 usage or a tool that would not run.

WHY.  A flag that moves silently has cost this project more than any bug: a
release recipe that lived in a gitignored file, experiments that never reached
the core, a switch nothing read.  gpsp_features.h and psp/fe_features.h say what
the emulator is.  This holds them to that:

  lock       the feature lists, make defaults and profile recipes equal
             tools/flags.lock, so changing one is a visible, deliberate diff
  recipes    no profile, makefile or wrapper restates a baked feature
  removed    a switch in docs/BUILD-SWITCHES.md "Removed" stays out of every
             #if, -D and make conditional
  dead       everything baked or passed by a makefile is read by some source
  sides      the frontend never tests a core feature (it would always be off:
             core features reach it as weak symbols), nor the core a frontend one
  docs       every baked switch has a line in docs/BUILD-SWITCHES.md
  oracles    every -D/-U in the twin's tables names something that exists
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LOCK = REPO / "tools" / "flags.lock"
CORE_HDR, FE_HDR = "gpsp_features.h", "psp/fe_features.h"
SWITCHES = "docs/BUILD-SWITCHES.md"

CORE_DIRS = (".", "mips", "arm", "x86", "libretro", "gbalink")   # non-recursive, see sources()
FE_DIRS = ("psp", "psp/labab", "frontend-common", "netdrv", "gbcore", "gbcore/tgbdual")
C_EXT = {".c", ".h", ".cc", ".cpp", ".S", ".s"}
# Read by both sides by design: they change what is linked, so each make gets them.
BOTH_SIDES = {"GBA_LINK", "REWIND", "LAYOUT_PIN", "LAYOUT_PAD_BYTES"}
PP = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b(.*)$", re.M)
WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def read(rel: str) -> str:
    return (REPO / rel).read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")


def header_defs(rel: str) -> dict[str, str]:
    return dict(re.findall(r"^#define (\w+) (\S+)$", read(rel), re.M))


def sources(dirs) -> list[Path]:
    out = []
    for d in dirs:
        out += [p for p in sorted((REPO / d).glob("*")) if p.suffix in C_EXT and p.is_file()]
    return out


def tested_words(paths) -> dict[str, set[str]]:
    """word -> files that test it in a preprocessor conditional."""
    seen: dict[str, set[str]] = {}
    for p in paths:
        rel = p.relative_to(REPO).as_posix()
        if rel in (CORE_HDR, FE_HDR):
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        text = re.sub(r"\\\r?\n", " ", text)
        for cond in PP.findall(text):
            for w in WORD.findall(re.sub(r"/\*.*?(\*/|$)|//.*$", " ", cond)):
                seen.setdefault(w, set()).add(rel)
    return seen


def make_defaults(rel: str) -> dict[str, str]:
    return dict(re.findall(r"^[ \t]*([A-Z][A-Z0-9_]*) \?= (\S+)[ \t]*$", read(rel), re.M))


def profiles() -> dict[str, dict[str, str]]:
    text = read("tools/build.sh")
    body = text[text.index("profile_flags() {"):text.index("# ----------------------------------------------------------------- args")]
    out = {}
    for name, block in re.findall(r"^    (\w+)\)\n(.*?)^      ;;$", body, re.M | re.S):
        vals = {}
        for key in ("CORE_FLAGS", "FE_DEFS", "TITLE", "PSP_MAKE_FLAGS"):
            m = re.findall(r'^\s*%s="([^"]*)"' % key, block, re.M)
            if m:
                vals[key] = " ".join(m[-1].split())
        out[name] = vals
    return out


def current() -> list[str]:
    """Everything the lock pins, as sorted `section key=value` lines."""
    lines = ["core %s=%s" % kv for kv in header_defs(CORE_HDR).items()]
    lines += ["frontend %s=%s" % kv for kv in header_defs(FE_HDR).items()]
    pins = ("GBA_LINK", "REWIND", "LAYOUT_PIN", "LAYOUT_PAD_BYTES", "CHEATS", "HOT_DATA_GUARD",
            "PSP_PIXFMT", "TGB_LAZY_TIMER", "PSP_LARGE_MEMORY")
    for rel in ("Makefile", "psp/Makefile"):
        d = make_defaults(rel)
        lines += ["make:%s %s=%s" % (rel, k, d[k]) for k in pins if k in d]
    text = read("tools/build.sh")
    lines += ["build.sh %s=%s" % kv for kv in re.findall(r'^(\w+)="\$\{\1:-(\w+)\}"', text, re.M)]
    lines += ["build.sh INPUT_FRESH=%s" % v for v in re.findall(r'"\$\{INPUT_FRESH:-(\w+)\}"', text)]
    for name, vals in profiles().items():
        lines += ["profile:%s %s=%s" % (name, k, v) for k, v in sorted(vals.items())]
    return sorted(set(lines))


def lock_lines() -> list[str]:
    if not LOCK.exists():
        return []
    return [l.strip() for l in LOCK.read_text(encoding="utf-8").splitlines()
            if l.strip() and not l.startswith("#")]


# ---------------------------------------------------------------- the checks
def check_lock(say):
    have, want = set(current()), set(lock_lines())
    if not want:
        say("lock", "tools/flags.lock is missing or empty: tools/check_flags.py --update")
    for l in sorted(have - want):
        say("lock", "not in tools/flags.lock: %s" % l)
    for l in sorted(want - have):
        say("lock", "gone from the tree, still in tools/flags.lock: %s" % l)


def check_recipes(say):
    core, fe = header_defs(CORE_HDR), header_defs(FE_HDR)
    for name, vals in profiles().items():
        for t in vals.get("CORE_FLAGS", "").split():
            if t.split("=")[0] in core:
                say("recipes", "profile %s restates baked feature %s" % (name, t))
        for t in vals.get("FE_DEFS", "").split():
            if t[2:].split("=")[0] in fe:
                say("recipes", "profile %s restates frontend feature %s" % (name, t))
    fe_text = read("psp/Makefile")
    for n in fe:
        if re.search(r"-D%s\b" % n, fe_text):
            say("recipes", "psp/Makefile passes -D%s; it belongs to %s" % (n, FE_HDR))
    for rel in ("tools/build_perf.py", "build.sh"):
        text = read(rel)
        for n in fe:
            if re.search(r"-D%s\b" % n, text):
                say("recipes", "%s passes -D%s" % (rel, n))
    if "-include $(CORE_DIR)/gpsp_features.h" not in read("Makefile"):
        say("recipes", "root Makefile no longer force-includes gpsp_features.h")
    if "-include fe_features.h" not in fe_text:
        say("recipes", "psp/Makefile no longer force-includes fe_features.h")


def removed_names() -> list[str]:
    text = read(SWITCHES)
    m = re.search(r"^## Removed\n(.*?)(?=^## |\Z)", text, re.M | re.S)
    names: list[str] = []
    for row in (m.group(1) if m else "").splitlines():
        if row.startswith("| `"):
            names += re.findall(r"`([A-Z][A-Z0-9_]+)`", row.split("|")[1])
    return names


def check_removed(say, words):
    names = removed_names()
    if len(names) < 22:
        say("removed", "docs/BUILD-SWITCHES.md lists only %d removed switches" % len(names))
    texts = {rel: read(rel) for rel in ("Makefile", "psp/Makefile", "tools/build.sh", "tools/drprof/Makefile",
                                        "tools/drprof/dr_corpus.py")}
    for n in names:
        for f in sorted(words.get(n, ())):
            say("removed", "%s is removed but %s tests it" % (n, f))
        for rel, t in texts.items():
            if re.search(r"-D%s\b|\$\(%s\)" % (n, n), t):
                say("removed", "%s is removed but %s sets it" % (n, rel))


def check_dead(say, words):
    for n in header_defs(CORE_HDR):
        if n not in words:
            say("dead", "%s is baked but no core or frontend source tests it" % n)
    for n in header_defs(FE_HDR):
        if n not in words and not n.startswith("ND_"):
            say("dead", "%s is in %s but no source tests it" % (n, FE_HDR))
    # A make switch that exists to pass its own name: `ifeq ($(X),1)` + `-DX`.
    for rel in ("Makefile", "psp/Makefile"):
        pairs = re.findall(r"ifeq \(\$\((\w+)\),1\)\n\s*\w*FLAGS\s*\+=\s*-D(\w+)", read(rel))
        for var, n in sorted(set(pairs)):
            if n not in words:
                say("dead", "%s: %s=1 passes -D%s and no source tests it" % (rel, var, n))


def check_sides(say):
    core, fe = set(header_defs(CORE_HDR)), set(header_defs(FE_HDR))
    fe_seen = tested_words(sources(FE_DIRS))
    core_seen = tested_words(sources(CORE_DIRS))
    # gpsp_profile.h and gpsp_config.h are read by both sides; the frontend sees
    # their answer for a core feature as "off", which they are written for.
    shared = {"gpsp_profile.h", "gpsp_config.h"}
    for n in sorted((core - BOTH_SIDES) & set(fe_seen)):
        say("sides", "frontend tests core feature %s: %s (always off there; use a weak symbol)"
            % (n, ", ".join(sorted(fe_seen[n]))))
    for n in sorted(fe - BOTH_SIDES):
        hit = core_seen.get(n, set()) - shared
        if hit and not n.startswith("ND_"):
            say("sides", "core tests frontend feature %s: %s" % (n, ", ".join(sorted(hit))))


def check_docs(say):
    text = read(SWITCHES)
    for n in list(header_defs(CORE_HDR)) + list(header_defs(FE_HDR)):
        if not re.search(r"\b%s\b" % n, text):
            say("docs", "%s has no line in %s" % (n, SWITCHES))


def check_oracles(say, words):
    known = set(words) | set(header_defs(CORE_HDR)) | set(header_defs(FE_HDR))
    all_src = sources(CORE_DIRS + FE_DIRS)
    for rel in ("tools/drprof/Makefile", "tools/drprof/dr_corpus.py", "tools/drprof/dr_build.sh"):
        for n in sorted(set(re.findall(r"-[DU]([A-Z][A-Z0-9_]{2,})", read(rel)))):
            base = n[len("GPSP_NO_"):] if n.startswith("GPSP_NO_") else n
            if base in known or base in {"NAME", "SOMETHING", "FOO"}:
                continue
            if not any(re.search(r"\b%s\b" % base, p.read_text(errors="replace")) for p in all_src):
                say("oracles", "%s names -D%s, which no source mentions" % (rel, n))


def norm(flags: str) -> set[str]:
    return set(flags.split())


def expected_fe() -> set[str]:
    return {"-D%s=%s" % kv for kv in header_defs(FE_HDR).items()}


def check_compile(say):
    """What the compilers resolve from a bare make line, in the build image."""
    if not shutil.which("docker"):
        raise SystemExit("check_flags: --compile needs docker")
    src = str(REPO).replace("\\", "/")
    cmd = ["docker", "run", "--rm", "-v", src + ":/build", "-w", "/build", "pspdev/pspdev", "sh", "-c",
           "echo @core; make -s platform=psp1 print-features; echo @fe; "
           "make -s -C psp EXTRA_DEFS='-DGPSP_PLAYABLE' print-features"]
    env = dict(os.environ, MSYS_NO_PATHCONV="1", MSYS2_ARG_CONV_EXCL="*")
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, encoding="utf-8", errors="replace")
    if r.returncode:
        raise SystemExit("check_flags: docker failed:\n" + r.stdout[-600:] + r.stderr[-600:])
    parts = r.stdout.replace("\r", "").split("@fe")
    got = lambda s: set(re.findall(r"^[A-Z0-9_]+=\S+$", s, re.M))
    want_core = {"%s=%s" % kv for kv in header_defs(CORE_HDR).items()} | {"GBA_LINK=1", "REWIND=1"}
    have_core, have_fe = got(parts[0]), got(parts[1] if len(parts) > 1 else "")
    want_fe = {"%s=%s" % kv for kv in header_defs(FE_HDR).items()}
    for t in sorted(want_core ^ have_core):
        say("compile", "core: %s %s" % (t, "missing from the compiler" if t in want_core else "unexpected"))
    for t in sorted(want_fe ^ have_fe):
        say("compile", "frontend: %s %s" % (t, "missing from the compiler" if t in want_fe else "unexpected"))


def check_build(say, out: Path):
    mp = out / "build-manifest.json"
    if not mp.exists():
        raise SystemExit("check_flags: no %s" % mp)
    m = json.loads(mp.read_text())
    if m.get("profile") != "release":
        say("build", "%s is profile %r, not release" % (out, m.get("profile")))
    want_core = {"%s=%s" % kv for kv in header_defs(CORE_HDR).items()} | {
        "GPSP_PROFILE=release", "GBA_LINK=1", "REWIND=1"}
    have = norm(m.get("coreFlags", ""))
    # RFU_SCHED=1, the adapter-ticks beta switch, is a release option when both
    # makes had it (tools/make_release.sh applies the same rule).
    if "RFU_SCHED=1" in have and "RFU_SCHED=1" in m.get("pspMakeFlags", "").split():
        want_core.add("RFU_SCHED=1")
    for t in sorted(want_core ^ have):
        say("build", "coreFlags: %s %s" % (t, "missing" if t in want_core else "extra"))
    prof = profiles()["release"]
    want_fe = norm(prof["FE_DEFS"]) | expected_fe()
    have_fe = norm(m.get("frontendDefines", ""))
    for t in sorted(want_fe ^ have_fe):
        say("build", "frontendDefines: %s %s" % (t, "missing" if t in want_fe else "extra"))
    if m.get("title") != prof["TITLE"]:
        say("build", "title %r" % m.get("title"))
    pad = make_defaults("psp/Makefile").get("LAYOUT_PAD_BYTES")
    if str(m.get("layoutPadBytes")) != pad or m.get("layoutPin") != 1:
        say("build", "layout pin=%s pad=%s, want 1 and %s" % (m.get("layoutPin"), m.get("layoutPadBytes"), pad))


def main() -> int:
    ap = argparse.ArgumentParser(description="fail when the build flags drift")
    ap.add_argument("--compile", action="store_true", help="also ask the compilers (docker)")
    ap.add_argument("--build", metavar="DIR", help="also check a release build's manifest")
    ap.add_argument("--update", action="store_true", help="rewrite tools/flags.lock")
    a = ap.parse_args()
    if a.update:
        LOCK.write_text("# tools/flags.lock -- the build flags as committed.  tools/check_flags.py fails when\n"
                        "# the tree differs; change it with --update, in the commit that means it.\n"
                        + "\n".join(current()) + "\n", encoding="utf-8", newline="\n")
        print("wrote %s (%d lines)" % (LOCK.relative_to(REPO), len(current())))
    fails: list[str] = []
    say = lambda area, msg: fails.append("%-8s %s" % (area, msg))
    words = tested_words(sources(CORE_DIRS + FE_DIRS))
    check_lock(say)
    check_recipes(say)
    check_removed(say, words)
    check_dead(say, words)
    check_sides(say)
    check_docs(say)
    check_oracles(say, words)
    if a.compile:
        check_compile(say)
    if a.build:
        check_build(say, Path(a.build))
    for f in fails:
        print("DRIFT " + f)
    if fails:
        print("check_flags: %d problem(s)" % len(fails))
        return 1
    print("check_flags: clean (%d core, %d frontend, %d removed)"
          % (len(header_defs(CORE_HDR)), len(header_defs(FE_HDR)), len(removed_names())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
