# abi-exit-ci: T6, T8, T11 and T14 (phase T proof tools)

PLAN: `gpsp/docs/design/RESTRUCTURE.md` §0.1 (gates SS, XS), §3.2-§3.6, phase T, §5.
Base: 01067e74, the clean baseline ELFs (`flavours.sh --baseline` output, `baseline/run1/`; a local folder,
not in the repo; see [baseline.md](baseline.md)). Nothing here edits emulator source or the build. Every run below
was made on 2026-11-05 against that baseline.

| Tool | Gate | Files |
|---|---|---|
| T6 `strong_syms.py` | SS | `strong_syms.spec`, `strong_syms.golden.tsv` |
| T8 `exit_matrix.py` + `tests/test_exit_model.c` | XS | `tests/abi-exit-ci_teardown_ref.inc.c` (reference model) |
| T11 `ci_greps.sh` (rules are in `ci_greps.py`) | source rules | `ci_greps.known`, `ci_greps.fe_evt_calls` |
| T14 `abi_asserts.py` | ABI | `abi_asserts.known`, `generated/<flavour>/fe_abi_asserts.h` |
| shared | | `aec_cparse.py` (C statement parser), `aec_pspcc.py` (psp-gcc -E/-dM/-fsyntax-only in docker, using the flavour's own logged compile line) |

Tests (all under `tests/`, prefix `abi-exit-ci_`):

```
# WSL, from gpsp/tools/refactor; O=<local output dir holding the baseline, e.g. mainpsp-tools-out beside the checkout>
MAINPSP_LOCK=$O/build.lock bash tests/abi-exit-ci_run_all.sh $O/baseline/run1 $O/scratch-abi-exit-ci/final
#   or one at a time:
bash tests/abi-exit-ci_test_strong.sh   $O/baseline/run1 OUT    # T6
bash tests/abi-exit-ci_test_exit.sh     $O/baseline/run1 OUT    # T8
bash tests/abi-exit-ci_test_ci_greps.sh OUT [PPDIR]             # T11 (PPDIR adds R1e)
bash tests/abi-exit-ci_test_abi.sh      $O/baseline/run1 OUT    # T14
```

Docker is used only for `psp-gcc -E`, `-dM`, `-fsyntax-only` and one two-file link (T6 L1-L3).
None of it is a PSP build. The tree is mounted read-only, and source mutants are copies that
are bind-mounted over a single file. The run-all script takes the shared `build.lock`.

---

## T6 `strong_syms.py` (SS)

```
strong_syms.py checkrun --run RUNDIR --golden strong_syms.golden.tsv     # 3 flavours
strong_syms.py check --elf X.elf --map X.map --flavour F --golden ...    # one ELF
strong_syms.py golden --run RUNDIR --src GPSP --out G.tsv                # regenerate
```

**What it proves.** For each hook and exported symbol, in each flavour, it checks four things:
- the ELF symtab has exactly one non-local definition, and its binding is GLOBAL, not WEAK;
- there is no LOCAL symbol with the same name (a `static` shadow);
- in the link map, the input section that holds the symbol's address comes from `main_psp.o`, so the override won and not the weak stub;
- for a hook, the core's weak stub (`.text.<name>` from `rfu.o` or `main.o`) is still linked. This means the core still calls that name. A rename on the core side would leave the override orphaned.

The spec lists:
- the 6 hooks of x1 H6, cited at rfu.c:96/113/351/403/440 and main.c:935 (this includes `gpsp_rfu_now_us` 7180 and `gpsp_rfu_flight_dump_hook` 7185);
- the 8 globals of x1 H5;
- `gpsp_net_wait_beat`, in the lab and harness flavours only. A release must not have it, and a release that does is reported as "PRESENT: review".

The golden file adds a **derived** list: every global that the map shows `main_psp.o` defining, where either another *linked* TU declares it `extern` or a tree header declares it.

**What it does not prove.**
- It does not check that the hook is *called*. It only checks that the stub it overrides is still in the link.
- It does not check that the hook *behaves* the same.
- It does not check ABI types. That is T14's job.
- The derived list only sees `extern` declarations and header prototypes. A TU that calls a function without declaring it would be missed, but the build's `-Wall` catches implicit declarations.
- Host test sources (`tools/*.c`) are not link ABI and are not listed.

**Acceptance: "green on base, in every flavour". Reproduced.**

| Flavour | Names checked | Result |
|---|---|---|
| release | 30 (6 hooks + 8 H5 + 16 derived), `gpsp_net_wait_beat` absent by design | green |
| lab | 31 (+ `gpsp_net_wait_beat`) | green |
| harness | 31 | green |

Mutants, all caught:
- M1: a hook's binding set to WEAK.
- M2: `g_running` renamed away.
- M3: `in_rate` made LOCAL.
- M4: the map owner of `.text.gpsp_rfu_now_us` changed to `rfu.o`.
- M5: `rfu.o`'s weak `.text.gpsp_rfu_trace_hook` removed.
- M6: the derived `pwr_hold` made LOCAL in lab.

Real-toolchain cases, built with psp-gcc in docker from a two-file link:
- L1: a strong override passes.
- L2: a `static` override is caught (WEAK winner, LOCAL shadow, map owner `core.o`).
- L3: a renamed override is caught (the weak stub links silently).

A golden regenerated from the baseline is identical to the committed one.

---

## T8 `exit_matrix.py` + `test_exit_model.c` (XS)

```
exit_matrix.py all --baseline RUNDIR --src GPSP --out OUT [--pp-dir D] [--main-c MUTANT.c]
exit_matrix.py check --golden OUT_A --cand OUT_B [--allow allow.tsv]       # the XS gate
gcc -std=gnu99 -DREWIND -DGBA_LINK <flavour -D> -DEXPECT_H='"OUT/<fl>/exit_expect.h"' \
    -DTEARDOWN_INC='"<implementation>"' tests/test_exit_model.c
```

`all` preprocesses `psp/main_psp.c` for each flavour with that flavour's logged compile line, as `-E` (with linemarkers) and `-dM`. It parses the function bodies with `aec_cparse` and writes the following for `raw/` and for each flavour:
- `exit_sites.tsv`: one row per exit site. Each row has the terminal line, code/why, the ordered vocabulary (IO HOST RW AUD VID EVT ES PRA HO RL EXIT NOTICE LOADFIN), the §3.3 `ORDER_*` selector and flags, and every call with its line.
- `exit_steps.tsv`: the shared loop teardown. It is located by anchors: every statement after `main`'s frame loop, split at the `if` that holds the `goto` (session_end, inproc_try_return, process_end). There is one row per call or guard, carrying its PLAN §3.2 step id (D0-D20, I1-I5, U1-U13, P1-P13), its guarding conditions and its runtime tags (g_relaunch, rig, force, refusal, why, reason, census, retain).
- `exit_scenarios.tsv`: the teardown **interpreted** under 8 runtime scenarios: XMB, XMB_REASON, PROFILE, LIST_OK, LIST_REFUSED, LIST_RETAIN_THEN_REFUSE, LIST_CENSUS and LIST_FORCE. Conditions are evaluated with C short-circuit semantics. A condition atom with no scenario value **fails the run**, so new runtime logic has to be classified before the gate can pass.
- `exit_expect.h`: the same expectations as C arrays, for `test_exit_model.c`.
- `raw_vs_e.tsv`: every call that differs between the raw-source parse and the -E parse, classified `#if` (the raw token's `#if` stack, line absent in -E) or `macro` (the name or its enclosing invocation is in `-dM`). Anything else is `UNEXPLAINED` and fails.

`test_exit_model.c` compiles an implementation on the host, using recording stubs and the frontend's macro layer for the flavour (fe_evt, LAB_HANDOFF, stall/drprof/core_health no-ops, the exit/handoff hijack). It then:
- plays every A-C site's flags through `model_exit_early(flags)`;
- plays every scenario through `model_session_end`, `model_inproc_try_return` and `model_process_end`;
- requires the recorded order to equal the -E expectations.

In phase T the implementation is `tests/abi-exit-ci_teardown_ref.inc.c`, which is PLAN §3.2/§3.3 written as code. After C1 it is `psp/teardown.inc.c` (TEARDOWN_INC), with an adapter if its ctx signatures differ.

**What it proves.**
- For each flavour, the ordered teardown call list of every exit site and of the loop teardown, after the preprocessor, which a raw-source awk cannot give.
- That the §3.3 flag set can express all 16/20/20 A-C sites, and that the §3.2 split reproduces the teardown in all 8 scenarios. The reference model passes 24/24 (release) and 27/27 (lab, harness).
- That PLAN §3.2's step order holds in the source of every flavour (step ids never decrease).
- That the release-live rows exist and are not behind `g_perf_rig`.
- That the raw-vs-E differences are exactly preprocessor effects.

**What it does not prove.**
- Calls through function pointers, and work done inside called functions. The model records only the names in its tracked set; helpers such as `free` and `snprintf`, and the `session_reset` assignments, are untracked.
- Loops are recorded once and never evaluated. The only loops are in the rig gate block and in macros.
- `g_perf_rig` is 0 in every scenario, so the rig paths D0 and I4's `sleep_sim_fire` are listed in `exit_steps.tsv` but not played.
- Thread timing: the F1/F2 races are not modelled.
- Out-of-band endings (§3.4) are listed as `E:stall_exit` sites only.
- The A-C site vocabulary collapses consecutive duplicates. The A3/A4 and C2 if/else EVT and notice alternatives are therefore one token.

**Acceptance vs PLAN.**

1. **"Reproduces x2's tables": reproduced, 19/19 A-C orders.** This holds for the raw source and for the harness and lab -E. In release, A1/A3/A4 are compiled out and the other 16 match with the EVTs removed. Two notes:
   - x2's rows for A1 and C1 leave out a diagnostic `fe_evt` that comes before the teardown (`freeze_rig_recover`, `mem_budget FAIL`). The comparison drops a leading EVT unless it is directly followed by `evt_shutdown`, and the report marks those two rows.
   - `scratch/exitseq.awk`, the basis, uses a fixed -16..+4 line window. Its output bleeds adjacent sites into each other (for example C4 picks up `me_rend_suspend`, which is outside the block). T8 scopes each site to its enclosing block, and its orders equal the awk's last segment per site.
2. **"Its -E mode differs from the raw-source mode exactly at the #ifdef/no-op-macro rows": reproduced.**

   | Flavour | Differing calls | `#if` | `macro` | Unexplained |
   |---|---|---|---|---|
   | release | 114 | 28 | 86 | 0 |
   | lab | 90 | 0 | 90 | 0 |
   | harness | 53 | 0 | 53 | 0 |

   Mutant M10 (a call deleted from the -E output only) is reported UNEXPLAINED.
3. **"Has the release-live rows": reproduced in release -E, none behind `g_perf_rig`.**
   - `gpsp_rtc_peek`: line 18297, step P5.
   - `heap_trace_report("post_unload")`: line 18219, step U13.
   - the `inproc_force` `fe_ini_get_int`: line 18223, step I3.
   - the `audio_hash` guard `if (g_relaunch != GL)`: line 18285, step P3. The `if` is on 18285 and the `fe_evt` it guards is on 18286, the line PLAN cites. In release the body is `((void)0)`, but the guard row is still present.
4. Other figures:
   - Sites: raw 20 (19 in `main` + `stall_exit`), release 16, lab 20, harness 20. This matches x2's "20 ExitGame pairs in main" (19 early + 18331).
   - Teardown rows: release 88 (71 calls, 16 guards), lab 118, harness 115.

Tests (`abi-exit-ci_test_exit.sh`, 17/17 pass):
- C0: selftest.
- C1: baseline `all` passes.
- C2: a regenerated matrix passes the XS check against the first.
- C3: the model in 3 flavours.
- Model mutants, each caught in the flavours where it can be seen:
  - M1: D3/D4 swapped (HW-1 F2 undone: the lock dropped before the hold).
  - M2: the two `inproc_refusal` calls merged (§0.2-2; caught by LIST_RETAIN_THEN_REFUSE).
  - M3: the audio_hash guard dropped. Caught in lab and harness; release is invisible by construction, because fe_evt is `((void)0)`, and the control asserts that.
  - M4: `gpsp_rtc_peek` put behind PERF_RIG.
  - M5: AV-first sites lose `vid_term`.
  - M6: EVT-first sites put audio first.
- Source mutants, each a copy of main_psp.c:
  - M7: the teardown's pwr_hold/play_lock_drop swapped back to the pre-F2 order (01067e74 L:18099/18100). Since HW-1 F2 the order is D4 (pwr_hold) then D3 (play_lock_drop); the check reports "D4 after D3" and XS fails.
  - M8: B1 loses `vid_term`. XS fails at B1 in all 3 flavours.
  - M9: `gpsp_rtc_peek` behind `if (g_perf_rig)`. Reported RIG-GUARDED.
- M10: see item 2 above.

**Regenerating the golden.** §3.6 says HW-1/HW-2 change the pre-image first, so the golden is regenerated from their tip, not from 01067e74. `exit_matrix.py all` on that tip writes the new golden. `check --allow` takes `flavour<TAB>site<TAB>fix-commit` rows for reviewed differences.

**C hand mirrors (part of every allow-list review; PLAN-P-beta5 P1).** These C tests copy an order or a constant of main_psp.c by hand. They read no text, so no `@extract` marker protects them, and XS cannot see them. A commit that changes what they mirror updates them in the same commit, and the allow-list review checks that it did:

| Mirror | Mirrors in main_psp.c |
|---|---|
| `tools/tests/test_sram_exit_retry.c` `exit_like_the_psp()` (comment "What the PSP's exit does to the save, in order") | the teardown's save order: `io_thread_stop()` (the writer's last pass, then synchronous ownership and a sync), then `fe_host_shutdown()`'s flush / sync / close |
| `tools/tests/test_ctl_map.c` `plat_mask()` and `rig_step()` (comments "composed exactly as main_psp.c's plat_input_bitmask()" and "One loop iteration as main_psp.c runs it") | `plat_input_bitmask()`'s game mask plus turbo, and the loop's pad sample / turbo step / per-frame input order |
| `frontend-common/fe_gblink.c` `NO_PEER_STEPS` (30 × 60 setup steps) | `LINK_START_GRACE_US` (30 s; in `fe/fe_priv.h` since P5, used by `gblink_service`), the peer-dead grace `gblink_service` sets while the partner's HELLO is still missing: the same wait, in display steps there and in microseconds here |

---

## T11 `ci_greps.sh`

```
ci_greps.sh [--src GPSP] [--phase base|P|HW2] [--pp DIR] [--rules R1,...]
```

The rules work on parser tokens, so comments and strings are never matched. They scan `psp/` and `frontend-common/`; `launcher/`, `labab/` and `tests/` are skipped.

| Rule | What it checks |
|---|---|
| R1 | No raw `sceKernelExitGame(` / `handoff_run(` outside fe_priv.h. **`--phase base`**: in main_psp.c a call is accepted only after the hijack macros (L:2596-2597). **`--phase P`**: no exception. **`--phase HW2`**: P plus `stall_exit`. `usb_handoff.c` and `lab_harness.c` are allow-listed by file: they are the handoff implementations, reached only through the wrapped call (x2 §1). With `--pp`, **R1e** also checks the -E output: every exit/handoff call in main_psp.c is preceded by `pwr_release_all(),`. |
| R2 | A `cold` / `noreturn` declaration (attribute, `_Noreturn`, or a macro whose body has it) without `section(...)` in the same declaration. |
| R3 | `pwr_hold` reachable from any `gpsp_*` function, directly or through same-file calls. The report prints the path. |
| R4 | Logic inside `fe_evt(...)` arguments: an assignment, `++`/`--`, a call whose name is a verb (SDK `sce*Set/Start/...`, or snake_case with a leading/trailing verb, or kill/destroy/inject anywhere), or any call not in the reviewed `ci_greps.fe_evt_calls`. |
| R5 | A `.c` that tests `GPSP_NO_TELEMETRY` before it includes `fe_evt.h`, directly or through a tree header. |
| R7 | (tier C, C2-0; fail mode since C2-1) A raw `fe_ini_get(` / `fe_ini_get_int(` on `HARNESS_INI` or `p_harness` outside `psp/fe/fe_priv.h` (macro definitions count, comments do not). The call form is fe_priv.h's `HARNESS_INT(key, def)` / `HARNESS_STR(key, out, n)` / `HARNESS_PRESENT()`. `--r7 fail` (the default) lists each site; a KNOWN row's key is the harness key the read names (the frame loop's `rewind_gedump`, raw in every build so the release compiles the frame loop the Lab build runs: frame parity). `--r7 warn` prints a count per file and never fails. The key table itself is `harness_keys.py` (README). |

**What it does not prove.**
- R1 does not see an exit reached through a function pointer that was assigned elsewhere (taking `sceKernelExitGame`/`handoff_run` by address is flagged). R1e covers main_psp.c and the non-header files it `#include`s (the phase-P fragments).
- R2 does not see GCC's own `cold` propagation (`.text.unlikely` placement by inference). That is T10/G3's input-section audit.
- R3 merges the call graph of every scanned file (so it follows calls across fragments, and through macros whose body calls `pwr_hold`), but it does not follow function pointers.
- R4's verb heuristic and its allow-list are reviewed text, not a purity proof. A getter with a hidden side effect passes once it is listed. The 121 listed names were generated at base and then reviewed; the probes `heap_largest` and icprobe's `time_cold`/`time_warm` are annotated in the file.
- R5 does not check `#include` order inside headers.

**Acceptance: "green on base, except the known r6 B3 site". Reproduced with one more known site, which T11 found.**
- R1 base: 40 raw calls in main_psp.c, all after the hijack. That is 21 `sceKernelExitGame` (20 in `main` + `stall_exit`) and 19 `handoff_run`, which matches PLAN §0.2's "40 call sites after it".
- R1e: every exit/handoff call in main_psp.c is wrapped: release 33/33, lab 40/40, harness 40/40.
- `--phase P` on base lists the P3 work: 40 conversions, all in main_psp.c, including `stall_exit`, so there is no exception in P.
- `--phase HW2` gives 39.
- R2: 0 declarations (PLAN: "today there are 0 .cold").
- R3: 9 `gpsp_*` functions, 0 reach `pwr_hold`.
- R4: 876 fe_evt call sites, 280 calls inside arguments. Two are KNOWN:
  - **r6 B3**: `sceCtrlSetSamplingCycle`, main_psp.c:14855, fixed in HW-2.
  - **NEW, not in r6 or PLAN**: main_psp.c:2725, `fe_evt("sleep_sim sav_handle_killed=%d", fe_host_sram_test_kill_handle())`. The argument sets `sram_fp_dead = 1` (fe_host.c:2016-2022). It sits under `GPSP_PERF_RIG` (L:2599). Lab and harness keep telemetry, so it runs in every shipped rig flavour. It would be silently skipped in any PERF_RIG build without telemetry: the sleep-sim fixture would then not kill the handle. I recorded it as KNOWN rather than fail the base, and propose fixing it together with B3 in HW-2. **This is a deviation from PLAN's acceptance text**: two known sites, not one.
- R5: 3 files test the macro (fe_util.c, main_psp.c, sidecar_dirs.c), 0 too early.

Tests (`abi-exit-ci_test_ci_greps.sh`, all pass):
- C1/C1': base and a copied tree are green.
- C2/C3: the phase P/HW2 counts.
- R1 mutants:
  - M1: a raw exit in a new module.
  - M2: a `handoff_run` before the hijack.
  - M3: an -E exit that loses its wrapper (R1e).
- R2 mutants:
  - M4: cold without section, with a control that has a section.
  - M5: noreturn without section.
- R3 mutants:
  - M6: a direct `pwr_hold`.
  - M7: a `pwr_hold` two calls deep (the path is printed).
- R4 mutant, M8: an assignment, `audio_stop()`, and an unreviewed getter inside fe_evt args. All 3 are caught.
- R5 mutant, M9: the macro tested before the include, with a control.

---

## T14 `abi_asserts.py`

```
abi_asserts.py all --run RUNDIR --src GPSP --out DIR          # DIR/<fl>/fe_abi_asserts.h
abi_asserts.py gen --flavour-dir RUN/<fl> --src GPSP --out DIR [--main-c F] [--frag F fe/X.inc.c]... [--core-h F TREEPATH] [--test-c F] [--known K]
abi_asserts.py unbuilt --src GPSP [--gen DIR]                 # copy sites no DIR/<fl>/fe_abi_asserts.h cites (no docker)
```

**How it builds the header.**
1. It reads the frontend side from `psp-gcc -E` of main_psp.c, so each flavour gets its own set of copies. It takes every `extern` (file or block scope) and every file-scope prototype of a function main_psp.c does not define.
2. For each one it emits `typedef <copy with NAME -> abi_fe_t_NAME>` and `_Static_assert(__builtin_types_compatible_p(__typeof__(&NAME), abi_fe_t_NAME *))`. Comparing **pointers** keeps const/volatile in the check.
3. The check TU is `#include "common.h"` + `#include "psp/sleep_q.h"` + the header, compiled with -fsyntax-only on the flavour's own logged `main.o` compile line (the core flags).
4. A discovery pass moves the names the check TU's headers do not declare (common.h's chain and psp/sleep_q.h only: `gbcore_prepare`, declared in `gbcore/gbcore.h`, is UNCHECKED because the check TU does not include that header), and frontend types the core TU cannot see, into an UNCHECKED list in the header, with the reason. So a flavour's header lists every copy in that flavour's `-E`: the asserted ones and the UNCHECKED ones. A copy in an `#if` arm the flavour does not compile is in neither list, and a copy in an arm that none of release, lab and harness compiles is in no generated header. `abi_asserts.py unbuilt --src GPSP` lists those sites (P-review r3, B6): it runs the same extraction on the raw TU (`aec_cparse.load_raw`, the shell with its fragments, every `#if` arm kept) and prints the copy sites no generated header cites. At the P-review r3 tree that is 6 of 124 sites: `gamepak_reopens` in `main()` (GPSP_STANDBY_NOTES), `gbab_reg` and `fe_gbadual_running_inst` (DRPROF_HW), `rom_hash_census` (ROM_HASH_CENSUS), `jit_pub_inline` and `xlat_scan_fast` (JIT_COST_KEYS). It refuses when a cited site is missing from the raw scan (other than the 4 `__lib_*` externs that `PSP_MODULE_INFO` expands) or an uncited site is in no `#if` group (the headers are stale). Most UNCHECKED copies (the `rfu_set_*` setters, `sched_fast`, `gpsp_wallclock`, ...) are not in fe_priv.h's index, which is PLAN-P-beta5 §5.2's set (the asserted names plus six). `tests/p0a_t14_tu.py index` checks that statement against the generated headers (P-review r2, N4), and that fe_priv.h names the sites `unbuilt` lists (r3).
5. Constants are asserted too: VRAM_DIRTY_SHIFT/PAGES against gba_memory.h, and MER_INPUT_US/MER_RETIRE_US/MER_MISS_LIMIT/VRAM_DIRTY_PAGES against `tools/test_ff_pipeline.c`.

The headers are generated into `generated/<flavour>/` and the scratch dirs. **They are not wired into the emulator build.** `generated/<flavour>/` holds the check TU (`abi_check.c` + `fe_abi_asserts.h`, from `abi_asserts.py all`) of the tree it was last regenerated on: compile-only (`-fsyntax-only`, core flags), never in any OBJS list, so no linked object changes. P1 first committed it on the single-file P1 tree; since the P-review commits it is the split tree's (after P4), so its cites are `fe/X.inc.c:N` and `main_psp.c:N` lines of the shell, which do not exist in the P1 tree. Regenerate rather than edit.

**On the split tree (PLAN-P-beta5 P4; P-review 2026-11-08, B3).** `psp/main_psp.c` is then a shell that includes `psp/fe/X.inc.c`. The `-E` view already follows the includes, and every cite is the file and line that hold the copy: `fe/X.inc.c:N`, or `main_psp.c:N` for the shell (a single-file tree keeps its old `main_psp.c:N` cites). The constants are read from the raw text in TU order (`tu_lines`: the shell with each `#include "fe/..."` expanded, `fe_priv.h` and `play_defaults.h` included), and **a constant that is not found fails the gen** (exit 1, `constants: CONSTANT MISSING`): before the fix, a split shell lost all six constant asserts at rc 0. `--frag FILE fe/X.inc.c` lays a mutated copy of one fragment over the tree (for `-E` and the constants). A KNOWN reason is printed with the label `abi_asserts.known reason, its line cites are main_psp.c at 01067e74`, because those cites are 01067e74 lines. `tests/p0a_b5.sh t14` holds the split-tree rows: every cite of the generated header must name its line (`tests/p0a_t14_tu.py cites`), the gpsp_rom_load_progress sites are found through the includes, a fragment copy and a constant mutated with `--frag` fail their asserts, a renamed constant fails the gen, and a tool that reads the constants from the raw shell only (the pre-review rule) is caught. `tests/abi-exit-ci_test_abi.sh` edits the split tree's `split.py inverse` (laid over the shell with `--main-c`) for its main_psp.c mutants.

**What it proves.** Each asserted copy has the core's exact C type: arity, parameter order and types, pointer levels, qualifiers and return type. This covers the 21-parameter `core_phase_take`, `smc_prof_take`, `smc_blk_take`, the 2041-2053 and 4856-4864 externs, and the copied constants.

**What it does not prove.**
- It cannot check copies with no core header declaration: 43 in release and 53 in lab/harness. Examples are the `rfu_set_*` setters, which rfu.c defines with no header, the weak probes, `gamepak_buffers`, and `gpsp_rtc_peek`. They are listed in the header, and they would be checked if a core header gained the declaration.
- It cannot check copies in `#if` arms that none of release, lab and harness compiles: no flavour's `-E` has them. `abi_asserts.py unbuilt` lists them (6 at P-review r3).
- It does not compare against the *definitions* in .c files.
- It does not check frontend-common or other TUs' copies.

**Acceptance: "compiles; a mutated prototype fails". Partly reproduced: it does not compile strictly on base.**

Strict mode has **7 genuine C-type mismatches** in the 01067e74 copies. The finding is that on the PSP, `u32` is pspkernel's `uint32_t` = `long unsigned int`, so `unsigned int` is a different C type although it has the same size and registers:
- `smc_prof_clock`, `vram_dirty_marks`, `reg`, `gpsp_rom_load_progress`, `translation_gate_targets` and `translation_gate_target_pc` are copied as `unsigned int`; the core declares `u32`.
- `gamepak_must_swap` is copied as `int (void)` but is `bool (void)` in gba_memory.h:315. This is a real return-type mismatch. It works only because GCC's MIPS callee returns 0/1 in v0.

These are listed in `abi_asserts.known` with a normalisation (`uint`: `unsigned int` -> `u32`; `ret=bool`). The rest of each declaration is still compared exactly. With that file the header compiles in all three flavours:
- release: 93 copies found, 50 asserted, 43 unchecked, 6 constants;
- lab and harness: 110 found, 57 asserted, 53 unchecked, 6 constants.

Without it (C2), exactly those 7 asserts fail. The proposed fix belongs to the P-phase `core_glue.h` move: copy the core's `u32`/`bool` spelling, then delete the known file.

Tier C C-6 did this with `core_fe.h`: C6-3 deleted the six `unsigned int` copies, and C6-4 deleted the `int gamepak_must_swap(void)` copy (the frontend now reads core_fe.h's `bool` declaration). Since C6-4 `abi_asserts.known` has no rows; C6-7 deletes the file.

Tests (`abi-exit-ci_test_abi.sh`, all pass):
- C1: 3 flavours compile.
- C2: strict mode gives exactly 7 failures.
- Prototype and type mutants:
  - M1: one `core_phase_take` parameter changed to `int*` in main_psp.c.
  - M2: the core's `core_phase_take` in main.h loses its last parameter.
  - M3: `flush_ram_full` changed to `u16`.
  - M4: `const` added to `cph_jit_total`.
- Constant mutants:
  - M5: VRAM_DIRTY_SHIFT changed to 11.
  - M6: the test file's MER_RETIRE_US changed to 9001u.
- M7: a KNOWN entry (`reg`) changed to `unsigned short[]` is still caught.

---

## Numbers that differ from PLAN (summary)

| Item | PLAN | Measured | Why |
|---|---|---|---|
| T11 known R4 sites | 1 (r6 B3) | 2 | main_psp.c:2725 `fe_host_sram_test_kill_handle()` inside fe_evt args (PERF_RIG only), not in r6 |
| T14 "compiles" | yes | only with 7 reviewed normalisations | `unsigned int` vs `u32` (= `unsigned long` on PSP) ×6, `int` vs `bool` ×1 |
| T8 x2 A1/C1 rows | x2's step list | +1 leading diagnostic EVT each | x2 omitted the pre-teardown log line; the comparison accounts for it and marks the rows |
| T8 audio_hash guard line | 18286 | 18285 (`if`), 18286 (`fe_evt`) | PLAN cites the guarded statement |

---

## Verifier fixes (adversarial pass, 2026-11-05)

Each of these was a way to get a false PASS (or, for T8 on a moved tree, a false FAIL) from the first version. Every one now has a mutant test.

| Tool | Hole | Fix | Test |
|---|---|---|---|
| T8 | Site ids came from 01067e74 line ranges. After any edit that moves lines (every phase-P step), ids became `-` or wrong: a 3-line shift gave 43 false XS differences. With a golden regenerated from a shifted tree (PLAN §3.6), all sites shared the key `-` and collapsed to one dict entry, so B1 losing `vid_term` passed both `all` (rc 0) and `check`. | Ids by ordinal in the raw view (when it has x2's 19 sites), mapped by line into the -E views; a unique `key` column; duplicate keys refuse the gate. | M11, M13 |
| T8 | `check_x2` printed `absent` and did not count it. | `ABSENT` fails unless the raw site's exit call sits inside an `#if` (release A1/A3/A4). | M13 (golden side) |
| T8 | The XS gate compared only the collapsed vocabulary: changing an exit code (`handoff_run(2,..)` -> `(3,..)`) passed. Out-of-band sites (`E:stall_exit`) were not gated. | The gate compares seq + code + why + the full ordered call-name list, for every site including `E:*`. | M12 |
| T8/T11/T14 | `load_pp` kept only tokens marked `main_psp.c`; code moved into an `#include`d `psp/fe/*.inc.c` fragment (PLAN §1.1) vanished from every -E view. | Also keep tokens from included non-header files (none at 01067e74, so base figures are unchanged). | T11 M3c |
| T11 R1 | Preprocessor lines were skipped: a raw exit in a `#define` of another header was invisible; taking the exit by address was not seen. | Macro bodies are scanned (the two base hijack defines are accepted only in `--phase base`; in P they are listed, 40 calls + 2 defines); bare references are flagged. | M1b, M1c |
| T11 R1e | Any call after `:` counted as wrapped and any call after `?` was skipped, so `rc ? 0 : sceKernelExitGame()` passed. | Structural: the call must sit inside a paren group starting `pwr_release_all(),` (at most 3 levels out). | M3b |
| T11 R3 | Per-file call graph; no macro aliases. | Union graph over all scanned files; function-like macros calling `pwr_hold` count as `pwr_hold`. | M7b, M7c |
| T11 R4 | A macro forwarding to `fe_evt` hides its arguments. | Such a `#define` fails R4. | M8b |
| T14 | The discovery pass moved any copy whose typedef did not compile into UNCHECKED: `extern fe_count_t flush_ram_full;` with `fe_count_t` = `unsigned short` (core: `u32`) gave rc 0, 49 asserted. | `abi_asserts.pinned` (release 50, lab 57, harness 57): a pinned name that is no longer asserted fails; `--write-pin` after review. Since code-health C6-7 the pin is gone: the TU includes core_fe.h, a copy of a core_fe.h name fails gen (the guard; a retyped one is also a compile error), and any other UNCHECKED copy fails `abi_asserts.ratchet` (0 UNCHECKED). | M8, since C6-7 M1/M2 |
| T14 | PLAN §1.4 lists `SQ_DONE == 0 && SQ_NONE == 1`; it was not asserted. | The header asserts it (the check TU includes psp/sleep_q.h). | M9 |
| T6 | A mistyped `--golden` path silently dropped every derived-ABI entry and stayed green. | A missing golden is an error. | - |

Note on PLAN §1.4: the table puts `fe_abi_asserts.h` under fe_priv.h, but the asserts need the core headers (`__typeof__(&NAME)`), which main_psp.c deliberately does not include (main_psp.c:1108-1112, 4851-4853). The header can only be compiled in a separate check TU with the core flags, as this tool does.

## Epoch C step C0a (2026-11-12): T8 repaired for the split tree and extended; T11 R6

Written on `claude/epochc-plan` (b7d0cafd plus tools). Plan: `gpsp/docs/design/EPOCH-C.md`.

**Why T8 was red on every post-P tree.**
- `run_all.sh`'s default baseline is the 01067e74 run, whose compile line has no `-DGBADHOC_MGIFT`. The `-E` views then lose the teardown's `mgift_stop(1)`, and the model fails against a golden that is wrong (16/24, 19/27). `exit_matrix.py all` now refuses such a baseline (`STALE BASELINE`: a define that `psp/Makefile` turns on by default is missing from the logged compile line). Give it a `flavours.sh` run of the tree itself; a `flavours.sh --baseline` run of the phase P tree (a local output, not in the repo) has the same defines as b7d0cafd. The reference model reads `GBADHOC_MGIFT` like the tree.
- The source mutants edited the single file by line number and wrote the copy to a directory without `fe/`. They now find their line by anchor, the raw view reads the fragments from the tree (`aec_cparse.load_raw(base=)`), and `--over fe/X.inc.c=FILE` mutates a fragment (read by the raw view, bind-mounted for `-E`).
- `p0a_test_exit_matrix.py` assumed the pre-P3 hijack. On a tree where P3 has landed it runs P3's sed backwards, and on a split tree C3 compares the `split.py inverse` single file with the shell read through its includes.

**What XS compares now (C0a-5).**
- **Text.** `exit_sites.tsv` and `exit_scenarios.tsv` have a `text` column: every call that passes a string literal, with its arguments (`fe_evt` format strings, handoff reasons, notices, the tags of `pwr_hold` and the teardown calls). `check` compares it. `exit_expect.h` carries each scenario's `fe_evt` format strings as C literals, and `test_exit_model.c` compares them with what the implementation passes.
- **Power hold.** `exit_matrix.py all` walks every function's direct `pwr_hold` / `pwr_release` calls:
  - branches that meet must agree;
  - a `goto L` must arrive holding what `L` was first reached with;
  - the frame loop is entered holding nothing and each pass is balanced;
  - every other function gives back what it takes on every return. A function may drop its caller's hold and take it again (HW-1's `boot_stop_notice`); `HOLD_KNOWN` lists the one toggle (`psp_rewind_pwr`).
  - Each site records the hold it exits with (`hold` column: `boot` for C1-C4 at b7d0cafd, for B1-C4 after HW-1), each scenario its ledger (`max=1 end=0`), and `check` compares both. The model fails a scenario that returns to the list, hands off or exits with a hold up.
- **HW-2 atoms.** On a tree whose `main()` reads them: `g_exit_req` adds `HOME_BEFORE_TEARDOWN`, `HOME_BEFORE_REFUSAL2`, `HOME_IN_UNLOAD` and `HOME_PROFILE`; `g_link_fatal_why` adds `LINK_FATAL`. A scenario that returns in-process ends with `g_running=N`, the value the reset left.
- **The reference model follows the tree.** `exit_expect.h` defines `EXPECT_D4_FIRST` (HW-1 F2), `EXPECT_SETTINGS_FLUSH` (HW-2 F3), `EXPECT_F1`, `EXPECT_F5`; the site flags gained `TD_ME` (HW-2 F9) and `PE_HO_BRANCHES` (HW-2 F7). One reference file is green at b7d0cafd, at claude/hw1-sleep-r2 7301ee2a and at claude/hw2-exit 0c6783aa (24/24, 27/27, 27/27; 24/24, 27/27, 27/27; 29/29, 32/32, 32/32).

Tests (`abi-exit-ci_test_exit.sh`, 29 checks at b7d0cafd, 30 on an F1 tree; `--selftest` 30 cases):
- S1: a baseline without `-DGBADHOC_MGIFT` is refused in 3 flavours.
- M14: a log line reworded in the model (lab, harness). M15: the return keeps its hold.
- M16: B1's log line reworded with the same calls: XS fails at B1 in lab and harness only.
- M17: the teardown's `evt_drop` line reworded: XS fails in every scenario, lab and harness.
- M18: the return's `pwr_release` deleted: the hold check fails (the goto and the LIST_OK ledger).
- M19: a fragment mutant (`--over fe/misc.a.inc.c`): `state_save_slot` keeps its hold.
- M20: the boot hold released before `net_bringup`: XS fails at C4 (hold `boot` -> none).
- M21 (F1 trees): process_end without the `g_exit_req` guard.

**T11 R6 (new).** Every static function of main_psp.c's unit that only `main()` calls carries `noinline` and an explicit `section(...)`, unless `ci_greps.r6_pinned` lists it (107 names at b7d0cafd: the ones that exist today). Reason: the C0a-6 probe (`epochc_probe.sh`; its outputs are not kept in the repo; [EPOCH-C.md](../../../docs/design/EPOCH-C.md) §4.2). Measured on release and lab at b7d0cafd, GCC 15.2.0:

| A function only `main()` calls, declared | lands in | effect on `main` |
|---|---|---|
| `noinline`, no section | `.text.<name>`, after `layout_pad_cold` | none today; GCC decides |
| `noinline, section(".text.startup.<x>")` | between `layout_pad_main` and `main` | moves `main` by its size |
| `noinline, cold`, no section | `.text.unlikely.<name>`, in front of `layout_pad_main` | moves the pad and `main` |
| `noinline, section(".text.fe_cold.<x>")` | main_psp.o's `.text.*` run, after `layout_pad_cold` | none |
| `noinline, noreturn`, no section | `.text.<name>`, after `layout_pad_cold` | none today |
| `static`, one caller, no `noinline` | inlined into `main` | grows `main` |

So an Epoch C function is `static __attribute__((noinline, section(".text.fe_cold.<name>")))`, never `.text.startup.*`. Regenerate the pinned list once, on the tree Epoch C starts from (`ci_greps.py --src GPSP --write-r6`).
The T11 self-test's mutants follow the split tree (the hooks live in fragments; P3's phase lists are empty): 26 checks.

**`epochc_exitmap.py`** also lists the teardown's steps (`D-step`), the exits outside main_psp.c's unit (`oob`: `lab_harness.c`, `usb_handoff.c`) and why the game list returns non-zero (`browser-ret`), and `--diff OLD NEW` compares two maps by what a row is, never by line, including the teardown's order.

## Epoch C step C1 (2026-11-13): the tools read the exit helpers as their bodies

C1 moves the pasted exit sequences into `fe/teardown.inc.c`: `exit_early_begin(f)` and `exit_early_end(f, code, why, pass)` for families A-C, `session_end(have_variant)` and `process_end(exit_code, exit_reason)` for family D (the in-process return stays in `main()`, because of its `goto`).

- **T8 / the exit map.** `aec_cparse.inline_helpers` replaces every call statement of those four helpers (`exit_matrix.EPOCHC_HELPERS`) by the helper's body before anything parses `main()`: parameters become the call's argument tokens, every token takes the call's line, file and `#if` stack, and every `if (COND) { .. } [else { .. }]` whose COND is constant (literals, the TU's enum constants, here the `XE_` flags) is reduced to the arm it takes, braces dropped. So a site, the teardown steps, the scenarios, the text and hold columns and the raw-vs-`-E` comparison see the code each site runs, and the golden of the start tree is compared unchanged. The helpers are never sites of their own. The helpers' constant `if`s must have braced arms and no `else if` (the inliner refuses otherwise). Because every inlined token carries the call's line, two same-name calls of one helper share a line in `raw_vs_e`'s (site, name, line) keys; a difference between them is still caught by the per-site call lists of XS.
- **T11 R6.** An `always_inline` static (stage s) passes: the compiler pastes it into `main()` and places nothing. `always_inline` with `noinline` fails. Stage o's helpers carry `noinline` and `section(".text.fe_cold.<name>")`.
- **exit_paths.** On a tree with the helpers, their text is compiled with the slices; the load-failed slice ends at its `exit_early_end` call; on a C1c tree the teardown slice is `main()`'s text from `session_end(have_variant);` to `process_end(exit_code, exit_reason);`. The mutants mutate whichever form the tree has; one more drops `vid_term` from the helper's EVT-first arm.
