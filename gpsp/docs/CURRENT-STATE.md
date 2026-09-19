# GBAdhoc: current state

Onboarding index. Every claim here is either verified against current source or
labelled as a hypothesis. Detailed experiments live in the linked records.

**Dated 2026-09-18.** If a statement here disagrees with the code, the code wins
— say so and fix this file.

## Commits and artifacts

| | |
| --- | --- |
| accepted hardware baseline | `7283f13ecd711396f8002cd7ef8f23f8e519a555`, checkout at `GBAdhoc/gpsp` |
| active development branch | `opus/performance-stability-fixes` in `builds/opus-performance-stability-worktree` |
| cold-decomposition checkpoint | `9f58f43` -- `main_psp.c` is close to exhausted of worthwhile cold boundaries; three measured and deferred, see `QUALITY-PROGRESS.md` |
| rollback artifact | `builds/unbound-stability-release-candidate-7283f13` — **do not modify** |
| hardware harness | `builds/opus-perf-harness` (campaigns, arm builds, analysis) |

Do not modify main/master, the accepted baseline checkout, the preserved
candidate, the outer GBAdhoc repository, or unrelated README/screenshot work.

## Build profiles

One command: `tools/build.sh [release|harness|diagnostic]`. Flag lists live
there and nowhere else; `gpsp_profile.h` rejects combinations that are silently
wrong.

Core flags are **shared by all three profiles** — a diagnostic build must be the
same emulator as the release or it answers a different question:

    SMC_GATES=1 SMC_GATES_SIMPLE=1 SMC_GATES_RANKED=1 SMC_GATE_BITMAP=1
    SMC_PARTIAL_SAFE=1 SMC_PARTIAL_STABLE_THUNK=1 SMC_PARTIAL_DIRECT_LINKS=1
    GBA_PC_MASK=1 BADJUMP_SAFE=1

| profile | adds to core | frontend defines |
| --- | --- | --- |
| release | `GPSP_PROFILE=release` | `-DGPSP_PLAYABLE -DVID_TRIPLE -DGPSP_PROFILE_RELEASE=1` |
| harness | `GPSP_PROFILE=harness` | adds `-DGPSP_KEEP_TELEMETRY -DGPSP_PERF_RIG` |
| diagnostic | `GPSP_PROFILE=diagnostic BADJUMP_REPORT=1 SMC_WRITE_HISTO=1` | `-DGPSP_PROFILE_DIAGNOSTIC=1` |

`BADJUMP_REPORT` and `SMC_WRITE_HISTO` write to the Memory Stick, which is why
`gpsp_profile.h` forbids them in a release. The release binary audit requires
`.playable-no-harness` and forbids `badjump.txt` / `smchisto.txt`.

## Verified performance findings

Hardware A/B, PSP-1000 and PSP-3000, against the `7283f13` control.

* **Dispatch islands were the regression.** Emitting a per-exit dispatch island
  cost +16.4%/+17.0% on `heart_soul_light` and +8.2%/+8.3% on
  `unbound_double_high`. Mechanism: translation-cache consumption → earlier full
  flushes → more retranslation (`xlat` 705 → 831, SMC events 33 → 51). Current
  code emits **no island**.
* **The cache-budget reservation did nothing.** `nobudget` landed on `dyn`.
  `UNMAPPABLE_ISLAND_BUDGET` was removed with the islands.
* **The extra SMC reset boundaries did nothing measurable.** The `fix` arm sat
  within noise of `dyn`. They were withdrawn because the evidence only justified
  ROM-load isolation.
* **`ff_psp.c` is cleared.** +0.1% to +0.2%, n=15, two consoles. Neutral within
  noise. Stop describing it as implicated.
* **Current implementation (`stub`)**: +0.14%/+0.12% on `heart_soul_light`.

## Unresolved

* **`unbound_double_high` residual, +3.5% to +3.65%**, reproducible on both
  consoles, independent of `ff_psp.c`. `BADJUMP_REPORT` file-I/O gating changed
  inlining and layout (`block_lookup_address_arm/thumb` 720 → 568 bytes,
  `badjump_recover` no longer a symbol), making layout a plausible common
  variable. **HYPOTHESIS — static code-size inspection does not establish
  causality.** The clean experiment is 7283f13 plus only that gating change.
* **PSP Go hard freeze: cause not demonstrated.** The shared stub is
  *containment* — an unresolvable translated exit can no longer jump into invalid
  native memory — but containment is not causality and does not establish correct
  guest behaviour afterwards. Do not claim islands or cache overflow caused it.
  Open until reproduced, or until long play testing makes release risk
  acceptable.

## Deferred

* **ME renderer visual divergence** — catalogued, not being fixed. 188 frames
  unmatched within ±8, identical frame numbers and hashes across both consoles;
  replicated on a second fixture (106 unmatched). Deterministic and
  content-dependent. The mechanism is a **hypothesis**: divergence consistent
  with per-scanline I/O capture combined with a single graphics-memory snapshot
  for the replayed frame. Which input is responsible is **not** established.
  See [ME-RENDERER-DIVERGENCE.md](ME-RENDERER-DIVERGENCE.md).

## Safety invariants that must not regress

1. **A translated block never contains a direct jump to an address the
   translator could not resolve.** Such an exit is patched directly to
   `bios_swi_entrypoint` and its `branch_source` cleared. Emits no code.
2. **An unmappable guest pc never reaches `jr $v0`** (`BADJUMP_SAFE`): soft-reset
   the GBA, not the console.
3. **Learned SMC state may not outlive the ROM that produced it.**
   `smc_gates_reset()` clears ranked candidates **at ROM load only**. Do not
   extend that boundary without new evidence.
4. `SMC_CAND_STALE_FRAMES` does not exist in active C code and is not a
   requirement of `SMC_GATES_RANKED`.

## Test and oracle boundaries

See [TEST-COVERAGE.md](TEST-COVERAGE.md). The two that most often mislead:

* `run_video_regress.sh` covers the **CPU renderer only**. PPSSPP never runs the
  second core, so it cannot reach ME mode, suspend/resume, or same-frame
  retirement. It was also a **no-op from the ADR-0067 rename until `d2857c1`**, so
  any earlier "verified identical output" claim is unsupported.
* PPSSPP is not performance evidence. It reported −14 µs where hardware showed
  +600 µs.

## Hardware

| device | drive | marker |
| --- | --- | --- |
| PSP-1000 | D: | `b620bc35-…` |
| PSP-3000 | F: | `4da06764-…` |
| PSP Go | G: | — (the freeze console; used for play testing) |

## ADRs

Status index: **[ADR-INDEX.md](ADR-INDEX.md)**, generated from the headings by
`tools/gen_adr_index.py`. Regenerate it rather than editing it.

`docs/DECISIONS.md` holds 46 numbers across 55 headings and stops at
**ADR-0053**, while active source and the outer HANDOVER cite ADR-0054..0082 —
those are not in this repository's records at all.

**Five** identifiers carry two primary records each: `ADR-0008` (a plain
duplicate-numbering mistake) and `ADR-0033`..`ADR-0036` (an imported
phase6-coreopt series overlapping the mainline numbering). They are aliased
`(mainline)` / `(phase6)` in the index and **not renumbered**, because active
source comments cite these numbers.

A heading of the form `ADR-NNNN addendum` is a continuation of the same record,
not a collision — frequently the place where that record's original conclusion
was corrected. `ADR-0043`..`0047`, `0051` and `0052` are absent from the file
entirely.

Most rows are marked `unreviewed`, which means nobody has checked them against
current behaviour — **not** that they are current.

Governing decisions most often cited by current code: ADR-0025 (only `gpsp_io`
touches the `.sav`), ADR-0033 (the video regression oracle), ADR-0067 and
ADR-0067b (the playable build: harness ini path made impossible, `fe_evt`
compiled out), ADR-0080d (the engine suspends around wireless bring-up).
