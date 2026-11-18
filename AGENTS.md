# AGENTS.md: how to change GBAdhoc safely

This guide is for anyone changing the code: new contributors, and AI coding
agents. It collects the rules that keep a change from breaking speed,
correctness or the tests, and explains why each rule exists. Read it before
your first commit.

GBAdhoc is a Game Boy Advance, Game Boy and Game Boy Color emulator for the
**PSP only**. It is a fork of libretro's gpSP. The emulator source is in
`gpsp/`; paths below are relative to `gpsp/` unless they start with `/`.
The desktop (SDL) build and the dynarec twin exist only as test tools. The
libretro, 3DS and other ports left over from upstream get no maintenance.
Multiplayer is two consoles only (Link Cable and Wireless Adapter, 1 vs 1).

Contents:

1. Words used in this guide
2. Where things are
3. Build profiles
4. The frontend is one file split into fragments
5. Layout: pins, the lock and re-pinning
6. Frame parity: release and lab builds must run the same per-frame code
7. Generated files: never hand-edit
8. Tests: which to run
9. The flavours gate, and which FAILs are expected
10. Changing the dynarec: predict first
11. Comments and docs
12. Project rules
13. Named traps

---

## 1. Words used in this guide

| word | meaning |
|---|---|
| dynarec | the dynamic recompiler: it translates GBA (ARM) code into PSP (MIPS) code at run time (`cpu_threaded.c`, `mips/`) |
| SMC | self-modifying code: a game writing over code that was already translated. The SMC machinery decides what translated code to throw away (gates, retirement, `SMC_REUSE`, `SMC_IMM_PATCH`) |
| ME | the Media Engine, the PSP's second CPU. GBAdhoc runs the GBA renderer on it |
| RFU | the GBA Wireless Adapter and its protocol (`rfu.c`) |
| TU | translation unit: one C file as the compiler sees it, after `#include`s |
| fragment | one of the `psp/fe/*.inc.c` files that together with `psp/main_psp.c` form one TU (section 4) |
| profile / flavour | a named build (`tools/build.sh PROFILE`). The gate tools call the three they compare (release, lab, harness) "flavours" |
| harness | a test build that a job file on the memory stick can drive, and that writes an event log (`EVT` lines) |
| autopilot | the input-script engine a harness build runs (`frontend-common/fe_autopilot.c`) |
| the Lab | the project's hardware test service: it queues jobs on real PSP-1000 and PSP-3000 consoles. It lives outside this repo and runs `lab` builds only |
| twin | the **dynarec twin**: the PSP's own translator built for Linux and run under qemu, used as a correctness oracle (`tools/drprof`). Older docs also call the SDL desktop build a "desktop twin" |
| corpus | the corpus oracle: the twin run on a whole ROM library from power-on (`tools/drprof/dr_corpus.py`) |
| mutant | a deliberately broken copy of a mechanism (`*_MUTANT` defines, `tools/drprof/mutants/`), used to prove a test can catch that bug |
| pin | a fixed start address for a region of code or data in the linked image (section 5) |
| hot order | the list of per-frame functions linked first, in a fixed order (`psp/layout/hot.ord`, `hot_order.txt`) |
| instance A / B | the two emulated GBAs of a Link Cable session on one PSP. Instance B's core is a renamed copy (`gbab_` prefix) |
| `TEL(...)` | a statement that only feeds telemetry; it runs in every build (section 6) |
| `PLAY_*` | the values a player build ships with (`psp/fe/play_defaults.h`) |

Common prefixes in the frontend: `mer_`/`merend` the ME renderer's frontend
side, `lat_`/`ll_` the latency tiers, `ffh_` the fast-forward helpers,
`pcarg` the `PCARG_SIDE` load/store stubs, `immp` `SMC_IMM_PATCH`.

## 2. Where things are

| what | where |
|---|---|
| the GBA core and dynarec | `gpsp/*.c`, `gpsp/cpu_threaded.c`, `gpsp/mips/` |
| the core's interface to a frontend | `gpsp/core_fe.h` |
| the GB/GBC core (TGB Dual, vendored) | `gpsp/gbcore/` (our patches: `gbcore/tgbdual/LOCAL-PATCHES.md`) |
| networking (ad hoc, UDP, relay) | `gpsp/netdrv/` |
| the PSP frontend | `gpsp/psp/` (the main TU: `psp/main_psp.c` + `psp/fe/`) |
| shared frontend code | `gpsp/frontend-common/` |
| tools, tests, gates | `gpsp/tools/` |
| docs | `gpsp/docs/` (start at `ARCHITECTURE.md`); design summaries in `gpsp/docs/design/`; decisions in `gpsp/docs/DECISIONS.md` and `gpsp/docs/adr/` |

## 3. Build profiles

One command builds each profile, in docker (`pspdev/pspdev`):

```
gpsp/tools/build.sh release      # what players install
gpsp/tools/build.sh harness      # test rig, PSP-1000 memory layout
gpsp/tools/build.sh harness64    # test rig, 64 MiB layout (PSP-2000/3000/Go)
gpsp/tools/build.sh lab          # the harness plus the Lab's task runner
gpsp/tools/build.sh diagnostic   # release plus instruments that write to the memory stick
gpsp/tools/build.sh soak         # long unattended runs whose point is the failure
```

All six compile the same emulator core (`gpsp_features.h`); they differ in
what the frontend adds. `gpsp/docs/BUILD-PROFILES.md` has the full table and
the combinations the build refuses.

- Core flags (anything touching the dynarec) go to the **root** make;
  frontend defines go to `psp/Makefile`. `tools/build.sh` keeps both lists
  in one place. Do not run the makes by hand with your own flags.
- Never pass variant knobs to a release: `CORE_EXTRA`, `FE_EXTRA` and
  `PSP_EXTRA` are refused for it.
- Compare the **ELF** md5, never the EBOOT's: the EBOOT carries timestamps.
- Use your own build directory. Two builds must never share one: the root
  make writes objects beside the sources.

## 4. The frontend is one file split into fragments

`psp/main_psp.c` is one TU of about 18,800 lines. It was kept as one TU on
purpose: inlining and function placement across it matter for speed (see
`gpsp/docs/design/RESTRUCTURE.md`). To make it readable it is split into a
246-line shell and 44 fragments, `psp/fe/*.inc.c`, which the shell
`#include`s in order. `psp/fe/README.md` (generated) maps every function to
its fragment.

**Fragments are generated. Never hand-edit them.** Each fragment is a verbatim
slice described by `tools/refactor/cutlist.txt`. To change frontend code:

1. Rebuild the single file: `python3 tools/refactor/split.py inverse --split . -o /tmp/main_psp.c`
   (run in `gpsp/`; any output path outside the tree).
2. Make your change in a git branch of that single file, and take the diff
   as a patch whose `+++ b/` path ends in `main_psp.c`.
3. Replay it: `python3 tools/refactor/split.py replay --split . --cutlist tools/refactor/cutlist.txt --patch P --out NEWDIR`.
   Replay refuses a change that adds or renames an item the cutlist does not
   know, instead of filing it in the wrong fragment.
4. Copy `NEWDIR/psp/main_psp.c` and `NEWDIR/psp/fe/*.inc.c` over the tree.
5. Regenerate what depends on the fragments (section 7) and run
   `python3 tools/refactor/fe_readme.py --check`.

A new function or global needs a cutlist row (`split.py gen --carry`); the
`psp/fe/README.md` "Conventions" section has the details.

About 20 host tests read regions of the frontend by marker
(`/* @extract:begin NAME */`). `psp/fe/README.md` lists which test reads
which region; rerun those tests when you touch one.

## 5. Layout: pins, the lock and re-pinning

The PSP's instruction cache is 16 KiB, 2-way, with 64-byte lines, so where a
function sits modulo 8 KiB decides which other code it evicts. Moving the
per-frame path by a few hundred bytes has changed frame time by up to about
1 ms. So the layout is fixed on purpose:

- The per-frame core functions are linked first, in the hot order.
- Each later region starts at a **pin**, a fixed address held in
  `tools/layout/layout.lock`. Code before a pin can grow into the gap in
  front of it without moving anything after it.
- `tools/build.sh` checks every release and lab build against the lock. A
  release must match it exactly, or the build fails.

Pinning is built in; `LAYOUT_PIN` can only be 1. The full story is
`gpsp/docs/LAYOUT-PINNING.md`.

**What a binary-changing commit has to do** (`LAYOUT-PINNING.md` §1.3,
"Rehearsal and what a change now has to do"):

1. Bytes land inside a region's slack: nothing to do. The build's
   `layoutPins` line shows each gap.
2. A region grows past its pin: the strict link stops and names the pin. Run
   **one** command and say in the commit which you chose:
   `python3 tools/layout/layout_lock.py regen release --spend-page PIN`
   (accept 8 KiB of PSP-1000 heap) or `--move PIN=0xADDR` (a new placement,
   which needs a hardware A/B). `--free-page` reverses a spent page after a
   shrink.
3. An unpinned per-frame anchor the lock checks moved (`pace_frame`,
   `size frame_loop`, `me_replay_lines`, data anchors): either change the
   code, or accept the move with `layout_lock.py write FLAVOUR NM` (it never
   touches pins) and a hardware A/B of the move.
4. A new object file: put it in a layout group in `psp/Makefile`, or the link
   fails its "ungrouped" check.

Never edit pin values or lock values by hand. `tools/layout/repin_rehearsal.sh`
lets you try a change's layout effect before committing it. When per-frame
code changes size a lot, regenerate the hot order with
`tools/layout/regen.sh`.

The GB core is not pinned and moves when the frontend changes size, so an A/B
of a frontend change must include a GB fixture.

## 6. Frame parity: release and lab builds must run the same per-frame code

The Lab runs **lab** builds only, never the release. Its speed and picture
results are evidence about the release only where the code that runs every
frame is the same in both builds. So:

- A function that runs every frame must be the same machine code in release
  and lab, and where they shared an I-cache set, they must keep sharing it.
- Telemetry bookkeeping (`TEL(...)`) runs in every build for that reason;
  only the reporting (`fe_evt`) is compiled out of player builds.
- Never make a release-only or lab-only edit to per-frame code (`frame_loop`,
  `pace_frame`, the ME presenter, the per-frame core path).

Gate **FP** enforces this: `tools/layout/frame_parity.py check` compares the
release and lab ELFs of one commit over the per-frame set
(`tools/layout/frame_parity.spec`, `psp_frame_path.txt`) against the ratchet
`tools/layout/frame_parity.base`. `flavours.sh --against` runs it. The base is
re-recorded (`frame_parity.py record`) only with a stated reason.

## 7. Generated files: never hand-edit

Change the input, then rerun the generator:

| file | generated by |
|---|---|
| `psp/fe/*.inc.c`, `psp/main_psp.c` (shell) | `tools/refactor/split.py` from the single file and `tools/refactor/cutlist.txt` (section 4) |
| `tools/refactor/cutlist.txt` | `tools/refactor/split.py gen --carry` |
| `psp/fe/README.md` | `tools/refactor/fe_readme.py` (`--check` fails a stale one) |
| `tools/refactor/generated/harness_keys.tsv` | `tools/refactor/harness_keys.py gen` (`check` needs no docker) |
| `tools/refactor/generated/<flavour>/fe_abi_asserts.h`, `abi_check.c` | `tools/refactor/abi_asserts.py gen` |
| `tools/refactor/strong_syms.golden.tsv` | `tools/refactor/strong_syms.py golden` |
| `tools/layout/layout.lock` values / pins | `tools/layout/layout_lock.py write` / `regen` |
| `tools/layout/frame_parity.base` | `tools/layout/frame_parity.py record` |
| `psp/layout/hot_order.txt`, `psp/layout/hot.ord` | `tools/layout/regen.sh` |
| `tools/flags.lock` | `tools/check_flags.py --update` |
| `docs/ADR-INDEX.md` | `tools/gen_adr_index.py` (from `docs/DECISIONS.md`) |
| `psp/build-manifest.json` | `tools/build.sh` |

`tools/refactor/abi_asserts.ratchet` is hand-kept but may only go down.
`psp/me/gbadhoc_me.prx` and `psp/home/gbadhoc_home.prx` are tracked build
products that every build rebuilds in place; `tools/build.sh` restores them.
Do not commit a rebuilt PRX by accident.

## 8. Tests: which to run

| change | run |
|---|---|
| any change, docs included | `python tools/run_host_tests.py` (every suite must PASS; a SKIP prints its reason) |
| frontend code | the host suites, a `tools/build.sh` of release and lab, `fe_readme.py --check` |
| anything that changes the binary | `tools/refactor/flavours.sh --against` the parent (section 9) |
| the dynarec or core timing | the twin oracle and the corpus (section 10), then hardware |
| speed | hardware only. PPSSPP and the twin are not speed evidence |

`run_host_tests.py` needs Python; its C suites run inside WSL with gcc. Its
options: `--only NAME`, `--strict` (a skip fails), `--json PATH`.
`gpsp/docs/TESTING.md` explains every layer, and
`gpsp/docs/TEST-COVERAGE.md` says what each test does **not** prove.

Speed results: only unhashed per-fixture frame times and fps are speed
evidence; whole-run numbers of hashed or link runs are not. Treat about 3 %
per game as noise. A shortfall of about 1 fps in one rare case is a note, not
a regression.

## 9. The flavours gate, and which FAILs are expected

`tools/refactor/flavours.sh --commit REV --out DIR --against REF` builds the
release, lab and harness flavours of a commit in a clean export and compares
them with a reference build of its parent. It writes one
`== <flavour> <gate> PASS|FAIL` line per check and a generated gate line.

Most gates are **byte-identity proofs** made for refactors that must not
change the binary. On an ordinary code change they FAIL, and that FAIL is the
report of what changed:

- **Expected to FAIL on a normal code commit:** G0 and G0var (preprocessed
  frontend tokens), G1 (section bytes), G2 (per-function code; read it as the
  list of functions you changed), HO (hot-order sizes), ST (callback stack
  depth, if those paths changed), BF (if you touched build files), DS (if you
  touched anything outside `psp/main_psp.c` and `psp/fe/`).
- **Must PASS on every commit:** SS (weak hooks and exports stay strong),
  LL (layout lock `release:ok` and `lab:ok`), CF (`check_flags.py` clean),
  CI (`ci_greps.sh` source rules), FP (frame parity).
- **On a pure refactor**, every gate must PASS.

Check that each expected FAIL is explained by your change: a G2 list naming a
function you did not touch is a finding. `gpsp/docs/TESTING.md` §3 has the
table.

## 10. Changing the dynarec: predict first

Changes to the dynarec, the CPU core, memory access, timing, the ME pipeline
or layout-sensitive hot code follow one rule: **write down the mechanism and
the predicted effects before you write the code.**

- State what the change does, which games or fixtures it should affect, by
  how much, and which it must not change.
- Tests confirm a prediction; they never replace one. A change meant to be
  behaviour-neutral that changes any game's output means the model is wrong:
  stop and explain it. Do not tweak until the test passes.
- Pick the test set from the prediction: one fixture per risk class.
- The twin oracle must show a behaviour-neutral change as identical, frame by
  frame, audio included. `dr_corpus.py mutants` must report every mutant
  caught when you touch a mechanism that has mutants.
- Timing changes (block boundaries, where `update_gba` runs) legitimately
  change the twin's hashes; they need an argument and a hardware A/B.
- Generated code must be deterministic and must not depend on where things
  happen to be in memory.
- The SMC mechanisms interlock: gates, entry gates, selective retirement,
  `SMC_REUSE` and `SMC_IMM_PATCH` each keep invariants the others rely on.
  Read the invariant blocks in `cpu_threaded.c` before changing
  `smc_retire_span`, `smc_add_gate` or any flush path.
- Prefer general fixes. A per-game patch or table entry is a last resort, and
  needs a demonstrated reason.

Label each commit "simplifies" or "adds complexity", with the reason. A gain
of a few tenths of a millisecond does not justify added complexity.

## 11. Comments and docs

- **Short, present tense, true.** A comment says what the code does now and
  why. History (dates, branch names, old numbers) belongs in the commit
  message or a dated doc section, not in the comment.
- A status claim ("off by default", "opt-in", "make X=1") must match the
  build files. When a switch is built in, fix every comment that calls it
  optional.
- No references to files outside the repo, and no private paths, machine
  names, addresses or personal details. If a design matters, summarise it in
  `gpsp/docs/design/` and link that.
- User-facing text (the root README's player sections, release notes, the
  in-app text) is the maintainer's. Keep internal notes out of it, and never
  call 58 fps "full speed": a real GBA runs at 59.7.

## 12. Project rules

- **PSP only.** Don't spend effort on the leftover upstream ports.
- **Memory.** The PSP-1000 has the least memory, and its heap is the budget.
  Heap goes to speed or real features, not convenience.
- **Never distribute ROMs or BIOS images.** Tests use the tester's own files.
- **Test the player's path.** A fixture should go through the real menus and
  the player's settings, not harness-only shortcuts, where it can.
- **Don't engineer around user faults** (failing memory cards, pulled sticks).
- **Free-running speed.** Games run as fast as they can below full rate; don't
  lock frame rates to hide a slowdown.
- **For agents:**
  - Don't run interpreters that read their program from stdin (`python -`,
    heredocs into `python`): write the script to a file so the command can be
    read and rerun.
  - Never kill processes by name; stop only processes you started, by id.
  - Don't open desktop windows; run GUI tests off-screen.
  - Waits on long jobs must detect a stall, not only completion.
  - Commit only on your own branch; don't push, merge or tag unless asked.

## 13. Named traps

Older comments name these traps. Each has bitten the project at least once.

| name | the trap |
|---|---|
| core-vs-frontend-defines | A core define passed to `psp/Makefile` never reaches the core archive, and a core-only define tested in frontend code is always undefined there. The build succeeds and the experiment silently tests nothing. `ci_greps` rule R8 keeps `core_fe.h` free of `#if`s for this reason. |
| psp-stub-order-trap | Naming a PSP library that psp-gcc's spec or the SDK's `build.mak` already links splits its import stubs. `psp-fixup-imports` then prints "could not fixup imports, stubs out of order" as a warning, make exits 0, and the EBOOT dies on hardware. `tools/build.sh` greps for it. |
| harness-ini-key-trap | A misspelled harness ini key is silently read as absent, and the default applies. Check keys against `tools/refactor/generated/harness_keys.tsv`. |
| heap-exhausted-by-design | The ROM page cache takes every free megabyte at load on a PSP-1000, so a frontend allocation made after a game loads fails there and succeeds on other models. Reserve memory at boot. |
| rtc-wallclock-psp | `time()` returns roughly the uptime on real PSP firmware (PPSSPP returns real time). Use the RTC wall clock hook. |
| ppsspp-heap-5mib-over-go | PPSSPP's 64 MiB layout gives about 5 MiB more heap than a real PSP Go, so memory decisions tested only in PPSSPP can fail on hardware. |
| kernel-icache-16k-noop | The firmware's I-cache invalidate does nothing useful for ranges of 16 KiB or more. Split the range or invalidate all; `JIT_CODE_DISCIPLINE` enforces this. |
| log-starvation-false-freeze | The event-log thread can starve under heavy emulation, so a log that stops growing is not proof of a freeze. Use an independent heartbeat. |
| smc-partial-invalidation | An SMC change once gave byte-identical video and corrupted audio. Correctness checks must include the sound. |
