# Low latency (claude/low-latency, prototype)

Issue #11: GBA "feels sluggish", GB/GBC feel better. Status: PPSSPP-verified for
the CPU/GB path; the ME arms are **unmeasured on hardware** until the A/B in
section 5 runs.

## 1. Where the latency comes from (code-derived; PPSSPP confirms the CPU path)

Unit: vblanks from the loop's input poll (just after vblank V(k)) to the vblank
whose NEXTFRAME flip latches the frame emulated in that loop iteration. The
`EVT lat` histogram measures this directly (`hist=` bins 0..7).

Shipping ME mode (4.1.1), frame N emulated in interval k:

| step | when |
|---|---|
| render of N posted to the ME (vispost) | vcount 160 of k, done ~3 ms later |
| render **retired** | vcount 160 of **k+1**, only by the next post (`me_rend_frame` step 1) |
| blitted (`me_rend_present`) | loop top of **k+2** |
| `vid_swap()`: SetFrameBuf **NEXTFRAME** | right after the wait for V(k+3) |
| latched | **V(k+4)**: NEXTFRAME issued just after a vblank waits for the next one (pspdisplay.h: NEXTFRAME = NEXTVSYNC) |

So **latch - poll = 4** for ME, **2** for the CPU renderer and GB/GBC (frame
drawn at the end of `retro_run`, then the same post-vblank swap), and **1** is
the floor for any emulator that runs a frame in a burst and then shows it.
That 2-vblank gap between ME and GB matches the reporter's "GB feels more
responsive".

Constant terms on top of that, the same on every path:

* the game: most read KEYINPUT in their VBlank handler (line 160 of frame N) and
  draw the result in frame N+1. This is +1 frame, and a real GBA has it too.
* scan-out: 0.8 ms of vblank, then ~58 us per PSP line (mid-screen ~8.7 ms),
  plus LCD response.
* **pad sample age.** The input is sampled once per vblank
  (`sceCtrlSetSamplingCycle(0)`). PPSSPP samples at vblank END
  (Core/HW/Display.cpp `DisplayFireVblankEnd` -> `__CtrlVblank`), so a poll made
  as the wait returns reads a sample **15.9 ms old** (`age_us`). Whether real
  firmware does the same is unknown. If it does, that is one more whole frame.

Button to top-of-screen photon, PSP vblanks (V = 16.68 ms), ignoring the shared
game-logic frame and the LCD:

| path | today | tier 0 | tier 1 | tier 1 + fresh pad (if HW samples at vblank end) |
|---|---|---|---|---|
| GBA, ME renderer (default) | 4V + 0.8 ms = 67.5 ms | 2V = 34.2 ms | 1V = 17.5 ms | ~1.05V |
| GBA CPU renderer, GB/GBC | 2V = 34.2 ms | 1V = 17.5 ms | n/a | |

(Add the average 8.3 ms from the press to the next poll, which every 60 Hz
poller has, and possibly +1V if the hardware pad sample is stale.)

## 2. The prototype: one binary, every arm (harness keys)

| key | default | what | cost |
|---|---|---|---|
| `flip_early` | 1 | tier 0a, every path: in the vblank wait, once the GE is idle (`vid_gu_busy()`, a peek), call `vid_swap()` BEFORE the vblank, so NEXTFRAME latches on the vblank being waited for. If the frame is late (no wait), nothing changes. | none: the wait was idle, and the GE is polled, never waited on |
| `me_early_retire` | 1 | tier 0b, ME: `me_rend_retire_if_idle()` at the loop top, so the render finished in k is blitted at the top of k+1 instead of k+2 | none: an idle check |
| `me_lowlat` | 0 | tier 1, ME: skip the loop-top present; in the wait, poll the ME (150 us sleeps); when the render retires with `blit_ema + me_lowlat_margin_us` (500) left before the vblank, blit + rewind HUD + OSD, poll the GE, flip before the vblank | no extra CPU work (the same blit, moved into idle time); see section 3 |
| `me_lowlat_recover` | 60 | frames of "would have made it" (measured in tier 0) before tier 1 is entered again | |
| `input_fresh` | 0 | probe: if the pad sample predates this vblank by more than 2 ms, re-peek in 50 us steps, at most 1.5 ms | up to ~0.9 ms at frame start (PPSSPP: 0.88 ms) |
| `ctrl_cycle` | 0 | probe: `sceCtrlSetSamplingCycle(N)` timer sampling (5555..20000) | PPSSPP age 2.7 ms at 5555 |

`me_sameframe` is unchanged (`me_lowlat` forces it off). Its measured ~1.5
ms/frame cost had one cause: it blits after the vblank and `vid_swap()`'s
`vid_gu_flush()` then waits out the whole GE blit on the CPU. Even so, it still
latched one vblank later than it could (post-vblank NEXTFRAME). Tier 1 blits
before the vblank, in time the wait was spending idle.

FF, rewind and link: tiers 0b and 1 are off under any FF (`g_ff_mult`,
`g_ff_uncapped`) and during a rewind (`mer_rewind_busy()`). Tier 1 is also off
under the FF draw gate. Tier 0a applies everywhere the paced branch runs (1x,
3x, link pacing, rewind display). The uncapped branch still swaps right away.
Pacing (vcount target, audio nudge, peer match) is untouched. The flip moves,
the cadence does not.

## 3. Tier 1 policy and its risks

* **Hit:** latch - poll = 1. **Miss** (render or blit not done in time): nothing
  new latches that vblank (one duplicate, the same as any late frame today). The
  late frame is blitted at the next loop top and flipped early, so it is shown
  one vblank later than a hit. The mode drops to tier 0 at once.
* **Re-entry** drops exactly one frame: the one retired at the loop top of the
  switch iteration is superseded. Hysteresis (`me_lowlat_recover`) limits this to
  at most one hitch per second for a game hovering at the deadline. Games below
  60 fps never re-enter: there is no wait in which to measure, so they stay in
  tier 0, which is already their best case.
* The critical path is `max(retro_run end, vis + render) + blit + OSD < 16.68 ms`.
  The comment at `g_mer_sameframe` measured capture 7.0 + render 3.2 + blit 1.5
  ms. Heavy games end `retro_run` at ~13 ms, so the blit ends at ~14.5 ms.
  Margins are thin there; the A/B decides.
* Stage ownership is the existing `me_rend_retire_if_idle` protocol. The GE
  reads the stage just presented, and the next post writes the other one, a
  full frame later.
* Never a tear: every flip is NEXTFRAME after the GE is idle (or flushed). A
  late submit only moves the latch to the next vblank.

## 4. Considered and not built

| option | latency gain | PSP cost | verdict |
|---|---|---|---|
| Run-ahead (1 frame) | -1V (removes the game's frame) | a second `retro_run` (6-13 ms) + state save/load per frame | does not fit heavy games; rejected |
| Frame boundary at line 160 (VBlank first, then lines 0-159) | -1V | ME can no longer overlap VBlank emulation (+render on critical path); savestate/rewind/link frame semantics move | too invasive for its gain; revisit only for light games |
| Late input at the vcount-160 hook (refresh KEYINPUT before the VBlank IRQ) | about 7 ms of input age, but only with a sub-frame pad sample (ctrl_cycle) | ~10 us | worth a follow-up after the age measurement; must be off in link (lockstep input) and logged for rewind |
| RetroArch frame delay (start emulation late) | D ms | shrinks the budget by D | inferior to the late-input hook; rejected |
| Progressive (per-line) ME rendering | only when the render is the critical path | renderer contract rewrite | not needed unless `vis+render > retro_run end` on hardware |
| IMMEDIATE flip at vblank start | same as flip_early | tears if the thread is late by >0.8 ms | flip_early gets the same latch safely |
| Double instead of triple buffering | 0 once flips are early (no queue forms) | | keep triple |

## 5. Hardware A/B still needed (nothing above is measured on a PSP)

`lathw.py` (scratchpad tool, uses the bench's `psp_one.sh`, ms0:/GBALAB only),
PRX build `builds/low-latency-out/lat-3-prx`, arms A (`flip_early=0
me_early_retire=0`), B (defaults), C (`me_lowlat=1`), D (C + `input_fresh=1`),
fixtures em_battle, hns_heavy, ub_rival_medium, aw2_battle, kf_doom_ii,
d2_drive, nfsu2_race. Read per arm:

* `EVT lat hist=` mode: expected A 4, B 2, C mostly 1; `latched_next` vs `not`
  (latch semantics on the real firmware; `immed` must stay 0).
* `age_us`: ~0 means the firmware samples at vblank start; ~16 ms means the
  stale sample is real, and `input_fresh`/`ctrl_cycle` become product work.
* C: `hit / miss_me / miss_ge / to_h / to_b`, `lead_min_us`, `blit_ema_us`.
* perf_window fps/work: B and C must match A within noise (~1%).
* still needed after that: an end-to-end camera test (owner's latency ROM +
  high-speed camera) to tie vblanks to photons.
