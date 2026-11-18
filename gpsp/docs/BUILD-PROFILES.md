# BUILD PROFILES

Six builds, six names, one command each. The list is `profile_flags()` in
`tools/build.sh`.

```
tools/build.sh release      # what a player installs; no environment needed
tools/build.sh harness      # the hardware test rig (PSP-1000 memory layout)
tools/build.sh harness64    # the same rig with the 64 MiB layout (PSP-2000/3000/Go)
tools/build.sh lab          # the harness plus the Lab task runner
tools/build.sh diagnostic   # release plus investigation instruments
tools/build.sh soak         # long-running harness plus bad-jump reporting
```

Options: `--no-clean` (only safe when no frontend define changed, see below),
`--out DIR` to stage the artifacts, `--expect TOKEN` to require a string in the
linked ELF. `GPSP_EBOOT_TITLE` changes only the XMB title, for a side-by-side
test install.

Profile defines and titles are set in `tools/build.sh` and checked by
`gpsp_profile.h`. A harness, lab or soak build is linked with
`PSP_LARGE_MEMORY=0` (the PSP-1000's 32 MiB layout), the memory budget the
project designs for; `psp/Makefile` defaults to 1 for everything else.

Words used below:
- **harness**: a build that a job file on the memory stick can drive
  unattended (`GPSP_PERF_RIG`) and that writes an event log
  (`GPSP_KEEP_TELEMETRY`). Its title says HARNESS so it is never mistaken for
  a player's build.
- **the Lab**: the project's hardware test service, which queues jobs on real
  consoles. It runs only `lab` builds (`psp/lab_harness.c`).

---

## What each profile is for

| | release | harness | harness64 | lab | diagnostic | soak |
|---|---|---|---|---|---|---|
| XMB title | `GBAdhoc` | `GBAdhoc HARNESS` | `GBAdhoc HARNESS64` | `GBAdhoc LAB` | `GBAdhoc DIAG` | `GBAdhoc SOAK` |
| for | players | rig runs on a PSP-1000 layout | rig runs that need 64 MiB (32 MiB carts held whole, ROM residency) | every Lab job | chasing one fault on a player-like build | long unattended runs whose point is the failure |
| `GPSP_PROFILE` (core) | release | harness | harness | harness | diagnostic | soak (checked as harness: see below) |
| event log to the memory stick | no | yes | yes | yes | no | yes |
| drivable by a job file | no | yes | yes | yes, plus the Lab queue in `ms0:/GBALAB` | no | yes |
| bad-jump report, IRQ integrity check, translation-depth probe | no | no | no | no | yes | yes |
| `PSP_LARGE_MEMORY` | 1 | 0 | 1 | 0 | 1 | 0 |
| `ME_CATCH` (ME exception screen) | yes | no | yes | no | no | no |
| layout lock enforced | yes (strict) | pins only | pins only | yes (`[lab]` section) | pins only | pins only |
| same emulation as release | - | yes | yes | yes | yes | yes |

The dynarec feature set is **shared by all six** on purpose: a diagnostic build
must be the same emulator as the release, or it answers a different question.
It is not a list in any recipe: `gpsp_features.h` is force-included into every
PSP core object (`docs/BUILD-SWITCHES.md` lists the switches and which are
built in). The profiles differ in what the frontend adds and in the
instruments above.

Layout: every profile is pinned (`docs/LAYOUT-PINNING.md`). The release and the
lab build must match their sections of `tools/layout/layout.lock`; the other
four have lock sections with pins only. The Lab measures lab builds, so the
per-frame code of release and lab must stay the same: gate FP
(`tools/layout/frame_parity.py`, run by `tools/refactor/flavours.sh --against`).

`SMC_WRITE_HISTO` (the self-modifying-code write histogram) is in no profile:
it writes `smchisto.txt` every 4000 flushes. Add it with
`CORE_EXTRA="SMC_WRITE_HISTO=1"` to a non-release build when you need it.

Variants of a non-release profile: `CORE_EXTRA="NAME=0"` (a core switch off),
`FE_EXTRA=-DNAME`, `PSP_EXTRA="VAR=VALUE"`. All three are refused for a
release.

`ROM_BUFFER_SIZE=15` is not in `gpsp_features.h` because the top-level
Makefile sets it for every PSP build (a PSP-3000 can otherwise allocate all 32
one-megabyte blocks, and Unbound then diverges during battle).

The soak profile passes `GPSP_PROFILE=soak` to the root make, which compiles
its core with `-DGPSP_PROFILE_HARNESS=1`, the same define its frontend gets, so
`gpsp_profile.h` checks both halves as a harness. The root make refuses any
`GPSP_PROFILE` other than `release`, `harness`, `soak` or `diagnostic` (unset
still means an unprofiled build), so a new profile name cannot silently
compile unprofiled.

---

## Why a single script

Before this, the three flavours were defined in four separate texts —
`./build.sh`, `tools/build_perf.py`, `tools/build_stability.sh` and
`releases/<name>/build.sh` — each carrying its own copy of a long flag list.
Drift between those texts is how the 7283f13 release candidate shipped
appending to `ms0:/badjump.txt` from inside the dynarec dispatch path:
`BADJUMP_SAFE` was wanted, its logging side was not, and nothing in the build
could tell the difference.

## Combinations the compiler now refuses

`gpsp_profile.h` is included by both halves of the build (the core through
`cpu_threaded.c`, the frontend through `psp/main_psp.c`).  Each `#error` there
is a combination that builds cleanly today and is **silently wrong** — one flag
shadows another, or a flag does nothing at all, or an experiment's control arm
is still on.  Combinations that are merely unusual are allowed; this is not a
taste filter.

| refused | because |
|---|---|
| more than one of `GPSP_PROFILE_RELEASE` / `_HARNESS` / `_DIAGNOSTIC` | one build, one profile |
| release + `GPSP_PERF_RIG` / `GPSP_KEEP_TELEMETRY` | that is the harness |
| release + `BADJUMP_REPORT` / `SMC_WRITE_HISTO` / `XLAT_DEPTH_PROBE` / `GPSP_ROMLOAD_DIAGNOSTICS` / `IRQ_INTEGRITY_CHECK` / `STALE_CHECK` | instruments that write to the player's memory stick |
| release + `STATE_HASH_TRACE` / `UPDATE_TRACE_LO` / `RTC_PIN` / `STALE_STACKNOTE` | tracing switches that change emulation or timing on purpose |
| release + `DRPROF_TWIN` / `DRPROF_HW` / `DRPROF_NEGCTL` | dynarec profiling instruments |
| release + `JIT_CODE_CHECK` / `JIT_CODE_AUDIT` / `JIT_SYNC_STATS` / `JIT_CODE_AB` | twin and debug instruments, and the hardware A/B selector |
| release + `GPSP_CATCH_SELFTEST` | a button combination crashes the PSP on purpose |
| `GPSP_PERF_RIG` without `GPSP_KEEP_TELEMETRY` | the rig reads the event log |
| `DISPATCH_CACHE` with `DISPATCH_CACHE_BITS` < 2 | the 2-way sets need at least two entries |
| `SMC_REUSE_DEBUG` outside the twin (`DRPROF_TWIN`) | it uses stdio and getenv |

The SMC sub-rule checks that used to stand here (a `SMC_GATE*` rule without
`SMC_GATES`, a `SMC_PARTIAL_SAFE` sub-flag without its parent,
`SMC_PARTIAL_DIRECT_LINKS` without `SMC_PARTIAL_STABLE_THUNK`) are gone: those
switches are built in (code-health A-6 and B-1), so the combinations cannot
occur.

The makes and `tools/build.sh` refuse more, before the compiler runs:

- **A built-in switch set to anything but 1** (`NAME=0`): the root Makefile's
  `GPSP_BUILT_IN` and `GPSP_BUILT_IN_B1` lists, and their fixed values
  (`SMC_REUSE_SPARE_KB=384` and others). `NAME=0` would otherwise build with
  the feature on and say nothing.
- **`LAYOUT_PIN` other than 1, and the old `LAYOUT_PAD_*_BYTES` pads**
  (both makes and `tools/build.sh`; `docs/LAYOUT-PINNING.md`).
- **`CORE_EXTRA`, `FE_EXTRA` or `PSP_EXTRA` with the release profile.**
- **The retired switches `CAND1`, `CAND2`, `COMBO3`, `MSR_FAST`** with any
  value but 1, and `HOT_DATA_GUARD=0`.

A **pixel-format** mismatch between the two halves is a link error:
`gba_memory.c` exports a symbol named after its layout and `psp/video_psp.c`
references the one it expects.  Each makefile defaults `PSP_PIXFMT` separately,
and `USE_PSP_RGB565_FORMAT` changes both `convert_palette()` in `common.h` and
the JIT's store emitter in `mips/mips_emit.h`, so a mismatch gives a core whose
pixels and generated code use one layout and a blitter that assumes the other.

## What the build verifies

* **A clean rebuild by default.** Neither makefile tracks define changes, so a
  build after a flag change with stale `.o` files silently drops the flag and
  the experiment reads as "no effect".  `--no-clean` is for iterating on code
  with the flags unchanged, nothing else.
* **Broken imports.** `psp-fixup-imports` fails as a *warning* and make still
  exits 0, leaving an EBOOT whose syscall imports are broken — it builds clean
  and dies on hardware.  Grepped for by text, because an exit status cannot
  catch it.
* **The artifact is newer than every source**, so a failed container cannot be
  papered over by the previous build's output.
* **Strings in the linked binary.** Release forbids `badjump.txt`,
  `smchisto.txt`, `irqchk.txt` and `home_stall_frame`, and *requires*
  `.playable-no-harness`, the marker proving that ADR-0067's automation
  neutralisation is compiled in (`docs/adr/ADR-0067.md`). Harness and
  harness64 require `.gpsp-harness.ini`; lab requires that plus the Lab task
  runner's strings; diagnostic requires the bad-jump and IRQ-check files and
  `.playable-no-harness` and forbids `smchisto.txt`; soak requires
  `.gpsp-harness.ini`, `badjump.txt` and `irqchk.txt` and forbids
  `smchisto.txt` and `.playable-no-harness`.
* **Loadable by a real PSP.** At most 4 program headers and 3 loadable
  segments; the firmware refused an EBOOT with more.
* **The layout.** The hot-core budget is reported, and the release and lab
  builds are checked against `tools/layout/layout.lock`
  (`docs/LAYOUT-PINNING.md`).
* **The memory budget.** The boot heap a PSP-1000 and a PSP Go would have with
  this image must stay above a floor (`docs/MEMORY-BUDGET.md`).
* **No warnings from GBAdhoc-owned code.** Toolchain and SDK paths are
  excluded; an accepted exception goes in `tools/warning-allow` with its reason.

## The manifest

Every build writes `psp/build-manifest.json`: profile, title, expanded core
flags and frontend defines, source commit and tree, whether the working tree was
clean, the docker image digest, whether it was a clean rebuild, the layout
results (`layoutPin`, `layoutPins`, `layoutLock`), the image size and boot
heaps, the EBOOT's md5 and sha256, the ELF md5, and the sha256 of each PRX
(ME, HOME, and online when built). `tools/make_release.sh` (through
`tools/make_release_check.py`) ships only a
manifest whose `layoutLock` is `release:ok`.

**Two things about comparing hashes.** The EBOOT's md5 changes between identical
builds -- the PBP carries timestamps -- which is why this project's rule is to
compare the **ELF** md5 and never the EBOOT's. And the ELF md5 changes with every
commit, because `GIT_VERSION` is `git rev-parse --short HEAD` and is compiled in
as a string. So an A/B that needs byte-identical output must hold `GIT_VERSION`
fixed; within one commit the ELF is reproducible.

A build from a dirty working tree says so, in the manifest and on stdout, because
it is not reproducible from a commit.

## Host tests

```
python tools/run_host_tests.py
```

Every host suite (54 at the time of writing; the registry is in the script),
one command, `PASS`/`FAIL`/`SKIP` with a reason for every skip, and a JSON
manifest (`--json PATH`; `--only NAME` runs one suite). Use `--strict` in a release gate to make any skip
fail the command; a skip is never counted as a pass.

It solves two environment problems particular to this repo: there is no native
gcc on the Windows host (suites that compile are re-run inside WSL), and a git
worktree's `.git` file holds a Windows path that git inside WSL cannot resolve
(so the `rom_load` baseline is extracted here, by the git that can see it, and
handed over as `GBADHOC_BASELINE_DIR`).  The PSP SDK headers that two suites
need are cached once from the pspdev image into `.sdk-include/` (~1 MB,
gitignored).
