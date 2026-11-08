# gpsp/tools/refactor: proof tools for the main_psp.c restructure

These are the phase-T tools of `builds/mainpsp-map/out/PLAN.md` (§0.1 gates, §5 tooling). Each one proves
one property of a candidate tree or ELF against an accepted one, so the restructure (phases P, HW-1..6) can
move code without moving behaviour or layout unnoticed. All of them are host Python or sh. psp-gcc in docker
(`pspdev/pspdev`) is needed only for `-E` views, a few compile/link mutants and builds. Nothing here is
linked into the emulator, and no tool edits emulator source.

The baseline every acceptance number was measured on is the clean 01067e74 (beta.4 base), built twice
per flavour by `flavours.sh`. It is G1-identical and byte-identical (`builds/mainpsp-tools-out/baseline/BASELINE.md`).

## Gate -> tool

| Gate | Proves | Tool | Doc |
|---|---|---|---|
| G0 | preprocessed token stream (literals kept byte for byte) identical in release/lab/harness | T3 `pp_diff.sh` (+ `pp_norm.py`) | docs/source-split.md |
| G1 | every SHF_ALLOC section (24 on 01067e74) by name, address, size and bytes; NOBITS by name/address/size; program headers; bytes outside sections in PT_LOAD; the `could not fixup imports` warning in any log | T2 `sect_cmp.py` (whole-dir determinism: `det_check.py`) | docs/sections-layout.md |
| G2 | per function: identical, identical modulo address, or changed; I-cache set of every hot.ord function | T1 `func_diff.py` | docs/elf-func.md |
| G2L | `main`'s loop window by address, inlined helpers included; main's whole body; main mod 8192 | T1b `loop_diff.py` | docs/elf-func.md |
| G3 | layout report: pads, `.data.pin` slack, key addresses mod 8192, the loop's set map, the input-section audit (fails on unplanned `.text.unlikely*`/`.hot*`/`.startup*`) | T10 `layout_report.py`; T5 `hotord_check.py` (hot order still true; opt-in `tools/layout/regen.sh --check`) | docs/sections-layout.md |
| LP | loop placement in the I-cache simulation | T16 `loop_lpsim/` | docs/stack-cache.md |
| ST | `power_cb`/`exit_cb` stack depth against the 4,000 B `cb_thread` stack, with library prologue frames; indirect sites must be reviewed | T12 `stack_depth.py` + `stack_resolve.txt` | docs/stack-cache.md |
| IC | per function: emitted, clone, or inlined into whom | T13 `inline_census.py` | docs/elf-func.md |
| SS | weak-override hooks and exported ABI still strong and global | T6 `strong_syms.py` + `strong_syms.golden.tsv`/`.spec` | docs/abi-exit-ci.md |
| XS | per exit site and flavour, the ordered call list (from `psp-gcc -E`) equals the golden matrix | T8 `exit_matrix.py` + `tests/test_exit_model.c` | docs/abi-exit-ci.md |
| (CI) | no raw exits outside fe_priv.h, no `pwr_hold` in hooks, no logic in `fe_evt()` args, ... | T11 `ci_greps.sh` | docs/abi-exit-ci.md |
| (ABI) | `fe_abi_asserts.h` static asserts of shared prototypes/constants | T14 `abi_asserts.py` | docs/abi-exit-ci.md |
| (split) | deterministic split of main_psp.c into fragments by `cutlist.txt`; inverse; replay | T4 `split.py`, T9 `globals.py`, T7 `../tests/extract.py` | docs/source-split.md |
| (doc) | `psp/fe/README.md` (module map, owners, conventions of the split) generated from the cutlist and the split tree; `--check` fails a stale one | `fe_readme.py` | docs/source-split.md |
| (build) | release + lab + harness with the right ROOT/psp make split; refuses a dirty baseline; fails on broken imports | T15 `flavours.sh` (+ `flavours_sidecar.sh` for `.su` and the map without changing the ELF) | docs/baseline.md (copy of mainpsp-tools-out/baseline/BASELINE.md) |

Shared libraries: `elf32.py`, `ef_mips.py`, `ef_dwarf.py`, `ef_srcscan.py`, `sl_dwline.py`, `sl_ldmap.py`,
`csrc.py`, `aec_cparse.py`, `aec_pspcc.py`, `modules.py`.

## How to run

Everything, against the baseline (WSL; about 10-15 min; prints one table row per step):

```
cd builds/mainpsp-tools-worktree
bash gpsp/tools/refactor/run_all.sh                       # default baseline + a mktemp scratch dir
bash gpsp/tools/refactor/run_all.sh BASELINE_DIR SCRATCH  # explicit
ONLY=elf_func,stack bash gpsp/tools/refactor/run_all.sh   # a subset
```

The docker steps take `builds/mainpsp-tools-out/build.lock` (`LOCK=` overrides). The 304/144 pad pair
comes from `PAD144=` (default `mainpsp-tools-out/scratch-elf-func/pad144/release`). Per-group runners are
in `tests/` (`*_run_all.sh`, `elf-func_run_tests.sh`, `stack-cache_test_*.sh`, `test_flavours.sh`).

A candidate against the accepted tree (typical P-commit use):

```
bash flavours.sh --commit REV --out OUT --lock .../build.lock --against ACCEPTED_RUN
#   ACCEPTED_RUN must be REV's own commit, its first parent or a commit with REV's gpsp/ tree;
#   an older ancestor (P7: the beta tip a merge brings in, or P0c) only with --ref-ancestor <its commit>
python3 sect_cmp.py ACCEPTED/release OUT/release            # G1
python3 func_diff.py A.elf B.elf --hot ../../psp/layout/hot_order.txt   # G2
python3 loop_diff.py A.elf B.elf                            # G2L
python3 layout_report.py OUT/release --ref ACCEPTED/release # G3
python3 loop_lpsim/loop_lpsim.py setmap A.elf B.elf         # LP (static part)
python3 stack_depth.py B.elf --su OUT/harness/su --resolve stack_resolve.txt   # ST
python3 strong_syms.py checkrun --run OUT --golden strong_syms.golden.tsv     # SS
python3 exit_matrix.py check --golden GOLD --cand CAND      # XS
bash pp_diff.sh --ref TREE_A --new TREE_B --logs RUN        # G0 (--logs is required since P0a)
bash pp_sweep.sh --ref TREE_A --new TREE_B --logs RUN       # G0 on the variant arms (-E only)
bash ci_greps.sh --phase P                                  # CI rules
```

All of these are also run by `flavours.sh --against`, which writes every verdict to
`checks.txt` and GENERATES the gate line (`gate.txt`) from it. A P step's gate line is that
file, never typed: `G0 ok rel/stb/lab/har G0var ok G1 ok rel/stb/lab/har G2 ... ST ok
rel/stb/lab/har LL ok rel/lab CF ok BF ok DS ok CI ok`. A gate that is missing, or that lacks
a flavour it must cover, makes the line red (`flavours_gate.py`).

### Fix pass after the adversarial review of P0a (2026-11-07)

- **Hermetic builds.** `flavours.sh` runs `build.sh` under `env -i` (fixed PATH, HOME, the docker
  client variables) with only DOCKER_IMAGE, GPSP_BUILD_*, RFU_SCHED and `FLAV_<NAME>_ENV`. All of
  it is in `flavour.json` `buildEnv`. Caller variables that build.sh would have read are listed in
  `callerEnvDropped`. With ME_LOWLAT=0 exported, the build is now the default build.
- **The `--against` contract** (exit 2 before any check). Both sides must be hermetic and clean,
  with all four flavours. The image, version, SDE, RFU_SCHED, buildEnv and extraEnv must be equal.
  layoutLock must be release:ok and lab:ok. The reference must be this commit, its first parent,
  or a commit with the same gpsp/ tree.
- **New verdicts in `checks.txt`:** G0 (`pp_diff.sh` on both commits' exports, per flavour), G0var
  (`pp_sweep.sh`), LL, CF (`check_flags.py` rc 0 and the same output on both trees), BF and DS
  (`step_scope.sh`: the build files are byte-identical, and the diff stays inside
  `psp/main_psp.c` and `psp/fe/`; `--scope-allow GLOB` widens that and is printed in the verdict).
- **`pp_sweep`** (`pp_sweep.py`) generates its arms from build.sh's own switch list: every value
  alone and every pair, on all four flavours. It derives each arm's compile lines by running
  build.sh dry plus `make -n`, and the base lines must equal the run's logs. It sweeps every file
  the step changes, plus every TU that includes a changed header or fragment. A skipped arm,
  flavour or file exits 2; the only escape is `--allow-prune NAME`. A `--arms`/`--flavours`
  subset exits 3 (not a gate).
- **`--runs` discovery** (`runs.py`). `det_check.py --runs`, `sect_cmp.py --runs` and
  `strong_syms.py checkrun` find the run's flavours themselves, so release-stable is included.
- **`ci_greps`** reads a `*.inc.c` fragment as part of its includer, through a logical view that
  is the unsplit text line for line. Hits name the fragment line. KNOWN R4 rows are keyed by
  function, offending token and argument tokens. `--no-logical` is the old view, kept as a mutant.
- **T13 / T1b attribute DWARF by compile unit.** An inline instance or line row belongs to
  main_psp.o's own text if its file is main_psp.c or a `*.inc.c` fragment. loop_diff orders a
  fragment against `main` by its `#include` line in the shell.
- **A real P4 build.** `tests/p4_commit.sh` writes split.py's output as a commit object on the
  run's commit; it is deterministic and on no branch. `tests/p0a_b5.sh` (step `p4`) builds that
  commit with `flavours.sh --commit --against RUN` and requires the whole generated gate line,
  then T13/T1b/T10/T16 on the P4 ELFs. This replaces the simulated `.su` approach as the P4 proof.

### P0a (PLAN-P-beta5 §3): what changed for the beta.5 base

- `flavours.sh` builds four flavours (`release`, `release-stable` = RFU_SCHED=0, `lab`, `harness`).
  Pins: `DOCKER_IMAGE=pspdev/pspdev@sha256:...` (build.sh honours it too), `--build-version V`,
  `--sde N`; all recorded in `flavour.json` with the image id and `psp-gcc --version`.
  `--against REF` refuses (exit 2) a reference with another image id, version, SDE or RFU_SCHED,
  then runs every tool by its real interface (absent = FAIL) and writes `checks.txt` and the
  generated gate line `gate.txt`. `--check-only --out RUN --against REF` re-checks a build.
  A P step: `flavours.sh --commit STEP --out OUT --lock L --against PARENT_RUN --build-version H --sde N`.
- The loop window of T1b/T10/T16 is found by anchor (`loop_anchor.py`) in the source the ELF was
  built from (its `flavour.json` commit), checked against main's `DW_AT_decl_line`; an explicit
  range must be the anchored one. 01067e74: 16368-18089 (x4's 16368-18090, same code lines).
- T8 `exit_matrix.py`: x2's sites by anchor (reason string + order), `FE_EXIT_GAME`/`FE_HANDOFF`
  read as the calls they wrap, raw view through `#include "fe/X.inc.c"` with (file, line) keys.
- T9 `globals.py`: owners from the cutlist (modules.py must agree), define sets derived from
  psp/Makefile, macro-globals tagged, refuses a split shell and a cutlist of another source.
- `pp_diff.sh --extra-defs`, `pp_sweep.sh`; T12 `.su` statics by object stem; T13 MGIFT and
  release-stable/harness configs; T11 R3 roots `gba_reload_note`/`gba_reload_progress`;
  `check_flags.py` scans `psp/fe`; the sidecar joins `\`-continued compile lines.
- The repairs and their mutants on another tree: `tests/p0a_b5.sh --tree T --run RUN [--cutlist C]`
  (also `B5_TREES=... run_all.sh`). `tests/fixtures/cutlist_fbe9da63.txt` is a P4-simulation
  stand-in (cutlist_b5 carried, §2.1 applied), not the P0b cutlist.

## What is proven, and what is not

Each tool has a `--selftest` and/or tests in `tests/` that run on the real baseline ELFs. Each also has
mutants: seeded changes the tool must catch. An adversarial verification pass then found false PASSes in
**every** group (ef_mips content-only tokens and canonical-name collisions; sect_cmp's dotfile log scan,
skipped flavours and redirected `sh_offset`; hotord placement; pp_diff collapsing whitespace inside
literals; split/globals/extract parsing gaps; exit_matrix site-id collapse; ci_greps call-graph holes;
abi_asserts pin loss; stack_depth's unwalked callback and hidden `jr`). All of them are fixed and have
mutants now. The details are in each doc's "verify"/"adversarial" section.

**Proven on 01067e74:** G1 determinism over two clean builds (3 flavours). The PLAN acceptance numbers
(table below), with the stated corrections. The mutants in every self-test.

**Not proven / open (carry into P):**
- **T16/LP has no measured rates.** Under the once-per-frame placeholder, the loop group is 100 % misses
  (the 24,060 B window is larger than the 16 KiB I-cache), so the simulation cannot rank layouts. `report`
  now says `LP: NOT INFORMATIVE` (exit 3). LP today is the **static** set map (`setmap`, G3). A seeded
  +160 B shift is visible statically (5 sets) but not in the simulation. A PPSSPP or DRPROF trace is owed.
- **T12:** 2,928 B (lab/harness) rests on the reviewed `__sbprintf` single-re-entry argument. Without that
  prune the bound is 4,056 B, which is over the 4,000 B stack. Kernel callback dispatch stack is not modelled.
- **T1:** a lui change justified by one `%lo` user does not re-check its other `%lo` users (it matters only
  for shifts that are multiples of 64 KiB). WEAK pairings (35 sites) can accept an immediate equal to a
  symbol's shift (`--strict-weak` lists them). For data moves, func_diff is conservative: it reports
  "lui never justified" rather than proving equivalence (seen in the pad probe).
- **T11:** GCC-inferred noreturn/cold is invisible to R2 and is left to G3's input-section audit.
- **T9/T4:** a global defined through a file-scope macro is invisible to csrc (none on 01067e74, by an ELF
  cross-check that is not yet a `globals.py` option).
- **T10's** loop window is hard-coded to main_psp.c:16368-18090, and **T8's** site identity depends on x2's
  19 early sites. Both must be re-anchored when P moves those lines.
- **T5's** placement threshold (16 KiB gap) was set from a 3,508 B baseline maximum, so it may need retuning.
  `regen.sh --check-only` now fails (PLACE) on an order that was regenerated but not yet relinked.
- **T14** compiles only with the 7 reviewed normalisations in `abi_asserts.known`. `fe_abi_asserts.h` cannot
  be included from fe_priv.h as PLAN 1.4 says, because it needs core headers; it lives in a separate check TU.
- **G1 on the merge tree** (`merge(candidate, opt tip)`) was not run: the merge's psp/Makefile conflicts.
  G0 there used the base compile lines.

## PLAN numbers that did not reproduce (PLAN should be corrected)

| PLAN | Measured on clean 01067e74 | Note |
|---|---|---|
| T2: 25 SHF_ALLOC sections | **24** | `scratch/crit_secs.py` itself prints 24, also on pin144-rel |
| T5: 17 size / 13 rounding | **17 / 12** | x4's prose names 11; the 12th is `me_rend_retire_if_idle` 576 -> 616 |
| T11: green except "the known r6 B3 site" | **2 known R4 sites** | r6 B3, and main_psp.c:2725 `fe_host_sram_test_kill_handle()` inside `fe_evt` args |
| T12: `standby_note` 352 B frame | **408 B** | |
| T8: `audio_hash` guard at 18286 | the `if` is at **18285**, the guarded `fe_evt` at 18286 | |
| T13: 127 inlined | 127 without a symbol = **121 inlined + 6 dead** | |
| PLAN 1.4: fe_abi_asserts.h from fe_priv.h | not possible | see above |

Reproduced as PLAN states: T1 5,163 / 140 / 5,022 / 1 on the 304/144 pair, 0 of 151 hot functions changing
set. T1b 220 lines / 115 sets by line, 9,912 B inlined, main mod 8192 = 6912, and (new baseline) a
by-address window of 24,060 B, 376 lines / 128 sets. T4 5/69 -> 0/0 over 335 functions. T9 0 unassigned and
the 23 found (+2 anonymous aggregates). T6 green in every flavour. T8 x2's tables and the release-live rows.
T10 re-pins the pair (304 <-> 144). T13 x4's counts. T14 compiles, and a mutated prototype fails. T15 two
clean builds identical.

## Compensating-pad probe

See `docs/pad-probe.md`. One scratch build, with `layout_pad_cold` (8,192 B) ordered just before `main`.
LP: set map restored (main mod 6912, 0 sets changed). G1: **fail**, because `.data`/`.bss` moved +8 KiB and
22,592 data-address words before `main` changed. No code address before `main` moved. The same-size
reorder (variant 1 there) is the predicted pass and still needs its build.

## Last full run (2026-11-05, `run_all.sh`, clean WSL shell, logs in `builds/mainpsp-tools-out/run_all-final/`)

```
step       tools                                            gates     res   rc   ok/F   time
flavours   T15 flavours.sh --selftest                       G1-build  PASS   0   16/0     1s
det_check  T15 det_check.py runs + selftest x3              G1-det    PASS   0    0/0    12s
elf_func   T1 func_diff, T1b loop_diff, T13 inline_census   G2 G2L IC PASS   0   68/0    86s
sect_lay   T2 sect_cmp, T5 hotord_check, T10 layout_rep     G1 G3     PASS   0   35/0    77s
src_split  T3 pp_diff, T4 split, T7 extract, T9 globals     G0        PASS   0   61/0   201s
abi_exit   T6 strong_syms, T8 exit_matrix, T11, T14         SS XS     PASS   0   68/0   242s
stack      T12 stack_depth                                  ST        PASS   0   56/0    85s
lpsim      T16 loop_lpsim (static part)                     LP        PASS   0   35/0     1s
run_all: ALL PASS
```
