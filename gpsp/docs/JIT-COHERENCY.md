# JIT cache coherency: a deterministic checker, and what it says about the resident-ROM derail

Branch `claude/jit-coherency-checker`, built on `claude/candidate-3.1-dr`. Written 2026-10-01.
Desktop and WSL only. No console was touched.

## Summary

1. **The checker.** `tools/jitcoh/` is a QEMU TCG plugin that runs beside the dynarec twin
   (`tools/drprof`) and models the Allegrex caches:
   - a write-back D-cache;
   - a non-snooping I-cache;
   - the PSP's four kernel cache calls, with the PSP's own arguments;
   - the patch handler's inline `cache` pair.

   It flags every executed translated-code word whose bytes, as the PSP would fetch them, differ
   from what the emitter last wrote. For each flag it names:
   - the writer (store PC, call stack, guest block);
   - the previous writer;
   - when the I-cache line was filled;
   - the last invalidate that covered the line;
   - the ranged invalidate that should have covered it but did not.

   It is validated: every sync site removed on purpose is flagged, and restoring it silences the
   checker. It separates I-side holes from D-side holes, and a model shim that perturbs no guest
   state (oracle PASS, 7/7).
2. **Under the kernel's true range semantics the dynarec's sync protocol is complete.** The ground
   truth is uOFW's reverse-engineered `sysmem`. The result holds for the shipped 3.0.0 ROM publish,
   for 8b48b48, and for bonly. On all seven `dr_oracle` fixtures (22,266 frames, each fixture
   including a state load), every model reports **0** stale executions. That includes the
   strictest one, which assumes any line may be resident at any time. No emitter write anywhere is
   left uncovered by an I-invalidate.
3. **The one thing the twin cannot see is how the firmware handles large ranges, and the hardware
   bisection points exactly there.**
   - `sceKernelIcacheInvalidateRange` switches algorithms at **16 KiB**, the I-cache size. Below
     that it runs an exact `cache 0x8` loop over every line. At 16 KiB or more it walks the tag RAM
     instead.
   - The dynarec hands it ranges ≥16 KiB **only** at site B: the out-of-line
     `translate_icache_sync` behind the dual and Thumb lookups.
   - It does so **in the state-load frame** of every fixture except AW2 (H&S, Unbound, Emerald),
     and AW2 never issues one. These are trees of up to 83 KiB, translated
     at once by the eager exit linking in `translate_block_thumb`.

   If that firmware path misses lines, the twin flags exactly the shipped core. It flags 2,609
   incidents in H&S heavy, in the state-load frame and in the tail-line pattern below. 8b48b48 and
   bonly are clean. So is the one-line rule proposed below. This hypothesis matches every
   hardware result of the bisection, including the two that a plain missing-sync bug cannot
   explain: "rounding B's range to lines does not help" and "site E does not matter".
4. **Proposed precise fix (needs one hardware confirmation):** never hand the kernel an I range of
   16 KiB or more. Invalidate the whole I-cache for those ranges instead; the cost is 256 line
   operations, the same as the 16 KiB loop it replaces.
   - Twin cost: 1-12 whole invalidates per run, against 1,704-5,179 for 8b48b48 and bonly.
   - Two ready-made tests decide it: a patched copy of the exact failing EBOOT, and a
     firmware-only probe. See "Hardware tests to run".
5. **The uninitialised ROM-buffer suspect is real, but it is not this bug.** `init_bios_hooks`
   translates BIOS `0x8` from an unwritten ROM block during `retro_load_game`.
   - Given a perturbed heap, the translator **crashes at boot** by patching a NULL
     `backpatch_address`.
   - On the Go/3000 LARGE tier that block is the lent static array, which is zeroed .bss, as in
   the twin. With zeroed memory the twin's runs are coherent and guest-identical.

## 1. Method

### 1.1 What the twin already gives

The drprof twin runs the PSP's own emitter (`mips/mips_emit.h`, `mips/mips_stub.S`,
`cpu_threaded.c`) as a static mipsel Linux program under qemu. It is bit-identical, frame by frame,
to the PSP harness (docs/DYNAREC-PROFILE.md, "fidelity of the twin"). qemu is coherent: it always
executes the bytes the emitter last stored. The checker turns that into an oracle. It runs the
real program and asks, at every fetch, what an Allegrex would have fetched instead.

### 1.2 Making the twin issue the PSP's maintenance calls one-to-one

- **`cpu_threaded.c`**, under `JITCOH_PSP_CACHE` (twin only). The PSP branch of
  `platform_cache_sync` and of the ROM publish in `translate_icache_sync` is compiled unchanged,
  against four functions named and typed like the kernel's:
  - `sceKernelDcacheWritebackRange`
  - `sceKernelIcacheInvalidateRange`
  - `sceKernelDcacheWritebackInvalidateAll`
  - `sceKernelIcacheInvalidateAll`

  In the twin they live in `tools/drprof/dr_host.c`.
- **The mailbox.** QEMU 9.2 exposes no MIPS registers to plugins. So each shim posts
  `{addr, size, caller, op}` to the mailbox `jitcoh_mbox`, op last. The plugin applies the
  operation when it sees the op store. The result is one model operation per PSP call, with the
  PSP call's arguments.
- **The patch handler.** On the PSP, `emit_phand` emits `cache 0x1A` (D hit writeback) and then
  `cache 0x08` (I hit invalidate) on the patched `jal`. The twin emits `synci` there instead. The
  plugin resolves each `synci` to the address of the patch store just before it: same base
  register and offset, verified at runtime. Over the whole suite, 0 were unresolved.
- **`drprof_xlat_pc`** (`drprof.h`, `DRPROF_TWIN`) publishes the guest PC of the translation in
  progress. It is saved and restored around each nested `translate_block_*`, so every emitter store
  is attributed to its guest block.
- **Layout.** The twin's caches start at `...f20`. `jshift=20` (the default) models them on the
  64-byte line alignment of the PSP's `memalign(64)` LARGE tier, so every line boundary is the
  PSP's.

### 1.3 The models (one run evaluates all of them on the same fetch stream)

| model | I-cache | D-cache | flags |
|---|---|---|---|
| **HAZ_I** ("hazard") | never evicts | coherent | a line fetched after its last I-invalidate, written, then the written word executed |
| **HAZ_D** | none (every fetch reads memory) | never evicts | a word executed before its write reached memory |
| **HAZ_ID** | never evicts | never evicts | both at once (worst case) |
| **HAZ_S** (strict) | any line may be resident at any time | coherent | a word executed after a value-changing write with no I-invalidate of its line since. This covers fills the model cannot see: prefetch, or fetches down a branch path that is not taken |
| **ACT_o<k>** ("actual") | 16 KiB, 2-way, 64 B, LRU; translated code shifted k lines against C code (a layout sweep) | LRU 16 KiB, 2-way, 64 B, write-back/allocate (`dlru=1`), or never-evict | what the real geometry would deliver |
| **ACT_o<k>j** | as ACT, but only translated code competes for the cache | as ACT | a bound for layouts whose C code evicts less than the twin's |

Geometry: the I-cache was measured on both consoles (docs/CACHE-MAP.md). The D-cache is
**assumed** to have the same geometry: the Allegrex's documented 16 KiB 2-way D-cache with
64-byte lines, write-back and write-allocate. Every result below holds for HAZ_D, which needs no
D geometry.

**Kernel range semantics.** These are taken from the PSP firmware's own code, as reverse-engineered
by uOFW (`src/kd/sysmem/start.S`, `sceKernelL1IcacheInvalidateRange` and
`sceKernelL1DcacheWritebackRange`):

- **I invalidate, size < 16 KiB:** the start is rounded down and the end up, and `cache 0x8` runs
  on every line. The range is inclusive.
- **I invalidate, size ≥ 16 KiB:** a tag-RAM walk over 8 KiB of indices (`cache 0x0`, then
  TAG_LO/TAG_HI), which hit-invalidates lines whose address, rebuilt from the tag, is in range.
- **D writeback, size < 64 KiB:** an exact inclusive loop of `cache 0x1A`.
- **D writeback, size ≥ 64 KiB:** a tag walk. As written it covers a superset of the range, which
  is harmless.

The plugin models the inclusive semantics by default. `isem=`/`dsem=` `naive|floor` model buggy
loops; they are kept only as sensitivity tests, because the firmware refutes them.
`ibig=noop`/`dbig=noop` model "the large-range path does nothing" (section 4).

### 1.4 Self-checks in every run

- `shadow_mismatch=0`: the checker's copy of every translated word equals the bytes qemu
  translated, checked at every TB translation (millions per run).
- `synci_unresolved=0`.
- `dr_oracle.py jc-nocoh jc-base` (the same tree with and without `JITCOH_PSP_CACHE`):
  **IDENTICAL on all 7 fixtures**. The shim and the bookkeeping change no guest state.
- `dr_oracle.py jc-base jc-ship`: **IDENTICAL 7/7**. As expected in a coherent machine, the
  shipped and fixed ROM publishes differ only in cache calls.

## 2. Checker validation (deliberately broken twins, `tools/jitcoh/variants/brk-*.patch`)

Incidents, meaning distinct (word, write) pairs executed stale, for H&S heavy, Unbound rival and
AW2. A0 and A101 are the ACT layouts with offsets 0 and 101 lines.

| variant | H&S heavy HAZ_I / HAZ_D / HAZ_S / A0 / A101 | Unbound rival | AW2 |
|---|---|---|---|
| base (sync intact) | 0 / 0 / 0 / 0 / 0 | 0 everywhere | 0 everywhere |
| **brk-ramsync**: RAM publish removed | 3,527,012 / 7,734,982 / 7,262,693 / 7.7M / 7.7M | 0.5M / 6.5M / 6.1M / 6.5M / 6.5M | 2,018 / 14,636 / 10,299 / … |
| **brk-thunk**: `smc_stable_thunk_link` sync removed | 19,434 / 60,672 / 60,606 / 60,672 / 60,672 | 12,637 / 54,314 / … | **0** (AW2 never retires a block) |
| **brk-thunk-ionly**: thunk written back, I not invalidated | 19,434 / **0** / 60,606 / 12,970 / 19,379 | 12,637 / **0** / 54,213 / 11,453 / **1,976** | 0 |
| **brk-phand**: patch-handler `synci` removed | 16,323 / 22,985 / 19,901 / 22,985 / 22,985 | 40,627 / 55,607 / … | 13,929 / 20,017 / … |

What this shows:

- Removing any one sync site is flagged. Restoring it gives 0.
- An I-only hole leaves HAZ_D at **0**: the models tell the I side from the D side, which is
  exactly the discrimination the hardware bisection made.
- With the real geometry, the same hole costs between 1,976 and 12,383 incidents depending only on
  the layout offset. This reproduces the bug's layout sensitivity in miniature.
- **The patch handler's hole is guest-invisible by design.** Every specialised memory handler
  re-checks its region and alignment. The checks are in `emit_pmemld_stub`, `emit_pmemst_stub`,
  `emit_openload_stub` and `emit_saveaccess_stub`, and each branches to the patch handler on a
  mismatch. So a stale memory-op `jal` reaches a handler that re-patches it. That handler's
  user-mode `cache` pair is therefore not a correctness dependency.

## 3. The three code variants under the kernel's true semantics

Whole `dr_oracle` suite (aw2, hns_heavy, hns_light, ub_rival, ub_double, ub_ow, em_battle). All
include a state load at frame 30. Every model, every layout offset:

| variant | what it is | all fixtures, all models |
|---|---|---|
| `ship` (`variants/ship.patch`) | the 3.0.0 ROM publish: ranged D + ranged I | **0** |
| `base` (this tree) | 8b48b48: ranged D + whole I on every ROM publish | **0** |
| `bonly` (`variants/bonly.patch`) | d7df3bb: whole I except on the ARM lookup | **0** (run under `ibig=noop`, which is strictly harsher) |

Also clean:
- `dlru=1` (D eviction modelled; H&S heavy, all four variants);
- `MALLOC_PERTURB_=85` and `=170` (a garbage heap; shipped core on aw2, hns_heavy, ub_rival and
  em_battle), which are also guest-identical to the unperturbed runs.

**Conclusion.** With the kernel calls behaving as the firmware code says, the dynarec leaves no
translated word stale for any fill policy: every value-changing write is followed by an
I-invalidate of its line before it executes. So **no emitter write site is the stale writer.**
There is nothing in the emitter's own sync protocol to fix.

The "naive loop" hypothesis is refuted by the firmware code itself. It assumed a kernel loop of
`for (p = addr; p < end; p += 64)` that misses an unaligned range's last line. It would have
flagged the RAM publishes (101 incidents in 120 frames of the shipped core).

## 4. The firmware's ≥16 KiB path: where the evidence points

### 4.1 Who hands the kernel ≥16 KiB ranges, and when

These are the `B` lines the plugin logs for every large call, from the shipped core over the whole
suite, excluding one boot-time call that is a twin artifact (section 6):

| fixture | ≥16 KiB I ranges | largest | frames |
|---|---|---|---|
| hns_heavy | 11 | 42,028 B | boot (10, 14, 15, 16, 16), **30, 30, 30** (state load), 535, 549, 1426 |
| hns_light | 11 | 42,028 B | the same pattern, **30 ×3** |
| ub_rival | 7 | 83,232 B | 13, **30 ×2**, 823, 852, 853 ×2 |
| ub_double | 10 | 83,512 B | 13, **30 ×2, 31 ×2**, 878, 906, 910 ×3 |
| ub_ow | 3 | 22,472 B | 13, **30 ×2** |
| em_battle | 1 | 21,464 B | **30** |
| aw2 | **0** | — | — |

**Every one of the 43 is issued by `translate_icache_sync`'s ROM half, called from
`block_lookup_address_dual` or `block_lookup_address_thumb`.** On the PSP that is the out-of-line
copy, **site B** in the bisection. **None comes from the ARM lookup's inlined copy (site E).**

The windows are this large because `translate_block_thumb`'s eager exit linking calls
`block_lookup_translate_thumb` for every direct exit (`cpu_threaded.c`, the loop at
`translation_target = block_lookup_translate_thumb(branch_target)`). That translates whole trees
of blocks recursively before the lookup publishes once. After a ROM flush, which every state load
does (`flush_dynarec_caches` → `flush_translation_cache_rom` resets the pointer to the watermark),
the first lookups rebuild the game's hot Thumb code in a few such trees, over addresses that held
the pre-load code.

### 4.2 If that path misses lines (`ibig=noop`)

| variant | aw2 | hns_heavy | hns_light | ub_rival | ub_double | ub_ow | em_battle |
|---|---|---|---|---|---|---|---|
| ship, HAZ_I | 0 | **2,609** | 5,044 | 489 | 859 | 394 | 528 |
| ship, HAZ_S | 0 | 10,414 | 13,747 | 5,483 | 7,500 | 1,087 | 598 |
| base (8b48b48) | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| bonly | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| **ship + `variants/bigall.patch`** | 0 | **0** | 0 | 0 | 0 | 0 | 0 |

Detail for H&S heavy, shipped core, `ibig=noop`:

- **The state-load frame (30) carries 2,404 of the stale executions.** The lines hold code from
  frames 2-16 at addresses the post-load trees rewrote. The writers are `translate_block_thumb`
  stores: the block prologue, `translate_thumb_instruction`, the exit stubs and the exit links. The
  invalidate that missed them each time is the ≥16 KiB ROM publish from site B, of sizes 0x443c,
  0x6940 and 0xa42c.
- **With the real geometry**, the twin's own translator footprint evicts those pre-load lines
  before they re-execute, at all 16 layout offsets swept, with D-LRU on or off.
- **When only translated code competes for the cache** (`ACT_o0j`), 5 real stale executions remain
  (frame 1426). Their mechanism needs no long-lived line:
  1. a small publish `[0x01003e60,+0x58)` is executed, so its last line (block code, then the
     unwritten bytes after it) enters the I-cache;
  2. the next tree, 20 KiB, starts **inside that same line** and is published in one call through
     the large path;
  3. the CPU then executes the cached zeros instead of the new block's first instructions.

  This line was fetched moments before the write, so eviction cannot be counted on.

### 4.3 Against the hardware record (docs/RESIDENT-ICACHE-FIX.md, memory notes)

| hardware result | missing emitter sync | firmware ≥16 KiB path misses lines |
|---|---|---|
| dies the first frame after a state load | possible | yes: the large trees come right after the ROM flush (frame 30/31) |
| whole D+I at B cures; no other single site does | only if B held the hole, but the twin finds no hole anywhere | yes: B issues all the large ranges |
| ranged D + **whole I** at B cures; ranged I + whole D dies | I side | I side |
| B's range **rounded to whole lines** dies | **contradicts**: rounding cures a ragged-edge hole | yes: still ≥16 KiB, still the tag walk |
| site E (ARM lookup) irrelevant | no prediction | yes: E never issues a large range |
| layout-sensitive (.text/.bss shift hides it; whole-MiB heap moves don't); Go always, 3000 sometimes | yes | yes: whether the stale line survives depends on what else fills its set |
| `cache_paranoid` (every sync whole) cures | yes | yes |
| `swap_stubs=1` / paged ROM cure | no prediction | layout (the stub size shifts every translation); not decisive |
| PPSSPP never | yes | yes |

**The tag walk is the only unverified link.** Its code is plausible: one index-load-tag per set,
reading both ways from TAG_LO and TAG_HI. But the firmware mostly calls it on freshly loaded
modules (`loadcore`), which were never executed, so a walk that missed lines would almost never
show there. Two tests settle it (section 5).

## 5. Hardware tests to run (staged, not run: the consoles belong to the main session)

Both are in `builds/jit-coherency-out/`.

### 5.1 The exact failing EBOOT, one call changed at site B

The binary-patch method of `builds/hns-patch` is used: no relayout. The method is verified here by
reproducing `pBA` byte for byte, and `base.PBP` is f4bc41ad itself. A 7-word helper sits in the
same code cave `pBA` used (over `jit_coherency_scan`, so keep `jit_coherency_scan = 0`). The
helper:

    sltiu t0,a1,0x4000 ; bnez t0,1f ; nop ; j IcacheInvalidateAll ; nop ; 1: j IcacheInvalidateRange ; nop

Each patched EBOOT redirects one `jal IcacheInvalidateRange` (`0x8976818`) to the helper. The
other stubs in this EBOOT are `0x8976810` (IcacheInvalidateAll), `0x8976800`
(DcacheWritebackRange) and `0x89767f8` (DcacheWritebackInvalidateAll). `PATCHES.json` lists every
byte.

| EBOOT | change | prediction if the large path is the cause |
|---|---|---|
| `base.PBP` (f4bc41ad) | none | dies (control) |
| **`pBS.PBP`** (50a4578c) | site B: I range ≥16 KiB → whole I, else unchanged | **alive** |
| `pBCS.PBP` (8548c4f7) | pBS + site C (RAM publish) | alive |
| `pBSctl.PBP` (785c8800) | same helper and call shape, but the threshold can never be met | **dies**: it controls for "any patch at B changes timing" |

Run it the way the bisection ran: the hnsrig H&S heavy state, `load_state=1`, the Go (where it
fails deterministically), and ABBA against base. **pBS alive with pBSctl dead convicts the
≥16 KiB path.** pBS dying refutes this hypothesis; the twin result then stands (no emitter hole),
and the cause lies outside what a cache model of the emitter can see.

### 5.2 Firmware probe, no emulator: harness `icache_inval_probe = 1`

The probe is `psp/icprobe.c` `icinv_probe_run`. The EBOOT is
`builds/jit-coherency-out/harness64-icinv` (pbp c91102c1).

How it works:
1. 64 one-line functions (`jr ra; li v0, OLD+i`) are made resident by calling them.
2. They are rewritten to return NEW+i, and D is written back exactly.
3. One invalidate mode is applied, and every function is called again.

Modes:
- `none` (residency control);
- `all`;
- `r_exact`;
- `r_16383` (the last size on the loop path) and `r_16384` (the first size on the tag walk);
- `r_32k` and `r_mid32k`;
- `d_big` (one 64 KiB D writeback, then whole I).

Each mode runs at three region offsets, 7 repetitions each, and every result is logged as
`EVT icinv … stale_sum=…`.

Reading it:
- **Valid run:** `none` > 0 and `all` = 0.
- **Verdict:** `r_16383` = 0 with `r_16384` / `r_32k` / `r_mid32k` > 0 means the kernel's
  large-range path misses lines.
- **PPSSPP negative control, run here:** every mode reads 0, `none` included, because PPSSPP models
  no cache. The probe runs cleanly.

## 6. Other findings

- **Upstream: the dynarec is enabled before its caches are initialised.**
  `retro_load_game → check_variables → main_enable_dynarec → init_emitter → init_bios_hooks`
  translates BIOS `0x8` before the BIOS and the cart are loaded, from ROM block 0, which is
  unwritten memory.
  - **Crash.** With a garbage heap (`MALLOC_PERTURB_=1`) the twin dies right there, in
    `translate_block_arm`. A block whose last ARM condition is 0xF never sets `backpatch_address`,
    and `generate_branch_patch_conditional(NULL, …)` writes to address 0.
  - **Bogus sync.** The same path calls `translate_icache_sync` while `last_ram_translation_ptr` is
    still NULL, so its RAM half syncs `[0, ram_translation_ptr)`. In the twin that is 23 MiB from
    address 0.
  - **Where it can bite.** On a LARGE-tier PSP both are benign. Block 0 is the lent static array,
    which is zeroed .bss, and the pointers are set in `dynarec_select_translation_caches`. On the
    SMALL tier (PSP-1000, or a short heap), block 0 comes from the heap and the RAM pointer is
    NULL, so both can bite: a boot crash, or one huge kernel call.
  - **Fix.** Zero block 0 before the first translation, as the `GBA_LINK` builds do
    (docs/GBA-LINK-MIRROR.md 4c), and initialise both `last_*` pointers with their caches. Better
    still, do not translate before `load_gamepak`.
- **The I-cache side of ROM residency is clean.** `rom_canary` already showed that no resident ROM
  page changes. The twin adds that no translated word is ever stale under correct kernel
  semantics, so lending the small-JIT array as ROM blocks 0-1 is not the hole.

## 7. Recommended fix and code locations

1. **`cpu_threaded.c`, `platform_cache_sync` (the PSP branch).** Ranges of 16 KiB or more get
   `sceKernelIcacheInvalidateAll()`; everything else keeps the ranged call. The D side is
   unchanged (`variants/bigall.patch`, 4 lines).
   - It never uses the firmware's large path.
   - It costs no more than the 16 KiB loop it replaces.
   - It is triggered 1-12 times per fixture run instead of 1,704-5,179 (8b48b48 / bonly), so it
     should not carry 8b48b48's AW2 cost.
   - It covers every caller (ROM and RAM publishes, init, recovery), not just site B.
2. **8b48b48** (whole I on every ROM publish) stays as the shipped safety net until test 5.1 or
   5.2 convicts the large path. Then it can be replaced by (1). If the tests clear the large
   path, keep 8b48b48 and treat the cause as outside the emitter's sync protocol (section 4.3).
3. **Section 6** (zero ROM block 0; initialise `last_*` pointers before the first translation) is
   independent of (1) and (2) and fixes real latent bugs in any case.

## 8. Using the checker (and for `claude/jit-code-discipline`)

    # WSL, from the tree; binaries in ~/drprof/bin, runs in ~/drprof/runs/jc/
    tools/jitcoh/jc_build.sh NAME [variants/X.patch ...]   # -> dr_host_jc-NAME (+ plugin)
    tools/jitcoh/jc_suite.sh jc-NAME [PREFIX]              # whole fixture library, exit 1 if anything flags
    JC_ARGS="ioff=0.37.64.101,maxev=4000,jshift=20,ibig=noop" tools/jitcoh/jc_suite.sh jc-NAME P
    tools/jitcoh/jc_report.py ~/drprof/runs/jc/P/<fixture>/coh.txt ~/drprof/bin/dr_host_jc-NAME \
        --model HAZ_I --events 5     # inside the drprof-qemu image (needs mipsel addr2line)

Plugin arguments: `ioff=` (dot-separated layout offsets; suffix `j` = translated code only),
`dlru=1`, `jshift=`, `isem=`/`dsem=`, `ibig=`/`dbig=` `noop`, `maxev=`. `JC_QENV=VAR=val` sets a
guest environment variable (for example `MALLOC_PERTURB_`). `JC_PSPCACHE=0` builds the same twin
without the PSP calls, for the oracle. The whole suite takes about 3.5 minutes per variant on 16
cores.

**What the refactor needs from it.** Port the core glue to the refactored tree:
- the `JITCOH_PSP_CACHE` block in `cpu_threaded.c`;
- `DRPROF_XLAT_PC_PUSH`/`POP` in `drprof.h` and around the two `translate_block_*` calls;
- the `dr_host.c` shims;
- the `tools/drprof/Makefile` link fixes (`fe_gblink.c`, `dr_gbstub.c`).

The single writer API's publish point then calls the same four kernel functions. The gates are:
- `jc_suite.sh` **0 in every model** (the discipline must not open a hole);
- `ibig=noop` **also 0** (the publish point must never hand the kernel an I range of 16 KiB or
  more);
- `dr_oracle.py` IDENTICAL against `jc-nocoh`.

Keep at least one `brk-*` negative control alive against the new code: the old patches will not
apply once the call sites move, so write the equivalent "drop one publish" switch.

## Files

- `tools/jitcoh/jitcoh_plugin.c`: the checker.
- `tools/jitcoh/jc_build.sh`, `jc_run.sh`, `jc_suite.sh`, `jc_report.py`.
- `tools/jitcoh/variants/`: `ship`, `bonly`, `bigall`, and `brk-{ramsync,thunk,thunk-ionly,phand}`.
- Twin glue:
  - `cpu_threaded.c` (`JITCOH_PSP_CACHE`, `drprof_xlat_pc`);
  - `drprof.h`;
  - `tools/drprof/dr_host.c` (kernel shims);
  - `tools/drprof/Makefile` + `dr_gbstub.c` (the twin linked again after the GB dual-link merge).
- `psp/icprobe.c`, `psp/main_psp.c`: harness `icache_inval_probe`.
- `builds/jit-coherency-out/` (not in git): `base.PBP`, `pBS.PBP`, `pBCS.PBP`, `pBSctl.PBP`,
  `PATCHES.json`, `harness64-icinv/`.
