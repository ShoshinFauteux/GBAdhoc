# DECOMP-FATAL-ERROR — what sets `RFU_STATUS_FATAL_ERROR` in Pokémon Emerald

Source: public pret decomp at `/home/rayquaza/gpsp-e2e/pokeemerald` (WSL). All line
numbers are from that tree. Read-only research; no emulator code touched.

`RFU_STATUS_FATAL_ERROR = 1` (`include/link_rfu.h:36`).
`RfuSetStatus()` is the only writer: `src/link_rfu_2.c:2522-2526` (`gRfu.status = status`).

---

## 1. Every site that sets FATAL_ERROR

There are **exactly 7 call sites**. Six of them are the *same two `case` groups*
replicated across the three LMAN callbacks; only one is independent. The "roughly five
routes, four timing / one queue" characterisation from prior work is **wrong in shape but
right in spirit**: the correct decomposition is **1 queue-overflow route + 5 distinct LMAN
messages**, and all 5 LMAN messages are reachable from the same 3 replicated sites.

### Which callback is live during a Union Room trade

`Task_RunUnionRoom` state `UR_STATE_INIT_LINK` calls `InitializeRfuLinkManager_EnterUnionRoom()`
(`src/union_room.c:2509-2519`, specifically `:2514`), which registers **`LinkManagerCB_UnionRoom`**
(`src/link_rfu_2.c:2640-2648`). The in-room trade does *not* re-register: the joiner becomes
`MODE_CHILD` via `RFUSTATE_UR_PLAYER_EXCHANGE` (`src/link_rfu_2.c:534-543`) and the host becomes
`MODE_PARENT` via `RFUSTATE_UR_FINALIZE` (`src/link_rfu_2.c:551-565`), both still under
`LinkManagerCB_UnionRoom`. `InitializeRfuLinkManager_JoinGroup` / `_LinkLeader` (which register
`LinkManagerCB_Child` / `_Parent`) are used only by the *menu-driven* link-group and Mystery Gift
flows (`src/union_room.c:404, 998, 1322, 1895, 2101, 2269`).

**=> For a Union Room trade + exit, only sites S1, S6 and S7 below are reachable.**
S2–S5 are listed for completeness.

| # | file:line | enclosing fn | condition |
|---|-----------|--------------|-----------|
| S1 | `src/link_rfu_2.c:2003` | `RfuCheckErrorStatus` (1987) | `else if (gRfu.sendQueue.full == TRUE \|\| gRfu.recvQueue.full == TRUE)` |
| S2 | `src/link_rfu_2.c:2252` | `LinkManagerCB_Parent` (2179) | `case LMAN_MSG_LMAN_API_ERROR_RETURN:` |
| S3 | `src/link_rfu_2.c:2261` | `LinkManagerCB_Parent` | `case LMAN_MSG_REQ_API_ERROR: case LMAN_MSG_WATCH_DOG_TIMER_ERROR: case LMAN_MSG_CLOCK_SLAVE_MS_CHANGE_ERROR_BY_DMA: case LMAN_MSG_RFU_FATAL_ERROR:` |
| S4 | `src/link_rfu_2.c:2323` | `LinkManagerCB_Child` (2267) | `case LMAN_MSG_LMAN_API_ERROR_RETURN:` |
| S5 | `src/link_rfu_2.c:2331` | `LinkManagerCB_Child` | same 4-way case group as S3 |
| **S6** | `src/link_rfu_2.c:2502` | **`LinkManagerCB_UnionRoom` (2370)** | `case LMAN_MSG_LMAN_API_ERROR_RETURN:` — also sets `gRfu.isShuttingDown = TRUE` |
| **S7** | `src/link_rfu_2.c:2511` | **`LinkManagerCB_UnionRoom`** | same 4-way case group as S3 — also sets `gRfu.parentFinished = FALSE` |

Nothing else in the tree writes status 1. `src/link_rfu_2.c:1741`
(`RfuSetStatus(gRfu.childRecvStatus, 0)` in `GetJoinGroupStatus`) *could* in principle copy a
partner-supplied byte of value 1 into `gRfu.status`, but the values a partner can send are
`RFU_STATUS_*` group codes written by `SendRfuStatusToPartner`/`SendLeaveGroupNotice`
(`src/link_rfu_2.c:1686-1699`); I found no path that sends `1`. Flagged as *not verified
exhaustively*, but it is not a Union-Room-exit route.

### The 5 LMAN messages that reach S3/S5/S7, and where they come from

| LMAN msg | value | emitted at | trigger (code, not inference) |
|---|---|---|---|
| `LMAN_MSG_REQ_API_ERROR` | 0xF0 | `src/AgbRfu_LinkManager.c:951`, in `rfu_LMAN_REQ_callback` (598) | `if (reqResult != 0)` for **any** REQ, except the `ID_SP_START_REQ` / `PCSWITCH_2ND_SP` special case (`:935-941`) |
| `LMAN_MSG_WATCH_DOG_TIMER_ERROR` | 0xF1 | `src/AgbRfu_LinkManager.c:427`, in `rfu_LMAN_syncVBlank` (423) | `if (rfu_syncVBlank())` → returns 1 when `gRfuStatic->watchdogTimer == 0` (`src/librfu_rfu.c:867-876`) |
| `LMAN_MSG_CLOCK_SLAVE_MS_CHANGE_ERROR_BY_DMA` | 0xF2 | `src/AgbRfu_LinkManager.c:957` | `if (reqCommandId == ID_CLOCK_SLAVE_MS_CHANGE_ERROR_BY_DMA_REQ)` — synthesised locally by `STWI_intr_timer` case 3 (`src/librfu_stwi.c:496-502`) |
| `LMAN_MSG_LMAN_API_ERROR_RETURN` | 0xF3 | `:135, :141, :154` in `rfu_LMAN_establishConnection` (127); `:193, :199, :212` in `rfu_LMAN_CHILD_connectParent` (186); `:1325, :1337` in two `UNUSED` fns | LMAN-API called while busy / while AGB is clock slave / bad args / PID not found |
| `LMAN_MSG_RFU_FATAL_ERROR` | 0xFF | `:460` (`LMAN_FORCED_STOP_AND_RFU_RESET`) and `:474` (`LMAN_STATE_SOFT_RESET_AND_CHECK_ID`), both in `rfu_LMAN_manager_entity` (432) | `rfu_LMAN_REQBN_softReset_and_checkID() != RFU_ID` |

`reqResult` for `REQ_API_ERROR` is **not only** an adapter-returned status byte. It is
`gSTWIStatus->error`, which librfu sets *locally* without the adapter ever replying with an
error code:

- `ERR_REQ_CMD_IME_DISABLE (6)` — `src/librfu_stwi.c:550`
- `ERR_REQ_CMD_SENDING (2)` — `src/librfu_stwi.c:558` (REQ issued while one is in flight)
- `ERR_REQ_CMD_CLOCK_SLAVE (4)` — `src/librfu_stwi.c:567` (REQ issued while `msMode == AGB_CLK_SLAVE`)
- `ERR_REQ_CMD_CLOCK_DRIFT (1)` — `src/librfu_stwi.c:623, 630`, from `STWI_restart_Command` after
  `recoveryCount` reaches 2, i.e. **the SIO32 word handshake did not advance in time**
- `ERR_REQ_CMD_ACK_REJECTION (3)` — `src/librfu_intr.c:132` (`ackActiveCommand == 0xEE`) and
  `src/librfu_intr.c:205, 253`

Codes: `include/librfu.h:173-178`.

### Reachability during a Union Room EXIT, and what an emulated adapter must do wrong

Ranked by fit with the field evidence (adapter never errored; recv queue never overflowed;
client went CLIENT→IDLE via adapter RESET with no disconnect issued; host then saw a 4 s
inactivity timeout; wireless link healthy, srtt ~63 ms).

**Rank 1 — S7 via `LMAN_MSG_CLOCK_SLAVE_MS_CHANGE_ERROR_BY_DMA` (0xF2).**
Path: `STWI_intr_timer` `timerState == 3` (`src/librfu_stwi.c:496-502`) → `STWI_reset_ClockCounter()`
→ `callbackM(ID_CLOCK_SLAVE_MS_CHANGE_ERROR_BY_DMA_REQ, 0)` → `rfu_LMAN_REQ_callback:955-959`
→ S7. `timerState 3` is armed *only* by `STWI_set_timer_in_RAM(100)` in the **clock-slave**
SIO paths (`src/librfu_intr.c:162, 221, 281`). Requirements to hit it: the AGB is clock slave
and the 32-bit SIO exchange that hands the clock back does not advance within **100 ms**.
Fits the evidence exactly — no adapter error code is involved (`reqResult` is literally passed
as `0`), no disconnect is issued, and the LMAN state is *not* forced to READY by this branch,
so the game simply stops driving the link and the peer times out. It is also the branch most
exposed on the **child**, which is the joining console (see asymmetry note below).

**Rank 2 — S7 via `LMAN_MSG_WATCH_DOG_TIMER_ERROR` (0xF1).**
`gRfuStatic->watchdogTimer` is armed to **360 frames (≈6.0 s)** on entry into clock-slave
(`src/librfu_rfu.c:849-855`) and re-armed to 360 on every `rfu_REQBN_watchLink` call while
`flags & 4` (`src/librfu_rfu.c:894-896`). On expiry `rfu_syncVBlank` tears the link down
*locally* — `rfu_STC_removeLinkData` for every slot and `gRfuLinkStatus->parentChild = MODE_NEUTRAL`
(`src/librfu_rfu.c:867-875`) — and returns 1. Requirement: no MSC / clock-slave notification for
6 s. Matches "no disconnect command ever issued" and "adapter RESET" perfectly, but 6 s of
silence is hard to square with "link healthy, srtt 63 ms" unless the adapter wedges. Second
place on that basis alone.

**Rank 3 — S7 via `LMAN_MSG_REQ_API_ERROR` (0xF0) with a locally-generated `reqResult`.**
Most likely sub-cases: `ERR_REQ_CMD_CLOCK_DRIFT` (SIO32 word handshake stalled past the
80 ms → 50 ms timers, twice) and `ERR_REQ_CMD_CLOCK_SLAVE` (game issued a REQ while the
adapter still had the AGB in clock-slave mode — a master/slave-state disagreement between
emulated adapter and game). Consistent with "adapter never returned an error to a command":
these never involve an adapter error byte. Note this branch *does* force
`lman.state = lman.next_state = LMAN_STATE_READY` (`:947-950`) and then
`rfu_LMAN_managerChangeAgbClockMaster()` — i.e. the game abandons the link manager without
disconnecting, exactly the observed "gave up before trying to leave".

**Rank 4 — S6 via `LMAN_MSG_LMAN_API_ERROR_RETURN` (0xF3), `LMAN_ERROR_AGB_CLK_SLAVE`.**
`rfu_LMAN_establishConnection:139-143` and `rfu_LMAN_CHILD_connectParent:197-201` both fire this
if `rfu_getMasterSlave() == AGB_CLK_SLAVE`. This is genuinely reachable on Union Room *re-entry*
after the trade (`UR_STATE_INIT_LINK` → `RFUSTATE_UR_CONNECT` →
`rfu_LMAN_establishConnection(MODE_P_C_SWITCH, 0, 240, ...)`, `src/link_rfu_2.c:527`), or on
`Task_TryConnectToUnionRoomParent` (`src/link_rfu_2.c:2847-2886`). Requirement: the emulated
adapter leaves the AGB stuck in clock-slave mode across the transition. Also sets
`gRfu.isShuttingDown = TRUE` (`:2504`), which stops `RfuMain1`/`RfuMain2` entirely
(`src/link_rfu_2.c:2025, 2046`) — a very good match for "the game had already given up".
Ranked below 1-3 only because it requires a specific state mismatch at a specific moment.

**Rank 5 — S7 via `LMAN_MSG_RFU_FATAL_ERROR` (0xFF).**
Only from `rfu_LMAN_REQBN_softReset_and_checkID() != RFU_ID`
(`src/AgbRfu_LinkManager.c:454-462, 466-476`). Requires the emulated adapter to fail the
soft-reset ID handshake. Note `rfu_LMAN_stopManager` is called with `forced_stop = FALSE`
at **every** call site in the game (`src/link_rfu_2.c:361, 546, 574, 1585, 2484`), so the
`LMAN_FORCED_STOP_AND_RFU_RESET` variant (`:453-463`) is **unreachable**; only
`LMAN_STATE_SOFT_RESET_AND_CHECK_ID` (entered from `rfu_LMAN_initializeRFU`) can fire it.
Inconsistent with the observed RESET succeeding.

**Rank 6 (ruled out) — S1, queue overflow.** `gRfu.recvQueue.full` / `sendQueue.full` are set
only in `RfuRecvQueue_Enqueue` (`src/link_rfu_3.c:393`) and `RfuSendQueue_Enqueue`
(`src/link_rfu_3.c:426`) when `count` reaches `RECV_QUEUE_NUM_SLOTS 32` / `SEND_QUEUE_NUM_SLOTS 40`
(`include/link_rfu.h:29-30`). Field evidence reports 0 drops, so this is excluded — but see §2,
it bounds pipelining depth.

**Not a route:** `LinkRfu_FatalError()` (`src/link_rfu_2.c:1420-1425`) despite the name only sets
`gRfu.disconnectMode = RFU_DISCONNECT_ERROR`; that path ends in `RFU_STATUS_CONNECTION_ERROR`
(`src/link_rfu_2.c:951-957`, `:2772`), i.e. the **recoverable** screen. Likewise the overworld
keep-alive (`src/overworld.c:2284-2288`, >60 frames) is recoverable.

### Why the joiner and not the host (inference, but code-grounded)

All three top-ranked routes are gated on the AGB being **clock slave**:
- watchdog: `flags |= 4` only when `masterSlave == AGB_CLK_SLAVE` (`src/librfu_rfu.c:849-856`);
- `timerState 3` (100 ms) armed only in `sio32intr_clock_slave` paths (`src/librfu_intr.c:162, 221, 281`);
- `ERR_REQ_CMD_CLOCK_SLAVE` by definition (`src/librfu_stwi.c:565-571`).

Both roles do briefly enter clock-slave (`rfu_REQ_sendData` sends `DataTxAndChangeREQ` when
`clockChangeFlag`, `src/librfu_rfu.c:1667-1671`), but only the **child** *stays* there:
`rfu_LMAN_REQ_sendData` forces `clockChangeFlag = TRUE` iff
`lman.childClockSlave_flag == RFU_CHILD_CLOCK_SLAVE_ON` (`src/AgbRfu_LinkManager.c:52-60`), and the
child's whole frame loop is driven from `MSCCallback_Child` while clock-slave
(`src/link_rfu_2.c:578-598`). The parent returns to master each frame via its own
`RfuMain1_Parent` REQs. So the joiner has orders of magnitude more clock-slave residency and is
the console that eats these timeouts. The host meanwhile only ever sees
`LMAN_MSG_LINK_LOSS_DETECTED_AND_DISCONNECTED` → `RfuSetStatus(RFU_STATUS_CONNECTION_ERROR, msg)`
at `src/link_rfu_2.c:2492` — the recoverable variant. **This is a complete explanation of the
observed host/joiner asymmetry.**

One more relevant gate: `RfuCheckErrorStatus` only raises the error screen when
`lman.childClockSlave_flag == 0` (`src/link_rfu_2.c:1989`). So the joiner's fatal status can be
latched during the trade and only *displayed* later — consistent with the error appearing "on
exit" rather than during the trade.

---

## 2. Maximum staleness / latency librfu tolerates

All values verbatim from the decomp.

### Hard SIO32 handshake deadlines (these are the binding real-time constraints)

`STWI_set_timer` / `STWI_set_timer_in_RAM` (`src/librfu_stwi.c:506-532`, `src/librfu_intr.c:345-371`)
use TIMER prescaler 1024; the `count` argument is milliseconds:

| count | `timerL` reload | `timerState` | on expiry (`STWI_intr_timer`, `src/librfu_stwi.c:482-504`) |
|---|---|---|---|
| 50 ms | 0xFCCB | 1 | `STWI_restart_Command()` |
| 80 ms | 0xFAE0 | 2 | `timerActive = 1`, arm 50 ms |
| 100 ms | 0xF996 | 3 | `timerActive = 1`, `STWI_reset_ClockCounter()`, **emit `ID_CLOCK_SLAVE_MS_CHANGE_ERROR_BY_DMA_REQ` → FATAL** |
| 130 ms | 0xF7AD | 4 | `STWI_restart_Command()` |

Arming points:
- **80 ms** re-armed at the top of every clock-master SIO32 interrupt (`src/librfu_intr.c:39`).
- **130 ms** when an unexpected word arrives in master state 0 or 1 (`src/librfu_intr.c:59-60, 85-86`).
- **100 ms** in the clock-slave paths (`src/librfu_intr.c:162, 220-221, 280-281`). **Clock-slave only.**

`STWI_restart_Command` (`src/librfu_stwi.c:611-637`): retries while `recoveryCount < 2`
(so **2 retries**), then `ERR_REQ_CMD_CLOCK_DRIFT` → `LMAN_MSG_REQ_API_ERROR` → FATAL.

**Practical budget:** as clock master, ~80 ms per SIO word before the retry machinery starts,
~3 × 130 ms ≈ **390 ms** absolute before FATAL. As clock slave, **100 ms is a hard wall with
no retries** — that is the tightest number in the whole stack.

### Frame-counted watchdogs

| constant | value | units | file:line | effect on expiry |
|---|---|---|---|---|
| `watchdogTimer` | **360** | VBlanks ≈ **6.0 s** | `src/librfu_rfu.c:854, 896` | link torn down locally, `rfu_syncVBlank` returns 1 → `WATCH_DOG_TIMER_ERROR` → **FATAL** |
| `nowWatchInterval` | 4 | VBlanks | `src/librfu_rfu.c:900` (`LIBRFU_VERSION >= 1026`) | how often link-loss is evaluated |
| `NI_failCounter_limit` | **300** | VBlanks ≈ **5.0 s** | `src/link_rfu_2.c:131` | `rfu_changeSendTarget` / `rfu_NI_stopReceivingData` (`src/AgbRfu_LinkManager.c:1269, 1285`) — **not** fatal |
| `linkRecovery_period` | **600** | VBlanks ≈ **10.0 s** | `src/link_rfu_2.c:130, 2647`; also `rfu_LMAN_setLinkRecovery(1, 600)` at `src/link_rfu_2.c:1578, 1826` | `LINK_RECOVERY_FAILED_AND_DISCONNECTED` → CONNECTION_ERROR (recoverable) |
| `linkRecovery_enable` | FALSE by default | — | `src/link_rfu_2.c:129`, forced 0 for Union Room at `:2646` | enabled during trade by `Rfu_SetLinkRecovery(TRUE)` (`src/trade.c:530`) and after player exchange (`src/link_rfu_2.c:1826`) |
| `name_accept_period` | **240** | VBlanks ≈ **4.0 s** | `src/link_rfu_2.c:355, 527` (`rfu_LMAN_establishConnection(..., 240, ...)`) | `nameAcceptTimer` → child-name accept expiry. **This is almost certainly the "4-second inactivity timeout" the host reports.** |
| `MC_TimerCount` | **32** | ×16.7 ms ≈ **534 ms** | `src/link_rfu_2.c:122` | RFU-internal MC timer → link-loss detection inside the adapter |
| `maxMFrame` | **4** | retransmits | `src/link_rfu_2.c:121` | RFU-level frame retransmit limit |
| `numChildRecvErrors` | **> 4** | consecutive bad seq | `src/link_rfu_2.c:851-853` | `RfuSetErrorParams` only (status unchanged) |
| overworld keep-alive | **> 60** | frames ≈ 1.0 s | `src/overworld.c:2286-2288` | `LinkRfu_FatalError()` → recoverable |

### Implication for lockstep + N frames of pipelining

Viable, with these bounds:

1. **N is bounded by the receive queue, not by any timer.** `RECV_QUEUE_NUM_SLOTS = 32`,
   `SEND_QUEUE_NUM_SLOTS = 40` (`include/link_rfu.h:29-30`); overflow sets `full = TRUE`
   (`src/link_rfu_3.c:393, 426`) which is site S1 → **FATAL**. So **N ≤ 31 frames** of buffered
   parent→child payload with margin, and the child's outbound backlog must stay under 40.
2. **Per-SIO-word latency, not per-frame latency, is what kills you.** The adapter may not defer
   an SIO32 word exchange by more than ~80 ms as clock master, and **not more than 100 ms as
   clock slave** — with no retries in the slave case. Pipelining N frames is fine only if the
   emulated adapter still answers each SIO word promptly out of a local buffer.
3. **Clock hand-back (`MS_CHANGE` / `DATA_TX_AND_CHANGE` → MSC) is the critical path.** 100 ms wall,
   then 6 s watchdog. Any design where the clock-slave→master transition waits on the network is
   unsafe.
4. `RfuMain2_Parent` busy-waits `while (gRfu.parentFinished == FALSE)` with only
   `gRfu.errorState != RFU_ERROR_STATE_NONE` as an escape (`src/link_rfu_2.c:835-839`) — the host
   spins the CPU until the MSC callback lands. Pipelining must not delay the parent's MSC.

---

## 3. Symbol and address of the RFU status word

**Symbol:** `gRfu` — `COMMON_DATA struct RfuManager gRfu` at `src/link_rfu_2.c:81`, declared
`include/link_rfu.h:238`.
**Field:** `gRfu.status`, `u8`, at **offset `0x0F1`** within the struct
(`include/link_rfu.h:193`; struct size `0xCF4`, `include/link_rfu.h:234`).
Useful neighbours: `gRfu.errorInfo` u16 @ `0x00A`, `gRfu.errorState` @ `0x0EE`,
`gRfu.isShuttingDown` @ `0x0EF`, `gRfu.linkLossRecoveryState` @ `0x0F0`.

**Absolute address: cannot be determined from the decomp alone — stated explicitly as required.**
`gRfu` is a COMMON symbol, so its address is decided at link time, and this checkout has no
build products (`build/` absent, no `.map`, no ARM toolchain in the WSL image — `arm-none-eabi-gcc`
is not installed). What the tree does pin down:

- COMMON section base is **`0x030022A8`** (IWRAM) — `ld_script.ld:43` comment, section body
  `ld_script.ld:44-46`.
- COMMON object order is `main.o, bg.o, (.align 4), window.o, text.o, sprite.o, link.o,
  AgbRfu_LinkManager.o, link_rfu_2.o, ...` — `sym_common.txt:1-10`.
- Within `link_rfu_2.o`, `gRfuAPIBuffer` (`RFU_API_BUFF_SIZE_RAM = 0x0E64`, `include/librfu.h:93`)
  precedes `gRfu` (`src/link_rfu_2.c:80-81`).
- Immediately before it, `AgbRfu_LinkManager.o` contributes exactly one COMMON symbol,
  `LINK_MANAGER lman` (`src/AgbRfu_LinkManager.c:17`).

I computed a candidate by summing the preceding COMMON symbols and got a value that did not
agree with my recollection of the published `gMain` address, which means either the struct-size
arithmetic or the intra-object COMMON ordering assumption is wrong. **I am deliberately not
publishing a guessed address**; a wrong watch address is worse than none.

**How to get it definitively (pick one):**
- Build the decomp with a real ARM toolchain and read `pokeemerald.map` (or `nm -n`), grep `gRfu`.
- Or take any public BPEE0 symbol list and read `gRfu`; then watch `gRfu + 0xF1`.
- Or locate it at runtime: `gRfu` is `0xCF4` bytes ending with `numChildRecvErrors`/`childRecvIds`,
  and `gRfu.parentChild` (offset `0x00C`) is set to `0xFF` by `ResetLinkRfuGFLayer`
  (`src/link_rfu_2.c:297-314`) immediately after the struct is zeroed — a distinctive
  "0xCF4 zero bytes with 0xFF at +0x00C" signature in IWRAM right after `OpenLink()`.

**Watch semantics once you have the address:** the byte flips `0 → 1` (`RFU_STATUS_OK` →
`RFU_STATUS_FATAL_ERROR`); `2` is `RFU_STATUS_CONNECTION_ERROR`, the recoverable variant.
`ResetLinkRfuGFLayer` zeroes the whole struct (preserving only `errorState` when it is
`RFU_ERROR_STATE_IGNORE`), so the status resets to 0 on each `OpenLink()`/`InitRFUAPI()`.
Watching `gRfu.errorInfo` (u16 @ `+0x00A`) at the same time gives the LMAN `msg` that caused it
— `0xF0`/`0xF1`/`0xF2`/`0xF3`/`0xFF` distinguishes the five routes in §1 directly.

---

## Caveats

- Everything in §1 and §2 is quoted from the decomp with citations. The role-asymmetry argument
  at the end of §1 and the ranking are **inference** built on those citations, and are labelled
  as such.
- I did not exhaustively prove that no partner-supplied byte can reach `gRfu.status` via
  `src/link_rfu_2.c:1741`; I found no sender of value `1`, but did not enumerate every writer of
  `gRfu.childRecvStatus`.
- The claim that the host's "4-second inactivity timeout" is `name_accept_period = 240`
  (`src/link_rfu_2.c:527`) is a numeric match, not a proven identification.
