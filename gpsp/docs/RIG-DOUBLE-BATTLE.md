# RIG-DOUBLE-BATTLE: the two-PSP FireRed double-battle rig (design, for review)

**Status (2026-09-26):** APPROVED with four amendments (§9), and BUILT through
§7 step 6 on PPSSPP.
- §10 records what the bring-up (G1) changed from the design below. Read it
  before trusting any address or step in §1.4.
- §11 is the G2 validation record; §12 is the owner handshake.
- Nothing has been written to either memory stick except the verified,
  read-only backup.

**Question the rig answers:** does the lag between clicking FIGHT and the
move menu appearing *grow over a session* in a FireRed wireless double
battle between a PSP-3000 and a PSP-1000? If it does, does the fix bound it,
without breaking the battle?

---

## 0. Context the design depends on

### 0.1 The mechanism being tested (why the rig measures what it measures)

The emulated RFU link is a closed loop of packets.

- **The host sends one packet every host frame.** In pokefirered this is
  `RfuMain1_Parent` → `rfu_LMAN_REQ_sendData`, once per frame.
- **The host receives at most one packet per frame.**
- **The client answers every host packet with exactly one packet of its
  own.** In pokefirered this is `MscCallback_Child` → `rfu_LMAN_REQ_sendData`.

So the number of packets circulating host → client → host never goes down:

- It goes up by one each time the host polls and finds nothing.
- Nothing in the protocol ever removes a packet.

Those packets *are* the round-trip latency. They sit in rfu.c's two receive
queues, costing one frame each. Several things add packets and none remove
them:

- every client frame lost against the host;
- every burst released by an ARQ retransmit;
- every stall on either console.

That is a ratchet, and it explains the owner's report exactly: latency is
"lowest right after connecting and grows until Communications failed". The
rfu.c header comment above `rfu_link_backlog()` has the full argument, with
decomp references. `tools/tests/test_rfu_link_backlog.c` shows it on the
production rfu.c: one 12-frame stall leaves 11 frames of standing backlog
20 s later, at equal rates.

Slowing either console only moves packets from one queue to the other,
because both games answer 1:1. Pacing cannot fix this, and the rig must not
assume that it does.

### 0.2 The fix under test ("link shedding", `rfu_shed_keep`)

**What a real adapter does.** It holds one buffer per direction, and a newer
frame replaces an unread one.

**What we can safely do instead.** We cannot replace frames blindly: link
data is a sequence, and dropping a command desyncs the games. But most
packets carry no command at all:

- In an active trade, 90–100 % of packets in each direction are a librfu LL
  frame whose only sub-frames are UNI with all-zero payloads. The census is
  `EVT rfu_empty n=534/593`, `600/600`, `596/596`.
- The receiving game discards exactly those. `RfuRecvQueue_Enqueue` skips an
  all-empty table, and a zero child command is "no command".

**The rule.** At each frame boundary, a receive queue deeper than
`rfu_shed_keep` (2) gives up those content-free packets, oldest first. Order
is preserved, and a packet with any content, any NI sub-frame, or any
unparseable header is never touched.

**Knob.** A harness/ini key: `rfu_shed_keep = 0` gives the historical queues
exactly, `2` is the fix. **The two arms of the A/B are the same EBOOT, and
differ only in this one key,** which every `rfu_shed` census line echoes.

### 0.3 Already in the worktree

Not yet committed at the time of writing: see the report.

- **rfu.c telemetry.** One line each per 600-frame window:
  - `rfu_backlog`: client standing backlog, `hi` and `now`;
  - `rfu_hbacklog`: host standing backlog;
  - `rfu_answin`: per-window answer latency, mean and max ms;
  - `rfu_txmix`: new vs RTX-resend transmissions;
  - `rfu_rxmix`: receives with data vs receives that found none;
  - `rfu_empty`: content-free received / received;
  - `rfu_shed`: packets shed and the `keep` in force.
- **Autopilot time stamps.** `ap_mark` and `ap_sync` now carry `f=` (the
  engine frame) and `t_ms=` (the console's wall clock).
- **Shedding.** `rfu_shed_keep` plus host tests: the classifier, ordering,
  "never drops content", off = historical, and the host side.

**PPSSPP A/B, same EBOOT.** Emerald trade, 40 ms RTT, 2 % loss on the join.
This is one run per arm, so it is indicative only.

| arm | client answer latency, mean per window | client standing backlog | host backlog hi |
|---|---|---|---|
| A (`keep=0`) | 100 → 141 → 128 → 180 ms | 4–6 (hi 12) | 17 |
| B (`keep=2`) | 11 / 12 / 9 / 10 ms | 0 (hi 1) | 1 |

---

## 1. DETERMINISM

### 1.1 Starting state: identical every run

| input to the guest | how it is pinned |
|---|---|
| **ROM** | Rig copies of the canonical images (ADR-0009): host **FireRed USA/Europe Rev 1** (`BPRE`, 0xBC=`01`, md5 `51901a6e40661b3914aa333c802e24e8`), join **LeafGreen Rev 1** (`BPGE`, `01`, md5 `9d33a02159e018d09073e700e1fd10fd`). The staging manifest records md5. The console logs `rom_id code= rev= crc32=` (new line, §7), and the summarizer checks it (I10). The owner's own ROM files are never used. |
| **Save** | Rig golden saves, restored by `hw_loop.py restore_save()` before every run (unchanged code). They are derived from `harness-kit/golden-saves-frlg/{host-firered,join-leafgreen}.sav`, which are parked at the Direct Corner attendant; see §1.3 for the moveset edit. |
| **Savestate** | **Not used.** A savestate cannot carry a live link session, so reloading one mid-link desyncs by construction. Every run is a cold boot plus the golden save. |
| **RTC** | FR/LG carts have no RTC, so gpSP's GPIO RTC is never engaged and nothing time-of-day reaches the guest. The summarizer asserts that no RTC/GPIO line appears. |
| **Config** | A rig-only `CONFIG.INI` per role, staged every run. It sets the speed profile (57.00), `me_mode` as the owner runs it (on), and the scale/filter defaults. `harness-kit/golden-saves-frlg/*-CONFIG.INI` is **not** reused, because it is stale: `net_session_fps=40`, `rfu_rx_cap=2`, `last_rom=EMERALD.GBA`. |
| **Harness ini** | Generated per run by `hw_loop.py` from a template, with `run_id` and `arm` injected (§2.5). Every key is echoed back and compared (§2.4). |
| **Pad** | Only the autopilot drives it. `have_script` already disables the UI chords. |
| **Frame counts** | No step depends on an absolute frame number. Every step is gated on game state. |

**RNG.** FR/LG seed the RNG from a timer when the player presses through the
title. The link battle then shares the host's RNG, and both engines consume
it identically. The *battle outcome* could therefore vary run to run, so the
design makes the flow independent of it:

- **All four battlers know only GROWL.** Move 45 targets both foes, so there
  is never a target prompt. It has 40 PP and 100 accuracy, so it cannot miss
  at stage 0: the check is `Random()%100+1 <= 100`.
- **It deals no damage,** so nothing faints.
- **At −6 Attack the game prints "won't go lower",** but the menus are
  unchanged.
- **Turn order and speed ties** change only which message plays first, never
  which menu comes next.
- **The run caps at 30 turns.** That is below 40 PP, so Struggle can never
  engage.

### 1.2 Symbols (pret/pokefirered `symbols` branch, `df4449a`, `pokefirered_rev1.sym` / `pokeleafgreen_rev1.sym`)

**FR and LG are identical for every symbol below.** This was checked by
diffing both `.sym` files over this exact list. The only differences are the
unrelated copies of `HandleInputChooseAction` in other controllers.

Column **V**:

- **D:** already verified live on this ROM (docs/AUTOPILOT.md FR/LG table).
- **B:** derived from the `.sym` or decomp; must be verified in PPSSPP
  bring-up (§7, gate G1) before any hardware run.

| use | symbol | address / value | V |
|---|---|---|---|
| game state | `gMain.callback2` | `0x030030F4` (u32) | D |
| overworld | `CB2_Overworld` | `0x080565C9` (Thumb) | D |
| title running | `CB2_TitleScreenRun` | `0x08078BB1` | D |
| main menu | `CB2_MainMenu` | `0x0800C2E9` | D |
| in battle | `BattleMainCB2` | `0x08011115` | D |
| link error screens | `CB2_LinkError` / `CB2_PrintErrorMessage` | `0x0800ACE9` / `0x0800AF41` | B |
| SaveBlock1 | `gSaveBlock1Ptr` | `0x03005008` (+0 pos, +4 location) | D |
| player tile | `gObjectEvents[0].currentCoords` | x `0x02036E48`, y `0x02036E4A` (map +7) | D |
| menu state | `sMenu` (menu.c, struct Menu) | `0x0203ADE4`: `cursorPos` +2 (s8), `maxCursorPos` +4 | B |
| text printer 0 | `sTextPrinters[0]` | `0x02020034`: `active` +0x1B, `state` +0x1C | B |
| quest-log replay | `gQuestLogState` | `0x0203ADFA` (u8; 0 = off) | B |
| link group UI | `sWirelessLinkMain` | `0x0203B05C` (ptr): Leader `state` at +12, Group `state` at +8 | B |
| battle type | `gBattleTypeFlags` | `0x02022B4C`: must have DOUBLE (1<<0), LINK (1<<1); IS_MASTER (1<<2) on host only | B |
| per-battler controller | `gBattlerControllerFuncs[4]` | `0x03004FE0` (u32 ×4) | B |
| action menu up | `HandleInputChooseAction` (player controller) | `0x0802E44D` | B |
| move menu up | `HandleInputChooseMove` | `0x0802EA25` | B |
| target menu (must never appear) | `HandleInputChooseTarget` | `0x0802E689` | B |
| cursors | `gActionSelectionCursor[4]` / `gMoveSelectionCursor[4]` | `0x02023FF8` / `0x02023FFC` (u8) | B |
| outcome | `gBattleOutcome` | `0x02023E8A` (u8, 0 while running) | B |
| game RFU queues | `gRfu.recvQueue.count` / `.full` | `0x03005AEE` / `0x03005AEF` | B |
| game RFU queues | `gRfu.sendQueue.count` / `.full` | `0x03005D22` / `0x03005D23` | B |
| map ids | Viridian Pokémon Center 2F (both golden saves, decoded offline: group 5, num 5, pos 10,4) / `BattleColosseum_2P` | `0x0505` / `0x0000` (group 0, num 0) | D (save) / B |
| colosseum | spawn / spot triggers | warp (6,8); spot0 (3,5) = host; spot1 (10,5) = join | B |

**Local battlers.** In a two-player link double the local battlers are
expected to be 0 and 2 on the host and 1 and 3 on the join. This is a
decomp reading of the link controller setup and is **B**: G1 logs
`gBattlerControllerFuncs[0..3]` at the first action menu on both consoles
and pins it.

**How B becomes D.** In G1 the bring-up script `logram`/`logptr`s every B
address *at the step that uses it*. The summarizer then compares against
this table, and the design is not frozen until every row reads as expected
on both editions. This is the procedure docs/AUTOPILOT.md used for the
existing FR/LG rows. The first assert of every run is the
.sym-matches-ROM proof already on record: `callback2` at boot reads
`CB2_InitCopyrightScreenAfterBootup|1`, `0x080EC835` on FR and `0x080EC80D`
on LG.

### 1.3 Golden saves ("known-outcome setup")

**The parked parties as they are today** (decoded with
`harness-kit/e2e/read_party.py`):

- host: Bulbasaur L5, Pidgey L4;
- join: Charmander L7, Pidgey L3.

Both Pidgeys know only Tackle, which would make the battle damage- and
RNG-dependent.

**New tool `tools/rig/make_rig_golden.py`:**

- Decodes each save and rewrites each party mon's four move slots to
  `[GROWL, 0, 0, 0]`, with PP 40 and PP-bonus 0.
- Recomputes the mon checksum (the decrypted substruct word sum) and the
  sector checksum.
- Refuses anything it cannot verify.

`read_party.py` is extended to print moves and PP. The rig golden saves are
accepted only when the decoder shows `GROWL/40` on every mon and the
position, map and party count are unchanged. Their md5s go into the staging
manifest.

This edits test data only. Moves are not legality-checked in link battles.

### 1.4 The two scripts: every step closed-loop

**Conventions.**

- `mash BTN until P`: mash until predicate P holds. The predicate is
  re-read before every injection (2 on / 6 off), so a consumed press stops
  the mash.
- `press` is used **only** where a following `waitram` proves its effect.
  HARNESS §4: a swallowed open-loop press is invisible.
- **No step mashes across a menu boundary.** Text pages are advanced with
  `press A`, then `waitram` for text printer 0 leaving its wait-for-key
  state. Menus are recognised by `sMenu.maxCursorPos`, and each menu in the
  sequence has a distinct value in order: 2 → 4 → 1 → 2.
- Every wait has a timeout. A timeout ends *that* console's script with
  `ap_fail` at a named step (§2.6).

**Pre-battle, both consoles:**

| # | step | predicate (closed loop) | timeout |
|---|---|---|---|
| P1 | boot to title | `callback2 == CB2_TitleScreenRun` | 60 s |
| P2 | through title → CONTINUE | mash A until `callback2 == CB2_MainMenu`, then mash A until `callback2 == CB2_Overworld` | 60 s |
| P3 | quest-log replay ends | mash B until `gQuestLogState == 0` (verify B skips it in G1; else wait) | 90 s |
| P4 | assert parked position | `location == 0x0505` (Viridian Pokémon Center 2F) and pos (10,4), decoded offline from both golden saves; the attendant stands two tiles north; facing asserted from `gObjectEvents[0]` | instant |
| P5 | talk; page through the welcome | press A + text-printer waits, until `sMenu.maxCursorPos == 2` (TRADE / COLOSSEUM / EXIT) | 30 s |
| P6 | pick COLOSSEUM | mash DOWN until `sMenu.cursorPos == 1`, then press A + waitram `maxCursorPos == 4` | 10 s |
| P7 | pick DOUBLE | mash DOWN until `cursorPos == 1`, then press A + text waits until `maxCursorPos == 1` (save YES/NO) | 20 s |
| P8 | save | press A (YES, `cursorPos == 0` asserted) + `waitsram` | 30 s |
| P9 | leader menu (JOIN / LEAD / EXIT) | text waits until `maxCursorPos == 2` | 20 s |

**Link-up (the handshake between consoles):**

| # | host | join | timeout |
|---|---|---|---|
| H1 | mash DOWN until `cursorPos == 1`; press A; `waitptr Leader.state == LL_STATE_AWAIT_PLAYERS (6)` | press A (`cursorPos == 0` asserted); `waitptr Group.state == LG_STATE_CHOOSE_LEADER_HANDLE_INPUT (3)` | 20 s |
| H2 | wait for the join request: `Leader.state == ACCEPT_NEW_MEMBER_PROMPT_HANDLE_INPUT (11)` | wait until the list shows the host (list-count probe pinned in G1), then press A; `Group.state == ASK_JOIN_GROUP (5)` | **300 s**: boot skew between a 1000 paging its ROM and a 3000 is tens of seconds |
| H3 | press A (YES); then each leader prompt in turn, each by its state value (pinned in G1), until `Leader.state == TRY_START_ACTIVITY (26)` | press A (YES); `Group.state == MAIN (6)`, then `== START_ACTIVITY (21)` | 120 s |
| H4 | both: `location == BattleColosseum_2P` | | 60 s |
| H5 | walk to spot0 (3,5): `holdmash` on live coordinates, route from `map_grid.py` | walk to spot1 (10,5) | 30 s |
| H6 | both: `callback2 == BattleMainCB2` and `gBattleTypeFlags` asserted (`logram`, I11) | | 120 s |

**What each side waits for.**

- **The join never presses toward the host before the host's group exists.**
  H2 is a list predicate, not a timer.
- **The host never presses before a request is in front of it.**
- **After H3 the games synchronise each other.** The activity start and the
  battle intro are both link-gated in the game itself.

**Battle, per turn t = 1..30:** L and R are this console's two local battlers.

| # | step | predicate | timeout |
|---|---|---|---|
| T1 | action menu for L | `funcs[L] == HandleInputChooseAction`; `actionCursor[L] == 0` asserted | 60 s |
| T2 | FIGHT | mash A until `funcs[L] != HandleInputChooseAction` (the game consumed it) | 5 s |
| T3 | move menu for L (**the owner's symptom**) | `funcs[L] == HandleInputChooseMove`; `moveCursor[L] == 0` asserted | 60 s |
| T4 | GROWL | mash A until `funcs[L] != HandleInputChooseMove` | 5 s |
| T5–T8 | same four steps for R | | same |
| T9 | per-turn probes | `logram` the game's `recvQueue.count`, `sendQueue.count`, `gBattleOutcome`, `gBattleCommunication[0..3]` | instant |

After turn 30:

1. `evt turns_done`.
2. A 600-frame pad, so the two consoles do not tear the link down while the
   peer is still in its last turn.
3. The script ends and `autoexit` fires.

**Scoring stops at `turns_done`.** The link error the later-exiting console
shows at teardown is expected and ignored.

**KO, battle end, miss.**

- **Miss:** cannot change the flow (§1.1).
- **KO or battle end:** impossible with GROWL-only parties inside 30 turns.
  If either happens anyway, the T1 wait times out, and the failure snapshot
  (§2.6) shows `gBattleOutcome != 0` or `callback2 != BattleMainCB2`. The
  run is then **SETUP-INVALID**: a broken fixture, not a lag result.
- **Target menu:** reaching `HandleInputChooseTarget` means a moveset error,
  and is SETUP-INVALID for the same reason.

**No drift, no deadlock.**

- Every inter-console dependency is one of the named predicates above,
  never a timed wait.
- Every predicate has a timeout.
- When one console fails, the other sees link loss and fails its own next
  wait, so the pair always terminates.
- `hw_loop` then collects both logs.

---

## 2. TELEMETRY: complete, and unable to lie

### 2.1 Clocks

| clock | source | used for |
|---|---|---|
| `f` | autopilot engine frame counter, which advances once per emulated frame from script load | frame-domain metrics |
| `t_ms` | that console's `sceKernelGetSystemTimeWide` (µs, monotonic per boot), divided by 1000 | wall-domain metrics |

**The two consoles' clocks are independent and never subtracted from each
other.** Every metric is a difference of two stamps on the *same* console.

**Resolution.** Predicates are evaluated once per frame against the previous
frame's RAM, so every stamp is ±1 frame, about 17.5 ms at 57 fps. The
summarizer states this beside every latency it prints.

### 2.2 Per-turn metrics

Each metric is computed per console and per local battler b, from the
`ap_sync` stamps of T1–T8.

| metric | start event | end event | units |
|---|---|---|---|
| **F2M_b(t)**, "FIGHT → move menu" | T2 satisfied: FIGHT consumed | T3 satisfied: move menu up | ms and frames |
| **TURN(t)**, "submitted → next action" | T8 satisfied: R's move consumed | T1 of turn t+1 satisfied | ms and frames |
| **PERIOD(t)** | T1 of turn t | T1 of turn t+1 | ms and frames |
| game queues | T9 `logram` | – | slots |

**F2M is the headline.** It is exactly the interval the owner described, and
its animation content is nil. TURN includes four GROWL animations, which are
constant per turn: it is the second witness.

### 2.3 Link metrics

These come once per 600-frame window, on both consoles. They are all
existing or §0.3 lines, and all are emitted by the hardware build.

| metric | line |
|---|---|
| client standing backlog | `rfu_backlog hi/now` |
| host standing backlog | `rfu_hbacklog hi/now` |
| client answer latency | `rfu_answin mean_ms/max_ms` |
| arm and shed count | `rfu_shed n keep` |
| content-free share | `rfu_empty` |
| RTX resends | `rfu_txmix` |
| host polls that found nothing | `rfu_rxmix` |
| real data discarded (must be 0 in arm B) | `rfu_pacedrop` |
| game recvQueue model peak | `rfu_gqpeak` |
| radio | `net_stats srtt/rto/retx/retx_age/txq_hi/spill` |
| both rates | `sess_cost pace=` (target / self_cap / peer_cap) and `fps` |
| missed frames | `frame_hist late/forgive` |
| session rate held | `session_pace` / `session_pace_miss` |
| ROM page-ins | `pg` per window: a new `romload` census line (the 1000 pages FR from the stick) |
| adapter | `adhoc_stats` |

### 2.4 Configuration can no longer fail silently (new, §7)

- **Echo on boot.** At boot the harness build writes `EVT cfg src=harness
  key=<k> raw=<v> applied=<v>` for every key in `.gpsp-harness.ini`, and
  `src=config` for `CONFIG.INI`.
  - A key the build does not know produces `EVT cfg_unknown key=<k>`.
  - A key whose applied value differs from the raw one (clamped or
    defaulted) produces `EVT cfg_clamped key= raw= applied=`.
  - This catches the misspelled-key trap and the `perf_timeout_s → 180` /
    `handoff_max_runs 0 → 20` class of trap.
- **Coverage is enforced by a test.** The known-key table is enforced by a
  host test that greps `main_psp.c`/`config_psp.c` for every
  `fe_ini_get_*(…, "<key>"…)` and fails on any key missing from the table.
  The table therefore cannot drift from the code.
- **The PC checks the result.** The PC knows what it staged. The summarizer
  requires, for every staged key, an `applied` that equals the staged value,
  and zero `cfg_unknown`/`cfg_clamped` lines. Otherwise the run is INVALID
  (I4).

### 2.5 Identity: run ID and build

- **Run ID.** `hw_loop.py` writes `run_id = <cycle>-<unix>` and `arm = A|B`
  into both staged inis. Each console logs them (via the §2.4 echo), and the
  logs are named `autoNNN-<arm>-<role>.log` by **PC cycle**. That is already
  hw_loop's pairing rule, and it retires the host `autoN` / join `auto(N-1)`
  trap.
- **Build.** The harness profile always emits `build eboot_crc=`: the CRC32
  of the running `EBOOT.PBP`, computed by the existing `eboot_crc()`, which
  today runs only with `vid_prof`. At collection, `hw_loop.py` computes the
  CRC32 and md5 of the EBOOT still on each card and writes
  `autoNNN-<arm>-<role>.meta.json` with the drive, volume serial, role, arm,
  run_id and both hashes.
- **Refusal.** The summarizer refuses to pair or score a run unless run_id,
  arm and EBOOT CRC agree across both logs and both meta files, and equal
  the staged manifest (I2, I3).

### 2.6 Failure snapshot (new, §7)

When a script fails, the autopilot's `ap_fail` line gains:

- `val=`: the failing predicate's last value;
- `it=`: the repeat iteration, i.e. the turn.

The frontend follows it with one `EVT fail_probe` line containing the words
listed in the harness key `fail_probe`: `callback2`, `gBattleOutcome`,
`gRfu` queue counts, `sWirelessLinkMain` state.

**Failure types**, classified from lines that exist on hardware:

| type | evidence |
|---|---|
| **LINK_ERROR** | `callback2 ∈ {CB2_LinkError, CB2_PrintErrorMessage}`, or an `rfu_state` drop / `rfu_discans` before `turns_done` |
| **TRANSPORT** | `net_error`, `session_stop` or `peer_disconnected` before `turns_done` |
| **LAG_TIMEOUT** | a T-step timed out, with `callback2 == BattleMainCB2` and none of the above |
| **CRASH** | no `EVT exit`, or no RESULT.TXT |
| **SETUP** | failure before H6, or a target menu, or `gBattleOutcome != 0` |

### 2.7 Validations: each metric is proven before it is trusted

| check | kind | procedure | required result |
|---|---|---|---|
| **V1: F2M measures the link** | synthetic control | arm A plus `net_latency_ms = 50` on **both** consoles. The existing transport fault shim is in the PSP RX path, so it runs on hardware, and adds 100 ms RTT. | F2M rises by k × 100 ms (±1 frame per stamp) for a small integer k, the round trips on the FIGHT→move path. k is measured in PPSSPP and confirmed on hardware. If F2M does not move, the metric is not measuring the link, and the rig is rejected. |
| **V2: the slope detects the bug** | positive control | arm A plus `pace_slow_us` on the join, a busy-burn that forces a rate deficit. This is the ratchet by construction. | F2M slope > 0, and `rfu_backlog now` climbing window over window. It must be seen in PPSSPP (G2) and on hardware (acceptance). |
| **V3: the null is flat** | null control | arm B, no injection, repeated | slope distribution → the noise band used in §3 |
| **V4: coherence** | cross-check | per turn, F2M − F2M(turn 1) against (client backlog + host backlog) × frame time | same sign and order. Not a pass/fail gate; a disagreement is investigated before any claim. |
| **V5: the unit proofs** | host tests | `test_rfu_link_backlog` (ratchet, bound, content never dropped), `rfu_peer_binding`, netdrv | pass on every build staged |

### 2.8 Invariants: a violation makes the run INVALID, reported with a reason and never scored

| # | invariant |
|---|---|
| **I1** | Both logs exist, each has `EVT boot_ok`, and each ends in `EVT exit`. If not, the run is CRASH or TRUNCATED: a failure, but not a lag sample. |
| **I2** | `run_id` and `arm` are identical on both consoles and equal the PC cycle. |
| **I3** | `build eboot_crc` is identical on both and equals the meta file and the staged manifest. |
| **I4** | Config echo: every staged key is applied as staged, with zero `cfg_unknown`/`cfg_clamped`. |
| **I5** | Script identity: `ap_loaded` carries the fixture md5 (new), which must equal the staged fixture's. |
| **I6** | No log loss: every `sess_cost` shows `evtio drop=0`, there are no `rfu_trace_lost` lines, and no `evt` ring drop is reported. The input-echo flood is off (`log_input = 0`, new key) so the ring is not filled by the script's own presses. |
| **I7** | Sequence integrity: for each turn, T1…T8 appear once each in order, `it=` rises by one, and `f` and `t_ms` are monotonic. A turn that breaks this is INVALID (listed), not "slow". |
| **I8** | Role and device *by content*: the host log reports `role=host` and PSP model 3000, the join `role=join` and model 1000 (new `psp_model` line). A swapped cable or drive letter shows up here, not in a wrong conclusion. |
| **I9** | Exactly one `session_start` per console, identical `session_pace fps=` and `net_pace_match` on both. |
| **I10** | `rom_id`: host BPRE rev 1, join BPGE rev 1, CRCs equal the manifest. |
| **I11** | `gBattleTypeFlags` has DOUBLE and LINK on both; IS_MASTER on the host only. |

**Traps already paid for, and where each is designed out:**

| trap | designed out by |
|---|---|
| host `autoN` / join `auto(N-1)` | PC-cycle naming plus run_id (I2) |
| run length confounding per-window rates | Scoring is per *turn index*, never per time window. Runs are compared at matched turns, and early failures are compared by turns completed (survival), never by averages over their shorter lives (§3). |
| `ap_mark` without frame numbers | `f=` and `t_ms=` on every `ap_mark`/`ap_sync`, and `it=` in repeat loops |
| log ring drops | `evtio drop=0` invariant (I6), and `log_input = 0` |
| log-writer starvation when the emulation thread spins | The writer thread's drops are counted (`evtio drop`), so starvation turns a run INVALID instead of silently thinning it. Per-turn stamps are taken on the emulation thread, not by the writer, so a late write cannot move them. |
| relaunch truncating `frontend.log` | New: at boot, an unclaimed `frontend.log` is renamed `frontend.prev.<n>.log` (keep 3) before the new one opens. `hw_loop` collects these too. A crash therefore leaves evidence. |
| misspelled or clamped ini keys | §2.4, I4 |
| instruments measuring nothing | V1–V3, and ADR-0058's rule: every census line is emitted every window, including zeros |

**No conclusions from absence.** The summarizer prints `unknown` for any
metric whose source line is missing, never 0 and never "didn't happen". A
window with no census is an unknown window. `turns_completed` counts only
turns whose full T1–T8 sequence is present.

**Hardware-only lines.** Every line above is produced by the harness EBOOT
on a PSP. The PPSSPP bring-up uses the same EBOOT, so no metric depends on
anything PPSSPP-specific. (The `faultdel` / `faultdrop` shim counters exist
on both.)

---

## 3. ORACLE / SCORING

**Per console, per run, printed by `summarize_log.py --battle`:**

- `turns_completed` (0–30) and the failure type (§2.6).
- **F2M** for L and R:
  - the turn-1–3 median and the last-3-turn median;
  - the maximum, and the turn it happened on;
  - the **least-squares slope** in ms per turn, over turns 3..N, and in
    ms per minute against `t_ms`.
- **TURN**: the same.
- **Link extremes and changes:**
  - backlog `hi` and first vs last `now`;
  - `answin` max and first vs last mean;
  - `srtt` max, `retx_age` max;
  - shed and pacedrop totals;
  - `late` and `forgive` totals;
  - page-ins.

These are extremes and changes, never averages of averages (HARNESS §6).

**VALID run:** all of I1–I11 hold, and the battle was reached (H6). A
failure is a valid *outcome*. Only broken measurement or setup is INVALID.

**PASS (per run):**

- VALID;
- 30 turns on both consoles;
- no LINK_ERROR or TRANSPORT;
- `rfu_pacedrop` total = 0;
- F2M slope inside the null band (§4);
- last-3 median F2M − first-3 median ≤ 50 ms, on both consoles and both
  battlers.

**FAIL (per run):** VALID, and any of:

- fewer than 30 turns because of LINK_ERROR, TRANSPORT or LAG_TIMEOUT;
- a slope above the band;
- a last-minus-first F2M above 50 ms.

---

## 4. A/B PLAN

- **Arms.**
  - **A:** `rfu_shed_keep = 0`, the historical build behaviour.
  - **B:** `rfu_shed_keep = 2`, the fix.
  - **Same EBOOT.** Only the `arm` and `rfu_shed_keep` lines differ between
    the staged inis.
- **Staging.**
  - `hw_loop.py` gains `--arms A=<stageA>,B=<stageB> --order ABBA`, which
    stages the next arm's directory each cycle. This replaces editing the
    stage dir by hand, and follows HARNESS §7: interleave, never block.
  - The arm is written into run_id and the log name.
- **Phase G (PPSSPP, no hardware).**
  - G1: symbol and state bring-up (§1.2) until three consecutive runs
    complete 30 turns with every B row confirmed.
  - G2: V1 and V2 on PPSSPP.
  - G3: summarizer self-test on recorded logs, including hand-corrupted ones
    (a dropped line, a mismatched run_id, a clamped key), which must come
    out INVALID with the right reason.
- **Phase H0 (hardware acceptance; the rig's own test).**
  - Host PSP-3000, join PSP-1000: the field pairing.
  - 2 × A, 1 × V1 (A with +50 ms), 1 × V2 (A with join slow), 2 × B.
  - **Accept the rig only if** every run is VALID, V1 moves F2M by its k,
    and V2 shows a positive slope.
  - If A does not reproduce growth on this pair and the V2 control does,
    the rig is sound and the report is "not reproduced in N runs". That is
    a real answer.
- **Phase H1 (the claim).**
  - Interleaved `A B B A A B B A A B B A`: 6 runs per arm.
  - **Noise band:** from the arm-B null slopes, the mean ± 3 SD, floored at
    ±2 ms per turn.
  - **Claim "B bounds the lag":**
    - every B run PASSes;
    - the F2M slopes separate completely, every B below every A (chance
      probability 1/924 for 6 vs 6);
    - B's turns-to-failure ≥ A's in every pair.
  - If the separation is not complete, the answer is "not demonstrated":
    extend to 10 per arm, and never average across it.
- **Phase H2.** Roles swapped (host 1000, join 3000), 4 + 4, because the
  owner saw lag on both sides.
- **Cost.** One run is about 3 minutes to battle, 30 turns at about 20 s,
  and handoff: roughly 15 minutes. H0 plus H1 is 18 runs, about 5 hours
  unattended.

---

## 5. SAFETY

- **Backups first.** Before the first run, a script copies from each stick:
  - every `PSP/GAME/*/roms/*.sav`, `*.sav.bak` and `*.st*`;
  - every `CONFIG.INI`/`config.ini` in each GBAdhoc folder;

  into `C:\Users\DrSto\OneDrive\Desktop\GBAdhoc\builds\wireless-stick-backup-2026-09-26\<drive>-<model>\…`,
  with an md5 manifest. The copy is re-read and verified before anything is
  written to either stick.
- **Separate app folder:** `PSP/GAME/GBADHOC-RIG/` on each stick.
  - **Contents:**
    - the harness EBOOT (XMB title "GBAdhoc RIG" so it cannot be mistaken
      for the player build);
    - `gbadhoc_me.prx`;
    - `roms/` with its own ROM copy and rig save;
    - the rig `CONFIG.INI` and `.gpsp-harness.ini`;
    - the fixture;
    - `handoff/`.
  - **`hw_loop.py` only ever writes under `PSP/GAME/<appdir>`.** Verified by
    reading it: `card_root()` scopes stage, restore, collect, `CMD.TXT` and
    `RESULT.TXT`. A new guard refuses any `--appdir` that is not exactly
    `GBADHOC-RIG`.
  - **The console side writes only under its own `g_dir_base`.**
    `usb_handoff.c` derives every path from it and relaunches its own
    `EBOOT.PBP`.
  - **The owner's main install is never read or written by the loop.**
- **Drive letters and roles.** Today D: = PSP-1000 and F: = PSP-3000.
  Letters drift, so roles are verified by content:
  - At setup, each card gets `GBADHOC-RIG/ROLE.TXT` (`host PSP-3000` /
    `join PSP-1000`), written only after the console's own `psp_model` line
    has been read off that card.
  - `hw_loop.py` refuses a cycle whose ROLE.TXT disagrees with `--host` /
    `--join`.
  - I8 re-checks the model from the log on every run.
- **Owner steps** (when the design is approved and G-phase is done):
  1. The lead confirms the backup manifest exists and verifies.
  2. Plug both PSPs into this PC by USB, on AC power, with the WLAN switch
     ON.
  3. On each PSP: XMB → Game → Memory Stick → **GBAdhoc RIG** → launch once.
     It boots straight into USB handoff and parks.
  4. Leave both consoles alone. The PC runs:
     `python harness-kit\hw_loop.py --host F: --join D: --appdir GBADHOC-RIG --arms A=<stageA>,B=<stageB> --order ABBA --golden <rig-golden> --logs <logs> --game frlg --forever --keep-going --timeout 86400`
     `--verify` is not used: the trade oracle does not apply, and the
     battle score is the oracle.
  5. **To stop:** the lead stops `hw_loop.py`. Each console stays parked in
     USB handoff, and the owner can quit it with HOME.

---

## 6. What is reused, what changes, and why

| piece | reused | changed | why |
|---|---|---|---|
| `harness-kit/hw_loop.py` | collection, flush/eject, golden restore, PC-cycle log naming, drift note, `--appdir`, solo mode | `--arms/--order` round-robin, run_id/arm injection into the staged inis, EBOOT CRC/md5 meta sidecar, ROLE.TXT check, appdir guard, collect `frontend.prev.*.log` | pairing, arm alternation and identity become facts in files, not in the operator's head |
| `psp/usb_handoff.c` | unchanged | – | appdir-agnostic through `g_dir_base`; relaunch proven over 300+ runs |
| `summarize_log.py` | extremes-and-changes rule, verbatim unknown events, generic `net_stats` parse | `--battle` section: per-turn table, slopes, invariants, INVALID reasons | the coordinator's slope-per-console requirement |
| `ab_compare.py` | spread-first comparison, "not enough runs" answers | new metrics (F2M slope, turns completed, backlog hi); groups by the logged `arm`, never by guessing config | a comparison is only as good as the arm it groups by |
| `frlg_recon_{host,join}.inputs` | Direct Corner entry and the host=LEAD / join=JOIN choice | every menu step closed-loop (§1.4), battle loop added | recon relied on open-loop presses with long waits |
| `emerald_tradecenter_*.inputs` | closed-loop `holdmash` walking on live coordinates | coordinates for the colosseum | proven pattern |
| autopilot (`fe_autopilot.c`) | engine and grammar | `f=`/`t_ms=` (done), `it=` in repeats, script md5 in `ap_loaded`, `val=` in `ap_fail` | §2.2, I5, I7, §2.6 |
| `read_party.py` | decoder | prints moves and PP | golden-save acceptance (§1.3) |
| rfu.c / main_psp.c telemetry | existing census discipline | §0.3 lines (done); `cfg` echo, `rom_id`, `psp_model`, `romload`, `fail_probe`, `log_input`, eboot_crc always on in harness, log preservation | §2 |

---

## 7. Build order after approval (each is a separate, reviewable commit)

1. **Autopilot identity and sequencing:** `it=`, script md5, `ap_fail val=`.
   Host test.
2. **Frontend identity and config:** `cfg` echo, `cfg_unknown`,
   `cfg_clamped`, key-table test, `rom_id`, `psp_model`, `romload` census,
   `fail_probe`, `log_input`, eboot_crc in harness, and `frontend.log`
   preservation.
3. **Tools:** `tools/rig/make_rig_golden.py` and the `read_party.py` moves
   output, with a round-trip test.
4. **Fixtures:** `frlg_battle_{host,join}.inputs`, developed in PPSSPP (the
   G1 gate).
5. **PC side:** the `hw_loop.py` changes and `summarize_log.py --battle`,
   with the G3 corrupted-log self-test.
6. **G2 validations** on PPSSPP, results written here.
7. **Hardware:** the owner handshake (§5), then H0 and H1.

## 8. Risks and open points for the reviewer

- **The link-group UI states** (H2/H3 values) and the exact prompt sequence
  on the leader side for a two-player group come from the enum only; G1 pins
  them. If FR/LG shows an extra prompt, the table gains a row, not a timer.
- **Shedding classifies librfu LL frames.** The format comes from the decomp
  (`llsf_struct`) and is unit-tested, but the only live evidence so far is
  Emerald. `rfu_empty` on FR/LG battle traffic, measured in G1, decides
  whether B has anything to shed in a battle. If battle traffic is mostly
  content, B will show little effect, and the report will say so rather
  than tune toward a pass.
- **The rig slows nothing.** The fixed session clamp stays at the owner's
  57.00. Arm A reproduces the field configuration exactly, apart from the
  added telemetry, whose cost is a handful of counters per frame.


---

## 9. Amendments from review (all four adopted)

1. **Cross-console desync detector, a PASS condition.**
   - **What is logged.** At every turn boundary (T1, both consoles at their
     action menu) each console logs its copy of all four party mons:
     - its own (`gPlayerParty`, m0/m1) and the peer's (`gEnemyParty`, e0/e1);
     - for each: personality+otId (`k*`), the 48 encrypted substructure bytes
       (`d*a`/`d*b`) and HP (`h*`).
   - **What the scorer does.** It decrypts, matches mons by personality, and
     compares species, moves, PP and HP per turn index.
     - Any mismatch is `FAIL: DESYNC`, with the turn. It is never counted as lag.
     - A boundary counts as compared only when both consoles decoded all four
       mons.
     - Fewer compared boundaries than completed turns makes the verdict FAIL
       (desync status unknown), never PASS.
   - **Validation.** `rfu_fault_corrupt = N` flips one content byte of the Nth
     content packet on the member. The key is harness-only, echoed in the log
     and host-tested. G2 `fault` records whether the detector trips (§11).
   - **Why the party copies and not `gBattleMons`.** G1 found that only the
     link MASTER runs the battle engine; the member's `gBattleMons` is zero for
     the whole battle.
     - The party copies are what both consoles hold. The master's SETMONDATA
       link commands keep them in step.
     - Evidence: PP falls by one per GROWL on both consoles in lockstep. In the
       G2 null run it read 40 -> 31 -> 20 at turns 1, 10 and 21, on both.
2. **Regression gates for arm B.** These run on hardware, with the `--verify`
   party oracle for the trades.
   - Emerald trade: harness-kit `emerald_tradecenter_*`, proven on hardware.
   - Emerald double battle: `emerald_battle_*`, derived by
     `tools/rig/mk_emerald_fixture.py`; PPSSPP bring-up in §11.
   - Union Room entry: `emerald_trade_{host,join}.inputs`, the Union Room
     board trade. It PASSed in PPSSPP with arm B at 40 ms RTT and 2 % loss,
     and again with a slowed join.
   - FR<->LG trade: the fixture is not yet brought up (§10.9).
   - **Mystery Gift DOES use the RFU path.** `psp/mgift_cart.c` injects the
     gift into the game's CLIENT adapter queue as NET_RFU_HOST_SEND.
     - Its frames are NI sub-frames and bare acks, which the shed classifier
       never removes.
     - Host test `test_mystery_gift_frames_never_shed` builds them exactly as
       `mgift_cart_frame()` does.
     - A hardware Mystery Gift receive is still on the owner's list.
3. **Realistic traffic.** Variant `swift`:
   - every mon knows only SWIFT: never misses, hits both foes, deals damage;
   - 3 PP-ups (32 PP);
   - HP/maxHP set to 999, so no KO happens inside a 12-turn cap.
   It is scored identically, and the report gives the `rfu_empty` share and
   the F2M slope for A vs B. The GROWL null run already shows 62-79 % of
   battle packets are content-free (join 0.62, host 0.79). So shedding has
   material to remove in a battle, not only in a trade.
4. **The owner's pairing, H3: FireRed on both consoles.** Variant `frfr`:
   - host: the rig golden FireRed save;
   - join: the owner's own FireRed save from the PSP-1000 backup
     (`D-PSP-1000/.../Pokemon - FireRed Version (USA).sav`, TID 0x2c71e412),
     GROWL-edited by the same verified tool.
   No trainer ID is forged.

**Log-starvation requirements** (from the coordinator, after the Sisyphus
finding):
- **Dropped lines are always visible.**
  - `fe_evt` marks every gap in-band: `EVT evt_gap dropped=N total=T`, pushed
    before the next line that fits.
  - At exit it writes `EVT evt_drop total=N` synchronously.
  - RESULT.TXT gains `frames=` / `evt_drop=` / `t_ms=`, written with sceIo
    after the log closes.
  - Host test: it injects starvation (the writer never runs; 1000 lines go
    into a 16 KB ring) and requires the announced drops to sum to the
    counter, and kept + dropped to equal lines written.
  - I6 reads these lines. A missing `evt_drop` makes the run INVALID.
- **Liveness never comes from log growth.**
  - hw_loop waits on RESULT.TXT only.
  - The PPSSPP runner waits on `EVT exit` or process exit, and stops 300 s
    after one side has exited.
  - The scorer takes `turns_completed` from the engine's own `it=` marks.
- **io_prio.** The rig keeps the field value, 0x2C, below the emulation
  thread, so link timing is the owner's. Any log loss is flagged by I6. G2
  `io42` runs the same null run with io_prio 42 to measure whether it moves
  F2M (§11).

## 10. What G1 (PPSSPP bring-up) changed

1. **Object slots in the link room.** `gObjectEvents[0]` is the RECEPTIONIST.
   The leader is slot 1 and spawns at (6,8); the member is slot 2, at (7,8).
   Walking uses slots 1 and 2.
2. **No holdmash overshoot in the link room.** Movement goes through the link
   key exchange there, so `holdmash` does NOT overshoot. Aim at the exact
   tile.
3. **Swallowed presses.** A lone `press` right after the quest-log recap, or
   on the frame a menu is created, is swallowed.
   - Talking: wait for the script context to be idle
     (`sGlobalScriptContext.mode` at 0x03000EB1 == 0), then `mashne A` until
     a script starts.
   - Menu choices: `mash DOWN` until the cursor moves, then `mash A` until the
     NEXT text starts printing (printer 0 active). That proves the choice was
     taken, and the mash stops before any A reaches that text.
4. **Key-wait pages.** The greeting and several prompts have key-wait pages.
   New op `mashif` presses A only on frames where text printer 0 is active,
   until the next menu is up. It can advance text but can never select a
   menu row.
5. **The save.**
   - It asks twice ("save?" then "overwrite?"), both YES by default.
   - The "saved the game" text closes by itself, so a press there would reach
     the next menu.
   - Each state is named by `sSaveDialogCB` (IWRAM 0x03000FA4).
6. **Group start.**
   - A 2-player leader starts the activity by itself once the member is
     accepted. There is no "members OK?" prompt.
   - The member presses A once in the list (`mashptrne` until the group state
     leaves 3) and nothing after, because B would leave the group.
7. **Only the master runs the battle engine.** The member's `gBattleMons`
   stays zero, hence §9.1's party-copy fingerprint. The local battlers are
   0/2 on the host and 1/3 on the member, as designed.
8. **Battle options.** The golden saves get battle scene OFF and text FAST
   (`make_rig_golden.py --scene-off --text-fast`). With them a GROWL turn
   takes about 48-63 s. Turn order and link logic are unchanged by options.
9. **Not built: a closed-loop FR<->LG TRADE fixture** (regression gate 2).
   - The harness-kit `frlg_recon_*` scripts are open-loop recon, not a gate.
   - The battle fixture's P1-P8 path is the same up to the service menu.
   - The trade room itself needs its own bring-up, about 2-4 PPSSPP
     iterations.
10. **Rig build and XMB title.** The rig runs the `tools/build.sh harness`
    EBOOT (XMB title "GBAdhoc HARNESS"). The separate folder
    PSP/GAME/GBADHOC-RIG is what keeps it apart from the owner's install. A
    separate "GBAdhoc RIG" title was not needed.

**G1 result:** FR host vs LG join, arm B, no latency.
- 30/30 turns.
- F2M flat: host L 1051 -> 1051 ms, slope 0.07 ms/turn; join L
  1102 -> 1101 ms.
- Link backlog <= 2; pacedrop 0.

That run predates the party fingerprint, so it scored DESYNC on the zero
`gBattleMons`. That is how §10.7 was found.

## 11. G2 validation record (PPSSPP)

| run | arm | condition | verdict | F2M L host / join, first3 -> last3, slope | notes |
|---|---|---|---|---|---|
| nullB | B | none, 20 turns | PASS | 1051 -> 1052 / 1102 -> 1084; 0.68 / 0.20 ms/turn | 21/21 boundaries compared, no desync; battle rfu_empty 0.79 host / 0.62 join |
| slowB | B | join pace_slow_us 13000, 12 turns | PASS | flat | the same slowdown as slowA; arm B plays through it |
| slowA | A | join pace_slow_us 13000, 12 turns | FAIL SETUP | n/a | never reached the battle: join rfu_pacedrop 586, backlog 22-23, then the link collapsed (the ratchet at its extreme, as in the PPSSPP trade A/B) |
| latA (1st) | A | net_latency_ms 50 | INVALID | n/a | the extra line was APPENDED after the base `= 0`, and the first line wins, so no latency was applied. This led to the `cfg_duplicate` event, scorer rule I4, and the runner now replacing keys |
| latA | A | +50 ms/side (srtt 25 -> 119-122 ms), 12 turns | **V1 PASS** | host L 1051 -> 1051 (purely local, unchanged, as it should be); host R 1668 -> 1935 first3; join L 1102 -> 1602; join R 1268 -> 2386 (vs nullB first3) | the injected latency shows in every link-dependent stamp. Arm A also RATCHETED under latency: host standing backlog 1 -> 2 -> 3 -> 4 -> 5 over turns 9-12, F2M slopes +49 / +37 / +73 ms/turn, rfu_shed 0. Caveat: the control is nullB (the other arm); latB/nullA are the same-arm controls and are queued |
| slowA2 | A | join pace_slow_us 6000, 12 turns | no ratchet | join R +8 ms/turn; join L stepped +417 ms (step, slope -1.6) | the join ran late (frame_hist.late 1003) but its backlog stayed 0-1: 6000 us is too mild to ratchet in PPSSPP. A 9500 us midpoint (slowA3) is queued |
| fault (1st) | B | rfu_fault_corrupt 40, `==` | INVALID | n/a | the 40th content packet was a 3-byte NI ack, `==` never fired again, and the run read as PASS. This led to `>=` plus a latch |
| fault (2nd) | B | rfu_fault_corrupt 40, `>=` | not a detector test | n/a | fired (`rfu_fault corrupted=43 off=5`) during LINK SETUP; the join never reached the colosseum (ap_fail at the warp wait). Moved to 1500 content packets, which lands inside turn 1's resolution (nullB join: 805 content packets by the turn-1 action menu, 2075 by turn 2) |
| fault (3rd) | B | rfu_fault_corrupt 1500, one packet, byte 5 | **FAULT UNDETECTED** (was reported as PASS) | flat | fired inside turn 1 (`corrupted=1500 off=5`); 12 turns completed, fingerprints agreed at 13/13 boundaries. One flipped bit in one chunk mostly lands in data the member only displays. So: the scorer now refuses to call a fired-fault run PASS (`FAULT UNDETECTED`, or INVALID if armed but never fired); `rfu_fault_span` walks the flip across the next S content packets at bytes 5, 6, 7, ...; and `rfu_fault_bytes` dumps each corrupted packet's original bytes so the run can say what it hit |
| faultspan | B | 1500 + span 48, generic flips (build 6ca535a) | FAIL on noise only (join f2m_L 3.0 ms/turn over 12 turns); no DESYNC | flat | 48 packets corrupted. The byte dumps then showed why generic flips miss: the FRLG parent frame is 73 bytes, the game's command is bytes 3..16 (chunk index, 0x89 SEND_BLOCK, 12 block bytes), the walk went into the unused tail, and the block data it did hit was PRINTSTRING (controller 0x10) and standby records that the member only displays |
| **fault** | B | `rfu_fault_mode = 1`, corrupt = 4: the 4th block chunk 0 that opens with a CONTROLLER_SETMONDATA record, bit 0 of its value byte (build eafd3c7) | **FAIL: DESYNC -- the detector trips** | flat | the corrupted packet: `46 00 05 / 00 89 / 00 03 03 00 08 00 00 00 / 02 09 00 27`, i.e. SETMONDATA to battler 3 (the join's 2nd mon), request 9 = move-1 PP, value 39 -> 38. The scorer: `DESYNC turn 2: mon 76e5b284: host (16, (45,0,0,0), (39,0,0,0), 15) join (... (38,0,0,0) ...)` -- Pidgey's GROWL PP, exactly the injected change. It shows at ONE boundary only: the host's next PP update overwrites it, so comparing every boundary is necessary, not a nicety |
| **slowA3** | A | join pace_slow_us 9500, 12 turns | **V2: the ratchet, then the field failure** | turn 1 only | reached the battle, finished turn 1, then the join's standing backlog climbed 1 -> 5 -> 15 -> 23 (cap 24) in about 20 s, rfu_answin mean 13 -> 415 ms, pacedrop discarded 195 packets of real data, and the link died (host LAG_TIMEOUT, join TRANSPORT after 1 turn): 'grows until Communications failed', compressed. slowB at the HARSHER 13000 us played 12 flat turns. A constant slowdown in PPSSPP is a cliff, not a ramp (6000 us neutral, 9500 us collapse), so the per-turn F2M SLOPE form of V2 comes from latA (backlog 1 -> 5, +37..+73 ms/turn) |

`tools/rig/g2_chain.sh` runs the remaining validations in this order:

| run | what it checks |
|---|---|
| emB | Emerald double battle bring-up |
| slowA / slowB | V2, the slowed join |
| latA | V1, the synthetic latency control |
| fault | whether the desync detector trips |
| nullA | the null run on arm A |
| io42 | whether io_prio 42 moves F2M |
| swiftA / swiftB | realistic traffic |
| frfrB | the owner's FireRed pairing |
| nullB2 | a second arm-B null |

Their rows go into this table with the report.

## 12. Owner handshake

Gates before hardware (PPSSPP): the desync detector trips on one corrupted
packet (fault), V1 moves F2M (latA), and V2 shows the ratchet (slowA3, latA).
Build: commit eafd3c7, harness EBOOT md5 9d642079, crc32 548ac844.

**Roles, by content.** D: = PSP-1000 = JOIN, F: = PSP-3000 = HOST.  The
cards cannot say which console they belong to (both volume serials read
0000-0000, and neither GBADHOC/log holds a model line), so the roles are
checked on the FIRST run: every harness log opens with `EVT psp_model ...
name=PSP-xxxx` read from the console itself (kuKernelGetModel), and hw_loop
STOPS both consoles and exits 2 if it disagrees with the card's ROLE.TXT.
The scorer checks it again (I8).

**Backup** (done 2026-09-26 13:32, 45 files, source re-read == copy):
`python tools/rig/backup_sticks.py --out ../wireless-stick-backup-2026-09-26 D:=PSP-1000 F:=PSP-3000`.
Re-checked before handover: both FireRed saves, the LeafGreen save and the
1000's FireRed state still match it.  If the owner plays before setup,
re-run it into a new dated directory.

**Stages** (builds/wireless-rig/<variant>/, MANIFEST.txt in each):
growl (H0/H1, 20 turns), frfr (H3, 20 turns), swift (realistic traffic, 12
turns); stage-A (shed off), stage-B (keep 2), stage-L (A + 50 ms per side),
stage-S (A, join slowed 6000 us per frame); golden/ holds the two saves the
loop restores before every run.

**Card setup** (writes only PSP/GAME/GBADHOC-RIG; the owner's GBADHOC is
only read):
`python tools/rig/setup_rig_cards.py --backup-verified ../wireless-stick-backup-2026-09-26 --variant-dir ../wireless-rig/growl --arm A host=F:=PSP-3000 join=D:=PSP-1000`

**H0** (warm-up + 6 runs, about 2.5 h):
`python tools/rig/hw_loop.py --host F: --join D: --appdir GBADHOC-RIG --arms A=../wireless-rig/growl/stage-A,B=../wireless-rig/growl/stage-B,L=../wireless-rig/growl/stage-L,S=../wireless-rig/growl/stage-S --order ABLSBA --runs 7 --golden ../wireless-rig/growl/golden --logs ../wireless-rig/logs/growl-H0 --game frlg --keep-going --timeout 3600`

**H1** after H0 is scored (12 runs, about 5 h): the same with
`--arms A=...,B=... --order ABBAABBAABBA --runs 13 --logs ../wireless-rig/logs/growl-H1`.

**Scoring** each pair: `python tools/rig/summarize_battle.py HOST.log JOIN.log --host-model PSP-3000 --join-model PSP-1000 --expect-crc 548ac844 --turns 20`.

## 13. H0 on hardware (2026-09-26, host PSP-3000 F:, join PSP-1000 D:, owner CONFIG.INI = 59.73 fps, ME on)

EBOOT 548ac844 (commit eafd3c7). The position probe (commit 2f8f5ee) took effect from **run 3**: fixture
md5 host 51df3e0d -> def2fbe0, join 8537a3f6 -> e363ca96. The md5 of every staged file, per run, is in
each campaign's `loop.log` (`staged:` lines); `growl/MANIFEST.txt` describes only the current stages. Scored with 5415092.

| run | arm | verdict | what happened |
|---|---|---|---|
| 1 | warm-up (A) | FAIL SETUP | both on their spot, but the battle never started; the host's standing backlog was 28-36 from the warp on. The owner saw the member miss his tile. No position probe yet |
| 2 | A | FAIL (growth) | 20 turns, no desync. f2m_sum 7921 -> 11372 ms, +111 ms/turn, with a step at turn 4; host backlog 7 -> 15 |
| 3 | B | FAIL on per-measure growth; **queues bounded** | 20 turns, no desync, positions agree. Host and join backlog <= 3 all run, answin 15-25 ms flat, ARQ 0.5-1.7 %. join f2m_L +24 frames at turn 18 while host f2m_R fell 13 (a redistribution). f2m_sum +21 ms/turn, from a late rise in turns 16-20 whose source is NOT the rfu queues (unexplained, n=1) |
| 4 / 5 / 6 | L / S / B | INVALID I12 RADIO | back to back 19:56-20:05. Join median ARQ retransmit rate 28 / 41 / 27 % (every other run <= 2.4 %), air loss 17-37 % both ways, rto pinned at 2.5 s, retx_age 9-17 s; the games dropped the link before the colosseum warp. The eject fallback preceded only run 4 |
| 7 | A | **FAIL: the field symptom** | 18 turns; the host backlog held at 7-9, stepped to 14 at turn 16, then ran away 13 -> 40 -> 52 -> 64 on healthy air (ARQ 0.3-0.5 %). 93 rfu_qdrop (real data discarded), host LINK_ERROR, join TRANSPORT (srtt 2.1 s, txq 227). The 32 lost trace events are that qdrop flood |

**Against the H0 acceptance criteria:** NOT met. V1 (L) and V2 (S) never reached a battle, because both
fell in the radio window, so the rig's hardware controls are unproven. What H0 does show:
- Arm A reproduces the field failure on this pairing, both as growth (run 2) and as the full collapse
  to a link error through a queue overflow (run 7).
- One arm-B run bounded both queues for 20 turns.
- No desync in any of the 3 battles.

### 13.1 H0b (`--order LSB --runs 4`), scored with 713b7a5

| run | arm | verdict | what happened |
|---|---|---|---|
| 1 | L | FAIL SETUP (join pacedrop 13) | the member's walk overshot UP by two tiles under latency and was blocked at x=15; air loss 4.6 / 2.6 %. The first I12 counted this run's retransmits, 57 % of which were spurious |
| 2 | S | FAIL SETUP (join pacedrop 55) | at 6000 us the PSP-1000 fell to 47 fps (core 14.4 ms + 6 ms against a 16.7 ms budget); join backlog 12 -> 24, then discards |
| 3 | B | **FAIL LINK_ERROR, 0 turns** | the battle started on both consoles, and died in the intro before turn 1's action menu (callback2 0x0800AF41 on both). The host's receive queue went 9 -> 44 -> 61 -> 64 between the colosseum and the battle start, and 13 packets were discarded. The host's content share was 73 % in the room and 97 % at the battle start, so keep=2 could shed only 68. The join paged ROM 14 times, its ARQ backed off to a 2.5 s RTO (srtt 177 ms, txq 77), and arrivals came in bursts (hi 61, now 0) |
| 4 | L | INVALID I12 | air loss 31.5 % host->join |

**Every hardware collapse so far is on the HOST side.** The client->host queue reached 36 (H0-1),
64 (H0-7) and 61 (H0b-3), while the join's queue stayed <= 13. `rfu_pace_catchup` is client-only by
construction (host pacecatch = 0 in every run), so the host queue has no restoring force except
shedding, and shedding removes only content-free packets.

## 14. The hold campaign (B vs H), designed 2026-09-27

**Arms, on ONE EBOOT** (9d6382e8, crc32 9056cbcc, commit 1d8e059), both at the owner's 59.73:
- **B** (`rfu_shed_keep = 2`, `rfu_hold = 0`): the discards happen as they always did.
- **H** (`rfu_shed_keep = 2`, `rfu_hold = 1`): nothing is discarded.

Their harness inis differ only in that one line. Stages are in `builds/wireless-rig/hold/growl/`. The
fixture has the timed-hold walk and the start-skew marks.

**Blocks:**
- *Noisy air* (Ad Hoc channel 1, beside the owner's router): `BHHBBHHBBHHB`, 6 per arm.
- *Clean air* (channel 11, the consoles side by side): `BHHBBHHB`, 4 per arm.

Interleaving is what makes drifting air comparable. Runs that I12 voids (air loss > 10 %) are
reported and do not count.

**"Hold works" on hardware means ALL of:**
1. H discards nothing: 0 rfu_qdrop, 0 rfu_pacedrop, 0 rfu_holdfail, over every H run.
2. On noisy air H loses fewer links than B: LINK_ERROR/TRANSPORT failures, B vs H, by Fisher's
   exact test (6 vs 6: 5/6 vs 0/6 gives p = 0.008). Every B failure is attributed (qdrop/pacedrop,
   or another cause).
3. The backlog recovers: each H run's standing backlog returns to <= 2 after every spike, and
   f2m_sum shows no growth (slope within the B-null band).
4. No desync: fingerprints agree at every turn boundary, and the positions agree.
5. On clean air H costs nothing: f2m_sum and turns completed are within B's range, and H never
   holds (rfu_hold hi = 0) unless a burst exceeds 64.

If (2) does not separate, the answer is "not demonstrated on hardware"; the PPSSPP fade result stands
on its own.

**Start skew, per arm and per channel:** `own_echo_ms` is the time from a console starting its walk to
its own avatar's first step, which is the round trip a player feels in the link room. Report the
host's and the join's medians and their difference (the owner's "host gets a half-tile head start");
`other_first_ms` goes alongside.

### 13.2 Channel 11 (growl-ch11, ABBAABBA x9, EBOOT 548ac844): FINAL, scored with 978bdb2

The owner moved both PSPs to Ad Hoc channel 11 (the router and mesh sit on channel 1) and put them
side by side.

| run | arm | turns | f2m_sum first3 -> last3 ms (slope ms/turn) | host standing backlog hi | discards |
|---|---|---|---|---|---|
| 1 (warm-up) | Q (B at 57) | 20 | 5258 -> 5206 (-3.3) | 2 | 0 |
| 2 | A | 20 | 7119 -> 8333 (+100.8) | 11 | 0 |
| 3 | B | 20 | 4969 -> 4956 (+0.5) | 2 | 0 |
| 4 | B | 20 | 5019 -> 4956 (-2.4) | 3 | 0 |
| 5 | A | 20 | 6742 -> 10767 (+176.8) | 17 | 0 |
| 6 | A | 20 | 7825 -> 7926 (-0.2) | 11 | 0 |
| 7 | B | 20 | 4858 -> 4959 (+4.1) | 3 | 0 |
| 8 | B | 20 | 4935 -> 4931 (-2.0) | 3 | 0 |
| 9 | A | 20 | 5879 -> 9548 (+96.7) | 14 | 0 |

All nine runs completed 20 turns on both consoles: no collapse, no discard, no desync.

**Verdict, against the criteria set in advance (section 4):**
- **Every B run on the floor, with no growth: MET.** B's summed wait is 4858-5019 ms at the start
  and 4931-4959 ms at the end (the PPSSPP null floor is ~5.1 s). B's slopes are -2.4 to +4.1 ms/turn,
  and its host standing backlog is <= 3.
- **Slopes separate completely, every B below every A: NOT MET.** Three A runs ratchet
  (+97, +101, +177 ms/turn), but A6 is flat (-0.2) and falls inside B's range. By the rule written in
  advance, the slope claim is "not demonstrated" at 4 vs 4.
- **Turns-to-failure B >= A: tied.** Every run on clean air completed 20 turns.
- **The LEVEL separates completely (not a pre-registered criterion, reported as such):**
  - Every A run starts above every B run (A first3 5879-7825 ms against B 4858-5019).
  - Every A run ends above every B run (last3 7926-10767 against 4931-4959).
  - A 4 vs 4 rank test with complete separation gives one-sided p = 1/70 = 0.014.
  - A6 is the case that shows it: flat, but stuck ~2.8 s above the floor. Arm A's latency ratchets
    up and does not come back, which is the field symptom. On arm B it is never there.
- The per-measure verdicts ("FAIL: join f2m_L slope 2.9 > band 2.0" on B3/B4) are single waits
  moving by a few ms/turn inside a flat sum: the band of 2.0 is too tight for one wait on hardware.
  Use f2m_sum for H1.

**Interpretation.** On clean air, shedding keeps the link at the floor for 20 turns, and without it
the latency accumulates. The evening's channel-1 collapses (H0b, BQ) were the air making bursts that
the 64-slot queue turned into link errors. That is the hold fix's territory, not shedding's.

## 14.1 Hold: PPSSPP validation (final build 9d6382e8, commit 1d8e059)

- **Radio fade** (`net_blackout_ms = 1500` every 20 s, both consoles):
  - H (hold on): 5 runs, **5/5 completed 8 turns**, 0 discards. The hold engaged (15-26 held), the
    join's standing backlog peaked at 43-44 and was back to 0-1 at the end, and the positions agreed.
  - B (hold off): **5/5 died in setup** from 21-22 pacedrops.
  - Arm A: died the same way.
  - Fisher 5/5 vs 0/5: p = 0.008.
  - One H run's f2m_sum rose (6.5 -> 10.5 s) while its queues were back at 0-1: a 1.5 s fade that
    lands inside a turn's measured waits adds up to 1.5 s to them. The other H runs were flat.
- **Random loss:**
  - At 8 % / 120 ms jitter the transport itself outlived the game's link timeout in every arm (I12 radio).
  - At 3 % / 40 ms nothing ever overflowed, so B0 and B were indistinguishable.
- **Detector:** the targeted fault still trips (DESYNC at turn 2, PP 39 -> 38) on the final build.
- **Walk:** the timed-hold walk passed 4/4 with no latency, +50 ms (arms A and B), and a join slowed by
  9500 us. Before the change, the old walk overshot under latency (burst2A: the member at (18, 11)).
- **Start skew (PPSSPP):** host own_echo 57 ms, join 90 ms. In PPSSPP the host sees its own step
  about 2 frames sooner than the join does.
