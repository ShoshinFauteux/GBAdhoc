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
| order | the rows of `psp/layout/hot.ord`, INCLUDEd first in `.text` by the link script `psp/gbadhoc.ld` (below; until code-health C3-1, `psp-ld --section-ordering-file=psp/layout/hot.ord`, same bytes) |
| static SMALL tier | `.balign 8192` + `.space JIT_PIN_OFFSET` in mips_stub.S |
| LARGE tier | `memalign(8192, size + JIT_PIN_OFFSET)`, base = block + JIT_PIN_OFFSET (cpu_threaded.c) |
| `JIT_PIN_OFFSET` | 0x1500 (gpsp_config.h), the centre of the best simulated plateau, 0x1200-0x1700 (section 3) |
| ordering file | generated, never hand-edited: `tools/layout/regen.sh` (from a `LAYOUT_PIN=1` build) rewrites `hot_order.txt` and `hot.ord` |

#### The link script (code-health C3-1)

The EBOOT links with one full script, `psp/gbadhoc.ld`, which psp/Makefile
writes on every make from `psp/gbadhoc.ld.in`:

- Outside its `GBADHOC BEGIN`/`END` blocks the template is psp-gcc's default
  script, verbatim. The make rule deletes the blocks, compares the rest with
  `psp-ld --verbose` (the script psp-gcc links with; checked to be the same
  text) and stops with the diff on any difference. A toolchain bump is noticed
  at the first link instead of linking with a stale default.
- Its one block today is `INCLUDE $(HOT_ORD)` right after `_ftext = .;`, so
  the hot order's rows come first in `.text`, as the ordering file put them.
  `gen_layout.py ord` writes `hot.ord` as rows only (no `.text : { }`).
- `psp/cold_tail.ld` and `psp/hot_data_guard.ld` stay INSERT scripts, given
  with `-T` *before* `gbadhoc.ld`. ld matches an input section to the first
  statement that names it, and an INSERT script's statements come first. So
  `.text.me_wmap_cold` and `.data.gbahot.*` go to their own output sections
  ahead of the default's `.text.*` and `.data.*` wildcards. Moved into the
  script after `.text` and `.data`, the wildcards would take them first:
  checked, the guard block and the cold tail move (G1 FAIL).
- Proof of the switch: EBOOT sha256 equal on all six build.sh profiles, and
  stripped ELFs equal for the `HOT_ORD=layout/hot_pair.ord` and
  `HOT_DATA_GUARD=0` arms. Only the unstripped symbol table's order differs.

#### Object groups (code-health C3-2)

psp/Makefile's `OBJS` is five groups joined, in link order: `HEAD_OBJS` (the
pad objects before main_psp.o), `FE_HEAD_OBJS` (main_psp.o .. stb_impl.o),
`FE_COMMON_OBJS` (netdrv, frontend-common), `GB_OBJS` (both TGB Dual cores)
and `FE_TAIL_OBJS` (the rest, instance B, the autopilot). A new object goes
into one of them, never into `OBJS`.

`tools/layout/groups_ld.sh` writes `psp/groups.ld` from them on every make, and
`gbadhoc.ld` INCLUDEs it just before the default's generic
`*(.text .stub .text.* .gnu.linkonce.t.*)`, which stays as the catch-all for
the core archive, the libraries and crtend:

- One statement per file, in command-line order. The generic statement placed
  the same sections file by file in the same order, so the bytes are the same
  (EBOOT-identical on six profiles; stripped ELFs equal for the hot_pair,
  `HOT_DATA_GUARD=0` and `LAYOUT_PAD_TEXT` sweep arms). The first group also
  takes psp-gcc's start files (`*/crt0.o */crti.o */crtbegin.o`), which the
  default order puts before every object.
- Each file is written `:FILE`, ld's "FILE, not an archive member", which is
  exact. A plain `*me_host.o` also matches home_host.o, any plain pattern also
  matches archive members by member name, and a plain name without a wildcard
  makes ld open the file again where the script names it.
- After the groups, `:[!/]*.o` (every relative-path object outside an archive)
  is matched between two marks, and an ASSERT fails the link if anything landed
  there: an object in no group. tools/tests/test_layout_groups.py links host
  objects through the same generator to check all of this.

The groups are where code-health C3-3's pins go: before FE_HEAD (until then
`layout_pad_cold`), FE_COMMON (`layout_pad_fe`), FE_TAIL (`layout_pad_gb`) and
the catch-all (`layout_pad_core`).

#### Pins (code-health C3-3)

The MAIN, TEXT, FE, GB and CORE pads are gone. Each was a number of zero bytes
that a person re-tuned after any size change before it. In their place,
`tools/layout/layout.lock` has, per flavour, five `pin NAME ADDR` lines:
`main`, `fe_head`, `fe_common`, `fe_tail` and `core`. ADDR is the absolute
address the region after the pin starts at.

- **How the link uses them.** psp/Makefile writes `psp/pins.ld` from the
  lock's `[LAYOUT_FLAVOUR]` section with `tools/layout/pins.awk` (POSIX awk; the
  pspdev image has no python). `gbadhoc.ld` INCLUDEs it and places pin `main`
  before `.text.startup`; `groups_ld.sh --pins` places the other four in
  front of their groups and the catch-all. Each pin is
  `. = ABSOLUTE(ABSOLUTE(.) + ((__pin_NAME - ABSOLUTE(.)) & 0x1FFF))`.
  The outer `ABSOLUTE` is required. ld 2.44 types `. + X`, with X absolute,
  as section-relative with X's absolute value, so the obvious
  `. += (P - ABSOLUTE(.)) & 0x1FFF` lands at P + ADDR(.text). On a small
  test link it put 0x08805a40 at 0x11009a80.
- **Recorded, not tuned.** Each ADDR is the end of the pad it replaces,
  read from the ELFs of all six profiles at 3790851f-identical 40311176. The
  gap fills the same bytes with the same zeros, so every EBOOT stays identical.
  `layout_pad_cold`'s `jr $ra; nop` stays as `LONG(0x03E00008) LONG(0)` before
  the `fe_head` gap until C3-7. The four rig profiles (harness, harness64,
  diagnostic, soak) get lock sections of their own, pins only. They used the
  release pad numbers, which put their regions at other offsets.
- **Symbols.** The link defines `__pin_from_NAME` (where the gap starts),
  `__pin_at_NAME` (where it lands) and, for one release, the old pad names at
  the gap's start (`layout_pad_cold`, `layout_pad_fe`, `layout_pad_gb`,
  `layout_pad_core`, `layout_pad_main`; size 0). `layout_lock.py check`
  compares `__pin_at_NAME` with the lock exactly. `layout_lock.py pins` gives
  the manifest's `layoutPins`, the flavour and each gap: release
  `main=4420 fe_head=5688 fe_common=7444 fe_tail=868 core=1472`.
- **The overrun policy (critique C-3 point 1).** A pin always reaches its
  offset. If the code before it grows past ADDR, the gap wraps and the region
  lands 8 KiB higher: one PSP-1000 heap page spent, silently. So an ASSERT
  before each pin fails the link when `__pin_strict` is set. tools/build.sh
  sets it for every release, and for any other profile when its arm is strict
  (`layout_lock.py strict`). A variant arm wraps, and the lock check reports
  the pin as MOVED with what that means. `layout_lock.py write` never changes a
  pin. Only `layout_lock.py regen FLAVOUR` does, and only with an explicit
  choice. `--spend-page PIN` moves that pin and every later one +8 KiB, and
  the commit says "page spent" with the heap census. `--free-page PIN` is the
  reverse. `--move PIN=0xADDR` sets a new offset, an I-cache placement change,
  and the commit carries a hardware ABBA.
- **What a change now costs.** A size change before a pin is absorbed by its
  gap, with no re-pin. Only `pin main` still places a measured thing, the
  frame loop inside `main` (C-1 commit 4 and C3-7 remove that). The holds
  inside the hot block are row pins since C3-5 (below).

#### The `.rodata` pin (code-health C3-4)

The RO pad (`LAYOUT_PAD_RO_BYTES`, zeros in main_psp.c) is `pin ro`. The link
map shows where it sat: psp-gcc's start file crt0.o has the first
`.rodata.str1.4`, so every object's merged strings land there, first in
`.rodata`, and the pad came right after them, in front of main_psp.o's tables.
`psp/ro.ld` (`groups_ld.sh --rodata`, INCLUDEd at the start of the default's
one-line `.rodata` statement as a `GBADHOC INLINE` insert) links the head
group's `.rodata` and then the pin; the default statement places the rest as
before. ADDR is the pad's end in each profile's map at 772c0d43. Because the
merged strings are before the pin, a few bytes more or less of strings move
nothing: the 4 B version-string trap that tools/layout/ro_margin.py guarded
(a commit hash ending like a table entry) is gone, and so is that tool. Checked:
a 40-byte longer core version string moves A's tables +64 B with the pad
(the release lock fails on `netpacket_iface`) and +0 with the pin.

#### Row pins (code-health C3-5)

The hot order's holds are gone: the zeros of `psp/layout_pad_warm.S` (A's
32 B lookup hole, the three DISPATCH_ASSOC holes, the XLAT pad),
`psp/layout_pad_late.S` (LATE, MLU), `psp/layout_pad_apb.S` (the partner
autopilot's place in a player build), B's zeros in `psp/gbalink_b_stubs.c`
(BSCAN, BXLAT and B's four holes) and the core's `smc_fill_*`,
`rfu_fill_update` and `memory_fill_term`. Each is an `@pin NAME a|b|ab
[OLDSYM]` row of `psp/layout/hot_order.txt` at the place of the zeros: in
A's rows (`a`), in B's (`b`) or both. `gen_layout.py ord` writes it into
hot.ord as a pin like the group pins, `__pin_row_NAME` (A) or
`__pin_row_NAME_b` (B), with the hold's old symbol name at the gap's start
for one release (A's only: B's zeros had none). The lock has a `pin row_*`
line per row pin in every section, in address order before the region pins:
the address the row after it started at, read from each profile's link map
at 772c0d43 (EBOOT-identical to the parent), so every EBOOT stayed identical. Where a flavour had
no zeros there (the SMC profile's rows in a non-release build, the apb rows in
a rig build, which links the partner's real autopilot), the gap is 0: the
pin then fails a strict link as soon as the code before it crosses the next
32 B step, the overrun policy the owner chose for rig profiles too.

- The gaps are the old holes byte for byte (release: `row_xlat=5632`,
  `row_xlat_b=5504`, `row_bscan_b=664`, `row_lookup=32`, ...; the manifest's
  `layoutPins` lists all 41 pins).
- A row that grows within its gap moves nothing after it. One that outgrows
  it fails the release link, naming the pin; `layout_lock.py regen` then
  spends a page (every later pin by address +8 KiB) or moves the pin (a new
  placement: hardware ABBA). `regen` orders "later" by address now, since the
  row pins come first in the lock.
- `layout_lock.py solve` and the `pad` lock lines are gone, and so is
  `tools/layout/lab-pads.env`: the lab flavour is `[lab]`'s pins. The `gap`
  (A's lookup hole) and `tail` (B's) lock lines are gone too: both holes are
  the pins `row_lookup` and `row_lkt_b`, checked exactly.
- `generate` (regen.sh) writes an order without `@pin` rows: a regenerated
  order is a new layout, recorded with its own pins.
- Non-default arms: the hot_pair order has no `@pin` rows, so without the
  hold objects its arm links a different layout than before (stated in the
  C3-5 commit; not measured).
- The intra-object holds stay, as C-3.md says: DC_STUB_FILL, the pcarg pad,
  `sched_hot_hold_text`.

**What C3-4 did not turn into pins.** C-3.md planned `.data`/`.bss` pins for
`sched_hot_hold_bss`/`_data` (main.c, serial.c) and `ap_steps_mem`
(fe_autopilot.c). They are intra-object holds, like `DC_STUB_FILL`: the main.c
and serial.c holds sit inside each core object's `.data`/`.bss`, and instance
B's copies inside gbalink_core_b.o's partial link, where no final-link pin can
reach; `ap_steps_mem` is the first thing in fe_autopilot.o's `.bss`, with the
object's statics after it, so a smaller array moves them and changes
fe_autopilot_frame's code. They stay, with their reasons in the source.

#### Rehearsal and what a change now has to do (code-health C3-6)

`tools/layout/repin_rehearsal.sh prepare BASE OUT NAME=REV|NAME=@PATCH...`
makes one commit object per case on top of BASE (no branch, no checkout): a
past merge's source change reversed, file by file where every hunk still
applies, or a patch applied forward. `report LOCKREV RUNS BASETAG TAG...`
reads the builds of those commits (tools/build.sh through the proof builder)
and prints, per flavour, whether the link passed, the slack each pin used
(`layout_lock.py stats --ref`) and every pin that moved or checked value that
differs. Run on the C3-5 tip (f522a394), release and lab:

| case | what it is | release | lab |
|---|---|---|---|
| B-4 3790851f, reversed | 7 of 14 files replay | does not compile (session.inc.c, teardown.inc.c need hunks that no longer apply) | same |
| HW-1 r2 4f1a691a, reversed | 5 of 10 files replay | links, EBOOT identical to the base, lock OK | the partial replay leaves a `-Wunused-function` (build.sh fails it); fe_common used -156 B, `at pace_frame` -156 |
| screenshots a5189b3f, reversed | 7 of 12 files replay | does not compile (misc.a.inc.c needs the new capture API) | same |
| `rehearsal/b4_hot.patch` | the SMC profile back in the release core (B-4's hot-row change) | no row pin overruns (the profile's hot functions come back into the gaps their fills left); `pin core` and `pin ro` overrun (the rest of the profile: B's copy of its cold functions links in FE_TAIL, before the core pin, and its tables grow .rodata): the link stops and names them | lab already has the profile: identical layout, 0 slack used |
| `rehearsal/hw1r2_fe_head.patch` | +528 B in main_psp.o (HW-1 r2's growth between exit_cb and pace_frame) | fe_common's gap takes 536 B; `at pace_frame` +536: the lock stops the build | same (+536) |
| `rehearsal/shots_fe.patch` | +2 KiB in video_psp.o, +1 KiB in fe_png.o, ~600 B of strings | fe_common takes the 2 KiB, `pin ro` the strings; fe_png's 1 KiB overruns `pin fe_tail` (868 B): the link stops | wraps (non-strict), reported |
| the same + `layout_lock.py regen release --spend-page fe_tail` | one command, no number chosen | 1 build: `release:ok`, 0 checked values differ (fe_tail, core, ro +8 KiB) | - |

What a binary-changing commit now has to do, by where its bytes land:

1. **Inside a region's slack** (any size change before a pin, within the gap
   `layoutPins` shows): nothing. No lock edit, one build. This covers the
   frontend groups, the cold core, `.rodata` (strings included) and every hot
   row that grows or shrinks within its row pin's gap.
2. **Past a pin's slack**: the strict link stops and names the pin. The commit
   runs one `layout_lock.py regen` command and says which it chose:
   `--spend-page PIN` (a PSP-1000 heap page, with the boot-heap census) or
   `--move PIN=0xADDR` (a new placement, with a hardware ABBA). No pad number
   is chosen by hand.
3. **Moving an unpinned per-frame anchor** that the lock still checks:
   `pace_frame` (ff_psp.o, behind main_psp.o in FE_HEAD), `fe_host_reset`
   (inside FE_COMMON), `size main`, `me_replay_lines`/`pipe_latch` (inside A's
   cold core), `netpacket_iface` (A's tables) and the `.data`/`.bss` anchors.
   No pin holds those; the lock stops the build. Code before them grew, so
   their I-cache or D-cache sets changed: the commit either accepts the move
   (`layout_lock.py write`, which never touches a pin, plus the hardware ABBA
   the move needs) or changes the code. This is C3-7's scope (per-frame
   frontend rows into the hot block), deferred by the owner, and C-1 commit
   4's (`pin frame_loop`).
4. **A new object**: it goes into a layout group in psp/Makefile, or the link
   fails on the ungrouped check.

There are no hand re-pins left: no pad numbers, no `solve` loop. Case 2 is a
recorded decision of one command; case 3 is a measurement decision about a
real placement change, which the old pads also forced (they hid it by tuning
the pad until the anchor came back, which moved the region start instead).

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
(`builds/layout-pin-out/all/pads`).  The pads were `psp/layout_pad.S`'s
`LAYOUT_PAD_TEXT`/`LAYOUT_PAD_BSS`, linked first, plus `LAYOUT_PAD_BYTES`, a
never-called 768 B function in main_psp.o.  (Since code-health C3-3 the
frontend pads are linker pins, which would absorb zeros linked in front of
them: `LAYOUT_PAD_TEXT` now moves the pins fe_head, fe_common, fe_tail and
core instead, tools/layout/pins.awk `sweep`, so the same code moves the same
N mod 8 KiB; `LAYOUT_PAD_BYTES` is refused.)

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

- **Code-health B-4, the one tier-B re-pin (claude/health: B-1 .. B-6 plus the rig objects out of the player link): release MAIN 4420, TEXT 5692, FE 7444, GB 868, CORE 1472, RO 1364 (BSCAN 664, BXLAT 5504, LATE 160, MLU 32, XLAT 5632 unchanged); lab MAIN 2636, TEXT 7660, FE 7152, GB 828, CORE 768, RO 1060 (BSCAN 888, LATE 2304, XLAT 5600 unchanged).** Since beta.5 the release had not linked (B-2 .. B-6 each built with the lab lock reported, the release lock refusing); this re-pin measures everything against the beta.5 kit ELFs (`builds/health-out/runs/baseline`, = 0d57ced90f33 / c791cc8ec39d). *What left the player link:* `usb_handoff.o` (rig builds only; psp/usb_handoff.h gives a player build the disabled functions, empty and `noipa`, so `main`'s frame loop keeps its calls), `heap_trace.o` (only with `HEAP_TRACE=1`, both flavours; psp/heap_trace.h) and, in the release core only, the SMC profile (main.h `SMC_PROFILE`): `smc_prof_take`, `smc_blk_take`, `smc_blk_note_writer`, `smc_blk_check_silent`, `smc_prof_exact`, the buckets and slots (about 3.2 KB of `.bss` per instance), with `smc_prof_note_store` reduced to the `smc_last_write_addr` it feeds the partial invalidation. *Hot core and warm tails:* the five profile rows are followed by release-only `smc_fill_*` rows (main.c: 192, 576, 480, 352, 1,152 B of zeros, each the row's beta.5 gap less the shorter function), B-2's `rfu_update` shrink (1024 -> 960 B 32 B-rounded) by `rfu_fill_update` (rfu.c, 64 B, both flavours), and the growth after it (B-2's `rfu_frame_update` +96 B release / +64 B lab, B-3's `memory_term` +32 B) by `memory_fill_term` (gba_memory.c, 224 B release / 256 B lab) in the place of `reload_timing_info`, which left the order (a WAITCNT write). B-3 had also tipped GCC into splitting the release's `ll_to_b` (`ll_to_b.part.0`, 36 B, which moved the 16 per-frame frontend rows after it); a player build inlines it again (always_inline, psp/fe/pacing.c.inc.c), as beta.5 did. shiftruns against the beta.5 ELFs: every hot-core and warm-tail symbol of A and B, B's hot path and both late/last sections at +0, except the 23 rows `spin_thumb_op` .. `memory_term` (weights <= 0.00015) at +96 (lab +64) in each instance and `reload_timing_info` (now cold text); hot core 7,552 B in 32 B slots (hot_budget OK). hotord_check: misplaced 0 in both flavours; unmatched are the rows the other flavour has (release: the three removed profile functions, `ll_to_b.part.0`; lab: the five fills) and `mer_sameframe_active` (as before). *main:* B-2 .. B-5 edited the frame loop (B-3's frame-boundary pause), so it is placed by the by-line scan against the beta.5 kit ELFs (builds/hw1-out/evidence/loopscan.py, loop lines 282-2026): release 21.2 % as built, 38.9 % at -112 (`main` 0x1c64), lab 41.6 % / 45.3 % at -20 (0x156c); loop_diff vs beta.5: window 23,048 / 23,056 B, 3 / 1 sets changed occupancy. `layout_lock.py solve` gave TEXT, FE and CORE (GB +0) against beta.5's anchors; RO puts A's tables in their 64 B step (`netpacket_iface` -24 release, +8 lab inside it; ro_margin 28/36 B and 36/28 B). *Data:* `.data` is held from instance B's data on (core `.data` +0); the frontend's `.data` after `usb_handoff.o`'s 12 B moves -12 .. +20 B (lab +28 B after B-5's strings). `.bss` is not held: the release's frontend `.bss` loses usb_handoff's 3,152 B and its core `.bss` the profile's, so B's region (8 KiB-aligned) and `iwram` (8 KiB-aligned) each start a page lower and the symbols between move (re-recorded: `rom_branch_hash` 0x1fe8, `gbab_rom_branch_hash` 0x04a8; lab 0x1be8 / 0x00a8, from B-2/B-3's own `.bss` changes); the JIT bases (self-aligned) and `_end` mod 8 KiB hold. Re-recorded in both sections: `at`/`size main`, `exit_cb`, `roster_valid`, `pte_mutex_check_need_init`, `netpacket_iface`, both `rom_branch_hash` lines; 38 entries, 0 differ (`release:ok`, `lab:ok`). **Heap:** PSP-1000 boot heap release 11,228,572 B (beta.5 11,203,996: +24,576, three pages: one of text and rodata, the frontend `.bss` page before B's region, the core `.bss` page before `iwram`), lab 10,589,596 B (unchanged). HW ABBA (em_battle, kf_doom_ii, 1000 and 3000) judges the moved frame loop, the 23 cold-ish rows and the `.bss` recolouring.

- **The beta.5 re-pin (claude/beta5-rc: claude/ff-15x, claude/spd-combo, claude/smc-cost, claude/link-desync and claude/epochc merged on 94d348a2; the one re-pin after Epoch C): release MAIN 4532, TEXT 1944, FE 7600, GB 868, CORE 6008, RO 28, BSCAN 664, LATE 160, MLU 32, XLAT 5632; lab MAIN 2656, TEXT 7456, FE 7396, GB 828, CORE 2500, RO 1228, BSCAN 888, LATE 2304, XLAT 5600.** Epoch C moved `main()`'s boot and session code into `boot_run`, `session_*` and the exit helpers (release `main` 34,380 -> 23,372 B, lab 49,444 -> 23,568), so `main`'s blocks match no earlier placement. The by-line scan of 94d348a2's re-pin (builds/hw1-out/evidence/loopscan.py: every 4 B shift of `main`, loop lines matched to 957a5c4fbb1b's kit ELF by a text diff against a34f8eaf's shell) scores the merged loop at 5.0 % as merged (lab 1.9 %) and best at 12.7 % (lab 13.5 %): MAIN puts `main` there, release 0x1cd4, lab 0x1580 (builds/b5rc-out/loopscan-p0-*.txt). `layout_lock.py solve` then gave TEXT, FE (+0), GB (+0) and CORE against the integration's anchors. A's warm tail came out 128 B (lab 64 B) shorter, which moved B's whole hot path: XLAT +128 / +64 puts `gbab_update_gba` and the rest of B back, BSCAN returns to smc-cost's 664 / 888. The RO pad puts A's tables back (`netpacket_iface` +16 in both: `.rodata`'s 64 B steps), its end 32 B inside its step both ways (ro_margin.py). `.data`, `reg`, `_gp`, `gbahot_start`, both JIT bases and B's region hold. **Heap.** Held exactly, the release needs RO 7468 after CORE 6776 (A's cold text and tables are pushed by the ME write maps' cold tail and link-desync's cold start, and the integration had spent RO), PSP-1000 boot heap 11,195,804 B: one page below b5int5. Giving A's cold text 768 B (CORE 6776 -> 6008, `_ZL10pipe_latchv#1` 0x1ee0 instead of 0x01e0; A's tables, `.data` and everything else unchanged) lets RO fall to 28 and returns that page: release 11,203,996 B (b5int5 76899307e1cd 11,212,188; -8,192 B, the code the train adds: smc-cost's helpers, the cold tail, the link cold start, Epoch C's out-of-line helpers), lab 10,589,596 B (unchanged, no trade needed). The provisional composition (60222285, two wrapped pads) cost 24,576 B. Re-recorded: `size main`, `exit_cb`, `pte_mutex_check_need_init`, `me_replay_lines`, `netpacket_iface`, and the release's `_ZL10pipe_latchv#1`; layout.lock [release] and [lab] 37 entries, 0 differ, `release:ok`, `lab:ok`; flags.lock check_flags clean. To hold A's cold text exactly instead: release CORE 6776, RO 7468 (one page).
- **5.0.0 integration re-pin (claude/beta5-base: claude/hw1-sleep-r2, claude/hw2-exit, claude/shots-displayed and claude/quickload-split merged on a34f8eaf): release MAIN 2304, TEXT 6484, FE 7712, GB 868, CORE 892, RO 0 (BSCAN 5272, LATE 128, XLAT 5504 unchanged); lab MAIN 8128, TEXT 4372, FE 7508, GB 828, CORE 4816, RO 1952 (BSCAN 5496, LATE 2272, XLAT 5504 unchanged).** The branches' own pads (the entries below) were each tuned on b7d0cafd and do not compose: the merged `main` holds HW-1's and HW-2's edits (release 34,264 B, lab 49,360 B), so its blocks match neither branch's. The same by-line scan HW-1 used (builds/hw1-out/evidence/loopscan.py: every 4 B shift of `main`, each loop line matched to its base line by a text diff, base = 957a5c4fbb1b's kit ELF and a34f8eaf's shell) scores the merged loop at 1.9 % unshifted (lab 11.8 %) and best at 32.4 % (lab 22.3 %), so MAIN puts `main` there: release 0x1420, lab 0x0ae0. `layout_lock.py solve` then gave TEXT, FE, GB and CORE (`pace_frame`, `fe_host_reset`, `hex_val`, `_ZL10pipe_latchv#1` back on their values), and the RO pad puts `netpacket_iface` back (release exactly; the lab 8 B under, `.rodata`'s 64 B steps; `ro_margin.py`: release 48/16 B, lab 32/32 B inside the step). Re-recorded in both sections: `at`/`size main`, `exit_cb`, `roster_valid`, `pte_mutex_check_need_init`, and the lab's `netpacket_iface`; 37 entries, 0 differ (`release:ok`, `lab:ok`). Hot core, warm tail, both JIT bases, B's region, `reg`/`gbahot_start`, `_gp` and `_end` are held by the lock as before. Cost: the merged frontend grew more than the pads' slack held (the code between `pace_frame` and `fe_host_reset` alone grew 3,204 B against FE's 2,724), and `main`'s placement wraps MAIN + TEXT past 8 KiB, so the release image is 16,384 B larger and the PSP-1000 release boot heap 11,212,188 B (957a5c4fbb1b: 11,228,572 B, -16,384); a `main` at mod 0x0cc4 would keep one of those pages at 15.9 % of loop bytes on base sets instead of 32.4 %. Lab boot heap 10,589,596 B (957a5c4fbb1b: 10,614,172 B).
- **spd-combo (claude/spd-combo 6049d0ea: ibranch d36ce6f6 + sched-hot 123674e7 + mlog-range 4eb36cfc on a34f8eaf): no pad change; layout.lock [release] and [lab] 37 entries, 0 differ.** SCHED_HOT put `cph_stmr` in `reg[60]` and the ME write maps made `reg[60]` `OAM_WMAP`; the merge built clean and the unfixed twin's mid-frame log differs from base on SSX 3 (frame 39). `cph_stmr` is a C variable in main.c again and `sched_hot_hold_bss` is 4 B smaller (148 -> 144 B in the release flags), so main.o's `.bss` keeps its size (`cph_stmr` itself +44 inside main.o; no other object's `.data`/`.bss` symbol moved). Every user of `reg[32..63]` now takes its index from gpsp/reg_slots.h, whose claim list fails the compile on a word claimed twice. The kit ELFs' moves against 957a5c4fbb1b are exactly the three branches' own (vs mlog's lab kit: ibranch's -192 in mips_stub.o's tail and -480 on `pcarg_commit` and the emitters, sched-hot's +32/+64 in main.o and B): the pads compose. Hot core 7,396 B (16 B over the order's 7,380: `update_gba` +16, sched-hot's). PSP-1000 boot heap unchanged (release 11,228,572 B, lab 10,614,172 B).
- **SCHED_HOT (claude/sched-hot, on claude/beta5-base a34f8eaf): no pad changes; three layout holds instead.** The scheduler's per-call state (`timer[]` packed to 16 B a timer, `video_count`, `execute_cycles`, `cpu_ticks`, `cph_ugba`, `cph_stmr`, `core_phase_lvl`, `sched_fast`, `serial_mode`, `serial_irq_cycles`, `timer_lazy`, HALT_HOP's `hop_active`) moved into `reg[32..61]` (gpsp/sched_hot.h, mips/mips_stub.S labels each slot; since claude/spd-combo `cph_stmr` is NOT among them, see the next entry): nothing in the gbahot block moves, it only gets used.  What the variables used to take stays taken, so nothing else moves: `sched_hot_hold_bss` (main.o, 148 B in the release flags) and `sched_hot_hold_data` (main.o and serial.o, 4 B each) and serial.o's 4 B `.bss` hold keep each object's `.data`/`.bss` size, and `sched_hot_hold_text` (64 B of zeros in main.o) gives back the two 32 B steps `init_main` and `main_write_savestate` shrank by.  moddiff vs the b84fb5d8 kit ELFs (957a5c4fbb1b and its release twin): release 5,019 / lab 5,196 text symbols +0; moved only main.o's own init/profile functions (`main_init_dynarec` .. `rand_seed`, 31 per flavour, +32/+64) and main.o's own statics (`frame_counter`, the hop and profile counters, -4 .. -144); every `.data`/`.bss` symbol of every other object +0.  `update_gba` grew 4,068 -> 4,084 B (the same 32 B-rounded 4,096), so the hot core and warm tail are unmoved.  layout.lock [release] and [lab] 37 entries, 0 differ, nothing re-recorded; PSP-1000 boot heap unchanged (release 11,228,572 B, lab 10,614,172 B).  The holds are part of the layout: if main.o or serial.o changes, re-measure them like any pad.

- **smc-cost (claude/smc-cost, on a34f8eaf): `LAYOUT_PAD_BSCAN_BYTES` release 5272 -> 664, lab 5496 -> 888; every other pad unchanged; one lock value re-recorded per section (`tail gbab_block_lookup_translate_thumb` 32 -> 40).** The SMC bookkeeping speedups (cpu_threaded.c: the retired-tag index, the union compare in `xs_blocks_current`, the out-of-line untag pass) shrink five pinned warm-tail functions in both instances: `xs_blocks_current` 1732 -> 1564 B, `block_lookup_translate_arm` 1744 -> 1656, `block_lookup_translate_thumb` 1792 -> 1720, `flush_translation_cache_ram_smc` 6572 -> 6232, `smc_retire_span` 3292 -> 3016, and add five out-of-line helpers (3,040 B per instance) that would otherwise land in each instance's cold text and move it. hot_order.txt gives each 32 B-rounded shrink back right after its function, with helpers where their rounded size fits (`ram_mirror_alias_pc` 128 + 64 B of `smc_fill_xs` zeros after `xs_blocks_current`; `smc_fill_lk` 96 and `smc_fill_lkt` 64 after the lookups; `smc_remember_retired_tag` 352 after `flush_translation_cache_ram_smc`; `smc_restore_retired_tag` 256 after `smc_retire_span`), and the two larger helpers (`xs_range_current` 480, `smc_untag_span` 1,824) join the bscan rows, which the BSCAN pad gives back (2 x 2,304 B). The zeros are `.text.smc_fill_*` sections (cpu_threaded.c `SMC_FILL_*`, never executed). B's thumb lookup still gets gbalink_b_stubs.c's 32 B of zeros, but at 1720 B the next 32 B boundary is 8 B further, so its `tail` reads 40 (re-recorded); what follows it is in place. symdiff against the b84fb5d8 kit ELFs (builds/b5int4-out/kit): every text, `.rodata`, `.data` and `.bss` symbol at +0 mod 8 KiB except B's bscan rows (+2,304, by design: `gbab_update_scanline` itself +0); sizes differ in the five functions above (and `dynarec_heap_tables_release` +8, cold); hotord_check unmatched 0, misplaced 0, hot.ord consistent. layout.lock [release] and [lab] 37 entries, 0 differ after the tail. The XS_HEAP block grows 256 B (the buckets); PSP-1000 boot heap unchanged (lab 10,614,172 B).
- **The link session's cold start (claude/link-desync, docs/GBA-LINK-MIRROR.md 29): release CORE 932 -> 8, RO 576 -> 0, LATE 128 -> 160; lab CORE 4892 -> 3420, RO 3200 -> 3136, LATE 2272 -> 2304, XLAT 5504 -> 5536; release re-records A's cold text and tables, lab re-records `pte_mutex_check_need_init`.** `dynarec_link_session` (604 B per instance, cold text after `layout_pad_core`), the Thumb-writer branch in `smc_writer_safe_range`, the tier code's limit refresh and the precomputed ROM limit (`translate_block_thumb` -8 B, `translate_block_arm` -4 B per instance) move: A's translators one 32 B step (lab: XLAT +32 puts them back), the late section 32 B (LATE +32 both flavours), and A's cold text +1,600 B (release) / +800 B (lab) mod 8 KiB. Lab: CORE puts `pipe_latch#1` back at 0x1960 and `me_replay_lines` at 0x0ea0 (the new function sits between them and `pte_mutex_check_need_init`, which moves to 0x0554, re-recorded); RO keeps `netpacket_iface` at 0x0148; .data/.bss/B's region/JIT bases +0; image 13,482,380 B, PSP-1000 boot heap 10,614,172 B (unchanged). Release: the growth crossed `.data.pin`'s slack, and the only way to keep the PSP-1000 heap page (11,228,572 B, unchanged) is to give the CORE and RO pads back (932 + 576 B), so A's cold text and tables are re-recorded where they now sit (`pte_mutex_check_need_init` 0x1ca0, `me_replay_lines` 0x1940, `pipe_latch#1` 0x0400, `netpacket_iface` 0x1ee8; `.data.pin` slack 124 B). Holding the old cold-text place would cost one 8 KiB heap page (CORE ~7.6 KiB); the hot core, the warm tail, both instances' lookups, B's region, `.data`/`.bss` and the JIT bases are at their locked places. layout.lock [release] and [lab] 37 entries, 0 differ.
- **5.0.0 integration, the SMC_REUSE call-site fix (claude/beta5-base b84fb5d8): `LAYOUT_PAD_XLAT_BYTES` 5824 -> 5504 (lab 5888 -> 5504), B's translator pad 5856 -> 5504, `.text.pcarg_layout_pad` 3456 -> 3584; every other pad unchanged, nothing re-recorded.** PCARG's three site emitters now log each `jal` they write (docs/PCARG-SIDE.md section 9) through `smc_reuse_log_site_ool`, a new 128 B function in each instance's cold text. The compiler answered with more than the emitters' own change: `translate_block_thumb` grew 352 B in both instances, A's `translate_block_arm` moved one 32 B step (release -32, lab +32), the emitters came out at 704 / 800 / 672 B (load / ARM store / Thumb store; 672 / 1,120 / 640 before) and the stub emitters (`emit_pmemst_stub` .. `emit_openload_stub`) are now placed in front of them. Net: +320 B (release) / +384 B (lab) in front of B's hot path, +352 B in front of the late section and `main`, -128 B of cold text per instance. The XLAT pad takes the first, B's translator pad the second, the cold pad the third. moddiff against the d9e9ce04 ELFs (e787e61fcbe9's kit and its release twin): every symbol at or above 0x08800000 is at +0 except A's `translate_block_arm` (+32 release, -32 lab), B's `translate_block_arm` (+352, its own start; the pad after it absorbs it), and the cold block between `pcarg_commit` (+128) and the first function after the emitters in each instance (translation-time and init-time code only). `.rodata`, `.data`, `.bss`, B's region and the JIT bases +0; layout.lock [release] and [lab] 37 entries, 0 differ; image and PSP-1000 boot heap unchanged (release 11,228,572 B, lab 10,614,172 B).
- **ME write maps (claude/mlog-range, over a34f8eaf): release CORE 932 -> 164, RO 576 -> 0, new MLU 32; lab CORE 4892 -> 4124, RO 3200 -> 2496, MLU 32 (no lab-pads.env line: the Makefile's).** The first cut (5fdb1f64fb08) moved per-line code and cost 2-2.8 % work on hardware in games that do not use it (docs/ME-MIDFRAME.md, "Layout"); this one keeps every per-frame function where it was. `ml_line_body` lost its inlined whole compare (0x6b0 -> 0x500 B); `ml_diff_rows` (0x18c B, -Os) has its own row in hot_order.txt's last rows after it, and LAYOUT_PAD_MLU_BYTES (psp/layout_pad_late.S, a section named like `ml_update_relevance`'s) fills the rest, so `ml_update_relevance`, `main` and the rest of the late rows are where they were (LATE, MAIN unchanged). The DMA note and the MIPS tails' emitter go to the cold tail (`ME_WMAP_COLD` = section .text.me_wmap_cold, psp/cold_tail.ld: an output section after .text), instance A's copies only (640 B): B's stay in B's text, which moves B's cold text up to 640 B; the CORE pad takes that and A's C handlers' growth back. A's cold text then sits within 32-64 B of its place: `pte_mutex_check_need_init` -32 (release 0x1d3c, lab 0x0814), `me_replay_lines` and `pipe_latch#1` +64 (release 0x1760 / 0x0220, lab 0x0ee0 / 0x19a0), all re-recorded; one CORE value cannot hold both ends (the C handlers between them grew 32-96 B). The cold tail pushes .rodata 640 B: the RO pad takes it back in the lab (A's tables exactly where they were) and 576 B of it in the release (`netpacket_iface` +128, re-recorded 0x1f28). `.data`, `reg`, `_gp`, the JIT bases and `_end` are unmoved in both: PSP-1000 boot heap unchanged, release 11,228,572 B, lab 10,614,172 B. The MIPS store stubs keep their size and place (the tails live inside SMC_STM_CHAIN's 8 KiB), so ROM translations start where they did.
- **5.0.0 integration (claude/beta5-base d9e9ce04: claude/p-tidy and claude/pcarg-side merged, PCARG's review hardening): release pads unchanged; `.text.pcarg_layout_pad` 3936 -> 3456; lab CORE 7708 -> 4892, RO 384 -> 3200.** PCARG_SIDE's own pads are in docs/PCARG-SIDE.md section 5 (XLAT 5824 release / 5888 lab, B's translator pad 5856, the core's cold pad). The hardening (the s16 range check in the three site emitters) grows `pcarg_load_site` 456 -> 672 B, `pcarg_arm_store_site` 960 -> 1100 and `pcarg_thumb_store_site` 484 -> 628 in each core instance, 480 B after the 32 B rounding. The cold pad sits directly in front of `pcarg_commit` and the emitters, so taking the 480 B out of it leaves everything linked after them where it was. moddiff vs the PCARG ELFs (ff63e05c6264's kit): release 5,040 text symbols +0 and 8 moved (both instances' `pcarg_commit` and the three emitters, -480 .. -128), `.rodata`, `.data`, `.bss` +0, no hot-order function moved; layout.lock [release] 37 entries, 0 differ, nothing re-recorded. PSP-1000 boot heap: release 11,228,572 B, lab 10,614,172 B (as the PCARG ELFs). **Lab only: instance A's cold core text is back where the beta.4 lab layout had it.** The beta.4 bisect on hardware found two steps of 0.5-1.2 fps on Hot Wheels, Top Gear Rally and Duke with identical JIT and ROM counters; both were traced to layout: A's cold core text (the text linked after `layout_pad_core`: `pte_mutex_check_need_init` .. the C memory handlers, `pcarg_lookup`, the CPU-side line renderer, the interpreter) had moved 2.5-4 KiB mod 8 KiB. In the beta.5 lab ELFs it sat 2,816 B after its place in beta460b4-final2-lab (46af5e537f8d: `me_replay_lines` 0x0f40, `pipe_latch#1` 0x1960). CORE -2816 puts `pipe_latch#1` at 0x1960 again (`me_replay_lines` 0x0ea0: the function is 180 B longer than in beta.4, so its start is 160 B under and its end matches); RO +2816 holds `.rodata`, `.data` and `.bss` (the sum of the pads is unchanged, so the heap is too). moddiff vs the PCARG lab ELF: 1,247 text symbols -2816 (`pte_mutex_check_need_init` .. the end of `.text`), the 8 emitter symbols, and 61 read-only symbols -2816, all of them the module import and export tables in front of the RO pad; no hot-order function, no `.data` or `.bss` symbol moved. layout.lock [lab]: three values re-recorded (`pte_mutex_check_need_init` 0x0834, `me_replay_lines` 0x0ea0, `_ZL10pipe_latchv#1` 0x1960), 37 entries, 0 differ. Not done, on purpose: (1) `ml_line_body` stays at 0x01c0 (beta.4 lab: 0x0060). It is 352 B from its beta.4 place, on the plateau the I+D model chose for it ("The capture line"), and in the release the LATE pad is 128 B, so moving it there costs an 8 KiB heap page. (2) The release keeps its lock (`pipe_latch#1` 0x01e0, 2,176 B after the beta.4 lab place). Its CORE pad is 932 B, so the same move needs CORE 6948 / RO 2752, one 8 KiB page of PSP-1000 heap; do it in the final re-pin if the hardware A/B of this lab build against ff63e05c6264 (same code, the old cold-text place) shows the gain. `tools/layout/psp_frame_path.txt` now lists `ml_line_body`, `me_replay_lines` and `pipe_latch`, so a layout simulation places this text (call counts are placeholders).

- **HW-1 H1 r5, the ME-lost wake held into the teardown (claude/hw1-sleep-r2 on 43e41707): release TEXT 452 -> 296, CORE 892 -> 888; lab TEXT 7344 -> 7204, CORE 7636 -> 7616, RO 8064 -> 8000; every other pad unchanged.** `wake_service` takes `pwr_hold("me_lost")` before the notice (release 996 -> 1064 B, lab 1480 -> 1524), `wake_me_lost_notice` restarts its timer after a sleep that lands in it (248 -> 300 in both), and `main` drops that hold right after `pwr_hold("teardown")`: `main` grows 36 B in the release (33796 -> 33832) and 44 B in the lab (48848 -> 48892) at its loop exit, after the frame loop, and keeps its address (release 0x14e8, lab 0x0aac). The TEXT pad gives the 156 / 140 B back. `layout_lock.py solve` gave TEXT and CORE; its lab CORE (+12) overshot by one 32 B step at the check (7616 is the value), and the lab's `.rodata` moved one 64 B step (RO 8064 -> 8000, still wrapped: no heap page). loop_diff against the 956faf4b kits: the release window at the same offset and mod 8192, 1 differing word, 0 sets changed occupancy; the lab's main differs in its register/stack-slot choice at the window's head (the window starts 8 B later, 0x18f4 -> 0x18fc mod 8192, 1 set changed occupancy, 1 differing word by loop_diff's comparison). Hot functions (hot.ord): 160 found, 0 changed set or size, both flavours. Re-recorded: `main`'s size, `exit_cb` (release 0x1ec4 -> 0x1e4c, lab 0x0a40 -> 0x09e0) and `pte_mutex_check_need_init` (release 0x1d74 -> 0x1d70, lab 0x132c -> 0x1318); 37 entries, 0 differ (`release:ok`, `lab:ok`). PSP-1000 boot heap unchanged: release 11,457,948 B, lab 10,818,972 B.
- **HW-1 H1, the ME-lost notice with no power hold (claude/hw1-sleep-r2 on 1d584a5b): release TEXT 328 -> 452; lab TEXT 7268 -> 7344; every other pad unchanged.** `wake_me_lost_notice` no longer takes and drops a hold (the suspend its release let through landed on the way to the teardown's save), so it is 124 B smaller in the release (372 -> 248) and 76 B in the lab (324 -> 248), and the compiler now emits it earlier in main_psp.o: 51 cold functions sit 372 B higher in the release and 89 sit 324 B higher in the lab (the pad's growth plus the function ahead of them), none of them in hot.ord; the TEXT pad takes the difference. `layout_lock.py solve` gave TEXT; its CORE suggestions (release +4, lab +20) overshot by one alignment step at the check and CORE stays as it was. `.rodata` keeps its size step in both flavours (in the lab some strings sit 80 B higher inside it), so the lab's RO pad stays wrapped and its PSP-1000 boot heap stays 10,818,972 B; the release's is 11,457,948 B. `main` did not move or change size: loop_diff against the 1d584a5b kits says LOOP SAME in both flavours, window at the same offset and size, 0 sets changed occupancy. Hot functions (hot.ord): 160 found, 0 changed set or size, both flavours. Re-recorded: `exit_cb` (release 0x1e48 -> 0x1ec4, lab 0x09f4 -> 0x0a40); 37 entries, 0 differ (`release:ok`, `lab:ok`).
- **HW-1 review r2 follow-ups (claude/hw1-sleep-r2 on 24752334: H6, H3, H4, H2, H1's wake notice, the no-ME harness knob): release TEXT 856 -> 328, CORE 876 -> 892, RO 576 -> 512; lab TEXT 7784 -> 7268, FE 2528 -> 2408, RO 128 -> 8064; every other pad unchanged.** main_psp.o's code between `exit_cb` and `pace_frame` grew 528 B in the release (new: `wake_me_lost_notice` 372, `net_bringup_rend_off` 312, `boot_stop_notice` 76; `gblink_begin` +160, `online_bringup_cancel` +64, `wake_service` +8; `silent_frame` -184, `gblink_service` -144, `ui_net_action` -136, which now call the one `net_bringup_rend_off` instead of each inlining the renderer's suspend) and 516 B in the lab (`wake_me_lost_notice` 324, `net_bringup_rend_off` 240, `boot_stop_notice` 76; `power_cb` +156 for the harness's ME-quiet log, written to both logs, `online_bringup_cancel` +68, `wake_service` +44; `ui_net_action` -148, `silent_frame` -140, `gblink_service` -104); the TEXT pad gives it back. Written out at the three callers, the hold cost the release 1,000 B, 144 B more than the TEXT pad held, and the only value left wrapped 8 KiB: that is why the sequence is one function. Lab only: me_host.o grew 120 B (`me_fake` -1 / -2), which the FE pad takes. `.rodata` grew one 64 B step in the release (RO 576 -> 512) and four in the lab, two more than its RO pad of 128 held, so the lab's RO pad wraps: 8064, +8,192 B of image, and the lab's PSP-1000 boot heap goes 10,827,164 -> 10,818,972 B (one more page than the HW-1 lab already gave up; the release's is unchanged, 11,457,948 B). `layout_lock.py solve` gave TEXT and FE; CORE (release, +32 on its first-order value) and RO were set from the check. `main` did not move or change size in either flavour: loop_diff against the 68023fbb kits, each ELF with its own source, finds 3 differing words in the release (the three `jal`s that now call `boot_stop_notice`, none on a loop line) and 154 in the lab (the same calls, the stack slots of a few spilled temporaries 4 B apart, `.rodata` addresses), with the window at the same offset and size and 0 sets changed occupancy in both. Hot functions (hot.ord): 160 found, 0 changed set or size, both flavours. Re-recorded: `exit_cb` (release 0x0058 -> 0x1e48, lab 0x0bf8 -> 0x09f4: it sits between the TEXT pad and the code that grew) and, in the release, `pte_mutex_check_need_init` (0x1d64 -> 0x1d74); 37 entries, 0 differ (`release:ok`, `lab:ok`).
- **HW-1 review r2 (claude/hw1-sleep-r2 on fedb7048): release TEXT 872 -> 856, CORE 892 -> 876; lab TEXT 7792 -> 7784, CORE 7628 -> 7636; every other pad unchanged.** A failed ME sleep sync no longer stops the module from the power callback (`me_standby_down`; docs/INPROC-RETURN.md, section 8), and the `standby >= 2` rung logs what the stop did; `power_cb` grew 16 B (lab 8 B) and the text after it moved (release +16 B at `pace_frame`, +32 at the CORE anchor, +64 at `netpacket_iface`; lab +8). `layout_lock.py solve` on those kits gave the two pads; `main` (release 0x14e8, lab 0x0aac), its size and the frame loop did not move. Re-recorded in both sections: `exit_cb` (release 0x0068 -> 0x0058, lab 0x0c00 -> 0x0bf8: it sits between the TEXT pad and `power_cb`) and `pte_mutex_check_need_init` (release 0x1d74 -> 0x1d64, lab 0x1324 -> 0x132c); 37 entries, 0 differ (`release:ok`, `lab:ok`). PSP-1000 boot heap unchanged (release 11,457,948 B, lab 10,827,164 B).
- **HW-1, the sleep-path fixes (claude/hw1-sleep on b7d0cafd; builds/mainpsp-map/out/PLAN.md HW-1): release MAIN 2284 -> 2504, TEXT 1632 -> 872, FE 2724 -> 2632, CORE 932 -> 892; lab MAIN 176 -> 8076, TEXT 692 -> 7792, FE 2600 -> 2528, CORE 7708 -> 7628, RO 384 -> 128; BSCAN, LATE, GB (and the release RO) unchanged.** The fixes edit `main` itself (r7-L5's boot holds before the frame loop, F2's swap after it; release `main` +32 B, lab +200 B), and GCC re-ordered `main`'s blocks: the loop's lines moved by -228 .. +1672 B (release) inside `main`, so no pad can put the loop back (PLAN §0.2-9). A by-line scan (every 4 B shift of `main`; each loop line matched to its base line by a text diff of the two sources, its code bytes counted on the base line's I-cache sets) scored the unshifted build at 2.4 % (lab 3.7 %) and the best shift at 32.5 % (lab 41.3 %), so the MAIN pad moves `main` there: release 0x140c -> 0x14e8, lab 0x0bd0 -> 0x0aac. TEXT, FE and CORE (and the lab's RO; `.rodata` moves in 64 B steps) then put `pace_frame`, `fe_host_reset`, `hex_val`, `me_replay_lines`, `_ZL10pipe_latchv#1` and `netpacket_iface` back; B's hot text, the hot core, the JIT bases, `reg`/`gbahot_start`, `_gp`, B's region and `_end` never moved. Re-recorded in both lock sections: `at`/`size main` (by design), `exit_cb` (main_psp.o's code between it and `pace_frame` grew: `power_cb` with me_standby_down's shutdown-and-release, `ui_net_action` with the online cancel) and `pte_mutex_check_need_init` (+24 B release, -16 B lab); 37 entries, 0 differ (`release:ok`, `lab:ok`). func_diff vs the e70d60aa release kit ELF: code differs in `main`, `power_cb`, `me_rend_release_vmem` (+ the new `me_rend_drop_vmem.part`, and `psp_rewind_pool_lost` no longer inlined into it), `wake_service`, `ui_net_action`, `psp_color_preview`, `mgnet_wifi_up`, `online_psp_up`, the new cancel hooks and the pads; `gblink_service` lost its `.constprop` clone (not in hot.ord); everything else is the same bytes, or differs only in `.rodata` addresses (+12 B of strings). PSP-1000 boot heap: release 11,457,948 B (unchanged), lab 10,827,164 B (-16,384: the lab's wrapped MAIN shift costs two pages). LP: the static by-line score above stands in for the simulation, which still has no measured rates; G5s on the rig judges, rebaselined because `main`'s start moved.
- **HW-2, the F4 hold (claude/hw2-exit 0c6783aa): release TEXT 84 -> 8, CORE 944 -> 956; lab TEXT 6964 -> 6944, CORE 7692 -> 7680 (every other pad unchanged).** `ui_net_action` grew by the power hold around its cable-restart teardown: +76 B release (pwr_hold and pwr_release are inlined there), +20 B lab (two calls). Nothing else changed size. `layout_lock solve` gave both steps; against the HW-2 kit before it (9dcd3286) loop_diff reports LOOP SAME in both flavours (`main` at 0x1368 / 0x0c90, the window on the same sets), and func_diff 160 of 160 hot functions on their I-cache sets, the only different functions being `ui_net_action`, the two pads and one address immediate in `_dtoa_r`. main_psp.o's code between the TEXT pad and `ui_net_action` (128 functions in release) sits 76 B (release) / 20 B (lab) lower. Re-recorded in both sections: `exit_cb` (that code) and `pte_mutex_check_need_init`. layout.lock [release] and [lab] 37 entries, 0 differ. PSP-1000 boot heap unchanged: release 11,457,948 B, lab 10,835,356 B. **The release TEXT pad has 8 B left**: the next growth of main_psp.o's code before `pace_frame` needs another pad or a full 8 KiB turn (the merge with HW-1 re-pins from the merged ELFs in any case).
- **HW-2 (claude/hw2-exit, the exit/settings/net fix candidate on the post-P beta line b7d0cafd): release MAIN 2284 -> 2120, TEXT 1632 -> 84, FE 2724 -> 2404, CORE 932 -> 944, RO 576 -> 512; lab MAIN 176 -> 368, TEXT 692 -> 6964, FE 2600 -> 2296, CORE 7708 -> 7692, RO 384 -> 128 (BSCAN, LATE, GB unchanged).** HW-2 grows `main` before the frame loop (F10, F5, W8, r6-B3, F7-F9 and the `exit_sim` harness hook) and after it (F1, F3, the chord), +456 B release / +372 B lab, and main_psp.o's other code (backup_save, the io latch, the cb_thread check, Mystery Gift's line 2). `main` itself stayed at 0x140c / 0x0bd0, but its loop lines moved +112 .. +576 B inside it, so as built only 4.2 % (release) / 19.3 % (lab) of the loop-line code bytes sat on the accepted ELFs' sets (builds/b5int2-out/kit, = b7d0cafd). The 4 B shift scan of `main` (loopscan: each loop line matched to its base line by a text diff) put `main` at 0x1368 (release, 36.8 %) and 0x0c90 (lab, 23.5 %); `layout_lock solve` and one RO step then put `pace_frame`, `fe_host_reset`, `hex_val`, `pipe_latch` and `netpacket_iface` back. Re-recorded in both sections: `main` (at, size), `exit_cb` (main_psp.o's code before it), `roster_valid` (netdrv, after transport_adhoc.c's WLAN-switch wait) and `pte_mutex_check_need_init`. layout.lock [release] and [lab] 37 entries, 0 differ. PSP-1000 boot heap: release 11,457,948 B (unchanged), lab 10,835,356 B (-8,192: the lab TEXT pad's +6,272 B crosses a page). `main`'s start mod 8192 moved: G5s is rebaselined.
- **"As displayed" screenshots (claude/shots-displayed, on claude/beta5-base b7d0cafd): release text 1632 -> 1000, FE 2724 -> 176, GB 996 -> 868, RO 576 -> 208 (CORE, BSCAN, MAIN, LATE unchanged); lab text 692 -> 24, FE 2600 -> 52, GB 956 -> 828, CORE 7708 -> 7680, RO 384 -> 8.** Each region's growth is taken by the pad inside it, so nothing wraps a page: the screenshot code in main_psp.o after `layout_pad_cold` (`player_screenshot` and its helpers, -Os: +632 B release, +668 B lab) by the text pad; the capture code in video_psp.o, the Settings row and the config key, and the streamed PNG writer by the FE pad -- `fe_png_stream.o` is linked before `layout_pad_fe` (psp/Makefile) for exactly that, since the FE pad had the room and the GB pad did not; fe_png.o's split into shared blocks (-128 B) by the GB pad. `main` is byte-identical (the chord's retry goes through `shot_ask_again`, so the loop's source is unchanged), `size main` and every per-frame anchor hold. Re-recorded: `exit_cb` (the first symbol after the text pad, which moves with it), the lab's `pte_mutex_check_need_init` (first after the CORE pad) and `netpacket_iface` 8 B under in both flavours: `.rodata` moves in 64 B steps here, the RO pad gives -8 or +56, and -8 is the nearer. The release RO pad is then 236, not 208: the pad follows the linker's merged string table, which holds the version string `v1.1.0-<sha7>`, and the linker folds a table entry that is a suffix of another into it, so the table is 4 B shorter for a commit whose short hash ends like an entry (`59` for `...f0ce959`; up to 12 B for three entries). At 208 the pad ended 4 B above a 64 B step, and such a commit moved A's tables (`netpacket_iface`) 64 B and failed the lock with no source change; at 236 it ends 32 B above it (28 B for that commit), with the tables where 208 had them. The lab pad ends 20 B above its step (16 B). `tools/layout/ro_margin.py ELF` prints the margin; a re-pin keeps 16 B on both sides. Release and lab images, and so the PSP-1000 boot heap, are byte-for-byte the size of b7d0cafd's (release 11,457,948 B, lab 10,843,548 B).
- **claude/quickload-split (the quick-load slot browser, adcab86b replayed onto b7d0cafd): release FE 2472, CORE 952, RO 512; lab FE 2348, CORE 7696, RO 320; every other pad unchanged.** The browser grows `ui_psp.o` text 244 B (ahead of the FE pad's anchor `fe_host_reset`) and frontend `.rodata` 320 B. `layout_lock.py solve` gave FE -244 and CORE +20 (release) / -12 (lab), and one RO step (-64) holds `netpacket_iface`. Every locked anchor is back on its offset except `pte_mutex_check_need_init`, which sits between the GB and CORE pads and follows CORE's step (release 0x1d5c -> 0x1d70, lab 0x1334 -> 0x1328; re-recorded, as HW-1 did). The same-frame fix (an answer on the frame of a direction keeps the slot shown) adds 8 B more to `ui_psp.o` text: FE 2480 -> 2472 (release) and 2356 -> 2348 (lab) put `roster_valid`, `fe_host_reset` and `hex_val` back, with no other pad and no lock entry touched (`layout_lock.py solve` also offered CORE +8, which is not needed: the alignment after the GB pad had absorbed the 8 B). Against the b7d0cafd kit (Lab cf44b8a9e624): 0 of the 160 hot-order symbols at a new address, main and its loop window at the same offsets (loop_diff: 0 sets changed occupancy; the only differing words are string-address immediates), image (release 12,638,604 B, lab 13,253,004 B) and PSP-1000 boot heap (release 11,457,948 B, lab 10,843,548 B) unchanged.
- **1.5x / 2x fast-forward (claude/ff-15x on b7d0cafd): release LAYOUT_PAD_BYTES 1632 -> 656, FE 2724 -> 2612; lab (lab-pads.env) LAYOUT_PAD_BYTES 692 -> 7700, FE 2600 -> 2488, CORE 7708 -> 7692; every other pad unchanged.** The multiplier ceiling's two helpers (`ffh_cap`, `ffh_cap_draw`) are `noinline` in main_psp.o's text between `exit_cb` and `pace_frame` (release +860 B there, +116 B before `exit_cb`; lab +1,184 B before `pace_frame`), and `main` grows by the call sites (release 33,764 -> 33,880 B, lab 48,648 -> 48,716 B; `main` itself stays at 0x140c). The text pads put `pace_frame` (0x0a20 / 0x0b0c) and `fe_host_reset` back; everything the CORE, GB, BSCAN, LATE and MAIN pads hold is unmoved. Re-recorded, release: `main`'s size, `exit_cb` 0x0264 -> 0x1f08 (between the pad and `pace_frame`, so no pad holds both), `netpacket_iface` 0x1ea8 -> 0x1eb8 (the five preset names: `.rodata` moves in 64 B steps, +48 is the other side); lab: `main`'s size, `exit_cb` 0x10a0 -> 0x0c44, `pte_mutex_check_need_init` 0x1334 -> 0x1324 (before the CORE anchor), `netpacket_iface` 0x0148 -> 0x0158. layout.lock [release] and [lab] 37 entries, 0 differ. PSP-1000 boot heap: release 11,457,948 B (unchanged); lab 10,835,356 B, -8,192 B: the lab's text pad has to wrap (-1,184 B is +7,008 B mod 8 KiB) and `.data` moves a page. The first cut inlined the helpers into `main` (+524 B of `main`, the 1x loop's window moving with it); out of line, the 1x loop keeps its code.
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

**The hot-core guard.** `tools/layout/hot_budget.py` (run by tools/build.sh on every pinned build) compares the hot core with the sizes `psp/layout/hot_order.txt` was generated for, in 32 B slots: the core is compiled with `-falign-functions=32`, so a function that grows inside its last 32 B step moves nothing. It also checks the placed extent (the head-hot `update_scanline_render`'s address minus `sound_timer`'s) against the slot sum, so a function linked into the core or a changed alignment fails too (tools/tests/test_hot_budget.py, host test `hot_budget`). Until code health B-6 (2026-11-15) it counted raw bytes, and every beta.5 build printed "OVER by 16 bytes" for `update_gba` 4,068 -> 4,084 B, the growth SCHED_HOT records above as "the same 32 B-rounded 4,096"; diagnostic and soak also showed `check_and_raise_interrupts` 292 -> 316 B (+24, same 320 B slot). In slots every beta.5 flavour (release, lab, harness, harness64, diagnostic, soak) reads 7,552 = 7,552, placed extent 7,552: OK. Neither trimming `update_gba` nor raising the order's numbers was needed, and the order was not regenerated. The header's "hot budget 6144 (used 6140)" is the generator's knapsack input at the last regeneration, before the beta.5 capture split was cut by hand; the guard does not compare against it.

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
