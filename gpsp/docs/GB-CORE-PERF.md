# GB core performance (TGB Dual on the PSP)

2026-10-28, branch claude/gbcore-perf (from claude/candidate-next a429765).
Goal: GB/GBC link trades at full speed on hardware (owner sees Crystal at
~50 fps while trading).  General levers only; hardware decides.

## 1. Where a trade frame goes

A link session runs TWO TGB machines per console (objcopy copies gbcore_* and
gbcoreb_*) in scanline interleave with an in-memory cable, plus input lockstep
over ad hoc.  From the hw2/M1 logs (core_prof, window-matched) and x86 perf:

| part | cost | notes |
|---|---|---|
| CPU interpreter, both machines | the bulk | per op: cpu_exec -> op_read -> jump table -> cpu_clock_advance -> cpu_timer_advance (a loop per instruction while TAC&4) |
| strict per-scanline alternation | ~2.9 ms/frame | I-cache: each copy's hot code is ~2x the 16 KiB cache; hw2 strict 12.8 ms vs batched 9.9 ms |
| LCD (drawn machine only) | ~2.4 ms | SOLO - SOLOH; the headless partner already skips LCD and APU |
| APU synthesis | small | drawn machine only |
| periodic sync hash | 2-4 ms spike every 240 frames | word FNV over ~160 KiB |
| net + frontend | ~0.6-1 ms | |

**Why the trade drops to ~50 fps:** the pair may batch (slot 0 leads up to
batch_lines=154) only while `cable_quiet` -- both ports' SC bit 7 clear plus 60
quiet frames.  In Pokemon's trade/battle screens the partner keeps its port
ARMED (external clock, SC=0x80) almost all the time, so the session runs strict
alternation for the whole trade.  linkplay x86, Gen 2 trade: only 25 % of
lines batched (Gen 1 16.5 %, battle 15 %).

## 2. Why the Lab fixture misses it

The Lab's gblink_gen1_trade / gblink_gen2_trade (59.8 fps, work 7.6 / 10.0 ms)
ran as ONE console with ONE machine: the staged task ini has `host=0 join=0`,
none of the fixture's `link.host_keys` (no gblink_*), and just the sav (+ st0)
-- a single-player Crystal/Red boot.  No second machine, no cable, no
alternation, no net.  It cannot see the trade cost.

**Replacement (built, one console, no radio):** the gb_dual bench
(GPSP_PERF_RIG) replays a linkplay input track for both machines:

```
rom = <crystal>.gbc            ; slot 0 (its sav = the host's sav)
gb_dual_rom = <crystal>.gbc    ; slot 1, headless
gb_dual_sav = slot1.sav        ; slot 1's battery
gb_dual_inputs = trade.inputs  ; linkplay inputs_out= track ("FRAME S0 S1" hex, "N end")
gb_dual_rtc = 1790500000       ; session clock seed
psp_clock = 1790500000
psp_clock_tz = 0
gb_dual_batch = 154
gb_dual_armed = 0|1            ; section 4
perf_rig = 1
autoexit_frames = 11200        ; Gen 2 trade ends at frame 11057
```

Track: `linkplay rom0=crystal rom1=crystal sav0=gen2-a.sav sav1=gen2-b.sav
ap0=gen2_host.ap ap1=gen2_guest.ap delay=4 rtc=1790500000 inputs_out=FILE`
(tools/gblink/scripts).  It costs like the real session's emulation (both
machines, real serial traffic) minus the radio.

## 3. Which candidate slowed GB

Lab work_ms, gb_crystal_walk: lab-final 8.82, lab-combo3 9.02, **lab-c3lat4
9.63** (+0.61), cpl1/f1c2-da2 unchanged since.  gb_shantae_town 14.31 ->
14.96, gb_ages_forest 9.67 -> 10.39.  c3lat4 with `input_fresh=0`: 8.95 /
14.28 -- back to combo3.  So the step is c3lat4's input_fresh pad wait, a
sleep counted in work_ms (memory input-fresh-work-ms), not GB CPU.  (Smaller,
older: lab-compat +0.3 and the compat-cost arms +0.5 on crystal_walk,
already gone by combo3.)

## 4. Levers, ranked (estimates; hardware decides)

1. **Armed batching** (`batch_armed`, link only, exact, low risk).  A batch may
   also start while slot 1's port is busy, as long as slot 0's is idle.  Slot
   0's first serial event still ends the lead (the existing cut/hidden path);
   the one read that could see a lagged port -- a CGB fast-transfer clocked by
   slot 0 while slot 1 trails -- is counted (`lag_reads`) and switches the
   mode off for the session.  Lines batched: Gen 2 trade 25 -> 89 % (93.6 %
   with quiet=1), Gen 1 16.5 -> 82 %, battle 15 -> 80 %.  Expected -2.5 to -4
   ms in busy trade windows (strict 12.8 vs batched 9.9 ms on hw2).
2. **Lazy timer** (`TGB_LAZY_TIMER`, all GB, exact).  cpu_clock_advance keeps
   a budget of clocks before the next timer event (TIMA tick, overflow
   reload, reload end); below it only the divider phase moves.  Timer writes
   (DIV/TIMA/TMA/TAC), reset and every state load call cpu_timer_dirty().
   x86 Gen 2 trade 1.60 -> 1.30 s (-20 %).  PSP guess -0.5 to -1.5 ms in
   heavy scenes; layout/code size may eat some (the -Os lesson).
3. Dispatch: op_read fast path, inline cpu_clock_advance.  Medium risk, code
   size moves the I-cache picture.
4. LCD renderer (drawn machine, ~2.4 ms).
5. APU synthesis (small).
6. Spread the periodic sync hash over frames (removes a 2-4 ms spike / 240).
7. Net/frontend (~0.6-1 ms).

## 5. Prototypes (both OFF by default)

**Armed batching** -- `gbdual_config_t.batch_armed`; session bit
`FE_GBLINK_MASK_ARMED_BATCH` (0x4, NEGOTIABLE: the host decides, both
consoles step alike, an older build on either side = plain session); harness
key `gblink_batch_armed` (host), bench key `gb_dual_armed`.  Both consoles
must agree because the periodic sync hash reads both machines at slot 0's
frame ends.

**Lazy timer** -- `make TGB_LAZY_TIMER=1` (psp/Makefile, adds
-DTGB_LAZY_TIMER to TGB_CFLAGS; both machine copies get it).

### Exactness

* linkplay x86, per-machine whole save state + picture hash at every frame
  end: strict, base b154, armed, armed quiet=1, armed b1232, and batch
  2/7/33/77/500 with armed quiet=1 -- all identical (gen1 <=6540, battle
  <=7200, gen2 <=11040 frames); saves identical; lag_reads 0 everywhere.
* sesssim (whole session stack: two consoles' fe_gblink, latency, bulk lane),
  Gen 2: armed off / on / on b1232 / guest-only request -- all rc=0,
  gblink_done frame=8344 hash=f61d7f984320b784 on both sides, saves equal;
  a guest-only request is ignored (armed=0).
* Lazy timer: tgb_ref `--statehash` over all 10 Lab gb_* fixtures -- every
  frame's state and picture hash identical to the base; linkplay trades
  identical.
* PPSSPP (correctness gate only): see section 6.

## 6. PPSSPP results

Builds gbp-a / gbp-b (section 7), headless PPSSPP, vhash every frame:

* All 12 registry gb_* single fixtures: A and B identical on every frame
  (0 diffs), all exit 0.
* One-console Gen 2 replay trade (section 2): gbp-b armed=1, gbp-b armed=0
  and gbp-a armed=1 -- per-frame vhash identical over 10,571 frames (the
  run stopped at perf_rig's default perf window, after the trade); serial
  bytes 2660 as in linkplay; slot 0's cartridge RAM at the end is
  byte-identical to linkplay's traded save (only the 48-byte RTC footer
  differs: it stamps the wall time at save).  So the replay fixture
  reproduces the real trade on one console.

Fixture files: builds/gbcore-perf-out/replay-gen2 (trade.inputs, slot0.sav =
the Lab's gblink_gen2_trade sav 4216b8ab, slot1.sav, harness-armed0/1.ini;
ROM = Crystal Rev 1, 301899b8).  For a Lab fixture set perf_from/perf_to to
cover frames ~300-10500.

## 6b. Lazy timer v1 lost on hardware; v2

3000, gbp-a vs gbp-b (input_fresh=0): Ages -12.9 %, Crystal walk -7.2 %,
SML -6.7 %, Crystal -5.2 %; but Shantae town +7.5 %, Shantae menu +6.2 %,
LADX +5.1 %, Kirby +3.5 %, Yellow +3.5 %.  Op counts per fixture (tgb_ref):
every winner runs with the timer ON all the time (Ages 22.7M ops, 98 % fast
path), every loser with it OFF all the time (TAC&4 clear: Shantae, LADX,
Kirby, Kirby 2, Red, Yellow) -- v1's budget was always 0 there, so it was a
pure extra load + branch per instruction in front of the base's own check.
x86 agrees (v1: Shantae menu +7 %, Yellow +6.5 %, Kirby 2 +10 %).

v2 (684d6d0): a stopped, idle timer gets budget TIMER_IDLE, so those games
take the fast path too; the fast path no longer writes sys_clock (only the
state savers read it: cpu_timer_sync() there and before every timer
register write; cpu_timer_forget() on reset/load); the walk is noinline, so
the per-op cpu_clock_advance shrinks from 0x674 (base) / 0x710 (v1) to 0x12c
bytes.  x86 min-of-7 vs base: timer on -15..-21 %, timer off -6..+1 % (a
base-vs-base arm shows +-5 % noise).  Exact: same proofs as v1 (tgb_ref 10
fixtures, linkplay 3 scenarios x 5 policies).

## 7. Hardware A/B to run

Builds (cand1 Lab recipe, tools/build.sh lab, PAD 5264), commit 6c09270:
`builds/gbcore-perf-out/gbp-a` (both switches off; armed is run-time) and
`gbp-b` (TGB_LAZY_TIMER=1).

1. Lazy timer: gbp-a vs gbp-b on the gb_* fixtures (both models).  Compare
   work_ms with input_fresh in mind (or `input_fresh=0` on both arms).
2. Armed batching: gbp-a, one-console Gen 2 replay fixture (section 2) with
   `gb_dual_armed=0` vs `=1`; then a real two-PSP Gen 2 trade with
   `gblink_batch_armed=1` on the host (both consoles on gbp-a).
3. Both: gbp-b + armed on the replay fixture.

## 8. Gen 2 armed: what the over-budget frames are (2026-10-28)

Hardware (armed=1): Gen 2 trade 58.6 fps, 12.88 ms mean, 25.3 % of frames
over 16 ms, max 32-35 ms in every arm.

Method: `linkplay framestat=FILE` on a TGB_OPCOUNT x86 build gives, for each
slot-0 frame, lines, instructions and slot switches per machine. A fitted
cost model, PSP ms = 1.25 us x (ops0+ops1) + 9.0 us x switches + 3 ms, gives
28 % of frames over 16 ms (hardware: 25.3 %). On the post-trade tail it gives
18.8 ms (hardware window: 18.1 ms). The 9.0 us per switch comes from the
armed0/armed1 hardware delta.

What dominates the frames over 16 ms:
- **CPU of both machines, split evenly**: about 8.0 ms each. Switching adds
  about 0.34 ms. The fixed part, about 3 ms, is the shown slot's PPU, the APU
  and the frontend.
- **Post-trade tail (frames ~9300-10500)**, the worst scene. Each machine runs
  a constant 6307 instructions per frame and never HALTs. It busy-polls the
  joypad: routines 0x0984-0x09a6 (bank 0) and 0x5714/0x576a (bank 1), about
  77 calls per frame, plus a DelayFrames-like loop at 0x468. I found no exact
  cheap skip. Only a faster CPU path helps.
- **Strict-stepping windows** (serial bursts at ~2700-3300, 3900-4500,
  5100-5700 and 8700-9300) are now minor. quiet_frames=1 is only about
  -0.13 ms mean.
- **The 32-35 ms max frames** are multi-frame advances, about 18 per 10k
  frames. One host frame runs a machine for more than 154 lines (252-308),
  usually around LCD on/off or slot 1 catching up. That is about two frames of
  emulated time, so it is inherent and happens in solo play too.
- **The 240-frame hash** is not in the bench path, so it is unmeasured. A fix
  would spread the hash over frames, which is a protocol change.

Sensitivity (model): a CPU speedup of x0.92 gives 26.7 % over and a 17.6 ms
tail. x0.85 gives 24.7 % and 16.5 ms. Only about x0.80 clears most of the
frames (6 %), because the busy scenes sit just above 16 ms.

x86 profile, Gen 2 trade with lazy v2:

| Function | Share |
|---|---|
| cpu_exec | 25 % |
| op handlers | 22 % |
| dattrans2n | 11 % |
| snd_render_orig | 7.5 % |
| set_ztbl | 7.2 % |
| cpu_clock_advance | 6.6 % (was 28-31 % before the lazy timer) |
| lcd_bg_render_color | 5.7 % |

### TGB_FAST_TILE (commit 7b9d20c, off by default)

dattrans2n decodes a tile row with a 256-entry spread table instead of eight
bit-by-bit dtwk() steps. It is a pure function of its inputs.

Exactness:
- tgb_ref: all 10 fixtures have identical state and picture hashes.
- linkplay: all 15 policy x scenario hashes are identical.
- PPSSPP gbp-a2 vs gbp-c2: all 12 gb_* fixtures have vhash diff=0.
- Gen 2 replay (armed=1): vhash md5 ad0eb470 on both builds; slot-0 save
  4a7f8a60b8f2 on both.

x86 speed vs lazy v2 alone: -0.5 to -7.9 % on most fixtures. Kirby +6 % and
Kirby 2 +3 % are inside the ±5 % noise.

### Builds (builds/gbcore-perf-out)

| Build | Switches | PBP md5 |
|---|---|---|
| gbp-a2 | baseline (all off) | 73582809 |
| gbp-b2 | TGB_LAZY_TIMER | 5d917bf0 |
| gbp-c2 | TGB_LAZY_TIMER + TGB_FAST_TILE | f049558f |

### Next levers (not prototyped)

1. quiet_frames=1 tied to armed. This needs a new mask bit because 0x4 is
   taken. The gain is small.
2. Skip set_ztbl on lines with no sprites (about 7 %, needs an exactness
   proof).
3. Work on the dispatch and op path (cpu_exec plus the handlers are about
   half the time). This is medium risk and is the only lever big enough for
   the tail.
4. Spread the session hash over frames (protocol change).
