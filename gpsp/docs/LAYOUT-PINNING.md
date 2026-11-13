# Layout pinning: where the GBA hot path sits in the I-cache

Branch `claude/layout-pinning-all` (claude/candidate-3.1-all fa89f4e plus
`claude/layout-pinning`), 2026-10-01.  **`LAYOUT_PIN` is OFF by default** (owner
guardrail): the default release EBOOT is byte-identical to fa89f4e's.
`LAYOUT_PIN=1` is the opt-in for the hardware bench, which is staged
(section 7).

## 0. Answer in one screen

* **The swing is mostly the translation cache's address, not the order of the
  C functions.**  The emitter writes its hottest code, the memory-access stubs
  every guest load and store calls, at the start of the ROM translation cache.
  On a 64 MiB console that cache is a `memalign(64)` heap block.  The heap
  starts at the end of `.bss`, so the block's address mod 8 KiB moves one for
  one with any change in code or data size.  Hardware logs prove it: the five
  benched builds' logged bases are `_end + 0x101a40 ± 0x80` (table, section 2).
  The I-cache is 16 KiB, 2-way, with 64 B lines (measured), so only the address
  mod 8 KiB matters, and that one number moves with every unrelated change.
* **Simulated on the twin**, AW2's I-cache miss time across the 32 possible
  cache placements ranges from 0.47 to 1.56 ms per frame (3.3×).  On the
  harness64 binaries the bench runs, padding alone moves AW2 between 0.48 and
  1.57 ms per frame.  The ±1 ms hardware swings between unrelated builds
  (GB-link-only -15 %, ME-fix-only +19 %) are that size.
* **Pinned** (`LAYOUT_PIN=1`), two things change:
  1. Both translation-cache tiers start at a fixed offset (0x1500) into an
     8 KiB way.
  2. The profiled per-frame path (the asm dispatcher/stubs, update_gba, sound,
     IRQ, memset/memcpy ...) is linked first in `.text`, in a generated order.

  Across 4 paddings of `.text` and `.bss` (64 B to 12 KB, plus a 768 B cold
  function), **no hot-path function moves, no hot-core line changes set, and
  the JIT stays at its pinned offset mod 8 KiB.**  Unpinned, the same paddings move every
  hot function and the JIT base.  (The JIT offset was 0x1300 when those pad
  builds were linked; the 0x1500 bench pair is checked the same way.)
* **Simulated gain** (release binaries, ms of miss time per frame, against the
  unpinned build's real placement):
  - AW2: 0.47 → 0.43
  - H&S heavy: 1.80 → 1.68
  - Unbound rival: 4.90 → 3.70
  - Emerald: 0.80 → 0.71

  Pinned beats even the BEST of the 32 unpinned placements on all four
  fixtures.  On the harness64 bench binaries the unpinned build sits at the
  unlucky end: AW2 1.49 ms pinned to 0.43 ms.
* **Correctness.**  The twin oracle is bit-identical, pinned base against
  unpinned, on 4 fixtures (14,361 frames, audio included).  PPSSPP `shash` is
  bit-identical on AW2 (6,061 frames).  On H&S it differs only on 20 IWRAM
  frames, which a same-EBOOT control run reproduces exactly (the cartridge RTC
  reads the live clock).
* **Cost.**  +228 B of `.text` (section alignment) and +17.4 KB of `.bss` (the
  8 KiB-aligned static cache), measured on the bench pair.  The LARGE tier
  takes up to 13.3 KB more heap.
  The ME PRX is unchanged (19ae57b6) in every build.
* **What hardware must answer:** whether the simulated gains and the pad
  stability hold on a console.  Staged as the 3000 queue's SLOT step: 16 runs,
  ~2 h (section 7).

## 1. Mechanism and method

### 1.1 The twin, placed at PSP addresses

`tools/drprof` (the dynarec twin: the PSP's translator under qemu-mipsel) runs
the fixtures.  `tools/layout/lpsim_plugin.c` replays every executed fetch
through LRU caches with the measured geometry (16 KiB, 2-way, 64 B lines), one
cache per candidate layout, all on the same trace.  Each fetch is first placed
at its PSP address:

* **Static code** goes by function name to the PSP ELF of that layout, at the
  same fraction of the function (`lp_map.py`).  Names rather than line tables,
  because the twin carries drprof hooks and a different compiler.
* **Translated code** goes to `JIT base + offset`: the emitted code and its
  order are the PSP's, so only the base differs.
* **What is left out.**  The renderer (`video.o`) runs on the ME and is not
  placed.  The twin's harness (`main`, `fe_crc32`) is excluded.
* **PSP-only per-frame code** (update_scanline's ME-capture head ×160, the ME
  host, audio, plat_video_frame) is fetched at every frame marker.  Its rates
  are placeholders from `tools/layout/psp_frame_path.txt`.

A miss costs 215.3 ns (measured, docs/CACHE-MAP.md).  Numbers in this
document are misses per frame and that cost in ms.  They are not frame time:
other threads, the D-cache and the kernel are not modelled.

### 1.2 The profile and the hot set

Fixtures are those of `dr_suite.sh`, profiled on the twin of THIS tree's
release dynarec (`DISPATCH_CACHE`, `SMC_RETIRE_WINDOW`, `SERIAL_IDLE_FAST`):

| fixture | frames |
|---|---|
| AW2 tour | 6,015 |
| H&S heavy | 3,043 |
| Unbound rival | 3,043 |
| Emerald battle | 2,045 |

No GB/GBC fixture: GB code is not on the GBA path (docs/CACHE-MAP.md covers GB).

Footprint covering 90 % of placed static fetches:

| fixture | by 64 B line | by whole function |
|---|---|---|
| AW2 | 3.4 KiB | 6.5 KiB |
| H&S | 11.0 KiB | — |
| Unbound | 9.8 KiB | — |
| Emerald | 4.9 KiB | 27 KiB |

H&S and Unbound need `translate_block_arm` for whole-function coverage, and it
is 367 KiB.  The emitted stub area covers 90 % in about 1 KiB.

`gen_layout.py weights` writes `tools/layout/profile_weights.txt` (each fixture
normalised to 1.0, summed).  `gen_layout.py generate` builds the order:

1. `mips_stub.o`'s `.text`, about 2.2 KiB: the dispatcher, the update_gba
   entry and the mode switches.
2. The HOT core: weight-dense functions up to 6144 B.  These are update_gba,
   sound_timer, sound_read_samples, check_and_raise_interrupts, update_serial,
   memset, set_cpu_mode, ...  update_scanline goes last, so only its ME head
   is in the core.
3. The WARM tail: every other profiled function.  It is pinned too.

Prebuilt libc/libgcc routines have no per-function sections, so they are
placed by archive member (`*:libc_a-memset.o`; newlib links as `libg.a`).

### 1.3 The link

| what | how |
|---|---|
| per-function sections | `-ffunction-sections` (root Makefile `LAYOUT_PIN=1` for the core, psp/Makefile for the frontend; the partial-linked GB core is excluded) |
| order | `psp-ld --section-ordering-file=psp/layout/hot.ord` (binutils 2.44 supports it) |
| static SMALL tier | `.balign 8192` + `.space JIT_PIN_OFFSET` in mips_stub.S |
| LARGE tier | `memalign(8192, size + JIT_PIN_OFFSET)`, base = block + JIT_PIN_OFFSET (cpu_threaded.c) |
| `JIT_PIN_OFFSET` | 0x1500 (gpsp_config.h), the centre of the best simulated plateau, 0x1200-0x1700 (section 3) |
| ordering file | generated, never hand-edited: `tools/layout/regen.sh` (from a `LAYOUT_PIN=1` build) rewrites `hot_order.txt` and `hot.ord` |

## 2. Today's swing is the JIT base: hardware evidence

The Go isolation bench (`candidate-out/drl-go-iso3`, AW2, ABBA) and the 3000
bench give one logged base per build.  The ELFs were rebuilt bit-exact (only
the `__TIME__` bytes differ).

| build | `_end` | logged JIT base | base − `_end` | base mod 8 KiB | Go AW2 mean |
|---|---|---|---|---|---|
| public 3.0.0 | 08f17e30 | 090198c0 | 0x101a90 | 0x18c0 | 6.35 ms |
| 3.0.0 + ME fix | 08f1a580 | 0901bfc0 | 0x101a40 | 0x1fc0 | 7.53 ms |
| 3.0.0 + GB link | 08f6dcb0 | 0906f6c0 | 0x101a10 | 0x16c0 | 5.37 ms |
| candidate − I-fix | 08f70400 | 09071dc0 | 0x1019c0 | 0x1dc0 | 6.98 ms |
| candidate (3000 log) | 08f70880 | 090722c0 | 0x101a40 | 0x02c0 | — |

**The calibration is honest, not good.**  Simulating these four builds at
their logged bases (the candidate twin's trace) ranks GB-link best.  That is
right.  It ranks the ME fix ahead of 3.0.0, which is wrong.  The ME-fix build
also runs different CPU code per line, and the trace did not include it.  The
model predicts the size of the effect and where it comes from, not each
build's number.  The bench (section 7) is the test.

## 3. Simulated results: release, claude/candidate-3.1-all

ms of miss time per frame (misses × 215.3 ns).

**Unpinned (candidate-3.1-all as is):**

| fixture | at its own base (0x0540) | 4 paddings | 32 possible bases: min / median / max |
|---|---|---|---|
| AW2 | 0.47 | 0.46-0.48 | 0.47 / 0.73 / 1.56 |
| H&S heavy | 1.80 | 1.79-1.84 | 1.77 / 1.87 / 2.10 |
| Unbound rival | 4.90 | 4.90-5.00 | 3.98 / 4.98 / 6.06 |
| Emerald battle | 0.80 | 0.79-0.82 | 0.77 / 0.83 / 1.22 |

**Pinned (0x1500):**

| fixture | ms/frame | vs unpinned at its base |
|---|---|---|
| AW2 | 0.43 | −9 % |
| H&S heavy | 1.68 | −7 % |
| Unbound rival | 3.70 | −25 % |
| Emerald battle | 0.71 | −12 % |

Choosing the budget and offset (`sim_report.py --pick`; the per-fixture
misses are normalised to each fixture's median, then averaged):

| layout | normalised misses over the offset sweep |
|---|---|
| unpinned | 0.98-1.52 |
| 3072 B core | 0.86-1.40 |
| 6144 B core | 0.87-1.41 |

The 6144 B core's plateau at 0x1200-0x1700 is 0.87-0.92, and 0x1500 is its
centre.  Choosing an offset is choosing a placement.  Pinning makes that
choice stay put: the static code is fixed, so a code change can no longer
re-roll it.

## 4. Stability: unrelated code no longer moves the hot path

`tools/layout/pad_check.py` compares each pad variant to its unpadded build
(`builds/layout-pin-out/all/pads`).  The pads are `psp/layout_pad.S`'s
`LAYOUT_PAD_TEXT`/`LAYOUT_PAD_BSS`, linked first, plus `LAYOUT_PAD_BYTES`, a
never-called 768 B function in main_psp.o.

The hot core is 36 functions (19 asm) and 269 cache lines; the warm tail is
75 functions.

| variant | pinned: core moved | pinned: warm moved | pinned: core lines changing set | pinned: JIT mod 8 KiB | unpinned: core moved | unpinned: lines changing set | unpinned: JIT base mod 8 KiB (LARGE tier) |
|---|---|---|---|---|---|---|---|
| none | 0 | 0 | 0/269 | 0x1300 | — | — | 0x0540 |
| text 64 + bss 64 | 0 | 0 | 0/269 | 0x1300 | 36/36 | 269/269 | 0x0540 |
| text 2600 + bss 4100 | 0 | 0 | 0/269 | 0x1300 | 36/36 | 269/269 | 0x1f40 |
| text 10004 + bss 12340 | 0 | 0 | 0/269 | 0x1300 | 36/36 | 269/269 | 0x1c40 |
| cold function 768 B | 0 | 0 | 0/269 | 0x1300 | 36/36 | 269/269 | 0x0840 |

The pinned rows were checked at both budgets (3072 and 6144).  These release
pad builds were linked while `JIT_PIN_OFFSET` was still 0x1300.  The final
0x1500 is checked on the bench pair: harness64 `lppin` against `lppinpad`
(text 2600 + bss 1216) gives 0 moved and 0/269 sets, with the JIT at 0x1500 in
both and the same `.bss` size.  The unpinned LARGE-tier column is the model
`_end + 0x101a40` (section 2).  In the
simulation, all five pinned variants give the same miss count, to the miss, on
all four fixtures.  The unpinned variants differ, and the 32-placement sweep
shows how far a less lucky padding goes: AW2 up to 1.56 ms.

The first attempt left 7 libc routines floating (memset, memcpy, free, ...):
they have no `.text.<name>` sections.  Placing them by archive member fixed it.
`pad_check.py --expect-pinned` exits non-zero if anything in the hot core moves.

## 5. Correctness

* **Twin oracle** (`twin_build.sh` variants `allrt` and `allrtpin`: runtime
  tier, unpinned vs pinned base; plus `allbase`, the fixed 10 MiB cache).
  Per-frame guest hashes (registers, IWRAM, EWRAM, I/O, palette, OAM, VRAM,
  audio) are IDENTICAL on all frames:

  | fixture | frames |
  |---|---|
  | AW2 | 6,061 |
  | H&S heavy | 3,100 |
  | Unbound rival | 3,100 |
  | Emerald | 2,100 |

  In the twin, the pinned static cache sat at 0x1300 mod 8 KiB (the offset
  then) against 0x1e60 unpinned, so the emitted code ran at different
  addresses.  Some first runs were OOM-killed (12 qemus at once) and were
  rerun one at a time.  Their common prefixes already matched.
* **PPSSPP shash, harness64.**  Reference: fa89f4e (base 0x907e6c0).  Pinned:
  base 0x9085500, i.e. 0x1500 mod 8 KiB.
  - AW2: IDENTICAL on 6,061 frames.
  - H&S heavy: 20 frames (11-30) differ in IWRAM only.  A second run of the
    REFERENCE EBOOT differs from the first on the same 20 frames, same field.
    That is the cartridge RTC, which reads the console clock in the harness.
    Every other field is identical on all 3,100 frames.
* **Default build** (`LAYOUT_PIN` unset): the release `EBOOT.PBP` md5
  23db6a49 is byte-identical to fa89f4e's release EBOOT (`GPSP_BUILD_VERSION`
  set to fa89f4e).  The ME PRX is 19ae57b6 in all builds.

## 6. Cost

* `.text` +228 B (section alignment; 292 B in the release link).  Code is
  unchanged; only addresses move.
* `.bss` +17,408 B (harness64, `JIT_PIN_OFFSET` 0x1500): the 8 KiB alignment
  of the SMALL tier's arrays, plus the offset.
* The LARGE tier allocates up to 8 KiB + 0x1500 more from the heap.  The probe
  is sized for it.
* **PSP-1000:** its ROM page cache takes the heap that is left.  17 KB less
  heap could cost it one 1 MiB block when the heap is near a block edge.
  This is not measured; check it with a 32 MiB harness build before pinning is
  enabled there.
* **Process:** when the per-frame path changes, run the twin profile again,
  then `regen.sh`.  A stale order still links; the pins hold, but the core
  slowly stops being the hot one.

## 7. Hardware bench (staged; consoles untouched by this agent)

`builds/hw-queue/stages/slot-3000/`, the 3000 queue's SLOT step.
`hwqueue.py verify` reports OK and `plan` lists it: 16 runs, about 113 min.
The arms are in `builds/hw-queue/slot_arms/layout_pin.py`, loaded by a
fail-safe additive hook in `queue_arms.py`; without slot files the arm table
is unchanged (md5-checked).  Restage with `tools/layout/stage_slot.py`.

All four builds are harness64 with ME PRX 19ae57b6.  AW2 tour (A) and H&S
heavy PAGED (H, `rom_cap 15`), ABBA:
`L-A-OFF L-A-PIN L-A-PINPAD L-A-OFFPAD L-H-OFF L-H-PIN L-H-PINPAD L-H-OFFPAD`,
then reversed.

| arm build | what | simulated AW2 | simulated H&S |
|---|---|---|---|
| `lpoff` | candidate-3.1-all fa89f4e as is | 1.49 | 1.90 |
| `lppin` | `LAYOUT_PIN=1` | 0.43 | 1.68 |
| `lppinpad` | `LAYOUT_PIN=1` + text 2600 / bss 1216 pad | 0.43 | 1.68 |
| `lpoffpad` | `LAYOUT_PIN=0` + the same pad | 0.48 | 1.80 |

Simulated values are ms of miss time per frame.

The pad was picked from 7 candidates as the one the simulation predicts moves
the unpinned build most.  Unpinned AW2 across those 7 pads ranges 0.48-1.57 ms.

**Judge** (`candidate-out/bench_score.py` logic, mean `core_prof` after the
load window):

1. **Stability** (the goal): `lppinpad` − `lppin` lies within the ABBA
   run-to-run spread on both fixtures.
2. **Mechanism:** `lpoffpad` − `lpoff` is clearly non-zero on AW2.  Predicted
   about −1 ms, if the model's size is right.
3. **Gain:** `lppin` ≤ `lpoff`, predicted about −1 ms on AW2 and −0.2 ms on
   H&S.

Every run must exit 0 and have `bjrec=0`.  If 1 holds and 2 does not, pinning
is still stable, but the cause of today's swing is elsewhere.  If 1 fails,
find what moved with `pad_check.py`.

**Follow-ups, not staged:**
* the same four arms on the Go, whose queue is one stage owned by the main
  session;
* a 32 MiB harness pair on the PSP-1000 for the SMALL tier and the heap cost.

## 8. Reproducing

```
# twin (WSL; private home ~/lp, because dr_build.sh rsyncs --delete into the shared ~/drprof)
tools/layout/twin_build.sh . allbase "" "-DDISPATCH_CACHE=1 -DSMC_RETIRE_WINDOW=1 -DSERIAL_IDLE_FAST=1"
tools/layout/twin_sim.sh ora/allbase/aw2 aw2 allbase -          # profile (counts.txt)
# order (Git Bash)
LAYOUT_PIN=1 tools/build.sh release && tools/layout/regen.sh
# layouts + simulation
tools/layout/pad_relink.sh OUT PIN TEXT BSS [FNPAD]            # PROFILE=harness64 for bench builds
tools/layout/mk_layouts.py --lp ~/lp --set S --fixture aw2 --twin allbase --prof runs/ora/allbase \
   --frame-path tools/layout/psp_frame_path.txt --sweep 256 --sweep-elfs NAME --elf NAME=elf.nm[@base]
tools/layout/twin_sim.sh sim/S-aw2 aw2 allbase /w/sim/S/aw2.lay
tools/layout/sim_report.py ~/lp/runs/sim/S-* --pick
tools/layout/pad_check.py --order psp/layout/hot_order.txt --asm-nm mips_stub.nm ref=a.nm pad=b.nm
```

Gotchas met on the way:
* `git diff` in WSL fails on a Windows worktree, so generate patches from Git
  Bash.
* `mips_stub.S` is CRLF in the worktree, so patch with `--fuzz=0` after
  normalising.  A fuzzed hunk once landed a `.balign` inside the dispatcher.
* `twin_sim.sh`'s JIT range for a `RUNTIME_JIT_CACHE` twin is wrong: dr_host
  reads the pointer variable's address.  Profile on the fixed-cache twin.

## Keeping the pads

The hot order and the JIT offset hold by themselves. Instance A's cold text, its `.rodata`, `.data` and `.bss` hold only if what is linked before them changes by a multiple of 8 KiB, so a change to the frontend's size re-tunes:

- **5.0.0 integration, the SMC_REUSE call-site fix (claude/beta5-base b84fb5d8): `LAYOUT_PAD_XLAT_BYTES` 5824 -> 5504 (lab 5888 -> 5504), B's translator pad 5856 -> 5504, `.text.pcarg_layout_pad` 3456 -> 3584; every other pad unchanged, nothing re-recorded.** PCARG's three site emitters now log each `jal` they write (docs/PCARG-SIDE.md section 9) through `smc_reuse_log_site_ool`, a new 128 B function in each instance's cold text. The compiler answered with more than the emitters' own change: `translate_block_thumb` grew 352 B in both instances, A's `translate_block_arm` moved one 32 B step (release -32, lab +32), the emitters came out at 704 / 800 / 672 B (load / ARM store / Thumb store; 672 / 1,120 / 640 before) and the stub emitters (`emit_pmemst_stub` .. `emit_openload_stub`) are now placed in front of them. Net: +320 B (release) / +384 B (lab) in front of B's hot path, +352 B in front of the late section and `main`, -128 B of cold text per instance. The XLAT pad takes the first, B's translator pad the second, the cold pad the third. moddiff against the d9e9ce04 ELFs (e787e61fcbe9's kit and its release twin): every symbol at or above 0x08800000 is at +0 except A's `translate_block_arm` (+32 release, -32 lab), B's `translate_block_arm` (+352, its own start; the pad after it absorbs it), and the cold block between `pcarg_commit` (+128) and the first function after the emitters in each instance (translation-time and init-time code only). `.rodata`, `.data`, `.bss`, B's region and the JIT bases +0; layout.lock [release] and [lab] 37 entries, 0 differ; image and PSP-1000 boot heap unchanged (release 11,228,572 B, lab 10,614,172 B).
- **5.0.0 integration (claude/beta5-base d9e9ce04: claude/p-tidy and claude/pcarg-side merged, PCARG's review hardening): release pads unchanged; `.text.pcarg_layout_pad` 3936 -> 3456; lab CORE 7708 -> 4892, RO 384 -> 3200.** PCARG_SIDE's own pads are in docs/PCARG-SIDE.md section 5 (XLAT 5824 release / 5888 lab, B's translator pad 5856, the core's cold pad). The hardening (the s16 range check in the three site emitters) grows `pcarg_load_site` 456 -> 672 B, `pcarg_arm_store_site` 960 -> 1100 and `pcarg_thumb_store_site` 484 -> 628 in each core instance, 480 B after the 32 B rounding. The cold pad sits directly in front of `pcarg_commit` and the emitters, so taking the 480 B out of it leaves everything linked after them where it was. moddiff vs the PCARG ELFs (ff63e05c6264's kit): release 5,040 text symbols +0 and 8 moved (both instances' `pcarg_commit` and the three emitters, -480 .. -128), `.rodata`, `.data`, `.bss` +0, no hot-order function moved; layout.lock [release] 37 entries, 0 differ, nothing re-recorded. PSP-1000 boot heap: release 11,228,572 B, lab 10,614,172 B (as the PCARG ELFs). **Lab only: instance A's cold core text is back where the beta.4 lab layout had it.** The beta.4 bisect on hardware found two steps of 0.5-1.2 fps on Hot Wheels, Top Gear Rally and Duke with identical JIT and ROM counters; both were traced to layout: A's cold core text (the text linked after `layout_pad_core`: `pte_mutex_check_need_init` .. the C memory handlers, `pcarg_lookup`, the CPU-side line renderer, the interpreter) had moved 2.5-4 KiB mod 8 KiB. In the beta.5 lab ELFs it sat 2,816 B after its place in beta460b4-final2-lab (46af5e537f8d: `me_replay_lines` 0x0f40, `pipe_latch#1` 0x1960). CORE -2816 puts `pipe_latch#1` at 0x1960 again (`me_replay_lines` 0x0ea0: the function is 180 B longer than in beta.4, so its start is 160 B under and its end matches); RO +2816 holds `.rodata`, `.data` and `.bss` (the sum of the pads is unchanged, so the heap is too). moddiff vs the PCARG lab ELF: 1,247 text symbols -2816 (`pte_mutex_check_need_init` .. the end of `.text`), the 8 emitter symbols, and 61 read-only symbols -2816, all of them the module import and export tables in front of the RO pad; no hot-order function, no `.data` or `.bss` symbol moved. layout.lock [lab]: three values re-recorded (`pte_mutex_check_need_init` 0x0834, `me_replay_lines` 0x0ea0, `_ZL10pipe_latchv#1` 0x1960), 37 entries, 0 differ. Not done, on purpose: (1) `ml_line_body` stays at 0x01c0 (beta.4 lab: 0x0060). It is 352 B from its beta.4 place, on the plateau the I+D model chose for it ("The capture line"), and in the release the LATE pad is 128 B, so moving it there costs an 8 KiB heap page. (2) The release keeps its lock (`pipe_latch#1` 0x01e0, 2,176 B after the beta.4 lab place). Its CORE pad is 932 B, so the same move needs CORE 6948 / RO 2752, one 8 KiB page of PSP-1000 heap; do it in the final re-pin if the hardware A/B of this lab build against ff63e05c6264 (same code, the old cold-text place) shows the gain. `tools/layout/psp_frame_path.txt` now lists `ml_line_body`, `me_replay_lines` and `pipe_latch`, so a layout simulation places this text (call counts are placeholders).

- **beta.5 JIT fix (claude/jit-thrash-fix 165839d3 merged into claude/beta5-base: bf7a68bd's one-block RAM_MIRROR alias retire, the SMC_REUSE spare 384 KiB): a `# ---- bscan` section in hot_order.txt; release BSCAN 7576 -> 5272, lab 7800 -> 5496, every other pad unchanged.** The fix grows both instances' `block_lookup_translate_arm` (1484 -> 1744 B), `block_lookup_translate_thumb` (1536 -> 1792), `flush_translation_cache_ram_smc` (6324 -> 6572) and `smc_retire_span` (2796 -> 3292) and shrinks `smc_stable_thunk_dispatch` (396 -> 312): +1,152 B per instance, 32 B-rounded. The pinned lab arms (9c14040c668b, 5b000c25dcab: BSCAN 6648, LATE 1120) held what the pads could: A's warm tail after the arm lookup sat +256 .. +1152 B off its sets and 3 of 37 lock entries moved (both thumb lookups +0x100, `gbab_update_gba` +0x480). Giving the growth back through the late section would put all 2,304 B in front of the LATE and MAIN pads, and no split of it between them fits both flavours (release LATE 128 / MAIN 2284, lab 2272 / 176): one flavour would lose an 8 KiB heap page. The new section links its rows inside B's hot path, in front of B's `update_scanline` (whose BSCAN zeros come first in its section), A's copy then B's (gen_layout.py write_ord; hotord_check knows it), so the BSCAN pad takes the bytes and nothing linked after B moves. The rows, each taken from right after the growth it gives back: `write_io_register32` (160 B, before the thumb lookup), `xs_trap_skip`, `iok_encode`, `core_phase_now` (512, around `flush_translation_cache_ram_smc`), `core_phase_frame_begin`, `smc_chain_store_iw`, `smc_local_narrow.isra.0` (480, after `smc_retire_span`); `iok_site` (96) moves behind `smc_stable_thunk_dispatch` and fills its -96. `rewind_backup_data` + `smc_chain_store_iw` (288 + 192) was the first cut and left A's tail from `retro_run` on 32 B under (the lab's `gbab_update_gba` 64 B): `rewind_backup_data`'s 32 B-padded end is what keeps the partner autopilot's 4 B-aligned stubs (`fe_autopilot_status` ..) and everything after them on their sets. moddiff vs the 9c1b279f kit ELFs (builds/beta5-int-out/kit): release 4,970 text symbols (2,854,260 B) +0, lab 5,147 (2,996,808 B) +0, `.rodata`, `.data` and `.bss` +0 (one compiler switch table renamed); moved: `rfu_update` and `getenv` +256, `smc_trap_entry` and `flush_translation_cache_ram_smc` +32, `retro_run` and `smc_blk_note_block` +160, the per-frame fe/input stubs and `rewind_backup_data` +352 (A; B the same without the frontend), and the eight rows themselves. Symbol sizes differ in the five functions above (both instances) and nowhere else. loop_diff: `main` placed the same, 0 sets changed; its words differ only in `lui` halves (24 in the release's loop window, 26 in the lab's), the spare's +512 KiB of `.bss` (against the 384 KiB pinned arm 5b000c25dcab the lab's `main` is same-mod-address). hotord_check 160 entries, 0 misplaced (selftest PASS). layout.lock [release] and [lab] 37 entries, 0 differ, nothing re-recorded. PSP-1000 boot heap: release 11,457,948 B, lab 10,843,548 B: 9c1b279f's less exactly the spare's 524,288 B (2 x 256 KiB), no layout page.
- **beta.5 integration (claude/beta5-base: HALT_HOP 343dd94d + claude/b5-online + the GBC palettes + the filters cache fix): release BSCAN 7576, LATE 128 (new), MAIN 2284, GB 996, CORE 932, RO 576 (text 1632, FE 2724 unchanged); lab BSCAN 7800, LATE 2272, MAIN 176, GB 956, CORE 7708, RO 384 (text 692, FE 2600 unchanged).** The cache fix (0fb93954, below "The capture line") was cut for beta.4's hot order; on this base it is re-cut so the hot core and A's warm tail do not move. `update_scanline` (the 88 B dispatch), `me_capture_line` and `update_scanline_render` take the renderer's old place at the end of the hot core (`update_scanline` 0x0200, `me_capture_line` 0x0260); the split is 32 B more than the renderer was, so `process_cheats` (8 B) joins the late section. The four `ml_*` helpers leave A's cold text for a new `# ---- last` section (gen_layout.py write_ord: after B's late copies, A then B; hotord_check knows it) in the order `ml_diff_page`, `ml_relevance`, `ml_line_body`, `ml_update_relevance`, and `psp/layout_pad_late.S` (LAYOUT_PAD_LATE_BYTES, in front of `ml_diff_page`) puts `ml_line_body` at 0x01c0. The placement comes from the I+D model (cap_map.py + capsim.sh, the b3cap twin profile in ~/lpcf, maps reproduced bit-identical for the fin7 c7 layout first): sweeping the helpers in 1 KiB then 128 B steps gave a plateau from +1792 to +2176 B after the late section's natural spot (em_battle I 1.102-1.125 ms, d2_drive 3.899-3.930) and 0x01c0 is its middle; the capture block +-64/128 B and `ml_diff_page`/`ml_relevance` moved in front of `ml_line_body` change nothing (<= 0.01 ms). Putting the helpers after B's late copies, not before, is what keeps the pads small in both flavours (before them the lab needed LATE 4096 + MAIN 6568 and lost two heap pages). Model, ms of I-cache miss per frame, fbe9da63-line base (HALT_HOP 343dd94d) -> this: release em_battle 1.700 -> 1.107, d2_drive 4.685 -> 3.894; lab 1.457 -> 1.117, 4.542 -> 3.895; D-cache 0.599 -> 0.601 / 1.322 -> 1.345 (release). CORE now holds A's cold text after video.o's helpers (`_ZL10pipe_latchv#1` .. the syscall stubs, 251 KB, +0; the lock's new CORE anchor) and lets the part before them (`pte_mutex_check_need_init` .. video.o's renderers, 230 KB) move +2880: holding that part instead (CORE 6252) needs RO +2016 for the tables and costs the release an 8 KiB page; the model ranks the two equal. `me_replay_lines` itself sits -160 (re-recorded). A's tables (`netpacket_iface`) -24 (`.rodata` moves in 64 B steps; +40 was the other side and models 0.01 ms worse in D). b5-online's `s_ol_random` moved from `.data` to `.rodata` (const): its 8 B pushed A's `.data` (`smc_back_lo` ..) 64 B onto `cpu_ticks`'s D-cache set (+0.07 ms modelled on d2_drive); `.data`, `.bss`, the guard block, B's region and the JIT are now +0. moddiff vs the 343dd94d release ELF: 2,120,200 B of text +0 (A's hot core and warm tail, B's hot text, main and the frame loop, fe_host, A's cold text after video.o); moved: the capture split and the late and last sections (by design), A's cold text before video.o's helpers (+2880), video.o's renderers after them (-160), the two GB cores (+948 / -3776: the GBC palettes, the GB pad holds `hex_val`), frontend `.rodata` (-768: the RO pad and its 64 B steps). layout.lock [release] and [lab] 37 entries, 0 differ (new: `update_scanline`, `me_capture_line`, `_ZL12ml_line_bodym`, `_ZL10pipe_latchv#1`, the LATE `pad` line). PSP-1000 boot heap: release unchanged from fbe9da63 (11,982,236 B); lab -8,192 B (11,367,836 B; its `.data.pin` lands 484 B over the page, and no pad can give that back without moving a held region). **Hardware 2026-11-07 (Lab, lab build 32a7db5e1071 vs fbe9da63's ef3d26117a48, ABBA x2, work ms/frame):** em_battle -6.1 % (1000) / -5.3 % (3000); with filters on (harness fx_scale 1, LCD grid) em_battle -5.4 / -5.5 % and d2_drive -3.6 / -3.7 %; Doom II -0.6 / -1.2 % (1000 58.90 fps); gb_crystal_walk -0.2 / 0.0 %; gb_sml_run -1.2 / -1.2 %; link set (1000 host, 3000 join) +1.06 % fps geomean (link_kam +1.8 %, link_em +1.3 %, link_nidl +0.0 %), 0 desyncs. PPSSPP gate 48/48, 0 stale, corpus 35/35.
- **claude/beta5-hop (the beta.5 base + HALT_HOP, claude/kirby-hop2), second arm: a `# ---- late` section in hot_order.txt; release BSCAN 7544, MAIN 5516, GB 532, CORE 6252 (text 1632, FE 2792, RO 1488 unchanged); lab text 692, BSCAN 7768, MAIN 5552, GB 492, CORE 4812, RO 1256 (FE 2668 unchanged).** HALT_HOP grows `update_gba` 212 B in each instance. The first arm (9e048401) held only what the pads could: A's hot core after `update_gba` and its whole warm tail (`serial_next_event` .. `translate_block_arm`, 640 KB) sat +224 B mod 8 KiB and B's hot text +256/+480, and hardware paid for it in solo play (em_battle +4.3/+4.5 % work on the 1000/3000, Doom II +1.9/+1.5 %, the same with HALT_HOP off at run time: the layout, not the hops). Now `memcmp`, `block_lookup_address_dual` and `strlen` (208 B; `process_cheats` stays, or the warm tail lands 32 B under) leave the hot core for a late section that `gen_layout.py write_ord` links after B's whole hot path (A's copy, then B's), with the `halt_hop_*` trio that hop2 had appended to hot.ord by hand; hot_order.txt records `update_gba` at 4068 B (hot core 5,760/6,144). moddiff vs the fbe9da63 ELFs: release 2,281,620 B of text +0 (A's warm tail, B's hot text, main, the frame loop, fe_host, A's cold text), `.rodata`, `.data`, `.bss`, B's region and the JIT +0; what still moves is A's and B's three small functions between `update_gba` and `update_scanline` (656 B, +224), the late section, the GB cores (+508: fe_gblink.c grew in front of them and the GB pad is anchored after them on `hex_val`) and B's cold text (+96). The lock's hot lookups and `gbab_update_gba` are back at fbe9da63's values; only `size main` differs (lab 48648, +16: the timer_lazy mask read). Layout.lock [release] and [lab] 33 entries, 0 differ. PSP-1000 boot heap unchanged from fbe9da63 (release 11,982,236 B, lab 11,376,028 B, image-derived).
- **claude/beta5-base + claude/opt-1000 a27c15fe (rounds 9-10): LAYOUT_PAD_GB_BYTES 1040 (release and lab), every other pad unchanged** (release FE 2792, CORE 6348; lab FE 2668, CORE 4908). netdrv's direct-mapped reorder buffer (044ef9c6) and the packed noise tables (7a39eb40) were already in the beta.5 base, so round 9's FE +96 does not apply here; what the merge adds is c9f61a13 (one noise loop over the packed table), which shrinks each GB core by 520 B (`snd_noi_produce` -180, `snd_render` -164, `snd_render_orig` -176; round 9 measured 348 on its own base). The GB pad after both GB cores takes the -1040, so `hex_val` (fe_cheats) onward is unmoved. moddiff vs the 7ffe4236 ELFs: release 2,610,220 B of text +0 and lab 2,744,580 B +0; only the GB cores' own code after the change moves (-520 / -1040). `.rodata`, `.data`, `.bss`, B's region and the JIT +0; layout.lock [release] and [lab] 33 entries, 0 differ (one new `at hex_val` line per section, recorded at 7ffe4236's offset, and the GB `pad` line). PSP-1000 boot heap unchanged from 7ffe4236 (release 11,982,236 B, lab 11,376,028 B, image-derived): the round-9 table gain is already in the base.
- **claude/beta5-base (4.6.0-beta.4.1 d27dc908 + claude/opt-1000 7a39eb40): release 1632 and 1488, FE 2792, CORE 6348, BSCAN 7704, and a new pad, LAYOUT_PAD_MAIN_BYTES 7980; lab (tools/layout/lab-pads.env) 708 / RO 1320 / FE 2668 / CORE 4908 / BSCAN 7928 / MAIN 8016** (against the round-8 ELFs r8.elf fb410a57 and r8lab.elf 51d607c5, which are nm-identical to round 7's shipped release and lab arm C; RFU_SCHED=1). beta.4's work after 7025085 (online play, the Wi-Fi picker, the edge glow, the hotfix) grew `main`'s code *before* the frame loop by 212 B, so with `main` at round 7's 0x14e0 the loop's code sat 212 B (mostly 176-212 B) off round 8's sets: 3.7 % of the loop-line words on their round-8 set. No existing pad could fix it: `main` is the first `.text.startup` section, linked straight after the ordered text, and LAYOUT_PAD_BSCAN_BYTES sits inside B's hot text, so moving it moves B's hot text after B's `update_scanline` (tried: -212 on BSCAN put B's lookups 224 B off). `psp/layout_pad_main.S` (`layout_pad_main`, a `.text.startup.layout_pad_main` section in an object linked before main_psp.o) is the missing degree of freedom: it moves `main` and everything after it and leaves the ordered text alone. 7980 (= -212 mod 8 KiB) puts the release loop at the best placement a word-by-word scan finds (48.3 % of loop-line words on round 8's set; the lab: 90.6 %); `main` itself now starts at 0x140c (release) / 0x0bd0 (lab). The other pads were re-tuned for what each region's per-frame code is, not for the first symbol after the pad: LAYOUT_PAD_BYTES for main_psp.o's per-frame run (`net_frame` .. `dest_geo`, with `pace_frame`; `exit_cb` stays +0x28-ish off because the code between it and pace_frame changed), FE for fe_host's run (`fe_host_reset` .. `fe_np_peer_min_fps`; netdrv/relay before it and the GB cores after it grew in beta.4 and sit off), CORE for A's cold text (99.4 % of it at +0, `me_replay_lines` 0x17c0), RO for A's tables (`netpacket_iface` 0x1ec0, 24 B under round 8's 0x1ed8: `.rodata` moves in 64 B steps here, +40 is the other side). B's hot text, A's hot core and warm tail, the JIT bases (0x1500), `reg`/gbahot, `_gp`, B's region and `_end` are unchanged; hotord_check 151/151 placed, hot core 5,756/6,144. **Cost: one 8 KiB step of PSP-1000 boot heap in the release** (the MAIN pad's 7,980 B cannot be taken back by the later pads, so `.data` moves one page: `.data.pin` slack 164 B); the lab had 7 KiB of slack and does not move. **Lab:** since beta.4 the lab's `vid_draw_prestaged` is the release's size (564 B; it was 648 B in the round-8 lab), so the lab's A warm tail (`update_input` .. `translate_block_arm`) sits 96 B and B's hot text before B's `update_scanline` (`gbab_sound_timer` .. `gbab_update_gba`) 64 B under round 8's lab, and no pad in the hot order can hold them; BSCAN +64 puts B's text after `update_scanline` (its lookups and the warm tail, ~600 KB) back. The lock now has a `pad` line for the new pad, anchors that name what each pad is tuned for (`gbab_block_lookup_address_arm`, `main`, `pace_frame`, `fe_host_reset`, `pte_mutex_check_need_init`), and both sections re-recorded from these builds.
- **claude/opt-1000 round 9: LAYOUT_PAD_FE_BYTES 3512 -> 3608 and a new LAYOUT_PAD_GB_BYTES 696** (psp/layout_pad_mid.S built a third time as `layout_pad_gb.o`, linked after `gbcore_tgbdual_linked_b.o`; lab: FE 3236 -> 3332, GB 696; the other pads unchanged), against round 9's layout-neutral tip c5ab9266 (nm identical to round 8's). netdrv's ARQ receive path shrank 96 B (direct-mapped reorder buffer) and each GB core's noise code 348 B (packed tables, one loop). The FE pad takes back the -96, so everything from `netdrv_poll_needed` to the GB cores is unmoved; netdrv's own first 1,808 B (`roster_valid` .. `peer_remove`, before the change) move +96, and the lock's FE anchor moved from `roster_valid` to `netdrv_poll_needed`. The GB pad takes the -696, so `hex_val` (fe_cheats) onward, i.e. fe_rewind, online, the link glue, B's cold text, A's cold text and tables, are unmoved. `.rodata` is 57,344 B (7 x 8 KiB) smaller, so `.data`, the guard block, `_gp`, `.bss`, B's region and the JIT move by whole pages. moddiff vs c5ab9266: 3,386 text symbols (2,593,332 B) +0, rodata/data/bss +0 but the two tables; layout.lock [release] 31 entries, 0 differ. PSP-1000 boot heap +57,344 B (image-derived).
- **claude/opt-1000 round 7: 1600 and 152, plus LAYOUT_PAD_FE_BYTES 3512, LAYOUT_PAD_CORE_BYTES 5544 and LAYOUT_PAD_BSCAN_BYTES 7704** (psp/Makefile, pinned in `tools/flags.lock`; CORE was 5416 in 80a16505 and became 5544 in 249b1c0e, when the tier functions after it shrank 128 B) (against the round-6 release ELF aa04683f and the round-3 ELF 0802244e, RFU_SCHED=1). Hardware measured round 4's layout 1.2-1.7 % slower on solo Doom II (hw-batch-3). Cause: B's `update_scanline` became an 8 B stub in round 4 (it was 7,712 B of video.o in the pinned order), so the rest of B's hot text and then `main` moved -7,712 B (+480 mod 8 KiB); later rounds moved main_psp.o's code after the pad (+704 vs round 3) and frontend-common (fe_host's per-frame rewind/input code, netdrv, -2,808) independently, because B's cold text and the browser/stb_image sizes between them changed. One pad cannot hold three regions, so round 7 adds two: `psp/gbalink_b_stubs.c` keeps B's `.text.update_scanline` at the renderer's size (zeros after the stub), `layout_pad_fe.o` (after stb_impl.o) and `layout_pad_core.o` (last object, before instance A's core archive) come from `psp/layout_pad_mid.S`. Result: `main` at 0x14e0 mod 8 KiB again (round 3's; loop_diff r3 vs r7: same placement, 1 differing word, 1 set changed occupancy), B's hot text, main_psp.o's code after the pad and frontend-common at round 3's offsets (3,540 text symbols, 2,138,072 B, at round 3's offset mod 8 KiB in the shipped round-7 ELF 2f3e3aae, against 888 in round 6; 3,539 at 80a16505), A's cold text (`me_replay_lines` 0x17c0) and tables (`netpacket_iface` 0x1ed8), `.data`, `reg`/gbahot (0x1b00), `_gp` (0x11f0), A's `.bss`, B's region and the JIT (0x1500) at round 6's (every data, rodata and bss symbol +0 mod 8 KiB). B's cold text moves +2,784 B. Cost: +16,384 B of text, two 8 KiB steps of PSP-1000 boot heap (11,941,276 -> 11,924,892 B). A layout that held only `main` and main_psp.o's code would cost one step; it was not chosen because fe_host's per-frame code would stay off round 3's sets.
  - **Two 32 B holes** came with 249b1c0e, when both instances' `block_lookup_translate_thumb` shrank one 32 B step: `psp/layout_pad_warm.S` (`layout_pad_warm_lookup`, 32 B) sits directly in front of A's copy and puts it back at round 3's 0x0880bb80 (set 110; round 6 had it at 0x0880bb60, set 109), and `psp/gbalink_b_stubs.c` appends 32 B of zeros to B's `.text.block_lookup_translate_thumb`, so B's hot text after it stays put. Each depends on one link-order fact: `layout_pad_warm.o` must link before the core archive and sort ahead of A's same-named section under hot.ord, and gbalink_b_stubs.c must stay the last member of B's partial link. `.fill N / 4` truncates a pad that is not a multiple of 4. hotord_check checks function order, not these holes.
  - **The lab flavour has its own pad set.** A lab build carries lab-only code in `main` before the frame loop and around `pace_frame`, so the release pads put its loop window 160 B off r3-lab's. The round-7 lab arms were built with `LAYOUT_PAD_BYTES=1440 PSP_EXTRA="LAYOUT_PAD_BSCAN_BYTES=7864 LAYOUT_PAD_FE_BYTES=3236 LAYOUT_PAD_CORE_BYTES=5532 LAYOUT_PAD_RO_BYTES=24"` (arm C, 9e41b91e) and `... LAYOUT_PAD_CORE_BYTES=5404` with the BSCAN literal 7864 (arm L, 80a16505; ref `opt-r7L`). Against a lab build of 0802244e this puts the loop window at 5580 mod 8 KiB (r3-lab's), main_psp.o's post-pad code, frontend-common and A's cold text at +0; the release's window is at 7052. Hardware measures only lab builds, so a lab result transfers to the release by analogy, not by identity.
- **claude/opt-1000 round 6: 1824 and 152** (against the round-5 release ELF 84056ab1, RFU_SCHED=1). rom_branch_hash became a 16 K-bucket static table on the SMALL tier (-196,608 B per instance) and the SMC_REUSE spare 128 KiB (-131,072 B per instance): A's `.bss` and B's region shrink by 327,680 B each, a multiple of 8 KiB, so `.data`, `reg`/gbahot (0x1b00), `_gp` (0x11f0), B's region start and the JIT (0x1500) are unmoved or move by multiples of 8 KiB. The release keeps the SMC profile code linked (smc_blk_watch = 0 switches it off), so the warm tail holds: smc_prof_note_store 300 -> 288 B (32-rounded -32) absorbs block_lookup_translate_thumb's +8 B (rounded +32); hot.ord 151/151, 6 size/offset notes, only `jit_code_wrote_.part.0`, `smc_prof_exact`, `vram_x_dma_note` and `block_lookup_translate_thumb` start 32 B lower (one set change, thumb 110 -> 109). Cold-text growth after the pad (the tier functions +8..+60 B, the DMA notes +20 B, in both instances) is +192 B; -192 on the text pad puts A's cold text (`me_replay_lines`, 0x17c0) and tables (`netpacket_iface`, 0x1ed8) back exactly. `main` unmoved (0x089396c0, loop set map unchanged), B's hot text unmoved; frontend text between the pad and B's cold text, and B's cold dynarec text, move -96..-192 B. Image 12,155,276 B (-655,360 vs round 5); B's lend pool 3,866,624 B (-327,680).
- **claude/opt-1000 round 5: 2016 and 152** (against the round-4 release ELF 83e4449b, RFU_SCHED=1). The browser lists left frontend `.bss` (134,664 B), stb_image went -Os (-9,192 B of text before the pad), the BIOS image moved from `.data` to `.rodata` (16 KiB), B's stubs and fe_gbadual grew a little. -4960 on the text pad puts A's cold text (`me_replay_lines` .., 0x17c0) back; -272 on the RO pad puts A's tables (`netpacket_iface`) at 0x1ed8, 16 B under round 4's 0x1ee8 (`.rodata` moves in 64 B steps here: 168 gives the same ELF, 184 gives +48). `.data` stays at 0x08b1c000; `reg`/gbahot (0x1b00), `_gp` (0x11f0), A's `.bss`, B's region and the JIT (0x1500) move by multiples of 8 KiB. Hot core unmoved (hot.ord 151/151, 0 set changes), B's text unmoved or -16384, `main` unmoved and the main loop's set map identical. Frontend text between `ui_psp.o` and the pad moves -280 B mod 8 KiB. Image 12,810,636 B (-147,456 vs round 4); `.data.pin` headroom 139 B.
- **claude/opt-1000 round 4: 6976 and 424** (against the round-3 release ELF 0802244e, RFU_SCHED=1). Instance B no longer links video.o, gba_cc_lut.o, cheats.o or bios_data.o (psp/gbalink_b_stubs.c): B's text -204,800..-231,296 B before the pad, B's `.data` -16,384 B, B's region -131,072 B (lend pool exactly 4 MiB). +4096 + 1280 on the text pad puts A's cold text (`me_replay_lines` ..) back at its round-3 offset mod 8 KiB (0x17c0); -6144 on the RO pad puts A's tables (`netpacket_iface`, 0x1ee8) back; `.data`, `reg`/`gbahot` (0x1b00), `_gp`, A's `.bss` and B's region sit at multiples of 8 KiB from round 3 (`.data` -278,528, `.bss` -294,912). Hot core and warm tail unmoved (151/151 hot.ord functions, 0 set changes; JIT 0x1500). `main` (not pinned) moves -7712 B (+480 mod 8 KiB). Image 12,958,092 B (-442,368 vs round 3); `.data.pin` headroom 123 B, as round 3.
- **claude/opt-1000 round 3: 1600 and 6568** (against the round-2 release ELF a5bfcb6c, RFU_SCHED=1). The round added 160 B of text (sound_state_index inlined into both cores' sound_write_savestate, netdrv_arena_borrow/return, the screenshot borrow) and 96 B of frontend .rodata, and removed two frontend statics; at the old pins `.data` overflowed `.data.pin` and moved a page (+8,192 B image, -8 KiB boot heap). -160 on the text pad puts A's cold text (`sound_flush_ring` .., 1,133 functions) back at +0 and -96 on the RO pad puts A's tables, `.data`, `.data.gbahot`, A's `.bss` and B's region back at +0: of 3,304 data/bss symbols only 164 frontend `.sbss`/`.bss` statics move (-4..-8 B). Hot core 0/249 functions moved, JIT 0x1500; frontend text -72..-240 B, B's text -32..-64 B. Image 13,400,460 B, as round 2.
- **claude/opt-1000 round 2: 1760 and 6664** (against the round-1 release ELF ef8d0de, RFU_SCHED=1). The round removed 458,752 B of .bss (both cores' sound_buffer 196,608 B each and audio_ring 65,536 B, all multiples of 8 KiB) and 64 B of .rodata; +64 on the RO pad puts A's tables (`netpacket_iface` .. `__FRAME_END__`) back where round 1 had them.  Against round 1: hot core 0/284 lines moved, JIT 0x1500, warm tail 10 functions -4 B (`audio_thread` / `plat_audio_frames` 4 B shorter), A's cold text +0, `.data`/`reg`/`gbahot` +0, every data/.bss symbol moved by a multiple of 8 KiB (0 exceptions, critic's moddelta.py); frontend text after the pad -184..-256 B and B's text -128..-256 B (no single pad realigns those and A).  PSP-1000 boot heap 10,696,092 B (+458,752 = 56 x 8 KiB, image-derived; B's 196,608 B of that is lent to A's ROM cache in solo play anyway).
- **claude/opt-1000 round 1: 1760 and 6600** (against the 4.6.0-beta.4 release ELF, 7025085 built with RFU_SCHED=1). The round removed 16,416 B of text+rodata (font_tex's 4 KiB font, xlat_scan_arm_ram x2, the partner autopilot's cold text, rig-only code) and 155,648 B of .bss.  1760 puts A's cold text (`ismemzero` ..) back at -16384 (mod 8 KiB 0) and the RO pad puts A's tables at -16416 (32 B under: `.rodata` input alignment absorbs a finer step, and `.data.pin`'s slack is 60 B, so +64 would push `.data` a page).  `.data` at -16384, `reg`/`gbahot` at 0x1b00, `iwram`/`ewram`/B's region at multiples of 8 KiB, hot core 0/284 lines moved, JIT 0x1500 (`pad_check.py`); the warm tail moves up to 544 B (`translate_block_*`, smaller by xlat_scan_arm_ram's call).  `psp/layout_pad_apb.S` holds the partner autopilot's 2,636 B in the warm tail.  PSP-1000 boot heap 10,237,340 B (+172,032 = 21 x 8 KiB).  Pads 0/0 would give one more 8 KiB page (the pads are 8,364 B together and `.data.pin`'s slack is 60 B) at the cost of A's cold text and tables leaving beta.4's sets -- unmeasured, left for a hardware A/B.
- `LAYOUT_PAD_BYTES` (text) and `LAYOUT_PAD_RO_BYTES` (`.rodata`, frontend before A), psp/Makefile, both pinned by `tools/flags.lock`. claude/b4-edge-glow (the ambient edge glow, on 521da33e) is 200 and 0: replacing the frozen game-frame snapshot and the palette ramp with the live glow shrank frontend text 192 B (16 B of it in `main`, ahead of the pad; the ambient code is `-Os`), so the text pad gives it back (8 -> 200) and everything from `me_replay_lines` on (A's cold text, its tables, `.data`, `.bss`, `_end`) sits exactly where 521da33e had it; `vid_draw_prestaged`, on the warm tail, keeps its 564 B (the glow sampler is `noipa` so its call compiles as the snapshot's did), so `pad_check.py --expect-pinned` gives 0 hot-core and 0 warm-tail functions moved, JIT 0x1500; image 14,014,860 B, PSP-1000 boot heap 10,081,692 B. claude/460-beta4-fix1 (the Wi-Fi picker, the online desync fixes and colour/ambient merged on c7a74a88) is 8 and 0: together they grew frontend text 296 B after `layout_pad_cold` and frontend `.rodata` 32 B, more than one pad can give back (c7a74a88 had 144 B of text pad and 156 B of `.data.pin` slack), so the text pad drops to its minimum (8 B, the probe function itself) and the `.rodata` pad to 0. `.data`, `.data.gbahot` (`gbahot_start`, `reg`), `.sdata`, `.sbss`, `.bss` and `_end` sit exactly where c7a74a88 had them (`.data.pin` slack 28 B), the hot core is unmoved (`pad_check.py --expect-pinned`: 0 moved, 0/284 sets, JIT 0x1500); A's cold text moves +160 B and its tables +128 B, the price of keeping the page. Image 14,014,860 B, PSP-1000 boot heap 10,081,692 B, as c7a74a88. Any further frontend growth costs `.data` a page (8 KiB of PSP-1000 heap) unless code is trimmed. The branches alone: claude/b4-colour-ambient and claude/b4-online-desync were 48 and 32, claude/b4-wifi-picker (575845d) 16 and 32: the Wi-Fi picker grew frontend text 128 B after `layout_pad_cold`, so 144 -> 16 puts everything from `online_psp_is_up` on (B, A's cold text and tables, `.data`, `.bss`) back where c7a74a88 had it; image 14,014,860 B, PSP-1000 boot heap 10,081,692 B, hot core unmoved (`pad_check.py`). Its first cut (+576 B) took `.data` a page (heap 10,073,500 B). c7a74a88 is 144 and 32. 4.6.0-beta.4 as released is 304 and 0 (b9dd305, with online play's session code moved into gbadhoc_online.prx): image 14,014,860 B and PSP-1000 boot heap 10,081,692 B, as with ONLINE=0; against 7025085 the hot core and warm tail are unmoved, A's cold text moves -15,008 B (1376 B mod 8 KiB), A's tables -144 B mod 8 KiB, `.data`/`.bss` -16 KiB. Lab: `LAYOUT_PAD_BYTES=6064 PSP_EXTRA="LAYOUT_PAD_RO_BYTES=2832"`. With online in the image it was 5312 and 200: the 222 MHz catch-up (52460f3, +288 B .text) crossed the 5584 pin's 28 B of slack, and 5312 brings `.data` back (slack 28 B, PSP-1000 boot heap 10,065,308 B); A's cold text sits 160 B under beta.3's mod 8 KiB, its tables +3272 B. Lab: `LAYOUT_PAD_BYTES=5744 PSP_EXTRA="LAYOUT_PAD_RO_BYTES=1400"`. Before the catch-up it was 5584 and 200: online play (+16,064 B .text, +1,400 B .rodata) and the list-lag fix (+1,152 B .text) came after the 6224 pin below and pushed `.data` a page (8 KiB of heap); 5584 brings it back (`.data.pin` slack 28 B), PSP-1000 boot heap 10,065,308 B. A's cold text now sits 192 B under beta.3's mod 8 KiB and its tables +3272 B; realigning either costs that page again. A Lab build of this tree lines up with beta460b3-lab at `LAYOUT_PAD_BYTES=5968 PSP_EXTRA="LAYOUT_PAD_RO_BYTES=1400"`. Before online, 4.6.0-beta.4 was 6224 and 200: against the 4.6.0-beta.3 release ELF (67045d8, 5840/200) the beta grew 34,336 B of .text + .rodata (filters v2 21,416; folders 7,016; MXPHunter's UI 3,024; #31's video preset 864; PR #32's load question 800; the 222 MHz stutter fix 800; XMB launch 416), most of it after `layout_pad_cold` and inside both instances' renderers, so no one pad realigns everything. 6224 puts A's cold text (`me_replay_lines` .. the syscall stubs, 639 functions) back at +32768, with the hot core (36 functions), A's `.data`, the guard's `gbahot` block, A's `.bss` (`reg`, `iwram`, `ewram`) and B's region at +32768 too and the warm tail within 32 B; A's tables sit +2000 B mod 8 KiB (aligning them would need 6.6 KiB more `.rodata` and push `.data` a page, 8 KiB of PSP-1000 heap). The extra 384 B fit in `.data.pin`'s alignment slack, so the image is unchanged; no pad choice recovers heap (removing both pads frees 6,040 B, short of the 8 KiB step). A Lab build of this tree lines up with beta460b3-lab (its tables +48 B) at `LAYOUT_PAD_BYTES=7344 PSP_EXTRA="LAYOUT_PAD_RO_BYTES=3840"`. The rebuilt 4.6.0-beta.3 (RAM_MIRROR_FIX keeping the alias's pc) is 5840 and 200: against the first beta.3 candidate's ELF (c279997, 6256/200, RFU_SCHED=1) `block_lookup_translate_arm` grew 152 B and `translate_block_*` moved, 416 B before the pad, so the pad gives it back; everything after `layout_pad_cold` (A's cold text, `.rodata`, `.data`, `.bss`, B's region) is unmoved, the hot core too, and the warm tail moves up to +384 B (`keysyms.py`/`nmdelta.py` in builds/monomyth-crash/pin). RAM_MIRROR_FIX (after 4.6.0-beta.2) is 6832 and 216: it grew `translate_block_thumb` 680 B and `translate_block_arm` 172 B in both instances and shrank `block_lookup_translate_thumb` 84 B, 1536 B before the pad, so the pad takes 8192 - 1536 more (176 -> 6832); against the beta.2 ELF the hot core is unmoved, everything after `layout_pad_cold` (A's cold text, `.rodata`, `.data`, `.bss`) is at +8192, and before it the warm tail moves -96 B, `translate_block_arm` +608 B, B's text +672..+1376 B and the 10 `main_psp.o` functions linked ahead of the pad +1536 B (out-of-line helpers were tried: they land after the pad, 192 B per instance, so no single pad realigns it). 4.6.0-beta.2 is 176 and 216: SMC_STM_FIXED grew `smc_trap_entry` 28 B (304 -> 208) and the route harness grew frontend text 36 B (208 -> 176); against the 304/216 beta.1 layout the hot core, B's text, A's cold text, its tables, `.data`, B's region and A's `.bss` are unmoved, only frontend text and frontend `.bss` shift (`nmdelta.py` in builds/beta460-out/tools). beta.1 was 304 and 216, measured against the shipped 4.5.0 ELF (EBOOT 3bdd4db4): instance A's cold text (`ismemzero` .. the syscall stubs, 665 functions) and its tables (`netpacket_iface` .. `__FRAME_END__`) at +16384, all of `.data`, B's region and A's `.bss` at multiples of 8 KiB, the hot core unmoved (0/284 lines), the JIT at 0x1500 and the guard's block at 0x1b00 (`pad_check.py`, `psp-nm`). The warm tail moves by up to 928 B because functions on it grew (`block_lookup_*` for BIOS_BOUND_FIX/XLAT_LAST_TRY_LAZY, `rfu_update`/`rfu_frame_update` for RFU_SCHED); the order is the 4.5.0 one, not regenerated. B's tables sit 8 B under theirs (.rodata moves in 16 B steps). The pads assume RFU_SCHED=1, which a beta builds with: a stable X.Y.Z (RFU_SCHED=0) re-tunes them. (7920/488 for the CPU clock setting, 528/976 before it, were tuned against 460-dev, not 4.5.0.) A Lab build of this tree matches the 4.5.0 Lab build (rc450g) with `LAYOUT_PAD_BYTES=6032 PSP_EXTRA="LAYOUT_PAD_RO_BYTES=7760"` (A's text and `.bss` exact, its .rodata 8 B over).
- A Lab build has different frontend sizes: to match rc450g it was built with `LAYOUT_PAD_BYTES=8144 PSP_EXTRA="LAYOUT_PAD_RO_BYTES=992"`.
- Nothing before B matters for `.bss` (B starts on an 8 KiB boundary), but the partner's `fe_autopilot_b.o` sits between B and A: the playable table is 6,232 B for that reason (22,528 less 16,296 keeps the sets).
- New state in the core goes in `.sbss`/`.sdata` (`CORE_SBSS`/`CORE_SDATA`, common.h; `GAMEPAK_SBSS`, gba_memory.c), not `.bss`/`.data`. RFU_SCHED's 16.5 KiB per instance (its ring) broke this and moved A's dispatch tables 452 B off their sets; in small data it costs 16 KiB of heap in a beta (B's copy is no longer in the lent region), nothing in a stable build. B's small-data copies are not reset at a session start: put nothing there that B uses.
- A function in `layout/hot.ord` must keep its 32-byte-rounded size. `memory_term` and `fe_autopilot_frame` were held there on purpose.

**The lock.** `tools/layout/layout.lock` has a `[release]` and a `[lab]` section: the offsets mod 8 KiB of `main`, `pace_frame`, the first symbol after each text pad, the pinned hot lookups and `update_gba` of both instances, A's cold text and tables, `gbahot_start`, `reg`, `_gp`, `iwram`, `ewram` and `_end`; B's region (`gbab_bregion_start`, `gbab_bregion_lend`), both instances' `rom_branch_hash` and their four JIT bases (`rom_`/`ram_translation_cache_static`, 0x1500: the code cache's colouring and the hot-data guard rest on it; `_end` alone misses a `.bss` change that grows before them and shrinks after, critique-8 MF3); `main`'s size (a change moves the frame loop's window); the warm hole in front of A's thumb lookup (`gap`, 32) and the 32 B after B's (`tail`, 32). tools/build.sh runs `tools/layout/layout_lock.py check` on every release and every lab build with `tools/layout/lab-pads.env` and records the result in the manifest as `layoutLock` (`release:ok`, `lab:differs`, `release:unchecked` when psp-nm gave nothing, or `none`). It fails the build on a difference only for the default switch set (RFU_SCHED=1, GBA_LINK, REWIND, ONLINE, CHEATS, INPUT_FRESH, MGIFT and LAYOUT_PIN on, ME_LOWLAT unset or 2, no PSP_EXTRA/CORE_EXTRA/FE_EXTRA/LAYOUT_PAD_BYTES); any other arm is reported. That policy, and what the check's exit code means, is `layout_lock.py strict` / `verdict` (tested in tools/tests/test_layout_lock.py, critique-9 SF3); build.sh only calls it. `tools/make_release.sh` packages only a manifest with `layoutLock` `release:ok`, so a release built another way (a stable RFU_SCHED=0 one included) must re-pin and record its own lock before it can ship (critique-8 MF2). A re-pin goes: build, `layout_lock.py solve FLAVOUR .build-elf.nm --makefile psp/Makefile` (release) or `--pads tools/layout/lab-pads.env` (lab) for first-order pad values (it does not model the next section's alignment or `.rodata`'s steps, and the RO pad has no symbol), relink, prove it (loop_diff, moddiff, hotord_check), then `layout_lock.py write FLAVOUR .build-elf.nm` and commit the lock with the pads. **`--gc-sections` would delete every pad and both holes** (nothing references them): the lock fails such a build, and the linker script would have to `KEEP()` them first.

Check by comparing `psp-nm -S -n` of the two ELFs: symbols past `layout_pad_cold`, tables and A's `.bss` must move by multiples of 8192.

## 9. The capture line (4.6.0-beta.4 filters cost, claude/b4-cachefix)

**Finding.**  Filters v2 cost beta.4 +0.22 ms (em_battle) / +0.27 ms (d2_drive)
of work per frame on hardware with filters off.  The cause is the PSP CPU's
per-scanline video path under the Media Engine renderer: update_scanline()'s
capture block (~100 instructions) plus the mid-frame log helpers
(`ml_line_body`, `ml_update_relevance`, ...).  The model above never saw it:
it excluded all of video.o (the renderer runs on the ME) and stood in for
update_scanline with "the first 384 bytes, 160 times".

* The capture block sat INSIDE update_scanline, wherever the compiler put it.
  Filters v2 only touched the renderer part of that 7.7 KB function, but the
  capture path moved from sets 34 / 58-62 (beta.3) to 44 / 68-72 (beta.4).
  Sets 69-72 also hold update_gba's per-line lines and spin_check: three
  per-line paths in a 2-way cache, thrashed on every scanline.
* The ml_* helpers were never pinned: they float in instance A's cold text,
  so any text growth before them moves them too (beta.3 0x0660, beta.4 0x09e0
  mod 8 KiB).  Driver 2 runs ml_line_body ~340k instructions a frame.
* The hot core, the warm tail (−32 B), `.data`/`.bss` and the heap were not
  the cause: the integrator's warm-tail re-pin, `.bss` pad and single-call-site
  arms all left the capture path where it was.

**The model (I-cache, 16 KiB 2-way, 64 B lines, 215.3 ns/miss).**  The twin
runs in capture mode 1 (`DR_MECAP=1`, tools/drprof/dr_host.c), so its CPU
executes exactly the PSP CPU's per-line video work, and video.o is placed like
any other object (tools/layout/cap_map.py).  update_scanline's own per-line
path cannot be placed by function fraction (two compilers lay it out
differently), so tools/layout/uspath.py runs the PSP ELF's code (a small MIPS
interpreter, capture mode 1, a typical line) and the twin's per-line
instructions are placed onto that path.  tools/layout/lpsimd_plugin.c adds a
D-cache (every named object placed by name, the translation cache at the JIT
base, heap/stack unplaced) and per-layout miss maps.  Lab builds, ms of miss
time per frame:

| layout | em_battle I | d2_drive I | em D | d2 D |
|---|---|---|---|---|
| beta.3 (beta460b3-lab) | 0.891 | 3.657 | 0.542 | 1.304 |
| filters-only (e94cdd0, beta460b4filt-lab) | 1.185 | 3.979 | 0.514 | 1.303 |
| beta.4 (beta460b4-zero-lab) | 1.185 | 3.979 | 0.503 | 1.310 |
| **fix (claude/b4-cachefix)** | **0.964** | **3.601** | 0.511 | 1.388 |
| fix, path block -64 / +64 B | 0.964 / 0.969 | 3.546 / 3.695 | | |

Predicted b3 -> b4 delta +0.29 / +0.32 ms (hardware +0.22 / +0.27); the fix
recovers 0.22 ms (75 %) of em_battle and all of d2_drive (I-cache).  The D-cache model
shows no b3/b4 difference above its noise (unplaced heap data), consistent with
the `.bss` pad arm; it gives the fix +0.01 / +0.08 ms (net -0.21 / -0.30 ms),
which is within what that crude placement can resolve.  The model does not rank the integrator's warm-tail arm
correctly (hardware 6.82 ms, model slightly better than beta.4): treat it as
right about the mechanism and the size, not about every arm.

**The fix (layout only for the core; the ME PRX is byte-identical).**
* video.cc: update_scanline() is now the capture dispatch only (88 B, two tail calls):
  `me_capture_line()` (the block, out of line; capture mode 2 tail-calls the renderer) or
  `update_scanline_render()` (the CPU renderer).  In the engine
  (`ME_PRX_BUILD`) `update_scanline_render` IS update_scanline, so the ME
  module's code is unchanged (gbadhoc_me.prx rebuilt bit-identical, cc723d23).
* hot.ord: after the hot core's 35 unmoved functions, `ml_line_body,
  ml_update_relevance, ml_diff_page, ml_relevance, me_capture_line,
  update_scanline, update_scanline_render`, then the warm tail.  The per-line
  path is now pinned: no renderer change can move it.  The order was chosen
  from 8 orders x 5 pads (tools/layout/cap_orders.py) and a 2-D sweep of the
  block's offset; this one needs no pad, and moving the block 64 B either way
  keeps it (table).
* Cost: none in memory.  `LAYOUT_PAD_BYTES` 304 -> 0 (psp/Makefile, flags.lock)
  absorbs the +292 B of section alignment, so the release image (14,014,860 B)
  and PSP-1000 boot heap (10,081,692 B) equal beta.4's; the lab build (pads
  6064 / 2832) keeps 14,612,876 B.  A's cold text and tables move (cold;
  the D-cache column above includes it for the lab build).

**Hardware A/B still owed** (the model is not a frame time): Lab `ab`,
beta460b4-zero-lab vs the fix's lab build, ABBA, em_battle + d2_drive, plus
the whole bench-ready set for losses (judge by docs/LAB noise rules).

Reproduce: `LP_HOME=~/lpcf tools/layout/twin_build.sh <beta.3 tree with the
dr_host.c DR_MECAP hunk> b3cap "" "<manifest coreFlags as -D>"`, then
`capsim.sh prof/em_battle em_battle b3cap -` (profile), `uspath.py ELF DIS NM
0 1 50 100 159` per layout (`FOLLOW=me_capture_line` on the split), `cap_map.py
... --path NAME=update_scanline:FILE`, `capsim.sh sim/X em_battle b3cap
/w/sim/S/em_battle.lay`, `cap_missdiff.py` for per-symbol / per-set deltas.

**Re-applying the fix on a re-pinned base (method).**  The fix has two parts
with different rules.
1. *The per-line path* (video.cc split, the seven hot.ord entries, hot_order.txt)
   sits in the hot section ahead of `layout_pad_cold`, so no pad moves it.
   Checked: instance A's `update_scanline` 0x1420, `me_capture_line` 0x0e00,
   `ml_line_body` 0x0220, `ml_update_relevance` 0x08e0, `ml_diff_page` 0x0b80,
   `ml_relevance` 0x0c60, `spin_check` 0x1c20 (mod 8 KiB) are identical in the
   release (pad 0), the release at pad 304 (an earlier arm) and the lab build
   (6064), and again after cherry-picking onto c7a74a88 (lab 5600/2864).  After
   any rebase, check these offsets (`psp-nm -n`, instance A = the lower copy).
2. *The pads* only hold memory.  The fix adds 404 B of `.text` (the split, plus
   32 B function rounding of the newly ordered functions in both instances).
   Lab: keep the base's lab pads (delta 0; 6064/2832 -> image 14,612,876 as
   the base).  Release: text pad -304 so `.data` stays on its page (needs
   `.eh_frame` end + 272 B + 1 B of `.data.pin` <= the page boundary).
   On f176816a (304/0) that is 0/0: image 14,014,860, PSP-1000 heap 10,081,692,
   as beta.4.  On c7a74a88 (144/32) the pad cannot go below 0: 0/32 builds
   `.data` 101 B over the page (image 14,023,052, heap 10,073,500, -8 KiB).
   There the fix needs >= 101 B found before `.data` (RO pad 32 -> 0 gives
   32) before it is heap-free.
- `LAYOUT_PAD_STR` now builds with a text pad of 0 and a `.rodata` pad > 0
  (it was defined only inside the text pad's #if).
