# GB / GBC link play between two PSPs (dual emulation + input lockstep)

Branch `claude/gb-dual-link`.  Game Boy trades and battles across two PSPs
without a live serial cable: **each PSP runs both players' Game Boys** with
their link ports wired together in memory, and only the buttons cross the
radio, in lockstep.  The earlier byte-cable approach (`netdrv/gb_link.c`,
serial bytes over ad-hoc) is not used: the Game Boy link protocol is timed in
microseconds and a radio round trip is milliseconds.

## 1. The machines (milestone 1)

* **Two TGB Dual cores in one binary.**  The adapter and core are linked
  into one relocatable (`ld -r -T gbcore/gbcore_instance.ld`), everything but
  `gbcore_*` is localised, and a second copy is renamed `gbcoreb_*`
  (psp/Makefile; `tools/run_gb_tests.py build_instances` for the host).  No
  struct refactor of the vendored core; every change to it is listed in
  `gbcore/tgbdual/LOCAL-PATCHES.md`.
* **Power-on** (`gbcore_power_on`) restores the instance's `.data` snapshot
  and clears its `.bss`: `gb_reset` leaves NR10-NR51 and FF6C/72-75 behind,
  so without it a second game in one process differs at frame 0 (tested).
* **Headless** (the partner's machine): no picture, no audio synthesis.  TGB's
  CPU-side `snd_update` never runs, so skipping synthesis is game-invisible;
  traces are identical drawn / slot 0 headless / slot 1 headless.
* **Sync hash** (`gbcore_sync_hash`): FNV-1a over the game-visible state
  (excludes the synthesis copy of the APU and `now_win_line`).
* **The pair** (`gbcore/gbcore_dual.{h,c}`): slot 0 then slot 1, one scanline
  each; per-machine frame counts; a stall (partner input not here) resumes
  exactly where it stopped.  The cable: TGB Dual's rule (peer waiting with
  external clock -> exchange) plus "armed during our transfer" (a peer that
  armed its external clock during our transfer and moved on still gets the
  byte): Pokemon holds external clock only a few instructions a frame and the
  plain rule never connected in lockstep.  Emulated RTC: `rtc_seed` + frames.

Cost (PPSSPP, harness): Crystal in game 3,405 us a frame alone, 5,395 us with
the headless partner (+58 %); uncapped 293 vs 185 fps.  EBOOT +246 KiB static
(the second core's text and bss) for every game; Emerald on the 32 MiB layout
keeps 13 ROM-cache blocks.  Hardware numbers: the rig's milestone-1 folders.

## 2. The session (`frontend-common/fe_gblink.{h,c}`)

One reliable ordered channel (netdrv ARQ via `fe_np_gb_send`), protocol
`GBAdhoc GB link v2`:

| message | what |
|---|---|
| HELLO | protocol, role, cartridge size / SHA-1 / title / console / palette, save size |
| CONFIG | host: input delay, RTC seed (the host's clock), hash interval |
| NEED | each: whether it has the partner's cartridge (SHA-1 match in its library) |
| ROM / ROM_RESULT | the cartridge, streamed into **memory only**, SHA-1 checked before use |
| SAVE | each battery image, RTC trailer re-based to the seed |
| READY | both machines power on from identical inputs |
| INPUT | each player's buttons by his machine's frame; delay D frames |
| HASH | sync hash of both machines every 240 slot-0 frames (60 before section 14; the host decides, CONFIG); mismatch = desync, stop, save nothing |
| END_REQ / END_AT | the host settles on an end frame both can reach, later while either cartridge RAM is still changing (quiet 120 frames, at most 8 deferrals) |
| FINAL | the final hashes are compared; only if equal does each PSP write **its own** save |
| ABORT | either side, with a reason |

A late input is a stall, never a divergence.  Nothing is written unless the
final hashes agree.  Harness keys: `gblink_role`, `gblink_delay` (also a
`config.ini` key, 1-30, the host's wins), `gblink_input_copies` (also a
`config.ini` key, 0-2, each console its own), `gblink_rtc_seed`,
`gblink_end_after`, `gblink_bulk`, `gblink_library`.

**Input delay and carried copies.**  With neither `config.ini` nor the
harness setting them, the host picks the delay by the session's kind, as the
Wireless Adapter does (`rfu_sched_d_for`): 3 frames ad hoc, 4 online
(`GBLINK_DELAY_ADHOC` / `_ONLINE`, `psp/fe/play_defaults.h`).  At 3 frames the
partner's buttons arrive with about 1.5-1.8 frames to spare (hardware
`lead`), barely a resend (~1.5 frames) and short of a retransmission (3), so
an ad hoc session also turns on netdrv's redundant DATA (`nd_config.redund`, `ND_T_DATAR`): each
datagram carries the last 2 still-unacknowledged payloads again, and one
lost datagram is filled by the next, one frame later.  `gblink_input_copies`
sets it (0-2; default 2 ad hoc, 0 online, `GBLINK_COPIES_*`).  Unlike the
earlier redundant-input datagram below, the copies ride in the datagram that
was going out anyway: no extra send.  The cable announces nothing at
admission (both consoles must run the same build, and every build reads
DATAR), and its DATAR never outgrows a plain frame (`red_body_max`), because
the ad hoc transport drops a bigger datagram on arrival.  `gblink_done`
splits stall episodes by how the awaited buttons came (`end_late`, `end_red`,
`end_retx`, `end_resend`, `end_other`; `fe_gblink.h` `FE_GBLINK_VIA_*`).

**UI**: GB/GBC in-game menu -> Wireless -> Host / Join (Scan) exactly as for
GBA; the connecting screen reads "GAME BOY LINK" with the status ("Receiving
<title>", "1.2 / 2.0 MB", "Exchanging saves...", "Linked: ..."); Disconnect
ends it cleanly ("Link ended. Game saved.").

## 3. Cartridge transfer (owner decision 2026-09-28)

A PSP without the partner's cartridge receives it over the session into
memory (never written to the Memory Stick, dropped at the end); skipped when
the library has one with the same SHA-1.  The receiver checks the SHA-1 before
powering on and aborts cleanly on a mismatch.

Measured in PPSSPP (harness, 139-byte chunks = the PSP netdrv payload of 144
less the chunk header; synthetic 8 MiB cart `tools/gblink/mksynth.py`):

| rate (chunks/frame) | 1 MiB | 2 MiB | 8 MiB | receiver ringdrop | notes |
|---|---|---|---|---|---|
| unpaced (M3) | | 257 s | | 705 | every drop an RTO |
| 24 | 5.25 s | 10.49 s | 41.97 s | 0 | |
| **32 (default)** | 3.94 s | ~7.9 s | 31.5-33.3 s | 0 | retx 2 in 60,000 |
| 40, backlog uncapped | 3.15 s | | never finished | 22,346 | collapse (see below) |
| 40, backlog 96 | 3.15 s | | 31.7 s | 40 | 322 resends |
| 56, ring 128 (experiment) | 2.24 s | ~4.5 s | 17.97 s | 0 | +11 KiB .bss, not adopted |

* The ceiling is the receiver's transport ring: 64 datagrams between two
  drains (one a frame).  32 a frame leaves 2x margin.
* The collapse at 40 came from the pacing: `netdrv_send_capacity` counts
  the spill list (~900 payloads), so bulk sends queued 800+ chunks and every
  loss was resent behind them (RTT 350 ms).  Bulk sends now keep at most 96
  payloads queued or in flight.
* 1 MiB (Red/Blue/Yellow) and 2 MiB (Gold/Silver/Crystal) meet the ~8 s
  budget at the default; **8 MiB does not (~32 s)**.  A 128-slot receive ring
  (+11 KiB .bss for every game, GBA included) with 56 a frame would give 2 MiB
  in ~4.5 s and 8 MiB in ~18 s; NOT adopted (owner, after hw5: ringdrop and bulk_drop were 0 on the radio, which is the limit; no compression either).  The
  hardware rig times 32 and 40 a frame on the real radio (arms X, W).
* The latency shim (`net_latency_ms`/`net_jitter_ms`) holds datagrams in that
  same receive ring until due, head-of-line, so it cannot measure a transfer
  under latency (20 ms + 10 ms jitter: 6,000+ ringdrops); it is fine for the
  1-2 datagrams a frame of a running session.

**Memory.** The session reads its own cartridge into memory while hashing it,
right after the single machine is freed, so the partner's cartridge cannot
land in the middle of that space: loading it at power-on instead failed with
an 8 MiB cart (free 15.1 MB, largest block 7.66 MB).  8 MiB + 1 MiB running:
5.13 MB free on both consoles (harness, 32 MiB layout, PPSSPP).

## 4. Validation without hardware

* **Bit identity**: single-player GB/GBC traces equal 3.0.0 (696ad10) through
  `gbcore/tests/bitident.c`; GBA untouched (host suites).
* **One process** (`tools/gblink/linkplay`): Gen 1 (Blue) and Gen 2 (Crystal)
  trades exact in the saves at delays 0/4/8 (`score_trade.py`, byte-exact
  party structs, Gen 2 friendship reset to 70); Gen 1 Colosseum battle at
  delays 0/2/4/8/12 (874-917 serial exchanges).  Every case identical drawn /
  slot 0 headless / slot 1 headless.
* **Two consoles simulated** (`tools/gblink/sesssim`, the whole fe_gblink
  stack over a simulated network): both trades at delays 2/4/8 x latency
  1/3/6 frames x jitter 0/6/20 (18/18) and the battle at delay 4 with the
  same three radios -- all DONE, every hash matched, saves equal to the
  one-process result at that delay; cartridge transfer, a corrupted transfer
  (ROM_BAD), an injected desync (stopped, nothing saved).  Host suite `gb`.
* **Two PPSSPP instances** over PPSSPP's ad-hoc: every hardware-rig arm, run
  from the staged files (`tools/gblink/run_stage_pair.sh`) and scored by the
  hardware scorer.  See section 6.

## 5. The hardware rig

Standards of `docs/RIG-DOUBLE-BATTLE.md` (own app folder, backup first,
roles by content, invariants, no conclusions from absence), driven by
`tools/rig/hw_loop.py --appdir GBADHOC-GBLINK` (now allowed by name).

* `tools/gblink/prepare_gblink_rig.py` -- stages (host-/join- files), golden
  saves, reference goldens, the synthetic cart, the milestone-1 folders on the
  same binary, the player build, MANIFEST.txt with the EBOOT CRC.
* `tools/gblink/setup_gblink_cards.py` -- the lead's one write to the sticks:
  only `PSP/GAME/GBADHOC-GBLINK*`; refuses without a verified backup, on a
  wrong Crystal revision, different Gen 1 carts, or a role clash.
* Arms: A Gen 1 trade, B Gen 2 trade, C Gen 1 battle, L = B with 30 ms + 15 ms
  jitter each side, X 2+1 MiB transfer, W = X at 40 a frame, Y 8 MiB to the
  PSP-1000.
* `tools/gblink/score_session.py` (one run) / `score_rig.py` (a session):
  validity I1-I10 (logs complete, same binary, keys applied, staged scripts,
  no log loss, roles and models by content, one session with equal delay and
  seed, cartridge identities), then PASS only with DONE on both, every hash
  matched, equal final hashes, the save files equal to the committed images,
  and the exact trade / golden / battle marks.  `tools/tests/test_gblink_score.py`
  proves 17 broken runs never pass.

Owner steps: a step-by-step sheet staged with the rig (not kept in the repo).

## 6. Results so far (PPSSPP)

Every staged arm, run from the staged rig folder exactly as the cards will
hold it (owner CONFIG.INI, golden saves, card ROM names; Blue standing in for
Red) and scored by `score_rig.py` as a hw_loop session -- all PASS:

| arm | what | wall | stalls host/join (longest) | cartridge received | running heap free |
|---|---|---|---|---|---|
| A | Gen 1 trade | 129 s | 4/1 (4) | -- | 12.47 MB |
| B | Gen 2 trade | 204 s | 0/0 | -- | 10.37 MB |
| C | Gen 1 battle | 139 s | 4/1 (4) | -- | 12.47 MB |
| L | B + 30 ms / 15 ms jitter each side | 206 s | 67/64 (2) | -- | 10.37 MB |
| X | transfer, 32/frame | 39 s | 0/0 | 1 MiB 3.94 s; 2 MiB 8.14 s | 11.42 MB |
| W | transfer, 40/frame | 37 s | 0/0 | 1 MiB 3.15 s; 2 MiB 7.30 s | 11.42 MB |
| Y | 8 MiB to the PSP-1000 model | 81 s | 0/0 | 1 MiB 3.94 s; 8 MiB 33.3 s | 5.13 MB |

Every run: both consoles DONE, every periodic hash matched, final hashes
equal, each save file the committed image, trades exact in the saves, battle
start and end seen by both scripts.  L shows the design working: 64-67
stalls where B had none, never more than 2 frames in a row, same result.
(Final binary 472b498; an earlier run on a0d3e07 gave the same verdicts.)  Y ends by the
8-deferral rule (`gblink_end_forced`): the synthetic cart writes its RAM every
frame.

GBA unchanged: the frame-exact video oracle (`tools/e2e/run_video_regress.sh`,
Emerald workload) captured with the 3.0.0 harness and compared against this
branch's harness: 1,091 frames pixel-identical.  Emerald's heap after load
(PPSSPP, 32 MiB layout): 1.908 MB at 3.0.0, 1.627 MB now (-281 KB: the second
core, the session code), still 13 ROM-cache blocks.  Milestone-1 folders on
the final binary: DUAL 5,397 us a frame, 185 fps uncapped (unchanged).

## 7. Known limits

* The PSP's own timing (cache coherency, WLAN scheduling) is untested until
  the rig runs; PPSSPP reproduces neither.
* Fixtures belong to one cartridge (a Gen 1 save holds ROM pointers); the
  rig's are made on the owner's Red and setup refuses any other.
* Symmetric handshakes tie-break to slot 0 clocking; games that need a
  specific side as clock master have not been tried beyond Pokemon.
* An end frame that never gets quiet cartridge RAM is forced after 8
  deferrals (logged `gblink_end_forced`).
* The 44-byte RTC trailer variant is not re-based (only the 48-byte one).
* The input delay is set by config.ini / harness only; there is no Settings
  row yet.

## 8. Hardware run hw1 and what changed

The hw1 findings (raw logs and notes are not kept in the repo), in short: the link
held (every periodic hash matched while sessions ran); four root causes, each
reproduced off-hardware and fixed:

| arm | root cause | reproduced | fix |
|---|---|---|---|
| A, C | Gen 1 fixtures made on Blue, rig ran Red: ROM pointers in the save, garbled map | linkplay, same frame 7359 | fixtures + references per cartridge, setup checks the SHA-1 |
| B, L | harness passed the seed as the console clock; the 1000's 2011 clock became a 15.6-year advance, RTC overflow, "The clock's time may be wrong." | linkplay, step 13 frame 2178 exact | harness freezes the cartridge clock at the seed; `psp_clock` fakes a console clock in PPSSPP |
| W | join tore the radio down at DONE before the host had its FINAL | log order | linger until nothing is unacknowledged (10 s max) |
| X, W, Y | ~3 % radio loss + netdrv cumulative ack: RTT inflated to ~1 s, RTO 2.5 s stalls, spurious resends; 2 MiB 372 s | PPSSPP `net_loss_pct=3`: 283 s | **bulk lane** (below) |

**Bulk lane** (`fe_gblink.h`, `transport_adhoc.h`): cartridge and save chunks
as 1037-byte datagrams (1024 data, CRC-32) on the same PDP socket outside
netdrv, a 32-slot receive ring allocated per session; the receiver
acknowledges over the ordered channel with its first gap and a bitmap of the
next 256 chunks every 2 frames; the sender resends only missing chunks after
srtt + 4 x deviation; the rate (datagrams a frame, start 4, max 16) doubles
while under 6 % of sends are resends, then +1, and falls by a quarter over
12 % / half over 25 %.  Negotiated in HELLO; a console without it uses the
ordered channel.  PPSSPP, 2 MiB + 1 MiB crossing at once:

| received loss | 2 MiB | 1 MiB |
|---|---|---|
| 0 % | 2.46 s | 1.32 s |
| 3 % | 4.17 s | 2.61 s |
| 10 % | 16.8 s | 10.3 s |

**Scanline batching** (`gbdual_config_t.batch_lines`): while neither
machine's serial port is busy and nothing serial happened for `quiet_frames`,
slot 0 runs up to a frame ahead; a serial event cuts the turn; while slot 1
catches up through lines strict alternation would run before the cut, slot
0's port is shown as it was before the cut line.  Proven exact off-hardware
(cable tests at 9 start offsets x 5 cases; Red/Crystal trades and the battle
at delays 0-12: saves byte-identical to strict).  Remaining inexact case: a
transfer that starts and completes inside slot 0's cut line (CGB fast
serial).  Hosts enable it with `gblink_batch`; default strict until the hw2
milestone-1 runs (DUAL vs DUAL8/DUALF, SOLOH) show the gain on the PSP.

## 9. After the owner's first hand-played trade

**Live link (no restart).**  `fe_gblink_local.state`: Host/Join takes the
running game's gbcore save state at a frame boundary; HELLO carries its size
(protocol 4), it crosses on the bulk lane (kind STATE; ordered channel as a
fallback), and `start_pair` loads each machine from its console's state after
powering it on from the battery image (the state holds CPU, RAM, VRAM, cart
RAM, MBC and APU but deliberately not the cartridge clock, which the image's
trailer supplies).  After DONE, `fe_host_gblink_close` boots the single
machine from the committed battery image and loads the final state captured
at the agreed end frame (`gblink_resume how=live_end`); after FAILED it loads
the pre-link state (`restore_prelink`) -- nothing of an unagreed session may
continue, since the game would save it itself later.  Harness: the game is
held from boot until the session starts (so the linked state is the loaded
`.st0` exactly), `gblink_after` plays on N frames, the autopilot's
`disconnect` ends a session at the same frame sesssim does, `gblink_resume
= 2` boots from the battery save instead (the clock check), `gblink_live = 0`
restores the power-on link.

**Clock.**  Sessions run on the host's clock; a battery image written back
kept the session stamp.  Host at 2011 (the PSP-1000): the guest's clock later
jumped 15.6 years (day-counter overflow -> Crystal's "The clock's time may be
wrong"; the owner's F: save shows day 63 + overflow).  Host at 2026: the
1000's clock was silently reset.  `gbl_commit` now re-stamps the trailer (48-
or 44-byte) with the local clock.  PPSSPP A/B (`gblink_rtc_keep_session`):
reproduced, fixed.

**Redundant inputs.**  `fe_gblink_platform.input_copies` (default 8): each
new input frame also goes out as one bulk-lane datagram with the player's last
8 frames; the receiver takes whichever copy is first (`take_input`), the
ordered copy still arrives and is ignored once held.  PPSSPP, 3 % loss:
stutter episodes 157/158 -> 1/0, longest 14/15 -> 1 frame, same final hash.

## 10. Hardware run hw2: what the logs say (2026-09-29)

All numbers are per frame or per 1000 frames, never totals: hw1's B died at
frame ~2180, hw2's sessions ran 6600-11400.

**The redundant input datagram costs more than it saves.**  V and U are the
same states, scripts and consoles; only `gblink_input_copies` differs (8 vs
0).  Their 600-frame `core_prof` windows line up one for one (same content),
and V is slower in every one: +1.35..+1.78 ms on the PSP-3000 host,
+0.72..+1.45 ms on the PSP-1000 join.  `core_prof` in a session includes
`fe_gblink_step` (fe_host.c `gblink_frame`), and the copy is one extra
`sceNetAdhocPdpSend` a frame, inline on the emulation thread: `sess_cost pdp`
rises from ~2.0 to ~2.9 calls a frame, and the mean per call from ~64 us to
180-380 us (the ad-hoc stack's threads run above ours, so what a send
triggers lands on the emulation thread).  The join's stalls DOUBLED with it
(34.3 vs 16.0 per 1000 frames), because the radio lost only 1-2 % -- the
copies rescued almost nothing.  Default is now 0 (the harness key stays).

**The rest of the hw1 -> hw2 `core_prof` rise is the content.**  hw2's
sessions start from the running game (live link), so the scenes differ from
hw1's power-on sessions window for window (hw2 A has windows at 14 and 19 ms
that hw1 A never had; its quiet windows are within 0.3 ms of hw1's).  The
one same-content comparison of the core alone, milestone-1 DUAL, rose
12.41 -> 12.80 ms (1000) and 12.37 -> 12.68 (3000), SOLO 5.87 -> 5.95: the
batching bookkeeping is ~40 extra MIPS instructions per scanline turn
(psp-objdump of `gbdual_advance`, 102 -> 316 lines), ~12k instructions or
~40 us a frame; the remainder is code layout.

**Who waits.**  Stalls are almost all the join's (V: host 1.7, join 34.3 per
1000).  Stepping on the LOCAL machine's frame boundary means the join (slot
1) needs the host's input for frame f+1 one frame before the host needs the
join's.  `gblink_pace_slot0` steps both consoles on slot 0's boundary; it
changes no machine state (sesssim: B's saves and final hash identical with
batching on or off).  Arm P measures it on hardware, K adds batching.

**Is the rig itself the slowdown?**  Its own accounting says no: `evt`
(formatting on the emulation thread) is 20-35 us a frame, the log writer
runs below the emulation thread (0x2C vs 0x2B) for 150-260 us a frame of
its own time, the stall watcher wakes once a second.  Arm T runs B with the
periodic reporting off (`telemetry_period`, one report at teardown) and no
stall watcher, so T vs B measures it directly.

**Laggy: two measures.**  Input delay is 4 frames in both builds (67 ms at
59.7 fps; 73 ms at the 54.6 fps a CPU-bound Crystal session holds).
Stutter is stalls: see above.

**The connecting screen.**  Every hw2 session's first window held a run of
33-50 ms frames: the own cartridge read from the stick 64 KiB a step (the X
host's 2 MiB = 32 frames, the join's 1 MiB = 16, Y's 8 MiB = 128 -- exact).
Reads are now bounded to 6 ms a step (with a clock), bulk sends to 8 ms.
The screen is the game as it stood, dimmed, with both directions' progress
at once and a moving marker; audio is muted for the screen's duration
(the ring is emptied, not replayed).

**Transfer speed.**  The goodput ceiling was not the radio: X's host
received 263 duplicates of 1389 datagrams -- resends of chunks that had
arrived, because acknowledgements went on the ordered channel, where one
lost netdrv packet holds the rest back past the bulk timer.  The AIMD then
read those as loss and held the rate at 1-4 datagrams a frame (2 MiB in
11.6 s = 180 KB/s).  Acknowledgements now go on the bulk lane (protocol 5);
the one that completes a transfer also goes ordered.  2 MiB in 8 s needs
262 KB/s = 4.3 KiB-datagrams a frame of goodput.

**A HELLO before the session.**  Found while checking the screen in
PPSSPP with the consoles starting their sessions 30 frames apart: the
partner's HELLO reached a console whose session did not exist yet, was
dropped, and was never resent -- both waited forever on "Connecting...".
The player flow starts both sessions right after the radio comes up, so the
window is small, but real.  fe_host now keeps a HELLO that arrives early and
replays it when the session starts (a disconnect forgets it).

**Rig (hw3).**  Arms T (B, reporting off), P (B, pace on slot 0), K (P +
batching); U is now the default (no copies) and V keeps copies 8 for the
comparison.  Harness keys: `telemetry_period`, `gblink_pace_slot0`,
`gedump_loops` (screen dumps by loop, for the setup screen) and
`gblink_start_frames` (the game runs N frames before the session: the
connecting screen then has a game to dim).  `SOFTGPU=1 run_stage_pair.sh`
uses PPSSPP's software renderer so the dumps show the real screen.

## 11. Hardware run hw3 (2026-09-29/30, runs 1-8 of 11)

| arm | over budget host/join | late | stalls /1000 host/join | notes |
|---|---|---|---|---|
| B | 28.0 / 28.8 % | 52 / 55 % | 6.2 / 4.2 | hw2 B join was 45.5 (copies on) |
| T (reporting off) | 27.9 / 29.0 % | 50 / 53 % | 7.6 / 6.9 | = B: the rig measures honestly |
| P (pace slot 0) | 27.9 / 27.3 % | 53 / 52 % | 1.3 / 22.6 | the join got WORSE: stays off |
| K (P + batching) | 27.0 / 26.0 % | 40 / 40 % | 4.0 / 20.0 | batching: core -1.7..-2.6 ms in busy windows |
| U / V (copies 0 / 8) | | | 6.8 / 4.2 vs 7.8 / 4.5 | V costs +1.3-1.5 ms core again |

Transfers with bulk-lane acks: X 2 MiB in 7.96 s (hw2 11.61), 1 MiB in
3.55 s; Y 8 MiB in 25.3 s (hw2 32.5).  Duplicates 47 of 2210 (hw2 263/1389).

Owner's observations during hw3, from the logs:
- "Ending the link..." 20-30 s: arm Y only.  Every session ended 308 frames
  after the request (END_MARGIN 300 + 2 x delay: 5-6 s); Y's synthetic
  cartridge writes its RAM every loop, so the end was deferred 8 times
  (SRAM_QUIET 120 frames each) and forced: 1268 frames at 45 fps = 28 s.
  A player sees the 5-6 s; the margin is now 8 + 2 x delay (the END_AT
  message always precedes the inputs the guest would need to pass it).
- 45 fps on the host: arm Y, real CPU.  The synthetic cart never halts and
  draws every frame; the host renders it (its local machine), core 20.6-21
  ms, every frame over budget.  The join logged 45 too; its chip read ~60
  because a stall's re-presented frame was counted (fixed).
- Dark host background: X/Y link from a held boot, so neither console has
  a picture to dim; in Y's ending the host's own game (the synthetic cart)
  is what is on screen.  Rig artifact; the player flow dims the running game
  (PPSSPP, both roles).  ME video/renderer were off on both consoles
  (`me_boot ... video=0 rend=0`).
- Audio: the link screen mutes (now logged as `gblink_mute on`); during
  RUNNING/ENDING below 60 fps the audio ring underruns -- the crackle a
  player hears whenever a session is CPU-bound.

**hw3 run 9: "Waiting for the other player" for 20+ minutes.**  Not a
lockstep deadlock: both consoles logged `adhoc_up group=GBLNK7` and sent
the whole time, and both received nothing (`adhoc_stats rx=0`), with no
`peer_connected` ever.  `sceNetAdhocctlConnect` joins a group by name or,
if it cannot see one, CREATES it; B ran straight after Y, both consoles
relaunched together, and each created its own GBLNK7 cell.  Runs 1-8 had
started a few seconds apart by chance.  A player's join always picks the
host's room from a scan, so the race is the rig's.  But a player could
still wait forever for a partner who never arrives, so now:
- the session gives up 30 s after it is ready without the partner's HELLO:
  "No other player found. Check both PSPs' WLAN switch, then choose
  Wireless again." (the failure screen stays 4 s; choosing Wireless again
  tears the group down and creates a new one);
- the harness join scans until the host's group is visible (up to 20
  scans) before connecting (`adhoc_join_scan found=.. tries=..`);
- the scorer names it: root cause NO_PEER, a FAIL (not INVALID).

**hw4: NO_PEER again, with the join's scan FINDING the host.**  auto003 (B
after Y): `adhoc_join_scan found=1 tries=3`, then both consoles up in
GBLNK7 at rx=0 for 30 s; auto008 passed the same way.  The scan-first fix
scanned, TORE the stack down, and then called sceNetAdhocctlConnect(name)
-- which runs its own quick scan and creates the group when that misses
the host.  So the join still created a second GBLNK7 whenever its connect
scan missed (2 of 3 relaunches).  Now every join (harness and player) scans
and calls sceNetAdhocctlJoin() on the scanned BSS in the same bring-up: a
join can no longer create a group.  Never seen: ADHOC_ERR_NO_GROUP ("Room
... not found").  Both consoles log `adhoc_ctl channel= bssid= ctl_peers=`
at bring-up and every heartbeat, so a split cell is visible in one line (two
BSSIDs) -- PPSSPP: same BSSID, ctl_peers=1 on both.

## 12. Hardware run hw5 (2026-09-30): NO_PEER gone, player defaults

One unattended hw_loop session (the consoles woke from the handoff park),
order YBJHNDUX x3, EBOOT 0fa77fc3 (b0f6c4d, clean), PSP-3000 host / PSP-1000
join.  `score_rig.py`: **24/24 PASS, NO_PEER 0, one ad-hoc cell confirmed in
24/24**.  Every run is a relaunch; J starts the host's radio 20 s after the
join's, H the join's 10 s after the host's, N both together.

What made it so, after hw4 (b417514 onwards):
* the join scans and JOINS the scanned BSS (`sceNetAdhocctlJoin`); it can no
  longer create a second group of the same name (hw4 auto001/003);
* the scan is a TIME budget (90 s harness, 15 s player), not 20 tries: the
  PPSSPP smoke showed 20 scans can be over in 10 s, less than the hw4 eject
  skew (20 s).  hw5: the join found the group in 1-6 scans, 0.85-7.4 s;
* the scorer names a NO_PEER (NO_GROUP / SPLIT_CELL / SAME_CELL /
  CELL_UNKNOWN) and requires one cell in every run.
* rig: hw_loop and setup_gblink_cards.py write only into a FRESH USB window
  (WINDOW.TXT token seen to change, >= 30 s left); a parked console's
  window is of unknown age.  PPSSPP rig: `run_rig_ppsspp.sh` runs a whole
  order as parallel pairs in private namespaces; with LAUNCH=together the
  join must wait for the relay server inside instance 1 (EBADF otherwise --
  an emulator artifact; a speculative stack re-arm for it was reverted).

**Player defaults** (hw5 runs them; B is the strict control): scanline
batching ON (154), redundant input copies 0, pace_slot0 off.  B vs D, same
content, 54 core_prof windows each:

| | host core mean / median | join core mean / median | emu fps host/join |
|---|---|---|---|
| B strict | 13.05 / 13.74 ms | 13.13 / 14.02 ms | 56.4 / 56.3 |
| D batching | 12.41 / 12.08 ms | 12.55 / 12.10 ms | 56.4 / 56.3 |

Saves byte-identical to the reference in both.  Stalls per run: B 80-109
host / 35-50 join, D 112-122 / 57-82 -- same order, not better; the gain is
CPU headroom, which the owner felt as smoother (hw4).  Batching stays on.

**Transfers** (bulk lane, radio-limited: ringdrop 0, bulk_drop 0): 8 MiB to
the 1000 25.7-32.0 s, 2 MiB 8.9-11.1 s, 1 MiB 4.0-7.4 s.  Accepted by the
owner as-is: no 128-slot ring, no compression.

**The Y ending** ("Link ended" on the 1000 long before the 3000), each
console's own clock, three runs alike:

| | end_at > final | final > done | linger > close | close > resume | end_at > resume |
|---|---|---|---|---|---|
| host 3000 | 21.54 s | 32 ms | 122 ms | 1.59 s | 23.30 s |
| join 1000 | 21.47 s | 108 ms | 353 ms | 0.27 s | 22.20 s |

The 21.5 s is Y's own: the synthetic cart writes its RAM every frame, so
the end is deferred 8 times (SRAM_QUIET) and forced, at the host's 45 fps.
The gap the owner saw is `close > resume`: the host restarts its single
machine on the 8 MiB cartridge (1.6 s) while the join restarts Red (0.27 s);
~1.1 s between the two screens.  Real 1-2 MiB carts: 0.46 s on both.  Left
as is (it is the cartridge read; a real game does not rewrite its RAM
every frame).

The hw5 logs, rig and owner steps are not kept in the repo.

## 13. The instruction cache: why DUAL costs what it does (2026-09-30)

`docs/CACHE-MAP.md`.  The Allegrex I-cache, MEASURED on both consoles (rig
arm I, `logs/cache1`): 16 KiB, 2-way, 64 B lines, a hit 9.9 ns, a miss
+215 ns.  The twin trace of the Gen 2 trade, replayed through that cache:

| | misses/frame | ms/frame |
|---|---|---|
| dual, strict | 36,307 | 7.82 |
| dual, batching 154 | 31,227 | 6.72 |
| copy A alone | 11,428 | 2.46 |
| conflict misses (what a layout could remove) | 760 | 0.16 |

So the second machine costs mostly through the cache (~5.4 ms of hw2's
~6.85 ms DUAL-SOLO gap, by this estimate), and that cost is capacity: each
core's hot code is 30-35 KiB (90 % of executions) against a 16 KiB cache.
Code layout can win back at most ~0.16 ms a frame; shifting, hot-packing
and colouring were all tried in simulation and none did better.  No layout
work: it would need a soak gate for ~1 %.  Fewer switches between the cores
is what pays, and that is batching (on by default).

## 14. Smoothness: steady 60 fps without crackle (claude/gblink-perf, 2026-10-02)

The owner's hand-played Gen 1 trade on the 3.1.0 beta (EBOOT 41361020,
PSP-1000 <-> PSP-3000): it worked, but "a bit laggy, drops frames here and
there, some crackly audio during frame drops".  Three possible causes were
separated before anything changed: (a) the two machines plus the session
cost more than 16.7 ms on the console, (b) lockstep stalls (the partner's
buttons late), (c) how a late or lost frame reaches the audio ring.

**In one screen.**
* (a) CPU dominates.  20-28 % of session frames over budget, a third to a
  half late, 57.9 fps; quiet scenes lost one frame a second to a 15 ms
  sync hash, busy scenes ran 16-18 ms (the two cores' I-cache misses).
* (b) Lockstep stalls are 0.4-1 % of frames, mostly made by (a); the rest
  is radio loss waiting out netdrv's 50 ms RTO.
* (c) The crackle is (a) and (b) reaching an audio ring that a session
  started EMPTY, with every gap a click.
* Fixed: word sync hash every 240 frames; the GB core at -Os (I-cache);
  stall absorption; resend requests on the bulk lane; session audio
  (cushion, fades, a PI rate follow, a latency bound); EVT gblink_win.
  Deterministic: 40/40 simulated sessions with every hash identical, 40/40
  PPSSPP pairs PASS, single-player GB/GBC/GBA frame- and audio-identical.
* The hardware must price -Os (D vs O) and confirm the rest: staged as one
  rig app per console, 21 runs (section 14.7).

### 14.1 Attribution (hardware logs hw2-hw5, re-read)

hw5 (EBOOT 0fa77fc3: the 3.1 beta's GB link code), Gen 2 trade, D = 4,
PSP-3000 host / PSP-1000 join, three runs per arm, only the windows while
the machines run (`tools/gblink/perf_report.py ../gb-dual-out/logs/hw5`):

| arm | console | fps | work ms | over 16.74 ms /1000 | missed vblank /1000 | stalls /1000 | longest stall |
|---|---|---|---|---|---|---|---|
| B strict | 3000 | 58.0 | 13.45 | 236 | 504 | 8.7 | 8 |
| B strict | 1000 | 58.0 | 13.70 | 244 | 551 | 3.9 | 11 |
| D batching | 3000 | 57.9 | 12.62 | 211 | 347 | 10.5 | 12 |
| D batching | 1000 | 57.9 | 12.81 | 204 | 359 | 6.3 | 11 |
| U live link | 3000 | 57.8 | 12.96 | 284 | 432 | 6.8 | 11 |
| U live link | 1000 | 57.9 | 13.18 | 280 | 443 | 3.9 | 5 |

hw2's Gen 1 trade (A, the owner's case, strict): 57.7 fps, 198-213 over
budget and 344-373 late per 1000, windows down to 47.8 fps.  The two
consoles behave alike: same CPU and clock (333 MHz); the 1000 is only
smaller in memory.

**(a) CPU dominates.**  A fifth of all frames is over budget and a third to
a half miss their vblank, against 0.4-1 % stalls.  The 276 running windows
(600 frames each) of hw5's B/D/U come in two shapes:

* **Quiet scenes** (work < 12 ms, 43 % of the windows): still 10.9 frames
  per 600 at 20-34 ms (median 11; window max median 26.8 ms) -- one every
  60 frames: the periodic **sync hash**, 64-bit FNV-1a by BYTE over both
  machines' memory (~160 KiB for two Crystals: a 64-bit multiply per byte
  on a 32-bit CPU, ~15 ms), plus the cartridge-RAM CRC-32 (also by byte,
  over a copy) on the same frame.  These are the "drops frames here and
  there": one hitch a second in scenes that otherwise fit with 5 ms spare.
* **Busy scenes** (57 %: the trade room, the trade animation, saving):
  mean work 15.3 ms, 209 of 600 frames at 16.7-20 ms; the heaviest third
  average 16.45 ms with half their frames over budget.  docs/CACHE-MAP.md
  showed most of the machines' cost is I-cache misses (each core's hot code
  at -O3 is twice the 16 KiB cache).

**(b) Stalls are minor and mostly made by (a).**  2.0 per window in quiet
scenes, 5.7 in busy ones, 7.4 in the heaviest: a console behind on CPU
makes its partner wait.  netdrv retransmits 2-20 times a window (1-3 %
radio loss); a lost INPUT waits for the 50 ms RTO behind the ordered
channel.  D = 4 is 67 ms against a 15-20 ms round trip.

**(c) The crackle is the audio path's reaction to (a) and (b).**  hw5 has no
ring telemetry (EVT audio_ring came later), so the mechanism from the code:
the connecting screen empties the ring (ring_r = ring_w), the session starts
at ZERO, and the ring only grows by the 59.94-vs-59.73 Hz surplus (~120
frames a second).  Below 59.73 fps, or on any stall (no emulation, no
samples), a 23 ms output chunk finds the ring short and is padded with
zeros: a step to silence and back, a click per gap -- at 57.9 fps, 3 % of
the time.  PPSSPP reproduces it once a session is made CPU-bound and the
radio jittery (14.3): base build, ~2 % of the output silence, ring minimum
1 frame.

### 14.2 What changed, and what carries over to the GBA link mirror

| fix | where | what | GBA mirror (claude/gbalink-session) |
|---|---|---|---|
| word sync hash | gbcore_tgbdual.c `sync_bytes` | two 32-bit lanes by word (FNV-1a by word + rotate-add), endian-independent, ~6x fewer cycles; protocol 6 (a 5 is refused at HELLO) | already hashes by word (gbalink_core.c lk_hash), not needed |
| hash every 240 frames | fe_host.c `l.hash_interval` | was 60; the FINAL hash still guards every commit; score_session.py reads `hash_every` | yes, with its own key: `gblink_hash_every` defaults to 60 there and its hash covers 2 x 384 KiB (~4 ms a check) |
| cartridge-RAM check by word | fe_gblink.c `ram_hash` | was CRC-32 by byte over a copy, every 30 frames | already by word (m_cart_crc, 2 x 128 KiB every 30 frames) |
| GB core at -Os | psp/Makefile `TGB_OPT` | 14.4 | no: gpSP's hot code is its translation cache |
| stall absorption | fe_gblink.c `fe_gblink_platform.wait`, fe_host.c `gbl_wait`, main_psp.c `plat_gblink_wait` | 14.4 | yes, the session layer is shared: the retry loop needs gbalink-session's `s->m ? s->m->advance(...) : gbdual_advance(...)`, gba_cable_start `p.wait`, gba_cable_frame the bulk drain and `gbl_waited_us` |
| resend requests | fe_gblink.c `BK_NACK`/`BK_RESEND`, `gblink_nack` | 14.4 | yes, as is (fe_gblink + the bulk lane both builds share) |
| session audio | main_psp.c `gblink_audio_*` | cushion, soft underruns, rate follow | yes: platform code on the ring every core feeds; the GBA cable service must call gblink_audio_session/frame, and ADR-0027's pace_audio_step must not also steer the step during a mirror session |
| EVT gblink_win | fe_gblink_take_window, main_psp.c | 14.5 | yes (fe_gblink); the GBA pair's own stats line stays |

### 14.3 PPSSPP: what it can and cannot say

PPSSPP runs the two Game Boys ~2.5x faster than a PSP and models no
I-cache, no WLAN thread scheduling and no PSP audio timing, so a plain
PPSSPP session never shows (a) at all (5.1 ms a frame, 0-2 stalls, no
silence).  The harness key `gblink_cpu_x100` (harness builds only) makes
each emulated frame cost N/100 of its measured session step by
busy-waiting: x250 gives 12.0-12.8 ms of work a frame, the hardware's mean.
It scales what PPSSPP measures, so it reproduces the shapes (the hash
spike, CPU-bound windows, partner stalls, ring underruns) but not the
I-cache: **-Os looks worse in PPSSPP** (more instructions, no miss
penalty).  So the PPSSPP A/Bs use the fixes built at -O3 ("after, -O3"),
and only the hardware can price -Os.  **None of these numbers is hardware
timing.**

Two PPSSPP consoles (tools/gblink/run_stage_pair.sh logic, Xvfb :133/:134,
host as the PSP-2000 model, join as the PSP-1000), the rig's staged arms,
each pair scored by score_session.py (every run below PASSes: both DONE,
every hash matched, saves equal to the reference).  "before" = 77aaabb (the
3.1 beta code) with only the `gblink_cpu_x100` knob added; "after -O3" =
the fixes built with TGB_OPT=-O3 (see above).  host / join:

| scenario | build | work ms | over 16.74 /1000 | late /1000 | stalls /1000 | longest | absorbed /1000 | ring min | silence % | gaps /min | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Gen 2 trade, plain PPSSPP | before | 5.1 / 5.1 | 0 / 0 | 0 / 0 | 0.0 / 0.2 | 1 | - | 1881 / 1901 | 0.00 / 0.00 | - | PASS |
| Gen 2 trade, plain PPSSPP | after (first build) | 5.1 / 5.2 | 0 / 0 | 0 / 0 | 0.0 / 0.0 | 0 | 0 / 348 | 2374 / 2382 | 0.00 / 0.00 | 0 / 0 | PASS |
| Gen 2 trade, x250 | before | 12.8 / 12.8 | 44 / 45 | 198 / 197 | 0.0 / 0.7 | 1 | - | 1265 / 1071 | 0.00 / 0.00 | - | PASS |
| Gen 2 trade, x250 | after -O3 | 12.6 / 12.6 | 34 / 31 | 81 / 87 | 0.0 / 0.1 | 1 | 0 / 0 | 1879 / 1699 | 0.00 / 0.00 | 0 / 0 | PASS |
| Gen 2 trade, x250 | after -Os (PPSSPP artifact) | 14.7 / 14.7 | 347 / 343 | 450 / 446 | 0.0 / 0.4 | 1 | 0 / 0 | 1 / 1 | 1.81 / 1.82 | 319 / 321 | PASS |
| Gen 1 trade, x250 | before | 12.0 / 12.0 | 33 / 34 | 131 / 126 | 0.0 / 1.0 | 1 | - | 2050 / 1692 | 0.00 / 0.00 | - | PASS |
| Gen 1 trade, x250 | after -O3 | 11.3 / 11.4 | 14 / 14 | 33 / 31 | 0.0 / 0.0 | 0 | 0 / 0 | 2300 / 2543 | 0.00 / 0.00 | 0 / 0 | PASS |
| Gen 1 trade, x250 | after -O3, policies off | 11.7 / 11.7 | 26 / 17 | 58 / 39 | 0.0 / 0.3 | 1 | 0 / 0 | 2230 / 2103 | 0.00 / 0.00 | 0 / 0 | PASS |
| Gen 2 + 3 % loss, x250 | before | 12.4 / 12.3 | 44 / 42 | 185 / 179 | 51.5 / 51.5 | 15 | - | 1 / 1 | 4.20 / 4.18 | - | PASS |
| Gen 2 + 3 % loss, x250 | after -O3, no NACK yet | 12.2 / 12.2 | 32 / 31 | 76 / 78 | 25.1 / 25.0 | 10 | 49 / 45 | 1 / 1 | 1.08 / 1.06 | 56 / 74 | PASS |
| Gen 2 + 3 % loss, x250 | after -O3 | 12.3 / 12.3 | 33 / 33 | 76 / 79 | 15.9 / 16.0 | 5 | 28 / 22 | 1 / 1 | 0.01 / 0.00 | 1 / 1 | PASS |
| Gen 2 + 30 ms/15 ms jitter, x250 | before | 12.4 / 12.4 | 42 / 41 | 163 / 166 | 24.6 / 24.6 | 3 | - | 1 / 1 | 1.86 / 1.86 | - | PASS |
| Gen 2 + jitter, x250 | after -O3 | 12.3 / 12.2 | 32 / 31 | 79 / 82 | 17.6 / 17.6 | 3 | 4 / 2 | 1 / 1 | 0.15 / 0.15 | 23 / 22 | PASS |
| Gen 2 trade, x300 (CPU-bound) | before | 15.4 / 15.4 | 349 / 355 | 582 / 576 | 1.9 / 1.2 | 8 | - | 1 / 1 | 3.15 / 3.16 | - | PASS |
| Gen 2 trade, x300 | after -O3, P-only 3 % follow | 15.3 / 15.3 | 375 / 378 | 528 / 512 | 0.7 / 0.4 | 3 | 1 / 0 | 1 / 1 | 2.26 / 2.27 | 362 / 364 | PASS |
| Gen 2 trade, x300 | after -O3 | 15.3 / 15.3 | 376 / 381 | 536 / 530 | 0.0 / 1.1 | 5 | 0 / 1 | 1 / 1 | 1.71 / 1.66 | 251 / 248 | PASS |

What PPSSPP says:
* **The hash spike is real and gone**: with the CPU scaled to the PSP's
  cost, late frames fall from 197 to 84 per 1000 (Gen 2) and from 128 to 32
  (Gen 1); the hash and interval do most of it (Gen 1 with the runtime
  policies off: 39-58).
* **Loss** is where resend requests pay: at 3 % loss, stalls 51 -> 16 per
  1000, the longest run 15 -> 5 frames, silence 4.2 % -> 0.01 % (one gap a
  minute).  Without the NACK it was still 25 per 1000 in runs of 10.
* **Jitter** beyond the input delay (30 ms + 15 ms each way: most of D=4's
  67 ms spent in flight) still stalls ~18 per 1000 -- the lockstep's own
  limit at D=4 -- but silence falls 1.86 % -> 0.15 %.
* **CPU-bound** (x300: 15.4 ms a frame, the hardware's busy scenes before
  -Os) is not fixed by any policy: 530 of 1000 frames still late; the
  follow cuts silence 3.2 % -> 1.7 %, but the windows that drop to 54 fps
  are past its 6 % bound.  That case is what -Os is for, and only the
  hardware can price it: in PPSSPP -Os costs 2 ms MORE (14.7 vs 12.6 ms at
  x250), the instructions without the cache.

### 14.4 The fixes in detail

**The GB core at -Os** (`psp/Makefile TGB_OPT ?= -Os`; `TGB_OPT=-O3` builds
the control).  The twin of the rig's Gen 2 trade (tools/cachemap: mipsel
GCC 12, the two cores' code at its own addresses through a 16 KiB 2-way
64 B LRU cache, 10,870 frames, batching 154):

| flags | instructions / frame | I-cache misses / frame | text a copy | model ms (CPI 1 + 215 ns a miss) |
|---|---|---|---|---|
| -O3 (MasterBoy, 3.1 beta) | 1.608 M | 19,349 | 148 KiB | 8.98 |
| -O2 | 1.616 M | 15,408 | 140 KiB | 8.16 |
| -O2, inline-limit 12 | 1.665 M | 10,351 | 101 KiB | 7.22 |
| **-Os** | 1.855 M | **5,687** | **85 KiB** | **6.79** |
| -O3, strict alternation | 1.603 M | 22,189 | | 9.58 |
| -Os, strict alternation | 1.850 M | 7,734 | | 7.21 |
| Gen 1 trade, -O3 / -Os | 1.477 / 1.698 M | 12,276 / 5,981 | | 7.07 / 6.38 |

At a pessimistic CPI of 1.4 the Gen 2 gain is still 1.9 ms a frame
(10.9 -> 9.0) and Gen 1's 0.4 ms.  -O1, -O2 without inlining and -Os with
more inlining were no better.  The code is integer-only and UBSan-clean:
gbcore/tests/bitident.c (picture, sound, state and battery image every
frame) on Crystal, Red, Blue and Kirby, 3000 frames each, is identical -O3
vs -Os on x86 and on mipsel under qemu.  GBA is untouched (LAYOUT_PIN keeps
its hot path where it was); the EBOOT loses ~126 KiB of GB text.

**Stall absorption.**  A step held up by the partner's buttons used to
return STALL at once: the display frame was lost even if the input arrived
1 ms later.  Now fe_gblink_step asks the platform's `wait()` -- pump the
network once (the input is delivered inside it) and say whether this
display frame still has room -- and retries the advance, which resumes
exactly where it stopped.  The PSP's budget: wait while this frame's vblank
has not passed and elapsed + the frame's usual work (an EMA) + 1.5 ms of
present still fit in 16.68 ms, never more than 12 ms; between pumps it
sleeps 100 us when nothing is queued.  A console that runs ahead now sits
at the edge and waits a fraction of a millisecond each frame (PPSSPP join,
unscaled: 543 of 600 frames waited, 0.3 ms each) instead of losing a whole
frame now and then.  The machines run on the inputs alone, so absorbing
changes when a step returns, never what it computes (14.6).

**Pickup at need** (2026-11-21, `fe_gblink_platform.pickup`; no key: it
runs wherever absorption's hook is wired).  The partner's buttons usually
reach the receive queue while the machines run, and only the loop-top pump
drains it, so a step held up mid-frame often had them in the queue already
(55 % of delay-3 stalls, 71 % on the guest; builds/link-latency-out).
Before asking wait(), the step now pumps once if netdrv says something is
pending (`netdrv_poll_needed`, no syscall) and resumes at once if that
brought the buttons, whatever the frame budget says.  Otherwise wait()
decides exactly as before.

**Resend requests** (`gblink_nack`, default 1).  With the CPU fixed, loss is
what is left: PPSSPP at 3 % loss still lost 25 frames per 1000 in runs of up
to 10, because a lost INPUT waits behind netdrv's cumulative acknowledgement
for its retransmission timeout (>= 50 ms) with everything after it.  Now a
display frame lost waiting for the partner's frame f sends `BK_NACK f` on the
bulk lane (again after 40 ms if still waiting), and the partner answers with
what it has from f on, up to 16 frames (`BK_RESEND`), taken like any copy.  A
partner that is merely behind has not made f yet and answers nothing, so the
cost is a 9-byte datagram per lost frame, nothing in steady play.  The END
protocol relied on END_AT preceding, on the ordered channel, every input
that could carry the guest past the end frame; a resend can carry such
inputs, so the host's resends carry the agreed end frame and the guest
applies it before the buttons (a later END_AT for the same frame is ignored,
a different one is a protocol failure).  In det_matrix the guest learned the
end from a resend in 3 of 40 sessions, with identical results.

**Session audio** (main_psp.c, GB/GBC sessions only; single-player output is
the old path byte for byte):
* *Cushion*: when the machines start, the ring gets 3072 frames (94 ms) of
  silence, so the first stalls and late frames draw on a reserve instead of
  an empty ring (single-player sits at 4096, 125 ms, after ~35 s anyway;
  3072 stays clear of that high water).
* *Soft underruns*: a dry ring holds the last sample and fades it to zero
  over 64 output frames (1.45 ms), and the stream fades back in: a dropout
  instead of two clicks.
* *Rate follow*: the resampler step follows the ring, holding it at the
  cushion: a PI controller on a 16-frame average of the level (the level
  saw-tooths by a 761-frame output chunk), 2 % proportional at full scale
  plus an integral that builds 1 % in 6 s at half the cushion's error,
  bounded to -6.0 % .. +0.5 %.  A session that holds 58 fps for a while
  plays ~2.9 % slow -- ADR-0027's trade for GBA sessions, a pitch drop
  instead of gaps, while the game itself runs that much slower -- and
  +0.36 % takes up the 59.94/59.73 Hz surplus,
  so the high-water nudge (a whole extra vblank, i.e. a stall for the
  partner) no longer fires during a session (it is skipped while the follow
  runs) and the session runs at the vblank's 59.94 fps.  The gains come
  from `tools/gblink/audio_follow_sim.py`, which models this ring: a faster
  integral (1 % a second, the first version) rang in a 12-20 s limit cycle
  swinging the pitch by up to 2.5 %.  Gaps a minute / pitch standard
  deviation, by (fps, share of frames lost to stalls):

  | follower | 59.94, 0 | 59.94, 0.5 % | 59.94, 1 % | 59.94, 3 % | 58, 0 | 58, 1 % | 57, 0 | 56, 0 |
  |---|---|---|---|---|---|---|---|---|
  | none (cushion 2048; the beta started at 0) | 0 | 29 | 70 | 183 | 232 | 263 | 319 | 389 |
  | P only, 3 % | 0 | 4 | 19 | 128 | 182 | 219 | 281 | 373 |
  | P 1 % + I 1 %/s, 3 %, cushion 2048 | 0 / 0.01 % | 4 / 0.84 % | 12 / 0.97 % | 54 / 1.03 % | 3 | 101 | 281 | 373 |
  | P 2 % + I 1 % in 6 s, 3 %, cushion 3072 | 0 / 0.01 % | 0 / 0.46 % | 0 / 0.56 % | 25 / 0.67 % | 4 | 101 | 279 | 371 |
  | **the same, 6 % (shipped)** | 0 / 0.01 % | **0** / 0.46 % | **0** / 0.56 % | **15** / 0.94 % | **4** | **9** | **18** | 122 |

  (The bound only binds in a sustained CPU-bound stretch; below ~56 fps
  only the CPU fix helps.)
* *Latency bound*: with the nudge off, samples that would queue past the
  high water + 1024 (156 ms) are dropped while the follow runs -- a skip,
  once.  PPSSPP showed why: right after the connecting screen a console
  runs a few frames flat out to catch its vblank target up, ~3000 frames
  into the ring at once, which the +0.5 % then took 90 s to drain.
* Keys (config.ini = the player's kill switch, the harness ini overrides):
  `gblink_absorb` (1), `gblink_nack` (1), `gblink_audio_cushion` (3072;
  0 = start empty), `gblink_audio_follow` (60 = 6.0 %; 0 = fixed rate and
  the old nudge).  Logged as `EVT gblink_smooth` at boot and
  `EVT gblink_audio on` per session.

**Not done, and why.**
* *Adaptive input delay.*  The lag the owner felt is the frame drops: the
  logs show no long input latency (D = 4 is 67 ms), and stalls are 0.4-1 %
  of frames, mostly CPU-made.  If it is wanted later, it needs no
  agreement: inputs travel keyed by the machine frame they belong to, so
  each console can lower its OWN send-ahead and the receiver never reads D
  -- but only below CONFIG's delay, because the end protocol (END_MARGIN +
  2 x delay; "the guest cannot pass the host's frame + delay without inputs
  sent after END_AT") rests on it.
* *An unreliable input lane for every frame.*  Redundant input datagrams
  cost a PdpSend a frame (hw2: +0.7-1.8 ms) for 1-3 % loss; the resend
  request below spends that only when a frame is actually lost.
* *Batching while the cable is busy.*  linkplay shows 74 % of the Gen 2
  trade's scanlines unbatched because a machine holds SC bit 7 (armed on
  the external clock), not because of serial traffic (quiet 60 -> 1 frame:
  25.4 -> 25.9 % batched).  Batching through an armed port needs a new
  exactness argument for the cut logic (on_serial_clocked reads the peer's
  arm state mid-batch); after -Os the switches cost far less anyway.
* *The Media Engine running the headless machine* in parallel during
  batched stretches: per-scanline ME sync and cache coherency are a project
  of their own, and PPSSPP cannot run the ME.

### 14.5 EVT gblink_win (every 10 s while the machines run)

    gblink_win dt_ms loops frames stalls st_ep st_max absorbed abs_ms abs_max_us
      wait_ms work_us=mean/max core_us=mean/max step_us=mean/max
      hash_us=n/max crc_us=n/max ovb late delay lead_min lead_mean_x10
      rxq_max ar_min ar_mean under starved step_ppm nacks resends

frames = display frames emulated; stalls/st_ep/st_max = frames lost to a
late input, their episodes, the longest run; absorbed = late inputs waited
for and still on time; work = the loop's frame work (vblank wait and
absorbed waits out); core = both machines (hooks and waits out); step = the
whole session step; hash/crc = the periodic sync hash / cartridge check;
ovb/late = frames over 16.74 ms of work / that missed their vblank; lead =
partner frames in hand when its machine starts a frame; rxq_max = netdrv
datagrams queued at a pump; ar_* = audio ring level (frames of 32768 Hz),
under = gaps, starved = output frames of silence, step_ppm = the resampler's
offset; nacks/resends = resend requests sent / answered.  `tools/gblink/perf_report.py` tables it per arm and console (and
reads the older 600-frame lines for builds without it).

### 14.6 Determinism

Absorption changes only when a step returns; the hash, -Os and the audio
policy change no machine state.  The evidence:

* **Two consoles simulated** (`tools/gblink/det_matrix.sh`, sesssim: both
  complete fe_gblink stacks over a simulated network): Gen 1 trade, Gen 2
  trade, Gen 1 battle and the Gen 2 live link x five radios (clean; jitter
  6 and 20 frames; 3 % and 5 % of the ordered channel resent late, holding
  everything behind it, the latter also losing 5 % of bulk-lane datagrams)
  x the policies off/on (absorption + resend requests) = **40/40 sessions
  DONE on both consoles with equal final hashes**, cartridge RAM equal to
  the one-process
  reference in every one.  Every periodic sync hash is the same at the same
  frame in all ten runs of its fixture (110 + 185 + 119 + 139 frames, 0
  differing), and the live link -- the only one whose end the script, not
  the clock, decides -- ends on 8344:2e416105e0317b19 in all ten.  (The
  trades' end frame follows a wall-clock end request, so it moves with the
  stalls; the hash at any frame does not.)  In 3 of the 40 the guest
  learned the end frame from a resend before END_AT.  The policies in the
  same matrix: Gen 1 at 3 % resends 571 -> 271 stalled ticks, jitter 6
  5625 -> 4397.
* **Host suite** `gb`: a new session test runs absorption off and on over
  4 % resends + jitter: 24 sync hashes identical, stalls 1095 -> 809.  All
  30 host suites pass.
* **One process**: make_references.sh on the final tree gives saves
  byte-identical to the hw5 references (gen1/gen2/battle/live); only the
  live link's final hash VALUE changed (the new hash function):
  8344:286f27da9b8b54cf -> 8344:2e416105e0317b19, rig reference updated.
* **-Os vs -O3**: bitident traces identical on x86 and mipsel (14.4).
* **Two PPSSPP consoles**: every pair run for this work -- 40 sessions,
  before and after builds, Gen 1/Gen 2 trades, plain and CPU-scaled, 3 %
  loss, 30/15 ms jitter -- PASSes score_session.py: both DONE, every periodic hash
  matched, equal final hashes, each console's save equal to the
  one-process reference, the trades exact.  The staged hardware rig itself
  (all seven arms, the base and -O3 binaries
  beside the new one) ran in PPSSPP and score_rig.py scored it **7/7 PASS**,
  one ad-hoc cell in every run.
* **Single-player, base (77aaabb) vs final, PSP builds in PPSSPP**: GB/GBC
  `vhash` every frame and the audio hash (every sample the core produced)
  over 3600 frames of scripted play are identical on Red, Blue, Kirby and
  Crystal (Crystal with its cartridge clock frozen: on the live clock two
  runs a minute apart differ on the title screen's clock for frames
  426-701, either build).  GBA: the video oracle
  (tools/e2e/run_video_regress.sh, Emerald) 1091 frames pixel-identical.
  The single-player audio output path is unchanged (the soft underrun,
  follow and latency bound are off outside a session).

### 14.7 Hardware A/B (staged, not yet run)

The staged rig and its owner steps (not kept in the repo): one rig app per
console (GBADHOC-GBLINK, hw_loop's USB handoff), PSP-3000 host + PSP-1000
join, order GAFZDOM x 3 (21 runs, ~1 h 45 min), three binaries side by side
(prepare_gblink_rig.py `--base-build` / `--o3-build`; MANIFEST lists each
arm's EBOOT crc32, which score_rig.py checks):

| arm | trade | binary | question |
|---|---|---|---|
| G | Gen 1 (the owner's) | before (77aaabb, 3.1 beta code) | the baseline |
| A | Gen 1 | after (all fixes, player defaults) | A vs G: the whole change |
| F | Gen 1 | after, absorb/nack/cushion/follow off | A vs F: the runtime policies; F vs G: hash + -Os |
| Z | Gen 2, batching | before | the baseline |
| D | Gen 2, batching | after | D vs Z |
| O | Gen 2, batching | after built -O3 | D vs O: -Os alone (PPSSPP cannot say) |
| M | Gen 2 + 30 ms / 15 ms jitter | after | absorption + NACK on a worse radio |

What it must confirm (`perf_report.py logs/perf1`, every run PASS first):
1. A vs G, D vs Z: frames over budget and late frames per 1000 fall; the
   quiet scenes lose no frames to the hash (hashmx ~1-2 ms, once per 4 s).
2. D vs O: `core` falls by ~1-2 ms a frame in the busy windows (the twin
   says 1.9-2.2 ms for Gen 2, 0.4-0.7 for Gen 1).  If it does not, -Os is
   not paying for its extra instructions on the real cache: TGB_OPT back to
   -O3 (one line, the rest stands).
3. A vs F: silence share and gaps a minute near 0 with the policies,
   absorbed > 0, stalls per 1000 lower, longest stall shorter.
4. M: still PASS; NACKs answered (resends > 0) and short stall runs.
5. By hand: the release build of the same commit is staged as
   GBADHOC-GBLINK-PLAY ("GBLplay") -- one Gen 1 trade between the 1000 and
   the 3000, by eye and ear.

Binaries (all clean builds; MANIFEST.txt has every md5):

| what | commit | EBOOT md5 |
|---|---|---|
| after, harness | 3665147 | a5f704ea34cdf8aaf4c647054e753a77 |
| after -O3, harness (TGB_OPT=-O3) | 3665147 | c4a666d8e42de02ee7f72062882a16fe |
| before, harness | 77aaabb | 6a240430ea33249b9e335f3432fc6376 |
| **player: release, "GBAdhoc 3.1.0 beta"** | 3665147 | **6d1ca4df0357a920d0466abaea15ca6d** |

The release ships beside the beta's own modules, unchanged:
`gbadhoc_me.prx` 19ae57b6d21f5d1c3545bf07ff516a4c and
`psp/home/gbadhoc_home.prx` d3434f4fd02260583c2e97ce7fe757e6
(the release build itself is not kept in the repo).

### 14.8 Hardware run perf1: -Os is slower on the PSP; the model was wrong

perf1 (2026-10-02, the rig of 14.7 on 3665147, PSP-3000 host / PSP-1000
join, GAFZDOM x 3 after a wake-up run): **21/21 PASS** -- determinism held on
hardware -- but the after builds cost MORE.  (The perf1 logs are not kept in
the repo.)  Read with the same instruments in every build (perf_report.py
now uses only frame_hist / core_prof / sess_cost / fps / audio_ring; the
first report mixed EVT gblink_win figures into the after builds' columns, and
cut sessions at gblink_end_at, which dropped the trade's busiest window from
some runs: one binary read 67 and 158 over budget per 1000).  Means of three
runs, host / join:

| arm | build | core ms | over budget /1000 | late /1000 | >= 20 ms /1000 | stalls /1000 | silence % |
|---|---|---|---|---|---|---|---|
| G | Gen 1, before | 11.43 / 11.13 | 157 / 150 | 262 / 247 | 22 / 22 | 6.8 / 5.2 | 1.99 / 1.89 |
| A | Gen 1, after (-Os) | 12.80 / 12.84 | 190 / 179 | 334 / 327 | 100 / 90 | 5.7 / 2.9 | 2.48 / 2.32 |
| F | Gen 1, after (-Os), policies off | 12.79 / 12.77 | 189 / 175 | 322 / 317 | 99 / 78 | 5.8 / 5.5 | 3.17 / 2.87 |
| Z | Gen 2, before | 12.59 / 13.09 | 195 / 232 | 360 / 407 | 29 / 35 | 26.5 / 5.5 | 3.28 / 3.51 |
| D | Gen 2, after (-Os) | 13.25 / 13.80 | 196 / 265 | 383 / 470 | 25 / 44 | 47.0 / 3.6 | 2.54 / 3.01 |
| O | Gen 2, after built -O3 | 12.96 / 12.59 | 235 / 214 | 359 / 337 | 24 / 21 | 4.2 / 6.5 | 2.49 / 2.30 |

Window-matched core (every run plays the same scripted trade, so window i is
the same scene; `perf_report.py --vs`), the least noisy comparison:

| comparison | host | join |
|---|---|---|
| A vs G (Gen 1, after -Os vs before) | +1.37 ms | +1.71 ms |
| F vs G (policies off) | +1.35 ms | +1.63 ms |
| D vs Z (Gen 2, after -Os vs before) | +0.66 ms | +0.71 ms |
| **O vs Z (Gen 2, after -O3 vs before)** | **+0.37 ms** | **-0.50 ms** |

What it says:
* **-Os is the regression.**  -Os vs -O3 on the same code (D vs O) costs
  +0.3 / +1.2 ms; every Gen 1 after arm was -Os.  The policies cost nothing
  (F = A).  The rest of the after build at -O3 is within console noise of
  the before build (O vs Z: +0.37 / -0.50 ms).  **TGB_OPT is back to -O3**
  (6d8435d); -Os stays a switch.
* **At -O3 the after build helps what it targets** (O vs Z, Gen 2): the
  host's stalls 26.5 -> 4.2 per 1000, silence 3.3-3.5 % -> 2.3-2.5 %, >= 20 ms
  frames 29-35 -> 21-24 per 1000.  Not enough: busy scenes are still CPU-
  bound (over budget ~200-270 per 1000 either way), so the audio still gaps.
* **The hash**: the after builds' longest periodic hash is 3.4-4.6 ms, not
  the ~1-2 ms estimated (the D-cache: ~160 KiB swept per check; the estimate
  counted instructions only).  The legacy arm of perf2 measures the old one
  directly.

**Why the models mispredicted** (so neither is trusted for CPU again):
* *The twin I-cache model (14.4)* priced -Os at -2 ms: 13.6 k fewer misses x
  215 ns against 250 k more instructions at CPI 1.  The hardware says
  +0.4..+1.7 ms, so on the PSP the misses -Os removes are worth far less
  than 215 ns each (the probe's figure is a pathological same-set chain;
  sequential line fills inside a function evidently cost much less), and/or
  -Os's extra calls, spills and branches cost more than one cycle each on
  the Allegrex (no branch prediction to speak of, load-use stalls), and the
  twin is GCC 12 mipsel-linux, not psp-gcc.  It also casts doubt on
  CACHE-MAP's "most of DUAL is misses": the model fit -O3 but failed the
  first prediction it was asked for.  Code-generation and layout changes
  are decided on hardware only.
* *PPSSPP with `gblink_cpu_x100` (14.3)* scales PPSSPP's own step time by a
  constant, so it keeps PPSSPP's distribution of frame costs, which is far
  narrower than the PSP's: at the same mean (12.6-12.8 ms) PPSSPP x250 had
  44 frames per 1000 over budget, the hardware ~200.  On the PSP the busy
  scenes (I-cache-bound strict alternation, relatively much dearer than in
  PPSSPP's JIT) dominate the late frames, and the hash spike is a minor
  share; in scaled PPSSPP the hash spike was most of them, so removing it
  looked like halving the late frames.  PPSSPP + the knob stays useful for
  what it showed correctly -- determinism, the protocol, stalls and loss
  recovery under injected radio faults, the audio path's behaviour -- and
  says nothing about CPU.

### 14.9 Round 2 (perf2): the bisect, staged

The staged rig2 and its owner steps (not kept in the repo), same pair and rig
app, ~85-90 min.  One binary (6d8435d, -O3) and one telemetry for arms 1-3
and 5; the changes are switched by harness keys -- `gblink_hash_legacy` (the
beta's byte-wise hash and CRC; sesssim reproduces the beta's live-link final
hash 8344:286f27da9b8b54cf with it, and the PSP build in PPSSPP the beta's
Gen 1 final hash 5f99ac8f8603b821), `gblink_hash_every`, and the policy keys:

| arm | what | answers |
|---|---|---|
| G | Gen 1, before binary | control |
| 1 | Gen 1, beta hash every 60, policies off | 1 vs G: the plumbing and telemetry alone |
| 2 | Gen 1, word hash every 240, policies off | 2 vs 1: the hash change |
| 3 | Gen 1, all defaults | 3 vs 2: the policies |
| Z | Gen 2, before binary | control |
| 5 | Gen 2, all defaults | 5 vs Z: perf1's O vs Z again |

A change stays only if its comparison is not worse in core and better in
what it targets.

### 14.10 Hardware run perf2: what stays (2026-10-02)

perf2 (the 14.9 bisect, 6d8435d -O3, 17/17 PASS; `logs/perf2`), means of
three runs (two for Z/5), host 3000 / join 1000, same instruments throughout:

| arm | build | core ms | over budget /1000 | late /1000 | stalls /1000 | longest stall | silence % | longest hash |
|---|---|---|---|---|---|---|---|---|
| G | Gen 1, before | 11.62 / 11.11 | 154 / 147 | 259 / 246 | 5.5 / 4.3 | 29 / 31 | 1.88 / 1.74 | -- |
| 1 | after, beta hash /60, policies off | 11.50 / 11.46 | 151 / 154 | 253 / 263 | 17.5 / 5.0 | 21 | 2.45 / 2.50 | 11.7 / 11.3 ms |
| 2 | + word hash /240 | 11.15 / 10.72 | 116 / 81 | 203 / 168 | 14.4 / 4.5 | 10 / 8 | 1.80 / 1.08 | 3.1 / 3.5 ms |
| 3 | + policies (all defaults) | 10.77 / 10.85 | 84 / 81 | 172 / 163 | 12.2 / 5.7 | 6 | 0.85 / 0.91 | 3.3 / 3.7 ms |
| Z | Gen 2, before | 12.51 / 13.26 | 219 / 245 | 372 / 401 | 25.1 / 7.0 | 89 / 90 | 3.87 / 4.14 | -- |
| 5 | Gen 2, all defaults | 12.70 / 12.83 | 230 / 229 | 370 / 352 | 5.2 / 3.6 | 6 | 2.48 / 2.44 | 4.1 / 4.3 ms |

* 1 vs G: the plumbing and telemetry cost nothing (window-matched core
  -0.12 / +0.35 ms, noise).  The beta's hash measured directly: 11.3-11.7 ms.
* 2 vs 1: the word hash every 240 frames removes it: over budget 151 -> 81-116,
  late 253 -> 168-203 per 1000.
* 3 vs 2: the policies: late frames and over budget down again (to 84 / 172),
  the longest stall 10 -> 6 frames, silence 1.1-1.8 % -> 0.9 %.
* 5 vs Z (Gen 2): core +0.18 / -0.43 ms (noise); the longest stall 90 -> 6
  frames, stalls 25 -> 5 per 1000 on the host, silence 4 % -> 2.5 %; over
  budget unchanged (~230 per 1000): Gen 2's busy scenes are CPU-bound.

**Kept** (claude/gblink-perf, -O3): the word hash every 240 frames and all
four policies (absorb, nack, audio cushion/follow/trim), their config.ini
kill switches as in 14.4.  **Reverted**: -Os (14.8).

**Round 3 (Gen 2 CPU) stopped by the owner** before staging.  The started
work -- a hardware phase sampler (GBPROF_HW) and an experimental busy-cable
batching rule (`active_lines`: switch pairs a frame 115 -> 15 in the Gen 2
trade, reference saves unchanged, timing not exact) -- is parked unvalidated
on `claude/gblink-perf3-wip`.  The measurement it would build on is already on
hardware: milestone-1 (hw2) DUAL strict 12.8 ms, 8 lines a turn 10.55,
154 lines 9.9, SOLO 5.94 + SOLOH 3.53 -- the per-scanline switch is ~2.9 ms of
a 12.8 ms frame.
