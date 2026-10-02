# Fast-forward presets: draw only what the LCD can show (2026-10-02)

Branch `claude/ff-presets` (base `claude/candidate-3.1-all` 53bca1d).
Supersedes the Unlimited / Unlimited Smooth rows of [FF-PROFILES.md](FF-PROFILES.md);
3x is unchanged. The display-buffer and GE/ME rules of
[FF-ARTIFACT-FIX.md](FF-ARTIFACT-FIX.md) are untouched and still apply.

## The problem

Neither unlimited preset knew the LCD shows at most 60 frames a second.

| preset | before |
|---|---|
| Unlimited | ME on: every emulated frame was offered to the ME, which rendered whenever it was idle (about half the frames). ME off: core frameskip 1-in-2. |
| Unlimited Smooth | Waited for the ME on every frame, so emulation was capped at the ME's render rate (1:1). ME off: every frame rendered and blitted. |
| 3x | Three frames per paced iteration, the last one presented (~180 emulated / 60 drawn). |

Each frame offered to the ME also costs the main CPU while it is emulated:
the per-line register capture, the mid-frame log, and the palette/VRAM/OAM
write-backs at the post. So frames drawn beyond what the screen can show
cost emulation speed twice.

## The rule (owner's spec, kept deliberately simple)

| preset | emulation | drawn frames |
|---|---|---|
| Unlimited | flat out | at most **30** a second |
| Unlimited Smooth | flat out | at most **60** a second |
| 3x | unchanged | unchanged |

The OSD chip text (`>> Unlimited`, `>> Unlimited Smooth`, `>> 3x`) is unchanged.

### The draw gate (`psp/ff_gate.h`)

At the **start** of every emulated frame the main loop asks the gate whether
this frame will be drawn. Boundaries sit on a fixed grid one period apart
(33333 us / 16667 us). A frame is drawn when its predicted **end** (its start
plus the previous frame's duration) is at or past the next boundary; the
boundary then advances by exactly one period.

* Using the predicted end, not the start, lands each draw at or just after its
  boundary. Advancing the grid by one period, rather than "one period after the
  last draw", keeps the long-run rate at exactly 30 / 60 instead of drifting
  low by a fraction of a frame every period.
* A frame duration over one period (menu, state load, slow emulation) is not a
  prediction: it counts as zero.
* A draw that finds the gate more than a period behind re-anchors the grid on
  itself (`resync` in EVT ff_win): lost boundaries are given up, never repaid
  as a burst of back-to-back draws.
* When emulation is slower than the draw rate, every frame is drawn.
* Re-arming (FF engaged, or the preset changed while held) draws the next
  frame at once.

The gate is header-only and platform-free; `tools/test_ff_pipeline.c` drives
that exact code (fixed-cost frames give 300 / 600 draws in 10 s from 1.7 ms to
5 ms frames, draws land in `[boundary, boundary + one frame)`, slow emulation
draws every frame, a 2 s stall neither bursts nor is repaid).

### A frame the gate turns away gets nothing

`fe_host_skip_override(skip)` forces the next frame's frameskip decision:
GBA through `frontend_skip_override` in `libretro/libretro.c` (applied after
the core's own frameskip policy, `-1` = no override), GB/GBC through
`gb_skip_frame`. That is the existing gpSP frameskip contract, so a gated-out
frame is emulated in full but:

* `update_scanline` returns at its first line: no CPU render, no per-line ME
  capture, no `MFL_FRAME_START`, so the mid-frame log never starts
  (`me_capture_visible_end` then has nothing to merge);
* `mer_vispost_active()` is false (`skip_next_frame`): no vcount-160 post;
* the core hands the frontend `NULL`, so `plat_video_frame` posts nothing,
  blits nothing, and the loop does not swap.

On the ME path a gated-out frame still calls `me_rend_frame(0)`, which only
**looks**: it retires a finished render (budget 0, never waits) so the next
loop top can present it. VRAM dirty pages, `PAL_UPDATED` and `OAM_UPDATED`
simply accumulate across gated frames and are consumed by the next drawn
frame's capture and post, exactly as for any frameskipped frame
(`ml_frame_start` already handles "a frameskipped frame leaves the flags set").

With the gate off (FF not engaged, 3x, sessions, bench, harness `ff`) the
override is `-1` every frame and the frameskip option rules as before.

### A frame the gate draws takes the 1x path

* **Posted at vcount 160, with the mid-frame log** (`mer_vispost_active()` is
  true for the gated presets), i.e. the ME renderer contract of normal play
  (docs/ME-RENDERER-DIVERGENCE.md, docs/ME-MIDFRAME.md). Before this change
  the unlimited presets posted at frame end with the log zeroed, so their
  images had the post-VBlank-memory errors those docs describe. 3x still posts
  at frame end, unchanged.
* **Never dropped.** If the previous render is still running (it was posted a
  whole gate period earlier, >= 16.7 ms, so normally long done) the drawn
  frame waits for it, bounded by the existing 50 ms ceiling; a wedged ME takes
  the existing CPU fallback (`me_rend_teardown("ff_render_wedge")`).
* **Presented once.** The loop-top present blits a stage only when it is new,
  and the uncapped swap only happens when something was drawn
  (FF-ARTIFACT-FIX rule 1 ownership logic unchanged; rule 2's GE-read flush
  sits in `me_rend_fill_desc`, so it covers the vcount-160 post too).
* The heartbeat watchdog keeps its once-per-16.7 ms throttle under the gate.
* CPU renderer (ME off, or after a fallback): the core renders the drawn frame
  and `plat_video_frame` blits it.

The core's own frameskip option is now always `disabled` while FF is engaged
(the old 1-in-2 `fixed_interval` for Unlimited without the ME is gone: the
gate does better). Audio during FF is unchanged (muted unless `ff_audio`, and
the core produces every sample either way).

## Telemetry and harness keys

* `EVT ff_win` (telemetry builds only; compiled out of a player build): every
  10 s of wall time, `mode` (off / 3x / unl / smooth / harness), `me`,
  emulated fps, **drawn** fps (new images put in the back buffer: an ME stage
  presented for the first time, or a CPU frame the core really drew),
  `core_us` (mean `retro_run`), `gated`, `resync`, frames.
* Harness `ff_preset = 3x | unlimited | smooth` selects the user preset that a
  `simff = N` hold engages (from frame 300) without staging a config.ini; it is
  echoed as `EVT ff_preset set=...` or `bad=...`.

Both are in commit 5fe6282, *before* the gate, so the old/new comparison
below uses the same instrument on both sides.

## Validation (PPSSPP: CPU renderer only)

Harness builds of 53bca1d (base), 5fe6282 (old = base + telemetry) and the
gate; runs in `builds/ffpresets-out/ppsspp/` (rig129.sh, Xvfb :129). PPSSPP's
clock is the emulated PSP clock, so the fps below are PPSSPP's cost model:
good for old against new, not a hardware prediction.

### Emulation unchanged (shash frame hashes, `c=` excluded)

| run (FF off) | frames | base 53bca1d vs the gate |
|---|---|---|
| AW2 tour (aw2.st0 + aw2_psp_script) | 6061 | **identical** |
| H&S heavy battle (heart_soul_heavy.st0 + battle.txt) | 3200 | identical from frame 31; IWRAM differs on boot frames 11-30 only (the known boot noise) |
| Emerald walk (emerald_3000.st0 + em_ff.txt) | 5283 | IWRAM differs on 2569 frames (plus r/pc/io on one frame against the first base run); a second **base** run differs from the first base run in exactly the same way, and the gate against that second base run differs in IWRAM only: cold-boot IWRAM noise; every other field identical |

Run twice for the gate (d47a1df, and the tree committed as bf5d633): same
result both times.

Fast-forward does not change emulation either: AW2 with each preset held from
frame 300 (gated-out frames included) is identical, every field on every
frame, to the FF-off base run; old against new per preset is identical too.

Host: `python tools/run_host_tests.py` 30/30 (the `ff` suite now covers the
gate; `video_buffers` passes unchanged).

### The presets, old against new

Each preset held from frame 300 to the end of the script; EVT ff_win windows
after the first FF window, averaged. ME off (PPSSPP has none).

| fixture | preset | old emu / drawn / core_us | new emu / drawn / core_us |
|---|---|---|---|
| AW2 | 3x | 154.3 / 51.4 / 5290 | 154.8 / 51.6 / 5290 |
| AW2 | Unlimited | 180.4 / 90.2 / 3665 | **238.7 / 30.0** / 2290 |
| AW2 | Smooth | 135.5 / 135.5 / 5661 | **202.8 / 60.0** / 3058 |
| Emerald | 3x | 147.9 / 49.3 / 5694 | 147.9 / 49.3 / 5696 |
| Emerald | Unlimited | 170.8 / 85.4 / 3950 | **223.3 / 30.1** / 2579 |
| Emerald | Smooth | 129.5 / 129.5 / 5804 | **195.1 / 60.0** / 3230 |

Per window the gate drew 600/600 (Smooth) and 300-301 (Unlimited) frames in
10 s, `resync=0`. Unlimited emulates 31-32 % faster than before and Smooth
50 %; Smooth is no longer bound by rendering (old Smooth drew every frame).
3x is the same (PPSSPP's 3x is emulation-bound near 150, below its 180 pace).

A caution the measurement taught: with `gedump_loops = 50` on, Smooth read
55-57 drawn and Unlimited 29.2-29.4, with `resync` 2-15 per window. Each GE
dump is a ~390 KB Memory Stick write on the emulation thread; a stall longer
than a period makes the gate re-anchor and give up the missed boundaries (by
design: no catch-up burst). Without dumps `resync` is 0. On hardware the same
will happen around SRAM flushes or ROM page loads; `resync` says how often.

### Screen (GE dumps, CPU renderer)

`gedump_loops = 50` in the first FF runs: 15-48 dumps per gated run, 39-64
for 3x and old Smooth (old Unlimited's alternate-frame skip never drew on a
multiple of 50, so it has only the 6 pre-FF dumps). `gecheck.py` flags fully
black bands inside the picture and hard horizontal seams: the 3x runs flag
the same dumps old and new, and every flag in a gated run is AW2 campaign or
attract artwork (banners, newspaper panels, the tiled "2" intro) or the
pre-FF boot frame. On inspection none is a black band or a torn frame.

## The Media Engine path (cannot run in PPSSPP)

PPSSPP has no Media Engine, so the ME behaviour rests on the code and on the
host pipeline test, which runs the real `me_rend_frame()` / `me_rend_present()`
against a fake 12 ms ME:

1. **Gated-out frames do no ME work.** `skip_next_frame` makes
   `update_scanline` return before the capture block, so neither the line
   registers nor the mid-frame log is touched, and `mer_vispost_active()` is
   false. The end-of-frame call is `me_rend_frame(0)`: retire if idle, never
   wait, never post, never count a drop (test: a busy ME costs < 1 ms and no
   drop; an idle one is retired).
2. **Drawn frames are 1x frames.** Posted from the vcount-160 hook with the
   log, the same producer as normal play; `g_mer_posted` keeps the
   end-of-frame call from posting twice. Stage/capture ownership is unchanged:
   one producer per frame, loop-top presentation, the GE-read flush in
   `me_rend_fill_desc` before the ME may overwrite a stage.
3. **No starvation, no stall.** A drawn frame follows the previous post by at
   least one gate period (16.7 / 33.3 ms) and a render is ~3-10 ms, so the
   retire wait is normally zero; when it is not, the frame waits (bounded,
   50 ms) instead of dropping, and a wedged ME falls back to the CPU. Between
   drawn frames emulation never waits for the ME (old Smooth waited on every
   frame).
4. **Mirror state across gaps.** VRAM dirty pages, `PAL_UPDATED` and
   `OAM_UPDATED` accumulate over gated frames; the next drawn frame's line 0
   (`ml_frame_start`: "a frameskipped frame leaves the flags set") and its post
   consume them. The affine seed is reloaded at every VBlank, so it is right
   at the drawn frame's line 0 whatever was skipped.
5. **Watchdog.** The ME's idle loop beats the heartbeat; the host check stays
   throttled to once per 16.7 ms under the gate, as Unlimited had.
6. **Expected effect.** Unlimited: the ME renders 30/s instead of about half of
   all frames, and ~87 % of frames skip the capture. Smooth: 60/s instead of
   one per emulated frame, and emulation is decoupled from the render rate.
   Drawn FF frames now get the vcount-160 post, so the post-VBlank-memory
   errors of the old FF post (ME-MIDFRAME "Not changed") are gone for them.

What the PC cannot show: the real capture and post cost on the Allegrex, ME
render times under the new cadence, and how the panel looks.

## Hardware A/B (staged, not run)

`builds/ffpresets-out/hw/`, **staged, not run.** One rig app
(`GBADHOC-DRPROF`, "DRPROF RIG") driven by the existing hnsrig/freezerig
handoff loop; `ffrig.py` adds the fixtures and arms to drrig the way
`builds/hw-queue/queue_arms.py` does. Steps for the lead and the owner are in
`hw/OWNER-STEPS.txt`.

* Builds: harness64 (64 MiB layout), ME on, CATCH PRX 19ae57b6.
  `ffold` = 5fe6282 (EBOOT md5 bc8455520734dc0a07255e9b66c3c514),
  `ffnew` = bf5d633 (9998ef0442670ee72ce5692bc75ad627); relabelled in the
  stage to 18217970e92534b7666505497963bbad / 316d83bb71935b71ea13e618b3a0d556.
* Fixtures: A = AW2 tour, E = Emerald (owner's 3000 state) Petalburg walk.
* Arms `F-{A,E}-{3X,UNL,SMO}-{OLD,NEW}`: `ff_preset`, `simff = 100000`,
  `shash = 0`; 12 arms, ABBA = 24 runs, ~1.5-2 h.
* `ffrig.py verify` checks md5s, that every arm key renders, and that every
  rendered key is read by both builds' source. Three arms ran in PPSSPP from
  the stage's own files (`hw/dry/`): exit 0, `ff_preset set=`, windows in the
  right mode (F-A-UNL-NEW drew 44.99 in its one partial window, F-E-SMO-NEW
  59.99).
* `ffrig.py score --logs ...` gives per arm: emulated fps, drawn fps,
  `core_us`, ME on/off, and any `me_rend off` or non-zero exit.

**The A/B must confirm**, on AW2 and Emerald:

1. NEW Unlimited draws ~30 and NEW Smooth ~55-60, with `me=1` throughout.
2. NEW Unlimited emulated fps >= OLD; NEW Smooth well above OLD Smooth (which
   was capped at the ME render rate).
3. NEW `core_us` below OLD for both (gated frames carry no capture), and the
   `me_rend` census shows a small `wait_mean_us` (drawn frames rarely wait).
4. 3x NEW == OLD within run-to-run noise.
5. No `me_rend off` (ff_render_wedge / watchdog / late), and every exit 0.
6. The owner sees no black bars, tearing or stale frames the OLD arms lack.

## Builds

| artifact | path | EBOOT md5 |
|---|---|---|
| playable "GBAdhoc 3.1 RC-ALL+FF" (bf5d633, clean tree) | `builds/ffpresets-out/release/` | `4cd26bc20104b446b7dab637878dd6f7` |
| playable old presets "GBAdhoc 3.1 RC-ALL FF-OLD" (5fe6282) | `builds/ffpresets-out/hw-builds/old-release/` | `bde5e9524ad2493d08e6b9f40a4defc3` |
| harness64 old / new (the rig) | `builds/ffpresets-out/hw-builds/{old,new}-h64/` | above |

The release ships `gbadhoc_me.prx` 19ae57b6 (ME_CATCH, as `tools/build.sh
release` builds it). Release EBOOT sha256
`297b08b9e41a6b9fbc7abe823cd1be4a85d4f96dff74ad305791356ff419e08f`.
