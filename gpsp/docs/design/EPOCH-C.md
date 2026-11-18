# Epoch C: one exit sequence, a readable main()

Epoch C was one step of the PSP frontend restructure ([RESTRUCTURE.md](RESTRUCTURE.md)). It took
`main()` apart into a boot function, a session made of phase functions, and **one exit sequence**.
The original plan is not kept in the repo; this page replaces it. Code comments that cite
"PLAN-EPOCHC-beta5 §N" or a step id such as "C3-2b" mean the matching heading below.

**Terms.**
- **Translation unit (TU):** one `.c` file as the compiler sees it after its `#include`s. The
  frontend is one TU: `gpsp/psp/main_psp.c` is a short **shell** that includes the **fragments**
  `gpsp/psp/fe/*.inc.c` in order ([../../psp/fe/README.md](../../psp/fe/README.md)).
- **Exit sequence / teardown:** the ordered steps that stop everything before the process ends:
  the Media Engine (ME, the PSP's second CPU), the save writer thread, the core and its save,
  audio (AUD), display (VID), the event log (EVT; `evt_shutdown` closes it, "ES"), the
  **handoff** (HO: the memory stick goes back to a test PC) and the exit to the PSP's system menu
  (the XMB, `FE_EXIT_GAME`). **Power hold:** `pwr_hold`/`pwr_release` keep the console awake.
- **In-process return:** "Quit to game list" without restarting; a heap **census** can refuse it,
  and the program then relaunches itself ([../INPROC-RETURN.md](../INPROC-RETURN.md)).
- **Flavours:** one source built as *release* (for players), *lab* (release plus test-Lab hooks)
  and *harness* (test builds driven by an ini file of test keys); *release-stable* existed then
  and has since been removed. An **`-E` view** is a flavour's preprocessed source
  (`psp-gcc -E`); the **ELF** is the linked program the tools measure.
- **`noinline` / section pinning:** `noinline` stops GCC pasting a function into its caller;
  `section(".text.x")` names the input section the linker places it by. The hot frame loop is
  placed on purpose with padding sections ("pads", [../LAYOUT-PINNING.md](../LAYOUT-PINNING.md)),
  so code that moves in front of it shifts the loop.

**§0 Decisions.** Scope: C1 + C2 + C3 and one final re-pin; C4 (regroup fragments) and C5
(cold-code harvests) deferred to after 5.0.0. Only class-I differences (§2.3) were to be
normalised; log-only and player-visible differences are kept exactly.

## §1 The problem

Before Epoch C, `main()` was about 4,550 lines in the shell: boot, selection, loading, network
bring-up, the frame loop and the teardown; the game-list return jumped back with `goto session_top`.

### §1.2 Inventory: about 45 endings

`tools/refactor/epochc_exitmap.py` lists every ending. They fall in four families:
- **A** (A1-A5): boot probes (test patterns, cache probes, a network test). AV-first:
  `AUD VID EVT ES HO EXIT`.
- **B** (B1-B10): at game selection (bad path, missing ROM, HOME or nothing picked in the list;
  B8 relaunches instead of handing off). EVT-first: `EVT ES AUD VID HO EXIT`.
- **C** (C1-C4): after selection, before the loop (`mem_budget`, `load_failed`, `bad_script`,
  `net_failed`). C1/C2 EVT-first; C3/C4 CORE-first (ME, writer, core, then the AV-first tail).
- **D**: 21 triggers inside the frame loop (HOME, menu Exit, START+SELECT, quit to list, profile
  change, failed reset, Lab and harness ends), all falling into one shared teardown.

20 of these were **pasted copies** of the exit sequence, in three orders. Exits outside the TU
(the Lab build's boot in `lab_harness.c`, the handoff's own end in `usb_handoff.c`) never release
the power hold, so boot must keep `init_paths` ahead of every `pwr_hold`. Family D's shared
teardown has steps D0-D20 (session end), I1-I5 (in-process return, unload U1-U12) and P1-P13
(process end): today the bodies of `session_end`, `inproc_try_return` and `process_end` (§3 C1).

## §2 The exit model

**§2.1 One sequence.** Every ending is a subset of `[site log line] [notice] session_end ->
inproc_try_return (game list only) -> process_end`, chosen by flags. Early exits (A-C) run their
subset before their own log line, then the tail `ES [AUD VID] HO EXIT`.

**§2.2 The check (T8, gate "XS").** `tools/refactor/exit_matrix.py` builds `exit_sites.tsv`, `exit_steps.tsv` and `exit_scenarios.tsv`
per flavour from the `-E` views. Its `check` command (the XS gate) compares a tree with a
**golden** taken from the tree Epoch C started from: per site and scenario, the ordered calls,
exit code and reason, the **text** of every call passing a string literal (log lines the Lab
parses) and the **power hold** a site exits with. Any difference fails unless an allow-list names
it. `tools/refactor/tests/test_exit_model.c` replays the teardown against the same expectations.

**§2.3 Deviations.** Classes: **I** invisible (normalise), **L** log-only, **V** player-visible, **K** deliberate (all
three kept). The one class-I item, N1, was EVT-first vs AV-first in B1-B10, C1 and C2. Kept on
purpose: C2 skips `fe_host_shutdown` so an unreadable save is not overwritten; B8's relaunch has
no handoff; the two separate `inproc_refusal` calls; the notice lengths.

## §3 Steps

Every commit was proven against the golden with an **empty allow-list**, plus byte comparisons of
the flavours (`tools/refactor/flavours.sh`): **G1** = every loaded section identical, **G2** =
differences confined to the named functions. Commit ids below resolve with `git show`.

| Step | What | Commit |
|---|---|---|
| C0, C0a | Tools: T8 on the fragment tree, text and hold columns (C0a-5, parts a-c), the section probe (C0a-6), rule R6, host tests of six player exits | `ae09a127` and neighbours |
| C0b | Tools merged on the real start tree; golden and exit map taken there | `f9243b87` |
| C1a, C1b, C1c (stage s) | Families A, B+C, D through the helpers, `always_inline` | `f981b593`, `47a04d76`, `b13ba89d` |
| C1 stage o | The four helpers out of line, each pinned | `ea0e9651` |
| C2 | Boot is `boot_run()` | `39c5247f` |
| C3-1 | The return's reset is `session_reset()` | `54f709ce` |
| C3-2a .. C3-2e | `inproc_unload`; `for (;;)` + `inproc_try_return`; `session_select`; `session_configure` + `session_load`; `session_net_up` + `session_pre_loop` | `ea9f278a`, `0561ae35`, `a150766e`, `59bcc400`, `e68a8b48` |
| C-pin | One re-pin after the merge | merge `eae51bba`, re-pin `9c680e61` |

### §3 C0 Tooling and baseline

C0a made the exit tools work on the fragment tree and added what Epoch C needed (details in
[abi-exit-ci.md](../../tools/refactor/docs/abi-exit-ci.md)). C0b took the golden on the start
tree, so every later step is compared with the behaviour before Epoch C.

### §3 C1 The one exit sequence

`gpsp/psp/fe/teardown.inc.c` holds it:
- `exit_early_begin(f)` runs what goes before a site's own `exit code=` log line ([ME] [CORE]
  [AUD VID]); `exit_early_end(f, code, why, pass)` runs the rest (ES [AUD VID], HO or relaunch,
  EXIT). The flags `XE_EVT_FIRST`, `XE_ME`, `XE_CORE`, `XE_RELAUNCH`, `XE_HO_PASS` are constants
  at each call, so every site keeps its order. The plan named one `exit_early(x)`; it became a
  pair because the site's log line, untouched, sits between the halves.
- `session_end(have_variant)`, in order: wake yield off, sleep quiesce undone,
  `pwr_hold("teardown")`, play lock dropped, (game list: backdrop and return hold), stall watch,
  HOME host, profiler, ME renderer and video, ME host shut down or kept for the return, ME stage
  buffers, network, Mystery Gift, writer thread, settings flush, the `evt_drop` line,
  `fe_host_shutdown`, rewind pool.
- `process_end(exit_code, exit_reason)`: audio, video, `audio_hash` (not on a return), clock and
  health lines, the `exit code=` line, `evt_shutdown`, `FE_HANDOFF`, the relaunch if one was
  asked for (HOME cancels it), `FE_EXIT_GAME`.

**Stage s, stage o.** Each family first moved as `always_inline` helpers (stage s: same
structure, ideally same bytes; release was G1-identical for C1a), then all four helpers went out
of line in one commit (stage o, the layout event). They are not `noreturn`: `sceKernelExitGame`
is not declared so, and a failed exit must still fall through to `main()`'s `return 0`. The tools
read each call as the helper's body with its flags folded (`aec_cparse.inline_helpers`,
`exit_matrix.EPOCHC_HELPERS`), so the golden applies unchanged.

### §3 C1n Normalisation (not done)

Planned: flip B1-B10, C1 and C2 to AV-first (N1). It did not land; those sites still pass
`XE_EVT_FIRST` in `fe/session.inc.c`. The planned harness knob to force a failed reset (C1c-x)
was dropped too; that path is covered by the host test `tools/tests/test_exit_paths.py` (X-6).

### §3 C2 Boot as a function

`gpsp/psp/fe/boot.inc.c`: `boot_run(argc, argv, boot_ctx *)` is `main()`'s boot moved token for
token. It returns -1 when done; family A's exits stay inside it. Boot order is unchanged and
`init_paths` still runs before any power hold. Values the session needs come back in `boot_ctx`;
the 8 KiB CONFIG.INI document lives only in boot's stack frame. The commit measured `main()`'s
release stack frame going from 12,464 to 4,192 B.

### §3 C3 The session

`gpsp/psp/fe/session.inc.c`. C3-1 moved the per-game reset into `session_reset` (tier C later
renamed it `loop_reset`, now in `fe/loop.inc.c`; see [TIER-C.md](TIER-C.md)). C3-2a moved the
unload into `inproc_unload`. **C3-2b** replaced `goto session_top` with one pass of `for (;;)` per
game ending in `if (!inproc_try_return(..)) break;` (planned under C1c, it needed the loop).
C3-2c to C3-2e moved the phases into `session_select`, `session_configure`, `session_load`,
`session_net_up`, `session_pre_loop`. C3-3: the hot loop's locals stayed `main()`'s. The tools read
the loop as the old goto (`aec_cparse.unloop_session`), moved code as `main()`'s (`splice_moved`).

## §4 Layout and re-pin

**§4.1** No Epoch C commit touched a pad; placement was reported per commit, fixed once at C-pin.

### §4.2 Keep main() the only thing in front of the loop

The C0a-6 probe (`tools/refactor/epochc_probe.sh` and `.py`, GCC 15.2.0, release and lab) added
test functions that only `main()` calls and read where they landed:

| Declared | Lands | Effect on `main` |
|---|---|---|
| `noinline`, no section | after `layout_pad_cold` | none today, but GCC decides |
| `noinline, section(".text.startup.x")` | between `layout_pad_main` and `main` | moves `main` |
| `noinline, cold`, no section | in front of `layout_pad_main` | moves the pad and `main` |
| `noinline, section(".text.fe_cold.x")` | after `layout_pad_cold` | none |
| `static`, one caller, no `noinline` | inlined into `main` | grows `main` |

**Rule:** every function Epoch C adds that `main()` calls is
`static __attribute__((noinline, section(".text.fe_cold.<name>")))`; all twelve new functions in
`teardown.inc.c`, `boot.inc.c` and `session.inc.c` are. **R6** in `tools/refactor/ci_greps.py`
enforces it: a static function only `main()` calls needs both attributes (or `always_inline`,
stage s) unless `ci_greps.r6_pinned` lists it as older than Epoch C.

### §4.3 The re-pin (C-pin)

`9c680e61`: `main` shrank from 34,380 to 23,372 B (release). The MAIN pad was chosen by scanning
every 4-byte shift of `main` for the best cache-set match of the loop, and the other pads were
re-solved ([../LAYOUT-PINNING.md](../LAYOUT-PINNING.md)). **§4.4** The plan's fallback if the
loop lost speed (second-best shift, then bisect kits K1 = C1, K2 = C1+C2, K3 = all, and ship
less of Epoch C) is not recorded in the repo as having been used.

## Landed, not done, deferred

Landed: C0a, C0b, C1a-c (stage s and o), C2, C3-1, C3-2a-e, C-pin. Not done: C1n, C1c-x.
Deferred past 5.0.0: C4, C5. Tier C later moved the frame loop itself out of `main()`
(`frame_loop`) and placed `main` in `.text.fe_cold.main` ([TIER-C.md](TIER-C.md)). Related: [RESTRUCTURE.md](RESTRUCTURE.md), [TIER-C.md](TIER-C.md),
[../../tools/refactor/docs/abi-exit-ci.md](../../tools/refactor/docs/abi-exit-ci.md).
