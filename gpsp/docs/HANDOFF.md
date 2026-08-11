# Orchestrator handoff — gpSP-AdHoc (written 2026-08-01 by Fable 5)

You are taking over as orchestrator mid-project. Read this, then `gpsp-adhoc-plan.md`
(workspace root — the mission brief and working agreement), then `docs/DECISIONS.md`
(every decision + why), then `docs/TESTING.md`. Do not re-derive or re-litigate anything
recorded there.

## WHERE THINGS STAND THIS MORNING (2026-08-03, branch `phase5m-morning`)

**Read this first; the sections below are still true but predate two overnight branches.**

`phase5m-morning` is `phase5k-perfnext` (perf) + `phase5j-exitroom` (RFU instrumentation),
folded together so **one** field build yields both datasets instead of costing the user two
sideloads for one hardware session. Staged as Build J
(`8e230fb4376d00a464a24d3b9878e1dd`) in `psp-agb-test-kit/`, with the ordered test plan in
that kit's `README.txt`.

Two things that fold taught us, both worth keeping:

- **A clean textual merge of two branches is not evidence that they compose.** The exit-room
  branch added a harness key reading `AUTOPILOT`; the fixed-rate branch had renamed that
  macro to `HARNESS_INI`. Different lines, no conflict, broken build. Compile every fold.
- **Both agents independently allocated ADR-0037.** Exit-room's is renumbered **0041**
  throughout; the frameskip fix keeps 0037 because code and the harness reference it.
  (`## ADR-0008` is duplicated too, but that predates this work and is on `main`.)

**Gates re-run on the combined build, all green:** boot (incl. the legacy-`autopilot.ini`
gates), `run_gu_color_test.sh` at blit modes 0/1/2 **and** `--gu-defer=1`, trade
`--radio=40 --net-frameskip=1`.

**A verification note on those colour gates.** The first attempt reported three passes and
was worthless: a shell variable did not expand, so it ran the *default* mode three times
rather than 0/1/2. It was caught only because the runs were checked against
`EVT blit_mode req=N mode=N` afterwards rather than trusted. **Assert which variant a run
actually exercised; a green result says nothing about which case produced it.**
`tools/e2e/run_blit_matrix.sh` does this and prints the mode each run really used.

**Known rig hazard, found independently by both agents:** the ad-hoc e2e tests are **not**
isolated by `SANDBOX_ROOT`. Port 27312 is compile-time and bound on `INADDR_ANY`,
`/dev/shm/PPSSPP_ID` is global, and the harness `pkill` matched every PPSSPP on the box.
Concurrent agents therefore killed each other's emulators and produced failures that read
exactly like transport regressions. **Do not run two ad-hoc tests at once until this is
fixed** (see `docs/TESTING.md`). This is an orchestration bug, not a code bug — it was
caused by running several agents against one machine.

## State of the project (rewritten 2026-08-02, after six hardware sessions)

**Everything is merged to `main`** (`7552464` + branding). Gates 0, 1, 3 and 4-E(core) are
closed; Phase 2 (UI, colour fix, scaling, FF, branding, variants), the netdrv real-radio
fix, the FR/LG rev-1 tables and the Phase-6 release packaging all landed. Build the core
explicitly before the frontend — `psp/Makefile` does NOT rebuild `gpsp_libretro_psp1.a`,
and a stale archive fails at link with undefined counter symbols.

**What is proven on real hardware:** one complete Emerald↔Emerald Union Room trade over
real ad-hoc radio (2026-08-01); the transport layer is flawless across six sessions —
`core_tx`/`core_rx` mirror exactly between consoles every time, `overflow=0 spill=0
txfail=0 drop_crc=0`, `retx_pct` 1–6%, measured RTT 35–110 ms (typ. ~51). **Networking is
not the problem and has not been for some time.**

**THE CURRENT BLOCKER — the emulator core has no headroom.** Field measurement, PSP-3000,
*no wireless session active*: `core=11390/21263` — the core alone uses ~11.4 ms mean and
spikes to **21.3 ms against a 16.75 ms frame budget**, single-player. In session it reaches
26–29 ms. Everything else we fixed was real but downstream of this: the capability
oscillation, the pace-matcher instability, and the trade failures all follow from a console
that periodically cannot finish a frame on time.
**The prime suspect has been eliminated — do not spend more time on it (ADR-0031).** Three
phases of instrumentation chased `flush_translation_cache_ram()`: ADR-0029 split it by cause
(every flush is SMC, cache exhaustion never happens), ADR-0030 found the address (99.6 % of
~4110 flushes/window land on one halfword), and ADR-0031 found the block, the writer, and the
price. The answer:
- The halfword is the **first instruction of a real two-instruction Thumb function the game
  builds on its stack** — `ReadFlash1` (`ldrb r0,[r0]; bx lr`), copied there by
  `SetReadFlash1+0x2c` so the flash driver never executes from the cartridge while polling.
  Block termination and tagging are both innocent; every covering block ends cleanly on an
  unconditional branch. This is textbook self-modifying code, and every flush is a true positive.
- **The flush is cheap: ~1.0-1.3 us each, measured inside the function** (`fus`), which is
  **0.13 % of core time**. Its `memset` averages 267 bytes, and it discards a RAM cache holding
  **1.4 blocks**. ADR-0029's "~70 us/flush on Allegrex" was derived by attributing an entire
  worst-frame excess to the flushes a priori; that is now known to be wrong by ~10x.
- **Do NOT build selective invalidation** (ADR-0030's deferred design): it would save ~1 us on a
  0.13 % path, and translated blocks are directly linked, so it needs the linking reworked too. If
  the storm ever must go, 99.7 % of it is *silent stores* — see ADR-0031.

**AND THE FRAME IS NOW BROKEN DOWN BY PHASE — the answer on the rig is VIDEO (ADR-0032).**
`EVT core_phase` brackets `retro_run` into an exact partition and cross-checks its total against
`EVT core_prof`'s `core=`. Steady in-session window on the rig, per frame:

| phase | mean us | share | |
|---|---|---|---|
| **vid** | 3235 | **48 %** | 160 x `update_scanline()` |
| **cpu** | 2443 | 36 % | dynarec execution + everything unbracketed |
| **blt** | 722 | 11 % | `video_run()` — post-process + `video_cb` |
| amix + aout | 291 | 4 % | the whole audio path |
| jit / rfu / oth | ~6 | ~0 | |

- **Video is 59 % of the core frame** (`vid + blt`) and the dynarec is 36 %. Three ADRs of work
  looked at the dynarec. `blt` is flat at 722/852 us in every window of every run.
- **Read `worst=` first, and NEVER sum the `max` column** — each phase's max is its own worst
  frame (they sum to 16530 us against a 10442 us frame). `worst=` is the breakdown of the frame
  that actually set the maximum, and it sums to it exactly in 31 of 31 windows checked.
- `core=` and `tot` are the same call measured twice. `core` strictly encloses `tot`, so it is
  always the larger, by 2-34 us of mean (fe_host's own clock reads and counter callbacks) and up
  to 174 us on one frame (a host-thread preemption between the two brackets). A NEGATIVE gap, or a
  mean gap outside that band, means one of the two brackets is wrong and neither should be trusted.
- **Which phase owns the worst frame varies:** boot windows `worst=vid:7573`; the SMC-storm window
  `worst=cpu:6160,jit:1310` with `wbk=308/2472`, i.e. a **flash save-write burst plus the re-JIT
  it triggers**, with video *below* its own average on that frame.
- **`core_phase` is a config.ini key**, 0-3, default 2 (~2.6 % of the frame in clock reads, all
  priced in-line by `rd x clk`). Level 1 is ~0.1 %. **Set `core_phase=0` in anything shipping.**
- **THE RIG IS NOT THE FIELD.** Rig `core` max 10.4-10.6 ms against the field's 21.3/26-29 ms —
  the rig has never reproduced the spike, so that table is a *hypothesis* about hardware. PPSSPP
  models neither Allegrex's caches nor uncached write cost, which would hit `vid` and `blt`
  hardest, so the field's video share should if anything be higher. **The next action is one
  hardware run with `core_phase=2`. Build nothing until that line comes back.**

**Field-proven guidance to keep:**
- **Put the weaker console in the HOST seat.** The join role costs ~12 fps on both consoles
  (3000: 58 host / 46 join; 1000: 56–58 host / 49–52 join) and sends ~1.9× as much. The
  cost is inside the core's RFU client path, not our transport.
- Both consoles must use the **same fixed ad-hoc channel** (XMB → Network Settings → Ad Hoc
  Mode → 1). Automatic leaves them on separate invisible groups.
- The user's memory stick is extremely slow: **34.8 ms for a single 4 KiB write**. All ms0
  I/O (EVT log, SRAM writes, dirty scan) is now on a low-priority writer thread; never put
  any of it back on the emulation thread.

**Config levers for hardware A/B without a rebuild** (`ms0:/PSP/GAME/gpsp-adhoc/config.ini`):
`net_frameskip` 0/1/2 (1 = adaptive, draws every other frame — use this; 2 is the legacy
`auto` that skips 97% of frames), `net_tx_thread`, `log_thread`, `sram_thread`,
`net_pace_match` **(REMAPPED by ADR-0033: 0 off, 1 = fixed-rate session clamp — default and
what `1` now means, 2 = the old ADR-0027/0028 adaptive matcher)**, `net_session_fps` (decimal,
**default `29.97` = two vblanks, ADR-0035 — was 40.00, which is unreachable**; any value is
snapped to `59.94/N` and the snap is logged), `net_session_fps_snap` (1 = snap, default;
0 = apply the raw request, for the A/B), `core_phase` 0/1/2/3 (ADR-0032 frame bracket; 2 = default
and what a diagnostic hardware run wants, 1 = coarse for the probe-cost A/B, 0 = off for a
shipping build), `blit_mode` 0/1/2 (ADR-0034 staging placement: 0 = cached RAM + an 82 KiB
cache writeback every frame, 1 = the same RAM through the uncached mirror, **2 = VRAM, now the
default — A/B decided on hardware, worth 713 µs/frame; see the next paragraph**).

**LANDED — the blit staging buffer is now in VRAM: 2513 → 1800 µs/frame (ADR-0034).** `blit_mode`
defaults to **2** and the hardware A/B is decided (PSP-1000, µs/frame, ~6 µs spread):

| `blit_mode` | `stage` | `gu` | `tot` |
|---|---|---|---|
| 0 cached + 82 KiB writeback | 1337 | 1176 | 2513 |
| 1 uncached mirror | 1041 | 1174 | 2215 |
| **2 VRAM (default)** | 1071 | **728** | **1800** |

**Read the `gu` column — it is the whole story and it is not the one we predicted.** Both the ADR
and I argued a staging *placement* could only move the CPU copy and would leave the GE wait alone.
Wrong: `gu` fell 38 % because the GE reads its texture out of VRAM far faster than out of main
RAM, and that dwarfs mode 2's slightly *worse* CPU-side writes (1071 vs mode 1's 1041). **Third
rig-derived conclusion overturned by hardware** (magnitude, then ratio, now a causal claim) — the
rig had reported all three modes identical to the microsecond. **Standing rule: the rig proves
correctness and control flow; it prices nothing touching caches, uncached memory or the GE, and it
cannot be trusted about which component a change will even affect.** VRAM was the user's idea,
rejected earlier — correctly — for the *translation cache* (Allegrex cannot fetch instructions
from VRAM) and then not re-examined for the framebuffer until much later.

**THIS FEEDS STRAIGHT BACK INTO `net_session_fps`, and it is the first thing to re-test.** The
PSP-1000 was at ~14.2 ms core + 2.5 ms blit ≈ 16.7 ms against a 16.743 ms refresh — *exactly* the
condition that made 40 fps unreachable (ADR-0035 §1: no frame fits in one vblank, so every frame
costs two, so the average collapses to ~30). At 1.8 ms the total is meaningfully **under** one
refresh for the first time, so a ~40 fps average may now be physically achievable. **Next run:
with `blit_mode=2`, try `net_session_fps=40.00` plus `net_session_fps_snap=0` and read the
achieved `EVT fps` and whether `session_pace_miss` fires.** If it holds ~40, raise the default; if
it still misses, 29.97 stays and the snap was right.

**Remaining lever on this path:** `gu` is still 728 µs/frame of the CPU blocked in `sceGuSync`.
Deferring it (double-buffer `gu_list`, kick the list, run the next emulated frame, sync just
before the vblank wait) is the clear next candidate — **but `gu_list` is a single static buffer
that `osd_draw()` reuses in the same frame, so it needs double-buffering first, and it must not be
built blind on a rig that cannot see GE timing.**

**Still open:** the core spike (above — cause still unknown, but it is *not* the SMC flush
storm; ADR-0031 closed that line); FR/LG cross-edition legs (blocked on user-supplied
parked saves, ADR-0022); `run_exit_room_test.sh` written but never successfully executed;
the `rfu_link_down` breadcrumb never fires in the field (call-site coverage, not linkage —
a frontend watchdog is the fix); three core patches now await upstream PRs (ADR-0011 queue
depth, the RFU activation hook, the profiling counters).

**User-side artifacts:** `psp-agb-test-kit\` (current two-console kit — push EBOOTs here,
NOT to drive letters unless a console is actually mounted; check first),
`C:\gpsp-adhoc-tools\ra-rig\` (RetroArch×2 bisection control — it proved the exit-room bug
was ours, keep it alive), `hw-kit-ours\` (original baseline kit).

## READY FOR HARDWARE — `phase5k-perfnext`: two EBOOTs, one variable each (2026-08-03)

Branch `phase5k-perfnext` (worktree `../wt-perfnext`), based on `phase5i-fixedrate` **plus that
worktree's uncommitted fixed-rate/blit-mode/profiling work**, snapshotted as the first commit so
this branch has a clean diff base. Three changes, one dead end, all four written up as
ADR-0037/0038/0039/0040.

**The find of the night, and it is a deletion (ADR-0039).** The frontend was spending
**1071 µs/frame — 6.4 % of the whole budget — undoing a swap the core had just done for free.**
`convert_palette` expands the GBA's BGR555 palette and was deliberately putting R in bits 11-15
to make libretro RGB565; `video_psp.c` then walked all 76 800 pixels putting R back in bits 0-4
to make the GE's 5650. `USE_PSP_RGB565_FORMAT` makes the core emit 5650 from the palette — on
512 palette writes instead of 76 800 pixels, in a cheaper expression than before — and the
staging step collapses to a `memcpy`. **`video.cc` needs no change:** its blend masks name two
5-bit positions that R and B simply trade, and G never moves. Proved by equivalence, not by that
argument: both pixel pipelines built, and the GE drawbuffer for Emerald frame 600 is
**byte-identical** between them.

**The frameskip bug is fixed and it is the biggest *perceived* win (ADR-0037).** The adaptive
skip compared achieved fps against the nominal 59.7275 instead of the applied session target, so
a console pacing perfectly at 29.97 concluded it was behind and dropped every second frame
forever — `emu=29.43 rendered=14.71 skipped=300` with 52-57 fps of headroom unused. Thresholds
are now a percentage of `g_pace_target_x100`; off-session they land within 3/100 fps of the old
constants, so the change is inert outside the case it fixes. Both transitions log `target=`.

**`gu_defer` is built and ships OFF (ADR-0040).** The previous agent's blocker — one static
`gu_list` reused by `osd_draw()` — is cleared with two alternating lists. Worth little solo and
potentially a lot in a clamped session, because the saving is bounded by the loop's idle time.
`blit_prof` now reports `wait=` (ADR-0038), the `sceGuSync` stall alone, which is the number that
tells you whether the deferral is doing anything.

**What the rig can and cannot say here.** It proved correctness for everything: colour test
passes both phases in all three `blit_mode`s and with `--gu-defer=1`, and the GE dumps are
byte-identical across format and across defer. It **cannot rank any of it** — it models neither
Allegrex's caches nor uncached/VRAM write cost nor GE rasterisation time, and has now been wrong
three times on exactly this path. Every change is config- or build-flag-selectable so hardware
decides.

**Left on the table, deliberately:** binding the GE straight at the core framebuffer (ADR-0034's
own numbers refute it — main-RAM texture `gu=1174` vs VRAM `728`), and `sceGuCopyImage` for the
staging copy (possible now, but it puts the source back in main RAM and adds to the critical
path that is already the blocking cost). Both written up with the reasoning so the next agent
starts from the argument rather than the idea.

## FIELD SESSION 2026-08-01 — the wireless fix WORKED, and what it exposed next

**An Emerald<->Emerald Union Room trade completed on real radio between two PSPs.** Issue #2
below is closed in the field, not just in the harness. The numbers, from both consoles' logs:

- `retx_pct=2–3 %` (the pre-fix field number was **280 %**), `dup` 1.8 % of rx
- `overflow=0 spill=0 txfail=0 ringdrop=0 rxerr=0` on both sides
- core payload counters **mirrored exactly** — host `core_tx=9129 core_rx=17553`, client the
  reverse: every core payload delivered in both directions, **zero end-to-end loss**
- **Real ad-hoc RTT measured for the first time: `srtt_us` 40,500–87,000, typically ≈52,000
  (≈52 ms)**, RTO adapting to ≈205 ms on spikes. **The 100 ms floor (ADR-0017) was right and
  needs no retune** — it is now calibrated against a working session instead of two broken ones.
  Recorded in docs/TESTING.md (`--radio=40` is the profile that matches reality) and
  docs/HW-BASELINE.md.

Three things came out of that session, addressed on branch `phase5-session-perf`:

1. **The lag is SRAM flushes, not the network.** Per-heartbeat-window fps shows only the windows
   containing an `EVT sram_flush` are slow — PSP-1000 50.4/50.7 fps against 56.5–57.6 either
   side, PSP-3000 55.2/56.5 against 58.96 — i.e. ~0.5–1.3 s of frozen frame loop while all
   131,072 bytes went to the memory stick synchronously on the emulation thread, repeatedly,
   because a trade makes the game re-save. **ADR-0020**: write only the 4 KiB blocks that
   changed, and report `ms=` in `EVT sram_flush` so it is never inferred again.
2. **Frameskip during sessions was pure loss.** Emulation held 58.9 fps sustained — identical to
   solo play — so ADR-0018's blanket `gpsp_frameskip=auto` was throwing away *rendered* frames
   for no throughput (the loop still waits for vblank afterwards). **ADR-0019**: measure it
   (`EVT fps emu=… rendered=… skipped=…`), then default to *not* touching frameskip during a
   session; `config.ini net_frameskip` offers `0 off` (default) / `1 adaptive` (auto_threshold
   with hysteresis, engaged only after sustained real-time slippage) / `2 auto` (ADR-0018
   verbatim, for hardware A/B).
3. **Exiting the Union Room after the trade showed the GAME's FATAL wireless screen** ("…please
   turn off the power" — gen-3's unrecoverable RFU path, not the recoverable comm-error dialog).
   **Our app did not crash**: both logs end with an orderly `EVT net_down` + `EVT exit code=0`
   (the user exited to the XMB by hand). So the question is what the emulated RFU does when the
   *game* ends its wireless session while our netdrv session is still up and still delivering the
   peer's in-room traffic — the core never tells us its RFU went idle. `EVT rfu_link_down` is the
   new breadcrumb for that transition, and `tools/e2e/run_exit_room_test.sh` drives an in-game
   room exit (both sides, or `--leaver=join`) after a completed trade, with
   `CB2_PrintErrorMessage|1 == 0x0800b1a1` as the direct oracle. **Bisect before blaming us:** the
   RetroArch×2 rig (`C:\gpsp-adhoc-tools\ra-rig\`) is the control — if stock core + RA netplay
   throws the same screen on room exit, this is upstream/core behaviour.


## ANSWERED IN PART — what the exit-Union-Room screen ACTUALLY IS, and the one number left to read (2026-08-03, branch `phase5j-exitroom`, ADR-0041)

**Read ADR-0041 before touching the exit-room issue again. Two hypotheses are now dead; do not
re-derive them.**

**The fatal-vs-recoverable asymmetry is not "the game handles roles differently". It is one
predicate.** `CB2_LinkError` sets `gWirelessCommType = 3` iff `!sLinkErrorBuffer.disconnected`
(pokeemerald `src/link.c:1589-1610`), and `CB2_PrintErrorMessage` has **no A-button handler for
3** (`:1669-1725`) — that is literally why the only way out is the power switch. And
`disconnected` comes from exactly one expression (`src/link_rfu_2.c:1995`):
`RfuGetStatus() == RFU_STATUS_CONNECTION_ERROR`. **CONNECTION_ERROR → recoverable; FATAL_ERROR →
unrecoverable.** Every link-loss path yields CONNECTION_ERROR, which is why the host — whose
adapter merely timed the client out — gets the polite screen. **So the question is no longer
"why did the link drop"; it is "what made the CLIENT's status FATAL rather than CONNECTION".**
That set is closed and has five members (ADR-0041); four of the five are timing, one is
queue overflow, and the first is *our emulated adapter answering a command with an error*.

**The breadcrumbs to read in the next field log, in this order:**
1. **`EVT rfu_cmderr cmd=0x.. state=..`** — our adapter refused a command. gpsp's `return -1`
   becomes SPI `0x996601ee`, which librfu decodes as `ERR_REQ_CMD_ACK_REJECTION`
   (`librfu_intr.c:128-132`) → `LMAN_MSG_REQ_API_ERROR` → **FATAL_ERROR** → the power-off screen.
   Never fires in the rig. If it fires in the field, the cause is named outright.
2. **`EVT rfu_rxburst frame_max=N`** — `RFU_CMD_RECV_DATA` commands served in ONE emulated frame.
   **Steady state is 1.** The game drains its 32-slot `recvQueue` once per frame and `full` is a
   *latch* that goes straight to FATAL_ERROR (`link_rfu_3.c:391-394,:437`,
   `link_rfu_2.c:1999-2005`). **This is the leading hypothesis**: a high-RTT transport delivers
   clumped, and ADR-0011 widened rfu.c's own queue 4→16 precisely so clumps would stop being
   dropped — so up to 16 now reach the game per frame. It explains the client-only fatality, the
   spotless transport counters, why RA on localhost is clean, and why it lands at room exit.
   **Rig baseline on the shipped build (`--radio=40`, full trade, srtt ≈83 ms): host 2, join 3,
   out of 32.** Compare the field against those, not against zero — a 10× margin is why the rig
   cannot fail this way.
   **Do NOT raise `RFU_PKT_QUEUE` again, and do NOT add pacing, before reading this number** —
   they are opposite fixes and only the measurement chooses.
3. `EVT rfu_login`, `rfu_state`, `rfu_cmd`, `rfu_unkcmd`, `rfu_qdrop` — the rest of the trace.
   If 1 and 2 are both quiet, the answer is the watchdog / clock-slave / checkID routes, i.e.
   **the frame-loop spike, not delivery**, and that puts this issue behind the core-perf work.

**Dead hypotheses (measured, not argued):** (a) the child is pinned in `RFU_STATE_CLIENT` because
only the parent disconnects it — refuted by the `--drop-disc` causation test: the child issues its
own `rfu_REQ_disconnect` (`link_rfu_2.c:945-962`) and both sides still left the room cleanly.
(b) The rig reproduces it — it does not: `run_exit_room_test.sh --leaver=both --radio=40` walks
both consoles out with no fatal screen. **The harness's remaining failure is its re-entry leg's
in-game save, and its exit oracle cannot tell the two error screens apart anyway** (it matches
`CB2_PrintErrorMessage`, which both variants run) — mash A for a few seconds and re-sample: only
the recoverable variant leaves that callback.

**Also: the bisection that "proved it is ours" has a gap.** The RetroArch×2 control was run
**host-leaves-first**; the field failure is on the **joiner**. Ordering is load-bearing here.

## ANSWERED — 99.6 % of the SMC flush storm is ONE halfword, `0x03007D90` (2026-08-02, branch `phase5f-smcaddr`, ADR-0030)

**Read this before the ADR-0029 section below — it narrows ADR-0029's answer to a single address.**

ADR-0029 said "the flushes are all SMC". ADR-0030 adds *where*: a new `EVT smc_addr` line on the
heartbeat cadence, next to `EVT core_prof`, buckets every SMC-triggering write by 256-byte page
(exact flat array, 2.3 KiB) and keeps the hottest exact addresses in an 8-slot heavy-hitter table:

```
EVT smc_addr win=<events> pages=<distinct> iw=<n> ew=<n> oth=<n> ovf=<n>
             hot=<page>:<n>,...(4)   top=<exact addr>:<n>,...(4)
```

**`win` must EQUAL `core_prof`'s `win=.../smc` + `.../dma`** — one bucket hit per flush. It does, in
every window of every run. `oth` must be 0 and `ovf=0` means the `top` counts are exact, not
estimates. Rig result, full Union Room trade, both consoles:

- **storm windows:** `win=4110 pages=2 iw=4110 ew=0 oth=0 ovf=0 top=03007d90:4095` (host) and
  `win=4122 ... top=03007d90:4096` (join). **~99.6 % of every flush is one halfword**, exactly
  counted.
- **Union Room entry:** `win=1` — the game's code DMA into `0x030046xx-0x03004Fxx` costs **one**
  flush, not thousands. Real overlay swapping happens and is essentially free.
- **`ew=0` in every window of every run.** EWRAM is not involved at all; this is entirely IWRAM.

**What that means.** A code copy cannot produce the storm: its first store clears the tags, so the
rest is free — which is exactly what the `dma=1` rows show. Firing 4095 times on one halfword means
the tag is re-established between every pair of writes: gpSP keeps re-translating a block covering
`0x03007D90` while the game keeps writing that same location. **That is over-tagged data, not
overlay swapping.**

**Next step, and it is now a small one:** find which block covers `0x03007D90`, where it starts, and
whether that halfword is an instruction the CPU actually executes or trailing data the block scan
walked into. Killing this one address removes ~99 % of the steady-state flushes and therefore ~99 %
of the 21 ms spike. Do that **before** selective invalidation — which ADR-0030 verifies is feasible
but records **three corrections** to (the `0x0101` halfword tags are stamped on *every* instruction,
not just entries; invalidate by zeroing `trentry->offset_arm/thumb` so the 32 767-entry tag stack is
not exhausted; and those halfword tags have no refcount while blocks overlap, so clearing them is a
correctness hazard). Read ADR-0030 before touching any of it.

**Cost of the instrumentation:** below the rig's resolution — the deterministic boot leg is
identical in 4 of 6 windows and 1 us off in the other two (rounding). Analytic bound ~0.2 us per SMC
event, i.e. <= 0.3 % of the flush it accompanies.

## ANSWERED — the core spike is SELF-MODIFYING CODE, and a bigger JIT cache is off the table (2026-08-02, branch `phase5e-dynarec`, ADR-0029)

**Read this before the ADR-0028 section below — it settles ADR-0028's open question.**

ADR-0028's `flush_ram_total` conflated two unrelated triggers of
`flush_translation_cache_ram()`: **cache exhaustion** (the RAM JIT cache filled up — a bigger
cache is the lever) and **self-modifying code** (a write landed on RAM holding translated code,
so the *whole* RAM cache is thrown away — a bigger cache is useless). ADR-0029 splits the counter
three ways — `flush_ram_full`, `flush_ram_smc` (CPU store, via `smc_write`), `flush_ram_dma`
(DMA, via `write_io_epilogue`; `CPU_ALERT_SMC` is raised nowhere else). `EVT core_prof` now reads
**`win=rom/full/smc/dma/page`**, same for `wspike=`/`spike=` and `corewin=`/`corespike=`.

**The rig answered flat out** (`run_trade_test_psp.sh --radio=40`, full trade, both consoles):

- host `win=0/0/4109/1/0`  `wspike=0/0/307/0/0`
- join `win=0/0/4121/0/0`  `wspike=0/0/247/0/0`
- boot, solo, 3600 frames: `win=1/0/70/0/0`

**`full=0` everywhere — cache exhaustion never happens, in any window, in any run.** The cache is
wiped by SMC long before it can fill. It is not DMA either (1 event in 4110). It is single CPU
stores from translated code.

**Consequences, all evidence-backed:**
- **Do not raise `RAM_TRANSLATION_CACHE_SIZE`.** It would buy exactly nothing and would cost a
  third of the PSP-1000's free RAM.
- **The freed-RAM ideas are moot for now and are deferred, not rejected** — `state_buf` (416 KiB)
  is static *on purpose* (the core's greedy `init_gamepak_buffer` eats whatever .bss returns, so
  a later `malloc` for a savestate would reliably fail), and `gamepak_page_loads` is 0 on the rig
  so there is nothing measured to spend the RAM on. Reasoning recorded in ADR-0029 §Decision 3.
- **Idle-flush at `SWI 0x02` is feasible but aimed at the wrong mechanism.** The hook point is
  clean (`CPU_ALERT_HALT` -> `cpu_sleep_loop`; no SWI decoding needed), but its trigger is cache
  fullness, which never occurs — and an SMC invalidation *cannot* be deferred to idle without
  running stale translated code.
- **Selective eviction needs a new data structure, not a smarter walk.** The RAM code tag is one
  `u16` per halfword; a halfword covered by two overlapping blocks can only record one of them,
  so a written address cannot name every block that must die.

**Arithmetic worth carrying into the next field read:** the rig's worst frame was 8456 µs against
a 5325 µs mean with 307 flushes in it — **~10.2 µs per flush**. At Allegrex speed (~70 µs) 307
flushes is ~21 ms, matching the field's `core` max of 21263 µs almost exactly. **First thing to
check in the next hardware log: that `full` and `page` are zero there too.** If they are, the
only remaining lever on the core is a per-region invalidation index — real surgery, not an
overnight change.

## OPEN — THE CORE MISSES THE FRAME DEADLINE ON ITS OWN (2026-08-02, branch `phase5d-peersync`, ADR-0028)

**This is priority one now, and everything below it in this file is downstream of it.**

**The fact, from the field, with no wireless involved.** The PSP-3000's *first* `EVT sess_cost`,
logged **before `peer_connected`**: `core=11390/21263` — **`retro_run` alone spends a mean of
11.4 ms and a maximum of 21.3 ms against a 16.75 ms frame budget, while playing Emerald
single-player.** In-session the max grows to 26121 and 29472 µs. The PSP-1000's solo window has
the same shape (`core=11698/21262`). The core *mean* (11.4 ms) is essentially the whole frame
mean (11.7 ms), so **there is no headroom left to absorb anything** — which is why capability
oscillates, which is why the pace matcher was unstable, which is why the link desyncs.

**This retires ADR-0027's rig-derived claim.** The rig said `core` max 10.5 ms against `frame`
max 24-35 ms, i.e. spikes *outside* `retro_run`. Hardware says the reverse: `core=11439/24681`
against `frame=11736/35079`. **Rig-vs-hardware divergence, recorded as such** — PPSSPP has a
different dynarec profile and a RAM-resident ROM, so it cannot model either mechanism below. The
rig proves control-loop correctness; it cannot price the core.

**LANDED — ADR-0028 instruments it, and does NOT guess at a fix.** Two mechanisms can cost tens
of ms inside `retro_run`, and both were **completely uncounted**:
1. **Translation-cache flush.** PSP defines `SMALL_TRANSLATION_CACHE` (`Makefile:236-249`) →
   **2 MiB ROM / 384 KiB RAM** JIT cache (vs 10 MiB/512 KiB elsewhere, `gpsp_config.h:14-25`).
   A ROM flush memsets a **256 KiB** branch-hash table and rewinds the cache, so **every block is
   re-JITted** — up to 2 MiB of generated code discarded. A RAM flush memsets up to **288 KiB**
   of SMC shadow. The pre-existing `flush_ram_count` is **cleared every frame** (`main.c:230`)
   and its only consumer is a commented-out printf; **there was no ROM-flush counter at all.**
2. **ROM paging.** `load_gamepak_page()` does a **32 KiB read from the memory stick, mid-
   emulation**, on every ROM page fault (also called from the translation path,
   `cpu_threaded.c:3037`). It engages when the ROM does not fit in RAM — our exact case:
   `mem_free=389120` against a 16 MiB Emerald. **ADR-0026 priced this user's stick at 34 ms for
   4 KiB.** Also uncounted.

**Third core patch, and the most trivially upstreamable of the three: three `u32`s and three
`++`.** `flush_rom_total`, `flush_ram_total`, `gamepak_page_loads`. No behaviour change.

**Read these in the next field log, in this order — one nonzero field names the cause:**
1. **`EVT core_prof core=mean/winmax/max win=rom/ram/page wspike=rom/ram/page spike=rom/ram/page`**
   — heartbeat cadence, **session or not**, because the decisive spike happens solo and
   `sess_cost` only exists while a session is live.
2. **`wspike=` is the whole point**: the counter delta on the worst frame *in that window*. An
   average would hide a once-a-minute event, and the all-time `spike=` latches during boot — the
   rig showed a window with **4110 RAM flushes** still reporting `spike=0/0/0`. Read `wspike`.
   - `wspike=1/0/0` or similar → **the ROM translation-cache flush.** Then, and only then, weigh
     a bigger cache (~380 KiB free vs a 2 MiB cache — quantify before trying), or flushing
     deliberately at a safe moment (menu, session start, room entry).
   - `wspike=0/0/1+` → **ROM paging off the memory stick.** Different fix entirely.
   - `wspike=0/0/0` with a large `winmax` → **neither.** The hunt moves inside the emulation
     loop and the next step is bracketing `retro_run` by phase (audio / video / CPU).
3. `EVT sess_cost … corewin=…/…/… corespike=…/…/…` — same data on the 10 s session cadence.
4. **`EVT av_info rate=32768` and `EVT audio_rate in=32768 out=44100 step=48695`** — see below.
   Compare `core=` mean against the pre-ADR-0028 field value of **11.4 ms**.

**`gpsp_sound_rate` 65536 → 32768 (ADR-0028).** The core's own text: *"Both values keep audio
timing exact. 65536 renders the full mixer bandwidth; 32768 matches the bandwidth of real
hardware's default PWM output and halves audio mixing work."* Real GBA PWM is 32768 Hz, so we
were paying double for bandwidth the console never produced — **inside `retro_run`, where the
11-12 ms mean lives.** Strictly better: cheaper *and* closer to hardware. **The trap:**
`in_rate` was hardcoded 65536 in `main_psp.c` and `audio_start()` runs *before* `fe_host_boot()`,
so changing the option alone would have resampled at twice the production rate (double-speed
audio into a draining ring). The rate is now plumbed via `fe_host_sample_rate()` —
**platforms must never resample from a literal again**; the header says so.

**Checked and deliberately NOT changed: `gpsp_sprlim`.** The option is named **"No Sprite
Limit"** — `disabled` *keeps* the GBA's per-scanline sprite limit. We were already on the
accurate **and** cheaper setting. The name reads backwards; a comment now guards it.

## The pace matcher after the field (ADR-0028 supersedes ADR-0027's control law)

**What ADR-0027 got right, hardware-confirmed on both consoles:** the mechanism, and above all
**the anti-ratchet** — `self_cap` stayed 56.47-59.01 (1000) and 58.60-58.84 (3000) on *every
line* while throttling, never sagging. Advertising capability rather than achieved rate was the
right call. **And the user-visible win: both consoles entered the Union Room together and BOTH
COULD MOVE, a first.** Failure moved later, to "standby for communication" when the players sit
down to exchange data.

**What was wrong: the control law chased an oscillating target.** `peer_cap` swung ~20 fps within
seconds (49.95, 44.71, 41.58, 38.89→floor→released, 41.08, 47.03, 51.00, 53.65, 55.41, 56.58,
52.60, ...). Against a symmetric 2 fps/s ramp we were permanently mid-chase, and we disengaged at
the floor mid-session. **The second log explained the swing: the consoles TRADE the slow role.**
The 3000 logged `self_cap` 56.61 → 49.95 → 44.71 → 41.58 **while `engaged=0`** — genuine
capability loss while *not* throttling — and those same values appear as `peer_cap` in the 1000's
log at the same moments (which independently confirms exchange and measurement are sound).
Whoever sits in the JOIN seat becomes the slow one, so **the identity of the slow peer flips.**

**Now: the target is the PAIR'S SUSTAINED WORST.**
- Decaying **low-water marks** on both capabilities: a new low is taken immediately, forgotten
  only after 4 s of nothing worse and then at 0.50 fps/window.
- **`goal = min(self_lo, peer_lo) + 0.50`. `min()` is symmetric**, so both consoles compute the
  same target and **a role flip does not move it** — the mover changes, the target does not.
- **One mover now falls out for free**: whoever is the binding constraint is already below the
  target and inserts no waits, because the throttle can only ever *add* delay. No flag decides.
- **Asymmetric ramp: fall 8.00 fps/window, rise 0.50.** Getting slow late desyncs a link;
  getting fast late costs nothing.
- **The floor CLAMPS, it no longer releases** — releasing against a peer at 38.89 snapped us back
  to 59.73 and made the gap 21 fps instead of 1.1. `EVT pace_floor … clamped, still pacing`.
- **`engaged` is a wide-hysteresis report, not a gate**, so the cycling (and the 3000 never
  re-engaging) cannot recur.
- `EVT pace_match` now carries **`self_lo=` and `peer_lo=`** — read those to see the marks
  working. **Treat the matcher as mitigation now, not the fix.**

## PARTLY SUPERSEDED (control law -> ADR-0028) — the roles run at DIFFERENT SPEEDS (2026-08-02, `phase5d-peersync`, ADR-0027)

**Read the ADR-0028 section above first.** The mechanism and the anti-ratchet below are confirmed
correct on hardware; the control law described here (symmetric ramp, engage/release gate, floor
releases) was replaced after the first field run. Kept for the reasoning, which still stands.

**The field finding this branch exists for — both consoles, current build, facts.** The
join/client role costs **~12 fps** against the host role: PSP-3000 **58 as host, 46 as join**;
PSP-1000 **56-58 as host, 49-52 as join**. The client sends ~1.9x the host (`core_tx` 3867 vs
2044, same session). **Transport is perfect** — `core_tx`/`core_rx` mirror exactly,
`overflow=0 spill=0 txfail=0 drop_crc=0`, `retx_pct` 1-6 % — and ADR-0021's per-frame session
costs total only ~500 µs (`pump=208 rx=186 arq=32 enq=111`, `pdp` 65-79 µs on its own thread).
**So the client's extra cost is inside `retro_run` — the core's RFU client path — not us.**
Symptom: in the Union Room the joining console stops accepting local input while still
rendering the peer's movement; the other console then hits the game's fatal link screen.

**Why the DIFFERENCE is the bug and not merely the slowness.** Two real GBAs both run at
59.7275 Hz, so their link timing is mutually consistent. **Gen-3's RFU counts link timeouts in
FRAMES, not seconds** (`RFU_DEF_TIMEOUT` = 32 frames). Two emulators ~20 % apart are
permanently inconsistent in a way two cartridges never are. **Equality matters more than
absolute speed:** if both run at the same rate, even a slower one, the game-side link timing
becomes mutually consistent again.

**FIELD RESULT ON THE FIXED RATE (PSP-3000, Build F) — THE MECHANISM WORKS, THE NUMBER DID
NOT.** `session_pace … ramp_done` fired, and `session_pace_miss … not chasing` fired repeatedly
**with the target never moving** — the no-chase rule is hardware-proven. But the console achieved
**35-38 fps against a 40.00 clamp while reporting `self_cap=52-57`**. Not a control bug:
**quantization**. We insert whole vblanks, so a frame costs 1 or 2 of them; 40.00 needs ~half the
frames to finish inside one 16.68 ms vblank, and when per-frame work sits just above that, none
do. The achievable rates are exactly `59.94/N` — 59.94, 29.97, 19.98, nothing in between.
**Default is now 29.97 (two vblanks) and every request is snapped and logged**
(`EVT session_pace_snap req=… applied=… vblanks=…`). **Note 2 vblanks = 29.97, not 29.86** —
29.86 is the *GBA's* 59.7275 halved, but what we insert is a *PSP display* vblank at 59.94 Hz; a
configured 29.86 snaps to 29.97 and says so. Full reasoning: ADR-0035 §1.

**SUPERSEDED AS THE DEFAULT — READ ADR-0033 FIRST.** Everything from here to the end of this
section describes the *adaptive* matcher, which is now `net_pace_match=2`. **The requirement
changed:** the user has said full-speed emulation during a session is a nice-to-have, not a
requirement — *"trading is a temporary activity... If the game only runs at half speed during
trades, that's fine, so long as it runs normally when our wireless functions are disabled."*
So the default is now a **fixed rate**: while a session is live both consoles clamp to
`config.ini net_session_fps` (default **40.00**), glide in and out at 4.00 fps/s, and
negotiate nothing. The matcher's target was walking 41→59→45→51 all session because the
*input* (peer capability) genuinely moves with game workload, and a moving applied rate is
itself a desync source. **`net_pace_match` is remapped: 0 off, 1 = fixed-rate (default, was
the matcher), 2 = the matcher.** An existing `config.ini` with `=1` silently adopts the new
policy, deliberately. Read in the field log: `EVT net_pace_match mode=1 policy=fixed
fixed=40.00 ramp=4.00`, then `EVT session_pace fps=… reason=session_start|ramp_done|
session_end`, and `EVT session_pace_miss actual=…` if a console cannot hold the rate (we
report it and do **not** chase downward — a known steady rate beats a correct-but-moving one).

**LANDED, NOW MODE 2 — ADR-0027: the faster console paces down to the slower peer.** Each side publishes
its emulated rate as a 2-byte payload on the existing `ND_T_PING` (no new frame type, wire
compatible both ways — an old peer's bare PING reads as *unknown*, never as *slow*), on a
**separate 500 ms cadence** because during a trade DATA flows every frame and the keepalive
PING would never fire. The frontend applies it with a fractional-vblank accumulator (target
46.50 → 1.2890 vblanks/frame → an extra vblank on 28.9 % of frames), floored at 40 fps,
capped at 59.7275, ramped 2.00 fps per second, Schmitt-triggered so **only one side ever
throttles**. Lever: **`config.ini net_pace_match`** = 1 on (default) / 0 off, with an
autopilot override — an A/B with no rebuild, exactly like `net_tx_thread`/`log_thread`/
`sram_thread`.

**THE TRAP, and how it is closed — read this before touching the loop.** If a console
advertised its *achieved* fps, the pair would **ratchet downward without bound**: A throttles
to 46 to match B, A now reports 46, B throttles to match, A re-measures lower, both crawl into
the floor — and it would look like a performance regression, not a control-loop bug. So a
console advertises its **capability**, derived from per-frame **work** time (the loop iteration
with every vblank wait excluded — ADR-0021's `frame=`): a frame costing *w* µs occupies
`ceil(w / 16.683 ms)` vblank periods, and the free-running rate is `frames x 59.94 / periods`,
EMA-smoothed over ~1 s windows. **Our own idle is not work, so throttling cannot move our own
advertised number.** `peer_cap` therefore means "how fast my partner CAN go", never "how fast
it is currently choosing to go", and the loop has exactly one mover.

**Audio: continuous, pitch drops with the pace.** Left alone, pacing to 46.5 fps drains the
ring and `audio_thread` emits silence for ~22 % of every second — a constant crackle. So
`g_audio_step` follows the target and production/consumption match exactly. The cost is a
proportional pitch drop (~4 semitones at 46.5), gliding rather than jumping because the pace
ramps. **If the user hates the pitch more than they hate a failed trade, `net_pace_match=0` is
the answer and ADR-0027 should be revisited with their verdict.** Full reasoning in ADR-0027
§audio, including why frameskip is not an alternative.

**READ THESE IN THE NEXT FIELD LOG, IN THIS ORDER (ADR-0033/0034/0035 — this list
supersedes the mode-2 list further down):**
1. `EVT net_pace_match mode=1 policy=fixed fixed=29.97 req=29.86 snap=1 vblanks=2` — the
   policy came up; `fixed=` is what is APPLIED and `req=` what was configured. If they differ,
   `EVT session_pace_snap` says so on its own line (ADR-0035).
2. `EVT session_pace fps=29.97 reason=session_start` then `reason=ramp_done` **on BOTH
   consoles, with the same `fps=`**. Announcing the clamp and reaching it are different
   claims; `ramp_done` is the one that says the console is really running at the clamp.
3. `EVT session_pace_miss actual=… — not chasing`. **On Build F this fired constantly against
   the unreachable 40.00; at 29.97 it should now be ABSENT.** If it still fires, the console
   cannot hold two whole vblanks per frame and the next step is N=3 (19.98) — NOT a smaller
   fraction, which would land back in the quantization trap. Either way check the next
   `sess_cost pace=` still reads `29.97/…`: the target must never move.
4. `EVT sess_cost … pace=target/self_cap/peer_cap/engaged`. Expect `29.97/<45-58>/<45-58>/1`.
   **`self_cap` well above the clamp while the ACHIEVED rate misses it is the quantization
   signature** — that is precisely what Build F showed, and it is why the snap exists.
5. `EVT config_corrupt` — **must not appear at all.** It is the ADR-0035 guard for the
   struct-layout corruption that shipped in Build F; one line means a stale object is back.
   Also check `config.ini group=` reads `GPSP07` and not `07`.
6. **Did the trade complete, did the join console keep accepting input, and how choppy was
   it?** That is the whole point now — the user accepted half speed, but not chop and not
   dropped inputs.
7. `EVT session_pace fps=59.73 reason=session_end` then `ramp_done_nominal`, and `EVT fps
   emu=` back at ~58-59 afterwards. **Full speed outside a session is the one hard
   requirement.**
8. `EVT blit_mode req=2 mode=2 name=vram` — the VRAM bump succeeded (only the PSP-1000 has
   been measured; the 3000 must confirm, and a fallback would read `mode=0 name=cached`).
   Then `EVT blit_prof` **in-session**, since the whole table so far is from solo play.
9. A/B it: `net_pace_match=2` restores the old adaptive matcher, `0` turns pacing off, and
   `net_session_fps_snap=0` runs the raw unreachable rate again for the steady-vs-jitter
   comparison.

**The mode-2 (adaptive) reading list, kept for the A/B:**
1. `EVT net_pace_match mode=2 …` — the policy came up.
2. `EVT pace_match target= self_cap= peer_cap= engaged= why=` — the control loop. Expect the
   HOST console to show `engaged=1` and the join console `engaged=0`: **exactly one mover.**
   `why=` is engage / release / ramp / hold (10 s heartbeat while engaged).
3. **`self_cap` across those lines is the ratchet detector.** It is capability, so it must
   stay flat while `engaged=1`. If it sags, the loop is chasing its own tail — that is a bug,
   and that one line is the proof.
4. `EVT sess_cost … pace=target/self_cap/peer_cap/engaged` — the same state on the 10 s cadence.
5. **Did the trade complete, and did the join console keep accepting input?** That is the
   whole point; the fps numbers are only how we explain the answer.
6. A/B it: `net_pace_match=0` should reproduce the 58-vs-46 split and the failure. (The
   adaptive matcher itself is now `net_pace_match=2`, not `1` — ADR-0033.)

**`EVT sess_cost` also gains `core=avg/max` — `retro_run` is finally bracketed** (ADR-0021
priced everything *except* the core and inferred the rest by subtraction). This bounds the
still-open ~41-49 ms `frame` maximum too: **if `core_max` tracks `frame_max` the residue is the
core/dynarec; if it does not, it is ours** — the GU blit/`sceGuSync` path, or the
once-per-60-frames `sceIoGetstat(DUMP_MARKER)` on a stick whose 4 KiB write costs 34 ms
(ADR-0026). That stat is harness-only and a strong deletion candidate, deliberately left alone
here as a separate variable.

**What the rig can and cannot prove.** The PPSSPP rig runs **both instances at full speed**, so
it does **not** exercise the field's role asymmetry and pace matching correctly stays inert
there — the default `--radio=40` run asserts exactly that (neither side throttles when the
peers already agree, so a healthy session pays nothing). To see it work at all, the harness
gained **`--slow-join=US`**, which burns US µs of *busy* work per frame on the join instance
only (so the cost lands in the same per-frame work time the capability estimate reads);
`--slow-join=6000` reproduces the field's 58-vs-46 gap. That run asserts one mover, the 40 fps
floor, and the anti-ratchet guard on `self_cap`. **`--pace-match=0|1`** pins the lever.
**Only hardware can settle whether equal rates actually fix the trade.**

## OPEN — the problem is STALLS now, not steady-state cost (2026-08-01, branch `phase5c-stalls`)

**The field, current shipping build, both consoles — facts, do not re-derive.** The wireless
layer is healthy (`overflow=0 spill=0 txfail=0 rxerr=0 drop_crc=0`, `retx_pct=2-4`,
`srtt_us≈35-60k`) and ADR-0021's per-frame session costs came back small (`pump≈200µs
rx≈190µs arq≈20µs enq≈118µs`, `PdpSend` 55-80 µs on its own thread — so the ADR-0021
hypothesis was **wrong about the mean and right to measure it**). What is left is the
**maximum**: PSP-1000 `frame=10194/62978`, PSP-3000 `frame` max **73403 µs**. Emulated fps
sits at 56.9-58.9 against 59.7275 *while frameskip discards 97 % of rendered frames* — so the
deficit is stalls, not steady-state work. A 63 ms freeze is four frames in which the peer gets
no answer, which is exactly when the user's trades now fail.

Other maxima in the same windows: `pump` 7449 µs, `rx` 7441 µs, `evt` **12002 µs** (1000) /
**12525 µs** (3000), `pdp` max **27514 µs** (one send blocked 27.5 ms in the WLAN driver — on
the TX thread, so it did not stall the emulator, but it says what that driver can do), and
`sram_flush … ms=25.791`.

**Increment 1 LANDED — ADR-0024: no memory-stick write happens on the emulation thread.**
`fe_evt`/`fe_log` now format into a 16 KiB SPSC ring; a `gpsp_io` writer thread at priority
0x22 (below main's 0x20, so it runs in the vblank slack) does the `fwrite`+`fflush`. Starts
after `fe_host_boot` so boot diagnostics stay synchronous; every exit funnels through one
`evt_shutdown()`. **`config.ini log_thread` (synonym `net_log_thread`) = 0 restores the inline
path — an A/B with no rebuild.** Gated on the shipped EBOOT: netdrv suite PASS,
`run_trade_test_psp.sh --radio=40` PASS (`artifacts/psptrader40-20260801-231058/`),
`run_boot_test.sh` PASS.

**Read these three fields in the next field log, in this order:**
1. `EVT log_thread mode=1 prio=0x22` — confirms the thread came up at all.
2. `sess_cost … evt=lines/us/max` — **now what the EMULATION thread paid** (format + ring copy
   + signal). It was 12002/12525 µs. Tens of µs = the move worked. The max is re-based when
   the sink installs, so the synchronous boot flushes cannot pollute it.
3. `sess_cost … evtio=us/max/drop/hi` — **the memory stick itself, on the writer thread.**
   `max` staying near 12000 is the *expected* result and proves the stick is still slow and
   simply out of the way. `drop` must be 0 (lines lost to a full ring); `hi` is ring
   high-water in bytes (rig: 422-502 of 16384).

**Increment 1 VERIFIED IN THE FIELD (PSP-3000).** `evt` max **12525 → 118 µs (~100x)**;
`evtio=22484/15602/0/422` — the writer thread absorbing the stick's real 15.6 ms, `drop=0`,
ring high-water 422 B of 16384; **`frame` max 73403 → 63772 µs**, ~10 ms recovered, as
predicted. Session health unchanged (emu 58.1-58.8 fps, transport clean).

**Increment 2 LANDED — ADR-0025: the `.sav` block writes leave the emulation thread too.**
The same field log fingered what was left: `frame` max was 37264 µs early and jumped to 63772
in the one window containing `sram_flush … wrote=57344 blocks=14/32 mode=delta ms=29.296`.
ADR-0020's budget bounded the loop but explicitly could not bound one block's overshoot, and
named the writer thread as the real end state. Now done: the dirty **scan** stays on the emu
thread (CPU, not I/O); every open/seek/write/flush/close is on the `gpsp_io` thread. Sync is a
per-block **byte** state (CLEAN/DIRTY/WRITING) — the emu thread only ever *raises* DIRTY, the
writer only CLEANs a block it still owns — plus a 4 KiB staging copy so the recorded CRC is
always the CRC of the bytes on disk. **No lock**; instead an ordering contract in fe_host.h,
enforced by `io_thread_stop()` running *before* `fe_host_shutdown()` and ending in a
synchronous `fe_host_sram_sync()`. Lever: `config.ini sram_thread` (0 = ADR-0020 in-frame
drain, 1 = default). Gate on the shipped EBOOT: netdrv PASS, **`run_save_test.sh` PASS**
(`artifacts/save-20260801-234138/` — in-game save round-tripped, crc f13fd310 both ways),
`run_trade_test_psp.sh --radio=40` PASS (`artifacts/psptrader40-20260801-234158/`),
`run_boot_test.sh` PASS (`artifacts/boot-20260801-234416/`).

**`ms=` is unambiguous now.** `EVT sram_flush` carries `slices=` (1 = `ms` *is* a single stall;
>1 = elapsed across that many drain calls), `worst_ms=` (largest single call = the real stall),
`blk_ms=` (worst single 4 KiB write — what a budget structurally cannot bound), `scan_ms=`
(the CRC scan, the only save-path cost the emu thread still pays) and `thr=` (1 = none of it
was on the emulation thread).

**Increment 2 VERIFIED IN THE FIELD.** `sram_flush … thr=1` on both consoles; `frame` max
**69470 → 47286 µs** (PSP-1000) and **63772 → 40932 µs** (PSP-3000) — ~22 ms off each, as
predicted. Saves clean. **And the hardware fact that explains the whole class:** the user's
memory stick costs **`blk_ms=34.817` for ONE 4 KiB block** and **`worst_ms=65.556` for a
six-block flush.** You cannot budget a device whose quantum is 35 ms into a 16.68 ms frame —
which is why the answer was to move the work, not schedule it better.

**Increment 3 LANDED — ADR-0026: the dirty SCAN follows the writes off the emulation thread.**
The field priced it at `scan_ms` **11.243 / 11.272 / 11.280 ms** — two thirds of a frame, every
300 frames (~5 s), **whether or not anything was dirty**, and unlike the writes it is pure CPU.
Spreading it across frames was the obvious fix and the wrong one: the cycles would still be
the emulation thread's, and the 1000 is already missing real time. So `sram_scan()` moved to
the writer thread on its own ~5 s wall-clock cadence — the **same** interval as before, so the
power-cut loss window is unchanged. `fe_host_sram_flush()` is now a request when a writer is
installed; a forced flush waits for scan-then-drain, and `fe_host_sram_sync()` counts a pending
scan as outstanding work. **The emulation thread now pays exactly nothing for the save path.**
Gate on the shipped EBOOT (md5 `78c7d4e72ab77209f34b70802b8d2f66`): netdrv PASS,
`run_save_test.sh` **PASS** (`artifacts/save-20260802-000119/`, crc ab00c401 written and
re-loaded), `run_trade_test_psp.sh --radio=40` PASS, `run_boot_test.sh` PASS
(`artifacts/boot-20260802-000356/`).

**Reading `ms=`/`scan_ms=` once `thr=1`: they are WALL CLOCK on a preempted low-priority
thread, not CPU.** Under uncapped fast-forward the rig shows `scan_ms=2405` — that is the
writer being starved because main never blocks at the vblank, not 2.4 s of CRC32. Once `thr=1`
the number that matters is `thr` itself.

**Still open on this branch, deliberately shipped separately (one variable per hardware run):**
the 63 ms residue is not yet bracketed (`retro_run` vs GU blit vs audio vs the once-a-second
`sceIoGetstat` on ms0 from `file_exists(DUMP_MARKER)`); `rfu_link_down` still cannot fire on
the field's path. See below.

**`rfu_link_down` — the linkage question is SETTLED, and it is not the linker.** The archive
carries the weak stub (`psp-nm gpsp_libretro_psp1.a` → `W gpsp_rfu_link_down_hook`) and the
linked EBOOT resolves it to main_psp.o's strong definition (`T gpsp_rfu_link_down_hook`), and
it *does* fire in the PPSSPP rig on the current binary (`reason=1` host / `reason=2` join,
`artifacts/psptrader40-20260801-231058/`). So the field's silence is **call-site coverage**:
the game never takes any of rfu.c's four hook sites. Note in particular that `RFU_DOWN_RESET`
(reason 3) is `#define`d but **never called** — `rfu_reset()` (the GPIO adapter reset the game
pulses) drops `rfu_state` to IDLE silently — and so do `RFU_CMD_HOST_STOP` with no clients
(rfu.c:417) and a failed `RFU_CMD_CONCOMPL` (rfu.c:478). A game that hits its own fatal
wireless screen and stops talking to the adapter fires nothing at all, by construction.

## OPEN — a live session still costs the PSP-1000 ~20 fps, and now it is INSTRUMENTED (2026-08-01, branch `phase5b-session-cpu`, ADR-0021)

**The measurement, from the user's hardware — treat as fact, do not re-derive.**
Solo, wireless entirely off: `EVT fps emu=57.65 / 58.66 / 58.64 / 57.64`. Emerald at ~98 %
of 59.7275 with everything rendered — **the console is not slow.** Session up, frameskip
off: `emu` collapses to **34-47**. Frameskip skipping ~97 % of frames (`rendered=1.9`):
only **50-57**. The PSP-3000 holds ~58.9 in the same sessions. Traffic is modest
(~65 core packets/s each way, `overflow=0 txfail=0 ringdrop=0`). **A session costs the
1000 about a third of its frame budget and it is not rendering.**

**Why that is a hunt for milliseconds, not for a big slow thing.** The loop is
vblank-locked, so frame time is quantised to 16.68 ms. A console solo at 57.6 fps already
fills most of a period; +2-4 ms tips a large fraction of frames into a *whole* extra
period. Same reason the 3000, which starts with more headroom, barely notices.

**`EVT sess_cost` now names the cost, once per ~10 s while a session is live** (full key
reference in ADR-0021):
`win clk_ns frame=avg/max pump=avg/max poll=n/work/avg/max rx=avg/max arq=avg/max
 pdp=calls/mean/max enq=mean/max txq=q/inline/full evt=lines/us/max txthr=N`.
`clk_ns` is this console's measured cost of one `sceKernelGetSystemTimeWide`, so the
instrumentation prices itself (~13 reads/frame).

**What the PPSSPP rig could and could not settle.** It settled the call *counts*, which
are core behaviour and therefore portable: during an active trade the core makes only
**~6 `poll_receive` calls per frame** (not the ~450 the WAITEVENT path allows) and
**~1.7 `PdpSend` calls per frame**. It cannot settle the *cost* of a `PdpSend`: in PPSSPP
that is a host UDP `sendto`.

**Leading suspect, therefore: `sceNetAdhocPdpSend`.** 1.7 calls/frame × ~1.5 ms would be
the entire cliff, and a slower WLAN path on the 1000 would explain the 3000 asymmetry.
Addressed by moving the send onto its own thread (`config.ini net_tx_thread`,
1 = prompt/default, 2 = deferred into the vblank slack, 0 = inline as before) — **but
whether that helps depends on how much of `PdpSend` sleeps versus computes, which only
hardware can say.** All three modes ship so the user can A/B them in one sitting.

**What to read in the next field log, in order:**
1. `sess_cost … pdp=calls/mean/max`. `calls/win ÷ 600 × mean` is the per-frame send bill.
   **Milliseconds → that is the 20 fps**; A/B `net_tx_thread` 1 → 2 → 0 and watch
   `frame=` and `EVT fps emu=`.
2. If `pdp=` is cheap: `frame=` minus (`pump` + `poll` + `evt`) is unaccounted work inside
   `retro_run` — bisect there next, and note `frame=max` for one-off spikes.
3. `txq=q/inline/full` — `inline` or `full` non-zero means the TX ring was too shallow.
4. `evt=lines/us/max` finally prices the ms0 flushes directly (previously argued innocent
   from line counts, now measured).
5. `EVT skip_engage mode=fixed_interval interval=1` if `net_frameskip=1` — the adaptive
   mode is bounded to 50 % now; the old `auto_threshold` version skipped 580/600 because
   the core's `FRAMESKIP_MAX=30` was the only bound (ADR-0021 §3).

**Landed on this branch:** the instrumentation; the gated mid-frame poll
(`netdrv_poll_needed` — 99.0 % of polls short-circuited in the rig, zero delivery loss,
but worth tens of µs/frame not ms); the TX offload thread; the bounded adaptive frameskip.
**Also a real bug the desktop rig caught and the PSP rig hid:** adding an OPTIONAL hook to
`nd_transport` segfaulted `sdl/main_sdl.c`, which passes an uninitialised stack vtable —
`*_transport_iface()` now `memset`s first and a unit test asserts it. ADR-0017's "run both
rigs" earned its keep again.

## CLEARED — per-frame session costs that were suspected and are NOT the problem

Investigated 2026-08-01 while chasing the session lag. All four were plausible; none of
them was the ~1 s stall ADR-0020 fixed. **Two of them are now MEASURED rather than argued**
(ADR-0021): log I/O is `evt=` in `EVT sess_cost`, and the ACK/send count is `pdp=`. Read
those before re-litigating anything below — and note that the ~20 fps session cost is
still open, so "innocent of the stall" is not "innocent of everything".

- **EVT log I/O to ms0 on the hot path.** A complete trade session writes **134 lines total**,
  69 of them the periodic ones (`fps`/`heartbeat`/`net_stats`/`adhoc_stats`/`sram_flush`).
  `net_stats` is every 5 s, the rest every 600 frames. `fe_evt` fflushes, but at ~1 line per
  5-10 s that is noise, not stutter. Measured on `artifacts/psptrader40-20260801-194017/`.
- **ACK amplification from the ADR-0017 prompt-ACK change.** Already coalesced: netdrv sets a
  per-peer `need_ack` flag and emits **at most one bare ACK per peer per pump tick**, and only
  when nothing of our own is due to piggyback on (netdrv.c:1136-1152). The rig bears it out —
  host `tx=5889 rx=5459` over a full session, i.e. no per-packet ACK inflation.
- **GU blit cache maintenance.** `psp/video_psp.c:77` already uses
  `sceKernelDcacheWritebackRange(fb_staging, sizeof(fb_staging))`, not `WritebackAll`. Nothing
  to win here.
- **RX thread starving the emu thread.** `ADHOC_RX_PRIO` 0x1E sits just above main's 0x20 by
  design (transport_adhoc.c:48-50), but the thread blocks in `sceNetAdhocPdpRecv` and wakes
  ~62 times/s to memcpy ≤576 B into the SPSC ring. That is the right shape: prompt draining,
  negligible preemption.

## OPEN — exit-Union-Room fatal wireless screen (investigation, 2026-08-01)

**BISECTION DONE — it is ours, not upstream.** The user ran the RetroArch-PC×2 control
(`C:\gpsp-adhoc-tools\ra-rig\`, stock gpsp core, RA netplay, localhost): Union Room entered, a
lap walked, **host walked out first, then the client — both left gracefully and saved, no
error, no fatal screen.** So this is not upstream behaviour, not a game rule about the host
leaving, and not the core's RFU being latency-intolerant by nature.

**What we know for certain.** Our process was healthy throughout — both field logs end with
an orderly `EVT net_down` + `EVT exit code=0` (the user left to the XMB by hand after seeing
the screen), `overflow=0 spill=0 txfail=0 rxerr=0 ringdrop=0`, and the core payload counters
are mirrored exactly across the link. Nothing was lost and nothing crashed. The screen is
gen-3's **fatal** RFU path ("…please turn off the power"), not the recoverable
"communication error, returning to the previous screen" dialog.

**LEADING HYPOTHESIS — the SRAM-flush stall causes it.** Leaving the Union Room makes the game
(a) run its RFU disconnect handshake with the peer and (b) **save**. (b) dirtied SRAM, our
dirty check fired, and the old `fe_host_sram_flush` wrote all 131,072 bytes to the memory
stick synchronously on the emulation thread — which the field heartbeats price at **~0.5-1.3 s
of frozen frame loop**. A peer that stops answering a teardown handshake for a second is
indistinguishable from a dead one. This explains everything the other theories did not: why RA
is clean (a PC's `.srm` write is free), why the error lands at room exit rather than during the
trade, and why the transport logs are spotless — **we never dropped anything, we just stopped
servicing the link at the worst possible moment.** ADR-0020 is therefore the candidate fix for
*both* the lag and this, and the build carrying it is what the user should retest.

**Evidence that WEAKENS the earlier "the peer never learns the partner left" theory** (it was
the leading one before the bisection; keep it as the fallback). The new `EVT rfu_link_down`
breadcrumb fired during a *normal* rig trade, and it propagated correctly: right after
`post_save`, the host logged `rfu_link_down reason=1 slot=0` (it issued `RFU_CMD_DISCONNECT`)
and the joiner logged `reason=2 slot=0` (it received `NET_RFU_DISCONNECT`) — then both games
carried on to `trade_complete` with no error. So an RFU teardown DOES cross our transport and
both sides DO see it. Artifacts: `tools/e2e/artifacts/psptrader40-20260801-192229/{host,join}.log`.

**Instrumentation landed (ADR-0019), and what to read in the next field log:**
`EVT rfu_link_down reason=N slot=N net=up|down` — `reason` 1 = this console issued
`RFU_CMD_DISCONNECT` (the player walked out), 2 = the peer sent `NET_RFU_DISCONNECT`,
3 = adapter reset, 4 = host-side client timeout. Plus `EVT sram_flush … ms=` right beside it.

- **Fatal screen gone + `sram_flush … ms=` in single digits** → the stall was the cause; close it.
- **Fatal screen still there but `ms=` is small** → the stall hypothesis is dead. Next suspect:
  the RFU teardown handshake's deadline against our measured ~51 ms RTT (rfu.c `RFU_DEF_TIMEOUT`
  = 32 frames ≈ 533 ms, `rfu_resp_timeout`, `RFU_DEF_RTXMAX` = 4). **Do not patch it
  speculatively** — guessing twice is how the ARQ storm survived Phase 4.
- `reason=1` on the leaver with **no** `reason=2` on the peer → the DISCONNECT did not cross;
  back to the fallback theory (propagate an explicit end-of-RFU wind-down, or stop feeding the
  core once its RFU is inactive).
- **No `rfu_link_down` at all** → the game ended its session without taking any of rfu.c's
  disconnect paths, and the hook needs another call site.

**Designed-but-not-built causation test:** in the PPSSPP rig at `--radio=40`, inject a
deliberate multi-hundred-ms emu-thread stall at the moment of room exit and check whether
`CB2_PrintErrorMessage` appears. That converts correlation into causation and becomes a
regression test for both bugs at once. It needs the exit-room harness below to be validated
first — building a causation test on an unrun harness would prove nothing.

**Harness case: written, committed, NOT yet run** — `tools/e2e/run_exit_room_test.sh` plus
`testdata/fixtures/emerald_exit_room_{host,join,host_stay}.inputs`. It completes the normal
trade, then walks both sides out of the room in-game (host first, joiner ~11 s later so two
avatars never contend for the entrance tile) and re-enters, with
`--leaver=join` to separate "a partner left" from "I left". Oracle is direct:
`logram cb2_*` samples `gMain.callback2`, and `CB2_PrintErrorMessage|1 == 0x0800b1a1` **is**
the fatal screen (same predicate the `--disconnect` trade test already uses). Navigation is
the inbound path reversed (raw coords = map + 7): col 3/2 down to row 10, right to col 7,
down onto the entrance warp. **It has never been executed**, so treat the navigation legs as
unverified — the first run may need coordinate fixes, and that is expected, not a defect.

## ISSUE #2 — RESOLVED 2026-08-01 (branch `phase5-netdrv-realradio`)

**Fixed, reproduced-then-proven, all regressions green.** The diagnosis below stands
exactly as written; what follows is the resolution. Details: ADR-0016 (never drop
RELIABLE), ADR-0017 (adaptive RTO), ADR-0018 (auto-frameskip), docs/ARCHITECTURE.md
(netdrv rewrite), docs/TESTING.md (`--radio` profiles).

**The harness could not see this bug, so that was fixed first.** The PSP ad-hoc
transport now carries the same debug fault shim the UDP backend always had
(`adhoc_transport_set_fault`, .gpsp-harness.ini `net_latency_ms`/`net_jitter_ms`/
`net_loss_pct`, applied on the RX side so two peers give ~2x latency as RTT), and
`run_trade_test_psp.sh` gained `--radio=40|80|160`. **Never tune ARQ timing again
without running these** — loopback RTT is microseconds, real radio is tens of ms, and
that single gap is the whole bug.

**Reproduced before the fix** (pre-fix EBOOT, PPSSPP rig):

| profile | retx/acked | dup/rx | overflow | outcome | artifact |
|---|---|---|---|---|---|
| `--radio=40` | 148 % | 53 % | 0 | trade completed | `artifacts/psptrader40-20260801-165305/` |
| `--radio=80` | 202 % | 60 % | 0 | trade completed | `artifacts/psptrader80-20260801-165553/` |
| `--radio=160` | 300 % | 63 % | **6279 (join)** | **both games wedged, trade never started** | `artifacts/psptrader160-20260801-165945/` |

The 160 ms artifact contains the field's kill shot verbatim — `LOG netdrv: arq overflow
peer=0 (payload lost)` x6348 on the JOIN side only, host clean, `txfail=0 ringdrop=0
faultdrop=0` — i.e. the same asymmetry and the same "nothing was actually lost" as the
two real consoles (field: 280 % / 68 % / overflow=2).

**Proven gone after the fix** (same rig, same profiles):

| profile | retx/acked | dup/rx | overflow | outcome | artifact |
|---|---|---|---|---|---|
| `--radio=40` | 148 % → **0 %** | 53 % → **0 %** | 0 | trade PASS, srtt 79 ms / rto 105 ms | `artifacts/psptrader40-20260801-171322/` |
| `--radio=160` | 300 % → **0 %** | 63 % → **0.4 %** | 6279 → **0** | trade PASS, srtt 210 ms / rto 251 ms | `artifacts/psptrader160-20260801-180303/` |

At 160 ms the fixed build moved 6145 frames where the broken one moved 83889 — **13.6x
less airtime for a trade that actually completed.** Queue high-water was 16/28 slots out
of 384; the spill never had to engage.

**Full regression sweep on the shipped commit** — all green, all exit 0:

| harness | verdict | artifact |
|---|---|---|
| netdrv unit suite (12 tests, 4 new) | PASS | `make -C netdrv test` |
| `run_trade_test.sh` (desktop, 5 % loss + 30 ms jitter) | PASS | `artifacts/trade-20260801-175001/` |
| `run_trade_test_psp.sh` (PPSSPP ad-hoc, clean) | PASS | `artifacts/psptrade-20260801-180046/` |
| `run_trade_test_psp.sh --radio=160` | PASS | `artifacts/psptrader160-20260801-180303/` |
| `run_nettest.sh` | PASS | `artifacts/nettest-20260801-175643/` |
| `run_boot_test.sh` | PASS | `artifacts/boot-20260801-175538/` |

**One regression was caught and fixed en route, and it is the interesting one.** The first
adaptive build passed every PSP radio profile and broke `run_trade_test.sh` — verified
against `main` so it was ours, not flake. Cause: pacing and loss *recovery* are different
deadlines. ADR-0010's "~60 ms cliff" is about recovery, and no honest RTO on a 30-80 ms
link can meet it; the old fixed 30 ms timer met it by retransmitting before an ACK could
arrive, which is blind redundancy — and is exactly what stormed on radio. Both are true at
once, so a transport now declares whether it pins recovery (`ND_RTO_FIRST_MAX_US`):
desktop/UDP does and keeps ADR-0010's ladder verbatim; PSP/ad-hoc does not, because 802.11
repairs loss beneath us. **If you touch this again, run both rigs — one alone will lie to
you.**

**What changed**

1. **RELIABLE is never silently dropped (ADR-0016).** A full ring toward a live peer is
   backpressure now: the payload goes to a per-peer spill FIFO and returns to the ring as
   it drains. Only a genuinely unabsorbable backlog (spill exhausted / OOM) or an oversize
   payload fails — and it fails **loudly**: `ND_STOP_TX_FAILED` → `EVT net_error
   reason=txq_overflow` → user-visible toast → teardown. Oversize is refused, never
   truncated, with a compile-time assert on the build's payload budget.
2. **Adaptive RTO (ADR-0017)** — per-peer SRTT/RTTVAR, `RTO = SRTT + 4·RTTVAR`, Karn, and
   a **peer-level retained** backoff (without the retention the estimator can never start
   when the floor sits below the true RTT — it stayed at zero samples until this was
   fixed). Floors are transport constants: desktop/UDP unchanged at 30 ms, PSP/ad-hoc
   100 ms (ceiling 800 ms).
3. **Right-sized ad-hoc slots (ADR-0016)** — `ND_MAX_PAYLOAD` 560 → 144 (RFU max is 104,
   roster 138; the 560 B budget existed for out-of-scope cable modes), which bought
   `ND_TXQ_CAP` 96 → 384 and `ND_WINDOW` 16 → 32 while PSP memory went **down 6.5 KiB**
   net (netdrv instance +19.7 KiB heap, transport ring −26 KiB .data).
4. **Prompt explicit ACKs**, **`srtt_us`/`rto_us`/`retx_pct`/`txq_hi`/`spill` in
   `EVT net_stats`** (this class now self-diagnoses from one log line), table-driven CRC16
   (~8x cheaper/frame), and **core auto-frameskip while `net=up`** (ADR-0018 — `fe_host`
   now answers the audio-buffer-status env it used to decline; the FF interlock is intact).

**Remaining risk for the next hardware session:** the harness models radio latency, not
radio *behaviour* — no WLAN power-save wakeups, no interference bursts, no 802.11 retry
backoff, and PPSSPP holds 59.7 fps where a PSP-1000 may not. The 100 ms floor is a
judgement call from two field logs; watch `srtt_us`/`rto_us` in the next field log and
retune the floor to what the radio actually reports. `--radio=160` is a stress profile,
not a plausible medium.

---

## TOP OPEN ISSUE #2 (original diagnosis, kept for the record) — session drops seconds after peers link

**Discovery is SOLVED** (see issue #1 below: both consoles on a fixed ad-hoc channel — 1 —
find each other reliably). New frontier: the two games discovered each other, both began
entering the Union Room, then **~a few seconds later the host (PSP-3000, `PSP-A`,
`role=host`) showed the game's "communication error", and the client (PSP-1000, `PSP-B`)
followed shortly after.**

### ROOT CAUSE (from the host's field log — quantitative, not speculative)

The host log (`PSP-A`, PSP-3000, full run captured) shows an **ARQ retransmit/duplicate
storm**, not packet loss and not a radio fault:

- Peak session counters before the freeze:
  `tx=3663 rx=5596 acked=837 retx=2336 dup=3805 core_tx=837 core_rx=1503`
- **retx/acked ≈ 2.8** — every delivered packet took ~3.8 transmissions.
- **dup/rx ≈ 68%** — two thirds of everything received was a duplicate the peer had
  already sent.
- Meanwhile `txfail=0 rxerr=0 ringdrop=0 drop_crc=0 drop_mal=0 overflow=0` — the radio
  delivered everything; **nothing was actually lost.**
- Compare the PPSSPP Gate-4E trade on the same code: `acked=1871 retx=205` →
  **retx/acked ≈ 0.11**. Real radio is ~25× worse. The harness could never surface this:
  loopback RTT is microseconds, real PSP ad-hoc RTT is milliseconds-to-tens-of-ms (worse
  with WLAN power-save).
- After the storm the game gave up: from `heartbeat frames=4200` onward `core_tx`/
  `core_rx`/`acked`/`retx`/`dup` are all **frozen** (837/1503/837/2336/3805) while
  `tx`/`rx` keep ticking at keepalive rate and `peers=1` holds. So **our session never
  died — the emulated RFU timed out** because effective latency ballooned. Comm error on
  the host first, then the client, exactly as observed.

Interpretation: `ND_RETX_MS` (30 ms floor, tuned on loopback per ADR-0010) is far shorter
than real ad-hoc RTT, so we retransmit before an ACK can possibly arrive → duplicates →
more airtime → more latency → more spurious retransmits. A textbook RTO death spiral that
only manifests on real radio.

### THE KILL SHOT (client log, PSP-B / PSP-1000, `role=join`)

The client log contains what the host's does not:

```
LOG netdrv: arq overflow peer=0 (payload lost)
LOG netdrv: arq overflow peer=0 (payload lost)
... overflow=2 ...
```

**We silently dropped two RELIABLE core payloads.** Per ADR-0003 every packet the core
sends is `RELIABLE|FLUSH_HINT` — dropping one is an unrecoverable contract violation: the
RFU state machine loses a command it will never see again, and the game's link is dead
from that instant even though the radio and our session stay nominally healthy. Both logs
then show `core_tx`/`core_rx` frozen forever (client 1505/837, host 837/1503) while
keepalives keep flowing and `peers=1` holds until teardown. **The link didn't drop; we
broke the guarantee the core depends on.**

Full causal chain (now evidenced end to end):
1. Real ad-hoc RTT ≫ 30 ms RTO floor → premature retransmits (host retx/acked ≈ 2.8,
   client ≈ 2.6; dup 68% host / 58% client; `txfail=0` everywhere — nothing was lost).
2. Duplicate storm saturates airtime → latency climbs → more spurious retransmits.
3. Send window (PSP `ND_WINDOW=16`) stalls awaiting acks; the tx queue
   (PSP `ND_TXQ_CAP=96`, halved for memory in Phase 4) backs up.
4. **Queue overflow → RELIABLE payload discarded** → RFU protocol state destroyed.
5. Games show "communication error" (host first, client after); our transport happily
   keeps pinging a corpse.

Asymmetry worth noting: the client transmitted ~1.6× more than the host
(client `tx=5736` vs host `tx=3663`) and it was the client that overflowed — consistent
with the PSP-1000 both generating more RFU traffic and pumping its ARQ slower.

### Fix plan (ordered; #1 is the fix, the rest are supporting)

1. **Never silently drop RELIABLE payload (correctness, do this first).** On tx-queue
   pressure the driver must apply **backpressure**, not discard: the netpacket `send_fn`
   path should block/queue-to-spill or, at absolute minimum, mark the session failed and
   tear down with a clear user-visible error. Silent loss corrupts the core's RFU state
   and produces exactly the "worked for 20 seconds then comm error" symptom. Add a unit
   test that asserts *no* RELIABLE payload is ever dropped under sustained overload.
2. **Adaptive RTO instead of the fixed 30 ms timer.** Per-peer TCP-style `SRTT`/`RTTVAR`,
   `RTO = SRTT + 4·RTTVAR`, exponential backoff, clamped to a **floor near 80–120 ms on
   PSP-adhoc** (keep the snappy value for UDP/desktop — make the floor a transport
   constant). This removes the storm that causes the overflow in the first place.
3. **Deepen the queue for free by right-sizing slots.** ADR-0003 sized frames for a 560 B
   payload to leave room for cable modes that are explicitly out of scope; **RFU's real
   max is 104 B** (SERIAL-PROTO-NOTES §2). Sizing ARQ slots to ~128 B lets `ND_TXQ_CAP`
   grow ~4× within the same PSP memory budget — directly undoing the Phase-4 halving that
   made overflow reachable.
4. **Ack promptly and explicitly.** A received DATA packet should trigger an ACK on the
   same pump tick rather than waiting to piggyback, so RTT samples are honest and
   retransmits aren't provoked by our own ack latency.
5. **Log RTT + storm indicators**: `srtt_us`, `rto_us`, retx-ratio, and queue high-water
   in `net_stats`, so any future field log self-diagnoses this class instantly.
6. **Secondary: pace/skip.** The PSP-1000 ran laggier, generated ~1.6× the traffic, and
   was the side that overflowed — a slower console also pumps ARQ less often. Engage core
   auto-frameskip while `net=up` so game logic and the ARQ pump hold real time; trim
   per-frame wireless overhead on slower units.
7. Re-examine ADR-0010/0011 tolerances (queue depth, 30 ms jitter edge) **with real-radio
   RTT numbers in hand** rather than loopback assumptions.

Validation note: the PPSSPP harness cannot reproduce this. Add a **latency-injection
profile to the adhoc transport's debug shim** (the UDP backend already has one) so
`run_trade_test_psp.sh` can run with e.g. 40 ms RTT + jitter and prove the adaptive RTO
before the next hardware session.

## OPEN ISSUE #1 (RESOLVED IN THE FIELD) — peer discovery needed a fixed channel

Two real PSPs (PSP-1000 + PSP-3000, ARK-4, WLAN on, `two-psp-kit`): both boot, both run
Emerald, both reach the Union Room. **They never see each other.** Host log (`PSP-A`,
`role=host`) is unambiguous:

- `EVT adhoc_up group=GPSP07 mac=00:26:43:4b:be:68` → group formed, real radio up
- `session_start id=0 peers=0`, `net_up role=host`, `mem_free=389120` (memory fine)
- 316 datagrams **transmitted**, `txfail=0`; **`rx=0` for the entire 5-minute run**
- `ringdrop=0 oversize=0 rxerr=0 ctlevt=1 ctldisc=0 ctlerr=0` — no errors, exactly one
  adhocctl event (the connect), never a peer event
- `core_tx` climbs to 352 (the game's RFU is actively broadcasting for partners),
  `core_rx=0`, `peers=0` throughout; clean `net_down` + `exit code=0`

Diagnosis: our stack is behaving correctly and transmitting; **the two radios are not in
the same IBSS.** `transport_adhoc.c:435` calls `sceNetAdhocctlConnect(group)`, which on
real hardware is create-or-join and depends on the system **ad-hoc channel** setting
(XMB → Settings → Network Settings → Ad Hoc Mode). Two PSPs on different channels each
create their own "GPSP07" group and are mutually invisible — which is exactly what this
log looks like. This class of bug **cannot appear in PPSSPP**, whose AdhocServer
matchmaker makes channels irrelevant (the plan's §7.1 honesty clause predicted precisely
this residual risk).

Next steps, in order:
1. Get **PSP-B's** `frontend.log` — if it also shows `adhoc_up` + `rx=0` + `ctlevt=1`,
   the split-group diagnosis is confirmed (two islands).
2. Set **both** consoles to the **same fixed ad-hoc channel** (1, 6, or 11 — not
   Automatic) and retry, host first. Highest-probability fix, zero code.
3. If still split: make the client path explicitly scan-then-join —
   `sceNetAdhocctlScan`/`GetScanInfo` machinery already exists at
   `transport_adhoc.c:510-546`; ensure join mode *requires* finding the host's group
   (retry/backoff) instead of falling through to `Connect` and creating a rival group.
   Consider `sceNetAdhocctlCreateEnterGameMode`-style explicit create (host) vs
   `sceNetAdhocctlJoin` (client) split, and log the resolved channel in `EVT adhoc_up`.
4. Add `EVT adhoc_peers n=... mac=...` polling (`sceNetAdhocctlGetPeerList`) so the log
   distinguishes "no peer in group" from "peer present, datagrams lost".

Everything above the radio is proven; this is the last unproven layer.

## Working agreement (non-negotiable, from the plan + practice)

1. **"Done" means a green harness run.** Never accept an agent's (or your own) claim
   without a passing script + artifacts. The e2e scripts in `tools/e2e/` are the law.
2. **Verify before you build; cite before you claim.** Read headers/code, don't recall
   them. Every [VERIFY]-grade fact gets a file:line citation in the docs.
3. **The user is not the debug loop.** Hardware is endgame-only (Gate 4-H) plus anything
   the user volunteers. Never ask them to sideload iteration builds.
4. **ADR everything** that deviates, decides, or touches the core. Core patches: minimal
   + upstreamable only (one exists: ADR-0011).
5. **Bisect with the RA rig** before blaming our stack; **reduce to transport echo**
   before debugging two unknowns.
6. **Background agents go stale** — when a detached run finishes, kick them with the
   verdict via a message; use process-exit tripwires rather than waiting on their
   monitors.
7. Environment quirks that will bite you: sandboxes/PPSSPP must live on native WSL FS
   (never /mnt/c); seed `GraphicsBackend = 3 (VULKAN)` under Xvfb; PSP LIBS ordering
   breaks import stubs (ADR-0012); OneDrive repo path is fine for builds, slow for I/O.

## How to work with Austin (read this twice)

Austin is sharp, playful, and deeply knows these games — his tile-level Union Room tip
saved a debugging session, and his product instincts (invisible variants, silent
wireless) improved the plan. Treat him as a collaborator, not a requirements source.
The tone that works: warm, direct, honest about uncertainty, zero condescension, and
answer his skepticism with evidence rather than defensiveness (he will poke — "why did
we even bother with X?" — engage the substance, concede what's fair, and show receipts).
Lead with outcomes, keep jargon translated, celebrate the wins — this project is his
childhood dream running on purple plastic, and the enthusiasm is load-bearing. Log his
game knowledge into the docs when he shares it. Be good to him.

— Fable 5, signing off. The hard part is done; land it gently.
