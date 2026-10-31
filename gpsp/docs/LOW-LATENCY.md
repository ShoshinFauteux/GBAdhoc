# Low latency (claude/low-latency, prototype)

Issue #11: GBA "feels sluggish", GB/GBC feel better. Status: tier 0 and the pad
finding are **measured on a PSP-1000** (section 6). Tier 1 is not yet shown to
work on hardware for a light game (em_battle) after two bugs found there were
fixed. It rarely engages on heavy frames, and one run ended with the console
dropping off PSPLink at exit (cause unknown, section 6).

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
| `me_lowlat` | 0 (`ME_LOWLAT=1` build: 1) | tier 1, ME: skip the loop-top present; in the wait, poll the ME (150 us sleeps); when the render retires with `need` left before the vblank, blit + rewind HUD + OSD, poll the GE, flip before the vblank. 1 = enter on the natural audio-ring nudge, 2 = also take that nudge early to enter (section 7) | no extra CPU work (the same blit, moved into idle time); see sections 3 and 7 |
| `me_lowlat_recover` | 60 | minimum frames in tier 0 after an exit before tier 1 may be entered again | |
| `me_lowlat_margin_us` | 250 | safety margin on top of the measured blit deadline (section 7) | |
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

## 6. Hardware results (PSP-1000, PSPLink, 2026-10-20)

Arms A = `flip_early=0 me_early_retire=0` (shipping), B = defaults (tier 0),
D = `me_lowlat=1 input_fresh=1` (run on lat-4, where tier 1 never engaged, so
it isolates input_fresh), C = `me_lowlat=1` on lat-5 (both tier-1 fixes). The `hist` mean is latch - poll in
vblanks. Each fixture ran 1500 measured frames.

| fixture | A fps / work ms | B fps / work ms | A mean | B mean |
|---|---|---|---|---|
| em_battle | 59.74 / 5.75 | 59.77 / 5.73 | 4.01 | 2.03 |
| hns_heavy | 59.49 / 9.53 | 59.48 / 9.46 | 4.02 | 2.16 |
| ub_rival_medium | 59.76 / 13.17 | 59.76 / 13.18 | 4.02 | 2.34 |
| aw2_battle | 59.76 / 4.84 | 59.77 / 4.76 | 4.01 | 2.01 |
| d2_drive | 59.76 / 13.01 | 59.76 / 12.99 | 4.01 | 2.48 |
| nfsu2_race | 59.77 / 9.53 | 59.77 / 9.53 | 4.01 | 2.04 |
| kf_doom_ii (sub-60) | 18.72 / 53.4 | 18.68 / 53.5 | 6.95 | 6.49 |

* NEXTFRAME latches on the vblank after the submit: `latched_next` covers
  every flip, `immed` ~0. The model in section 1 holds on real firmware.
* Tier 0 costs nothing measurable and removes ~2 vblanks (~33 ms) at full
  speed. Its residual 3s and 4s (ub_rival, d2) are renders that finish after
  the next loop top (vis ~7.9 ms + render ~7 ms on the 1000).
* Sub-60 (Doom, ~3.2 vblanks a frame) gains little: no vblank wait happens, so
  flip_early never fires. Follow-up: submit the flip from the vcount-160 hook
  once the loop-top blit is done (OSD must move to the loop top for that).
* **The pad sample is stale on hardware too:** `age_us` ~15.9 ms at the poll
  (the firmware samples at vblank end, like PPSSPP). `input_fresh=1` brings it
  to 0.3-0.5 ms (waits ~0.74 ms; work +0.6-0.7 ms; fps unchanged on em/hns/ub).
  That is one more whole frame, available to every core including GB/GBC.
* Tier 1, first attempts: lat-3 presented the PREVIOUS render (fixed in
  55962bd); lat-4 never entered the mode (fixed in 64a9e58).
* Tier 1 on lat-5 (C):

  | fixture | fps / work ms | hist 0..7 | mean | hit | entries (to_h) |
  |---|---|---|---|---|---|
  | em_battle | 59.78 / 5.69 | 0,959,790,23,6,1,0,2 | 1.49 | 957 | 7 |
  | hns_heavy | 59.49 / 9.59 | 0,41,2521,239,56,15,4,11 | 2.15 | 38 | 5 |
  | nfsu2_race | no log: see below | | | | |

  No cost: fps and work match tier 0. The latch - poll = 1 floor is reached
  (72% of frames in em_battle's steady window). Every miss costs ~60 frames
  in tier 0 before re-entry; on hns_heavy ~8% of frames would miss, which
  resets the 60-in-a-row streak, so the mode rarely engages. Next: a ratio rule
  (e.g. >= 95% would-hit over the last 120) instead of a consecutive streak,
  and a measured blit deadline (`blit_ema` reads ~1.5 ms on the 1000).
* **Open risk:** the lat-5 tier-1 nfsu2_race run reached frame ~3296 of 3330
  at full speed with normal heartbeats, then the PSP-1000 dropped off
  PSPLink around exit; frontend.log was lost. lat-4 nfsu2 with the same key
  (tier 1 never engaging) exited cleanly. Not reproduced, not explained: it
  could be tier 1, the exit path, or PSPLink/USB. It must be repeated before
  tier 1 goes anywhere near a default.

## 7. Tier 1 policy, second version (claude/lowlat-t1)

The prototype (section 3) entered tier 1 by superseding the frame the loop
top had just retired, so every entry dropped a frame, and it needed 60
would-hit frames in a row. On hns_heavy (~8 % misses) that almost never
happened. The rework, in `main_psp.c` at "TIER 1 POLICY":

* **The picture moves by one vblank whenever the mode changes.** Leaving
  repeats a frame, and entering at an arbitrary frame skips one. The repeat
  on leaving can't be avoided: the frame that missed is already late.
  Entering is now **drop-free**.
* **Entry happens only in a wait that is already two vblanks long.** That
  wait is the audio-ring nudge (`want` 2). It fires about once every 288
  frames at 1x, because the PSP runs at 59.94 Hz and the GBA at 59.73, and in
  tier 0 it shows a frame twice anyway. The frame the loop top retired is
  flipped early and latches on the first vblank. This iteration's own
  render is then presented in the second interval and flipped before the
  second vblank: N-1, N, N+1, with nothing skipped and nothing repeated. The
  repeat the nudge would have made pays for the latency step. If the render
  or the GE is late, the mode is not entered, and the loop top shows the
  frame next iteration as tier 0 always does.
* **Nothing changes in the long run.** Each exit adds one repeat and each
  entry removes one, so over any stretch tier 1 shows exactly the frames
  tier 0 shows, give or take one repeat that moves in time. Emulation still
  starts on the same vblanks. Only the moment a finished picture is flipped
  changes, and the guest never sees it, so replays, vhash and link lockstep
  are unaffected. Link sessions are excluded anyway (`link_session_any`).
* **Eligibility.** Over the last 128 measured frames at most 6 may have
  missed (95.3 %), none of the last 32 may have missed, and at least
  `me_lowlat_recover` frames must have passed since the last exit. A frame
  "would have made it" if its render retired before the vblank after its own
  poll with `need` us to spare. In tier 0 this is measured in the wait. In
  tier 1 it is the hit or miss itself.
* **Exit** happens on the first miss, or on a loop top that finds an unshown
  frame (it is shown; never skipped).
* **`need`** is measured per console: the EMA of present-to-GE-idle (blit +
  OSD) + 4 mean deviations + one poll period (150 us) + `me_lowlat_margin_us`.
  It is clamped to 0.8..8 ms. A GE that finishes after the vblank pushes the
  EMA and the deviation up. It starts conservative (EMA 2 ms, deviation
  0.25 ms, so `need` is 3.4 ms) until real blits have been timed.
* **`me_lowlat 2`.** Waiting up to ~5 s for the next nudge after a miss caps
  how much of a marginal game can run in tier 1. Mode 2 takes the nudge as
  soon as the ring is within one frame of audio (`in_rate/60`) of the
  high-water mark, once eligible. That moves a nudge earlier and never adds
  one: every nudge holds back one frame of production, so their long-run
  rate is set by the clock mismatch alone. It happens at most once per
  exit, and the ring sits at most one frame (~17 ms) lower until the next
  nudge is due.

`EVT lat` gains `eng` (iterations in tier 1), `slot`/`slot_fail` (entry slots
tried and failed), `win=misses/n` and `clean`, `need_us`, `blit_dev_us`,
`lead1_min_us` (smallest lead of a tier-1 flip before its vblank), `vb`
(vblanks in the window) and `pull` (mode-2 early nudges). `to_h`/`to_b` count
entries and exits.

Host test: `tools/tests/test_lowlat_policy.py` (in the `ff` host suite)
compiles the production code into a model of the vblank, NEXTFRAME flip, GE,
ME and audio ring. It covers the low-latency globals, us_to_vblank,
me_rend_present, me_rend_retire_if_idle, lat_swap, the wait functions, and
main()'s loop-top, nudge and wait text. It runs eight load profiles in
tier 0, mode 1 and mode 2 and checks that:

* no frame is skipped or shown out of order;
* the poll vblanks are identical (mode 1);
* there are no extra repeats;
* the nudge count is unchanged and the audio is never starved (mode 2);
* the mode engages where there is room and not where there is none.

Five mutants must fail: prototype entry, no latch check, no deadline, no
exit, early nudge anywhere.

Model results (12000 frames, mean poll->latch in vblanks):

| profile | tier 0 | mode 1 (in tier 1) | mode 2 (in tier 1) |
|---|---|---|---|
| light (5.5 ms work) | 2.00 | 1.03 (97 %) | 1.01 (98 %) |
| medium, 2 % spikes | 2.00 | 1.77 (23 %) | 1.56 (44 %) |
| bursty phases | 2.00 | 1.76 (24 %) | 1.60 (40 %) |
| hns-like, 8 % spikes | 2.00 | 2.00 (0 %) | 1.94 (6 %) |
| heavy / ub-like / sub-60 | 2.00 / 2.00 / 3.40 | unchanged, never entered | unchanged |

GB/GBC and the CPU renderer already sit at the floor with tier 0: the
`hist` mode on gb_crystal is 1 (Lab runs, 2026-10-30). Below 60 fps (Doom,
~3.2-3.4) no vblank wait happens, so neither tier applies. The follow-up for
those games is still the vcount-160 flip (section 6).

### 7.1 Hardware results (Lab, PSP-3000 + PSP-1000, 2026-10-30/31)

Build llt1b (bde4a96, rc450 recipe, tier 1 off by default), arms by
`me_lowlat`. Mean poll->latch in vblanks / ms; "eng" = share of iterations
in tier 1 (mode 2). 3000 jobs j448482/jf05877/j414b08, 1000 jobs
j21c902/j539653/j0f2d78.

| fixture | tier 0 | mode 1 (3000) | mode 2 (3000) | mode 2 (1000) | eng (3000/1000) |
|---|---|---|---|---|---|
| own_fe8 | 2.00 / 33.4 | 1.21 / 20.1 | 1.06 / 17.7 | 1.18 / 19.6 | 94 / 83 % |
| aw2_battle | 2.00 / 33.4 | 1.40 / 23.3 | 1.26 / 21.0 | 1.27 / 21.1 | 74 / 73 % |
| em_battle | 2.00 / 33.4 | 1.51 / 25.2 | 1.28 / 21.4 | 1.35 / 22.5 | 72 / 66 % |
| hns_light | 2.02 / 33.7 | 1.41 / 23.4 | 1.42 / 23.8 | 1.54 / 25.6 | 59 / 48 % |
| nfsu2_race | 2.12 / 35.4 | 1.94 / 32.3 | 1.68 / 28.1 | 1.71 / 28.4 | 41 / 43 % |
| hns_heavy | 2.02 / 33.8 | never entered | 1.94 / 32.3 | 1.91 / 31.9 | 9 / 12 % |
| ub_rival_medium | 2.22 / 37.0 | never entered | never entered | never entered | 0 |
| kf_doom_ii | 2.98 / 49.7 | never entered | never entered | never entered | 0 |
| gb_crystal_walk | 1.00 / 16.7 | already 1 | already 1 | already 1 | n/a |

fps, work and repeats equal tier 0 within noise in every arm. Every exit was
a ME render that missed the deadline (3-8 per fixture). There were zero late
GE blits and zero failed entries. Measured blit ~1.5 ms, so need is
~2.0-2.4 ms; the smallest tier-1 lead was 269 us (1000) / 699 us (3000).
`me_lowlat_recover=0` changed nothing measurable, so it stays at 60.

* Exit cycles: 30/30 launch-play-exit per console (hns_heavy, em_battle,
  nfsu2_race x10, mode 2), all exit code 0 on the first attempt.
* Frame identity: vhash + mesum streams, tier 0 vs mode 2 on 5 fixtures with
  tier 1 engaged 44-73 %. Zero differing frames.
* Correctness gate: llt1b vs rc450 PASS (48/48, corpus 35/35).
* Full-suite A/B: llt1c (mode 2 baked, cd2af77) vs rc450. Work is +0.03 %
  (3000) / +0.05 % (1000) better overall, and fps is equal.
  * Mario Tennis +3.3 % work on both consoles. It is not tier 1: llt1b at
    me_lowlat=0 vs rc450 shows the same +3.3 %, and me_lowlat 0 vs 2 in one
    binary is identical. The cause is code layout: the frontend change moves
    ~3700 core .text symbols by +2956/+2976 bytes.
  * fe8, aw2 and firered come out 3-4 % better from the same shift.
