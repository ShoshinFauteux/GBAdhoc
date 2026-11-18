# GBAdhoc: where to start

This page used to be a dated snapshot (2026-09-18) of branches, flag lists and
findings. It went stale, so it is now a short index. Everything it pointed to
is below.

| you want | read |
| --- | --- |
| the rules for changing code safely | [`../../AGENTS.md`](../../AGENTS.md) |
| how the pieces fit together | [`ARCHITECTURE.md`](ARCHITECTURE.md), [`SUBSYSTEMS.md`](SUBSYSTEMS.md) |
| the six build profiles and the one build command | [`BUILD-PROFILES.md`](BUILD-PROFILES.md) |
| every dynarec switch and which are built in | [`BUILD-SWITCHES.md`](BUILD-SWITCHES.md) |
| how the code is tested, and what each test does not prove | [`TESTING.md`](TESTING.md), [`TEST-COVERAGE.md`](TEST-COVERAGE.md) |
| where the hot path sits in the I-cache, and the layout lock | [`LAYOUT-PINNING.md`](LAYOUT-PINNING.md) |
| the frontend's fragment map | [`../psp/fe/README.md`](../psp/fe/README.md) (generated) |
| numbered decisions | [`DECISIONS.md`](DECISIONS.md) (ADR-0001..0053), [`adr/`](adr/README.md) (ADR-0043 and later), [`ADR-INDEX.md`](ADR-INDEX.md) |
| design plans that code comments cite | [`design/`](design/README.md) |
| known limitations, as players see them | the root README, "Status and known limitations" |

## Safety invariants that must not regress

These were on the old page and still hold (checked 2026-11-17):

1. **A translated block never jumps directly to an address the translator
   could not resolve.** Such an exit is patched to `bios_swi_entrypoint` and
   nothing is emitted (`cpu_threaded.c`, the comment above the patch loop).
2. **An unmappable guest pc never reaches `jr $v0`** (`BADJUMP_SAFE`, built
   in): the GBA soft-resets, not the console.
3. **Learned SMC gate state does not outlive the game that produced it.**
   `smc_gates_reset()` runs at ROM load (`gba_memory.c`, after the cartridge's
   fixed gates are set), at the in-game "Reset game" (`gba_reset_power_on`)
   and at a link session's cold start (`lk_cold_start`, `gbalink_core.c`). It
   does not run at a cache flush or a save-state load; see the comment in
   `cpu_threaded.c` beside `flush_dynarec_caches` before extending it.

## Test boundaries that most often mislead

* `tools/e2e/run_video_regress.sh` covers the **CPU renderer only**. PPSSPP
  never runs the second CPU (the Media Engine), so it cannot reach the ME
  renderer, suspend/resume, or same-frame retirement.
* PPSSPP is not performance evidence. Speed is measured on real consoles.
