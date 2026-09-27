# BUILD SWITCHES — the inventory

Every dynarec and diagnostic switch in the tree, with a verdict. 27 in the
top-level Makefile plus 6 frontend defines.

**Why this exists.** Nine of these ship. Five are experiments that were tried
and rejected, with the measurement that rejected them. Several have no recorded
verdict anywhere — not in `DECISIONS.md`, not in the Makefile — so the only way
to know what they were for was to read `cpu_threaded.c`. A flag with no verdict
is worse than no flag: it looks like an option.

Two defects found while compiling this list and fixed in the same commit:

* Four comment blocks (`BADJUMP_REPORT`, `GBA_PC_MASK`, `SMC_GATES_CODED`,
  `SMC_GATES_SNAP`) had drifted away from their own `ifeq` guards and were
  stacked above `SMC_GATES_SNAPF`'s, so reading the Makefile top to bottom
  attributed each explanation to the wrong flag. `SMC_SCAN_SPCLAMP`'s comment
  sat above `SMC_WRITE_HISTO`'s guard the same way.
* `SMC_GATES_RANKED`'s comment described an `SMC_GATE_PROMOTE_HITS` threshold
  (default 256) **that the code does not have**. Frequency thresholds were tried
  and failed twice; the shipped rule is sample count *plus* a changed-word ratio
  *plus* a staleness bound. Anyone tuning "256" would have been tuning nothing.

---

## Shipped in every profile

These are the flags `tools/build.sh` passes for release, harness and diagnostic
alike. Changing one changes emulation.

| switch | what it does |
|---|---|
| `SMC_GATES` | put a translation gate at a self-modified address so later blocks split there. Deliberately independent of `SMC_PARTIAL*`: gates bought Unbound its speed and also broke its audio, so the two must be testable apart. |
| `SMC_GATES_SIMPLE` | the 2.0.1 add-only gate rule. Once placed, a gate never moves, so an already-translated block can never disagree with the table. 2.0.2's evicting rule is what crashed Heart & Soul. |
| `SMC_GATES_RANKED` | promote after 64 observed writes when at least half changed the word. Counts are lifetime within one ROM: there is no active staleness window, so sound-mode changes do not reset the evidence. See below. |
| `SMC_GATE_BITMAP` | same gate table and boundaries, tested through a bitmap instead of comparing every live gate per compiled instruction. |
| `SMC_PARTIAL_SAFE` | after a reported SMC trap, selectively retires ranges for a bounded ARM STMIA/LR shape; this M4A-motivated heuristic is not ROM-specific. Other reported writes use the full flush. Block-store emitters check only the final word, so an earlier code write can escape detection if that final word is untagged. |
| `SMC_PARTIAL_STABLE_THUNK` | a permanent per-block RAM entry thunk, so retiring a block redirects the thunk rather than orphaning inbound links. |
| `SMC_PARTIAL_DIRECT_LINKS` | keep RAM→RAM links direct; route only persistent ROM→RAM links through an island. Requires the stable thunk. |
| `GBA_PC_MASK` | mask the emulated pc to 28 bits before the region lookup, as the GBA does. **Not sufficient alone** — a masked garbage target lands in a region the translator *does* handle. |
| `BADJUMP_SAFE` | an unmappable guest pc soft-resets the GBA instead of `jr`-ing to it. Cannot regress: the path it replaces crashes 100% of the time. |
| `ROM_BUFFER_SIZE=15` | the PSP default ROM page-cache cap (root Makefile; `ROM_BUFFER_SIZE=N` on the root make overrides it). Since 2026-09-26 it is only the *default* of the runtime `gamepak_buffer_cap`: CONFIG.INI `rom_resident = 1` (default since 3.0.0) raises the cap to the cart's size when every block can be taken while the 1 MiB post-load reserve is held (spare-pool blocks lent by the LARGE JIT tier first); otherwise the extra blocks go back and the cart pages (64 MiB consoles only; a 1000's loop always stops at ~13). The 2026-09-16 PSP-3000 Unbound failure that motivated the cap is **hardware-only**: the exact failing binary is bit-identical resident vs paged in PPSSPP. See `docs/ROM-RESIDENCY.md` before changing the default. |
| `SMC_GATE_CHARGE` | (3.0) a block that ends at a translation gate subtracts its pending cycles before the gate's indirect exit. Upstream's gate exit charges nothing, so code in front of a gate ran for free; the H&S mixer's low endpoint gate made the mixer loop partly free, which moved timer/sound-DMA timing (the PPSSPP full/control audio split). Only `SMC_SCAN_GATE` blocks change, so games that never earn a gate emit identical code. Revert for an A/B with `SMC_GATE_CHARGE=0`/unset. No hardware verdict yet. |
| `DISPATCH_CYCLE_CHECK` | (3.0) `mips_indirect_branch_arm`/`_thumb` call `mips_update_gba` when the cycle budget is spent, as direct branches already do. Without it a guest loop closed only by indirect branches or gate exits never completes a frame and the main thread spins forever (no crash, no badjump.txt, log stops). Two instructions per indirect branch; changes IRQ/timer delivery points, so audio/video hashes move for every game. `_dual` (BX) is deliberately not checked. No hardware verdict yet. |
| `DMA_SMC_FLUSH` | (3.0) an HBlank/VBlank DMA whose destination is tagged as translated code now full-flushes the RAM translation cache and re-dispatches (`changed_pc`), as the CPU-triggered DMA path in `write_io_epilogue` always did; upstream `update_gba` discarded the alert, leaving stale translations live. One OR and one test per DMA. Perf risk: a game that DMAs every frame into a tagged region gets a full flush per frame — watch `dma_flush=` in the soak `core_health` line (`dma_smc_flushes`). No hardware verdict yet. |

**Open after 3.0 (known, not done):** an SMC coverage probe for the unchecked
words of block stores (P4 in the 2026-09-24 dynarec audit); a cycle check in
`mips_indirect_branch_dual` (BX-closed loops can still spin without a frame —
it needs the T-bit settled before an IRQ can be taken); and the I/O alerts that
non-final STM words drop (`mips_emit.h`, `strop == 3` in the I/O store stub).

**Current M4A range-gate caveat:** `smc_partial_install_range_gate()` inserts
its computed endpoints directly. It bypasses `SMC_GATE_EARNED` and the normal
gate filter, and those endpoints persist across soundtrack changes until a new
ROM loads. Selective retirement now requires both normalized endpoints to be
present; if gate-table capacity leaves either one missing, the operation falls
back to a full RAM-cache flush. This closes a readiness-check gap, but there is
no hardware evidence that the old half-installed case caused a freeze, or that
a Heart & Soul soundtrack switch installs an invalid endpoint.

### What `SMC_GATES_RANKED` actually requires

All three, in `cpu_threaded.c`'s `smc_gate_earned`:

```
SMC_GATE_MIN_SAMPLE    64 writes of evidence
chg * 2 >= hits        at least half of them CHANGED the word
```

Two conditions, not three.  A staleness rule (`SMC_CAND_STALE_FRAMES`,
"the evidence is from one continuous episode") was proposed and
**withdrawn**; it is not present in `cpu_threaded.c` and is not a
requirement of `SMC_GATES_RANKED`.

Measured on the Heart & Soul rival battle, over one 8000-flush window:

| address | same | changed | what it is |
|---|---:|---:|---|
| `0300168c` | 120 | 62953 | real code being patched — the gate that matters, 97.8% of all flushes |
| `03001404` | 66 | 1 | a constant re-stored into a code block — the address that crashed us |

That gap is structural, not tuned. Frequency was the wrong discriminator and
failed twice: an absolute threshold is outgrown by any cold address, and a ratio
to the hottest candidate degenerates because a promoted gate stops counting and
the maximum freezes. Both crashed.

---

## Rejected, with the measurement

Kept in the tree because the measurement is the point; refused in a release
build where that applies.

| switch | verdict |
|---|---|
| `SMC_SKIP_SAME` | **White-screens Heart & Soul at boot** — the idempotent-store compare is wrong. `gpsp_profile.h` refuses it in a release. |
| `SMC_GATES_CODED` | Demands the write address itself already be a block start, which rejects `0300168c` because at the moment of the write that address is *interior* to its block. 59.9 → 46-58 fps. |
| `SMC_GATES_SNAP` | Snaps backwards to the block head: valid, but the block starting at that head still spans the SMC region, so isolation is lost. 1.43M flushes, 31-48 fps, against 116k and a locked 59.9 for add-only. |
| `SMC_PARTIAL` | Superseded by `SMC_PARTIAL_SAFE`. Its block-copy detector is the ancestor of the current writer fingerprint. |
| 2.0.2's evicting gate rule (no flag; the `#else` branch when neither `SIMPLE` nor `CLUSTER` is set) | Eviction reassigns a gate's address while blocks compiled against the old layout are live, and gpSP's tag map holds one slot per halfword — it cannot represent the resulting overlap. This is the Heart & Soul crash. |

---

## Built but with no recorded hardware verdict

**These are the ones to be careful with.** Each compiles, each changes
behaviour, and none has a result written down. Listed so that "there is a flag
for that" is never mistaken for "that was tried".

| switch | what it would do | status |
|---|---|---|
| `SMC_GATES_SNAPF` | Snap the gate *forward* to the first proven boundary at or after the write, which is the only variant designed to keep validity **and** isolation. | Never measured. The most interesting unexplored option here. |
| `SMC_GATES_CLUSTER` | Add-only plus 2.0.2's cluster grouping, without its eviction — keeps the half that cut dispatcher lookups (196k → 59k) and drops the half that crashed H&S. | Never measured on hardware. |
| `SMC_SCAN_SPCLAMP` | End `scan_block` at the stack pointer instead of the top of IWRAM, so scans stop tagging the live stack as code (10122 flushes measured in one battle from exactly that). | Never measured. Plausible performance win. |
| `SMC_SP_CHECKED` | Send SP-based block stores down the general path so their final word reaches the SMC check; Thumb `PUSH {..., LR}` also uses the checked helper for LR. | Never measured. Does not check every word in a multiword store, so it is only a partial coverage fix. Likely costs speed on a hot memory path; measure on hardware. |
| `SMC_PARTIAL_SAFE_FRAMEFULL` | One full flush per frame regardless. | Never measured; no comment in the Makefile. |
| `SMC_PARTIAL_SAFE_NO_TRAMP` | Selective retirement without the trampoline. | Never measured; no comment. |
| `SMC_PARTIAL_DIRECT_GATE` | Patch the gate fall-through directly instead of through the dispatcher. | Never measured; no comment. |
| `GATE_SLOTS`, `GATE_RATIO`, `CLUSTER_BYTES` | Override `MAX_TRANSLATION_GATES`, `SMC_GATE_RATIO`, `SMC_GATE_CLUSTER`. | Tuning knobs, not experiments. `CLUSTER_BYTES` 128 is sized to H&S's M4A layout (two loop copies 152 bytes apart), not picked for roundness. |
| `OVERCLOCK_60FPS` | Upstream's 60 fps overclock build. | Untouched by this project. |

## Control arms

`SMC_PARTIAL_SAFE_CONTROL` disables the selective-invalidation fast path while
leaving everything else in place. It exists to make an A/B one variable. It is
refused in a release, because shipping it reads as an unexplained performance
regression with no apparent cause.

## Diagnostics

| switch | writes | profile |
|---|---|---|
| `BADJUMP_REPORT` | `ms0:/badjump.txt` — registers, gate table, previous block entry, and re-enables `badjump_recover`'s file append | `diagnostic` |
| `SMC_WRITE_HISTO` | `ms0:/smchisto.txt` every 4000 flushes — write addresses and storing pcs bucketed, plus the count of unmappable block exits contained | `diagnostic` |

Both write to the memory stick from inside the dynarec dispatch path, on the
emulation thread. Both are refused in a release by `gpsp_profile.h`.

`XLAT_DEPTH_PROBE` (profiles `soak`, `diagnostic`; refused in a release) writes
nothing itself: it records the deepest nested block translation and the lowest
frame address at translation entry (`xlat_depth_max`, `xlat_sp_min`,
`xlat_depth_probe_reset()`), which the frontend's `core_health` EVT line prints
as `xlat_depth` / `stack_used` every 600 frames and after every state load.
Each nesting level costs ~380-430 B of the 256 KB main-thread stack.

`IRQ_INTEGRITY_CHECK` (profiles `soak`, `diagnostic`; refused in a release)
snapshots r0-r14, CPSR and the return address at every IRQ entry
(`check_and_raise_interrupts`, and the re-raise paths in
`execute_spsr_restore_body` / `execute_store_cpsr_body`) and compares them at
the matching IRQ return. The BIOS IRQ stub saves r0-r3/r12/lr and the game's
handler preserves r4-r11 by ABI, so any difference is the emulator losing
guest state inside an interrupt. On a mismatch it appends to
`ms0:/irqchk.txt` and dumps guest RAM to `ms0:/irqlost-N-{iwram,ewram,regs}.bin`
for the first two. One struct copy per IRQ. `IRQ_INTEGRITY_SUMMARY` adds a
heartbeat line every 16384 returns (PPSSPP only).

`STALE_CHECK` (PPSSPP only; refused in a release) snapshots the guest words of
every translated IWRAM block and reports a block that is still live (start tag
unchanged, body not retired) after its source changed without an SMC event,
to `ms0:/stale.txt`. `STALE_DISPATCH_ONLY` restricts the per-dispatch work to
the block being entered (and reports `EXECUTING STALE (dispatch)`). Far too
slow for hardware.

### The ROM-residency A/B apparatus (2026-09-26)

Harness keys in `.gpsp-harness.ini`, read by `tools/build.sh harness64`
EBOOTs (`harness` pins the 32 MiB layout and cannot hold a 32 MiB cart). All
six were checked bit-identical to the default over 9,270 post-load frames of
`unbound_double_high` in PPSSPP (registers, IWRAM, EWRAM, I/O, palette, OAM,
VRAM, audio), so each arm differs from the control only in the hardware
variable it names. Protocol: `docs/ROM-RESIDENCY.md`.

| key | effect |
|---|---|
| `rom_cap = N` | ROM page-cache cap in 1 MiB blocks (1..32), no probe. `32` holds a 32 MiB cart whole on a 64 MiB console |
| `swap_stubs = 1` | a resident cart still gets the paged-ROM load stubs (NULL-page check), so its JIT code and layout match a paged run exactly |
| `cache_paranoid = 1` | every JIT cache sync becomes a full D-cache writeback-invalidate plus full I-cache invalidate |
| `rom_ballast_kb = N` | hold N KiB of heap before the ROM cache, so the blocks land higher (17408 puts 15 paged blocks at 0x0A20xxxx-0x0B10xxxx) |
| `jit_coherency_scan = N` | every N frames, count RAM-translation-cache words whose cached view differs from memory (code the D-cache holds but instruction fetch cannot see): `EVT jit_coh` |
| `shash = 1` | one line per frame to `log/shash.txt` (not the lossy event ring): audio hash, r0-r15/CPSR, IWRAM, EWRAM, I/O, palette, OAM, VRAM hashes. `shash_dump_from/_to` add raw dumps. Needs `vhash = 1` |

### The 2026-09-24 gate-layout A/B apparatus

Pass through `CODE_DEFINES='-D...'` on the root make. Each changes emulation or
timing on purpose and is refused in a release.

| switch | effect |
|---|---|
| `GATE_NOCHECK` | gate exits jump past the dispatcher's `DISPATCH_CYCLE_CHECK`, so a gate is never an `update_gba` point and block layout cannot move interrupt timing |
| `GATE_ONLY_FULLFLUSH` | install the endpoint gates but never activate partial retirement |
| `GATE_HIGH_ONLY` | install only the checked (high) endpoint gate — the one-gate layout |
| `RTC_PIN=<epoch>` | fixed RTC, so boot is deterministic across runs (the wall clock otherwise leaks into IWRAM) |
| `STATE_HASH_TRACE` | `ms0:/statehash.txt`: per-frame ticks, PC, IWRAM and EWRAM hashes; `STATE_DUMP_LO/HI` add raw dumps |
| `UPDATE_TRACE_LO/HI` | `ms0:/utrace.txt`: every `update_gba` entry (ticks, overshoot, PC, registers, IWRAM hash) and every SMC event in the frame window; `UTRACE_DUMP_T/T0` dump RAM at exact tick values |
| `STALE_STACKNOTE` | log IWRAM translations above 0x03007000 (H&S runs ARM routines copied onto the stack) |

With `GATE_NOCHECK` + `GATE_ONLY_FULLFLUSH` + `RTC_PIN` the one-gate and
two-gate layouts are timing-identical (same ticks, overshoot and PC at every
`update_gba`). Before the gate-word tagging fix they diverged at H&S frame 23100
in exactly one mixed sample (stale `adds sb, r6, sb, asr #23` at 0x03001688);
after it they are bit-identical over 1001 frames (audio `84b2bbff`).

## Frontend defines

| define | meaning |
|---|---|
| `GPSP_PLAYABLE` | build a binary for someone to play (ADR-0067). Among other things it points the harness ini at a filename that cannot exist, so a leftover `.gpsp-harness.ini` on a player's card is inert. |
| `VID_TRIPLE` | three framebuffers instead of two. Fixes tearing; brought a fast-forward black-bar artifact, since resolved for all three presets by `g_fb_filled` plus `vid_swap()`'s display-ownership read-back (`docs/FF-ARTIFACT-FIX.md`). |
| `GPSP_KEEP_TELEMETRY` | keep the EVT logger. Without it `fe_evt()` compiles to `((void)0)` and does **not** evaluate its arguments (ADR-0067b). |
| `GPSP_PERF_RIG` | the performance harness: autopilot plus perf windows. Requires `GPSP_KEEP_TELEMETRY`. |
| `GPSP_ROMLOAD_DIAGNOSTICS` | ROM-load instrumentation. Refused in a release. |
| `GPSP_STANDBY_NOTES` | standby/resume tracing. |
| `GPSP_STALL_RECORDER` | hard-freeze bisect arm: the harness stall watch (a 0x10-priority thread that wakes once a second) in a playable build, always on at 6 s. Writes `log/stall.txt` (main-thread state, loop phase, ME mailbox) only when the main loop has not iterated for 6 s. Adds two stores per loop iteration. |
| `GPSP_CATCH_SELFTEST` | L+R+UP+TRIANGLE stores to address 0, so the `ME_CATCH` screen can be seen working once. Refused in a release. |

`ME_CATCH=1` on `psp/Makefile` (passed down to `psp/me`) links `psp/me/catcher.c`
and `catcher_vec.S` into the Media Engine PRX. The PRX's `module_start` then
registers a kernel default exception handler that, on any unhandled CPU
exception in any thread, draws EPC / BadVAddr / Cause / GPRs / nearby memory /
the ME mailbox into all three VRAM framebuffers and holds the picture. The
EBOOT does not change at all, and every existing PRX symbol keeps its address:
the two entry points are wrapped rather than edited. Without the flag the PRX
is byte-identical to the committed one (fca9dee3). Only effective when the ME
boots (`me_boot = 1`).

Layout A/B switches for the same PRX (hard-freeze bisect, 2026-09-26; all
appended after every existing object, so nothing earlier moves):
`ME_CATCH_NOREG=1` swaps the registration for the already-imported release
call (same code size, catch layout, nothing registered); `ME_PAD_RODATA=N`
appends N bytes to `.rodata`, which moves all of the ME's `.bss` by N (3856
reproduces the catch data addresses; 12528 with `ME_CATCH=1` puts them at
stock + 16 KB); `ME_PAD_BSS=N` appends N bytes after the last ME variable, so
only `_end` moves.

## Build plumbing, not experiments

`HAVE_DYNAREC`, `CROSS_COMPILE`, `FORCE_32BIT_ARCH`,
`SMALL_TRANSLATION_CACHE`, `RUNTIME_JIT_CACHE`, `PSP_PIXFMT`,
`FRONTEND_SUPPORTS_RGB565`, `GPSP_PROFILE`.

`RUNTIME_JIT_CACHE` (always on for `platform=psp1`) sizes the translation
caches at startup instead of at compile time, so one EBOOT serves every model.
The SMALL tier (2 MB ROM / 384 KB RAM code, `SMALL_TRANSLATION_CACHE`) is
static and is what a PSP-1000 always gets.  In `retro_init`, before the ROM
page cache takes the heap, `dynarec_select_translation_caches()` tries one
`malloc` of the LARGE tier (10 MB / 512 KB) plus 20 MB of headroom (the
15-block ROM cache, the 1 MB post-load reserve, 4 MB frontend slack); if that
fits -- only possible with the 64 MiB layout -- it allocates the LARGE tier
from the heap, otherwise it keeps the static arrays and touches nothing.
Harness builds log the result as `EVT jit_cache tier=... reason=...` and the
post-load malloc budget as `EVT heap_budget`; `jit_small = 1` in
`.gpsp-harness.ini` forces the SMALL tier for an A/B on one console.

`BIG_JIT=1` (the old compile-time LARGE tier for every model) is refused by
the root Makefile: a PSP-1000 cannot afford it and the runtime tier replaces it.

---

## The rule for adding one

A switch needs three things or it becomes another entry in the "no recorded
verdict" table:

1. A comment **immediately above its own `ifeq`**, saying what it does and what
   it would prove.
2. An entry here once it has been measured, with the number.
3. A `gpsp_profile.h` check if it must never reach a release, or if it silently
   shadows another switch.
