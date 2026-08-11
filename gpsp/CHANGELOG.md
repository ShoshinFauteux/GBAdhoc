# Changelog

All notable changes to **PSP AGB / gpSP-AdHoc** — the native PSP frontend for the modern
gpSP core with GBA Wireless Adapter multiplayer over PSP ad-hoc WiFi.

Format loosely follows [Keep a Changelog](https://keepachangelog.com/). Versions are
semantic. The emulator core itself comes from
[libretro/gpsp](https://github.com/libretro/gpsp); core changes made here are listed
separately and are intended for upstream.

---

## [0.1.0] — unreleased

First release. Everything below exists and has been exercised; the "Known issues" section
is not a disclaimer boilerplate, it is the actual list.

### Added — the frontend

- **Native PSP frontend** hosting the gpSP libretro core in-process (statically linked, no
  RetroArch). Boots to a ROM browser, runs GBA games, exits cleanly with the save flushed.
- **Video:** GU-accelerated blit with scaling presets `1x` / `fit` / `stretch`, each with
  nearest or bilinear filtering — Triangle cycles the five presets in-game, and the choice
  persists in `config.ini`.
- **Audio:** core mixes at 65536 Hz, resampled to the PSP's 44100 Hz output on its own
  thread through a lock-free ring; the ring's fill level is the pacing master.
- **Fast-forward** on Square (hold by default, toggle optional), multipliers 1.5× / 2× / 3×
  / uncapped, core frameskip engaged while active, on-screen chip. Forced to 1× while a
  wireless session is live.
- **In-game menu** on `Select`+`Start` held ~¼ second: Resume / Save state / Load state /
  Wireless / Settings / Exit. No HOME-button hooking, so it is CFW-safe.
- **Settings** (persisted): video scale, video filter, FF multiplier, FF hold-vs-toggle,
  room code.
- **Savestates**, one slot per ROM (`<rom>.st0`), staged through a static buffer so they
  never compete with the core's ROM allocations.
- **CPU clock** set to 333/333/166 MHz at init and logged, per the project's
  overclock-first policy for CFW hardware.
- **Structured `EVT` log** at `log/frontend.log` — a product feature, not scaffolding: it
  is what makes a field failure diagnosable without the user in the debug loop.
- **XMB branding:** title "PSP AGB" (after the AGB-015 adapter), custom ICON0/PIC1.

### Added — wireless

- **`netdrv`**, a full-mesh reliable-datagram driver implementing the libretro
  `retro_netpacket` contract: sliding-window ARQ with retransmission and duplicate
  detection, CRC + exact-length framing, host-broadcast roster with generation counters,
  asymmetric death authority, and per-peer channels. All core traffic is
  `RELIABLE|FLUSH_HINT`, which is what the emulated adapter demands.
- **PSP ad-hoc transport** over `sceNetAdhoc` PDP datagrams: adhocctl group create/join,
  a dedicated RX thread, blocking receive in timeout slices so teardown never hangs. Static
  singleton allocation — a session can't fail to start because the heap fragmented.
- **UDP transport** for the desktop development twin, with injectable packet loss and
  jitter for impairment testing.
- **Wireless UI panel:** Host session, Join by scan (~10 s ad-hoc scan with a results
  list), Join by room code (`GPSP00`–`GPSP99`), live session status row, Disconnect, plus
  a persistent session chip during play.
- **Save auto-backup:** `<rom>.sav` → `<rom>.sav.bak` before every session start.
- **Savestates blocked** (save *and* load) while a session is active, with a toast — the
  core does not serialise adapter state, so a mid-session load would desync both consoles
  permanently.
- **Silent-wireless mode** for per-game "invisible" builds: the session negotiates itself
  (join-first, promote-to-host on a jittered timeout) at the moment the game activates the
  adapter, with zero emulator UI. See `docs/VARIANTS.md`.
- **Per-game variant builder** (`tools/make_variant.sh`): an EBOOT with its own XMB title
  and icon that boots straight into one baked ROM, keeps saves in a private namespace, and
  brings wireless up silently.

### Added — validation

- An autonomous PPSSPP-based end-to-end harness (`tools/e2e/`) that drives the real EBOOT:
  boot, 30-minute soak, in-game save persistence across restarts, GU colour-order
  regression check against real GE readback, UI smoke, transport echo, and a **fully
  autonomous Emerald ↔ Emerald Union Room trade with a party-swap oracle checked in both
  RAM and the post-run `.sav`** — on the desktop twin under 5 % packet loss + 30 ms jitter,
  and on two PPSSPP instances over the PSP ad-hoc transport. Plus peer-disconnect runs that
  assert the survivor recovers with the game's own error dialog and exits cleanly.
- `tools/make_release.sh` (this release's packaging) and `docs/HARDWARE-ACCEPTANCE.md`
  (the two-console hardware gate).

### Changed — the emulator core (minimal, intended for upstream)

- **`rfu.c`: link-frame queues 4 → 16** (`RFU_PKT_QUEUE`). The core buffered
  netpacket-delivered frames in fixed 4-deep queues and silently discarded overflow — a
  depth that assumes a zero-latency transport. Any real transport delivers legitimate
  clumps deeper than 4, gen-3 games never retransmit their one-shot commands, and a dropped
  clump wedges both games with no diagnostics. Two array sizes and the matching
  enqueue/dequeue loops; no logic change; behaviour-identical at zero latency. Relevant to
  upstream RetroArch netplay over a WAN, not just to us.
- **`rfu.c`: weak `gpsp_rfu_activated_hook()`** fired when a game completes the adapter
  login handshake — 8 lines, a no-op for any frontend that doesn't override it. This is
  what makes silent wireless possible without per-ROM RAM tables.

- **`rfu.c`: weak `gpsp_rfu_link_down_hook(reason, slot)`** — the symmetric partner of the
  activated-hook, fired when the emulated RFU link ends: locally (`RFU_CMD_DISCONNECT`, i.e.
  the player left the room), on a peer `NET_RFU_DISCONNECT`, on adapter reset, or on a
  host-side client timeout. Also a no-op for any frontend that doesn't override it. Nothing
  in the libretro netpacket contract tells a frontend that the *core's* wireless session
  ended — `netpacket_stop()` is frontend-driven only — so a log could not distinguish "the
  game ended the session" from "the frontend tore the transport down". Now it can.
- **`rfu.c`: two receive-path bounds checks.** `NET_RFU_CLIENT_SEND` took its payload length
  from a header byte (0–255) straight into a 16-byte buffer, with no `len >= blen + 12`
  guard — unlike `NET_RFU_HOST_SEND` immediately above it, which has both — so a malformed
  or foreign packet could read past the received datagram and overwrite neighbouring client
  slots. `NET_RFU_BROADCAST` read 6 payload words behind a `len >= 12` gate. Legitimate
  senders cap both, so no working behaviour changes; this is hardening, not a diagnosed
  fault.

All four are offered upstream with this release (see `docs/RELEASE.md`).

### Known issues

- **Wireless session stability on real radio is being actively fixed.** In a field session
  with two real consoles the games discovered each other and began entering the Union Room,
  then dropped to "communication error" a few seconds later. The cause is understood and
  quantified from both consoles' logs: our ARQ retransmit timeout is a fixed 30 ms floor
  tuned on loopback, which is far shorter than real ad-hoc round-trip time, so we
  retransmit before an acknowledgement can possibly arrive. That produces a duplicate storm
  (68 % of received traffic was duplicates; ~3.8 transmissions per delivered payload
  against 1.1 in the emulated harness), the send window stalls, the transmit queue fills,
  and **two RELIABLE payloads were silently dropped** — which destroys the emulated
  adapter's state machine even though the radio and our session stay nominally healthy.
  Nothing was actually lost on the air: `txfail=0`, `rxerr=0`. The fix is a correctness
  change (never silently drop RELIABLE payload — apply backpressure or fail the session
  visibly) plus an adaptive RTT-based retransmit timeout with a PSP-ad-hoc floor. The
  emulated harness cannot reproduce this class at all, which is exactly why it survived to
  the field.
- **Peer discovery requires a fixed ad-hoc channel.** Two consoles on `Automatic` each
  create their own identically-named group and never see each other, silently. Documented
  as a prerequisite rather than fixed in code; a scan-then-join-with-retry path that cannot
  fall through to creating a rival group is the intended code-side improvement.
- **Cross-edition trade support is incomplete.** The automated trade oracle covers Emerald ↔
  Emerald. FireRed ↔ LeafGreen (and cross-edition combinations generally) need their own
  autopilot address tables built from pret rev-1 symbols before they can be claimed as
  tested. The games themselves are expected to work; we simply haven't proven it.
- **PSP-1000 memory headroom is tight.** The core allocates the ROM greedily and a wireless
  session adds roughly 300 KiB of driver plus the system's network pool and thread stacks
  on top. It fits — with a 16 MiB ROM fully resident there was headroom to spare in
  measurement — but the 1000-series is the binding constraint, and the ARQ window and queue
  depth are already halved there relative to the desktop build. The field run also suggests
  the 1000 pumps the driver more slowly than a 3000 and generates more traffic; engaging
  frameskip while linked is on the list.
- **No suspend/resume handling.** The frontend keeps the console awake during a session but
  does not tear the session down on suspend or rebuild it on resume. Behaviour across a
  suspend is currently unknown and is an explicit observation step in the hardware gate.
- **Re-linking after a disconnect** requires going back through the game's own wireless
  counter; the emulator does not attempt to re-establish a dropped session by itself.
- **Two-console hardware acceptance has not been signed off.** Single-player on real
  hardware is verified (Pokémon Emerald at ~59 fps of 59.73 at 333 MHz on a CFW PSP); the
  full wireless trade is verified in emulation only. `docs/HARDWARE-ACCEPTANCE.md` is the
  gate that closes this, and it is gated behind the wireless fix above.

### Not in scope

Link **cable** emulation, internet play, infrastructure-mode WiFi, lobby servers, GB/GBC
games, e-Reader, more than 5 players, RetroArch feature parity, and non-PSP platforms. The
desktop build exists as a development twin and test rig, not as a product.
