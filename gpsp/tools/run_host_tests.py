#!/usr/bin/env python3
"""run_host_tests.py -- THE host test command.

    python tools/run_host_tests.py [--json PATH] [--only NAME ...] [--strict]

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
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

PASS, FAIL, SKIP = "pass", "fail", "skip"
MAX_HOST_SUITES = 30


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
                    "export %s=%s; " % (k, shlex.quote(
                        wsl_path(Path(v)) if os.path.isabs(v) else v))
                    for k, v in extra_env.items())
            cmd = ["wsl", "-e", "sh", "-c",
                   "cd %s && %s%s" % (shlex.quote(wsl_path(REPO)), exports, shell_cmd)]
            return subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=timeout)
        if self.cc and self.cc != "gcc":
            shell_cmd = re.sub(r"\bgcc\b", self.cc, shell_cmd)
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
    ok, gb_detail = extract_gb_baseline(dest / "gb")
    if not ok:
        return False, gb_detail
    return True, "extracted %d files at %s; %s" % (
        len(BASELINE_FILES), BASELINE_COMMIT[:12], gb_detail)


# run_gb_tests proves single-player GB/GBC bit-identical to the 3.0.0 release
# (gbcore/tests/bitident.c): the whole GB core and adapter of that commit.
GB_BASELINE_COMMIT = "696ad10a21d92e83400d5fb45a2856e6032d2824"
GB_BASELINE_PATHS = ("gbcore", "frontend-common/fe_console.h")


def extract_gb_baseline(dest: Path) -> tuple[bool, str]:
    try:
        names = subprocess.run(
            ["git", "ls-tree", "-r", "--name-only", GB_BASELINE_COMMIT,
             *GB_BASELINE_PATHS], cwd=str(REPO), capture_output=True,
            check=True, text=True).stdout.split()
    except Exception:
        return False, "GB baseline commit %s is not in this repository" % GB_BASELINE_COMMIT[:12]
    for rel in names:
        blob = subprocess.run(["git", "show", "%s:%s" % (GB_BASELINE_COMMIT, rel)],
                              cwd=str(REPO), capture_output=True, check=True)
        out = dest / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(blob.stdout)
    return True, "GB baseline %d files at %s" % (len(names), GB_BASELINE_COMMIT[:12])


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

        # The browser's favourites list (roms/favourites.txt): in-place parse,
        # hashed lookup, add/remove compaction, serialise, and the full-list /
        # full-pool / oversized-file edges.  Pure code shared with the PSP.
        ("fe_favs", True, lambda: None,
         "gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror "
         "-fsanitize=address,undefined -Ifrontend-common "
         "-o /tmp/t_fe_favs tools/test_fe_favs.c && "
         "ASAN_OPTIONS=detect_leaks=0 /tmp/t_fe_favs"),

        ("mystery_gift", True, lambda: None,
         "python3 tools/run_mgift_tests.py"),

        # Exercise the real GBP latch and rumble timer after a savestate clock
        # rewind, including restored GPIO rumble and fractional frame duty.
        ("gbp_rumble_restore", True, lambda: None,
          ("gcc -std=gnu99 -w -DINLINE=inline -ffunction-sections "
           "-fdata-sections -Ilibretro/libretro-common/include "
           "-o /tmp/t_gbp_rumble tools/tests/test_gbp_rumble_restore.c "
           "-Wl,--gc-sections && /tmp/t_gbp_rumble")),

        # Regression coverage for nested channel-validation loops in the real
        # savestate preflight.  Malformed late channels must be rejected before
        # any component's state-reading function is entered.
        ("savestate_preflight_loops", True, lambda: None,
         ("gcc -std=gnu99 -O1 -g -w -fsanitize=address,undefined "
          "-ffunction-sections -fdata-sections "
          "-DINLINE=inline -Ilibretro/libretro-common/include "
          "-Ifrontend-common tools/test_savestate_preflight_loops.c input.c main.c "
          "savestate.c gba_memory.c sound.c -Wl,--gc-sections "
          "-Wl,--allow-multiple-definition "
          "-o /tmp/t_savestate_preflight_loops && "
          "ASAN_OPTIONS=detect_leaks=0 /tmp/t_savestate_preflight_loops")),

        ("cpu_interrupt_policy", True, lambda: None,
         "gcc -std=gnu99 -Wall -Wextra -Werror "
         "-o /tmp/t_cpu_interrupt_policy "
         "tools/tests/test_cpu_interrupt_policy.c && "
         "/tmp/t_cpu_interrupt_policy"),

        ("sram_interrupted_write", True, lambda: None,
         "gcc -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter "
         "-fsanitize=address,undefined -ffunction-sections -fdata-sections "
         "-Ifrontend-common -Inetdrv -Ilibretro/libretro-common/include "
         "-o /tmp/t_sram_interrupted_write "
         "tools/test_sram_interrupted_write.c -Wl,--gc-sections && "
         "ASAN_OPTIONS=detect_leaks=0 /tmp/t_sram_interrupted_write"),

        # FF durability nudge: a battery save made during fast-forward is
        # persisted without waiting for FF to end; zero yields when nothing
        # is pending; a write racing the writer's scan is never credited.
        ("sram_ff_nudge", True, lambda: None,
         "gcc -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter "
         "-fsanitize=address,undefined -ffunction-sections -fdata-sections "
         "-Ifrontend-common -Inetdrv -Ilibretro/libretro-common/include "
         "-o /tmp/t_sram_ff_nudge "
         "tools/tests/test_sram_ff_nudge.c -Wl,--gc-sections && "
         "ASAN_OPTIONS=detect_leaks=0 /tmp/t_sram_ff_nudge"),

        # The gamepak buffers: file lifecycle, then the ROM page cache's
        # startup budget (docs/ROM-RESIDENCY.md) -- the real
        # init_gamepak_buffer against a byte-budgeted malloc: the post-load
        # floor, the paged fallback, the PSP-1000's unchanged mallocs and the
        # spare pool that must never reach free().
        ("gamepak_file_lifecycle", True, lambda: None,
         "gcc -std=gnu99 -O1 -g -w -ffunction-sections -fdata-sections "
         "-DINLINE=inline -Ilibretro/libretro-common/include "
         "-Ifrontend-common -o /tmp/t_gamepak_file_lifecycle "
         "tools/test_gamepak_file_lifecycle.c -Wl,--gc-sections && "
         "/tmp/t_gamepak_file_lifecycle && "
         "gcc -std=gnu99 -O1 -g -w -fsanitize=address,undefined "
         "-ffunction-sections -fdata-sections "
         "-DINLINE=inline -Ilibretro/libretro-common/include "
         "-Ifrontend-common -o /tmp/t_gamepak_residency_budget "
         "tools/tests/test_gamepak_residency_budget.c -Wl,--gc-sections && "
         "ASAN_OPTIONS=detect_leaks=0 /tmp/t_gamepak_residency_budget"),

        # Exercise the production DMA span planner across paired region
        # transitions, including decrementing and staggered boundaries.
        ("dma_region_segments", True, lambda: None,
         ("gcc -std=gnu99 -O1 -g -w -fsanitize=address,undefined "
          "-ffunction-sections -fdata-sections -DINLINE=inline "
          "-Ilibretro/libretro-common/include -Ifrontend-common "
          "-o /tmp/t_dma_region_segments "
          "tools/tests/test_dma_region_segments.c -Wl,--gc-sections "
          "-Wl,--allow-multiple-definition && "
          "ASAN_OPTIONS=detect_leaks=0 /tmp/t_dma_region_segments")),

        ("gpio_savestate_mirror", True, lambda: None,
         "gcc -std=gnu99 -O1 -g -w -ffunction-sections -fdata-sections "
         "-DINLINE=inline -Ilibretro/libretro-common/include "
         "-Ifrontend-common -o /tmp/t_gpio_savestate_mirror "
         "tools/tests/test_gpio_savestate_mirror.c savestate.c "
         "-Wl,--gc-sections && "
         "/tmp/t_gpio_savestate_mirror"),

        ("rtc_protocol", True, lambda: None,
         "gcc -std=gnu99 -O1 -g -w -ffunction-sections -fdata-sections "
         "-DINLINE=inline -Ilibretro/libretro-common/include "
         "-Ifrontend-common -o /tmp/t_rtc_protocol "
         "tools/tests/test_rtc_protocol.c -Wl,--gc-sections && "
         "/tmp/t_rtc_protocol"),

        ("cheat_codebreaker_bounds", True, lambda: None,
         "gcc -std=gnu99 -O1 -g -w -fsanitize=address,undefined "
         "-ffunction-sections -fdata-sections "
         "-DINLINE=inline -Ilibretro/libretro-common/include -Ifrontend-common "
         "-o /tmp/t_cheat_codebreaker_bounds "
         "tools/test_cheat_codebreaker_bounds.c cheats.c "
         "-Wl,--gc-sections && ASAN_OPTIONS=detect_leaks=0 "
         "/tmp/t_cheat_codebreaker_bounds"),

        ("savestate_bson_bounds", True, lambda: None,
         "gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror "
         "-Wno-old-style-declaration -fsanitize=address,undefined "
         "-ffunction-sections -fdata-sections -DINLINE=inline "
         "-Ilibretro/libretro-common/include -Ifrontend-common "
         "-o /tmp/t_savestate_bson_bounds "
         "tools/test_savestate_bson_bounds.c -Wl,--gc-sections && "
         "ASAN_OPTIONS=detect_leaks=0 /tmp/t_savestate_bson_bounds"),

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

        # Compile the real autopilot parser in both profiles: the rig raises
        # its file-step limit and enables the harness-only `state` command.
        # The runner also checks committed fixtures and predicate duration
        # bounds, which vary with when a RAM/SRAM predicate becomes true.
         ("autopilot_script", True, lambda: None,
         ("gcc -std=gnu99 -Wall -Wextra -Werror -Ifrontend-common -o /tmp/t_ap_release "
          "tools/test_autopilot_script.c && "
          "gcc -std=gnu99 -Wall -Wextra -Werror -DGPSP_PERF_RIG -Ifrontend-common "
          "-o /tmp/t_ap_rig tools/test_autopilot_script.c && "
          "python3 tools/run_autopilot_script_tests.py "
          "/tmp/t_ap_release /tmp/t_ap_rig")),

        # Exercise the production RFU packet handler with hostile and valid
        # peer identities.  Sanitizers are useful here because this is a
        # white-box protocol test with deliberately malformed packets.
        ("rfu_peer_binding", True, lambda: None,
         ("gcc -std=gnu99 -O1 -g -Wall -Wextra "
          "-Wno-old-style-declaration -Wno-missing-field-initializers "
          "-fsanitize=address,undefined -ffunction-sections -fdata-sections "
          "-DINLINE=inline -Ilibretro/libretro-common/include "
          "-Ifrontend-common -o /tmp/t_rfu_peer_binding "
          "tools/tests/test_rfu_peer_binding.c -Wl,--gc-sections && "
          "ASAN_OPTIONS=detect_leaks=0 /tmp/t_rfu_peer_binding")),

        # -ffunction-sections/-fdata-sections are REQUIRED: without them
        # --gc-sections has nothing to discard, so every GU call video_psp.c
        # makes stays undefined and the link fails.  The command documented in
        # docs/FF-ARTIFACT-FIX.md omitted them, which is why this suite was
        # only ever run by hand.  VID_TRIPLE is defined by the test itself.
        # The link-latency ratchet and its fix: the production rfu.c queues
        # driven by a scripted host and game, proving a stall is permanent
        # latency without shedding and bounded with it, and that shedding
        # never drops or reorders a packet with content.
        ("rfu_link_backlog", True, lambda: None,
         ("gcc -std=gnu99 -O1 -g -Wall -Wextra "
          "-Wno-old-style-declaration -Wno-missing-field-initializers "
          "-fsanitize=address,undefined -ffunction-sections -fdata-sections "
          "-DINLINE=inline -Ilibretro/libretro-common/include "
          "-Ifrontend-common -o /tmp/t_rfu_link_backlog "
          "tools/tests/test_rfu_link_backlog.c -Wl,--gc-sections && "
          "ASAN_OPTIONS=detect_leaks=0 /tmp/t_rfu_link_backlog")),

        # The rig's honesty invariants (docs/RIG-DOUBLE-BATTLE.md §2): the
        # autopilot's it=/crc=/val=/logbytes output, the in-band evt_gap
        # marker under injected writer starvation, and the ini lookup audit
        # that names a misspelled key.
        ("rig_telemetry", True, lambda: None,
         ("gcc -std=gnu99 -Wall -Wextra -Ifrontend-common "
          "-o /tmp/t_ap_engine tools/tests/test_ap_engine.c && /tmp/t_ap_engine && "
          "gcc -std=gnu99 -Wall -Wextra -Wno-unused-function -Ifrontend-common "
          "-o /tmp/t_evt_gap tools/tests/test_evt_gap_ini_audit.c && "
          "/tmp/t_evt_gap")),

        # The rig's PC side: the golden-save editor (synthetic-save round
        # trip, sector isolation, refusal of a corrupt input), hw_loop against
        # fake consoles, and the scorer against synthetic and deliberately
        # corrupted logs (G3).  One suite: the runner caps the suite count.
        ("rig_pc", False, lambda: None,
         "python3 tools/rig/test_make_rig_golden.py && "
         "python3 tools/rig/test_hw_loop.py && "
         "python3 tools/rig/test_summarize_battle.py"),

        # + the scale geometry and TRIANGLE presets (integer 2x included),
        # from the same production file (docs/DISPLAY-FEATURES.md).
        ("video_buffers", True, no_sdk,
         ("gcc -std=gnu99 -w -DGPSP_PLAYABLE -ffunction-sections -fdata-sections "
          "-I%s -Ifrontend-common -Ipsp tools/test_video_buffers.c "
          "-Wl,--gc-sections -o /tmp/t_vb && /tmp/t_vb && "
          "gcc -std=gnu99 -w -DGPSP_PLAYABLE -ffunction-sections -fdata-sections "
          "-I%s -Ifrontend-common -Ipsp tools/test_video_geometry.c "
          "-Wl,--gc-sections -o /tmp/t_vgeo && /tmp/t_vgeo && "
          "gcc -std=gnu99 -w -DGPSP_PLAYABLE -DUSE_PSP_RGB565_FORMAT "
          "-ffunction-sections -fdata-sections "
          "-I%s -Ifrontend-common -Ipsp tools/test_video_geometry.c "
          "-Wl,--gc-sections -o /tmp/t_vgeo565 && /tmp/t_vgeo565")
         % (sdk, sdk, sdk)),

        ("perf_loop", False, lambda: None,
         "python3 tools/test_perf_loop.py"),

        # The residency rig's summarizer refuses runs it cannot vouch for:
        # wrong arm, foreign EBOOT, un-echoed or unknown keys, applied values
        # that differ from the staged ones, and a diverged or missing oracle.
        ("resrig_summary", False, lambda: None,
         "python3 tools/rig/test_resrig_summary.py"),

        # Keep the standalone transport protocol/queue suite in the same
        # aggregate gate as the emulator-side RFU tests.
        ("netdrv", True,
         lambda: None if (env.wsl or have("make")) else "make not found",
         "make -C netdrv test"),

        ("sound_timer_step", True, lambda: None,
         ("gcc -std=gnu99 -Wall -Wextra -Wno-old-style-declaration "
          "-ffunction-sections -fdata-sections -DINLINE=inline -I. "
          "-Ilibretro/libretro-common/include "
          "-o /tmp/t_sound_timer_step "
          "tools/test_sound_timer_step.c -Wl,--gc-sections && "
          "/tmp/t_sound_timer_step")),

        # GB/GBC: gb_link protocol, GB netpacket routing, browser console
        # filter, TGB Dual smoke and serial hook, and the production fe_host
        # GB boot/save/state path (tools/run_gb_tests.py builds the vendored
        # core once for all of them).
        ("gb", True, lambda: None,
         "python3 tools/run_gb_tests.py"),

        # CONFIG.INI: the reader's bounds, then the control-remap table
        # (psp/ctl_map.c: defaults == 3.0 over every pad transition, the
        # conflict rule, validation and save/load) in both build flavours.
        # One suite because the runner caps the suite count.
        ("fe_ini_bounds", True, lambda: None,
         ("gcc -std=gnu99 -Wall -Wextra -Werror -ffunction-sections "
          "-fdata-sections -Ifrontend-common -o /tmp/t_fe_ini_bounds "
          "tools/test_fe_ini_bounds.c -Wl,--gc-sections && "
          "/tmp/t_fe_ini_bounds && "
          "gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror "
          "-fsanitize=address,undefined -Ifrontend-common -Ipsp "
          "-o /tmp/t_ctl_map tools/tests/test_ctl_map.c psp/ctl_map.c "
          "psp/config_psp.c frontend-common/fe_util.c && "
          "ASAN_OPTIONS=detect_leaks=0 /tmp/t_ctl_map && "
          "gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror -DGPSP_PLAYABLE "
          "-DGPSP_CATCH_SELFTEST -Ifrontend-common -Ipsp "
          "-o /tmp/t_ctl_map_rel tools/tests/test_ctl_map.c psp/ctl_map.c "
          "psp/config_psp.c frontend-common/fe_util.c && /tmp/t_ctl_map_rel && "
          # Save-state delete (docs/CONTROL-REMAP.md section 11): the one
          # function that names the files a delete removes.  Here because the
          # runner caps the suite count and this suite owns ctl_map, whose
          # Controls page sits beside it.
          "gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror "
          "-fsanitize=address,undefined -Ifrontend-common -Ipsp "
          "-o /tmp/t_state_delete tools/tests/test_state_delete.c && "
          "ASAN_OPTIONS=detect_leaks=0 /tmp/t_state_delete")),
    ]


# ---------------------------------------------------------------------- main
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=str(REPO / "host-test-results.json"))
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--strict", action="store_true",
                    help="return failure if any registered suite is skipped")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if args.only is not None and not args.only:
        ap.error("--only requires at least one suite name")

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

    all_suites = suites(env, baseline)
    if len(all_suites) > MAX_HOST_SUITES:
        print("host suite cap exceeded: %d registered; maximum is %d"
              % (len(all_suites), MAX_HOST_SUITES), file=sys.stderr)
        return 2
    suite_names = {name for name, _, _, _ in all_suites}
    unknown = sorted(set(args.only or ()) - suite_names)
    if unknown:
        ap.error("unknown test suite name(s): %s" % ", ".join(unknown))

    results = []
    for name, needs_cc, pre, cmd in all_suites:
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
    git_status = subprocess.run(["git", "status", "--porcelain=v1",
                                 "--untracked-files=all"], cwd=str(REPO),
                                capture_output=True, text=True).stdout.splitlines()
    manifest = {
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "repo": str(REPO),
        "commit": subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(REPO),
                                 capture_output=True, text=True).stdout.strip(),
        "workingTreeClean": not git_status,
        "workingTreeChanges": git_status,
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
    return 1 if counts[FAIL] or (args.strict and counts[SKIP]) else 0


if __name__ == "__main__":
    sys.exit(main())
