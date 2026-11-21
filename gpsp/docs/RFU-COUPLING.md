# Coupling the two consoles over the emulated Wireless Adapter

Design, 2026-11-02; built since (see Status). Since code-health C-4 scheduled ticks are the adapter's only
protocol, in every build. Branch `claude/rfu-reference`; the evidence (PPSSPP traces and the hw1
hardware run) is not kept in the repo.

## The problem

Each console runs its own game at its own speed, and `rfu.c` delivers packets whenever they arrive. Real
adapters never see this: two GBAs run at one rate. Two desktop instances over loopback, with no radio at all:

| | Resent | Dropped | Client queue |
|---|---|---|---|
| Tennis, matched 55/55 | 2.7 % | 0 | 1 |
| Tennis, host 57 / client 55 | 58-59 % | 6-7 % | 6 |
| Golf round, matched 44/44 | 0 | 0 | 1 |
| Golf round, host 50 / client 44 | 0 | 8.6 % | 16-17 |

A faster client does no harm. A faster host does, and 2 fps is enough. Hardware shows the same picture
(Tennis 45-51 % resent, Golf 13-14 % dropped).

The delivery rules (`frame_pace`, shed, hold, supersede, `rfu_gq_depth`) all treat this symptom, and all carry
thresholds read off Pokémon. That is why adapter play works mainly for Pokémon.

## What failed: holding the leader

`rfu_match` guessed how far ahead a console was and slept a few ms. On the desktop it worked. On hardware it
cut Golf's drops to 0.2 %, but fps fell (Tennis 57/54 to as low as 49/48, Golf 50/43 to 40/40). Holding the host
also delays the client, so a guess of "ahead" feeds itself, and radio jitter supplies wrong guesses. A smoothed
reading was worse on the desktop (Tennis sank to 43-47 fps). Do not tune this further.

## Options

| | Speed | Memory | Verdict |
|---|---|---|---|
| Timed hold | loses fps on hardware | none | Rejected, above |
| Mirror, as the cable does (both GBAs on each console) | Two cores: Golf already runs 43-50 fps with one | Core B | Rejected for now |
| **Scheduled ticks, one GBA per console** | Slower console sets the pace | Small | **Recommended** |

## Recommended: scheduled ticks

Same principle as the cable (no console advances past what it knows about its partner), without a second core.

1. **Tick.** One tick = one emulated frame. Frame numbers are agreed when the link forms, using the frame stamps
   packets already carry.
2. **Publish.** During frame F a console collects what the game sends. At the end of F it sends it, marked F, plus
   a "tick F complete" mark, even if nothing was sent.
3. **Deliver.** Data the partner published for tick F reaches this game at the start of frame F + D, never sooner
   and never later.
4. **Wait.** A console may not start frame F + D until it holds the partner's tick F. It waits on that fact and
   nothing else, keeps pumping the network, and stops the moment the packet lands.

**Why this cannot sink like the hold.** The hold slept on an estimate. Here a console waits only for a packet that
is really missing, and for no longer. Neither console can be slowed below what the slower console and the wire
allow.

**Frame rate.** The pair runs at the slower console's speed, free-running and uneven, as cable sessions do. That
is coupling, not a fixed cap, so it should fit the no-locked-rates rule, but the owner has the last word. Today's
forced 57 fps (`PCFG_PROFILE_FPS_X100`, ADR-0073) gets reviewed once ticks work; it may have no job left.

**What goes away.** Backlog cannot form, because each console reads what one tick gave it. `frame_pace` (with its max
hold and catch-up), shed, hold and supersede were retired in code-health C-4 (see Status). `rfu_gq_depth` stays as a
census (GQPEAK), not a rule.

**Better testing.** Delivery no longer depends on wire timing, so a session with the same inputs should replay
identically under any jitter. Hash comparison then becomes possible for adapter fixtures, as for the cable, which
the Lab cannot do today. That is expected, not shown, until the wall-clock reads in `rfu.c` are removed.

### Choosing D

Wire time is about 5 ms. W is work per frame (client 16 ms Tennis, 22 ms Golf). Estimates, not measurements:

| D | Frame period | Tennis | Golf | Round trip seen by the game |
|---|---|---|---|---|
| 1 | W + wire | ~47 fps | ~37 fps | 2 frames |
| 2 | W, if the wire is under a frame | client's own speed | client's own speed | 4 frames, ~70 ms |
| 3 | W | same | same | 6 frames, ~105 ms |

D = 2 is the starting value: it keeps fps and beats today's ~100 ms. Games tolerate it:

- Tennis at matched rates kept resends at 2-5 % out to a 10-frame round trip (72 ms each way) and finished every
  run. It only started failing to hold 55 fps at 108 ms each way.
- Golf rounds ran on hardware at 5-6 frames.

So delay was never the Tennis problem. Today's 100 ms round trip is not what makes it hitch.

### Lifecycle

- **Coupled only while linked.** From a completed connection to its disconnect. Scanning and room hosting stay as
  they are.
- **Connection itself is delay-sensitive.** In the first Golf sweep, without the lobby fix, most runs at 27 ms or
  more each way timed out connecting. So the connect exchange stays unscheduled and as fast as today.
- **A partner that goes silent.** After a limit shorter than the game's own link timeout, stop waiting and let
  ordinary disconnect handling run, so the game shows its own error. The 30 s no-peer rule from the cable does
  not fit here.
- **Stalls.** A long wait returns to the frontend at a frame start, like the cable's stall absorption, so audio
  and the screen stay alive. Reuse that code.
- **Pause, sleep, in-game menu.** Stopping one console stops the other, as on the cable. Decision for the owner:
  Union Room players may mind this more than a battle does.
- **Three to five players.** Not supported: multiplayer is two consoles (owner decision, tier C). A third
  console in an adapter room is not scheduled and, since C-4, has no delivery rules either; no test covers it.
- **Wire format.** New packets and a new meaning for old ones: both consoles need the same version. The planned
  build-mismatch refusal must land with this.

## Risks

1. **Jitter becomes visible stalls.** Today a late packet is hidden in a queue; here it pauses both consoles.
   One-way times at matched rates on hardware were mostly under 120 ms, with p99 at 75-118 ms in the lobby and
   150-212 ms in Golf's one matched round, which also showed one 763 ms spike. Most of the old 800 ms spikes came
   from mismatch (matched p99 150 ms against 790-830 ms), but anything above D's cushion will show. The cable
   lives with the same exposure and plays well, so this is expected to be fine, and it is the first thing to measure.
2. **Sub-frame timing.** A frame-sized tick may be too coarse for a game that exchanges more than once a frame.
   Today's ideal case answers inside a frame.
3. **The Golf lobby race (`claude/rfu-lobby`) is not solved by this.** The fix passes at no delay and at 36 ms each
   way, but scripted steady presses wedged 8 of 8 at 5-27 ms each way (2-5 frames of round trip) on the desktop.
   Uneven human presses passed before the fix. Ticks fix the round trip at a value, so whichever D is chosen either
   sits in that band for steady presses or does not. Test with a jittered script and with turbo.
4. **Client cost.** The client works ~3 ms harder than the host, and the pair will now run at its speed. Worth
   a look on its own.

## Plan

Each step gets a hardware answer before the next; one build awaiting hardware at a time.

1. **Desktop prototype**, off by default, `rfu_sched = D`. Pass: Tennis at 57/55 shows under 5 % resent and
   nothing dropped, the slower rate held, no queue growth.
2. **Jitter check on the desktop**: repeat with loss and jitter. Pass: same game-visible timing; identical hashes
   once wall-clock reads are gone.
3. **PPSSPP, then hardware ABBA**, ticks vs today: Tennis, Golf, Emerald trade and battle, FireRed, long Golf,
   both console orders. Pass: fps within a frame of the slower console's solo speed, drops near 0, stalls scored.
4. **Fixtures** for what is not covered yet: other adapter games, three consoles, disconnects, net conditions.
5. **Retire the old rules** one at a time, with a hardware run after each. Review the 57 fps force.
6. **Beta build**: a natural first nightly once Tennis and Golf hold up in real sessions.

## Status (beta.6 online tuning, 2026-11-17)

- **D by session kind.** `rfu_sched_d_for` (rfu_sched_api.h): 2 ad hoc, 4 in an online room, set at each session's
  start (`net_start_np`, EVT `rfu_sched d= kind=`). Both consoles know the kind and run the same build, so nothing
  is negotiated and they cannot disagree. A harness `rfu_sched` (the Lab's `-dN` arms) is used for every session.
  Hardware basis: Lab ONLINE-SUITE.md 10.3 (36.8 / 41.5 / 45.8 linked fps at D = 2 / 3 / 4).
- **The NI window is unchanged at D = 4** (test_rfu_sched `test_ni_window_d4`): inside an NI exchange every NI
  packet reaches the other game 1 frame later, and Golf's race (the child presses in the frame the parent moves on
  to UNI) lands 1 frame later, at D = 2 and D = 4 alike, because the parent's own NI sends hold it at D = 1. What D
  does change: an NI packet that starts an exchange after more than RFU_SCHED_NI_FRAMES of UNI on both sides can
  arrive up to D frames late (2 -> 4), like any UNI data.
- **No give-up while the network holds the ticks** (`rfu_gate_step`, `netdrv_link_held`). Lab j7c1b2e (D = 4): one
  lost packet waited ~2.5 s behind a backed-off RTO with 139 frames parked behind its gap; the VPS gave up at 2 s
  three times, ran free, and on relock both receive queues overflowed (`rfu_qdrop` 8 + 8, the trade's save never
  finished). A free run is a lead, and a game drains its partner's data at about one packet a frame, so a lead can
  only arrive as a burst. The gate now waits while netdrv shows a gap, silence (1 s) or our data unacked for 1 s,
  until the tick lands or netdrv's peer death (8 s) ends the link; the 2 s give-up is for a partner whose path is
  clear. test_rfu_sched: an outage at D = 4 delivers every packet on schedule with queues at depth 2; the old rule
  in the same outage loses 240 of 1307 packets with the host queue at 64.
- **Redundant DATA online** (netdrv `nd_config.redund`, `ND_T_DATAR`, `ND_FEAT_REDUND`). Each first transmission
  carries up to 2 earlier unacknowledged payloads, so one or two lost datagrams cost one or two frames instead of an
  RTO stall on the ordered lane (~9-18 s per trade at the 3000's 2.4-5.3 % loss, ONLINE-SUITE.md 10.3). Online
  adapter rooms only; ad hoc adapter rooms never send it (the cable does ad hoc: GB-LINK.md, `gblink_input_copies`). RFU packets are 104 B, so a datagram grows by at most
  2 + 2 x 108 = 218 B (346 B), never past 584 B (the relay's 600 B datagram holds it unchanged).
  tools/tests/test_netdrv_redund.c: at 3 % random loss, 602 of 2500 payloads came late without it, 1 with it.
- **Hardware, build 01c4b13a50d5 (a2073cf7), 2026-11-17.** PSP-3000 vs PPSSPP on the Lab VPS, Emerald Trade Center,
  linked phase (frames / wall, in_trade_center..trade_done) on the 3000: direct 54.3 / 53.5 / 53.1, relay 54.6 /
  53.1 / 54.0 (jf26b10 j5b8a98 j3c8ebc, j50a5e5 j6c33ec ja6c497): mean 53.8 against the study's 45.8 at D = 4
  (ONLINE-SUITE.md 10.3). 6/6 PASS; window waits 8.5-12.5 s on the 3000 (study 19-27 s); the VPS filled 123-173
  gaps per window from redundant copies (the 3000 -> VPS losses), the 3000 0-1; rfu_qdrop 0, no give-up. Local ad hoc
  pairs (rfu_em_trade, rfu_mt, rfu_mg, both host orders, jed5e4c j1e5d88): 12/12 PASS at d=2 kind=adhoc, no qdrop.
  Mario Golf's lobby at D = 4 (ad hoc pair, `rfu_sched = 4` on both, j282b92 jc644a5): 4/4 PASS, both orders. No
  online Golf case exists.

## Status (code-health C-4, 2026-11-16)

Ticks are the only adapter protocol (owner decision A-0: stable 5.0.0 ships them).

- **No switch.** `RFU_SCHED` is not a make variable or `#ifdef` any more, and `tools/release.sh` has no version-string
  branch: a stable tag builds the protocol the betas shipped. `tools/build.sh` accepts `RFU_SCHED=1` as a no-op.
- **One dial.** `rfu_sched = D` (harness key; desktop `--rfu-sched D`), default 2 on the PSP and the desktop.
  `D = 0` is a diagnostic arm: raw queues, no rules, and netdrv refuses a ticks partner (`ND_FEAT_RFU_SCHED`).
  libretro and 3ds have no tick gate and run D = 0.
- **The older rules are gone**: frame pace with its max hold and catch-up, link shedding, hold-never-discard and
  supersede, with their harness keys (`rfu_frame_pace`, `rfu_pace_max_hold`, `rfu_shed_keep`, `rfu_hold`,
  `rfu_supersede`), the CONFIG.INI key `rfu_pace_max_hold` (a player's file keeps the line, unread), the desktop flags
  `--rfu-play` / `--rfu-pace`, and trace events 19, 22, 23, 30, 33, 34, 36, 37. The censuses the Lab reads stay
  (rfu_backlog, rfu_hbacklog, rfu_qrun, rfu_gqpeak, rfu_empty, rfu_answin).
- **4.5.0 cannot link** over the adapter with this build: it sends no ticks, so netdrv refuses the pair.
- **The 57 fps force.** Every speed-profile net session is lifted to 59.73 (ADR-0073's reason is gone); the
  compatibility profile and `net_session_fps_force` keep their meaning.

## Status (2026-11-03)

Built and on hardware as the build switch `RFU_SCHED=1` (tools/build.sh, off by default then; docs/BUILD-SWITCHES.md).

- **Hardware, PSP-1000 + PSP-3000, D = 2.** Lab build d5973f7fc046 (`claude/rfu-ticks` 2e5c288), Lab notes `rfut460`:
  11 of 11 passed across both console orders. Slice fps (the Lab's own measure, host/join): Tennis 56.2/56.1 and
  55.9/55.8; Golf 46.3/46.6, 46.9/47.1, 46.6/46.6, 45.2/47.2, every lobby reached the round; long Golf (17 min)
  48.7/48.6; Emerald trade 56.7/56.7; battle 56.3/56.3; host loss shows the join's error screen at D = 2 and D = 0.
  The 57 fps session force (ADR-0073) still applies, which is why nothing passes 57.
- **Earlier lab builds** (`claude/rfu-reference`, RFU_TL, flight recorder): resent 2.6-5 % against 43-60 % off,
  dropped 0 against 13-15 %, client queue 0 against 32-48. With ticks on, frame pace, shedding, hold and supersede
  off gave the same numbers as on, so ticks now switch them off.
- **Client-vs-host work gap.** Off, the client works 2.7 ms (Tennis) to 4.7 ms (Golf) more than the host; with ticks
  0.4-1.2 ms, because both run at the slower console's pace. Not pursued further.
- **Frontend wait.** fe_host_rfu_gate waits up to 12 ms per pass, then gives the pass back without a frame so the
  menu and HOME stay live. Long Golf: 3-4 % of passes gave up a frame, about 5 % of wall time spent waiting.
- **Wait notice.** A hold that lasts 1 s (the gate's, or a cable lockstep stall) puts "Waiting for the other
  player..." in a bottom-left chip until the first frame runs (frontend-common/fe_linkwait.h, psp/fe/link.d.inc.c
  link_wait_osd; EVT `link_wait show=0|1`). A partner declared dead clears it: the lost link lets the gate run and
  the disconnect flow takes over. The CPU path re-presents the held picture while the chip is up.
- **Mixed builds.** netdrv admission carries ND_FEAT_RFU_SCHED: a ticks build and one without refuse each other.
- **Open.** The Golf lobby race (Risks 3) is narrower, not gone: desktop launch-gap sweeps with steady presses pass
  22/24 at D = 2 and 18/24 at D = 0; with uneven presses 47/48 and 48/48. Hardware with ticks 15 of 17 lobbies over
  three builds, off about 5 of 9. Three consoles, sleep and in-game menu pauses on hardware, and the player-route
  fixture for adapter rooms are not done.
