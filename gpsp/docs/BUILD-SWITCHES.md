# BUILD SWITCHES — the inventory

What a PSP build is made of, in four parts:

1. **[Ships](#ships)**: every switch that is on in the release, as one table per
   header (`gpsp_features.h` for the core, `psp/fe_features.h` for the
   frontend), plus the make features that choose what is linked.
2. **[Measurement tools](#measurement-tools)**: diagnostics, oracles, mutants and
   profile defines that are kept because a profile, a tool, a test or the Lab
   uses them. None of them is in a release; `gpsp_profile.h` refuses the ones
   that must never be.
3. **[Removed](#removed)**: deleted switches, which `tools/check_flags.py` keeps
   out of the tree.
4. **[The rule for adding one](#the-rule-for-adding-one)**.

**Why this exists.** A flag with no verdict is worse than no flag: it looks like
an option. A few unreferenced experiments and diagnostics are still in the
source and are scheduled for deletion (code-health tier A); they are deliberately
not described here, so nothing below should be read as "there is a flag for that".

## How the shipped set is built

**`tools/release.sh X.Y.Z` makes a release** (docs/RELEASE.md), and
**`tools/build.sh release`, with no environment, is the release build.** The core
switches that ship are lines in `gpsp_features.h`, which the root Makefile
force-includes (`-include`) into every core object of `platform=psp1` and
`platform=drprof-mipsel`, assembler included. The frontend's constants are lines
in `psp/fe_features.h`, force-included by `psp/Makefile`: the `ND_*` transport
profile always, `VID_TRIPLE`, `PLAY_INPUT_FRESH` and `PLAY_FF_FLIP_PACE` when the
build is `GPSP_PLAYABLE`. No recipe lists any of them, and `tools/flags.lock` pins
all of them (see **The drift check** below).

The two sides do not share defines. The frontend never tests a core feature (it
would always read as off there; core features reach it as weak symbols) and the
core never tests a frontend one.

Other platforms never see the headers. `platform=unix`, the SDL desktop build and
the host tests compile these switches off, so **the `#else` paths are not dead
code** and stay until the desktop build bakes them too.

| to do this | write this |
|---|---|
| one feature off, an A/B arm | `CORE_EXTRA="XLAT_PURE=0" tools/build.sh harness` (root make `XLAT_PURE=0`, which is `-DGPSP_NO_XLAT_PURE`). A release refuses `CORE_EXTRA`. |
| a different value | `CORE_EXTRA="SMC_SINGLE_HOT=32"` |
| the link, the rewind, pinning off | `GBA_LINK=0`, `REWIND=0`, `LAYOUT_PIN=0` (both makes get the value); `INPUT_FRESH=0`; `LAYOUT_PAD_BYTES=N`; `ONLINE=0`, `MGIFT=0` (frontend only) |
| a frontend value | `FE_EXTRA="-DND_WINDOW=128"`: a `-D` wins over `psp/fe_features.h`; `-DGPSP_NO_VID_TRIPLE` drops a playable default |
| a twin variant | `VDEFS` is the complete list: `tools/drprof/Makefile` defaults to `FEATURES=explicit` (`-DGPSP_FEATURES_EXPLICIT`, the header defines nothing), so a switch a variant omits is off and `-UDISPATCH_CACHE` works |
| a twin of what ships | `FEATURES=baked tools/drprof/dr_build.sh ship`; add `-DGPSP_NO_NAME` to drop one |
| one-off variants of a non-release profile | `tools/build.sh` takes `CORE_EXTRA=...` / `FE_EXTRA=...` / `PSP_EXTRA=...`; it refuses them with `release` |

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
build whose list is not exactly the header's. `frontendDefines` is resolved the
same way (`make -C psp print-features`): the profile's defines plus what
`psp/fe_features.h` gave that make line.

**The drift check.** `tools/check_flags.py` needs no build and takes a second;
`--compile` adds what the compilers resolve (docker), `--build DIR` a release
manifest. `tools/make_release.sh` runs the first and last and `tools/release.sh`
all three. It fails when:

| area | what drifted |
|---|---|
| `lock` | a feature list, make default (the layout pads included) or profile recipe differs from `tools/flags.lock` |
| `recipes` | a profile, makefile or wrapper restates a baked feature, or a force-include is gone |
| `removed` | a switch in **Removed** below is back in an `#if`, a `-D` or a make conditional |
| `dead` | something is baked or passed and no source tests it |
| `sides` | the frontend tests a core feature, or the core a frontend one |
| `docs` | a baked switch has no line in this file |
| `oracles` | the twin's tables or `dr_build.sh` name a `-D` no source mentions |

Changing a shipped flag is deliberate: edit the header, run
`tools/check_flags.py --update`, and commit the lock with it.

---

## Ships

### Core: `gpsp_features.h`

Every row is on in every PSP profile (release, lab, harness, diagnostic, soak) and
in the twin's `FEATURES=baked`. Changing one changes emulation or speed. The
order is the header's. "Exact" means the same guest state as without it (speed
only); the root Makefile's comment above each `ifeq` and the cited doc have the
measurements. Values other than 1 are given.

| switch | what it does |
|---|---|
| **SMC translation gates** | |
| `SMC_GATES` | put a translation gate at a self-modified address so later blocks split there. Deliberately independent of `SMC_PARTIAL*`: gates bought Unbound its speed and also broke its audio, so the two must be testable apart. |
| `SMC_GATES_SIMPLE` | the 2.0.1 add-only gate rule. Once placed, a gate never moves, so an already-translated block can never disagree with the table. 2.0.2's evicting rule is what crashed Heart & Soul (see **The rejected evicting rule** below). |
| `SMC_GATES_RANKED` | promote after 64 observed writes when at least half changed the word (see **What `SMC_GATES_RANKED` requires** below). Counts are lifetime within one ROM. |
| `SMC_GATE_BITMAP` | same gate table and boundaries, tested through a bitmap instead of comparing every live gate per compiled instruction. |
| `SMC_GATE_CHARGE` | a block that ends at a translation gate subtracts its pending cycles before the gate's indirect exit; upstream's gate exit charged nothing, so code in front of a gate ran for free (the Heart & Soul mixer's audio split). Only gated blocks change. |
| `SMC_GATE_SELFPATCH` | the ranked rule also promotes a store patching the code just ahead of itself, even re-storing the same bytes, and gates the lowest word written (Mario Golf's table gate). Needs `SMC_GATES_RANKED`. |
| `ENTRY_GATES` | when the RAM translation cache is exhausted, every RAM block start becomes a gate and the gated block jumps straight to its continuation, so a routine entered at many points is translated once (Driver 2: 887 cache-full flushes -> 1 at 384 KiB, 24-35 -> 59.7 fps on a PSP-3000's small tier). Armed only by an exhaustion flush. Requires `SMC_GATE_BITMAP`. |
| `ENTRY_GATES_RANGE` | the exhaustion-time gate harvest walks only the tag range the following full flush clears. Needs `ENTRY_GATES`. |
| `ENTRY_GATES_FAST` | the entry-gate harvest tests four tag slots per load. Exact. |
| **Selective SMC retirement** | |
| `SMC_PARTIAL_SAFE` | after a reported SMC trap, selectively retires ranges for a bounded ARM STMIA/LR shape (M4A-motivated, not ROM-specific); other writes use the full flush. See the M4A range-gate caveat below. |
| `SMC_PARTIAL_STABLE_THUNK` | a permanent per-block RAM entry thunk, so retiring a block redirects the thunk rather than orphaning inbound links. |
| `SMC_PARTIAL_DIRECT_LINKS` | keep RAM->RAM links direct; route only persistent ROM->RAM links through an island. Requires the stable thunk. |
| `SMC_RETIRE_WINDOW` | bound `flush_translation_cache_ram_range`'s tag walk by the longest live block extent since the last full RAM flush instead of `MAX_BLOCK_SIZE*4`. Oracle IDENTICAL on 7 fixtures; twin -6..-12 % on SMC games. |
| `SMC_RETIRE_LOCAL` | retire walks start at a per-64-byte bound on where a covering block can start. Same retirements, shorter walks. Needs `SMC_RETIRE_WINDOW`. |
| `SMC_RETIRE_LIST` | `smc_retire_span`'s validation walk lists the block-start tags and the retire pass visits only those. Same retirements. |
| **Single-store and block-store SMC coverage** | |
| `SMC_PARTIAL_SINGLE` | with `SMC_PARTIAL_SAFE`, also admit single-word ARM stores from IWRAM code into IWRAM to selective retirement (`smc_writer_safe_range`). |
| `SMC_SINGLE_THUMB`, `SMC_SINGLE_HALF` | the same from Thumb STR/STRH/STRB and from ARM STRH single stores. |
| `SMC_SINGLE_ANYPC`, `SMC_SINGLE_EWRAM` | the same for writers outside IWRAM (ROM, EWRAM, BIOS) and for EWRAM targets. |
| `SMC_SINGLE_UNTAG` | also lift dead block-start tags in the written range. |
| `SMC_SINGLE_HOT` | **64**: the widened single-store classes engage only after this many of their traps (or once selective retirement is already active). |
| `SMC_STM_RANGE` | a short writeback STMIA from IWRAM into IWRAM retires its own span instead of a full RAM flush (FIFA 07's code generator). |
| `SMC_STM_CHAIN` | a non-final block-store word landing on tagged RAM marks the next word, so the final word's checked store traps (the STM hole). Needs `SMC_STM_RANGE`. |
| `SMC_STM_WIDE` | widens `SMC_STM_RANGE` to 2-16 registers and lets such a store activate selective retirement (Hot Wheels). |
| `SMC_STM_AHEAD` | `SMC_STM_WIDE` retires forward over the tagged run after an advancing copy's write. Needs `SMC_STM_WIDE`. |
| `SMC_STM_FIXED` | the same span retirement for a fixed-base `STMIA Rn,{..}` (no writeback), with no forward widening: ipatix's ImprovedSoundMixer pastes its IWRAM resampling loop this way (The Morrow's Golden Country, ~4 full RAM flushes a frame without it). Changes block boundaries, so timing; not exact. Needs `SMC_STM_RANGE`. |
| **Unchanged-code filters and in-place patching** | |
| `SMC_SAME_SKIP` | for every IWRAM halfword a RAM scan reads, record the value it read; a single-store SMC trap in IWRAM then skips `smc_retire_span` when every block it would retire matches its records over its whole source extent (retiring it would only retranslate identical code). The trap and re-dispatch are unchanged. Not the shelved `SMC_SKIP_SAME` (see Removed). Twin: Doom core -15.9 %, Duke -11.4 %; `EXEC_STALE_CHECK` 0 hits; its mutant `SMC_SAME_SKIP_MUTANT_NOCHECK` is caught (111k stale executions on Doom in 600 frames). Changes timing; judged by the Lab gate's replayed lockstep. |
| `SMC_SAME_ADAPT` | record halfwords for `SMC_SAME_SKIP` only while it pays: recorded in bulk per translated block (`xs_note_block`); after each 65536 recorded halfwords with fewer than 256 skips, recording and checking stop; a re-probe follows single-word retirements. Exact like `SMC_SAME_SKIP` (it only skips less). Hot Wheels +0.00 % (`SMC_SAME_SKIP` alone +13.5 %). Mutant `SMC_SAME_ADAPT_MUTANT_NOREC` is caught by `tools/drprof/synth/xs_norec.s`. |
| `SMC_SAME_REPROBE` | **1024**: `SMC_SAME_ADAPT`'s first re-probe threshold, in single-word retirements while off. |
| `SMC_IMM_PATCH` | an IWRAM ARM immediate the game keeps rewriting is emitted in a fixed lui/ori form and patched in place instead of retiring the block (Doom II, Duke, Ice Nine). Needs `JIT_CODE_DISCIPLINE`. |
| `XS_HEAP` | the `SMC_SAME_SKIP` and `SMC_RETIRE_LOCAL` tables (up to 69 KiB) come from one heap block, so the hot arrays after them in `.bss` keep their D-cache sets. |
| `XS_WALK_PAIR` | `xs_blocks_current` tests two tag slots per aligned load. Same blocks, same order. |
| `XS_NOTE_FAST` | `xs_note_block` records eight halfwords at a time. Exact. |
| **Translated code size** | |
| `PCARG_SIDE` | a single load or store calls its stub without the PC-argument word (about 8 % of hot translated bytes); the stubs' slow paths recover it from a side table outside the translation caches (docs/PCARG-SIDE.md). Exact per access; smaller RAM code can move a full flush (timing). `PCARG_SIDE_PAD=1` (twin oracle only) pads every site back to the base's size. |
| **Translation reuse across full flushes** | |
| `XLAT_PURE` | `scan_block` clears every instruction's update-cycles slot, so a block's translation depends only on its own guest bytes (the precondition of reuse). Changes timing where a stale slot used to be 1. |
| `SMC_REUSE` | a full RAM flush keeps the native code; a RAM block whose guest bytes are unchanged is revived instead of translated again. Speed only; needs `XLAT_PURE`. |
| `SMC_REUSE_SPARE_KB` | **384**: the RAM code cache's spare, in KiB, that `SMC_REUSE` keeps. |
| `SMC_REUSE_MODE` | **3**: 0 off, 1 record only, 2 on, 3 on without cost rules (ships), as one initialised word so hardware arms share a layout. |
| **DMA onto translated code** | |
| `DMA_SMC_FLUSH` | an HBlank/VBlank DMA onto code tagged as translated full-flushes the RAM translation cache and re-dispatches, as the CPU-triggered DMA path always did; upstream `update_gba` discarded the alert, leaving stale translations live. |
| `DMA_SMC_RETIRE` | a DMA onto translated code retires exactly the tagged words it wrote instead of a full flush (Top Gear Rally's overlays). Needs `SMC_STM_RANGE`. |
| `DMA_SMC_ADAPT` | `DMA_SMC_RETIRE` only while it measures cheaper than the flush (probe and back off). |
| `DMA_ADAPT_V2` | `DMA_SMC_ADAPT`'s cost model re-priced from the twin; ties go to the full flush. |
| **Dispatch and translator speed** | (docs/DYNAREC-PROFILE.md) |
| `DISPATCH_CACHE` | `mips_indirect_branch_*` probe a {guest pc -> host entry} table in asm before `save_registers` and the C block lookup; filled by the C lookup, emptied by every flush entry point. The cycle check still runs first. Oracle IDENTICAL on 7 fixtures; twin -4..-23 % core instructions. |
| `DISPATCH_PRECISE` | SMC retirement forgets only the retired block's entry instead of emptying both tables; only canonical RAM pcs are cached (a mirrored pc shares the tag, not the key). Twin: Doom -4.6 %, Duke -4.7 %, state hashes identical. |
| `DISPATCH_ASSOC` | the dispatch cache's 4 KiB per ISA as 256 2-way sets instead of 512 direct-mapped slots; `mips_indirect_branch_dual` enters the ARM/Thumb probe; `DC_STUB_FILL` keeps `mips_stub.o(.text)` its old size so the pinned hot order does not move. Same entries, keys and invalidation points. Twin: C fall-throughs per frame Doom II 140 -> 55, Duke 193 -> 50, Ice Nine 83 -> 47. |
| `DISPATCH_LOOKUP_PC` | `lookup_pc` (I/O write epilogue, `update_gba` PC change, halt wake) probes the dispatch cache when no JIT publish is owed, else makes the C lookup as before. 98-99 % hits on the twin. Off by itself under `JIT_CODE_AB` (gpsp_profile.h). |
| `XLAT_FAST` | translator host-work speedups; emitted code, tags and counters unchanged. |
| `JIT_COST` | the cost of producing translated code (docs/JIT-COST.md). Exact. |
| `JIT_CODE_DISCIPLINE` | code zones that own whole cache lines, one writer API recording every code write, one publish point (D writeback + I invalidate of exactly the recorded lines), whole I invalidate after every ownership change (docs/JIT-CODE-DISCIPLINE.md). Oracle and PPSSPP IDENTICAL; `JIT_CODE_AUDIT` 0 unrecorded writes. |
| `SPIN_SKIP` | the translator proves a backward branch closes a side-effect-free loop (loads without writeback, ALU ops, compares; every read register never written in the loop or written earlier in the same iteration); the taken back-edge asks `update_gba` whether to end the slice when every load address is RAM, palette, VRAM, OAM, ROM or a few event-only I/O registers. Anything else vetoes the loop. Not keyed on any address or ROM. Moves timing by less than one loop iteration per event; judged by the Lab gate's replayed lockstep. Oracle `SPIN_SKIP_VERIFY`; mutants `SPIN_SKIP_MUTANT_REGS`, `SPIN_SKIP_MUTANT_STORE`, `SPIN_SKIP_MUTANT_INSKIP`. |
| **Scheduler, interrupt and serial fast paths** | |
| `SCHED_FAST` | scheduler fast paths in `update_gba`, compiled in; the run-time `sched_fast` turns them on. Exact. |
| `IRQ_IO_FAST` | IE/IF/IME stores inline in the I/O store stub, compiled in; the run-time `irq_io_fast` turns them on. Exact. |
| `SERIAL_IDLE_FAST` | skips `update_serial`'s call chain with no transfer pending and no link client role. Exact. |
| `MSR_FAST` | CPSR writes that switch mode and SPSR restores bank the registers in `mips_stub.S` instead of calling C (OpenLara, IRQ dispatchers); the pending-IRQ case keeps the C path. Exact. Run-time kill switch `msr_fast`. Needs `FIQ_BANK_FIX`. |
| `HALT_HOP` | while the CPU is halted, one scheduler jump over the H-blank edges nothing can see (`main.c` `halt_hop_begin`); the run-time mask `timer_lazy` (bit 1 HALT_HOP, bit 2 HOP_DRAWN) starts at `HALT_HOP_DEFAULT` 6. Exact: corpus 35/35, pair-twin hashes equal to beta.4 on Kirby AM / NiDL. |
| **Guest correctness** | |
| `GBA_PC_MASK` | mask the emulated pc to 28 bits before the region lookup, as the GBA does. Not sufficient alone: a masked garbage target can land in a region the translator handles. |
| `BADJUMP_SAFE` | an unmappable guest pc soft-resets the GBA instead of `jr`-ing to it. The path it replaces crashes every time. |
| `DISPATCH_CYCLE_CHECK` | `mips_indirect_branch_arm`/`_thumb` call `mips_update_gba` when the cycle budget is spent, as direct branches do; without it a loop closed only by indirect branches or gate exits never completes a frame. `_dual` (BX) is not checked. |
| `FIQ_BANK_FIX` | bank FIQ's r8-r12 correctly in `set_cpu_mode` (OpenLara). |
| `BX_ALIGN_FIX` | a BX into ARM state with bit 1 set goes to the word below, as on the ARM7TDMI. |
| `BACKUP_BUS_FIX` | a 16/32-bit dynarec load from SRAM/flash returns the byte in every lane, as the 8-bit bus does. |
| `COND_ENTRY_FIX` | a branch into the middle of a run of same-condition ARM instructions gets its own condition check. |
| `NV_COND_FIX` | an ARM word with condition 0xF (NV) is never executed, as on the ARM7TDMI (gpSP crashed in the translator on one, issue #10). Blocks without NV words are byte-identical. |
| `BIOS_BOUND_FIX` | a region-0 pc above the 16 KiB BIOS is unmappable: exits to it go through the dispatcher instead of translating ROM pages as BIOS code (issue #10). |
| `XLAT_LAST_TRY_LAZY` | the last of a lookup's four translation tries links every exit through the dispatcher, so a legal pc whose eager links filled a cache is not reset by `BADJUMP_SAFE`. |
| `RAM_MIRROR_FIX` | a RAM pc reached through a mirror alias shares the canonical tag, but each translation records the alias it was made at; a lookup at another alias flushes the RAM cache and retranslates, and a block exit to another alias is linked lazily. Code that really runs at an alias sees the alias's pc, as on the GBA (The Morrow's Golden Country, Pokemon MonoMyth: fixture mono_trainer_crash). Twin counters: `RAM_MIRROR conflicts= alias_xlat=`. |
| `TIMER_RELOAD_FIX` | a timer that has just overflowed reads its reload value, not 0 (Need for Speed Underground). Guest-visible. |
| `VRAM_EXEC` | translate and run code the game copied into VRAM (Big Mutha Truckers, Street Racing Syndicate, jsmolka nes.gba). |
| **Data placement** | |
| `HOT_DATA_GUARD` | the stub's hot data block (memory map, `reg`, palette/OAM, spsr, patcher tables) at a fixed D-cache set offset (`mips_stub.S`, `hot_data_guard.ld`). |
| `HOT_DATA_OFFSET` | **0x1b00**: where `HOT_DATA_GUARD` places the block in its 8 KiB way. |

**Open (known, not done):** an SMC coverage probe for the unchecked words of block
stores (P4 in the 2026-09-24 dynarec audit); a cycle check in
`mips_indirect_branch_dual` (BX-closed loops can still spin without a frame: it
needs the T-bit settled before an IRQ can be taken); and the I/O alerts that
non-final STM words drop (`mips_emit.h`, `strop == 3` in the I/O store stub).

**M4A range-gate caveat:** `smc_partial_install_range_gate()` inserts its
computed endpoints directly. It bypasses `SMC_GATE_EARNED` and the normal gate
filter, and those endpoints persist across soundtrack changes until a new ROM
loads. Selective retirement requires both normalized endpoints to be present; if
gate-table capacity leaves either one missing, it falls back to a full RAM-cache
flush. There is no hardware evidence that the old half-installed case caused a
freeze, or that a Heart & Soul soundtrack switch installs an invalid endpoint.

#### What `SMC_GATES_RANKED` requires

Two conditions, in `cpu_threaded.c`'s `smc_gate_earned`:

```
SMC_GATE_MIN_SAMPLE    64 writes of evidence
chg * 2 >= hits        at least half of them CHANGED the word
```

A staleness rule (`SMC_CAND_STALE_FRAMES`) was proposed and **withdrawn**; it is
not in the code. An earlier comment described an `SMC_GATE_PROMOTE_HITS`
threshold (default 256) that the code never had. Measured on the Heart & Soul
rival battle, over one 8000-flush window:

| address | same | changed | what it is |
|---|---:|---:|---|
| `0300168c` | 120 | 62953 | real code being patched — the gate that matters, 97.8% of all flushes |
| `03001404` | 66 | 1 | a constant re-stored into a code block — the address that crashed us |

That gap is structural, not tuned. Frequency was the wrong discriminator and
failed twice: an absolute threshold is outgrown by any cold address, and a ratio
to the hottest candidate degenerates because a promoted gate stops counting and
the maximum freezes. Both crashed.

#### The rejected evicting rule

2.0.2's evicting gate rule (no flag: the `#else` branch when `SMC_GATES_SIMPLE`
is off) reassigns a gate's address while blocks compiled against the old layout
are live, and gpSP's tag map holds one slot per halfword, so it cannot represent
the resulting overlap. This is the Heart & Soul crash. It stays in the source
for the desktop build until that bakes `SMC_GATES_SIMPLE` too.

#### Tunables and core defaults outside the header

| name | what it is |
|---|---|
| `ROM_BUFFER_SIZE` | **15** (root Makefile; `ROM_BUFFER_SIZE=N` overrides): the default of the run-time ROM page-cache cap `gamepak_buffer_cap`. CONFIG.INI `rom_resident = 1` (default since 3.0.0) raises the cap to the cart's size when every block fits while the 1 MiB post-load reserve is held (64 MiB consoles only; a 1000 stops at ~13). See docs/ROM-RESIDENCY.md before changing it. |
| `GATE_SLOTS`, `GATE_RATIO`, `CLUSTER_BYTES` | root make overrides of `MAX_TRANSLATION_GATES`, `SMC_GATE_RATIO`, `SMC_GATE_CLUSTER`. Tuning knobs, unset in every profile. `CLUSTER_BYTES` 128 is sized to H&S's M4A layout (two loop copies 152 bytes apart). |
| `ROM_BRANCH_HASH_BITS_SMALL`, `ROM_BRANCH_HASH_BITS_LARGE` | the ROM block hash (`rom_branch_hash`, gpsp_config.h): 14 bits (64 KiB) on every PSP build, 16 only without `SMALL_TRANSLATION_CACHE` (other platforms, the twin's 10 MiB model). `#ifndef` overrides for the twin's sizing studies; a shipped build never sets them. |
| `DISPATCH_CACHE_BITS` | 9 (cpu.h): 512 entries per ISA. |
| `ME_AFFINE_LINES`, `ME_MIDFRAME_LOG` | video.h, default 1: the Media Engine capture records the affine reference counters per line, and logs palette/OAM/VRAM written during the visible lines for the engine to apply line by line (docs/ME-MIDFRAME.md). |
| `OVERCLOCK_60FPS` | upstream's 60 fps overclock build. Untouched by this project; in no profile. |

### Frontend: `psp/fe_features.h`

Its comments keep the field logs behind each value.

| define | ships at | what it does |
|---|---|---|
| `ND_MAX_PAYLOAD` | 144 | largest reliable payload; netdrv refuses anything bigger, loudly |
| `ND_WINDOW` | 256 | ARQ window, and the per-peer reorder buffer |
| `ND_TXQ_CAP` | 384 | per-peer reliable backlog |
| `ND_RTO_MIN_US` | 200000 | retransmit floor |
| `ND_RTO_MAX_US` | 2500000 | retransmit ceiling; must not sit below what the estimator computes |
| `VID_TRIPLE` | 1 | three framebuffers instead of two. Fixes tearing; its fast-forward black-bar artifact is resolved for all three presets by `g_fb_filled` plus `vid_swap()`'s display-ownership read-back (docs/FF-ARTIFACT-FIX.md). |
| `PLAY_INPUT_FRESH` | 1 | the low-latency pad re-peek on by default (`INPUT_FRESH=0` on `tools/build.sh` turns it off) |
| `PLAY_FF_FLIP_PACE` | 7 | fast-forward presentation pacing (docs/FF-PRESETS.md): bit 0 no early flip over an unlatched one under 3x, bit 1 the Smooth/Unlimited hold, bit 2 3x through the draw gate under a 3-frames-per-vblank ceiling. Presentation only. `-DGPSP_NO_PLAY_FF_FLIP_PACE` = 0 |

### Make features: what is compiled and linked

These change what is linked, so they stay make variables; `tools/flags.lock` pins
their defaults.

| variable | default | what it does |
|---|---|---|
| `GBA_LINK`, `REWIND`, `CHEATS`, `LAYOUT_PIN` | 1 (both makes) | the GBA link pair, rewind, the per-game cheats menu (docs/CHEATS.md) and the pinned layout. `tools/build.sh` passes one value to both makes. `LAYOUT_PIN=0` is the pre-pinning link, byte for byte. |
| `ONLINE` | 1 (psp/Makefile) | experimental online play (docs/ONLINE-TUNNEL.md), compiled in and off until Settings > Online play is turned on; `ONLINE=0` compiles it out. |
| `MGIFT` | 1 (psp/Makefile, tools/build.sh) | Mystery Gift, the Wonder Card receiver for FireRed/LeafGreen/Emerald (psp/mgift_*.c). `MGIFT=0` compiles out its objects, the Wireless > Mystery Gift row and screen, the Controls binding row, the chord and the per-frame poll; the infrastructure Wi-Fi bring-up (mgift_net.o) stays while online play is in. MGIFT=0 is a variant arm (the lock is reported, not enforced) and would need its own re-pin to ship (`main` shrinks 684 B). |
| `RFU_SCHED` | 0 (psp/Makefile, tools/build.sh); `tools/release.sh` sets 1 for a `-beta`/`-nightly` version, and build.sh passes one value to both makes | scheduled ticks for the emulated Wireless Adapter (docs/RFU-COUPLING.md): each console publishes its frame's adapter traffic with a tick, the partner's game gets it D = 2 frames later. Replaces frame pace, shedding, the overflow hold and supersede while it runs, and changes the wire format: netdrv refuses a session between a ticks build and one without (`ND_FEAT_RFU_SCHED`). Harness key `rfu_sched` = D, 0 = off. The beta kits are built with it; whether 5.0.0 ships it is an owner decision. |
| `ME_CATCH` | 1 in the release and harness64 profiles | links `psp/me/catcher.c` and `catcher_vec.S` into the Media Engine PRX: an exception handler that draws EPC / BadVAddr / Cause / GPRs / nearby memory / the ME mailbox into all three framebuffers on any unhandled CPU exception. The EBOOT does not change and every existing PRX symbol keeps its address (the two entry points are wrapped). Only effective when the ME boots. |
| `TGB_LAZY_TIMER` | 1 (psp/Makefile) | scoped to the GB core's objects. |
| `PSP_LARGE_MEMORY` | 1 (psp/Makefile); 0 in harness, lab, soak | the 64 MiB memory layout flag in the EBOOT. |
| `PSP_PIXFMT` | psp5650 (both makes) | a variable because it reaches the ME sub-make. |
| `HOT_DATA_GUARD` | 1 (psp/Makefile) | the link script that follows the core's `HOT_DATA_GUARD`. |

**The layout pads** (`LAYOUT_PAD_*` in psp/Makefile, every value pinned by
`tools/flags.lock`; docs/LAYOUT-PINNING.md "Keeping the pads" says what each
holds): `LAYOUT_PAD_BYTES` (after `main`), `LAYOUT_PAD_RO_BYTES` (`.rodata`),
`LAYOUT_PAD_FE_BYTES` (after stb_impl.o), `LAYOUT_PAD_GB_BYTES` (after both GB
cores), `LAYOUT_PAD_CORE_BYTES` (before instance A's core archive),
`LAYOUT_PAD_MAIN_BYTES` (in front of `main`), `LAYOUT_PAD_LATE_BYTES` and
`LAYOUT_PAD_MLU_BYTES` (the ME capture's helpers), `LAYOUT_PAD_XLAT_BYTES` (in
front of A's `translate_block_arm`), and instance B's `LAYOUT_PAD_BSCAN_BYTES`
(B's `update_scanline` held at the renderer's size) and `LAYOUT_PAD_BXLAT_BYTES`
(after B's `translate_block_arm`), which psp/gbalink_b_stubs.c refuses to compile
without. A lab build takes the committed lab set, `tools/layout/lab-pads.env`
(`LAB_PADS=0` for the release pads); a sweep overrides it through
`LAYOUT_PAD_BYTES=` and `PSP_EXTRA=`, which build.sh refuses for the release
profile. `tools/layout/layout.lock` records where the pads put the symbols that
carry the layout; build.sh fails a release, or a lab build with the committed
set, that differs (`LAYOUT_LOCK=0` lets a lab build through during a re-pin,
never a release). The manifest records the result as `layoutLock`, and
tools/make_release.sh packages only `release:ok`. `HOT_ORD` (default
`layout/hot.ord`) names the ordering file.

**Build plumbing, not experiments:** `HAVE_DYNAREC`, `CROSS_COMPILE`,
`FORCE_32BIT_ARCH`, `SMALL_TRANSLATION_CACHE`, `RUNTIME_JIT_CACHE`,
`FRONTEND_SUPPORTS_RGB565`, `GPSP_PROFILE`, `platform=drprof-mipsel` (the PSP's
translator as a static Linux mipsel archive for qemu, `tools/drprof/Makefile`).
`RUNTIME_JIT_CACHE` (always on for `platform=psp1`) sizes the translation caches
at startup, so one EBOOT serves every model: the SMALL tier (2 MB ROM / 384 KB
RAM code, static) is what a PSP-1000 always gets; `retro_init` tries one `malloc`
of the LARGE tier (10 MB / 384 KB) plus 20 MB of headroom and keeps the static
arrays if it does not fit. Harness builds log `EVT jit_cache tier=... reason=...`
and `EVT heap_budget`; `jit_small = 1` in `.gpsp-harness.ini` forces the SMALL
tier. `BIG_JIT=1` (the old compile-time LARGE tier) is refused by the root
Makefile.

**The SMC profile switch (ADR-0030; main.c).** `smc_blk_watch == 0` turns the
store/DMA profile and the RAM flush timer off at run time; the code stays linked,
so the pinned layout does not depend on it. A release starts it at 0, every other
profile at `SMC_BLK_WATCH_DEFAULT`. `-DGPSP_SMC_PROFILE` keeps the profile on in
a release; `-DGPSP_NO_SMC_PROFILE` gives any build the release behaviour (the
twin's `fr` variants; for a lab build it goes in through
`CORE_EXTRA="EXTRA_CFLAGS=-DGPSP_NO_SMC_PROFILE"`, a core define). Verify an arm
from its ELF: `psp-objdump -s -j .data` at `smc_blk_watch` reads 0 with the
profile off, `0x03007d00` with it on; the arms are otherwise layout-identical.

---

## Measurement tools

None of these is in a release. Each is kept because a profile, a tool, a test or
the Lab names it; the "used by" column says which. `gpsp_profile.h` refuses the
ones marked "refused in a release".

### Profile defines (frontend)

| define | meaning |
|---|---|
| `GPSP_PLAYABLE` | build a binary for someone to play (ADR-0067); every `tools/build.sh` profile sets it. Among other things it points the harness ini at a filename that cannot exist, so a leftover `.gpsp-harness.ini` on a player's card is inert. |
| `GPSP_KEEP_TELEMETRY` | keep the EVT logger. Without it `fe_evt()` compiles to `((void)0)` and does **not** evaluate its arguments (ADR-0067b). |
| `GPSP_PERF_RIG` | the performance harness: autopilot plus perf windows. Requires `GPSP_KEEP_TELEMETRY`. |
| `LAB_HARNESS` | the lab profile's Lab-driven harness (psp/lab_harness.c). |
| `GPSP_ROMLOAD_DIAGNOSTICS` | ROM-load instrumentation. Refused in a release. |
| `GPSP_STANDBY_NOTES` | standby/resume tracing (the Lab names it). |
| `GPSP_STALL_RECORDER` | hard-freeze bisect arm: the harness stall watch (a 0x10-priority thread that wakes once a second) in a playable build, always on at 6 s. Writes `log/stall.txt` (main-thread state, loop phase, ME mailbox) only when the main loop has not iterated for 6 s. Adds two stores per loop iteration. |
| `GPSP_CATCH_SELFTEST` | L+R+UP+TRIANGLE stores to address 0, so the `ME_CATCH` screen can be seen working once. Refused in a release. |
| `GPSP_HEAP_TRACE` | `HEAP_TRACE=1` on psp/Makefile: the heap census (psp/heap_trace.c). |
| `GPSP_MEM_BUDGET` | the memory-budget instrumentation (docs/MEMORY-BUDGET.md). |
| `JIT_COST_KEYS` | the harness ini keys for `JIT_COST`'s pieces. In no profile: Lab candidate builds add `FE_EXTRA=-DJIT_COST_KEYS`, so compare a Lab EBOOT only with one built the same way. |
| `FX_SWEEP` | with `GPSP_PERF_RIG`: the harness steps through the display effects and times the ME renderer per step (`fx_sweep_step`). |
| `ROM_HASH_CENSUS` | see below; needs the core half too. |

`ME_CATCH_NOREG=1`, `ME_PAD_RODATA=N` and `ME_PAD_BSS=N` (psp/me) are the
hard-freeze bisect's layout arms for the ME PRX, all appended after every
existing object: the first swaps the catcher's registration for the
already-imported release call (same size, nothing registered), the second moves
all of the ME's `.bss` by N (3856 reproduces the catch data addresses; 12528 with
`ME_CATCH=1` puts them at stock + 16 KB), the third moves only `_end`.

### Core diagnostics

| switch | what it measures | used by |
|---|---|---|
| `BADJUMP_REPORT` | `ms0:/badjump.txt`: registers, gate table, previous block entry; re-enables `badjump_recover`'s file append | `diagnostic`, `soak` profiles; refused in a release |
| `SMC_WRITE_HISTO` | `ms0:/smchisto.txt` every 4000 flushes: write addresses and storing pcs bucketed, plus unmappable block exits contained | root make; refused in a release |
| `XLAT_DEPTH_PROBE` | the deepest nested block translation and the lowest frame address at translation entry (`xlat_depth_max`, `xlat_sp_min`), printed by the `core_health` EVT line as `xlat_depth` / `stack_used`. Each nesting level costs ~380-430 B of the 256 KB main-thread stack. | `soak`, `diagnostic`; refused in a release |
| `IRQ_INTEGRITY_CHECK` | snapshots r0-r14, CPSR and the return address at every IRQ entry and compares them at the matching return; a mismatch appends to `ms0:/irqchk.txt` and dumps guest RAM for the first two. `IRQ_INTEGRITY_SUMMARY` adds a heartbeat line every 16384 returns (PPSSPP only). | `soak`, `diagnostic`; refused in a release |
| `STALE_CHECK` | snapshots the guest words of every translated IWRAM block and reports a block still live after its source changed without an SMC event (`ms0:/stale.txt`). `STALE_DISPATCH_ONLY` restricts the per-dispatch work to the block entered; `STALE_CHECK_ALL_ENTRIES` routes every RAM-bound exit through the dispatcher so every entry is seen (changes timing); `STALE_ABORT` makes a stale entry abort (the twin/corpus oracle's "assert" kind). Far too slow for hardware. | PPSSPP; refused in a release |
| `EXEC_STALE_CHECK` | every executed RAM instruction re-reads its guest opcode and compares it with the one it was translated from; counts to stderr (`EXEC_STALE_ABORT=1` aborts on the first). The 0-hit counts behind the `SMC_SAME_*` verdicts. | twin |
| `ROM_HASH_CENSUS` | walks `rom_branch_hash` at every 10th heartbeat and every other `core_health` EVT and logs `EVT rom_hash` (chain depths, buckets used, walk cost). Both halves or it does not link: `CORE_EXTRA="EXTRA_CFLAGS=-DROM_HASH_CENSUS" FE_EXTRA=-DROM_HASH_CENSUS`. Cold code: the arm's layout differs from the lab's. | in no profile |
| `SMC_CLASS_PROBE` | classifies every SMC trap by writer shape to `ms0:/smcclass.txt` | root make |
| `SMC_WRITER_PROBE` | dumps the distinct ARM block-store shapes `smc_writer_safe_range()` is asked about to `log/writer.txt` | root make |
| `SMC_FALLBACK_STATS` | counts why SMC traps fall back to a full RAM flush (`EVT core_health smcfb=`) | root make, tools/drprof |
| `SMC_RETIRE_WINDOW_CHECK`, `SMC_RETIRE_LOCAL_CHECK`, `ENTRY_GATES_RANGE_CHECK` | also walk what the bound skips and count (twin: abort on) a live block it would miss | root make, twin |
| `XLAT_SPLIT_DIAG` | the translator's scan/emit time split | root make |
| `TRACE_INSTRUCTIONS`, `TRACE_REGISTERS`, `TRACE_EVENTS`, `PC_RING` | interpreter/dynarec traces (`PC_RING`: the last 256k instructions, dumped when `$PC_RING_PC` is entered with r1 negative) | root make, twin |
| `RTC_PIN=<epoch>` | fixed RTC, so boot is deterministic across runs | gate-layout apparatus below |
| `STATE_HASH_TRACE` | `ms0:/statehash.txt`: per-frame ticks, PC, IWRAM and EWRAM hashes; `STATE_DUMP_LO/HI` add raw dumps | gate-layout apparatus |
| `UPDATE_TRACE_LO/HI` | `ms0:/utrace.txt`: every `update_gba` entry and every SMC event in the frame window; `UTRACE_DUMP_T/T0` dump RAM at exact tick values | gate-layout apparatus |
| `STALE_STACKNOTE` | log IWRAM translations above 0x03007000 (H&S runs ARM routines copied onto the stack) | gate-layout apparatus |
| `DRPROF_HW` | phase word `reg[40]` + a 1 kHz sampler thread; `EVT drprof` windows. Root make **and** `-DDRPROF_HW` frontend. PPSSPP: hash-identical to base, +4 % cost. | `GBADHOC-DRPROF` rig; refused in a release |
| `DRPROF_TWIN` | translation-time class map + zone markers for the qemu plugin; emitted code unchanged | twin only; refused in a release |
| `XS_COST_PROBE` | `XS_COST` / `RS_COST` counters on the twin's exit line (xs_blocks_current and smc_retire_span work) | twin VDEFS |
| `SMC_COST_VERIFY` | runs the reference `xs_blocks_current` and the retired-tag table's linear search beside the fast ones; `SMC_COST_VERIFY xs_mismatch= rt_mismatch=` on exit (0 on Hot Wheels, Doom II, Duke, Ice Nine, Payback) | twin VDEFS |
| `SMC_REUSE_DEBUG` | the twin's self-check of `SMC_REUSE`'s contract (tools/drprof/pcarg_check.py) | twin; refused in a console build |
| `JIT_CODE_CHECK` | zone asserts on every record and every lookup return | twin/desktop; refused in a release |
| `JIT_CODE_AUDIT` | shadows both code zones; flags any code word changed without a record (0 on 7 fixtures + soak) | twin only |
| `JIT_SYNC_STATS` | counts of the original cache-sync path | twin only; refused in a release |
| `JITCOH_PSP_CACHE` | runs the PSP cache-sync code against hooked kernel calls (docs/JIT-COHERENCY.md) | tools/jitcoh |
| `SPIN_SKIP_VERIFY` | never skips; checks at the next event that registers and RAM/VRAM/OAM/palette/IO are unchanged | twin oracle |
| `SMC_BLOCK_EXACT` | the corpus oracle's exact-block class | tools/drprof/dr_corpus.py |
| `GPSP_TEST_GAMEPAK_BUDGET` | builds the ROM page cache's PSP branch for a host test | tools/tests/test_gamepak_residency_budget.c |
| `LINKBENCH`, `LINKBENCH_AWFIX` | the link bench's FIFO counters (docs/GBA-LINK-FEASIBILITY.md) | tools/linkbench |
| `RFU_FLIGHT` | the RFU flight recorder, live where its dump can be read (lab) | lab build |
| `RAM_CACHE_KB` | the RAM code cache size for the twin's sizing studies | twin only |
| `XLAT_NO_LIMITS`, `XLAT_NO_GATEMAP`, `XLAT_NO_EXITPF`, `XLAT_NO_RAMEND`, `XLAT_NO_DCLEAR`, `XLAT_NO_SCANFN` | switch one `XLAT_FAST` piece off (root make `XLAT_NO=PIECE[:PIECE...]`) | GBAdhoc-Lab `analysis sweep --xlat-pieces` (backend/analysis/sweep.py, twin.py); `XLAT_NO_EXITPF`'s line is mutant-patch context (tools/drprof/mutants) |
| `JIT_COST_NO_SCANWIN` | ablation of `JIT_COST`'s windowed gate scan | tools/drprof/mutants (patch context) |
| `SMC_BLOCK_SAME` | (with `SMC_BLOCK_EXACT`) a block-store word that hits code but leaves it unchanged is skipped | root make; tools/drprof/dr_corpus.py class `CLS_H` |
| `TIMER_LAZY` | prototype, guest-visible: timers with no IRQ and no cascade stop stopping the CPU (docs/GBA-LINK-MIRROR.md section 26); run-time key `timer_lazy` | root make; tools/drprof/dr_corpus.py what-if `tlazy` |
| `GBALINK_LEAN_OUTLINE` | the four lean cable hooks (serial.h) out of line | `GBALINK_LEAN_OUTLINE=1 tools/build.sh`; tools/layout/layout_lock.py exempts it |
| `FE_INI_DOC_LEGACY`, `SC_HOST_TEST` | host-test hooks | tools/tests |
| `FE_PNG_LOW_DEPTH` | 1/2/4-bit indexed screenshots (off in every console build) | tools/run_host_tests.py builds both depths |
| `SMC_EVTRACE` | one line per SMC event (writer, word, retirements, fallback reason) to `$SMC_EVT_FILE` between frames `$SMC_EVT_LO..HI` | twin VDEFS; tools/drprof/dr_profile.sh forwards the variables |
| `XLAT_THUMB_NO_UNSWITCH` | host compiler only: an empty asm keeps psp-gcc from unswitching `translate_block_thumb`'s loop on `ram_region` (two copies, 1545 stack stores -> one copy, 256). `tools/build.sh` runs `tools/xlat_spill_guard.sh` when it is on. Its cost needs a hardware A/B. | root make |
| `JIT_CODE_AB` | the original sync model compiled in beside the discipline; harness key `jit_code_mode` selects it. Oracle and PPSSPP IDENTICAL in every mode. | root make, harness; tools/drprof/jc_build_hw.sh, jc_variants.sh; refused in a release |
| `LAYOUT_PAD_TEXT` / `LAYOUT_PAD_BSS` | link `psp/layout_pad.S` first so every object moves by N bytes (hardware layout sweeps) | `PSP_EXTRA=`; refused with `release` |

`DRPROF_NEGCTL` (+1 cycle per Thumb instruction) is the twin oracle's negative
control: it must FAIL the oracle, and it does on all 7 fixtures.

### Oracle mutants

Each breaks one rule on purpose, to prove an oracle or a test catches that class
of bug. They are never in a build that is handed to anyone.

| mutant | breaks | caught by |
|---|---|---|
| `SPIN_SKIP_MUTANT_REGS`, `SPIN_SKIP_MUTANT_STORE`, `SPIN_SKIP_MUTANT_INSKIP` | drops `SPIN_SKIP`'s register rule / allows stores / drops the in-skip check | twin oracle (`tools/drprof/synth/spin_inskip.s`) |
| `SMC_SAME_SKIP_MUTANT_NOCHECK` | trusts any covering block | `EXEC_STALE_CHECK` (111k stale executions on Doom, 600 f) |
| `SMC_SAME_ADAPT_MUTANT_NOREC` | skips without a recorded translation | `tools/drprof/synth/xs_norec.s` |
| `XS_WALK_PAIR_MUTANT` | the paired tag walk | `tools/drprof/synth/xs_pair.s` |
| `SMC_STM_RANGE_MUTANT_LASTWORD`, `SMC_STM_CHAIN_MUTANT`, `SMC_STM_CHAIN_MUTANT_NOPENDING`, `SMC_STM_CHAIN_MUTANT_NOFILTER`, `SMC_STM_AHEAD_MUTANT` | the block-store coverage rules | `tools/drprof/synth/stm_*.s` (root make sets them) |
| `SMC_COVERAGE_MUTANT_SKIP_LAST`, `SMC_COVERAGE_MUTANT_BLOCK_IGNORE` | the SMC coverage checks | root make (smc-coverage) |
| `VX_DMA_NOTE_MUTANT` | VRAM_EXEC's DMA note | `tools/drprof/synth/vram_dma.s` |
| `JIT_CODE_AUDIT_NEGCTL` | drops one code-write record | `JIT_CODE_AUDIT` (60,672 on H&S heavy) |
| `ME_WMAP_MUTANT` | the ME write maps' markers | tools/tests/run_me_wmap_tests.sh |
| `REWIND_MUTANT=n` | a store path that forgets the rewind's written-page map | tools/rewind_test.py |

### The ROM-residency A/B apparatus (2026-09-26)

Harness keys in `.gpsp-harness.ini`, read by `tools/build.sh harness64` EBOOTs
(`harness` pins the 32 MiB layout and cannot hold a 32 MiB cart). All six were
checked bit-identical to the default over 9,270 post-load frames of
`unbound_double_high` in PPSSPP, so each arm differs from the control only in the
hardware variable it names. Protocol: docs/ROM-RESIDENCY.md.

| key | effect |
|---|---|
| `rom_cap = N` | ROM page-cache cap in 1 MiB blocks (1..32), no probe. `32` holds a 32 MiB cart whole on a 64 MiB console |
| `swap_stubs = 1` | a resident cart still gets the paged-ROM load stubs (NULL-page check), so its JIT code and layout match a paged run exactly |
| `cache_paranoid = 1` | every JIT cache sync becomes a full D-cache writeback-invalidate plus full I-cache invalidate |
| `rom_ballast_kb = N` | hold N KiB of heap before the ROM cache, so the blocks land higher |
| `jit_coherency_scan = N` | every N frames, count RAM-translation-cache words whose cached view differs from memory: `EVT jit_coh` |
| `shash = 1` | one line per frame to `log/shash.txt`: audio hash, r0-r15/CPSR, IWRAM, EWRAM, I/O, palette, OAM, VRAM hashes. `shash_dump_from/_to` add raw dumps. Needs `vhash = 1` |

### The 2026-09-24 gate-layout A/B apparatus

`RTC_PIN`, `STATE_HASH_TRACE`, `UPDATE_TRACE_LO/HI` and `STALE_STACKNOTE` above,
passed through `CODE_DEFINES='-D...'` on the root make; each is refused in a
release. With gate exits skipping the cycle check, no partial retirement and
`RTC_PIN`, the one-gate and two-gate layouts are timing-identical. Before the
gate-word tagging fix they diverged at H&S frame 23100 in exactly one mixed
sample (stale `adds sb, r6, sb, asr #23` at 0x03001688); after it they are
bit-identical over 1001 frames (audio `84b2bbff`). The arms that answered this
question are under Removed.

---

## Removed

Deleted in 4.6.0: rejected, superseded or spent, and in no oracle, mutant or Lab
script. The code is in the 4.5.0 tag. The two defects found while first
compiling this list (comment blocks attributed to the wrong `ifeq`, and an
`SMC_GATE_PROMOTE_HITS` threshold the code never had) were fixed then.

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
| `XLAT_CENSUS`, `XLAT_REUSE_SIM`, `SMC_DIFF_CENSUS`, `XLAT_PURE_PROBE`, `SMC_EVTRACE_XLAT` | Code-health A-2. One-off twin studies (2026-10-27/28) sizing translation reuse, translation purity and SMC immediate rewrites (the levers they sized, `SMC_REUSE`, `XLAT_PURE` and `SMC_IMM_PATCH`, carry their own notes). No tool or Lab script built them. |
| `IDLE_STATS`, `IDLE_VERIFY` | Code-health A-2. Twin counters/oracle for the game table's idle branch. Finding kept in `gba_memory.c` (Advance Wars' entry is not a wait). `SPIN_SKIP_VERIFY` stays. |
| `SMC_WORK_STATS`, `SMC_WORK_WINDOWS` | Code-health A-2. Twin dumps of the SMC work counters, used once to fit `DMA_ADAPT_V2`'s prices. The counters stay under `DMA_SMC_ADAPT`. |
| `CORE_PHASE_FORCE`, `LAYOUT_PROBE_PAD` | Code-health A-2. A pinned `core_phase` for one measurement build; a twin cache-address shift (layout-dependence probe). |
| `PERF_TEST`, `RFU_DEBUG`, `SERIALPROTO_DEBUG`, `REGISTER_USAGE_ANALYZE`, `MEMORY_STATS_ANALYZE` | Code-health A-2. Upstream gpSP's libretro perf counters, debug printfs and interpreter counters, never built here. The debug-print, memory-stats and register-usage call sites stay as no-op macros. |
| `JIT_COST_NO_PUBINL`, `JIT_COST_NO_SCANCOND` | Code-health A-3. `JIT_COST` ablations: the kernel cache calls instead of user-mode line ops at publish (the run-time key `jit_pub_inline = 0` still selects them), and the arm_set_condition switch instead of the flag-set table in the fast scan. The table's own mutant (`jc_scancond_bit7.patch`) stays. |
| `DMA_ADAPT_TIE_FLUSH` | Code-health A-3. `DMA_ADAPT_V2` with ties going to the flush: Mario Golf left retire on a 7 % flush probe and never came back (-3.6 % vs -5.0 %), TGR gained nothing (the finding is in the comment above `da2_cost`). |
| `SMC_REUSE_WHOLE_I` | Code-health A-3. A hardware A/B arm: the whole I-cache invalidated on a virtual (`SMC_REUSE`) flush as well. |
| `SMC_UNTAG_MARKS_ONLY` | Code-health A-3. `SMC_SINGLE_UNTAG` lifting only interior marks, not retired blocks' own tags. |
| `TGB_FAST_TILE` | Code-health A-3. GB tile rows decoded by a 1 KiB table: exact, but no gain on hardware (gbp-c2 vs gbp-b2, 13/13 "same", mean ~+0.7 % worse; docs/GB-CORE-PERF.md). |

Files removed with no switch of their own: `netdrv/transport_inet.{c,h}` and its
psp/Makefile rule (code-health A-1): a sceNetInet netdrv backend that no `OBJS`
list ever linked; Mystery Gift owns its socket in `psp/mgift_net.c`.

---

## The rule for adding one

A switch that ships goes in `gpsp_features.h` (`psp/fe_features.h` for the frontend),
and only there, with `tools/flags.lock` updated in the same commit and a row in
**Ships** above. A measurement tool needs three things or it is an experiment
waiting to be deleted:

1. A comment **immediately above its own `ifeq`** (or its `#if`), saying what it
   does and what it would prove.
2. A row under **Measurement tools** naming the profile, tool, test or Lab job
   that uses it, and the measured number once there is one.
3. A `gpsp_profile.h` check if it must never reach a release, or if it silently
   shadows another switch.
