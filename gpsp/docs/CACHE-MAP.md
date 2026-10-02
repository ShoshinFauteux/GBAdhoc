# Instruction-cache map for the two GB cores (dual emulation)

Branch `claude/gb-dual-link`.  Question (GB-LINK.md §10: DUAL costs 2.1x
SOLO, "the remainder is code layout"): do the two TGB Dual instances, which
alternate every scanline, evict each other's code from the Allegrex
instruction cache, and can a linker layout that keeps their hot code in
disjoint cache sets win that back?

**Answer (2026-09-30): no layout can.**  About 2 % of the dual misses are
conflict misses -- 0.16 ms of a ~13 ms frame -- and the rest are capacity
misses.  The geometry the simulations assume was then MEASURED on both
consoles (16 KiB, 2-way, 64 B lines; a miss costs 215 ns), so the result
stands on hardware.  No layout-changing link was built.  The lever that
works is scanline batching, which is on by default since hw5.

## Tools (`tools/cachemap/`)

| tool | what |
|---|---|
| `twin_profile.sh` | builds the two cores exactly as psp/Makefile does (one partial link, the copy renamed `gbcoreb_*`) for mipsel with the PSP's -O3 -g, runs `linkplay` under qemu-mipsel with `icount_plugin` (every executed instruction address, per-frame marker `gbdual_advance`, the first 120 frames skipped) |
| `cachemap.py` | charges the twin's counts to the PSP ELF through both DWARF line tables (copy A/B told apart by the rename delta), adds the PSP-only per-frame path (`psp_frame_path.txt`: frontend, video, audio, netdrv), maps hot lines to sets and scores conflicts; `--emit-map` writes twin-address to PSP-address maps; `--relocate` applies a candidate layout before any link |
| `icsim_plugin.c` / `icsim.sh` | replays the twin's instruction fetches at PSP addresses through LRU set-associative caches, one per layout, in one run (`CACHE=size=,ways=,line=`) |
| `layout.py` | candidate layouts as relocations: `shiftB<k>`, `hotpack<k>`, `color<a>_<p>` (each copy's hottest p % in its own sets) |
| `psp/icprobe.c` + `icprobe_geometry.py` | the geometry MEASURED on a console (harness `icache_probe = 1`, rig arm I) |

`psp/Makefile` builds with `-g`: line tables for the map.  The EBOOT is
packed from the stripped ELF, so the shipped bytes are unchanged.

## Results: the rig's Gen 2 trade (arm B), 10,930 frames, strict alternation

Every row replays the same trace.  Misses are per frame.

| cache | layout | misses | A | B |
|---|---|---|---|---|
| 16 KiB 2-way 64 B (the Allegrex, measured) | as linked | 36,307 | 19,684 | 15,589 |
| same | copy B shifted 8-112 lines (10 variants) | 35.6k-37.0k | | |
| same | hot-first packing | 36.3k-37.0k | | |
| same | colouring, A gets 32-96 of 128 sets, 60-95 % coloured | 36,249-60,842 | | |
| **16 KiB 16-way** (no conflicts to speak of) | as linked | **35,547** | 19,352 | 15,413 |
| 32 KiB 2-way | as linked | 17,127 | 9,515 | 6,972 |
| 16 KiB 2-way, batching 154 | as linked | 31,227 | 17,310 | 12,937 |
| 16 KiB 16-way, batching 154 | as linked | 27,013 | 14,845 | 11,507 |
| 16 KiB 2-way, copy A alone | as linked | 11,428 | | |
| 16 KiB 16-way, copy A alone | as linked | 5,457 | | |

What this shows:
* **Dual:** even a nearly fully associative 16 KiB cache misses 35.5k a frame
  against 36.3k at 2-way.  Only ~2 % of the misses are conflicts, which is
  all a layout can remove, and every layout tried lands within noise or
  worse.  The hot code does not fit: 50 % of a core's executions sit in
  5.8 KiB, 75 % in 17.6 KiB, 90 % in 30-35 KiB.  Colouring halves each
  core's cache, and that costs more than it saves.
* **Solo:** half the misses ARE conflicts (11.4k vs 5.5k), so a single core
  would gain from layout.  But solo GB runs far above full speed on every
  console, so there is nothing to buy.
* **What works:** fewer core switches.  Batching 154 cuts misses by 14 %
  (2-way) and 24 % (16-way).  hw5 measured it on the PSP: core -0.65 ms mean,
  -1.7 ms median per frame (GB-LINK.md §12).
* **Stability:** a layout-changing link would need a soak gate (the freeze is
  contained, not root-caused) to buy at most ~2 %.  Not done.

Limits of the model: the twin is GCC for mipsel-linux, mapped to the PSP
binary by source line; it models the core code only, not other threads, the
kernel or interrupts (all of which add capacity pressure, so the conclusion
only gets stronger); D-cache not modelled.

## In milliseconds (the measured miss cost)

At 215.3 ns per miss (below), from the table:

| case | misses/frame | ms/frame in misses |
|---|---|---|
| dual, strict, 2-way (the PSP) | 36,307 | **7.82** |
| dual, strict, 16-way (no conflicts) | 35,547 | 7.65 |
| **conflict share (all a layout could remove)** | 760 | **0.16** |
| dual, batching 154, 2-way | 31,227 | 6.72 (-1.09) |
| copy A alone, 2-way | 11,428 | 2.46 |
| copy A alone, 16-way | 5,457 | 1.17 |
| copy B alone, 2-way | 6,019 | 1.30 |
| dual, 32 KiB 2-way (not a PSP) | 17,127 | 3.69 |

* Dual vs solo: the dual trace spends ~5.4 ms a frame more in I-cache misses
  than copy A alone (7.82 vs 2.46).  hw2 measured the whole DUAL-SOLO gap at
  ~6.85 ms (12.80 vs 5.95 ms core, milestone 1).  So by this estimate most of
  the cost of the second machine is its code evicting the first machine's,
  not its instructions.
* The best any layout could do is ~0.16 ms a frame (about 1 % of a 13 ms
  frame), against a layout-changing link that needs a soak gate.  Not done.
* Batching's simulated -1.09 ms sits between hw5's measured -0.65 ms (mean)
  and -1.7 ms (median).  The model is an estimate (the twin's code, mapped by
  source line; no other threads), but it lands in the measured range.

## Geometry on hardware (measured 2026-09-30, rig arm I, logs/cache1)

Both consoles, twice each, identical:

| console | geometry | hit | miss |
|---|---|---|---|
| PSP-3000 (host) | 16,384 B = 2-way x 8,192 B, 64 B lines, 128 sets | 9.9 ns | +215.2 / +215.4 ns |
| PSP-1000 (join) | the same | 9.9 ns | +215.2 / +215.4 ns |

This is the geometry the tools default to and every simulation above used.
The miss is ~72 cycles at 333 MHz.  Harness EBOOT 838178b4 (c8f12a2);
`score_rig.py` RIG VERDICT PASS (arm I on both, and the N link session
after it).

`icache_probe = 1` (psp/icprobe.c) runs generated code in user mode:
- chains of K one-line blocks at stride S;
- at S = way size every block falls in one set, so the chain misses from
  K = ways + 1;
- each stride is read as a ratio to the S = 64 row (same code, same per-pass
  loop overhead, never conflicts);
- cold passes at strides 16-256 give the line size.

How the probe works.  `tools/cachemap/icprobe_geometry.py` reports size, ways, line and the miss
cost in ns.  It says NO_GEOMETRY for a log without a step (PPSSPP, which
models no cache: its negative control, checked) and CONTRADICTORY rather than
guessing.  Five simulated geometries x 3 noise seeds are recovered exactly
(`tools/tests/test_icprobe_geometry.py`, gb suite).  The miss cost turns the
table above into ms per frame (misses x ns).

`icsim.sh` takes `CACHE=size=..,ways=..,line=..` for other geometries (the
Media Engine's, for instance, if it ever runs a core).

## For the two-gpSP-core GBA design

The dynarec's hot code is its translation cache, generated at run time, not
the ELF.  So an ELF layout map covers only the interpreter, the memory
handlers and the frontend.  The method carries over:
- the probe gives the geometry and miss cost;
- a trace replayed through `icsim` at the addresses code actually occupies
  (for gpSP, the translation-cache addresses) separates conflict misses
  (layout can help) from capacity misses (only fewer switches or less code
  can help).

Answer that split first, before any layout work.
