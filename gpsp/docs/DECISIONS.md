# Architecture Decision Records — gpSP-AdHoc

Format per ADR: Context / Decision / Alternatives / Consequences / Upstream impact.

---

## ADR-0001 — Base repo and commit

- **Context:** Plan §3.1/§11 requires pinning a base. Compared `libretro/gpsp` master vs `davidgfnet/gpsp` master on 2026-07-31.
- **Decision:** Base = `libretro/gpsp` master @ `5b6e751f4abf368509146cd143c949c1946ac1ae` (2026-07-21). `davidgfnet/master` (`602512d`, 2026-01-27) is a strict ancestor — libretro is 36 commits ahead, 0 behind. Remotes kept: `upstream` (libretro), `davidgfnet` (for RFU-related WIP branches: `rfu_v1`, `wip_multiplayer`, `link_wip`).
- **Alternatives:** Basing on davidgfnet/master — rejected, strictly older.
- **Consequences:** Rebases against upstream stay cheap; davidgfnet's experimental branches remain visible for reference.
- **Upstream impact:** None yet.

## ADR-0002 — Local repo only until user picks hosting

- **Context:** `gh` CLI is not installed/authenticated; plan §11.9 asks for the user's repo-visibility preference.
- **Decision:** Work in the local clone with a branch-per-phase. Defer creating the GitHub fork until the user states a preference (public fork of libretro/gpsp vs private repo).
- **Alternatives:** Creating a fork immediately — blocked on auth anyway.
- **Consequences:** No remote backup yet; user should answer the hosting question soon.
- **Upstream impact:** None.

## ADR-0003 — Transport design corrections from the Phase-0 code read

- **Context:** Plan §4.3 assumed a star topology (host relays), ≤128 B payloads, and mixed reliability flags. The SERIAL-PROTO-NOTES read of the actual core (2026-07-31) contradicts all three.
- **Decision:** The netdrv driver is designed as a **full mesh**: any peer can unicast to any client_id and any peer can broadcast (the RFU host role is independent of libretro client_id; client↔client traffic is real). Frame budget sized for **560 B payload** (RFU max is 104 B; cable modes reach ~520 B — cable is out of scope but the headroom is free on PDP datagrams). ARQ carries **all** traffic: the core sends exclusively `RELIABLE|FLUSH_HINT`, so the unreliable path is implemented but cold. `protocol_version` string is exactly `"gpSP v1.0"` and must be matched in session handshake. CRC + exact-length framing is mandatory (core receive path is not garbage-tolerant: rfu.c:735, rfu.c:845); client_ids restricted to 0–4.
- **Alternatives:** Host-relay star — rejected, would add latency and contradicts core behavior; per-flag QoS paths — pointless given all-RELIABLE reality.
- **Consequences:** Session roster must map client_id↔MAC for every peer on every peer; ARQ throughput is the whole game; `send_fn` must be re-entrant (core sends from inside `receive`).
- **Upstream impact:** Four core quirks found during the read are logged in SERIAL-PROTO-NOTES open questions for upstream report (no local core patches).

## ADR-0004 — Clock policy: §9 governs over risk-register row 6

- **Context:** Plan §9 says default 333 MHz everywhere including sessions; plan risk-register row 6 mitigation says "222 during sessions by default". They conflict.
- **Decision:** §9 governs (v1.1 explicitly made CFW-overclock-first the policy; the risk row is stale v1.0 text). Default 333/333/166; Settings fallback 266/222 kept as insurance for Gate 4-H.
- **Consequences:** Harness asserts clock=333 in logs during session tests.

## ADR-0005 — Desktop twin links the core as a static archive of unix objects

- **Context:** Phase 1 needs the SDL desktop twin to host the core. Options: dlopen the upstream `gpsp_libretro.so`, link the .so at build time, or link statically.
- **Decision:** Static: `sdl/Makefile` runs `make platform=unix clean && make platform=unix` in the repo root, then archives the resulting objects into `sdl/libgpsp_core.a` (excluding other-arch dirs `mips/ arm/ 3ds/ jni/` — unix and psp1 builds share object filenames in the same tree, so the clean-first + exclusion guard is load-bearing). PSP links `gpsp_libretro_psp1.a` the same static way.
- **Alternatives:** dlopen — extra indirection for zero benefit in a single-core frontend; linking the .so — leaves the desktop and PSP link models different for no reason.
- **Consequences:** Both frontends are single self-contained binaries speaking `retro_*` in-process; core rebuilds are explicit (`make core`); stale-arch objects can never leak into a link (verified: the first build failed exactly that way and the guard fixed it).
- **Upstream impact:** None (no core changes; unmodified core builds used).

## ADR-0006 — SRAM policy: persist the full 128 KiB, CRC32 dirty-compare

- **Context:** FRONTEND-AUDIT §5 + open question 3: the core always reports 0x20000 SRAM and has no dirty signal; `.sav` interop could argue for trimming to logical backup size.
- **Decision:** Persist the full 128 KiB (matches RetroArch `.srm` behavior and the user's real Emerald save, which is exactly 131072 bytes). Dirty detection = CRC32 of the buffer every 300 frames (~5 s) and on exit; write only on change (`fe_host_sram_flush`), `EVT sram_flush crc=...` on every write. Round-trip verified byte-identical on desktop (`--force-sram-write`).
- **Alternatives:** Trim to logical size — complicates the flush path and buys nothing for our own frontend; revisit only if cross-emulator `.sav` exchange becomes a feature.
- **Consequences:** Saves from VBA/mGBA (often already 128 KiB for gen-3) load as-is; smaller files load into an 0xFF-prefilled buffer (core normalizes).

## ADR-0007 — PSP memory posture: PSP_LARGE_MEMORY=0, 1 MiB heap slack, greedy ROM buffer kept

- **Context:** pspsdk's build.mak defaults MEMSIZE=1 (64 MiB large-memory mode) on FW≥6.00; the core greedily mallocs up to 32×1 MiB ROM blocks at `retro_init` (FRONTEND-AUDIT §8). Target hardware is PSP-1000 (24 MiB user partition).
- **Decision:** `PSP_LARGE_MEMORY = 0` in psp/Makefile so PPSSPP models the standard partition and Gate-1 numbers stay honest for the 1000-series; `PSP_HEAP_SIZE_KB(-1024)` leaves 1 MiB outside the heap; all frontend buffers are static/.bss/VRAM and created before `retro_init`; `ROM_BUFFER_SIZE` left at 32 (16 MiB Emerald loads fully resident).
- **Measured (PPSSPP, MEMSIZE=0, 2026-07-31):** after core init + Emerald load: `EVT mem_free=765952 max_block=524288` — ~750 KiB heap headroom with the ROM fully resident. Adequate for Phase 1; the netdrv/UI phases must budget from static allocations or shrink `ROM_BUFFER_SIZE` (documented compile knob), and the real-hardware verdict still lands at Gate 4-H.
- **Upstream impact:** None (build flags only).

## ADR-0008 — netdrv deviations from §4.3 and Phase-3 smoke scope

- **Context:** Phase-3 netdrv implementation (netdrv/, frontend-common/netpacket_host.c; design in docs/ARCHITECTURE.md). Three points deviate from the plan-§4.3 letter, and the two-instance smoke cannot observe core-originated RFU traffic without the autopilot.
- **Decisions:**
  1. **`connected()`/`disconnected()` are called host-side only** (plan §4.3 step 3 said "fire connected(peer) for each roster member"). libretro.h:3155-3165 defines them as host-side calls; on non-host instances they would only perturb cable-mode SIOCNT id math (SERIAL-PROTO-NOTES §6). `EVT peer_connected/peer_disconnected` still logs on all sides.
  2. **Roster sync is state-based, not event-based**: instead of relying on a one-shot ROSTER_UPDATE per change, the host broadcasts the full roster (with a generation counter) on change AND every 1 s; clients apply newer generations and diff to synthesize connect/disconnect events. A JOIN nonce detects same-address process restarts and resets stale ARQ channels. Loss-tolerant by construction — a dropped roster costs one refresh interval, never consistency.
  3. **Death authority is asymmetric**: host judges clients, clients judge only the host link (client↔client liveness is host-relayed via roster generations). Removes split-brain that symmetric 4 s timeouts would allow under partial connectivity.
  4. **Netsmoke scope (Gate-3 partial):** sitting at the Emerald title screen the core originates zero RFU packets (rfu.c broadcasts only while the game hosts an RFU session), so `run_netsmoke.sh` asserts: session establishment both sides, ≥ 60 s alive under 5 % loss + 30 ms jitter, and cross-process reliable delivery via a 1 Hz frontend-level probe (104 B, magic "PRB0", ≥ 12 B so the remote core's RFU receiver drops it silently on the documented magic check, rfu.c:865-867). `core_tx` is reported, not asserted. The full core-driven exchange is the Gate-3 trade test once the autopilot drives both instances into the Union Room.
- **Alternatives:** calling connected() on clients (spec-violating); event-based roster with per-event reliable delivery (more states, same outcome); asserting core RFU traffic in the smoke (impossible without navigation).
- **Consequences:** Gate 3's "trade over localhost UDP with 5 % loss + 30 ms jitter" remains open until the autopilot trade script exists; the transport half of that gate is proven by unit tests + netsmoke.
- **Upstream impact:** None.

## Gate 1 record — PSP frontend boot test PASSED in PPSSPP (2026-07-31)

`tools/e2e/run_boot_test.sh` exit 0, first run. Evidence in
`tools/e2e/artifacts/boot-20260731-160543/`: `frontend.log` (EVT sequence:
boot_ok, clock=333, bios=real sha1=300c20df..., rom_loaded code=BPEE
size=16777216, av_info fps=59.7275, sram_load size=131072 crc=3e141a01,
mem_free=765952, frame_dump, heartbeats to 3600, exit code=0),
`frame_000600.bmp` (visually confirmed: Emerald intro rain-on-leaves scene,
pixel-identical framing to the desktop twin's dump), `.sav` byte-identical
after the run (md5 e5af0862...). Desktop-twin evidence (WSL,
`~/gpsp-e2e/desktop-run/`): same EVT sequence, 3600 frames, frame-600 BMP
inspected, 128 KiB save round-trip byte-identical under --force-sram-write.
Remaining Gate-1 items for later Phase-1 work: 30-minute soak (needs FF),
save-persistence across restarts with an actual in-game save.

## Gate 1 record — COMPLETE (2026-07-31, second pass)

The two remaining items landed with the autopilot engine (ADR-0008), all on
the PSP `EBOOT.PBP` in the PPSSPP sandbox, all exit 0 on first attempt:

- **In-game save persistence** — `tools/e2e/run_save_test.sh` PASS, artifacts
  `tools/e2e/artifacts/save-20260731-172544/`. Run 1: RAM-predicate-synced
  start-menu SAVE under uncapped FF (title → main menu → overworld →
  start menu → save dialog → overwrite → flash write detected by SRAM CRC
  3e141a01→151e834c → interactivity re-check), `.sav` rewritten
  (md5 e5af0862…→5739c531…). Run 2: `sram_load crc=151e834c` (the run-1
  flush) and player restored at the saved spot — `gSaveBlock1Ptr`-derefed
  pos=0x00060033 loc=0x00002000 equal across runs; restored overworld BMP
  (Route 117 Day Care) pixel-identical between desktop twin and PSP runs.
- **30-minute soak** — `tools/e2e/run_soak.sh` PASS, artifacts
  `tools/e2e/artifacts/soak-20260731-172944/`. 108,024 emulated frames
  (> 30 min at 59.7275 Hz) of overworld activity under uncapped FF:
  clock=333, heartbeats gapless 600..108000 (180 beats, no EVT gap), script
  interactivity check at the end, clean exit, `.sav` byte-identical.
- **Boot test regression** — `run_boot_test.sh` re-PASS with the
  autopilot-enabled EBOOT (`boot-20260731-180128/`); `mem_free=765952`
  unchanged (ADR-0007 headroom intact).

Gate-1 checklist (plan §5 Phase 1) is fully green. Autopilot addresses +
provenance: docs/AUTOPILOT.md; harness recipes: docs/TESTING.md.

## ADR-0008 — Autopilot: RAM predicates over frame timing; SRAM-CRC as the save oracle

- **Context:** Plan §7.1.3 + risk-register row 11: blind frame-stamped input scripts flake on game
  timing/RNG. The engine needed a sync primitive and a "the game really saved" signal.
- **Decision:** Every state transition in a script waits on an emulated-RAM predicate
  (`waitram`/`mash` on `(mem[addr]&mask)==val`), reading EWRAM via
  `retro_get_memory_data(SYSTEM_RAM)` and IWRAM via the stored `SET_MEMORY_MAPS` descriptors.
  Function-pointer predicates compare against `pret_symbol | 1` (Thumb bit). Save completion is
  detected game-agnostically by the frontend's own CRC32 of the 128 KiB SRAM buffer changing
  (`waitsram`/`mashsram`), not by a game symbol. Addresses live in the committed scripts with the
  verified table + provenance in docs/AUTOPILOT.md; the engine itself is game-agnostic.
  Uncapped FF is shared harness infrastructure in both frontends (no pacing, muted audio, 1-in-32
  blits); the user-facing FF feature remains Phase 5A.
- **Alternatives:** OCR/pixel matching on frame dumps — fragile and slow; savestate-parked fixtures
  only — still needs predicates to *create* fixtures; per-game core hacks — violates prime
  directive 1.
- **Consequences:** Scripts are per-ROM-revision (BPEE rev 0 now; FR/LG need their own tables from
  pret symbols at Phase 4). Predicate timeouts fail fast with `EVT ap_fail` and exit code 3, so a
  wrong address can never silently pass. Verified live: every predicate fired identically on the
  desktop twin and the PSP EBOOT in PPSSPP on the first attempt.
- **Upstream impact:** None (frontend-only).

## Resolved [VERIFY] log (Phase 0)

- Base repo: libretro/gpsp ahead of davidgfnet — resolved (ADR-0001).
- Makefile PSP target: `platform=psp1` → `gpsp_libretro_psp1.a`, dynarec on — confirmed, built clean 2026-07-31 (pspdev GCC 15.2).
- Desktop build: `platform=unix` → `gpsp_libretro.so` — built clean in WSL.
- Core option keys: `gpsp_serial` (auto/disabled/rfu/mul_poke/mul_aw1/mul_aw2), `gpsp_bios` (auto/builtin/official), `gpsp_frameskip` (+`_threshold`/`_interval`) — SERIAL-PROTO-NOTES §3.
- Netpacket registration: env call 78 in `retro_init`; `poll` is NULL — SERIAL-PROTO-NOTES §1.
- Max RFU payload: 104 B fixed — SERIAL-PROTO-NOTES §2.
- Savestate RFU coverage: **absent** — savestate block during sessions is mandatory (SERIAL-PROTO-NOTES §7, FRONTEND-AUDIT §7).
- Pixel format on PSP: RGB565, 240×160, pitch 480 — FRONTEND-AUDIT §2.
- Audio: s16 stereo batch, 65536 Hz default — FRONTEND-AUDIT §3.
- SRAM dirtiness: no core signal; frontend hashes — FRONTEND-AUDIT §5.
- Flash 128K for FR/LG/E: auto via gba_over.h (BPRE/BPGE/BPEE entries) — no manual config needed — FRONTEND-AUDIT §5.
- PSP-1000 32 MB fit: paper verdict FITS (~19.8 MiB core total, ~3–4 MiB headroom in ~24 MiB user partition); measure at Gate 1 — FRONTEND-AUDIT §8.
- Appendix B crib sheet: corrected in ADHOC-NOTES §8 (productStruct 4th field, no adhocctl event constants in SDK, 8-byte MAC buffer, PdpDelete/DelHandler added). 17 semantics questions deferred to PPSSPP-source cross-check.

## Gate 0 record — reference trade PASSED (2026-07-31)

User ran the RA-PC×2 reference rig (docs/RA-REFERENCE.md): Emerald↔Emerald Union Room trade,
real save (AUSTIN) ↔ clone_sav.py clone (CLONEB). **Worked first try, zero jank** — discovery,
greeting, trade, and post-trade in-game saves all clean; both saves verified intact after
relaunch. Reference configuration: RetroArch 1.22.2 win64, gpsp core nightly dll (2026-07-31,
buildbot `nightly/windows/x86_64/latest`), `gpsp_bios=official` (real BIOS), `gpsp_serial=auto`,
frameskip off, netplay via `--host`/`--connect 127.0.0.1`.

Implications: upstream RFU + netpacket path confirmed working in our hands; clone_sav.py output
is Union-Room-legal (distinct-trainer edge validated); this rig is the standing "us or upstream?"
bisection control (plan §3.2).

Gate 0 status: reference trade ✔, PPSSPP harness bootstrap ✔, docs ✔. Hardware perf baseline:
user opted IN, run still pending (kit ready) — recorded as scheduled, not waived; Phase 1 does
not depend on its numbers.

## User decisions (answered 2026-07-31)

1. **Commit derived fixtures?** (§3.4) — **Scripts only.** Input scripts are committed; `.sav`/savestates stay gitignored and are regenerated by the harness.
2. **Repo hosting** — **Local only for now**; revisit before Phase 6 (ADR-0002 stands).
3. **Phase-0 hardware perf baseline** — **Yes.** User runs the one-time stock-software baseline; kit + checklist in `docs/HW-BASELINE.md`. Results to be recorded there.

## Open questions (need user input, non-blocking)

- None currently. Blocked-on-user *assets*: `gba_bios.bin` + Pokémon ROMs into `testdata/` (§3.4).

## ADR-0009 — Canonical test ROMs and the patched Emerald

- **Context:** User supplied FR (BPRE Rev 1) + LG (BPGE Rev 1) and swapped their play copy of Emerald for an RNG-seed-bugfix patch (retail Emerald always seeds RNG=0 at boot; the patch fixes that).
- **Decision:** Harness canonical ROMs = **retail** Emerald BPEE rev0 (RNG=0 bug retained: deterministic runs, and the verified autopilot address table targets it) + FR/LG **Rev 1** (autopilot tables must use pret rev1 symbols). The patched Emerald lives in testdata/ and the RA rig (user play copy) but is not a harness target.
- **Consequences:** If behavior ever differs patched-vs-retail, the RNG patch is the first known difference to check. RA-rig launchers updated to the patched filename.

## ADR-0010 — netdrv ARQ resize + timing retune from the Gate-3 trade runs

- **Context:** The first impaired trade runs (5 % loss + 30 ms jitter) wedged mid-Union-Room with
  the driver formally "healthy". Instrumented runs (`--trace` RAM sampling + netdrv death/reset
  logging) isolated three distinct causes: (1) **tx-ring overflow** — an active RFU link runs
  ~120 reliable payloads/s in bursts; window 8 with backoff to 480 ms stalls on a lost head until
  the 64-slot ring overflows and payloads are dropped, breaking the RELIABLE contract exactly at
  a one-shot game command (observed `overflow=68`, host and joiner deadlocked in
  WAIT_FOR_RESPONSE/HANDLE_CONTACT); (2) a trial 60 ms first-retx floor broke the RFU
  link-establishment exchange outright — the emulated link layer needs lost frames recovered in
  ~30 ms; (3) spurious keepalive deaths at 4 s while the peer process was alive (loaded-host
  scheduling stalls; also wall-clock time sources are step-prone under WSL/NTP).
- **Decision:** `ND_WINDOW` 8→32, `ND_TXQ_CAP` 64→192, `ND_RETX_US` stays 30 ms (documented as
  load-bearing), `ND_RETX_MAX_US` 480→240 ms, `ND_PEER_DEAD_US` 4→8 s; every frontend time
  source feeding netdrv switched to CLOCK_MONOTONIC; permanent diagnostics added (roster-driven
  ARQ channel-reset log; keepalive-death log with silent-ms + rx counters; ROSTER-source drops
  now counted). Unit-test horizons updated for the 8 s death threshold; full suite green.
- **Alternatives:** Adaptive RTT-based RTO — more machinery than the problem needs at these
  rates; per-payload deadline drop — would violate the RELIABLE contract by design.
- **Consequences:** ~600 KiB per instance (desktop fine; PSP halves the knobs per ADR-0007 if
  needed); duplicate traffic under jitter is accepted noise (`dup` counter, receiver dedups).
  `run_trade_test.sh` asserts `overflow=0` forever after.
- **Upstream impact:** None (netdrv is ours).

## ADR-0011 — First core patch: rfu.c packet queues 4 → 16 (RFU_PKT_QUEUE)

- **Context:** With the driver clean, impaired trades still wedged: pret-side tracing showed the
  joiner's trade request never reached the host's game. rfu.c buffers netpacket-delivered link
  frames in fixed 4-deep per-direction queues (`pkts[4]`) and **silently discards** overflow
  ("Host dropped a client packet" ×407 in one run). Depth 4 assumes an essentially zero-latency
  transport (real radio slot timing = one frame per emulated frame); any transport with real
  latency (ARQ retransmits, jitter — ours, or RetroArch netplay over a WAN) delivers legitimate
  clumps deeper than 4, and gen-3 games never retransmit their one-shot command packets, so a
  dropped clump wedges both games with zero diagnostics. Reproduced deterministically at 10–30 ms
  injected jitter; impossible to hit on the RA-PC reference rig (sub-ms localhost), which is why
  upstream has not seen it (plan §3.2 bisection reasoning applied).
- **Decision:** Minimal patch per plan §4.1: `#define RFU_PKT_QUEUE 16` replacing the hardcoded
  4s (two array sizes, two enqueue loops, two dequeue memmoves; no logic change). ~250 ms of
  clumped deliveries absorbed at the link's steady ~2 frames/frame. Compiles clean for unix and
  PSP (allegrex) targets.
- **Alternatives:** Pacing deliveries in our netpacket bridge to ≤1 frame/emulated-frame —
  keeps the core pristine but throttles below the link's real multi-exchange-per-frame demand
  and just moves the queue somewhere the core can't see; rejected.
- **Consequences:** With ARQ delivery loss-free and the queues no longer lossy, the whole
  "silently vanished game command" wedge class is gone; 5 %+30 ms trades complete.
- **Upstream impact:** **Yes — first local core change.** Small, behavior-preserving at zero
  latency, and directly relevant to upstream netplay-over-WAN; to be offered upstream with the
  Phase-6 RFC (plan §5 Phase 6) alongside the SERIAL-PROTO-NOTES quirk list.

## Gate 3 record — COMPLETE (2026-08-01)

Two desktop twin instances complete an autonomous Emerald↔Emerald **Union Room trade** over the
netdrv UDP driver with **5 % loss + 30 ms jitter injected on both sides** — the plan §5 Phase-3
letter, RetroArch not involved. Evidence:

- **Fixtures** (plan §7.1 step 4): `tools/e2e/make_trade_fixtures.sh` plays both saves (user's
  AUSTIN + regenerated CLONEB) from Route 117 to the Mauville PC 2F wireless counter under
  uncapped FF and in-game-saves there → `testdata/fixtures/emerald_parkedA/B.sav` (gitignored,
  regenerated; scripts committed). Artifacts `tools/e2e/artifacts/parkfix-20260731-184108/`.
- **Trade** — `tools/e2e/run_trade_test.sh` exit 0, artifacts `artifacts/trade-20260801-*/`:
  session up + peer_connected both sides; trading-board registration (slot-2 Magcargo, DRAGON
  requested) by the host; board offer (slot-1 Salamence) by the joiner; CB2_LinkTrade animation;
  the game's own automatic in-game save flushed on both sides; both back on the Union Room map;
  **oracle**: party personalities/species swapped exactly (RAM logs and post-run `.sav` decode:
  host slot 2 == joiner's Salamence `0x44e162bf`/397, joiner slot 1 == host's Magcargo
  `0xd6c3c1f1`/219); driver behavior log clean: `overflow=0 drop_crc=0 drop_mal=0`, acked>0 —
  RELIABLE ordering respected under impairment. Wall-clock ≈ 4 min including fixture check.
- **Disconnect** — `run_trade_test.sh --disconnect` exit 0, artifacts
  `artifacts/tradedisc-20260801-*/`: joiner SIGKILLed 8 s into the trade animation; host
  survives with the game's own dialog ("Communication error… A Button: Registration Counter",
  `CB2_PrintErrorMessage` screenshot) and a clean `EVT exit code=0` — no crash, and the game
  itself offers wireless re-entry.
- **Regressions:** netdrv unit suite green after ADR-0010; `run_netsmoke.sh` re-PASS; core
  builds clean for unix and PSP targets with ADR-0011.
- Address-table and script-engine additions: docs/AUTOPILOT.md (Gate-3 sections); harness
  recipe: docs/TESTING.md (Gate-3 trade e2e).

Not claimed by this gate: PSP transport (Phase 4), PPSSPP two-instance trade (Gate 4-E).

## ADR-0012 — PSP ad-hoc transport: static singleton, halved ARQ knobs, autopilot-only surface

- **Context:** Phase-4 `transport_adhoc.c` (plan Appendix B, semantics per ADHOC-NOTES §8/§11).
  Three sizing/surface decisions were open: netdrv's ~600 KiB ADR-0010 footprint vs the ~512 KiB
  post-boot max contiguous block (ADR-0007 measurement), transport state placement, and how the
  frontend exposes wireless before the UI panel exists.
- **Decisions:**
  1. **PSP halves the ARQ knobs** via the new `#ifndef` overrides: `-DND_WINDOW=16
     -DND_TXQ_CAP=96` (psp/Makefile) → ≈ 300 KiB netdrv instance, allocated at session start.
     Desktop keeps the Gate-3 sizing. Window 16 at ~1–2 ms ad-hoc RTTs still clears the core's
     ~120 payload/s demand by an order of magnitude; the load-bearing 30 ms retx floor is untouched.
  2. **Transport is a static .bss singleton** (ring + MFS-sized RX scratch ≈ 38 KiB; no heap):
     PSP runs exactly one session, and .bss cannot fail at session start the way a large malloc can.
  3. **Surface is autopilot.ini-driven** (`host=1`/`join=1`/`nick`/`group`, `nettest=1` echo mode)
     — mirrors the desktop twin's `--host/--join` flags so the harness drives both rigs
     identically; the Phase-4 UI panel builds on the same `net_bringup()`/`net_teardown()` calls.
  4. **RX robustness details:** blocking PdpRecv in 250 ms timeout slices (teardown never hangs);
     recv at MFS capacity 1444 so `NOT_ENOUGH_SPACE` (which leaves the datagram queued, §11.7)
     is unreachable for any legal datagram; WOULD_BLOCK on the nonblocking send path counted as
     loss, not error (ARQ owns reliability).
  5. **Link rule:** `-lpspnet`/`-lpsputility` must NOT be named in psp/Makefile LIBS — build.mak's
     tail and gcc's spec closure (libcglue → sceUtilityGetSystemParamInt) link them late; naming
     them earlier splits those modules' import-stub blocks across two archive scans and
     psp-fixup-imports fails "stubs out of order" (= broken imports on real hardware). Found by
     map-file bisection; documented in the Makefile.
- **Alternatives:** static-instance shim inside netdrv (touches shared code for a PSP-only
  concern); PTP streams (rejected in plan Appendix B); UI-first wiring (blocks the gate on
  another workstream).
- **Consequences:** the PSP and desktop drivers differ only in two compile-time constants;
  Gate-5C soak must re-validate the halved window under 3+ peers. Real-hardware teardown order
  strictness (ADHOC-NOTES §10.12) remains a Gate-4H observable.
- **Amendment (Gate-4E disconnect finding):** the first PSP `--disconnect` run failed the
  harness's `overflow=0` assert with overflow=46 on the survivor: the game keeps sending
  RELIABLE frames to the SIGKILLed peer for seconds, the window head can never be acked, and
  the halved 96-slot ring fills before the game's own link timeout (desktop's 192 slots happened
  to absorb the same burst at Gate 3). Since drops toward an unreachable peer are inevitable at
  ANY finite depth and void once keepalive death discards the queue, netdrv now classifies a
  ring-full drop toward a peer silent > 2× keepalive as the new `tx_drop_dead` stat instead of
  `tx_overflow`; `overflow` keeps its strict live-peer meaning and every harness keeps asserting
  it to 0. Alternative rejected: relaxing the harness assert in disconnect mode — that would
  also hide genuine live-phase overflows.
- **Upstream impact:** None (netdrv is ours).

## Gate 4-E record — Emerald↔Emerald leg COMPLETE (2026-08-01)

The PSP transport + the Gate-4E Emerald trade legs, all agent-autonomous on the PPSSPP
two-instance rig (TESTING.md recipe; built-in AdhocServer on inst1), all exit 0:

- **Transport echo** (`run_nettest.sh`, bring-up step a): full sceNetAdhoc bring-up on both
  instances (adhocctl group GPSP07, PdpCreate 0x4A4B, RX thread), 30/30 ping/pong round trips
  both directions, distinct MACs (02:00:00:00:00:01 / 00:02:02:02:02:02), ctlerr=0, clean
  exits. Artifacts `tools/e2e/artifacts/nettest-20260801-110223/`. Flips TESTING §8's last
  "Assumed" row (two local instances complete adhocctl create/join + PDP send/recv) to Verified.
- **Trade** (`run_trade_test_psp.sh`): autonomous Union Room board trade between two PPSSPP
  instances running the EBOOT — same fixtures/scripts/oracle as Gate 3. Oracle held on RAM and
  on the post-run `.sav` both sides (host slot 2 ← 0x44e162bf/397, join slot 1 ← 0xd6c3c1f1/219);
  driver health under the PDP transport: `overflow=0 drop_crc=0 drop_mal=0`, acked>0, adhoc
  layer clean (`ringdrop=0 txfail=0 rxerr=0 ctlerr=0`; ~7.4k datagrams host-tx). Memory:
  free partition 748 KiB → 588 KiB (max block 384 KiB) after wireless bring-up — the 160 KiB
  delta is the sceNet pool + net/RX thread stacks; the ≈300 KiB halved-knob netdrv instance
  lives inside the app heap. Artifacts `artifacts/psptrade-20260801-110411/`.
- **Disconnect** (`--disconnect`): join PPSSPP process SIGKILLed 8 s into the trade animation;
  host survived with the game's own comm-error dialog (screenshot), `disc_done`, clean exit,
  `overflow=0` with the dying-link drops classified `drop_dead=47` (see amendment below; the
  first run failed exactly there and drove the fix). Artifacts
  `artifacts/psptradedisc-20260801-113746/` (first-run failure preserved in `-113124/`).
- **Regressions:** netdrv unit suite green (twice: after knob overrides, after drop_dead);
  desktop trade + boot test re-run green (see harness artifacts of the same date).

Still open for the full Gate-4E letter: the FireRed↔LeafGreen trade leg (needs FR/LG rev-1
autopilot address tables — separate task), save-across-restart re-verification on the PSP rig,
and re-hosting without app restart as an explicit scripted assert (the game's own counter
re-entry path is exercised, an end-to-end re-host script is not). Hardware remains Gate 4-H.

## ADR-0013 — Silent-wireless policy + second core patch (RFU activation hook)

- **Context:** Per-game "invisible" variant builds (user direction 2026-08-01; docs/VARIANTS.md)
  must bring wireless up with zero emulator UI, at the moment the game itself starts using the
  adapter. The core exposes no RFU-activation signal to the frontend.
- **Decision:** (1) Second core patch (after ADR-0011): `rfu.c` fires a **weak no-op hook**
  `gpsp_rfu_activated_hook()` when a game completes the adapter login handshake
  (`0xB0BB8001` → WAITCMD) — 8 lines, zero behavior change for libretro builds; the PSP frontend
  overrides it. (2) Policy on hook: back up the save, bring the transport up on the variant's
  fixed group, then **join-first / promote-to-host** on a jittered timeout (240 + clock%120
  frames) for `role=auto`; `role=host/join` pins. Promotion restarts only the netpacket layer
  (`fe_np_stop` + start-as-host; core stop/start is part of the netpacket contract) — the
  adhocctl group stays up. WLAN-off → persistent OSD warning, single attempt, no retry storm.
- **Alternatives:** polling core RAM for game-specific link state (per-ROM tables, violates
  game-agnostic frontend); always-on session from boot (hosts a radio session for solo players);
  scan-based host detection (real-hw scan only sees a BSS after a host exists → same race, more
  blocking time).
- **Consequences:** simultaneous auto-role activations inside one jitter window can double-host
  (benign: game finds no partner, retry re-enters policy); documented in VARIANTS.md. Validated
  by `run_silent_trade.sh` (promotion leg + pinned-join leg + full Gate-4E trade oracle).
- **Upstream impact:** hook is small and generic (any frontend wanting an RFU-activity signal);
  offer with the ADR-0011 patch in the Phase-6 RFC.

## ADR-0014 — Phase-2 UI decisions (deviations & choices within plan §8)

- **Menu chord:** Select+Start held 15 frames (CFW-safe, no HOME hooking); disabled while an
  autopilot script drives (scripts own the pad).
- **Frontend keys:** Triangle = video-preset cycle, Square = fast-forward. The core's turbo-A/B
  mappings on these buttons are dropped (GBA has no free buttons; turbo was a RetroArch-ism).
- **FF frameskip:** during FF the frontend sets `gpsp_frameskip=fixed_interval, interval=1`
  live (new `fe_host_option_set_live` + `GET_VARIABLE_UPDATE`). The plan's "auto frameskip"
  needs the audio-buffer-status callback, whose occupancy signal is meaningless under FF's
  muted/drained audio — fixed_interval is the deterministic equivalent. Hw ceiling data
  (uncapped ≈1.1–2.3×) says 2× needs it.
- **Join panel:** scan list via new `adhoc_transport_scan()` (adhocctl init-to-handler, SCAN
  event, GetScanInfo, full teardown — blocking ≤10 s with a "Scanning…" screen) plus manual
  room-code entry as a GPSP**00–99** selector instead of a free-text keyboard (v1 scope;
  full text entry deferred with nickname editing to a later phase — nickname remains
  config.ini-editable).
- **Savestates:** one slot (`<rom>.st0`), blocked during sessions per plan §4.5 with a toast;
  416 KiB staging is static .bss (never fights the core's greedy ROM heap, ADR-0007).
- **Save backup:** every session start copies `<save>.sav` → `.sav.bak` (single rotation v1;
  plan §4.5's ×3 rotation deferred).
- **Colors/font:** dark theme per §8; bundled font = Linux kernel VGA 8x16 (GPL-2.0, same
  license as the project; byte-identical import, `psp/font_8x16.c`).
- **Generic-build XMB branding:** title exactly **"PSP AGB"** (AGB-015 adapter homage),
  ICON0/PIC1 derived from user-supplied `psp/assets/xmb_source.png` by
  `psp/assets/make_assets.py` (icon letterboxes the full device; PIC1 fill-crops). Verified:
  PARAM.SFO TITLE + byte-identical ICON0/PIC1 via unpack-pbp, tile rendered in PPSSPP's
  game list (artifacts/xmb_evidence.png).

## Phase-2 record — GU color bug (hw finding a) fixed with a regression-proof check (2026-08-01)

Real-hardware baseline showed the GU blit R/B-swapped on the real GE: libretro RGB565
(R bits 11-15) was uploaded directly as GU_PSM_5650, which is PSP channel order (R bits 0-4).
PPSSPP masked it on screen (its texture-decode/framebuffer-encode round-trip is
self-consistent) and the BMP dumps bypass the GU path entirely — nothing on the old harness
could see it. Fix: RGB565→PSP-5650 field-swap into a staging buffer inside the blit
(2 px/word; ~0.3 ms/frame at 333 MHz). Verification designed to outlive us:
`gedump_at`/`testpat` read back the **GE drawbuffer** (raw VRAM decoded with the real 5650
layout) and `run_gu_color_test.sh` asserts 8 known color bars through the production blit
path AND pixel-identity of in-game GE output vs the core-buffer BMP, under PPSSPP **softgpu**
(the only renderer whose VRAM readback is real). Sensitivity proven: the pre-fix code fails
the test with the exact hardware symptom.

## ADR-0015 — v0.1.0 scope calls: no clock setting, suspend/resume deferred, no FPS counter

- **Context:** Writing the Gate 4-H acceptance checklist surfaced three items the plan
  implied but the code does not have: a CPU-clock item in Settings (plan §8), a
  suspend/resume power-callback handler (plan §9), and an on-screen FPS counter.
  User ruled on all three directly (2026-08-01).
- **Decision:**
  1. **No CPU-clock setting in Settings — permanently out of scope.** User: "these PSPs
     will probably never run below 333 MHz ever again." The frontend keeps asserting
     333/333/166 at init and at session start (ADR-0004); the plan §8 Settings item and
     the §9 "266/222 fallback for flaky units" insurance are both dropped. Do not re-add
     without new evidence that a specific unit is unstable at 333 with WLAN active.
  2. **Suspend/resume power-callback handler deferred to a post-v0.1.0 update**, not a
     release blocker. Today the frontend calls `scePowerTick` during sessions and flushes
     SRAM on the exit callback; sleeping mid-session is untested and may drop the link
     ungracefully. Acceptance checklist step 11 stays exploratory with no pass criterion.
  3. **No on-screen FPS counter.** The `EVT heartbeat frames=N t_us=...` ladder gives
     more precise numbers from the log (that is how the hardware baseline was measured),
     and the OSD stays uncluttered.
- **Consequences:** v0.1.0 ships without these; CHANGELOG known-issues should mention the
  suspend/resume gap honestly. Removes two UI surfaces from the release-polish workload.
- **Upstream impact:** None.

## ADR-0016 — RELIABLE payloads are never dropped: backpressure + spill, and a right-sized ad-hoc slot

- **Context:** Field logs from two real PSPs (2026-08-01, docs/HANDOFF.md issue #2) contain the
  kill shot `LOG netdrv: arq overflow peer=0 (payload lost)` x2 on the client. Per ADR-0003 every
  packet the core sends is `RELIABLE|FLUSH_HINT`, and the gen-3 games never retransmit their
  one-shot RFU commands. Dropping one silently destroys the RFU state machine; both consoles then
  sat with `core_tx`/`core_rx` frozen while our transport kept exchanging keepalives — "the link
  didn't drop; we broke the guarantee the core depends on." The old `arq_enqueue` discarded the
  payload, bumped `tx_overflow` and returned -1 into a `void` core callback that cannot react.
- **Decision:**
  1. **A full ring toward a LIVE peer is backpressure, not loss.** Overflowing payloads go to a
     per-peer **spill FIFO** (one malloc per payload, sized to the payload, order preserved) and
     are pulled back into the ring as it drains. `netdrv_send` returns 0 — nothing is lost.
  2. **If the backlog is genuinely unabsorbable** (spill cap `ND_SPILL_MAX` = 512/peer, or OOM)
     the driver **fails the session loudly**: `tx_overflow++`, a netdrv log line, and a deferred
     `session_stopped(ND_STOP_TX_FAILED)` fired from the next `netdrv_pump` — never under the
     core's `send_fn` stack. The frontend emits `EVT net_error reason=txq_overflow ...`, toasts
     "Wireless error: link overloaded" and tears down. A visible failure beats a corrupted link.
  3. **Oversize payloads are refused, never truncated:** `len > ND_MAX_PAYLOAD` counts
     `tx_oversize`, logs which budget was exceeded and (for RELIABLE) raises the same explicit
     failure. A compile-time assert additionally refuses any build whose payload budget cannot
     hold the 138 B roster / 44 B JOIN / 104 B RFU max.
  4. **Right-size the ad-hoc slot to pay for depth.** ADR-0003 sized frames at 560 B for the
     link-CABLE Advance Wars protocol (~520 B, SERIAL-PROTO-NOTES 2.3), which is explicitly out of
     scope. The PSP profile compiles `-DND_MAX_PAYLOAD=144` (RFU max 104, roster 138), which buys
     `ND_TXQ_CAP` 96 -> **384 (4x)** and `ND_WINDOW` 16 -> 32 at **neutral memory**: the netdrv
     instance goes ~322 -> ~347 KiB while the transport's .bss RX ring drops ~37 -> ~11 KiB.
     Desktop/UDP keeps 560 B so the cable protocols stay representable there.
- **Alternatives:** *Block inside `send_fn`* — impossible without re-entering the pump from under
  the core's stack (the core sends from inside `receive`, SERIAL-PROTO-NOTES 6): deadlock risk for
  no gain over spilling. *Grow `ND_TXQ_CAP` alone* — does not remove the cliff, only moves it, and
  costs PSP memory 1:1. *Keep dropping but log louder* — the drop itself is the bug.
- **Consequences:** The Gate-4E `--disconnect` classification is preserved untouched: a full ring
  toward a peer silent > 2x keepalive is still `tx_drop_dead` and does **not** spill (those drops
  are inevitable at any depth and void once keepalive death discards the queue — ADR-0012
  amendment). `tx_overflow` keeps its strict meaning and every harness still asserts it 0, but it
  can now only occur alongside an explicit session failure. New stats: `tx_spill`, `tx_oversize`,
  `txq_hiwater`. Unit tests `overload` (zero loss under sustained overload at 40 ms RTT) and
  `txfail` (forced-unrecoverable => explicit `ND_STOP_TX_FAILED`) are the regression wall.
- **Upstream impact:** None (netdrv is ours).

## ADR-0017 — Adaptive per-peer RTO with transport-level floors (supersedes ADR-0010's fixed 30 ms)

- **Context:** ADR-0010 froze `ND_RETX_US` at 30 ms because the emulated RFU link-establishment
  exchange died when a lost frame took >= 60 ms to recover — measured on **loopback**, where RTT
  is microseconds. On real ad-hoc radio RTT is 40-80 ms, so the 30 ms timer fires *before an ACK
  can physically arrive*. The field logs are unambiguous: host `retx/acked ~ 2.8`, `dup/rx ~ 68 %`
  and `txfail=0 rxerr=0 drop_crc=0` — **the radio lost nothing**; we generated the entire storm
  ourselves, and the duplicate traffic inflated latency until the tx queue overflowed.
- **Decision:** Per-peer TCP-style estimator (RFC 6298 shape): `SRTT`/`RTTVAR` EWMA,
  `RTO = SRTT + 4*RTTVAR`, **Karn's algorithm** (never sample an ack for a retransmitted slot) and
  exponential backoff **retained at peer level**, rate-limited to one doubling per RTO interval.
  The retained backoff is load-bearing: with a floor below the true RTT, *every* slot's first
  transmission times out, so *every* ack is Karn-ambiguous and a per-slot backoff can never escape
  (observed directly — the estimator sat at 0 samples until the backoff was made peer-persistent).
  Floor/ceiling are **transport constants**: desktop/UDP keeps 30 ms / 240 ms (loopback SRTT ~1 ms,
  so the floor governs and Gate-3 timing is bit-for-bit unchanged); PSP/ad-hoc uses
  **100 ms / 800 ms** (psp/Makefile). A received DATA frame is acked in the **same pump tick** — as
  a bare ACK immediately after the transport drain when we have nothing due to piggyback on — so
  ack latency (which is part of the peer's RTT sample) never waits on the retx/keepalive scan or
  on an early return from it.
- **Amendment (found by the Gate-3 desktop regression, same session): pacing and loss RECOVERY
  are two different deadlines.** The first adaptive build passed every PSP profile but failed
  `run_trade_test.sh` — reproducibly, standalone — with the trade stalling at host `contacted` /
  join `at_board`. Cause: on that rig (5 % injected loss + 30 ms jitter each way) the honest SRTT
  is ~68 ms, so RTO settles at 100-170 ms, and a lost frame now takes that long to come back.
  ADR-0010's "~60 ms cliff" is real, and it is a statement about *recovery* latency. The old
  fixed 30 ms timer satisfied it by retransmitting **before an ACK could possibly arrive** — that
  is blind redundancy, not ARQ, and it is exactly what made the same code storm on real radio.
  Both facts are true at once. So the deadline is split: `ND_RTO_FIRST_MAX_US` caps a slot's
  FIRST retransmission (loss recovery); the adaptive RTO governs everything after it (pacing,
  backoff). Declared per transport, like the floors: **desktop/UDP 30 ms** (sdl/Makefile —
  restores Gate-3 behaviour exactly on a rig whose injected loss has no layer beneath it to
  repair anything), **PSP/ad-hoc unset** (802.11 retransmits below us — the field logs show
  `txfail=0 drop_crc=0`, i.e. the radio lost nothing — so the estimator governs and no duplicate
  storm is generated). Fast retransmit on duplicate ACKs was implemented and **rejected**: our
  receiver re-acks on jitter-induced reordering too, so on a lossless 40 ms link it fired
  constantly and pushed retx/acked back from 0 % to 47 % — reintroducing the storm to solve a
  problem only the lossy desktop rig has.
- **Verification (the number that matters):** unit test `rto` runs RFU-shaped traffic (2
  payloads/frame each way at 60 fps) on a lossless 40 ms-RTT link. Fixed 30 ms floor:
  **retx/acked = 100 %, dup/rx = 27 %, RTT samples = 0**. Adaptive: **retx/acked = 0 %,
  dup/rx = 0 %, SRTT ~ 46 ms, RTO ~ 55 ms, 1600+ Karn-valid samples.** On the PPSSPP rig with the
  new latency-injection profile a full Emerald trade went **retx/acked 148 % -> 5 % at 40 ms RTT**
  and **202 % -> 8 % at 80 ms** (artifacts in tools/e2e/artifacts/psptrader{40,80}-*).
- **Alternatives:** *Raise the fixed floor to 100 ms everywhere* — punishes desktop and still
  guesses; the lesson of this bug is that one constant cannot serve both media. *Wire timestamps
  for exact RTT* — needs a wire-format version bump for accuracy the EWMA already delivers.
  *Per-slot backoff only* — demonstrably deadlocks under Karn (see above).
- **Consequences:** ADR-0010's "the 30 ms floor is load-bearing" is now scoped to the desktop/UDP
  transport, where it is unchanged. The RFU link-establishment concern is preserved on PPSSPP
  (loss-free localhost => retransmits are ~nil, so the floor never engages) and on real radio the
  honest estimate is what the link actually needs. `EVT net_stats` now carries `srtt_us`, `rto_us`,
  `retx_pct`, `txq_hi` and `spill`, so any future field log self-diagnoses this class instantly.
- **Upstream impact:** None (netdrv is ours).

## ADR-0018 — Core auto-frameskip while `net=up` (the frontend now answers the audio-buffer-status env)

- **Context:** Fix-plan item 6 (docs/HANDOFF.md): the PSP-1000 was the laggier console, generated
  ~1.6x the traffic and was the side that overflowed. A console that cannot hold 59.7 fps also
  pumps the ARQ less often and stretches every emulated-link timeout. `gpsp_frameskip=auto` exists
  in the core but needs `RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK`, which
  FRONTEND-AUDIT 1 had us declining (the core then logs "Frameskip disabled" and never skips).
- **Decision:** `fe_host` accepts the env call and stores the core's callback;
  `fe_host_audio_buffer_status(active, occupancy_pct, underrun_likely)` is fed once per main-loop
  iteration from the PSP audio ring (`underrun_likely` below 25 % fill — the ring is pace-nudged to
  ~75 %, so this only trips when genuinely behind). `net_bringup()` sets `gpsp_frameskip=auto`;
  `net_teardown()` restores `disabled`; `active` is false while FF mutes audio, so the existing FF
  policy (`fixed_interval`) and the plan 4.4 FF-during-session interlock are untouched. Also on the
  cheap-overhead line: netdrv's CRC16 moved from bit-serial to a 256-entry table (~8x cheaper per
  frame, 512 B of .rodata, bit-identical output) — every frame is CRC'd twice, on build and parse.
- **Alternatives:** `fixed_interval` while net=up — skips unconditionally even on a console that is
  keeping pace. Frontend-side deadline skipping — duplicates logic the core already has.
- **Consequences:** Only video frames are skipped; `retro_run` count, autopilot frame predicates and
  the netpacket cadence are unaffected. Desktop is unchanged in practice (it never sets auto).
- **Upstream impact:** None.

## ADR-0019 — A wireless session no longer blanket-enables frameskip (supersedes ADR-0018's policy)

- **Context:** ADR-0018 set `gpsp_frameskip=auto` for the whole session on the theory that a
  console failing to hold 59.7 fps stretches every emulated-link timeout and pumps the ARQ less
  often. The first *successful* field session (2026-08-01, two PSPs, real radio, Emerald<->Emerald
  Union Room trade completed) disproved the premise on **both** consoles: the heartbeat ladder
  held **58.9 fps emulated, sustained, start to finish** — Δt ≈ 10.18 s per 600 frames, identical
  to solo play. Emulation was already real-time; `auto` was discarding *rendered* frames for no
  gain, and the user reported exactly that as stutter (worse on the PSP-1000). Two mechanisms made
  it worse than it sounds: (a) `auto` skips whenever `audio_buff_underrun` is true, which our
  `AUDIO_UNDERRUN_PCT`=25 hook reports on a transient ring dip that a vblank-locked loop produces
  with plenty of CPU headroom (psp/main_psp.c `audio_status_frame`); (b) the main loop still waits
  for vblank after the skip (`sceDisplayWaitVblankStart`), so a skipped frame buys back **zero**
  emulation throughput — it is pure loss. And none of it was visible, because the only frame number
  we logged was `retro_run` count.
- **Decision:** **Measure first.** `fe_host` counts rendered frames (`video_refresh` with pixels)
  against skipped ones (the core's `data==NULL` present, libretro/libretro.c:460) and emits
  `EVT fps emu=A.AA rendered=B.BB skipped=N win=F dt_us=D` on the existing heartbeat cadence;
  `fe_host_frame_stats()` exposes the same totals. **Read `rendered` precisely:** it counts the
  frames the *core* handed the frontend, so `emu - rendered` is core frameskip and nothing else.
  During uncapped fast-forward the PSP frontend additionally drops 31 of every 32 *presents*
  (`FF_PRESENT_MASK`, psp/main_psp.c) without the core skipping anything, so an FF run correctly
  reads `rendered == emu` at 170 fps. That is exactly the case this counter is not about — during
  a wireless session FF is interlocked to 1x, so the two numbers coincide with what reaches the
  screen, which is the case it exists for. Then **change the policy**: config.ini
  `net_frameskip` — `0 off` (**default**: a session never touches frameskip), `1 adaptive`
  (start disabled; engage `auto_threshold` only after **3 consecutive 1 s windows below 57.00 fps
  emulated**, release after **5 windows above 58.50** — hysteresis with a dead band, not `auto`'s
  per-frame twitch; `gpsp_frameskip_threshold` from `net_skip_threshold`, default 25), `2 auto`
  (ADR-0018 verbatim, retained so the user can A/B the two on hardware in one sitting).
  `autopilot.ini net_frameskip` pins the mode for the harness. Transitions log
  `EVT skip_policy` / `skip_engage` / `skip_release`. Default-off is justified by measured
  headroom: the solo uncapped ceiling is **67-137 fps (1.12-2.29x)** (docs/HW-BASELINE.md).
- **Alternatives:** *Keep `auto`, raise the underrun threshold* — still per-frame, still trips on a
  dip the loop then does nothing with. *Frontend-side deadline skipping* — duplicates core logic
  and would need the same hysteresis anyway. *Remove the audio-status hook entirely* — throws away
  the mechanism the adaptive mode needs, and the hook is free when no frameskip mode is active
  (the core deinstalls the callback, so `fe_host_audio_buffer_status` early-returns).
- **Consequences:** The FF interlock (plan §4.4) is unchanged, except that releasing FF now hands
  frameskip back to the *policy* rather than unconditionally to `auto`. ADR-0018's mechanism
  (frontend answers `SET_AUDIO_BUFFER_STATUS_CALLBACK`) stands; only its blanket application is
  withdrawn. Also added here: `gpsp_rfu_link_down_hook()`, the symmetric partner of ADR-0013's
  activated-hook, fired when the emulated RFU link ends locally / by peer DISCONNECT / by host-side
  client timeout, surfaced as `EVT rfu_link_down reason=N slot=N net=up|down`. The field could not
  tell "the game ended the wireless session" from "we tore the transport down"; now it can.
- **Upstream impact:** The rfu.c hook is a weak no-op by default (same pattern already accepted for
  the activated-hook). Two receive-path bounds checks in rfu.c are genuinely upstreamable:
  `NET_RFU_CLIENT_SEND` read its length from a header byte (0-255) straight into a 16-byte buffer
  with no `len >= blen + 12` guard — unlike `NET_RFU_HOST_SEND` immediately above it, which has
  both — and `NET_RFU_BROADCAST` read 6 payload words behind a `len >= 12` gate. Legitimate senders
  cap both, so no working behaviour changes; hardening, not a diagnosed fault.

## ADR-0020 — The emulation thread is never stalled by a save write (dirty blocks + held handle + budgeted drain)

- **Context:** The same two field logs carry the *real* lag. Computing fps per heartbeat window,
  the windows containing an `EVT sram_flush` are the slow ones and nothing else is:
  the PSP-1000 client runs **56.5-57.6 fps** normally and **50.65 / 50.38 fps** in its two flush
  windows (Δt 11.84 s and 11.91 s against a normal ≈10.6 s); the PSP-3000 host runs **58.96** and
  drops to **56.49 / 55.20**. That is ~0.5-1.3 s of *frozen frame loop* per flush. Cause:
  `fe_host_sram_flush` rewrote all **131,072 bytes** to the memory stick, synchronously, on the
  emulation thread, whenever the whole-image CRC moved — and a trade makes the game write its save
  repeatedly, so the 5 s dirty check (`SRAM_CHECK_INTERVAL`) kept re-triggering it.

  **And it is very probably not only lag.** The user ran the RetroArch-PC×2 control (stock gpsp
  core, RA netplay, localhost): Union Room entered, a lap walked, **host out first, then the
  client — both left gracefully and saved, no error**. So the fatal wireless screen both PSPs
  showed on room exit is **not** upstream behaviour, not a game rule about the host leaving, and
  not the core's RFU being latency-intolerant. The one thing our environment does that a PC does
  not is *stop running the emulator for about a second while it writes 128 KiB to a memory stick* —
  and leaving the Union Room is exactly when the game saves. A peer that stops answering a
  teardown handshake for ~1 s is indistinguishable from a dead one. That also explains why the
  transport logs are spotless (`overflow=0`, zero loss, counters mirrored): we never dropped
  anything, we just stopped servicing the link at the worst possible moment.
- **Decision:** The requirement is not "write less", it is **never stall the emulation thread for
  long, at any point, on any hardware**. Three things together (frontend-common/fe_host.c):
  1. **Dirty 4 KiB blocks only.** 32 blocks, a CRC each; 4 KiB is the GBA flash sector gen-3
     erases and writes one at a time, so a mid-save check finds one or two. The whole-image CRC
     still gates whether we write at all, and `fe_crc32` chains — so its value is **bit-identical**
     to the old single-shot call and every existing log line and oracle comparison still matches.
  2. **The `.sav` is held OPEN (`"r+b"`) for the run.** Re-opening per flush pays FAT/directory
     cost that would dominate once writes are small, and under (3) it would be paid per *frame*.
  3. **Writes drain under a wall-clock budget** (`SRAM_FLUSH_BUDGET_US` = 3 ms, ~18 % of a frame),
     a few blocks per frame from `fe_host_sram_service()` in `fe_host_run_frame`, continuing on
     following frames until the dirty set is empty. One block may overshoot the budget; nothing
     else can. This is self-limiting on hardware whose write throughput we do not know **and
     cannot measure from the PPSSPP rig, whose ms0 is a host filesystem**.

  Forced flushes (`fe_host_sram_flush(1)`, exit and menu paths) drain to **completion** before
  returning — the smoothness work must not weaken the save guarantee. Blocks are CRC'd from the
  bytes actually written rather than from the scan snapshot, so a block the game touches mid-drain
  is simply seen as dirty again by the next scan. Any seek/write/flush failure abandons deltas and
  the next scan does one full rewrite. `EVT sram_flush` carries `wrote=`, `blocks=n/32`,
  `mode=full|delta` and **`ms=`**, so the stall is measured in every future log rather than
  inferred from heartbeat arithmetic — and so the next hardware session can *prove* it is gone.
- **Alternatives:** *Defer all writes to session end* — bounded risk (we take a `sav_backup`
  before a session) but it trades save safety for smoothness, which is the one trade this project
  does not make. *Lengthen `SRAM_CHECK_INTERVAL` while `net=up`* — makes the stall rarer, not
  smaller, and widens the loss window. *Move the write to its own thread* — the real end state,
  but it needs a snapshot buffer and a save-in-flight state to reason about at exit and suspend;
  the budgeted drain gets the same bound with none of that, and `ms=` will say if it is not enough.
  *Dirty blocks alone* (the first cut of this ADR) — measured 118,784 bytes instead of 393,216
  across a rig trade (−70 %), but a genuine 14-sector game save still wrote 57,344 bytes in one
  frame, which on hardware is still hundreds of ms. Necessary, not sufficient.
- **Consequences:** Same dirty check, same flush points, same flush-on-exit and flush-on-menu
  guarantees. Strictly *safer* on power loss than before: `"wb"` truncated the save before
  rewriting it, an in-place block update never does. Desktop and PSP share the change (fe_host);
  the desktop twin's forced exit flush is unchanged in behaviour. **If the room-exit fatal screen
  survives this build on hardware, the stall hypothesis is dead** and the next suspect is the RFU
  teardown handshake's deadline against our measured ~51 ms RTT (rfu.c `RFU_DEF_TIMEOUT` = 32
  frames / `rfu_resp_timeout`) — deliberately not patched here, because guessing twice is how the
  ARQ storm survived Phase 4.
- **Upstream impact:** None (frontend-common is ours).

## ADR-0021 — A live session's per-frame cost is measured, not guessed (and the sends come off the emulation thread)

- **Context:** The user measured, on real hardware (PSP-1000, ARK-4, 333 MHz, shipping
  build), the number this ADR exists for: **solo, wireless entirely off,
  `EVT fps emu=57.65 / 58.66 / 58.64 / 57.64`** — Emerald at ~98 % of the 59.7275 target
  with every frame rendered. Bring a session up with frameskip OFF and `emu` collapses to
  **34-47 fps**. Turn frameskip on hard enough to skip 97 % of rendered frames
  (`rendered=1.9`) and it only reaches **50-57**. **So it was never about drawing** — and
  the PSP-3000 holds ~58.9 fps in the same sessions. Session traffic is modest: ~65 core
  packets/s each way (`tx≈16k rx≈14k` over 227 s), `overflow=0 txfail=0 ringdrop=0`.
  A live session costs the PSP-1000 about **20 fps — a third of its frame budget** — and
  **no log line in the product could say where it went.** ADR-0020 removed the save stall;
  this is what is left.

  **Why a few milliseconds reads as twenty frames per second.** The main loop is
  vblank-locked (`sceDisplayWaitVblankStart`), so frame time is quantised to 16.68 ms. A
  console solo at 57.6 fps is already spending most of a vblank period; add ~2-4 ms of
  session work and a large fraction of frames cross the boundary and cost a *whole* extra
  period. That mechanism turns a small per-frame cost into a cliff, and it is also why the
  PSP-3000 — which starts with more headroom — barely notices the same work. So the hunt
  for "the 20 fps" is a hunt for **milliseconds**, and milliseconds are exactly what
  nothing was measuring.

- **Decision — measure first, in the product, cheaply.**
  `EVT sess_cost` is emitted once per `adhoc_stats` window (600 frames, ~10 s) while a
  session is up, plus once at teardown. Everything is a per-frame average in microseconds
  unless noted, integers only (newlib-nano prints `%f` as nothing on PSP):

  `win clk_ns frame=avg/max pump=avg/max poll=n/work/avg/max rx=avg/max arq=avg/max
   pdp=calls/mean/max enq=mean/max txq=queued/inline/ringfull evt=lines/us/max txthr=N`

  - **`clk_ns`** is the measured cost of one `sceKernelGetSystemTimeWide()` on *this*
    console, timed over 256 calls at session start. The instrumentation has to be
    auditable, so the log carries the constant that prices it: the shipping build makes
    ~13 clock reads per frame, i.e. `13 × clk_ns` of self-cost, and every count below can
    be converted to time by whoever reads the log.
  - **`frame`** is one main-loop iteration with the vblank wait EXCLUDED — the number the
    16.68 ms budget is actually against, per the quantisation argument above.
  - **`pump` / `poll` / `rx` / `arq`** split the driver: the once-per-frame pump, the
    core's mid-frame `poll_receive` pumps, the transport drain + parse + deliver-into-core,
    and the ARQ timer/retransmit/keepalive/roster scan. `rx` and `arq` cover both kinds of
    pump, so they roughly sum to `pump`+`poll`; the nesting is documented rather than
    subtracted out.
  - **`pdp`** is `sceNetAdhocPdpSend`: calls in the window, then **mean and max per
    call**. This is the one cost the PPSSPP rig structurally cannot tell us — there it is
    a host UDP `sendto`, on hardware it is a WLAN driver round trip.
  - **`enq`** is what the *emulation thread* paid per send, `txq` how the sends were
    routed, `evt` the EVT/LOG lines written and the microseconds spent inside
    `fprintf`+`fflush` to the memory stick.

  Cost of the instrumentation itself: 2 clock reads per frame (the `frame` bracket), 2 per
  `fe_np_pump`, 3 per netdrv pump (`prof_mark` closes one section and opens the next off a
  single read), 2 per poll that did work, 2 per send. `nd_config.prof_us` NULL and
  `fe_evt_set_clock(NULL)` disable all of it at zero cost, which is the desktop default.

- **Decision — three changes, in descending order of certainty.**

  1. **The core's mid-frame poll is gated (`netdrv_poll_needed`).** `rfu_update()` calls
     `netpacket_poll_receive()` from `update_serial()` — on every video event while the
     RFU sits in `WAITEVENT` (rfu.c:936-950, main.c:143) — and every one of those ran a
     full pump: a monotonic-clock syscall, a transport probe, a per-peer ack scan and a
     per-peer ARQ window scan (a `% ND_TXQ_CAP` integer division per in-flight slot, on a
     CPU with no hardware divider). The gate is syscall-free: it says yes only when the
     transport reports a datagram waiting (`nd_transport.pending` — two volatile loads on
     PSP) or when we have ARQ payload queued that has not gone out (`tx_due`). The latter
     is load-bearing: keeping the core's *outbound* latency sub-frame is the only reason
     rfu.c polls mid-frame at all. Answering yes when idle stays correct, merely wasteful,
     so a transport with no `pending` hook (desktop/UDP) behaves exactly as before.
     **Rig-measured: 164,920 polls, 1,600 pumps — 99.0 % short-circuited, every payload
     still delivered in order.** Honest scoping: the same rig says the core makes only ~6
     `poll_receive` calls per frame during a live trade, not the ~450 the WAITEVENT path
     allows, so on hardware this is worth tens of microseconds per frame, not
     milliseconds. It is free, it is proven, and it is what makes the profiling above
     affordable — but on this evidence it is **not** the 20 fps.

  2. **`sceNetAdhocPdpSend` moves off the emulation thread (`net_tx_thread`).** ~1.7 sends
     per frame during an active trade (rig-measured; the field's ~70/s agrees). If one
     send costs ~1.5 ms on a PSP-1000's WLAN stack — entirely plausible for a kernel and
     driver round trip on 2004 hardware, and it would explain the PSP-3000 asymmetry —
     that alone is ~2.5 ms/frame, which is the whole cliff. So `send_to`/`broadcast` now
     copy into an SPSC ring (32 slots, ~5 KiB .bss, single producer, single consumer,
     FIFO; the ARQ above tolerates reorder anyway) and signal a TX thread. A full ring
     falls back to sending inline rather than dropping, and teardown drains the ring
     before `PdpDelete` so the BYE frames still reach peers.
     **Which priority wins depends on something only hardware can answer** — how much of
     `PdpSend` is the driver *sleeping* (a thread hides that completely) versus computing
     in kernel mode (a thread only moves it) — so all three placements ship:
     `net_tx_thread=1` **prompt** (default; priority 0x1F, just above main: the datagram
     leaves with today's latency and we win back whatever part of the send blocks),
     `=2` **deferred** (0x21, just below main: the signal does not reschedule, so the send
     lands in the slack the emu thread already donates at the vblank wait — a true
     offload, at up to one frame of TX latency), `=0` **inline** (pre-ADR-0021 behaviour).
     One sitting, three data points, no rebuild.

  3. **The adaptive frameskip mode is bounded (amends ADR-0019).** Mode 1 skipped 580 of
     600 frames on hardware and *still* missed 60 fps. That is not a tuning miss, it is
     the mechanism: `auto_threshold` skips whenever audio-buffer occupancy is below the
     threshold, a console that is behind never refills the ring, and the only bound is the
     core's `FRAMESKIP_MAX = 30` consecutive skips (libretro/libretro.c:96, 1361-1377) —
     30 skipped, 1 rendered, forever. A console short of ~1.4x needs half its renders
     back, not 97 % of them. Mode 1 now engages `fixed_interval` with interval 1 (skip
     one, render one: a hard bound, and no occupancy feedback loop at all). The hysteresis
     around it, the default (`0` off) and mode 2 (`auto`, ADR-0018 verbatim) are unchanged.

- **Alternatives:** *Coalesce several ARQ frames into one datagram* (≤144 B payloads
  against a ~1444 B PDP MTU, so 8-10 fit) — the right next lever **if** `pdp=` comes back
  expensive AND the TX thread fails to hide it. Not done blind: it is a wire-format change
  on a link whose two endpoints are flashed independently, and in steady state there is
  little to merge, because the prompt ACK already piggybacks (`arq_tx_due` sees the core's
  reply — enqueued from inside `deliver` — before the bare-ACK decision), so the send count
  is already near the protocol minimum. *Time every poll* — 912 extra clock syscalls per
  frame would have distorted the very thing being measured; counting them and publishing
  `clk_ns` prices them without paying for them. *Cache the clock across a frame* — the ARQ
  estimator's honesty depends on real timestamps (ADR-0017).

- **Consequences:** `EVT sess_cost` and `EVT net_tx_thread mode=N` are new grep-stable
  lines; `EVT skip_engage` now reads `mode=fixed_interval interval=1` instead of `thr=`.
  `nd_transport` gained an OPTIONAL `pending` hook and `nd_config` an OPTIONAL `prof_us` —
  **and that is how this ADR cost a segfault worth writing down.** `sdl/main_sdl.c` passes
  an *uninitialised stack* `nd_transport` to `udp_transport_iface()`, which filled only the
  fields it knew about; the new optional hook stayed stack garbage and `netdrv_poll_needed`
  called through it, killing the desktop twin the instant the core first polled
  (`run_trade_test.sh`, join side, SIGSEGV at `at_board`, `rc=139`). The PSP rig never saw
  it — its vtable happens to live in `.bss`. Both `*_transport_iface()` now `memset` the
  struct first, the contract is written into `netdrv.h`, and `test_udp_backend` fills the
  struct with `0xA5` before the call and asserts `pending == NULL`. **ADR-0017's "run both
  rigs — one alone will lie to you" earned its keep a second time.**

- **Verification:** netdrv unit suite, 13 tests, PASS. The new one is `poll_gate`: it
  drives the driver at the frontend's real cadence (28 gated polls per virtual millisecond
  plus one unconditional pump per 16 ms "frame") over a 10 % loss / 20 ms jitter link and
  asserts in-order exactly-once delivery both ways, that every queued payload opens the
  gate, and that the gate actually gated. `run_trade_test_psp.sh --radio=40` PASS
  (`retx_pct=0`, `overflow=0`, `txq=N/0/0` — every datagram through the TX ring, none
  inline, ring never full). `run_trade_test.sh` (desktop, 5 % loss + 30 ms jitter) PASS.
  `run_boot_test.sh` PASS, `.sav` byte-identical.

- **Honest limit:** PPSSPP's `sceNetAdhocPdpSend` is a host UDP socket and its ms0 is a
  host filesystem, so the rig can prove the instrumentation reports sanely and that none of
  this regresses correctness — it **cannot** price a PSP-1000's WLAN driver. **The verdict
  on the 20 fps is a field log.** Read `pdp=` first: calls per frame times the mean is the
  send bill. If that is milliseconds, A/B `net_tx_thread` 1 → 2 → 0 in one sitting and
  watch `frame=` and `EVT fps emu=`. If `pdp=` is cheap, the answer is in `frame=` minus
  everything itemised, and that residue is the next thing to bisect.

- **Upstream impact:** None (netdrv, frontend-common and psp/ are ours).

## ADR-0022 — FR/LG parked saves are supplied by hand, not played from a new game

- **Context:** Emerald's trade fixtures are cloned from a save the user already had and driven a
  few screens by `emerald_park.inputs`. FR/LG start from nothing, and the Union Room attendant
  is gated on `FLAG_SYS_POKEDEX_GET` **and** `CountPartyNonEggMons >= 2`
  (`data/scripts/cable_club.inc CableClub_EventScript_UnionRoomAttendant` /
  `..._CheckPartyUnionRoomRequirements`), while the earliest Poké Balls in the game are the five
  Oak hands over *with* the Pokédex. That forces the whole opening: intro/naming, starter, rival
  battle, Route 1 north, Viridian Mart for Oak's Parcel, Route 1 south, Oak, Route 1 grass to
  catch a second mon, then a Pokémon Center. A working chain was built and run
  (`firered_stage1` → `frlg_stage4`, four scripts, ~76k frames, ending with a Rattata caught and
  a two-mon party) before this decision was taken.
- **Decision:** **Do not own that path.** FR/LG parked saves are user-supplied assets, dropped
  into `testdata/fixtures/<edition>_parked.sav` exactly like the Emerald source save, with a
  written contract (PC 2F (6,4) facing north, Pokédex, 2+ non-egg Kanto-dex mons — see the
  header of `testdata/fixtures/frlg_trade_host.inputs`). The stage scripts are kept, banner-
  marked "NOT A FIXTURE PATH", and referenced by no test script; they exist as the evidence
  behind the "verified live" rows of the FR/LG address table in docs/AUTOPILOT.md.
- **Alternatives:** finish the chain (one more stage: Route 1 → Viridian PC 2F → park) and own
  it forever. Rejected: it is a large amount of brittle machinery — every leg is a hand-planned
  tile route through tall grass where a wild encounter can interrupt any step — to reproduce
  something a human produces in under a minute, and it buys the harness nothing that a supplied
  `.sav` does not.
- **Consequences:** the FR↔LG legs are blocked on an asset rather than on work, and
  `run_trade_test_psp.sh` says so with the exact path it wants instead of trying to generate.
  The union-room/link/trade addresses stay marked symbol-derived-not-observed until the first
  real run. If the supplied saves are parked somewhere unexpected, the failure is a named
  predicate timeout at the counter, which is the diagnosable outcome.
- **Upstream impact:** none.

## ADR-0023 — Cross-edition harness: one edition table, and ROM revision checked before launch

- **Context:** `run_trade_test_psp.sh` hard-coded Emerald everywhere — ROM path, the two fixture
  names, both input scripts, the Union Room map id `0x3c19` asserted at the end, and the `.sav`
  party offsets `read_party.py` decodes. FR/LG differ in every one of those (Union Room is
  `0x0400`; the party lives at SaveBlock1 `+0x34/+0x38` instead of `+0x234/+0x238`).
- **Decision:** `--roms=HOST,JOIN` over `{emerald,firered,leafgreen}`, with all six per-edition
  facts in one small `ed_*()` table at the top of the script; adding a leg is data, not code.
  `read_party.py` grows a `--game=emerald|frlg` flag with auto-detection. `map_grid.py` learns
  FR/LG's u32 metatile attributes, 640-metatile primary split, and one-way ledges so route
  planning against `pret/pokefirered` works with the same invocation. Separately, the script now
  verifies each ROM's header **game code (0xAC) and revision byte (0xBC)** before launching
  anything.
- **Alternatives:** a second `run_trade_test_frlg.sh`. Rejected — it would fork ~200 lines of
  rig, assertion and oracle logic that has nothing to do with the edition, and the two copies
  would drift.
- **Consequences:** the default invocation is unchanged and still the Gate-4 leg. The revision
  check is the cheap fix for a specific trap ADR-0009 called out: the address tables are
  per-revision constants, so a rev-0 FR image would fail as a predicate timeout hundreds of
  frames into the run rather than as "wrong ROM" — now it fails in one second with the byte it
  read. `--disconnect` stays Emerald-only and refuses rather than silently substituting a
  script.
- **Upstream impact:** none (harness only).

## ADR-0024 — No memory-stick write happens on the emulation thread: the EVT/LOG writer moves to its own thread

- **Context:** The 2026-08-01 field logs from both consoles, on the shipping build, price the
  problem exactly. The wireless layer is healthy (`overflow=0 spill=0 txfail=0 rxerr=0
  drop_crc=0`, `retx_pct=2-4`, `srtt_us` 35-60k) and the per-frame session costs are small
  (`pump≈200us rx≈190us arq≈20us enq≈118us`, `PdpSend` 55-80 us on its own thread). **But
  `frame=10194/62978` — mean 10.2 ms against a 16.68 ms budget, and a max of 62.9 ms.** The
  PSP-3000 host is no better: `frame` max **73403 us**. In a vblank-locked loop a 63 ms frame is
  four periods during which the peer gets no answer at all, and the user's trades now reliably
  fail at the moment the games run their trade/exit handshake.

  One named component of that residue is ours and is pure I/O: **`evt` max 12002 us on the
  PSP-1000 and 12525 us on the PSP-3000** — three quarters of a frame spent inside a single
  `fprintf`+`fflush` to the memory stick, on the emulation thread, for one log line. ADR-0021
  measured it precisely so it would never have to be argued again; this is the ADR that acts on
  the measurement. (The `evt` line count itself is innocent — 6-19 lines per 10 s window — so
  logging *less* was never the fix. The fix is logging from somewhere else.)

- **Decision:** `fe_evt` gains an optional **asynchronous sink**, and the PSP frontend installs
  one backed by a dedicated writer thread.
  1. **`fe_evt`/`fe_log` never touch the file when a sink is installed.** The line is formatted
     into a 16 KiB byte ring (`FE_EVT_RING`, power of two, free-running indices, single
     producer / single consumer) and the platform's `wake()` is called. `fe_evt_service()` —
     called only by the writer thread — does the `fwrite`+`fflush`. A line that does not fit is
     dropped **whole**, never truncated, so the log stays parseable, and the drop is counted.
  2. **The writer thread runs at priority 0x22, BELOW main's 0x20.** Deliberately: signalling
     must not preempt the emulator, and the writes then land in the slack the emulation thread
     already donates at `sceDisplayWaitVblankStart`. This is ADR-0021's `deferred` TX placement
     reused — and here there is no latency to trade away, because nothing waits on a log line.
  3. **It starts only after `fe_host_boot()` returns.** Boot diagnostics (BIOS sha1, ROM code,
     `mem_free`) stay synchronous, so a load failure is on the stick even if the main loop is
     never reached. It cannot fail a run either: if the thread or semaphore cannot be created we
     log `log_thread mode=0 reason=thread_failed` and stay inline.
  4. **Every exit path funnels through one `evt_shutdown()`** (stop the thread, hand logging
     back to the synchronous path, drain the ring on the caller's thread, close). `fe_evt`
     tolerates exactly one consumer at a time and the ordering is written into the header
     contract — stop your thread *first*, then `fe_evt_set_async(NULL)`.
  5. **A `config.ini` lever, not a rebuild:** `log_thread` (`net_log_thread` is accepted as a
     synonym so the A/B can never silently no-op) — `1` = threaded (default), `0` = the
     pre-ADR-0024 inline path. `autopilot.ini log_thread` pins it for the harness.
- **What the next field log must show.** `EVT sess_cost` keeps `evt=lines/us/max` but its
  meaning is now **what the emulation thread paid** (format + ring copy + signal), and gains
  **`evtio=us/max/drop/hi`** — the memory stick itself, on the writer thread, plus dropped lines
  and ring high-water. So: `evt=` max collapsing from ~12000 to tens of microseconds is the
  proof the move worked; `evtio=` max staying at ~12000 is the *expected* confirmation that the
  stick is still slow, just no longer in the emulator's way; `drop` must be 0.
- **Alternatives:** *Log less* — the counts were already innocent (ADR-0021 measured 6-19 lines
  per 10 s window), so this would have traded away diagnosis for nothing. *Buffer and flush
  every N lines* — turns 12 ms every few seconds into 40 ms occasionally, and loses the tail of
  the log on a crash, which is the exact case these logs exist for. *Drop the per-line fflush* —
  same objection: an unflushed log after a hard hang tells us nothing.
- **Consequences:** Desktop is byte-for-byte unchanged (no sink installed → the original
  synchronous path, same output). One new `EVT log_thread mode=N` line. `evt=`'s semantics
  changed and the change is recorded here and in the `sess_cost` comment. This ADR does **not**
  touch the SRAM writer — the field's `sram_flush ... ms=25.791` is a separate increment, and
  shipping them separately is deliberate: one variable per hardware run.
- **Upstream impact:** None (frontend-common is ours).

## ADR-0025 — The .sav block writes leave the emulation thread too (ADR-0020's budget was necessary, not sufficient)

- **Context:** ADR-0024 shipped and the field verified it on a PSP-3000: emu-thread `evt` max
  **12525 → 118 µs (~100x)**, the writer thread absorbing the stick's real **15602 µs** with
  `drop=0`, and `frame` max **73403 → 63772 µs** — about 10 ms recovered, exactly as predicted.
  The same log then fingers what is left. `frame` max was **37264 µs** early in the run and
  jumped to **63772 µs in the very heartbeat window containing**
  `EVT sram_flush crc=a3df0f6e size=131072 wrote=57344 blocks=14/32 mode=delta ms=29.296`.

  ADR-0020 anticipated this and said so: its budgeted drain bounds *the loop*, but explicitly
  "one block may overshoot the budget", and it named "move the write to its own thread" as
  **the real end state**, deferred because it needed a snapshot buffer and a save-in-flight
  state to reason about at exit. That is now the only stall left worth this much, so it is time
  to pay for those two things properly.
- **Decision:** the dirty **scan** stays on the emulation thread (it is CPU over a 128 KiB
  buffer, not I/O, and moving it would mean snapshotting the whole image); every **open, seek,
  write, flush and close** moves to the ADR-0024 writer thread. Four pieces:
  1. **A per-block BYTE state replaces the dirty bitmap.** `CLEAN` / `DIRTY` / `WRITING`. Two
     threads share it, and a byte store is atomic on MIPS where a read-modify-write on a
     bitmap word is not. **The emulation thread only ever raises `DIRTY` and never clears**;
     the writer only `CLEAN`s a block it still owns (`state == WRITING` after the write). So a
     block the game touches mid-write is re-raised, the writer's compare-and-clear fails, and
     the next pass rewrites it. That is the entire synchronisation and it needs no lock.
  2. **A 4 KiB staging copy** — the writer copies the block, writes the copy, and CRCs *the
     copy*. The recorded CRC is therefore always the CRC of the bytes on disk, which ADR-0020's
     "CRC the live buffer after writing it" could not quite promise. Strictly tighter, 4 KiB.
  3. **An explicit ordering contract instead of a mutex** (written into fe_host.h): install the
     interface after `fe_host_boot`; exactly one thread calls `fe_host_sram_service_io()`; and
     **stop that thread and `fe_host_set_io(NULL)` BEFORE `fe_host_shutdown()`** — because
     shutdown closes the file and then `retro_unload_game()` frees the very buffer the writer
     reads. `io_thread_stop()` is that contract in code and ends with a synchronous
     `fe_host_sram_sync()`, so anything the writer had not finished (including the case where
     it had to be terminated) completes single-threaded before anything is closed or freed.
  4. **`fe_host_sram_sync()` waits by YIELDING, and never takes the file over.** The writer runs
     *below* the caller, so a busy wait would starve the thread being waited on. On timeout
     (4000 × ~1 ms) it logs `EVT sram_sync_timeout` and returns rather than putting two writers
     on one `FILE*` — the shutdown path then drains it synchronously and cannot fail to
     complete. **Save integrity is never traded for smoothness; forced flushes still drain to
     completion before returning.**
- **`ms=` is no longer ambiguous.** The field's `ms=29.296` could have been one stall or an
  elapsed-across-frames total, and nothing in the log said which. `EVT sram_flush` now carries
  `slices=` (drain calls it took — `1` means `ms` *is* a single stall, `>1` means it is elapsed
  across that many), `worst_ms=` (largest single call — the actual stall), `blk_ms=` (worst
  single 4 KiB write, the thing a budget structurally cannot bound), `scan_ms=` (the CRC scan,
  i.e. the only part of a save the emulation thread still pays for) and `thr=` (1 = none of it
  was on the emulation thread).
- **Alternatives:** *Snapshot all 128 KiB and let the writer work from that* — 128 KiB of .bss
  against a measured `mem_free=389120` during a session, to solve a tearing problem the
  re-dirty rule already solves for 4 KiB. *A mutex around the file* — real, but it makes the
  emulation thread block on the memory stick again in exactly the case that matters, which is
  the bug. *Defer all writes to session end* — refused in ADR-0020 and still refused.
- **Consequences:** `config.ini sram_thread` (0 = ADR-0020's in-frame budgeted drain, 1 =
  default) is the A/B lever, `autopilot.ini sram_thread` pins it for the harness, and
  `EVT log_thread` now reads `mode=N sram_thread=N prio=0x22`. Desktop passes no interface and
  keeps ADR-0020's behaviour byte-for-byte. **`run_save_test.sh` is part of this ADR's gate,
  not an afterthought** — it is the only harness that proves an in-game save survives the round
  trip, and this ADR touches every write on that path.
- **Also fixed here** (both cosmetic, both spotted in the same field log, both nearly free):
  `EVT skip_engage fps=0.00` fired while the in-game menu was open, because the core is paused
  and the fps window divided real seconds by zero frames — the window is now re-based every
  menu frame. And `sess_cost` windows spanning a menu open/close reported nonsense magnitudes
  (`pump=14674 arq=8773`) from a one-frame window — windows under 60 frames are now folded into
  the next instead of emitted, except at teardown, which always reports.
- **Upstream impact:** None (frontend-common is ours).

## ADR-0026 — The dirty scan follows the writes off the emulation thread (and what the user's memory stick actually costs)

- **Context — record the hardware fact first, because it justifies both this ADR and the last
  one.** The user's memory stick is *brutally* slow: the field measured **`blk_ms=34.817` for
  a SINGLE 4 KiB block write**, and **`worst_ms=65.556` for a six-block flush**. That is the
  number ADR-0020's 3 ms budget was trying to bound and structurally could not — one block
  overshoots the entire budget by 12x. It is also why this class of bug was so severe on this
  particular console, and why the answer was to move the work rather than to schedule it more
  cleverly. **You cannot budget a device whose quantum is 35 ms into a 16.68 ms frame.**

  ADR-0025 landed and the field verified it: `sram_flush … thr=1` on both consoles, `frame` max
  **69470 → 47286 µs** (PSP-1000) and **63772 → 40932 µs** (PSP-3000) — ~22 ms removed on each.

  What that exposed is the scan. `scan_ms` came back at **11.243 / 11.272 / 11.280 ms** — two
  thirds of a frame, on the emulation thread, every 300 frames (~5 s), **whether or not
  anything was dirty**. Unlike the writes it is pure CPU (a per-block CRC32 over the live
  128 KiB buffer, no file access at all), and unlike the writes it is *guaranteed periodic*.
- **Decision:** the scan follows the writes onto the writer thread. `sram_scan()` is factored
  out and called from `fe_host_sram_service_io()` on its own ~5 s wall-clock cadence — the same
  interval as the emulation thread's `SRAM_CHECK_INTERVAL`, so **the window in which a save can
  be lost to a power cut is unchanged: this moves work, it does not relax a guarantee.**
  `fe_host_sram_flush()` becomes a request when a writer is installed (`sram_scan_req`), and a
  forced flush waits for the *whole* cycle — scan, then drain — via `fe_host_sram_sync()`,
  which now counts a pending scan as outstanding work. With no writer installed (desktop, or
  `sram_thread=0`) the function is ADR-0020's synchronous path, unchanged.

  **Why moving it beats spreading it.** Spreading the CRC over frames was the obvious fix and
  is the wrong one on this hardware: the cycles would still be spent on the emulation thread,
  just in smaller pieces, and the console that matters is already missing real time in the
  Union Room (49-52 fps on the 1000 against 58 on the 3000). On a single-core machine the only
  place to put 11 ms that costs the emulator nothing is time the emulator is not using — and
  the vblank wait is exactly that. Moving it also keeps the code honest: one owner for the
  whole save path, one ordering contract, no new latency knob.

  **Racing the game stays harmless, and is pre-existing.** A block read while the game writes
  it is seen as dirty, written, and CRC'd from the ADR-0025 staging copy; if it moved again the
  next sweep catches it. The whole-image `crc=` is still the chained per-block CRC computed in
  the same pass, so it remains bit-identical to the original single-shot value and every
  existing log line and harness oracle still matches.
- **Consequences:** after this ADR the emulation thread pays **exactly nothing** for the save
  path — no scan, no write, no open, no close. `fe_host_run_frame`'s periodic flush is skipped
  outright when a writer is installed. `EVT sram_flush`'s `scan_ms=` now reports time spent on
  the writer thread (`thr=1` covers the whole operation), which is what makes "the save path is
  free now" a measurement rather than a claim. `sram_thread=0` remains the A/B lever and
  reverts scan and writes together.
- **Alternatives:** *Spread the CRC across frames (1/8th per frame)* — see above; wrong thread.
  *Scan only blocks the core plausibly touched* — the core exposes no SRAM write notification,
  so this would mean inferring dirtiness from emulated bus activity: a core patch, on the save
  path, to save CPU we can simply relocate. *Lengthen the interval* — makes the stall rarer,
  not smaller, and widens the loss window, which ADR-0020 already refused.
- **Upstream impact:** None (frontend-common is ours).

## ADR-0027 — Peers match each other's emulated frame rate (equality beats absolute speed)

- **Context — the field finding, on both consoles, current build. Facts; do not re-derive.**
  The join/client role costs **~12 fps** against the host role: PSP-3000 **58 fps as host, 46 as
  join**; PSP-1000 **56-58 as host, 49-52 as join**. The client sends ~1.9x the host (`core_tx`
  3867 vs 2044 in the same session). Transport is *perfect* — `core_tx`/`core_rx` mirror exactly
  between consoles, `overflow=0 spill=0 txfail=0 drop_crc=0`, `retx_pct` 1-6 % — and ADR-0021's
  per-frame session costs total only ~500 µs (`pump=208 rx=186 arq=32 enq=111`, `pdp` mean 65-79
  µs on its own thread). **So the bulk of the client's extra cost is inside `retro_run` — the
  core's RFU client path — not our transport.** Symptom: in the Union Room the joining console
  stops accepting local input while still rendering the peer's movement; the other console then
  hits the game's fatal link screen. Trades never complete.

  **Why a rate *difference* is the bug, not merely slowness.** Two real GBAs both run at
  59.7275 Hz, so their link timing is mutually consistent by construction. **Gen-3's RFU counts
  link timeouts in FRAMES, not wall-clock seconds** (`RFU_DEF_TIMEOUT` = 32 frames). Two
  emulators running ~20 % apart are therefore permanently inconsistent in a way two cartridges
  never are: the faster side burns through its frame-counted deadlines while the slower side is
  still assembling a reply. Chasing the client's missing 12 fps means optimising the core's RFU
  path — open-ended, and possibly a core patch. **Making the two rates equal is a frontend
  change we can ship today, and equality is what the games actually require.**
- **Decision: while a session is live, the faster console paces itself down to the slower
  peer's rate. Equality matters more than absolute speed — both consoles run slower during a
  session, deliberately.**

  1. **What crosses the wire.** Each side publishes its emulated frame rate in hundredths
     (5973 = 59.73) as a **2-byte little-endian payload on `ND_T_PING`** — an existing frame
     type that carried no payload, so no new type and no version negotiation. A peer on an older
     build sends a bare PING and simply reads back as *unknown* (0), never as *slow*; a bare
     PING never clobbers a previously reported value, because absence of a report is not a
     report of zero. `netdrv_set_local_fps()` / `netdrv_peer_min_fps()` are the whole API;
     netdrv carries the number and owns none of the policy.

     **The keepalive cadence alone would have exchanged nothing when it matters.** PING is
     suppressed whenever we sent anything else, and during a trade we send DATA every frame — so
     ADR-0027 adds a *separate* `ND_FPS_REPORT_US` = 500 ms cadence that runs regardless of
     other traffic. One 18-byte datagram per peer per 500 ms (~36 B/s) against a session that
     already moves ~65 packets/s each way. `test_peer_fps_exchange` asserts the crossing happens
     on a **deliberately saturated** link, because that is the only case the field runs.

  2. **A console advertises its CAPABILITY, never its throttled rate — this is the load-bearing
     detail.** If each side advertised its *achieved* fps, the pair would ratchet downward
     without bound: A throttles to 46 to match B, A now measures and reports 46, B sees "peer is
     at 46" and throttles itself, A re-measures lower, and both crawl into the floor. That
     failure would present as a performance regression, not as a control-loop bug, and would be
     miserable to diagnose in the field. So capability is derived from **per-frame WORK time** —
     the loop iteration with every vblank wait excluded, the same quantity ADR-0021 reports as
     `frame=`. A frame costing *w* µs occupies `ceil(w / 16.683 ms)` vblank periods (minimum
     one); summed over a ~1 s window the free-running rate is `frames x 59.94 / periods`,
     smoothed by an EMA with 1/3 weight on each new sample. **Our own deliberate idle is not
     work, so throttling ourselves cannot move our own advertised number.** `peer_cap` therefore
     always means "how fast my partner *can* go", never "how fast my partner is currently
     choosing to go", and the loop has exactly one mover: the genuinely faster console.

  3. **One mover, a floor, a ceiling, hysteresis, and a ramp.**
     - **One mover.** Engaging requires a real gap (`self_cap > peer_cap + 0.50 + 1.50` fps);
       releasing requires only 0.50 fps — a Schmitt trigger. Two consoles of similar capability
       both stay off and **neither** throttles, so a healthy session pays nothing.
     - **Floor.** A peer reporting below **40.00 fps** is not chased: we log `EVT pace_floor`
       once and leave it to the existing failure paths. Values outside 10-200 fps are ignored
       outright, so a malformed or stale field can never stall a console.
     - **Ceiling.** The target never exceeds the GBA's own 59.7275 Hz.
     - **Hysteresis + ramp.** The goal re-targets only on a >1.00 fps move, and the *applied*
       target glides **2.00 fps per ~1 s window** rather than stepping — ~7 s to close the
       field's 58-to-46 gap, well inside a Union Room session, without a visible lurch.
     - **Release.** If the peer speeds up (it leaves the Union Room and its RFU cost drops) or
       goes silent, the goal returns to nominal and the target ramps back off. **Nothing latches
       for the rest of the session**, and `net_teardown` resets pacing and audio to nominal.

  4. **How the target is applied: a fractional vblank.** The loop is vblank-locked, so the only
     lever is how many vblanks a frame waits for — and a whole extra vblank is a jump from 59.94
     to 29.97 fps, far too coarse to match 46.5. Instead the fraction is accumulated: a target
     needs `59.94 / target` vblanks per frame, and we spend one extra vblank whenever the
     carried fraction crosses 1. Target 46.50 → 1.2890 vblanks/frame → an extra vblank on 28.9 %
     of frames, averaging exactly 46.50. Bounded at 2 extra vblanks and the accumulator never
     banks debt, so a pathological target cannot freeze the loop.

- **Audio: the stream stays CONTINUOUS and the pitch drops with the pace. This was a real
  trade-off and it went the other way from the brief's default preference, so here is the
  reasoning.** The core produces audio per *emulated* frame at `in_rate` (65536 Hz), and the
  audio thread consumes at a fixed 16.16 step into the 44100 Hz output. Run the emulator at 46.5
  of 59.7275 fps and production falls to 78 % of consumption while the consumer keeps its rate:
  the ring drains and `audio_thread` emits **silence** for the shortfall (`avail < 2` → zeros).
  That is ~22 % of every second as gaps — a continuous buzz/crackle, the worst-sounding option
  available. Dropping *rendered* frames does not help: frameskip changes what reaches the screen,
  not the rate at which the game and its audio advance, and ADR-0019 already established that
  skipping buys nothing in a vblank-locked loop. **So `g_audio_step` follows the pace target**
  (`step x target / 59.7275`), which makes production and consumption match exactly: no
  underrun, no crackle, no dropouts — at the cost of a proportional pitch drop, ~4 semitones at
  46.5 fps. It glides rather than jumps because the pace itself ramps, so it reads as a tape
  slowdown, which is *exactly what it is*: the console is genuinely running the game slower. The
  effect is bounded by the 40 fps floor, scoped strictly to an engaged throttle (nominal
  otherwise, and restored at teardown), and switchable at runtime via `net_pace_match=0`. If the
  user judges the pitch worse than the trade completing, that lever is the answer and the ADR
  should be revisited with their verdict.
- **Consequences.** `config.ini net_pace_match` = **1 on (default)** / 0 off, with an
  `autopilot.ini` override, exactly like `net_tx_thread` / `log_thread` / `sram_thread` — a
  hardware A/B with no rebuild. New log lines: `EVT net_pace_match mode= nominal= floor=` at
  session start, `EVT pace_match target= self_cap= peer_cap= engaged= why=` on every change
  (engage / release / ramp) plus a 10 s `why=hold` heartbeat while engaged, and `EVT pace_floor`
  when a peer is too slow to chase. **`self_cap` visibly sagging while `engaged=1` is the
  ratchet, and that one line says so directly** — the harness asserts it cannot happen.
  `EVT sess_cost` gains `pace=target/self_cap/peer_cap/engaged`. Both consoles run slower during
  a session; that is the point, and it is the honest cost.
- **Also landed here (cheap, and it closes an ADR-0021 gap): `retro_run` is bracketed.**
  ADR-0021 priced every part of a session *except* the core, and inferred the client's cost was
  inside `retro_run` by subtraction. `EVT sess_cost` now carries `core=avg/max` measured directly
  (two clock reads per frame on top of ADR-0021's ~13). This also bounds the still-open ~41-49 ms
  `frame` maximum: **if `core_max` tracks `frame_max` the residue is the core/dynarec; if it does
  not, the residue is ours** — the GU blit / `sceGuSync` path, or the once-per-60-frames
  `sceIoGetstat(DUMP_MARKER)` on a stick whose 4 KiB write costs 34 ms (ADR-0026). That stat is
  a harness-only convenience and is a strong candidate for deletion, but it is a *separate*
  variable and is deliberately not changed here.
- **Alternatives.** *Optimise the core's RFU client path* — the real fix for absolute speed, and
  still worth doing, but it is open-ended, likely a core patch, and does not remove the class:
  any two consoles of unequal capability re-create it. Pace matching makes the games consistent
  regardless. *Pin both sides to a fixed low rate (e.g. 45)* — costs speed on every session
  including healthy ones, and still breaks against a console that cannot hold 45. *Lengthen the
  core's RFU timeouts* — a core patch that lies to the game about time, refused for the same
  reason ADR-0017 refused to guess: HANDOFF is explicit that speculative timing patches are how
  the ARQ storm survived Phase 4. *Advertise achieved fps instead of capability* — the ratchet
  above; rejected on analysis, and the harness now guards it. *Speed the slow side up with
  frameskip* — ADR-0019 measured this: it discards rendered frames and buys no throughput,
  because the loop still waits for vblank afterwards.
- **Upstream impact:** None. netdrv and frontend-common are ours; the core is untouched.

## ADR-0028 — The core itself misses the frame deadline: attribute the spike, stabilise the matcher, stop paying for audio nobody can hear

- **Context — ADR-0027 shipped, the field answered, and the answer reordered the whole problem.**

  **What worked, and it is not small.** Both consoles logged `EVT net_pace_match mode=1
  nominal=59.73 floor=40.00`; engage / ramp / floor / release all fired; and **the anti-ratchet
  held on real hardware, on both consoles** — PSP-1000 `self_cap` 56.47-59.01 on every line
  while `target` walked to 50.11, PSP-3000 `self_cap` 58.60→58.84 flat across its whole engaged
  run — never sagging. The advertise-capability-not-achieved-rate decision was the right one and
  is now hardware-proven. **User-visible: both consoles entered the Union Room together and BOTH
  COULD MOVE — a first.** The failure moved later, to the "standby for communication" screen
  when the players sit down to actually exchange data.

  **Problem 1 — the matcher was chasing an oscillating target.** `peer_cap` from the 1000's log,
  in order: 49.95, 44.71, 41.58, 38.89 (floor, released), 41.08, 47.03, 51.00, 53.65, 55.41,
  56.58, 52.60, 51.71, 50.87, 49.61, 51.10, 49.79, 51.10, 53.45, 55.28, 56.50, 57.85, 58.21.
  **~20 fps of swing within seconds.** Against ADR-0027's symmetric 2.00 fps/s ramp the applied
  target was never the right one, and we disengaged at the floor *mid-session*.

  **And the second log said why the swing exists: the consoles TRADE the slow role.** The 3000's
  own trace shows `self_cap` 56.61 → 49.95 → 44.71 → 41.58 **while `engaged=0`** — i.e. genuine
  capability loss while not throttling. Those exact values appear as `peer_cap` in the 1000's
  log at the same moments, which independently confirms the exchange and the measurement are
  sound. Whichever console sits in the JOIN seat becomes the slow one, so **the identity of the
  slow peer flips**, and a loop that tracks "whoever is behind right now" keeps reversing
  direction. The 3000 also released and then never re-engaged even as it became the slower side.

  **Problem 2 — and this is the root cause of Problem 1: the core misses the deadline on its
  own, with no session at all.** The 3000's *first* `sess_cost`, logged **before
  `peer_connected`**, wireless not yet carrying anything: `core=11390/21263` — **core max 21.3
  ms, mean 11.4 ms, against a 16.75 ms frame budget.** In-session it grows to 26121 and 29472
  µs. The 1000's solo window has the same shape (`core=11698/21262`). So gpSP alone periodically
  blows the frame deadline *while playing Emerald single-player*, and that is what makes
  capability oscillate, which is what destabilised the matcher, which is what desyncs the link.
  **Everything ADR-0024/25/26/27 fixed was real, and all of it was downstream of this.**

  **This also retires ADR-0027's rig-derived claim.** The rig measured `core` max 10.5 ms against
  `frame` max 24-35 ms and I concluded the worst-case spikes were *outside* `retro_run`. On
  hardware it is the reverse: `core=11439/24681` against `frame=11736/35079` — the core alone
  accounts for most of the worst frame, and the core *mean* (11.4 ms) is essentially the entire
  frame mean (11.7 ms). **Recorded as a rig-vs-hardware divergence, not a correction of a typo:**
  PPSSPP on a desktop host has a completely different dynarec cost profile and a RAM-resident
  ROM, so it cannot model either of the mechanisms below. The rig can prove control-loop
  correctness; it cannot price the core.

- **Decision 1 — attribute the spike instead of guessing at it, with the smallest possible core
  patch.** The two mechanisms that can plausibly cost tens of milliseconds inside `retro_run`
  were both, remarkably, **completely uncounted**:

  1. **Translation-cache flush.** The PSP build defines `SMALL_TRANSLATION_CACHE`
     (`Makefile:236-249`), giving a **2 MiB ROM / 384 KiB RAM** JIT cache against 10 MiB / 512 KiB
     elsewhere (`gpsp_config.h:14-25`). `flush_translation_cache_rom()` memsets a **256 KiB**
     branch-hash table and rewinds the ROM cache pointer, so **every block must be re-JITted
     afterwards** — up to 2 MiB of code thrown away. `flush_translation_cache_ram()` memsets up
     to **288 KiB** of SMC shadow. The existing `flush_ram_count` is **cleared every frame**
     (`main.c:230`) and its only consumer is a commented-out `printf`, so it cannot attribute
     anything; **there was no ROM-flush counter at all.**
  2. **ROM paging.** `load_gamepak_page()` does a `filestream_seek` + **32 KiB
     `filestream_read`** from the memory stick, *in the middle of emulation*, on every ROM page
     fault — and it is called from the translation path too (`cpu_threaded.c:3037`). It only
     happens when the ROM does not fit in RAM, which is exactly our case: the field reports
     `mem_free=389120` after the core's greedy buffer allocation, against a 16 MiB Emerald.
     **ADR-0026 measured this user's memory stick at 34 ms for 4 KiB.** Also uncounted.

  So: four monotonic counters, no behaviour change — `flush_rom_total`, `flush_ram_total`
  (`main.c`/`main.h`, incremented in `cpu_threaded.c`), `gamepak_page_loads`
  (`gba_memory.c`/`.h`). **This is our third core patch and it is the most trivially
  upstreamable of the three: three `u32`s and three `++`.** ADR-0011 is the precedent.

  **Counting per frame is not enough — an average hides a once-a-minute event.** So `fe_host`
  snapshots the counter *deltas of the frame that set the maximum*. `fe_host_config` gains an
  optional `core_counters` callback supplied by the frontend, so `frontend-common` stays
  core-agnostic (same boundary discipline as the `gpsp_rfu_link_down_hook` weak symbol). Two new
  log fields, and a new line so **solo play is covered — the decisive fact was a spike with no
  session, and `EVT sess_cost` only exists while one is live**:
  - `EVT core_prof core=mean/winmax/max win=rom/ram/page wspike=rom/ram/page spike=rom/ram/page`
    on the heartbeat cadence, session or not.
  - `EVT sess_cost … corewin=…/…/… corespike=…/…/…`.

  **Two maxima, and the second one is a bug the rig caught in my own instrumentation.** The
  all-time max is usually set during boot, so its snapshot latches early and reads `0/0/0`
  forever — a rig window that logged **4110 RAM flushes** still reported `spike=0/0/0`. A stuck
  snapshot would have made the field log inconclusive, which is the one thing this build exists
  to avoid. So `wspike=` reports the worst frame *in the current window*, re-based every
  heartbeat, and is the field to read.

  **A nonzero `wspike` field names the cause outright. All zeros against a large `winmax` rules
  out both and moves the hunt inside the emulation loop.** No fix is attempted here on purpose: guessing twice is how the ARQ
  storm survived Phase 4 (ADR-0017), and a 2 MiB cache enlargement against ~380 KiB of free
  memory is not a change to make blind.

- **Decision 2 — the pace target is the PAIR'S SUSTAINED WORST, not the peer's current rate.**
  - **Decaying low-water marks.** Each side keeps one for *both* capabilities. A new low is taken
    **immediately** (the worst is what desyncs a link and must never be smoothed away); a low is
    forgotten only after **4 s** of nothing worse, and then at **0.50 fps per window**. This is
    what turns a 20 fps oscillation into a number that holds still.
  - **`goal = min(self_lo, peer_lo) + 0.50`.** **`min()` is symmetric**, so both consoles compute
    the *same* target from the *same* two numbers, and **a role flip does not move it** — the
    identity of the mover changes, the target does not. A steady 48 beats an accurate-but-moving
    39-58.
  - **One mover now falls out for free rather than being enforced.** Whichever console is the
    binding constraint is already below the target and inserts no waits, because the throttle can
    only ever *add* delay. ADR-0027's `self_cap > peer + gap` engage test is gone; nothing
    decides who moves.
  - **Asymmetric ramp: fall 8.00 fps per window, rise 0.50.** Getting slow late is what desyncs a
    link; getting fast late costs nothing.
  - **The floor CLAMPS, it no longer releases.** ADR-0027 released at 40 fps and the field showed
    that is actively worse: against a peer at 38.89 it snaps us back to 59.73 and makes the gap
    21 fps instead of 1.1. We still never pace *ourselves* below 40 — that was the floor's real
    purpose. `EVT pace_floor … clamped, still pacing`.
  - **`engaged` is now a wide-hysteresis report, not a gate** (engage 1.50 below nominal, release
    within 0.40). The throttle follows the applied target alone, so the engage/release cycling
    the field showed — and the 3000 never re-engaging — cannot recur.
  - `EVT pace_match` gains `self_lo=` and `peer_lo=` so the field can see the marks working
    rather than infer them.

- **Decision 3 — `gpsp_sound_rate` 65536 → 32768.** The core's own option text
  (`libretro/libretro_core_options.h`): *"Both values keep audio timing exact. 65536 renders the
  full mixer bandwidth; **32768 matches the bandwidth of real hardware's default PWM output and
  halves audio mixing work.**"* Real GBA PWM output is 32768 Hz, so 65536 was buying bandwidth
  the console never produced, at double the mixing cost, **inside `retro_run` — exactly where the
  11-12 ms mean lives.** Strictly better: cheaper *and* closer to hardware.

  **The trap this walked into, and the fix.** `psp/main_psp.c` had `static unsigned in_rate =
  65536;` **hardcoded**, and `audio_start()` runs *before* `fe_host_boot()` (the audio ring is
  allocated before the core eats the heap, FRONTEND-AUDIT §8). Changing the option alone would
  have left the resampler consuming at twice the production rate — double-speed playback into a
  permanently draining ring. So the rate is now plumbed: `fe_host` records
  `retro_get_system_av_info`'s `sample_rate` and exposes `fe_host_sample_rate()`; the PSP
  frontend adopts it right after boot and recomputes the resampler step, which the audio thread
  already re-reads every output chunk (ADR-0027 made that possible). `EVT audio_rate
  in=… out=… step=…` records it. **The header says it outright: platforms must resample from
  `fe_host_sample_rate()`, never from a literal.**

- **Also checked and deliberately NOT changed: `gpsp_sprlim`.** The option is named **"No Sprite
  Limit"** — `disabled` *keeps* the GBA's per-scanline sprite limit. We were already on the
  hardware-accurate **and** cheaper setting; enabling it would render sprites real hardware
  drops, costing accuracy *and* speed. The name reads backwards, so a comment now guards it
  against a well-meant future "optimisation".
- **Consequences.** The matcher is mitigation now, not the fix — a stable target still helps and
  it is already written. The next field log should name the core spike outright. Both consoles
  still run slower during a session, deliberately (ADR-0027). Audio pitch behaviour is unchanged
  in kind; the pitch *baseline* is unchanged because the resampler follows the reported rate.
- **Alternatives.** *Enlarge the translation cache* — plausible if flushes are the answer, but
  ~380 KiB free against a 2 MiB cache means it must be measured first, and it may simply not fit.
  *Flush deliberately at a safe moment (menu, session start, room entry)* — attractive and cheap
  **if** flushes are confirmed; still a guess today. *Pre-load the whole ROM to kill paging* —
  16 MiB against 380 KiB free, so not on a PSP-1000 without giving something else up. *Bracket
  inside `retro_run` by phase (audio/video/CPU)* — the next step if the counters come back all
  zero, but it is a bigger, more invasive patch than three `u32`s and should not be spent first.
- **Upstream impact:** the counters are three `u32`s and three increments in `main.c`, `main.h`,
  `cpu_threaded.c`, `gba_memory.c`, `gba_memory.h` — no behaviour change, trivially upstreamable,
  and genuinely useful to any port on constrained hardware. The `gpsp_sound_rate` and
  `gpsp_sprlim` decisions are frontend option choices, not core changes.

## ADR-0029 — The core's spike is self-modifying code, not a small JIT cache: split the flush counter and stop planning around the wrong mechanism

- **Status:** accepted 2026-08-02, branch `phase5e-dynarec`. Supersedes ADR-0028's *"weigh a
  bigger cache"* alternative and retires it on evidence.
- **Context.** ADR-0028 landed `flush_ram_total` and the rig immediately reported a window with
  **4110 RAM translation-cache flushes, 307 of them inside the single worst frame**
  (`win=0/4110/0 wspike=0/307/0`). That counter was **useless for deciding anything**, because
  `flush_translation_cache_ram()` has two entirely unrelated triggers that it conflated:
  1. **Cache exhaustion** — `translation_ptr > translation_cache_limit` in
     `translate_block_arm/thumb` (`cpu_threaded.c:3104`, `:3267`). PSP's
     `SMALL_TRANSLATION_CACHE` gives a **384 KiB** RAM JIT cache vs 512 KiB elsewhere
     (`gpsp_config.h:15-20`). A bigger cache makes these rarer.
  2. **Self-modifying code** — a write into RAM that holds translated code raises
     `CPU_ALERT_SMC` / hits `smc_write`, and the dynarec stubs then throw away the **entire** RAM
     cache. **A bigger cache does nothing for these.**
  The two demand opposite fixes, and the plan of record was to spend ~128 KiB of a ~372 KiB
  hardware memory budget on the cache. That would have been spent on the wrong mechanism.

- **Decision 1 — split the counter by cause, and then split the SMC cause by source.** Pure
  counters, no behaviour change:
  - `flush_ram_full` — incremented at the two exhaustion sites in `cpu_threaded.c`.
  - `flush_ram_smc` — a new entry point `flush_translation_cache_ram_smc()` that the store stubs
    call (`smc_write`) instead of the plain function.
  - `flush_ram_dma` — a second entry point `flush_translation_cache_ram_dma()` for the
    `write_io_epilogue` path. This is exactly and only the DMA case: `CPU_ALERT_SMC` is raised
    **nowhere** except `dma_write_iwram` / `dma_write_ewram` (`gba_memory.c:1835`, `:1858`).

  The split had to reach into assembly on all four dynarec targets. On MIPS the SMC sites go
  through `cfncall`, whose `targetid` indexes an `fnptrs` table that lives contiguously in the
  register-base object at `FNPTRS_BASE`; two entries appended (`# 10`, `# 11`), with nothing
  after them in `.data`, so the offset arithmetic is untouched. ARM/ARM64/x86 call the symbol
  directly. `EVT core_prof` now reads `win=rom/full/smc/dma/page` (same for `wspike=`/`spike=`,
  and `corewin=`/`corespike=` in `EVT sess_cost`).

- **The measurement, and it is unambiguous.** PPSSPP rig, `run_trade_test_psp.sh --radio=40`,
  full autonomous Union Room trade, both consoles:

  | | rom | **full** | **smc** | dma | page |
  |---|---|---|---|---|---|
  | host `win=` (600 frames) | 0 | **0** | **4109** | 1 | 0 |
  | host `wspike=` (worst frame) | 0 | **0** | **307** | 0 | 0 |
  | join `win=` | 0 | **0** | **4121** | 0 | 0 |
  | join `wspike=` | 0 | **0** | **247** | 0 | 0 |
  | boot test (solo, 3600 frames) | 1 | **0** | **70** | 0 | 0 |

  **Cache exhaustion never happens — not once, in any window, in any run.** The RAM JIT cache is
  wiped by SMC long before it can fill. Every flush but one is a **single CPU store** from
  translated code landing on a tagged halfword; **DMA is not the source either** (1 event in
  ~4110). ROM flushes and ROM page faults stay at zero.

  **And it prices the spike.** The worst frame is 8456 us against a 5325 us window mean, with 307
  SMC flushes in it: **~10.2 us per flush on the rig**. On a 333 MHz Allegrex with cold caches the
  same work (wipe the cache, then re-JIT every IWRAM/EWRAM block on demand) is several times
  that — 307 flushes at ~70 us is ~21 ms, which is the field's `core` max of **21263 us** almost
  exactly. The mechanism accounts for the whole observed spike.

- **Decision 2 — do NOT raise `RAM_TRANSLATION_CACHE_SIZE`.** `flush_ram_full = 0` means the
  384 KiB cache is *never* the binding constraint. +128 KiB would buy nothing, and would spend a
  third of the free RAM on a PSP-1000 to do it. **The freed-memory work below is likewise not
  justified by anything we can currently measure**, and is deferred rather than done blind.

- **Decision 3 — deferred, with reasons, so nobody re-derives these.**
  - *Freeing `state_buf` (416 KiB, `fe_host.c:394`) by allocating savestates on demand.* The
    static buffer is **deliberate**: `init_gamepak_buffer()` (`gba_memory.c:2297`) is a **greedy**
    allocator that mallocs 1 MiB blocks until failure, so it eats whatever .bss gives back — the
    boot log reads `mem_free=765952 max_block=524288`, i.e. that loop already ran the heap down
    below one block. Returning 416 KiB would hand the ROM buffer one more megabyte (of sixteen)
    and then make a runtime `malloc(416 KiB)` for a savestate **reliably fail**. That trade is
    only worth making if ROM paging is actually costing us, and `gamepak_page_loads` is **0**
    across every rig run. Revisit if a *hardware* log shows `page` nonzero.
  - *Moving `fb_staging` (82 KiB, `psp/video_psp.c:53`) into VRAM.* Sound in principle — the CPU
    writes it, the GU reads it as a texture, and >1 MiB of VRAM is spare — but it cannot be
    validated on the rig (PPSSPP is far more forgiving about VRAM addressing than real Allegrex),
    it touches the same blit path as the known real-hardware R/B channel bug, and the payoff is
    82 KiB of RAM we have no measured use for. Not shipped blind.
  - *Deterministic flush at idle, hooked to `SWI 0x02` (Halt/VBlank-wait).* **The hook point
    exists and is clean** — gpsp runs the real BIOS for SWI (`bios_swi_entrypoint`,
    `init_bios_hooks`), so the halt surfaces as `CPU_ALERT_HALT` -> `cpu_sleep_loop` in the stub;
    no SWI decoding needed. **But the proposed trigger is cache fullness, and cache fullness
    never occurs** (`full=0`), so it would never fire. And the mechanism that *does* fire cannot
    be deferred to idle: an SMC store has already overwritten instructions that may execute on
    the very next branch, so postponing the invalidation means running stale translated code.
    Correct and feasible; aimed at a mechanism this workload does not exhibit.
  - *LRU / selective block eviction instead of nuke-and-pave.* Deferred. Beyond the
    back-reference graph problem (gpsp patches direct jumps between blocks), the **tag scheme
    itself cannot express it**: RAM code is tracked by one `u16` per halfword holding either a
    block-start tag or `0x0101` (`cpu_threaded.c:2485-2523`). A halfword covered both by a block
    starting there *and* by an earlier block spanning it can only store one of the two, so given
    a written address there is no way to find every block that must die. Finer-grained
    invalidation needs a **new** per-region block index, not a smarter walk of the existing one.

- **Consequences.** The build shipped tonight is measurement-only: no behaviour change, no memory
  change, no timing change. What it buys is that the next hardware log names the mechanism
  directly instead of leaving `ram=` ambiguous, and confirms (or refutes) on real silicon that
  `full` and `page` are zero there too. If hardware agrees, the only remaining lever on the core
  spike is a new per-region invalidation index — a real data-structure change, correctly out of
  scope for an overnight build.
- **Gates.** `run_boot_test.sh` PASS, `run_trade_test_psp.sh --radio=40` PASS (trade completed,
  parties swapped and saved on both sides), netdrv unit suite PASS, `run_save_test.sh` PASS.
- **Upstream impact.** Three `u32`s, two short wrapper functions, and a retargeted call in each of
  the four dynarec stubs. No behaviour change. Strictly more useful than ADR-0028's counter to
  any gpsp port on constrained hardware, and the same patch answers "is my JIT cache too small?"
  for every one of them.

## ADR-0030 — WHERE the SMC writes land: 99.6 % of the flush storm is ONE halfword. Add address profiling; it is over-tagged data, not overlay swapping

- **Status:** accepted 2026-08-02, branch `phase5f-smcaddr`. Extends ADR-0029; changes none of its
  decisions. Counters and logging only — **zero behaviour change**, same discipline as ADR-0029.
- **Context.** ADR-0029 established that every RAM translation-cache flush is SMC-triggered
  (`full=0`, `smc=4109`, `dma=1` per 600-frame window, 307 in the worst frame ≈ the 21 ms hardware
  spike). It could not say *why* the game hits translated code ~410x/second, and the two candidate
  answers wanted opposite work:
  1. **Real overlay swapping** — Pokémon genuinely copies code into IWRAM. Nothing to fix at the
     source; selective invalidation is the only lever.
  2. **False positives** — blocks over-extend across literal pools / embedded data, so ordinary
     *data* writes hit code tags. Then tighter block termination is a far cheaper fix.

- **Decision — bucket the written address, aggregate per heartbeat window, emit one line.**
  Two tiers, because one alone cannot decide it:
  - **Coarse:** a flat `u16[1152]` (2.3 KiB of `.bss`) indexed *exactly* — 128 pages of 256 bytes
    for IWRAM, 1024 for the EWRAM code window. No hashing, no collisions, no allocation. One
    saturating increment per event; the only loop runs once per window.
  - **Fine:** Space-Saving over 8 slots holding *exact* addresses. A heavy hitter survives every
    eviction; churn among one-off addresses shows up as a large `ovf`. At most 8 compares per event.

  ```
  EVT smc_addr win=<events> pages=<distinct> iw=<n> ew=<n> oth=<n> ovf=<n>
               hot=<page>:<n>,...(4)   top=<exact addr>:<n>,...(4)
  ```

  **One bucket hit per flush — the units are the point.** `win` must EQUAL `EVT core_prof`'s
  `win=.../smc` + `.../dma`, and it does, in every window of every run (73/15/1/4110 host,
  73/15/1/4122 join, 70 boot). Getting that invariant required a deliberate choice at the DMA
  sites: a DMA raises `CPU_ALERT_SMC` for *every* tagged halfword it crosses but costs exactly
  **one** flush, so the profiler records only the first (`alerts` is local to one `dma_tf_loop`
  call). The first cut counted per halfword and reported `win=1894` against 73 flushes — a single
  code DMA drowning the CPU-store events 25:1 and inventing a "scattered overlay copy" that does
  not cost anything. **If a future change makes `win` and `smc+dma` disagree, the profile is
  lying.** `oth` must likewise be 0; nonzero invalidates the IWRAM/EWRAM split and the addresses.

  **How the address reaches the profiler (MIPS).** At the SMC branch inside a store stub
  (`mips_emit.h` `emit_pmemst_stub`) `reg_rv` holds `lui_constant + masked_region_offset` and
  `reg_a0` holds the offset; both are dead afterwards because `smc_write` never returns to the
  memop caller, and neither is touched by `save_registers`. So `smc_write` passes them as two
  arguments (two `move`s and one `cfncall`, taken only on an actual SMC event) and the difference
  recovers the stub's `lui` constant, which names IWRAM vs EWRAM. The emitter **publishes** both
  constants (`smc_prof_lui_iwram/ewram`) rather than the profiler re-deriving the arithmetic, so
  the discriminator is exact by construction — and `oth=0` in every window of every run confirms
  it. `fnptrs` gains entry `# 12`; the DMA sites (`gba_memory.c`) call the profiler directly
  inside the existing alert branch. ARM/ARM64/x86 stubs are untouched.

- **The measurement, and it is emphatically not "overlay swapping".** PPSSPP rig,
  `run_trade_test_psp.sh --radio=40`, full autonomous Union Room trade, both consoles.

  | window | events (= flushes) | pages | iw / ew | ovf | hottest exact address |
  |---|---|---|---|---|---|
  | join, storm | 4122 | **2** | 4122 / **0** | **0** | **`0x03007D90` : 4096 (99.4 %)** |
  | host, storm | 4110 | **2** | 4110 / **0** | **0** | **`0x03007D90` : 4095 (99.6 %)** |
  | both, Union Room entry | 1 | 1 | 1 / 0 | 0 | `0x03004664` (one DMA) |
  | boot (solo, 3600 frames) | 70 | 1 | 70 / 0 | 0 | `0x03007D48`:31, `0x03007D3C`:25 |

  - **`ovf=0` in every window of every run.** The heavy-hitter table never had to evict, so every
    `top` count is **exact and complete**, not an estimate.
  - **EWRAM is never involved. `ew=0` everywhere, on both consoles.** This is entirely IWRAM.
  - **The storm is a single halfword.** `0x03007D90`, ~4095 of ~4110 flushes per 600-frame window.
    One instruction, one address, ~410x/second, each throwing away the entire RAM JIT cache.
  - **The code DMAs cost one flush each.** The overlay copy into `0x030046xx`-`0x03004Fxx` shows up
    as `dma=1` and one bucket hit. Real overlay swapping happens, and it is *free* by comparison —
    gpSP already handles a whole-range copy with a single flush.

- **Read of the evidence — hypothesis 2, and the target is one address.**
  A code copy cannot produce the storm's shape: its first store clears the tags, so the rest of
  the copy raises nothing (which is exactly what the `dma=1` rows show). Firing 4095 times on one
  halfword means the tag is *re-established between every pair of writes* — gpSP keeps
  re-translating a block covering `0x03007D90` while the game keeps writing that same location.
  That is over-tagged data: a location gpSP believes is code, written by something that is
  treating it as data.
  **Next step, and it is now small: find which block covers `0x03007D90` — where it starts, and
  whether that halfword is an instruction the CPU executes or trailing data the block scan walked
  into.** Killing this one address removes ~99 % of the steady-state flushes and therefore
  ~99 % of the 21 ms spike. Do that before attempting invalidation surgery.

- **Cost.** Measured, not asserted. Deterministic boot leg (3600 frames, 70 SMC events),
  A/B against `main` built from the same tree: per-frame core mean **identical in 4 of 6 windows**,
  the other two differing by **1 us** (5803->5804, 5640->5641 — integer-division rounding on the
  window total). Baseline `6467/6057/5803/5640/4649/4827`, instrumented
  `6467/6057/5804/5641/4649/4827`. In the storm windows the A/B delta has *opposite signs* on the
  two consoles and sits inside the +-40 us late-run rig noise, so it is not resolvable there.
  Analytic bound: at most ~70 Allegrex instructions per SMC event ~= **0.2 us**, i.e. **<= 0.3 % of
  the ~70 us flush it accompanies** and ~0.01 % of wall clock at 411 events/s. Memory: 2.3 KiB
  (page buckets) + 64 B (slots).

- **Design note for the next agent: "neuter, don't free" selective invalidation — VERIFIED, with
  three corrections.** The proposal is to patch a dead block's *entry point* to jump back to the
  dispatcher rather than reclaim its memory. Checked against the code:
  - **The load-bearing claim holds.** `allocate_tag_##type(tagp)` is called from
    `block_lookup_translate_##type(pc)` (`cpu_threaded.c:2565-2572`) with `tagp` = the tag halfword
    of the *looked-up PC*, and `VALID_TAG(tagn) = (tagn > 0x0101)`. A jump into the middle of
    translated code therefore reads `0x0101`, fails `VALID_TAG`, and allocates a **new** tag and a
    **new** block starting at that PC. And every direct block-to-block link is patched with the
    result of `block_lookup_translate_##type(branch_target)` (`:3168`, `:3327`), which is always
    either `ram_translation_ptr + block_prologue_size` (fresh) or
    `&ram_translation_cache[trentry->offset_##type]` (existing) — i.e. **always a block entry**,
    specifically the post-prologue address recorded in `ramtag_type`. So patching that one address
    catches every stale jump, and there is no back-reference graph to maintain. **Confirmed.**
  - **Correction 1 — the tag array is *not* "entries only".** `scan_block` calls
    `smc_write_##type##_yes()` for **every instruction of the block** (`cpu_threaded.c:2893-2922`),
    stamping `CODE_TAG_BLOCK16/32` (`0x0101`) on every halfword it covers. Only the *index* tags
    (`> 0x0101`) mark entries. This does not break the plan — it is what makes SMC detection work
    mid-block — but any statement of the form "tags mark entries" must be scoped to `VALID_TAG`.
  - **Correction 2 — invalidate by clearing `trentry->offset_##type`, not the tag.** Zeroing the
    offset makes the next lookup re-translate while **reusing the same tag**. This matters: tags
    are handed out from a stack that only counts down (`ram_block_tag`, `INITIAL_TOP_TAG` 0xFFFF
    down to `LAST_TAG_NUM` 0x0101, so **32 767 tags**, reset *only* by a full flush). If
    invalidation consumed a fresh tag per re-translation, 410 events/s would exhaust the tag stack
    in ~80 s and force exactly the full flush the change exists to avoid. Reusing the tag avoids
    that; the resource that then runs down is the 384 KiB code area, which is the correct and
    intended outcome (`flush_ram_full` would start being nonzero — today it is 0, so there is real
    headroom, as claimed).
  - **Correction 3 — the `0x0101` halfword tags have no refcount, and blocks overlap.** A mid-block
    entry creates a second block covering halfwords the first one also covers, and
    `smc_write_*_yes` only writes a tag when the existing one is `0`, so both blocks share the same
    stamps. Clearing a dead block's `0x0101` range would therefore silently un-tag halfwords still
    covered by a *live* block — a missed SMC alert, i.e. executing stale translated code, which is
    a correctness bug, not a performance one. The address-to-blocks index must either refcount the
    halfword tags or leave them set and accept spurious alerts on dead ranges.
  - Sizing (a page-bucketed array of `{start,end,entry}`, ~48 KiB) and the memory argument
    (`state_buf` is 416 KiB, deferred in ADR-0029) are unchanged and not re-litigated here.
  - **Not implemented.** Recorded only. And given the measurement above it is no longer the
    obvious first move: one halfword accounts for ~99 % of the cost, and a whole-range DMA copy —
    the case selective invalidation would help most — already costs only one flush.

- **Gates.** `run_boot_test.sh` PASS, `run_trade_test_psp.sh --radio=40` PASS (trade completed,
  parties swapped and saved on both sides), netdrv unit suite PASS. No soak and no save test —
  nothing save-adjacent is touched.
- **Upstream impact.** One `u16[1152]`, an 8-slot table, three short functions, two published
  constants in the MIPS emitter, one extra `fnptrs` entry, and two `move`s in `smc_write`. No
  behaviour change. Offered alongside ADR-0029's counter split in `docs/UPSTREAM-SMC-REPORT.md`.

## ADR-0031 — The SMC storm is genuine self-modifying code: Emerald builds a two-instruction function on its stack. Block termination and tagging are both innocent, and the flush costs ~1 us, not ~70

- **Status:** accepted 2026-08-02, branch `phase5g-smcblock`. Extends ADR-0030 and **corrects
  both** ADR-0030's read of the evidence and ADR-0029's cost model. Counters and logging only —
  **zero behaviour change**, same discipline as ADR-0029/0030.

- **Context.** ADR-0030 proved the storm is one halfword, `0x03007D90`, ~99.6 % of ~4110 RAM
  translation-cache flushes per 600-frame window. It read that as *"over-tagged data: a location
  gpSP believes is code, written by something that is treating it as data"* and set the next step:
  find the block that covers the halfword. Three candidate fixes were open, wanting very different
  work: (a) tighten block termination, (b) stop tagging halfwords that are not instructions,
  (c) selective invalidation (ADR-0030's deferred "neuter, don't free" design).

- **Decision — profile the covering BLOCK, the WRITER, and the price of the flush.** Four
  additions, all fixed-size `.bss`, all on paths that already cost a full cache flush:
  - **Which block.** At the end of every RAM block scan, `scan_block` has tagged exactly
    `[block_start_pc, block_end_pc)`. If that range overlaps a watched 256-byte page, record the
    start PC, the mode, the end the scanner settled on, and **why it stopped** — a new
    `scan_exit_reason` set at each of `scan_block`'s five exits (unconditional branch / MAX_EXITS /
    translation gate / MAX_BLOCK_SIZE / the `0x3007FF0` end-of-IWRAM clamp). A 256-byte page, not
    one address: the exact hot address moves with game state (`0x03007D3C/48/58` at boot,
    `0x03007D90` in a link session) and pinning one would have missed the runs that matter.
  - **Which writer.** `smc_write` already stores the GBA PC (`reg_a2` = storing instruction + 4 ARM
    / + 2 Thumb) into `reg[REG_PC]` before calling out, so the store profiler reads it for free.
  - **What it cost.** `flush_translation_cache_ram()` is timed directly (`fus`), its tag `memset`
    counted (`fkb`), and its whole-region fallback counted (`fwide`) — instead of inferring a
    per-flush price by dividing a worst-frame excess by the flush count, which is what ADR-0029
    did. Plus `xlat`, RAM blocks translated per window, which prices the re-JIT.
  - **Was the flush even necessary.** At each translation, keep a byte copy of the covering block;
    at the SMC event (the store is already committed — it sits in the SMC branch's delay slot)
    compare. `sil=<same>/<diff>/<unknown>`: `same` = the store wrote the bytes that were already
    there, so the translation was still valid and the flush bought nothing.

  ```
  EVT smc_block watch=<page> xlat=<n> ovf=<n> wovf=<n> fus=<us> fkb=<KiB> fwide=<n>
                sil=<same>/<diff>/<unknown>
                blk=<start>:<end>:<a|t>:<reason>:<n>,...  wr=<PC>@<addr>:<n>,...
  EVT smc_code  head@<start> <16 words>      (one-shot, for offline disassembly)
  ```

- **The measurement, and it settles all three candidates.** PPSSPP rig,
  `run_trade_test_psp.sh --radio=40`, storm window, host console:

  ```
  blk = 03007d90:03007d94:t:1:4096          wr  = 082e1a9c@03007d90:4107
  xlat = 5713   fus = 5097 us   fkb = 1072   fwide = 0   sil = 4095/14/0
  join console, same window:                             sil = 4095/14/10
  ```

  1. **The block STARTS at `0x03007D90`.** The hot halfword is the block's *first instruction*, not
     something a runaway scan walked into. The block is **4 bytes — two Thumb instructions** — and
     `reason=1`: `scan_block` ended it on an unconditional branch. **Every** covering block in
     every window of every run ended for reason 1; not one hit `MAX_BLOCK_SIZE` (4) or the
     `0x3007FF0` clamp (5). Block termination is exonerated by direct evidence.
  2. **The bytes are `7800 4770` = `ldrb r0,[r0]` ; `bx lr`** (`EVT smc_code head@03007dcc`). That
     is `ReadFlash1`, pokeemerald `src/agb_flash.c:105-108`. It is a real, complete, correctly
     terminated function. Tagging is exonerated too.
  3. **The writer is `SetReadFlash1+0x2c` = `0x082E1A9C`, 4107 of 4110 events.** That is the
     `while (i != 0) { *dest++ = *src++; i--; }` loop in `agb_flash.c:123-127`, which copies
     `ReadFlash1` into a **stack** buffer and points the `PollFlashStatus` function pointer
     (`0x03007844`) at it — so the flash driver never executes from the cartridge while the chip is
     in ID/poll mode. `0x03007D90` is stack: in the Emerald map the last symbol below it is
     `gRfuSIO32Id` at `0x030078A0` and the next above is `SOUND_INFO_PTR` at `0x03007FF0`
     (BIOS default SP_usr is `0x03007F00`).
  4. **The boot leg shows the same mechanism one level up.** `ReadFlash+0x64` (`0x082E1B38`, 56
     events) copying **`ReadFlash_Core`** — 0x22 bytes — into `readFlash_Core_Buffer[0x40]` at
     `0x03007D48`; that block is recorded as `03007d48:03007d6a:t:1:42`, i.e. 34 bytes, 42
     translations, again terminated on an unconditional branch. The second writer,
     `CgbSound+0x12/+0x68`, is the sound engine reusing the same stack depth for its own locals —
     a genuine stale-code write that genuinely needs the flush.
  5. **CPU, not DMA.** `win=0/0/4109/1/0` in the same window: 4109 CPU-store flushes against 1 DMA,
     and every writer PC above comes from the CPU store stub.

  **So the answer is (c), and not by elimination — by identification.** The address holds
  executable code and stack data at different times because **the game deliberately puts
  executable code on its stack**. Every flush in the storm is a true positive. Neither (a) nor (b)
  can help, and neither should be attempted.

- **And ADR-0029's cost model was wrong — the flush is cheap.** Measured, not inferred:
  - **`fus = 4229 us` for 4110 flushes = ~1.03 us per flush** on the rig, *including* the two clock
    reads the measurement itself adds. That is **0.13 %** of that window's core time
    (600 frames x 5423 us).
  - **`fkb = 1072` KiB over 4110 flushes = 267 bytes memset per flush**, and `fwide = 0` — the
    whole-region fallback (32 KiB IWRAM / 256 KiB EWRAM, taken when `code_max <= code_min`) never
    fires. Wiping the tag mirror costs nothing.
  - **`xlat = 5718` RAM block translations for 4110 flushes = 1.39 blocks re-JITted per flush.**
    "The entire RAM cache is thrown away" is true and nearly free: Emerald keeps almost no code in
    RAM — `SoundMainRAM_Buffer` (`0x03001AA8`), `IntrMain_Buffer` (`0x03002750`), and these stack
    thunks. Everything else runs from ROM, and the ROM cache is never flushed (`rom=0` throughout).
  - The storm window's mean core time (5423 us) is **lower** than a quiet window's (6385 us).

  ADR-0029 derived ~10.2 us/flush by dividing a whole worst-frame excess (8456 - 5325 us) by the
  307 flushes in it, i.e. it attributed the entire spike to the flushes a priori. The direct
  measurement is ~10x smaller. **The SMC storm does not explain the field's 21.3 ms `core` max**,
  and the hardware spike must be re-attributed before any more work is spent on invalidation.
  (Caveat kept honest: this is PPSSPP, not Allegrex. But the two components that would scale —
  a 267-byte memset and 1.4 small block translations — cannot plausibly reach 70 us each.)

- **What to do, and what not to do.**
  - **Do NOT build selective invalidation.** ADR-0030 deferred it as invasive; it is now also
    pointless — it would save ~1 us per event on a path costing 0.13 % of core time. And there is
    a second obstacle ADR-0030 did not record: **translated blocks are directly linked.** Every
    external block exit is patched with the raw code address returned by
    `block_lookup_translate_##type(branch_target)` (`cpu_threaded.c:3168`, `:3327`), so neutering a
    dead block's tag does not stop an already-patched direct branch from jumping straight into its
    stale code. Selective invalidation therefore needs the *linking* changed too (a back-edge list,
    or routing external exits through a trampoline), not just an address-to-block index. That is
    why upstream flushes everything, and on this evidence it is the right trade.
  - **If the storm ever does need to go, the lever is silent-store elision — and 99.7 % of the
    storm is elidable.** `sil = 4095/14/0` (host), `4095/14/10` (join), `4095/15/0` at
    `--radio=160`: **4095 of ~4109 SMC events wrote bytes that were already there.** The flash
    driver copies the same two halfwords from the same ROM address to the same stack address every
    time, so the code it "modifies" is byte-identical to the code that was translated, the
    translation stays valid, and the full-cache flush buys nothing. Only **14-15 events per window
    are real code changes** — the sound engine (`CgbSound`) reusing that stack depth, and those
    genuinely need the flush.
  - **How it would have to be done, and why it is NOT done here.** The obvious place — compare in
    `smc_write` — does not work: the store is emitted in the SMC branch's *delay slot*
    (`mips_emit.h`, `emit_pmemst_stub`), so the old value is already gone by the time C runs. Two
    real options, neither shipped:
    1. Move the compare into the emitted stub. Correct and cheap at runtime, but it is
       delay-slot surgery on the hottest path in the dynarec, replicated across four backends.
    2. A C-only, conservative variant: keep a small fixed table of recently translated **short**
       RAM blocks with a copy of their bytes; at an SMC event, elide the flush **iff** the written
       address lies inside a tracked block whose *whole* body still matches its copy. Soundness
       conditions, all of which the prototype instrumentation here already satisfies: refresh a
       block's copy at every translation; **drop every copy on a flush** (otherwise a pre-flush
       copy can vouch for bytes a post-flush translation never saw); compare the whole block, not
       the written unit (a block can be corrupted at one end and rewritten at the other); and
       treat "not tracked" as "flush", never as "elide". Overlapping blocks are safe because the
       test is really address-local: if the bytes did not change, *no* translation covering them
       was invalidated.
    **Neither is implemented, deliberately.** The measured payoff on the rig is ~0.13 % of core
    time, i.e. below what the rig can resolve, so the mission's bar — *implement only if it is
    clearly safe AND provable in the rig* — is not met in either direction. Revisit only if a
    hardware measurement of `fus` shows the flush is expensive on Allegrex in a way PPSSPP hides.
  - **The stack is already unsound, for the record.** `arm_block_memory` / `thumb_block_memory`
    with `rn == REG_SP` emit a direct `sw` with **no SMC check at all** (`mips_emit.h:1386-1397`,
    `:1723-1734` — *"Assume IWRAM, the most common path by far"*). So an ordinary `push` over a
    stack-resident thunk silently corrupts translated code today and raises nothing. It is benign
    here only because the game rewrites the thunk before every call. This shows up in the data as
    `sil` `diff` events: the bytes had already changed before the store that finally tripped the
    trap.

- **Cost of the instrumentation.** Two clock reads per flush (~4110/window), one range test and at
  most four compares per SMC event, a 4-slot block table with four 64-byte block copies, and one
  `scan_exit_reason` store per `scan_block` exit. The gates below are the A/B: the flush counters
  are unchanged (`win=0/0/4109/1/0` before and after) and `core=` stays inside rig noise.

- **Gates.** `run_boot_test.sh` PASS, `run_trade_test_psp.sh --radio=40` PASS,
  `run_trade_test_psp.sh --radio=160` PASS (`sil=4095/15/0`, `fus=5448` for 4110 flushes =
  1.33 us each, `wspike=0/0/279/0/0`), netdrv unit suite PASS (`ALL TESTS PASSED`). No save test —
  nothing save-adjacent is touched.
- **Upstream impact.** Diagnostic only; the finding is what matters upstream and is folded into
  `docs/UPSTREAM-SMC-REPORT.md`: the flush storm in Pokemon gen-3 is the flash driver's
  stack-resident `ReadFlash1`/`ReadFlash_Core` thunks, every flush is a true positive, and the
  flush itself is ~1 us — so a finer-grained invalidation is not worth building.

## ADR-0032 — Where the core's frame actually goes: bracket `retro_run` by phase. On the rig VIDEO is 59 % of it and the dynarec 36 %, and the phase maxima must never be summed

- **Status:** accepted 2026-08-02, branch `phase5h-corebracket`. Successor to ADR-0031, which closed
  the SMC line and left the field's `core=11390/21263` unattributed. Instrumentation only —
  **zero behaviour change**, same discipline as ADR-0029/0030/0031.

- **Context.** Three phases of work chased `flush_translation_cache_ram()`, and ADR-0031 priced it
  directly: ~1.0-1.3 us a flush, 0.13 % of core time, 1.39 blocks re-JITted. So the 21.3 ms hardware
  maximum belongs to something nobody had measured. Counters cannot answer this — only a clock
  inside `retro_run` can — and the candidate list (dynarec dispatch, per-scanline rendering, the
  end-of-frame blit, the audio mixer, the flash emulation the game polls constantly) spans four
  files and two threads of prior suspicion.

- **Decision — an EXACT partition of `retro_run`, levelled by probe cost.**
  Every microsecond of the call lands in exactly one bucket, and `tot` is cross-checked against
  `EVT core_prof`'s `core=` — the same call measured by two independent brackets and two clock
  sources, so the agreement is a check rather than a restatement.

  ```
  tot   the whole retro_run call
  |- emu  execute_arm{,_translate}()      the emulated frame
  |  |- vid   sum of update_scanline()    per-scanline rendering  (160/frame)
  |  |- amix  sum of render_gbc_sound()   GBC/PSG mixing, all 13 call sites
  |  |- dsnd  sum of sound_timer()        direct-sound FIFO drain (~448/frame)
  |  |- jit   sum of translate_block_*()  dynarec compilation
  |  \- cpu   RESIDUE: translated-code execution, block lookup + icache sync,
  |            memory stubs, timer/serial/DMA/IRQ bookkeeping, backup memory
  |- blt  video_run()                     post-process + video_cb
  |- aout audio_run()                     resample + audio_batch_cb
  |- rfu  rfu_frame_update()
  \- oth  RESIDUE: input poll, frameskip policy, option re-check, outer probes

  EVT core_phase lvl=<0-3> clk=<ns/read> f=<frames> rd=<reads/frame>
                 tot=<mean>/<max> cpu=... vid=... blt=... amix=... aout=...
                 dsnd=... jit=... rfu=... oth=...
                 worst=cpu:N,vid:N,blt:N,amix:N,aout:N,dsnd:N,jit:N,rfu:N,oth:N
                 cnt=<update_gba>/<dma>/<sound_timer> bk=<rd>/<wr> wbk=<rd>/<wr>
                 neg=<cpu>/<oth>
  ```

  - **THE PROBE COUNT IS THE BUDGET.** A clock read is ~0.5 us on the rig and ~1 us on Allegrex, so
    the profile is levelled and the level is a `config.ini` key (`core_phase`, default 2, no rebuild
    needed): **0** off (where every non-PSP frontend leaves it, 0 reads); **1** the coarse split
    only, ~10 reads/frame; **2** + `vid`/`amix`/`jit`, ~337; **3** + `dsnd`, ~1233. `rd` and `clk`
    are both in the line, so `rd x clk` prices it in its own units — and level 1 vs level 2 is a
    direct A/B that *measures* the cost instead of asserting it.
  - **`worst=` is the attribution; the `max` column must never be summed.** Each phase's `max` is
    its own worst frame, and those are different frames: in host window 1 the maxima sum to 16530 us
    against a 10442 us frame. `worst=` is the breakdown of the single frame that SET `tot`'s max,
    and its parts sum to it **exactly — checked, 31 windows out of 31, zero residue.**
  - **`neg=<cpu>/<oth>` guards the partition** and is not decoration — see the next section.
  - **The two brackets do NOT agree exactly, and the gap is named rather than waved through.**
    Over 31 windows (both consoles of the trade run plus the boot leg), `core - tot` is **always
    positive**, 2-34 us of mean and 2-174 us on a single frame. It has to be: `core=` is taken in
    `fe_host_run_frame` and strictly ENCLOSES `tot`, so it also contains fe_host's own two
    `time_us()` reads, up to two `core_counters()` callbacks, the call into `retro_run` — and any
    host-thread preemption landing between the two brackets, which is what the 174 us single-frame
    outlier is. Separately, `tot`'s mean exceeds the sum of the nine phase means by **2-4 us**,
    which is exactly the nine independent truncating integer divisions. Neither gap touches the
    attribution: `worst=` sums to `tot`'s max in **31 of 31 windows**, with no residue at all.
  - **Backup memory is COUNTED, not timed.** A save burst is 131072 `write_backup` calls; two clock
    reads each would add ~0.26 s to that one frame, so the probe would dwarf and distort exactly
    what it was measuring. `bk` is the per-frame call count and `wbk` the same two counts on the
    worst frame; the time itself stays inside `cpu`.

- **The instrumentation was wrong on its first cut, and its own guard caught it.** `neg` reported a
  negative `cpu` residue on 8 frames of a boot window. Cause: `translate_block_##type()` calls
  `block_lookup_translate_##type()` to patch its external exits (`cpu_threaded.c:3191`, `:3352`),
  which re-enters `translate_block_##type` — so an unguarded bracket counted every nested compile in
  both the inner and the outer accumulator. Boot window 1 read `jit=83/8730`; with the nesting guard
  it reads `jit=28/1943`, and `neg=0/0` in every window of every run since. The inner tier now uses
  `core_phase_enter/leave`, where only the OUTERMOST active bracket is timed (the depth is reset
  every frame, so a non-local exit cannot leak it). The same hazard exists at level 3 —
  `sound_timer -> dma_transfer -> write_io_register##tfsize -> render_gbc_sound` — and the same
  guard gives `dsnd` that mixing time rather than letting `amix` claim it a second time.
  **Without `neg` in the line this ADR would have reported a JIT cost 4.5x too large.**

- **The measurement.** PPSSPP rig, `run_trade_test_psp.sh --radio=40`, host console, a steady
  in-Union-Room window (600 frames). Level 2, so ~166 us of `vid` is its own 320 clock reads:

  | phase | mean us | % of frame | what it is |
  |---|---|---|---|
  | **vid** | **3235** | **48 %** | 160 x `update_scanline()` |
  | **cpu** | **2443** | **36 %** | dynarec execution + all unbracketed core work |
  | **blt** | **722** | **11 %** | `video_run()` — post-process + `video_cb` |
  | amix | 219 | 3 % | `render_gbc_sound()` |
  | aout | 72 | 1 % | `sound_read_samples` + `audio_batch_cb` |
  | jit | 0 | ~0 | steady state translates nothing |
  | rfu / oth | 3 / 3 | ~0 | emulated adapter; input + frameskip policy |
  | **tot** | **6700** | | against `EVT core_prof core=6702` |

  - **Video is 59 % of the core frame** (`vid + blt`), ~57 % after deducting the probe from `vid`.
    This is the largest single finding and it is not where three ADRs of work have been looking.
  - **`blt` is 722 us and almost perfectly flat** — max 852 in every window of every run, session or
    not, boot or trade. It is a fixed 240x160 cost, and on PSP it is followed by our own
    `video_psp.c` upload, which is OUTSIDE `retro_run` and therefore not in this line at all.
  - **Audio is 291 us total (4 %).** ADR-0028 halved the sample rate for ~5 % of the mean and that
    was worth having, but there is nothing left here. `dsnd` reads 461-470 us at level 3, and the
    level-2/3 A/B shows `cpu` did not shrink by one microsecond while the frame grew by exactly the
    probe cost — so the true direct-sound FIFO drain is **below the resolution of a 1 us clock at
    448 calls a frame**, and level 3 need not be run again.
  - **`cnt=671/29/448`** — the dynarec returns to C ~671 times a frame. That round trip is a
    plausible component of `cpu` and is the next thing to bracket if `cpu` has to be split further.
  - **Which phase owns the worst frame depends on the window, and `worst=` says so directly.**
    Boot window 1: `tot` max 10349, `worst=vid:7573` — video owns it outright. The SMC-storm window
    (`win=0/0/4110/1/0`): `tot` max 9806, `worst=cpu:6160,jit:1310,vid:1373` with `wbk=308/2472` —
    the worst in-session frame is a **flash save-write burst plus the dynarec re-compilation it
    triggers**, and video is *below* its own average on that frame. The boot leg's worst frame is
    the save READ: `wbk=18703/16`, `cpu:6703`, i.e. ~0.26 us per `read_backup`. So the flash path is
    cheap per call and expensive in bursts — the shape a counter reveals and a mean hides.

- **Cost, measured three ways and self-consistent.** Deterministic boot leg (3600 frames), one
  build, `core=` mean per window from `EVT core_prof`; `clk=519` ns measured on the rig:

  | level | reads/frame | `core=` mean, 6 windows | delta vs previous | predicted `rd x clk` |
  |---|---|---|---|---|
  | 0 | 0 | 6615 6194 5945 5781 4790 4966 | — | — |
  | 1 | 10 | 6620 6200 5950 5787 4795 4972 | **+5 / +6** | +5.2 |
  | 2 | 337 | 6791 6372 6120 5958 4969 5145 | **+171 / +173** | +170 |
  | 3 | 1233 | 7260 6840 6589 6427 5437 5613 | **+467 / +469** | +465 |

  Every delta lands on its prediction. Level 1 costs **0.1 %** of the frame; level 2 **2.6 %**, of
  which ~166 us sits inside `vid` (320 of its 337 reads) and inflates that phase by ~5 %. Anything
  not actively being profiled should set `core_phase=0`.

- **Rig vs hardware — say it plainly.** The rig `core` max is **10.4-10.6 ms**; the field is
  **21.3 ms solo and 26-29 ms in session**. The rig does not reproduce the spike and never has, so
  **the table above is a hypothesis about the field, not a measurement of it.** It is a well-founded
  one: PPSSPP JITs Allegrex to x86 and models neither the 8/16 KiB caches nor uncached write cost,
  and the two phases that would suffer most from that are precisely the two largest — `vid` writes
  240x160x2 bytes of framebuffer per frame through the scanline renderer and `blt` reads all of it
  back. If anything the field's video share should be **higher** than 59 %, not lower.
  **The next action is a hardware run with `core_phase=2`, and nothing should be built until that
  line comes back.** If it confirms video, the levers are known, cheap and none of them are the
  dynarec: the scanline renderer's per-mode paths, and whether `blt`'s post-process can be elided.

- **Gates.** `run_boot_test.sh` PASS, `run_trade_test_psp.sh --radio=40` PASS (trade completed,
  parties swapped and saved on both sides; `neg=0/0` in all 13 windows on both consoles, `worst=`
  summing to `tot`'s max in 31/31 windows, and `core - tot` positive and bounded as above), netdrv
  unit suite PASS (`ALL TESTS PASSED`). No save test — nothing save-adjacent is touched.
- **Upstream impact.** Six accumulators and a nesting depth in `.bss`, four bracket pairs, one
  wrapper around `render_gbc_sound`, and four call-count increments. `core_phase_lvl` is 0 unless a
  frontend sets it, so every other libretro frontend compiles and runs exactly as it did.

## ADR-0033 — The requirement changed: a wireless session clamps both consoles to a FIXED frame rate. No negotiation, no control loop, no hunting

- **Status:** accepted 2026-08-02, branch `phase5i-fixedrate`. Supersedes ADR-0027/ADR-0028 as the
  *default* session pacing policy; that matcher is kept intact and selectable, not deleted.
  **AMENDED BY ADR-0035: the DEFAULT RATE IS WRONG BELOW.** The mechanism in this ADR is
  hardware-proven, but 40.00 fps is unreachable by construction — whole-vblank pacing can only
  hold `59.94/N`, and the field achieved 35-38 against a 40.00 clamp. The default is now
  **29.97** (two vblanks) and any configured rate is snapped and logged. Read ADR-0035 §1 before
  acting on any number in this ADR.

- **Context — the user changed the requirement, and that is the whole reason this exists.**
  Full-speed emulation *during a wireless session* is now explicitly a **nice to have**, not a
  requirement. In the user's words: *"trading is a temporary activity... If the game only runs at
  half speed during trades, that's fine, so long as it runs normally when our wireless functions
  are disabled... so long as we can limit how choppy it is, and as long as it will accept our
  inputs."* Outside a session the emulator must stay full speed, and it already does (58.6 fps
  solo). **Nothing below is a performance finding; it is a requirements change, and the design
  follows from it.**

- **Why the adaptive matcher is now the wrong shape.** ADR-0027/0028's controller derives its
  target from each peer's *measured capability*, and the field showed that capability genuinely
  fluctuates with game workload — `peer_cap` walking 49.9, 44.7, 41.6, 38.9, 41.1, 47.0, 51.0,
  53.7, 55.4, 56.6, 52.6 within seconds, ~20 fps of swing. ADR-0028's low-water marks damped that
  but could not remove it, because the input really is moving. So the applied `target` walks
  41 → 59 → 45 → 51 for the entire session. **A moving applied rate is itself a desync source** —
  Gen-3's RFU counts link timeouts in FRAMES, so what the link needs is not the *right* rate, it is
  a rate that HOLDS STILL and is the SAME on both consoles. The controller delivers neither, and
  under the new requirement it is buying accuracy nobody asked for at the cost of the one property
  that matters.

- **And the hardware profile says the hunt can never converge.** In-session steady state, PSP-3000
  in the joining seat (facts, measured, not re-derived here): `cpu 7754 (54 %) vid 3069 (22 %)
  blt 2973 (21 %) audio 436 (3 %)` — **14253 µs of a 16750 µs budget, 85 % full before the
  frontend does anything.** The host-vs-join difference is entirely `cpu` (6071 → 7754, +28 %):
  the **game's own RFU driver code** running on the emulated CPU, because the client half does
  more work. That is not our code and not ours to optimise. Capability therefore swings 45-58 fps
  on whichever console is joining while the host holds 58-59, and it will keep doing so. A
  controller whose setpoint is derived from that number is chasing the game's own workload.

- **Decision: while a session is live, both consoles clamp to the same fixed frame rate,
  `config.ini net_session_fps`, default 40.00.** Chosen to sit comfortably below what *both*
  consoles sustain (45-58) so neither side is ever the binding constraint. It is a constant, so
  both sides compute it identically with **no negotiation, no exchange, and no control loop**.
  Outside a session, nominal 59.7275 as before.
  - **Ramp, both ways.** A step from 59.73 to 40.00 at session start is a stall by another name —
    the frame it lands on is ~8 ms longer, and the audio step jumps with it. The applied rate
    glides at **4.00 fps/s** (~5 s in, ~5 s out) on a per-frame time base, not the adaptive path's
    1 s windows. **The glide out runs AFTER teardown**, which is why the throttle is gated on the
    applied target and explicitly *not* on `g_net_up`: cutting the throttle the instant the
    session ends, while the audio step is still seconds from nominal, is the ring-overflow
    ADR-0027 §audio already warned about.
  - **Audio follows the applied rate exactly**, unchanged in kind from ADR-0027: production and
    consumption match, the stream stays continuous, and the pitch drops proportionally (~7
    semitones at 40.00). **The user explicitly approved the pitch drop.**
  - **A console that cannot hold the rate is REPORTED, NOT CHASED.** `EVT session_pace_miss
    actual=… fixed=… self_cap=… — not chasing`, at most one line per 10 s, re-armed when it
    recovers. Chasing downward would reintroduce exactly the moving target this ADR removes, and
    the user prefers a known steady rate to a correct-but-moving one. The check only runs once the
    glide has settled, because during the ramp the achieved rate is legitimately not the clamp yet.

- **`net_pace_match` IS REMAPPED, AND THE REMAP CHANGES THE MEANING OF AN EXISTING KEY. Read this
  before reading any older log.**

  | value | before ADR-0033 | from ADR-0033 |
  |---|---|---|
  | `0` | off — both consoles free-run | **unchanged** |
  | `1` | **adaptive matcher (ADR-0027/0028)** | **fixed-rate clamp — DEFAULT** |
  | `2` | *(invalid, clamped to 1)* | **the adaptive matcher, verbatim, for the A/B** |

  An existing `config.ini` carrying `net_pace_match=1` therefore **silently adopts the new
  policy**. That is deliberate: `1` still means "pace during a session", and only the *how*
  changed. Anyone wanting the old behaviour must now write `2`. The value is echoed with a name in
  the log so no field log is ever ambiguous about which policy ran:
  `EVT net_pace_match mode=1 policy=fixed nominal=59.73 floor=40.00 fixed=40.00 ramp=4.00`.
  Keeping `2` alive is not politeness — it is the only way to A/B "steady 40" against "accurate
  but moving" on real consoles, which is a question only the field can settle.

- **New config surface.** `config.ini net_session_fps` (and the same key in `autopilot.ini`), a
  **decimal** — `40`, `40.5`, `40.00` all parse; anything else falls back to the default rather
  than half-parsing, because a silently-wrong session rate is a desync the log would not explain.
  Clamped 20.00-59.7275: above nominal is not a clamp, and below 20 the audio pitch drop stops
  being a tape slowdown and starts being a different instrument.

- **New log lines.** `EVT session_pace fps=<applied goal> reason=<session_start|ramp_done|
  session_end|ramp_done_nominal>` — four reasons rather than the two the brief named, because
  "the clamp was announced" and "the console actually reached it" are different claims and the
  field has to be able to tell them apart. `EVT session_pace_miss actual=…` as above. `EVT
  sess_cost … pace=target/self_cap/peer_cap/engaged` is unchanged in shape and still populated:
  the peer's advertised capability is still *read* in fixed mode even though nothing is negotiated
  from it, because "what could the pair actually have sustained" is the one question a fixed clamp
  cannot answer for itself, and it is what tells the user whether 40.00 was a good number.

- **Consequences.** Every session costs ~33 % of nominal speed, including healthy ones. Per the
  changed requirement that is the trade being chosen, not a regression. The ADR-0027 measurement
  machinery (capability EMA, `fe_np_set_local_fps`, the `ND_T_PING` fps payload, the low-water
  marks) is **retained and still runs** — mode 2 needs all of it, and mode 1 needs the window to
  compute its achieved rate for the miss check. `pace_window()` is the shared half; the policy
  halves are separate and neither can perturb the other.
  **What the rig can and cannot prove:** the rig runs both instances at full speed, so it does
  **not** reproduce the field's host/join asymmetry and never has. What it *can* prove — and now
  does, because unlike the adaptive matcher a fixed clamp is **not** inert on symmetric peers — is
  that the policy comes up, that both sides announce the *bit-identical* constant (a mismatch
  fails the gate outright), that the glide into the clamp completes, that it is released at
  teardown, and that no `pace_match` control-loop line is emitted in mode 1.

- **Alternatives.** *Keep the adaptive matcher and damp it harder* — more filtering on an input
  that is genuinely moving buys lag, not stability, and the requirement no longer values the
  accuracy it is paying for. *Negotiate the fixed rate between peers* — a handshake to agree on a
  constant both sides already have in a config file, plus a version-skew failure mode, for nothing.
  *Clamp only the faster console* — that is a control loop again, and the two consoles trade the
  slow role (ADR-0028), so it reverses direction mid-session. *Pick 45 or 50* — inside the
  measured 45-58 join-seat band, so the join console would sometimes be the binding constraint and
  the clamp would silently stop being a clamp; 40 is below the whole band with margin.
  *Chase downward on a miss* — rejected above, by the user's stated preference.

- **One real bug this shipped and then caught, recorded because the shape recurs.** The glide has
  to run when no session is up (the glide *out* happens after teardown), so `pace_fixed_ramp()` is
  called from the first frame — at which point `goal` was still its `.bss` zero, which reads as
  "ramp to 0 fps". A solo game was throttled to the `PACE_MAX_EXTRA_VB` floor with no session in
  sight: `EVT fps emu=29.89` in `run_boot_test.sh`, wireless never touched. Fixed by reading an
  unset goal as nominal; `emu=59.94` after. **The gate caught it only because `EVT fps` is logged
  solo as well as in-session** — a session-scoped counter would have missed it entirely, which is
  the same argument ADR-0028 made for logging `core_prof` outside sessions.
- **Gates** (all on the shipped EBOOT): `run_boot_test.sh` PASS (`emu=59.94` solo — no pacing
  outside a session), `run_trade_test_psp.sh --radio=40` PASS (trade completed, mons swapped and
  saved both sides, `retx/acked=0 %`, both consoles `pace=40.00/59.1x/58.8x/1`), `--radio=160`
  PASS (timing touched, so the queue-stall profile is required), netdrv unit suite PASS. No soaks.
- **Upstream impact:** None. Frontend-only; netdrv and the core are untouched.

## ADR-0034 — Put the blit's staging buffer in VRAM: 2513 → 1800 µs per frame, and most of the win is the GE, not the copy

- **Status:** accepted 2026-08-02, branch `phase5i-fixedrate`; **decided by hardware 2026-08-02
  and rewritten in place.** The instrumentation and the three-way lever were the original
  decision; the *default* is now `blit_mode = 2` (VRAM). Two of this ADR's original conclusions
  were wrong and are corrected below rather than quietly dropped — the wrong ones are the
  interesting part.

- **THE HARDWARE VERDICT.** PSP-1000, same console, same route per run, 6-8 windows of 600
  frames per mode, spread within ~6 µs (µs/frame):

  | `blit_mode` | `stage` | `gu` | `tot` | vs baseline |
  |---|---|---|---|---|
  | 0 cached RAM + 82 KiB writeback | 1337 | 1176 | **2513** | — |
  | 1 uncached mirror | 1041 | 1174 | **2215** | −298 µs |
  | **2 VRAM (default)** | 1071 | **728** | **1800** | **−713 µs** |

  **713 µs/frame back — 4.3 % of a 16.74 ms frame — and it is not where either of us looked.**

- **Correction 1: `blit_mode` was supposed to move `stage` only. It moves `gu` more.** Both this
  ADR and ADR-0035 §2 stated that a staging *placement* cannot touch the GE wait, so the lever
  addressed "at most ~54 %" of the blit. **Falsified.** `gu` fell **1176 → 728 (−38 %)** in mode
  2, because the GE fetches its source texture out of VRAM far faster than out of main RAM — the
  buffer's location changes the *GE's* read cost, not just the CPU's write cost. Meanwhile mode
  2's CPU side is marginally **worse** than mode 1's (1071 vs 1041: uncached main-RAM writes beat
  VRAM writes), and it does not matter at all, because the GE saving dwarfs it. **A lever aimed at
  one half of a cost turned out to work mainly on the other half.**

- **Correction 2: the original headline — "the blit is the CPU copy, not the GE" — was wrong,
  twice over.** It came from the rig's `stage` 97 % / `gu` 3 %. Hardware says ~54 % / ~46 %
  (ADR-0035 §2), and now says the GE half is also the *movable* half. The title of this ADR has
  been changed to say what is actually true.

- **This is the THIRD rig-derived conclusion overturned by hardware on this project**, and the
  three failure modes are all different, which is why the rule keeps having to be relearned:
  1. ADR-0028 — rig `core` max 10.5 ms vs field 21.3 ms: a *magnitude* wrong.
  2. ADR-0035 §2 — `stage`/`gu` 97/3 vs 54/46: a *ratio* inverted.
  3. **Here — "the modes cannot affect `gu`": a *causal claim* falsified.** The rig could not
     have caught this one even in principle: it reported all three modes identical **to the
     microsecond**, which was its own confession that it models none of the relevant physics.

  **Standing rule, now earned three times: the rig proves correctness and control flow. It
  prices NOTHING that touches caches, uncached memory, or the GE — and it cannot be trusted about
  which component a change will even affect.**

- **Credit: VRAM was the user's suggestion.** It was raised earlier and initially rejected —
  correctly — for the *translation cache*, because the Allegrex cannot fetch instructions from
  VRAM. That rejection was right about the target and was then allowed to stand as though it were
  about the idea. Nobody re-examined VRAM for the *framebuffer*, where there is no instruction
  fetch and the GE is the main consumer, until much later. **The principle was sound; only the
  first target was wrong.** Worth remembering as a review habit: when an idea is rejected, record
  *what* it was rejected for, so the rejection does not silently generalise.

- **Consequence for session pacing, and it is a big one.** The PSP-1000's per-frame work sat at
  roughly 14.2 ms core + 2.5 ms blit ≈ **16.7 ms, essentially exactly one 16.743 ms refresh** —
  which is *precisely* the condition ADR-0035 §1 identified as making 40 fps unreachable (no frame
  fits inside one vblank, so every frame costs two). Dropping the blit to ~1.8 ms puts total work
  **meaningfully under one refresh for the first time**. So the fixed-rate clamp may no longer be
  stuck at 29.97: a 1.5-refresh average near 40 becomes physically possible. **This must be
  re-measured, not assumed** — see the next-run list below.

- **Context.** The hardware profile puts `blt` at **2973 µs mean / 3530 max, 21 % of every frame**,
  and unlike `cpu` (the game's own RFU driver on the emulated CPU) **this one is our code** —
  `psp/video_psp.c` converts 240x160x2 = 76 800 B of libretro RGB565 into `fb_staging` in PSP 5650
  channel order, then calls `sceKernelDcacheWritebackRange` over 82 432 B. Even 1.5-2 ms back is
  ~10 % of the frame, and ADR-0033 has just spent 33 % of session speed buying stability, so the
  frame budget is exactly where the remaining wins have to come from.

- **First: the measurement, because "blt" was two different things wearing one name.** ADR-0032's
  phase bracket reported `blt` as a single number (`video_run()` — post-process + `video_cb`).
  `video_cb` lands in our `vid_draw_frame`, which does two things with completely different cost
  models: a **CPU copy** and a **`sceGuSync` that blocks until the GE has finished rasterising**.
  A single number cannot tell those apart, and the three candidate fixes only address the first.
  So `vid_draw_frame` now brackets them separately and `EVT blit_prof mode= name= n= stage=mean/max
  gu=mean/max tot=` reports both on the same 600-frame cadence as `core_prof`.

  **Rig result, 600-frame windows, in-game (`run_boot_test.sh`):**

  ```
  EVT blit_prof mode=0 name=cached n=600 stage=700/829 gu=22/23 tot=722
  ```

  `tot=722` lands exactly on ADR-0032's rig-measured `blt` of 722 µs, which is a clean cross-check
  of two independently written brackets — that part held up. **Everything the rig said about the
  RATIO did not**: `stage` 97 % / `gu` 3 % here, against ~54 % / ~46 % on hardware. The conclusion
  drawn at the time — "the GE and the `sceGuSync` wait are not the problem, the hunt is the CPU
  copy" — **was wrong, and is left standing here on purpose** so the reasoning that produced it
  can be seen. **Splitting the bracket was still the right call**: it is what made the hardware
  answer legible when it arrived, and a single `blt` number could never have shown that the win
  was in `gu`.

- **Decision 1 — bracket, ship the alternatives, and let the field choose.** `config.ini blit_mode`
  selects where the staging buffer lives:

  | mode | placement | writeback | rationale |
  |---|---|---|---|
  | `0` | cached RAM | `sceKernelDcacheWritebackRange`, 82 KiB/frame | the original path |
  | `1` | same RAM via the uncached mirror `0x40000000\|addr` | **none needed** | stores go straight to memory, so the 76.8 KiB never crosses the bus twice and 1288 cache-line flushes disappear; pays uncached store latency instead |
  | `2` **default** | VRAM (`sceGeEdramGetAddr()` + a bump above the display and depth buffers), uncached | **none needed** | no writeback *and* the GE reads its texture from local memory — the second half is what actually won, and the guessed drawback (slower CPU writes to VRAM) is real but irrelevant |

  Mode 2 bump-allocates at `FB_BYTES * 3` (816 KiB of 2 MiB used by two display buffers plus depth),
  64-byte aligned, and **checks `sceGeEdramGetSize()` rather than asserting**: if it will not fit it
  falls back to mode 0 rather than scribbling on the framebuffer. `EVT blit_mode req= mode= name=`
  logs the mode **actually in force**, not the one requested — a field log that reported the request
  would be a lie the moment the fallback fired.

- **Decision 2 (SUPERSEDED — the default is now 2; kept because the reasoning was right at the
  time and the evidence in it is still the best argument against ever trusting the rig here)
  — the default does NOT change, and that is the point.** **PPSSPP models neither
  Allegrex's caches nor uncached write cost** (ADR-0032 says this outright, and it is why the rig
  `core` max is 10.5 ms against the field's 21.3 ms). So the rig's `stage` number for modes 1 and 2
  is *not evidence about hardware*, and picking a new default from it would be exactly the
  speculative timing change HANDOFF forbids — "speculative timing patches are how the ARQ storm
  survived Phase 4" (ADR-0017). The lever exists so the user can A/B all three **on the consoles,
  in one sitting, with no rebuild**, exactly like `net_tx_thread` / `log_thread` / `sram_thread` /
  `core_phase`. `EVT blit_prof`'s `stage=` is the number that decides it.

  **And the rig proved its own blindness, which is the cleanest possible justification for not
  choosing here.** The three modes measured on PPSSPP:

  ```
  EVT blit_prof mode=0 name=cached   n=600 stage=699/829 gu=22/23 tot=721
  EVT blit_prof mode=1 name=uncached n=600 stage=699/829 gu=22/23 tot=721
  EVT blit_prof mode=2 name=vram     n=600 stage=699/829 gu=22/23 tot=721
  ```

  **Identical to the microsecond, including the maxima.** Deleting an 82 KiB cache writeback and
  moving every store to an uncached mapping changed *nothing*, because on a desktop host there is
  no Allegrex cache to write back and no uncached store to be slow. Anyone tempted to pick a
  default from a rig number should read those three lines first.

- **Correctness IS settled on the rig, and separately from cost.** softgpu renders into emulated
  VRAM, so `run_gu_color_test.sh` — which asserts 8 known RGB565 bars through the production blit
  path against a raw drawbuffer readback, and then that an in-game GE region is pixel-identical to
  the core buffer — proves that writing through the uncached mirror or into VRAM produces
  **byte-identical output**. It takes `--blit-mode=0|1|2` and all three legs pass, with mode 2
  logging `mode=2 name=vram` (i.e. the VRAM bump allocation succeeded and did not fall back).
  Cost is a hardware question; correctness is not, and the two are gated separately.

- **A trap closed in passing.** Switching modes leaves the RAM buffer holding dirty cache lines
  from the previous mode; one of those evicting later over an uncached write is a corrupted frame
  that would look like a GE bug, with the aliasing invisible in the source. `vid_set_blit_mode()`
  therefore does a **writeback-invalidate** of the RAM buffer on every mode change, in every
  direction.

- **Consequences.** Two `sceKernelGetSystemTimeLow()` reads per blitted frame (~1 µs each on
  Allegrex, ~0.3 % of the blit) — cheap enough to leave on permanently, and `blit_prof` is the only
  reason this A/B could be decided at all. `blit_mode` persists through `pcfg_save` and overrides
  from `.gpsp-harness.ini`. VRAM use rises by 82 KiB (of ~1.2 MiB free above the display and depth
  buffers); the size check and fallback mean a console with a different VRAM layout still boots,
  and `EVT blit_mode req=2 mode=0 name=cached` would say so rather than failing silently.

- **THE REMAINING LEVER, and why it is not built yet.** Even in the winning mode `gu` is **728 µs
  of the CPU doing nothing but waiting for the GE**, every frame. With the texture already in
  VRAM, overlapping that wait with the next emulated frame is the only lever left on this path,
  and it is now *more* attractive than before, not less — 728 µs is still 4.3 % of a frame.
  The design: **double-buffer `gu_list`**, kick the display list, run the next `retro_run`, and
  `sceGuSync` at the last possible moment before `sceDisplayWaitVblankStart()`.
  **The blocker is real and unchanged:** `gu_list` is a single static buffer and `osd_draw()`
  starts a second list into it in the same frame, so today the GE would still be reading the array
  while the CPU overwrites it — silent, intermittent display corruption, the worst possible
  failure to debug. **Do not build it blind.** The rig cannot see GE timing at all (it reported
  all three modes identical to the microsecond), so this needs designing properly and gating on
  hardware, exactly like the placement A/B was.
- **What the next hardware run must bring back:** (a) `EVT blit_mode req=2 mode=2 name=vram` —
  confirmation the VRAM bump succeeded on *both* consoles, since only the PSP-1000 has been
  measured; (b) `EVT blit_prof` in-session as well as in-game, because every number in the table
  above is from solo play and the session adds transport work; (c) **crucially, a re-test of
  `net_session_fps`** — see the pacing consequence above. The blit saving may have moved the
  console back across the one-refresh boundary, in which case 29.97 is now needlessly slow.
- **Alternatives.** *`sceGuCopyImage` (GE does the copy)* — the GE cannot do the R/B field swap the
  PSP 5650 texture format requires, so the CPU still has to touch every pixel; it would move the
  copy, not remove it. *Elide the conversion by changing the core's output order* — a core change,
  out of scope, and it would fork us further from upstream for a frontend problem. *Uncached main
  RAM (mode 1)* — measured, real (−298 µs), and beaten by VRAM by another 415 µs; kept as the
  fallback-of-choice if VRAM ever has to be given up.
- **Gates** (all on the shipped EBOOT). `run_boot_test.sh` PASS,
  `run_gu_color_test.sh --blit-mode=0` / `=1` / `=2` **all three PASS** (bars exact + in-game GE
  region pixel-identical to the core buffer, 38 400 px) — **the correctness of the new default was
  gated before it became the default**, which is the only reason a placement change to the display
  path is safe to ship on one console's timing numbers.
  `run_trade_test_psp.sh --radio=40` and `--radio=160` PASS; netdrv unit suite PASS.
- **Upstream impact:** None. `psp/` only.

## ADR-0035 — The field answers ADR-0033/0034: 40.00 fps is unreachable by construction (snap to whole vblanks), the blit ratio INVERTS on hardware, and a missing Makefile dependency shipped a silent struct-layout corruption

- **Status:** accepted 2026-08-02, branch `phase5i-fixedrate`. Three corrections from the first
  PSP-3000 run of Build F (`05345990`). Successor to ADR-0033 and ADR-0034 on the points below;
  everything else in those two stands.

### 1. The fixed rate works. The NUMBER was impossible.

- **What the field showed.** `EVT session_pace fps=40.00 … ramp_done` fired correctly, and
  `session_pace_miss … — not chasing` fired repeatedly **with the target never moving** — the
  no-chase rule is now hardware-proven, which was the point of writing it. But the achieved rate
  sat at **35-38 fps against the 40.00 clamp, with `self_cap=52-57`**: the console reported it
  could go *faster* than the rate it was failing to hold.
- **That is not a contradiction and not a control bug — it is quantization, and it was latent in
  ADR-0027's fractional-vblank accumulator from the day it was written.** We pace by waiting whole
  PSP vblanks (16.68 ms), so a frame costs 1 or 2 of them. Averaging 40.00 requires ~half the
  frames to finish *inside one vblank*. When per-frame work sits just **above** 16.68 ms — which
  is exactly where the profile put it, `cpu 7754 vid 3069 blt 2973 audio 436` = 14.3 ms plus our
  frontend — **no frame can**, every frame becomes 2 vblanks, and the mean collapses toward 29.97
  with jitter from frames that occasionally take 3. `self_cap` is derived from *work* time and so
  is blind to this: it correctly says "this console could free-run at 52-57", and the throttle
  correctly cannot deliver 40. Both numbers are right; the target was the wrong shape.
- **ADR-0028's low-water machinery hid this for the adaptive matcher** because its target was
  always moving anyway — a target that never settles never exposes that its settling point is
  unreachable. Fixing the rate is what made the quantization visible. **The bug was older than
  the feature that revealed it.**

- **Decision: the requested rate is SNAPPED to a rate the throttle can actually hold, and the snap
  is logged.** The rates that need no headroom assumption are exactly `PACE_VBLANK_HZ / N` —
  **59.94, 29.97, 19.98** — and `N` is capped at 3 because `PACE_MAX_EXTRA_VB` is 2 (a snap the
  throttle could not deliver would be the same lie in a different place). `net_session_fps`
  default moves **40.00 → 29.97 = exactly two vblanks**. `EVT session_pace_snap req=40.00
  applied=29.97 vblanks=2` fires whenever the snap moves the number, and `EVT net_pace_match` now
  carries `fixed=` (applied) beside `req=` — a line showing only the request would be the same
  silent lie the field just caught.
  - **Nearest by error in the RATE, never by rounding the divisor.** `59.94/40.00 = 1.4985`;
    rounding *that* to N=1 hands back nominal and silently cancels the clamp. The search compares
    `|59.94/N − req|` over N ≥ 2 instead, and **never snaps back up to nominal** once the user has
    asked for pacing — a request within the engage band of nominal is not a request to pace at
    all and is left alone.
  - **The achievable set is sparse and there is deliberately nothing between 59.94 and 29.97.**
    With whole-vblank pacing there cannot be. Anything in between is reachable *only* when frames
    genuinely fit in one vblank, which is the assumption the field falsified.

- **CORRECTION FOR THE FIELD CONFIG: two vblanks is 29.97, not 29.86.** 29.86 is 59.7275/2 — the
  *GBA's* frame rate halved — but the thing we insert is a **PSP display vblank at 59.94 Hz**, so
  the achievable rate is 59.94/2 = 29.97. The consoles are currently configured to 29.86; that
  now **snaps to 29.97 and says so in the log**, so the run is still valid and the 0.11 fps needs
  no second mechanism. It is called out here because a reader who believes 29.86 is exact will
  otherwise read the applied 29.97 as drift.

- **Kept switchable: `net_session_fps_snap` = 1 on (default) / 0 off.** With `0` the raw request
  goes to the fractional accumulator unchanged. This exists so the field can A/B **"steady 29.97"
  against "nominally 40, actually 35-38 with jitter"** — that is the comparison this ADR is an
  argument about, and it should stay possible to run rather than only to assert.

### 2. The blit ratio INVERTED on hardware. Half the cost is the GPU wait, and the placement A/B cannot touch it.

- ADR-0034 measured `stage` (CPU convert + writeback) at **97 %** and `gu` (list build +
  `sceGuSync`) at **3 %** on the rig, and concluded the hunt was the copy. **Hardware, mode 0:**

  | | rig | **PSP-3000** |
  |---|---|---|
  | `stage` mean/max | 700 / 829 | **1338 / 1583** |
  | `gu` mean/max | 22 / 23 | **1140 / 1731** (rising to 1595 mean later) |
  | `tot` | 722 | **2479** |
  | split | 97 % / 3 % | **~54 % / ~46 %** |

- **PPSSPP cannot model a real `sceGuSync` blocking on real graphics hardware** — its GE work is
  done by the host GPU or by softgpu on a timescale unrelated to the PSP's. So the rig did not
  merely *understate* `gu`, it reported a ratio that reverses.
- **Corroborated on a second console, and the agreement is the interesting part.** PSP-1000,
  mode 0, 8 windows: `stage ≈ 1337  gu ≈ 1176  tot ≈ 2513 µs`, **spread 6 µs across the whole
  run**. Against the PSP-3000's `stage=1338 gu=1140 tot=2479` that is agreement to ~1 %, on two
  different SoC revisions. **So the blit cost is structural, not console-specific**, and the
  ~46 % GE wait is not one console's quirk. The 6 µs spread also sets the noise floor for the
  pending `blit_mode` comparison: **any mode 1 / mode 2 difference above ~10 µs is real signal**,
  which is an unusually clean A/B to be handed.
- **I then drew a second conclusion here, and it was ALSO wrong — see ADR-0034 for the
  correction.** This ADR originally said: *"`blit_mode` addresses at most ~54 % of the blit; the
  other ~46 % is the CPU sitting idle waiting for the GE, and no staging placement can shorten
  it."* The reasoning was that where a buffer lives changes who writes it, not how fast the GE
  rasterises. **The hardware A/B falsified it outright**: moving the staging buffer to VRAM cut
  `gu` from **1176 to 728 µs (−38 %)**, because the buffer's location also changes the GE's
  *read* cost. The placement lever turned out to work mainly on the half I had just argued it
  could not touch. Full table and consequences in ADR-0034.
- **The remaining wait still has a known shape and a real hazard.** Even at 728 µs, deferring
  the sync (kick the list, run the next emulated frame, sync at the last possible moment) is the
  last lever on this path — but `gu_list` is a **single static buffer** and `osd_draw()` starts a
  second list into it in the same frame, so the GE would still be reading it. Double-buffering
  `gu_list` and moving the sync to just before `sceDisplayWaitVblankStart()` is the shape of the
  fix. **Still not attempted: a concurrency change to the display path is the last thing to build
  on a rig that cannot see GE timing at all.**
- **This is the second time a rig ratio has failed to survive hardware, and the first time it
  INVERTED** (ADR-0028 was the first: rig `core` max 10.5 ms vs field 21.3 ms). A third followed
  within the day — ADR-0034's falsified causal claim above. The standing rule is now explicit and
  three times earned: **the rig proves correctness and control flow; it prices nothing that
  touches caches, uncached memory, or the GE, and it cannot be trusted about which component a
  change will even affect.**

### 3. A missing Makefile dependency shipped a silent struct-layout corruption into the user's config.ini

- **What the field saw.** After a session, the app's own config writer produced
  `blit_mode = 1347637319` and `group = 07`. `blit_mode` is a 0-2 enum. The read path clamps it so
  nothing visibly broke, but a garbage value **at save time** means memory was wrong, not the file.
- **It is not a buffer overrun, and `net_skip_threshold_str` is innocent.** `1347637319` =
  `0x50535047` = the ASCII **"GPSP"** little-endian — the first four bytes of the room code
  `GPSP07`. So a writer put the string **four bytes early**, leaving `"07"` in the real `group`.
- **Root cause: `psp/Makefile` had no header dependencies.** `build.mak` supplies only an implicit
  `%.o: %.c` rule, so changing a HEADER rebuilt nothing. Adding `blit_mode` to `psp_config` left
  `ui_psp.o` — timestamped from before that field existed — compiled against the old layout, while
  `config_psp.o` and `main_psp.o` used the new one. Every translation unit agreed on the symbol and
  disagreed on the offsets. **It does not fail at link and it does not warn.** `ui_psp.c` writes
  `snprintf(g_pcfg.group, …, "GPSP%02d", nn)` from the room-code UI, and that write went to the
  stale offset. Confirmed by compiling both layouts side by side:

  ```
  new: blit_mode=52 group=56
  old: group=52            <- the stale ui_psp.o wrote "GPSP07" here
  ```

  52 is exactly `blit_mode` in the shipped struct. Both field symptoms fall out of one cause.
- **Therefore `group = 07` is the SAME bug, not a second one, and not a design.** The room-code UI
  legitimately stores the whole `"GPSP07"` string and the bring-up path uses it verbatim; there is
  no "numeric part only" convention and nothing prefixes `GPSP` at runtime. **The coordinator's
  instinct is the right one though — two consoles disagreeing on the room looks exactly like a
  wireless failure — and this bug could genuinely have caused it**, because the corrupted `group`
  is written back to config.ini and persists across boots. **Any console that ran Build F and
  opened the wireless UI should have its `config.ini group=` checked and reset to `GPSP07`.**
- **Decision 1: `-MMD -MP` plus `-include $(OBJS:.o=.d)` in `psp/Makefile`,** with the `.d` files
  swept by `clean` and ignored by git. Verified: touching `config_psp.h` previously rebuilt
  **nothing**; it now rebuilds exactly `config_psp.o`, `main_psp.o` and `ui_psp.o` — the three
  translation units that include it, and no others.
- **Decision 2: `pcfg_validate()` runs before every save and makes this class LOUD.** Every field
  in `psp_config` is a small enum or a bounded string, so "outside its range" is proof rather than
  suspicion. It logs `EVT config_corrupt when=save field=blit_mode value=1347637319 range=0..2
  -> 0` and *then* clamps — clamping silently on the way out would have erased the only evidence
  that anything was wrong. A short `group` is caught the same way.
- **Why the guard belongs at SAVE and not only at load.** The load path already clamped, which is
  why nothing crashed — and is also why the bug survived. **A clamp that repairs a value without
  reporting it converts a memory-corruption signal into normal operation.** That is the general
  lesson, and it applies to every defensive clamp in this codebase.

- **Gates** (all on the shipped EBOOT, from a clean rebuild): `run_boot_test.sh` PASS,
  `run_trade_test_psp.sh --radio=40` PASS, `--radio=160` PASS,
  `run_gu_color_test.sh --blit-mode=0|1|2` PASS, netdrv unit suite PASS.
- **What the next hardware run must bring back.** `EVT session_pace_snap req=29.86 applied=29.97
  vblanks=2` (the consoles' current config), then `session_pace … ramp_done` and — the whole
  point — **`session_pace_miss` should now be ABSENT**, with `sess_cost pace=29.97/…` and the
  achieved rate at 29.97 rather than 35-38. If a miss still fires at two whole vblanks, the
  console cannot hold 2 vblanks/frame and the next step is N=3 (19.98), not a smaller fraction.
  Also: `EVT config_corrupt` must not appear at all, and `config.ini group=` must read `GPSP07`.
- **Upstream impact:** None. `psp/` only.

## ADR-0036 — The harness control channel gets an internal name: `autopilot.ini` → `.gpsp-harness.ini`, and a leftover legacy file is reported rather than obeyed

- **Status:** accepted 2026-08-02, branch `phase5i-fixedrate`. User request, and a design-error
  correction.

- **Context — three incidents, one root cause, and the root cause was ours.** A stale
  `autopilot.ini` left on a memory stick silently brought a **wireless session up at boot during
  a solo performance benchmark**; the user only noticed mid-run, from the frame rate, and had to
  disable it by hand. The same leftover file had twice before skipped the ROM browser
  unexpectedly and auto-hosted when the user wanted manual control. The user's request is
  unambiguous: *"I'd like autopilot permanently disabled going forward."*

  **The file was doing exactly what it was designed to do; the design was wrong.** The harness
  genuinely needs a way to drive host/join, pick a ROM, force fast-forward and pin performance
  knobs without a human. What it should never have had is a **filename that looks like a normal
  user config sitting in the same directory as two real ones** (`config.ini`, `variant.ini`).
  Anyone tidying a kit, copying a folder, or reading a directory listing sees three ini files
  and no way to tell which one is internal. **That is our error, not the user's mistake**, and it
  is the thing worth fixing — the individual leftover files were only the symptom.

- **Decision 1 — the channel is renamed to `.gpsp-harness.ini`, and `autopilot.ini` is no longer
  read at all.** The leading dot plus the `gpsp-` prefix make it self-describing as internal:
  a human packaging a kit will not create it, will not mistake it for a setting, and in most file
  listings will not even see it. Every consumer moved in one commit — `psp/main_psp.c` (path
  setup, all 27 key reads, and the `have_harness` branch that also suppresses the ROM browser),
  all ten `tools/e2e/*.sh` drivers that write it, `docs/TESTING.md`, `docs/AUTOPILOT.md`,
  `docs/VARIANTS.md`, `docs/ARCHITECTURE.md`, `docs/HANDOFF.md` and the README.
  - **Renaming is the fix, not a flag.** A "disable autopilot" switch would live in `config.ini`
    and could itself be stale, absent, or overridden by the very file it was meant to disable.
    Making the channel unnameable-by-accident removes the failure mode instead of adding a guard
    against it.

- **Decision 2 — a legacy `autopilot.ini` is IGNORED, REPORTED, and NEVER DELETED.** On boot:
  `EVT legacy_autopilot_ignored file=… — autopilot.ini is no longer read; the harness channel is
  .gpsp-harness.ini (ADR-0036). This file is being left alone, not deleted.`
  - **Not deleted, on purpose.** It is the user's file even when it is in the way, and an
    emulator that silently removes files from a memory stick is a worse problem than the one
    being solved.
  - **Reported, on purpose.** The whole failure was that a solo benchmark came up in a wireless
    session with *nothing in the log saying why*, leaving the user to infer it from the frame
    rate. One line at boot converts that into an immediate answer.

- **Decision 3 — the LIVE channel is never silent either.** `EVT harness_ini active file=… —
  automated control channel is present; ROM browser suppressed and harness keys apply` fires
  whenever `.gpsp-harness.ini` exists. Renaming makes an accidental trigger unlikely; this makes
  an accidental trigger *visible*. Had this line existed, the original incident would have been
  a five-second diagnosis rather than a lost benchmark run. **The generalisable rule: any
  mechanism that can change what the user sees without the user asking must announce itself in
  the log.**

- **What deliberately did NOT change.**
  - **`variant.ini` keeps its name and behaviour** — it is user-chosen and user-facing, which is
    precisely the property the harness channel lacked.
  - **The input-script ENGINE is still called autopilot** — `frontend-common/fe_autopilot.[ch]`,
    the `script =` key, and the `EVT ap_loaded` / `ap_done` / `ap_fail` lines are untouched. Only
    the *file* moved. Prose in the docs that says "autopilot" now means the engine, and
    `docs/AUTOPILOT.md` opens by saying so, because a half-rename would be worse than either
    option.

- **Consequences.** Every e2e driver writes the new name, so the harness is unaffected once
  rebuilt; a mixed pairing (old script + new EBOOT) simply gets no harness channel and lands in
  the ROM browser, which is a visible failure rather than a silent one. **Any memory stick from
  before this build should be checked for a leftover `autopilot.ini`** — it is now inert, and the
  log will say so, but it is still clutter.
- **Gates.** `run_boot_test.sh` PASS, `run_trade_test_psp.sh --radio=40` and `--radio=160` PASS,
  `run_gu_color_test.sh --blit-mode=0|1|2` PASS, netdrv unit suite PASS — every one of these
  drives the EBOOT through the renamed channel, so the rename is exercised by the entire gate
  set rather than by a dedicated test. Additionally asserted directly: a planted legacy
  `autopilot.ini` containing `join=1` produces `EVT legacy_autopilot_ignored`, does **not**
  bring up a session, and is still present on disk afterwards.
- **Upstream impact:** None. Frontend and harness only.

---

## ADR-0037 — The adaptive frameskip was measuring against a rate we stopped aiming at: compare with the APPLIED pace target

- **Status:** accepted 2026-08-03, branch `phase5k-perfnext`. Ships ON; it is a bug fix, not a lever.

- **The bug, in one field line.** `EVT fps emu=29.43 rendered=14.71 skipped=300` — during a
  clamped session, with `self_cap` simultaneously reporting **52-57 fps of capability sitting
  unused**. The emulator was hitting its configured target essentially perfectly and throwing
  away every second frame for it.

- **Cause.** `SKIP_SLOW_FPS_X100 5700` / `SKIP_FAST_FPS_X100 5850` were absolute constants,
  derived from the GBA's nominal 59.7275 Hz. That was correct for as long as the nominal rate
  was the only rate we ever aimed at. **ADR-0033 changed that** and the constants did not
  follow: a session clamped to `net_session_fps` (40.00, or 29.97 after ADR-0035's whole-vblank
  snap) measures 29.43 < 57.00, decides it is behind on three consecutive windows, engages
  `fixed_interval` interval 1, and never releases — because it can never reach 58.50 while
  something else is deliberately holding it at 29.97. A permanent 50 % render loss caused
  entirely by a units mismatch between two modules.

- **Decision.** Both thresholds are now a percentage of `g_pace_target_x100`, the *applied*
  (ramped) target: `SKIP_SLOW_PCT_TGT 954` and `SKIP_FAST_PCT_TGT 980`, x10. Percentages rather
  than a fixed offset because the target GLIDES over ~5 s at session start and end, and a fixed
  offset would mean a different thing at each point on the ramp.

- **Why this is safe everywhere else.** Off-session `g_pace_target_x100` is `PACE_NOMINAL_X100`
  (5973), so the thresholds evaluate to **5697 / 5853** against the old **5700 / 5850**. Within
  3/100 fps. The change is inert outside the case it fixes.

- **Both transitions now log `target=`.** A field log that says only `skip_engage fps=29.43`
  cannot be read without knowing what 29.43 was supposed to beat; that is precisely how this
  survived several sessions.

- **Expected effect:** ~30 drawn frames per second during a clamped session instead of ~15, at
  no cost. This is likely the largest *perceived* improvement in this branch, and the cheapest.

- **What the rig proved:** boot, trade (`--radio=40`) and the colour test all pass, i.e. no
  regression. **Correction to my own first draft of this bullet:** I wrote that the rig "does not
  clamp, so it cannot demonstrate the fix firing." That is wrong — the rig runs
  `net_pace_match=1` at `net_session_fps=29.97` by default and its sessions clamp exactly like
  the field's (`EVT fps emu=29.97 rendered=29.97 skipped=0`). The only reason the case was
  unreachable is that `net_frameskip` had no harness override, so the adaptive policy never ran
  on the rig at all (`EVT skip_policy mode=off`). **`run_trade_test_psp.sh --net-frameskip=1`
  now exists**, and `--net-frameskip=1 --radio=40` against a 29.97 clamp is the field case
  reproduced: the pre-fix code engages `fixed_interval` within ~3 s and never releases.

  Worth stating plainly because it is the second time on this project that "the rig can't test
  that" turned out to mean "nobody wired the knob through."

- **Upstream impact:** None; PSP frontend only.

---

## ADR-0038 — Split the blit's `gu` number into list-building and the GE wait, because only one of them is recoverable

- **Status:** accepted 2026-08-03, branch `phase5k-perfnext`. Instrumentation only; ships ON.

- **Why.** ADR-0034 measured the blit as `stage` + `gu`, where `gu` was "the GE list build and
  the sceGuSync that waits for it" — 728 µs in VRAM mode. A deferred-sync scheme (ADR-0040) can
  recover *only the sync*; list building is real CPU work that happens either way. Nobody could
  say whether the change was worth 700 µs or 70, because the two had never been measured apart.
  A previous agent designed the deferral and correctly declined to build it blind. This closes
  that gap rather than arguing about it.

- **Decision.** `blit_prof` gains `wait=avg/max`, bracketing `sceGuSync` alone. **`wait` is a
  SUBSET of `gu`, not a new term of `tot`** — `tot` is still `stage + gu`. `gu - wait` is list
  building.

- **Deliberately, `wait` counts the stall wherever it happens.** Under `gu_defer` the sync moves
  out of `vid_draw_frame` into `vid_gu_flush()`, and charges the same accumulator. So the A/B is
  a straight comparison of one number between two runs.

- **Upstream impact:** None.

---

## ADR-0039 — Emit the GE's channel order from the core's palette conversion and delete the frontend's per-pixel swap

- **Status:** accepted 2026-08-03, branch `phase5k-perfnext`. Default for `platform=psp1`.
  **The single largest frontend win available tonight, and it is a deletion.**

- **The finding.** The frontend was spending **1071 µs/frame — 6.4 % of the 16743 µs budget —
  undoing a swap the core had just performed for free.**

  `convert_palette` (`common.h`) expands the GBA's native **BGR555** palette entries into a
  16-bit display format, and was deliberately placing R in bits 11-15 to produce **libretro
  RGB565**. `stage_convert_rgb565` (`psp/video_psp.c`) then walked all 76 800 pixels of every
  frame putting R back into bits 0-4 to produce the GE's **PSP 5650**. Two swaps, cancelling.

- **Decision.** `USE_PSP_RGB565_FORMAT` makes `convert_palette` emit 5650 directly:
  `((value & 0x1F) | ((value & 0x7FE0) << 1))`. The staging step becomes a `memcpy`.

- **Three reasons this is the right layer to fix it at.** (1) The conversion runs on **palette
  writes**, not on pixels — 512 entries against 76 800. (2) The new expression is the **cheapest
  of the three** layouts: one mask, one shift, one or, against the old four-term form. (3) There
  was already a precedent for a build-time layout switch here (`USE_XBGR1555_FORMAT`), so this
  is an added branch in an existing `#if` chain, not a new mechanism.

- **`video.cc` needs NO change, and that is not luck.** Its blend path separates a pixel into
  `0G0R0B` and clamps with `SATR_MSK 0x0000F800` / `SATB_MSK 0x0000001F` and overflow bits
  `OVFR_MSK 0x00010000` / `OVFB_MSK 0x00000020`. R and B are **both 5 bits** and occupy exactly
  the two positions those masks name, so exchanging which channel sits where permutes the mask
  *names* and leaves every constant and all the arithmetic identical. G never moves at all —
  bits 5-10 in both layouts. `blend_a`, `blend_b` and `brightf` apply uniformly to all three
  channels, so there is no per-channel asymmetry for the swap to disturb.

- **VERIFIED BY EQUIVALENCE, NOT BY THAT ARGUMENT.** The argument above is why it *should*
  work; it is not why we believe it. Both pixel pipelines were built and their rendered output
  compared:

  | artifact | rgb565 build | psp5650 build |
  |---|---|---|
  | `ge_000600.bmp` — Emerald frame 600, GE drawbuffer | `f923822a4af227dd580a5399bfef91a1` | **identical** |
  | `ge_000000.bmp` — colour-bar pattern, GE drawbuffer | `ee831f210318a5849d7c128517b80991` | **identical** |
  | `frame_000600.bmp` — core buffer | `61cd89eee61747d8089a75ed33e19f8f` | **identical** |

  Byte-identical output through two independent formats, across the full chain: palette
  conversion → `video.cc` blending → staging → GE rasterisation. `run_gu_color_test.sh` passes
  both phases in all three `blit_mode`s.

- **REJECTED, and priced: bind the GE straight at the core's framebuffer.** With the formats now
  agreeing this looks free — no copy at all. **ADR-0034's hardware A/B already refutes it.** A
  main-RAM texture cost the GE `gu = 1174`; the VRAM staging texture costs `728`. A direct bind
  puts the texture back in main RAM and spends ~446 µs of GE time to save ~600 µs of CPU time,
  *before* counting the 76.8 KiB D-cache writeback the GE would still need and the fact that the
  core's buffer is `malloc`'d with no alignment guarantee for a texture base. Keep the copy. This
  is the third time on this path that the obvious move has been wrong for a reason only hardware
  reported (ADR-0034 correction 1, ADR-0035 §2); the pattern is now the expectation.

- **REJECTED, unmeasured: `sceGuCopyImage` for the staging copy.** Now *possible* with no
  conversion in the way, and it would take the copy off the CPU entirely. But it makes the GE
  read the source out of main RAM — the exact thing the 1174-vs-728 number says is expensive —
  and adds it to the critical path that is already the blocking cost, to save a `memcpy`. Not
  shipped, and deliberately not shipped as a third variable tonight. If `wait` comes back large
  and `stage` small, revisit; the reasoning is recorded so the next agent starts from here.

- **The one real hazard: the flag is set in TWO makefiles and nothing checks agreement.**
  `Makefile` (psp1 block) and `psp/Makefile` both define `PSP_PIXFMT`. Getting them out of step
  does not fail to build and does not warn — it renders with R and B swapped. Mitigations:
  `run_gu_color_test.sh` phase A pins the frontend side absolutely (known colours in, asserted
  colours out, no core in the loop); the boot log now carries `pixfmt=psp5650 stage=copy`; and
  the equivalence check above would have caught a mismatch. `make PSP_PIXFMT=rgb565` on both
  restores the old layout for a hardware A/B.

- **Colour correction must stay off in this build, and now cannot be turned on.** `gba_cc_lut`
  is a 32 768-entry table baked in libretro channel order (`tools/generate_cc_lut.c`), so under
  5650 it would hand red's correction curve to blue and return a libretro-ordered pixel.
  `fe_host_option_set_live()` refuses `gpsp_color_correction` and logs
  `EVT option_refused key=... reason=pixfmt_psp5650`. **Frame mixing is unaffected** — its
  `0x821` mask is the low bit of each 5/6/5 field in either layout.

- **Expected effect:** `stage` falls from 1071 µs toward the cost of a 76.8 KiB copy. The
  residual is the copy itself; **the rig cannot price it** (it models neither Allegrex's caches
  nor uncached/VRAM write cost, and has now been wrong three times on exactly this path). Read
  `stage=` in `blit_prof`.

- **Upstream impact:** `common.h` gains one `#elif` branch, opt-in and inert by default. Of
  genuine interest to any frontend whose display format is not libretro RGB565 — the same
  trade exists on Vita and 3DS.

---

## ADR-0040 — Let the GE finish during the vblank wait instead of blocking on it (`gu_defer`, default OFF)

- **Status:** accepted 2026-08-03, branch `phase5k-perfnext`. **Ships OFF.** Config-gated for a
  hardware A/B; the rig can prove it correct and cannot price it.

- **Why.** The blit ended with `sceGuSync`, stopping the CPU dead until the GE had rasterised —
  **728 µs of the 1800 µs blit, doing nothing.** The main loop's very next act, a few
  microseconds later, is `sceDisplayWaitVblankStart`: idling anyway. Deferring the sync past the
  vblank wait lets the GE work through time the frame was already spending.

- **The blocker a previous agent identified was real, and this is what it cost to clear it.**
  `gu_list` was a single static buffer that `osd_draw()` reuses within the same frame. There are
  now **two**, alternated by `gu_next_list()`, which is the exact requirement: a frame issues at
  most a blit list and an OSD list before the pre-swap flush, and `gu_next_list()` flushes rather
  than hand out a third. That bound is enforced by a counter, not by an argument about call
  sites, so it holds if the UI ever grows another overlay pass.

- **The invariant.** No buffer swap and no drawbuffer readback may happen with a list in flight.
  `vid_swap()` and `vid_dump_ge()` therefore call `vid_gu_flush()` **unconditionally** — the
  invariant does not depend on the config key being read correctly.

- **Be honest about the size of this.** The saving is bounded by the loop's idle time, so it is
  **small at full speed** (~700 µs of slack in a 16743 µs budget, most of which the blit itself
  was consuming) and **large during a clamped session**, where a 40 fps target leaves ~9 ms
  idle. In-session is where the budget hurts — but it does mean a solo-play A/B may show almost
  nothing while a session A/B shows a lot. **A/B it in a session.**

- **What the rig proved, and what it did not.** With `--gu-defer=1` the colour test passes both
  phases, and the GE drawbuffer dumps are **byte-identical** to the non-deferred run — no
  tearing, no stale readback, correct ordering between the blit list and the OSD list. PPSSPP
  does model the GE list queue, so this is meaningful coverage of the concurrency. It says
  **nothing** about what the overlap is worth; PPSSPP does not model GE rasterisation time.

- **Read `wait=` in `blit_prof`.** Under `gu_defer` the sync moves into `vid_gu_flush()` and
  charges the same accumulator (ADR-0038), so `wait` should collapse toward zero if the vblank
  is absorbing the GE's work, and stay high if it is not. That is the whole measurement.

- **Upstream impact:** None; PSP frontend only.

## ADR-0041 — The exit-Union-Room screen is the game's FATAL RFU path, and we now know exactly what produces it (instrumentation, no fix)

*(ADR number: 0030–0036 are claimed by other in-flight branches; this one is 0037 to avoid a
merge collision. Branch `phase5j-exitroom`.)*

- **Context.** After a completed Union Room trade the joining console shows gen-3's
  **unrecoverable** wireless screen ("…please turn off the power") while the host shows the
  **recoverable** one ("press A"). Six sessions of transport evidence exhausted every delivery
  explanation: `overflow=0 spill=0 txfail=0 drop_crc=0`, core payload counters mirrored exactly,
  `retx_pct` low. The RetroArch×2 control exits cleanly. Nobody had ever established *what the
  game does differently* between the two screens, so every hypothesis was unfalsifiable.

- **The asymmetry is one byte, and it is decided by ONE predicate.** `CB2_LinkError`
  (pokeemerald `src/link.c:1589-1610`) does `if (!sLinkErrorBuffer.disconnected) gWirelessCommType = 3;`
  and `CB2_PrintErrorMessage` (`:1669-1725`) at `gMain.state == 160` only offers an A-button
  handler for `gWirelessCommType == 1` (`ReloadSave`) and `== 2` (`DoSoftReset`). **`== 3` has no
  handler at all** — that *is* the "turn off the power" screen. And `disconnected` is set from
  exactly one expression (`src/link_rfu_2.c:1995`):
  `SetLinkErrorBuffer(..., RfuGetStatus() == RFU_STATUS_CONNECTION_ERROR)`.
  **`RFU_STATUS_CONNECTION_ERROR` -> recoverable. Anything else (i.e. `RFU_STATUS_FATAL_ERROR`)
  -> unrecoverable.** Every `LMAN_MSG_LINK_LOSS_*` path yields CONNECTION_ERROR; that is why the
  host, whose adapter merely timed its client out, gets the polite screen.

- **So the client-side routes to the fatal screen are a CLOSED SET** (all verified in pret):
  1. `LMAN_MSG_REQ_API_ERROR` — **the adapter answered a command with an error.**
  2. `LMAN_MSG_WATCH_DOG_TIMER_ERROR` — 360 VBlanks (~6 s) as clock slave with no MSC callback
     (`AgbRfu_LinkManager.c:423-429`, `librfu_rfu.c:838-879`).
  3. `LMAN_MSG_CLOCK_SLAVE_MS_CHANGE_ERROR_BY_DMA` (`librfu_stwi.c:493-501`).
  4. `LMAN_MSG_RFU_FATAL_ERROR` — `AgbRFU_checkID` failed after a soft reset
     (`AgbRfu_LinkManager.c:460,474`).
  5. **`gRfu.sendQueue.full || gRfu.recvQueue.full`** (`link_rfu_2.c:1999-2005`).

  (`LMAN_MSG_LMAN_API_ERROR_RETURN` also latches the error but sets `isShuttingDown`, and
  `RfuMain2` early-returns on that (`:2046`), so it cannot reach the screen by itself.)

- **Route 1 is directly ours, and the coupling is exact.** gpsp's `rfu_process_command()` answers
  `return -1` with the SPI sequence `0x996601ee` + an error code (`rfu.c` `RFU_COMSTATE_RESPERR`).
  librfu decodes an `0xEE` ack byte as `ERR_REQ_CMD_ACK_REJECTION` (`librfu_intr.c:128-132`), which
  becomes `reqResult = 3`, which fires `LMAN_MSG_REQ_API_ERROR` (`AgbRfu_LinkManager.c:933-953`),
  which is `RFU_STATUS_FATAL_ERROR`. The only forgiveness is a narrow one — TX/RX/MS_CHANGE only,
  mid-MSC only, child only (`:835`), plus a child's rejected `ID_DISCONNECT_REQ`
  (`librfu_rfu.c:1116-1126`). **Every other `return -1` in rfu.c is a direct generator of the
  unrecoverable screen**, and there are seven of them, all state-dependent.

- **Decision: instrument, do not fix.** `gpsp_rfu_trace_hook` (weak, no-op for libretro, same
  contract as the ADR-0013/0019 hooks) reports, as `EVT` lines:
  `rfu_cmderr` (a command answered ERROR — route 1, named directly), `rfu_state` (all nine
  adapter state transitions with cause; three were previously silent), `rfu_cmd` (the structural
  commands, per-frame chatter filtered), `rfu_unkcmd` (commands this adapter model never
  implemented — `ID_CPR_*` 0x32-0x34 link recovery, 0x35/0x36), `rfu_qdrop` (rfu.c's own queue
  overflowing), `rfu_login` (the `AgbRFU_SoftReset`+`checkID` handshake — route 4), and
  `rfu_rxburst` (see below). ~2.5 lines/s in a Union Room session, on the ADR-0024 writer thread.

- **What the rig then said — three results, two of them negative and worth as much.**
  1. **A clean Union Room session never produces a single `rfu_cmderr`, `rfu_unkcmd` or
     `rfu_qdrop`.** The parent/child switch cycles `0x19 SC_START`(->host) / `0x1b SC_END`(->idle) /
     `0x1c SP_START` / `0x1e SP_END` ~2.3 s apart, ~46 times a session, always from a legal state.
  2. **The rig does not reproduce the bug.** `run_exit_room_test.sh --leaver=both --radio=40`:
     both consoles walked out of the Union Room, `cb2_after_exit` = CB2_Overworld on both, no
     fatal screen. (The harness still fails afterwards, on the *re-entry* leg's in-game save —
     a navigation defect downstream of everything this ADR is about.)
  3. **A hypothesis was killed by measurement, exactly as intended.** It looked compelling that
     the child is pinned in `RFU_STATE_CLIENT` — the child's *documented* leave is an NI
     `LEAVE_GROUP_NOTICE` that the PARENT answers with `rfu_REQ_disconnect`
     (`link_rfu_2.c:1613-1674`), rfu.c has no other route back to IDLE (`HOST_STOP` refuses to
     clear a populated host; `HOST_START` only clears slots when already IDLE; `ID_RESET_REQ` 0x10
     and `ID_STOP_MODE_REQ` 0x3d are no-op ACKs) — and `SC_START` from `RFU_STATE_CLIENT` is one of
     the seven `return -1`s, i.e. the fatal screen, and it is client-only by construction. The
     causation test (`--drop-disc`, a debug fault that swallows the parent's DISCONNECT notice —
     see `netpacket_host.h`) **refuted it**: with the notice swallowed the child issued its *own*
     `rfu_REQ_disconnect` anyway (`RfuMain1_Child`, `link_rfu_2.c:945-962`; our trace shows
     `rfu_state new=idle cause=7`) and both consoles still left the room cleanly. **Do not
     re-derive this one.**

- **The leading hypothesis now, and the number that settles it — route 5.** `gRfu.recvQueue` has
  32 slots, is filled once per MSC callback (`link_rfu_2.c:590`) and drained **once per game
  frame** (`:937`), and `full` is a **latch**: `RfuRecvQueue_Dequeue` returns FALSE immediately
  once set (`link_rfu_3.c:391-394,:437`), so it never recovers — it goes straight to
  `RFU_STATUS_FATAL_ERROR`. On real radio the adapter delivers one frame per frame and that queue
  cannot outrun its drain. Over a transport with tens of ms of RTT, deliveries arrive **clumped** —
  and **ADR-0011 deliberately widened rfu.c's own queue from 4 to 16 so as to stop discarding those
  clumps.** ADR-0011 was right that dropping them wedged the trade; what it could not know is
  whether the clumps then overflow the *game's* queue instead. Nothing has ever measured that.
  `EVT rfu_rxburst frame_max=` is that measurement: `RFU_CMD_RECV_DATA` commands served in one
  emulated frame. **Steady state is 1. Anything climbing toward 32 is the mechanism.**
  **Rig baseline, measured on the shipped build** (`run_trade_test_psp.sh --radio=40`, full
  trade, `srtt_us` ≈83 000): **host high-water 2, join high-water 3.** So a healthy 40 ms-RTT
  session peaks at 3 of 32 — a 10× margin, which is why the rig cannot fail this way and why the
  field number is the one that matters. Read it against these two values, not against zero.
  This also
  explains, without needing any transport difference, why RA on localhost is clean (sub-ms RTT
  never clumps) and why the failure lands at *room exit* (the child stops draining across the map
  fade/warp/save while the parent keeps sending).

- **A gap in the bisection that "proved it is ours", recorded so it is not leaned on again.** The
  RetroArch x2 control was run with the **host walking out first** (docs/HANDOFF.md, the
  exit-Union-Room issue). The field failure is on the **joining** console. Ordering is not
  incidental here — the child's leave depends on the parent's `rfu_REQ_disconnect` — so the
  control does not actually cover the failing case. The conclusion may still be right; the
  evidence is weaker than it reads.

- **Consequences.** No behaviour change ships: the trace is weak-hooked and additive, and the
  `--drop-disc` fault is off unless `autopilot.ini rfu_drop_disconnect` is set. Read `rfu_cmderr`
  and `rfu_rxburst` in the next field log, in that order — one of them names the cause, and if
  both are quiet the answer is routes 2-4 (timing, i.e. the frame-loop spike, not delivery).
  **Do not raise `RFU_PKT_QUEUE` again and do not add pacing before `rfu_rxburst` is read** —
  both are plausible-and-opposite fixes and only the measurement distinguishes them.

- **Also fixed en route (cross-agent hygiene, not this bug).** The e2e harnesses ended with
  `pkill -9 -f "gpsp-adhoc/EBOOT.PBP"`, which matched **every** instance on the machine: two
  agents running harnesses concurrently shot each other's emulators down and each blamed its own
  change. Now scoped to `$SANDBOX_ROOT/inst`. This cost one wasted run during this branch's work.

- **Gates.** netdrv unit suite PASS; `run_boot_test.sh` and `run_trade_test_psp.sh --radio=40`
  on the shipped EBOOT — see the commit message.

- **Upstream impact.** The trace hook is a fourth candidate core patch: weak symbols, no
  behaviour change, and it makes gpsp's emulated adapter answerable to the question "which command
  did you refuse, and in what state?" — which is unanswerable today and is the difference between
  a recoverable and an unrecoverable error in every gen-3 game.

## ADR-0042 — Cap how many RFU packets a joining console hands the game per frame (`rfu_rx_cap`, default 0 = off)

- **Status:** proposed 2026-08-03, branch `phase5m-morning`. **This is a hypothesis test, not a
  known fix.** Default is OFF; nothing changes for anyone who does not set it.
- **Context.** The exit-Union-Room fatal screen has survived every explanation so far. ADR-0041
  named the predicate exactly (`CB2_LinkError` sets the unrecoverable variant iff the adapter
  status is `RFU_STATUS_FATAL_ERROR` rather than `CONNECTION_ERROR`) and enumerated a closed set
  of five routes to FATAL. Two field sessions on 2026-08-03 eliminated most of it:

  | evidence | what it rules out |
  |---|---|
  | **zero `rfu_cmderr`** across two full sessions | route 1 — our adapter answering a command with an error. This was the one route that would have been our own bug in `rfu.c`. |
  | second session had `srtt=63 ms`, `spill=0`, `retx=8 %`, `txq_hi=26` — a healthy link — **and crashed identically** | the transport. The 40 fps run that preceded it had `srtt=358 ms`, `rto` pinned at max, `spill=283`, and produced the *same* failure, so link quality is not the variable. |
  | client trace goes `client → idle cause=9 (RESET)`, with **no `DISC_LOCAL` and no `DISC_PEER`** | the disconnect handshake. The game never issued a disconnect through our adapter at all; it soft-reset, which is what librfu does *after* the game has already declared a fatal error. |
  | host saw only `rfu_link_down reason=4` (TIMEOUT) then `peer_disconnected` | the host. Its polite "press A" screen is strictly downstream: client dies, client resets its adapter, traffic stops, host times it out after 4 s. Nothing to fix there. |

- **The one anomaly left.** `rfu_rxburst` — `RFU_CMD_RECV_DATA` served in a single emulated frame:

  ```
  PPSSPP rig (exits cleanly, every time)   2-3
  field host   (survives, polite screen)     2
  field client (dies, fatal screen)          6      <- both sessions
  ```

  The console that is served **bunched** is the console that dies, and the rig — which never
  reproduces the crash — is also the configuration that never bunches. Same game code on both.

- **Mechanism (inferred, NOT proven by our logs).** The game drains its own 32-slot `recvQueue`
  once per frame and `full` is a *latch* straight to `FATAL_ERROR`
  (`link_rfu_3.c:391-394,:437`, `link_rfu_2.c:1999-2005`). While it is tearing the connection
  down — the "Terminating connection please wait" box, which is exactly where the field crash
  lands — it is not running its normal drain loop, so anything still arriving accumulates.
  **This last step is about the game's internals and we have not instrumented them. Treat it as
  the surviving hypothesis, not a finding.**

- **Decision.** Add `rfu_rx_cap`: the most *non-empty* `RECV_DATA` reads a **client** adapter will
  serve in one emulated frame. Beyond the quota we answer "no data this poll" — a reading the
  game already handles on every idle frame — and **leave the packet queued**. Held packets cost
  one frame of latency and are never dropped: `RFU_PKT_QUEUE` is 16 deep, which ADR-0011 widened
  from 4 for exactly this bunching. `0` = unlimited = historical behaviour, and is the default.
  No effect on a hosting adapter.
- **Why a knob rather than just doing it.** ADR-0041 was explicit that raising the queue and
  pacing delivery are *opposite* fixes and only measurement chooses between them. This makes the
  measurement possible on one EBOOT: same binary, one config line, A/B in the field.
- **What would falsify this.** `rfu_rx_cap=2` or `3`, `EVT rfu_rxhold` lines present (proving the
  cap actually bound), and **the client still dies**. That kills delivery bunching outright and
  moves the remaining weight onto the timing routes — watchdog, clock-slave, `checkID` — which
  would put this issue behind the core-performance work rather than in front of it.
- **The failure mode to watch for.** If the host sustains more than the cap per frame, our own
  queue backs up and `EVT rfu_qdrop` starts firing. That is already traced. A run with `rfu_qdrop`
  in it is not evidence about the hypothesis — the cap was set too low and starved the link.
- **A caution for whoever reads the next log.** `rfu_rxburst` counts `RECV_DATA` *commands*, not
  packets carrying data; empty polls are included. `rfu_rxhold` and the cap count only non-empty
  deliveries. Do not compare the two numbers as if they measured the same thing.
- **Gates.** Uncapped must pass `run_trade_test_psp.sh --radio=40` unchanged (the default path is
  untouched); capped must complete a trade *and* emit `rfu_rxhold`, or the run proves nothing.

### ADR-0042 addendum — what the rig actually established, 2026-08-03

The gate matrix, after the reporting bug below was fixed:

| `rfu_rx_cap` | result |
|---|---|
| 0 (default) | PASS — default path untouched |
| 1 | **FAIL**, `rfu_rxhold frame_max=29`, `qdrop=0` |
| 3 (the shippable value) | PASS, but `rxhold=0` — **never bound** |

**The premise is weaker than ADR-0042 assumed.** It reasoned that withholding a packet
costs one frame of latency. It does not: at `cap=1` the game **re-polled a withheld packet up
to 29 times inside the same frame** and the session died with nothing lost (`qdrop=0`,
`cmderr=0`). The game busy-polls until it gets what it expects, so starving `RECV_DATA` does
not delay the game — it spins it. Any future pacing scheme has to survive that, and "hold it
for a frame" is not a description of what happens.

`cap=3` cannot be validated here at all: the rig's client peaks at exactly 3 deliveries per
frame, which is *why* 3 was chosen (make the field look like the configuration that never
crashes) and also why the rig can never make it bind. **A PASS at `cap=3` in the rig means
"harmless", not "works".** Only hardware, which reaches 6, exercises the path.

**Reporting bug, fixed here and worth not repeating.** `RFU_TR_RXHOLD` was first emitted on
*every* withheld poll: 610,788 events in one session, through a memory-stick-backed log
channel, which broke the run it was measuring — a self-inflicted Heisenbug. It now reports a
per-frame high-water only, the same discipline `RFU_TR_RXBURST` and the `rfu_cmd` filter
already used. Adding a trace to a hot path without asking what its worst-case rate is has now
cost one wasted gate cycle; the existing events had all solved this and the pattern was there
to copy.

**Build hazard seen the same day:** a rebuild produced a byte-identical EBOOT while the source
was 27 minutes newer, and the output filter hid it. Verify a build by looking for its new
symbols (`psp-nm rfu.o | grep ...`), not by trusting that make ran.

---

## ADR-0048 — Split `rx_dup`, and report the four numbers that make a retransmission legible

**Status:** accepted. Instrumentation only; no behaviour change.

**What went wrong.** A whole debugging session was spent treating the client's retransmission
rate as the suspect in the Union Room exit failure. It is not the suspect. Pairing the two
consoles' logs — the client's `retx` against the *host's* `dup`, which are opposite directions
and therefore cannot both live in one log — gives:

| run | client retx | host dup | wasted | outcome |
|---|---|---|---|---|
| 40 fps | 937 | 721 | 76 % | clean trade |
| 50 fps | 1918 | 1372 | 72 % | fatal |
| 50 fps | 448 | 341 | 76 % | fatal |

The waste rate is the same in the run that *worked*. It is a constant of the protocol, not a
symptom of the failure. The host wastes 80–84 % of its own retransmissions too. Any fix aimed
at the retransmit rate would have been aimed at something that is equally present when
everything is fine.

**Why it was invisible.** `summarize_log.py` summarised one log at a time, so a cross-console
quantity could not be expressed in it. The shape of the tool became the shape of the analysis.
It now takes both logs and prints a `CROSS-CONSOLE` block; that block exists specifically so
this class of number cannot hide again.

**`rx_dup` was two counters wearing one name.** It was incremented both when `seq` fell *behind*
`rx_expect` (a genuine duplicate — the peer retransmitted something we already had) and when
`seq` ran `>= ND_WINDOW` *ahead* of it (the peer violated the window contract — not a duplicate
at all). A "spurious retransmission rate" computed from the sum is not a rate. Split into
`rx_dup` and `rx_beyond_win`. The 76 % above is therefore an upper bound until the next run
reports `beyondwin=0`.

**The four fields added to `EVT net_stats`:**

- `rttvar_us` — RFC 6298 computes `rto = srtt + 4*rttvar`, then clamps. Without `rttvar`, an
  `rto_us` equal to the ceiling cannot be distinguished from a computed value that happens to
  land there, so it could not be established whether the clamp was truncating a larger true RTO
  and firing retransmissions early. The failing runs report exactly `800000`; the working one
  reports exactly `200000`. Both are clamp values, and both were being read as measurements.
- `beyondwin` — see above.
- `retx_age=mean/max` (ms) — a payload's age at the moment it is resent, measured from its own
  first transmission rather than from the RTO that was intended. These differ whenever the ARQ
  scan is late, which distinguishes a misconfigured timer from a starved thread.
- `reorder_hi` — deepest the receive reorder buffer got. A gap holding delivery while successors
  pile up behind it is head-of-line blocking, which the emulated RFU experiences as a stall, not
  as loss — a different failure with a different fix.

**What this does not claim.** None of these explain why the client's `srtt` rises to 336 ms while
the host's *falls* to 47 ms over the same link at the same moment. That asymmetry is the open
question; these four fields exist to stop the retransmission counters from being misread while
it is investigated.

**Build hazards fixed at the same time**, both of which had produced a confident wrong answer:

- `build.sh` ran its containers with `-w /build`, which Git Bash rewrote to
  `C:/Program Files/Git/build`. Every container died, the `[ -f ]` checks passed on the
  *previous* build's artifacts, and the script reported success with an unchanged md5 — the
  exact failure it was written to prevent. Fixed with `MSYS_NO_PATHCONV=1`, an explicit exit-code
  check per container, and an assertion that the EBOOT ends up newer than the newest source.
- The proof step used `psp-nm` on two object files, so it could not see statics, symbols in other
  objects, or changed string literals. The replacement searches the linked binary. Note the
  near-miss: the obvious `strings | grep` version of that check is *worse than useless here* —
  `strings` does not exist in Git Bash, so it reads an empty stream and reports every token
  missing while the build is in fact correct. Verified against a positive and a negative control.

**Undocumented at time of writing:** ADR-0043 through ADR-0047 exist only as code comments
(`rfu.c`, `psp/main_psp.c`). ADR-0047 in particular — the vblank pacing fix — is the most
consequential change of the session and has no entry here.

---

## ADR-0049 — `gu_defer` defaults ON: a clean hardware A/B, and two hypotheses it killed

**Status:** accepted. Supersedes ADR-0040's "default OFF" on hardware evidence.

ADR-0040 added `gu_defer` and defaulted it OFF because it had only been validated on the
desktop rig, which cannot price GPU timing. It has now been A/B'd on two consoles at 40 fps
with every other setting fixed:

| | `gu_defer=1` | `gu_defer=0` |
|---|---|---|
| blit `gu` / `wait` / total | 52 / 101 / **809 µs** | 730 / 687 / **1486 µs** |
| host `reorder_hi` | 110 | 101 |
| join `txq_hi` | 120 | 106 |
| outcome | graceful disconnect | graceful disconnect, trade completed |

It is worth **677 µs of blit time per frame** on real hardware — close to the ~690 µs
predicted — and it does not cause the ARQ pileup, which is within noise of itself either way.
The config default in `psp/config_psp.c` stays 0 so that a stock build behaves as documented;
the shipped `CONFIG.INI` sets it to 1.

**Hypothesis killed #1 — `gu_defer` as the cause of the 40 fps regression.** The preceding run
had failed at 40 fps where an earlier 40 fps run succeeded, and `gu_defer` was the only variable
that had moved in a direction that could hurt. It was a fair suspicion and it was wrong. Recorded
because the reasoning was sound and the conclusion still needed the test.

**Hypothesis killed #2 — the SRAM flush as the gap-opener.** A 107 ms `sram_flush` had been
flagged as plausibly dropping the packet that starts the retransmission cascade. Laying the
flush events against the `reorder_hi` steps in log order falsifies it outright: the host's buffer
climbs 0 → 18 → 27 → 66 → 101 with no flush anywhere in that span, and its later flushes
(11 / 21 / 22 ms) move it not at all. The client's 50.9 ms and 11.1 ms flushes likewise pass
without a step. The save is a real stall and remains worth fixing; it is not this.

**What the run established instead.** The retransmission asymmetry is not an incident, it is the
link's resting state:

```
join retx = 2754      host retx = 12          (230 : 1)
join reorder_hi = 8   host reorder_hi = 101
join txq_hi pinned at 106 of a 128 window, all session
```

The client's `retx` climbs linearly the whole session (14 → 241 → 925 → 1554 → 2754) with the
send queue permanently near the window limit. Consistent with the ADR-0048 finding: cumulative-only
acknowledgement means a single gap leaves every correctly-received successor unacknowledged, so
the sender resends a window's worth to a peer that already holds all of it.

**Open, and the reason the next run raises `core_phase` to 2:** a single `retro_run` call reached
**38.8 ms** on the client (`cpu` residue 37.1 ms) against a 25 ms budget. At ~100 packets/sec a
stall that size is ample to open the gap. At level 1 `cpu` is undifferentiated — translated code,
memory stubs, timers, and backup-memory emulation in one bucket — so the spike cannot be
attributed without the inner brackets.

---

## ADR-0050 — Split the transmit queue into hot metadata and cold payload

**Status:** accepted. Behaviour-preserving; netdrv unit suite green before and after.

**The measurement that prompted it.** With `core_phase=2` on two consoles at 40 fps, the
emulated-CPU call counters came back *identical* — `cnt=677/29/448` host, `676/29/448` client —
while `cpu` mean was 4680 µs on the host and 8113 µs on the client. Same number of emulation-loop
iterations, same DMA count, same sound-timer count, 1.73x the time. `jit` was ~0, SMC flushes were
0, backup-memory calls were 0. The emulated GBA is not doing more work on the client; the same
work is running slower.

**The layout that explains it.** Per peer:

```
nd_txslot (old) = 168 B  ->  txq[384]     = 64512 B
nd_rxslot       = 150 B  ->  reorder[128] = 19200 B
                             PER PEER     = 83712 B      D-cache is 16384 B
```

`arq_tx_due()` and `arq_pump_tx()` each walk the whole in-flight window every pump to read one
8-byte timer per slot, and on some paths the window is walked twice. Interleaved with payloads
that stride is 168 B across 21504 B — 128 of the PSP's 256 data-cache lines touched to read 1 KB
of timers, dragging 144-byte payloads it never reads into cache alongside them. The client is the
console that actually fills that queue (`txq_hi` pinned at 106 vs the host's 12, inbound bursts of
9 packets a frame vs 2), which is precisely the asymmetry the `cpu` residue shows.

**The change.** `nd_txslot` becomes two parallel arrays: `nd_txmeta txq[]` (24 B: the two
timestamps, seq, len, rtx) and `uint8_t txpay[][ND_MAX_PAYLOAD]`. The hot walk is now 24-byte
stride over 3072 B contiguous — 48 cache lines instead of 128, and sequential, so the prefetcher
can do something with it. Payloads are touched only when a packet is actually (re)transmitted.

A compile-time assert pins `sizeof(nd_txmeta) <= 24`, with a comment explaining that new fields
belong in the payload array or in `nd_peer`. Verified by negative control: adding a `uint64_t`
fails the build.

**Why this is the prerequisite for the scratchpad, not a detour.** The PSP has 16 KB of on-chip
scratchpad at `0x00010000` sitting entirely unused. A 64512-byte txq could never live there. The
9216-byte metadata array for a full 384-slot queue fits with 7 KB to spare. The split is what turns
"use the scratchpad" from impossible into a placement decision.

**Status of the causal claim.** That D-cache contention explains the 1.73x `cpu` asymmetry is a
*hypothesis*. It fits every observation — identical call counts, scales with network load,
directional toward the console that queues more — but it is not yet measured. The test is direct:
if the split moves client `cpu` mean toward the host's on otherwise identical runs, the theory
holds. If `cpu` does not move, the theory is wrong and the cache is not where those 3.4 ms go;
the layout change is still correct on its own terms, but the asymmetry would need another
explanation.

**Not done here:** the renderer. `vid` runs 3.0-6.5 ms mean, the largest cost the frontend owns,
and remains the target for headroom and fast-forward. It is deliberately *not* the fix for the
trade failure — `vid` is steady, and the frame that likely opens the network gap is a 40 ms
outlier whose 27.8 ms lives in `cpu`.

### ADR-0050 addendum — the hypothesis was wrong, and so was the number that motivated it

Tested on hardware, both consoles, `core_phase=2 gu_defer=1` at 40 fps, same build either side of
the change. The pre-stated test was: *if the split moves client `cpu` mean toward the host's, the
D-cache theory holds; if `cpu` does not move, the theory is wrong.*

```
                 cpu mean (all windows, n=15-16)
                 host      join      ratio    gap
fat  layout      5945      6831      1.15     +886 us
split layout     6155      6789      1.10     +635 us

client: 6831 -> 6789   (-0.6%)      host: 5945 -> 6155  (+3.5%)
```

**`cpu` did not move. The D-cache-contention hypothesis is not supported.**

**Two errors, both mine, both avoidable.**

*The magnitude was never plausible.* The hot scan misses ~128 cache lines, walked at most twice
per pump, once per frame. At ~150 cycles a miss on a 333 MHz Allegrex that is roughly
`256 * 150 / 333e6` = **115 us/frame** — about 1.7 % of a 6800 us `cpu` mean. The split could not
possibly have recovered 3433 us. The cache-line count was correct and the time it implied was
never checked. Count the cycles before believing a layout change is worth milliseconds.

*The 1.73x asymmetry was a single-window artifact.* It came from comparing the last `EVT
core_phase` line of each log. Across every window the asymmetry is 1.15x (886 us), not 1.73x
(3433 us) — real, but half the size, and unremarkable enough that it would not on its own have
justified this work. The rule this project already had (report extremes and changes, never a
single sample) was not applied to the sample that started the investigation.

**What stands regardless.** The layout was genuinely wrong: 83712 B of per-peer ARQ state against
a 16384 B D-cache, with a hot timer scan striding 168 B to read 8. The split is correct, the unit
suite is green either side, the `sizeof(nd_txmeta) <= 24` assert prevents regression, and the
9216 B metadata array is what makes scratchpad placement possible at all. It is simply worth
~1 %, not ~50 %.

**Unattributed:** the same run was the first with clean Union Room exits on BOTH consoles, and the
transport was the healthiest measured — client `retx` 1363 -> 601, both peers' RTO back at the
200 ms floor, `srtt` symmetric at 91/95 ms. That improvement is NOT claimed for this change: the
mechanism proposed for it has just been falsified, and run-to-run variance today has been large
enough to produce a swing this size on its own. Establish it by repeating the identical build
before attributing anything.

**Still open:** the 1.15x `cpu` asymmetry has no explanation. `cnt` remains identical across
consoles (677-679/29/448), so the emulated GBA executes the same work either side; something makes
it ~600-900 us/frame slower on the client and it is not the ARQ layout.

---

## ADR-0053 — Unattended hardware loop, milestone 1: the console hands over its own memory stick

**Status:** implemented, PC half tested, **console half unproven on hardware.**

Every measurement in this project costs a human: plug in, launch, walk into the Union Room,
trade, walk out, unplug, swap consoles, repeat. That cost is why runs are precious and why
"just run it again" is a bad ask. Milestone 1 removes the plugging.

**The console never exits to the XMB.** This was the design's main open question and the answer
is that nothing about it needs the XMB:

- `sceUtilityLoadUsbModule(PSP_USB_MODULE_PSPCM)` loads the storage modules from **user mode** —
  the build is `PSP_MODULE_INFO(..., 0, ...)` + `THREAD_ATTR_USER`, and ARK-4 6.61 permits this.
- `sceUsbActivate()` makes **this process** the mass-storage device. Nobody navigates to
  "USB Connection"; the emulator hands out its own stick mid-process.
- `sceKernelLoadExec()` relaunches **this process** into whatever the PC just staged.

The XMB is reached only if the loop gives up and falls through to `sceKernelExitGame()`, which
is the intended failure mode.

**One USB transition per run.** The obvious design — flip USB off periodically to poll for a
command file, flip it back on — races the PC's view of the volume on every cycle. Instead the
window opens once and closes on whichever comes first: the PC mounts us and then ejects (the
intended path; `PSP_USB_CONNECTION_ESTABLISHED` makes the eject observable, so a fast script
finishes in seconds instead of always paying the full window), or the window expires. The
command file is read only after the volume is ours again.

**Ordering is the safety property.** `handoff_run()` is called after `evt_shutdown()`, which is
after `net_teardown()`, `io_thread_stop()` and `fe_host_shutdown()`. Every thread that can touch
ms0 is stopped and the log is closed before USB is offered. The handoff keeps its own breadcrumb
file (`handoff/STATE.TXT`) written with raw synchronous `sceIo` calls, because the event log is
gone by then and the first hardware run of this file is the one most likely to fail halfway.

**Off unless asked.** `handoff = 1` lives in `.gpsp-harness.ini`, never `CONFIG.INI` — the same
reasoning as ADR-0036, where a stale `autopilot.ini` auto-hosted a wireless session during a solo
benchmark three times. A stale file that silently toggles USB on a console someone is playing is
that failure again. `handoff_window_s` (90) and `handoff_max_runs` (20) bound it.

**PC half (`tools/hw_loop.py`), tested against a simulated pair of cards:**

| behaviour | verified |
|---|---|
| collects both logs, rotates them off the card | yes |
| role-qualified staging (`join-CONFIG.INI` -> client only) | yes — host got `rfu_rx_cap=0`, client `rfu_rx_cap=2` |
| prefixed source files not left behind on the card | yes |
| writes `RUN` mid-chain, `STOP` on the last run | yes |
| only one console present -> times out, exit 1 | yes |
| nonzero exit -> stops the chain, exit 2 | yes |
| `--keep-going` -> continues and asks for `RUN` | yes |

Role-qualified staging is the feature that matters: it is what lets a run put a different config
on the client than the host, which is exactly the shape of the experiments this project keeps
needing.

**Deliberately NOT deployed.** The cards carry `D1B13BF5` + `rfu_rx_cap=2`, the build the pending
Union Room exit experiment was configured for. The handoff build is `89BC77C7` and waits for its
own validation slot. Staging an unvalidated binary under a pending experiment produces a result
that cannot be attributed to either change.

**What is genuinely unproven.** The console half has never run. Static verification goes as far
as it can — the import stubs (`__stub_module_sceUsb`, `__stub_module_sceUsbstorBoot`,
`__stub_module_sceUtility`) are present, `handoff_config`/`handoff_run` are linked at real
addresses, and nothing is unresolved — but linking is not running. Specifically untested: whether
`PSP_USB_MODULE_PSPCM` is the correct module on ARK-4 6.61, whether user-mode `sceUsbActivate`
succeeds there, and whether `sceKernelLoadExec` back into our own EBOOT works from this state.
First bring-up should set `handoff_window_s` generously and read `handoff/STATE.TXT` afterwards;
it records each step precisely so a partial failure is diagnosable without guessing.

### ADR-0053 addendum — validated on the rig, and the bug it caught

`tools/e2e/run_handoff_test.sh` runs the real loop against two PPSSPP instances: the shipping
EBOOT, the real `hw_loop.py`, real `sceUsb*` calls, real `sceKernelLoadExec`. Nothing in the PSP
binary is conditional on the emulator.

```
PASS: 4 chained runs on two consoles, self-relaunched, logs collected
  host: relaunches=3  usb_fail=0  loadexec_fail=0  RUNS.TXT=4
  join: relaunches=3  usb_fail=0  loadexec_fail=0  RUNS.TXT=4
  collected logs: 8/8, all carrying a build stamp
```

**Negative control passes too**: `--negative` disables the handoff and requires the assertions to
fail. They do — "no STATE.TXT, handoff never ran", 0/4 logs. Three vacuous gates have shipped in
this project already; a gate that cannot fail measures nothing.

**The bug the rig caught before hardware could.** The window was originally cut short by watching
`PSP_USB_CONNECTION_ESTABLISHED` (0x002) rise when the PC mounted us and fall when it ejected.
PPSSPP names the same bit `USB_STATUS_STARTED`, and PPSSPP's name is the correct one. The console's
own trace settles it:

```
usb state 0x222 at 0ms ACTIVATED CABLE STARTED
```

All three bits are set at **0 ms**, immediately after `sceUsbStart`, with no host involved. The
original logic would have latched "mounted" on its first poll and then waited for a bit that only
clears on `sceUsbStop` — never detecting an eject, never shortening a window. `PSP_USB_CABLE_CONNECTED`
(0x020) does not help either: that is the physical cable, and ejecting a volume in Windows unmounts
it without unplugging anything.

**There is no reliable "the PC has finished" signal in the USB state.** The design now needs none:
serve the window, release the volume, look for the command file, re-arm if absent. The USB state is
logged for diagnosis and never used as control. That this path is identical on hardware and in the
rig is the reason the rig result transfers at all.

**What the rig does NOT establish, and must not be claimed:** real USB enumeration by a host OS, and
whether the PSP's filesystem sees the PC's freshly written `CMD.TXT` rather than a cached directory
entry. Both processes share one kernel's page cache here, so that question is answered on hardware
or not at all. It remains the most likely first-contact failure.

### ADR-0053 addendum 2 — the harness, finished as far as the rig can take it

Three more holes closed after the design review, each with a test that fails without the fix.

**1. Every early exit now hands over.** `main()` had EIGHT exit paths that called
`sceKernelExitGame()` directly — no ROM, bad variant, load failed, bad script, nettest fail, and
**net_failed**. None reached `handoff_run()`. A console that failed to start therefore vanished to
the XMB, published nothing, and the PC waited for a result that was never coming. In an unattended
chain that is the likeliest failure of all: the host being slow to come up after a relaunch makes
the client's adhocctl group join fail, and one dead console ends the chain. All eight now publish
their result and hand over.

Tested by `--startup-fail`, which withholds the ROM so `find_first_rom` fails deterministically:

```
host: published run=1 exit=2 reason=no_rom
join: published run=1 exit=2 reason=no_rom
published=2/2  early-exit-handed-over=2      PASS
```

The first version of that test forced the failure by making both consoles JOIN with nobody
hosting — but whether adhocctl fails depends on which instance wins a startup race, so it passed
or failed at random. A non-deterministic test is worse than none.

**2. The golden restore was erasing its own evidence.** `collect()` restored the baseline save
before anything read the post-run save — so the only proof a trade occurred was destroyed by the
mechanism that makes runs repeatable. The post-run save is now copied off the card first and kept
beside the log as `autoNNN-<role>-EMERALD.SAV`.

**3. The chain now verifies trades, not just exits.** `exit=0` proves the input script reached its
last line. `--verify` decodes the post-run save with the existing party oracle
(`tools/e2e/read_party.py`) and requires the party to DIFFER from the golden baseline. Deliberately
a weak oracle — the full who-got-whose-mon check needs both consoles' pre-state and lives in
`run_trade_test_psp.sh` — but it needs only the baseline, so it can run on every link unattended.
An unreadable save returns "unknown", never "unchanged": a save we failed to decode is not a pass.

Verified three ways: identical saves -> `no trade occurred`; the two consoles' real saves (which are
permutations of each other, exactly what a completed trade leaves) -> `party changed`; a missing
file -> `unknown`. And **in situ**: the handoff rig test boots and exits without ever trading, so
the oracle must report NOT VERIFIED every time. It does — `not-verified=6 false-positives=0`. That
negative assertion is now part of the test, because an oracle that cannot fail is worth nothing.

**Rendezvous, checked rather than assumed:** `ND_S_JOINING` re-broadcasts JOIN every 500 ms with no
deadline — netdrv never gives up. The bound is elsewhere: `net_bringup` blocks in adhocctl group
join for up to 30 s, and if that fails the console now exits *and hands over* rather than
disappearing, so a lost rendezvous costs one run instead of the chain.

**Suite status:** positive (3-4 chained runs, both consoles self-relaunching) PASS; negative control
(handoff disabled -> assertions must fail) PASS; startup-fail PASS.

**Still only answerable on hardware:** real USB enumeration by a host OS, and whether the PSP sees
the PC's freshly written `CMD.TXT` or a cached directory entry. Both rig processes share one
kernel's page cache, so the rig cannot speak to it.

---

# THE phase6-coreopt ADR SERIES (merged 2026-08-09)

**NUMBER COLLISION - HANDOVER 4.1.**  The sections below are the
phase6-coreopt branch's OWN ADR-0030..0036, written in parallel with the
mainline numbers above and describing ENTIRELY DIFFERENT decisions (the
video oracle, the renderer path profile, the overdraw measurement).  Never
cite one of these numbers without saying 'coreopt'.

## ADR-0033 — Nothing in the renderer gets optimised until a frame-exact oracle exists, and the oracle has been made to fail on purpose

- **Status:** accepted 2026-08-02, branch `phase6-coreopt` (worktree `wt-vfpu`, cut from `main`).
  Note on numbering: `main` carries ADRs up to 0029; 0030-0032 live on unmerged branches
  (`phase5f-smcaddr`, `phase5g-smcblock`, `phase5h-corebracket`). This ADR takes 0033 to avoid a
  collision when those land. It depends on ADR-0032's finding (the software scanline renderer is
  one of the two largest phases of the core frame) but not on its code.

- **Context.** The next work item is optimising `video.cc`, the core's software scanline renderer.
  A renderer optimisation that is subtly wrong is worse than no optimisation: the failure mode is a
  few wrong pixels in one blending path in one scene, which no boot test, trade test or save test
  can see. `run_gu_color_test.sh` checks the *GU blit*, downstream of the renderer, and compares one
  frame. It is the wrong instrument for this and always was.

- **Decision — hash the CORE's output buffer, every frame, and diff against a golden run.**
  - The frontend hashes the pointer `retro_video_refresh` handed it (`fe_host_last_frame`), which is
    **upstream of our GU blit** — so this tests the renderer, not the presentation path.
    FNV-1a over the 240x160 halfwords, emitted as `EVT vhash f=<frame> h=<hash>`. A golden run is
    just the ordered list of those lines: text, diffable, and it names the first frame that broke.
  - Enabled only by `autopilot.ini`'s `vhash=1`. Off, it is one branch per frame.
  - Workload: `testdata/fixtures/emerald_vregress.inputs` — intro fades/affine/bitmap, title,
    main menu, then `ff off` and every frame rendered: overworld scrolling in all four directions,
    the start menu (window + blended panel), and the party screen (dense OAM). 1090 frames, of
    which ~600 are rendered at full rate. The script must reach all six of its markers or the run
    is failed rather than passed, so a "pass" can never quietly mean "covered less".
  - `tools/e2e/run_video_regress.sh` with four modes: `--selftest`, `--capture`, compare (default),
    and `--perturb=N`.

- **The three things that make it evidence rather than decoration.**
  1. **Determinism proved before goldens were trusted.** `--selftest` runs the workload twice and
     diffs the two runs: **1090/1090 frames identical.** Without this a golden is a coin flip.
  2. **It has been made to fail, twice, on purpose.** `--perturb=N` rebuilds the core with a
     deliberate fault in `video.cc` (`VREGRESS_PERTURB`, 0 in every normal build), runs, asserts the
     comparison FAILS, and rebuilds clean:
     - `--perturb=1` flips **one bit of one pixel (x=120, y=80) per frame** -> **1090 of 1090**
       frame hashes changed. Single-pixel sensitivity is measured, not claimed.
     - `--perturb=2` makes `merge_blend` **forget to saturate the blue channel** — a plausible
       subtle bug, exactly the class a vectorised rewrite introduces -> **18 of 1090** frames
       changed. It catches a path-specific fault that only 1.7 % of the workload even reaches.
  3. **It goes green again.** After the perturbation runs rebuilt the tree, the default compare
     passes 1090/1090 against the golden. A harness that cannot return to green is not a gate.

- **Cost and shape.** The hash is ~38 400 iterations a frame — real, but it runs only in oracle runs.
  `Makefile` gains `CFLAGS += $(EXTRA_CFLAGS)` so a one-off `-D` build needs no Makefile surgery.
  `psp/Makefile`'s `INCDIR` gains `..`.

- **Gates.** `run_boot_test.sh` PASS, `run_save_test.sh` PASS, `run_video_regress.sh` PASS (golden),
  `--selftest` PASS, `--perturb=1` and `--perturb=2` both correctly FAIL the comparison.

- **Honest limits.** (a) It is one game, one save, one route — it proves *no regression on what it
  covers*, and its coverage is the marker list above, not "the GBA". Mode 3/5 bitmap, obj-window
  and mosaic paths are touched lightly or not at all. (b) It runs on PPSSPP; it validates
  *arithmetic*, and would not catch a fault that only appears with real Allegrex cache or alignment
  behaviour. (c) FNV-1a/32 could in principle collide; on the evidence above it has never masked a
  change, and a collision would have to be constructed.

## ADR-0034 — Where the renderer's time actually goes: it is ONE loop (4bpp tiled text BG, 4x overdrawn), and the VFPU targets in the brief are 0-4 % of it

- **Status:** accepted 2026-08-02, branch `phase6-coreopt`. Instrumentation only, **zero behaviour
  change**; `video_prof.h` compiles to nothing unless `-DVIDEO_PROF` is passed.

- **Context.** ADR-0032 established the renderer's size but not its shape. `video.cc` has ~20
  distinct paths (text / mosaic / affine BG, 4bpp vs 8bpp, sprites regular / affine / mosaic, four
  colour-effect modes, eight window configurations, three bitmap modes). The proposed work — VFPU
  vectorisation of "tile/sprite compositing, alpha blending, the palette-to-RGB conversion" —
  presumes which of them dominate. That presumption had never been measured.

- **Decision — counters for structure, levelled clocks for cost, and the probe priced by A/B.**
  `video_prof.h` adds per-frame counters (calls and PIXELS per path — hardware-independent, so they
  transfer from the rig to a PSP unchanged) and four nested-guarded timing brackets. Two levels:
  **1** = counters + the `update_scanline()` total (320 clock reads/frame); **2** = + per-path
  brackets (~1820). The frontend emits `EVT vid_prof` / `EVT vid_worst` per window and cross-checks
  against `fe_host_core_prof()`'s `core=` **over the same window**, so the renderer's share is a
  ratio of two measurements rather than of one measurement and a remembered number.

- **The probe prices itself, and two independent estimates agree.** Level 1 -> 2 over four steady
  windows: `ttot` rose 856/847/842/857 us for 1504/1503/1479/1504 extra reads = **569/564/569/570 ns
  per read**; `core=` rose 852/850/837/850 over the same windows = **566/565/566/565 ns**. Two
  brackets, two quantities, one answer: **~567 ns/read on the rig** (ADR-0032 measured 519 ns
  independently, from a different workload — same order).

- **The measurement.** PPSSPP rig, `emerald_vregress` workload, four consecutive 100-frame windows
  of steady overworld walking. The counters are identical in both builds, so the level A/B is clean:

  | quantity | value | note |
  |---|---|---|
  | `core=` (whole `retro_run`) | **6396 us** (level 1) | ~6215 with the level-1 probe removed |
  | `ttot` renderer total | **3474 us** | **~56 % of the core frame** |
  | `tbg` tiled BG rendering | **3454 us raw** | **89-99 % of the renderer** (bound below) |
  | `tobj` sprites | **157 us raw** | **3-4.5 %** |
  | `tmrg` blend + brightness | **0 us** | not one microsecond in steady overworld |
  | `tfil` backdrop fill | **0 us** | |

  - **Bracket correction, stated as a bound rather than a point.** A bracket's measured span
    includes an unknown fraction (0 to 1) of one clock read, because `sceKernelGetSystemTimeLow`
    samples mid-call. `tbg` spans 640 brackets a frame, `tobj` 112. So `tbg` is in [3091, 3454] and
    `tobj` in [93, 157]. At the midpoint (0.5 read/bracket) `tbg`=3272, `tobj`=125 and the residue
    (`order_obj`, `order_layers`, window dispatch, per-line setup) is **77 us** — coherent. At the
    extreme correction the residue goes negative, which is how we know 1.0 is too aggressive.
    **Best estimate: BG tiles are ~94 % of the renderer, +/-5.**

- **The structural counters are exact, and they explain it.** Per frame in the overworld:
  `txtc=640 txtpx=153600` — **four full-width tiled text layers on every one of 160 scanlines.
  153 600 background pixels rendered for a 38 400-pixel screen: 4.0x overdraw.**
  `txt8c=0` — **not a single 8bpp text call**; every one of the 640 is 4bpp.
  `enone=160`, `objbld=0` — **no colour effect is active at all**, so the hot instantiation is
  exactly `render_scanline_text_fast<u16, FULLCOLOR, isbase, false>` ->
  `render_tile_Nbpp<u16, FULLCOLOR, /*is8bpp=*/false, isbase, hflip>`. One function, four template
  instantiations, ~94 % of the renderer. Cost: **20-22 ns per BG pixel on the rig.**

- **This retargets Phase 3, and says so plainly.** The three VFPU candidates named in the brief
  measure, in the workload that matters:
  - **alpha blending / brightness: 0 %** of steady overworld. It is 14 % on the title/menu windows
    (`mrgpx=9216` at ~59 ns/px, `brtpx=12600` at ~49 ns/px) — expensive per pixel, and rare.
  - **sprite compositing: 3-4 %** overworld, rising to 16 % in the start menu (391 sprites/frame)
    and **20 % on the party screen** (581 sprites/frame) — worth something, never the frame.
  - **palette-to-RGB conversion:** already done once per palette write into `palette_ram_converted`,
    not per pixel. There is nothing there to vectorise.
  - **The dominant loop is a 4-bit index -> 16-entry palette GATHER plus a predicated 16-bit store.**
    The Allegrex VFPU is a 4x32 float unit with no gather, no integer bitwise ops and no
    data-dependent lane permute. **It cannot do this loop.** Reporting that now is cheaper than
    discovering it after writing the assembly.
  - The levers that *do* fit the measurement are scalar and structural: collapse the per-pixel
    nibble-extract + table-load + halfword-store into byte-pair (2 px) table loads and 32-bit
    stores, and exploit the base layer's total absence of transparency. That is the Phase-3 target.

- **Rig vs hardware, and an unresolved discrepancy that must not be papered over.** This is a rig
  measurement; PPSSPP models neither the Allegrex caches nor uncached writes, so absolute times do
  not transfer. The *counters* do. Separately: the brief for this workstream states a hardware split
  of `cpu` 7.75 ms / `vid` 3.07 ms / core 10.8 ms, i.e. video is 22 % of the core frame on hardware
  against 46-56 % on the rig — the dynarec 3x slower on hardware while the renderer is unchanged.
  **Two things about that figure need resolving before it is planned around.** (1) No hardware
  `EVT core_phase` line exists anywhere in this repo, in any branch, or in the two test kits;
  ADR-0032 closes by asking for exactly that run and nothing since records its return. (2) 3.07 ms
  is numerically identical to ADR-0032's *rig* `vid` (3235 us) minus its own stated probe cost
  (166 us) = 3069 us. That may be coincidence, but it is the kind of coincidence that has to be
  checked rather than assumed. It also runs against ADR-0032's own reasoning, which predicted the
  field's video share should be **higher** than the rig's, not half of it, because the renderer is
  the most memory-bound part of the core. **Action: obtain the raw hardware `EVT core_phase` line.**
  The Phase-3 target above does not depend on which figure is right — BG tiles are ~94 % of the
  renderer either way — but the *value* of the whole workstream does.

- **RESOLVED 2026-08-02 — the hardware line exists, the doubt was right for the wrong reason, and
  the divergence is itself the finding.** The field run is now in the tree: `docs/FIELD-CORE-PHASE.md`
  (`da8df27`), transcribed verbatim from the user's own PSP-1000/PSP-3000 pair, Build E,
  `core_phase=2`. Steady in-session window on the joining console:
  `tot=14253 cpu=7754 vid=3069 blt=2973 amix=313 aout=123`, i.e. **`cpu` ~54 %, `vid` ~22 %,
  `blt` ~21 %, audio ~3 %**.
  - **My inference was wrong; raising it was still correct.** I suspected 3.07 ms was ADR-0032's rig
    figure minus its probe (3235-166=3069). It is not — it is a raw hardware mean with the level-2
    probe *active*, and the match to three digits is coincidence. What was true, and what justified
    the challenge, is that the number could not be verified against anything in the repo. That is
    indistinguishable from a fabricated figure, and the fix was to commit the raw line, which has
    now been done.
  - **ADR-0032's prediction failed, and the failure is instructive.** It reasoned that the field's
    video share should be *higher* than the rig's because the renderer is the most memory-bound
    part of the core. Hardware shows the opposite: **22 % against the rig's 59 %.** The reason is
    not that video got cheaper but that `cpu` got far more expensive — 7754 us on hardware against
    2443 on the rig, **3.2x** — while `vid` barely moved (3069 hardware vs 3235 rig, both
    probe-inclusive, hardware ~5 % *faster*). PPSSPP under-models dynarec dispatch, icache and
    uncached-write cost dramatically and models straight-line scanline rendering well.
  - **Two consequences, both actionable.** (1) **Rig ratios are a hypothesis about hardware, never
    a substitute** — now demonstrated twice on this project, and it should be treated as a standing
    rule. (2) But the converse is also measured and is good news for this workstream: **for the
    renderer specifically the rig is a faithful proxy** (within ~5 % in absolute microseconds), so
    a Phase-3 A/B measured on the rig can be believed for `vid` even though the same rig's *share*
    of frame cannot. Nothing here needs a hardware run to iterate; only the final number does.
  - **Re-scoped target.** The renderer is **3069 us of a 16750 us frame = 18 %**, and BG tiles are
    ~94 % of it — **~2.9 ms, ~17 % of the frame, in one function.** Halving it returns ~1.45 ms
    (~8.7 % of frame). `cpu` is larger but gated to Phase 4; `blt` (2973 us, our own GU upload) is
    larger still but lives in the frontend, not the core, and belongs to another workstream.

- **DO NOT RE-TRY THE VFPU ON THIS RENDERER.** Stated plainly so nobody spends a second week on it:
  the loop that is ~94 % of the renderer is a **4-bit index into a 16-entry palette (a gather) plus
  a predicated 16-bit store**. The Allegrex VFPU is a 4x32 *float* unit with **no gather, no integer
  bitwise operations and no data-dependent lane permute**; a 16-way select would take ~15 `vcmov`s
  to replace one `lhu`. The three targets originally proposed for it — alpha blending, sprite
  compositing, palette→RGB conversion — measure **0 %, 3-4 % and 0 %** respectively in the
  workload that decides the frame. The remaining win here is **algorithmic (the 4x overdraw), not
  SIMD**, and being algorithmic it is portable — which makes it the better upstream contribution.

## ADR-0035 — Phase 3 attempt 1 (byte-pair palette LUT) is REVERTED: pixel-exact but measurably slower, and its control run did not reconcile

- **Status:** rejected 2026-08-02, branch `phase6-coreopt`. The code is **not** in the tree; this
  ADR is the record, because the negative result is the deliverable. Nothing about ADR-0033 (the
  oracle) or ADR-0034 (the profile) depends on it, and both remain in place and green.

- **What was tried.** ADR-0034 identified one target: `render_tile_Nbpp<u16, FULLCOLOR, 4bpp,
  isbase, hflip>`, ~94 % of the renderer, seven instructions a pixel (nibble extract, add
  sub-palette base, scale, add table base, `lhu`, `sh`, branch). A 4bpp tile byte holds two pixels,
  so a 256-entry byte-indexed table per sub-palette turns a pair into one 32-bit load and — when
  the destination is 4-byte aligned — one 32-bit store:
  - `tile4_pair[sp][b]` the pair's two RGB565 colours; `tile4_opaq[sp][b]` a 2-bit opacity mask.
  - Index 0 mapped to palette entry 0, so **one** table served both the base layer (store
    unconditionally, no branches at all) and overlay layers.
  - Invalidation via new per-sub-palette generation counters (`palette_gen_sub[16]`,
    `palette_bg_dirty()`) bumped from the three `write_palette*` macros and savestate load.
  - Compile-switched (`GPSP_FAST_TILE4`), 20 KiB of `.bss`.

- **It was correct.** The ADR-0033 oracle passed **1090/1090 frames pixel-identical to the golden**
  with the fast path enabled. That is the one thing this attempt got right, and it is exactly what
  the oracle was built for: the idea could be discarded on performance grounds alone, with
  correctness never in question.

- **It was slower, on the workload that matters.** Level-1 profile, four steady overworld windows,
  identical counters (so identical emulated work):

  | build | `ttot` (renderer, us) | vs baseline |
  |---|---|---|
  | baseline (ADR-0034 tree) | 3481 / 3464 / 3361 / 3593 | — |
  | `GPSP_FAST_TILE4=1` | 4450 / 4459 / 4378 / 4395 | **-28 %** |

  On the one-layer copyright/title screens the same build ran `ttot` 901/948 against the baseline's
  1382/1450 (**+34 %**). So the table helps dense, opaque, few-sub-palette screens and hurts the
  four-layer overworld, which is the case that decides the frame.

- **One hypothesis was tested and falsified, which is why the counter existed.** The first guess was
  invalidation thrash: Emerald animates palettes, and the initial design used a single global
  generation, so every palette write rebuilt every table. A `t4bld` counter was added to the
  ADR-0034 profiler to measure it rather than argue about it. With per-sub-palette invalidation
  `t4bld` fell to **6-7 rebuilds a frame** (~1800 iterations, well under 1 % of the phase) and the
  timing **did not move at all** — byte-identical, because PPSSPP's `sceKernelGetSystemTime` is
  derived from emulated cycles and is therefore deterministic. The regression is not rebuild cost.
  A second hypothesis (the fast path had dropped the scalar code's all-transparent-row early-out)
  was also implemented and also moved nothing, which says those rows are rare.

- **And then the control run did not reconcile, so nothing is being claimed.** A build with
  `GPSP_FAST_TILE4=0` — which differs from the baseline only by the dormant code and the
  `palette_bg_dirty()` hook, and can therefore only be equal or slower — measured `ttot`
  3086/3060/2924/3168, i.e. **12 % FASTER than the baseline**. That is not a possible result. A
  freshly rebuilt true baseline from `HEAD` reproduced the original numbers to within 1 us
  (3481/3464/3361/3593 vs 3480/3463/3360/3593), so the baseline and the rig are sound and the
  fault is somewhere in that one build or its run. **Since the -28 % and the +34 % were produced by
  the same unexplained measurement chain, none of the three figures above can be treated as
  established, and the change was reverted rather than left in the tree default-off.** Leaving code
  whose measurement does not add up would be exactly the failure this project has repeatedly
  avoided.

- **What a second attempt must do first**, before any renderer code is written again:
  1. **Make the A/B mechanical.** Every measurement here rebuilt in place, and `rsync -a` preserving
     mtimes silently skipped a source sync at least once (caught, but only by noticing that two
     different builds produced byte-identical output). A `--ab` mode in `run_video_regress.sh` that
     builds both variants into separate trees, runs both, and prints the delta would have made the
     discrepancy impossible to miss and cheap to bisect.
  2. **Record the build identity in the log.** `EVT vid_prof` should carry a hash of the EBOOT it is
     running, so a stale binary can never be mistaken for a result.
  3. **Reconsider the target.** The instruction-count argument for the pair table is sound
     (~2.5 vs ~7 instructions a pixel) but the overworld's overlay layers are dominated by
     *transparent* pixels, which the scalar path already skips in two instructions and the table
     path cannot beat. A better shape is probably to attack the base layer only (1 of the 4 layers,
     no transparency at all, so a pure unconditional 32-bit-store fill) and leave the overlay
     layers alone.

- **Gates after the revert.** Tree is byte-identical to ADR-0033/0034's commit.
  `run_video_regress.sh` PASS (1090/1090), `run_boot_test.sh` PASS.

## ADR-0036 — The 4x overdraw is real and half of it is thrown away: 52 % of background stores are overwritten before the frame is shown, and the base layer is 93 % covered

- **Status:** accepted 2026-08-02, branch `phase6-coreopt`. Measurement + measurement-chain repair.
  No behaviour change: `VIDEO_PROF=3` and the build-CRC line are compile-switched and off by default.
  This ADR answers question (a) — *is the overdraw real, and how much of it is genuinely occluded
  rather than transparent-and-needed* — and sizes the two candidate fixes. It does **not** implement
  one; that is a separate change, and ADR-0035 is the reason it will not be attempted until the
  measurement chain has been proven.

### Part 1 — the measurement chain, repaired first (ADR-0035's three fixes)

ADR-0035 had to discard three numbers because the chain that produced them was untrustworthy:
one `rsync -a` silently skipped a source sync (mtime-preserved), and a control build measured
12 % faster with the feature compiled out. Both are now structurally impossible to miss:

1. **The binary identifies itself.** `EVT build eboot_crc=<crc> size=<n>` at boot, and `bld=<crc>`
   in every `EVT vid_prof` line. The CRC is taken over the EBOOT the console is actually running,
   not a compile-time stamp — a stale EBOOT reports its own old CRC, which is the point.
   This was immediately load-bearing: two "different" runs during this work reported the identical
   `bld=f216ccd6`, which is how the stale binary was caught in seconds instead of hours.
2. **`tools/e2e/run_video_ab.sh` — never rebuild in place.** Each side is built into its own tree
   (`git archive <ref>` or a working-tree copy with all objects/archives/EBOOTs excluded), from
   scratch. The script **fails the comparison**, rather than reporting a delta, if: the two sides
   produce identical EBOOTs; the two runs report the same `bld=`; or the two runs' structural
   counters (`lines`, `txtpx`, `txtc`, `objspr`) differ in any window — the last one meaning the two
   sides did not render the same frames and no timing delta could be meaningful.
3. **Steady windows are marked, not eyeballed.** The report flags windows with `txtpx=153600`
   (four full-width layers = the overworld) and reports the mean over exactly those.

### Part 2 — the overdraw, measured

`VIDEO_PROF=3` tracks coverage per STORE: layers paint back to front, so a store to a pixel already
stored on this scanline means the *earlier* store was wasted. Steady overworld, four 100-frame
windows, per frame:

| quantity | value | |
|---|---|---|
| `txtpx` BG pixels **requested** | 153 600 | 4 layers x 240 x 160 |
| `bgwr` BG pixels **actually stored** | **79 509** | 52 % — the other 48 % were transparent and skipped |
| `bgocc` stores later **overwritten** | **41 109** | **51.7 % of all stores are wasted** |
| survivors | 38 400 | |

- **The partition closes exactly**: 79 509 - 41 109 = **38 400 = the screen, to the pixel.** Every
  store either survives to the framebuffer or is overwritten, and the counters say so with no
  residue. That is the check that makes the rest of this credible.
- **Per layer** (`bgl3` is drawn first, `bgl0` last): `bgl3=38 400` every window — **the base layer
  writes every pixel of the screen, always** — then `bgl2≈35 838` (**93.3 % of the screen**),
  `bgl1≈1 787`, `bgl0≈3 482`. And `bgocc ≈ bgl0+bgl1+bgl2` to within one pixel per 100 frames:
  **essentially every store by BG2/BG1/BG0 lands on a pixel that was already painted.**
- **So the waste is concentrated in one place.** It is not diffuse 4x overdraw: it is the base layer
  painting all 38 400 pixels and BG2 then covering 93 % of them. ~35 800 stores a frame — 45 % of
  all BG stores — are made by a layer that is about to be almost entirely hidden.
- **Other scenes, for scale**: start menu 44 % of stores wasted, party screen 31 %, title/menu 68 %.
  The overworld figure is not an outlier.
- **Tile-row granularity, which decides the design**: `trow≈19 132` full 8-pixel 4bpp tile rows are
  drawn per frame (the maximum is 4 x 30 x 160 = 19 200), and `trowfull≈9 605` of them — **50.2 %** —
  have all eight pixels opaque. A fully opaque tile row completely hides the corresponding eight
  pixels of every layer beneath it, so occlusion can be exploited at **tile granularity (30 tests a
  layer-line) rather than pixel granularity (240)**, which is what makes it cheap enough to be worth
  doing at all.

### Part 3 — the two candidate designs, and their honest risk

Both are algorithmic and portable — no Allegrex-specific code, which makes either a better upstream
contribution than any SIMD work (and the VFPU is ruled out for this loop, ADR-0034).

- **(A) Front-to-back with a coverage mask.** Render BG0 first, maintain a per-scanline coverage
  bitmap, draw lower layers only where uncovered, and fill whatever is still uncovered with the
  backdrop at the end. Upper bound on the win is the full 41 109 wasted stores (52 % of BG stores).
  **Risk:** it inverts the draw order, and the STCKCOLOR/blend paths depend on back-to-front
  stacking semantics. It must therefore be scoped to the FULLCOLOR, no-colour-effect case only —
  which is exactly the overworld (`enone=160`, `objbld=0`), so the scoping costs nothing where it
  matters and keeps every effect path untouched.
- **(B) Opacity pre-pass at tile granularity.** Keep the existing back-to-front order; before
  drawing, walk the upper layer's tilemap row and build a 30-bit "this tile column is fully opaque"
  mask; lower layers skip those columns. Captures the ~93 % of the base layer that BG2 hides, at
  30 tests per layer-line, and **changes no rendering semantics at all** — it only skips work that
  is provably invisible. Smaller ceiling than (A), much smaller blast radius.
- **The trap both must clear, and it is the same one that killed ADR-0035**: a store is one
  instruction, so any per-pixel test that replaces it must cost less than one instruction to be
  worth it. This is precisely why the 50.2 % fully-opaque tile-row figure matters — at tile
  granularity the test is amortised over eight pixels, at pixel granularity it is not. **(B) first**,
  because it is the one whose arithmetic obviously works and whose semantics cannot regress.

### Expected value, stated as a bound rather than a promise

Hardware (`docs/FIELD-CORE-PHASE.md`): `vid` = 3069 us of a 16 750 us frame (18 %); BG tiles are
~94 % of that (ADR-0034), so **~2.9 ms**. If (B) removes ~93 % of the base layer's stores and the
base layer is 48 % of stores, the ceiling is roughly a **third off `tbg`, i.e. ~0.9 ms, ~5 % of the
frame** — and only if store elimination translates proportionally into time, which it will not
exactly. Nothing here will be claimed until `run_video_ab.sh` reports it with both sides' `bld=`
differing and their counters identical.

- **Gates.** Tree builds clean at default (`VIDEO_PROF` unset), `run_video_regress.sh` PASS,
  `run_boot_test.sh` PASS. The level-3 build is a measurement tool only — it more than doubles the
  renderer's time (`ttot` 11 236 vs 3 481) and its clocks must never be quoted.
