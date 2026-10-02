# JIT CODE DISCIPLINE — one model for the dynarec's code memory

Branch `claude/jit-code-discipline` (off `claude/candidate-3.1-dr`, 2026-10-01).
Switch: root make `JIT_CODE_DISCIPLINE=1`, **off by default**. With it off,
every line this branch adds compiles to nothing: the release EBOOT is
byte-identical to the candidate's (section 6.1).

**Labels.** *Twin* = the PSP's own translator under qemu (`tools/drprof`),
exact instruction counts, no caches. *PPSSPP* = emulator, no caches.
*Hardware* = a PSP. Nothing in this document was run on a console; section 8
is the hardware plan for the main session.

---

## 0. Answer in one screen

* **The model.** Translated code lives in three zones (STUB, ROM, RAM) that
  own whole 64-byte cache lines. Every write of code is recorded through one
  small API; nothing chooses its own sync range any more. One publish
  point writes back and invalidates exactly the recorded lines before any
  code can run, and the whole I-cache is invalidated once after every
  ownership change (cache flush, emitter rebuild, tier lend, state/ROM
  load). The kernel is never handed a range of 16 KiB or more (the
  checker agent's leading explanation of the derail); such a span becomes
  the whole invalidate. It replaces 8b48b48's whole invalidate on every
  ROM publish.
* **Guest behaviour is unchanged:** `dr_oracle.py` IDENTICAL on all 7
  fixtures for base vs base + discipline and candidate vs candidate +
  discipline (and the A/B build's modes), plus a 32-reload soak.
  PPSSPP, on the real harness64 EBOOTs: every arm IDENTICAL to the
  candidate on the AW2 tour and the H&S heavy battle (resident). Switch
  off: the release EBOOT is byte-identical to the candidate's.
* **The API is complete, by audit:** a shadow of both zones finds 0 code
  words changed without a record in 515k checks (all fixtures + the soak);
  dropping one record (the negative control) gives 60,672.
* **Coherent by construction, by the cache checker** (the parallel agent's
  twin model of the Allegrex caches, merged): its acceptance suite
  `jc_suite.sh` with `ibig=noop,dbig=noop` reads **0 in every model on
  every fixture**, and the kernel never sees a range of 16 KiB or more.
  Also 0 under the inclusive semantics and under the (refuted) line-loop
  bugs, where the candidate has millions.
* **Cost:** twin core instructions −0.0% to +0.3% vs the candidate; ~25x
  fewer whole I-cache invalidates (H&S heavy 0.12/frame vs 0.85; Unbound
  rival 0.02 vs 1.04; the 16 KiB rule adds 1-11 per run); ~10% fewer kernel sync calls on SMC-heavy games.
  The candidate's measured "+18% on AW2" is a layout effect, not its
  whole-I cost (AW2 does 0.2 ROM publishes a frame; section 6.5), so the
  hardware bench compares policies on ONE binary across paddings.
* **Hardware:** not run (consoles are the main session's). Section 8 is the
  exact plan: a same-binary layout sweep to reproduce the derail with the
  old syncs and show the discipline survives it, the mechanism probe (mode
  3), the bench, and a reload soak; `builds/jit-discipline-out/` holds the
  builds and the drrig arms.

---

## 1. Why

Upstream gpSP was written for x86, whose instruction cache is coherent with
data writes. The MIPS port added cache maintenance where someone noticed it
was needed: nine `platform_cache_sync` call sites, each choosing its own
range, plus a tenth writer in emitted code. Whether translated code is
visible to instruction fetch therefore depended on every one of those sites
being right, for every path, on every memory layout.

The Allegrex (all PSP models) has a write-back data cache and an
instruction cache that does not snoop it. Code written by the CPU reaches
instruction fetch only when (D) its data-cache lines have been written back
and (I) no instruction-cache line still holds an older copy. PPSSPP models
neither, which is why every bug of this class is hardware-only and
layout-sensitive: whether a stale line survives until it is executed depends
on what else maps to its cache set.

The resident-ROM derail (`docs/RESIDENT-ICACHE-FIX.md`) is that class. It was
bisected on the failing binary to stale I-cache lines that a ranged
invalidate of the newly published ROM range does not reach; 8b48b48 cures it
by invalidating the whole I-cache on **every** ROM publish.

Memory also changes role: a flush resets the translation pointers and the
same addresses receive a new generation of code; the LARGE JIT tier moves
the zones into heap memory that was data a moment ago and lends the idle
SMALL ROM array to the cart as data; ROM load and emitter rebuild rewrite the
stubs in place. None of these were treated as events by the old code.

## 2. The model

### 2.1 Zones

| zone | where | written by | lifetime |
|---|---|---|---|
| STUB | `[rom_translation_cache, rom_cache_watermark)` | `init_emitter` (stubs), `init_bios_hooks` (the BIOS SWI entry and its eager targets) | immutable until the next emitter rebuild ("sealed") |
| ROM | `[watermark, rom_translation_ptr)` | ROM/BIOS translations | until a ROM flush |
| RAM | `[ram_translation_cache, ram_translation_ptr)` | IWRAM/EWRAM translations, SMC thunks and retirements | until a full RAM flush |

Code never lives anywhere else. Two kinds of metadata share the
allocations and are never executed: the 8-byte ROM hash header before each
ROM block, and the RAM tag table that grows down from the top of the RAM
allocation. Neither shares a byte with code; the check build asserts the tag
table never meets the RAM code, and the audit build tracks every header word
so a write to one is never mistaken for a code write. (Moving the headers
out would change the emitted layout; there is no coherency reason to.)

### 2.2 One writer API (`jit_code.h`)

| macro | meaning |
|---|---|
| `JIT_CODE_EMITTED(lo, hi)` | a block's whole emission, recorded once at commit (`translate_block_*`, where `rom/ram_translation_ptr` advance) |
| `JIT_CODE_WROTE(lo, hi)` | a patch of existing code (thunks, retirement, stubs) |
| `JIT_PATCH_BRANCH(src, dst)` | the branch patch plus its record |
| `JIT_CODE_META(lo, hi)` | zone metadata (audit builds only) |

Every record is rounded out to whole 64-byte lines and merged into an
overlapping or touching span (32-entry table; a full table publishes early,
which is always safe). **No call site chooses a sync range any more.**

### 2.3 One publish point

`jit_code_publish()`: D-cache writeback of every recorded line, then I-cache
invalidate of the same lines — or of the **whole** I-cache when code memory
changed owner since the last publish — then the record is cleared. Inline
fast path: one load, one branch when nothing is pending.

**Never a kernel range of 16 KiB or more** (`docs/JIT-COHERENCY.md`
section 8: the kernel's ranged calls may take a different path at the cache
size, and the derail's stale lines are exactly the ones a ≥16 KiB ROM
publish should have invalidated). D writebacks go to the kernel in 8 KiB
pieces; an I span that large becomes the whole invalidate (counted
`big_i`; 1-11 per fixture run, all in load frames).

It runs:
* in `translate_icache_sync`, i.e. before a block pointer leaves
  `block_lookup_address_{arm,thumb}` (and through them `_dual`) and
  `badjump_recover`;
* after any patch of existing code outside a translation (SMC retirement:
  `flush_translation_cache_ram_range`, `flush_translation_cache_ram_block`);
* at the end of `init_bios_hooks`.

### 2.4 Ownership changes

`jit_code_owner_changed(why)` marks the next publish as whole-I:

| event | where | reached from |
|---|---|---|
| ROM flush | `flush_translation_cache_rom` | state load, ROM load, cheat install, a full ROM cache, SMC partial activation |
| RAM flush (full) | `flush_translation_cache_ram` | state load, SMC/DMA full flushes, a full RAM cache |
| emitter rebuild | `init_bios_hooks` (end of `init_emitter`) | ROM load / reset, dynarec enable |
| init | `init_dynarec_caches` | ROM load / reset |
| tier | `dynarec_select_translation_caches` (LARGE) | boot: zones move into heap memory, the SMALL ROM array is lent to the cart as data |

Deferring the invalidate to the next publish is exact, not an
approximation: no translated code can run between the event and that publish
(section 4), and a publish happens before any code runs.

### 2.5 The one writer outside the API

The memory-region **patch handler** (`mips_emit.h emit_phand`) is emitted
code: when a memory access hits a region other than the one its call site
was last patched for, it rewrites that call site's `jal` and syncs the one
line itself (`cache 0x1A` = D writeback, then `cache 0x08` = I invalidate;
`synci` in the twin). It only ever replaces a `jal <stub>` with another
`jal <stub>`, and every stub re-checks the region, so even a stale fetch of
the old `jal` re-enters the patch handler and produces the right result: it
cannot derail. The audit recognises exactly this pattern (old and new word
both `jal` into the STUB zone) and nothing else.

## 3. Every code-writing site

| # | site | before | now |
|---|---|---|---|
| 1 | emitter: a block's body (`translate_block_{arm,thumb}`) | covered by `translate_icache_sync`'s `[last, ptr)` per zone | `JIT_CODE_EMITTED(start, commit_ptr)` |
| 2 | in-block patches before commit (conditional skips, internal branches, SMC islands, contained bad jumps) | inside `[last, ptr)` | inside the emission span (audited) |
| 3 | eager exit links after commit, `SMC_PARTIAL_DIRECT_GATE` link | inside `[last, ptr)` because nothing published in between | `JIT_PATCH_BRANCH` |
| 4 | SMC stable thunk link (`smc_stable_thunk_link`) | own `platform_cache_sync(entry, entry+8)` | `JIT_CODE_WROTE`; published by the translating lookup |
| 5 | SMC stable thunk dispatch (`smc_stable_thunk_dispatch`) | own sync of 16 bytes | `JIT_CODE_WROTE`; published by the lookup or the retirement |
| 6 | SMC partial retirement trampolines (`flush_translation_cache_ram_range`, `entry-16 .. entry+8`) | own sync per block | `JIT_CODE_WROTE` per block, ONE publish per retirement batch |
| 7 | `SMC_PARTIAL` retirement (`flush_translation_cache_ram_block`, not shipped) | own sync | same as 6 |
| 8 | emitter stubs (`init_emitter`) | `platform_cache_sync` of the stub range in `init_bios_hooks` | ownership change + `JIT_CODE_WROTE(stubs)` |
| 9 | BIOS SWI entry (`init_bios_hooks` -> lookup) | the lookup's sync | the lookup's publish (with the rebuild's whole-I) |
| 10 | `badjump_recover`'s reset-vector translation | `translate_icache_sync` | publish |
| 11 | patch handler (emitted) | its own `cache` ops | unchanged; audited (2.5) |
| — | ROM hash headers, RAM tag table | data | metadata, never code (2.1) |
| — | `DISPATCH_CACHE` | writes only its own data tables | unchanged; filled after the publish, emptied by every flush |

## 4. Every way into translated code passes a publish

| path into code | publish |
|---|---|
| `execute_arm_translate_internal` (frame start) | `block_lookup_address_{arm,thumb}` |
| `mips_indirect_branch_*` (C lookup) | same |
| `mips_indirect_branch_*` (`DISPATCH_CACHE` hit) | the entry is inserted after the C lookup's publish; a later patch of the code it points at (an SMC thunk relink) is published by the lookup that made it; retirement and every flush clear the table |
| `lookup_pc` (IRQ, PC change, after SMC and DMA flushes, halt wake) | lookup |
| `execute_store_cpsr` with a PC change | lookup |
| `badjump_recover` | publish, then return |
| return into the calling block (`mips_update_gba`, cheats, SWI, PSR, memory stubs) | the calling block is unchanged by these handlers; every handler that flushes reports a PC change and goes through `lookup_pc` instead (`update_gba` DMA path, `smc_write`, `write_io_epilogue`) |
| a direct block link | target published when the link was made (the target is translated, and published, before or with the block linking to it) |

---

## 5. Switches and builds

| switch (root make unless noted) | what | where allowed |
|---|---|---|
| `JIT_CODE_DISCIPLINE=1` | the model above; zones 64-byte aligned on the SMALL tier too | any profile (off by default) |
| `JIT_CODE_CHECK=1` | zone asserts on every record and every lookup return (zones own whole lines, sealed STUB zone, tag table never met, returned pointer inside committed code, nothing pending) | not release; needs the discipline |
| `-DJIT_CODE_AUDIT` (twin) | the writer audit (6.3); implies the check | not release |
| `-DJIT_CODE_AUDIT_NEGCTL` (twin) | drops ONE record (the SMC thunk link): the audit must fail | twin only |
| `JIT_CODE_AB=1` | harness only: the original model compiled in beside the discipline; harness key `jit_code_mode` = 0 legacy (pre-8b48b48), 1 legacy + 8b48b48, 2 discipline, 3 discipline without the ownership invalidate. One binary for every arm of a hardware A/B | not release; needs the discipline |
| `-DJIT_SYNC_STATS` (twin) | counts of the original sync path | twin only |
| psp `LAYOUT_PAD_TEXT=N` / `LAYOUT_PAD_BSS=N` (`tools/build.sh` `PSP_EXTRA=`) | links `psp/layout_pad.S` first: every object's code / bss moves by N | not release |

`gpsp_profile.h` refuses each illegal combination. The harness logs
`EVT jit_code mode=M` at boot and, every 600 frames in `core_health`,
`jitc=publishes/lines/whole_i/early own=rom/ram/rebuild bjrec=N`
(`bjrec` = BADJUMP_SAFE recoveries, i.e. derails caught; weak symbols, so
every core links).

Tools: `tools/drprof/jc_variants.sh` (the twin variants), `jc_stats.sh`
(sync statistics per fixture), `jc_ppsspp.sh` (PPSSPP shash oracle),
`jc_build_hw.sh` (the hardware builds), `scripts/reload_soak.txt` (40 state
reloads with battle input between them).

---

## 6. Evidence

### 6.1 Switch off = the candidate, byte for byte

`tools/build.sh release`, same `GPSP_BUILD_VERSION`, this branch vs
`claude/candidate-3.1-dr` (32b6d71): **EBOOT.PBP identical** (md5
`e4aa8244`), and every loaded section of the ELF identical (`.text`,
`.rodata`, `.data`, `.sdata`, `.sceStub.text`, `.lib.stub`,
`.rodata.sceModuleInfo`). Only `.debug_line` differs (line numbers moved).

### 6.2 Guest behaviour: the differential oracle (twin)

`tools/drprof/dr_oracle.py`, every fixture, every frame, every field
(audio, registers, IWRAM, EWRAM, I/O, palette, OAM, VRAM):

| A | B | result |
|---|---|---|
| base (3.0.0 dynarec flags) | base + discipline | IDENTICAL 7/7 |
| cand (candidate-dr flags) | cand + discipline | IDENTICAL 7/7 (before and after the zone alignment) |
| cand | the A/B build, mode 2 | IDENTICAL 7/7 |
| cand | the A/B build, mode 0 | IDENTICAL 7/7 |
| cand | cand + discipline + audit, reload soak (H&S heavy, 32 reloads, 16,000 frames) | hash-identical |

(6,061 + 4 × 3,100 + 1,705 + 2,100 frames per comparison.)

### 6.3 The writer API is complete: the audit (twin)

The audit build shadows both zones' committed code and, at every publish and
once a frame, requires every changed word to be inside a span being
published, a ROM hash header, or the patch handler's `jal` swap.

| run | audit passes | unrecorded writes | zone violations | patch-handler swaps recognised |
|---|---:|---:|---:|---:|
| all 7 fixtures | 188,467 | **0** | **0** | 163,518 |
| reload soak (32 reloads) | 326,525 | **0** | **0** | 230,772 |
| **negative control** (one record dropped), H&S heavy | 56,519 | **60,672** | 0 | — |
| negative control, Unbound rival | 50,537 | **54,314** | 0 | — |
| negative control, AW2 (no SMC thunks) | 7,984 | 0 | 0 | — |

The negative control proves the audit can see the bug class; zero on the
real build proves no code byte changes without being recorded.

### 6.4 Coherency: the twin cache checker (claude/jit-coherency-checker)

The parallel agent's checker (`tools/jitcoh`, a qemu plugin modelling the
Allegrex I and D caches; `docs/JIT-COHERENCY.md`, merged into this branch)
runs the core's own PSP cache calls against hooked stand-ins
(`-DJITCOH_PSP_CACHE`) and flags every executed translated-code word whose
bytes, as the modelled caches would deliver them, differ from what was last
written:

* `HAZ_I` the I line was fetched since its last invalidate, then written,
  then executed; `HAZ_D` executed before its D line was written back;
  `HAZ_ID` both; `HAZ_S` **written after its line's last invalidate, fetched
  or not** (covers prefetch and speculative fills: the strictest);
  `ACT_o<k>` the real geometry (16 KiB 2-way, 64-byte lines, LRU) at four
  code-vs-C layout offsets.
* Kernel semantics: by default the firmware's own inclusive ranged
  invalidate. `ibig=noop` / `dbig=noop` model "a ranged call of 16 KiB or
  more does nothing" — the checker agent's leading explanation of the
  derail (its section 8): only the old Thumb ROM publish (site B) ever
  passed such a range, up to 83 KiB in the state-load frame. `isem=naive` /
  `floor` model buggy line loops; the firmware code refutes them, so they
  are sensitivity tests only.

**Acceptance (the brief's rule): `tools/jitcoh/jc_suite.sh` on the
discipline, `ibig=noop,dbig=noop`, every fixture: 0 in every model** (aw2,
hns_heavy, hns_light, ub_rival, ub_double, ub_ow, em_battle; `suite
exit=0`). The checker's own count of kernel calls: `big_ranges I(>=16KiB)=0
D(>=64KiB)=0` — the publish point never hands the kernel a big range
(2.3) — and the twin hashes are identical to the candidate's on all 7.

The other runs (whole library + the 16,000-frame reload soak, 8 runs each;
totals of stale executions):

| model | kernel semantics | HAZ_I | HAZ_S | ACT_o0 |
|---|---|---:|---:|---:|
| **2 discipline** | incl / naive / floor | **0 / 0 / 0** | **0 / 0 / 0** | **0 / 0 / 0** |
| 1 candidate (8b48b48) | incl | 0 | 0 | 0 |
| 1 candidate | naive (refuted) | 182,214 | 2,212,422 | 534 |
| 1 candidate | floor (refuted) | 21,605,898 | 42,642,263 | 1,294,262 |

And H&S heavy, 1,200 frames, every model of the A/B build (whole-I = whole
invalidates issued):

| model | semantics | HAZ_I | HAZ_S | whole-I |
|---|---|---:|---:|---:|
| 0 legacy (pre-8b48b48) | incl / naive / floor | 0 / 47,997 / 932,797 | 0 / 121,815 / 1,849,621 | 0 |
| 1 candidate | incl / naive / floor | 0 / 7,561 / 888,912 | 0 / 118,282 / 1,787,225 | 3,500 |
| **2 discipline** | incl / naive / floor | **0 / 0 / 0** | **0 / 0 / 0** | 178 |
| 3 discipline, no ownership invalidate | incl / naive / floor | 0 / 0 / 0 | 0 / 0 / 0 | 0 |

What this says:
* **The discipline is coherent by construction under every model and every
  kernel hypothesis** — inclusive, big-range no-op, and even the refuted
  line-loop bugs — because it only ever passes whole, aligned lines in
  ranges under 16 KiB, and invalidates the whole I-cache otherwise.
* Under the firmware's real semantics the old syncs were complete too (the
  checker's finding); the derail needs the big-range path, which the
  candidate survives only because its whole-I happens to be at site B.
* Mode 3 being clean says that, in the model, precise publishing alone is
  coherent; the ownership invalidate is defence in depth (6.6).

`jshift`: the checker's default `jshift=0x20` shifts translated code by 32
bytes in the model to give the twin's 32-byte-aligned static caches the
LARGE tier's line alignment. Discipline builds are 64-aligned already and
round real addresses, so they run with `jshift=0`.

### 6.5 Cost

**Sync work (twin counts, steady state = frames 600 to the end).**
The candidate does one whole I-cache invalidate per ROM publish; the
discipline one per ownership change. Kernel calls are two per span.

| fixture | candidate whole-I / frame | discipline whole-I / frame | candidate sync calls / frame | discipline spans / frame |
|---|---:|---:|---:|---:|
| AW2 tour | 0.21 | 0.003 | 0.22 | 0.22 |
| H&S heavy | 0.85 | 0.12 | 37.2 | 33.3 |
| H&S light | 0.85 | 0.11 | 5.9 | 2.9 |
| Unbound rival | 1.04 | 0.018 | 29.5 | 28.6 |
| Unbound double | 0.93 | 0.012 | 19.3 | 18.8 |
| Unbound overworld | 0.17 | 0.014 | 23.0 | 22.5 |
| Emerald battle | 1.10 | 0 | 1.1 | 1.1 |
| H&S reload soak (whole run) | 53,364 total | 1,908 total | — | — |

D-writeback volume is the same (H&S heavy 244 vs 240 lines/frame); the
discipline's spans are line-rounded and merged, so it makes ~10% fewer
kernel calls on the SMC-heavy fixtures and ~28x fewer whole invalidates.

**Instructions (twin, core instructions per frame, measured window).**

| fixture | candidate | discipline | change | p95 change |
|---|---:|---:|---:|---:|
| AW2 tour | 650,223 | 650,013 | −0.0% | −0.2% |
| H&S heavy | 1,145,455 | 1,148,775 | +0.3% | +0.2% |
| H&S light | 623,577 | 624,045 | +0.1% | −0.2% |
| Unbound rival | 1,768,639 | 1,768,787 | +0.0% | +0.1% |
| Unbound double | 1,242,446 | 1,243,486 | +0.1% | +0.3% |
| Unbound overworld | 1,002,524 | 1,002,139 | −0.0% | −0.0% |
| Emerald battle | 723,050 | 722,995 | −0.0% | −0.0% |

The worst case, H&S heavy +3.3k instructions/frame, is the bookkeeping
(`jit_code_wrote_` 1.5k, `jit_code_publish_` 1.6k): about 21 µs at the
calibrated 6.5 ns/instruction. The twin cannot price the kernel calls or the
I-cache refill after a whole invalidate; at most 256 lines × 215 ns ≈ 55 µs
per whole-I (measured miss cost, icprobe), the discipline saves up to
~40 µs/frame on H&S heavy and ~55 µs/frame on Unbound rival against the
candidate.

**The candidate's "+18% on AW2" is not its whole-I cost.** In steady state
AW2 does 0.21 ROM publishes per frame, so 8b48b48's whole invalidate costs
at most ~12 µs/frame there, not 1.29 ms. And H&S, with four times as many
whole-I's per frame, measured only +0.33 ms (PUB 9.69 → CAND 10.02 ms, Go).
The Go numbers (`builds/candidate-out/logs-go-iso*`): CNOFIX 6.98, CBONLY
7.16, CAND 8.27 ms on AW2 — three binaries whose only differences are where
one sync site's code sits. That is a code-layout effect (hot code meeting
in the same I-cache sets), the same sensitivity that makes the derail come
and go. So a hardware comparison of two different binaries prices layout
as much as policy; the plan (section 8) therefore measures policy on ONE
binary (the A/B build, modes 1 vs 2) at several paddings, and only then
compares the shipping binaries.

### 6.6 The resident-ROM derail, revisited

The bisection (`docs/RESIDENT-ICACHE-FIX.md`) showed: a whole I invalidate
at the out-of-line ROM-publish site B (the Thumb/dual lookups) cures it; the
same at the ARM lookup's inlined ROM publish does not; rounding site B's
range out to whole lines does not; ranged-D + whole-I does. The checker
agent (`docs/JIT-COHERENCY.md` section 8) found that under the firmware's
real ranged semantics every sync was complete, that only site B ever passes
the kernel an I range of 16 KiB or more (from `translate_block_thumb`'s
eager exit linking: 0x443c, 0x6940, 0xa42c bytes, up to 83 KiB in the
state-load frame), and that if that big-range path leaves lines alone, the
stale executions are exactly pre-load code at addresses the post-load
translations rewrote. That fits every bisection arm, including why rounding
the range could not help (a bigger range is still a big range).

The discipline closes it twice over, by construction:
1. it never hands the kernel a range of 16 KiB or more (2.3);
2. a state load is an ownership change, so the first publish after it
   invalidates the whole I-cache anyway — before any code can run.

The A/B build tells the two apart on hardware, on one binary (8.3): mode 0
(old syncs) should die where it died before; mode 2 must not; mode 3
(no ownership invalidate, but still no big ranges) isolates rule 1.

### 6.7 PPSSPP

`tools/drprof/jc_ppsspp.sh` on the staged harness64 EBOOTs (PPSSPP, no
caches: this checks guest behaviour on the real PSP build, not coherency).
Reference: the candidate's own harness64 EBOOT (`cdr`,
`candidate-out/dr-harness64`, 32b6d71). Every frame and field of `shash`,
audio included, from the state load on:

| arm | AW2 tour (6,031 frames) | H&S heavy, resident (3,170 frames) |
|---|---|---|
| `jcoff` (switch off) | IDENTICAL | IDENTICAL |
| `jcd` (discipline) | IDENTICAL | IDENTICAL |
| `jab-p0`, mode 0 (legacy) | IDENTICAL | IDENTICAL |
| `jab-p0`, mode 1 (legacy + 8b48b48) | IDENTICAL | IDENTICAL |
| `jab-p0`, mode 2 (discipline) | IDENTICAL | IDENTICAL |
| `jab-p0`, mode 3 (no ownership invalidate) | IDENTICAL | IDENTICAL |

Including the 30 boot frames before the load, 11 of the 12 runs are also
identical; the H&S mode-1 run differs in IWRAM on boot frames 11-30 only,
because that PPSSPP instance started 3 s later (`rtc_seed` 15:13:42 vs
15:13:39): the cart's RTC seconds, the known boot-time wall-clock byte.
The harness telemetry worked as staged: `EVT jit_code mode=M` on every A/B
run, and `core_health` per window, e.g. H&S `jcd`
`jitc=52316/710000/360/0 own=3/359/2 bjrec=0` against mode 1's 3,951 whole
invalidates. Logs: `~/jcppsspp/` (WSL), summary
`builds/jit-discipline-out/ppsspp-oracle.txt`.

---

## 7. Coordination with claude/jit-coherency-checker

* That branch (a18828b) is **merged into this one** (aa1b286); the only
  conflict was adjacent `#if` blocks at the top of the cache section. Its
  twin build fix (`fe_gblink.c`, `dr_gbstub.c`) was already identical here.
* The discipline's publish calls the PSP kernel functions under
  `defined(PSP) || defined(JITCOH_PSP_CACHE)`, so the checker sees them one
  to one.
* Acceptance: `JC_ARGS="ioff=0.37.64.101,maxev=4000,jshift=0,ibig=noop,dbig=noop"
  tools/jitcoh/jc_suite.sh jc-disc` (built with `jc_build.sh disc` and
  `VDEFS="-DDISPATCH_CACHE=1 -DSMC_RETIRE_WINDOW=1 -DSERIAL_IDLE_FAST=1
  -DJIT_CODE_DISCIPLINE=1 -DJIT_CODE_CHECK=1"`): exit 0, every model 0 on
  every fixture (6.4). Discipline builds need `jshift=0` (64-aligned zones,
  absolute line rounding).

---

## 8. Hardware validation (for the main session; nothing here was run)

All on the existing rig (one rig app, `GBADHOC-DRPROF`, USB handoff loop:
`drrig.py` arms + `hnsrig.py` window-aware loop, Go on the M2 card with the
chkdsk gate). Builds are staged in `builds/jit-discipline-out/hw/`; the exact
`drrig.py` additions are in `builds/jit-discipline-out/DRRIG-ARMS.py`.

### 8.1 Builds

| kind | what |
|---|---|
| `cdr` | the current candidate: `claude/candidate-3.1-dr` 32b6d71 harness64 (copy of `candidate-out/dr-harness64`) |
| `jcoff` | this branch, switch off (control: the candidate plus the new telemetry) |
| `jcd` | this branch + `JIT_CODE_DISCIPLINE=1`: the product |
| `jab-pN` | + `JIT_CODE_DISCIPLINE=1 JIT_CODE_AB=1`, `.text` padded by N = 0, 1024, 2560, 3776, 5120, 6656 bytes (distinct offsets modulo the I-cache way size, 8 KiB; 3776 ≈ the 3.7 KiB that once hid the bug). The pad is linked first; only `main` (GCC's `.text.startup`, placed first by the linker script) precedes it, so every other function moves by N (checked with `psp-nm`) |

Provenance (`build-manifest.json` in each, clean trees, one ME PRX
`19ae57b6` for all): `jcd` and `jab-p*` from aa1b286 (with the 16 KiB rule),
`jcoff` from 774c608 (switch off: the rule does not apply). The PPSSPP
results of 6.7 are from the 774c608 builds; the rule changes only which
kernel calls are made, which PPSSPP does not model, and the twin oracle and
checker runs (hash-identical) are on the merged tree.

Fixtures: **A** AW2 tour; **H** H&S heavy battle (`hns.txt`; resident by
default, paged with `rom_cap = 15`); **R** (new) H&S heavy +
`tools/drprof/scripts/reload_soak.txt`: 40 state loads, ~15,700 frames,
`autoexit_frames = 16300` — forty chances per run for the post-load derail.
Death = `bjrec` > 0 in any `core_health` line (a reload soak recovers from a
derail at the next load, so the last frame cannot show it), or the shash
ending at pc 0x186c.

### 8.2 Phase 1 — find layouts where the original code derails (PSP Go, ~1 h)

Plan `jcrepro`: fixture R, mode 0 (`jit_code_mode = 0`, the pre-8b48b48
syncs), one arm per padding, ABBA x2 = 12 runs.
Then the same on the PSP-3000 if the Go gives fewer than two dying layouts.

### 8.3 Phase 2 — the A/B at each dying layout (Go, 8 runs per layout)

Plan `jcmech-pN`: fixture R, modes 0, 1, 2, 3 on `jab-pN`, ABBA x2.

| result | reading |
|---|---|
| 0 dies, 2 never dies | the discipline cures it on this layout, same binary |
| 1 dies too | the candidate's fix is incomplete on this layout |
| 3 lives, 0 dies | the 16 KiB rule alone cures it (the checker's big-range explanation) |
| 3 dies, 2 lives | the ownership invalidate is what cures it (something the model cannot see) |

Pass: mode 2 has `bjrec = 0` in every run, and its shash is identical across
runs of one layout (and to mode 1 wherever mode 1 survives) — the hardware
oracle, audio included.

If no layout dies in phase 1, the bug is not reproducible on this code base;
run 8.5 on both 64 MiB consoles and report "unreproduced, soak clean".

### 8.4 Bench (Go, then 3000; ~2 h each)

1. **Policy, on one binary** (layout cannot differ): fixtures A and H
   (resident) on `jab-p0`, modes 1 vs 2, ABBA x2 (8 runs); then A on
   `jab-p2560` and `jab-p5120`, modes 1 vs 2 (8 runs) for the layout spread.
   Accept: mode 2 ≤ mode 1 within run-to-run noise at every padding.
2. **The shipping binaries**: A, H paged (`rom_cap = 15`) and H resident on
   `cdr` vs `jcd`, ABBA x2 (12 runs). Accept: `jcd` ≤ `cdr`. If 1 passes and 2
   does not, the difference is layout, not policy (6.5), and should be
   reported as such rather than tuned.
3. Read alongside: `jitc=` whole-I counts per window (expected per 6.5).

Score with `builds/candidate-out/bench_score.py` logic (mean of the
per-window `core_prof` means after the load window).

### 8.5 Soak

Fixture R on `jcd`, 3 runs on the Go and 3 on the 3000 (~15 min per
console); every run `bjrec = 0`, shash identical across runs. The PSP-1000
(SMALL tier, now 64-byte aligned under the switch): a 32 MiB `harness`
build (`CORE_EXTRA="JIT_CODE_DISCIPLINE=1" tools/build.sh harness`) on the
AW2 tour and H&S paged, vs the candidate's `harness` build.

---

## 9. Open risks

* **Not yet run on a PSP.** Everything in section 6 is twin, checker or
  PPSSPP. The checker models the caches; it cannot model firmware or
  silicon behaviour it was not told about (e.g. an instruction prefetch
  buffer that survives a hit invalidate). The ownership invalidate is the
  hedge for that class; phase 2's mode 3 measures whether it is needed.
* **The kernel's big-range behaviour is inferred, not measured.** The
  discipline does not depend on it (no range ≥ 16 KiB, whole aligned lines
  only); a one-off probe on a console (fill lines, rewrite, ranged
  invalidate of ≥ 16 KiB, execute) would settle it — the checker agent
  owns that question.
* **Full RAM flushes now cost a whole-I** (0.003–0.12 per frame in steady
  state, 0.12/frame on H&S heavy). Cheaper than the candidate everywhere
  measured; a game with a full-flush storm (thousands per second) would pay
  ~55 µs each. None of the seven fixtures comes near.
* **The patch handler stays outside the API** (2.5): it is emitted code and
  syncs its own line. Argued safe (a stale `jal` re-enters the handler) and
  audited, not routed through the API.
* **Metadata shares the zones** (ROM hash headers, RAM tag table): never
  executed and never sharing a byte with code; asserted and audited, not
  moved.
* **Layout still moves performance** (6.5). The discipline removes layout
  from correctness, not from speed; the bench must keep measuring on one
  binary across paddings.
* **Mode 0 on hardware deliberately runs the old, buggy model**: the A/B
  build is harness-only and refused in a release.
