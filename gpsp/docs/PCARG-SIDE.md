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
  In DRPROF_HW builds the miss path's `pcarg_lookup` publishes hardware phase 5 (`DRPH_PCARG`, "pcarg"), not
  flush/SMC; the front cache itself writes no phase (JIT glue, phase 0).
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

**Camelot and SMC_REUSE-heavy titles** (added after review: the gate's lockstep cannot tell a wrong `k` from
timing, because its interpreter replays the candidate's own I/O values and IRQ times, and its LAB_LOCKSTEP build
takes another timer-read path). Plain gate twins (`gate_twin.build`, TWIN=0, shipping paths, no LAB_LOCKSTEP)
of e70d60aa vs 474f7899 + PCARG_SIDE_PAD, both `-DRAM_CACHE_KB=384` (the PSP's RAM cache), state + framebuffer
hash every frame (builds/pcarg-out/pad_smc.py, pad_smc.json): **identical on all nine runs**: Lab fixtures
kf_mario_golf_advance_tour (3,900), kf_mario_tennis_power_tour (3,900), own_mario_tennis (3,900), kf_payback
(2,200), and corpus power-on runs of Golden Sun: The Lost Age, Golden Sun, Mario Golf, Mario Tennis and Payback
(3,000 each). The pad arm's counters on the same runs (TWIN=1 bookkeeping, same emitted code; pad_ctr.py):

| run | new-form sites | old (table full) | truncations | C lookups | SMC_IMM_PATCH patches |
|---|---:|---:|---:|---:|---:|
| kf_payback | 25,591,330 | 3,091,306 | 3,732 | 183,687 | 1,537 (SMC_REUSE norec 1,589) |
| Payback corpus | 22,042,020 | 2,584,091 | 3,016 | 153,399 | 714 (norec 1,376) |
| kf_mario_golf_advance_tour | 159,960 | 0 | 100 | 5,262 | 0 (learned 79) |
| kf_mario_tennis_power_tour | 138,889 | 0 | 61 | 7,746 | 0 |
| own_mario_tennis | 112,251 | 0 | 51 | 4,766 | 0 |
| Mario Golf corpus | 105,967 | 0 | 72 | 4,808 | 0 |
| Mario Tennis corpus | 84,883 | 0 | 71 | 7,693 | 0 |
| Golden Sun TLA corpus | 31,012 | 0 | 36 | 5,884 | 0 |
| Golden Sun corpus | 15,975 | 0 | 18 | 1,831 | 0 |

`bad=0` everywhere. Payback fills both tables (max ROM 12,288, RAM 8,192 entries) and mixes the site forms,
rewinds the tables thousands of times and runs SMC_IMM_PATCH over revived SMC_REUSE bodies; it stays
hash-identical. So the Ice Nine, Golden Sun TLA and Mario Golf divergences of the gate (section 7) are not a
wrong recovered argument: with the code size held, the guest runs exactly as the base.

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
4,256 B (mod 8 KiB) of cold text per instance (4,736 B with the range check of section 8). Three pads give it back:

| pad | where | release / lab |
|---|---|---:|
| `LAYOUT_PAD_XLAT_BYTES` (new, psp/Makefile, lab-pads.env) | in front of A's translate_block_arm (psp/layout_pad_warm.S); B's hot path follows it | 5,824 / 5,888 |
| B's translator pad | after B's translate_block_arm (psp/gbalink_b_stubs.c) | 5,856 |
| `.text.pcarg_layout_pad` | the core's cold text, both instances (mips_emit.h, PSP only) | 3,936 (3,456 with section 8) |

The side table is one 128 KiB object per instance, so `.bss` after it moves by whole ways. Result against the
cf44b8a9e624 lab ELF (and the e70d60aa release ELF): layout.lock [lab] and [release] 37/37 with the committed
values; every data/bss symbol at the same offset mod 8 KiB; of the hot-order functions only the two
translate_block_arm copies moved (their code changed); 62 (lab) / 60 (release) text symbols moved in all, all
of them the core's rewritten cold code. PSP-1000 boot heap: release 11,228,572 B, lab 10,614,172 B (both
-229,376 B: A's and B's 128 KiB tables less 32 KiB of translator text).

**ROM page cache: the arm gets one block MORE in solo play, not less** (an earlier version of this section had
the sign wrong). A solo game lends B's region (`gbab_bregion_start` .. `gbab_bregion_lend`) to A's ROM cache
as spare pool 2, in whole 1 MiB blocks. B's 128 KiB table lies inside that span, so it grows from 0x3F0000
(4,032 KiB: 3 blocks) to 0x410000 (4,160 KiB: 4 blocks), in both the lab and the release ELF. Heap blocks are
taken after the spare pools. In PPSSPP's model the 224 KiB smaller heap costs no heap block, so the total rises
by one: `EVT rom_cache` Hot Wheels blocks 12 -> 13, spare 3 -> 4, pages 382 -> 386; Duke and TGR 12 -> 13; Doom
II (`plan=heap_short`, a 16 MiB cart that pages) 14 -> 15. On a PSP-1000, whose heap is ~5 MiB smaller than
PPSSPP's, the smaller heap could still cost a heap block, so there the net is +1 or 0 blocks per game.

So the hardware A/B is NOT controlled for the ROM cache by the layout lock alone: on a cart that pages (Doom
II, any `resident=0` fixture) an extra ROM block can speed the arm up by itself. For a codegen verdict, stage
the same `rom_cap = N` (GPSP_PERF_RIG harness key, both arms) at the base's block count, or read each arm's
`EVT rom_cache blocks=` / `rom_diag` and report the difference with the result. A cart that is resident
(`resident=1`: Duke, Hot Wheels and TGR in PPSSPP's model) never pages, so the extra block goes unused; check
the flag on the hardware runs, since a PSP-1000 has less room.

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
* With the pad oracle (section 3: same code size, hash-identical to the base on seven fixtures plus nine
  Camelot / SMC_REUSE runs, Golden Sun TLA and Mario Golf among them), this puts every difference on flush timing.

Lab build **ff63e05c6264** (`pcarg-bef8021-lab`, builds on cf44b8a9e624). The hardware A/B against cf44b8a9e624
is layout-identical by the lock (section 5). 474f7899 (the hardware phase fix) changes DRPROF_HW builds only;
the lab and release builds compile it out, so ff63e05c6264 stays the build under test. The A/B is not
ROM-cache-identical, though: pin `rom_cap` or report the `EVT rom_cache` block counts (section 5) before crediting a speed difference to the codegen.

## 8. Review hardening (second review; claude/beta5-base b2e97a19, re-pinned in d9e9ce04)

The second review found no bug and asked for three things.

* **The s16 range (F3).** The side table stores an s16. For a plain site (no `k`) the old form's
  `generate_load_pc` emits the absolute PC (`lui/ori`) when `new_pc - stored_pc` is outside [-32768, 32767],
  and no immediate can stand for that. `pcarg_near` (mips_emit.h) is the one compare, at translation time, in
  `pcarg_load_site`, `pcarg_arm_store_site` and `pcarg_thumb_store_site`: such a site keeps the old form,
  byte for byte what a `PCARG_SIDE=0` build emits. It is never taken today: `stored_pc` is the block's start
  and a block is at most 1024 instructions, so the delta is 0 .. 4100. Note what the fallback does and does
  not buy: the stubs' decoders read a plain PC only in [0, `IOK_PLAIN`), with or without PCARG_SIDE, so a
  block that long needs the decoders changed too. The fallback only keeps PCARG_SIDE from adding a silent
  truncation of its own. Counter: `far` on the twin's `PCARG sites` line.
* **The registers `pcarg_dec` loses (F4).** Its header in mips_stub.S now lists them: `$8`, `$9` (guest r2, r3:
  the in/out pair) and `$10`-`$12` (r4-r6) on every call; on a front-cache miss also `$13`-`$15` (r7-r9),
  `$24`, `$25` (r11, r12) and `$28` (r13), because only `$1`-`$9` and `$ra` are spilled around the C call.
  That is any build, not only a PIC one: o32 lets `pcarg_lookup` clobber them. Every caller runs after the
  register save.
* **Where `bad == 0` is checked (F7).** The release has no runtime check of the table (sorted keys, every
  entry a call); `pcarg_bad` counts both under DRPROF_TWIN only, and the Lab gate's twins are plain (TWIN=0),
  so the gate never prints it. The check lives in `tools/drprof/pcarg_check.py`: run it for every change to
  mips/mips_emit.h, mips/mips_stub.S or cpu_threaded.c's translator, before the gate. It builds five twins of
  the candidate with the Lab's gate_twin (`-DRAM_CACHE_KB=384`) and fails unless, on every fixture:
  the pad arm (`PCARG_SIDE_PAD=1`) and the forced arm (`PCARG_SIDE_PAD=1 PCARG_NEAR_MAX=63`: every plain site
  more than 63 bytes into its block takes the F3 fallback, and still emits the base's `addiu`) are
  hash-identical to the base on every frame, and the same two arms built with TWIN=1 report `bad=0`, with
  `far=0` in the pad arm and `far>0` in the forced arm.

      python -I gpsp/tools/drprof/pcarg_check.py --lab-backend LAB/backend \
        --base-manifest BASE/build-manifest.json --cand COMMIT --cand-manifest CAND/build-manifest.json

Run on b2e97a19 against e70d60aa (cf44b8a9e624's source), state and framebuffer hash on every frame
(d9e9ce04 changes only the PSP layout pad and the lab pads after it): **PASS, 4 of 4**.

| fixture (frames) | pad arm | forced arm | pad: new / old / bad / far | forced: new / old / bad / far |
|---|---|---|---|---|
| kf_payback (2,200) | identical | identical | 25,591,330 / 3,091,306 / 0 / 0 | 24,112,817 / 4,569,819 / 0 / 1,722,895 |
| kf_ice_nine (3,900) | identical | identical | 40,053 / 0 / 0 / 0 | 39,250 / 803 / 0 / 803 |
| kf_doom_ii (3,900) | identical | identical | 64,776 / 0 / 0 / 0 | 62,136 / 2,640 / 0 / 2,640 |
| kf_hot_wheels_stunt_track_challenge (3,900) | identical | identical | 384,711 / 0 / 0 / 0 | 377,678 / 7,033 / 0 / 7,033 |

Payback's pad-arm counters equal section 3's (the hardening changes no emitted byte when the range holds), and
its forced arm mixes all three reasons for an old-form site (table full 2,846,924, forced 1,722,895) with
3,732 table truncations.

Layout: the three emitters grew 480 B per instance; `.text.pcarg_layout_pad` gives it back (3,936 -> 3,456),
docs/LAYOUT-PINNING.md "Keeping the pads". PSP-1000 boot heap unchanged (release 11,228,572 B, lab 10,614,172 B).

Gate of the merged line (Lab build **e787e61fcbe9**, `b5int3-d9e9ce0-lab`, base cf44b8a9e624): smoke PASS 6/6;
full: **47/47 fixtures PASS** (Payback by the gate's lockstep: timing), stale PASS (0 in 37 GBA fixtures), test
ROMs PASS (jsmolka 6 -> 6/13), corpus 32/35 identical plus Mario Tennis timing. The gate's verdict is
NEEDS-REVIEW for the same two corpus ROMs as in section 7, at the same frames (Golden Sun: The Lost Age f283,
Mario Golf f1155, reconverges), which its 16x-cache lockstep cannot reproduce. The lockstep at the plain twin's
cache size (section 7's method, builds/b5int3-out/ls_small.py) classifies both **timing** on this build, with
section 7's figures: the candidate dynarec equals the interpreter replaying its own I/O and IRQs for 19.98 M and
40.00 M instructions.
