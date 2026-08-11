# HANDOVER — the actual state of PSP AGB / GBAdhoc

## 0.0 SESSION 2026-08-10 — ME RENDERER: input contract PROVEN bit-exact on desktop

**Goal:** move the *whole* scanline renderer (not just the staging copy) onto the
Media Engine, so the ~6 ms/frame render leaves the main CPU — which also frees
main-CPU time for the WLAN driver (the real input-eating lever, ADR-0062 lineage).

**Desktop dev+validation loop established (no PSP needed for renderer LOGIC):**
The SDL twin (`sdl/gpsp_sdl`) builds+runs in WSL Ubuntu (native gcc/SDL2), linking
the SAME `video.cc`. Loop: edit on Windows → rsync to native fs → `make` (~10 s) →
run Emerald headless with the autopilot fixtures. PPSSPP/PSP-EBOOT path is NOT
available here (no psp-gcc in WSL); the ME *execution* remains hardware-only, but
the renderer *logic* is fully desktop-validatable.

**Two compile-gated probes added to `video.cc` (default OFF, zero cost — same
discipline as `video_prof.h`):**
- `RASTER_PROF` (`-DRASTER_PROF=1`): per-frame census of render-relevant LCD
  registers / palette / OAM / VRAM changing *between* scanlines.
- `RENDER_REPLAY` (`-DRENDER_REPLAY=1`): each rendered frame, re-runs the REAL
  renderer from ONLY the captured input snapshot+per-line-ioreg log into a shadow
  buffer and asserts bit-equality with the live frame. Verified non-perturbing
  (clean vs replay build → identical live frames from a pristine save).

**FINDINGS (the numbers that decide the ME renderer design):**
- Emerald is **98.3–99.9 % snapshot-safe**: across ~78 k measured frames
  (overworld / menus / party / save / Union Room / trade menu), almost nothing
  render-relevant changes mid-frame. The 60 k-frame soak is 99.9 % safe.
- The ONLY per-line *register* raster is **BG1HOFS scroll** (a fixed ~32 frames in
  the boot→title→load prefix); steady-state gameplay has essentially none.
- Mid-frame **VRAM/palette/OAM DMA** is the only real snapshot-breaker: **~1.7 %**
  of frames (worst fixture), concentrated in transitions/menu graphics loads.
- **CONTRACT PROVEN:** `RENDER_REPLAY` replayed ~66 k frames with **0 mismatches**.
  The complete, bit-exact ME renderer input set is:
  `{ vram(96 KB), oam_ram, palette_ram_converted, per-line io_registers log,
     affine_reference seed, reg[OAM_UPDATED] }`. No un-enumerated input exists.

**⇒ ME renderer architecture (decided on data):** ME renders each frame from a
frame-start snapshot + a compact per-line register-delta log (covers BG1HOFS
scroll bit-exactly); the main CPU detects mid-frame VRAM/palette/OAM DMA (~1.7 %)
and renders those frames itself (fallback). The renderer is already pure
computation; extraction = back its globals with the snapshot buffers in ME memory.
Desktop tools: `~/gbadhoc/probe.sh <DEFINE> <fixture>` in WSL.

## 0.0 SESSION 2026-08-09 (night) — THE MEDIA ENGINE RUNS, video staging offloaded

**The PSP's second core (the Media Engine) now does the client's video staging,
proven on hardware in verified trades.** This was the "we're only using one core"
problem; it is no longer true. Commits `08e3a43` (ADR-0080 bring-up + Stage-1),
`11d3963` (ADR-0080a idle throttle), `97bd62f` (ADR-0080b watchdog fix), on
`phase5m-morning`. All ME keys default OFF — the shipped binary is byte-for-byte
single-core until an ini opts in.

**Architecture** (`psp/me/`, `psp/me_host.{c,h}`):
- `psp/me/gbadhoc_me.prx` — a kernel PRX (loaded at runtime with `kuKernelLoadModule`,
  the usb_handoff pattern) that boots the ME from its reset vector (melib stub,
  (c)mrbrown via DaedalusX64, GPL-2.0) into a mailbox dispatcher. No exported
  functions, no function pointers across the boundary — every ME job is `ME_CMD_*`
  code resident in the PRX. Parameterised via `sceKernelStartModule` args.
- Mailbox (`psp/me/me_mbox.h`) — 64-byte-aligned main RAM, accessed by BOTH cores
  ONLY through the `0x40000000` uncached alias. That is the entire cache-coherency
  contract; there is no other synchronisation.
- Stage-1 video (`plat_video_frame` + `vid_draw_prestaged`): double-buffer the core's
  own `gba_screen_pixels` by flipping the pointer between frames (zero main-CPU
  copies); post each frame to the ME for the 480→512-stride staging copy; draw the
  last completed staging buffer (one frame latency). Collision → drop the frame.

**Validated on hardware (fs-me, PSP-1000 pair):**
- Bench ratio gate: ME copy at CPU parity when reading cached — **701µs vs 708µs**
  (mode 1), coherency `sum=ok`. Uncached READS are 3.8× slower → the ME must read
  cached (the staging job does, via a ranged writeback before post).
- **The offload works: `blit_prof stage=0` (was 751µs), `tot=61µs` (was 805).
  ~750µs/frame of staging copy moved off the main Allegrex.** Join-only me_video,
  three runs: 37,200 frames, **0 drops, 0 vmiss**, srtt 17–22ms (champion band —
  a busy ME every frame does NOT regress the WLAN link), 3/3 trades verified.

**Working config: HOST plain (no ME), JOIN `me_boot=1 me_video=1`.** The client is
the historical bottleneck, so offloading its video is the whole point; the host has
headroom and stays on the proven path. Keys: `me_boot` (load PRX + handshake),
`me_bench` (boot-time microbenchmark), `me_video` (Stage-1 offload) — all default 0.

**Two lessons paid for on hardware, both fixed:**
- The ME idle dispatcher polled the mailbox flat-out with uncached accesses,
  saturating the memory bus the WLAN DMA shares (srtt 17→44ms, a failed trade).
  Fix (ADR-0080a): idle backoff — spin on a cached local, poll ~every 50µs.
- The me_video watchdog checked job completion in the *same frame* it posted the
  job (always "busy"), self-destructing the offload after 8 frames. Fix
  (ADR-0080b): count a miss at the next frame's retire; host watchdog is
  heartbeat-only.

**KNOWN, deferred:** `me_video` on the HOST deterministically breaks `np_start`
(session creation) — 2/2 on the fixed EBOOT while the join is flawless. Host stays
on the plain path (the target config anyway). Likely fix if host offload is ever
wanted: defer me_video activation until after the session is up, so the ME video
path never overlaps ad-hoc group creation. Not needed for the client win.

**Next** (natural progression, per Ideas.md #1 and MEDIAENGINE-FINDINGS): the
staging copy was the plumbing PoC. The larger prize is the renderer itself on the
ME (~6ms), verified against the phase6-coreopt pixel oracle now merged; and audio
mixing via journal-and-replay with an audio oracle. Both are pure-computation jobs
on the correct side of the syscall line.

---

## 0. SESSION 2026-08-09 (evening) — read this first, it moves §5's list

Four commits on `phase5m-morning`: `276e445` (the 47-file overlay, committed),
`ca27334` (ADR-0075), `8a2a07f` (ADR-0076), `662ea29` (UI v2).  Everything below is
**source-verified and build-verified but NOT hardware-tested**; both new rfu knobs
default OFF, so the staged EBOOT is behaviour-identical until an ini turns them on.

**Open question 4 is CLOSED.** `gRfu`'s guest address is resolved without agbcc:
pret/pokeemerald's `symbols` branch publishes `pokeemerald.sym` for the retail-matching
build.  Validated twice before use — `gMain` reads `0x030022C0` (byte-identical to the
address already proven in production by the fixture's cb2 predicates) and the sym file's
`gRfu` size `0xCF4` equals `sizeof(struct RfuManager)` in the local decomp headers.
  - `gRfu` = 0x03005000; `recvQueue.count` = **0x030059E6**; `sendQueue.count` =
    **0x03005C1A** (the Gate A number, §5.1); `gHeldKeyCodeToSend` = **0x03005DA8**
    (0x00 = nulled/eaten, 0x11 = empty); `childSendCount` = 0x03005CD0;
    `disconnectMode` = 0x03005CE4; `errorState` = 0x030050EE.

**Open question 1 has a fix built and an instrument aimed at it.**  The fs-gate logs
settle what the overnight campaign left open: `rfu_arrival clumped=12..116/256` on the
join — the withdrawn clumping claim is RE-EARNED on unconfoundable evidence — and the
v7 heldKeys probe ran with its positive control good (`hk_left=0x0020`) showing
`hk_up=0x0040` while `y` does not move: **the game reads the key and discards it,
on hardware, in our build.**  Every same-frame delivery pair is one extra MSC callback
between two of the child's main-loop passes, which is simultaneously Gate B queue
growth and the Gate A send-queue ratchet condition.
  - **ADR-0075 (`rfu_frame_pace`, default 0):** admit at most N non-empty deliveries
    per emulated frame, gated IDENTICALLY in `RFU_CMD_RECV_DATA` and
    `rfu_data_avail()`.  The falsified `rfu_rx_cap` withheld in one place while
    announcing in the other — that inconsistency IS the WAITEVENT re-poll spin.
    Consistent gating = no spin, nothing dropped, ≤1 MSC/frame by construction,
    which is also the real radio's cadence.  `rfu_frame_pace = 1` is the arm.
    Census: `EVT rfu_pace n=<paced>/600 q_hi=<our-queue-high>`.
  - The join fixture is now **v8** (`91e19dfa`, in `testdata/fixtures/` and staged):
    v7 + zero-frame `logram` of `sq_*` (sendQueue.count), `rq_*` (recvQueue.count),
    `hkc_*` (gHeldKeyCodeToSend) bracketing the UP legs, plus `sq/rq/dm/err_out` at
    the walk-out.  Proven injection-identical to v7 (non-logram op diff is empty).
    **Reading it:** swallowed UP + `hkc=0x00` + `sq>=2` confirms Gate A end to end;
    `sq<=1` with `hkc=0x00` kills Gate A and points at the IsSendingKeysToLink
    branch; `rq>4` means Gate B was live at that instant.

**Open question 2 has a fix built.**  The lobby error is a RACE the slow client loses,
and the decomp names every leg (link_rfu_2.c:1132-1147, :945-961, :2492): the child
needs RFUCMD_DISCONNECT (as DATA) → clock-master change → its OWN rfu_REQ_disconnect,
which with LEAVE_GROUP already set raises no error.  Our adapter going IDLE on the
host's NET_RFU_DISCONNECT before step 3 makes the next WAITEVENT answer
RESP_DISC slots=0xF, and pokeemerald's LINK_LOSS handler sets CONNECTION_ERROR
unconditionally.  ADR-0074 deferred on the wrong condition (queue drain — the queue
was already empty); the right condition is TIME.
  - **ADR-0076 (`rfu_disc_grace`, default 0):** hold CLIENT state for N frames after
    the peer disconnect; the game's own DISCONNECT inside the window cancels the
    teardown (trace `rfu_discq mode=5` = race won), expiry = historical behaviour
    (mode=4).  `rfu_disc_grace = 60` is the arm.  Also fixes a latent ADR-0074 bug:
    a pending deferral now clears on adapter reset.

**Staged in `<Logs>/stage-live`** (previous set backed up to `stage-live-prev-0809`):
`EBOOT.PBP` = `10e5273d` (ADR-0075+0076 built in, both OFF — so run 1 is still an
honest baseline that also carries the v8 probes), join fixture v8, inis unchanged.
`join-pace1.gpsp-harness.ini.ARM` sits alongside with the two arm keys commented.
**Run order:** (1) baseline with v8 — this alone can confirm/kill Gate A;
(2) `rfu_frame_pace = 1` on the join; (3) `rfu_disc_grace = 60` on the join;
(4) both.  Judge (2) by `rfu_pace`/`rfu_arrival`/trade rate, (3) by
`rfu_discq mode=5` vs `rfu_discans`.

**UI v2 (`662ea29`).**  Themed full-screen UI (gradient pages, accent header/footer),
ROM browser is now a box-art card gallery (`<appdir>/boxart/<rom>.bmp`, 24/32-bpp BMP,
template cards otherwise; art pool is malloc'd, browser-lifetime only), sectioned
settings (WIRELESS/VIDEO/GAMEPLAY, profile still first per ADR-0071), and a
**Session overlay** toggle (`osd_wireless`, default shown) that hides the
linked/hosting chip during a session; the WLAN-off warning is deliberately not gated.
Profile relaunch now triggers on actual difference vs value at settings entry, which
also keeps the harness `ui_demo` relaunch-free.  New primitives `vid_gradient` /
`vid_image` in video_psp.  Playable path verified behaviourally: `psp-nm -u fe_evt.o`
lists no fopen/fwrite/fflush/vsnprintf (sole undefined: an inert `fclose`).

**Candidate binaries** (untested on hardware): `builds/candidates-0809/`
`GBAdhoc-playable-UNTESTED.PBP` (`5937be7d`), `HARNESS-UNTESTED.PBP` (`2a8b663a`,
= tree tip incl. UI).  The shipped, tested binaries in `builds/playable/` and
`builds/harness/` are unchanged.

---

**Written 2026-08-09.** This document supersedes `docs/DECISIONS.md` wherever the two
disagree. DECISIONS.md is a *chronological* record — it contains conclusions that were
later overturned, and it stops at ADR-0053 while the code has reached ADR-0074. Read this
first, then read DECISIONS.md only for the ADRs this document flags as still load-bearing.

**Companion documents:**
- `Logs/INDEX.md` — which of the 13 findings documents are still true, which batches carry
  the current conclusions, and seven specific ways these logs have been misread before.
- `Logs/BATCH-INDEX.md` — one row per experiment, generated from the logs.

---

## 1. What the project is, in one paragraph

A fork of libretro **gpSP** with a native PSP frontend, carrying the GBA's serial link over
`sceNetAdhoc` so two PSPs can use the emulated **GBA Wireless Adapter**. The adapter
emulation itself (`rfu.c`, `serial_proto.c`) is **David Guillen Fandos's work**, inherited,
not ours. What this project added is the transport (`netdrv/`), the PSP frontend (`psp/`),
the shared frontend layer (`frontend-common/`), and an unattended hardware test harness
(`tools/hw_loop.py` + an autopilot script language).

**It works.** Emerald↔Emerald trading between two PSPs, back-to-back, unattended.

---

## 2. THE MOST IMPORTANT THING TO KNOW

**The binding constraint is RATE MISMATCH between the two consoles, not frame rate itself.**

> **Full speed remains the goal.** 57.00 is a *workaround for the client's performance
> deficit*, not a finding that slower is better. At 59.73 the client lands ~2 fps short of
> the host and the two consoles disagree about how long a frame is; that disagreement is
> what breaks trading. **A client that could actually hold 59.73 would make 59.73 the right
> answer** — the mismatch would be zero and frames would be at their shortest, which is also
> where delivery clumping is lowest. Closing that deficit (§7, the Media Engine) is the way
> back to full speed, and nothing in this document argues against pursuing it.

Emerald refuses to read the *client's* buttons whenever its RFU receive queue is more than
4 deep — `KeyInterCB_SelfIdle`, `pokeemerald/src/overworld.c:2520`:

```c
if (GetLinkRecvQueueLength() > 4)
    return LINK_KEY_CODE_HANDLE_RECV_QUEUE;   // never reads the buttons
```

That queue drains **exactly once per frame** and exists **only on the child** — the parent
code path never touches it. So the host structurally cannot suffer this and the client
structurally can. A host producing packets faster than the client consumes them fills the
queue and it never empties.

At 59.73 the host reached ~59.4 fps and the client ~56.6. `net_pace_match = 1` is a
**ceiling**, not a floor: it stops a fast console exceeding the target and cannot lift a
slow one. Nothing reconciled them.

Measured, ~150 scripted runs, both consoles, party-oracle verified:

| session rate | trades | client failed to reach its seat |
|---|---|---|
| 59.73 | 21/45 = 0.47 | 17/45 |
| 55.00 | 15/25 = 0.60 | 5/25 |
| **57.00** | **13/13 = 1.00** | **0/13** |
| 53.00 | 8/13 = 0.62 | 5/13 |

**57.00 ships today.** It is the highest rate at which the client still tracks target
closely (−0.7 fps, versus −2.0 at full speed), so the mismatch is small enough not to fill
the queue.

**Lower is NOT safer, and this is the part that points back at full speed.** Delivery
clumping rises monotonically as the rate falls — 15.7% → 18.3% → 22.2% at 59.73 / 55 / 53 —
because a longer frame catches more arrivals. So the two effects pull against each other:
dropping the rate shrinks the mismatch but worsens clumping. There is a **knee**, not a
floor, and the knee moves *upward* as the client gets faster. Every millisecond recovered
from the client's frame budget raises the best achievable rate. **Full speed is reachable if
the client can hold it** — that is an engineering problem (§7), not a closed door.

`net_session_fps` **only applies while a wireless session is live**; single-player is
unpaced and always has been.

---

## 3. The builds, exactly as they are

One source tree, one `-DGPSP_PLAYABLE` define, three EBOOTs.

### Harness build (`GBAdhoc [HARNESS]`)
- No `-DGPSP_PLAYABLE`. All `PLAY_*` defaults are 0 → every knob comes from
  `.gpsp-harness.ini` and behaviour matches the pre-playable historical baseline.
- Telemetry **on**. Autopilot **available**. USB handoff **available**.
- Branding: `assets/harness_icon0.png` / `harness_pic1.png` (inverted amber), passed on the
  `make` command line.

### Playable builds (`GBAdhoc`, `Pokémon Emerald`, `Pokémon FireRed`)
- `-DGPSP_PLAYABLE`. Currently shipped md5s: `dfb9ac2b`, `14a07535`, `290f002f`.
- Harness ini path is neutered → a leftover `.gpsp-harness.ini` is inert, no autopilot,
  no USB handoff.
- Telemetry **compiled out**: `GPSP_NO_TELEMETRY` is derived **inside `fe_evt.c`**, not in
  `main_psp.c`. Verify behaviourally — `psp-nm -u frontend-common/fe_evt.o` must list no
  `fopen`/`fwrite`/`fflush`/`vsnprintf`. **Both times this build was wrong, every
  token-presence check passed and only the symbol check caught it.**
- Forced: `core_phase = 0`, `gu_defer = 1`, `net_session_fps_snap = 0`.
- Defaults via `PLAY_*`: `emu_prio 0x2B`, `io_prio 0x2C`, `rfu_poll_min_cycles 34952`,
  `rfu_idle_poll_cycles 34952`, `dump_marker_poll 0`, `boost_us 0`.
- Video default: **fit + nearest** (408×272), `PCFG_SCALE_DEF 1`.
- Trading profiles (ADR-0071): Settings → Trading profile. speed = **5700**, compat = 2997.
  Changing it saves config and **relaunches** via `sctrlKernelLoadExecVSHMs2` from the end
  of normal teardown — SRAM flushed first. Do not relaunch from inside the UI.

### Gotchas that have each cost a session
- **`make` does not track CFLAGS.** `make clean` in `psp/` before *and* after any build
  that changes defines, or the next build silently inherits `-DGPSP_PLAYABLE`.
- **`PSP_PIXFMT` must match between core and frontend.** Passing `rgb565` to the core while
  the frontend defaults `psp5650` renders with **red and blue swapped**, builds clean, warns
  nothing. Use the default on both.
- **`PLAY_FPS_X100` (5973) is dead code.** Superseded by `PCFG_PROFILE_FPS_X100` (5700).
  Delete it; it will mislead.
- **`handoff_max_runs = 0` does NOT mean unlimited** — `usb_handoff.c` falls back to 20.
  It is currently 100000. When `RUNS.TXT` reaches the cap, `handoff_run()` returns *before*
  clearing `CMD.TXT` and writing `RESULT.TXT`, and the console sits at the XMB looking dead.

---

## 4. ADR STATUS — read this before trusting DECISIONS.md

### 4.1 CRITICAL: ADR numbers collide across branches

Unmerged branches have their **own ADR-0033…0037** describing *entirely different
decisions* from the same numbers on `phase5m-morning`:

| number | on `phase5m-morning` (current) | on an unmerged branch |
|---|---|---|
| ADR-0034 | blit staging buffer → VRAM | `phase6-coreopt`: "where the renderer's time actually goes: it is ONE loop" |
| ADR-0036 | harness ini rename | `phase6-coreopt`: "the 4× overdraw is real and half is thrown away: 52%" |
| ADR-0037 | adaptive frameskip vs applied rate | `phase5j-exitroom`: exit-Union-Room = FATAL RFU path (**merged as ADR-0041**) |

**Never cite an ADR number without saying which branch.** The `phase6-coreopt` renderer
ADRs were never merged and are directly relevant to the Media Engine work (§7).

### 4.2 Falsified — do not re-propose

- **`rfu_rx_cap = 2`** (ADR-0042). Tested on hardware, made things *worse*. The arithmetic
  explains why: the game drains 1/frame, so a cap of 2 is *above* the drain rate and can
  never clear the gate. A capped arm is also **self-blinding** — the cap triggers a re-poll
  spin that corrupts the very metrics used to judge it.
- **45.00 fps as the shipping default.** Promoted on one night's 8/8, falsified the next at
  0.67–0.80. Reverted.
- **Timeout scaling** as "more faithful emulation" — the arithmetic is the reverse. At 59.73
  we are *exactly* faithful (1.00×); 29.97 is 1.99× generous. It is compensation for a slow
  radio, not fidelity.
- **`emu_prio_boost_us` / the protected priority window** (ADR-0065). The emulation thread is
  preempted **2.0 times per frame**, not 20 — and every one is *our own TX thread at 0x1F*.
  The WLAN driver never preempts the emulation thread as a thread. The boost adds exactly one
  preemption (its own warden) and removes none. Ships 0.
- **`adhoc_net_prio` (raising the WLAN stack).** Tried, ships disabled.
- **The translation cache as the frame-spike cause.** `smc_block ovf` is a *profiler
  slot-table* overflow, not a JIT-cache overflow. The RAM JIT cache has **never** filled
  (`flush_ram_full = 0` across 4995 windows).
- **The renderer as the optimisation target at 8.6 ms.** Direct instrumentation says
  **6.07 ms**, and `cpu` (dynarec) at 5.5–6.9 ms mean is comparable — with single-frame
  spikes to **23–25 ms**, which is the real throughput problem.
- **ADR-0074, the deferred disconnect.** Built on the theory that a backed-up queue was
  destroying the Union Room exit negotiation. The instrument added alongside it read
  **`rfu_discq queued=1`** — one undelivered packet, not a shredded handshake. Premise dead.
  Ships **disabled** (`rfu_disc_defer = 0`).

### 4.3 Withdrawn evidence — actively misleading

- **`rfu_rxburst` counts POLLS, not deliveries.** `rfu_rx_this_frame++` is the first
  statement of `RFU_CMD_RECV_DATA`, before the cap check and regardless of whether a packet
  is handed over. The client polls hardest precisely when it is *waiting and receiving
  nothing*. So "join bursts reach 9 against the host's 1–2" is a **poll-rate asymmetry** and
  is **not** evidence that we deliver in clumps. Any conclusion resting on it must be
  re-earned. `rfu_arrival` (§6) is the replacement and is immune to the confound.
- **`rfu_rxgate peak` is always 0 and meaningless.** The trace ring packs each argument into
  **12 bits** (`(ev&0xFF)<<24 | (a&0xFFF)<<12 | (b&0xFFF)`), so `(peak << 16)` was masked
  away entirely. `run_max` shares that ceiling and **wraps silently above 4095**.
- **`rfu_rxgate n` over-reports sustained saturation.** The modelled queue is *unbounded*, so
  once depth passes 4 it can never return and `n=600/600` becomes trivially true. Useful for
  **onset** (first saturated window), not for level.
- **ADR-0013's premise was wrong.** "Adapter login = the player used a wireless feature" is
  false: Emerald probes the adapter at **boot** (first login at emulated frame 3) and logs in
  ~10 more times per session. Fixed by ADR-0070, which moved the activation hook to
  `BCRD_START`/`HOST_START`. **Untested on hardware.**

### 4.4 Still load-bearing

ADR-0016/0017 (ARQ backpressure + adaptive RTO), ADR-0020/0024/0025/0026 (nothing writes
ms0 on the emulation thread), ADR-0021 (session cost measured), ADR-0033 (fixed-rate session
clamp — the *mechanism*; the rate is now 57.00), ADR-0034 (VRAM blit staging), ADR-0035
(the `-MMD -MP` Makefile dependency — **do not remove**, it shipped a silent struct-layout
corruption), ADR-0039 (pixel format), ADR-0040 (`vid_swap` syncs the GE first),
ADR-0049 (`gu_defer` on), ADR-0053 (USB handoff), ADR-0057 (START+SELECT abort),
ADR-0060 (CLIENT_ACK keepalive — 1.92× → 0.96 traffic parity, retx 1811 → 596),
ADR-0062 (`emu_prio` below the WLAN stack — **the single biggest win, 0/40 → 16/34**),
ADR-0068 (host mid-frame receive; srtt 2.02 → 0.91 frames, p = 0.0002, costs ~+6.5 pts host
frames-over-budget), ADR-0069 (`sceIoGetstat` removed; **12.4 ms** blocking, once a second),
ADR-0071 (trading profiles), **ADR-0073 (57.00)**.

---

## 5. Open questions, in priority order

1. **The client still eats inputs, and Gate B may not be why.** Rate matching removed the
   sustained over-delivery, and a clump is self-correcting on the next quiet frame — so
   "navigable but not smooth" is the wrong shape for a receive-queue problem. **Prime
   suspect is Gate A**, `UpdateHeldKeyCode` (`overworld.c:2453`): if
   `GetLinkSendQueueLength() > 1` the DPAD/START/A are rewritten to `LINK_KEY_CODE_NULL`,
   silently. Threshold **1**. The client issues `SEND_DATAW` and blocks in WAITEVENT until
   the host answers; a late answer queues the next frame's key behind it. The host never
   waits, so its send queue is always empty. **Never measured.** Needs `gRfu`'s guest
   address (see 4 below).
2. **The exit "press A to return to lobby" persists.** It is
   `RFU_CMD_RESP_DISC` slots=`0xF` at `rfu.c` (search `RFU_TR_DISCANS`), emitted when the
   adapter is IDLE inside WAITEVENT after the host sent `NET_RFU_DISCONNECT`. Confirmed
   client-only: 10 of 88 join logs, 0 of 88 host logs. The user reports the real behaviour
   should be a *negotiated* exit with dialogue on both sides, so this is a genuine bug.
   Same root suspected as (1): the client is simply behind.
3. **Frame spikes.** `cpu` mean 5.5–6.9 ms, single frames to **23–25 ms**. Not the adapter
   (9 µs/frame) and not the renderer. Unattributed.
4. **`gRfu`'s guest address is unresolved** — the one number that would close (1) directly.
   Needs `agbcc` + a BPEE rev 0 baserom to build the decomp; a `modern` build gives
   addresses that do not match retail. Then `logram` reads `gRfu.recvQueue.count` with no
   code change.
5. **Statistical hygiene.** All rate arms were run in **blocks, not interleaved**, and 57.00
   rests on 13 runs. Blocked comparisons have produced two false conclusions on this rig.
   An interleaved 55/57 batch would firm it up. 58.00 is unexplored purely because round
   numbers were chosen.

---

## 6. Telemetry — proposals only, NOTHING HAS BEEN REMOVED

> **Status: nothing in this section has been applied.** Every probe listed below is still
> live in the harness build. These are recommendations for the next maintainer to accept,
> reject, or defer. The rationale is given so the decision can be made on evidence rather
> than on my say-so.

125 distinct `fe_evt` kinds exist; a real run emits ~28. `input` alone is **1549 of ~2400
lines** in a typical join log.

**Keep — active investigations depend on these:** `rfu_rxgate`, `rfu_arrival`,
`rfu_discans`, `rfu_answer_census`, `rfu_state`, `rfu_cmd`, `rfu_login`, `net_stats`,
`sess_cost`, `frame_hist`, `fps`, `session_pace*`, `ap_*`, `exit`, `heartbeat`.

**Proposed for removal — the investigation that motivated them is closed:**

| probe | why it no longer earns its place | risk of removing |
|---|---|---|
| `smc_block`, `smc_addr`, `smc_code` | ADR-0029/0030/0031 concluded the SMC flush costs ~1 µs and the RAM JIT cache has never filled (`flush_ram_full = 0`, 4995 windows). ~40 lines/run. | Low. Re-add if anyone reopens the dynarec-spike question. |
| `rfu_rxburst` | **Withdrawn as misleading** (§4.3) — it counts polls, not deliveries. Leaving it emitting invites the same false conclusion again. | **Removing is safer than keeping.** If kept, rename it `rfu_pollrate` so its meaning is on its face. |
| `rfu_rxhold` | Instruments `rfu_rx_cap`, which is falsified and shipped off. | Low. |
| `preempt` (ADR-0064) | Answered its question and closed: 2.0 preemptions/frame, all our own TX thread at 0x1F. Costs 4 syscalls + 8 clock reads per frame when on. | Low — already defaults off. |
| `blit_prof` | Renderer work is parked pending the Media Engine decision (§7). | **Medium — do not remove if the ME work starts.** It is the before/after instrument for exactly that. |

**Gate behind a key rather than delete:** `input`. At 1549 lines/run it is 60% of log volume,
but it is the only per-frame record of what the core was actually handed, and it was
decisive for the input-gating diagnosis. A `log_input = 0/1` key preserves it for the runs
that need it.

**Dead code, unrelated to telemetry:** `PLAY_FPS_X100` (5973) in `psp/main_psp.c` is no
longer read — `PCFG_PROFILE_FPS_X100` (5700) supersedes it. It survives only as a
misleading constant.

---

## 7. Workspace — deletion CANDIDATES, nothing has been deleted

> **Status: nothing here has been removed.** The user deliberately left this decision to the
> next maintainer. Sizes and unmerged-commit counts below were measured on 2026-08-09; re-run
> the checks before acting, because a worktree that was clean then may not be now:
>
> ```
> git worktree list                          # from wt-morning
> git log --oneline HEAD..<branch>           # per branch: unmerged commits
> du -sm <dir>                               # size
> ```

**13 registered git worktrees** share one repo. Three have commits **not** in
`phase5m-morning`:

| worktree | branch | unmerged | verdict |
|---|---|---|---|
| `wt-vfpu` | `phase6-coreopt` | **8 commits** | **KEEP.** Frame-exact video oracle, renderer profiled by path, and the 4× overdraw analysis (52% discarded). Directly needed for the Media Engine work. |
| `wt-exitroom` | `phase5j-exitroom` | 3 commits | KEEP until checked — its ADR-0037 became ADR-0041, but the `rfu_rxburst` rig baseline may be the only copy. |
| `wt-fixedrate` | `phase5i-fixedrate` | 1 commit | Likely merged in substance; verify the one commit, then drop. |

**Candidates — 0 unmerged commits each.** Removing the *worktree* does not remove the
*branch*: the commits stay in the repo and the branch pointer survives, so the work is
recoverable with `git worktree add` at any time. This is a disk-space operation, not a
data-loss one.

`wt-perfnext`, `wt-sync`, `wt-smcaddr`, `wt-stalls`, `wt-phase5b`, `wt-dynarec`,
`wt-smcblock`, `wt-corebrk` — **~486 MB combined.**

Use `git worktree remove <path>`. Do **not** `rm -rf` — that leaves stale administrative
entries in `.git/worktrees` and `git worktree list` then reports paths that no longer exist.
(If someone has already done that, `git worktree prune` cleans it up.)

**Non-git kit directories** — snapshots of builds shipped to hardware, superseded by
`Logs/playable-build/` and `Logs/variant-*/`: `two-psp-kit` (35 MB), `psp-agb-test-kit`
(29 MB), `hardware-baseline-kit` (20 MB), `hw-kit-ours` (18 MB), `HW-TEST-2026-08-03`
(4 MB). **~106 MB, safe.** Caveat: `docs/HW-BASELINE.md` cites the baseline kit's numbers;
the numbers are already in the doc, so the directory is not needed to preserve them.

**Empty artefacts** (0 MB, Windows `;C` suffix junk): `gpsp-adhoc;C`, `wt-morning;B`,
`wt-morning;C`, `wt-smcaddr;C`, `psp`. Safe.

**`gpsp-adhoc` (150 MB) is the MAIN worktree** — branch `main`, registered first. Not a
stale copy. Do not delete.
