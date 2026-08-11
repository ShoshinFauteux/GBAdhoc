# SERIAL-PROTO-NOTES — gpsp netpacket / RFU wire-protocol audit

Phase 0 recon deliverable (plan §3.3.1). Source of truth: libretro/gpsp master @ 5b6e751
(branch `phase0-recon`), read end to end: `serial.c`, `serial.h`, `rfu.c`, `serial_proto.c`,
`libretro/libretro.c`, `libretro/libretro-common/include/libretro.h`, plus `gba_memory.c`
(mode autodetect), `main.c`/`savestate.c` (savestate coverage), `gpsp_config.h`.
All paths below are relative to the repo root. Every claim is cited as `path:line`.

---

## 1. How the core registers the netpacket callback

- Registration happens in `retro_init` via environment call 78:
  `environ_cb(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, (void*)&netpacket_iface);` — libretro/libretro.c:704.
  The comment there notes the interface is optional (a frontend may return false; solo play still works).
  `RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE` is `78` — libretro/libretro-common/include/libretro.h:1832.
- The registered struct (libretro/libretro.c:546-554):

  | field | value | impl |
  |---|---|---|
  | `start` | `netpacket_start` | libretro/libretro.c:494-499 — stores `send_fn`/`poll_receive_fn`, sets `netplay_num_clients = 0`, `netplay_client_id = client_id` |
  | `receive` | `netpacket_receive` | libretro/libretro.c:507-520 — dispatches by `serial_mode` to `rfu_net_receive` / `serialpoke_net_receive` / `serialaw_net_receive` |
  | `stop` | `netpacket_stop` | libretro/libretro.c:502-505 — NULLs both stored fn pointers |
  | `poll` | **`NULL`** | libretro/libretro.c:550 — the frontend does NOT need to implement the per-frame `poll` call for this core |
  | `connected` | `netpacket_connected` | libretro/libretro.c:523-540 — admission control, see §6 |
  | `disconnected` | `netpacket_disconnected` | libretro/libretro.c:542-544 — decrements `netplay_num_clients` |
  | `protocol_version` | `GPSP_NETPACKET_VERSION` = **`"gpSP v1.0"`** | libretro/libretro.c:553; defined at gpsp_config.h:7 |

- `protocol_version` semantics: when non-NULL the frontend compares it *instead of* `library_version`
  to decide compatibility (libretro.h:3172-3174, 3186). **Our frontend must exchange and compare the
  string `"gpSP v1.0"` between peers during session handshake** — this is what makes FireRed↔Emerald
  cross-ROM linking legal (no content comparison, only protocol string).
- Struct layout (order of fields) is at libretro.h:3178-3187; `stop`/`poll`/`connected`/`disconnected`
  are all "Optional - may be NULL" from the frontend's perspective, but this core provides all except `poll`.

## 2. Wire format between peers

All multi-byte header words are big-endian on the wire: `netorder32` is a byte-swap on
little-endian builds (common.h:118-122). Three independent protocols exist, selected by
`serial_mode`; only one is active per session (dispatch at libretro/libretro.c:508-519).

### 2.1 RFU protocol (`SERIAL_MODE_RFU`) — the one that matters for Pokémon FR/LG/E

Common layout: `u32 magic 'RFU1' (0x52465531)` + `u32 msg_type` + `u32 header_word` + optional payload
(rfu.c:168, builders at rfu.c:183-225). Receiver requires `len >= 12` and the magic, else the packet
is silently dropped (rfu.c:720, 865-867).

Message types (rfu.c:170-177):

| type | value | direction | on-wire size | builder / handler |
|---|---|---|---|---|
| `NET_RFU_BROADCAST` | 0x00 | hosting peer → ALL (client_id 0xffff) | **36 B** (3 hdr words + 6 payload words) | send rfu.c:193-205 (`pkt[9]`, `sizeof(pkt)`=36); recv rfu.c:728-738 |
| `NET_RFU_CONNECT_REQ` | 0x01 | joining peer → hosting peer (unicast) | **16 B** | send rfu.c:183-191 via rfu.c:407; recv rfu.c:740-771 |
| `NET_RFU_CONNECT_ACK` | 0x02 | hosting peer → joiner (unicast) | **16 B** | send rfu.c:761; recv rfu.c:773-785 |
| `NET_RFU_CONNECT_NACK` | 0x03 | hosting peer → joiner (unicast) | **16 B** | send rfu.c:766, 769; recv rfu.c:787-791 |
| `NET_RFU_DISCONNECT` | 0x04 | either direction (unicast) | **16 B** | send rfu.c:538, 546; recv rfu.c:794-807 |
| `NET_RFU_HOST_SEND` | 0x05 | RFU-host → each connected RFU-client (unicast fan-out) | **104 B** (3 hdr words + 92 data bytes, zero-padded) | send rfu.c:207-225 via rfu.c:467-472; recv rfu.c:809-830 |
| `NET_RFU_CLIENT_SEND` | 0x06 | RFU-client → its RFU-host (unicast) | **104 B** | send rfu.c:478-483; recv rfu.c:832-853 |
| `NET_RFU_CLIENT_ACK` | 0x07 | RFU-client → RFU-host, ACKs each HOST_SEND | **16 B** | send rfu.c:816-817; recv rfu.c:855-863 |

Sizes are **fixed per type**: `rfu_net_send_cmd` always sends 16 bytes (rfu.c:190),
`rfu_net_send_bcast` always 36 (rfu.c:204), `rfu_net_send_data` always 104 (rfu.c:224) regardless
of the useful payload (RFU-host data ≤ 90 bytes gated at rfu.c:467; RFU-client data ≤ 16 bytes
gated at rfu.c:478; remainder zero-padded at rfu.c:222).

**RFU min/typical/max: 16 / 16-104 / 104 bytes.**

Cadence during an active RFU session:
- Host announcement broadcast: every `BCST_ANNOUNCE_VB`=30 vblanks, i.e. ~2/s (rfu.c:30, rfu.c:696-701;
  first one immediate via `tx_ttl = 0xff` at rfu.c:365). Sent from `rfu_frame_update`, which
  `retro_run` calls once per frame (libretro/libretro.c:1429-1436).
- Data exchange is game-driven: each `RFU_CMD_SEND_DATA`/`SEND_DATAW`/`RTX_WAIT` command from the game
  produces one 104-B packet per connected client (host side, rfu.c:463-472) or one 104-B packet to the
  host (client side, rfu.c:474-483). Each HOST_SEND received triggers an immediate 16-B CLIENT_ACK
  (rfu.c:815-817). Pokémon polls roughly once per frame in-session, so budget ≈ 2-5 packets/frame/peer
  during active linking, plus the 0.5 Hz broadcast. Between commands, nothing flows.
- Timeouts the transport must beat: the RFU-host drops a client after 240 frames (~4 s) without
  a CLIENT_SEND or CLIENT_ACK from it (rfu.c:704-714); received broadcast entries expire after
  255 frames without renewal (TTL 0xff set at rfu.c:734, decremented per frame at rfu.c:687-693).
  Game-visible wait timeout defaults to 32 frames ≈ 533 ms (`RFU_DEF_TIMEOUT`, rfu.c:33, applied
  rfu.c:647), configurable by the game via `RFU_CMD_SYSCFG` (rfu.c:275-279).

### 2.2 Link-cable Pokémon protocol (`SERIAL_MODE_SERIAL_POKE`)

- Single message shape: `u32 magic 'MPK1' (0x4d504b31)` + `u32 flags` (bit31 = payload present,
  low 16 = sender state) + 16 bytes (8 halfwords) payload area — **always 24 bytes**
  (serial_proto.c:103, 125-136; receiver requires exactly `len == 24`, serial_proto.c:375).
- Always sent as `RETRO_NETPACKET_BROADCAST` (serial_proto.c:135) — from masters AND slaves.
- Cadence: one packet per completed 9-halfword serial frame; slaves self-clock at ~9.5 IRQs/frame
  (`SLAVE_IRQ_CYCLES_C` 28672, serial_proto.c:111), which nets out to roughly 1 packet/frame/peer
  in connected state, ~1/frame during handshake (serial_proto.c:110-113 comments).
- Peer timeout: 240 frames (`MAX_FRAME_TIMEOUT`, serial_proto.c:113, enforced per-frame at
  serial_proto.c:237-244 via `serialpoke_frame_update`, called from retro_run at libretro/libretro.c:1433-1435).

### 2.3 Link-cable Advance Wars protocol (`SERIAL_MODE_SERIAL_AW1/AW2`)

- `u32 magic 'MAW1' (0x4d415731)` + `u32 flags` (`cmd<<16 | state<<8 | wcnt`) + `wcnt` 16-bit words:
  size = **8 + 2*wcnt bytes** (serial_proto.c:400, 418-427). Always broadcast (serial_proto.c:426).
- `wcnt` is encoded in 8 bits (receiver parses `flags & 0xff` and validates `len == cnt*2 + 8`,
  serial_proto.c:686, 694, 705), so the largest *valid* AW packet is **8 + 255*2 = 518 bytes**; the
  sender's stack buffer allows up to 8 + 256*2 = 520 (`u32 pkt[2 + 128]`, serial_proto.c:420). The
  packet-transfer path can call `serialaw_senddata` with `count` up to ~258 words (data[0] ≤ 0xFF plus
  tail, serial_proto.c:547-552), overflowing the 8-bit wcnt field — receiver will drop those. See Open
  questions.

### 2.4 Sizing rule for our transport

**True maximum payload the core ever hands `send_fn`: ≈ 520 bytes (AW mode); RFU-only sessions max
at 104 bytes; Pokémon-cable at 24.** The libretro contract itself allows up to 64 KB per packet
(libretro.h:3091-3092) but this core never approaches that. A single transport frame of 576+ bytes
of payload capacity covers everything with no fragmentation (plan §4.3's "≤128B" guess is correct
for RFU, but AW mode needs ~520 — size for that, or v1 may declare AW unsupported by policy).

### 2.5 RELIABLE vs UNRELIABLE

**Every single send from this core uses `RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT`.**
All three protocols funnel through one wrapper, `netpacket_send()`:
libretro/libretro.c:488-492 (`netpacket_send_fn_ptr(RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT, buf, len, client_id)`,
comment: "Force all packets to be flushed ASAP, to minimize latency").
`UNRELIABLE` (0) and `UNSEQUENCED` (1<<1) are never used (flag definitions: libretro.h:3082-3086).
Consequence for `netdrv`: the ARQ path handles 100 % of traffic, including the periodic RFU broadcast;
`FLUSH_HINT` on everything means: never batch/Nagle, transmit at call time.

## 3. Core options controlling serial/RFU mode (+ BIOS and frameskip keys)

Option definitions: libretro/libretro_core_options.h; parsing: `check_variables()` libretro/libretro.c:918-1118.

- **`gpsp_serial`** ("Link Cable Connectivity"), values (libretro_core_options.h:116-129):
  `"auto"` (default) / `"disabled"` / `"rfu"` / `"mul_poke"` / `"mul_aw1"` / `"mul_aw2"`.
  Parsed at libretro/libretro.c:982-1000 into `serial_setting` → `SERIAL_MODE_DISABLED/RFU/SERIAL_POKE/SERIAL_AW1/SERIAL_AW2/GBP/AUTO`
  (constants serial.h:20-26). Note: the parser additionally accepts the value **`"gbp"`**
  (libretro/libretro.c:996-997) which is *not* offered in the options UI list. Only read at
  content-load time (`started_from_load` guard, libretro/libretro.c:947) and applied through
  `load_gamepak(..., serial_setting)` (libretro/libretro.c:1239 → gba_memory.c:2925).
- **Auto resolution**: with `"auto"`, `serial_mode` is resolved per-game from the built-in override DB
  (`FLAGS_SERIAL` in `gbaover[]`, gba_memory.c:1673-1677) and a Pokémon-family heuristic:
  Ruby/Sapphire (`AXV`/`AXP`) → `SERIAL_MODE_SERIAL_POKE`, all other Pokémon-family ROMs
  (incl. FR/LG/Emerald) → **`SERIAL_MODE_RFU`** (gba_memory.c:2977-2987); Pokémon-engine ROM hacks
  → `SERIAL_MODE_SERIAL_POKE` (gba_memory.c:2961-2971). Our frontend can simply leave `gpsp_serial=auto`
  for the flagship titles, or pin `"rfu"` from the wireless UI.
- **BIOS**: **`gpsp_bios`**, values `"auto"` (default) / `"builtin"` / `"official"`
  (libretro_core_options.h:56-67; parsed libretro/libretro.c:948-958). Related:
  **`gpsp_boot_mode`** = `"game"`/`"bios"` (libretro_core_options.h:69-78).
- **Frameskip** (for the FF workstream):
  **`gpsp_frameskip`** = `"disabled"` (default) / `"auto"` / `"auto_threshold"` / `"fixed_interval"`
  (libretro_core_options.h:154-165; parsed libretro/libretro.c:1034-1047);
  **`gpsp_frameskip_threshold`** = `"15"`…`"60"` step 3, default `"33"` (libretro_core_options.h:167-190);
  **`gpsp_frameskip_interval`** = `"0"`…`"10"`, default `"1"` (libretro_core_options.h:192-210).
  Frameskip and threshold/interval are re-read every frame on `GET_VARIABLE_UPDATE`
  (libretro/libretro.c:1438-1439), unlike `gpsp_serial`/`gpsp_bios` which are load-time only.

## 4. RetroArch-specific assumptions beyond the netpacket API

- **No frontend-identity reads.** The core never queries frontend name/version; nothing branches on
  "is RetroArch". Netpacket-adjacent environment usage is limited to the SET call itself
  (libretro/libretro.c:704).
- **Savestate/time-manipulation interlock is the frontend's job.** libretro.h:1844-1847 specifies that
  while 2+ players are connected, the frontend disables pausing, slow motion, fast forward, rewind and
  savestate loading. The core does nothing to protect itself (see §7) — **our frontend must implement
  this block itself** (plan §4.5 already mandates it).
- **No savestate sync / lockstep is expected** — cores run free; the only shared state is the packets.
  The core's own reproducibility care (seeding RNG from `cpu_ticks`, not `time()`: rfu.c:244-251,
  254-257) exists for replay determinism, not for netplay sync.
- Timing assumption: `connected()`/`disconnected()` are host-side-only calls per the API
  (libretro.h:3155-3165), and the core's client-count tracking relies on that (see §6): on non-host
  instances `netplay_num_clients` stays 0 and is only used by cable-mode timing math
  (serial.c:175) and SIOCNT ID bits (serial.c:67-74) — for RFU it is irrelevant.
- Other env calls in `retro_init` (`GET_INPUT_BITMASKS` libretro/libretro.c:696,
  `SET_FASTFORWARDING_OVERRIDE` probe libretro/libretro.c:700) are optional and unrelated to netplay.

## 5. Client↔client traffic — YES, it exists; the topology is NOT "libretro-host↔clients"

Critical distinction: the **RFU role** (who executed `RFU_CMD_HOST_START`) is independent of the
**libretro client_id** (who is netpacket host = id 0). Any peer, including libretro client 3, can be
the RFU session host. Evidence:

- The broadcast table `rfu_peer_bcst[]` is indexed directly by sender's libretro `client_id`
  (rfu.c:164, 731-737). When the game asks to connect to a device-id, the core looks up which
  *libretro client_id* announced it and unicasts `NET_RFU_CONNECT_REQ` to **that id** —
  `rfu_net_send_cmd(i, NET_RFU_CONNECT_REQ, reqid)` where `i` is any peer's client_id (rfu.c:402-411).
- A connected RFU-client stores `rfu_client.host_id = client_id` of whoever ACKed (rfu.c:781) and
  thereafter unicasts `NET_RFU_CLIENT_SEND` / `CLIENT_ACK` / `DISCONNECT` to that id
  (rfu.c:480, 538, 816). Nothing constrains `host_id` to 0.
- `RETRO_NETPACKET_BROADCAST` (0xffff) is used **by every instance that hosts an RFU session**
  regardless of its libretro id (rfu.c:203-204), and in cable modes by *all* peers every frame
  (serial_proto.c:135, 426). The API explicitly supports broadcast "from the host as well as clients"
  (libretro.h:3094-3095).

**Consequence for the adhoc driver:** we must deliver unicast between *arbitrary peer pairs* and
broadcast *from any peer* — not merely hub-and-spoke around netpacket-id 0. On PSP ad-hoc every peer
hears every peer, so direct delivery satisfies this without an application-level relay; a relay is only
needed if we chose a hub topology (don't). This matches RetroArch's own behavior (its TCP star relays
client↔client through the host transparently — the core cannot tell).

## 6. Callback timing contract, and when the core sends

- **`start(client_id, send_fn, poll_receive_fn)`**: call once when the session begins; id 0 = session
  host with no peers yet, id > 0 = client already fully connected to the host (libretro.h:3123-3135).
  Core stores both pointers and resets counters (libretro/libretro.c:494-499). They must stay valid
  until `stop` (libretro.h:3130-3134).
- **`stop()`**: session over; core forgets the pointers (libretro/libretro.c:502-505). Note the core's
  emulated RFU link does NOT tear down on `stop` — the game discovers the peers' absence via its own
  timeouts (rfu.c:704-714, 887-928).
- **`poll`** (frontend→core, per-frame between `retro_run`s, libretro.h:3149-3153): **NULL for this
  core** (libretro/libretro.c:550) — the frontend need not call anything per frame beyond delivering
  received packets.
- **`receive(buf, len, client_id)`**: deliver inbound packets whenever the frontend processes the
  network — between `retro_run` calls, and synchronously from inside `poll_receive` when the core
  calls it (libretro.h:3111-3121).
- **`connected(client_id)` / `disconnected(client_id)`**: host side only (libretro.h:3155-3165).
  `connected` is the admission gate: it consults `serial_mode` and rejects when full — caps at
  `MAX_RFU_NETPLAYERS`=32 total peers for RFU, `MAX_SERMULT_NETPLAYERS`=4 for cable modes
  (libretro/libretro.c:523-540; constants gpsp_config.h:32, 35). Beware: for
  `SERIAL_MODE_DISABLED`/`GBP`/`AUTO` the `maxpl[...] - 1U` computation underflows to 0xFFFFFFFF and
  accepts everyone — see Open questions.
- **Where the core sends from** (all via the RELIABLE wrapper, §2.5):
  1. **Inside `retro_run` mid-frame**, from emulated-CPU IO writes: SIOCNT writes → `rfu_transfer` →
     `rfu_process_command` → sends (serial.c:119-131 → rfu.c:407/467-483/538-548), and cable-mode
     master sends (serial.c:170-181 → serial_proto.c:139-235/486-568).
  2. **Inside `retro_run` end-of-frame**, from `rfu_frame_update` (broadcast, rfu.c:696-701), called at
     libretro/libretro.c:1429-1436.
  3. **From inside the `receive` callback** — CONNECT_ACK/NACK (rfu.c:761, 766, 769) and CLIENT_ACK
     (rfu.c:816-817) are sent while handling an inbound packet. Since `receive` may itself run inside
     `poll_receive`, **`send_fn` must be safely callable re-entrantly from within the frontend's own
     receive-dispatch loop.** This is legal per the API: send may be called "during retro_run or any of
     the netpacket callbacks" (libretro.h:3106-3108) — and it is the only context it may be called from,
     which the core honors (never from other threads; the core is single-threaded).
- **`poll_receive` is called by the core mid-frame**: whenever the RFU is in a WAITEVENT state
  (game issued WAIT/RTX_WAIT/SEND_DATAW), `rfu_update` — which runs from the emulation loop via
  `update_serial` (serial.c:200-216) — invokes `netpacket_poll_receive()` to fetch packets early and
  cut latency (rfu.c:871-884; wrapper libretro/libretro.c:483-486). The in-tree comment warns this
  dispatch is synchronous and re-enters `rfu_net_receive` on the same thread (rfu.c:877-883). Our
  driver's `poll_receive` implementation must drain the RX ring and call `receive` inline, on the
  calling (main/emu) thread — exactly the plan-§4.3 threading model.

## 7. Savestate coverage of RFU/serial state

- The savestate is composed of exactly five sections: `cpu`, `input`, `main`, `memory`, `sound`
  (savestate.c:136-140 read, 178-182 write). `savestate.c` itself contains zero references to
  rfu/serial structures (verified by search — no matches for `serial|rfu|gbp` in savestate.c).
- What IS persisted, inside the `main` section: `serial-irq-cycles` (the pending serial-IRQ scheduler
  counter, main.c:432, restored main.c:379-385 via accessors serial.c:31-32) and `gbp-state`
  (main.c:434, 397-401). The rationale comment is at serial.c:26-30 and explicitly states
  "serial_mode is a libretro core-option, restored independently".
- What is NOT persisted: the entire RFU device model — `rfu_state`, `rfu_comstate`, `rfu_host`,
  `rfu_client`, `rfu_peer_bcst`, buffers and timeouts (statics at rfu.c:118-164) — and the entire
  cable-protocol state `serstate` (serial_proto.c:62-91). A state loaded mid-session resumes with the
  RFU model in whatever state the *current process* has, desynced from the game's own librfu state.
- **Conclusion for plan §4.5:** the core does NOT serialize RFU session state; savestate load AND save
  must be blocked while linked (RetroArch does this per libretro.h:1844-1847), and solo-mode loads of
  states captured while "linked" are also unsafe. This confirms the `[VERIFY]` with answer "no coverage".

## 8. Everything else a frontend author must know

- **MTU / size**: no packet exceeds ~520 B (§2.4); RFU tops out at 104 B. The API ceiling is 64 KB
  (libretro.h:3092). No fragmentation needed over PSP PDP datagrams.
- **In-order delivery**: the core only ever requests RELIABLE (= guaranteed AND ordered,
  libretro.h:3084), so ordering questions for UNRELIABLE never arise. Note the RFU queues are shallow —
  4 pending packets per direction (rfu.c:138-142, 149-153) — and the model *deliberately emulates RF
  loss* when queues are empty/full (packets dropped with a debug log, rfu.c:827, 851; "no data"
  responses synthesized on timeout, rfu.c:910-928). Late/slow delivery degrades to in-game retries, not
  desync. Duplicated delivery is NOT tolerated-by-design; our ARQ must dedupe.
- **Keepalives**: none at netpacket level from the core. Liveness is inferred inside the RFU protocol
  from CLIENT_SEND/CLIENT_ACK arrival (clttl reset, rfu.c:842, 862; 4 s expiry rfu.c:704-714). The
  transport-level keepalive/disconnect detection in plan §4.3 step 4 is on us, and `disconnected()`
  only matters host-side (§6).
- **Garbage tolerance — mostly good, two real holes.** Magic+length checks drop trash (rfu.c:719-720,
  865-867; serial_proto.c:375, 682, 694, 705) and `NET_RFU_HOST_SEND` validates `len >= blen + 12`
  (rfu.c:812-813). BUT: (a) `NET_RFU_BROADCAST` reads 24 payload bytes after checking only `len >= 12`
  (rfu.c:720, 735-736) — an OOB read on a truncated broadcast; (b) `NET_RFU_CLIENT_SEND` takes
  `blen = hdata >> 24` (up to 255) and `memcpy`s it into a 16-byte slot with no bound check
  (rfu.c:838-846 vs rfu.c:139-140). Honest senders never exceed 16 (rfu.c:478), but **our transport
  must never deliver truncated or corrupted frames to `receive`** — CRC + exact-length framing in the
  adhoc driver is load-bearing, not cosmetic. Do not "deliver what arrived and let the core sort it out".
- **client_id ranges the core can handle**: RFU indexes `rfu_peer_bcst[client_id]` guarded by
  `client_id < 32` (rfu.c:731); but the cable receivers index `peer[client_id]` into 4-element arrays
  with NO bounds check (serial_proto.c:376-392, 686-689). **Assign contiguous small ids: 0-3 for cable
  modes (0-4 players), and small ids for RFU.** Our roster (host=0, clients 1-4) satisfies both.
- **`send_fn` thread affinity**: only from `retro_run` or netpacket callbacks (libretro.h:3106-3108) —
  the core complies; our driver may therefore assume all core→driver calls happen on the emu thread.
- **`stop()` mid-wait**: `poll_receive` loops must handle `stop` being called during polling
  (libretro.h:3112-3115). Since our `poll_receive` is non-blocking ring-drain, trivially satisfied,
  but do not free driver state from inside a dispatch that the core initiated.
- **Session start state**: `netpacket_start` zeroes `netplay_num_clients` but does NOT reset the RFU
  or cable FSMs; those reset on GPIO pulse / mode change from the game side (rfu.c:229-251,
  serial.c:86-89, 93-94, 164-165). Starting a netpacket session mid-game is fine — the games
  re-initialize the adapter when entering the Union Room.
- **Cross-ROM sessions**: nothing in the protocol carries ROM identity; compatibility discovery is done
  by the games themselves via broadcast game-name data (`RFU_CMD_BCST_DATA` payload, rfu.c:346-349,
  relayed verbatim in the 24-byte broadcast payload). FR↔Emerald therefore works iff both cores run
  RFU mode and the frontend matched `protocol_version`.

## Open questions

1. **`netpacket_connected` underflow for non-multiplayer modes**: `maxpl[serial_mode] - 1U` with
   `maxpl = 0` (DISABLED/GBP/AUTO) yields `0xFFFFFFFF`, so the guard at libretro/libretro.c:533-536
   never rejects and every client is *accepted* in modes that should take none. Looks like an upstream
   off-by-design bug; harmless for us if we only start sessions when `serial_mode` is RFU/cable, but
   the frontend should not rely on the core to refuse players in disabled modes. Candidate upstream
   report.
2. **AW-mode wcnt overflow**: `serialaw_senddata` can be called with `count` > 255
   (PACKETXG path allows data[0]=0xFF + tail + cmd ≈ 258 words, serial_proto.c:546-552, 649-655), which
   truncates in the 8-bit wcnt field (serial_proto.c:419) so the receiver's `len == cnt*2 + 8` check
   (serial_proto.c:705) drops the packet. Unclear if real AW traffic ever hits >255 words. Not a
   transport problem (we just carry ≤ ~520 B), but caps the AW claim in §2.4.
3. **`NET_RFU_CLIENT_SEND` case falls through into `CLIENT_ACK`** when the host's packet queue is full
   (no `break` between rfu.c:832-853 and rfu.c:855-863; the early `return`s at rfu.c:848 mask it in the
   normal path). Effect is benign (re-zeroes clttl) but it is an upstream code smell worth flagging.
4. **`serialaw_frame_update` is never called**: `retro_run`'s per-frame switch only services RFU and
   POKE (libretro/libretro.c:1429-1436), so AW peers never hit the 240-frame timeout path declared at
   serial.h:57. AW-mode ghost peers persist forever. Upstream question; irrelevant to the Pokémon goal.
5. **Exact per-frame packet rate for Pokémon Union Room** is game-driven (§2.1); the 2-5 pkts/frame
   figure is an inference from the command model, not a measurement. Measure on the RA-PC×2 reference
   rig during Gate 0 and record real numbers here.
6. **An ERROR answer from the emulated adapter is a GAME-FATAL event, and rfu.c has seven of them.**
   `rfu_process_command()`'s `return -1` becomes the SPI sequence `0x996601ee` + an error code
   (`RFU_COMSTATE_RESPERR`). gen-3's librfu decodes an `0xEE` ack byte as
   `ERR_REQ_CMD_ACK_REJECTION` (pret `src/librfu_intr.c:128-132`) -> `reqResult = 3` ->
   `LMAN_MSG_REQ_API_ERROR` (`src/AgbRfu_LinkManager.c:933-953`) -> `RFU_STATUS_FATAL_ERROR` ->
   `CB2_LinkError` with `disconnected = FALSE` -> `gWirelessCommType = 3`, the error screen with
   **no A-button handler** (`src/link.c:1589-1610`, `:1669-1725`). A *connection* loss instead
   yields `RFU_STATUS_CONNECTION_ERROR` and the recoverable dialog. The only forgiveness is
   TX/RX/MS_CHANGE mid-MSC on a child (`AgbRfu_LinkManager.c:835`) and a child's rejected
   `ID_DISCONNECT_REQ` (`librfu_rfu.c:1116-1126`). **So every state-dependent `return -1` in rfu.c
   is a generator of an unrecoverable error screen, not a recoverable one.** `EVT rfu_cmderr`
   (ADR-0041) reports them. The seven are: `HOST_START` while CLIENT; `HOST_STOP` while IDLE;
   `HOST_ACCEPT` while IDLE; `CONNECT`/`ISCONNECTED`/`CONCOMPL` while HOST; and
   `SEND_DATA`/`SEND_DATAW`/`RTX_WAIT` while neither HOST nor CLIENT.
7. **Adapter commands rfu.c does not implement.** `ID_RESET_REQ` (0x10) and `ID_STOP_MODE_REQ`
   (0x3d) are aliased to `RFU_CMD_INIT1`/`INIT2` and answered with a bare ACK that changes no
   state; the link-recovery trio `ID_CPR_START/POLL/END_REQ` (0x32/0x33/0x34) and 0x35/0x36 fall
   to `default:` and are also bare-ACKed. For STOP_MODE this happens to be masked — librfu's
   `rfu_REQ_stopMode` calls `AgbRFU_SoftReset` first, whose RCNT pulse reaches `rfu_reset()` and
   *does* clear the state (confirmed in the rig: 0x3d is always observed arriving at
   `state=idle`). `ID_RESET_REQ` has no such cover. `EVT rfu_unkcmd` reports the `default:` cases.
   Note also that `rfu_reset()` clears `rfu_host` and `rfu_peer_bcst` but **not `rfu_client`** —
   stale `devid`/`clnum`/queued packets survive a reset until the next `NET_RFU_CONNECT_ACK`
   memsets them.
