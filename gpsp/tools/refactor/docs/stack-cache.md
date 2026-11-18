# T12 `stack_depth.py` and T16 `loop_lpsim`: stack and cache proof tools

These are the phase-T tools for gates **ST** and **LP** (PLAN.md §0.1 and §5) of the
main_psp.c restructure. Both run on the host and need no PSP build. T16's simulation
needs WSL, docker and the `drprof-qemu` image.

All acceptance runs used the clean baseline of 01067e74:
`mainpsp-tools-out/baseline/run1/{release,lab,harness}` beside the checkout (a local output, not in the repo; see [baseline.md](baseline.md)).

---

## T12 `stack_depth.py` (gate ST)

### Usage

```
python3 gpsp/tools/refactor/stack_depth.py ELF --su FLAVOUR/su --resolve gpsp/tools/refactor/stack_resolve.txt [--json OUT] [--tree N]
python3 gpsp/tools/refactor/stack_depth.py --selftest ELF --su FLAVOUR/su --resolve gpsp/tools/refactor/stack_resolve.txt
bash    gpsp/tools/refactor/tests/stack-cache_test_stack_depth.sh [BASELINE_RUN_DIR]
```

Exit codes: 0 PASS, 1 FAIL (over budget), 3 INCOMPLETE, 2 usage error.

INCOMPLETE means the total is within budget, but something is still unresolved. That
can be an indirect call, an unresolved `jr`, a dynamic frame, recursion or an unknown
call target. In that case the figure is only a lower bound.

The defaults follow the PLAN:
- roots `power_cb,exit_cb`
- base `cb_thread`
- budget 3,200 B
- limit 4,000 B (`0xFA0`, main_psp.c `setup_exit_callback`)

### What it proves

- **The call graph comes from the linked ELF**, so it reflects what runs after inlining,
  cloning and LTO. Every STT_FUNC body is decoded, and edges are classified as follows:

  | Instruction | Treated as |
  |---|---|
  | `jal`, `bal`, `bgezal` | call |
  | `j`, or a branch leaving the function | tail call, summed like a call (an upper bound) |
  | `jalr` | **indirect call: flagged and never summed as 0** |
  | `jr rX` with rX ≠ `ra` | indirect jump |

  An indirect jump counts as a switch only when a jump table is found for that function,
  meaning at least 2 consecutive words in a data section point into it. Otherwise it is
  flagged.
- **Frame sizes come from a prologue scan** (`addiu sp,sp,-N`), and `lui/ori + subu sp`
  is resolved. This covers newlib and the SDK, which have no `.su` files.
  - With `--su`, the tool matches every `-fstack-usage` record (file + name for statics)
    and uses `max(.su, prologue)`.
  - Every disagreement between the two is listed.
  - On the baseline the prologue scan agrees with **1759/1759** (harness),
    **1831/1831** (lab) and **1680/1680** (release) matched records. That validates the
    scanner on our own objects.
- **Syscall stubs** have a 0 B user frame.
- **`...CB` syscalls** reachable from a callback are flagged as re-entry points. None
  are found on the baseline.
- **Recursion** is flagged.
- **Reviewed resolutions** come from `stack_resolve.txt`. Every line carries its
  evidence, and there are four kinds:
  - indirect-call targets
  - `-` for "this site makes no call"
  - `!prune` for noreturn termination paths
  - `!unroll A B [-X]` for proven bounded recursion
- **Two figures are always printed.** The raw bound has no prunes and no unroll (cycles
  are cut). The reviewed bound uses the resolve file. The verdict is based on the
  reviewed bound.

### What it does NOT prove

- **Kernel callback dispatch is not modelled.** It is unknown whether the kernel adds
  any user-stack bytes when it dispatches a callback inside `sceKernelSleepThreadCB`.
- **Interrupt handlers are not covered.** They run on the kernel stack.
- **Function pointers are trusted to the resolve file.** The tool knows only the
  targets that file lists, and a new writer of `gpsp_wallclock`, `gamepak_evict_hook`,
  `evt_clock`/`evt_wake` or `host_io` makes that file stale. Re-review it whenever the
  cited code changes.
- **Feasibility is not checked.** The bound assumes every path can run together, for
  example `emit → snprintf → _dtoa_r`, even when no `%f` format can reach it.
- **Prunes and unrolls are judgement calls, documented in the file.** The tool trusts
  them; it does not prove them.

### Acceptance against PLAN §5 / phase T

| PLAN item | Result |
|---|---|
| Runs on an existing harness ELF with `.su` files from a scratch build | Yes. The `.su` files come from `flavours_sidecar.sh` (the baseline's `su/`), and `-fstack-usage` changes no loadable byte (BASELINE.md) |
| The prologue scan finds a non-zero frame for `vsnprintf` and `fopen` | `vsnprintf` has its own 120 B frame (subtree 856 B). **`fopen`'s own frame is 0 B**: it is a tail-call wrapper (`j _fopen_r`). Its subtree is 1,752 B, so the PLAN wording holds only for the subtree. Also measured: `fprintf` 40 B (subtree 2,440), `snprintf` 152 B (888), `fclose` 0 B wrapper (464) |
| Report < 3,200 B | **Met**, with the reviewed resolve file: harness and lab 2,928 B, release 312 B. Details below |
| Today's figure recorded as the baseline | Recorded below |
| Indirect calls flagged, never summed as 0 | Yes. Without the resolve file the harness has 24 `jalr` sites and 2 unresolved `jr` sites, and the verdict is INCOMPLETE. Mutant M2 covers this |

**Baseline figures**, cb_thread (16 B) + the worst root. exit_cb is 0 B in every flavour.

| Flavour | Reviewed total | Worst path (reviewed) | Raw bound (no prunes) |
|---|---|---|---|
| release | **312 B** (PASS) | power_cb 40 > me_rend_release_vmem > fe_host_rewind_suspend > gpsp_rewind_clock_mode > gpsp_rewind_clock_sync > rtc_current_time 32 > __fixdfdi > ... > __subdf3 104 | 312 B |
| lab | **2,928 B** (PASS) | power_cb 64 > standby_note 408 > fprintf 40 > _vfprintf_r 360 > __sbprintf 1152 > _vfprintf_r 360 > __sprint_r > __sfvwrite_r > _fflush_r > __sflush_r > __swrite > _lseek_r > _lseek > _lseekDir 376 | 3,512 B |
| harness | **2,928 B** (PASS) | same as lab | 3,512 B |

### Findings worth carrying into the plan

1. **The standby_note frame is 408 B, not 352.** On the clean lab and harness ELFs,
   `standby_note` is 408 B in both the prologue scan and `.su`. PLAN §0.1 ST says
   "a 352 B frame". In the release build `standby_note` is compiled out
   (`GPSP_NO_TELEMETRY`), so release has no telemetry stack at all.
2. **The raw bound of 3,512 B is over the 3,200 B budget**, though under the 4,000 B
   limit. It runs through newlib's `_dtoa_r → __multadd → __assert_func`, which aborts
   on a Balloc failure, and then through `fiprintf(stderr) → isatty → _stat → mktime →
   tzset → siscanf → realloc`. `!prune __assert_func` removes it. This path is
   reachable from `fe_evt` inside `fe_host_rewind_suspend`, which `power_cb` calls
   through `me_rend_release_vmem` in the lab and harness builds.
3. **`__sbprintf` is the largest frame on the graph (1,152 B).**
   - Newlib's `_vfprintf_r` falls back to `__sbprintf` for an unbuffered stream, which
     includes standby.log if `__smakebuf_r`'s malloc failed, and `__sbprintf` re-enters
     `_vfprintf_r` once.
   - The resolve file unrolls that cycle exactly once. Its second pass cannot reach
     `__smakebuf_r`, because the fake FILE has a buffer. Both are newlib source facts,
     and they are cited in the file.
   - **Without that context prune, the strict bound is 4,048 B, which is over the
     4,000 B stack.** That path needs `__smakebuf_r` to run inside the fake FILE pass,
     which the newlib code rules out. It is listed here so that a reviewer can check
     the argument.
4. **The worst real path is the telemetry `fprintf` in `standby_note`** and its newlib
   fallbacks. The PLAN's HW-2/telemetry work, which removes standby notes from the power
   callback, would shrink it to the release figure.

### Mutants (`--selftest`, all flavours PASS)

- **M1:** the frame of a function on the worst path is raised in the ELF bytes past the
  budget. The total grows by exactly that amount and the verdict is FAIL.
- **M2:** a `jal` in power_cb becomes `jalr $t9`. It is flagged INDIRECT and the verdict
  becomes INCOMPLETE.
- **M3:** exit_cb gets a `jal standby_note` (release: `me_rend_release_vmem`). exit_cb's
  depth grows (0 → 2,848 B).
- **M4:** a callee calls power_cb back. It is flagged RECURSION.
- **M5:** power_cb's `.su` record is raised by 48 B. It is reported as a MISMATCH, and
  `max()` is used.
- **M6:** a `lui/ori/subu sp` large frame is resolved (69,632 B). M6b: `subu sp` with
  an unknown register is flagged DYNAMIC.
- **Control:** without the resolve file, the harness verdict is not PASS.

---

## T16 `loop_lpsim` (gate LP)

These files are in `gpsp/tools/refactor/loop_lpsim/`. The existing layout tooling is
**unchanged**: `tools/layout/lp_map.py` is imported read-only, and `lpsim_plugin.c` is
copied, not edited.

| File | Role |
|---|---|
| `loop_lpsim.py` | `window`, `loopfile`, `shift-nm`, `setmap`, `layouts`, `report`, `--selftest` |
| `loop_lpsim_plugin.c` | `lpsim_plugin.c` plus a fourth group **L**: per-layout `loop=FILE` lines fetched at every frame marker after F. Without `loop=` it is the original |
| `loop_sim.sh` | one twin run with **both** plugins on the same trace: old → `sim_old.txt`, new → `sim_new.txt`; then `report`. Writes only under `$LP/loop/` |
| `dwline.py` | DWARF `.debug_line` decoder, copied from `scratch/x4_dwline.py` |

### Usage

```
python3 loop_lpsim.py window ELF                       # loop window, main mod 8192, set map
python3 loop_lpsim.py setmap ELF_A ELF_B [--shift-b N] # static set comparison, no docker
# simulation (WSL):
python3 loop_lpsim.py layouts --lp ~/lp --set S --fixture em_battle --twin rc45f --prof runs/prof/rc45f \
   --frame-path gpsp/tools/layout/psp_frame_path.txt \
   --elf acc=ACCEPTED.elf --elf cand=CANDIDATE.elf [--elf s160=ACCEPTED.elf+160] [...#bylines] [--rates R]
flock .../mainpsp-tools-out/build.lock bash loop_lpsim/loop_sim.sh S em_battle rc45f
bash gpsp/tools/refactor/tests/stack-cache_test_loop_lpsim.sh [BASELINE_RUN_DIR]   # no docker
```

`ELF+N` is a never-linked candidate: every text symbol at or after `main` is moved by
N bytes. This is the PLAN's "seeded 160 B shift before main". The tool reads `elf.nm`
next to the ELF.

### What it proves

- **The loop window, by line and by address.**
  - By line: the main_psp.c rows of L:16368-18090 inside `main`.
  - By address: every byte of `main` between the first and last such row, which
    includes inlined helpers. The bytes in the window are attributed by file.
  - main's start mod 8192 and the set histogram are reported too.
- **The cache-simulation extension reproduces the existing twin simulation exactly.**
  Both plugins see the same qemu trace, and the J/S/F misses and accesses of every
  `-noloop` row match `lpsim_plugin.c`'s.
- **The loop's miss delta of a candidate against the accepted ELF**, at the given fetch
  rates, plus the total delta, so that eviction of the hot path counts too.

### What it does NOT prove

- **The rates are placeholders** (see the TODO below).
- **Loop lines are fetched as one block at the frame marker, not interleaved where they
  run.** The real loop runs code before and after `fe_host_run_frame`. Repeated fetches
  of one line inside a block always hit, so inner loops (the vblank/lat wait polls)
  cannot show conflicts in this model.
- **The twin is `dr_host_rc45f` (4.5.0 final), not a twin of 01067e74.** Static code is
  placed by function name, so it places correctly, but the profile is the 4.5.0 core's.
  Rebuilding the twin is `tools/layout/twin_build.sh . <variant> ...`.
- **Other threads, the D-cache and the ME are not modelled**, as in
  LAYOUT-PINNING.md §1.1.
- **The JIT base is held at 0x1500 mod 8 KiB** (LAYOUT_PIN) in every layout, so a
  candidate whose JIT base moves is not modelled.

### Acceptance against PLAN §5 / phase T

| PLAN item | Result |
|---|---|
| Reproduces the twin's current static-code misses | **Met.** em_battle, 2,045 frames: old plugin vs new `-noloop` rows **IDENTICAL** on all 6 layouts, e.g. accepted J 4,244,043/121,645,302, S 3,770,922/205,677,005, F 300,086/2,699,400. aw2 results are in the "Twin runs" section |
| Adds the loop window at measured rates | **Not met: TODO.** A PPSSPP trace was not allowed in this run, and no trace producer exists in the tree. Placeholder rates were used |
| Ranks a seeded 160 B shift before main as a measurable change | **Not met under placeholder rates.** The shift changes main mod 8192 (6912 → 7072) and the static set map (5 sets change occupancy), and the tool reports it. But the simulation gives **ΔL = 0 and ΔTotal ≈ 0 misses/frame** for the window model. The reason is that under the twin's traffic (~4,300 misses per frame, more than a full 16 KiB turnover) a loop line fetched once per frame always misses, wherever it sits. The answer may change with measured rates or interleaved fetches; it does not with these placeholders |
| By-address window recorded (input to T1b) | release: main 0x0893bb00 (mod 8192 = **6912**), window main+10,372..+34,432 (24,060 B, 376 lines, all 128 sets). Of that, 9,828 B are loop rows, **13,280 B are main_psp.c rows outside the loop range (inlined helpers)**, and 952 B come from headers. x4's 220 lines / 115 sets by line **reproduce exactly** on the clean release ELF |
| Flavour figures (no x4 reference) | lab: main mod 8192 = 4160, 273 lines / 123 sets by line, window 28,224 B / 442 lines. harness: 4160, 282 / 123, 27,392 B / 429 lines |

### Twin runs (2026-11-05, `$LP/loop/runs/t16base/<fixture>/report.txt`)

Layouts: release baseline `acc`, `s160` (+160 B before main), `s8192` (+one way, a
control that must equal acc), each with the `window` model (all lines of the
by-address window, once per frame) and the `bylines` model (the 220 by-line lines,
once per frame; rows suffixed `b`).

The first em_battle run showed +1 L access per frame on the shifted rows. That was a
plugin bug: a loop file's long `#` header line overflowed a 128-byte `fgets` buffer, and
its tail was parsed as an address. The buffer is now 1,024 bytes. The numbers below
come from the rerun.

Copies of the reports and raw sim files are in
`scratch-stack-cache/t16base/` (local, not kept in the repo).

**Reproduction.** On both fixtures the old plugin and the new `-noloop` rows are
IDENTICAL on all 6 layouts (J, S and F misses and accesses). The 8 KiB control is
identical to acc in every group, which shows the shift and placement machinery adds
nothing by itself.

| Fixture | Frames | acc (window) L / total misses per frame | s160 − acc (window) | accb (bylines) L / total | s160b − accb | s8192 − acc |
|---|---|---|---|---|---|---|
| em_battle | 2,045 | 376.0 / 4,464.5 (0.961 ms) | ΔL 0.0, ΔTotal 0.0 | 220.0 / 4,303.3 | ΔL 0.0, ΔTotal −0.7 (−0.0002 ms) | 0 / 0 |
| aw2 | 6,015 | 376.0 / 3,272.3 (0.705 ms) | ΔL 0.0, ΔTotal 0.0 | 220.0 / 3,107.4 | ΔL −0.1, ΔTotal +1.0 (+0.0002 ms) | 0 / 0 |

The loop's own cost under the placeholder rates is 376 (window) or 220 (bylines)
misses per frame, about 0.081 / 0.047 ms. Every loop line misses every frame.

The shift's effect on the twin's own static code (the `-noloop` rows) is also tiny:

| Fixture | ΔF (whole run) | ΔS | ΔJ | Per frame |
|---|---|---|---|---|
| em_battle | +12 | 0 | 0 | 0.006 |
| aw2 | +45 | +26 | +6 | 0.013 |

Under these placeholders the +160 B shift is about 0.0002 ms per frame, roughly
10⁻⁴ of the 3 % noise floor. **The tool ranks the shift as non-measurable.** That is a
model result, not a hardware result. It says that `main`'s placement matters only
through lines fetched many times per frame, which only a measured trace can show.

### T16 measured rates: TODO

`--rates FILE` takes `# frames N` followed by `<psp_hex_addr> <executions>` rows. That
is the `counts.txt` format of `tools/cachemap/icount_plugin.c`, with PSP addresses.
Each 64 B line in the window is fetched `max(executions)/N` times per frame.

**Nothing in the tree produces this file for PSP code today.** PPSSPP (pinned
`d90fdee8`, `tools/e2e/setup_ppsspp.sh`) has no block-count export, and DRPROF_HW
samples a phase word, not PCs.

The missing piece is a PPSSPP-side per-block execution counter over
`[main, main+size)` of the EBOOT under test, dumped after a fixture. One way to build
it is a patch to the PPSSPP build that setup_ppsspp.sh makes: count IR/JIT block
entries by PSP PC, and write the counts on exit. Once that exists, the commands are:

```
# 1. capture (placeholder until the counter exists): run the Lab fixture in the patched PPSSPP, write rates.txt
# 2. simulate with measured rates:
python3 gpsp/tools/refactor/loop_lpsim/loop_lpsim.py layouts --lp ~/lp --set meas --fixture em_battle \
   --twin rc45f --prof runs/prof/rc45f --frame-path gpsp/tools/layout/psp_frame_path.txt \
   --rates rates.txt --elf acc=<accepted>/gpsp_adhoc.elf --elf s160=<accepted>/gpsp_adhoc.elf+160
flock mainpsp-tools-out/build.lock bash gpsp/tools/refactor/loop_lpsim/loop_sim.sh meas em_battle rc45f
```

Interleaving the loop's fetches with the core's, rather than fetching them as one block
at the marker, needs the measured trace's order as well as its counts. That is a
second extension, also open.

### Self-test and mutants (`--selftest`, no docker; PASS on release, lab and harness)

- **Release:** the 220 / 115 by-line figure equals x4's.
- **The window contains every by-line row.**
- **M1:** +160 B before main changes main mod 8192 and the set histogram.
  **C1:** +8192 B restores the set map exactly.
- **M2:** `shift-nm` moves only symbols at or after `main` (4,920 in release).
- **M3:** a rates file is honoured inside the window, and rows outside it are ignored.
- **C2/M4:** `report` passes identical J/S/F rows, computes the loop delta, and fails
  when the S group differs.
- **Plugin enum check:** the copy adds L, and `tools/layout/lpsim_plugin.c` is unchanged.

---

## Adversarial verification pass (2026-11-05, verifier)

The verifier wrote new mutants against copies of the baseline ELFs (in
`scratch-stack-cache-verify/`, a local dir not kept in the repo, never the worktree sources) and
found false PASSes in both tools. Each is fixed, and the mutant is now in `--selftest`.

### T12 `stack_depth.py`: false PASSes found and fixed

| Mutant | Before the fix | Fix | After |
|---|---|---|---|
| **M7** `cb_thread`'s `jal sceKernelGetThreadId` becomes `jal fopen` | release 312 B PASS: the base thread's own callees were never walked, only `frame(cb_thread)` was added | the base is walked as one more root: total = max(frame(base) + worst callback, depth(base)) | release 1,776 B; lab 2,928 B (fopen's 1,768 is below the standby path) |
| **M8** a `jal` in a reviewed caller (`emit`) becomes `jalr` | PASS: one resolve line covered *every* indirect site in the caller, old or new | each resolve line must carry `sites=N` (jalr + jr count when it was reviewed); a different count is RESOLVE_STALE | INCOMPLETE |
| **M9** `jr t9` (indirect tail call) added to `_vfprintf_r` / `_close` | PASS: one jump table anywhere in the function hid every `jr` in it | a `jr` is switch dispatch only if its register comes from `lw rX, lo(rB)` and a constant the function builds plus lo lands in a table pointing strictly inside the function (an `{f, f}` pointer pair no longer counts) | flagged JR, INCOMPLETE. The four real switch functions (`_vfprintf_r`, `_svfprintf_r`, `__ssvfiscanf_r`, `_close`) still classify |
| **M10** a resolved callee renamed in `.symtab` (`__swrite` → `__swritf`, a clone rename such as `.part.0`) | PASS, and the lab total silently dropped 2,928 → 2,912: absent callees were skipped | a callee absent from an ELF that has the caller is RESOLVE_STALE (`?name` marks a flavour-only callee) | INCOMPLETE |
| **M11** `cb_thread` registers another function in place of `exit_cb` (the lui/addiu retargeted) | PASS: the roots were a fixed list | every function address `cb_thread` builds (lui + addiu/ori) must be a root or a direct callee, otherwise CB_UNLISTED | INCOMPLETE |
| **M12** `lui v0,1; addiu v0,a0,16; subu sp,sp,v0` (also `seb`, `lwl`) | frame 65,536 B, not DYNAMIC: `addiu`/`ori` from an untracked register, SPECIAL3, `lwl/lwr` and COP moves kept a stale constant | every register write drops the tracked constant; constants are also dropped at branch targets and after every branch/jump | DYNAMIC |
| **M13** two different decrements in one function | `max()`: `__ssvfiscanf_r` has `addiu sp,-416` and later `addiu sp,-8` | the distinct decrements are summed (upper bound), and MULTI_FRAME is reported as a note | 424 B |

The resolve file lost one bogus callee. `sess_saved_wallclock` is a static pointer
*variable* (fe_gbadual.c:620), not a function, so it was silently absent in every flavour.
Its value is one of the two listed callees, and the evidence now says so.

**Figures after the fixes** (they reproduce on run1 for all three flavours):
- reviewed total: release 312 B, lab/harness 2,928 B (PASS, unchanged)
- raw bound: lab/harness **3,520 B** (was 3,512: `__ssvfiscanf_r` +8)
- strict bound without `-__smakebuf_r`: **4,056 B** (was 4,048)
- with no prunes at all: **4,944 B**

These raw and strict numbers supersede the ones above. `standby_note` is 408 B, as
the builder found.

### T16 `loop_lpsim`: issues found and fixed

- **The loop group is placement-blind under the placeholder model, and `report` used to
  exit 0 with `dL +0.0`.** In both twin runs the accepted layout's L row is 376/376 per
  frame (em_battle 768,920 of 768,920; aw2 2,261,640 of 2,261,640). A loop fetched once
  per frame never survives the frame's other ~4,300 misses. The release window
  (24,060 B) is also larger than the whole 16 KiB I-cache, so one sweep evicts itself.
  No placement can change L under this model.
  - `report` now prints `LP: NOT INFORMATIVE` when the accepted L misses are ≥ 99 % of
    its accesses, and the CLI exits **3** (not a pass).
  - Selftest M6.
- **A missing accepted layout passed.** A typo in `--accepted`, or a run without that
  row, printed no deltas and exited 0. It now fails (M5). A candidate without its
  `-noloop` twin or its old-plugin row also fails. The reproduction check now compares
  the totals as well as J/S/F.
- **Unaligned shifts dropped straddled lines.** `loop_fetches` added the shift to
  64 B line starts. With +160 B, every source line straddles two PSP lines, and only one
  was fetched. Byte addresses are now shifted before bucketing (M7: 376 → 377 lines on
  release).
- **Still open:** "reproduces the twin's current static-code misses" is shown only as
  old plugin == new plugin `-noloop` on one trace. That proves the copy does not perturb
  J/S/F. `~/lp/runs` holds no earlier sim of the 01067e74 ELFs to compare against, so
  no independent reproduction exists.
