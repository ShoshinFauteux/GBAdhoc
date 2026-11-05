# Rewind (prototype)

Status: **prototype on the desktop twin and in PSP harness builds** (sections 10 and 11 are the current state), branch `claude/rewind`, behind the build switch
`REWIND` which is **off by default**. Nothing here is on the PSP yet. With
`REWIND` undefined the build is the same code as before the branch: all 36
core and desktop-frontend objects have identical `.text`, `.data`, `.rodata`
and `.bss` against the baseline commit (section "Proof that off is off").

What it does: hold the rewind control and the game runs backwards, every frame,
at full frame rate, with the sound reversed. Let go and play continues from the
frame on screen.

## 1. Build and try it

```
cd sdl
make REWIND=1 EXTRA_CFLAGS=-DGPSP_PERF_RIG      # also rebuilds the core with -DREWIND
./gpsp_sdl --rom game.gba --bios-dir DIR --rewind
```

(The desktop link needs the usual no-op stand-ins for the GB core; the recipe
is in `tools/e2e/run_me_timing_sim.sh`. `-DGPSP_PERF_RIG` only raises the
script engine's step limit, which the tests need.)

Hold **BACKSPACE** to rewind. In a script, `rewind N` holds the control for N
display ticks. Options:

| option | meaning | default |
|---|---|---|
| `--rewind` | turn it on (reserves the memory, once) | off |
| `--rewind-ring BYTES` | delta ring budget | 2 MiB |
| `--rewind-interval N` | frames between snapshots | 6 |
| `--rewind-max-frames N` | input-log length = longest rewind | 16384 (4.6 min) |
| `--rewind-burst N` | most frames a rewind tick may emulate | 2 |
| `--rewind-cross-save` | policy: allow rewinding across a cartridge save | not allowed |
| `--rewind-no-ff` | policy: fast-forwarded frames are not rewindable | they are |
| `--rewind-full-flush` | a restore empties both translation caches, as a state load does (section 6) | ROM cache kept |
| `--rewind-hash F`, `--rewind-redo`, `--rewind-fuzz FROM SEED`, `--rtc-pin T`, `--rewind-at F`, `--dump-shown A B`, `--dump-audio F` | test and measurement instruments | |

The on-screen readout is deliberately plain ("REWIND -0:03.9", a bar of the
history that is left, "START" at the limit). The real look is a separate job.

## 2. Design as built

Files: `frontend-common/fe_rewind.c/.h` is the mechanism. It knows nothing
about the GBA, never allocates, and is driven through three callbacks (gather a
state image, restore one, run one frame with a given input). The glue for the
GBA core is in `frontend-common/fe_host.c` under `#ifdef REWIND`; the desktop
wiring is in `sdl/main_sdl.c`.

### 2.1 What a snapshot is

The save-state image, whole (`gba_save_state`, 416 KiB fixed layout), **plus the
two things a save state leaves out and a rewind cannot**:

1. `audio_samples_accumulator` (libretro.c): the fractional audio-frame count.
   Without it the frame after a restore hands out one sample more or fewer than
   it did the first time.
2. The part of the sound mix ring that is in flight (mixed, not yet read out).
   A state load zeroes that ring, which is up to a frame of silence. Harmless
   once; a rewind restores every few frames, and it would crackle ten times a
   second. Carried as 16-bit values when they fit (they do in every run so far).

Both were found by the determinism test, not by reading the code: the first
version, a plain state save and load, replayed 348 of 351 frames with different
audio. They are in `gpsp_rewind_gather` / `gpsp_rewind_restore`
(libretro/libretro.c) and `sound_rewind_extra_*` (sound.c), all under `REWIND`.
The image is 458,768 bytes.

Reusing the save-state serialisation rather than hand-picking arrays was the
brief's preference and it held up: it is fast enough on the desktop (15 us) and
the hand-picked alternative would have had exactly the two holes above without
anything pointing at them. On the PSP the serialiser's cost is a different
matter (section 5).

**The cartridge save** (flash / EEPROM / SRAM contents) was not in a snapshot
up to stage 8 (the save barrier below). Since stage 9 it is: section 15.

### 2.2 The ring

- `ref`: the **newest** snapshot, whole, at frame `ref_frame`.
- A byte ring of **backward** deltas. Record k turns snapshot k+1 into snapshot
  k: the XOR of the two, zero runs run-length coded by 32-bit word
  (`u16 skip, u16 count, count words`).
- A second image-sized buffer to gather into.

Every delta hangs off its newer neighbour. So:

- Stepping back one snapshot is always exactly one delta, however long the
  history. There are no keyframes and no chain to replay.
- When the ring is full the **oldest** record is dropped and nothing else is
  touched. That is the whole "ring wraps" case.
- One delta larger than the entire ring empties it and starts again from that
  snapshot (counted, `overflow`; exercised by the unit test).

Taking a snapshot is two separate steps on purpose: **gather** (the machine
serialises itself; emulation thread, frame boundary) and **pack** (compare with
`ref`, write the delta, swap the two buffers; touches only rewind memory). On
the PSP pack is what the second core would do. `fe_rewind_pack()` is idempotent
and is called before anything that needs the ring settled.

Also kept: the **input of every frame** (`uint16`, a ring indexed by frame
number), and the **pictures and sound of two segments** (a segment = the frames
between two snapshots): in normal play, the segment being played and the one
before it.

Memory at the defaults, reserved in one block when rewind is enabled:

| piece | bytes |
|---|---|
| two state images | 917,536 |
| delta ring | 2,097,152 |
| capture, 2 segments x 6 frames x (76,800 picture + 4,800 sound) | 979,200 |
| input log, snapshot index, one reversed-sound buffer | 70,400 |
| **total** | **4,064,288 (3.88 MiB)** |

### 2.3 The control is a level, sampled every tick

There is no "begin rewind" and no "end rewind" to get out of step with. The
platform calls `fe_host_rewind_tick(held)` once per display tick and runs a
normal frame only if it answers PLAY.

Two positions: `now`, the frame on screen, and `mach`, the frame the machine is
really at. They are equal in normal play.

- **Held.** The frame before `now` is shown and `now` moves back one. That
  frame is already captured, so *the first tick of a press does no emulation at
  all*. In the background the machine is restored to the snapshot before the
  segment being shown and re-runs that segment with the logged inputs,
  capturing pictures and sound, so it is ready when `now` gets there. If it is
  not ready, the picture holds for a tick. Nothing ever waits for it.
- **Released.** If the machine is not at `now`, it is brought there a burst of
  frames per tick while the picture holds. It only ever runs **forward** from a
  snapshot at or before `now`, taking again the snapshots it passes. So when
  play resumes the newest snapshot is at or before `now`: no snapshot of the
  abandoned future survives, by construction rather than by a truncation step.
- **Pressed again while that is happening.** The captures are still there; the
  rewind continues from `now`.

**Every tick does a bounded amount of work, whatever the history of presses:
at most `burst` emulated frames and one restore.** That bound is what makes
tapping the control like a DJ harmless, and the tests assert it.

A short tap is cheaper still: the machine is not sent back to the previous
segment until `now` is within reach of the start of its own, so letting go
after a few frames costs one restore and at most one segment of catch-up.

### 2.4 Policies (both are options; the owner has not decided)

- **Save barrier** (default on). The core counts cartridge-save writes
  (`backup_write_gen`). A frame in which that counter moved is not recorded and
  the history restarts after it. Nothing before a save can be reached again, so
  the `.sav` can never be newer than the game. `--rewind-cross-save` turns it
  off: then the game state goes back and the save does not.
- **Fast-forward** (default: rewindable). `--rewind-no-ff` makes fast-forward a
  barrier in the same way.

### 2.5 Where rewind is refused

`fe_host_rewind_blocked()` is an explicit list, not an accident of what is
loaded: any GB/GBC link session, a GBA cable session, the GBA mirror pair, an
emulated-wireless-adapter session. While blocked nothing is recorded and the
history is empty when it ends. GB/GBC games are not supported at all in the
prototype.

Also: a state **load** restarts the history; a state **save** is refused while
a rewind is on screen (the machine is not at the frame shown).

## 3. Tests

| test | what it is | result |
|---|---|---|
| `tools/tests/test_fe_rewind.c` | the core against a fake 64 KiB machine, no ROM. 17 seeded cases (interval 1/2/6/10, burst 1/2/3, a ring that wraps, a ring too small for one delta, a 64-frame input log, random barriers, toggling every tick, tapping at the limit). Every tick: the frame shown is the one before, with the reference picture and reversed sound; a frame run leaves the reference state; ring self-check; at most `burst` frames and one restore; play returns within a bounded number of ticks of a release | PASS, in the host tests (suite `fe_favs`, because of the 30-suite cap): 30 of 30 suites pass |
| `tools/rewind_test.py test` | real game: play K frames, rewind 300 / 45 / 7 / 1 and once past the start of the history, replay each stretch with the same inputs. Reversed frames must be the forward frames in reverse (picture and reversed sound); replayed frames must leave an identical rewind image | section 3.1 |
| `tools/rewind_test.py fuzz` | real game, the abuse test. Buttons are a fixed function of the frame number; the control is a seeded storm (several hundred transitions: every-tick toggling, short taps, holds of seconds, holds into the limit). A second run that never rewinds is the reference for **every** frame | section 3.2 |

`tools/rewind_check.py` is the verdict for the last two. It reads the per-frame
log (`--rewind-hash`) and fails on any frame out of order, any reversed frame
that differs, any replayed frame that differs, or any frame shown at or before
a barrier.

These need ROMs, so they are scripts, not host-test suites. They ran in WSL on
the desktop twin (x86 dynarec).

### 3.1 `test`: rewind, replay with the same inputs

Frames shown backwards (wrong) / frames replayed and compared (wrong):

| game | default | `--rewind-full-flush` |
|---|---|---|
| Driver 2 (driving) | 952 (0) / 593 (0) PASS | 952 (0) / 593 (0) PASS |
| Pokemon Emerald | 2668 (0) / 593 (0) PASS | 2668 (0) / 593 (0) PASS |
| Golden Sun | 1660 (0) / 593 (0) PASS | 1660 (0) / 593 (0) PASS |
| Minish Cap | 2890 (0) / 593 (0) PASS | 2890 (0) / 593 (0) PASS |
| Aria of Sorrow | 3220 (0) / 593 (0) PASS | 3220 (0) / 593 (0) PASS |
| Mario Kart (racing) | 1578 (0) / 593 (0) PASS | 1578 (0) / 593 (0) PASS |

Also PASS on Driver 2 and Emerald with `--rewind-burst 1`,
`--rewind-interval 2` and `--rewind-interval 10`. Minish Cap and Aria make
cartridge saves in this script (12 and 7 barriers); no frame at or before one
was shown.

The two policy switches, smoke-checked only: with `--rewind-cross-save` Minish
Cap and Aria PASS the same test while rewinding through those save writes.
With `--rewind-no-ff` and the whole run fast-forwarded, nothing is recorded and
the control does nothing, as specified. There is no test of a run that
switches between the two speeds.

### 3.2 `fuzz`: the tap storm, against a run that never rewound

Seeds 1, 2, 3 per game. "Steps" are script steps of the control (each a hold
or a release, 1 to 2500 ticks long). Every forward frame of the storm run is
compared with the clean run: whole rewind image, picture, sound, input.

| game | steps of the control | rewind ticks | shown backwards (wrong) | forward frames compared (wrong) | most work in a tick | worst tick | verdict |
|---|---|---|---|---|---|---|---|
| Driver 2 | 2270 | 19,538 | 7313 (0) | 24,096 (0) | 2 frames + 1 restore | 3.2 ms | PASS 3 of 3 |
| Pokemon Emerald | 2109 | 16,313 | 11,799 (0) | 19,839 (0) | 2 + 1 | 1.8 ms | PASS 3 of 3 |
| Golden Sun | 2109 | 16,313 | 9879 (0) | 19,839 (0) | 2 + 1 | 2.2 ms | PASS 3 of 3 |
| Minish Cap | 2109 | 16,317 | 11,492 (0) | 19,799 (0) | 2 + 1 | 2.1 ms | PASS 3 of 3 |
| Aria of Sorrow | 2109 | 16,317 | 12,284 (0) | 19,814 (0) | 2 + 1 | 13.1 ms (once) | PASS 3 of 3 |
| **total** | **10,706** | **84,798** | **52,767 (0)** | **103,387 (0)** | | | **15 of 15** |

In every run: frames in order with none skipped or repeated, no frame shown at
or before a barrier, the ring self-check clean at the end, no invariant
counter raised, the picture never held more than 4 ticks after a release, and
the first tick of a press cost 14 us at worst.

The 13.1 ms tick is one tick in 16,317. The same campaign shows 13 ms normal
frames too, with no rewind involved, so it is probably the PC and not the
emulator; that was not chased down. The work done in that tick, counted rather
than timed, was within the bound like every other, and the bound is asserted
in frames and restores, not in microseconds.

Variations, one seed each on Driver 2 and Emerald, all PASS: `--rewind-burst 1`
(most work in a tick: 1 frame + 1 restore; the picture holds up to 11 ticks
after a release), a 256 KiB ring (wraps constantly).

**The same storm with `--rewind-full-flush`**: Driver 2, Emerald, Minish Cap
and Aria pass 3 of 3. **Golden Sun fails 2 of 3**: 13 of 19,839 forward frames
have a different state from the clean run. Same picture, same sound, and the
state matches again one or two frames later. Section 6 has the cause; it is
why the default restore keeps the ROM cache. With the interpreter instead of
the dynarec the same two seeds pass.

## 4. Measurements (desktop: WSL on the dev PC, x86-64, x86 dynarec)

### 4.1 Size

Recording starts at the frame given, so boot and menus are excluded where the
script allows. Driver 2 is the driving section only; Mario Kart is a race;
the other four are the generic button-mash script, which is mostly intros and
menus, so they are a floor rather than typical play.

| game (frames) | KB/s at 2 / 6 / 10 | mean delta at 6 | largest | history in 1 / 2 / 4 MiB at 6, as run |
|---|---|---|---|---|
| Driver 2, driving (2960-4250) | 524 / 291 / 174 | 29.9 KB | 39.7 KB | 3.8 / 6.8 / 13.8 s |
| Mario Kart: Super Circuit, racing (3900-5800) | 227 / 101 / 70 | 10.3 KB | 14.7 KB | 9.9 / 20.1 / 40.7* s |
| Golden Sun (600-5000) | 163 / 81 / 52 | 8.3 KB | 103 KB | 10.3 / 20.7 / 46.7 s |
| Pokemon Emerald (600-5000) | 90 / 52 / 36 | 5.3 KB | 45 KB | 19.4 / 38.5 / 79* s |
| Minish Cap (600-5000) | 82 / 46 / 32 | 4.7 KB | 101 KB | 21.2 / 48.2 / 90* s |
| Aria of Sorrow (600-5000) | 87 / 44 / 34 | 4.5 KB | 29 KB | 24.3 / 46.7 / 93* s |

\* the run was shorter than the ring: bytes/s extrapolated. The others are the
history the ring actually held at the end of the run.

Two things to know:

- **Driver 2 while driving is 2.5 times the earlier estimate** (118 KB/s over
  frames 600-5000, which was mostly menus). It draws 3D into a bitmap, so most
  of VRAM and a lot of EWRAM change every frame, and XOR + run-length cannot
  squeeze that. Its delta barely grows with the interval (18 KB at 2 frames, 30
  KB at 6, 30 KB at 10), so for this kind of game the interval is the lever:
  10 frames gives 11.8 s in 2 MiB.
- These numbers are 20-50 % above the earlier probe for the 2D games as well.
  The earlier probe counted six memory arrays byte by byte. This is the whole
  image: CPU, timers, sound registers, the sound in flight (about 0.7 KB per
  snapshot, 7 KB/s at a 6-frame interval), by word, with a 4-byte header per
  run.

### 4.2 Time, on the desktop

| | mean | worst seen |
|---|---|---|
| gather (serialise 459 KB) | 12-28 us | 265 us |
| pack (compare + delta) | 42-65 us | 330 us |
| step back one snapshot (apply one delta) | 3-9 us | 108 us |
| restore, ROM cache kept (default) | 47-68 us | 190 us |
| restore, as a state load does it | 62-77 us | 177 us |
| one normal frame | 122-369 us | about 2 ms, plus rare 13 ms outliers |
| one re-run frame, ROM cache kept / full flush | D2 352 / 383, Emerald 131 / 160, Golden Sun 212 / 236, Minish Cap 131 / 147, Aria 175 / 196, Mario Kart 217 / 341 us | |
| a whole rewind tick, including presenting | 0.7-1.1 ms | 1.9 ms in the clip run; 1.8-3.2 ms with the tests' per-frame checksums |

Stepping back one interval of 6 frames is therefore one delta, one restore and
six re-run frames: 1.0-2.4 ms here, against 0.7-2.2 ms for six normal frames.

Cost of the control itself (section 2.3):

| | cost |
|---|---|
| first tick of a press (burst 2) | no emulation; 1 us mean, 14 us worst |
| any tick, held or released | at most 2 frames + 1 restore (asserted); never exceeded |
| after a release | the picture holds 0-4 ticks (burst 2), 0-11 ticks (burst 1) |
| worst rewind tick in the fuzz runs | 1.8-3.2 ms per game, and one 13.1 ms tick that is probably the PC rather than the emulator (section 3.2; not proven). These include the test's own per-frame checksums |
| recording, in normal play | +10 to +25 us a frame (Driver 2 +6 to +8 %, Emerald +8 to +15 %): the picture copy, the input log, a snapshot every 6th frame |

## 5. What this means for a 333 MHz PSP

**Nothing in this section was measured on a PSP.** The desktop numbers above
do not scale by a clock ratio, because this work is memory traffic and the
desktop does it inside its cache. The estimate instead leans on two things
that *were* measured on hardware and are already in this repo:

- `docs/GB-LINK.md`: a word-wise hash over 2 x 384 KiB of guest memory takes
  about 4 ms. That is about 190 KiB per millisecond for a read-and-combine
  pass by word.
- The same file: a byte-wise FNV over about 390 KiB takes 5.1-5.8 ms.

From those, with the arithmetic shown so it can be argued with:

| step | traffic | estimate | where |
|---|---|---|---|
| gather | read 390 KiB + write 390 KiB (the image is 416 KiB but 26 KiB is padding; the serialiser's `memset` of all 416 KiB first would add ~2 ms and should go) | **4-5 ms** | main CPU, at a frame boundary |
| pack | read 2 x 459 KiB by word, write a few KB | **5-6 ms** | second core |
| step back | a few KB | well under 1 ms | |
| restore | write 390 KiB, zero the 256 KiB sound ring, validate; then RAM code is translated again (and ROM code too with the full flush) | **6-7 ms plus retranslation** | main CPU |
| re-run frame | a normal frame of emulation | whatever the game costs | main CPU |
| normal-play capture | 77 KB picture copy per frame | 0.4-0.8 ms, or nothing if the renderer draws straight into the capture slot | |

What follows from that:

1. **Pack is fine.** 5-6 ms on the second core every 6 frames (100 ms) is a
   small share of it. Whether it collides with the renderer's own work on that
   core has not been looked at.
2. **Gather is the problem to solve first.** 4-5 ms on the main CPU on one
   frame in six is +0.8 ms a frame on average, but it lands on one frame. A
   light game has that slack; Driver 2 or a CFRU hack near 60 fps does not, and
   would drop one frame in six. Ways out, none built: let the second core do
   the copy during the main thread's dead time at the end of a frame (and only
   make the main thread wait if it gets back first); skip a snapshot when the
   frame has no slack; a longer interval. The owner's rule that the second core
   is always there makes the first one the natural design.
3. **Rewinding at full speed needs a frame of emulation per tick, plus a
   restore every 6 ticks.** That is more than normal play costs, by the restore
   and the retranslation. On a heavy game the design does not stall, it drops
   reverse frames: the picture holds for a tick when the next segment is not
   ready. `burst` 1 (never more than one frame a tick) is the setting to expect
   on the PSP; the picture then holds up to 11 ticks (0.18 s) after a release.
4. **Memory.** 3.88 MiB at the defaults, reserved at boot. A PSP-1000's heap
   is taken by the ROM cache by design (the "heap exhausted by design" note), so
   on a 1000 this comes out of ROM residency or it does not fit. Not
   investigated. The capture buffers (0.93 MiB) could live in VRAM instead.

## 6. What a restore has to flush

**What the existing load path does.** `gba_load_state` ends with
`flush_dynarec_caches()`: the ROM translation cache is emptied (pointer reset,
256 KiB branch hash zeroed, dispatch cache cleared), the RAM translation cache
is emptied, and all 288 KiB of RAM code tags (the shadow halves of IWRAM and
EWRAM) are zeroed regardless of where code was. After it every block the game
runs is translated again. It also zeroes the 256 KiB sound ring and rebuilds
the converted palette.

**What a rewind restore actually needs.**

- The **ROM cache does not need flushing.** A ROM block's translation depends
  on the ROM, the BIOS and the gate tables, none of which a rewind changes.
  ROM blocks cannot hold a stale link into RAM code either: an ARM or Thumb
  direct branch cannot reach from 0x08000000 to 0x02000000 or 0x03000000 (the
  translator has a guard for the case anyway, the SMC_PARTIAL_SAFE island).
- The **RAM cache does**: the RAM under translated blocks was replaced without
  going through the store paths that watch for that. But only where code was:
  the normal self-modifying-code flush (`flush_translation_cache_ram`, tracked
  code ranges) is enough, and is a path the emulator already takes thousands
  of times a session.

**Built, and the default: the restore keeps the ROM cache**
(`gba_rewind_light_restore`, savestate.c; `--rewind-full-flush` gives the
state-load behaviour back for comparison). What it saves, measured as bytes of
x86 code the translator emitted again per restore:

| game | ROM code, full flush | ROM code, light | RAM code (either) |
|---|---|---|---|
| Mario Kart | 690 KB | 0 | 13 KB |
| Aria of Sorrow | 352 KB | 0 | 25 KB |
| Pokemon Emerald | 224 KB | 0 | 21 KB |
| Driver 2 | 218 KB | 0 | **280 KB** |
| Golden Sun | 203 KB | 0 | 22 KB |
| Minish Cap | 149 KB | 0 | 24 KB |

On the desktop that retranslation is 100-740 us per restore, which is 0.5-3
normal frames' worth. If the ratio carries to MIPS, the full flush alone would
cost a PSP several milliseconds to tens of milliseconds on every restore.

**It is also the exact one, and the full flush is not.** The fuzz test found
this (section 3.2): with the full flush, Golden Sun replayed a handful of
frames whose state differed from the clean run, with the same picture and the
same sound, and matched again one or two frames later. The differing bytes
(`RW_DUMP_FRAME`, a test instrument in fe_host.c) were the program counter,
three registers, one stack word and the cycle counters, all by a few cycles:
the frame had ended one translated block later. A frame ends at the first
block boundary after its cycle budget runs out; emptying the ROM cache makes
the translator rebuild blocks from different entry points, so the boundaries
move. The guest's own timeline is not changed, which is why it converges. With
the ROM cache kept, the blocks are the ones the first pass built and the same
three seeds pass; with the interpreter (`--option gpsp_drc=disabled`) they
pass too. So this is the translator's frame cut depending on block layout, not
something the rewind does, and an ordinary state load has the same property.
It matters here because a rewind promises the *same* frame.

The same argument applies, in principle, to RAM code, which the default still
flushes. No test has shown it (Driver 2 runs its engine from RAM and passes
every seed), but it is not proven absent, and item 1 below would close it.

**Still to do for the PSP, in order of value:**

1. Driver 2 runs its engine from RAM, so even with the ROM cache kept it
   retranslates 280 KB every restore. The fix is to flush RAM code only if the
   restore changed a word that has a code tag, which the delta already knows.
   Not built.
2. Skip zeroing the whole sound ring (only the window matters) and the
   image validation (the image never left memory).
3. Copy only what differs. Not possible from the delta alone, because the
   machine has run on since the snapshot; it needs a compare or dirty tracking.

Translator changes are under the dynarec guardrails (corpus oracle, one
switch, off by default). Keeping the ROM cache changes no translator code, only
which existing flush a restore calls, and the whole feature is behind `REWIND`;
it should still get the corpus run before it ships.

## 7. Proof that off is off

`tools/rewind_offcheck.sh OLD NEW` builds the core and the desktop frontend's
objects from the commit the branch started at and from the branch, both
without `REWIND`, and compares every object's sections. Result on the final
tree: 36 objects compared, identical `.text`, `.data`, `.rodata` and `.bss`.
Every line the branch adds to an existing source file is inside
`#ifdef REWIND`, and the new files are not compiled without it. (The one
exception is `tools/run_host_tests.py`, which gained the unit test.)

## 8. Not done, not verified

- **Nothing on the PSP.** No PSP frontend wiring, no second-core pack, no
  hardware timing. Section 5 is an estimate.
- The MIPS dynarec has not run a single rewind. The determinism results are
  for the x86 dynarec. The restore path is the state-load path, which the MIPS
  build uses every day, but keeping the ROM cache across a restore is new
  there, and whether the MIPS translator's frame cut moves with block layout
  the way the x86 one does (section 6) is not known.
- Six games. The dynarec guardrails ask for the whole library.
- The cartridge **clock** (RTC). On the desktop the clock is frame-counted and
  rewinds with the state. On the PSP it follows the wall clock, so a replayed
  frame that reads it could read a different second than the first time. Not
  examined; Pokemon reads it rarely, Boktai-style games constantly.
- With `--rewind-cross-save` the game state goes back and the save does not.
  The emulator handles it (section 3.1), but nothing tests what a game makes
  of a save file that is ahead of it.
- The interactive key (BACKSPACE in a window) is wired exactly like the
  scripted control but nobody has pressed it: every run here was headless.
- Core frameskip. A skipped frame is captured as a copy of the one before.
  Not tested; the desktop never skips. On the PSP the picture comes from the
  second core's renderer, which is a different capture path altogether.
- Cheats, rumble, the solar sensor: not looked at.
- A link session that starts in the middle of a rewind. Rewind is refused
  during sessions, but the transition itself has no test.
- GB/GBC games: refused.
- The stall path (picture holds because a segment is not ready) is simple but
  barely exercised: normal play keeps the previous segment, so it almost never
  happens on the desktop.

## 9. Decisions for the owner

1. **Save barrier**: never rewind across a cartridge save (built, default) or
   allow it?
2. **Fast-forward**: are fast-forwarded stretches rewindable (built, default)?
3. **How far back.** 2 MiB at 6 frames is 7 s of Driver 2 and about 40 s of
   Pokemon. More ring, a longer interval for heavy games, or accept it?
4. **Interval.** Longer = more history per byte and fewer gathers, but a
   longer wait after letting go and bigger capture buffers (0.16 MiB per frame
   of interval).
5. **At the start of the history** the picture holds and the readout says
   START. Is a held picture right, or should it bounce back to play?
6. **Reverse frames under load** on the PSP: drop them (built) or slow the
   rewind down so none are skipped?
7. **PSP-1000**: 3.9 MiB has to come from somewhere.
8. Noted, not built: the button is TRIANGLE by default, replacing the
   screen-scale shortcut, remappable in `psp/ctl_map.[ch]`.

## 10. Phase 2, stage 2: what recording costs, and what ends the history

Sections 1-9 describe the desktop prototype. This section is the state after
stage 2 (branch `claude/rewind`, on top of `claude/rc-4.0.0`). The PSP keys,
log lines and hardware runs are in `builds/rewind-out/psp/README.md`.

### 10.1 Where the 17.5 ms of stage 1 went

On a PSP-3000 stage 1 cost 8.6 ms to gather and 8.0-8.6 ms to pack, every 6
frames. Counted in bytes moved through a 16 KiB data cache:

| step | traffic |
|---|---|
| gather: clear the 416 KiB image | 416 KiB written |
| gather: the save-state writers copy the six arrays | 387 KiB read, 387 KiB written again |
| gather: sound in flight | 16 KiB written (32 KiB before the 4096-entry window) |
| pack: compare image with reference | 459 KiB + 459 KiB read |
| pack: delta + bring the reference up to date | 5-30 KiB written twice |

About 2.1 MiB per snapshot, of which the machine's own state is 0.39. The
writers are not slow as code (in PPSSPP, which charges per instruction, the
gather is clear 0.40 ms + writers 1.47 ms + sound 0.06 ms); the time on
hardware is cache misses, and the image is touched three times before the
compare touches it a fourth. `rewind_gprof = 1` prints the same split on
hardware. That split on hardware is NOT measured yet.

### 10.2 What replaced it

1. **In place** (`rewind_inplace`). The six arrays are compared where they
   lie; the reference is updated in place by the compare. No second image, no
   clear, no copy. What is still serialised is the rest of the save state
   (about 4 KiB) and the sound in flight. This removes the gather.
2. **Written-page map** (`rewind_dirty`). The core keeps
   `gba_rewind_clean[]`: EWRAM and VRAM in 1 KiB pages, set by every writer,
   cleared only by the rewind. A snapshot reads the pages marked since the
   last one and nothing else of those two arrays; IWRAM (32 KiB), OAM,
   palette and I/O are always compared. Emerald then reads 12 % of the state
   per snapshot, Driver 2 while driving 26 %.
   The map is the rewind's own. The first attempt reused the renderer's
   `vram_clean`, which the ME post and the mid-frame log clear behind the
   rewind's back; a snapshot lost every page they had taken.
3. **Wall-clock pacing** (`rewind_pace_ms`): a snapshot needs that much real
   time since the last one as well as `interval` frames.

The cost of the map itself is in every store, rewind on or off, in a REWIND
build: three instructions per EWRAM store (srl / addu / sb, no lui: the map
sits beside the register block), two more per VRAM store, four in the
SP-relative block-store trampoline.

### 10.3 The rule, and how it is checked

**Every writer of EWRAM or VRAM bytes marks.** The writers are listed in
gba_memory.h: the dynarec store stubs (MIPS, x86), the SP-relative block
store path, the C store macros, DMA, cheats, the frontend's poke, reset and
state load (which mark everything).

- `rewind_verify` / `--rewind-verify`: after every snapshot the whole machine
  is compared with the reference. A word that differs is a write the map hid.
- The determinism test and the tap-storm fuzz (section 3) run on top.
- Mutants: `REWIND_MUTANT=1` (the byte-store stub does not mark) and `=2`
  (DMA does not mark). Both are caught: by verify on every game that
  exercises the path (mutant 2: four of six games; the other two never DMA
  into EWRAM in the test window), and mutant 1 by the fuzz without verify
  (thousands of frames differ from the clean run). On the MIPS dynarec in
  PPSSPP mutant 1 gives `rewind_verify_fail snaps=1185 words=13842` on
  Emerald and fails the fuzz on Driver 2.
- Results on the final tree: desktop, verify on, 36 ROMs x 14,400 frames
  (about 85,000 snapshots): 0 hidden words. Desktop fuzz, 5 games x 3 seeds,
  in three configurations (maps; maps + verify + pool + sleeps; image
  layout, 1 seed): 263,466 forward frames compared against clean runs, 0
  wrong; save files identical. PPSSPP (MIPS): see the PSP README.

### 10.4 What is outside a snapshot ends the history

A snapshot is the save-state image plus the sound in flight. Anything else
the game can change, or be changed by, cannot be taken back, so the history
ends at the frame it happens:

| what | how it is noticed | note |
|---|---|---|
| the cartridge save (SRAM, flash, EEPROM) | the core's `backup_write_gen` moved during the frame | **UP TO STAGE 8 ONLY; since stage 9 the save is in the snapshot and nothing ends here (section 15).** The rule was: a rewind never crosses a save. The history stays shut until 60 frames have passed with no further write (`save_quiet`), so it begins after the LAST write of a save and a rewind cannot land inside one |
| a serial device with unsaved state | `gba_rewind_serial_gen` (serial.c): a call into the emulated wireless adapter, the cable or the multiplayer protocol | found on Emerald, which probes the adapter in its first frames: a re-run met an adapter that had already answered. The Game Boy Player (state saved) and a port with nothing on it do not count |
| the cheat list | `gba_rewind_cheat_gen` | a code switched on or off, the list reloaded |
| the frontend writing guest RAM | `fe_host_mem_write` | |
| a state load, a link session, a sleep (pool memory) | | |

The cartridge clock is not on the list: it is read-only to the game and, in
frame mode, a function of the frame counter and a base that are both in the
state. With the wall clock published (normal play on a PSP) a re-run frame
that reads it can read a later second than the first run did; that affects
only the frames re-run for display, and play always resumes from a restored
snapshot. Not fixed.

Frames re-run for a rewind run with the save **write-protected**
(`gba_rewind_backup_ro`): the statements that change a byte of the save are
skipped and counted (`replay_save_hits`, must be 0), while the chip's
command state runs normally because that state is in the snapshot. The save
file's writer only ever copies that memory, so nothing a rewind does can
reach the disk.

The readout's stop state: `fe_host_rewind_info.at_limit` and `.limit_reason`
(`FE_RW_LIMIT_SAVE` = "save point"), and `EVT rewind_limit ... why=save`.

Tests: `tools/tests/test_fe_rewind.c` cases `save_*` (a fake machine that
writes a save every 701 frames, five writes over eleven frames: no frame at
or before the last write is ever shown again, the save bytes equal the clean
run's at the frame on screen every tick, no re-run frame writes);
`tools/rewind_savetest.sh` (Pokemon Emerald, the real START-menu save: 352
barrier frames, both rewinds stop two frames after the last write with
`why=save`, `.sav` on disk and in memory identical to a run that never
rewound); every F line of the per-frame log carries the save's CRC.

Which games write their save all the time: `tools/rewind_survey.sh`, 36 ROMs,
four minutes of menu mashing each. Fire Emblem: The Sacred Stones wrote in
10 of 19 windows (it keeps a suspend save); Shrek in 3, Golden Sun and
Pokemon Pinball in 1; the other 32 only at file creation. This is menus, not
play: a game that autosaves during play (Fire Emblem after every move) will
show a short history and stop at "save point". Proposed: nothing more than
that readout.

### 10.5 Memory on the PSP

`rewind_vmem`: everything is carved from the END of the 4 MiB volatile pool
downwards (`fe_host_rewind_set_pool`); the engine keeps the first megabyte
(`PSP_VM_ENGINE_BYTES`) and `EVT vmem engine_top=... ok` confirms it stayed
inside. At 2 MiB of history with no pictures kept the rewind reserves
3,129,536 of the 3,145,728 bytes above the boundary: 16 KiB to spare. A larger ring goes to the heap on its own
(reserved before the cartridge loads); the rest stays in the pool.

The pool does not survive a sleep. `me_rend_release_vmem` calls
`fe_host_rewind_suspend` first (history dropped, no pointer into the pool
kept, a heap ring kept) and the wake path calls `fe_host_rewind_resume` after
the engine is rebuilt. If a rewind was on screen when the console slept,
play resumes from where the machine was, within two capture windows of the
picture.

Not solved: the pictures and sound of stage 4 (0.93 MiB at the defaults) do
not fit beside a 2 MiB ring; and claude/me-vmem adds 278 KiB to the engine's
end, which by arithmetic (not measured) leaves it only tens of KiB under the
boundary. The 416 KiB scratch image the in-place layout still carries is the
first thing to remove.

## 11. Phase 2, stages 3-4 as built: the picture, the ring, the restore

Hardware numbers for stage 2 are in the coordinator's log; nothing in this
section has run on a PSP. The keys and the runs are in
`builds/rewind-out/psp/README.md`.

### 11.1 The pictures cost no copy

A stage of the engine is 256 x 161 pixels, and so is a slot of the rewind's
picture windows. While the rewind records, `me_rend_fill_desc` points the
stage it is about to post at the slot of the frame being emulated, so the
engine draws that frame straight into the history. Presenting follows the
pointer. A frame re-run for a rewind is posted at the end of its visible
lines and its stage retires without becoming "ready"; while a rewind is on
screen the slot it hands out is drawn in the engine's place, and when play
resumes the ready stage is pointed at the slot that is showing.

A frame can have no picture (the engine was busy, or fast-forward did not
draw it): it is flagged, and going back over it keeps the picture that is
up. So a fast-forwarded stretch goes back at the rate it was drawn.

With the engine down (and under PPSSPP) fe_host copies the core's own
picture into the slot: 77 KiB a frame on the main CPU. That is the only
configuration in which pictures cost recording time.

Sound: 4.8 KiB a frame is kept and played reversed. It is optional
(the owner's call); dropping it frees 58 KiB.

### 11.2 Memory

Per game, with the shipped options, all carved from the END of the 4 MiB
volatile pool above the engine's megabyte (3,145,728 bytes):

| piece | bytes |
|---|---|
| pictures and sound, 2 windows x 6 frames | 1,046,784 |
| reference image (sized for the larger layout) | 458,768 |
| input log, snapshot index, small buffers | 119,688 |
| **left for the ring, in 64 KiB steps** | **1,507,328** |

The 416 KiB scratch image stage 2 carried is gone: the in-place layout uses
the static buffer every state save already goes through.

The ring may be in two pieces, so the heap pays only for what the pool
cannot hold:

| Rewind length | ring in the pool | ring on the heap |
|---|---|---|
| Short, 1 MB | 1,048,576 | 0 |
| Medium, 2 MB (default) | 1,507,328 | 589,824 (0.56 MiB) |
| Long, 4 MB | 1,507,328 | 2,686,976 |
| 8 MB (harness `rewind_mib` only) | 1,507,328 | 6,881,280 |

The heap piece is taken before the cartridge is loaded, so on a PSP-1000 it
comes out of what the ROM cache would otherwise have taken (18 of its 32 KiB
pages at 2 MB). Ways to make 2 MB free, none built: windows of 4 frames
instead of 6 (-341 KiB of pictures, but half again as many snapshots); no
reversed sound (-58 KiB); moving the engine's boundary down by the 166 KiB
it leaves spare (measured on hardware with me-vmem).

### 11.3 A restore

Stage 2 measured 8.2-8.9 ms on hardware. What it did: copy six arrays back
(387 KiB read, 387 KiB written), clear the 256 KiB sound ring, validate the
image and run every loader's range checks, load, flush the RAM translation
cache; and the frames after it translated that code again (280 KB of it on
Driver 2, which is why a re-run frame cost 18 ms against 9.5).

Now (`fast_restore`):

- only the pages that can differ are copied: those written since the last
  snapshot (the map) and those the stepped-over deltas changed in the
  reference (`ref_touched`). 94-109 KiB on Driver 2, 44-45 KiB on Emerald,
  IWRAM and the small arrays included;
- the RAM translation cache stays unless a word under a code tag changed
  (compared before the copy, only inside the tracked code ranges);
- only the stretch of the sound ring in flight is cleared;
- the validation and the range checks are skipped: the image never left
  memory.

`rewind_verify` compares all six arrays with the snapshot after every
restore (`restore_verify_bad`). In PPSSPP a restore went 2.94 -> 1.1 ms
(Driver 2) and 2.84 -> 0.86 ms (Emerald); 0.55 ms of that is the state
loader looking its keys up, which is what is left to remove.

Keeping the RAM code also removed the last known inexactness: with the
stage-2 restore one frame of 3,939 re-run in Emerald's fuzz left a machine a
few cycles off the first pass (the frame cut moved with the rebuilt blocks,
section 6); with the quick restore, 0 of 65,247.

### 11.4 The clock

While recording, the cartridge clock is base + frames, both in a snapshot.
The base is brought up to the console's clock when a snapshot is taken on
the first pass, and a re-run that passes such a frame takes the same base.
So a re-run frame reads the second it read the first time, and the clock
never differs from the console's by more than the frames fast-forward gains
between two snapshots. Test: a wall clock that gains a second every 20
ticks; frames re-run for a rewind leave the first pass's machine in 12,935
of 12,935 cases over six games; without the fix, in 10 of 5,221 over three.

### 11.5 No snapshot on a frame with no room

After each normal-speed frame: core time + 2.5 ms (what the platform needs
per frame) + the cost of the last snapshots, against the frame period. Over:
the snapshot waits, frame by frame, for at most 48 frames. A game that never
has room pays one snapshot in 48 frames instead of one in 6. Rewinding such
a stretch starts from snapshots up to 0.8 s apart: the first window takes
that long to appear, and snapshots are left behind on the way so the ones
before it do not.

### 11.6 The control and the ring

Section 2.3 holds; on top of it the PSP adds what the designer specified.
A press rises for 8 ticks from wherever the last one had fallen to (the game
holds its frame meanwhile), then goes back one frame a tick for 0.8 s, then
up to four. A tick always re-runs one frame and more only while it has taken
less than 9 ms, so what is reached is what the game's own cost allows; the
ring shows the speed reached. The ring, its arc (history left), head, tail
and readout are four scalars that each move one step a tick towards where
they should be. (The speed ramp and the 9 ms rule were replaced in
section 12.)

### 11.7 Not done, not verified

- Nothing in this section has run with the Media Engine.
- The store-mark cost with rewind off is argued from store counts
  (tools/rewind_marks.sh), not measured on hardware; with the toggle off the
  marks are no longer emitted at all.
- Pack on the second core is not built. The in-place compare reads the
  machine's memory, so it cannot run while the game does; the slack rule is
  what protects forward speed instead.
- The Settings rows, the Button mapping row, the refusal ring and the stop
  texts have not been looked at on a screen (one PPSSPP capture shows the
  ring, head, tail and readout over a reversed picture).
- GB / GBC games: refused, as before.

## 12. Phase 2, stage 5: an even pace

Hardware, stage 4 (PSP Go, Driver 2 fixture): 295 rewind ticks, 171 frames
shown, 204 ticks where the picture stood still. Roughly every second tick
was a stall. This section is why, and what replaced it.

### 12.1 Why half the ticks stalled

A rewind shows frames it has to re-run first, one window of 6 at a time
(restore the snapshot below, run forward, hand the pictures out backwards).
Three things in stage 4 worked against each other:

1. **The window before was started too late, every time.** The machine was
   left alone until the frame on screen was within `margin` frames of the
   start of its window, so that a tap never sends it further back than it
   has to go. `margin` was computed from `burst` (6 / 4 + 1 = 2): it
   assumed four frames can be re-run in a tick. On hardware one can. So
   each window began to be re-run when two frames of the current one were
   left, needed six ticks, and the picture stood for about four of them:
   six shown, four stalled, again and again. That alone is the measured
   ratio.
2. **The speed asked for was not the speed the game can be re-run at.** After
   0.8 s the control asked for up to four frames a tick. Production is one
   frame a tick at best, so the pictures came in bursts of a whole window
   followed by a wait.
3. **A tick that restores and re-runs a frame is longer than a display
   period** on a heavy game (17.7 ms mean, 29 ms worst, measured), which
   halves the loop rate for that tick.

`rewind_verify` was on in that run: it compares 387 KiB after every restore,
inside `restore_us`. The 9.1 ms restore there is the check, not the product
(3.1 ms worst without it in the other fixtures).

### 12.2 What is built

**The window before starts at once.** The late start is kept for taps only
(fewer than three frames gone back). A held rewind starts the next window
as soon as it enters the current one, and the PSP starts the first one
while the press is still rising (a held tick that goes back nothing, from
half way up the 8-tick rise).

**A tick that restored runs a frame as well only if both fit** in the tick
budget; the budget is now a prediction (time so far + what a frame has been
costing < budget) instead of "stop once over".

**One drawn frame a tick.** Two posts back to back make the main CPU wait
for the renderer.

**A picture stride** (`fe_rewind_set_stride`). With stride s only the frames
a rewind stepping s at a time lands on keep a picture; the others are
re-run with no slot, which on the PSP means no post to the second core at
all (`gpsp_visible_done_hook`). A tick shows the last picture there is in
the stretch it went over, never one from outside it.

**The cadence** (`psp_rewind_tick`): s frames back every p ticks, chosen
from what the game costs and then kept.

| pace | p, s | pictures a second |
|---|---|---|
| x1 | 1, 1 | 60 |
| x1 | 2, 2 | 30 |
| x2/3, x1/2, x1/3, x1/4 | 3,2 / 2,1 / 3,1 / 4,1 | 20, 30, 20, 15 |
| x2 (after 1 s of holding) | 1,2 / 2,4 / 3,6 | 60, 30, 20 |
| x4 (after 2.5 s) | 1,4 / 2,8 | 60, 30 |

A pace fits when one window, packed into ticks the way the core packs it
(the restore, then frames while the next still ends inside the budget, one
drawn frame a tick, at most `burst`), takes no more ticks than showing it
does. The costs are running means the host keeps: a normal frame, a re-run
frame with and without a picture, a restore. Fastest pace that fits and is
not faster than asked; at equal speed the more pictures a second. Before
anything has been re-run the guesses are frame x 1.25, frame, 6 ms.

If more than one show in eight falls short over at least 36 ticks, the pace
actually managed since it began is taken as what the game can do, and the
fastest pace under 9/10 of that is chosen; the ceiling stays for the rest
of the hold. (History recorded in fast-forward has its snapshots further
apart, so the way to each window is re-run too: the model does not know
that, the measurement does.) A longer hold only ever moves to a faster
pace, a shortfall only to a slower one.

The ring's head turns at the mean pace instead of jumping on show ticks.

### 12.3 Sound while rewinding

`rewind_sound` (harness) / `fe_host_rewind_opts.sound_mode`:

| key | what |
|---|---|
| 2 | the frames gone over are heard backwards. Only at x1 with a picture every tick; at every other pace it is silent anyway |
| 1 | silence; the samples are not kept either |
| 0 | silence, and the frames a held rewind re-runs skip this emulator's own sound synthesis (`sound_synth_off`, the GBA link's headless switch). The game's sound code on the emulated CPU runs as ever |

Mode 0 leaves the sound buffer of those frames different from the first
pass, so they must never become play: the core is told (`fe_rewind_taint`),
takes no snapshot from such a machine, and restores once more when the
control is let go. A sleep in the middle of such a hold continues from the
machine as it stands, and the first frames after the wake may differ in
sound (state and picture do not; seen in the desktop sleep fuzz, which is
why that combination is not in the campaign).

Measured in PPSSPP at p1 s1 (same build, same script, 293-tick hold):

| | reversed | silent | silent, synthesis skipped |
|---|---|---|---|
| Driver 2: re-run frame | 9.40 ms | 9.35 | 9.29 |
| Driver 2: tick | 9.45 ms | 9.35 | 9.30 |
| Driver 2: frames shown per tick | 0.99 | 0.99 | 0.99 |
| Emerald: re-run frame | 5.18 ms | 5.15 | 4.90 |
| Emerald: tick | 5.24 ms | 5.17 | 4.98 |
| Emerald: frames shown per tick | 0.97 | 0.97 | 0.97 |

Keeping the sound costs 0.5 %; our synthesis is 0.6 % (Driver 2) to 5 %
(Emerald) of a re-run frame. Neither moves a game from one pace to the
next. The default stays 2.

### 12.4 A restore, split

`rewind_gprof = 1` now prints, after each gesture, the mean of every part:

```
EVT rewind_rprof n= jit_close_us= arrays_us= verify_us= rest_copy_us=
    sound_clear_us= load_us= (cpu= inmain= memory= sound= tail=) maps_us= audio_us=
```

`restore_us` in `rewind_gesture` is their sum; `unpack_us` (walking the
deltas back to the snapshot) is separate and is per step, not per restore.
PPSSPP, Driver 2: arrays 0.46-0.52 ms, loader 0.55 (sound 0.36, memory
0.11, input + main 0.05), unpack 0.34-0.57, everything else under 0.05.
The loader's sound part reads its fields by key from the top of the
document each time; that is the next thing to remove if hardware shows the
same shape.

### 12.5 Other ways to smooth it, priced

- **Keep more drawn pictures.** A slot is 256 x 161 x 2 = 80.5 KiB; the two
  windows are 12 slots, 966 KiB. A second of pictures is 4.7 MiB at full
  size, 1.2 MiB at half size in each direction. It would hide the start of
  a hold and nothing else: frames older than what is kept still have to be
  re-run. Not built.
- **Snapshots twice as far apart** halve the restores per frame shown (a
  restore is 3-9 ms on hardware, so 0.25-0.75 ms a frame) for 966 KiB more
  window and a first window twice as long. Not built.
- **Every second frame, re-run frames without a picture, a fixed pace:**
  section 12.2.

### 12.6 Not done, not verified

- None of this has run on hardware. The cost of a re-run frame with and
  without a picture there (`EVT rewind_cost`) decides which pace each game
  gets; PPSSPP has no second core, so its 9.5 against 8.3 ms says nothing
  about it. Stage 4 measured 14.8 ms for a drawn re-run frame against 9.5
  for a normal one; whether that falls with one post a tick is not known.
- A re-run frame without a picture is not posted to the renderer, but the
  core still captures it. Skipping the capture is not built.
- A drawn frame that alone is longer than a display period makes that tick
  two periods long whatever the pace.

## 13. Phase 2, stage 6: how much is left, a faster pace, a player's build

Stage 5 on the Go was clean (14 of 14) and the owner watched it: it works,
the ring does not say how much rewind is left, and it feels slower than it
has to, Heart & Soul above all. Then a player's build refused the rewind in
every game.

### 13.1 The ring is a gauge

The ring is the history. Fully lit = as much to go back to as there was at
the press. The lit part ends at a bright dot that leaves the top
counter-clockwise as the game runs backwards; behind it is a faint track.
Dot back at the top = nothing left ("Start of history" / "Save point").
`rewind_ring` (harness) picks: 1 the gauge (default), 2 the gauge with
"-0:06   0:04 left" under it, 3 two rings (the turning head outside, a thin
gauge inside), 0 the first version. Pictures of each over a Driver 2 frame:
`builds/rewind-out/ring-gauge/`. Still the plain ring: no arrows, no glyph.

### 13.2 Why stage 5 was slow, and the pace now

Three mistakes, all in `psp_rewind_tick`:

- **A tick was one display period, always.** A game whose frames are 9 ms
  can be re-run three to two periods, but never two to one, so every pace
  was capped at one frame a period. Now a pace with p > 1 gives the tick
  that shows the whole p periods to work in: it runs across the periods in
  between, in which the picture would stand anyway. Time is counted in real
  display periods (the loop's own clock), so a long tick does not slow the
  cadence.
- **A shortfall demoted by a guess.** "More than one show in eight short
  over 36 ticks" tripped on the first window of a hold (2 of 12), and the
  new pace was 0.9 x what had been managed, which from x2/3 is x1/2. Now:
  more than one in four over 48 periods, the first 18 periods of a hold do
  not count, and what is ruled out is that speed and anything more than
  15% above what was managed; the fastest pace left that fits is taken. A
  speed that held for 90 periods is remembered for the game and later holds
  may take it whatever the model says.
- **The model packed frames into single ticks with a 13 ms budget**, so a
  restore (4 ms) and an undrawn frame (10 ms) could never share one. It is
  now arithmetic: one drawn frame + (s - 1) undrawn + their share of a
  restore must fit in p periods less 2.5 ms; and a whole window's work plus
  half a period must fit in the time a window takes to show (the window
  before cannot be begun earlier: two buffers).

Paces are no longer a table: every "s frames every p periods" with p up to
3 (and the slow ones up to 5), fastest that fits and is not faster than the
hold asks (x1, x2 after 1 s, x4 after 2.5 s).

From the costs the Go measured in stage 5 (ESTIMATES; nothing here has run
on hardware):

| game | drawn / undrawn / restore | first second | after |
|---|---|---|---|
| Driver 2 | 17.4 / 10.5 / 4.1 ms | x1 at 30 pictures a second | the same |
| Driver 2, worst sample | 20.5 / 12.3 / 4.1 | x0.75 at 15 | the same |
| Heart & Soul (heavy music) | 10.0 / 8.6 / 3 (guess) | x1 at 60 | x1.33 at 20 |
| Unbound double battle | 6.7 / 7.4 / 3 (guess) | x1 at 60 | x1.67 at 20 |
| a 6 ms game | 6 / 5.5 / 2.5 | x1 at 60 | x2 at 60 |

Stage 5 gave Driver 2 x0.5 and Heart & Soul x0.5. x4 needs frames of
about 3.5 ms; no game measured so far has them, so the ramp ends at what
the game's own cost allows. Going from x1 at 60 to x1.33 at 20 trades
picture rate for distance; `rewind_speed_max = 1` keeps x1.

### 13.3 Costs

- **The loader** read every field by walking the document from its top.
  During a rewind restore a lookup now starts where the last one in the
  same document ended. PPSSPP: loader 0.55 -> 0.26 ms (its sound part 0.36
  -> 0.14). Hardware had 0.94 (0.59).
- **Undrawn frames are run as skipped frames** (`frontend_skip_override`,
  what a fast-forward frame that is not drawn already is): the core does
  not capture them. In PPSSPP, where the core also draws, an undrawn frame
  went from 7-9 ms to 2.8-3.6 (Driver 2) and 4.8 to 1.9 (Emerald); on
  hardware the second core draws, so the saving is only the capture and is
  not known. `rewind_head_skip = 0` turns it off.
- **Waiting for the second core.** A drawn re-run frame posts at the end of
  its visible lines, and before posting it used to spin up to 9 ms for the
  picture before it to finish, then give up without a picture anyway. For
  re-run frames that is now 2.5 ms. `EVT rewind_me` reports what a post
  took, how much of it was that wait, how many gave up, and what showing a
  picture waited for one still being drawn. The hardware logs show the
  renderer taking 12-21 ms for a Driver 2 frame (`me_rend rend_mean_us`),
  which is why a drawn frame there costs what a normal frame costs (15 ms)
  and an undrawn one 9-12: I have not shown where the rest of the
  difference goes when two posts are 33 ms apart, and that line is how to
  find out.
- **The arrays** (1.7 ms of a 4.1 ms restore) are the pages that can differ
  being copied back, about 100 KiB in Driver 2. Deciding a page does not
  need copying means reading both copies of it, which costs what the copy
  does. Left as it is.

### 13.4 A player's build refused the rewind

`fe_host_rewind_blocked()` tested `gba_cable`, which a build that can host
a link-cable room sets at every GBA game load, not when a session runs. No
harness run was ever in that state. It now tests for a running session
(`gbl`, the pair being active, a packet session). It was the only place
the rewind looks at link state.

`tools/rewind_player.sh ARM GAME SCRIPT FRAMES` runs the scripted rewinds
with the harness key `link_avail = 1` (the cable made available exactly as
a player's build does it), once with instance B loaded late and once at
boot, and fails if the rewind is ever denied, if nothing is shown
backwards, or if the checker is not clean. `EVT rewind_denied why=` is
logged once per refused press.

The refusal ring has a text per reason: link play, "Rewind isn't available
in Game Boy games yet" (with the setting on, Triangle in a GB / GBC game
now says so instead of cycling the video preset), and a general one.

`REWIND=1 tools/build.sh <profile>` is the switch (like `GBA_LINK=1`); the
release profile refuses `CORE_EXTRA` / `PSP_EXTRA`, and passes its warning
gate with the rewind compiled in.

### 13.5 Not done, not verified

- Nothing in this section has run on hardware.
- A link session starting while the rewind holds its memory: the session
  loads instance B then, and the rewind's heap share (about 590 KiB at
  2 MB) is still taken. Not looked at.
- Long ticks hold the ring's own motion for their length (20 or 30 updates
  a second at those paces).

## 14. Phase 2, stage 7: how much history, and why

The owner, playing: "the amount of rewind time you get is not very
consistent or easy to understand", at every size from 1 to 8 MB.

### 14.1 Everything that shortens or ends the history

| what | effect | on the ring / in the log |
|---|---|---|
| the ring is full | the oldest snapshots go | "left"; `last_drop=history` |
| a busy scene | each snapshot is larger, so the same ring holds fewer seconds (section 14.2) | `bytes_per_snap`, `gap_frames` |
| rewinding | what is gone back over is given up when play resumes | |
| the game writes its save | **nothing, since stage 9** (section 15). Up to stage 8: ended here, and for 60 frames after the last write | (was "to save point" / "Save point"; `save`) |
| a state is LOADED | ends here | "to state load"; `state_load` |
| the cheat list changes (also once at boot) | ends here | "to cheat change"; `cheats` |
| the game drives the link port: the wireless adapter (Pokemon at boot, in the Union Room and the like), a cable or multiplayer mode change | ends here, and for 60 frames after | "to link use"; `serial` |
| sleep | ends at the wake | "to sleep"; `sleep` |
| a link session; fast-forward if `rewind_ff = 0` | not recorded while it lasts | `not_recorded` |
| the frontend writes game memory | ends here (harness only today) | `poke` |

Checked, not the case: a state SAVE does not end it (PPSSPP, Driver 2,
`state_save_at`: history unchanged); having the cable available does not
change what the link port does to it (the same two boot-time drops with and
without `link_avail` on Emerald); toggling fast-forward, the slack rule and
the pacing only space snapshots out, they drop nothing. Not checked by
running: the in-game menu (it runs no frames and calls nothing in the
rewind unless a state is loaded or a cheat toggled there); games that save
often (each write costs everything before it plus one second: the line
below shows it as `drops: save=` climbing).

One real fault found: with the history partly on the heap (every size
above 1.5 MB), a heap that could not give the whole remainder gave none of
it, silently: 3 to 8 MB could mean 1.5 MB. It now asks for half a megabyte
less until it gets something, and `EVT rewind_ring asked= granted=` says
what was obtained. Whether that happened on the Go is not known (PPSSPP's
heap is 5 MiB larger and grants 8 MB).

`EVT rewind_avail` (heartbeat cadence, every build with the event log):
```
rewind_avail avail_ms= history_frames= ring_asked= ring_granted= ring_used=
    gap_frames= bytes_per_snap= last_drop=WHY@FRAME drops: save= serial= cheat=
```

### 14.2 The same ring, three times the seconds apart

A snapshot is what changed in six frames. Measured: Emerald 4.8-5.2 KB,
Driver 2 driving 30 KB (PPSSPP); on hardware 6-33 KB across four games. At
a fixed six frames 2 MB is 43 s of Emerald and 7 s of that Driver 2 scene.
That, more than any event above, is the "no rhyme or reason".

**A target.** The ring is asked to hold about ten seconds a megabyte.
While snapshots are large they are taken further apart, up to 18 frames
(`fe_rewind_set_target`; `rewind_target_s`, `rewind_gap_max`). A scene that
rewrites the same memory every frame is exactly the case where a snapshot
does not grow with its distance: the Driver 2 scene stays at 30 KB at 18
frames, and 2 MB holds 22.7 s instead of 7.2 (PPSSPP, same run otherwise).
Emerald stays at six frames and 43 s. Settings now reads "about 20 s
(2 MB)".

**What it costs** (PPSSPP, Driver 2, where it stretches; nothing where it
does not):
- Rewinding such a stretch re-runs the way from each snapshot to its
  window as well: 1.5 frames re-run per frame shown there instead of 1.0.
  The first hold of the fixture went from x1.69 with 4 short shows of 171
  to x1.13 with 45 of 172 (the gap was 18 frames there).
- Letting go has further to run forward to the frame shown. In that run a
  45-tick hold was followed by 50 ticks before play resumed, against 3 to
  11 without the target. I have not found why it is that many (the gap
  alone would be 18), and it is the cost I would look at first on
  hardware.
- Memory: none. Forward speed: fewer snapshots, so less work.
`rewind_target_s = 0` is the old behaviour.

**Smaller snapshots instead?** Rewrites with identical values already cost
nothing (the delta is an XOR, zero runs are one word). The sound in flight
is one or two KiB of a snapshot. What is large is memory that really
changes every frame, a bitmap game's frame buffers above all. I have not
measured the split per region, so I do not know of a cheap win.

### 14.3 Not done, not verified

- Nothing here has run on hardware.
- The cost of the target on hardware (pace and the wait at release in a
  stretched scene) is the open question; `rewind_target_s = 0` against the
  default on the same fixture answers it.
- With the slack rule a busy game already takes snapshots up to 48 frames
  apart; the target's limit of 18 does not bound that.

## 15. Phase 2, stage 9: the cartridge save goes back with the game

The owner, after playing Fire Emblem (a couple of seconds of history at any
size, because it rewrites its suspend save all the time): "we need to allow
save reversal while rewinding ... it makes perfect sense." This replaces
"a rewind never crosses an in-game save" (2.4, 10.4). The other barriers
stay: a state load, a cheat-list change, link-port activity, sleep, and the
refusal during link sessions.

### 15.1 The save in memory

- `gamepak_backup`, all 128 KiB whatever the chip, is the LAST span of a
  snapshot (`rw_span_sav`, fe_host.c). It is compared where it lies, by its
  own written-page map, 1 KiB pages (`gba_rewind_backup_dirty`,
  gba_memory.c). The save has eight writers, all C, all in that file
  (`BACKUP_MARK`): translated code reaches it only through `write_backup` /
  `write_eeprom`. So the map is exact: a snapshot in which the game did not
  write its save reads none of it, one in which it did stores the words
  that changed.
- The chip's command state (flash mode, command position and bank, the
  EEPROM mode, address and bit counter, the clock chip) was already in the
  save-state image, so it is in the "rest" span. **Decision: no quiet
  window.** A rewind may land on any frame, also between two writes of one
  save: the bytes written so far and the chip's state are both exactly the
  first pass's, and the frames after it write the rest. Checked on every
  re-run frame and every resume by the tests below (the unit test's write
  depends on the byte it replaces).
- A restore copies back the pages that can differ: written since the last
  snapshot or restore, or changed in the reference by a step back
  (`rw_sav_restore`); `rewind_verify` compares all 128 KiB after it.
- Frames re-run for a rewind write the save as the first pass did
  (`gba_rewind_backup_ro` stays 0).
- Cost: 131,072 bytes more reference image. On the PSP at 2 MB the ring is
  now 1,376,256 bytes in the pool + 720,896 on the heap (was 1,507,328 +
  589,824); the pool keeps 13 KiB to spare (PPSSPP, `rewind_mem low=`).
  Recording cost in a frame that does not write the save: taking a 128-byte
  map per snapshot.

`rewind_cross_save = 0` (harness) / `--rewind-stop-at-save` (desktop) give
the old rule back for comparison. The ring texts "to save point" / "Save
point" can only appear in that mode.

### 15.2 The save file

The file does not follow the save around (fe_host.c, "the save and the
file"):

1. **While a rewind is on screen nothing is written.** `rw_sav_hold` is
   raised before the first restore of a press and stays until play resumes;
   the writer thread returns at once, a scan and a drain refuse. The
   emulation thread first lets a save of the game's own that is on its way
   to the stick land (at most 250 ms; a drain is tens of milliseconds; this
   is the press right after "saved the game"), raises the hold, and waits
   for the writer to be idle (`sram_io_busy`) so no block is copied while
   the rewind is replacing it. `EVT rewind_save_wait` when it had to wait.
2. **When play resumes** and the save in memory differs from the file, the
   file is REPLACED, whole (`rw_sav_commit_run`): `X.sav.rw-new` is written
   and closed, `X.sav` is renamed to `X.sav.rw-old`, the new file is renamed
   to `X.sav`, the old one removed. (fe_ini_doc_save's sequence: rename is
   never asked to overwrite.) A rewind changes many sectors at once in a way
   no game does (a Pokemon save slot back to its previous generation), so
   the block-by-block update that is right for a game's own writes would
   leave a mixture if the power went half way. A rewind that crossed no save
   write finds nothing to do (`noop`).
3. **A press during the replace** abandons the new file before the renames
   and leaves the request standing: tapping never queues writes.
4. **Before any `.sav` is loaded** (`rw_sav_recover`, in every REWIND
   build, also with the setting off): no `.sav` but both others = the
   replace had the complete new file, it is renamed in; only the old one =
   it is put back; a lone `.rw-new` is a fragment and is removed.
5. **Failures.** A failed write of the new file touches nothing else and is
   tried again; after three in a row the whole image is rewritten in place
   (`rewind_save_commit GAVE_UP`), the pre-existing non-atomic path.
6. Exit, quit and sleep in the middle of a rewind: the file gets the save
   of the frame the MACHINE is at (within two windows of the picture),
   through the same replace.

What is on the stick, when:

| moment of a power cut / crash | the file |
|---|---|
| any time while the control is held or play has not resumed | what it was when the control went down |
| while `.rw-new` is being written | the same (the fragment is removed at the next load) |
| between the two renames | no `.sav`; the next load takes the complete new one |
| after | the save of the frame landed on |

**Not covered, said plainly.** (a) If the game's own save was still being
flushed when the control went down and the stick did not take it within
250 ms, the file stays that mixture of the game's old and new sectors for
as long as the rewind lasts: the exposure the normal flush has always had
for one write, stretched. (b) The new image is copied a block at a time
while the game runs on; a save the game makes in that second is partly in
it, as it would be in a block-by-block flush. (c) Between the two renames
another program reading the stick sees no `.sav`. (d) None of the file path
has run on a memory stick: rename, remove and the timing are PPSSPP's.

### 15.3 Tests and results (desktop x86 dynarec unless said)

| test | result |
|---|---|
| `test_fe_rewind.c`, 9 new `savespan_*` cases (50 in all) | PASS; rewinds went back over 11-133 save writes per case. Mutants: restore leaves the save alone, 84 checks fail; a write is not marked, 93 |
| `tools/rewind_savetest.py`: Emerald (flash 128K, clock chip), the owner's save, START-menu save in frames 2095-2187 | 20 of 20: rewound to frame 625 across all 86 writes; save in memory and file on disk = the never-rewound run's save of frame 625; played on and saved again: 3,693 frames identical to a run that never rewound and pressed the same buttons, `.sav` files identical; replayed with the same inputs: 2,389 frames identical, file identical |
| ...killed at each step of the replace | step 1, 2: next start loads the save from before the rewind; 3 (`took=new`), 4: the save landed on; nothing left beside the `.sav` |
| ...killed at the 700th held tick | file = the save from before the rewind |
| ...2 replaces fail / 5 fail | file at exit = the save landed on (retry / in-place fallback) |
| `tools/rewind_campaign.sh`: the stage 8 sets on 6 games (Fire Emblem added) + save sets, 57 jobs, 103 checked runs | 57 of 57: 126,521 pictures shown backwards, 827,090 forward frames against clean runs, 223,796 re-run frames, 0 wrong; 76 save writes gone back over |
| `tools/rewind_savesweep.sh`: all 36 ROMs, rewind across the last save write the mash script causes | 20 games write: SRAM 9, flash 64K 4, flash 128K 1, EEPROM 512 5, EEPROM 8K 1. In all 20 the save of every re-run frame (425-2,481 per game) and of every resume is the first pass's. 14 pass outright; the 6 that fail do so in picture or state, not in the save: 15.5 |
| PPSSPP (MIPS dynarec, writer thread), `tools/rewind_ppsspp_save.sh s9-save` | PASS: 145 save-writing frames gone back over, 2,370 re-run frames, `rewind_save_commit ... thr=1`, file on disk = the save landed on; PPSSPP killed in the middle of the rewind: file = the save from before it |
| host tests | 30 of 30 |
| REWIND off | 36 desktop objects identical to stage 8 in `.text .data .rodata .bss` (`tools/rewind_offcheck.sh`) |

**Fire Emblem: The Sacred Stones**, `tools/rewind_fe_history.sh` (four
minutes of START / A / d-pad from power-on, 2 MB, target on; 94 frames
write the save): history at each 10-second heartbeat, after the first
minute. Old rule: 0 to 44 s, median 9.1 s, under 5 s at 7 of 18, 31 drops.
Now: 25 to 45 s, median 28 s, never under 25 s, no drop. This is menus and
the first map by button mashing, on the desktop; nobody has played a
chapter with it.

### 15.4 The frame cut again

`rewind_check.py --allow-cut N`. A game that copies its flash routines into
RAM to save (Emerald) changes code under translated blocks between two
snapshots; a restore across that has to drop the RAM translation cache
(`code_kept=391/398`), the blocks are rebuilt from other entry points and a
frame ends a block earlier or later (section 6). Emerald: 3 of 2,383 re-run
frames differ from the first pass in 8 bytes (program counter, cycle
counters), same save, same picture, and the frames played after them are
identical. Golden Sun does the same at stage 8 already (one frame of 345
with a 256 KiB ring and a 2 s target). Counted as `cut_moved`; one in the
whole campaign.

### 15.5 Found on the way: holes that are not the save's

Rewinding to the first frames after power-on was not reachable before (file
creation ended the history), and the scripted test had only ever run on six
games. Both now run on all 36 (`tools/rewind_library.sh BUILD 4000`).

- **Fixed: the wave-RAM pending flag** (`gbc_sound_wave_update`) is not in
  a save state. Lost by a restore while the sound is off, it left the wave
  channel's samples stale until the game wrote the wave RAM again: Minish
  Cap replayed 240 frames after power-on with a different machine. Now in
  the snapshot (libretro.c). An ordinary state load still has the hole.
- **NOT fixed, present at stage 8 as well (same counts on both builds):**
  the scripted test at frame 4000 fails on 5 of 36 games.

| game | pictures shown backwards that are wrong | replayed frames that differ | re-run states that differ |
|---|---|---|---|
| Golden Sun: The Lost Age | 1,015 of 1,119 | 132 of 593 | 1,053 of 1,121 (955 bytes of sound in flight are zero in the re-run) |
| Iridion II | 1,008 of 1,275 | 540 of 593 | 0 |
| The Lord of the Rings: The Third Age | 488 of 3,317 | 36 of 593 | 0 |
| Advance Wars | 197 of 2,415 | 1 of 593 | 0 |
| Lara Croft: Legend | 62-64 of 2,842-3,353 | 0-1 | 0 |

  In the save sweep, where the rewind reaches the first 150 frames after
  power-on, two more fail that pass at frame 4000: Castlevania: Harmony of
  Dissonance (36 of 670 pictures' sound, 25 re-run states, 1 replayed
  frame) and Shrek: Super Slam (192 of 437 replayed frames). Not looked
  into; the wave-RAM flag above was this kind of thing.
  With the save crossed, Fire Emblem: The Binding Blade shows 2 wrong
  pictures of 2,121 (frames that were behind a save barrier before). Where
  the state is equal and the picture is not, something the renderer keeps
  outside the save state is the suspect (the affine reference latches are
  not in it; not shown to be the cause). Golden Sun 2 differs in state and
  sound as well. None of this was looked into further: the next thing to
  do, before more features.

### 15.5a What kind each failure is (first pass; handoff notes)

`tools/rewind_classify.sh BUILD 4000 GAME...` (keeps the per-frame log and
prints, per kind, the count and the first frame and field: F = a frame
replayed with the same inputs, Q = the machine a re-run frame leaves, R = a
picture or sound shown backwards). Stage 9 build, x86 dynarec:

| game | F (forward game after a rewind) | Q | R | kind |
|---|---|---|---|---|
| Golden Sun: The Lost Age | 132, first frame 3701: state + audio | 1,069, first 3986: state | 1,015, first 3991: audio | **state not restored (sound). Release-blocking.** |
| Iridion II | 540, first 3701: video only | 0 | 1,008: video | picture only, state equal |
| LOTR: The Third Age | 36, first 1248: video only | 0 | 488: video | picture only |
| Advance Wars | 1, frame 4184: video | 0 | 197, first 3986: video | picture only |
| Lara Croft: Legend | 1, frame 4184: video | 0 | 64, first 3854: video | picture only |
| FE: The Binding Blade | 0 | 0 | 2, first 3224: video | picture only |

What is established:
- **Golden Sun 2 fails the same way with the interpreter**
  (`--option gpsp_drc=disabled`), without the quick restore, and with the
  image layout and the full flush (the plain state-load path). So it is
  neither the translator nor the rewind's own restore: the save-state image
  plus the rewind's extras do not hold everything the sound depends on.
  The bytes that differ are the sound in flight (955 bytes, zero in the
  re-run where the first pass has samples) and the audio of the very first
  replayed frame differs. Golden Sun 1 (same engine) is clean. Looked at
  and ruled out: `volume_halve` (it is saved, as "volume"). NOT found.
  Next step: dump frame 3701 both ways (`--rewind-dump-frame`), then
  compare sound.c's globals one by one against `sound_write_savestate`
  (the timers that clock the FIFOs, `sound_on`, the FIFO refill request to
  DMA, anything in main.c's timer array that the image leaves out).
- **Iridion II and Advance Wars are clean with the interpreter.** The state
  after every frame is equal in the dynarec too, so the machine is right
  and the picture is not: the scanline renderer is called at block
  boundaries, and a replayed frame whose blocks are laid out differently
  (translated later, from other entry points) lets a mid-frame register
  write fall on the other side of a scanline. Not proven beyond the
  interpreter result; LOTR, Lara Croft and Binding Blade were not run with
  the interpreter. If that is it, it is the frame cut inside the frame: the
  forward game is right, single pictures differ in raster effects, and a
  REPLAYED frame can differ from the first pass in its picture with no
  rewind bug at all (the F counts above), so the checker needs to tell
  "picture differs, state equal" apart from a real divergence.
- Harmony of Dissonance and Shrek near power-on: not classified.

Not done from the coordinator's list of 2026-10-10: the general fixes; the
whole-library command with a before/after table (the owner's ~100 titles
included, and a PPSSPP subset); the production self-check (the test build's
per-frame state CRC is a full gather + CRC of 459 KiB, far too dear for a
PSP; a version hashing only the "rest" span and the pages the map says
were written would be the thing to price); waiting for the game's own
flush before a rewind starts instead of the 250 ms bound; copying the image
for the replace under the flush's own ownership; and running the recovery
check only when a `.rw-new` / `.rw-old` exists.

### 15.6 Not done, not verified

- Nothing in this section has run on a PSP. The file path's timing there
  (the 250 ms wait at a press right after a save, the 128 KiB replace by
  the writer thread, rename and remove on a memory stick) is unmeasured.
- A sleep in the middle of a rewind with a replace owed: by reading only.
- Fire Emblem in a chapter, by hand.
- 15.5.

## 16. Stage 10: rewind right in every game (the library failures, closed)

Third engineer, 2026-10-10. Everything in 15.5 / 15.5a is explained and,
where it was a defect, fixed. None of it was a per-game case.

### 16.1 Golden Sun: The Lost Age (and Harmony of Dissonance near power-on): a direct-sound resampling bug in the core

Not a hole in the save state. `sound_timer` (sound.c) kept only the
FRACTIONAL part of the resampling phase after each FIFO sample. For a FIFO
clocked at or below the output rate (32,768 Hz) that is right. Golden Sun 2
clocks its FIFO at 63,072 Hz (timer 0 reload 266): the step is 1.92, the
integer part was thrown away, and the channel wrote one output sample per
FIFO sample whatever the step. Two consequences:

- the direct sound of such a game played at about half speed, overlapping
  itself (a plain emulation bug, in every build, rewind or not);
- the channel's position in the mix ring ran away from the mixer's and
  lapped the whole 65,536-entry ring about once a second (measured: its
  distance from the read position took every value from 50 to 65,418 within
  300 frames; in the other games it stays within 6 entries of the
  mixer's). So "sound in flight" lay anywhere in the ring, and a rewind
  carries 8,192 entries from the read position (a state load: none). That
  is the 955 zero bytes of 15.5a.

Fix (commit 2363e97): take exactly 1.0 off the phase instead of keeping the
fraction; identical arithmetic for a step <= 1.0. The state loader no longer
refuses a saved phase of 1.0 or more (a state saved by this build in such a
game does not load in an older build; older states load here). After it the
channel stays within 2 entries of the mixer in Golden Sun 2. Shrek's
channel also moved (it sat 254 entries behind the mixer before, 2 after),
so it had a fast stretch too. `sound.o` therefore differs from stage 9 with
REWIND off: intended.

**A plain state load** was affected the same way and more (it restores none
of the ring by design: "up to one frame of silence", sound.c). With the fix
that design holds again in these games. The wave-RAM pending flag (15.5) is
still not in an ordinary save state.

NOT verified: by ear. Nobody has listened to Golden Sun 2 before and after;
the arithmetic says the music was wrong before. Worth one listen on a PSP.
LOTR: The Third Age shows a wandering channel position too, but there the
channel's timer is stopped (a stale index, nothing written): harmless.

### 16.2 "Picture differs, state equal" (Iridion II, LOTR, Advance Wars, Lara Croft Legend, FE Binding Blade): an x86-only palette conversion bug. The block-layout theory is REFUTED

The handoff's reading (the scanline renderer runs at block boundaries, so a
different block layout moves a register write across a scanline) is wrong.
Evidence:

1. A trace at every scanline render of frames 3699-3703 of Iridion II
   (program counter, cpu_ticks, the cycle remainder, a hash of the
   registers, CRCs of VRAM, OAM, palette RAM, I/O, IWRAM, EWRAM), first pass
   against the replay after a rewind: identical on all 160 lines of every
   frame. The machine reaches every scanline in the same state at the same
   cycle.
2. `--flush-ram-at 3000 7` (new instrument: drop the RAM translation cache
   every 7 frames in a run that never rewinds) against the same run without
   it, 4,300 frames, six games (the five above and Golden Sun): 0 frames
   differ in state, picture or sound.
3. What did differ at render time: `palette_ram_converted`, 39 entries in
   Iridion II. The x86 store stub (`ext_store_palette16b`, x86_stub.S)
   shifted bit 15 of the written value into the lowest green bit;
   `convert_palette` (the C paths, and the rebuild after a state load)
   masks it. A colour written by translated code with bit 15 set was one
   green step off until a restore rebuilt the table: the FIRST pass was the
   wrong one, by 1/64 of green in those colours.

Fix (commit a9aaf01): one `and $0x1F, %cl` in the stub. All five games
clean. **The PSP is not affected**: the MIPS emitter's `palette_convert` is
the C macro bit for bit (`(v << 1) & 0xFFC0 | v & 0x1F`). This was a
desktop-twin artefact; the picture on a PSP does not depend on
translation-cache history through this path. (By reading the emitter; not
run on PPSSPP.)

So the checker needs no "picture" class: a picture that differs is a
failure, with no allowance.

### 16.3 What DOES depend on translation-cache history: a few cycles of interrupt timing (Shrek near power-on; the "frame cut" of sections 6 and 15.4)

Shrek: Super Slam, rewinds that reach the first 150 frames after power-on:
from frame 51 on the re-run machine differs from the first pass. At frame
51: only CPU registers, `cpu-ticks` (9 cycles apart), `exec-cycles`,
`video-count` and timer 0's count. Later (frame 120): the counters agree
again and 18 bytes of IWRAM at 0x03007B94..7D43 differ, i.e. dead values
under the interrupt stack. Picture, sound and save are the reference's on
every frame. Clean with the interpreter; clean with `--rewind-full-flush`
(the same restore with both translation caches dropped as well).

So this one is real and pre-existing: with other translated blocks in the
cache (here: a cache warmed by the game's later code, against the cold
cache of the first pass) an interrupt is taken a few cycles earlier or
later. My reading of why, NOT proven: a store that raises an alert (an I/O
write that starts a timer, HALT, an IRQ enable) calls `update_gba` with the
cycle register as it stands, and the cycles of the current block not yet
subtracted (`cycle_count`, flushed at branch targets) depend on where the
block's scan put its entry points. The machine that results is as valid as
the first one (the emulator's timing is that coarse everywhere), and a
rewind does not need the first pass's future, but it is not bit-identical,
and a game that never halts keeps the shift.

What follows from it:
- The checker has `--allow-timing N` (rewind_check.py; rewind_test.py
  passes it on): a frame whose STATE differs while picture, sound, input
  and save are the reference's is counted as `timing_only`, not failed; a
  re-run frame whose state differs and whose save does not likewise
  (`rerun_timing_only`; its picture and sound are checked when shown).
  Anything a player can see or hear still fails. Default 0 (strict, as
  before); `rewind_all.sh` uses -1 and prints the count per game.
- **The production self-check (the coordinator's item 2) cannot demand a
  bit-identical machine** on a re-run frame unless the cut is made
  independent of the cache first: it would fire in Shrek, Emerald (15.4)
  and Golden Sun. Options: fix the accounting in both emitters (flush the
  pending cycle count before a store that can raise an alert: a dynarec
  change, under the dynarec campaign's rules); or drop both translation
  caches on every restore (the retranslation cost on a PSP was the reason
  for `code_kept`; not priced); or check something weaker. Not decided,
  not built.
- Whether the MIPS dynarec moves the cut the same way: not measured (a
  PPSSPP run of the boot test is owed).

### 16.4 The standing test

`tools/rewind_all.sh BUILD [PLAY]` (WSL): the 36-ROM corpus and every GBA
ROM of the owner's library (34 files in `GBAdhoc/roms` today, read in
place, nothing written beside them), three runs per game: the scripted
rewinds at frame 4000, the same after 250 frames (reaching power-on), and
the tap storm (2 seeds x 6,000 ticks) against a run that never rewound.
One row per game, exit status 0 only if all pass. Results: 16.6. The PPSSPP
(MIPS) subset is NOT in the command yet.

### 16.5 The owner's decision of 2026-10-10: the save is PERMANENT under rewind (replaces section 15's design; NOT built)

"Rewind takes back the game, never the save." Stage 9's machinery (the
save span, the held flush, the `.rw-new`/`.rw-old` replace and its
recovery) is to be REMOVED and the flush path to behave exactly as with
rewind off. The rule set as given by the coordinator:
1. a rewind never changes the save's contents, in memory or in the file;
2. a save write does not end or shorten the history;
3. a rewind may pass over a save but never LANDS inside one in progress
   (snap to the nearest frame outside; "inside" = from the first
   write/command until the chip is idle and N quiet frames have passed; N
   per chip to be given, and what happens in a game that never stops
   writing);
4. the chip's state machine after a landing is idle; re-run frames that
   pass an old save sequence run with the contents write-protected (stage
   8's `gba_rewind_backup_ro`) while the chip's command state still runs,
   and reads of save DATA during a re-run must return what the first pass
   read (the contents are newer now): an undo log of overwritten save bytes
   used only to serve re-run reads, or reads logged with the inputs; how
   often games read their save during play is to be counted over the
   corpus;
5. documented consequence: after rewinding past a save, the cartridge's
   save is from the later moment;
6. tests: Emerald (file CRC and mtime unchanged across a rewind; save again
   and cold-boot), every frame of the save window as a release point, Fire
   Emblem: The Sacred Stones under the fuzz with a cold boot from the
   resulting file, one game per chip, kill at any moment, mutants for the
   write-protect and the snap;
7. `rewind_cross_save` stops being a product choice; the "to save point"
   ring text goes.

Notes for whoever builds it (I did not start):
- Stage 8's mechanism is in git (the commits before "the cartridge save is
  rewound with the game"); `gba_rewind_backup_ro` still exists in
  gba_memory.c next to the eight writers (`BACKUP_MARK`). Stage 9 added
  `rw_span_sav` and `rw_sav_*` (fe_host.c, "the save and the file"),
  `gpsp_rewind_backup_*` (libretro.c) and `rw_sav_recover` in `sram_load`.
- Reads during a re-run: every read of save data goes through the C
  readers in gba_memory.c, so an undo log is exact: at each write of the
  first pass push (frame, address, old byte); a re-run at frame f serves a
  read of address a with the oldest logged old byte whose frame is after f,
  else the live contents. Bounded by the history (drop entries older than
  the oldest snapshot); a Pokemon save is tens of KiB of writes, so the log
  needs a cap, and an overflow should end the history at that save (the
  stage 8 rule) rather than serve wrong bytes.
- The checker's save field then has to mean "the live save, untouched": E
  and Q lines compare against the newest contents, not the frame's.
- The hardening I had begun for the old design (no copy while the game
  writes, a timing split in `rewind_save_commit`, recovery only when the
  .sav is missing) was discarded uncommitted: moot.
- Stray `.rw-new` / `.rw-old` files: stage 9 only ever ran from the lab
  folders (harness and `-play` builds handed to the coordinator); a one-time
  cleanup is cheap to keep if a player build of stage 9 was ever used with
  the owner's saves, otherwise not worth code.
- The hardware run that prompted the decision (`k9_em_save`, PSP-1000): a
  whole-file replace took 1.3 s, then 11.3 s. The first replace's image
  (crc b2527d49) is the save as it stood at frame 2114, in the middle of
  the game saving AGAIN after the rewind had landed at frame 2064 (the
  landing itself had the right save: `E 2064 46b8d87a`, the loaded one);
  the final file 082cf974 is the game's own completed second save. Nothing
  was wrong in that run, and it shows the weak point the owner's rule
  removes.

### 16.6 Results of `tools/rewind_all.sh` (desktop twin, x86 dynarec, 2026-10-10)

70 games: the 36-ROM corpus (`C_`) and the 34 GBA ROMs of the owner's
library (`L_`; most are the same titles). Full tables:
`builds/rewind-out/psp/s10a/table_before_f3.txt` and `table_after_g6.txt`.

| | pass | fail | timing-only frames |
|---|---|---|---|
| before (stage 9, twin `f3`) | 55 | 15 | 13,006 |
| after (twin `g6`, commits 2363e97 + a9aaf01) | 70 | 0 | 1,824 |

Before, the corpus rows that failed (the library rows of the same titles
fail the same way): Advance Wars (test, boot, fuzz), Harmony of Dissonance
(boot), FE: The Binding Blade (test, fuzz), Golden Sun: The Lost Age (all
three), Iridion II (all three), Lara Croft: Legend (test, fuzz), LOTR: The
Third Age (test, fuzz), Penny Racers (fuzz: 159 pictures, 63 forward
frames; not in the handoff's list, found by the fuzz, gone with the same
two fixes).

After: 659,757 pictures shown backwards, 1,861,983 forward frames compared
with a reference, 712,889 frames re-run for a rewind: 0 wrong in picture,
sound, input or save. Timing-only frames (16.3), per game: Shrek 407, LOTR
476, Advance Wars 10, Penny Racers 10, Advance Wars 2 6, Unbound 6, Golden
Sun 2; 0 in the other 29 corpus games.

The boot run asks for 100 re-run frames, not 300 (`--min-redo`): three
games have fewer than 300 frames of history 250 frames after power-on.

### 16.7 Handoff (2026-10-10, end of the third engineer's context)

Done: item 1 except its PPSSPP subset. Not started: the permanent save
(16.5, now the next thing), the production self-check (blocked on 16.3's
question), pace and the ring number.

- WSL work area `~/rewind-agent/s10`: `twin.sh NAME` (as tools/
  rewind_twin.sh; applies `s10/NAME.patch` if present), twins `g6` (this
  tip's code) and `f3` (stage 9), `rewind_all.sh` (copy of the tool),
  `classify.sh`, `flushtest.sh BUILD ROM [AT EVERY FRAMES]` (16.2 point 2),
  `drift.sh` (direct-sound position against the mixer; needs the `g0.patch`
  instrument), `slcmp.py` + `sl_insert.py` (the per-scanline trace of 16.2
  point 1, inserted into a twin's main.c; env `SLTRACE=lo,hi`
  `SLTRACE_OUT=file`). Results: `~/rewind-agent/all_g6`, `all_f3`.
- PSP builds of this code (a9aaf01): `builds/rewind-out/psp/s10a`
  (harness, `gbadhoc.prx` 4512540b) and `s10a-play` (EBOOT daaca284). They
  still contain the stage 9 save design.
- Owed: PPSSPP/MIPS subset of the standing test; one listen to Golden Sun 2
  on hardware; whether MIPS shows 16.3's timing shift; the state loader's
  wave-RAM flag.

## 17. Stage 11: the cartridge save is PERMANENT under rewind (built; replaces section 15)

Fourth engineer, 2026-10-11. The owner's rule: "Rewind takes back the game,
never the save, like a real cartridge." Commit 4d8df8b. Section 15's
machinery is gone from the tree (the save span, the 128 KiB reference copy,
the held flush, the `.rw-new` / `.rw-old` replace and its recovery pass,
`rewind_cross_save`, `--rewind-stop-at-save`, the "save point" ring texts).
Nothing in the rewind reads, writes, renames or holds back the `.sav`; the
flush path does not know a rewind exists.

### 17.1 The rules as built

1. **A rewind never changes the save's contents.** Every frame re-run for a
   rewind (held, or catching up after a release) runs with
   `gba_rewind_backup_ro = 1`: the eight statements in gba_memory.c that
   change a byte of `gamepak_backup` are skipped (`BACKUP_RO`), and
   `backup_write_gen` does not move, so the file's writer sees nothing.
   A restore does not touch the save either (it is not in a snapshot).
2. **A save write does not end or shorten the history.** `rw_after_frame`
   no longer has a save barrier. (The serial port's barrier and its quiet
   period are unchanged.)
3. **The chip's command state still runs in a re-run** (flash mode and
   command position, bank, the EEPROM mode / address / bit counter): it is
   in the snapshot and a re-run frame has to leave it as the first pass
   did. This emulated chip has no time: an erase or a program completes in
   the write that asks for it, and there is no status polling to answer
   (flash ID reads and the EEPROM's ready bit come from that state, so they
   are the first pass's by construction).
4. **What a re-run frame READS of the save comes from a read log**
   (`rewind_backup_data`, gba_memory.c): on the first pass every byte of
   save contents the game reads is appended to a byte ring (one entry per
   EEPROM byte, not per bit); with the write-protect on, the read is
   answered from the log at the position the snapshot recorded. The log's
   position is the last 8 bytes of a snapshot's first span, so it goes back
   with every restore. I chose the read log over 16.5's undo log because it
   needs no view of the old contents and no ordering of writes against
   reads: writes are dropped, reads are replayed, both in program order.
   Its cost is in 17.3.
5. **Play never continues from inside a save in progress once the save has
   changed after that frame** (fe_rewind.c, "where not to stop"). The
   platform reports each first-pass frame as `FE_RW_IN_BUSY` (the game
   wrote to the chip, data or command, or read save contents, or the chip
   is not idle at the end of the frame) and `FE_RW_IN_CHANGE` (the contents
   changed). A frame t may not be landed on when a BUSY frame lies in
   (t - N, t] AND a CHANGE lies after t, in this timeline or in one that
   was abandoned (`last_change` is sticky across landings). When the
   control is let go on such a frame (a release, the end of a tap, the
   limit), `now` moves to the NEAREST frame that may be landed on, the
   earlier of two equally near, and the control is ignored until the
   machine is there. The frame the rewind began at can always be landed
   on, so one always exists; a game that never stops writing gets the
   nearest gap, and if the history holds none, the frame the rewind began
   at (nothing is lost, the rewind just did not take).
   - Why "AND a change lies after t": without one the save is byte for byte
     what it was at t, so the machine of frame t, chip state included, is
     exact wherever t lies. This is what keeps a game that touches its chip
     every frame rewindable at all.
   - Why READS count as busy: a game loading its save over several frames
     (Pokemon's Continue) must not get half of it from the log (old) and
     the rest live (newer): it would build a state from two saves and could
     then write that back.
   - N = `rewind_save_quiet` = 60 frames. Measured (17.3): of 743 quiet
     gaps between two busy frames over the corpus, 352 are 30 frames or
     less, 57 are 31-60, 334 are longer. There is no knee in that
     distribution, so N is NOT derived from it: one second is a margin, and
     its cost is only how far a release inside a save moves (17.3). A
     multi-step save whose steps are more than a second apart would be
     treated as two saves; I know of no such game and cannot exclude one.
6. **After a landing the chip is idle** whenever the save has changed after
   the landing frame (rule 5 includes "not idle at the end of the frame"
   in BUSY); otherwise it is in the first pass's state, which matches the
   contents. Checked on every resume of every test (`E frame busy save`).
7. **After a landing the game sees the real, newer save.** Correct by the
   owner's rule. One visible consequence, found by the library test:
   Driver 2 initialises its save in frame 3 after power-on; rewound to
   frame 2 and played on, it finds a save and does not initialise it again,
   so those frames differ from the first time (40 save bytes read instead
   of 120). That is a cartridge with a save in it being switched on.

### 17.2 The read log's limit, and what it costs

128 KiB by default (`rewind_rlog_bytes`, a power of two; it lives in the
rewind's own buffer, so the Settings toggle off costs nothing and at 2 MB
the PSP pool keeps the same 13 KiB to spare as in stage 9: `rewind_mem
buf=180232@pool ... low=08503300`). History whose reads have left the ring
is dropped (`fe_rewind_trim`, the newer part stays; `rlog_trims`); if one
frame reads more than the ring holds the history restarts after it
(`rlog_resets`; in the corpus that is the frame at power-on or Continue in
which a game reads its whole save).

So the history reaches back over at most ~127 KiB of save READS. Measured:
- Pokemon Emerald reads 290,816 bytes at Continue, and **114,702 bytes for
  one START-menu save** (it reads every sector back after writing it,
  twice). One save fits; a rewind cannot go back over two. That is the
  honest limit of this design: with `rewind_rlog_bytes = 262144` it is two
  saves, for 128 KiB of memory.
- An undo log of overwritten bytes (16.5) would cost about half per Pokemon
  save and nothing for reads; it needs a view of the old contents for
  re-runs. NOT built: it is the follow-up if the owner wants several saves
  of reach.

### 17.3 Measured over the corpus (twin `h3`, the standing test's three runs per game)

How often games read their save during play: 35 of the 36 corpus ROMs
touch the chip in the first ~4,000 frames of button mashing; 21 write it.
Bytes read (largest of the runs): Pokemon FireRed / LeafGreen / Emerald /
Unbound / Heart & Soul 344,064; FE Binding Blade 203,074; Ruby / Sapphire
172,032; FE Sacred Stones 158,084; Advance Wars 1 and 2 147,457; Golden
Sun 131,072 and 122,880; Mystery Dungeon 108,370; the rest under 62 KiB;
Driver 2 120. Re-run frames were served 2.5 million save bytes from the
log; **0 reads had nothing logged**, in every game. Releases that fell
inside a save and were moved: 30 in 6 games, furthest 54 frames (Penny
Racers, which touches its chip every few frames for long stretches), 22
elsewhere.

### 17.4 Tests and results (desktop twin `h3`, x86 dynarec, unless said)

| test | result |
|---|---|
| `tools/rewind_all.sh h3` (36 corpus + 34 library ROMs; test, boot, fuzz) | **70 of 70** (`builds/rewind-out/psp/s11a/table_h3.txt`): 656,985 pictures shown backwards, 1,817,073 forward frames compared, 710,551 re-run frames, 0 wrong; timing-only frames 128. Per re-run frame and per resume the save's CRC is held against the last played frame's: never changed |
| `tools/rewind_savetest.py --mutants` (Emerald, a copy of the owner's save, START-menu save in frames 2095-2187, 8 MiB of history) | 16 of 16. Rewound from frame 3214 to 577 across all 86 writes; the file's size, mtime and CRC identical when the control went down and when play resumed, and equal to the never-rewound run's at exit; 57,358 save writes refused in re-runs, 114,702 save bytes served from the log, 0 unlogged; saved again from 577 (88 frames wrote) and the resulting file, started cold, reaches the picture the never-rewound file reaches (not the no-save one); killed at rewind ticks 3, 400, 1500: the file is the save the game wrote; 159 releases with frames 2092..2250 on screen: the 92 inside the save moved to 2094 or 2187, none landed in 2095..2186 |
| mutants | re-runs write the save: caught (Emerald savetest; FE in rewind_all). Re-runs read the live save: caught in FE Sacred Stones (152 wrong pictures, 284 wrong forward frames) and Driver 2; NOT visible in Emerald (what it reads back while saving is what it has just written). No snap: 3 of 3 releases inside the save caught; unit test built `-DMUT_NOSNAP` fails 33 checks |
| per chip | 21 corpus games write their save under the mash script and all pass (17.3). Section 15.3's sweep of the same ROMs counted SRAM 9, flash 64K 4, flash 128K 1, EEPROM 512 5, EEPROM 8K 1; I did NOT re-derive which game has which chip, so "one game per chip type" rests on that count plus Emerald (flash 128K) in the savetest |
| Fire Emblem: The Sacred Stones, `tools/rewind_fe_history.sh` (4 minutes, 94 save-writing frames, 418,359 save bytes read) | history 25 to 45 s, median 28 s, no drop, 0 log cuts (stage 9: the same). With a 64 KiB log: 26 cuts, minimum 12.9 s. Under the tap storm (rewind_all fuzz): PASS, re-run frames identical to the first pass |
| `test_fe_rewind.c` "savekeep_*" (9 cases) | PASS: 15-170 writes refused per case, releases inside a save moved back / forward, the save's hash unchanged on every tick without a played frame |
| host tests | 30 of 30 |
| PPSSPP (MIPS dynarec), Emerald, `tools/rewind_ppsspp_save.sh s11a` | PASS: 145 save-writing frames gone back over, 2,454 re-run frames, unlogged=0, file on disk = save in memory at the end |
| PPSSPP, Driver 2 in the player's state, `tools/rewind_player.sh s11a d2` | PASS, B late and B at boot (1,382 frames shown backwards each, never denied) |

The checker (rewind_check.py) changed meaning with the rule: the save field
of a forward frame is no longer compared with the reference's (it may be
newer); the rest of the frame still is, until the game READS its save with
a newer save in place: from that frame the run is its own reference
(`own_timeline_from_save_read`; Driver 2's frame 3, FE). Forward frames
compared fell by 2.4 % against stage 10 for that reason.

### 17.5 Not done, not verified

- **Nothing of this has run on a PSP.** The hardware lines are in
  `builds/rewind-out/psp/README.md`, stage 11.
- Reach across saves is one Pokemon save (17.2). The undo log is the fix.
- N = 60 is a margin, not a measured bound (17.1 rule 5).
- "One game per chip type": EEPROM 512 vs 8K and flash 64K vs 128K are not
  told apart in my table; flash 128K is Emerald only.
- `tools/rewind_offcheck.sh` (objects of a REWIND-off build identical to
  before) was NOT run; by reading, the new macros expand to the old
  expressions without REWIND.
- With REWIND built in and the toggle off, a save read costs one load and
  a branch more (`gba_rewind_rlog == NULL`). Not measured; save reads are
  not on any hot path.
- The EEPROM path of the read log (one entry per byte, further bits from
  the entry before the position) is exercised by the EEPROM games of the
  corpus only as far as the mash script reads them.
- A kill DURING the game's own save is the exposure the normal flush has
  always had; the rewind adds no write, so it adds none.
- tools/rewind_campaign.sh, rewind_savesweep.sh, rewind_library.sh still
  pass `--rewind-stop-at-save` in places (accepted and ignored) and were
  not re-run.

### 17.6 Handoff

Twins in `~/rewind-agent`: `h3` = commit 4d8df8b. Work area `s11`
(`run_all.sh` = the whole chain of 17.4, about 50 minutes; `ppsave.sh`,
`player.sh`). PSP: `builds/rewind-out/psp/s11a` (harness, gbadhoc.prx
a08d71bf) and `s11a-play` (EBOOT e3bda827). Items 2 (the direct-sound fix
of 2363e97 proved right or replaced), 3 (the timing-only class, the
production check) and 4 of the coordinator's brief are NOT started.

## 18. The direct-sound fix (2363e97), proved; and the x86 palette stub (a9aaf01)

Fourth engineer, 2026-10-11. Work area `~/rewind-agent/s12` (dumps, the mGBA
reference, `sweep.sh`, `st.sh`, the two MIPS twins in `dr/`).

### 18.1 What was wrong, exactly, and since when

`sound_timer` writes output samples while the resampling phase is below 1.0
and adds `step = FIFO rate / output rate` per sample. It then kept only the
fraction of the phase. With `step <= 1` that is exact. With `step > 1` the
integer part is the output samples the channel has ALREADY covered; dropping
it made every FIFO sample write one output sample.

**It is not an upstream bug and not years old.** Upstream gpSP mixes at
65,536 Hz, where no game's step exceeds 1. This project made the rate a
parameter (636a795, 2026-07-21) and the frontend selects 32,768 Hz (db57575,
2026-08-02; in every tag from v3.0.0 on). From then on a FIFO clocked above
32,768 Hz was wrong. My predecessor's "half speed" is the right arithmetic
for a 63 kHz FIFO but not what one hears; measured:

- **Castlevania: Harmony of Dissonance** (FIFO about 40.2 kHz, step 1.225):
  the music played 18.5 % slow and flat (long-term spectrum = the fixed
  build's with every frequency x 0.815), and because the channel wrote
  faster than the mixer read, its position lapped the 1-second ring and
  later sound was ADDED onto sound not yet played.
- **Golden Sun: The Lost Age** (63,072 Hz, step 1.925): the same, at 0.52
  speed, lapping about once a second; the result is a smear with no clean
  pitch relation left (best fit x 1.04, poor).

### 18.2 Against references (60 s from power-on, no input, music playing)

| | Golden Sun 2 | Harmony of Dissonance |
|---|---|---|
| sample correlation with THIS emulator at 65,536 Hz (upstream's path, bit-identical with and without the fix), before / after | 0.007 / 0.998 | 0.012 / 0.992 |
| spectrogram similarity with **mGBA** (built in WSL, real BIOS, best lag), before / after / 65,536 Hz path | 0.598 / 0.997 / 0.997 | 0.419 / 0.969 / 0.969 |
| pitch factor against mGBA, after | 1.000 | 1.000 |

Penny Racers plays no direct sound in that minute: no evidence either way
from this fixture (its audio is bit-identical before and after for 4,000
frames of the mash script too; the fuzz failure of 16.6 came later in play).

### 18.3 Which games change at all

`s12/sweep.sh`: every corpus and library ROM, 4,000 frames of the mash
script, audio before (twin f3), after (h3), and after at 65,536 Hz. **67 of
70 are bit-identical.** The three that change (corpus and library copies
alike), correlation with the 65,536 Hz output before -> after: Golden Sun
2 0.041 -> 0.980; Iridion II 0.984 -> 0.999; Harmony of Dissonance 0.738
-> 0.774 (under button mashing the reference itself is a poor yardstick
there: the PSG noise channel is generated per output sample; the no-input
run above is the clean measurement). LOTR: The Third Age and Shrek did not
change in this window.

### 18.4 The dynarec corpus oracle (MIPS emitter under qemu, 32,768 Hz)

Two twins built from this tree with and without the one-line change
(`dr_host_sndnew` / `sndold`), the corpus script, 6,000 frames, all 36
ROMs (Payback's pair finished late and is included). Registers, PC, IWRAM, EWRAM, I/O, palette, OAM, VRAM, the frame
buffer and the sample COUNT are identical in every frame of every game.
The audio hash differs in exactly three: Harmony (from frame 202), Golden
Sun 2 (from 5), Iridion II (from 177). So the change has no effect on the
guest or on timing, on the PSP's own emitter.

### 18.5 Save states

- The format version is NOT bumped and need not be.
- A state saved by the fixed build loads in an older build, EXCEPT when it
  was saved in one of the affected games at a moment the phase was 1.0 or
  more: the old loader refuses it (`fifo-frac > 0xFFFFFF`). In Golden
  Sun 2 that is often (by the arithmetic; I did not count). Unaffected
  games: never.
- An OLD state in the NEW build, affected game: it loaded, and the channel
  kept the runaway ring position it was saved with, so the music was wrong
  for good (correlation with never-loaded play 0.11 over 16 s). **Fixed in
  this commit**: a plain state load puts a channel that is more than 4,096
  ring entries from the mixer back beside it (never the case in a state of
  an unaffected game or of the fixed build; a rewind's own restore is
  exempt). After it: correlation 1.000 (one sample of offset).
- **The plain state-load sound loss in Golden Sun 2 is cured**: fixed build,
  its own state: bit-identical to never-saved play from 25 ms after the
  load. Before: different for 10.8 s.

### 18.6 The PSP

Same C file, no assembly involved; 18.4 is the PSP's emitter. PPSSPP,
Golden Sun 2, 2,400 frames, PSP builds without (s9-save) and with the fix
(s11a): guest state identical in every frame; `core_prof` mean 9,307 ->
9,232 us (the fixed loop writes fewer samples; not slower). **NOT
confirmed: the audio OUTPUT on a PSP build.** I could not get the harness
to hash audio (`audio_oracle = 1` left `n=0`), so the audible proof is the
desktop and qemu builds of the same code. One listen to Golden Sun 2's
title on hardware closes it.

### 18.7 Verdict

**Ship in the release candidate**, independently of rewind. Minimal patch,
sound.c only: (1) in `sound_timer`, `ds->fifo_fractional = fifo_fractional -
0x1000000u;` in place of `fp8_24_fractional_part(fifo_fractional)`; (2) in
`sound_check_savestate`, drop the `index > 0xFFFFFF` bound on `fifo-frac`;
(3) in `sound_read_savestate`, the far-channel resync for old states (the
REWIND guard around it disappears in a tree without REWIND). Release note:
states saved in Golden Sun 2 / Harmony / Iridion II by the new build may be
refused by 3.5.0 and older.

### 18.8 a9aaf01 (the x86 palette store stub)

Desktop only: the core change is five lines of `x86/x86_stub.S`, which no
PSP build links (the rest of the commit is test instruments in the SDL twin
and under REWIND). Baselines: the video golden
(`tools/e2e/goldens/video_emerald.hashes`) is captured in PPSSPP and the
dynarec oracle runs the MIPS emitter under qemu, so neither can carry the
wrong green bit. Only x86-twin results could: the rewind tables before
a9aaf01 (which is how it was found). I did not audit other branches' x86
twin logs.

## 19. Stage 11b: after the first hardware run of stage 11

The coordinator ran `s11a` on a PSP-3000 (results `builds/fix351-out/
results/w11_*`): 8 of 8 exit 0, `unlogged=0` everywhere, the cold boot from
the save made after a rewind reaches the map, Driver 2 and Unbound hold
59.7 fps. Three things came out of it.

### 19.1 The game's own save flush was starved by a held rewind (fixed)

`w11_em_save`: `sram_flush ... ms=19817 blk_ms=9761 scan_ms=15170 thr=1`;
`w11_em_again`: 27 s. The writer thread runs below main; a rewind's ticks
never block; so a save made just before a hold stayed half written on the
stick for as long as the control was held. Nothing the rewind wrote: the
ordinary flush's exposure (milliseconds) stretched to the length of the
hold. Stage 9 had waited for the writer at engage; removing its machinery
removed that too. My omission.

As built (`rw_flush_help`, fe_host.c):
- first tick of a press: if a save is not yet on the stick, main asks for
  the scan and sleeps in 1 ms steps until it is, at most 250 ms. Logged
  every time it happens: `EVT rewind_flush_wait ms= on_disk=`.
- every further rewind tick while anything is still pending: one 1 ms
  sleep for the writer.
- The save in memory cannot change during a rewind, so there is no settle
  time to respect.
- `rewind_save ... flush_waits=N/MAXms flush_yields=N` at exit.

**The same starvation in ordinary play.** By reading, not measured:
`fe_host_sram_ff_nudge` made main sleep for the writer only in
fast-forward; at normal speed it "asks, does not wait", which is enough
only while main blocks at the vblank. A game that does not reach full
speed gave the writer nothing. That is pre-existing and independent of
rewind (the comment at the `sram_flush` line already records
`scan_ms=2405` under uncapped fast-forward before the nudge existed). Now:
a save still not on the stick 1.5 s after its last write is treated as
fast-forward treats it (`EVT sram_nudge_starved`, then 1 ms sleeps every
second frame, the existing cap). **This part belongs on the release
candidate too**; it is `fe_host.c` only, no REWIND involved. A link
session: the same code path, not looked at separately.

**The assertion:** `EVT sram_flush_slow ms= scan_ms= blk_ms=
why=writer_starved|stick` whenever a flush was in flight for more than
500 ms. Must not appear; `stick` (one 4 KiB write over 250 ms) is the
card, `writer_starved` is us.

### 19.2 A rewind is not begun inside a save in progress (new rule)

Found while fixing 19.1, and it is a hole in section 17 as I built it: the
save is permanent, so a rewind that BEGINS while the game is writing its
save and lands before the save started leaves the save half written, in
memory and on the stick, for good (a cartridge pulled during "Saving...").
Rule 5 of 17.1 protects where a rewind may stop, not where it may start.
Now: while the game wrote its save, or left the chip in the middle of
something, within the last `rewind_save_quiet` (60) frames, the control
does nothing (`EVT rewind_denied why=saving`, `denied_saving=` in
`rewind_save`). Reads do not count here, so a game that polls its chip
stays rewindable. The player's cost: the rewind does not respond for one
second after a save completes.

### 19.3 "1388 of 1388 backward pictures wrong" on hardware (the check was meaningless there; fixed)

Not a picture fault. The `F` line hashed the CORE's output buffer; with the
second core drawing, the core's buffer is not drawn at all, so every `R`
line (the picture actually shown, from the buffer the ME rendered) was
compared with a hash of nothing. PPSSPP has no ME, so it passed there. Now
the `F` line hashes the picture the rewind KEEPS for that frame (the one it
will show), from the same buffer `R` reads; a frame with no kept picture
on such a platform is written `--------` and rewind_check.py prints how
many it could not check instead of failing or passing them silently. NOT
verified: on hardware (the next run of the hash line is the test).

### 19.4 State and handoff (2026-10-11, the fourth engineer's context is nearly spent)

Tip 422cc18. Twin `h6` = that code. Verified on it or on `h5` (the same
minus the hash-line change): rewind_all 70 of 70 (h5), a 5-game subset
(h6), rewind_savetest 16 of 16 (h5), host tests 30 of 30 (before 422cc18;
it touches fe_host.c only), PPSSPP Emerald across a save on `s11b` PASS.
PSP: `builds/rewind-out/psp/s11b` (gbadhoc.prx 4f6240ec) and `s11b-play`
(EBOOT 0b742af0).

NOT verified:
- the flush wait and the per-tick yield have not run with a save actually
  pending (PPSSPP: `flush_waits=0`); hardware is the test (README, 11b);
- the starved nudge at normal speed (19.1) has not been provoked at all;
- the picture hash on hardware (19.3);
- the unit test does not model "no rewind begun mid-save" (it is in
  fe_host.c, not fe_rewind.c); rewind_all's tap storms now simply skip
  presses that fall inside a save.

Open, in the coordinator's order: (a) the read log's reach for a Pokemon
player (17.2: with two saves in the history the rewind stops at the END of
the older save's reads, i.e. it can cross the newer save only; an undo log
would roughly double that per 128 KiB) -- not quantified further;
(b) `tools/rewind_offcheck.sh` NOT run, required before the release
candidate; (c) item 3 (the timing-only class, MIPS, the production check)
and item 4: NOT started; (d) (closed: Payback's oracle pair finished, identical);
(e) PSP audio-output proof of 18 (the harness's audio hash stayed empty
with `audio_oracle = 1`; find the right key or listen).

## 20. Stage 12: the two save-test failures (item 1 of the fifth brief), and what "off" costs

Fifth engineer, 2026-10-11. No emulator code changed in this stage: the
code under test is 422cc18 (twin `h6`). Evidence, scripts and outputs:
`builds/rewind-out/psp/s12-item1/`, WSL `~/rewind-agent/s13/`.

**Verdict: neither failure is a defect in the rewind. Both were the test
asserting something the owner's rule does not promise, and both showed only
with `testdata/Pokemon Emerald All Shiny Fixed.sav` because of where that
save stands in the game, not because of 422cc18.** The old test, unchanged,
on `h6`: owner's save 14 of 14, the testdata save 12 of 14
(`st_td_before.out`). The revised test: 17 of 17 with each save, mutants
included (`st_td2.out`, `st_ow2.out`).

### 20.1 "again": the reference was the wrong game

The check compared a cold start from the file saved AFTER a rewind with a
cold start from the file of the run that never rewound. Those are two
different saves by the owner's rule: the never-rewound run saved at frame
2113; the rewound run went back to frame 577 (just after Continue) and
saved there, at frame 1021 of its own timeline, 18 seconds of game time
earlier. The testdata save stands in a town with two wandering characters
on screen; they stand elsewhere 18 seconds earlier, and the game saves
where they stand (`cold_again.png` / `cold_ref.png`: 542 pixels, two
sprites; the player, the map and everything else identical). With the
owner's save the two cold pictures happen to be equal (c8c95a84; I did not
look at why, presumably nobody walks on that screen), so the check passed
there for the wrong reason.

What the rule promises, and what is now asserted (`rewind_savetest.py`,
"again"; `tools/rewind_eqscript.py`):
- a run that NEVER rewinds, given frame for frame the buttons the rewound
  run ended up playing (first-pass frames 1..577, then the frames played
  after the landing), from the same starting file, **writes a file that is
  byte for byte the rewound run's**, and its machine, picture and sound at
  the last frame are the rewound run's. Testdata save: both 8202473e,
  frame 2541 equal. Owner's save: both f75b6acd, frame 2523 equal.
  (The script builder checks itself: every key the hash log records must be
  the key the script says for that frame, or the check fails.)
- the file at exit is the save in memory at the last frame;
- started cold, the game loads it (not the no-save picture) and does not
  write it.

The comparison with the first save's cold start is still printed, as
information.

Also fixed: the count of frames that "wrote" the second save included frame
578, where the log's save field steps from the first pass's value to the
permanent save's (87 real frames with the owner's save, not 88).

### 20.2 "window", frames 2173-2178: not a landing inside a save; the dynarec's cache-history timing (16.3), reaching the sound

Frame by frame (testdata save, save written in 2113..2205, snapshots every
6 frames at 2160, 2166, 2172, ...):
- frame 2166 reads 3,025 save bytes and writes none; 2167 reads 466 and
  writes; 2173 touches nothing; the game's save routine runs from RAM.
- a release with frames 2173..2178 on screen: the rewind had restored
  snapshot 2166 and re-run 2167..2172 (to have their pictures); that
  restore crosses changed RAM code, so the RAM translation cache is dropped
  (15.4; `code_kept=174/176`). The control goes up, the frame is inside the
  save, `now` moves FORWARD to 2205, the last write's frame (rule 5 of
  17.1), and the machine is run on from where it stands, 2173..2205.
  `E 2205 0 6deac0f6`: chip idle, save unchanged, `inside=0 chip_busy=0
  changed_by_rewind=0`. All six releases land there. No release of the 159
  lands in 2113..2204.
- what failed was the comparison of the frames played after the landing
  with the first pass: state and sound differ from 2206 on, the picture
  never. The first difference is at the re-run of 2167: 8 bytes of the
  458,768-byte image (offsets 0x328, 0x339, 0x34a, 0x3b1, 0x60e62, 0x61529,
  0x61540-1), counters that move by 13 in one byte each, no RAM.
- releases at other frames continue from a machine re-derived from an
  earlier snapshot without that cache drop in its last step, and are the
  first pass bit for bit.

**The proof that this is the cache and not the rewind:** the reference run
(never rewound) with `--flush-ram-at 2167 0` (drop the RAM translation
cache once, before frame 2167):
- differs from the plain reference in state from 2167 to the end of the
  run (1,467 frames) and in sound in 1,304 frames, picture in none;
- **is the rewound run**: the same machine state in all 39 re-run frames
  2167..2205, and the same picture and sound in all 74 frames played after
  the landing (state too, except 2229..2232 where the twin's script
  presses A and the rewound run's does not; they agree again from 2233).
- `--flush-ram-at 2165 0` does neither (control).
- `--rewind-full-flush` does not remove it, as expected: the first pass's
  cache is not a cold one either.

So a session that never rewinds gets the same machine, sample for sample,
whenever its RAM cache happens to be dropped at that frame (which ordinary
self-modifying code does all the time). It is the class of 16.3, with one
correction to 16.3: **the shift is not always invisible to the sound
hash**. Here the game starts its "saved" jingle on a PSG channel a few
cycles later; measured on the output stream (`acmp.py`): 5 to 7 of 549
samples per frame differ, by multiples of 1024, for about 35 frames, i.e.
the edges of a square wave fall one sample over. The stream is identical
before and after.

What is now asserted ("window"): every release passes rewind_check against
the first pass, or, where the last restore's re-run frames differ from the
first pass, against the never-rewound twin with the cache dropped at the
first such frame (`rewind_check.py --ref-after-landing`), picture and sound
to the sample; never inside the save. Result, testdata save: 159 releases,
92 moved (to 2112 or 2205), 0 inside, 6 played on as the twin (2173..2178,
cache dropped at 2167). Owner's save: 159 releases, 92 moved, 0 inside, 0
twins. The mutant "a rewind may stop anywhere" still fails 3 of 3.

Consequences:
- `rewind_all.sh` would report such a case as a failure (sound differs).
  It has not met one in 70 games; if it does, the twin is the test.
- The production safety net (16.3, item 3) cannot use the sound either
  unless the cut is made cache-independent.

### 20.3 A host test that 422cc18 broke (stale expectation, fixed)

`run_host_tests.py`: 29 of 30 on the tip; `sram_ff_nudge` asserted that at
normal speed a save pending for 200 frames never makes main sleep. 422cc18
changed exactly that on purpose (19.1: after 1.5 s main sleeps for the
writer, as in fast-forward), and its author ran the host tests before the
commit. The test now holds both sides of the rule: nothing but asking for
85 frames; from 1.5 s on main yields and the save reaches the file. 30 of
30. **Note for the integrator:** this rule changes NORMAL PLAY in every
build (no REWIND involved): a game below full speed with a save pending for
1.5 s gets 1 ms sleeps every second frame until the save is on the stick,
at most `SRAM_FF_NUDGE_CAP` of them.

### 20.4 "Off costs nothing": measured, NOT fixed (item 2, findings only)

Same tree, player build, REWIND=1 against REWIND unset
(`s11b-play` / `norw-play` manifests; `nm_rw_prev.txt`, `nm_norw.txt`):
image 13,580,656 against 13,416,816 = **+163,840 bytes**, `bootHeapPsp1000`
10,515,896 against 10,679,736 = the same 163,840. Where it is:

| section | bytes | what |
|---|---|---|
| .text | +73,868 | the rewind's code (fe_rewind.c, fe_rewind_pack.c, the rewind half of fe_host.c, libretro.c, main_psp.c), test instruments included |
| .rodata | +2,784 | |
| .data | +736 | |
| .bss | +81,920 | `rw_present` 76,800 (the picture shown while rewinding), `rw_test_aud` 4,800 (a test instrument), `rw_clk` 1,024, `rw` 568 |
| padding | +4,532 | section alignment |

The brief's list (a 128 KiB buffer, two picture buffers, a BIOS copy) is
not the rewind's: `buf` (backup_save), `pat` (run_testpat) and `bios`
(log_bios_evt) are in both builds; only their compiler suffixes differ.
So: about 82 KiB can move into the rewind's own buffer (taken when the
toggle is on: `rw_buf_need`, fe_host.c), and about 77 KiB is code, which
cannot be allocated at run time; the test instruments could be compiled out
of a player build (`GPSP_PERF_RIG`) to shrink it. "Equal to a no-rewind
build within a few hundred bytes" is not reachable without a loadable
module. NOT done: no code changed.

Instructions with the toggle off, by reading, NOT measured:
- MIPS emitted code: the page marks are emitted only when
  `gba_rewind_marks_wanted` (mips_emit.h, `rw_map_emit`); off, the stubs
  are a no-rewind build's.
- x86 (the desktop twin): the store stubs mark UNCONDITIONALLY
  (x86_stub.S `MARK_RW_EWRAM_*`). If the +0.06..0.19 % was counted on the
  x86 twin, this is probably most of it and it does not exist on the PSP.
- C store paths on every platform (`RW_MARK_EWRAM` / `RW_MARK_ALL`,
  gba_memory.h: DMA, the C write functions): unconditional, one or two
  stores each.
- per frame: the frontend's rewind hooks behind `rw_on`; the save hooks
  (`rewind_backup_data`: one load and a branch per save byte read).

The MIPS count (qemu oracle) per fixture is the measurement owed.

### 20.5 State and handoff (2026-10-11, fifth engineer)

Commits of this stage: tests and docs only. Verified on `h6`:
rewind_savetest 17 of 17 with both Emerald saves; unit test PASS; host
tests 30 of 30; `rewind_all.sh h6`: 70 of 70, timing-only frames 444, the same as `h5` (`s12-item1/table_h6.txt`).

NOT verified / not done:
- item 2 (20.4) is analysis only; item 3 not started.
- the twin proof (20.2) was made by hand for 2173 and is applied by the
  test to all six releases; it is x86. Whether the MIPS dynarec moves the
  cut at the same place is not known (a PPSSPP run of the window test is
  owed); the mechanism (a RAM cache drop moves a few cycles) is the same
  code path on MIPS.
- nothing new ran on a PSP. No new PSP build: the binaries of 19.4
  (`s11b` gbadhoc.prx 4f6240ec, `s11b-play` EBOOT 0b742af0) are this code.

## 21. (not in this branch)

Section 21, the sixth engineer's handoff ("off costs nothing" and the first
look at review finding 1), exists only in the WIP commit 2e1e8ee on
`claude/rewind`.  Sections 22-24 refer to it.

## 22. Stage 14: the history ending in the middle of a rewind (review finding 1, FIXED)

Seventh engineer, 2026-10-11.  Branch `claude/rewind-savefix`, made from
6535270 (the merge of the playtest candidate), NOT from the unverified WIP
commit 2e1e8ee, which stays where it is on `claude/rewind` and is not built
on.  Section 21's quarantine is not in this branch.  Evidence: WSL
`~/rewind-agent/s15/` (`before_k1.out`, `after_k2.out`,
`after_k2_nobudget.out`, `after_k3*.out`, `sl_pp.sh`, `pp/`), twins `k1`
(db910e4: 6535270 plus the test instrument only), `k2` (b2b3563), `k3`
(the tip).

### 22.1 The defect, reproduced

While a rewind is on screen the machine is not at the frame shown: it is
re-running OLD frames, with the save write-protected, and may stand in the
middle of an old save sequence.  `fe_host_rewind_suspend` (a sleep: the
power callback takes the volatile pool) let play go on "from where the
machine IS", a frame `snap_now()` / `noland()` never saw, with the save
writable again; `fe_host_rewind_tick` returned PLAY from the same kind of
frame when a link session began mid-rewind.  The game then finished the tail
of the OLD save sequence over the newer save.

`tools/rewind_sleeptest.py` (new; the desktop instrument
`--rewind-sleep-tick N` = one suspend, pool overwritten, resume, at the Nth
tick of a rewind).  Emerald, the `testdata` save (a copy), in-game saves in
the START menu, then the control held back across them into the FIRST save,
one run per tick.

- **One save cannot show it** (section 21's test): the replayed tail writes
  the bytes that are there.
- **Two saves cannot show it in Pokemon either.**  The game keeps two
  copies and writes them in turn: save 2 does not touch the 14 sectors
  save 1 wrote, so the replayed tail of save 1 again rewrites identical
  bytes.  Run on the unfixed code with two saves: 191 sleeps, the machine
  inside save 1's write sequence in 91 of them, 0 changed files.
- **Three saves show it.**  Save 3 goes over save 1's sectors.

Every tick 3600..3900 of the hold (the machine at frames 2030..2334; save 1
was written in 2113..2205, save 3 is the last):

| | 6535270 + the instrument (`k1`) | fixed, b2b3563 (`k2`) |
|---|---|---|
| sleeps | 301 | 301 |
| machine inside save 1's write sequence | 91 | 91 |
| **save file damaged** | **89**: a mixture, 1 to 13 sectors of save 1 in a file of save 3, the more the earlier in the sequence the machine stood | **0** |
| landed before the history went | 0 | 301 |
| the game saved again after the wake, byte for byte as it does when the control is simply let go at that tick | 38 | 112 |
| file = save 3 | 174 | 189 |

A cold start loads a save from each of the 13 different mixtures (the
picture is not the no-save one); WHICH save the game takes from such a file
was not looked at.

What the test asserts, and why: by the owner's rule (17) the cartridge
after any rewind is the LAST save the game made, so the file at exit must
be save 3 byte for byte, with one other right outcome: **what letting go at
that same tick leaves** (a run with `rewind N` and no sleep, compared byte
for byte).  That differs from save 3 only where a release lands just before
a save the player had already confirmed: the game plays into it again and
writes it, whole, from where it now is, which by the rule is a new save
(the "again" case of `rewind_savetest.py`), not a mixture.  So: for the
save, a sleep in the middle of a rewind is a release.

In the product's configuration (128 KiB read log) the history ends in the
tail of the SECOND-newest Pokemon save (17.2), whose sectors the newest
save did not touch: on a PSP today this defect cannot produce a changed
file in Pokemon.  It can in any game that writes its one save in place and
saves twice within the history (most of the library); no ROM test here
covers such a game.  That says which games are exposed; it is not a reason
to ship without the fix.

### 22.2 The fix as built

The rule: **play may only resume from a frame the no-land rule has vetted;
whoever is about to end the history, or to do anything else to the machine,
lands the rewind first.**

1. `fe_host_rewind_land(budget_us)` (fe_host.c): the control is let go and
   the rewind's own ticks run until one says PLAY.  Nothing new decides
   where: it is `snap_now()` / `release_work()`, the path every release
   takes.  `fe_host_rewind_busy()`: a tick has answered something other
   than PLAY and none has answered PLAY since.
2. Where it is called (psp/main_psp.c `psp_rewind_land`):
   - **the sleep handshake's park point**, before `g_pwr_parked = 1`, with
     what is left of the power callback's 250 ms wait less 60 ms for the
     engine to go idle (the callback's clock is `g_pwr_park_t0`);
   - `state_save_slot` and `psp_state_load` (every caller);
   - the loop on which the in-game menu is open or the pause overlay is
     about to open: everything they can do then finds the machine landed;
   - the Mystery Gift shortcut; and `gba_cable_start` itself (fe_host.c).
3. `fe_host_rewind_tick_step`: a rewind that is refused from now on (a
   session began) lands through its normal ticks instead of returning PLAY
   from the old frame.
4. **The power switch waits while it matters** (`psp_rewind_pwr`):
   `fe_host_rewind_unvetted()` = a rewind is in progress and the MACHINE
   stands on a frame play may not continue from (`fe_rewind_mach_noland`,
   the same `noland()` asked of `mach`).  While that is so the PSP holds
   `scePowerLock` (`pwr_hold("rewind")`, as for every other window that
   cannot park); the request is delivered when the rewind has moved on or
   landed.  Outside that state nothing is held.  Cost: with the control
   held at a history start that lies inside an old save's tail, the switch
   does nothing until the control is let go (45 s in a PPSSPP run whose
   script held that long).
5. **The last resort** (the landing did not fit and the lock did not
   defer): `fe_host_rewind_suspend` asks the same rule of the machine's own
   frame.  Passed: play goes on from there (what every build did; the
   player loses the fraction of a second between the machine and the
   screen).  Not passed: **the game is reset before its next frame**
   (`rw_reset_if_owed`; `EVT rewind_unvetted_reset`; the PSP says "Slept
   during a rewind: game restarted from its save").  It starts from the
   save, which no re-run frame touched: a cartridge switched off and on.
   The brief's fallback ("restore the machine to the newest frame") is not
   available: stepping back through the ring consumes it (`step_back`, "the
   snapshots stepped over are the abandoned future"), and the present is
   only reachable by re-running every frame gone back over.  Where the
   reset has to happen: the PSP's loop runs frames between the teardown and
   the actual suspend (the callback's last 80 ms) and again after the
   resume, before the overlay; the check is therefore at the top of every
   normal frame (`rw_latch_input`), not only in `fe_host_rewind_resume`.
6. Why not the quarantine of 21.2 (write-protect kept on after the wake):
   I did not reproduce or explain its three anomalous runs (F = 2119, 2167,
   2173), so it is not revived.  A guess, NOT checked: with writes refused
   and reads live, a game that erases a flash sector and polls it sees its
   own refused write.

### 22.3 Tests and results

| test | result |
|---|---|
| `rewind_sleeptest.py`, `k1` against `k2`, ticks 3600..3900, every tick | the table of 22.1: 89 damaged -> 0 |
| the same with `--rewind-sleep-budget -1` (no landing at all: only the last resort), ticks 3700..3830 | 131 sleeps, the machine inside save 1's sequence in 91: 122 resets, 9 went on from a vetted frame; 0 damaged |
| `test_fe_rewind.c`, new checks in the nine `savekeep_*` cases and `stride2_save_paced` | PASS: after every tick `fe_rewind_mach_noland()` is true only for a machine inside an overtaken save, is true for every such frame the machine is left on (15 to 6,908 ticks per case), never true when the tick said PLAY |
| PPSSPP (MIPS dynarec; the PSP's main loop and `power_cb` through the sleep simulator), `s15/sl_pp.sh`, harness of the tip; three saves, the sleep fired on a loop with the machine inside an old save (`sleep_sim_rewind_tick = -5`) | `sl_a1`, lock honoured: `deferred_ms=4493`, `rewind_land ok=1`, save 3 intact, rewind_check PASS.  `sl_a2`, `sleep_sim_nohold = 1`: the landing did not fit (`ok=0 ticks=6 us=179529`: a PPSSPP on a loaded host), `vetted=0`, `rewind_unvetted_reset`, save 3 intact, exit 0.  `sl_a3`, plus `rewind_park_land = 0`: reset, save 3 intact, exit 0.  (rewind_check fails after a reset by construction: the frames after it are another game's.) |
| host tests | 30 of 30 |

NOT covered: a session start in the middle of a rewind (the menu lands
first, `gba_cable_start` lands again, the tick lands a third time: all by
reading, no test drives it); the Mystery Gift shortcut; `scePowerLock`
actually deferring a real switch during a rewind (hardware, 23.3); the
budgeted landing on a PSP (how often about 190 ms is enough there).

### 22.4 Audit: every path that ends or suspends the history, or acts on the machine, while a rewind is in progress

"Vetted" = the frame play resumes from passed `noland()`, so the save chip
is idle whenever the save changed after it.

| path | before | now | verdict |
|---|---|---|---|
| sleep, loop parks (`power_cb` -> `me_standby_down` -> `psp_rewind_pool_lost` -> `fe_host_rewind_suspend`) | resumed from `mach`, unvetted | lock while unvetted; land at the park point; else vetted `mach`, else reset | FIXED; desktop + PPSSPP |
| sleep, loop cannot park, rewind in the pool | history kept across the suspend | section 23 | FIXED (23), PPSSPP |
| sleep with the rewind on the heap (no pool) | nothing suspended; the rewind goes on after the wake | lands at the park point all the same | safe before and now |
| engine grows past the pool boundary (`psp_rewind_pool_check`, inside `me_rend_init`) | suspend, unvetted, never resumed | suspend + `fe_host_rewind_abandon`: vetted `mach` or reset (no frame can be run from there) | FIXED by reading; cannot happen today (the engine ends well below the boundary) |
| link session start (menu -> `ui_net_action` -> `gba_cable_start`; the session's state is the machine as it stands, for BOTH consoles) | `tick` returned PLAY from `mach`; the old frame became the session's state | the menu lands; `gba_cable_start` lands; `tick` lands | FIXED by reading, not driven by a test |
| wireless / Mystery Gift start (menu or shortcut) | as above | the menu or the shortcut lands | FIXED by reading |
| state SAVE (shortcut, menu, harness) | `fe_host_state_save` REFUSED it (`reason=rewinding`): safe, the player got "Save failed" | lands, then saves the frame on screen | safe before; better now |
| state LOAD | machine replaced; `rw_barrier(STATE)`: safe (what a state holds is the state's business) | lands first, then the same | safe |
| cheat switched in the menu | the menu could open mid-rewind; the rest of the landing then re-ran logged frames under another cheat list, i.e. not the frames the log vetted | the menu lands first; the change is a barrier after the next played frame | FIXED by reading |
| in-game menu, pause overlay (Pause shortcut) | opened over a rewind in progress | land first | FIXED by reading |
| quit to the game list, exit, HOME | nothing resumes; the save in memory is the game's last (no re-run frame writes it) and that is what is flushed | unchanged | safe |
| Share game data | reads the save in memory / on the stick: the game's last | unchanged (reached through the menu: landed) | safe |
| a poke, a cheat-list change, link-port use, the read log overflowing (`rw_after_frame` -> `rw_barrier`) | only after a normally played frame, i.e. after PLAY | unchanged | safe |
| a normal frame run without PLAY (`fe_rewind_frame`'s anomaly path) | would reset the history at `mach + 1` | unchanged; no caller does it (both PSP call sites and the desktop's ask the tick first) | safe by construction, not guarded |
| the guest-fault reset (`BADJUMP_SAFE`) during a re-run frame | machine reset mid-landing; the landing then re-runs logged inputs on a booting game | unchanged | safe for the save (a reset machine is not inside a save); the game is not the one on screen |
| fast-forward pressed | the rewind stays at display speed (main loop) | unchanged | safe |
| video preset shortcut | scale / filter only | unchanged | safe |

### 22.5 Found on the way (not fixed; for the owner or the coordinator)

- **A new save made after rewinding past TWO Pokemon saves may not be the
  one the game loads next.**  My reading of the files, not a traced fact:
  the game in memory holds the save counter of the frame landed on, the
  cartridge is two saves ahead, so its next save goes to the copy it thinks
  is the older one with a counter lower than the other copy's.  Seen as
  the whole-file 02d3db2b outcomes of the first probe runs (save 1 redone
  over save 3's sectors = the file as it was after save 2).  Nothing is
  mixed.  Not reachable with the product's 128 KiB read log (one Pokemon
  save of reach); it follows from rule 7 of 17.1 in a game that keeps
  generations, and is a reason not to enlarge the read log without thought.
- The pause overlay (`ui_wake_menu`, the Pause shortcut) is a blocking loop
  with no park point and the engine up: 23.2.

## 23. A sleep that tears nothing down (review finding 2, FIXED), and what cannot park

### 23.1 The defect and the fix

`power_cb` waits 250 ms for the main loop to park.  When it does not, the
branch that gives the volatile pool back is skipped if the rewind is a
tenant (`!me_rend_active() && !psp_rewind_pool_tenant()`), and the
`park_timeout` branch tears nothing down either.  `g_pwr_slept` stays 0, so
neither `psp_rewind_pool_lost` nor `psp_rewind_pool_back` runs: the rewind
keeps its reference image, ring, read log, picture windows and the core's
bookkeeping (`rewind_mem buf=180232@pool mem=1576088@pool`) in memory the
OS owns while the machine is asleep, and used them after the wake as if
nothing could have touched them.  The same holds for `me_busy` and for
`standby = 0`.

Fix (psp/main_psp.c): RESUME_COMPLETE with `g_pwr_slept == 0` while the
rewind is a tenant sets `g_rw_pool_suspect`.  The main loop, at its top and
again before the next rewind tick (a blocking loop returns in the middle of
an iteration), calls `psp_rewind_pool_restart`: `fe_host_rewind_suspend` +
`fe_host_rewind_resume` in the same block, which copies the reference from
the machine as it is and empties the ring.  The ring then says "Woke from
sleep here".  A rewind that was in progress cannot be vetted from that
memory (`fe_host_rewind_distrust`): the game is reset (22.2 item 5).

Test (PPSSPP, harness of the tip; new harness keys `sleep_sim_nopark`: the
loop stands still without parking; `sleep_sim_trash_pool`: the rewind's
3,132,672 bytes of the pool are overwritten with 0xA5 while "asleep", which
is what the OS is entitled to do; `rewind_pool_restart = 0`: the mutant):

| run | result |
|---|---|
| `sl_b1` (fix) | `power teardown_skipped reason=park_timeout`, `resume_complete slept=0`, `rewind_pool_restart why=sleep_without_teardown on=1 pool=1`; three rewinds afterwards, the last one held into `rewind_limit frame=5204 why=sleep`; rewind_check PASS (201 pictures backwards, 468 re-run frames, 0 wrong); exit 0 |
| `sl_b2` (mutant: no restart) | the first rewind after the wake restores the overwritten history: `rewind_verify_fail words=86016`, `guest_fault pc=a5a5a5a5 action=reset`, rewind_check FAIL |

The mutant is this defect made certain (on a PSP the pool may or may not
survive); it is not a measurement of how often a real sleep damages it.

### 23.2 Which loops cannot park

The park point is one place: the top of the main loop (`g_pwr_park_req`).

- **The in-game menu parks.**  It is driven by the main loop: one
  `ui_frame` per iteration and `continue`, back to the park point.  The
  other reviewer was right.
- **Every long main-thread job is under `pwr_hold`** (boot, session start
  and end, state save / load, the Save States page's reads, the wireless
  and Mystery Gift bring-up, Share's network, the wake rebuild, the
  teardown): the switch is deferred, not timed out.
- **`ui_wake_menu` cannot park**: the wake / pause overlay is a modal loop
  (`while (g_running)` in ui_psp.c) that never returns to the park point.
  After a real sleep that is harmless (the engine is down, the rewind
  suspended, `power_cb` takes its "renderer off" branch).  Opened with the
  **Pause shortcut (SELECT + TRIANGLE by default)** in a running game it is
  not: the engine is up and the rewind is in the pool, a sleep from there
  is `park_timeout`.  This is the one in-game loop found that genuinely
  cannot park.
- `ui_notice` and the ROM browser's loops are blocking too, but no game is
  running there (the rewind is not a tenant).

**Not the rewind's, and not fixed here:** with the Pause overlay up the
ENGINE keeps its buffers in the pool across that sleep exactly as the
rewind did, with or without REWIND in the build (`me_rend_active()` alone
sends `power_cb` to `park_timeout`).  The comment above `me_standby_up`
records what that did on a PSP-3000 for the overlay after a sleep ("it
restarted to the XMB").  The same shape, one shortcut away, is still there.
A park point inside `ui_wake_menu` would close it for both tenants; not
attempted (the teardown snapshots the picture the overlay is showing).

### 23.3 Hardware steps for the owner

Player build `builds/rewind-out/psp/s14-play`.  Use a save you can lose (a
copy of a `.sav`, or a fresh game); Rewind ON in Settings.  With the
harness build (`s14-harness`) the same steps leave the lines quoted in
`log/frontend.log`.

A. **Sleep in the middle of a rewind (finding 1).**
   1. Pokemon Emerald (or any GBA game): save in the game, play 10 s, save
      again, play 10 s, save a third time.  Wait for the save to finish,
      then two more seconds.
   2. Hold the rewind button.  The game runs backwards through the third
      save ("Saving..." is on screen backwards).  WHILE that is on screen
      and the button is still held, slide the power switch.
   3. Expected: the console does NOT sleep at once; it sleeps when the
      picture has left the save, or when the button is let go.  (Slide the
      switch at any other moment of a rewind: it sleeps at once.)
      Harness: `pwr_hold why=rewind ms=...`, then `power suspending`,
      `rewind_land_psp why=sleep ok=1`.
   4. Wake, Continue.  Expected: the game goes on from the picture that was
      on screen, give or take a fraction of a second; the rewind ring, held
      again, says "Woke from sleep here" at its start.  NOT expected, and
      to be reported with the log if seen: the game restarting at its title
      with the message "Slept during a rewind: game restarted from its
      save" (harness: `rewind_suspend_unlanded ... vetted=0`,
      `rewind_unvetted_reset`).  That is the safe last resort; seeing it
      means the switch was not deferred.
   5. Quit to the game list, start the game, Continue: it loads the THIRD
      save (the place and play time of the last save made).
   6. Repeat step 2 five times at different moments of the rewind,
      including with the ring at "Start of history" and the button still
      held (there the switch may do nothing until the button is let go:
      known, 22.2 item 4).

B. **Sleep at the Pause overlay (finding 2).**
   1. In a GBA game with Rewind ON, play 20 s, press SELECT + TRIANGLE (the
      pause overlay), slide the power switch, wait 10 s, wake.
   2. Choose Continue, then hold the rewind button.  Expected: it rewinds
      normally and stops at "Woke from sleep here"; no garbage picture, no
      reset.  Harness: `power teardown_skipped reason=park_timeout`,
      `power resume_complete slept=0`, `rewind_pool_restart
      why=sleep_without_teardown on=1 pool=1`.
   3. If the console instead returns to the XMB or shows a black screen on
      the wake: that is 23.2's last paragraph (the engine, not the rewind).
      Repeat with Rewind OFF: if it still happens it is that, and it is in
      the candidate as installed.

C. **Menu and quick save during a rewind.**  While holding rewind, press
   the menu shortcut, and separately the quick-save shortcut.  Expected:
   the picture settles within a moment (the rewind lands), the menu opens /
   "State saved" appears; loading that state gives the picture that was on
   screen.  (Before: "Save failed".)

## 24. The three things the review had not read; the WIP commit; state and handoff

### 24.1 Read, by the seventh engineer (2026-10-11)

**The HUD and cadence code** (psp/main_psp.c: `psp_rewind_hud_step`,
`psp_rewind_hud`, `psp_rwc_fits`, `psp_rwc_choose`, `psp_rewind_tick_run`).
Nothing unsafe found.  Checked: every division is guarded (`avail0`, the
cadence's `p` is at least 1 before it divides, `t_ticks >= 48`,
`g_rwc.ticks` tested); the readout's 64-byte text cannot overflow (at most
about 51 characters, and each `snprintf` is given what is left); the anchor
cannot fall below `now`; `g_rw_show` is cleared wherever the pool goes
(`psp_rewind_pool_lost`, `psp_rewind_pool_restart`) before
`psp_rewind_present` can use it.  One fact that matters elsewhere: a tick
may be given up to five display periods (about 81 ms) to work in, so the
loop can be that late in seeing a park request; the park point's landing
budget is computed from the callback's own clock for that reason.

**The in-game menu in the middle of a rewind.**  Before: it opened over a
rewind in progress.  A quick save was refused (`fe_host_state_save`,
`reason=rewinding`: safe).  A session start was NOT: `gba_cable_start`
serializes "the running game at this frame boundary" as the session's
state for both consoles, i.e. the old frame the machine stood on, and a
cheat switched there made the rest of the landing re-run logged frames
under another cheat list.  Now the menu lands first (22.2, 22.4).

**The SP-relative block-store trampoline's mark** (mips/mips_emit.h,
"Special trampoline for SP-relative ldm/stm").  It covers descending
stores.  In both callers (`arm_block_memory`, `thumb_block_memory`) the
address handed to the trampoline is always the LOWEST address of the
transfer, whatever the direction: `arm_block_memory_offset_down_a` /
`_down_b` and `thumb_block_address_preadjust_down` / `_push_lr` subtract
the whole length first, and the words are then written upward from it
(`offset += 4`).  The transfer is at most 16 words (ARM) or 9 (Thumb
push with LR): 64 bytes above the masked address, inside the page marked
and the next one (1 KiB pages; the map has a spill byte after page 255).
By reading; `tools/rewind_marks.sh` was not run.

### 24.2 The WIP commit 2e1e8ee ("off costs nothing"): opinion

It should wait; none of it belongs in this weekend's beta.  What it
changes is the per-frame path of every game, rewind on or off (the three
core callbacks swapped through `rw_hooks()`, `fe_host_run_frame` split in
two, the autopilot's rewind step turned into a pseudo-button) and, more to
the point, WHERE THE WRITTEN-PAGE MARKS ARE MADE: the DMA loops no longer
mark per word, `rw_dma_mark()` marks a transfer's pages once beforehand,
and on MIPS only when marks are wanted.  The marks are the contract the
fast restore stands on, a missed one is silent state corruption after a
rewind, and a player's build runs without `rewind_verify`.  Its evidence
is thin exactly there: `rewind_all.sh h7` did finish, 70 of 70
(`~/rewind-agent/all_h7.out`), but that is the x86 twin, whose store stubs
mark unconditionally; nothing ran on the MIPS emitter (no PPSSPP run, no
corpus oracle `rwoff`, no `rewind_offcheck.sh`, no unit or host tests), and
its own stated goal is not reached (`dma_transfer` is 48 bytes longer, so
the pinned tail still moves; data placement is untouched).  The one piece
with a real player benefit, the 82 KiB of static buffers moved into the
rewind's own allocation, changes what the pool has to hold (it kept 13 KiB
to spare) and so needs the 1000's memory checked on hardware.  Finish it
after the beta, with the MIPS-side proof first.  The save quarantine in
that commit is superseded by section 22 and can be dropped.

### 24.3 State

Branch `claude/rewind-savefix` (not pushed, not merged anywhere):

| commit | what |
|---|---|
| db910e4 | the test and its desktop instrument (FAILS on this code: `--expect-damage`) |
| b2b3563 | land before the history ends (finding 1) |
| 2d9645a | PSP: the power switch waits while the machine stands inside an old save |
| ba6f41d | PSP: a sleep that tore nothing down restarts the history (finding 2); harness instruments |
| ac16639 | "in progress" = no tick has said PLAY yet; the reset is said on screen |
| ec5c44c | a state save after a landing is not refused |
| (this) | docs 22-24 |

Builds of ec5c44c, clean rebuilds (`workingTreeClean` is false in the
manifests only because of the untracked `psp/gpsp_adhoc.prx` and this
document):

| | file | md5 |
|---|---|---|
| `builds/rewind-out/psp/s14-play` (player; `GBA_LINK=1 REWIND=1 tools/build.sh release`) | EBOOT.PBP | 1e26a0bec76732a5571e9bf090d60546 |
| | gbadhoc_me.prx | 0f2840f85c04f6da4d0bc01110008318 |
| | gbadhoc_home.prx | d3434f4fd02260583c2e97ce7fe757e6 |
| `builds/rewind-out/psp/s14-harness` (`GBA_LINK=1 REWIND=1 PSP_EXTRA="BUILD_PRX=1" tools/build.sh harness`) | gbadhoc.prx | e6a852502c79838e867e53045895e4c0 |
| | EBOOT.PBP | 1863cf962fe810967c64fa90f33d324d |
| | gbadhoc_me.prx | 2f252eeb3c48c26e9016ed5a4f336d65 |
| | gbadhoc_home.prx | d3434f4fd02260583c2e97ce7fe757e6 |

Player image 13,597,040 bytes, boot heap on a PSP-1000 10,499,512: the
same as the candidate's `s3` to the byte count (the additions fit the
section alignment).

### 22.6 / 24.3a Verification before the merge of the candidate (twins `k3` = ac16639, `k4` = ec5c44c; evidence copied to `builds/rewind-out/psp/s14/`)

| test | result |
|---|---|
| `tools/rewind_all.sh k3` | **70 of 70**: 657,607 pictures backwards, 1,820,277 forward frames, 710,935 re-run frames, 0 wrong; timing-only 444 (as `h6`).  **Said plainly: the first pass was 60 of 70** (`table_k3_first.txt`).  Ten library rows failed in the nine minutes 16:17-16:26, runs that did not complete (exit -11) and one with wrong frames (Emerald, 2,946 pictures); all ten are ROMs read in place from the Windows disk, and in those same minutes a PSP build in docker died with "No space left on device".  Re-run alone (`RW_ONLY`, 21 rows), all pass.  I take it for the machine, not the code; that is an inference, the failed runs' work folders were not kept |
| `rewind_savetest.py --mutants`, testdata save | `k3` and `k4`: 17 of 17 |
| `rewind_sleeptest.py` on `k3`, every tick 3690..3840 | 151 sleeps, 91 inside save 1's sequence, all landed, 0 damaged |
| ... `--rewind-sleep-budget -1`, every tick 3700..3830 on `k2` / every 4th on `k3` | 131 sleeps: 122 resets, 9 on from a vetted frame, 0 damaged / 33: 31 resets, 0 damaged |
| ... the whole hold, every 24th tick (1..5353) on `k3` | 224 sleeps, all landed, 0 damaged |
| `test_fe_rewind.c` (ASan + UBSan) | PASS, 50 cases; built `-DMUT_NOSNAP`: 33 failures, as before |
| `tools/run_host_tests.py` at ec5c44c | 30 of 30 |
| PPSSPP, `s14-harness` of ec5c44c: `f_ctl` (three saves, hold, no sleep) | PASS, 1,379 pictures backwards, 1,481 re-run frames, 0 wrong; save 3 intact |
| ... `f_a1` / `f_a2` / `f_a3` (22.3), `f_b1` / `f_b2` (23.1) | as in those sections, repeated on this build: lock deferred 4,505 ms then landed, PASS; landing did not fit -> reset, save intact; no landing -> reset, save intact; restart, PASS; mutant fails |
| ... `f_ss`: a state save asked for on every loop of a held control | 41 saves, all `rc=0`, none refused.  NOT a test of "save after a real landing": each save's landing put the press back to its start, so nothing was ever shown backwards.  That path is by reading (ec5c44c) and hardware step C |
| ... `tools/rewind_player.sh s14-harness d2` (the player's configuration) | PASS, B late and B at boot (849 / 850 frames shown backwards, never denied) |


### 24.4 Open risks, in order

1. **Nothing of sections 22-23 has run on a PSP.**  In particular
   `scePowerLock` deferring a real switch in the middle of a rewind, and
   whether the park point's landing fits its budget there (23.3 A).  If the
   lock does not defer, the outcome is the safe one (a reset), but it would
   be a visible one.
2. The last resort resets the game.  It is the only state available that
   is consistent with the save without the history, and it is reached only
   when the lock did not defer AND the landing did not fit AND the machine
   stood inside an old save.  The owner may prefer otherwise; the
   alternative that keeps the session is an overlay for the save chip
   during the wake (writes kept apart, reads served from them), which
   needs up to 128 KiB that the 1000 does not have at that moment.
3. The Pause overlay and a sleep (23.2): the engine's side of it is not
   fixed and predates the rewind.
4. The second-newest-copy effect (22.5) if the read log is ever enlarged.
5. No ROM test of a game that writes its one save in place and saves twice
   within the history (22.1).  The mechanism is the same; the unit test's
   machine is such a game.
6. `tools/rewind_offcheck.sh` and `tools/rewind_marks.sh` were not run
   (this branch adds nothing to a REWIND-off build outside `#ifdef REWIND`
   and `GPSP_PERF_RIG`, by reading).
7. The whole-hold sleep test samples every 24th tick outside the window
   around save 1 (a run per tick over 5,367 ticks is about six hours on
   the loaded machine).

## 25. After the merge of the candidate (686235c), and the readout's pace tag

2026-10-11, same engineer.  `claude/rc-4.0.0-beta-x` at 686235c (review
fixes in fe_host.c's save paths, Share, link) was merged INTO
`claude/rewind-savefix` (3db2b71; nothing the other way).  The merge was
automatic; the candidate's hunks touch none of the rewind, park or state
code (read hunk by hunk in fe_host.c and main_psp.c).  Then fb7e715, the
owner's polish.  **The builds of 24.3 are superseded by these.**

Builds of fb7e715, clean rebuilds, `REWIND=1 GBA_LINK=1`
(`workingTreeClean` false only for the untracked `psp/gpsp_adhoc.prx`):

| | file | md5 |
|---|---|---|
| `builds/rewind-out/psp/s14-play` (player) | EBOOT.PBP | 0a9eb4dacc97dca7328d21e7fdb1aa5a |
| | gbadhoc_me.prx | 0f2840f85c04f6da4d0bc01110008318 |
| | gbadhoc_home.prx | d3434f4fd02260583c2e97ce7fe757e6 |
| `builds/rewind-out/psp/s14-harness` (`BUILD_PRX=1`) | gbadhoc.prx | 071900a578d8587d73e17c0f0ebffbb6 |
| | EBOOT.PBP | 224a1f11d6148503735feed75471aa39 |
| | gbadhoc_me.prx | 2f252eeb3c48c26e9016ed5a4f336d65 |
| | gbadhoc_home.prx | d3434f4fd02260583c2e97ce7fe757e6 |

Image 13,597,040 bytes, boot heap on a PSP-1000 10,499,512 (player).

Verification on the merged tree (twin `k5` = 3db2b71; the PPSSPP set on the
harness of 3db2b71, then the control and the lock run again on fb7e715's):

| test | result |
|---|---|
| `tools/rewind_all.sh k5` | 70 of 70 at the first pass: 657,607 pictures backwards, 1,820,277 forward frames, 710,935 re-run frames, 0 wrong; timing-only 444.  (So the ten failures of 24.3a's first pass did not come back on a quiet machine.) |
| `rewind_savetest.py --mutants` | 17 of 17 |
| `rewind_sleeptest.py`, every tick 3690..3840 | 151 sleeps, 91 inside save 1's sequence, all landed, 0 damaged |
| ... `--rewind-sleep-budget -1`, every 4th tick 3701..3829 | 33 sleeps: 31 resets, 2 on from a vetted frame, 0 damaged |
| `test_fe_rewind.c` | PASS |
| `tools/run_host_tests.py` | 30 of 30 at fb7e715.  (The first run on the merged tree was 29 of 30: `fe_ini_bounds`, test_ini_doc.c:132, which then passed 3 of 3 alone.  It uses fixed file names in /tmp and other test runs were going on on the machine; nothing in this branch touches it.) |
| PPSSPP `m_ctl` / `m_a1` / `m_a2` / `m_b1` / `m_b2` | PASS, save 3 intact / lock deferred 4,494 ms, landed, PASS / reset, save intact / restart, PASS / mutant fails (`guest_fault pc=a5a5a5a5`) |
| PPSSPP `tools/rewind_player.sh` (Driver 2, the player's configuration) | PASS, B late and B at boot (849 / 850 shown backwards, never denied) |
| PPSSPP on fb7e715's harness: control and lock run | PASS, save 3 intact / lock deferred 4,499 ms, `rewind_land_psp why=sleep ok=1`, PASS, save 3 intact |

### 25.1 The pace tag (fb7e715)

`"   x2"` / `"   x4"` is no longer appended to the rewind readout: with a
game on the margin between two paces it came and went, and because the
line is centred the whole readout moved sideways each time.  Nothing about
the pace itself changed (`g_rwh.sp` still drives the ring's tail).  No test
or screenshot comparison expected the tag (the `rewind_gedump` screenshots
are not compared with anything).

**The readout can still move sideways, for another reason, NOT fixed:**
the font is proportional and its digits are not of one width (Inter at
15 px in `psp/font_ui.h`: "1" advances 6 px, "7" 8, "4" 10, the others 9).
Every time a digit of either time changes, the line's width changes by up
to 4 px and the centred line shifts by up to 2 px: about once a second at
x1, more often at x4, and twice as often with the "left" time counting as
well.  Left-anchoring the line, or drawing digits on a fixed 10 px
advance, would stop it.

## 26. Rewind length: Short, Medium, Long (rc2, owner decision 2026-10-12)

The Settings row "Rewind history  N MB" (1..8, default 2) is now **"Rewind
length"** with three values. No engine behaviour changed, and the default
is still 2 MB: only which sizes the player can name.

| Value | History | CONFIG.INI |
|---|---|---|
| Short | 1 MB | `rewind_mb = 1` |
| **Medium** (default) | 2 MB | `rewind_mb = 2` |
| Long | 4 MB | `rewind_mb = 4` |

- LEFT / RIGHT (or X) cycles Short, Medium, Long. With the cursor on the row
  the footer says what the value is: "Short: 1 MB of history. The lightest
  on memory." / "Medium: 2 MB of history." / "Long: 4 MB. Needs the extra
  memory of a PSP-2000, 3000 or Go." Nothing is refused on a PSP-1000: the
  ring asks the heap for half a megabyte less until it is granted (13.x),
  as before.
- **The key is still `rewind_mb`, an integer.** On load any other number
  becomes the nearest named length not above it: 3 is Medium, 5 to 8 are
  Long, anything below 1 is Short. No key, an empty value or one that is
  not a number is Medium (`pcfg_rewind_mb_snap`, config_psp.h).
- **A card written by rc1 or an s3 build already says `rewind_mb = 2`**
  (the key is written at every save, whatever its value): Medium, which is
  also what a fresh install gets. A card on which a player had chosen 3, or
  5 to 8, comes down to Medium or Long.
- **The harness key `rewind_mib` is untouched** (raw megabytes, 1..8, read
  after the player's setting and overriding it). The lab configuration
  (`rewind = 1` without `rewind_product`) and `rw_config`'s fallback for a
  caller that names no size (the desktop twin) stay at 2, the same size.
- The ring readout needs no change: it works from the ring's granted bytes
  and the frames actually held (`rewind_avail`), never from a default size.

### 26.1 Where each length's memory comes from, and when

From the code (`rw_mem_take`, `psp_rewind_pool_offer`, the block before
`fe_host_boot` in main_psp.c):

- **When.** At game start, BEFORE the cartridge is loaded and so before the
  ROM cache / residency allocation: `psp_rewind_read_ini()`, then
  `fe_host_rewind_reserve()`, then `fe_host_boot()`.
- **Only if** Rewind is ON at that moment and the game is a GBA game. With
  Rewind off, or in a GB / GBC game, nothing is reserved at all
  (`fe_host_rewind_prepare(NULL)`); the length setting then costs nothing.
- **The fixed parts** (pictures and sound 1,046,784; reference image
  458,768; logs 119,688) are carved from the volatile pool above the
  engine's megabyte, for every length.
- **The ring**: in this build the pool has 1,376,256 bytes left for it
  (`EVT rewind_ring asked= granted= (pool + heap)`, rc2 harness in PPSSPP;
  the 1,507,328 of 10.5's table is an earlier stage's figure).

| Length | ring in the pool | ring on the heap |
|---|---|---|
| Short, 1 MB | 1,048,576 | **0** |
| Medium, 2 MB (default) | 1,376,256 | 720,896 (0.69 MiB) |
| Long, 4 MB | 1,376,256 | 2,818,048 (2.69 MiB) |

- **Short takes nothing from the heap.** A PSP-1000 loses no ROM-resident
  block to it. Medium (the default) costs the 1000's ROM cache about 0.69 MiB
  (22 of its 32 KiB pages), as rc1 did; Long about 2.69 MiB, or less if the
  heap refuses (then the history is shorter than asked: `EVT rewind_ring
  asked= granted=`).
- **During a link session the memory is still held.** The rewind is refused
  for the session (`fe_host_rewind_blocked`: "link") and records nothing,
  but neither the pool pieces nor the heap piece is given back; a cable
  session's second machine is loaded beside them (13.5 said so for the
  heap share at 2 MB and it is still true). With Short that heap piece is
  zero.
- **Switching Rewind off in Settings** takes effect at the next game start;
  the running game keeps what it reserved until it is left
  (`PSP_REWIND_GAME_OVER`).
- If the volatile pool cannot be had at all (`rewind_pool unavailable`),
  everything, the ring included, goes to the heap for any length.

### 26.2 Verification (rc2, e28c157)

| test | result |
|---|---|
| `tools/run_host_tests.py` | 30 passed, 0 failed, 0 skipped |
| `test_ctl_map -DREWIND`: `test_rewind_length` (mapping -20..100, cycle, names, 14 files loaded and saved back, the six row values round-tripped) | OK |
| `tools/rewind_all.sh rc2`, the default (2 MiB) | 70 of 70 at the first pass; timing-only 444 |
| `rewind_savetest.py --mutants` | K=5, 17 of 17 |
| `rewind_all.sh`, **Long** (`--rewind-ring 4194304`), Emerald x3, Driver 2, Payback x2, Penny Racers x2 | 8 of 8 |
| `rewind_all.sh`, **Short** (`--rewind-ring 1048576`), all 70 | **66 of 70**, 0 wrong frames anywhere; the four are two games, in the corpus and in the library (below) |
| PPSSPP `rewind_player.sh` (Driver 2), default config | PASS, 941 / 905 shown backwards, ring 2,097,152 (pool 1,376,256 + heap 720,896) |
| PPSSPP, CONFIG.INI `rewind_mb` absent / 1 / 2 / 3 / 4 / 8 | ring asked and granted 2 / 1 / 2 / 2 / 4 / 4 MiB |
| PPSSPP, harness `rewind_mib = 6` over `rewind_mb = 1` | 6,291,456 asked and granted: the raw key still overrides |
| PPSSPP, `rewind = 0`, `rewind_mb = 4` | no `rewind_pool` / `rewind_ring` line: nothing reserved |
| PPSSPP `ui_controls_demo = 7`, dark and light | the row in each value with its footer; the card then says `rewind_mb = 2` |

**Short, the two games.** Both are the same on the rc1 twin (7d46dad) at
1 MiB, and both are the same on a quiet machine, so neither comes from this
change: 1 MB has been a selectable size since the setting existed.

- **Payback** (`test`): "only 234 frames were run again (need 300)". The
  scripted rewind asks for 300 frames and 1 MiB holds 234 of this game
  (61 barriers in the run). Every frame shown and re-run is right. A limit
  of the size, reported by the test as a failure.
- **Penny Racers** (`fuzz`, seed 1): `rewind_check` says "play resumed at
  4078 but 4065 was on screen" and the same for 4073, with
  `moved_off_a_save=4`. No picture, forward frame, re-run frame or save is
  wrong, and the save after the storm equals the clean run's. It passes at
  2 and at 4 MiB. Not explained here: it is for the rewind engineer to say
  whether the checker's rule or the landing is at fault when the history is
  this short. **Open.**
