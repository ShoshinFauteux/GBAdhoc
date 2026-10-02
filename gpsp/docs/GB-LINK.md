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
| HASH | sync hash of both machines every 60 slot-0 frames; mismatch = desync, stop, save nothing |
| END_REQ / END_AT | the host settles on an end frame both can reach, later while either cartridge RAM is still changing (quiet 120 frames, at most 8 deferrals) |
| FINAL | the final hashes are compared; only if equal does each PSP write **its own** save |
| ABORT | either side, with a reason |

A late input is a stall, never a divergence.  Nothing is written unless the
final hashes agree.  Harness keys: `gblink_role`, `gblink_delay` (also a
`config.ini` key, 1-30, the host's wins), `gblink_rtc_seed`,
`gblink_end_after`, `gblink_bulk`, `gblink_library`.

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

Owner steps: `builds/gb-dual-out/OWNER-STEPS.txt`.

## 6. Results so far (PPSSPP)

Every staged arm, run from `builds/gb-dual-out/rig` exactly as the cards will
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

`builds/gb-dual-out/HW1-FINDINGS.txt` has the evidence.  In short -- the link
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

Logs: `builds/gb-dual-out/logs/hw5`; rig and steps: `builds/gb-dual-out/rig`,
`OWNER-STEPS.txt`.

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
