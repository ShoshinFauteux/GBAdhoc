# PCARG_SIDE: the PC argument leaves the call site

Branch `claude/pcarg-side` (base claude/beta5-base e70d60aa). Switch `PCARG_SIDE` (gpsp_features.h, on;
`PCARG_SIDE=0` builds it off). Scripts and twin data: `builds/pcarg-out/`.

## 1. Why

The PSP's translated code is I-cache bound (16 KiB, 2-way, 64 B lines): codegen ideas that add hot bytes lost
4-12 % on hardware even with fewer instructions, and only ideas that shrink hot code pay. In the hot translated
bytes of the shooters, the largest removable item is the PC argument of a memory call site
(claude/cache-aware-codegen, docs/CACHE-AWARE-CODEGEN.md section 4: memory call sites are 36 % of Doom's hot
bytes, the PC argument 8 %):

    load:   jal stub ; addiu a1, reg_pc, imm          store:  addiu a2, reg_pc, imm ; mov a1 ; jal stub (mov a1 in the slot)

`imm` is the site's PC delta, and since the MonoMyth fix it also carries `k`, the cycles `reg_cycles` runs
ahead of the access (iok_encode). Only the stubs' slow paths read it: open bus and BIOS protection (openload),
the live timer counters (I/O loads of TM0..3), I/O writes and `smc_write`. The fast paths never look.

An older version (codegen-shrink a7f2ddcd, loads only, no `k`) kept its PC table as a trailer INSIDE the
translation cache: hardware -0.2..-1.8 % on 8 of 9 games, but Hot Wheels +2.1 % because the trailers made RAM
code bigger (RAM translation flushes 229 -> 276).

## 2. Design

* **New-form site.** Load: `jal stub ; X`, where X is the access's last address word (computed by this access,
  not a branch and not in a delay slot), swapped into the delay slot. Store: `jal stub ; mov a1` (the value
  word keeps the slot; ARM and Thumb). One word less per site. Swap, LDM/STM and every other a1/a2 user stay
  as they were.
* **Side table, outside the translation caches.** Per cache (ROM, RAM): `key` = the site's return address
  (jal + 8), `imm` = the immediate the addiu would have carried, so the argument is exactly `reg_pc + imm`,
  `k` included. Entries are kept sorted by address: a block's sites are appended when the block is committed
  (code is only written at a cache's translation_ptr), and a block that starts below the end of the last one
  (the pointer moved back: a flush, or an abandoned translation) first drops every entry at or after it.
  One 128 KiB object per core instance (`pcarg_st`): ROM 12,288 entries, RAM 8,192, a 512-entry front cache.
  A full table, a block with 256 new-form sites, or a word that may not move makes the site OLD form
  (unchanged code); the stubs handle both, so the fallback is exact.
* **Recovery.** `mips_stub.S pcarg_dec`: return address -> front cache (direct mapped) -> on a miss
  `pcarg_lookup` (binary search, fills the front cache). Found: argument = `reg_pc + imm`; not found: the
  site is old form and its register holds the argument. Then the same decode as `emit_iok_decode`.
* **Stubs.** The openload and I/O-write decodes and the timer-read path call pcarg_dec after their register
  save; `smc_write` too. The BIOS stub's inline "PC in BIOS?" test needs the PC, so unaligned BIOS reads go
  to the openload C path, which applies the same protection (`read_memory*`). Every rewritten slow path is
  padded to its old size (`pcarg_pad_to`): no stub moves, so the hot stubs keep their I-cache sets, and
  `mips_stub.o(.text)` (the head of the hot order) keeps its size (`pcarg_dec` has its own section).
* **Determinism.** The table's content is a function of the translation history alone; the front cache only
  changes how fast an answer comes. The JIT code's guest effect is identical by construction (section 3).

## 3. Exactness: the pad oracle

`PCARG_SIDE_PAD=1` (twin only) emits a nop after every new-form site, so every block keeps the base's size,
every flush happens at the same block, and the guest must run exactly as the base. Twins of the cf44b8a9e624
manifest (e70d60aa) vs the same plus PCARG_SIDE_PAD, 3,900 frames (the Lab window): **state hashes identical
on Doom II, Duke, Ice Nine, Top Gear Rally, Hot Wheels, Emerald battle and FireRed** (Emerald with 6,056
sites past the ROM cap, so both forms mixed). The real arm (no pad) is identical on Emerald and FireRed and
diverges by timing on the others (smaller RAM code moves a full flush; first difference at frame 179 Hot
Wheels .. 3,667 Doom): Hot Wheels' cache resets 286 -> 237 in the window.

## 4. Twin prediction (cache-aware twin, docs/CACHE-AWARE-CODEGEN.md)

Arms: twin of cf44b8a9e624's manifest (e70d60aa) vs the same + PCARG_SIDE; placement from the cf44 lab ELF;
frames 300..3900 (Ice Nine and Emerald end with their scripts); 3.6 ns/instruction + 215 ns/miss;
% of hardware work ms.

| fixture | instr/frame | misses/frame | pred. pinned base | mean of 4 bases (range) |
|---|---:|---:|---:|---:|
| Doom II | -2.67 % | -1,819 | -3.30 % | -2.92 % (-3.54 .. -1.97) |
| Duke Nukem Advance | -2.78 % | -1,308 | -2.59 % | -2.14 % (-2.59 .. -1.34) |
| Ice Nine | -2.68 % | -804 | -1.96 % | -2.07 % (-2.21 .. -1.94) |
| Top Gear Rally | -1.96 % | +550 | +0.06 % | 0.00 % (-0.24 .. +0.10) |
| Hot Wheels | -3.17 % | -708 | -2.03 % | -2.32 % (-2.73 .. -2.03) |
| Emerald battle (control) | -0.98 % | -748 | -2.94 % | -2.98 % (-7.42 .. -0.45) |
| FireRed (control) | -0.90 % | -1 | -0.38 % | -0.22 % (-0.38 .. -0.02) |

Top Gear Rally is flat: its instructions fall like the others' but its translated-code misses rise (it diverges
by timing at frame 212, before the window, so this is two different executions). Recovery is cheap: 3-18 C
lookups a frame (the front cache answers the rest).

Re-scored on the final code (12,288 / 8,192 entries) with the arm's OWN placement (its re-pinned lab ELF,
section 5; base: the cf44 ELF), same window:

| fixture | instr/frame | misses/frame | pred. pinned base | mean of 4 bases (range) |
|---|---:|---:|---:|---:|
| Doom II | -2.39 % | -1,971 | -3.40 % | -2.84 % (-3.40 .. -2.28) |
| Duke Nukem Advance | -1.90 % | -1,556 | -2.60 % | -2.15 % (-2.60 .. -1.83) |
| Ice Nine | -1.79 % | -602 | -1.39 % | -1.07 % (-1.62 .. -0.06) |
| Top Gear Rally | -3.18 % | +656 | -0.18 % | -0.73 % (-1.54 .. -0.18) |
| Hot Wheels | -3.72 % | -906 | -2.46 % | -2.69 % (-3.73 .. -1.95) |
| Emerald battle (control) | -0.65 % | -744 | -2.79 % | -2.52 % (-6.87 .. +0.16) |
| FireRed (control) | -0.75 % | -27 | -0.41 % | +0.03 % (-0.41 .. +0.36) |

The twin read code-shape changes at 0.7-1.0x of hardware and placement at about half
(docs/CACHE-AWARE-CODEGEN.md section 6 on claude/cache-aware-codegen): read these as "about 2-3 % on Doom, Duke
and Hot Wheels, 1 % on Ice Nine, Top Gear Rally unclear".

## 5. Layout (re-pinned, docs/LAYOUT-PINNING.md)

The emitted stubs keep every offset (each rewritten slow path is padded to its old size) and `mips_stub.o(.text)`
keeps its size. In the EBOOT, the out-of-line site emitters shrink both instances' translators (release:
translate_block_thumb -2,304 B, translate_block_arm -28,096 B; lab -28,160 B) and the side table's code adds
4,256 B (mod 8 KiB) of cold text per instance. Three pads give it back:

| pad | where | release / lab |
|---|---|---:|
| `LAYOUT_PAD_XLAT_BYTES` (new, psp/Makefile, lab-pads.env) | in front of A's translate_block_arm (psp/layout_pad_warm.S); B's hot path follows it | 5,824 / 5,888 |
| B's translator pad | after B's translate_block_arm (psp/gbalink_b_stubs.c) | 5,856 |
| `.text.pcarg_layout_pad` | the core's cold text, both instances (mips_emit.h, PSP only) | 3,936 |

The side table is one 128 KiB object per instance, so `.bss` after it moves by whole ways. Result against the
cf44b8a9e624 lab ELF (and the e70d60aa release ELF): layout.lock [lab] and [release] 37/37 with the committed
values; every data/bss symbol at the same offset mod 8 KiB; of the hot-order functions only the two
translate_block_arm copies moved (their code changed); 62 (lab) / 60 (release) text symbols moved in all, all
of them the core's rewritten cold code. PSP-1000 boot heap: release 11,228,572 B, lab 10,614,172 B (both
-229,376 B: A's and B's 128 KiB tables less 32 KiB of translator text; B's table sits in B's region, which a
solo game lends to A's ROM cache, so a solo game loses 96 KiB of ROM page room).

## 6. JIT counters (PPSSPP, lab builds, 3,900-frame fixtures)

`EVT core_health` at the end of each fixture (builds/pcarg-out/jt_run.py, the Lab's perf harness keys; both
builds on the SMALL tier, PPSSPP 64 MiB model): `jitc` = publishes / lines / whole-I / early, `own` =
ownership changes by ROM flush / **RAM flush** / rebuild.

| fixture | jitc cf44b8a9e624 | jitc PCARG | own (RAM flushes) | rom_flush |
|---|---|---|---|---|
| Doom II | 141012/362009/32/0 | 140880/331334/30/0 | 3/28/2 -> 3/26/2 | 3 -> 3 |
| Duke Nukem Advance | 85205/198217/24/0 | 85072/187086/22/0 | 3/21/2 -> 3/19/2 | 3 -> 3 |
| Hot Wheels | 198004/2062308/640/3266 | 180057/1588209/380/3385 | 3/307/2 -> 3/238/2 | 3 -> 3 |
| Top Gear Rally | 163400/1242373/40/6110 | 160087/871049/31/3497 | 3/20/2 -> 3/18/2 | 3 -> 3 |

RAM translation flushes fall on all four (Hot Wheels -22 %: the old in-cache trailer's +21 % is reversed), ROM
flushes are unchanged, and the published lines fall 8-30 %. `EVT jit_cache`: both at mod8k 0x1500; the
watermark is 8 B lower (23,908 vs 23,916) because it is taken after init_bios_hooks translates the BIOS SWI
entry, whose two single loads each lost their word; the emitted stubs themselves keep every offset.

## 7. Correctness gate (Lab gate CLI, candidate = the lab kit, base cf44b8a9e624)

* **Smoke: PASS**, 6/6 (em_battle, gb_shantae_town, hns_heavy, kf_doom_ii, kf_need_for_speed_underground,
  ub_rival_medium).
* **Full: 46/47 fixtures PASS** (Payback and Mario Tennis by the gate's own lockstep: timing), **stale PASS**
  (0 stale executions in 37 GBA fixtures), **test ROMs PASS** (jsmolka 6 -> 6/13), **corpus 32/35 identical**
  plus Mario Tennis timing. The gate said NEEDS-REVIEW for three divergences its lockstep could not reproduce:
  Ice Nine (twin from frame 65; PPSSPP one frame, 3838, reconverges), Golden Sun: The Lost Age (corpus f283)
  and Mario Golf (corpus f1155, reconverges). Its instrumented twin runs a 16x RAM cache, where PCARG's smaller
  code never moves a flush, so base and candidate do not differ at all there (L1 identical).
* **The same lockstep at the plain twin's cache size** (builds/pcarg-out/ls_small.py: the gate's own build,
  Pair and classify, lockstep defines + `-DRAM_CACHE_KB=512`, the gate's frame rule) reproduces all three, and
  classifies all three **timing**: the candidate dynarec equals the interpreter replaying its own I/O and IRQs
  for 53.97 M (Ice Nine), 19.98 M (Golden Sun TLA) and 40.00 M (Mario Golf) instructions; the builds first
  differ at a timer-counter read one tick apart (Ice Nine, TM0CNT_L 0xd07e / 0xd07d) and at an IRQ taken
  at a different instruction (the other two).
* With the pad oracle (section 3: same code size, hash-identical to the base on seven fixtures), this puts every
  difference on flush timing.

Lab build **ff63e05c6264** (`pcarg-bef8021-lab`, builds on cf44b8a9e624). The hardware A/B against cf44b8a9e624
is layout-identical by the lock (section 5), so it measures the codegen.
