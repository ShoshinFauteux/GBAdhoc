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

- `LAYOUT_PAD_BYTES` (text) and `LAYOUT_PAD_RO_BYTES` (`.rodata`, frontend before A), psp/Makefile, both pinned by `tools/flags.lock`. RAM_MIRROR_FIX (after 4.6.0-beta.2) is 6832 and 216: it grew `translate_block_thumb` 680 B and `translate_block_arm` 172 B in both instances and shrank `block_lookup_translate_thumb` 84 B, 1536 B before the pad, so the pad takes 8192 - 1536 more (176 -> 6832); against the beta.2 ELF the hot core is unmoved, everything after `layout_pad_cold` (A's cold text, `.rodata`, `.data`, `.bss`) is at +8192, and before it the warm tail moves -96 B, `translate_block_arm` +608 B, B's text +672..+1376 B and the 10 `main_psp.o` functions linked ahead of the pad +1536 B (out-of-line helpers were tried: they land after the pad, 192 B per instance, so no single pad realigns it). 4.6.0-beta.2 is 176 and 216: SMC_STM_FIXED grew `smc_trap_entry` 28 B (304 -> 208) and the route harness grew frontend text 36 B (208 -> 176); against the 304/216 beta.1 layout the hot core, B's text, A's cold text, its tables, `.data`, B's region and A's `.bss` are unmoved, only frontend text and frontend `.bss` shift (`nmdelta.py` in builds/beta460-out/tools). beta.1 was 304 and 216, measured against the shipped 4.5.0 ELF (EBOOT 3bdd4db4): instance A's cold text (`ismemzero` .. the syscall stubs, 665 functions) and its tables (`netpacket_iface` .. `__FRAME_END__`) at +16384, all of `.data`, B's region and A's `.bss` at multiples of 8 KiB, the hot core unmoved (0/284 lines), the JIT at 0x1500 and the guard's block at 0x1b00 (`pad_check.py`, `psp-nm`). The warm tail moves by up to 928 B because functions on it grew (`block_lookup_*` for BIOS_BOUND_FIX/XLAT_LAST_TRY_LAZY, `rfu_update`/`rfu_frame_update` for RFU_SCHED); the order is the 4.5.0 one, not regenerated. B's tables sit 8 B under theirs (.rodata moves in 16 B steps). The pads assume RFU_SCHED=1, which a beta builds with: a stable X.Y.Z (RFU_SCHED=0) re-tunes them. (7920/488 for the CPU clock setting, 528/976 before it, were tuned against 460-dev, not 4.5.0.) A Lab build of this tree matches the 4.5.0 Lab build (rc450g) with `LAYOUT_PAD_BYTES=6032 PSP_EXTRA="LAYOUT_PAD_RO_BYTES=7760"` (A's text and `.bss` exact, its .rodata 8 B over).
- A Lab build has different frontend sizes: to match rc450g it was built with `LAYOUT_PAD_BYTES=8144 PSP_EXTRA="LAYOUT_PAD_RO_BYTES=992"`.
- Nothing before B matters for `.bss` (B starts on an 8 KiB boundary), but the partner's `fe_autopilot_b.o` sits between B and A: the playable table is 6,232 B for that reason (22,528 less 16,296 keeps the sets).
- New state in the core goes in `.sbss`/`.sdata` (`CORE_SBSS`/`CORE_SDATA`, common.h; `GAMEPAK_SBSS`, gba_memory.c), not `.bss`/`.data`. RFU_SCHED's 16.5 KiB per instance (its ring) broke this and moved A's dispatch tables 452 B off their sets; in small data it costs 16 KiB of heap in a beta (B's copy is no longer in the lent region), nothing in a stable build. B's small-data copies are not reset at a session start: put nothing there that B uses.
- A function in `layout/hot.ord` must keep its 32-byte-rounded size. `memory_term` and `fe_autopilot_frame` were held there on purpose.

Check by comparing `psp-nm -S -n` of the two ELFs: symbols past `layout_pad_cold`, tables and A's `.bss` must move by multiples of 8192.
