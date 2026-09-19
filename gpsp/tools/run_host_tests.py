#!/usr/bin/env python3
"""run_host_tests.py -- THE host test command.

    python tools/run_host_tests.py [--json PATH] [--only NAME ...]

Every host-runnable test in one place, with one rule: a suite that cannot run
says so, by name and with a reason.  Nothing is allowed to disappear quietly.

WHY THIS EXISTS.  The suites were four separate scripts plus three test files
with no runner at all (test_perf_rig.c, test_video_buffers.c,
test_mgift_profiles.c were documented in docs/ and invoked by hand), so
"the host tests pass" meant whichever ones the person remembered.  Worse, a
suite that could not build looked identical to one that had no failures.

TWO ENVIRONMENT PROBLEMS IT SOLVES, both specific to this repo on Windows:

  * There is no native gcc.  The only C compiler is inside WSL, so a suite that
    compiles something is re-run there.  Paths are translated; nothing else
    about the suite changes.
  * A git worktree's .git file holds a WINDOWS path, which git inside WSL
    cannot resolve -- so a suite that shells out to `git show` for a baseline
    fails with exit 128 in every worktree.  The baseline is therefore extracted
    HERE, with the Windows git that can see it, into a directory handed to the
    suite as GBADHOC_BASELINE_DIR.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

PASS, FAIL, SKIP = "pass", "fail", "skip"


# --------------------------------------------------------------- environment
def have(cmd: str) -> bool:
    return shutil.which(cmd) is not None


def native_cc() -> str | None:
    for c in ("cc", "gcc", "clang"):
        if have(c):
            return c
    return None


def wsl_available() -> bool:
    if not have("wsl"):
        return False
    try:
        r = subprocess.run(["wsl", "-e", "sh", "-c", "command -v gcc"],
                           capture_output=True, timeout=60)
        return r.returncode == 0
    except Exception:
        return False


def wsl_path(p: Path) -> str:
    """C:\\a\\b -> /mnt/c/a/b.  wslpath is authoritative when present."""
    try:
        r = subprocess.run(["wsl", "-e", "wslpath", "-a", str(p).replace("\\", "/")],
                           capture_output=True, text=True, timeout=60)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    except Exception:
        pass
    s = str(p).replace("\\", "/")
    if len(s) > 1 and s[1] == ":":
        return "/mnt/" + s[0].lower() + s[2:]
    return s


class Env:
    """How to run a shell command that may need a C compiler."""

    def __init__(self) -> None:
        self.cc = native_cc()
        self.wsl = False if self.cc else wsl_available()

    @property
    def can_compile(self) -> bool:
        return bool(self.cc) or self.wsl

    def why_not(self) -> str:
        return ("no C compiler: none of cc/gcc/clang on PATH, and WSL is "
                "unavailable or has no gcc")

    def run_path(self, p: Path) -> str:
        """A path spelled the way commands from run() will see it."""
        return wsl_path(p) if self.wsl else str(p)

    def run(self, shell_cmd: str, extra_env: dict[str, str] | None = None,
            timeout: int = 900) -> subprocess.CompletedProcess:
        env = dict(os.environ)
        if extra_env:
            env.update(extra_env)
        if self.wsl:
            exports = ""
            if extra_env:
                exports = "".join(
                    "export %s=%s; " % (k, wsl_path(Path(v)) if os.path.isabs(v) else v)
                    for k, v in extra_env.items())
            cmd = ["wsl", "-e", "sh", "-c",
                   "cd %s && %s%s" % (wsl_path(REPO), exports, shell_cmd)]
            return subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=timeout)
        return subprocess.run(["bash", "-c", shell_cmd], cwd=str(REPO), env=env,
                              capture_output=True, text=True, timeout=timeout)


# ------------------------------------------------------------------ baseline
# run_rom_load_tests compares the current ROM loader against a fixed commit.
BASELINE_COMMIT = "ecdd8bcfa7396a41523bcf3d4230d3f1ee99da7f"
BASELINE_FILES = ("psp/config_psp.c", "psp/config_psp.h", "gba_memory.c")


def extract_baseline(dest: Path) -> tuple[bool, str]:
    """Materialise the baseline files with the git that can actually see the
    repo.  Returns (ok, detail)."""
    try:
        subprocess.run(["git", "cat-file", "-e", BASELINE_COMMIT + "^{commit}"],
                       cwd=str(REPO), capture_output=True, check=True)
    except Exception:
        return False, "baseline commit %s is not in this repository" % BASELINE_COMMIT[:12]
    for rel in BASELINE_FILES:
        try:
            blob = subprocess.run(["git", "show", "%s:%s" % (BASELINE_COMMIT, rel)],
                                  cwd=str(REPO), capture_output=True, check=True)
        except subprocess.CalledProcessError:
            return False, "baseline lacks %s" % rel
        out = dest / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(blob.stdout)
    return True, "extracted %d files at %s" % (len(BASELINE_FILES), BASELINE_COMMIT[:12])


# -------------------------------------------------------------------- suites
SDK_CACHE = REPO / ".sdk-include" / "sdk"


def psp_sdk_include() -> Path | None:
    """The PSP SDK headers.

    Two suites include production PSP code (psp/video_psp.c, psp/mgift_net.c),
    so they need <pspkernel.h> and friends.  Those headers live in the pspdev
    docker image, so they are cached once into .sdk-include/ (gitignored, about
    1 MB) and reused.  Only the sdk/ subtree: pspdev's psp/include is a full
    newlib tree, and putting that on the path makes assert() resolve to newlib's
    __assert_func, which then fails to link against glibc.
    """
    if (SDK_CACHE / "pspgu.h").exists():
        return SDK_CACHE
    for cand in (os.environ.get("PSPDEV", ""), "/usr/local/pspdev"):
        if cand and (Path(cand) / "psp/sdk/include/pspgu.h").exists():
            return Path(cand) / "psp/sdk/include"
    if have("docker"):
        try:
            SDK_CACHE.parent.mkdir(parents=True, exist_ok=True)
            env = dict(os.environ, MSYS_NO_PATHCONV="1", MSYS2_ARG_CONV_EXCL="*")
            subprocess.run(
                ["docker", "run", "--rm",
                 "-v", "%s:/out" % SDK_CACHE.parent, "pspdev/pspdev",
                 "sh", "-c", "cp -r /usr/local/pspdev/psp/sdk/include /out/sdk"],
                capture_output=True, timeout=300, check=True, env=env)
            if (SDK_CACHE / "pspgu.h").exists():
                print("sdk: cached PSP SDK headers into %s" % SDK_CACHE)
                return SDK_CACHE
        except Exception as e:
            print("sdk: could not cache headers from the pspdev image (%s)" % e)
    return None


def suites(env: Env, baseline: Path | None):
    """name -> (needs_compiler, precondition() -> reason|None, shell command)"""

    def no_baseline():
        return None if baseline else "baseline unavailable (see log)"

    def no_sdk():
        return None if psp_sdk_include() else (
            "PSP SDK headers unavailable: not on this host, and they could not "
            "be cached from the pspdev docker image")

    sdk_path = psp_sdk_include()
    # Commands run with the repo as cwd, and under WSL a Windows absolute path
    # is meaningless -- so express the include as a repo-relative path whenever
    # it lives inside the tree (which the cached copy always does).
    if sdk_path is None:
        sdk = "/nonexistent"
    else:
        try:
            sdk = sdk_path.relative_to(REPO).as_posix()
        except ValueError:
            sdk = env.run_path(sdk_path)
    return [
        # name, needs_cc, precondition, command
        ("smc_safety", True, lambda: None,
         "gcc -Wall -Wextra -O1 -o /tmp/t_smc tools/test_smc_safety.c && /tmp/t_smc"),

        ("ff", True, lambda: None,
         "python3 tools/run_ff_tests.py"),

        ("mystery_gift", True, lambda: None,
         "python3 tools/run_mgift_tests.py"),

        # Includes psp/mgift_net.c, which includes <pspkernel.h>: the SDK stubs
        # in the test replace the FUNCTIONS, not the headers.
        ("mystery_gift_profiles", True, no_sdk,
         ("gcc -std=gnu99 -w -ffunction-sections -fdata-sections -I%s "
          "-Ifrontend-common -Ipsp -Ilibretro/libretro-common/include "
          "tools/test_mgift_profiles.c -Wl,--gc-sections -o /tmp/t_mgp "
          "&& /tmp/t_mgp") % sdk),

        ("rom_load", True, no_baseline,
         "python3 tools/run_rom_load_tests.py"),

        ("perf_rig", True, lambda: None,
         "gcc -std=gnu99 -Wall -Wextra -Ifrontend-common "
         "-o /tmp/t_rig tools/test_perf_rig.c && /tmp/t_rig"),

        # -ffunction-sections/-fdata-sections are REQUIRED: without them
        # --gc-sections has nothing to discard, so every GU call video_psp.c
        # makes stays undefined and the link fails.  The command documented in
        # docs/FF-ARTIFACT-FIX.md omitted them, which is why this suite was
        # only ever run by hand.  VID_TRIPLE is defined by the test itself.
        ("video_buffers", True, no_sdk,
         ("gcc -std=gnu99 -w -DGPSP_PLAYABLE -ffunction-sections -fdata-sections "
          "-I%s -Ifrontend-common -Ipsp tools/test_video_buffers.c "
          "-Wl,--gc-sections -o /tmp/t_vb && /tmp/t_vb") % sdk),

        ("perf_loop", False, lambda: None,
         "python3 tools/test_perf_loop.py"),
    ]


# ---------------------------------------------------------------------- main
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=str(REPO / "host-test-results.json"))
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    env = Env()
    where = ("native %s" % env.cc) if env.cc else ("wsl gcc" if env.wsl else "none")
    print("compiler: %s" % where)

    tmp = Path(tempfile.mkdtemp(prefix="gbadhoc-host-tests-"))
    baseline: Path | None = None
    bdir = tmp / "baseline"
    ok, detail = extract_baseline(bdir)
    print("baseline: %s" % detail)
    if ok:
        baseline = bdir

    results = []
    for name, needs_cc, pre, cmd in suites(env, baseline):
        if args.only and name not in args.only:
            continue
        reason = None
        if needs_cc and not env.can_compile:
            reason = env.why_not()
        if reason is None:
            reason = pre()
        if reason is not None:
            print("  SKIP %-22s %s" % (name, reason))
            results.append({"suite": name, "status": SKIP, "reason": reason})
            continue

        extra = {"GBADHOC_BASELINE_DIR": str(baseline)} if baseline else None
        t0 = time.time()
        try:
            r = env.run(cmd, extra_env=extra)
            out = (r.stdout or "") + (r.stderr or "")
            status = PASS if r.returncode == 0 else FAIL
            code = r.returncode
        except subprocess.TimeoutExpired:
            out, status, code = "timed out", FAIL, -1
        dt = time.time() - t0
        print("  %-4s %-22s %5.1fs" % (status.upper(), name, dt))
        if status == FAIL or args.verbose:
            for line in out.strip().splitlines()[-25:]:
                print("       | %s" % line)
        results.append({"suite": name, "status": status, "exit": code,
                        "seconds": round(dt, 2),
                        "output": out[-4000:] if status == FAIL else ""})

    counts = {s: sum(1 for r in results if r["status"] == s) for s in (PASS, FAIL, SKIP)}
    manifest = {
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "repo": str(REPO),
        "commit": subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(REPO),
                                 capture_output=True, text=True).stdout.strip(),
        "compiler": where,
        "baseline": detail,
        "counts": counts,
        "suites": results,
    }
    Path(args.json).write_text(json.dumps(manifest, indent=2))

    print("\n%d passed, %d failed, %d skipped -> %s"
          % (counts[PASS], counts[FAIL], counts[SKIP], args.json))
    if counts[SKIP]:
        print("SKIPPED suites are not passes. Each reason is in the manifest.")
    return 1 if counts[FAIL] else 0


if __name__ == "__main__":
    sys.exit(main())
