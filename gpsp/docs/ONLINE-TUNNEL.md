# Online play: carrying the ad hoc link over the internet

Study and design, 2026-11-05. Branch `claude/online-latency` (base `claude/ram-mirror-pcfix`
356d6d4, the 4.6.0-beta.3 source). Evidence: `builds/online-out/`. **Nothing here has gone over the
real internet.** Latency was injected on the two-console Lab rig (PSP-3000 host, PSP-1000 join).

## 0. Answer first

<!-- RESULTS -->

## 1. What the link protocol is today

Online play needs no new game-facing protocol. GBAdhoc already sends everything as **netdrv frames**
(`netdrv/netdrv_wire.h`: magic `GPN1`, 24-byte header, CRC16, payload up to 144 B on the PSP). These
travel over a **transport**, a six-hook vtable (`nd_transport`, `netdrv.h`). Three transports exist:

| transport | carries | used by |
|---|---|---|
| `transport_adhoc.c` | sceNetAdhoc PDP, IBSS, MAC-addressed | every PSP-to-PSP session (shipped) |
| `transport_udp.c` | desktop UDP, peers seeded on the command line | SDL twin, host tests |
| `transport_inet.c` | sceNetInet UDP over **infrastructure Wi-Fi** (apctl) | Mystery Gift Station (shipped) |

The third one matters most here. The PSP already associates with an access point, gets an IP over DHCP and
runs netdrv over a UDP socket (`psp/mgift_net.c` brings apctl up). An online session needs a fourth
transport beside these: the same UDP socket, but aimed at a relay server instead of a broadcast address.
netdrv, `rfu.c` and the link sessions do not change.

Three kinds of session sit on netdrv. They react to delay differently:

| session | how the consoles stay together | delay knob today |
|---|---|---|
| **GBA Wireless Adapter** (`rfu.c`, Union Room, Mario Golf/Tennis) | scheduled ticks (`rfu_sched.h`): partner data for frame F arrives at the start of F + D, and frame F + D waits for the partner's tick F | `rfu_sched` = D (default 2) |
| **GBA link cable** (mirror, `fe_gbadual.c` + `fe_gblink.c`: AW2, Emerald Trade Center/Colosseum, Super Circuit) | input lockstep: each console runs BOTH GBAs; only buttons cross; frame f uses buttons sent at f - D | `gblink_delay` = D (default 4) |
| **GB/GBC link** (`fe_gblink.c` over `gbcore_dual`) | the same input lockstep | `gblink_delay` = D (default 4) |

Reliability is netdrv's ARQ: sliding window 256, adaptive RTO (RFC 6298) with a 200 ms floor and a
2.5 s ceiling on the PSP, keepalive PING every 250 ms, peer dead after 8 s. The GB session adds an
unreliable **bulk lane** for cartridge and save transfers.

## 2. The latency experiment

### Method

The delay shim already existed: harness keys `net_latency_ms`, `net_jitter_ms` and `net_loss_pct`
(`adhoc_transport_set_fault`, `transport_adhoc.c`). It holds each **received** datagram in the
transport's receive ring for N ms, so N on both consoles adds **2N ms of round trip**. The queue stays
FIFO, so jitter does not reorder packets. In a release build the keys are dead, because `HARNESS_INI`
names a file that cannot exist, and at 0 the shim costs one integer test per datagram. No new key was
needed. The Lab already validates these keys, and the gblink fixtures already set them.

Two caveats apply to every number below:

- The GB **bulk lane** bypasses the delay (it is a separate ring). GB cartridge and save transfers are
  measured without latency, and only the running trade is delayed.
- The rig's own radio adds a real round trip of 20-40 ms (`latency_ms` p50 in the Lab verdicts). "Added
  RTT" is on top of that.

The sweep added 0, 20, 40, 60, 100 and 150 ms of RTT (`net_latency_ms` 0/10/20/30/50/75 on both sides).
It ran on the beta.3 Lab build `465d0ec94162` with these cases:

- cable `-speed` cases (no per-frame hashing, but the periodic lockstep HASH is still checked): `link_em`
  (Trade Center trade), `link_emb` (Colosseum battle), `link_aw2` (10-turn battle), `link_msc` (Super
  Circuit race)
- adapter: `rfu_em_trade`, `rfu_em_battle`
- GB/GBC: `gblink_gen1_trade`, `gblink_gen2_trade` (hashed)

Queued by `builds/online-out/queue_sweep.py`. One attempt per case (`lab_attempts = 1`), so a failure is
not hidden by a retry.

<!-- TABLE -->

### Theory, which the table checks

For the input lockstep (cable and GB), frame f on one console needs the partner's buttons for f. The
partner sent them D frames earlier, so a frame is on time while

    one-way latency + one pump period + one batching frame  <  D x 16.74 ms

When that fails, the pair settles to a speed that the round trip allows: about **2D frames per RTT**. With
D = 4 that is full speed up to an effective RTT of about 130 ms, and roughly 2 x 4 / RTT beyond it.
A larger D buys back the speed, at the cost of D frames of input lag. That lag is invisible in a
turn-based game and noticeable in a race.

The adapter's ticks work the same way, with D = 2: on time while the one-way latency plus a pump is
under about 2 frames (33 ms), so it is expected to slow first. Unlike the cable, a real game protocol sits
on top. The Union Room counts its own timeouts in frames, and the connection phase is kept unscheduled
and as fast as today (RFU-COUPLING.md), which makes connecting delay-sensitive.

### What breaks first

<!-- BREAKS -->

### Mitigation prototype: auto input delay (`gblink_delay_auto`)

The lockstep sessions already have the right knob, `gblink_delay`. Nobody can be expected to tune it
per opponent. So the host now measures and chooses:

- `fe_gblink_local.delay_auto` = a cap in frames, 0 = off (the default, the behaviour of every build so
  far). Harness key `gblink_delay_auto`.
- When the host has the guest's HELLO, it holds its CONFIG until netdrv has 2 RTT samples (the HELLOs'
  acknowledgements supply them), or for 90 steps at most. It then sets

      D = ceil((srtt / 2 + 2 x rttvar + T) / T) + 1,   T = 16.74 ms,

  clamped to [`gblink_delay`, cap]. On the PSP-to-PSP radio (srtt ~35 ms) this gives 4, today's default,
  so the radio path is unchanged.
- The guest takes D from CONFIG as it always has. The **wire is unchanged**, and a guest of any build
  works with it. GB/GBC and GBA cable sessions are covered alike (`fe_host.c` fills both platforms).
- Host test: `sesssim auto=N`; `tools/tests/gblink_session_tests.py` runs a 12-frame round trip. With
  delay 3 there were 1251 stalls on each side; with auto delay 8 there were 0 on each.

<!-- MITIGATION -->

The adapter (RFU) has its own knob, `rfu_sched` = D, which a harness can already set. An automatic choice
for it would follow the same rule. It is not built yet, because the adapter's handshake phase drops D to 1
(`RFU_SCHED_NI_FRAMES`), so the connect exchange, not the running link, is what sets its ceiling.

## 3. Architecture

    PSP (infrastructure Wi-Fi) --UDP--> relay server <--UDP-- PSP
             \___ HTTPS (optional): room code lookup / status page on the website

**The PSP's radio.** 802.11b, 2.4 GHz only; no 5 GHz at all. Security on stock firmware: open, WEP,
WPA-PSK (TKIP) and WPA-PSK (AES). Those are WPA1, and most current routers no longer offer them.
**WPA2** needs ARK-4 CFW with its built-in WPA2 plugin (the `wpa2psp` plugin, ARK-4 since February
2025): enable it in *Extras > Custom Firmware Settings* (newer ARK-4 releases move that menu under the
Game column), then reboot. It is 2.4 GHz only, and with a WPA2/WPA3 mixed-mode router the PSP may not get
an address over DHCP, so a manual IP is needed. There is **no evidence of WPA3 support** on any firmware
or plugin. An access point set to "g/n only", "WPA3 only" or 5 GHz is invisible to the PSP. A phone
hotspot works when set to 2.4 GHz and WPA2 (with ARK-4's plugin) or an older mode. The player picks a
saved connection with the system's own network dialog (`sceUtilityNetconf`), so GBAdhoc never handles a
Wi-Fi password. `mgift_net.c` today builds a scratch open profile for the Mystery Gift phone, and the
online path should use the user's saved profiles instead.

**Requirements the player sees** (Online screen, and the error copy when association fails):

| failure | what the PSP shows |
|---|---|
| no saved connection, or scan finds nothing | "No Wi-Fi network found. The PSP only sees 2.4 GHz networks, and your router or hotspot must offer 802.11b." |
| association/authentication fails | "Couldn't join this network. Stock PSP firmware supports WEP and WPA (TKIP/AES). For WPA2, install ARK-4 and turn on WPA2 in Custom Firmware Settings, then reboot. WPA3 isn't supported." |
| associated, no IP address (DHCP timeout) | "Connected, but the network gave no IP address. On a WPA2/WPA3 mixed router, set a manual IP in the PSP's network settings." |
| relay unreachable | "Can't reach the GBAdhoc online server. Check that this network has internet access." |

**NAT.** Both PSPs sit behind home NATs. The PSP has no UPnP and no STUN, and the player cannot be asked
to forward ports on a handheld. UDP hole punching through a rendezvous server works with many home NATs
(endpoint-independent mapping), but it fails on symmetric NATs and carrier-grade NAT, which phone hotspots
often use. **Relay first:** every packet goes PSP -> server -> PSP. That always works, the PSP only ever
talks to one well-known address, and the server cost is trivial (below). On top of it, a **direct path** (rendezvous plus
UDP hole punching, section 5) takes the relay out of the path when both NATs allow it, and falls back
to the relay automatically when they do not.

**Room codes.** The host asks the server for a room and gets a **4-letter** code (A-Z, 456,976 codes;
owner decision, section 5 "Matchmaking"), and shows it. The joiner enters it on a D-pad letter wheel. A room
holds 2 players (5 later: netdrv already supports up to 4 clients), expires 10 minutes after creation if
nobody joins, and 60 s after its last packet.

**What runs where**

| piece | runs on | does |
|---|---|---|
| `transport_relay.c` (new) | PSP | the `nd_transport` over one sceNetInet UDP socket to the relay; fake MACs = relay slot ids |
| online menu (new) | PSP | Host online / Join online (code) in the Wireless menu, beside the ad hoc rooms |
| relay (new) | VPS (`gbadhoc-relay`), or GBAdhoc XMB on the host player's PC (same `relay_core.c`, section 6) | UDP: room create/join, forward frames between the room's slots, keepalives, limits |
| status page (optional) | the existing website behind Cloudflare | server up/down, player counts, version notes |

## 4. Prior art

- **PRO Online / Adhoc Prometheus** (CFW plugin by Coldbird, used by PRO Online's servers). It hooks the
  firmware's sceNetAdhoc* calls on a real PSP and redirects them over infrastructure Wi-Fi. A
  matchmaking server (TCP 27312) tracks groups per game product code and hands out peer addresses. **Game
  data then goes peer to peer over UDP**, which is why those communities document port forwarding, DMZ
  and VPNs.
- **PPSSPP** ships the same matchmaking server (`Core/HLE/proAdhocServer.cpp`, `SERVER_PORT 27312`) and
  client. Its data path is also peer to peer, with a port offset (default 10000) to dodge low ports.
  Public servers exist (socom.cc). A PPSSPP issue (#18903) reports a game still working at 300 ms of
  simulated latency and 20 % loss. Third-party services advertise packet relays on top; I could not
  verify them.

**What is reusable.** The shape: a small matchmaking server with rooms keyed by game and a group name,
and a keepalive model. **What is not:** the peer-to-peer data path (the NAT problem above), and the need to
hook the firmware. GBAdhoc owns the transport seam, so a relay transport replaces the whole hooking layer.
Speaking PRO's TCP protocol gains nothing, because PRO clients could not join a GBAdhoc session anyway: the
admission check (`ND_STOP_VERSION`) needs the same GBAdhoc build.

## 5. Protocol

### Framing

Each UDP datagram carries a relay header followed, for DATA, by an **unchanged netdrv frame**:

    0  4  magic 'GBR1'
    4  1  version (1)
    5  1  type: CREATE, CREATED, JOIN, JOINED, PEER, DATA, KEEP, BYE, ERROR
    6  1  from slot (0 = host, 1..4; 0xFF before a slot is assigned)
    7  1  to slot   (0xFF = every other slot: netdrv's broadcast)
    8  8  session token (server-issued in CREATED/JOINED; 0 before)
    16 .. body (DATA: one netdrv frame, <= 168 B; CREATE: build id + game id; JOIN: room code + build id)

The relay never parses netdrv frames. It checks magic, version, token and slot, and forwards. Integrity
stays netdrv's own CRC16. netdrv's MAC addressing maps onto slots: the transport reports a peer's "MAC" as
`02:47:42:52:00:<slot>`, so the roster, WELCOME and admission logic run unchanged. The 16-byte header
plus a 168-byte frame is far below any internet MTU.

### Keepalives, NAT bindings, liveness

- In a session, netdrv already sends at least a PING every 250 ms, plus DATA every frame or two. That keeps
  every NAT binding open with no extra traffic.
- In the lobby (room created, nobody joined), the transport sends KEEP every 15 s. Home NATs commonly drop
  idle UDP bindings after 30-120 s.
- The server drops a slot after 20 s of silence and tells the others (BYE). netdrv's own 8 s peer-death
  rule fires first, so the session sees an ordinary disconnect.

### Reliability over UDP

It is already there: netdrv's ARQ is what the shipped ad hoc link uses, over a radio that loses about 3 %.
What changes on the internet:

- **RTO.** The adaptive estimator (srtt + 4 rttvar, floor 200 ms) follows a 100-200 ms path on its own.
  The 200 ms floor stays.
- **Window.** 256 in flight at ~120 payloads/s covers RTTs of over 2 s. No change.
- **Loss.** Home internet loses 0-1 %, a busy 2.4 GHz room 1-3 %. The GB session's redundant input copies
  (`gblink_input_copies`, unreliable, beside the ordered copy) remove most loss stalls. They should be on
  online.
- **Reordering.** The relay forwards in arrival order. The internet rarely reorders a single flow, and
  netdrv's receive window absorbs it.

### Direct path: rendezvous, hole punching, relay fallback

The relay always works, but every packet takes two internet legs. When both NATs allow it, the PSPs can
talk directly: the **same server** acts as a rendezvous and tells each PSP the other's public UDP
endpoint, both punch, and game traffic goes peer to peer. The relay stays the automatic fallback.

**Can the PSP do it?** Yes. Shipped code already makes every call it needs. `transport_inet.c` (Mystery
Gift) opens a `SOCK_DGRAM` socket with `sceNetInetSocket`, sets `SO_REUSEADDR` and `SO_NONBLOCK`,
`sceNetInetBind`s a fixed local port, `sceNetInetSendto`s to addresses it learned at run time, and
`sceNetInetRecvfrom`s from **any** source (an unknown sender is added to its address book). Hole punching
needs only one more property: the PSP sends to the rendezvous and to the peer from the **same socket**, so
the NAT reuses the mapping the rendezvous saw. That is a rule for `transport_relay.c` (one socket for
everything), not a firmware capability. Not yet proven: whether the PSP's stack delivers a datagram
from a peer it has not sent to yet. It does for the phone in Mystery Gift, where the phone answers
first. In a punch, both send first, so this does not depend on it either way.

**Rendezvous protocol** (new GBR1 types, same header as section 5):

    PROBE      PSP -> server, port P and port P+1 (two server sockets)
    PROBED     server -> PSP: the public ip:port it saw on that socket
    CANDIDATES server -> both PSPs of a room, once both are in:
               peer's public ip:port (from PROBE), peer's private ip:port (it reports it in JOIN/CREATE),
               a 64-bit punch nonce, and "punch at T + 150 ms" (T = server time of this packet,
               sent to both at once)
    PUNCH      PSP <-> PSP: nonce, my slot, a counter
    PUNCH_ACK  PSP <-> PSP: the nonce echoed, the counter echoed (an RTT sample)
    PATH       PSP -> server: DIRECT (with measured RTT) or RELAY, for the room's status / Online tab

**NAT classification first.** Each PSP PROBEs two server ports from its one socket. If the server sees
the **same public port** for both, the mapping is endpoint-independent: punchable. If the ports
differ, the NAT is symmetric (endpoint-dependent): no punch, relay at once. A public address in
100.64.0.0/10 (CGNAT, reported by the PSP from its own apctl IP compared with what the server sees) is
usually symmetric or double-NAT. Try once with a short budget, and go to the relay on failure.

**Punch timing.** On CANDIDATES both PSPs wait until the announced start, then send PUNCH to the peer's
public endpoint **and** its private endpoint (two players behind one router, a common case at home)
every 20 ms for 2 s: 100 tries, ~10 KB total. The first PUNCH that arrives opens the other side's
mapping. A PSP answers every PUNCH with PUNCH_ACK, and the path is up when a PSP has **received a
PUNCH_ACK** (proof the round trip works both ways). Both sides then switch DATA to the direct endpoint.
The relay slot stays registered and KEEP'd as the fallback.

**Keep-alives.** Home NATs drop idle UDP mappings after 20-120 s, some sooner. During a session netdrv
traffic (DATA every frame or two, PING every 250 ms) keeps the mapping open. Outside a session, and in
the lobby, the transport sends a PUNCH (doubling as keep-alive) every **10 s** on the direct path, and a
KEEP every 15 s to the relay so the fallback slot stays alive.

**Fallback rule** (any one of these, decided per PSP, and both follow the first to decide):

- the NAT test says symmetric on BOTH sides (and neither names a router forward), so no punch is
  attempted;
- no PUNCH_ACK within 2 s of the start (of pumped time: a stall of the caller does not count);
- on an established direct path, nothing from the peer for **1.5 s** while the relay still answers
  KEEP (a NAT rebinding or a dropped mapping).

The PSP that decides sends PATH RELAY to the server, which tells the other (`PEER path=relay`), and both
send DATA via the relay from the next packet. netdrv's ARQ covers the packets lost in the switch: it is
reliable and ordered, and a few hundred ms of loss is under its 8 s peer-death rule. The relay is the
last resort, not the end: a PSP on the relay punches again after 3 s, then 6, 12, 24 and every 30 s,
and a punch that arrives while it is on the relay starts its own burst at once (see below).

### Direct wherever possible (2026-11-05, after the first real tests)

The owner's rule: every millisecond counts, so the relay carries a session only when no direct path
exists. Two real sessions went through the relay that should not have:

1. **Same Wi-Fi, PPSSPP (this PC) and the Go: relay (room EYPJ).** Not the firewall: Windows Defender
   Firewall was off on the active Private profile all evening (no profile change in its event log),
   and the same PC went direct with the 3000 (LANT). The relay log tells the story. The host reported
   `path=relay` exactly 2 s 150 ms after the join (its whole punch window, not one PUNCH_ACK), and the
   Go never reported a path of its own. The joiner had stopped pumping its socket right after JOINED:
   the menu path brings the room up first, then backs the save up (128 KiB onto the Go's internal
   storage), draws the LINKING screen and starts netdrv, and only netdrv's recv runs the punch timers.
   The host's 2 s window passed while the Go read nothing. The in-process model reproduces it exactly
   with a 3 s joiner stall (both sides `fellback`, no direct), and the harness path, which brings the
   room up after the backup, never showed it.
2. **The Go on cellular (symmetric carrier NAT) to PPSSPP at home: relay.** By design then: the server
   offered no punch if either side was symmetric.

What the transport does now (`transport_relay.c`, `relay_core.c`):

- **The punch window counts pumped time.** A gap of more than 100 ms between timer runs is a stall;
  the burst's end moves by the gap, and so does a direct path's silence clock (the packets wait in the
  socket, they were not lost).
- **Every PUNCH is answered, in any state**, and a PUNCH that arrives while we are on the relay starts
  our burst at once, so two bursts that missed each other line up. A late PUNCH_ACK still makes the
  path direct: it is the round trip.
- **Retries.** On the relay a PSP bursts again after 3, 6, 12, 24 and then every 30 s (20-byte PUNCHes,
  ~2 KB a burst). It stops only while the path is direct.
- **Candidates, private first:** the LAN address, the public endpoint the server saw, the **router
  forward** (the peer's public IP with its own socket port, or the port it names in `map_port`), and
  the **peer-reflexive** address: where the peer's punches actually came from. A symmetric NAT gives
  the peer a new port toward us that the server never sees; we answer it and punch it. If the LAN
  candidate answers after a public one, the path moves to the LAN.
- **The server offers a punch unless both sides are symmetric** (and neither names a forward). One
  reachable side is enough.

What goes direct (in-process model `test_relay_nat`, 19/19, and the real-socket `natlab.py`, 12/12):

| host | guest | path |
|---|---|---|
| same LAN (with or without a 3 s joiner stall) | | direct, 5-6 ms |
| cone (EIM) | cone, public, or cone with a 3 s stall | direct |
| address-restricted cone (any port of an IP it sent to) | symmetric / CGNAT | direct (peer-reflexive) |
| cone + UPnP forward of 27500 (or another port with `map_port`) | symmetric / CGNAT | direct |
| symmetric + UPnP forward, `map_port` set | symmetric | direct |
| port-restricted cone, no forward | symmetric | relay: the cone drops the CGNAT's new port, and the CGNAT drops ours |
| symmetric, no forward | symmetric | relay: neither side has an address the other can predict |

**Who can open a forward:**

- **A PPSSPP or PC host:** `python gpsp/tools/relay/upnp_map.py add` maps UDP 27500 on the router to
  this PC (`del` removes it). No GBAdhoc setting is needed: the relay already offers public-IP:27500.
- **The desktop host (GBAdhoc XMB):** it maps its own socket's port with UPnP and sends that external
  port as `map_port` in CREATE/JOIN (bytes 36-37 of the body; `relay_client --map-port N` tests it).
- **A PSP host:** not from the firmware. The PSP's `sceNetUpnp` library is used by a few games, but
  the publicly known NIDs are only init/term/start/stop and a NAT-info query; no add-port-mapping
  entry is documented, so homebrew cannot ask it for a forward (unverified on hardware). What would
  work is our own minimal IGD client in `gbadhoc_online.prx`: SSDP over `sceNetInet` multicast, one
  HTTP GET of the description and one SOAP `AddPortMapping` over a `sceNetInet` TCP socket, a few KB of
  code, loaded only online. Not built. Meanwhile a PSP host gets the same result if the owner forwards
  UDP 27500 to it once on the router, and most home routers are at least address-restricted, which
  needs nothing.

**On screen.** The session chip reads `Online ABCD  Direct 12 ms` or `Online ABCD  Via relay 48 ms`
(the direct round trip from the 500 ms PUNCH keep-alive, or twice our KEEP round trip to the server),
updated every second, and a change of path is a toast. The module does it (a shim on netdrv's recv
in `psp/online/main.c`) through two new host-table entries, the chip and the toast (`OL_VERSION` 3):
the EBOOT gains no code. Its layout has no text to spare (the 48 B pad is the only slack and
`.data.pin` has none), and the two pointers fit in `.data`'s alignment padding: the release ELF's
sections and symbols sit exactly where b9005900's do, boot heap 10,081,692 B on the PSP-1000.


**Expected win.** Direct removes the detour through the VPS. For two players in one region the relay's
+20-60 ms (section 6) becomes 0. For players far apart it matters less: the geography dominates.

**Testing it without the internet** (cheap, all on the PC; nothing leaves the machine):

1. **Simulated NATs in WSL network namespaces.** Two namespaces per "home" (a client and a NAT box)
   plus one for the server, joined by veth pairs. The NAT box runs `iptables -t nat -A POSTROUTING -j
   MASQUERADE`, which on Linux gives endpoint-independent mapping, so a punch must work. With
   `--random-fully` the port changes per destination, which behaves as a symmetric NAT and must fall back.
   Add a conntrack UDP timeout of 20 s to check the keep-alives. The clients are the SDL twin
   (`sdl/main_sdl.c` over `transport_relay`) or a small Python driver. Run every cell of {EIM, symmetric}
   x {EIM, symmetric} x {same LAN, different LAN}: punch where possible, relay otherwise, and the
   session completes in every cell.
2. **The relay core's host tests** with an in-process NAT model (mapping and filtering behaviour as
   parameters), so the fallback rule is tested deterministically: drop the mapping mid-session and the
   path must switch to the relay within 1.5 s, with no netdrv session loss.
3. **Two real PSPs on the LAN**, through `gbadhoc-relay` on this PC (bound to the LAN address only). There
   is no NAT between them, so the punch succeeds on the private candidate. That proves the PSP side: one
   socket, unsolicited PUNCH received, the switch from relay to direct and back (the fallback forced by a
   harness key that drops direct traffic). With the Lab's `net_latency_ms` still on, the Lab fixtures run
   over both paths.

None of these touch the real internet. A real-internet punch test (home Wi-Fi against a phone hotspot)
is step 6 of the plan and needs the owner's go-ahead.

### Matchmaking (owner decisions, 2026-11-05)

UI reference: the online-play mockup (claude.ai/artifact/4i55Tc1mgfisHUnZ9fPoRv), direction approved.

**Room codes: four letters.** A-Z, 26^4 = 456,976 rooms (`RL_CODE_LEN 4`, built). Shown big on the
host's screen. The code lives while the host's room screen is open, 10 minutes at most. The join screen
warns about O/Q and I/J. Guessing is bounded: 30 CREATE+JOIN a minute per IP. With 1,000 live rooms a
blind guess hits one about 0.2 % of the time, and a stranger who lands in a room still sits in the
host's lobby, where the host sees the name and does not have to press Ready. The host can also remove a
joiner (KICK).

**Quick match, per game.** A public room is a room the host marks public at CREATE (flag bit). The
server keys it by **game id**: the cartridge's 4-character game code plus the 64-sample SHA-1 that the
link session already checks. Different releases are therefore different games, the same rule a cable
applies. New messages:

    LIST    c->s  game_id[24] page[1]           (padded to the size of the reply page: no amplification)
    ROOMS   s->c  up to 6 x { code[4], host nickname[16], console[1], region[2], link mode[1],
                              host->server RTT ms[2] } + total open rooms for this game[2]

The ping column is an estimate (both sides' RTT to the server). The real figure appears in the lobby
once the path is up. The list shows **"N players online"** for the game, so an empty list says why it is
empty.

**Recent partners.** Not a friends list: the last few consoles you linked with, with "online now" and
an invite.

- **Console ID:** 64 random bits plus a 128-bit secret, made on the PSP the first time online play is
  turned on and kept in `config.ini`. It is never the PSP's MAC, PSID or nickname. The secret
  authenticates every recent-partner request, so nobody can read a list by guessing an ID.
- **What the server stores** per console ID: at most **8** partners, each as {partner console ID, the
  partner's nickname at the time, game id, last-linked date}, plus "online now" while a connection is
  open. **No IP addresses** are stored with the history, and no session contents.
- **How long:** a partner entry expires **30 days** after the last link with that partner. A console ID
  unseen for 90 days is deleted with its list.
- **How to clear it:** Settings > Online > *Forget recent partners* sends `FORGET(id, secret)`, which
  deletes the list on the server. *Reset online identity* also makes a new ID, so the old one is
  orphaned and expires. The server's page states the same retention.
- **Invites:** `INVITE(my id, secret, partner id, game id, room code)`. If the partner is online in GBAdhoc,
  they get a toast ("Mara invites you: Pokemon Emerald, room KQTR") and accept from Wireless > Online.
  Invites are rate-limited (10 per hour per ID) and only reach consoles in your recent list.

**The host picks the link mode in the lobby.** For a game that links both ways (Link Cable and Wireless
Adapter, e.g. Emerald), the host's lobby row cycles the mode. The choice travels as room metadata
(`MODE host->server->guests`), and the session starts in that mode on both consoles. The guest sees it
and cannot change it.

**The connection-quality chip's position is a user setting:** bottom right (default, where the session
chip sits today), top left (the FPS chip's slot), or "only when the ping class changes". It is
client-side only; the server is not involved.

**Ping thresholds per game come from the latency sweep** (section 2, the per-game table): the lobby's
verdict ("Great for trades, battles may pause") uses each game's measured tolerances, not guesses.

### How the lockstep maps onto it

The lockstep needs nothing new: the partner's buttons arrive later, and D absorbs that (section 2). The
budget per session kind:

<!-- BUDGET -->

### Bandwidth

A running session moves about 65-120 datagrams a second each way, of 40-190 B (the field logs and
`nd_stats`): roughly **10-25 KB/s per player per direction**, and about 50-100 KB/s at the relay per
2-player room. A 1 Gbit/s VPS port carries over 1,000 rooms. CPU is a table lookup per packet.

## 6. Server

**Requirements.** A public **UDP** port, so a VPS. A Cloudflare Tunnel publishes HTTP(S) services (the
website, the Lab portal) and does not carry arbitrary inbound UDP from the public internet. Cloudflare's
UDP products (Spectrum) are paid and enterprise-oriented. Use the tunnel for an optional HTTPS status
page and room lookup. The relay itself goes on a small VPS (1 vCPU, 512 MB-1 GB is plenty), near the
players: a central North American location keeps coast-to-coast RTT around 60-90 ms. Two regions later if
needed.

**Software.** One process, about 500 lines in Go or Python asyncio. It keeps a room table in memory:
no database and **no accounts**. Restarting it ends the live sessions, and that is the whole failure model.

**Security and abuse basics.**

- **No amplification.** A reply to an unverified source is never larger than the request (CREATED and
  ERROR are short). DATA is forwarded only between addresses that hold a valid token for the same room.
- **Spoofing.** Each slot is pinned to a source ip:port and a 64-bit random token. Packets that fail
  either check are dropped. A NAT rebinding (new port, same token) re-pins once per 10 s.
- **Room guessing.** Rate-limit JOIN per source IP (5 a minute). Codes expire. A full room refuses.
- **Flooding.** Rate-limit per slot: about 300 datagrams/s and 64 KB/s, where a real session uses 120 and
  25. Cap rooms per IP (3) and in total.
- **Version.** CREATE and JOIN carry the GBAdhoc build id. A mismatch is refused with a readable ERROR,
  before netdrv's own admission check would refuse it inside the session.
- **Privacy.** Nothing is logged beyond counters (rooms, bytes, errors). Peers never learn each other's IP
  address, which the relay model gives for free. There is no encryption: the PSP has no DTLS. The payload
  is button presses and game link traffic, and the page that explains the feature says so.

### One relay core, two hosts: the VPS server and GBAdhoc XMB for Windows

The owner wants `GBAdhoc-XMB.exe` (the Windows XMB Entry Maker, `tools/xmbwrap/app`: C, Winsock
`net.c`, a WebView2 UI) to be able to host the relay as well. Spec: **one relay core, two builds.**

- **`relay_core.c`** is portable C with no threads and no allocation after init. It holds the room
  table, the GBR1 parser, slot pinning, tokens and rate limits. It sees one UDP socket through a tiny
  socket shim (POSIX / Winsock) and a monotonic clock. Host tests run it over loopback with the SDL
  twin's `transport_relay`, the same way netdrv's tests run.
- **`gbadhoc-relay`** (Linux) is the core plus a `poll()` loop, a `/health` counter endpoint and
  systemd. This is the always-on **public relay** on the VPS.
- **GBAdhoc XMB, Online tab, "Host online room"** links the same core into the app, running on its own
  thread while the tab says so:
  1. Bind UDP (default port 27420; configurable).
  2. **UPnP-IGD** port mapping (SSDP discovery, then `AddPortMapping`, lease 1 h, renewed every 30 min,
     deleted on stop). NAT-PMP/PCP as a second try.
  3. Learn the public address (the IGD's `GetExternalIPAddress`, checked by one round trip to the public
     relay's echo).
  4. Make the room and show its **code**. The code is registered with the public relay as a pointer
     (code -> the app's public ip:port), so a joining PSP only ever types a code. It looks the code up at
     the public relay and is redirected to the app.
  5. **Fallback.** If the router refuses or has no UPnP, the mapped port is unreachable (the echo test
     fails), or the public address is CGNAT space (100.64.0.0/10) or private, the app says "Your router
     didn't open a port, so this room runs on the shared GBAdhoc relay", and the room is created on the
     public relay instead. The app then only shows the code and the session's status.

**Trade-offs.**

| | public relay (VPS) | relay in the desktop app |
|---|---|---|
| reachability | always: both PSPs dial out | needs a UPnP router and a public IPv4. CGNAT, many ISPs' IPv6-only plans, dorms and phone hotspots cannot host |
| latency | PSP A -> VPS -> PSP B: two internet paths. With a central VPS, about +20-60 ms over a direct path for two players in one region | the hosting player's PSP is on the app's LAN, so its leg is ~2-5 ms (Wi-Fi). Only the remote player crosses the internet: often the **lowest** RTT possible, below the VPS path |
| availability | as good as the VPS | only while that PC is on and the tab is open. Sleep or a closed lid ends the room |
| operator cost | the project pays for and polices one server | none. Each host carries their own traffic (~25 KB/s a player) |
| abuse surface | central limits, one place to fix | each host's PC exposes one UDP port. The same core limits apply, and the port is open only while hosting |
| privacy | peers never see each other's IP | the remote player learns the host's public IP, which the tab says before hosting |

Recommendation: ship the **public relay first** (it works for everyone) and the desktop host as a
"lower ping when your router allows it" option on the same code. The desktop host never needs the PSP to
know the difference: it is the same GBR1 protocol at another address.

**What the Online tab shows** (the app's WebView2 UI, beside the entry maker's tabs):

- **Status line:** "Not hosting" / "Hosting room **KQTR** on this PC (port 27420 open via UPnP)" /
  "Hosting on the shared relay: your router didn't open a port".
- **Room code**, large, with a copy button. Under it, players: slot, nickname (netdrv's), PSP model if
  known, connected time, **ping to this PC** in ms (from the relay's KEEP/DATA timing), packets/s, and
  loss %.
- **Network check:** UPnP found or not, mapped port, public IP (hidden behind "show"), NAT type (open /
  UPnP / CGNAT / unknown), and the round trip to the public relay.
- **Buttons:** Host online room / Stop. Settings: port, "Use the shared relay only", "Allow up to 4
  players" (later).
- **Log** (collapsible): joins, leaves, version refusals ("Refused: the PSP runs GBAdhoc 4.6.0-beta.3,
  this room needs beta.4"), rate-limit drops.
- A one-line notice: game files never pass through this PC. Only link traffic does, as between two PSPs.

**What the server never carries.** No ROM and no save data, beyond what link play already exchanges
between the two consoles. The GB/GBC session can **stream the partner's cartridge** when this console's
library lacks it (fe_gblink NEED/ROM, bulk lane). **Online, that path is switched off:** both players must
own the cartridge, the session compares SHA-1s only, and a mismatch is refused with "Both players need this
game". The battery-image and save-state exchange that every GB/GBA cable session already performs (so both
consoles run identical machines) stays, as link play already does it. "Share game data" (save sharing) is
not offered online.

### Desktop hosts and discovery (built 2026-11-05, branch claude/online-desktophost)

**Why.** Desktop hosting is the encouraged path, so the project does not have to run a public relay for
everyone forever. `relay.rayquaza.ca` stays up while traffic is low. A desktop host runs the same core as
the server (`relay_core.c`, the cookie challenge, CANDIDATES, PUNCH), so it does what the server does:
two PSPs that join a desktop-hosted room are introduced and punch a direct path, and the PC relays only
when the punch fails. Relaying through the PC is the default when the punch fails (owner, 2026-11-05).

**How a PSP finds a desktop host: two codes, one Join box.** Wireless > Online > *Join with code* takes
either code, and the PSP tells them apart by length (owner, 2026-11-05: both codes, kept separate; the room code
is shown first, the invite code is the backup):

- **Room code**: four letters, looked up on `relay.rayquaza.ca` (the directory). The PSP sends LOOKUP. If
  the code is a desktop host's, the answer is that host's address, and the PSP talks GBR1 to the host as
  its server from then on: PROBE, JOIN, KEEP, relay, rendezvous. If it is a room on the shared server
  ("here"), or nothing, or an older server that does not answer within 1.6 s, the PSP carries on at the
  server as before. So existing shared-server rooms are unchanged, and the joiner's wait for a host that
  is not up yet works as before.
- **Invite code**: 8 characters (11 for a host on another port) that carry the PC's IPv4 address and a
  check. No server is asked at all. The online module checks the code before it opens a socket (the Join
  screen passes it on as typed: a decoder in the EBOOT cost 976 B of text and, past the 8 KiB step that
  `.data` is pinned to, 8 KiB of boot heap), so a mistyped code never sends a packet; the toast says
  "a room code is 4 letters, an invite 8 or 11" or that the invite does not check out. Format below.

**The Join box** (Wireless > Online > *Code*): 11 places, each spun with up/down through `A-Z`, `0-9` and
a blank; left/right move, never past a blank, and the first blank ends the code. Four letters is a room
code, 8 or 11 characters an invite (dashes are only for display on the PC; the PSP shows the code
ungrouped).

**The desktop is the host.** Both players press *Join with code* (the design mockup's flow). The first
PSP into a desktop host's room opens it and is told **slot 0**: it runs the netdrv host, and its link
mode becomes the room's (`relay_core.c open_home`). The second is slot 1 and follows that mode. *Create
room* still makes a private room on the shared server. A PSP that Creates with a desktop's code also
works: the desktop's home code may be named in a CREATE. A JOIN with no code (a PSP that came by an
invite) lands in the host's **home room** (`relay_core_set_home`), the room whose code the desktop shows
and registers.

**Wire format** (new GBR1 types, same 16-byte header; `relay_proto.h`):

    REGISTER    17  host -> directory: code[4] (0s = server picks) adv_port[2] priv_ip[4] priv_port[2]
                    flags[1] cookie[8]; header token = 0, or the entry's token to refresh.  Padded to 64 B,
                    a refresh to 32 B.
    REGISTERED  18  directory -> host: code[4] ttl_s[2] adv_port[2]; header token = the entry's token
    LOOKUP      19  PSP -> server: code[4] nonce[4]; padded to 64 B
    LOCATED     20  server -> PSP: code[4] nonce[4] kind[1] (0 none, 1 a room here, 2 a desktop host)
                    host_pub_ip[4] host_pub_port[2] host_priv_ip[4] host_priv_port[2] your_ip[4]
    UNREGISTER  21  host -> directory: code[4]; header token = the entry's token

**Directory rules** (the same as rooms):

- A first REGISTER needs the cookie (a COOKIE answers it). The entry's address is the **source** of the
  REGISTER and never an address the host claims, so nobody can point a code at a victim. Only the port is
  advertised: it is the router's UPnP mapping, which the app requests port-for-port.
- No reply is larger than its request: REGISTER and LOOKUP are padded. A refresh can only be answered by
  REGISTERED or COOKIE (24 B each), so it may be 32 B. A server without a directory answers a short
  REGISTER with nothing.
- Limits: 120 LOOKUPs a minute per IP. A LOOKUP of a code that names nothing counts as a join guess (30
  a minute per IP, shared with JOIN; repeating the same missed code is waiting, not guessing). 2 codes per
  IP, 256 entries (`max_dir`). An entry lives `RL_DIR_TTL_S` = 180 s after its last REGISTER; the host
  refreshes every 60 s and sends UNREGISTER on Stop.
- One namespace: a directory code is never a room code on the shared server, and the other way round.
- **The shared server answers lookups only.** It never makes a room for a directory code and never
  forwards a directory room's traffic. The tests assert both: `test_relay_desktop` counts forwarded DATA
  at the directory, and `natlab.py` greps its log for rooms.
- **Privacy.** The directory stores a code, the host's source address, the advertised port, the host's
  LAN endpoint and a token. It never stores who looked a code up. A LOOKUP tells the asker the host's
  public address, which is the price of a direct connection to a PC. The host's LAN endpoint is given
  only to a PSP that asks from the host's own public address (the same home), because many routers do not
  loop their public address back. That PSP then connects over the LAN.

**What it costs `relay.rayquaza.ca`** (UDP payload; add 28 B of IP and UDP headers per datagram on the wire):

| what | datagrams | bytes |
|---|---|---|
| a desktop host starts (REGISTER, COOKIE, REGISTER, REGISTERED) | 4 | 176 B |
| keeping it listed (32 B refresh + 24 B answer, every 60 s) | 2/min | 3.4 KB per hour (measured 3,480 B in `test_relay_desktop`'s one-hour run) |
| each PSP that joins by room code (LOOKUP + LOCATED) | 2 | 105 B |
| each PSP that joins by invite code | 0 | 0 |
| Stop (UNREGISTER) | 1 | 20 B |

A two-player hour by room code costs the shared server about **3.8 KB** of payload, about 7 KB on the
wire. For comparison, relaying that same hour would be roughly 70-180 MB (10-25 KB/s a player each way,
section 5). Measured per session in the NAT model: 386 B with both players by room code, 176 B with both
by invite code (registration only). The design mockup's estimate was about 4 KB per room-hour.

**Invite codes** (`netdrv/invite_code.h`; tests in `tools/relay/test_invite.c`). The alphabet is Crockford
base32: `0-9 A-Z` without `I L O U`. Codes are shown in groups of four with dashes. On input, case, dashes
and spaces do not matter, and `O`->`0`, `I`/`L`->`1`, `U`->`V` are folded.

- **Short form, 8 characters = 40 bits:** the IPv4 address (32 bits, `a` first), then
  `CRC-8(a,b,c,d) ^ 0x5A` (8 bits). The port is GBAdhoc's default, 27420, and its probe port is 27421. This
  is exactly the mockup's format, and the test checks vectors made by the mockup's own encoder.
- **Long form, 11 characters = 55 bits:** the IPv4 address, the port (16 bits), then
  `CRC-7(a,b,c,d,port_hi,port_lo) ^ 0x5A & 0x7F` (7 bits). This is for a host on another port.
- CRC-8 uses polynomial 0x07 and CRC-7 uses 0x09 (x^7+x^3+1, primitive). Both start from 0, are not
  reflected, and run MSB first. 0x5A is the format's version tag.
- **Deviation from the mockup:** the mockup's long form used the low 7 bits of the CRC-8, and
  `test_invite` found it lets about 1 in 128 single-character typos through. A real CRC-7 lets none
  through. The short form is unchanged.
- **What the check catches, measured exhaustively:** every single wrong character and every swap of two
  different characters, in both forms (4.96 M + 0.54 M short-form and 6.8 M + 1.07 M long-form cases).
  About 1 in 256 (short) or 1 in 128 (long) other mistakes get through: random strings pass 0.34 % /
  0.68 % of the time. Decoding also refuses 0.x.x.x, 224.0.0.0 and above, and port 0.
- **Codes never expire** (owner, 2026-11-05). An invite is valid while the PC keeps its address and port.
  It encodes the PC's IP, so hiding the public IP on the Online tab (the default) is cosmetic for anyone
  who holds the code. The app says so.

**Desktop app (GBAdhoc XMB, branch `relay-discovery`).** When hosting, the app picks a home code,
registers it with the directory (on by default; a setting, and `XMB_DIRECTORY=off`), and shows the room
code first while the directory is reachable, with the invite code as the backup. When the directory is
down, the invite code becomes the main option. It also shows a LAN invite for PSPs at home, and each
player's path as **Direct** / **Via host** (the PSP's session chip says *direct* / *via host* too). The
public IP stays hidden until *Show*. There is no QR code (owner, 2026-11-05).

**Tests, all offline:**

- `test_relay_desktop`: the NAT model with a directory, a desktop host behind a UPnP-forwarding router,
  and two PSPs with real netdrv sessions. 11 cases: by code / invite / mixed, cone, symmetric, public, a
  PSP in the PC's home without NAT loopback, and both players joining. There is also a one-hour directory
  run with a server restart (the code is kept) and UNREGISTER.
- `test_invite`, and `fuzz_relay` over the new types on a directory core and a home-code core: 2 M
  datagrams x 6 seeds under ASan/UBSan, with no amplification. (Seed 7 once reported "18 bytes out for
  15 in (type 5)". That was the fuzzer's fault: a timer's PEER LEFT to a member still in a room was
  checked as if it answered the last datagram, whose source happened to be that member. The fuzzer now
  clears the source before ticking the cores.)
- `natlab.py BIN desktop`: real sockets in network namespaces. The directory is on 203.0.113.1. The
  desktop host is `gbadhoc-relay --home --register` behind a cone NAT with UPnP as two static forwards.
  PSPs are behind cone and symmetric NATs. The paths are direct where both NATs allow it and relayed
  otherwise. 8/8 on 2026-11-05; again on 092a4879 with the shared matrix, 14/14 (`unshare -rn`, no
  root). `fuzz_relay` 2 M x seeds 1, 3, 5, 7 (even seeds equal odd ones: the seed is OR 1), and
  `test_invite` / `test_relay_nat` 8/8 / `test_relay_desktop` 12/12, all PASS under ASan/UBSan.
- **PPSSPP, two GBAdhoc instances by invite code** (2026-11-05, the lab build of this branch,
  `RFU_SCHED=1`): `tools/relay/ppsspp_online.sh` with `RELAY_ARGS="--home QWER --public 127.0.0.1"`, which is
  the desktop host's mode in a loopback-only namespace. Both harness inis set `online_room` to the invite
  `FW00-00DX` (B typed it as `fw0000dx`) and `online_relay` to an unreachable TEST-NET address, so only the
  invite can find the host. Both decoded 127.0.0.1:27420 and landed in the home room QWER. A was slot 0 and
  hosted, and the path was direct. The Emerald Union Room trade (`rfu_em_trade`) logged `trade_complete` on
  both, with clean exits after 162 s. Jobs and logs: `builds/online-out/ppsspp-invite`.
- **PPSSPP, room code + invite code** (2026-11-05, lab build of 092a4879, the module checking codes):
  two relays in the loopback namespace, a directory on 127.0.0.1:27430 and the desktop host
  (`--home QWER --register 127.0.0.1:27430`) on 27420 (`builds/desktop-relay-kit/work/ppsspp_dh.sh`).
  A typed the room code `QWER` with `online_relay` at the directory: one LOOKUP, LOCATED as a desktop
  host. B typed the invite `fw0000dx` with an unreachable `online_relay`. Both in room QWER, path
  direct both ways, `trade_complete` on both, clean exits after 161 s. The directory saw 1 REGISTER,
  1 LOOKUP, no room and no forwarded byte: 393 B of payload for the session.
- **Boot heap, ONLINE=0 against ONLINE=1** (release, `RFU_SCHED=1`, this branch): both 14,014,860 B
  image and 10,081,692 B PSP-1000 boot heap, the same as 4.6.0-beta.4 (c7a74a88). The pads are 112/48
  (beta.4: 144/32): against beta.4's ELF the hot core, A's cold text, `.bss` and the guard's block are
  unmoved, A's tables sit -24 B (the `.rodata` pad moves them in 64 B steps: 72 puts them at +40) and
  B's `.data` +64 B. The module's imports are unchanged (ThreadManForUser, sceNetInet, sceNetResolver).

**Still to test on hardware and the real internet:**

- a real PSP decoding an invite and joining a PC;
- the slot-0 joiner as netdrv host on hardware;
- a real router's UPnP mapping as the advertised port;
- hairpin-less routers with a PSP at home;
- carrier-grade NAT on the host side (the app says "players outside cannot reach this PC");
- the directory on the VPS;
- real-internet punching (home Wi-Fi against a phone hotspot), which needs the owner's go-ahead.

## 7. Implementation plan

1. **Relay server and a desktop twin.** Write the relay. Add `transport_relay` to the SDL build first
   (`transport_udp.c` is the model) and run two desktop instances through a local relay. This is where the
   protocol gets debugged, as Mystery Gift's twin was.
2. **PSP transport.** `netdrv/transport_relay.c`, transcribed from `transport_inet.c`: one socket, DNS
   lookup of the relay host (sceNetResolver), CREATE/JOIN handshake, slot-MAC mapping, KEEP in the lobby.
   The apctl bring-up reuses `mgift_net.c`'s ladder with the user's saved profile (sceUtilityNetconf).
3. **Online switches.** Force `gblink_input_copies` on, cartridge transfer off, and `gblink_delay_auto` on
   (cap 12) whenever the transport is the relay. Feed `rfu_sched` D from the measured RTT the same way,
   after the hardware sweep for it.
4. **Lab: an internet-shaped case.** Run the real relay on the LAN with `net_latency_ms`/`net_jitter_ms`
   on the consoles, so the same fixtures run through the relay path: a matrix of 0/60/100/150 ms with
   10 ms jitter and 1 % loss, on all eight fixtures.
4b. **Direct path** (section 5, "Direct path"): PROBE/CANDIDATES/PUNCH in the relay core and
   `transport_relay`, the WSL-namespace NAT matrix, then the two-PSP LAN test. The relay stays the
   default until the matrix passes.
5. **Menu.** Wireless > Online (experimental) > Host / Join. Join shows the OSK for the code, the room
   shows the code, the measured ping and the chosen delay ("Ping 92 ms, input delay 6 frames").
6. **Real internet, owner's two consoles.** One console on home Wi-Fi, one on a phone hotspot (CGNAT):
   both trades, the AW2 battle and a Super Circuit race. Then a test with a remote friend (KarnFu).
7. **Beta.4 inclusion** (below), and only after steps 4 and 6 pass.
8. **Desktop host** (after the public relay works): link `relay_core.c` into GBAdhoc XMB, add UPnP-IGD
   with the shared-relay fallback and the Online tab, and register room codes as pointers on the public
   relay. Test it behind a UPnP router, a router with UPnP off (it must fall back), and a CGNAT hotspot.

### What an experimental beta.4 needs

- A menu entry **labelled Experimental**, **default off**, hidden behind a Settings toggle ("Online play
  (experimental)"), with one screen that says what it is: link play through a GBAdhoc relay, Wi-Fi
  needs 2.4 GHz and WPA2 or older, both players need the same GBAdhoc version and their own copy of
  the game, and no game files are sent.
- A ping and delay readout in the room, and the game list filtered to what is known to work: the
  turn-based fixtures first (Pokemon trades and battles, Advance Wars 2). Super Circuit is marked "may
  lag".
- The relay host name in `config.ini` (default: the project's relay), so a community can run its own.
- A kill switch on the server side: the relay answers CREATE with "online play is paused" when needed.
- The netdrv admission check unchanged: a mismatched build is refused, never run.
- Zero cost when it is off. The relay transport is linked but never initialised, and the ad hoc path is
  byte-identical. The Lab's multiplayer suite must show no change against beta.3 (multiplayer-no-
  regressions).

## 8. Constraints honoured

- No ROM or save data crosses the server beyond what link play already exchanges (section 6).
- Nothing went over the real internet in this study. The delay was injected between two consoles on the
  same desk.
- The delay shim stays harness-only (dead in release). The auto delay is off unless a key asks for it.
