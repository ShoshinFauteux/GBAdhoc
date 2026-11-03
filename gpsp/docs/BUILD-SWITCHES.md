# BUILD SWITCHES — the inventory

Every dynarec and diagnostic switch in the tree, with a verdict.

**Why this exists.** A flag with no verdict is worse than no flag: it looks like
an option.

## How the shipped set is built (since 4.6.0)

**`tools/release.sh X.Y.Z` makes a release** (docs/RELEASE.md), and
**`tools/build.sh release`, with no environment, is the release build.** The core
switches that ship are lines in `gpsp_features.h`, which the root Makefile
force-includes (`-include`) into every core object of `platform=psp1` and
`platform=drprof-mipsel`, assembler included. The frontend's constants are lines
in `psp/fe_features.h`, force-included by `psp/Makefile`: the `ND_*` transport
profile always, `VID_TRIPLE` and `PLAY_INPUT_FRESH` when the build is
`GPSP_PLAYABLE`. No recipe lists any of them, and `tools/flags.lock` pins all of
them (see **The drift check** below).
`GBA_LINK`, `REWIND`, `CHEATS` and `LAYOUT_PIN` change what is compiled and
linked, so they stay make variables, default 1 for `psp1` in both makefiles.
`LAYOUT_PAD_BYTES` and `LAYOUT_PAD_RO_BYTES` (docs/LAYOUT-PINNING.md, "Keeping the pads") are the defaults in `psp/Makefile`, `HOT_DATA_GUARD`'s link
script follows the core, `PSP_PIXFMT` stays a variable because it reaches the ME
sub-make, and the `TGB_*` switches stay scoped to the GB core's objects. This
produced the 4.5.0 EBOOT byte for byte.

The two sides do not share defines. The frontend never tests a core feature (it
would always read as off there; core features reach it as weak symbols) and the
core never tests a frontend one.

Other platforms never see the header. `platform=unix`, the SDL desktop build and
the host tests compile these switches off, so **the `#else` paths are not dead
code** and stay.

| to do this | write this |
|---|---|
| one feature off, an A/B arm | `CORE_EXTRA="XLAT_PURE=0" tools/build.sh harness` (root make `XLAT_PURE=0`, which is `-DGPSP_NO_XLAT_PURE`). A release refuses `CORE_EXTRA`. |
| a different value | `CORE_EXTRA="SMC_SINGLE_HOT=32"` |
| the link, the rewind, pinning off | `GBA_LINK=0`, `REWIND=0`, `LAYOUT_PIN=0` (both makes get the value); `INPUT_FRESH=0`; `LAYOUT_PAD_BYTES=N` |
| a frontend value | `FE_EXTRA="-DND_WINDOW=128"`: a `-D` wins over `psp/fe_features.h`; `-DGPSP_NO_VID_TRIPLE` drops a playable default |
| a twin variant | `VDEFS` is the complete list: `tools/drprof/Makefile` defaults to `FEATURES=explicit` (`-DGPSP_FEATURES_EXPLICIT`, the header defines nothing), so a switch a variant omits is off and `-UDISPATCH_CACHE` works |
| a twin of what ships | `FEATURES=baked tools/drprof/dr_build.sh ship`; add `-DGPSP_NO_NAME` to drop one |

**`-UNAME` does not switch a baked feature off**: gcc applies `-D`/`-U` before any
`-include`, so the header would define it again. That is what `GPSP_NO_NAME` is
for. `gpsp_profile.h` still refuses a combination that is silently wrong, so
turning off something another feature needs is a compile error.

`CAND1`, `CAND2`, `COMBO3` and `MSR_FAST` as `tools/build.sh` switches are gone:
`=1` from an old recipe does nothing, anything else fails the build.

**The manifest.** `coreFlags` in `build-manifest.json` is what the compiler
resolved (`make print-features`, `-dM` on the exact make line), not a copy of a
recipe. GBAdhoc-Lab's `gate_twin.py` rebuilds the twin from it and its scheduler
matches fixture `requires` against it, so an empty list would degrade both
silently; `tools/build.sh` fails instead. `tools/make_release.sh` refuses any
build whose list is not exactly the header's.

`frontendDefines` is resolved the same way (`make -C psp print-features`): the
profile's defines plus what `psp/fe_features.h` gave that make line.

**The drift check.** `tools/check_flags.py` needs no build and takes a second;
`--compile` adds what the compilers resolve (docker), `--build DIR` a release
manifest. `tools/make_release.sh` runs the first and last and `tools/release.sh`
all three. It fails when:

| area | what drifted |
|---|---|
| `lock` | a feature list, make default or profile recipe differs from `tools/flags.lock` |
| `recipes` | a profile, makefile or wrapper restates a baked feature, or a force-include is gone |
| `removed` | a switch in **Removed** below is back in an `#if`, a `-D` or a make conditional |
| `dead` | something is baked or passed and no source tests it |
| `sides` | the frontend tests a core feature, or the core a frontend one |
| `docs` | a baked switch has no line in this file |
| `oracles` | the twin's tables or `dr_build.sh` name a `-D` no source mentions |

Changing a shipped flag is deliberate: edit the header, run
`tools/check_flags.py --update`, and commit the lock with it.

The rest of this file records what each switch does. Where a section below says
"OFF by default" or "root make (`NAME=1`)" about a switch in `gpsp_features.h`,
that was its state when it was measured; the header is the state now.

---

## How this list came to be

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

Part of `gpsp_features.h`, so release, harness and diagnostic alike. Changing
one changes emulation. Not the whole list: the header is.

| switch | what it does |
|---|---|
| `SMC_GATES` | put a translation gate at a self-modified address so later blocks split there. Deliberately independent of `SMC_PARTIAL*`: gates bought Unbound its speed and also broke its audio, so the two must be testable apart. |
| `SMC_GATES_SIMPLE` | the 2.0.1 add-only gate rule. Once placed, a gate never moves, so an already-translated block can never disagree with the table. 2.0.2's evicting rule is what crashed Heart & Soul. |
| `SMC_GATES_RANKED` | promote after 64 observed writes when at least half changed the word. Counts are lifetime within one ROM: there is no active staleness window, so sound-mode changes do not reset the evidence. See below. |
| `SMC_GATE_BITMAP` | same gate table and boundaries, tested through a bitmap instead of comparing every live gate per compiled instruction. |
| `ENTRY_GATES` | when the RAM translation cache is exhausted, every RAM block start becomes a gate and the gated block jumps straight to its continuation, so a routine entered at many points is translated once instead of once per entry (Driver 2: 887 cache-full flushes -> 1 at 384 KiB, per-frame state bit-identical on the twin, 24-35 -> 59.7 fps on a PSP-3000's small tier). Armed only by an exhaustion flush; reset with the SMC gates (ROM load, link cold start). Requires `SMC_GATE_BITMAP`. |
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

### The rest of the shipped set

One line each; the root Makefile's comment above each `ifeq` has the detail.
"Exact" means the same guest state as without it (speed only).

| switch | what it does |
|---|---|
| `SMC_GATE_SELFPATCH` | the ranked rule also promotes a store patching the code just ahead of itself, even re-storing the same bytes, and gates the lowest word written (Mario Golf's table gate). Needs `SMC_GATES_RANKED`. |
| `ENTRY_GATES_RANGE` | the exhaustion-time gate harvest walks only the tag range the following full flush clears. Needs `ENTRY_GATES`. |
| `ENTRY_GATES_FAST` | the entry-gate harvest tests four tag slots per load. Exact. |
| `SMC_RETIRE_LOCAL` | retire walks start at a per-64-byte bound on where a covering block can start. Same retirements, shorter walks. Needs `SMC_RETIRE_WINDOW`. |
| `SMC_RETIRE_LIST` | `smc_retire_span`'s validation walk lists the block-start tags and the retire pass visits only those. Same retirements. |
| `SMC_SINGLE_THUMB`, `SMC_SINGLE_HALF` | selective retirement from Thumb STR/STRH/STRB and from ARM STRH single stores. |
| `SMC_SINGLE_ANYPC`, `SMC_SINGLE_EWRAM` | the same for writers outside IWRAM (ROM, EWRAM, BIOS) and for EWRAM targets. |
| `SMC_SINGLE_UNTAG` | also lift dead block-start tags in the written range. |
| `SMC_STM_RANGE` | a short writeback STMIA from IWRAM into IWRAM retires its own span instead of a full RAM flush (FIFA 07's code generator). |
| `SMC_STM_CHAIN` | a non-final block-store word landing on tagged RAM marks the next word, so the final word's checked store traps (the STM hole). Needs `SMC_STM_RANGE`. |
| `SMC_STM_WIDE` | widens `SMC_STM_RANGE` to 2-16 registers and lets such a store activate selective retirement (Hot Wheels). |
| `SMC_STM_AHEAD` | `SMC_STM_WIDE` retires forward over the tagged run after an advancing copy's write. Needs `SMC_STM_WIDE`. |
| `SMC_STM_FIXED` | the same span retirement for a fixed-base `STMIA Rn,{..}` (no writeback), with no forward widening: ipatix's ImprovedSoundMixer pastes its IWRAM resampling loop this way (The Morrow's Golden Country, ~4 full RAM flushes a frame without it). Changes block boundaries, so timing (MonoMyth, Unbound diverge); not exact. Needs `SMC_STM_RANGE`. |
| `SMC_SAME_REPROBE` | `SMC_SAME_ADAPT`'s first re-probe threshold, in single-word retirements while off. Ships at 1024. |
| `SMC_IMM_PATCH` | an IWRAM ARM immediate the game keeps rewriting is emitted in a fixed lui/ori form and patched in place instead of retiring the block (Doom II, Duke, Ice Nine). Needs `JIT_CODE_DISCIPLINE`. |
| `XS_HEAP` | the `SMC_SAME_SKIP` and `SMC_RETIRE_LOCAL` tables (up to 69 KiB) come from one heap block, so the hot arrays after them in `.bss` keep their D-cache sets. |
| `XS_WALK_PAIR` | `xs_blocks_current` tests two tag slots per aligned load. Same blocks, same order. |
| `XS_NOTE_FAST` | `xs_note_block` records eight halfwords at a time. Exact. |
| `SMC_REUSE_SPARE_KB` | the RAM code cache's spare, in KiB, that `SMC_REUSE` keeps. Ships at 256. |
| `SMC_REUSE_MODE` | 0 off, 1 record only, 2 on, 3 on without cost rules (ships), as one initialised word so hardware arms share a layout. |
| `DMA_SMC_RETIRE` | a DMA onto translated code retires exactly the tagged words it wrote instead of a full flush (Top Gear Rally's overlays). Needs `SMC_STM_RANGE`. |
| `DMA_SMC_ADAPT` | `DMA_SMC_RETIRE` only while it measures cheaper than the flush (probe and back off). |
| `DMA_ADAPT_V2` | `DMA_SMC_ADAPT`'s cost model re-priced from the twin; ties go to the full flush. |
| `XLAT_FAST` | translator host-work speedups; emitted code, tags and counters unchanged. |
| `JIT_COST` | the cost of producing translated code (docs/JIT-COST.md). Exact. |
| `SCHED_FAST` | scheduler fast paths in `update_gba`, compiled in; the run-time `sched_fast` turns them on. Exact. |
| `IRQ_IO_FAST` | IE/IF/IME stores inline in the I/O store stub, compiled in; the run-time `irq_io_fast` turns them on. Exact. |
| `SERIAL_IDLE_FAST` | skips `update_serial`'s call chain with no transfer pending and no link client role. Exact. |
| `FIQ_BANK_FIX` | bank FIQ's r8-r12 correctly in `set_cpu_mode` (OpenLara). |
| `BX_ALIGN_FIX` | a BX into ARM state with bit 1 set goes to the word below, as on the ARM7TDMI. |
| `BACKUP_BUS_FIX` | a 16/32-bit dynarec load from SRAM/flash returns the byte in every lane, as the 8-bit bus does. |
| `COND_ENTRY_FIX` | a branch into the middle of a run of same-condition ARM instructions gets its own condition check. |
| `NV_COND_FIX` | an ARM word with condition 0xF (NV) is never executed, as on the ARM7TDMI: no code, 1S, not an exit. gpSP crashed in the translator on one (issue #10). Blocks without NV words are byte-identical. |
| `BIOS_BOUND_FIX` | a region-0 pc above the 16 KiB BIOS is unmappable: exits to it go through the dispatcher instead of translating ROM pages as BIOS code (an eager-link chain filled the ROM cache, issue #10). |
| `XLAT_LAST_TRY_LAZY` | the last of a lookup's four translation tries links every exit through the dispatcher, so a legal pc whose eager links filled a cache is not reset by BADJUMP_SAFE. |
| `TIMER_RELOAD_FIX` | a timer that has just overflowed reads its reload value, not 0 (Need for Speed Underground). Guest-visible. |
| `VRAM_EXEC` | translate and run code the game copied into VRAM (Big Mutha Truckers, Street Racing Syndicate, jsmolka nes.gba). |
| `HOT_DATA_OFFSET` | where `HOT_DATA_GUARD` places the stub hot block in its 8 KiB way. Ships at 0x1b00. |

Frontend, `psp/fe_features.h` (its comments keep the field logs behind each value):

| define | ships at | what it does |
|---|---|---|
| `ND_MAX_PAYLOAD` | 144 | largest reliable payload; netdrv refuses anything bigger, loudly |
| `ND_WINDOW` | 256 | ARQ window, and the per-peer reorder buffer |
| `ND_TXQ_CAP` | 384 | per-peer reliable backlog |
| `ND_RTO_MIN_US` | 200000 | retransmit floor |
| `ND_RTO_MAX_US` | 2500000 | retransmit ceiling; must not sit below what the estimator computes |
| `PLAY_INPUT_FRESH` | 1 | the low-latency pad re-peek on by default (`INPUT_FRESH=0` on `tools/build.sh` turns it off) |

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
build where that applies. Those deleted since are under "Removed".

| switch | verdict |
|---|---|
| 2.0.2's evicting gate rule (no flag; the `#else` branch when `SMC_GATES_SIMPLE` is off) | Eviction reassigns a gate's address while blocks compiled against the old layout are live, and gpSP's tag map holds one slot per halfword — it cannot represent the resulting overlap. This is the Heart & Soul crash. |

---

## Built but with no recorded hardware verdict

**These are the ones to be careful with.** Each compiles, each changes
behaviour, and none has a result written down. Listed so that "there is a flag
for that" is never mistaken for "that was tried".

| switch | what it would do | status |
|---|---|---|
| `GATE_SLOTS`, `GATE_RATIO`, `CLUSTER_BYTES` | Override `MAX_TRANSLATION_GATES`, `SMC_GATE_RATIO`, `SMC_GATE_CLUSTER`. | Tuning knobs, not experiments. `CLUSTER_BYTES` 128 is sized to H&S's M4A layout (two loop copies 152 bytes apart), not picked for roundness. |
| `OVERCLOCK_60FPS` | Upstream's 60 fps overclock build. | Untouched by this project. |
| `RFU_SCHED` (tools/build.sh `RFU_SCHED=1`, both makes; off by default) | Scheduled ticks for the emulated Wireless Adapter ([RFU-COUPLING.md](RFU-COUPLING.md)): each console publishes its frame's adapter traffic with a tick, the partner's game gets it D = 2 frames later, and a console waits only for a tick that is really missing. Replaces frame pace, shedding, the overflow hold and supersede while it runs. Changes the wire format: netdrv refuses a session between a ticks build and one without (`ND_FEAT_RFU_SCHED`). Harness key `rfu_sched` = D, 0 = off. | Lab builds on `claude/rfu-reference` (RFU_TL), PSP-1000 + 3000, 2026-11-02: Tennis 56/56 fps (off 57/54), resent 2.6-5 % (off 43-60 %), dropped 0; Golf 46-48 both sides (off 49/42 with 13-15 % dropped). This switch (`claude/rfu-ticks` 2e5c288, lab build d5973f7fc046, 2026-11-03): 11/11 pass, both console orders: Tennis 56/56, Golf 45-47 both sides (lobby 5/5), long Golf 17 min 48.7/48.6, Emerald trade 56.7, battle 56.3, host-loss error screen at D = 2 and D = 0. Desktop: a ticks build and one without refuse each other, both orders. |
| `ME_AFFINE_LINES` (video.h, default 1 on `claude/me-midframe`) | The Media Engine capture records the affine reference counters per line and the engine installs them, so a BG2X/Y/BG3X/Y reload in HBlank (mode-7 floors and roads) reaches it. | Desktop model: Mario Golf's aim view 1558 wrong frames -> 23; clean frames bit-identical. Hardware pending. [ME-MIDFRAME.md](ME-MIDFRAME.md) |
| `ME_MIDFRAME_LOG` (video.h, default 1 on `claude/me-midframe`; harness key `me_midlog`) | Undo/redo log of palette/OAM/VRAM written during the visible lines, applied by the engine line by line. Adds one store to the MIPS palette stub (`PAL_UPDATED`) and one to the VRAM stub (`vram_clean[96]`). | Desktop model: all synthetic cases and H&S heavy's 5 remaining frames exact; recorder ~0.5 us/frame on the desktop. PSP cost not measured. [ME-MIDFRAME.md](ME-MIDFRAME.md) |

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
timing on purpose and is refused in a release. `GATE_NOCHECK`,
`GATE_ONLY_FULLFLUSH`, `GATE_HIGH_ONLY`, `NO_GATE_TAG_FIX` and `NO_BL_FIX`, the
arms that answered this question, were deleted in 4.6.0 (see "Removed").

| switch | effect |
|---|---|
| `RTC_PIN=<epoch>` | fixed RTC, so boot is deterministic across runs (the wall clock otherwise leaks into IWRAM) |
| `STATE_HASH_TRACE` | `ms0:/statehash.txt`: per-frame ticks, PC, IWRAM and EWRAM hashes; `STATE_DUMP_LO/HI` add raw dumps |
| `UPDATE_TRACE_LO/HI` | `ms0:/utrace.txt`: every `update_gba` entry (ticks, overshoot, PC, registers, IWRAM hash) and every SMC event in the frame window; `UTRACE_DUMP_T/T0` dump RAM at exact tick values |
| `STALE_STACKNOTE` | log IWRAM translations above 0x03007000 (H&S runs ARM routines copied onto the stack) |

With gate exits skipping the cycle check, no partial retirement and `RTC_PIN`, the
one-gate and two-gate layouts are timing-identical (same ticks, overshoot and PC at every
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
| `JIT_COST_KEYS` | the harness ini keys for `JIT_COST`'s pieces. In no profile: Lab candidate builds add `FE_EXTRA=-DJIT_COST_KEYS`, so compare a Lab EBOOT only with one built the same way. |

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
`malloc` of the LARGE tier (10 MB / 384 KB) plus 20 MB of headroom (the
15-block ROM cache, the 1 MB post-load reserve, 4 MB frontend slack); if that
fits -- only possible with the 64 MiB layout -- it allocates the LARGE tier
from the heap, otherwise it keeps the static arrays and touches nothing.
Harness builds log the result as `EVT jit_cache tier=... reason=...` and the
post-load malloc budget as `EVT heap_budget`; `jit_small = 1` in
`.gpsp-harness.ini` forces the SMALL tier for an A/B on one console.

`BIG_JIT=1` (the old compile-time LARGE tier for every model) is refused by
the root Makefile: a PSP-1000 cannot afford it and the runtime tier replaces it.

### Dynarec profiling and the two speed prototypes (2026-09-29)

All OFF by default; `docs/DYNAREC-PROFILE.md` has the measurements.

| switch | where | what it does | status |
|---|---|---|---|
| `DISPATCH_CACHE` | root make (`DISPATCH_CACHE=1`) | `mips_indirect_branch_*` probe a 512-entry-per-ISA {guest pc -> host entry} table in asm before `save_registers` + the C block lookup; filled by the C lookup, emptied by every flush entry point (full RAM, ROM, partial retire). The cycle check still runs first. | Prototype. `tools/drprof/dr_oracle.py` IDENTICAL on 7 fixtures; PPSSPP IDENTICAL; twin -4..-23% core instructions. Hardware A/B staged (`GBADHOC-DRPROF` rig, arms A-PROTO/H-PROTO). |
| `DISPATCH_PRECISE` | root make (`DISPATCH_PRECISE=1`, needs `DISPATCH_CACHE`) | SMC retirement (`smc_stable_thunk_dispatch`, the one place a RAM block's thunk is sent back to the dispatcher) forgets only that block's {pc -> thunk} entry instead of emptying both tables; only canonical RAM pcs (IWRAM 03000000-03007FFF, EWRAM 02000000-0203FFFF) are cached, because a mirrored pc shares the block's tag but not its key. Full RAM/ROM flushes still empty everything. | cpu-levers 2026-10-26. Off by default. Twin (c3lat4 flags, frames 300-1200): Doom core -4.6%, Duke -4.7% (dispatch instructions halved); per-frame state hashes IDENTICAL to the build without it. |
| `XLAT_THUMB_NO_UNSWITCH` | root make (`XLAT_THUMB_NO_UNSWITCH=1`) | Host compiler only: emitted code and guest behaviour are unchanged. psp-gcc 15.2 `-O3` unswitches `translate_block_thumb`'s per-instruction loop on `ram_region` (`-fopt-info-loop`), which compiles the Thumb decoder twice (37.7k MIPS instructions). The doubled function keeps `translation_ptr` in a stack slot: 1545 stack stores and 2973 stack loads. An empty `asm("" : "+r"(ram_region))` at the top of the loop makes the condition non-invariant. The result is one copy of 18.1k instructions with 256 stack stores, and `translate_block_arm` is untouched (82.8k, 88 stores, never unswitched). `tools/build.sh` runs `tools/xlat_spill_guard.sh` when the switch is on; the build fails above 700 stack stores. Found because SPIN_SKIP happened to tip GCC out of unswitching (cpl3's -96 KiB image). | cpu-levers 2026-10-26. Off by default. The twin cannot measure it: the twin's GCC 12 never unswitched (18.2k instructions, 125 stores either way). Twin state hashes are identical with it on (Doom, Duke, H&S heavy, Golf, 300 f). Its cost has to be measured on hardware (lab A/B). |
| `SPIN_SKIP` | root make (`SPIN_SKIP=1`, MIPS only) | The translator proves that a backward branch closes a side-effect-free loop (`spin_analyze`): loads without writeback, ALU ops and compares only. Every register or flag read in the loop is either never written in it or written earlier in the same iteration, and load-address registers are never written. So iterations repeat exactly until memory changes, and only an event (IRQ, DMA) can change it. The taken back-edge then asks `update_gba` (descriptor in `reg[REG_SPIN_DESC]`) whether to end the slice. The answer is yes when every load address is RAM, palette, VRAM, OAM or ROM (not cartridge GPIO), or DISPSTAT/VCOUNT/IE/IF/IME/DMA CNT_H. Anything else (timer counters, KEYINPUT, serial, backup) vetoes the loop for the life of its translation, and it runs cycle by cycle as before. Not keyed on any address or ROM. `SPIN_SKIP_VERIFY` (twin oracle) never skips; it checks at the next event that registers and RAM/VRAM/OAM/palette/IO are unchanged. Mutants `SPIN_SKIP_MUTANT_REGS` and `SPIN_SKIP_MUTANT_STORE` drop the register rule and allow stores, respectively. | cpu-levers 2026-10-26. Off by default. Twin numbers and oracle results are in the cpu_threaded.c comment and the commit. Moves timing by less than one loop iteration per event, so it is judged by the Lab gate's replayed lockstep. |
| `SMC_SAME_SKIP` | root make (`SMC_SAME_SKIP=1`, needs `SMC_PARTIAL_SINGLE` + `SMC_PARTIAL_STABLE_THUNK`) | for every IWRAM halfword a RAM scan reads, record the value it read (a later scan that finds a different value poisons the halfword until the next full RAM flush; the record is cleared only where no live block covers the halfword). A single-store SMC trap in IWRAM then skips `smc_retire_span` when every block it would retire matches its records over its WHOLE source extent -- retiring it would only retranslate identical code. The trap and re-dispatch are unchanged; no emitted code changes. Exact whatever wrote memory (an unreported STM word, DMA, BIOS CpuSet changes a halfword -> no match -> retire). Not the shelved 2026-09-12 `SMC_SKIP_SAME` (stub-level, pre-store compare, skipped the trap): see the comment in cpu_threaded.c. +52 KiB .bss. | cpu-levers 2026-10-26. Off by default. Twin vs DISPATCH_PRECISE (frames 300-1200): Doom core -15.9%, Duke -11.4% (xlat -56%/-46%); EXEC_STALE_CHECK 0 hits on Doom, Duke, H&S heavy, Unbound rival, Mario Golf/Tennis; mutant SMC_SAME_SKIP_MUTANT_NOCHECK (trust any covering block) caught: 111k stale executions on Doom in 600 frames. Changes timing (not bit-identical); judged by the Lab gate's replayed lockstep. |
| `SMC_SAME_ADAPT` | root make (`SMC_SAME_ADAPT=1`, needs `SMC_SAME_SKIP`) | record IWRAM halfwords for SMC_SAME_SKIP only while it pays: the record is taken in bulk per translated block (`xs_note_block` from `ramtag_note_extent`, nothing in the scan loop); after each 65536 recorded halfwords with fewer than 256 skips recording and checking stop; after 256 single-word IWRAM retirements while off it re-probes. Each tag remembers whether its translation was recorded (`xs_rec`) and a skip needs every covering block recorded. Full-flush clears of `xs_ok/xs_bad` are skipped when nothing was recorded since the last one. | cpu-levers, OFF by default. Exact like SMC_SAME_SKIP (it only skips less). Twin vs cpl1 flags, core work/frame: Hot Wheels +0.00 % (SMC_SAME_SKIP alone +13.5 %: 9.45M halfwords recorded for 0 traps), Mario Tennis +0.7 % (alone +12.0 %), Doom -16.3 %, Duke -11.7 % (hashes = SMC_SAME_SKIP). EXEC_STALE_CHECK 0 on 7 fixtures (default and 2048-halfword window) and on 35 corpus ROMs x 3000 f; mutant `SMC_SAME_ADAPT_MUTANT_NOREC` caught by `tools/drprof/synth/xs_norec.s` (twin, 20 f: mutant EXEC_STALE hits=1, real 0). Release with the switch off: ELF 241ee67a / EBOOT 52fb7c71 (unchanged). |
| `SMC_RETIRE_WINDOW` | root make | bound `flush_translation_cache_ram_range`'s tag walk by the longest live block extent since the last full RAM flush instead of `MAX_BLOCK_SIZE*4`. | Prototype. Oracle IDENTICAL on 7 fixtures; twin -6..-12% on SMC games, 0 elsewhere. Relies on every live block having a recorded extent (see the doc). Hardware A/B staged with `DISPATCH_CACHE`. |
| `DRPROF_HW` | root make **and** `-DDRPROF_HW` frontend | phase word `reg[40]` + a 1 kHz sampler thread; `EVT drprof` windows | Bench only (`GBADHOC-DRPROF` rig, arms A-SAMP/H-SAMP). PPSSPP: hash-identical to base, +4% cost. Refused in a release. |
| `DRPROF_TWIN` | `tools/drprof` only | translation-time class map + zone markers for the qemu plugin; emitted code unchanged (`dr_oracle.py base base-notwin` IDENTICAL) | Host twin only. Refused in a release. |
| `DRPROF_NEGCTL` | `tools/drprof` only | +1 cycle per Thumb instruction: the oracle's negative control | Must FAIL the oracle; it does, on all 7 fixtures. |
| `platform=drprof-mipsel` | root make | the PSP's translator as a static Linux mipsel archive for qemu (`tools/drprof/Makefile`) | Host only. |

`tools/build.sh` takes `CORE_EXTRA=...` / `FE_EXTRA=...` for one-off variants of
a non-release profile; it refuses them with `release`.

### The JIT code discipline (2026-10-01)

All OFF by default; `docs/JIT-CODE-DISCIPLINE.md` has the design and the
measurements.

| switch | where | what it does | status |
|---|---|---|---|
| `JIT_CODE_DISCIPLINE` | root make | code zones that own whole cache lines, one writer API recording every code write, one publish point (D writeback + I invalidate of exactly the recorded lines), whole I invalidate after every ownership change; replaces 8b48b48's whole-I on every ROM publish | Oracle IDENTICAL on 7 fixtures + a 32-reload soak; PPSSPP IDENTICAL (AW2, H&S resident); audit 0 unrecorded writes; twin cache checker 0 stale executions under every model; twin −0.0..+0.3% instructions. Off: release EBOOT byte-identical. Hardware plan staged, not run. |
| `JIT_CODE_CHECK` | root make | zone asserts on every record and every lookup return (stderr) | Twin/desktop. 0 violations. Refused in a release; needs the discipline. |
| `JIT_CODE_AUDIT` | `tools/drprof` only | shadows both zones; flags any code word changed without a record | 0 on 7 fixtures + soak; its negative control `JIT_CODE_AUDIT_NEGCTL` (one record dropped) FAILS: 60,672 on H&S heavy. |
| `JIT_CODE_AB` | root make, harness | the original sync model compiled in beside the discipline; harness key `jit_code_mode` 0 legacy / 1 + 8b48b48 / 2 discipline / 3 no ownership invalidate: one binary per hardware A/B | Oracle and PPSSPP IDENTICAL in every mode. Refused in a release. |
| `JIT_SYNC_STATS` | `tools/drprof` only | counts of the original sync path | Twin only. Refused in a release. |
| `LAYOUT_PAD_TEXT` / `LAYOUT_PAD_BSS` | `psp/Makefile` (via `tools/build.sh` `PSP_EXTRA=`) | link `psp/layout_pad.S` first so every object moves by N bytes | Hardware layout sweeps. `PSP_EXTRA` refused with `release`. |

---

## Removed

Deleted in 4.6.0: rejected, superseded or spent, and in no oracle, mutant or Lab
script. The code is in the 4.5.0 tag.

| switch | what it was |
|---|---|
| `GATE_NOCHECK`, `GATE_ONLY_FULLFLUSH`, `GATE_HIGH_ONLY`, `NO_GATE_TAG_FIX`, `NO_BL_FIX` | The 2026-09-24 gate-layout A/B arms: gate exits past the cycle check, endpoint gates without partial retirement, the one-gate layout, and the pre-fix behaviour of the gate-word tag and BL fixes. Their question is answered above. |
| `SMC_REUSE_ARMING` | bb8b7b2's arming window for `SMC_REUSE`, kept as a cost knob. The spare invariant made reuse exact everywhere, so no arming was left to do. |
| `SMC_SKIP_SAME` | **White-screens Heart & Soul at boot** — the idempotent-store compare is wrong. Measured dead on the merits too: at the H&S hot word 0x0300168c the stored value changed 11,613 times against 24 identical, so a working filter would remove 0.2 % of the flushes. `SMC_SAME_SKIP`, which compares what each translation read, is the idea done right. |
| `SMC_PARTIAL` | Superseded by `SMC_PARTIAL_SAFE`. Its block-copy detector is the ancestor of the current writer fingerprint. |
| `SMC_PARTIAL_SAFE_FRAMEFULL` | One full flush per frame regardless. Never measured; no comment in the Makefile. |
| `SMC_PARTIAL_SAFE_NO_TRAMP` | Selective retirement without the trampoline. Never measured; no comment. |
| `SMC_PARTIAL_DIRECT_GATE` | Patch the gate fall-through directly instead of through the dispatcher. Never measured; no comment. |
| `SMC_PARTIAL_SAFE_CONTROL` | The A/B control arm: selective invalidation off, everything else in place. |
| `SMC_GATES_CODED` | Demands the write address itself already be a block start, which rejects `0300168c` because at the moment of the write that address is *interior* to its block. 59.9 → 46-58 fps. |
| `SMC_GATES_SNAP` | Snaps backwards to the block head: valid, but the block starting at that head still spans the SMC region, so isolation is lost. 1.43M flushes, 31-48 fps, against 116k and a locked 59.9 for add-only. |
| `SMC_GATES_SNAPF` | Snap the gate *forward* to the first proven boundary at or after the write, which is the only variant designed to keep validity **and** isolation. Never measured. The most interesting unexplored option here. |
| `SMC_GATES_CLUSTER` | Add-only plus 2.0.2's cluster grouping, without its eviction — keeps the half that cut dispatcher lookups (196k → 59k) and drops the half that crashed H&S. Never measured on hardware. |
| `SMC_SCAN_SPCLAMP` | End `scan_block` at the stack pointer instead of the top of IWRAM, so scans stop tagging the live stack as code (10122 flushes measured in one battle from exactly that). Never measured. Plausible performance win. |
| `SMC_SP_CHECKED` | Send SP-based block stores down the general path so their final word reaches the SMC check; Thumb `PUSH {..., LR}` also uses the checked helper for LR. Never measured. Does not check every word in a multiword store, so it is only a partial coverage fix. Likely costs speed on a hot memory path; measure on hardware. |
| `STATIC_ALU_PC` | root make (`STATIC_ALU_PC=1`, MIPS only; needs `DISPATCH_CYCLE_CHECK`) `add pc, pc, #imm` / `sub pc, pc, #imm` with S clear (any condition) jumps to a constant (instruction + 8 +/- imm). Translated as the direct branch a `b`/`bcc` to that address gets, instead of an indirect jump through `mips_indirect_branch_arm` (dispatch-cache probe or lookup on every taken pass). It is always linked as an external exit to the block that starts at the target, which is the block the dispatcher found, so the scan, block extents and cycle-update points do not change. Cycle charging matches (conditional: neither path charges the pending count; unconditional: both do, then both run update_gba with the target when the budget is spent). Not converted: S set, an unaligned target, a target in another region, more than 8 in one block. `STATIC_ALU_PC_MUTANT` (oracle test) biases the target by one word. cpu-levers 2026-10-26. Off by default. Guest state is bit-identical while the RAM translation cache is not under pressure (twin, Duke with an 8 MiB RAM cache: 600 f identical). At the shipped 512/384 KiB the different code size moves full-flush timing, which moves gated block layouts (Duke diverges intermittently from f54, reconverges), the same class as any codegen change. Twin: Doom -0.36 %, Duke -1.04 % core work (disp -7.8k / -21.6k host instructions per frame). The mutant is caught on Doom and Duke. |
| `NO_GAME_SPEED_HINTS`, `NO_GAME_IDLE_HINT`, `NO_GAME_GATE_HINTS` | Ignore the game table's idle-loop and gate seeds. `gba_memory.c` keeps what the twin study found them worth. The table stays. |

---

## The rule for adding one

A switch that ships goes in `gpsp_features.h` (`psp/fe_features.h` for the frontend),
and only there, with `tools/flags.lock` updated in the same commit. A switch needs
three things or it becomes another entry in the "no recorded verdict" table:

1. A comment **immediately above its own `ifeq`**, saying what it does and what
   it would prove.
2. An entry here once it has been measured, with the number.
3. A `gpsp_profile.h` check if it must never reach a release, or if it silently
   shadows another switch.
