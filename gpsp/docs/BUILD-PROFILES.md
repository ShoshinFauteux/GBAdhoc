# BUILD PROFILES

Three builds, three names, one command each.

```
tools/build.sh release      # what a player installs
tools/build.sh harness      # the hardware performance rig
tools/build.sh diagnostic   # release plus investigation instruments
```

Options: `--no-clean` (only safe when no define changed — see below), `--out
DIR` to stage the artifacts, `--expect TOKEN` to require a string in the linked
ELF.

Everything below is derived from `tools/build.sh` and `gpsp_profile.h`, which
are the only two places a profile is defined.

---

## What each profile is for

| | release | harness | diagnostic |
|---|---|---|---|
| XMB title | `GBAdhoc` | `GBAdhoc HARNESS` | `GBAdhoc DIAG` |
| EVT log to the memory stick | no | yes | no |
| drivable by a CMD.TXT job file | no | yes | no |
| `ms0:/badjump.txt`, `ms0:/smchisto.txt` | no | no | yes |
| same emulation as release | — | yes | yes |

The dynarec flags are **shared by all three** on purpose: a diagnostic build
must be the same emulator as the release, or it answers a different question.

```
SMC_GATES=1 SMC_GATES_SIMPLE=1 SMC_GATES_RANKED=1 SMC_GATE_BITMAP=1
SMC_PARTIAL_SAFE=1 SMC_PARTIAL_STABLE_THUNK=1 SMC_PARTIAL_DIRECT_LINKS=1
GBA_PC_MASK=1 BADJUMP_SAFE=1
```

`ROM_BUFFER_SIZE=15` is not on that list because the top-level Makefile sets it
for every PSP build (a PSP-3000 can otherwise allocate all 32 one-megabyte
blocks, and Unbound then diverges during battle).

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
| release + `BADJUMP_REPORT` / `SMC_WRITE_HISTO` / `GPSP_ROMLOAD_DIAGNOSTICS` | writes to the player's memory stick |
| release + `GPSP_PERF_RIG` / `GPSP_KEEP_TELEMETRY` | that is the harness |
| release + `SMC_PARTIAL_SAFE_CONTROL` | an A/B control arm; reads as an unexplained performance regression |
| release + `SMC_SKIP_SAME` | white-screens Heart & Soul at boot |
| `SMC_PARTIAL_SAFE` + `SMC_PARTIAL` | SAFE silently wins |
| `SMC_GATES_CLUSTER` + `SMC_GATES_SIMPLE` | CLUSTER silently wins |
| `SMC_GATES_SNAPF` + `SMC_GATES_SNAP` | SNAPF silently wins |
| `SMC_GATES_CODED` + SNAP/SNAPF | two gate-address filters at once |
| any `SMC_GATE*` sub-rule without `SMC_GATES` | compiles to nothing, so the experiment never ran |
| any `SMC_PARTIAL_SAFE` sub-flag without its parent | same |
| `SMC_PARTIAL_DIRECT_*` without `SMC_PARTIAL_STABLE_THUNK` | a direct link into a body that has moved |
| `GPSP_PERF_RIG` without `GPSP_KEEP_TELEMETRY` | the rig reads the event log |

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
* **Strings in the linked binary.** Release forbids `badjump.txt` and
  `smchisto.txt`, and *requires* `.playable-no-harness` — the marker proving
  ADR-0067's automation neutralisation is compiled in.  Diagnostic requires all
  three.  Harness requires `.gpsp-harness.ini`.
* **No warnings from GBAdhoc-owned code.** Toolchain and SDK paths are
  excluded; an accepted exception goes in `tools/warning-allow` with its reason.

## The manifest

Every build writes `psp/build-manifest.json`: profile, title, expanded core
flags and frontend defines, source commit and tree, whether the working tree was
clean, the docker image digest, whether it was a clean rebuild, and the EBOOT's
md5 and sha256 plus the ME PRX's sha256.

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

Eight suites, one command, `PASS`/`FAIL`/`SKIP` with a reason for every skip,
and a JSON manifest.  A skip is not a pass and the runner says so.

It solves two environment problems particular to this repo: there is no native
gcc on the Windows host (suites that compile are re-run inside WSL), and a git
worktree's `.git` file holds a Windows path that git inside WSL cannot resolve
(so the `rom_load` baseline is extracted here, by the git that can see it, and
handed over as `GBADHOC_BASELINE_DIR`).  The PSP SDK headers that two suites
need are cached once from the pspdev image into `.sdk-include/` (~1 MB,
gitignored).
