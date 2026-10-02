# GBA link-cable multiplayer: feasibility benchmarks

Branch `claude/gba-link-bench` (off the published 3.0.0 source, 696ad10).
Written 2026-09-29. This is measurement to choose a design, not the feature.

Every number is labelled by where it came from:
- **HW**: a real PSP.
- **PPSSPP**: the MIPS dynarec in PPSSPP (WSL).
- **host**: the x86 dynarec in the unix core, run by `tools/linkbench/lb_host`.

PPSSPP does not model cache or memory timing, so its times are not performance
numbers (it pins 59.9 fps at any emulated clock). Only HW times are used for cost.

## TL;DR

| | Mirror / dual emulation (M) | Thin client (T) | Protocol emulation (P, found here) | Delayed virtual cable (D) |
|---|---|---|---|---|
| Hard requirement | cross-console determinism | bandwidth | a per-game protocol fake | a game that tolerates stale SIO words |
| Evidence | **bit-identical**: 28 PPSSPP arms over 3 titles with different dynarec histories, plus **PSP Go HW = PPSSPP for 6,020 frames** (ME and CPU arms) | AW2 mean 37.5 KB/s at 60 fps (zero-run + LZ4); Emerald 43 KB/s. Fits 100 KB/s at about 48 fps delivered and 200 KB/s at about 58 fps | two AW2 instances linked over UDP play turns at 0 ms and 30 ms ±15 ms after one bug fix; stuck in the lobby at ≥100 ms + 5 % loss | not measured: no two-instance SIO path exists |
| CPU | 2 cores on **every** console; about 12.4 ms mean, 5.9 % of frames over budget (Go projection) | the same 2 cores on the **host**, plus encode; the client is nearly idle | 1 core (today's cost) | 1 core |
| Generality | every link-cable game (multi-pak and single-pak), no per-game work | every game, no per-game work | only the games with a fake: AW1/AW2 and Pokémon gen 3 in gpSP today | untested; very likely few games |
| Verdict | **GO** (recommended long-term design) | feasible, but no cheaper than M and strictly more complex | **fastest path for AW2 alone**; not a platform | not recommended without a model |

**Recommendation.**
- **AW2 first:** use P. The code already exists in the core: `serial_proto.c` has `SERIAL_MODE_SERIAL_AW2`, auto-selected for `AW2E`. It needs:
  - the overflow fix (section 4),
  - a hardware test over real ad-hoc,
  - a latency budget check.
- **Platform:** build M (dual GBA cores plus input lockstep, reusing the GB dual-link machinery). It is the only design that reaches "as many games as possible" with no per-game work. The determinism it needs is demonstrated, not assumed.
- **T:** keep it as the fallback, for the case where M's determinism breaks in long hardware sessions, or where the client must be a PSP-1000 that cannot afford two cores.

---

## 1. Determinism (go/no-go for M)

**Oracle.** For every frame, `shash` hashes the guest-visible state:
- r0–r15 and CPSR,
- IWRAM and EWRAM data (not the SMC tag halves),
- I/O, palette, OAM and VRAM,
- a rolling audio hash.

On the PSP it is the harness `shash = 1` key, now v3 with a per-frame `c=` cost field. On the host it is `lb_host --hash`. `tools/linkbench/cmp_hash.py` reports the first differing frame and field.

**Method.** Every arm runs the **same measured segment**: a savestate reload (the autopilot `state` step) followed by the same frame-stamped input script. Only what happened before the reload differs. Arms (`tools/linkbench/run_det_ppsspp.sh`):

| arm | what differs before/under the measured segment |
|---|---|
| cold | boot, load at frame 30, reload after 10 frames |
| cold_rep | repeat of cold (repeatability) |
| warm_tour | the whole tour played first, then reload (warm translation caches, SMC history, ranked gate candidates) |
| warm_intro | soft reset plus 2,000 frames of intro first |
| jit_small | small translation-cache tier (2 MiB / 384 KiB) instead of large (10 / 0.5 MiB) |
| rom_cap2 | ROM cache capped at 2 × 1 MiB blocks, so heavy paging instead of resident |
| ballast | 5 MiB heap ballast, so every JIT/ROM buffer lands at a different address |
| warm_small | warm_tour + small tier |
| warm_cap2 | warm_tour + 2-block ROM cap + 3 MB ballast |
| paranoid | `cache_paranoid = 1` (full D+I sync on every JIT sync) |

The histories really did differ. These counters are summed from `EVT core_prof`:

| title | frames compared | cold | the most different arm |
|---|---|---|---|
| Advance Wars 2 (owner's state, full scene tour) | 6,020 | ram_smc 37, page_load 1 | warm: ram_smc 56; warm_cap2: page_load 186 |
| Pokémon Emerald (owner's state, walk + menus) | 2,492 | page_load 1 | warm_cap2: page_load 377 |
| Heart & Soul (SMC-heavy; `heart_soul_heavy` fixture, rival battle) | 3,942 | ram_smc 17,038, ram_full 57 | warm: ram_smc 36,955, ram_full 124; warm_small: rom_flush 6, ram_full 168; warm_cap2: page_load 1,586 |

**Results (PPSSPP, harness64 EBOOT, the shipped core flags).** Every arm against cold was **IDENTICAL** over the whole segment, including the audio hash:
- 9/9 arms on AW2,
- 9/9 on Emerald,
- 9/9 on H&S.

That is 27 comparisons plus the 3 cold_rep repeats.

**Hardware (PSP Go, `GBADHOC-AW2BENCH`, 2026-09-29).** Both the ME arm and the CPU arm were **IDENTICAL to the PPSSPP golden for all 6,020 frames**, including the audio hash. So a real console computes the same game, bit for bit, as the emulator does. The PSP-1000 run is pending (same folders, `builds/gba-link-bench-out/OWNER-STEPS.txt`).

**Host (x86 dynarec).** warm_tour, warm_intro and late_load (500 frames of BIOS/intro before the first load) all match cold on guest state. The only difference is the frontend's audio frame split: 548 against 549 samples in the first frame. That is output buffering, not guest state.

**Where it does diverge (the real rules for M).**
- **Different engines.**
  - The interpreter build diverges from the dynarec at the very first frame (r, pc, IWRAM).
  - The x86 dynarec matches the MIPS dynarec for 3,090 frames of AW2, then diverges at f=3132 (r, pc, io, in the minimap scene).
  - Cycle accounting belongs to the engine, so **both consoles must run the same core binary.** Same EBOOT, same flags, including the SMC_* set, which changes timing (docs/UNBOUND-PERFORMANCE.md).
- **SMC translation gates.** They survive a savestate load and move block boundaries: SMC_GATE_CHARGE is on precisely because gates change timing. In every H&S arm the same two gates (`0300168c, 03001688`) had been learned within 10 frames of the first load, so gate *sets* never differed at a reload. **Not tested:** a session where the two consoles hold different gate sets. The cold-start rule should reset gate state and ranked candidates at session start: call `smc_gates_reset()`, which today runs only at ROM load.
- **Everything outside the savestate.** RTC seed, save RAM and BIOS must be exchanged at session start, as GB dual-link already does. The GB hw1 lesson was an RTC day carry from a 2011 console clock.

**Cold identical start rules (proposed):**
- same EBOOT and core flags,
- same BIOS (sha1),
- same ROM (sha1),
- same save and state bytes,
- `smc_gates_reset()` plus `flush_dynarec_caches()` at session start (the state load already flushes the caches),
- the RTC frozen at a shared seed.

Nothing else was needed in any arm.

**Verdict M: GO.** No first divergence was found, so there is no cause to report. Honest limits:
- the longest segment is 6,020 frames (100 s) on one hardware model;
- two gate sets that differ are untested;
- ME render timing does not affect guest state; that holds by construction and was confirmed by the ME/CPU HW arms matching.

## 2. Thin-client bandwidth (T)

**What a client needs each frame.** The ME renderer's proven input set (docs/ME-RENDERER-DIVERGENCE.md):
- 1 KiB VRAM pages,
- OAM (1 KiB),
- palette (1 KiB, raw; the client converts),
- the per-line LCD register capture (160 × 64 halfwords = 20 KiB),
- affine seed + `OAM_UPDATED` (20 B).

`lb_host --bw` measures the delta against what the client already holds, at k = 1, 2, 3, 4, 6 (send every k-th frame, merged), with these encodings:
- **raw**: ME granularity (whole changed pages and changed 128-byte capture lines);
- **rle**: zero-run XOR patch, 4-byte headers, applied in place;
- **rlz4**: rle then a real LZ4 block encoder (round-trip self-tested);
- **defl1**: deflate level 1 of the XOR delta. The PSP can inflate in hardware (`sceKernelDeflateDecompress`); encoding would cost the host.

Scenes are autopilot `evt` marks (`scripts/aw2_scenes.txt`, `scripts/em_scenes.txt`). Emulator data from the host core; the delta is game behaviour, not engine timing.

**AW2, bytes per sent frame, k = 1** (`bw_report.py`; KB = 1000 B):

| scene | frames | rlz4 median | p95 | max | KB/s raw | rle | **rlz4** | defl1 |
|---|---|---|---|---|---|---|---|---|
| campaign map + dialogue | 125 | 6 | 1,307 | 6,890 | 362 | 34.3 | **20.8** | 39.8 |
| mission select | 428 | 267 | 1,967 | 11,278 | 678 | 84.2 | **37.4** | 45.1 |
| mission-start animation | 406 | 94 | 2,023 | 17,163 | 372 | 57.4 | **40.2** | 58.4 |
| briefing / dialogue | 612 | 15 | 1,737 | 3,333 | 162 | 32.8 | **27.1** | 51.5 |
| map idle | 136 | 13 | 1,631 | 3,209 | 95 | 29.4 | **25.1** | 51.1 |
| unit select + move | 272 | 42 | 1,737 | 2,975 | 147 | 34.9 | **28.5** | 53.1 |
| **battle animation** | 526 | 90 | 1,748 | 36,396 | 282 | 42.9 | **32.0** | 54.1 |
| map scrolling | 430 | 44 | 1,806 | 3,801 | 272 | 36.6 | **30.9** | 53.5 |
| map menu | 196 | 6 | 1,724 | 2,965 | 122 | 32.9 | **26.4** | 51.4 |
| minimap | 162 | 13 | 1,741 | 3,333 | 199 | 36.6 | **29.2** | 51.4 |
| CO screen | 314 | 148 | 2,090 | 12,072 | 504 | 56.6 | **43.1** | 58.5 |
| attract intro (full-screen art) | 2,410 | 651 | 1,961 | 43,646 | 769 | 89.6 | **45.5** | 43.7 |
| **all** | 6,017 | 127 | 1,958 | 43,646 | 494 | 63.4 | **37.5** | 48.9 |

- The p95 of about 1.7–2 KB in every map scene comes from animated map tiles (sea, idle units): 3–8 VRAM pages rewritten several times a second. They compress poorly.
- The maxima are scene loads, where most of VRAM is rewritten.
- A full keyframe (resync) is 57 KB with rlz4.
- **Not reached from the owner's save: the CO-power animation.** The tutorial mission's meter never filled. The closest measured scenes are the full-screen transitions: mission-start stamp, CO screen, attract art. All are ≤45 KB/s mean and ≤43.6 KB peak.

**Pokémon Emerald** (second title, heavier scrolling): walking is 50.6 KB/s mean and 1,983 B p95; party and summary screens are 32–43 KB/s; overall 43.2 KB/s rlz4. The keyframe is 23 KB.

**Frame skip, rlz4 KB/s** (AW2 all scenes):

| k | 1 | 2 | 3 | 4 | 6 |
|---|---|---|---|---|---|
| KB/s | 37.5 | 30.5 | 26.8 | 24.7 | 22.3 |

Skipping saves little. The animated tiles change anyway, so the merged delta shrinks slowly.

**Budgets: an adaptive partial-update model.**
- The host sends a frame only when the link has drained; that frame carries everything changed since the last *sent* frame.
- ADPCM audio is reserved out of the budget.
- Each packet is charged 16 B of framing.

Delivered fps / longest freeze / p95 queueing delay:

| scene | 100 KB/s | 200 KB/s | 300 KB/s |
|---|---|---|---|
| map idle / scroll / menus / unit move | 47–49 fps, 50–67 ms, 18–26 ms | 59.6–60, 17–33 ms, 8–9 ms | 60, 17 ms, 5–6 ms |
| battle animation | 47.6, **433 ms**, 16 ms | 56.1, 200 ms, 8 ms | 57.7, 133 ms, 5 ms |
| mission-start / CO screen | 42–43, 333–350 ms, 20–30 ms | 55–57.5, 133–150 ms, 10 ms | 57–59, 83 ms, 7 ms |
| attract intro | 50.8, 517 ms, 20 ms | 58.2, 233 ms, 10 ms | 59.1, 150 ms, 7 ms |
| Emerald walking | 50.5, 50 ms, 21 ms | 59.6, 33 ms, 10 ms | 60, 17 ms, 7 ms |

- Every scene fits 200 KB/s with essentially full smoothness.
- At 100 KB/s, map play runs at about 48 fps. Scene transitions and the battle intro freeze for 0.3–0.5 s: a full-VRAM reload queues behind the link.
- **Budgets are per direction.** The client→host input stream is negligible.

**Audio (compact format).**
- DirectSound: AW2 and Emerald both stream FIFO A and FIFO B at 224 B/frame each (13,379 Hz, 8-bit). That is 26.9 KB/s raw, or 13.4 KB/s as 4-bit ADPCM.
- PSG registers change 0.02 (AW2) to 0.9 (Emerald) bytes per frame.
- **Proposal:** send the two FIFO streams (ADPCM) plus PSG register deltas (at most about 30 B/frame). The client runs `sound.c`'s mixer itself. The alternative is ADPCM of the host's mixed output: mono 16 kHz is 8 KB/s, stereo 32 kHz is 32 KB/s, and the host pays the encode.
- The client must know the host's timer-derived sample rate. That goes in each packet header.

**Verdict T:** bandwidth is **not** the blocker. The mean is 38–43 KB/s plus 13 KB/s of audio against a hardware-measured ad-hoc 100–300 KB/s (the owner's figure; hw2 X/W to confirm). What T really costs:
- the host needs the same two cores as M (section 3), plus encoding;
- the client's own inputs gain a round trip plus a frame;
- a client "ME render from network" mode and an audio mixer path must be built;
- the ME's known 1.7 % of mid-frame-DMA frames (docs/ME-RENDERER-DIVERGENCE.md) render from end-of-frame state on the client with no CPU fallback.

## 3. CPU cost

**HW, PSP Go** (`hw_cost_report.py builds/gba-link-bench-out/hw-go/*`). Core ms per frame, retro_run only:

| scene | ME arm median / p95 / max | CPU arm (renderer on CPU) median / p95 |
|---|---|---|
| campaign / select / briefing / battle | 5.67–5.75 / 5.9–6.2 / 8.5–18.8 | 11.30–11.34 / 11.6–11.8 |
| map scroll | 5.88 / 6.33 / 7.99 | 12.24 / 12.80 |
| map menu / CO screen | 5.79–5.98 / 10.1 / 13.1 | 12.3–12.4 / 17.4 |
| minimap | **9.67** / 10.11 / 14.36 | 16.97 / 17.31 |
| attract intro | 5.51 / 5.91 / 14.84 | 11.83 / 18.06 |
| **all** | **5.67 / 9.44 / 18.77, mean 5.92** | **11.43 / 17.52, mean 12.49; 880 frames > 16.7 ms** |

- The renderer costs about 5.6 ms of main CPU per frame. The ME takes it off completely.
- PPSSPP says a flat 5.2 ms (ME arm) and cannot see the scene differences. That is the known rig limit.

**Two cores (M on every console; T on the host), projected from the Go ME arm.** The partner machine runs without rendering (capture skipped for M, captured but not drawn for T). The GB hardware lesson: dual cost 2.1× solo on HW against 1.58× in PPSSPP, from I-cache thrash.

| factor over solo | mean | median | p95 | frames > 16.7 ms |
|---|---|---|---|---|
| 2.0× (no thrash) | 11.8 ms | 11.3 | 18.9 | 5.8 % |
| 2.1× (the GB lesson) | 12.4 ms | 11.9 | 19.8 | 5.9 % |
| 2.3× (pessimistic) | 13.6 ms | 13.0 | 21.7 | 6.2 % |

- Two AW2 cores fit the Go's frame with about 25 % headroom in play.
- About 6 % of frames overrun. They cluster in the minimap / menu / CO-screen scenes (solo 9.7–10 ms) and in scene loads. In turn-based AW2 these are dropped frames, not gameplay errors.
- **Estimate only.** Two GBA cores in one process do not exist yet. gpSP is globals plus a dynarec, and each instance needs its own translation caches and SMC tables.
- A thin-client host adds the delta encode: zero-run scan of the dirty pages and capture plus LZ4 of about 1–2 KB. Estimated 0.3–1 ms on the 333 MHz MIPS; not measured.

**Memory for two cores (analysis).**
- The ROM buffer can be shared: both sides run the same cartridge, read-only.
- Each core needs its own RAM, VRAM and tag arrays (about 1 MB) plus JIT caches.
- The large tier is 10.5 MB per core. Two of them fit a 3000/Go.
- A **PSP-1000** would need two small tiers (2 × 2.4 MB) beside the ROM cache on a heap of about 22 MB. That is plausible for an 8 MB cart like AW2, and tight for a 16–32 MB cart.
- Measuring this with `heap_census` on the 1000 is the first step of any M build.

**Hardware bench:** staged in `builds/gba-link-bench-out/`:
- `GBADHOC-AW2BENCH` (ME) and `-CPU`, with `OWNER-STEPS.txt` and `MANIFEST.txt`;
- the PPSSPP golden for the determinism compare in `golden/`.

The Go is done (`hw-go/`). The PSP-1000 run will answer 1000-class cost and cross-model determinism.

## 4. Delayed virtual cable, and design P (found while checking)

**D (generic delayed cable) was not measured.**
- gpSP has no local two-instance SIO path: one global core per process.
- A generic model would also need the partner's multiplayer-mode word to arrive in the *same* transfer; with D frames of delay, every transfer returns stale words.
- Evidence it does not work generically: upstream gpSP chose to write **per-game** protocol fakes instead (below).
- What D would take: two cores in one process (the M prerequisite), a SIO model between them, and then per-game delay tests. That is M's hardest part without M's correctness.

**P: gpSP already emulates the AW1/AW2 link protocol for netplay.**
- Code: `serial_proto.c`, `serialaw_*`; `gba_over.h` selects it for `AWRE/AWRP/AW2E/AW2P`.
- It fakes the peer's clock and exchanges whole AW packets over netpacket, so it is latency-tolerant by design. Pokémon gen 3 has the same kind of fake (`serialpoke`).
- The PSP frontend's netpacket path is not RFU-gated. It should carry P unchanged, but this is **untested on a PSP**.

Desktop probe (`tools/linkbench/p_link.sh`): two SDL twins, `scripts/aw2_link_{host,join}.txt`, netdrv UDP with injected latency/jitter/loss.

| one-way latency ± jitter, loss | stock core | with the bench-only fix |
|---|---|---|
| 0 ms | lobby → map/CO/rules → Day 1 → 2P turn → Day 2 → 2P Day 2: **full turn exchange** | same |
| 30 ± 15 ms, 0 % | **host aborts, "stack smashing detected", 4/4 runs** | full turn exchange |
| 60 ± 40 ms, 3 % | full turn exchange (1 run, desktop netdrv profile) | into the battle, turns progressing (the blind script runs late) |
| 100 ± 60 ms, 5 % | lobby never admits 2P | same |
| 200 ± 100 ms, 5 % | lobby never admits 2P | same |

**Bug (upstream code, affects P on the PSP).** A PACKETXG frame is up to 255 + 1 + 2 = 258 words. `serialaw_senddata()` packs it into `u32 pkt[2 + 128]`, a 256-halfword stack buffer, and into an 8-bit count field. Instrumentation logged `wcnt=258 state=0 cmd=4fff` right before the abort. The candidate fix, compiled only under `LINKBENCH_AWFIX`:
- `pkt[2 + 130]`,
- the receiver takes the word count from the datagram length.

**The ≥100 ms lobby failure is not isolated.** Candidates: the lobby's own timeouts, or the transport at that loss. Ad-hoc jitter is "tens of ms", so 30–60 ms is the relevant regime, and it works there.

**Verdict P:** the fastest way to AW2 link. Required work:
- fix the overflow,
- run a hardware session over real ad-hoc,
- check for a stall at ≥100 ms spikes.

It covers only AW1/AW2 and Pokémon gen 3 (and RFU games, already supported). Every other title needs its protocol reverse-engineered.

## Generality (owner requirement: as many games as possible)

| design | games covered | per-game work |
|---|---|---|
| **M** | every 2-player link-cable game: multi-pak (Mario Kart SC, Four Swords, FFTA, Fire Emblem arena, Pokémon, AW) and single-pak/multiboot (the partner boots over emulated SIO, so no partner ROM is needed). 3–4 players would need 3–4 cores and are out of reach. | none. Needs a correct SIO model between the two in-process cores (normal 8/32-bit, multi-player, UART) plus input lockstep (GB dual-link design). The input delay (RTT/2 plus jitter buffer, likely 2–4 frames) matters for action games (MKSC, Four Swords) and is irrelevant for turn-based ones. |
| **T** | every 2-player game, same list | none. The client's input latency is one round trip plus a frame, and scene loads freeze at low budgets. |
| **P** | AW1, AW2, Pokémon gen 3 (plus RFU titles) | one protocol fake per game, reverse-engineered |
| **D** | unknown; likely only games whose protocols tolerate stale words | per-game validation at best |

Only M and T generalise. M needs no bandwidth, no ME client mode and no audio pipeline, and its correctness requirement is now demonstrated. It is the recommended platform.

## Biggest risks

1. **M: two gpSP cores in one process.** Globals, the dynarec's translation caches, SMC tag arrays and the memory map all need duplicating (the objcopy renaming used for TGB Dual is much harder for a dynarec with absolute addresses in emitted code). The alternative is a context-swap design. Budget: the PSP-1000's memory.
2. **M: long-session determinism** across two different consoles and models. Only 100 s on one Go has been compared so far. Differing SMC gate sets are untested; reset them at session start.
3. **CPU:** a projected 12–14 ms mean and about 6 % dropped frames on a Go for AW2. Heavier games (Emerald, and SMC-heavy hacks where the JIT dominates) could exceed the budget; a hardware dual-core measurement is needed. On the 1000: unknown until the bench runs.
4. **P:** the upstream overflow above, plus untested behaviour under real ad-hoc spikes.
5. **T:** input latency for the client, the 1.7 % ME mid-frame-DMA frames, and 0.3–0.5 s freezes on scene loads at 100 KB/s.

## Reproduce

All WSL (`~/gbalink`: ROMs copied read-only from F:, BIOS sha1 300c20df…):

```sh
cd tools/linkbench && make && make INTERP=1 && make gpsp_sdl_lb && make AWFIX=1 gpsp_sdl_lb_awfix
./run_det_host.sh ROM STATE SAV scripts/aw2_scenes.txt OUT          # host arms
tools/build.sh harness64 --out DIR                                   # Windows side
MARK=campaign_map ./run_det_ppsspp.sh EBOOT_DIR ROM STATE SAV OUT PPOUT 4   # PPSSPP arms
lb_host --rom R --bios-dir B --save S --state ST --script scripts/aw2_scenes.txt --bw X.csv --log X.log
python3 bw_report.py X.csv X.log
./p_link.sh OUT scripts/aw2_link_host.txt scripts/aw2_link_join.txt 9000 JIT LOSS LAT
python tools/linkbench/hw_cost_report.py builds/gba-link-bench-out/hw-go/GBADHOC-AW2BENCH
```

**Instrumentation hygiene.**
- `LINKBENCH` / `LINKBENCH_AWFIX` exist only in `tools/linkbench` builds.
- shash v3's `c=` is harness-only (`GPSP_PERF_RIG`).
- Release ELF 755cc16f: none of `LB serialaw`, `shash v3`, `lb_fifo`, `c=%u`.
- Host tests: 30 passed, 0 failed.
- The branch also fixes a latent non-PSP link break: `smc_gates_refresh_values()` had no definition without `SMC_GATES`.
