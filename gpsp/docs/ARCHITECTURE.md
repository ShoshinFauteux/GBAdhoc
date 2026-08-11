# ARCHITECTURE — gpSP-AdHoc

Living document. Started in Phase 3 with the netdrv section; frontend/UI
sections land with their phases. Plan references are to gpsp-adhoc-plan.md.

---

## netdrv — the Netpacket-over-transport driver (plan §4.3, ADR-0003, ADR-0008)

### Module map

| file | role |
|---|---|
| `netdrv/netdrv.h` | public API: session, send, pump, stats, transport vtable |
| `netdrv/netdrv_wire.h` | wire format constants + build/parse (shared with tests) |
| `netdrv/netdrv.c` | session state machine, roster, per-peer ARQ — plain C, no OS deps; time enters via `pump(now_us)`, I/O via the vtable |
| `netdrv/transport_udp.c` | desktop backend: POSIX UDP, MACs faked from ip:port, fault-injection shim |
| `frontend-common/netpacket_host.c` | bridge to the core's `retro_netpacket_callback` (env call 78), EVT logging, net_stats |
| `netdrv/tests/test_netdrv.c` | unit suite on a deterministic in-memory mesh + one real-UDP test (`make -C netdrv test`) |
| `netdrv/transport_adhoc.c` | PSP backend: sceNetAdhoc PDP, static singleton, RX thread + SPSC ring (Phase 4; semantics per ADHOC-NOTES §8/§11) |

### Wire format

Every datagram = 16-byte header + payload, exact length, little-endian:

```
u32 magic  'GPN1'          u16 seq   ARQ / unreliable sequence
u8  ver    1               u16 ack   cumulative: next seq expected from addressee
u8  type   see below       u16 len   payload bytes (<= ND_MAX_PAYLOAD)
u8  src    client_id 0-4   u16 crc   CRC16-CCITT (0x1021/0xFFFF) over
u8  dst    id/0xFF bcast             header-with-crc=0 + payload
```

Types: `DATA(0) ACK(1) UDATA(2) UDATA_US(3) JOIN(4) WELCOME(5) ROSTER(6)
BYE(7) PING(8)`. Special ids: `0xFF` broadcast, `0xFE` unassigned joiner,
`0xFD` (in WELCOME.your_id) join refused. Payload budget 560 B per
ADR-0003 (RFU max 104 B, AW-cable headroom); max frame 576 B — fits a
single PDP datagram, no fragmentation.

Payload budget is a build constant (`ND_MAX_PAYLOAD`): 560 B on
desktop/UDP, **144 B on PSP/ad-hoc** (ADR-0016 — RFU max is 104 B, the
roster is 138 B, and the 560 B headroom existed only for out-of-scope
cable modes). Both ends of a session always run the same profile.
CRC16-CCITT is table-driven (512 B of .rodata, ~8× cheaper than the old
bit-serial loop; every frame is CRC'd on build and on parse).

A receiver drops any frame failing: size ≥ 16, magic, ver, known type,
`len ≤ ND_MAX_PAYLOAD`, datagram size == `16+len` (exact), CRC — then, on the data
plane, src-id↔MAC roster consistency. This wall is load-bearing: the core's
receive path is not garbage-tolerant (SERIAL-PROTO-NOTES §8; rfu.c:735,
rfu.c:845). Nothing that fails validation is ever delivered.

### Session state machine

States: `IDLE → JOINING → ACTIVE` (client), `IDLE → ACTIVE` (host).

- **Host** (`netdrv_host`): local id 0, session starts immediately. On
  `JOIN{proto[24], nick[16], nonce u32}` (broadcast by joiners every 500 ms):
  protocol string must equal the core's `protocol_version` ("gpSP v1.0")
  or the JOIN is ignored; assign lowest free id 1-4; `WELCOME{your_id, gen,
  roster}` unicast; roster broadcast to all. Duplicate JOIN (lost WELCOME)
  → idempotent re-WELCOME. Same MAC + *new* nonce = process restart →
  channel reset, id kept, re-admitted. Session full → WELCOME with
  your_id=0xFD (client stops, reason `REFUSED`).
- **Client**: retries JOIN until WELCOME; on WELCOME stores host MAC,
  fires `session_started(id)`, applies the roster.
- **Roster is host-authoritative state, not events**: `{gen u8, entries
  {id, mac[6], nonce u32, nick[16]}}` broadcast on every change and
  refreshed every 1 s. Clients apply only newer generations (int8 wrap
  compare) and diff → `peer_connected` / `peer_disconnected`; a nonce/MAC
  change under a reused id resets that ARQ channel. A client that falls
  out of the roster tears down (`EVICTED`). This is deliberately
  loss-tolerant: a dropped ROSTER costs one refresh interval, never
  consistency.
- **Peer frame-rate report (ADR-0027)**: PING optionally carries a 2-byte
  little-endian emulated frame rate in hundredths (5973 = 59.73 fps), on a
  **separate 500 ms cadence** that runs regardless of other traffic — the
  keepalive arm below is suppressed by any other send, so during a trade
  (DATA every frame) it would never fire, which is exactly when the number
  is needed. Wire compatible both ways: a peer that does not report sends
  the bare PING it always did and reads back as *unknown* (0), never as
  *slow*, and a bare PING never clobbers a previously reported value.
  `netdrv_set_local_fps()` / `netdrv_peer_min_fps()`; netdrv carries the
  number and owns none of the policy. The value is a console's **capability**
  (work-time derived, throttle-independent) — see ADR-0027 for why
  advertising the *achieved* rate makes the pair ratchet to a standstill.
- **Keepalive/death**: PING (carrying the piggyback ack) to every peer
  idle ≥ 250 ms; peer dead after 8 s of silence (ADR-0010: 4 s fired
  spuriously when a loaded CI host stalled a whole process; real radio has
  dropouts too — plan risk #5 says stay generous; the game's own error
  path reacts far sooner). All frontend time sources feeding netdrv are
  CLOCK_MONOTONIC — wall-clock steps must never age ARQ/keepalive timers. **Death authority**: the
  host judges clients; clients judge only the host link (host dead → whole
  session torn down, per plan §4.3 — no host migration). Client↔client
  liveness is host-relayed via roster generations, which removes
  split-brain entirely.
- `BYE` on clean leave (host BYE = session over for clients).

### Data plane / ARQ

Full mesh (ADR-0003): any peer unicasts to any client_id; reliable
broadcast fans out over each per-peer ARQ channel. Per peer pair:

- TX: a fixed ring of queued payloads (desktop 192, PSP 384) with a sliding
  window (desktop/PSP 32) and an **adaptive per-peer RTO** (ADR-0017).
  `FLUSH_HINT` honored by transmitting at enqueue time when inside the
  window (nothing is ever Nagled).
- **RTO** (ADR-0017, supersedes ADR-0010's fixed 30 ms): per peer,
  `SRTT`/`RTTVAR` EWMA → `RTO = SRTT + 4·RTTVAR`, clamped to a
  **transport-level** floor/ceiling — desktop/UDP 30 ms/240 ms (loopback
  SRTT ≈1 ms so the floor governs; Gate-3 timing unchanged), PSP/ad-hoc
  **100 ms/800 ms** (real radio RTT is 40–80 ms). Karn's algorithm: an ack
  for a retransmitted slot is never sampled, and the exponential backoff
  is **retained at peer level** (one doubling per RTO interval) — without
  that retention, a floor below the true RTT means every first transmission
  times out, every ack is ambiguous, and the estimator can never start.
  Why this exists: the loopback-tuned 30 ms floor produced a measured
  retx/acked of 2.8 with 68 % duplicate RX on real radio that had lost
  nothing (docs/HANDOFF.md issue #2).
- **Loss recovery is a separate deadline from pacing.** `ND_RTO_FIRST_MAX_US`
  caps a slot's *first* retransmission; the adaptive RTO governs every
  retransmission after it. The emulated RFU exchange dies if a lost frame
  takes much longer than ~60 ms to come back (ADR-0010), which no honest
  RTO on a 30-80 ms link can satisfy — so each transport declares it:
  **desktop/UDP 30 ms** (sdl/Makefile: 5 % injected loss with nothing
  underneath to repair it), **PSP/ad-hoc unset** (802.11 retransmits below
  us; the estimator governs). Getting this wrong is not theoretical — the
  first adaptive build passed every PSP radio profile and stalled the
  Gate-3 desktop trade.
- RX: cumulative acks piggybacked on every unicast frame (DATA/ACK/PING);
  in-order delivery with a reorder stash; duplicates re-acked and dropped.
  A DATA frame is acked **in the tick it arrives**: a bare ACK goes out
  immediately after the transport drain when we have nothing due to
  piggyback on, so ack latency (part of the peer's RTT sample) never waits
  on the retx/keepalive scan or an early return from it.
- Unreliable path (`UDATA` drop-stale by seq, `UDATA_US` always-deliver)
  is implemented but cold: the gpsp core sends 100 % RELIABLE|FLUSH_HINT
  (SERIAL-PROTO-NOTES §2.5).
- **RELIABLE is never dropped** (ADR-0016). A full ring toward a live peer
  is *backpressure*: the payload goes to a per-peer **spill FIFO**
  (malloc'd per payload, order preserved) and is pulled back into the ring
  as it drains — `netdrv_send` returns success. Only if the spill is also
  exhausted (`ND_SPILL_MAX` 512/peer) or malloc fails does the driver
  count `tx_overflow` and **fail the session explicitly** with
  `ND_STOP_TX_FAILED`, raised from the next pump (never under the core's
  `send_fn` stack) → `EVT net_error reason=txq_overflow` + a user-visible
  toast + teardown. Oversize payloads are refused the same way, never
  truncated. A full ring toward a peer silent > 2× keepalive stays
  `tx_drop_dead` and does not spill (ADR-0012 amendment).
  `netdrv_send_capacity()` remains a pacing hint for frontend senders.

Throughput note: window/RTT bounds reliable throughput. The core's active
link demands ~120 payload/s (Gate-3 measurement). Window 32 sustains
~1000/s at 30 ms RTT, ~400/s at 80 ms, ~200/s at 160 ms — so the ad-hoc
profile keeps ≥3× headroom across the whole plausible radio range, where
the Phase-4 window of 16 had none left at 80 ms.

Memory note: a netdrv instance is `5 × (ND_TXQ_CAP × slot + ND_WINDOW ×
rxslot)`. Desktop (560 B payload, 192/32) ≈ 600 KiB. **PSP/ad-hoc**
right-sizes the payload to 144 B (ADR-0016: RFU max is 104, roster 138 —
the 560 B budget existed for cable modes that are out of scope), which
buys `ND_TXQ_CAP` 96 → 384 and `ND_WINDOW` 16 → 32 at neutral cost:
instance ≈322 → ≈347 KiB while the transport's .bss RX ring drops ≈37 →
≈11 KiB. A build whose payload budget cannot hold the roster/JOIN/RFU
maxima fails to compile (static assert in netdrv.c).

### Threading contract

Single-threaded pump model. **Every** netdrv call happens on the emu/main
thread; `netdrv_pump(now_us)` runs once per frame plus inside the core's
`poll_receive` callback (the core polls mid-frame in RFU WAITEVENT states,
rfu.c:871-884). Callbacks (`deliver` → core `receive`, peer events) are
invoked inline from pump; `netdrv_send` is re-entrant from inside
`deliver` because the core sends ACKs from inside `receive`
(SERIAL-PROTO-NOTES §6). Nested pump calls are guarded (`in_pump`).

**The mid-frame poll is gated (ADR-0021).** `poll_receive` reaches us from
`update_serial()`, i.e. on every video event while the RFU waits, so the
frontend asks `netdrv_poll_needed()` first and only pumps when the answer
is yes — a datagram is waiting (`nd_transport.pending`, which must be
syscall-free) or ARQ payload is queued and has not gone out (`tx_due`,
which is what keeps the core's outbound latency sub-frame). The predicate
is conservative: saying yes when idle is merely wasteful, so a transport
that leaves `pending` NULL (desktop/UDP) keeps the old behaviour exactly.
`nd_transport` therefore has OPTIONAL members, and **every**
`*_transport_iface()` must `memset` the struct before filling it — callers
pass uninitialised stack storage, and an optional hook left as stack
garbage is a call through a stack value (it segfaulted the desktop twin
once; `test_udp_backend` now asserts against it).

**PSP (Phase 4, implemented — `netdrv/transport_adhoc.c`)**: the RX
thread (prio 0x1E, just above main's 0x20) lives entirely *below* the
transport line: it blocks in `sceNetAdhocPdpRecv` with 250 ms timeout
slices (bounds teardown latency), reads the datagram length from the s32
in/out param (never the return value — ADHOC-NOTES §11.7), and publishes
into a 64-slot SPSC ring (volatile head/tail; single-CPU MIPS);
`transport_adhoc`'s `recv()` hook drains that ring on the main thread.
netdrv itself needed no changes and no locks. `send_to`/`broadcast` are
nonblocking `PdpSend` (WOULD_BLOCK counts as loss — ARQ owns
reliability); broadcast = FF:FF:FF:FF:FF:FF; `local_addr` = the cached
`sceWlanGetEtherAddr` MAC. Since ADR-0021 the send is **mirrored**: it goes
into a second 32-slot SPSC ring (main thread produces, a TX thread
consumes and makes the syscall), so `PdpSend` — whose real cost only
hardware knows — need not run on the emulation thread. FIFO, so ordering
holds; a full ring sends inline rather than dropping; teardown drains the
ring before `PdpDelete` so BYE frames still leave. `config.ini
net_tx_thread` picks the TX thread's priority relative to main:
1 = prompt (0x1F, above — default), 2 = deferred (0x21, below, so sends
land in the vblank slack), 0 = no thread at all.

A **third** thread joins them since ADR-0024: `gpsp_io` at priority 0x22,
below main, whose only job is the memory stick. `fe_evt`/`fe_log` format
a line into a 16 KiB SPSC byte ring and signal it; it does the
`fwrite`+`fflush`. The field measured one flush at 12002 µs (PSP-1000) and
12525 µs (PSP-3000) — three quarters of a vblank period, on the emulation
thread, for one log line. Priority 0x22 is the point: the writes land in
the slack the emulator already donates at the vblank wait, and nothing
ever waits on a log line. It starts only after `fe_host_boot()` (boot
diagnostics stay synchronous, so a load failure reaches the stick even if
the main loop never runs) and every exit path funnels through one
`evt_shutdown()` that stops the thread before draining the ring — exactly
one consumer, ever. `config.ini log_thread` (synonym `net_log_thread`)
= 0 restores the inline path for a hardware A/B without a rebuild.
`EVT sess_cost`'s `evt=` field now prices the *emulation thread's* share
and the new `evtio=us/max/drop/hi` prices the stick. The RX scratch is MFS-sized (1444 B) so an
oversize/foreign datagram is consumed and dropped instead of wedging the
socket (NOT_ENOUGH_SPACE leaves it queued, §11.7). Bring-up/teardown
follow the verified ADHOC-NOTES §1.5 ladder exactly (WLAN-switch check
first, strict reverse teardown, partial-init unwind that preserves the
failing stage + SCE code for the EVT log); the whole transport is one
static .bss singleton — no heap (ADR-0007). The frontend surface is
.gpsp-harness.ini-driven (`host=1`/`join=1`/`nettest=1`) until the wireless
UI panel lands.

Memory: with the desktop sizing a netdrv instance is ≈ 600 KiB — over the
PSP's post-boot ~512 KiB max contiguous block. The PSP build therefore
compiles the **ad-hoc profile** (`-DND_MAX_PAYLOAD=144 -DND_WINDOW=32
-DND_TXQ_CAP=384 -DND_RTO_MIN_US=100000 -DND_RTO_MAX_US=800000`,
psp/Makefile — ADR-0016/ADR-0017), giving ≈ 347 KiB heap-allocated at
session start, plus ≈ 12 KiB transport .bss (ring + MFS scratch) and the
128 KiB `sceNetInit` pool. Actual post-bring-up numbers are logged as
`EVT mem_free=... net=up`.

**Debug fault shim (harness only)**: `adhoc_transport_set_fault(latency_ms,
jitter_ms, loss_pct, seed)` mirrors the UDP backend's shim, applied on the
receive path (PdpSend is fire-and-forget), so each side contributes
`latency_ms` to the one-way path and two peers give ~2× RTT. Driven by
.gpsp-harness.ini (`net_latency_ms` / `net_jitter_ms` / `net_loss_pct`),
counted in `EVT adhoc_stats faultdel=/faultdrop=`, zero-cost when unset.
This is what lets the PPSSPP rig — whose AdhocServer loopback RTT is
microseconds — reproduce real ad-hoc radio at all;
`run_trade_test_psp.sh --radio=40|80|160` are the standing profiles.
Its absence is precisely how the issue-#2 storm reached hardware.

### netpacket bridge (frontend-common/netpacket_host.c)

`fe_host` stores the interface the core registers via env call 78
(`fe_host_netpacket_cb()`); `fe_np_start{transport, is_host, nick, clock}`
creates netdrv with `protocol = rcb->protocol_version` and maps:

| netdrv event | core call |
|---|---|
| `session_started(id)` | `start(id, send_fn, poll_receive_fn)` + `EVT session_start id=N peers=M` |
| `deliver(buf,len,src)` | `receive(buf,len,src)` |
| `peer_connected(id)` | host side only: `connected(id)`; false → peer refused (admission). `EVT peer_connected id=N` on all sides |
| `peer_disconnected(id)` | host side only: `disconnected(id)`; EVT on all sides |
| `session_stopped(reason)` | `stop()` + `EVT session_stop reason=N` |
| core `send_fn(flags,buf,len,id)` | `netdrv_send` (flags pass through — ND_* mirror RETRO_NETPACKET_* numerically) |
| core `poll_receive()` | `netdrv_pump(now)` |

`connected`/`disconnected` are host-side-only per libretro.h:3155-3165 —
this refines plan §4.3 step 3 ("fire connected for each roster member"),
matching RetroArch behavior and SERIAL-PROTO-NOTES §6 (on non-host
instances the calls would perturb cable-mode SIOCNT id math for nothing).

`EVT net_stats tx= rx= acked= retx= dup= drop_crc= drop_mal= drop_unk=
overflow= drop_dead= core_tx= core_rx= peers= srtt_us= rto_us= retx_pct=
txq_hi= spill=` every 5 s is the harness's liveness and health signal.
The last five fields exist so an ARQ storm self-diagnoses from a single
log line: the field failure would have read `retx_pct=280` with `rto_us`
pinned at the 30000 floor (docs/HANDOFF.md issue #2).
`overflow` counts reliable drops toward a
LIVE peer only (the contract violation every harness asserts to 0, now
always accompanied by `EVT net_error reason=txq_overflow` and teardown —
it can no longer happen silently);
`drop_dead` counts ring-full drops toward a peer silent > 2× keepalive —
a disconnection in progress, where drops are inevitable at any finite
queue depth (observed at Gate 4-E `--disconnect`: the game keeps sending
RELIABLE frames to a SIGKILLed peer until its own link timeout). The optional 1 Hz `--net-probe` reliable broadcast
(104 B, magic "PRB0", silently discarded by the remote core's RFU magic
check) lets the netsmoke harness assert cross-process ARQ delivery before
the autopilot can generate real Union-Room traffic (ADR-0008).

### Desktop wiring

`gpsp_sdl --host [PORT]` / `--join IP:PORT` (default port 19019 = 0x4A4B),
`--nick`, `--net-loss PCT`, `--net-jitter MS`, `--net-probe`. UDP MACs are
`ip(4B,BE):port(2B,BE)`, so roster entries decode directly to socket
addresses on every peer — the mesh needs no relay. Fast-forward is forced
off while a session is active (plan §4.4 interlock).
`tools/e2e/run_netsmoke.sh` is the standing two-instance smoke: 5 % loss +
30 ms jitter both sides, asserts session up on both, ≥ 60 s alive, probe
ACKed, clean exits.
