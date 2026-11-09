# Source-split tools: T3 pp_diff, T4 split + cutlist, T7 extract, T9 globals

Phase T of `builds/mainpsp-map/out/PLAN.md` (§0.1 G0, §1.2-§1.3, phase T, P1, P4, §5).
These tools only read sources, write into scratch directories, and run `psp-gcc -E` in the
`pspdev/pspdev` image. They never edit `psp/main_psp.c` and make no PSP build. The G1 build
recorded below was a separate scratch build.

All numbers are from 01067e74 (beta.4 base) unless marked. The scratch evidence is in
`builds/mainpsp-tools-out/scratch-source-split/`.

## Files

| File | What it is |
|---|---|
| `pp_diff.sh` | T3, gate G0 |
| `split.py` | T4: `cover`, `gen`, `split`, `inverse`, `replay`, `--selftest` |
| `cutlist.txt` | T4's output for this tree's single-file `main_psp.c` (its header names the source sha256). Since PLAN-P-beta5 P1 it follows the P branch: every step that edits main_psp.c regenerates it (`gen --carry` the previous one) in the same commit. **After P4 `psp/main_psp.c` is the split shell, and the cutlist describes the split tree's INVERSE** (`split.py inverse --split gpsp -o F`: the shell with every `fe/X.inc.c` expanded in place; the header's sha256 is the inverse's). Regenerate it with `split.py gen --split gpsp --carry gpsp/tools/refactor/cutlist.txt -o gpsp/tools/refactor/cutlist.txt`; `gen --src` on the shell (the default `--src`) is refused before `-o` is opened. Never hand-edit it |
| `fe_readme.py` | writes `psp/fe/README.md` (PLAN-P-beta5 §5.2: module descriptions, owners, the fragment → module map, the conventions of the split) from the cutlist's FRAG/F/G rows, the split tree and the PLAN §1.2 module table; `--check` exits 1 when the committed README is stale; `--selftest` (`tests/fe_readme_test.py`). P-review r2: the module descriptions are PLAN.md §1.2's Responsibility cells verbatim (`--plan PLAN.md` compares them, and the §1.3 exported names, §1.1 never-TU rows and §2 epochs it transcribes, cell for cell); every harness and mgift function is listed with its `#if` context read from the inverse (none = compiled by every build); the readers of each @extract region come from the host tests' `extract.extract` calls and the helpers that pass a name on (`marked_function`: `'fn:' + name`), resolved from literals with `ast`, and a read it cannot resolve is refused. P-review r3: every table cell escapes the pipe character (GFM splits a row at every unescaped one, code spans included) and the output is refused unless every table row has its header's GFM cell count (`--gfm FILE` checks any Markdown file); names are bare unless the cutlist defines them more than once (`name#N`); the copy sites in no T14 flavour come from `abi_asserts.py unbuilt` |
| `tests/fixtures/cutlist_01067e74.txt` | the 01067e74 cutlist (was `cutlist.txt` until P1), used by the self-tests that run on the 01067e74 tree |
| `modules.py` | PLAN §1.2/§1.3 as data: module ranges, the plan's name list, x1 groups, owner modules |
| `csrc.py` | shared C-source model: census.py ported, with the `__attribute__` fix |
| `globals.py` | T9: `census`, `rename`, `--selftest` |
| `../tests/extract.py` | T7: marker extraction library and CLI (`gpsp/tools/tests/extract.py`) |
| `extract_trace.py` | T7 support: records which byte ranges each host test slices today |
| `tests/source-split_*` | self-tests and mutants. `source-split_run_all.sh` runs all of them (WSL, about 3 min) |

## T3 `pp_diff.sh` (G0)

```
bash pp_diff.sh --ref TREE_A --new TREE_B [--overlay SPLIT_DIR] [--logs RUN_DIR] [--flavours ...] [--sde N] [--keep DIR]
bash pp_diff.sh --selftest [GPSP_DIR] [SPLIT_DIR]
```

How it works:
- **Compile line.** Each flavour's exact compile line for `main_psp.o` is taken from a flavours.sh build log (default `baseline/run1/<flavour>/logs/.build-eboot.log`). `-c -o` becomes `-E -P`. Both trees are preprocessed with that same line.
- **Overlay.** `--overlay` (split.py output) is applied inside the container, on a container-local copy of the tree. No host file is written, and every `#include` resolves as it would in a real build. Fragments look in `psp/fe/` first.
- **Verdict.** The verdict is on the token stream (`pp_norm.py`): whitespace outside string and character literals is collapsed to one space, and literals are kept byte for byte. The diff shown is line-based. (Until the 2026-11-05 review this was `tr -s`, which also collapsed blanks inside literals, so a doubled blank or a TAB in a string, a `.rodata` change, passed G0. Mutants M6/M7.) An empty preprocessed stream is an error, never "identical".
- **Build stamp.** `SOURCE_DATE_EPOCH` is pinned (default 1793908328, as flavours.sh pins it). Without the pin, Lab and harness always differ in the `fe_evt("build stamp", __DATE__, __TIME__)` at L:13926. The first unpinned run showed this, and mutant M4 now checks it.

**Proves:** the preprocessed `main_psp.c`, with every `#include` expanded, has the same tokens in the release, Lab and harness flavours. This also catches `__LINE__`/`__FILE__`/`__COUNTER__` changes, a different `#if` branch being taken, and an include that resolves to another file.

**Does not prove:**
- Anything about other translation units.
- Codegen or layout. G1 covers those.
- Debug info.
- Flag changes: both sides get the same compile line by design. For a tree with different make flags, point `--logs` at a build of that tree.
- A tree whose `psp/Makefile` would give a different compile line. The merge tree below has a conflicted Makefile, so its G0 used the base's compile lines.

## T4 `split.py` + `cutlist.txt`

```
python split.py cover [--src F] [-v]                         # crit_cover reproduced
python split.py gen [--src F | --split GPSP_DIR] [--carry OLD] [--assign KEY=MODULE ...] -o cutlist.txt
python split.py split [--src F] --cutlist cutlist.txt --out DIR   # DIR/psp/main_psp.c (shell) + DIR/psp/fe/*.inc.c
python split.py inverse --split DIR -o F
python split.py replay --split DIR --cutlist cutlist.txt --patch P --out DIR2
python split.py --selftest [--src F]
```

`--src` defaults to this tree's `psp/main_psp.c`. Before P4 that is the single file; after P4 it is the shell, which `gen`, `split` and `cover` refuse. On the split tree, regenerate in two steps:

```
python split.py inverse --split gpsp -o F                    # the single file this tree's cutlist describes
python split.py split --src F --cutlist gpsp/tools/refactor/cutlist.txt --out DIR
python split.py gen --split gpsp --carry gpsp/tools/refactor/cutlist.txt -o C   # gen also takes --split itself
python split.py replay --split gpsp --cutlist gpsp/tools/refactor/cutlist.txt --patch P --out DIR2
```

DIR and DIR2 are new directories (`split` refuses an `--out` that is its source's own tree); copy their `psp/main_psp.c` and `psp/fe/*.inc.c` over the tree.

### The cutlist

The cutlist has one row per function (`F`) and per global definition (`G`). Each row reads `name#occ`, then the owner module, then the fragment it sits in, then the anchor (its first line, exact, with the occurrence number). Each fragment also has a `FRAG` row whose anchor is its first line.

Cuts are found by anchor text, never by line number. A cut is legal only where all of these hold:
- `#if` depth is 0.
- It is not inside an item, a comment or a directive continuation.
- Glue such as comments, externs and `#define`s goes with the following item.
- An own-line `/* @extract:end NAME */` (T7) goes with the PRECEDING item: it closes text above it, so `gen` never starts a unit (or a fragment) with one, and `split` refuses a FRAG anchor on one (PLAN-P-beta5 P1; self-test C6/M16). A begin marker is glue like a comment: a fragment can start with one.

The generator also follows these rules:
- **Head.** Module info, thread attributes and the pads (L:1-160) stay in the shell when the gap allows it.
- **Relative includes.** A unit containing a `../` include stays in the shell, because from `psp/fe/` that include would resolve somewhere else.
- **Mixed `#if` blocks.** A top-level `#if` block that holds functions of two modules goes into the fragment of its majority module. The minority functions are reported as "pinned".

`gen` without `--carry` uses the §1.2 line ranges and the plan's name list (`modules.py`). That is valid only on 01067e74's line numbers. On any other tree, `gen --carry` keeps every name's module and refuses new names. The review files new names with `--assign`.

**The split tree (P-review 2026-11-08, B4).** The cutlist always describes the single-file `main_psp.c`. After P4 that file is `split.py inverse` of the shell and its fragments, and the cutlist's `source:` sha256 is the inverse's. `gen`, `split` and `cover` refuse a `--src` holding `#include "fe/X.inc.c"` lines, and `gen` refuses before it opens `-o` (until this fix, `gen` on the shell, which is the default `--src` on the P tree, wrote a 4-item, 1-FRAG cutlist over `-o` and only then refused). `gen --split GPSP_DIR` reads the inverse itself. `gen` writes `-o` through a temp file, renamed only after its own split check passes, so a refused gen never leaves a half cutlist. Self-test C7, M17 and M18. Only `gen` has `--split`: the refusal tells `split` and `cover` to write the inverse (`split.py inverse --split <gpsp dir> -o F`) and pass `--src F` (P-review r2: it used to name a `split --split` that does not exist). Self-test M19 runs every command a refusal names.

### What `split` refuses

`split` writes nothing and exits 1 in any of these cases:
- an item is unlisted, listed twice, or listed but absent;
- an anchor is not found, is out of order, or is not on a cuttable line;
- a fragment's `#if`/`#endif` do not balance;
- an item is not in the fragment its row names;
- an item has a stale anchor;
- a fragment contains a `../` include;
- the inverse is not byte-identical to the source;
- `--out` is the source tree.

### Proves and does not prove

**Proves:**
- The split is deterministic and exactly invertible.
- Every function and global is assigned to exactly one module and one fragment.
- Every cut sits outside an open `#if`.
- Together with T3 and a build, the split is G0- and G1-neutral.

**Does not prove:**
- That the module assignments are right. That is the one-time cutlist review (PLAN §1.2).
- Anything for a tree the cutlist was not generated or carried for. `replay` and `--carry` refuse instead of guessing.

## T7 `gpsp/tools/tests/extract.py`

```
python gpsp/tools/tests/extract.py NAME [--src FILE ...]
python gpsp/tools/tests/extract.py --list | --check | --enumerate [GPSP]
python extract_trace.py GPSP OUT.json        # WSL: runs every reader unchanged, records its slices
```

A region is marked `/* @extract:begin NAME */ … /* @extract:end NAME */`.

- A marker alone on its line adds nothing to the region.
- An inline marker cuts at the exact character.
- Other markers inside a region are removed.
- Every name needs exactly one begin and one end; otherwise extraction raises an error.

`plant()` inserts markers for given spans, and `strip_markers()` removes them.

**What the old tests take today.** `extract_trace.py` runs each reader unchanged, with `main_psp.c` handed out as a tracking `str`. It records every slice of the file, and slices of slices, plus the group-0 span of every `re.search/match/fullmatch/finditer/findall` run on it. The result is the ground truth: `trace-01067e74.json`, 138 regions from 12 tests.

Limits of the trace: compiled-pattern methods, `split`/`partition` pieces, and strings derived with `replace` or `+` are not recorded. Those are mutant builders, not extractions.

**Proves:** extraction by markers returns the same text as the old slicing, region for region.

**Does not prove:**
- That every region a future test needs is marked. P1 enumerates them by grep at that time.
- Anything about the C hand-mirrors `test_sram_exit_retry.c` and `test_ctl_map.c`. They read no text; they belong to the XS allow-list review (abi-exit-ci.md, T8: "C hand mirrors").

**P1 (PLAN-P-beta5 §4 P1, on the beta.5 base).** The 13 readers `--enumerate` finds read `psp/main_psp.c` through `read_source()` and take their regions through 65 marker pairs, each on its own lines around whole lines (no code line is edited by a marker):
- `fn:NAME`: a function's definition through its closing brace's line; the tests' brace matcher (`test_psp_runtime.function`, via `marked_function`) still runs inside the region and returns the same text.
- `def:`/`decl:`: the `#define` or declaration lines a test greps.
- `ff:`, `lowlat:`, `menu:`, `qs:`, `sl:`, `sb:`, `vol:`, `spsc:`: the range a test slices (its old start/end anchors are still checked inside the region).

Whole-file facts stay whole-file, over `read_source()`'s text: the ring-index classification of test_spsc_mg_wiring, the `sceAudioOutputBlocking` count and the apply's presence (test_volume), the single `loop_park()` (test_sleep_menu) and the single `"menu"` load (test_state_load_ask). `enumerate_readers` counts `read_source(` as a read, so a converted reader is still found, and it walks all of `gpsp/` recursively (paths relative to it), so a reader added in any subdirectory is found; `tools/refactor/` (the gate tools, which read main_psp.c by design) is left out unless `--gate` is given (12 on 01067e74, 13 on the P0c tree and on P1; 8 more with `--gate`). The cutlist P1 commits (`cutlist.txt`, carried from the P0b cutlist) keeps every row's module, fragment and occurrence; with the end-marker rule above every region stays inside one fragment (each `fe/*.inc.c` passes `extract.py --check` on its own), every fragment is the parent's text plus its own markers and `@lab` tags, and three FRAG rows re-anchor on the begin marker that now opens them (power.b `fn:power_cb`, pacing.d `lowlat:late_present`, shell.b `fn:main`). Every marker is checkable from the tree alone: main_psp.c minus its 130 own-line marker lines and its 18 `/* @lab */ ` tags is the parent byte for byte. The step's record, the planting script and the old-vs-new evidence are kept with the other P evidence outside the repository (`builds/mainpsp-p-out/p1/` in the owner's workspace, not tracked).

## T9 `globals.py`

```
python globals.py census [--cutlist cutlist.txt] [--basis .../scratch/vars_g.json] [--json F] [--tsv F]
python globals.py rename OLD NEW -o OUT
python globals.py --selftest
```

**What the census does:**
- **Parsing.** It is census.py's parse with the fix: balanced `__attribute__((...))` removal before the declarator parse. The attribute text is kept, including its `section(".sbss")`.
- **Lifetime classes.** They come from gen.py's rules, release flag set. The main() phases come from anchors (`session_top:` and the `while (g_running)` braces), not from hard-coded line numbers.
- **Owner module.** Each global gets an x1 group (md.py's regexes, extended for the 23 missed definitions) and then an owner per PLAN §1.2's Owns column (`modules.py`).
- **Fragment and writers.** It reads each global's fragment from the cutlist and lists cross-module writers.

**What `rename` refuses:**
- an occurrence inside `fe_evt`, `LAB_HOOK`, `LAB_FFP` or `FE_LP_SET` arguments (r6 B3);
- an exported or weak-hook name;
- a collision with an existing name;
- a name that a local variable or parameter shadows;
- a name that is not defined in the file.

Otherwise it renames code tokens only, and leaves comments and strings alone.

**Proves:** no definition is left without an owner, and the parser no longer drops attributed declarations.

**Does not prove:**
- That writes are detected exactly. Detection is census.py's heuristic. For example, `g_vc_work` comes out `const` because it is only written through a pointer.
- That the owners are right where the plan is silent (E):
  - G02 apart from `g_running`/`g_relaunch` → power;
  - `g_all_clean` → G13 merend;
  - `g_ffh_*`/`g_ffc_*` → G03 and `g_dfr*`/`g_lf_*` → G14, both pacing.
- Anything about the header-borne objects in main_psp.o (`rig_stats`, `LINK_GAMES`).

## Acceptance results against PLAN §5

| Tool | PLAN acceptance | Result | Reproduced? |
|---|---|---|---|
| T3 | identical on base vs base | Identical in all three flavours. Release 16,788 lines (471,796 token bytes), Lab 19,113, harness 19,083. 10 of 10 self-test cases pass: 2 controls, a split control, and the token, rig-only, `__LINE__`, unpinned-SDE, include and (review) two string-literal-whitespace mutants | yes |
| T3 | runs on 01067e74 and on merge(candidate, opt tip) | base vs split(base): identical ×3. merge vs split(merge): identical ×3 (release 16,551 lines, Lab 19,142, harness 19,112) | yes, with the caveat on the merge tree's flags above |
| T4 | 0 unassigned / 0 doubly assigned of the 335 functions (today 5 and 69) | `cover`: 5 functions in no range and 69 in several (crit_cover, `main` excluded). After the plan's assignments: **0 / 0**. 51 by name, 260 by a single range, 24 by the plan's link/misc rule; the plan's "27 link/misc" count matches (27). Cutlist: 926 rows (335 F + 591 G; 924 / 589 before the review's parser fix added `g_fxs_gba`/`g_fxs_gb`; fragments unchanged, the split output is byte-identical), 43 fragments (41 `.inc.c` + 2 shell parts; shell 4,700 lines), 2 functions pinned by `#if` (`online_build_id` stays in the shell with its `../` includes; `psp_rewind_tick` sits in the rewind fragment). Gen is deterministic | yes |
| T4 | G0/G1 on 01067e74 | G0: identical ×3. **G1**: a scratch build of 01067e74 with the split, made by flavours.sh under the lock: 24 SHF_ALLOC sections identical in release, Lab and harness; the EBOOT is byte-identical in all three. Only DWARF differs (file names). Evidence in `scratch-source-split/g1/` | yes |
| T4 | G0/G1 on every merge(candidate, opt tip) | merge-tree `416ed75c` (claude/460-beta4 c7a74a88 + claude/opt-1000 2603f4e0), written with `git merge-tree`; no ref or worktree was touched. main_psp.c auto-merges; the Makefile, LAYOUT-PINNING.md and flags.lock conflict. The base cutlist is correctly **refused**: 2 new functions (`mgift_game_ok`, a second `vid_prof_frame`) and 2 absent globals (`g_shot_work`, `g_shot_work_bytes`). With `--carry` plus 2 `--assign` (mgift, harness): split OK, inverse byte-identical, G0 identical ×3. **G1 on the merge tree was not run**: the tree does not build until its Makefile conflict is resolved | G0 yes, G1 not done |
| T4 | refuses an unbalanced `#if`, unlisted or doubly listed items; `--inverse`; `--replay` | 22 of 22 self-test cases pass (7 controls, 15 mutants). Mutants: row deleted, row duplicated, anchor moved inside an `#if`, wrong fragment, new function, stray `#if 1`, renamed function, dead anchor, `--out` = source, replay adding a function, carry with a new function; (review) misspelled row module, misspelled FRAG module, FRAG module/name disagreement, a function moved bodily into another fragment. Replay of a body edit changes exactly 1 fragment | yes |
| T7 | same text as the old `str.index` extraction | 138 traced regions (119 distinct spans) from the 12 readers, all planted at once: every extraction byte-identical; strip restores the source; 5 grammar mutants caught, plus (review) plant inside a comment / a string refused and a region across a fragment cut read in source order by `read_source()` (13 of 13 self-test cases) | yes |
| T7 | readers enumerated by grep (12 at x1, 13 on opt-1000) | 12 on 01067e74; 13 on the merge tree (adds `test_spsc_mg_wiring.py`, which reads through a path variable; the enumerator handles that) | yes |
| T9 | 0 unassigned; the 23 found | **0 unassigned** of 591 definitions, **23 attributed definitions found**. Against x1's `vars_g.json`: 0 of 620 entries lost; the new definitions are the 23 plus 2 that x1 lost to a second parser bug (`g_fxs_gba`/`g_fxs_gb`, L:6773, anonymous `struct {...}` type with an initializer; FX_SWEEP+rig only, in no built ELF). All 566 x1 definitions get x1's lifetime class and group (0 differences). Release definitions 481 = x1's 458 + 23. 15 of 15 self-test cases pass, including 7 rename refusals | yes, with the corrections below |

### Corrections and findings for the integrator

1. **The 23 attributed definitions are at different lines than PLAN says.** PLAN §1.3 and x1 say "the 21 `.sbss`/`.sdata` pins 3148-3171". In fact 19 sit at 3148-3171 and 2 sit at 13740-13741 (`g_dfr_clk_n`, `g_dfr_slow`), which is what PLAN §1.2 row 0 lists separately. The total is 21 pins + `g_vc_work` + `g_all_clean` = 23. census.py also dropped **8 attributed `extern` declarations** (`__attribute__((weak))`, e.g. `badjump_recoveries` 7302/10767, `jit_code_mode` 1432). Only definitions count toward the 23.
2. **The main loop ends at L:18067, not 18070.** By its braces, the loop is 16368-18067, which agrees with §1.2's loop range. x1/gen.py used 18070.
3. **Three host tests fail on 01067e74 as it stands.** This is under WSL (gcc 15.2, Python 3.14), unrelated to markers:
   - `run_ff_tests.py`: its stub does not define `LAB_FFP`/`lab_ffpace_me_retire`, so the extracted code fails to compile.
   - `test_menu_press.py`: "the present does not drop the answer's edges after wake_service".
   - `test_sleep_menu.py`: "in-game present no longer runs wake_service".

   So "HT green" is not true on the base today. Either confirm these on the project's usual host or fix them before P1 relies on HT.
4. **P1 must switch each test to `extract()` in the same commit that adds its markers.** With every traced span planted as markers but the tests unchanged, 5 tests lose an exact-text anchor: menu_press, sleep_browser, sleep_menu, sleep_quiesce and state_load_ask. Markers belong around extraction regions only, not around the regex probes the tests also run (e.g. `^static int g_rw_pwr;$`). 121 of the 138 traced regions need inline markers; only 17 start and end on line boundaries.
5. **pp_diff needs the `SOURCE_DATE_EPOCH` pin.** Two unpinned runs differ in the Lab and harness stamp. The tool pins it by default.
6. **Known divergence on the merge tree.** The plan's line ranges give 11 unassigned functions on the merge tree. That is why a cutlist must be carried by name and never regenerated from §1.2 line numbers after 01067e74.

### Adversarial review 2026-11-05: false passes found and fixed

Each was reproduced on a copy before the fix and is now a self-test mutant.

| Tool | False pass | Fix |
|---|---|---|
| T3 | A doubled blank or a TAB inside a release-live string literal (`.rodata` change) was IDENTICAL: `tr -s` collapsed whitespace inside literals | token view is `pp_norm.py` (literals byte for byte); empty stream is an error. M6/M7 |
| T4 | A misspelled module in a row or a FRAG row (`powr`, `shel`) was accepted | modules checked against `modules.MODULE_ORDER`; a FRAG's name must start with its module. M12-M14 |
| T9 | `rename` left a use inside a `#define` body unrenamed (csrc blanks directives), so the flavour that expands the macro broke | refused. M8 |
| T9 | `rename` never checked "used by another TU" (the docstring said it did) | a non-static OLD is refused if another source in `--root` names it, or if no tree is given. M9/M9b |
| T9 / T4 | `static const struct {...} g_fxs_gba[] = {...};` (and `g_fxs_gb`) was not a definition: the aggregate regex took the initializer's last `}` | csrc matches the type's own closing brace. M10. Census 589 -> 591, cutlist +2 G rows |
| csrc | An unterminated quote (`#error can't`, a marker split a comment) ate its newline: later line numbers off by one, a following `#endif` lost | newline kept; asserted. M11 |
| T7 | `plant()` put a marker inside a block comment (its `*/` ends the comment: broken C) or a string literal | refused. M7/M8 |
| T7 | `--src shell fe/*.inc.c` concatenated fragments alphabetically, so a region crossing a cut (link.e -> mgift) silently took unrelated fragments' text | `read_source()` expands the shell's `#include "fe/X.inc.c"` in place; P1's tests should read through it. M9 |

Still open (documented, not fixed): a global defined through a macro (`PIN(g_x);`) is invisible to csrc, so neither the census nor the cutlist lists it. On 01067e74 an ELF cross-check (every LOCAL OBJECT under `FILE main_psp.c` in the release and harness ELFs) found no file-scope definition missing from the census; the extras are function-local statics, header statics (`LINK_GAMES`, `TEXT_FOLD_*`, `rig_stats`) and `CSWTCH`. Re-run that cross-check when a new macro is introduced at file scope.

### Commands as run

```
python gpsp/tools/refactor/split.py cover
python gpsp/tools/refactor/split.py gen --label "01067e74:gpsp/psp/main_psp.c" -o gpsp/tools/refactor/cutlist.txt   # now tests/fixtures/cutlist_01067e74.txt
python gpsp/tools/refactor/split.py split --cutlist gpsp/tools/refactor/cutlist.txt --out <scratch>/split-base
bash gpsp/tools/refactor/pp_diff.sh --ref gpsp --new gpsp --overlay <scratch>/split-base          # WSL
python gpsp/tools/refactor/globals.py census --basis builds/mainpsp-map/out/scratch/vars_g.json
bash gpsp/tools/refactor/tests/source-split_run_all.sh                                          # WSL, all self-tests
```

The G1 build script is `scratch-source-split/g1_build.sh` (flavours.sh `--src` on a throwaway git repo of 01067e74 plus the split; `GPSP_BUILD_VERSION=01067e7`; SDE 1793908328; run under `flock build.lock`).
