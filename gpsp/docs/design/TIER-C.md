# Tier C: code-health designs C-0 to C-6

Tier C was a set of seven code-health designs for the PSP frontend and its tooling. The original
design notes are not kept in the repo; this page replaces them. Comments in the tree cite them as
`C-5.md §2.1`, `C-2.md §2.4 check c`, `CRITIQUE C-1`, `C5-0` and so on: each has a heading or a
named item below. A sub-step id such as `C4-2` means "design C-4, commit 2".
Related: [RESTRUCTURE.md](RESTRUCTURE.md), [EPOCH-C.md](EPOCH-C.md),
[the frontend README](../../psp/fe/README.md), [the refactor tools](../../tools/refactor/README.md),
[LAYOUT-PINNING.md](../LAYOUT-PINNING.md).

**Terms.** A *translation unit (TU)* is one C file as the compiler sees it after `#include`s; the
PSP frontend is one TU (`psp/main_psp.c`, a shell that includes `psp/fe/*.inc.c`). The *frame loop*
runs once per emulated frame. *Harness ini keys* are settings in a test-only `.gpsp-harness.ini`
that the Lab (the project's test rig) writes; players never have it. *Telemetry* is counters that
only feed the `EVT` log lines the Lab parses. *ABI* here means the names and types the core exports
to the frontend. A *rename map* lists reviewed `old_global -> struct.field` renames. A *gate* is a
pass/fail check a commit must clear ("EBOOT=": every binary byte-identical). *Player builds* are
release and diagnostic; *Lab/rig builds* define `GPSP_PERF_RIG` and keep telemetry.

## C-0

**Problem.** A ROM page read failure (Memory Stick pulled) paused the game once; a second failure
after Resume never paused, and two frontend latches outlived the game.
**Landed (C0-1).** The core flag `gamepak_read_failed` is three-state (0 good, 1 failed and not
reported, 2 reported), owned by one poll: `gamepak_read_poll()` in [gba_memory.c](../../gba_memory.c),
called through `fe_host_rom_read_poll()` ([fe_host.c](../../frontend-common/fe_host.c)) once per frame
boundary in [loop.inc.c](../../psp/fe/loop.inc.c). Both latches are gone. Host test:
[test_gamepak_read_fail.c](../../tools/tests/test_gamepak_read_fail.c) step 8.
**Dropped.** C0-2, a Lab fixture that renamed the ROM mid-game (`rom_unplug`), landed and was
reverted: the project dropped that scenario.

## C-1

**Problem.** The frame loop was `main()` (about 23.5 KB), so edits to boot code moved the loop's
cache placement, and main's 51 locals hid which ones were loop state.
**Landed.** The loop is `frame_loop(loop_t *L, const session_ctx *S)` in
[psp/fe/loop.inc.c](../../psp/fe/loop.inc.c), called once per game; `main()` is cold glue
([main_psp.c](../../psp/main_psp.c)). `loop_t` holds what the loop writes, `session_ctx` what it
only reads; `loop_reset()` replaced the 13-parameter `session_reset`.

- **Decision 1:** one call per game, not one per frame; the loop's `continue`s and `break`s stay.
- **Decision 2:** sections. `frame_loop` is `noinline, noclone` in `.text.startup.fe_frame_loop`,
  main_psp.o's only `.text.startup` input, so the linker can pin it; `main` is in
  `.text.fe_cold.main`. `noclone` keeps GCC from renaming it (`.isra`, `.constprop`).
- **Decision 3** (cited as "C-1.md 2.3"): copy-in and copy-out, so the moved text keeps its local
  names; read-only copies are `const`. Sound because no loop local has its address taken.
- **Decision 4:** the six one-shot harness keys in `loop_t` are sticky across in-process returns.
- **Commit 1** (C1-1): the tools learn the new shape (`loop_anchor.py` function mode, the exit
  map, `loop_diff`, ...), byte-identical; [c1_frame_loop_test.py](../../tools/refactor/tests/c1_frame_loop_test.py).
- **Commits 2-3** (the body as four in-function fragments): **dropped** (CRITIQUE C-1).
- **Commit 4** (C1-4): the one binary change. `pin main` became `pin frame_loop`
  ([layout.lock](../../tools/layout/layout.lock)), placed once by a by-line scan of cache sets.
  "T13" is its inline census: callees main's loop called out of line are marked `noinline`
  (a few `always_inline`) so GCC's inlining did not change.
- **Commit 5** (C1-5): `main` keeps `boot_ctx bc` and `loop_t L`; `loop_reset(&L, &bc, &slot)`.
  [test_session_reset.py](../../tools/tests/test_session_reset.py) checks each `loop_t` field.
- **Commit 6** (C1-6): the function moved whole into `fe/loop.inc.c`, byte-identical.

### C-1x note
C-1x was an optional per-iteration split (`frame_iter` plus a cold `frame_iter_rare`). A static
count on the release ELF (cache lines shared by common and rare code, at 215 ns per miss) bounded
the gain at about 0.016 ms per frame, roughly 0.1 % of a 16.7 ms frame, against an added iteration
protocol and another inline census. **Dropped, not built.**

## C-2

**Problem.** About 200 harness-ini reads were compiled into every player build, returning their
defaults at run time; the key list existed only as greps.

### §2.4 The key table and its checks
[harness_keys.py](../../tools/refactor/harness_keys.py) writes
[generated/harness_keys.tsv](../../tools/refactor/generated/harness_keys.tsv) (key, getter,
default, site, flavours) from each flavour's preprocessed source, and checks:
- **check a:** every default is side-effect free (no call, assignment, `++`/`--`);
- **check b:** a key's default text is the same in every flavour that compiles it, so a player
  build equals a harness build without the key (ADR-0067's guarantee, as a check);
- **check c:** no raw `fe_ini_get*(HARNESS_INI|p_harness ...)` outside
  [fe_priv.h](../../psp/fe/fe_priv.h): `ci_greps.py` rule **R7**, with one exception in
  `ci_greps.known` (the frame loop's `rewind_gedump` read).

**Landed.**
- C2-0: the table, the checks, R7. C2-1: every read goes through `HARNESS_INT`/`HARNESS_STR`,
  byte-identical. C2-2: the four player functions (`psp_state_load`, `psp_game_reset`, ...) moved
  from `fe/harness.f.inc.c` to `fe/misc.c.inc.c`.
- C2-3: `HARNESS_FOLD` (derived from `GPSP_PERF_RIG`, never set by hand) makes each read its
  default in a player build; `HARNESS_KEEP_WRITTEN` keeps two folded knobs as stores (frame parity).
- **C2-4'** replaced C2-4: instead of turning rejected knobs into constants (`HARNESS_KNOB`, dropped
  per CRITIQUE C-2 point 1), 16 rejected-experiment keys and their code were deleted in every build
  (`me_shadow`, `me_sameframe`, `emu_prio_boost_us`, `dump_marker_poll`, `vram_probe`,
  `gblink_input_copies`, ...). Parked or still-used keys stayed (`rom_vmem`, `jit_small`,
  `core_phase`, `me_midlog`, `disc_await`, ...).
- C2-5: the cold units `fe/harness.c` and `fe/harness.e` are telemetry-only. **Reversed:** the rest
  of C2-5 (no autopilot, no harness path, no per-frame autopilot gate in a player build), undone for
  frame parity.

## C-3

**Problem.** I-cache placement was held by 11 hand-tuned zero-byte pads plus about 25 hold
sections; every size change upstream needed a manual re-pin. **Landed (C3-1 to C3-6, all byte-identical).** One link script, [gbadhoc.ld.in](../../psp/gbadhoc.ld.in)
(psp-gcc's default script verbatim, checked by psp/Makefile on every link, plus marked blocks);
object groups (C3-2, [groups_ld.sh](../../tools/layout/groups_ld.sh)); the text pads became linker
**pins** read from `layout.lock` (C3-3: `frame_loop`, `fe_head`, `fe_common`, `fe_tail`, `core`, per
profile); `pin ro` for `.rodata` (C3-4); the hot order's holds became row pins (C3-5, `@pin` rows in
`psp/layout/hot_order.txt`); [repin_rehearsal.sh](../../tools/layout/repin_rehearsal.sh) and
`layout_lock.py stats` (C3-6).
- **Overrun policy** ("critique C-3 point 1"): a pin that would wrap fails a strict link; only
  `layout_lock.py regen` moves a pin, and the commit says `--spend-page` or `--move`.
- **Differs from the design:** `cold_tail.ld` and `hot_data_guard.ld` stay INSERT scripts; the
  `.data`/`.bss` holds and intra-object holds (`DC_STUB_FILL`, the pcarg pad, `sched_hot_hold_text`)
  stay, since no final-link pin reaches inside an object. Details: LAYOUT-PINNING.md "Pins".

### C3-7 note
C3-7 would have fixed the hot block's size and moved per-frame frontend functions (`pace_frame`,
`net_frame`, `fe_host_reset`) into it. Code that runs once per frame is evicted from the 16 KiB
I-cache every frame wherever it sits, so placement only changes which emulator lines it evicts: a
static bound of about 0.07 ms per frame. Three earlier commits that moved only per-frame frontend
code each got a Lab ABBA run on a PSP-1000 and a PSP-3000 (November 2026, work ms per frame) and
stayed within about ±0.6 % on GBA fixtures. **Dropped, not built.** Kept: the GB core has no pin of
its own and moves with frontend size changes, so A/B runs of frontend changes include a GB fixture.

## C-4

**Problem.** Two Wireless Adapter protocols shipped, chosen by the version string; the stable tag
would have built the arm no beta had tested.
**Landed.** Scheduled ticks are the only protocol ([RFU-COUPLING.md](../RFU-COUPLING.md) "Status").
- C4-1: `RFU_SCHED` is no build switch; `tools/release.sh` has no version branch;
  `tools/build.sh` accepts `RFU_SCHED=1` as a no-op. Byte-identical.
- C4-2: frontends stop configuring the old rules; the desktop build defaults to D = 2.
- C4-3a / C4-3b: supersede, frame pace, max hold and catch-up, link shedding and hold-never-discard
  deleted from [rfu.c](../../rfu.c), with their keys and trace events.
- C4-4 (hand re-pin): not needed, C-3 had landed; the lock only accepted moved anchors.
- C4-5: docs, rig scripts, and the protocol's first unit test, [test_rfu_sched.c](../../tools/tests/test_rfu_sched.c).
  The matching Lab key-list change lives outside this repo.

## C-5

**Problem.** 483 file-scope globals in the frontend TU, 100 written only to feed telemetry, many
defined in another module's fragment with no visible owner.

### §2.1 Storage kinds
Per module X: telemetry `struct fe_X_tel X_tel`, state `struct fe_X X`, and defaults `struct
fe_X_cfg X_cfg` (only objects with an initializer, so nothing moves between `.bss` and `.data`).
Telemetry stores are wrapped in `TEL(...)` ([fe_priv.h](../../psp/fe/fe_priv.h)). §2.2 lists what stays
standalone ([globals.standalone](../../tools/refactor/globals.standalone)).

### §2.3 Module grouping
Objects grouped by module and name family; landed renames: [maps/c5-1-pacing-tel.map](../../tools/refactor/maps/c5-1-pacing-tel.map).

### §3 Gates
**G0R:** a batch must be a pure rename. `pp_diff.sh --rename-map MAP` hands each flavour's two
preprocessed token streams to [g0r.py](../../tools/refactor/g0r.py), which maps new names back and
compares (`--tel` also unwraps `TEL(...)`); statements whose right side calls a function are listed
for review (§4).
- **C5-0** (tools, landed): `globals.py struct` ([gstruct.py](../../tools/refactor/gstruct.py)),
  `census --module X --check`, G0R, with self-tests.
- **C5-1** (landed): `struct fe_pacing_tel pacing_tel`; `TEL()` replaced the older `FFW_*` macros.
- **C5-2** (landed): `merend_tel`, `audio_tel`, `input_tel`, `power_tel`.
- **C5-3**, the pilot (cold state structs): built on a side branch, **not landed**. It passed its
  gates with no measured benefit, made every batch a layout event and put run-time state in `*_cfg`
  structs. **C5-4 to C5-10 were not continued.**

**Changed after landing:** the design compiled telemetry out of player builds; at HEAD `TEL(...)`
runs in every build (frame parity), and only the `EVT` output is compiled out (ADR-0067b).

## C-6

### §1 The problem
The frontend TU declared core names by hand ("copies", about 120 sites); release had 39 that the
ABI check T14 could not verify, plus two real type mismatches (`gamepak_must_swap`,
`jit_code_stats`). Including `common.h` was rejected: core headers change shape with defines only
the core's build passes. Non-ABI names (linker symbols, an SDK function its header lacks) are listed
in [abi_asserts.platform](../../tools/refactor/abi_asserts.platform).

### §2 Target design
One define-free header, [core_fe.h](../../core_fe.h): the core's exports in the core's types.
`common.h` includes it, so every core definition compiles against it; the frontend includes it
instead of copying. A name only some builds define is declared plainly, and the frontend adds
`#pragma weak` beside a use that tests its address.

### §5 Risks
The main risk: core_fe.h growing a core-only `#if` or a `section`/`weak` attribute. Rule **R8**
`core_fe_pure` in [ci_greps.py](../../tools/refactor/ci_greps.py) fails that (mutant self-test).
- **C6-1** (landed): T14's platform class, R8, and a ratchet
  ([abi_asserts.ratchet](../../tools/refactor/abi_asserts.ratchet), `flavours_gate.py ratchet`).
- C6-2 to C6-6 (landed): core_fe.h, then the copies deleted; C6-4 made `gamepak_must_swap` `bool`;
  C6-6 removed the lab/harness copies and needless weak tests.
- **C6-7** (landed): T14 is a guard; [abi_asserts.py](../../tools/refactor/abi_asserts.py) fails a copy
  of any name core_fe.h declares. The pin file went.

At HEAD: 0 unchecked copies in every flavour; 3 asserted copies remain (translation-cache names whose
type `cpu.h` decides per port). Dropped: a libretro/3DS compile check. Other TUs were out of scope.

## Frame parity

After C2-3, C2-5, C5-1 and C5-2 landed, the project decided that every function that runs per frame
must be the same code in the player build and the Lab build: the Lab runs only Lab builds, and its
speed and picture-hash results must still prove the shipped build. Those commits' per-frame parts
were reversed or patched (see each section). The gate is
[frame_parity.py](../../tools/layout/frame_parity.py) (gate FP in `flavours.sh`); see
LAYOUT-PINNING.md "Frame parity".

## Critique and decisions

A review of all seven designs against the source changed several plans before work started.
**CRITIQUE C-0:** the proofs missed the second core instance and the test twin; not urgent for
5.0.0, so it landed after C-3 and C-1. **CRITIQUE C-1:** drop the in-function fragments (commits
2-3) and move the whole function later. **CRITIQUE C-2:** point 1, delete rejected knobs instead of
`HARNESS_KNOB`; point 2, freed bytes become heap only when a page boundary is crossed.
**CRITIQUE C-3:** point 1, add the overrun policy; point 2, defer C3-7. **CRITIQUE C-4:** fix the
release script first; deleting supersede changes D = 0 ports. **CRITIQUE C-5:** telemetry first
(C5-0 to C5-2), pilot C5-3, defer the rest; fold `FFW_*` into `TEL()`. **CRITIQUE C-6:** sound; one
wrong line cite; a ports compile check (later dropped).

**Decisions.** 1: PSP only (desktop and twin stay compilable as test tools). 2: multiplayer is two
consoles; three or more in one adapter room is unsupported (closes C-4 risk R6). 3: speed
experiments C3-7 and C-1x were to be measured; both were then dropped on their notes. 4: C-5 pilot,
then decide (not continued). 5: weak-symbol address tests go with C6-4/C6-6. 6: rejected harness
options are deleted in every build (C2-4').
