# GBA link cable by dual emulation (the "mirror")

Branch `claude/gbalink-mirror` (from `claude/candidate-3.1`, 8b48b48). Started 2026-10-01.

**Goal.** A general GBA link cable for two PSPs:
- normal 8/32-bit, multi-player and general-purpose SIO, for any game;
- no per-game code;
- benchmark game Advance Wars 2.

**Design.** Each PSP runs both GBAs with an in-memory cable between them; only buttons cross ad-hoc, in lockstep. It is the GB/GBC design (docs/GB-LINK.md) applied to gpSP.

**The bar** is upstream's AW-specific serial fake (`serialaw_*`), measured on `claude/gbalink-baseline`.

Every number below is labelled:
- **HW**: a real PSP;
- **PPSSPP**: the MIPS dynarec in PPSSPP (not a timing source);
- **host**: the x86 dynarec in the desktop prototype;
- **est.**: an estimate, with its basis.

## TL;DR

| question | answer | evidence |
|---|---|---|
| Duplicated cores or context swap? | **Duplicated** (instance B = the core relocatable with every global renamed `gbab_*`) | A context swap copies ≥ 1.4 MB per switch; the exact cable needs 60-400 switches a frame (section 1) |
| Does a linked AW2 battle run? | **Yes, a whole one.** The AW baseline's RAM-gated 10-round battle runs with both machines on the real autopilot engine: both scripts DONE, the day counter 1→10 on both machines, 109,729 multi-player transfers. It also runs on the PSP build in PPSSPP. | host, section 4; PPSSPP |
| Deterministic? | **Yes.** Rerun, instance swap, lookahead 2464/5000/10000, and shared vs own ROM (resident or paged) are hash-identical: 9000 and 16000 frames × 2 machines | host, section 4 |
| Is stopping a core invisible to the guest? | **Yes, at the core's own events.** Stops every 37-5000 cycles are hash-identical to never stopping, over 6000 AW2 frames. An extra scheduler event is **not** invisible (it diverged at frame 964). | host, section 2 |
| Memory | Instance B costs 2.58 MiB plus its JIT tier (2.38 small / 10.5 large). Statically linked, that is taken from every game's ROM cache on a PSP-1000, so the shipping form loads B only for a session (section 3). | psp-size of the 3.1 core |
| On the PSP's dynarec? | **Yes, in PPSSPP**: the same linked battle runs (9,618 transfers); rerun, lookahead 10000, shared vs own ROM, 32 vs 64 MiB layout are all hash-identical (16,418 frame lines) | PPSSPP, section 4b |
| Same cartridge, one ROM? | **Yes.** Shared ROM with a private page 0 per core (the GPIO/RTC port lives in ROM page 0) is hash-identical to two copies, resident and under heavy paging | host + PPSSPP, section 3 |
| Fits a PSP-1000? | **AW2 yes** (PPSSPP 32 MiB model): A on the small JIT tier, the 8 MiB cart resident and shared, 1.16 MB post-load floor, with B statically linked | PPSSPP |
| Solo play changed? | **No.** A release build of this branch disassembles identically to 8b48b48 (everything is under `GBA_LINK`) | psp-objdump |
| Two SMC-heavy cores? | **Clean in PPSSPP.** Heart & Soul's heavy battle, partner mirroring the player's buttons, sharing a paged 32 MiB ROM (117/113 page faults): deterministic across reruns and paging. The two machines' RAM, VRAM, OAM and palette are identical on all 3,368 frames. | PPSSPP, section 4c |
| Found on the way | **An upstream uninitialised read** (gpSP `retro_load_game` translates the BIOS SWI vector from an unwritten ROM block) leaked heap content into guest state. It is zeroed in `GBA_LINK` builds; release is untouched and the owner decides. | valgrind, section 4c |
| CPU (PSP-3000, HW) | **14.2 ms a frame mean, p95 16.0, 2 % of frames over 16.7 ms** for two AW2 cores in the link session, at the new default lookahead (half a frame). Solo 5.7 ms: 2.5× solo. Lookahead 2464 (409 switches a frame) cost 21.3 ms, so a switch costs ~17 us. | HW, section 12 |
| Same game on PSP and PPSSPP? | **Yes, bit for bit.** All 9 completed 3000 runs match the PPSSPP golden on every frame of both machines: the full 10-round battle (2 × 30,567 lines) and the link session (4 × 16,419). | HW, section 12 |
| Two consoles? | **The session layer exists (section 14).** The GB link's protocol runs the mirror over ad-hoc; two PPSSPP consoles play the whole AW2 battle with both machines' per-frame hashes identical on both consoles to the agreed end. Hardware pair rig staged (3000 + Go). **Section 15:** the GB link's smoothness fixes carried over (each switchable), a NACK bug fixed for GBA sessions, and a boot reserve without which a PSP-1000 cannot join at all; the 3000 + 1000 pair rig (hw1) runs before/after + the AW fake. | PPSSPP; sections 14, 15 |
| Other games? | **Section 16: seven fixtures, one command** (`multigame/run_matrix.sh`), two PPSSPP consoles x loopback / 15 ms ±5 ms jitter / 3 % loss. Sonic Advance race, Mario Tennis match, Emerald Cable Club trade and battle, AW2: PASS in all three conditions (per-frame identical, goldens, oracles). Mario Golf is deterministic, but its link never forms (open, 16.4). Found and fixed: the RFU-adapter probe's normal-mode transfer never started (16.3). Every game's session is multi-player 16-bit. **Super Circuit (16.7):** a two-player Multi-Pak VS race in which both karts complete lap 1, driven closed-loop from RAM; PASS in all three conditions, 1.14x AW2's cost in PPSSPP. | PPSSPP; section 16 |
| Stable on HW? | **One unexplained hang.** Run 5 of session 2 (the full battle without the hash) never handed off. The console was unattended, so it is undiagnosed; the evidence is preserved on the card (section 12). The same arm is clean in PPSSPP (twice) and makes no large-range cache call; the hang-study build and rig are in section 13. | HW; PPSSPP |

## 1. Duplicated cores vs context swap

**gpSP's state is globals at fixed addresses.**
- The MIPS emitter bakes absolute addresses of `iwram`, `ewram`, `vram`, `oam_ram`, `palette_ram` and `io_registers` into its memory stubs.
- The stubs live at the start of each instance's ROM translation cache.
- C code uses the arrays directly.

### Context swap (one core, state copied in and out)

- **What a switch must move.** The guest RAM with its SMC tag halves, plus VRAM, I/O, palette, OAM, the read map and the scattered C globals: about 0.75 MB each way, so ≥ 1.4 MB per switch.
- **Translation caches cannot be copied.** They are megabytes. They would have to be per instance with pointer swaps, and their stubs still embed one instance's addresses.
- **The alternative refactor.** Making every array a pointer and every global a struct field would touch every hot path of a core the owner's rules say to keep stable. It would also add a load to every C memory access in solo play.
- **How many switches the cable needs (host, AW2 link session, section 4).**
  - exact for this session: 62 switches a frame (lookahead 10000) to 410 (lookahead 2464);
  - one switch per frame (lookahead = a frame) already changes the game (section 4).
- **What that would cost.** At 62 switches a frame, a swap moves ~87 MB per frame.

**Verdict: impossible**, by two orders of magnitude.

### Duplicated cores (chosen)

**How it is built.**
- The core objects are partially linked into one relocatable.
- A second copy has every global it defines renamed `gbab_*` (`tools/gbamirror/Makefile`; the PSP rule will mirror psp/Makefile's TGB Dual rule).
- Undefined references (libc, the cable bus) stay shared.

**Why it works.**
- Each instance's emitter writes its own instance's addresses, so the "absolute addresses in emitted code" risk in the feasibility doc dissolves: each copy of the emitter knows only its own globals.
- Instance A is the core exactly as it ships, so solo play is the same code.

**What it costs.**
- Memory: section 3.
- I-cache: two copies of the hot C paths compete for 16 KiB. docs/CACHE-MAP.md shows the GB cores' cost there is capacity, not conflicts.

**Owner's note on sharing translated code.** It is not shareable as built, because emitted code embeds per-instance stub addresses. It would need base-register-relative stubs, so it is considered only if the HW numbers justify it.

## 2. Stopping and resuming a core

**The mechanism.**
- A core can only be stopped at an `update_gba` call.
- `gbalink_entry_yield` postpones the whole call: the dynarec returns to the stepper (bit 31, as a frame end does).
- Resuming makes exactly that call with exactly that argument, then re-enters translated code at REG_PC.
- The halt loop has the same check between iterations; resuming calls `update_gba(0)`, which is the next iteration.

**Invisible: measured.** `tools/gbamirror/yieldtest.sh` runs slot 0 alone through the link loop, stopped every C cycles. Against never stopping, AW2 for 6000 frames is IDENTICAL for C = 37, 300, 1000 and 5000.

**An extra event is not invisible: measured.**
- The first version also made the run horizon a scheduler event, so a core would stop within a block of it. That diverged at frame 964 (C = 5000).
- The mechanism: `update_gba` clamps an overshoot past -64 cycles, so where the calls fall decides how many cycles are dropped.
- So the horizon is not an event. A core stops at its first own event at or after it, which is at most ~960 cycles later (the H-blank edges).
- Cable events (transfer ends, the other port's actions) are events, as gpSP's solo serial already makes them.

## 3. Memory census

### Instance B

From `psp-size` of the 3.1 core archive. The JIT tier is listed separately.

| part | bytes |
|---|---:|
| text (the whole core: dynarec 551 K, renderer 184 K, interpreter 79 K, colour LUT 66 K, memory 66 K, ...) | 1,072,821 |
| data | 70,708 |
| bss without the static small JIT (guest RAM + tags 0.7 M, ROM branch hash 256 K, sound ring 256 K, save RAM 128 K, sprite lists 100 K, ...) | 1,560,792 |
| **B without a JIT tier** | **2.58 MiB** |
| JIT, small tier (2 MiB ROM + 384 KiB RAM) | 2.38 MiB |
| JIT, large tier (10 MiB + 512 KiB) | 10.50 MiB |

- Trimmable later, with a measured reason: the renderer, LUT and sprite lists (B is headless), rfu/cheats/vfs. That is about 0.5 MiB.
- **JIT tiers do not affect what the guest computes** (feasibility arms `jit_small`, `ballast`). Each console can therefore give each instance the tier it can afford, independently of the partner.

### The pool each console has for ROM plus B

The pool is what is left after instance A's load; ROM blocks are 1 MiB.
- **PSP-1000, 32 MiB layout:** a 16 MiB cart gets 13 blocks plus 1.9 MiB free. The pool is **≈ 14.9 MiB**, including the 1 MiB post-load floor.
- **64 MiB (Go, the tightest), A on the large tier:**
  - 31.4 MiB at the probe plus the 2 MiB lent static array, so **≈ 33.4 MiB** including the floor;
  - the 3000 is not measured; PPSSPP says 5 MiB more.

### Sessions

Usable means the pool minus the 1 MiB floor.

| console | B form | ROM space in a session | same game (one shared ROM) | two ROMs |
|---|---|---:|---|---|
| PSP-1000 | B + small JIT (4.96 MiB) | 8.9 MiB | AW2 8 MiB **resident**; 16 MiB carts 8 of 16 blocks (shared paging) | GS1+GS2 24 MiB: 37 % resident; R/S + FR/LG/E 32 MiB: 28 %. Paging both carts, speed unmeasured |
| Go/3000 | B + small JIT | 27.4 MiB | 32 MiB carts 27 of 32 (mild paging); ≤16 MiB resident | GS1+GS2 24 MiB resident; MMBN pairs 16 MiB resident; R/S + FR 32 MiB 86 % |
| Go/3000 | B + small, A dropped to small for the session (+8.1 MiB) | 35.5 MiB | all resident | R/S + FR 32 MiB **resident** |
| Go/3000 | B + large JIT (13.1 MiB) | 19.3 MiB | ≤16 MiB resident | MMBN, ≤ 19 MiB pairs |

### Statically linked B

- **Cost.** It costs every game in solo play 4.96 MiB of pool, permanently.
  - On the 1000 a 16 MiB cart drops from 13 to 8 resident blocks.
  - On a Go a 32 MiB cart stops being resident.
- **Use.** That is acceptable for a link-only bench build (`GBA_LINK=1`), which is how milestones 3-4 run. It is **not** shippable.

### Shipping form

B is a relocatable loaded at session start into the ROM pool and released at the end: an in-process "overlay".
- **Mechanism.** A ~200-line MIPS ELF relocator for R_MIPS_32/26/HI16/LO16 (`-G0`, no PIC). Its undefined references resolve against a generated table of the EBOOT symbols B uses: `psp-nm` of the 3.1 core lists 58, all libc plus the 4 cache syscalls.
- **File.** The image is a file beside the EBOOT, like the ME PRX.
- **Why not `sceKernelLoadModule`?** It needs partition memory outside newlib's heap, and the heap takes all but 1 MiB.
- **Gate.** The overlay changes no emitted code, so the bench build's numbers and oracle carry over. The gate is the PPSSPP oracle with static B vs loaded B, which must be identical.

### Shared ROM (owner, 2026-10-01): one buffer for same-game sessions

- **Resident case.** B's read map points at A's blocks.
- **Paged case.** B's page faults call A's loader, which owns the one LRU pool; A's eviction calls an unmap hook that clears the page in B's map too.
- **Translations survive eviction in both.** They are keyed by PC, and a reloaded page has the same bytes.
- **The GPIO catch.** `update_gpio_romregs` writes the RTC/rumble registers into ROM page 0 (0xC4-0xC9 of the page the read map points at). B therefore gets a **private copy of page 0** (32 KiB), mapped in all three wait-state windows, so each core sees only its own clock port.
  - Page 0's code bytes are identical.
  - 0xC4-0xC9 is header data, never executed, so translations do not care.
- **As built (gba_memory.c, `GBA_LINK` only), no refactor of `load_gamepak` was needed.**
  - A exports a `gbalink_rom_share`: its read map, a page function and the cartridge facts.
  - B, with `gamepak_share_in` set before its `retro_init`, allocates no pool. Its `load_gamepak` adopts A's pages instead of reading the file and then runs the usual cartridge setup on its private page 0.
  - B takes A's backup-type detection. A scanned the blocks in file order at load, which B cannot see once A has paged.
  - A's `evict_gamepak_page` calls B's `gamepak_share_unmap`.
- **Measured** (AW2, 9000 frames × 2 machines), every arm hash-identical to two private copies:

  | host arm | A's pool | page faults A / B |
  |---|---:|---:|
  | resident | 32 MiB | 1 / 1 |
  | paged | 2 MiB | 131 / 129 |
  | paged, instances swapped | 2 MiB | 131 / 129 |
  | heavily paged | 1 MiB | 1,092 / 998 |

  PPSSPP (PSP build): shared vs own copy, identical.
- **Cross-console rule this exposes.** A paged cart's backup detection scans only the resident blocks, so two consoles with different pools could in principle detect different backup types for the same game. The session should carry the host's detected type. This is pre-existing and affects only carts whose signature is past the first resident blocks.

### Two ROMs (owner, 2026-10-01)

The cross-ROM pairs:
- R/S ↔ FR/LG/E,
- R ↔ S,
- GS1 → GS2,
- MMBN versions.

**How they work.** Each instance has its own pool.
- A's pool shrinks at session start (free blocks, unmap their pages).
- B's pool is capped before B's `retro_init`, so B never takes "blocks until malloc fails".

**The partner's cart.**
- It comes from the local library by SHA-1, or over the GB link's bulk lane (docs/GB-LINK.md §3: 8 MiB in 25-32 s on HW).
- A received cart that does not fit in memory cannot be paged from memory. On the 1000 every 16 MiB partner cart is in that position.
- **Owner decision needed:** either require both carts in the 1000's library, or allow a session-only temp file on the stick (GB link's rule is "never written to storage").

### Routing (owner, 2026-10-01)

**Why the mirror needs no routing.** On the mirror path the game sees a real cable and no wireless adapter, so the game itself picks its cable Cable Club. FR/LG/E included: their cable trade with R/S is how gen 3 cross-version trading works.

**What the session/UI must do.**
- Offer "Link cable" for any GBA game, separately from Wireless (RFU).
- RFU's auto-routing only chooses the default.
- `SERIAL_MODE_CABLE` is set only by the link session (`gbalink_core.c` `begin`), and nothing in `rfu.c` or the RFU routing changes.

## 4. The desktop prototype (milestone 2: done)

`tools/gbamirror/` runs on Linux/WSL against the unix core (x86 dynarec):
- `gl_host` links instance A and the renamed instance B;
- `aw2link.sh` runs the session;
- `yieldtest.sh` is the invisibility test;
- `contact.py` makes the screen sheets.

**The session.**
- Inputs: the owner's AW2 save and state on both machines; slot 0 driven by `aw2_link_host.txt`, slot 1 by `aw2_link_join.txt`. These are the design-P probe's blind scripts, unchanged.
- Hashes: the shash fields of both machines, every frame.
- The sequence, seen on both screens:
  - soft reset, title,
  - Link > Multi-Pak,
  - the lobby with 1P and 2P,
  - Teams > Rules,
  - a 2P battle: Day 1 → both players end their turn → Day 2 (funds 11,000 → 22,000).
- It finishes at frame 9000.
- Transfers: 11,977 multi-player transfers completed on both ports; never blocked; 0 bus overflow.

**Determinism**, hash files compared over all 18,000 frame lines:

| arm | switches/frame | result vs lookahead 2464 |
|---|---:|---|
| rerun | 410 | IDENTICAL |
| `--swap`: slot 0 on instance B, slot 1 on A | 410 | IDENTICAL |
| lookahead 5000 | 131 | IDENTICAL |
| lookahead 10000 | 62 | IDENTICAL |
| lookahead 280896 (a frame) | 5 | differs from f=2512 |

The last row is the design working as stated. The parent's own actions reach the child exactly, which is why 2464-10000 agree. A child-initiated event, its mode or pin change, reaches the parent up to `lookahead` late. A frame late was enough to change AW2's lobby timing. That is still deterministic on both consoles, but it is a different game from the exact one, so batching must stay below the point where child events matter (section 6).

**Cable lateness counters** (lookahead 2464):

| direction | late events | max lateness |
|---|---:|---:|
| parent → child | 5,473 of 11,981 | 64 cycles |
| child → parent | 3 of 4 | 2,423 cycles |

The 64-cycle figure is block overshoot.

**A whole battle.**
- **The scripts.** The AW baseline's RAM-gated scripts (`claude/gbalink-baseline` 8aa913a, `tools/awlink/scripts/aw2_battle_{host,join}.txt`): the same lobby, then 10 rounds, each turn a cursor tour and End, gated on AW2's HUD.
- **The engines.** The scripts drive both machines through two copies of the real autopilot engine. `gl_host --ap0/--ap1`: slot 1's copy is `fe_autopilot.o` with its API renamed and its RAM/input hooks on instance B.
- **Result.**
  - Both autopilots DONE: frame 14,485 (host) and 15,013 (join).
  - The day counter is logged 1→10 by both machines.
  - 109,729 multi-player transfers.
  - 35 s of wall time for 60,000 frames.
- **Determinism.** Rerun, instance swap, lookahead 10000 with own ROM, and shared ROM paged at 2 MiB are all hash-identical over 31,999 frame lines.

## 4b. The PSP build in PPSSPP (milestone 3)

**Build.** `GBA_LINK=1 tools/build.sh harness64` (or `harness`):
- B is the whole core archive partially linked with 603 globals renamed;
- the ELF grows by text +1.09 MB and bss +4.14 MB, as the census predicted;
- `tools/build.sh` refuses `GBA_LINK=1` for a release.

**How the bench runs.**
- Harness keys: `gba_dual = 1` (the pair), `gba_dual_script` (the partner's blind script), `gba_dual_mirror`, `gba_dual_lookahead`, `gba_dual_own_rom`, `gba_dual_hash = 1` (log/gbadual_hash.txt, both machines every frame).
- Slot 0 is the player's machine on the ME renderer, driven by the autopilot. Slot 1 is the headless partner.
- Every state load the player's machine makes restarts the pair from that state.

**Results.** `tools/gbamirror/bench_ppsspp.sh`, AW2 link session, 9100 frames. The default PPSSPP model is the PSP-1000's 32 MiB.

| arm | exit | core_prof (us/frame, last windows) | pair |
|---|---|---|---|
| solo (no partner) | 0 | 6,057-6,075 mean | - |
| dual, lookahead 2464 | 0 | 13,124-14,595 mean | 412 switches/frame, 0 blocked, 9,618 multi-player transfers |
| dual, rerun | 0 | identical | hash-identical |
| dual, lookahead 10000 | 0 | 12,721-13,875 mean | 62 switches/frame, 6,320 blocked parent runs; hash-identical |
| dual, partner with its own ROM copy | 0 | 13,134-14,506 | hash-identical |
| dual, the `harness` (32 MiB layout) EBOOT | 0 | same | hash-identical |

- PPSSPP times are not a timing source; the ratio is a hint only.
- The screens (autopilot dumps of slot 0) show the same Link > Multi-Pak > battle sequence with the DAY banners at each turn change.

**Memory, PSP-1000 model, B static.**
- the heap before the ROM browser is 9.96 MB;
- A takes the small tier ("heap_short");
- AW2 8 blocks resident, shared with B;
- the post-load floor is 1.16 MB.

## 4c. Cold start, an upstream uninitialised read, and two SMC-heavy cores

**Cold start (commit 233e2fc).**
- **The problem.** A state load keeps the dynarec's learned SMC translation gates and ranked candidates (savestate.c), and gates move block boundaries and charge cycles. So a machine's long-played copy (instance A on its own console) and its fresh copy (instance B on the partner's) could diverge in SMC-heavy games.
- **The fix.** `begin()` now restores the cartridge's configured gate set (a snapshot taken in `load_gamepak`), forgets the candidates and flushes the translations: the feasibility doc's cold-start rule.

**The uninitialised read.** Found while checking that the cold start's flush is guest-invisible: on the host it was not.
- **The mechanism.**
  - `retro_load_game` calls `check_variables`, which enables the dynarec before the BIOS is mapped.
  - `init_bios_hooks` then translates "BIOS 0x8" from `load_gamepak_page(0)`, i.e. ROM block 0 straight from `malloc`.
- **valgrind** (memcheck, `--smc-check=all`, in a debian container): that is the ONLY uninitialised read in a 40-frame link session (23 contexts, all under `init_bios_hooks`).
- **Its effect.**
  - The desktop battle's frame-1 IWRAM hash depended on unrelated heap use: the size of a script buffer, or whether a log file was open.
  - `MALLOC_PERTURB_` 1-3 crashed the unmodified instance A in exactly that translation.
- **The fix.** `GBA_LINK` builds zero block 0 in `init_gamepak_buffer`. Every heap variant and `MALLOC_PERTURB_` 1/2/85 are then hash-identical, and the cold start is invisible.
- **Release is untouched.**
  - This is upstream gpSP behaviour.
  - In solo play its only reach is how the SWI entry is first translated, before a reset regenerates it. Yet its content did reach guest state here, by a path not yet pinned down.
  - **Owner decision:** whether to zero block 0 in release too. It costs one 1 MiB `memset` at boot. It is a candidate cause for layout-sensitive oddities, untested.
- **Desktop battle after both fixes:** rerun, swap, lookahead 10000, shared ROM paged at 2 MiB and at 1 MiB (3,840/3,369 page faults) are IDENTICAL over 31,999 lines.

**Two SMC-heavy cores** (PPSSPP, build before the two fixes).
- **The run.** Heart & Soul `heart_soul_heavy` + `battle.txt`. The partner gets the player's buttons (`gba_dual_mirror`). The ROM is shared and paged: 32 MiB cart, 15-block pool, 117/113 page faults over 2,970 frames.
- **Results.**
  - exit 0;
  - rerun and the `rom_cap 15` arm hash-identical;
  - the two machines identical in IWRAM, EWRAM, palette, OAM and VRAM on all 3,368 frames compared.
  - I/O differs on every frame by design (SIOCNT's SI bit: parent vs child); registers differ on 6 frames.
- **Cost.** core_prof is 16.35 ms dual vs 7.38 solo; PPSSPP, a hint only.

## 5. SIO model (`gbalink_core.c`)

The cable is an AGB-005 Game Link Cable, slot 0 at the parent end. Per GBATEK:

**Multi-player.**
- The parent (SI grounded) starts. At the start SIOMULTI0-3 is set to FFFF on both ports and the child's busy bit is set.
- At the end both ports get [parent, child, FFFF, FFFF], busy clears, the ID bits are set (0/1) and the IRQ fires if enabled.
- SD (bit 3) means both ports are in multi-player mode.
- RCNT 0-3 mirror SC/SD/SI/SO; games test SC.
- Time per transfer: gpSP's per-baud figure × 2 players.

**Normal mode is one-way on this cable.**
- The parent's SI is grounded, so the parent always receives 0.
- The child receives the parent's bits when it is armed (start set, external clock), whichever end supplies the clock.
- SO follows SIOCNT bit 3 only while start = 0 (GBATEK's latch).
- SIOCNT bit 2 at the child is the parent's SO.

**General purpose.**
- The four pins are wired as above; an input pin is not driven, so it reads its pull-up.
- SI falling edge raises the IRQ.

**Not modelled.**
- UART: no two-GBA game known.
- JOY BUS: GameCube.
- Multiboot to a cartless partner: it needs the real BIOS and a cartless boot of instance B, and is a later milestone.

**Timing.**
- Events carry the session cycle they happened at.
- An I/O write's cycle comes from the store stub: one `sw` of the cycle counter to `reg[62]` before the C handler, in GBA_LINK builds only, MIPS and x86.

## 6. Stepping, batching, and the CPU projection

**The rule (gbalink_pair.c).**
- The child runs to (parent.now − one scanline).
- The parent runs to (child.now + lookahead).
- When the parent needs the child's word, it is BLOCKED until the child has passed the transfer's start.

**Switch count is the GBA analogue of the GB link's scanline batching.**
- Measured switches: 410 / 131 / 62 / 5 per frame at lookahead 2464 / 5000 / 10000 / 280896.
- The exact-for-AW2 range ends somewhere between 10000 and a frame.
- The default will be chosen on HW cost, with the hash oracle as the gate.
- An adaptive rule (large lookahead only while neither port is in a linkable mode) is the next step if the HW cost of 62 switches is material.

**CPU projection per model (est.).**
- From the feasibility doc's Go HW numbers for AW2: solo 5.92 ms mean with the ME.
- Two cores at the GB-measured 2.0-2.3× gives 11.8-13.6 ms mean, with ~6 % of frames over 16.7 ms (menus, minimap, scene loads).
- The PSP-1000 and 3000 run the same 333 MHz Allegrex. The 1000's solo AW2 run is still pending in the feasibility rig, so the same projection applies until it reports.
- DYNAREC-PROFILE's `DISPATCH_CACHE` (−22.9 % of AW2 core instructions, oracle-identical) would remove ~1.3 ms per core, ~2.6 ms per pair. Bench-only, off by default.
- **Not yet in the estimate:** the switch cost (the core's exit and re-entry, plus I-cache refill).

## 7. The netplay layer

**What `fe_gblink` (docs/GB-LINK.md §2, §8-12) already has, proven on HW (hw5 24/24):**
- the protocol: HELLO, CONFIG, NEED/ROM, SAVE, STATE, READY, INPUT, HASH, END/FINAL, ABORT;
- the bulk lane;
- the 30 s no-peer rule;
- the scan-then-join radio bring-up;
- the end-frame agreement.

**Where it touches the machines.** About ten call sites: `gbdual_create/advance/sync_hash`, `gbdual_api()->state_*`/`save_ram_*`, and GB header fields.

**Plan.**
- Extract a machine-pair vtable from fe_gblink: create from cart + save + state, advance with an input callback, sync hash, battery image, state capture, cart identity.
- Implement it for GB with `gbdual` and for GBA with `gbalink_pair`.
- **Gate:** the GB host suite (sesssim 18/18, the trades and battle) must be bit-identical before and after, then one GB HW rig run.
- **Why not a fork?** Two copies of a 2,000-line protocol would mean every hardware lesson twice.

**For the first PSP bench (milestone 3-4)** the rig stages identical ROM, save and state on both consoles, so no transfer is needed. The bench uses HELLO (identity: EBOOT CRC, ROM SHA-1, state CRC), CONFIG, INPUT, HASH and END.

That was the plan. The HELLO that shipped (`fe_gblink.c`, `send_hello`) carries the protocol number, role, console, palette, ROM size, ROM hash, header, save and state sizes, and no build identity: two builds with the same `FE_GBLINK_PROTO` would start a session and fall out of step at the first HASH. Since 4.6.0 netdrv refuses them before HELLO (GBA-LINK-UI.md, Admission check).

**GBA specifics the vtable must carry** (found while building milestone 3):
- **The link is always live.** The player's machine is the running core (instance A), and a 32 MiB cart is never read into memory for hashing. So the own-ROM path is SHA-1 by streaming the file, and the partner's machine is B, started from the partner's STATE (a GBA state is ~0.4 MB; the bulk lane moves it in ~2 s).
- **Both consoles must start both machines from states.** That includes A reloading its own just-serialized state. They must also reset both instances' SMC gates (`smc_gates_reset`) and flush the dynarec, per the feasibility cold-start rules.
- **The cartridge clock.** The GBA RTC is `rtc_base_time + frame_counter` from the state. Both instances get the session's wall-clock hook (`gpsp_wallclock`), so a state with an implausible base cannot re-seed from each console's own clock.

## 8. Coordination and schedule

**Baseline agent (`claude/gbalink-baseline`).** `docs/GBA-LINK-BASELINE.md` did not exist yet at 2026-10-01 10:30. This branch needs from it:
- the AW-fake frame time on HW,
- stall counts,
- the turn-exchange latency, same scenes.

Both agents want the 1000 + 3000 pair.
- **10:59.** `GBA-LINK-BASELINE.md` still did not exist, but `claude/gbalink-baseline` has `tools/awlink` with RAM-gated battle scripts (`aw2_battle_{host,join}.txt`: 10 rounds, a cursor tour and End each turn, gated on the HUD CO tilemap 0x0201D49C). The mirror's pair rig should drive the SAME scripts so the two numbers compare scene for scene. That needs RAM predicates on the partner machine too, which `gbalink_script` does not have yet.
- **The 3000's dynarec chain finished at 10:54** ("plan complete").
- This branch's next hardware need is **single-console**: section 10, one console at a time, no radio. It can use whichever console is free; the pair rig waits for the session layer.

**Rules kept.**
- `rfu.c` and the RFU routing: untouched.
- `serial_proto.c`: untouched.
- The handoff harness: untouched.
- Every change is under `GBA_LINK`. A release build defines none of it; `GBA_LINK` is a ROOT-make flag, like the dynarec flags.

## 9. Open risks

1. **The HW cost of switches and two hot paths in a 16 KiB I-cache.** It decides the default lookahead and whether AW2 holds 60 fps on a 1000.
2. **Long-session cross-console determinism.** Proven only for 100 s on a Go (feasibility) and 9000 frames on the host here. The HASH message catches a desync; it cannot prevent one.
3. **Child-initiated cable events arrive up to `lookahead` late.** That is exact for multi-player games and approximate for normal or general-purpose protocols driven by the child.
4. **The overlay loader** is new code in the EBOOT's address space. It must be gated by an identical-oracle run against static B.
5. **Cross-ROM on the PSP-1000** needs paging two carts at once, with an owner decision on the partner cart's storage.
6. **Gates.** `smc_gates_reset()` must run at session start on both instances, per the feasibility cold-start rules.

## 10. Hardware bench, single console (milestone 4a: staged)

**The rig.** `tools/gbamirror/mirrig.py` is ONE rig app, `GBADHOC-GBAMIRROR` ("GBA MIRROR RIG"), driven by the USB handoff (drrig/resrig pattern; `harness-kit/hw_loop.py` unchanged). It is one EBOOT: the harness64 `GBA_LINK` build of commit b22a499, `EBOOT crc32 08af569c`.

**The arms differ only in harness keys.** Each cycle is the AW2 link session above, both machines on this console:

| arm | what |
|---|---|
| M-DUAL | the pair, lookahead 2464 |
| M-SOLO | the player's machine alone, to the lobby: one core's cost of the same scenes |
| M-DUAL10K | lookahead 10000 |
| M-OWN | the partner with its own ROM copy; 64 MiB plans only |
| M-BATTLE | both machines on the baseline's RAM-gated 10-round battle: the player's machine through the harness autopilot, the partner through its own copy of the engine (`fe_autopilot_b.o`, harness key `gba_dual_autopilot`) |

**Plans.**
- `p1000`: BATTLE, DUAL, SOLO, DUAL10K, then reversed (8 cycles).
- `p64`: adds M-OWN (10 cycles).
- Timing: about 3-5 min a cycle.
- Stages: `builds/gbalink-mirror-out/rig3-{p1000,p64}`, EBOOT crc32 69cb4adc (commit 233e2fc, clean build, with the cold start and the block-0 fix).
- Owner steps: `builds/gbalink-mirror-out/OWNER-STEPS.txt`.

**Proven in PPSSPP before staging.**
- **The whole handoff loop** (`mirrig_dry.sh`): install, one launch, every collection, golden restore, ini swap and relaunch.
  - rig1, an earlier EBOOT, same rig code: all 6 cycles.
  - rig3: cycle 1 (M-BATTLE) collected, then the relaunch into cycle 2. PPSSPP then died at frame 7,341 (the known PPSSPP launch wear: docs memory "PPSSPP rig wears out"), so the run was stopped there.
  - The collected rig3 M-BATTLE cycle matches the golden: `shash` 15,319 lines and `gbadual_hash` 30,567 lines IDENTICAL. Both autopilots DONE, day lines 20, 23,409 transfers.
- **The report** (`mir_report.py`): the dry run's logs compare IDENTICAL to the golden on both oracles (the player's `shash` and both machines' `gbadual_hash`).
- **Costs in the dry run** (link window, PPSSPP, so a hint only):

  | arm | mean | p95 |
  |---|---:|---:|
  | DUAL | 13.52 ms | 15.00 ms |
  | DUAL10K | 13.06 ms | 14.13 ms |

**What it answers.**
- the two cores' real cost per model: `shash.txt` `c=` per frame, split pre-link / link;
- the switch cost (DUAL vs DUAL10K);
- **cross-console determinism of BOTH machines**: `gbadual_hash.txt` vs the PPSSPP golden made from the exact staged files (`golden_ppsspp.sh`);
- the 1000's memory with B static.

**Report:**

    python tools/gbamirror/mir_report.py --rig LOGS --golden GOLD

**Not answered here.** Radio, stalls and input delay need the pair rig. That waits for the session layer (section 7).

## 11. Next steps (in order)

1. **Single-console HW bench** (section 10, staged). It gives:
   - the real cost of two cores per model,
   - the switch cost (DUAL vs DUAL10K),
   - cross-console determinism of both machines vs PPSSPP.
   The result chooses the default lookahead and tells whether a PSP-1000 holds 60 fps.
2. **The session layer** (section 7):
   - generalise `fe_gblink` with a machine-pair vtable, gated by the GB host suite bit-identical, then one GB HW run;
   - GBA: live link only, own-ROM SHA-1 by streaming, the partner's STATE over the bulk lane, both machines cold-started from states;
   - a "Link cable" choice for every GBA game, separate from Wireless (RFU).
   Then the two-PPSSPP ad-hoc pair (`run_rig_ppsspp.sh` pattern), then the 1000 + 3000 pair rig against the AW baseline: same battle scripts, same scorer measures.
3. **Shipping memory:** load instance B as an in-process overlay at session start (section 3), gated by static-B vs overlay oracle identity.
4. **Two ROMs** (cross-ROM pairs), with an owner decision on the partner cart's storage on the PSP-1000.
5. **Optimisations, each behind its own switch, OFF by default, oracle-gated:**
   - adaptive lookahead;
   - headless audio skip for the partner (sound is 7-24 % of a core);
   - DYNAREC-PROFILE's DISPATCH_CACHE / SMC_RETIRE_WINDOW;
   - trimming B (renderer, colour LUT).
6. **Cartless multiboot** (single-pak games): instance B boots the real BIOS without a cart and receives over the emulated SIO.

## 12. PSP-3000 results (2026-10-01, single console, no owner launch)

**How it ran.** The 3000 sat parked in another rig's handoff loop (`GBADHOC-DRPROF`).
- `tools/gbamirror/mirhand.py` took it over through freezerig's window-aware loop: verified backup (`backup_sticks.py` plus a verified copy of the folder, in `builds/mirror-backup-2026-10-01-3000`), install into that folder inside an open window, then CMD=RUN.
- The EBOOT was 69cb4adc (commit 233e2fc), harness64 `GBA_LINK`, ME renderer on, `jit_cache` large, AW2 resident and shared.
- Session 1 ran arms 1-5 with `gba_dual_hash` on. Session 2 ran the lookahead sweep, mostly with the per-frame hash off.
- Logs: `builds/gbalink-mirror-out/logs-3000`, `logs-3000c`, `layout-3000-all`; report: `REPORT-3000.txt`.

**Frame cost.** Core microseconds per frame (shash `c=`), averaged over the link window, frames 1500+.

| arm | lookahead | switches/frame | per-frame hash | mean | p95 | over 16.7 ms |
|---|---:|---:|---|---:|---:|---:|
| M-SOLO (one core) | - | - | - | **5.71** | 6.04 | 0 % |
| M-DUAL | 2464 | 411 | on | 31.69 | 35.37 | 100 % |
| M-OWN (own ROM copy) | 2464 | 411 | on | 31.76 | 35.44 | 100 % |
| M-DUAL10K | 10000 | 62 | on | 26.51 | 30.30 | 100 % |
| M-BATTLE (10 rounds) | 2464 | 409 | on | 31.74 | 35.19 | 100 % |
| M-BATTLE140K | 140448 | 7 | on | 24.88 | 28.58 | 100 % |
| M-DUAL-NH | 2464 | 411 | off | 21.31 | 22.69 | 100 % |
| M-DUAL50K-NH | 50000 | 13 | off | 14.70 | 16.44 | 2.1 % |
| **M-DUAL140K-NH** | **140448** | **7** | **off** | **14.20** | **15.99** | **2.0 %** |

**What the numbers mean.**
- **The per-frame two-machine hash costs ~10.4 ms** (31.7 vs 21.3). It is instrumentation (`gba_dual_hash`, ~0.8 MB hashed inside the timed frame), and no player build does it.
- **Switches cost ~17 us each** (21.3 vs 14.2 ms for 404 fewer a frame): a cold I-cache and the dynarec's exit and entry.
- **Half a frame is now the default** (`GBALINK_DEFAULT_LOOKAHEAD`, commit de6a039). It is hash-identical to 2464 on the desktop, in PPSSPP and on this console.
- **Two cores cost 2.49× one** (14.2 vs 5.7). The feasibility projection was 2.0-2.3×.
- **Headroom.** The menus and title before the lobby (pre-link window) cost 12.0 ms. In the battle, 2 % of frames run over budget, so AW2 linked on a 3000 is just inside 60 fps.
- Measurement noise: two runs of the same arm, measured on different days, are not available yet; every arm ran once.

**Compared with the AW fake.** The fake runs one core, so its cost is M-SOLO's 5.7 ms plus netplay. The mirror pays 8.5 ms a frame more for generality and still holds 60 fps on the 3000. The rest of the comparison (stalls, turn latency over the radio) needs the pair rig.

**Determinism, hardware vs PPSSPP.** Every completed run is IDENTICAL to the PPSSPP golden of its arm:
- `shash` (the player's machine) in all 9 runs;
- `gbadual_hash` (both machines, every frame) in all 5 hashed runs: the battle 30,567 lines (twice) and the link session 16,419 lines (three times).
- The `-NH` arms are compared on `shash` only.

**The hang (session 2, run 5, M-BATTLE140K-NH).**
- CMD=RUN went in at 15:13:03 and the eject took. There was no RESULT.TXT and no USB window for 2 h; a live run hands off within ~7 min.
- The same arm with the hash (run 1) and every other arm completed. Nobody saw the screen, so "frozen", "hung in a loop" and "never relaunched" cannot be told apart.
- The lead stopped the loop. Recovery that keeps the evidence (the mirror EBOOT has no `freeze_rig_recover`, so relaunching first would overwrite the logs): `builds/gbalink-mirror-out/FREEZE-3000-RECOVERY.txt`.
- **Before any further hardware use, read the frozen run's `heartbeat.txt`** for the last frame. Then rerun M-BATTLE140K-NH several times, ABBA against M-BATTLE140K. This sits in the same family as the open ME / I-cache freezes in memory ("freeze reproduced: battlecycle", "resident-icache fix"); two cores double the code that runs between I-cache syncs.
- The 3000 is NOT parked as it was found. The card holds the mirror rig in `GBADHOC-DRPROF` with run 5 staged, and it needs a power cycle.

## 13. The PSP-3000 hang: what can be ruled out without the console (2026-10-01)

The 3000's logs from the hung run are still on its card, so the hang is not diagnosed. This section records what was tested without the console, and the build and rig that will diagnose it next time.

**Labels.** HW = the PSP-3000 sessions of section 12. PPSSPP and the desktop host as in section 4.

### 13.1 What the hang is not

| hypothesis | test | result |
|---|---|---|
| A software hang in the arm itself: the stepper, the cable, either core | **The exact arm in PPSSPP.** M-BATTLE140K-NH from the staged files (EBOOT 69cb4adc), on both the 64 MiB and the 32 MiB model | Both exit 0 at frame 15,318. Both autopilots DONE. `shash` is identical to the golden and to the 3000's own M-BATTLE140K run (md5 `abfc8eae` over 15,318 lines) |
| The same, with the per-frame hash off | The hash only writes a file. On the 3000, M-BATTLE140K (hash on) completed and matched on every frame of both machines | The two arms compute the same guest. Emulation and stepping do not depend on wall time |
| A stepper livelock: both cores waiting, or a core that never advances | Read `gbalink_pair_step` against `gbalink_entry_yield` / `gbalink_halt_yield`: a chosen core is always below its horizon, and blocked waits are capped at 4. Desktop battle with the new guard (13.2): idle runs 0; at most 9 runs per step at lookahead 140448 (455 at 2464); at most 1 partner frame per step | No livelock path. In any case the stepper is deterministic, so a livelock would also have hung PPSSPP and the desktop |
| Ad-hoc ring overflow, lockstep between two consoles | - | Does not apply: the bench has no radio and no second console (section 10). The two-PPSSPP ad-hoc recipe has nothing to run until the session layer exists (section 7) |
| Heap exhaustion | 3000 boot: 13.3 MB free after the load. B is static; the shared ROM path allocates only page 0 (32 KiB) | No late allocation of any size |
| The firmware's large-range cache path (`docs/JIT-COHERENCY.md` section 4: I ≥ 16 KiB walks the tag RAM; D ≥ 64 KiB) | New counters in both cores, PPSSPP, the whole arm. The emitter is deterministic, so the calls PPSSPP sees are the calls the 3000 made | During the session: **0 ranges** at or over the thresholds in either core. Instance A: 40 RAM flushes and 3 ROM flushes. Instance B: 22 RAM flushes on its SMALL tier and 3 ROM flushes. The one large call is at boot (13.2, item 5) |
| The second core's code buffer breaking the sync rules | Read both instances' sync paths. B is the same object code renamed, with its own zones (the SMALL static arrays) and its own `last_*` pointers. Neither instance writes the other's code. The I-cache is shared, so one core's code evicts the other's lines; that only shortens how long a stale line can live | Under the true kernel semantics both are complete, as one core is (the checker's result). Two cores add no cross-instance writer |

**What is left.** Everything that differs between PPSSPP and a PSP:

1. **A hardware-only guest derail.** Evidence for it:
   - the 3000 still has the open freeze family (memory: battlecycle, resident I-cache);
   - the `-NH` arm no longer runs ~10 ms of hashing and fprintf a frame, which on HW flushed both 16 KiB caches every frame.

   Evidence against it: the only coherency mechanism with hardware evidence, the large-range path, is not exercised by this arm (table above).
2. **Never relaunched.** The handoff relaunch or the card failed after CMD=RUN, before run 5 started.
3. **A Media Engine wedge.** The ME is on in every arm, as in the battlecycle freeze.

The card decides between these. `log/heartbeat.txt` is appended by a priority-0x10 thread and never truncated:
- no line with `run_id=5-1790881981` means the run never started (case 2);
- `phase=7` on the last lines means it stopped inside `me_rend_frame` (case 3);
- `phase=2` with the frame frozen means it stopped in the core (case 1).

### 13.2 What changed (commit "gbalink: hang study")

All of it is under `GBA_LINK`, so a release build is untouched.

1. **Progress guard** (`gbalink_pair.c`). A run that returns HORIZON with neither clock moved counts as idle. More than 64 in a row stops the pair (`E_STUCK`). So does one step with more than 200,000 runs or more than 16 partner frames (`E_RUNAWAY`). The limits are 3 to 4 orders of magnitude above the measured values. The stop is deterministic, so both consoles of a future session stop at the same run. Desktop battle, 16,000 frames × 2 machines, at lookaheads 140448 and 2464: hash-identical to the earlier run (md5 `3c638774`).
2. **The partner's derail is no longer silent** (`fe_gbadual.c`). After a derail, BADJUMP_SAFE sets a flag and the frontend resets the machine. The frontend read only A's flag, so B's `gbab_badjump_reset_pending` was never read. A derailed partner therefore kept running from the reset vector without a reset: the "zombie" that once spun a console solid. Now either machine's derail logs `gbadual_guest_fault slot=N pc=...` and ends the session.
3. **A failed pair releases the player's machine** (`gbadual_stop`). Before, after a pair error the frontend fell back to `retro_run` with A still in cable mode. Every `update_gba` then postponed itself, so A never completed another frame. Now the pair is ended, both cores leave cable mode, and the player's machine continues alone.
4. **`cache_paranoid` reaches instance B** (`gbab_platform_cache_paranoid` = A's), at load and at every session start. Before, the harness key fixed only half the code.
5. **B's boot sync from address 0** (`cpu_threaded.c`).
   - The fault: B is forced onto the SMALL tier, so its first RAM publish handed the kernel `[0, ram_translation_ptr)`. Measured: one I invalidate and one D writeback of 151,144,832 bytes at boot. This is the upstream bug in `docs/JIT-COHERENCY.md` section 6.
   - The fix: the low end is now the cache's start.
   - It happens at boot of every run, clean ones included, so it is not the hang. Release keeps the upstream behaviour; the owner decides.
   - After the fix the staged EBOOT reports `big_i=0/0 big_d=0/0` for the whole arm.
6. **Telemetry.**
   - `EVT gbadual_health`: idle runs, step maxima, both machines' badjump / ROM / RAM flush counts, large-range kernel calls per core, and paranoid per core.
   - `heartbeat.txt` and `stall.txt` lines end with the pair's position: `pair=on run=<slot inside run(), -1 none> step_runs= f=A/B sw= bpc=<partner pc> bbj=`. A pair looping without frames shows `sw` moving while `f` stays put. A core stuck inside run() shows the same `run=` on every line.
7. **`freeze_rig_recover`** is ported from claude/freeze-rig 2d5b84d, so a relaunch after a freeze hands the evidence off before running anything. The mirror EBOOT lacked it, which is why the recovery note says not to launch the app. (`rig_preserve_log` already kept one generation of frontend.log and shash.txt, and heartbeat.txt is never truncated, so a relaunch of the old EBOOT loses less than that note feared. It still reruns the arm, so the note stands.)
8. **Rig.**
   - `mirrig.py`: the base ini now sets `stall_watch_s = 15` and `freeze_rig_recover = 1`; new arm `M-BATTLE140K-NH-PAR` (the hung arm + `cache_paranoid = 1`).
   - `mirhand.py`: new plan `hang`.
   - `mir_report.py`: prints the health line, every pair/guest-fault/recovery event, the stall record, and, for a run that did not exit cleanly, the heartbeat's last line.

**Verified in PPSSPP** (64 MiB; the dirty-tree build, then the staged EBOOT 23b41a8e from c7c83d8):
- M-BATTLE140K-NH and M-BATTLE140K-NH-PAR exit 0.
- `shash` is identical to the golden (`abfc8eae`).
- `paranoid=1/1` in the -PAR arm.
- The heartbeat carries the probe.
- A boot that finds an uncollected `frontend.log` exits 7 (`prev_run_uncollected`) and keeps it as `frontend.prev.log`.

### 13.3 Next on hardware: ONE rig app

`builds/gbalink-mirror-out/OWNER-STEPS-HANG.txt`; stage `hand-hang` (mirror stage `rig4-hang`, EBOOT crc32 23b41a8e, ME PRX 402c68b4).

1. **Evidence first.** Follow `FREEZE-3000-RECOVERY.txt` as written: photograph, power off and on, XMB USB, copy `log\*` and `handoff\*`. Read `heartbeat.txt`'s last lines (13.1).
2. **Then the hang rig** (`hand-hang`, plan `hang`, 4 ABBA rounds = 8 runs, ~45 min). It installs into the same `GBADHOC-DRPROF` folder, from the XMB USB connection (`--no-window`). The owner launches "GBA MIRROR RIG" once.
   - Arms: M-BATTLE140K-NH (the arm that hung, the control) and M-BATTLE140K-NH-PAR (one switch: every cache sync of both cores whole).
   - A freeze is handled by the loop: power-cycle, relaunch, and the app hands the evidence off at once.

**Reading the result:**
- NH freezes and PAR never does: a coherency hole. Next comes the twin checker on a two-core build.
- Both freeze: not coherency. Next is ME off, as one switch.
- `gbadual_guest_fault slot=1` in a log: the partner derailed, and the session now survives it.
- Neither freezes in 8 runs: the hang is rarer than 1 in 8. Run the plan again before reading anything into it.

**Caveat.** `cache_paranoid` adds whole-cache syncs. If PAR runs measurably slower in mir_report's cost lines, a clean PAR is partly a timing result too: only the -NH arms at 50K/140K lookahead ran under 16.7 ms a frame, and the hang was in one of them.

**Not done: the drprof twin.** Its checker (`tools/jitcoh`) models one contiguous `.jit` range and one host (`dr_host.c`). Running the mirror there means porting `gl_host` to the mipsel twin and giving the plugin two zone sets. The counters above answer its main question for this arm (no large-range calls), so the port waits for a hardware result that points at coherency.

## 14. The two-console session (2026-10-02, branch `claude/gbalink-session`)

Commits 76361ea (the layer) and dcface5 (the session clock). Section 7's plan, built: the GB link's session layer runs the GBA mirror, so the two consoles exchange only buttons and hashes over ad-hoc, in lockstep, and both compute the same two GBAs.

### 14.1 What was built

**One session layer for both systems.** `fe_gblink` (HELLO, CONFIG, STATE, SAVE, READY, INPUT, HASH, END_AT, FINAL, ABORT; bulk lane; early-HELLO replay; 30 s no-peer rule) now drives any machine pair through a vtable, `fe_gblink_machines` (fe_gblink.h). `NULL` is the GB/GBC pair, unchanged. `fe_gbadual_machines()` is the GBA mirror's.

| step | GB/GBC pair | GBA mirror (vtable) |
|---|---|---|
| own cartridge | read into memory, SHA-1 | SHA-1 by streaming; the running core keeps its own copy |
| partner cartridge | same / library / transfer | must be the same cartridge (SHA-1); `rom_missing` otherwise |
| HELLO header | 0x134..0x14F | 0xA0..0xBB (title, code, maker) |
| machines | power on from battery (+ live state) | both GBAs restarted from the two consoles' save states + battery images (cold start, `lk_begin`) |
| slot 0 | host's machine | host's machine, on both consoles; instance A is the local machine (slot 1 on the join), B the partner's |
| CONFIG `batch` | scanline batching | the pair's lookahead in scanlines (114 = half a frame) |
| per-frame step | `gbdual_advance` | `gbalink_pair_step` (STALL = a partner input not here yet; resumable) |
| HASH (every 60 slot-0 frames) | `gbdual_sync_hash` | both machines' combined guest hash (regs, IWRAM, EWRAM, I/O, palette, OAM, VRAM) |
| end | FINAL at the agreed slot-0 frame | final hash at slot 0's agreed frame on both; each console captures ITS machine at that machine's own frame boundary (a GBA core can only be serialized there) |
| failures | peer lost, desync, abort | + 20 s with no partner input, or no FINAL, ends the session (a frozen partner whose radio stays associated) |
| clock | the host's seed re-bases the battery clock | both cores read the session's clock (CONFIG's seed) for the session: a state whose RTC base is not a real date re-anchors identically on both consoles (B had no hook at all: uptime) |

**Frontend (all under `GBA_LINK`; release builds compile none of it).**
- `fe_host`: `gba_cable_start/frame/close`. The player's machine is never shut down: the session serializes it at a frame boundary, and a failed session restores that state and battery. The battery writer is held for the session, so only an agreed end reaches the stick (the GB rule).
- `main_psp`: harness `gblink_role = host|join` with `gba_dual = 1` on a GBA game is a cable session: the GB link's ad-hoc channel (`fe_np_start_gb`), none of RFU's session pacing, the loaded state held until the link forms, `gblink_hash_every`.
- Off by default: there is no menu entry yet (section 11, step 2's "Link cable" choice is still to do).

**Gate kept.** The GB host suite (`run_gb_tests.py`: sesssim protocol tests, bit-identity to 3.0.0, linkplay) is PASS and its output byte-identical before and after (54 lines, same final hashes, e.g. `af6d0ba1a6e5b9bf`).

### 14.2 Two PPSSPP consoles (ad-hoc, the AW2 battle)

`tools/gbamirror/run_session_ppsspp.sh` (two PPSSPP in one private namespace, Xvfb `:127`/`:128`, PPSSPP's ad-hoc server) and `score_session.py`. The host's machine runs `aw2_battle_host.txt` on the host console, the join's `aw2_battle_join.txt` on the join console: the baseline's RAM-gated 10-round battle (md5-identical to `claude/gbalink-baseline`'s rig scripts). Delay 4 frames.

| run | radio | per-frame hash | result | HASH msgs | final (frame 14625) | per-frame files | stalls host/join | core ms/frame | wire each way |
|---|---|---|---|---|---|---|---|---|---|
| r2 (build s2) | loopback | on | both DONE, exit 0 | 243/243 both | `a2eb2b47a9241ad3` both | identical, 29,241 lines (flush fixed after) | 5 / 8 | 13.3 | 2.4 KB/s, 120 datagrams/s |
| r3 (rig EBOOT 232dbad6, commit 76361ea) | loopback | on | **PASS** | 243/243 both | `a2eb2b47a9241ad3` both (= r2) | **identical, 29,249 lines, to f=14625** | 6 / 9 | 13.3 | 2.4 KB/s, 120/s |
| r4 (rig EBOOT) | 15 ms one-way + 5 ms jitter each side (srtt ~52 ms) | off | **PASS** | 243/243 both | `a2eb2b47a9241ad3` both (= r3) | (off) | 6 / 11 | **9.6** | 2.4 KB/s, 123/s, retx 56/28 |
| r5 (rig EBOOT) | 3 % of received datagrams dropped, both sides (hw1 measured ~3 %) | on | **PASS** | 243/243 both | `74cb7f725de11dcd` both (end agreed at f=14623) | **identical, 29,245 lines** | 732 / 734 (264 / 275 episodes, longest 20-23) | 13.2 | 2.3 KB/s, 115/s, retx 717/728 |

- **r6**, the build the hardware rig carries (EBOOT af98e599, dcface5), loopback with the per-frame hash: **PASS**, FINAL `a2eb2b47a9241ad3` (= r2/r3), per-frame files identical to f=14625.
- **The one-console bench is unchanged.** M-BATTLE140K on the session build is identical to the rig3 PPSSPP golden (EBOOT 69cb4adc): `gbadual_hash` 30,567 lines, and `shash` with its `c=` timing field removed.
- **The battle completes on the link.** The day counter reaches 10 on both machines (host f=13890, join f=14420); the host's script is DONE at f=14490 (the one-console bench: 14485), and the host ends the session 120 frames later; both commit and resume (`gblink_resume how=live_end`).
- **The same game whatever the radio does.** The periodic hashes of r2, r3 and r4 are equal frame for frame (e.g. f=2940 `be13c43bd2ca8896` in all three): latency and jitter only add stalls, never a different machine.
- **Loss costs stutter, never correctness.** At 3 % loss every lost INPUT waits for netdrv's retransmission (~3 stalls a second, up to 23 frames). The GB link measured the redundant-input datagram (`gblink_input_copies`) as a net loss on HW (hw2), so it stays off here too; the HW pair says how much loss the real radio has.
- **The per-frame two-machine hash** (`gba_dual_hash`, instrumentation only) costs ~3.7 ms a frame in PPSSPP (13.3 vs 9.6) and ~10 ms on a 3000 (section 12); the session's own HASH (every 60 frames) is in both numbers.
- PPSSPP is not a timing source (section 4b); the HW pair measures cost.

**Cost and bandwidth.** In PPSSPP the session's frame costs what the one-console bench's dual arm cost (13.3 ms vs 13.1-14.6, section 4b): the session layer adds no visible work. On the wire a running session is one INPUT (7 bytes) a frame each way plus netdrv's acknowledgements, and a 13-byte HASH a second: **~2.4 KB/s and ~120 datagrams/s each way** (netdrv frames incl. its header; `net_stats tx_bytes/rx_bytes`, new). Setup moves each console's STATE (425,984 B) and battery image (131,072 B) over the bulk lane once.

### 14.3 Hardware

`builds/hw-queue/pairrig.py` + `PAIR-README.txt`: one rig app per console (3000 = host, Go = join), started automatically when both overnight queues are complete; arms M (per-frame hash) and Q (none) after the GB link and AW-fake pairs.

**2026-10-02, first takeover: the Go stayed black at the wake arm (W) and its `log/` stayed empty. It was not memory.** The EBOOT on the Go's card (md5 `abd13048`) was arm W's `af98e599` with its FAT chain rewritten after the verified install:
- cluster 89 was missing;
- the last cluster was a fragment of the `shash.txt` the takeover had just deleted.

That corrupts `.ctors`/`.data`, so crt0 crashes before `main`. PPSSPP reproduces it with the card's file and boots `af98e599` cleanly.

The link build fits the Go with room to spare: 39.2 MB of heap at boot against a measured 3.1-3.6 MB floor. The budget is now a documented number with a build-time and a boot-time check (`docs/MEMORY-BUDGET.md`, branch `claude/go-memory-budget`).

### 14.4 Open

- The "Link cable" menu entry (player-facing Host/Join for a GBA game) and the shipping overlay for instance B (section 3).
- Two-ROM pairs: the partner's cartridge by library lookup or transfer (the GB path exists; the GBA side needs paging of a received cart).
- A GBA host test of the vtable path (sesssim covers the GB pair only); the GBA path is validated in PPSSPP.
- The session's end wait (`END_MARGIN` + 2 x delay) assumes slot 1 completes the agreed frame within a few scanlines of slot 0, true when both states were taken at frame boundaries; a late capture is logged (`gblink_final_capture_late`).

## 15. Smoothness fixes carried over, and the 3000 + 1000 hardware pair (2026-10-02, branch `claude/gbalink-hw`)

Branch `claude/gbalink-hw` = `claude/gbalink-session` + `claude/go-memory-budget` (b6f9f14) + the GB link's smoothness work (`claude/gblink-perf`, GB-LINK.md section 14, cherry-picked without the reverted `-Os`).

### 15.1 What carries over, and the kill switches

The GBA cable session is the same `fe_gblink` session with `fe_gbadual`'s machines, so the GB fixes apply with the same keys (config.ini = the player's kill switch; the harness key wins for an A/B arm):

| change | GBA hookup | key (default) | before |
|---|---|---|---|
| stall absorption | `gba_cable_start` sets `p.wait`. The pair's STALL returns at a frame start before anything runs, so a retry computes what the next display frame would have. `gba_cable_frame` drains the bulk lane through `gbl_drain_bulk` and keeps the absorbed wait out of `core_prof`. | `gblink_absorb` (1) | a late input lost the display frame |
| resend requests | `p.nack`. **Fixed on the way:** `fe_gblink_receive_bulk` ignored NACK/RESEND unless the GB pair (`dual`) existed, so a GBA session's requests went unanswered (PPSSPP, 3 % loss: 1,474 NACKs, 0 resends). The test is now `dual \|\| m`. | `gblink_nack` (1) | netdrv's retransmission timeout only |
| session audio: 3072-frame cushion, PI rate follow (bound 6 %), ring trim | `gblink_service` already drives it for any session. While it is on, ADR-0027's `pace_frame` / `pace_extra_vblanks` stand aside, so one controller owns the resampler step. (They are parked at nominal in a cable session anyway: `net_bringup` skips `pace_session_begin`.) | `gblink_audio_cushion` (3072), `gblink_audio_follow` (60) | ring started empty, fixed rate |
| soft underruns (fade out/in) | new switch | `gblink_audio_soft` (1) | silence steps |
| sync hash every 240 frames | the GBA hash is unchanged (both machines' combined guest hash); only the interval moves | `gblink_hash_every` (0 = 240) | 60 |
| EVT `gblink_win` every 10 s | adds `pg=A/B`: 32 KiB ROM pages read from the stick that window by each GBA | `gblink_win_ms` (10000) | - |

`perf_report.py` reads GBA session logs unchanged (frame_hist, sess_cost, core_prof, fps, gblink_stats). It takes audio starvation from `gblink_win` when a build has no EVT `audio_ring`, and adds `pg/min`.

### 15.2 Evidence (PC)

- **Host suites:** 30/30 PASS (`run_host_tests.py`, GB link protocol tests included).
- **Two PPSSPP consoles, AW2 battle.** The host runs the 64 MiB build and model; the join runs the 32 MiB build on the PSP-1000 model (`run_session_ppsspp.sh`, Xvfb :133/:134).

| run | radio | build | result | FINAL | per-frame (both machines, both consoles) |
|---|---|---|---|---|---|
| c0 (control: session rig EBOOT 232dbad6) | loopback | session | PASS, 243/243 HASH | `a2eb2b47a9241ad3` @ 14625 | identical, 29,249 lines |
| p1 | loopback | after, defaults | PASS, 60/60 | `a2eb2b47a9241ad3` @ 14625 | identical, 29,249 lines, = c0 frame for frame |
| p2 (before the NACK fix) | 15 ms + 5 ms jitter, 3 % loss | after | PASS, 60/60. NACKs 1,474/1,135, **0 resends** | `c554051ef31f0e0c` @ 14611 (end agreed earlier) | identical, 29,221 lines, = p1's prefix |
| p2b (final binaries) | same | after | PASS, 60/60. Resends 677/1,539 (1,577/1,515 inputs resent). Stalls 2,710 -> 2,046 (host), 2,367 -> 1,219 (join); longest 21 -> 8 and 18 -> 9 frames | `b15d3d3886a1057b` @ 14617 | identical, 29,233 lines |
| s-A, s-B, s-F | loopback | the staged rig's arms, exact files (`run_stage_pair.sh`) | PASS (A 60/60, B 243/243; F `score_awlink` PASS: 10 turns, state_watch agreed) | A and B `a2eb2b47a9241ad3` @ 14625 | (the rig arms have no per-frame hash) |

- The FINAL frame depends on when the host's END_AT lands, which is radio timing. The game does not: p1, p2 and p2b agree on every frame they share, and the periodic hash at frame 14400 is `60aa80930892fee5` in all three.
- PPSSPP is not a timing source here. Its numbers (core ~9.5 ms, for example) are a protocol check, never a CPU prediction.

### 15.3 The PSP-1000 could not join: memory, and the boot reserve

The first two-PPSSPP run with the join on the 1000 model failed at once with `gblink_start FAILED reason=create`. The core takes ROM blocks until malloc fails at load. AW2's 8 MiB cart had gone resident and left 1.25 MB, and a session's buffers come after that.

Measured on AW2 (`gblink_pre` -> `gblink_end`), a session peaks at its end at **2,284,912 B**: its own and the pre-link state + battery, the partner's, and the final copies.

So a configured cable session now reserves **2,560 KiB** before the boot gate (`cable_reserve_kb`):
- it is handed back if it would leave the core below `MEM_GBA_CORE_MIN` (the zero-ROM-block hang);
- it is freed the moment the session starts;
- the 1000's ROM cache pages instead.

**The PSP-1000's heap budget.** These are PPSSPP 1000-model numbers for the 32 MiB link harness (image 13,203,968 B). The real 1000's `heap_plus_image` is still unmeasured, so the rig's `EVT mem_boot` from the 1000 will be the first hardware number.

| point | free | largest |
|---|---|---|
| heap at boot (`mem_boot`) | 10,892,800 | 10,883,072 |
| reserve taken (`pre_jit`) | 7,216,184 | |
| `post_load`: small JIT tier (2,097,152 + 393,216), ROM cache **5 of 8 blocks (pages)** | 1,778,936 | 913,408 |
| reserve handed back | 4,400,296 | 2,621,440 |
| `gblink_running` | 2,673,352 | 1,634,304 |
| `gblink_end` (lowest) | 2,116,264 | 1,077,248 |

The 3000 (harness64) has the large JIT tier, the cart resident (15 blocks), and 15.9 MB free at `gblink_pre`. In PPSSPP the 1000 read ~11 ROM pages a minute (`pg=` 4-8 per 10 s while the battle's code moves).

### 15.4 Single-player and the one-console bench

- **Single-player GBA, before (b6f9f14) vs after (a9f1591).** Non-link harness builds, PPSSPP: Emerald, the video-oracle workload, every marker reached. **1,091 frames' core output identical, and the audio oracle identical** (`173df965` over 598,293 samples). The release flavour has no `GBA_LINK`. Its binary does change (the GB link fixes and the session-audio gate are in main_psp.c), but outside a session `g_gbl_audio_on` is 0, so the pacing, the audio thread and the resampler take the old paths.
- **The one-console bench arm M-BATTLE140K on the link build** (harness64 4f72106f, PPSSPP) is identical to the rig3 golden (EBOOT 69cb4adc): `gbadual_hash` 30,567 lines and `shash` 15,319 lines (its `c=` timing field removed). The bench path is unchanged.

### 15.5 The hardware pair rig (hw1)

`tools/gbamirror/prepare_gbalink_hw_rig.py` writes `builds/gbalink-hw-out/rig`. Its MANIFEST.txt lists every md5 and each arm's per-role EBOOT crc32.
- **Consoles:** PSP-3000 = HOST (F:, harness64); PSP-1000 = JOIN (D:, harness).
- **App folder:** the GB link rig's own `PSP/GAME/GBADHOC-GBLINK`, because the parked app relaunches its own folder. hw_loop.py's USB handoff drives it.
- **Staged every cycle:** the cart and BIOS with every arm, and the golden AW2 save and state restored.
- All EBOOTs are relabelled "GBALINK rig".

| arm | what | binaries |
|---|---|---|
| B | mirror, every smoothness change OFF (`gblink_absorb/nack/audio_cushion/audio_follow/audio_soft = 0`, hash every 60): the `claude/gbalink-session` behaviour, with the new telemetry | after (a9f1591): host harness64 4f72106f, join harness c488d1e5 (+ the 2,560 KiB reserve, which the 1000 needs to join at all) |
| A | mirror, defaults (the ported fixes) | same |
| F | the AW2 link-cable fake (the baseline: one core, RFU-style netplay), as built | baa63503 (claude/gbalink-baseline 8aa913a) |

- Every arm plays the same RAM-gated 10-round AW2 battle (the baseline's scripts, md5-identical), delay 4, with no per-frame hash (it costs ~10 ms a frame on a PSP).
- B is the "before" inside the after binary, so both arms carry the same telemetry. (The session build has no starvation counters.)
- Order BAABFABBAFAB: 12 runs, ~80 min.

**Takeover.** Both consoles were parked in the GB rig app: perf2 ended at 21:09, and `handoff_park_s = 0` means the park has no limit.
1. Scoped read-only backups first: `hnsrig backup --appdir GBADHOC-GBLINK --scope PSP/GAME/GBADHOC` -> `builds/gbalink-hw-out/backups/{3000,1000-2222}`, each with MANIFEST.verified.
2. hw_loop's wake cycle stages arm B inside a fresh window and commands RUN.
3. The last cycle commands STOP, so both consoles end parked.

**Scoring:** `python tools/gbamirror/score_gbalink_hw.py --logs ../gbalink-hw-out/logs/hw1 --rig ../gbalink-hw-out/rig`. It runs score_session.py on each mirror run and score_awlink.py on each F run, then perf_report with the window-matched core difference A vs B.

## 16. Beyond AW2: the multigame correctness rig (2026-10-02, branch `claude/gbalink-multigame`)

AW2 alone could hide a game-specific hole. The mirror now has a fixture per link-capable game on this PC, and one command that runs them all in two PPSSPP consoles.

### 16.1 The rig

`tools/gbamirror/multigame/` (Linux/WSL):

| file | what |
|---|---|
| `run_matrix.sh [OUT] [names...]` | **The one command.** AW2 runs first as the known-good control; the matrix stops if it fails, because PPSSPP wears out after ~40 launches and then every game "fails". Then every `fixtures/*.fixture` runs under {loopback, 15 ms ±5 ms jitter, 3 % loss}, plus a cost arm (loopback, no per-frame hash). Then the control runs again, then the table prints. Xvfb `:138`/`:139` only, one session at a time, each in its own private namespace; the script kills only its own processes, by PID. |
| `run_fixture_ppsspp.sh OUT FIXTURE [--lat --jit --loss --nohash]` | One fixture as a two-console session (`run_session_ppsspp.sh` generalised). Host = harness64 on PSPModel 1 (the 3000); join = harness on PSPModel 0 (the 1000, whose ROM cache pages). It md5-checks the ROM, states and batteries against the fixture. |
| `score_fixture.py` | Runs on top of `score_session.py`, which needs both consoles to exit 0 after `gblink_done`, every HASH matched, equal FINALs, and per-frame files identical to the end. This adds: both autopilots DONE; every marker in MARKS logged by BOTH consoles' scripts; the golden (both machines' per-frame lines at GOLDEN_FRAME); the trade (SWAP) or battle (CROSS) oracle; and `GAME_FAIL` for a known game-level failure. On a desync it prints the first divergent frame, the slot and the hashed region (r/i/e/io/p/o/v). |
| `matrix_report.py` | The table and the cost ranking. |
| `fixtures/<game>.fixture` | ROM, battery and state names and md5s, scripts, markers, oracle, golden, and the SIO modes seen. `msc.fixture` is Super Circuit's (16.7). |
| `scripts/<game>.spec` → `pair_script.py` → `<game>_host.txt`, `<game>_join.txt` | One spec per fixture. `H`/`J` lines belong to one machine, and the other machine gets the same number of frames as a `wait`, so blind scripts stay frame-aligned; `B <host> \|\| <join>` gives the two machines different input on the same frames. `gen_msc.py` builds Super Circuit's pair from `msc.spec` plus a generated closed-loop driver (16.7). |
| `dev_pair.sh`, `sheet.py`, `mkstate.sh` | The fast development loop. The desktop mirror (`gl_host`, x86) runs both machines with the real autopilot on each and dumps both screens: 60,000 frames in ~30 s. `sheet.py` makes a contact sheet. `mkstate.sh` makes a fixture's power-on state: power-on plus 30 frames with the battery in. That is exactly where the dev loop starts, and what the harness loads and holds until the link forms. |

`gl_host` gained:
- `--save-state` / `--save-state-at`;
- `--save-out0/1`: write a scripted battery (e.g. a Golden Sun save);
- `--rom1`: instance B's own cartridge, for two-ROM pairs;
- `--sio-trace A:B`;
- `--ram-dump DIR:F0:F1:STEP`: both machines' EWRAM+IWRAM every STEP frames (the RAM searches behind Super Circuit's driver, 16.7);
- an **SIO census** at exit: which mode each port sat in, for how many frames, and how many transfers completed in each mode.

Binaries, ROMs, states and batteries live outside the repo, in `~/gbamulti/{eb,roms,fx}`; the EBOOTs are also in `builds/gbamulti-out/fix-harness{64,}`. Each fixture pins them by md5.

### 16.2 Fixtures and the SIO modes each game uses

| fixture | game | session | battery / state | SIO (gl_host census) |
|---|---|---|---|---|
| aw2 | Advance Wars 2 | the existing 10-round battle (control) | owner's save+state on both | multi-player 16-bit (25,395 transfers in the battle) |
| sa1 | Sonic Advance | VS > Multi Game Pak > Race, Neo Green Hill 1: lobby, character and zone select, then ~88 s of racing with both positions crossing every frame | fresh cartridge (name entry scripted) | multi-player 16-bit (116,890 transfers; the port sits in multi from boot) |
| mt | Mario Tennis: Power Tour | Multiplayer > P1 vs P2 singles, Hard court, 1 set of 2 games: ~125 s of rallies, points and games on both screens | owner's 3000 save (host) and 1000 save (join): different batteries | multi-player 16-bit (178,062); a normal 32-bit wireless-adapter probe for 31 frames (235 transfers) on entering Multiplayer |
| mg | Mario Golf: Advance Tour | Multiplayer > With Game Link | owner's 3000 save on both | multi-player 16-bit (~50,000); the normal 32-bit adapter probe (266); JOY BUS and general-purpose at boot. **The link never forms** (16.4) |
| em | Pokémon Emerald (stand-in for Ruby, 16.5) | Cable Club > Trade Center: both players sit, trade party slot 0, then the trade animation and the post-trade save | the owner's two rig saves (different parties, parked at Mauville PC 2F) | multi-player 16-bit (50,263); the normal 32-bit adapter probe at boot (944) |
| emb | Pokémon Emerald | Cable Club > Colosseum > Single Battle: three turns (FIGHT and the first move, both sides) | same | same |

**All six run the session itself in multi-player 16-bit.** Other modes appear only as probes:
- normal 32-bit is the Nintendo RFU library's wireless-adapter probe ("NI" handshake, `SIODATA32 = FFFF494E`);
- JOY BUS and general-purpose appear only at boot.

No game here uses normal mode for the link itself, and none uses UART.

**What each fixture asserts.** Every RAM-gated fixture asserts that the game got where it should on BOTH machines, using markers from each console's own autopilot. On top of that:
- `em` asserts the trade with its oracle: each machine's party slot 0 is the other's former slot 0;
- `emb` asserts both machines see the same battle: each side's HP is the other's opponent HP.

### 16.3 Bug found and fixed: the adapter probe never completed

**The bug** (`gbalink_core.c`, shared code, `GBA_LINK` only).
- The RFU library's probe writes SIOCNT `5080` (start, external clock), then `5081` (start, internal clock).
- The port started a normal-mode master transfer only on start's rising edge, so this transfer never ran: start stuck, no IRQ, and the game fell back on its timeout.
- On hardware the shifter clocks as soon as the internal clock is selected with start set.
- Seen in Mario Golf, Mario Tennis and Emerald: `n_xfers` 0 before; 266 / 235 / 944 after.

**The fix.** A master transfer starts when the written value has start plus the internal clock and no transfer is running, and either start rose or the clock source just switched from external.

**What the fix exposed** (`gbalink_pair.c`).
- With both ports masters, the child clocks its own transfers while the parent runs a lookahead ahead.
- Each child transfer blocks; the pump makes the parent's reply; the child completes the transfer, runs on, and blocks on the next one.
- The stepper's deadlock guard counted those consecutive BLOCKED runs and stopped the pair (`E_DEADLOCK`, Golf at frame 2038).
- The guard now resets when the blocked machine's clock has moved. A real deadlock (blocked again at the same time, either slot) still trips it.

**Unchanged where it should be.**
- AW2 never uses normal mode. The desktop battle is hash-identical before and after (31,999 lines, 16,000 frames × 2), and the PPSSPP session matches its golden at f=14490 on the rebuilt EBOOTs (harness64 008abba9, harness 8c8e30bc).
- Sonic is identical.
- Mario Tennis and Emerald now complete the probe, so their machines differ from the old build from the probe on. That difference is the fix, and both still link.

### 16.4 Mario Golf: the link never forms (open)

**The symptom.** Both machines sit on "Please connect the Game Boy Advance Game Link cable" forever. It is deterministic, before and after 16.3, at lookahead 2464 as at the default, and whichever console enters the screen first. It is not a desync: the two consoles agree.

**What the transfers show** (a debug build tracing every completed multi-player transfer):
- The parent sends Camelot's 16-word packets (`fefe`, sequence, checksum, data, `0000` ×4). Mario Tennis uses the same format with an `aaaa` header.
- Golf's child only ever answers with the parent's PREVIOUS word (a loopback echo) and never starts its own sync.
- Tennis's child sends its own `aaaa`/`5555` sync from the first packet and is word-aligned with the parent by the third.
- So Golf's child link task never starts.
- Golf has an extra multi-player routine that Tennis lacks: ROM 0x08019584 reads SIOMULTI0-1 as one word, runs a state machine at 0x02004120 and keeps a player id at 0x02003014. In this session it is dormant: state 0, id 0, and its SIOMULTI copy at 0x02002F00 is never written.

**The cause is not found.** Candidates still open:
- something the mirror does not model in the general-purpose phase just before: both ports drive SD/SO and poll RCNT every ~15 cycles for ~17,000 cycles;
- a child-side condition that only the real cable meets.

`fixtures/mg.fixture` keeps Golf in the matrix with `GAME_FAIL`, so a fix shows as a changed verdict. **Real-hardware check wanted:** two PSPs, Golf, With Game Link.

### 16.5 What is not here yet

- **Mario Kart: Super Circuit** was added on 2026-10-03, once the owner copied the ROM (section 16.7).
- **Golden Sun / The Lost Age.** The only GS battery on this PC (the Go's) is blank. A scripted new game can save near Vale (`gl_host --save-out0` does it), but Battle mode unlocks only after leaving Vale, and a blind script cannot reach that in reasonable time. It needs an owner save past Vale (GS1), or a TLA save with Battle unlocked. GS1 → TLA transfer is a two-ROM pair.
- **Pokémon Ruby ↔ Ruby.** There is no Ruby battery here, and the Cable Club needs the Pokédex and two Pokémon. Emerald stands in: the same Gen 3 link layer and Cable Club code (pret), the owner's two real rig saves, and both a trade AND a battle. A Ruby fixture is the Emerald spec with Ruby's symbols, once a save exists.
- **Ruby ↔ Sapphire (two ROMs): feasible, not built.**
  - **The core is ready.** On the desktop mirror the two cartridges run in one pair (`gl_host --rom1`: Ruby on A, Sapphire on B). Both reach the main menu, and a rerun from fixed states is hash-identical. Each instance has its own pool and its own page 0 / RTC port.
  - **The session blocks it.** `fe_gblink` requires the same SHA-1 for a vtable pair (`gblink_partner_rom ... (same cartridge needed)`), and `fe_gbadual` loads B from A's ROM and shares A's buffer.
  - **The partner cart has to come from somewhere:** the library or the bulk lane. That needs section 3's owner decision for the PSP-1000: two 16 MiB carts cannot both be resident, and a received cart cannot be paged without a file.

### 16.6 The matrix (PPSSPP, two consoles; EBOOTs harness64 008abba9 + harness 8c8e30bc = 9e8819e)

`bash tools/gbamirror/multigame/run_matrix.sh` (cells: verdict and the agreed end frame; no run had a desync, so the desync column is empty):

| game | loopback | 15 ms ±5 ms jitter | 3 % loss |
|---|---|---|---|
| aw2 (control) | PASS f=14625 | PASS f=14625 | PASS f=14623 |
| sa1 Sonic Advance race | PASS f=9264 | PASS f=9264 | PASS f=9260 |
| mt Mario Tennis match | PASS f=12340 | PASS f=12340 | PASS f=12329 |
| em Emerald trade | PASS f=6175 | PASS f=6175 | PASS f=6172 |
| emb Emerald battle | PASS f=6959 | PASS f=6959 | PASS f=6955 |
| mg Mario Golf | GAME-FAIL f=4910 | GAME-FAIL f=4909 | GAME-FAIL f=4910 |

- **What PASS means here.** Both consoles' per-frame two-machine hashes are identical to the agreed end. Every HASH message matched and the FINALs are equal. Both machines match the fixture's golden at its fixed frame, and the golden is the same in all three conditions. Every marker was reached on both consoles, and the trade / battle oracle holds. The end control (AW2 loopback after the matrix) also passed.
- **Mario Golf** is deterministic in every arm (per-frame identical, golden equal across conditions). It fails only on its known game-level failure, 16.4.
- **The end frame moves under loss** (14623 vs 14625) because the host's END_AT lands by radio timing. The machines do not move: the goldens are taken at fixed frames before the end.
- **PPSSPP screen dumps** (`log/frame_*.bmp`, each console's own machine) confirm that the blind fixtures play on the PSP's engine too: Sonic shows the two-player lobby, the zone select and the race at 1:20 with the partner's arrow; Tennis is in the match at 15-15 under 3 % loss.
- **One fixture fix on the way.** emb first ran six turns. After six turns on the desktop the HP was 5 and 1, and on the PSP's engine a mon fainted, so the 7th prompt never came. It now runs three turns: HP 130/90 on PSP, 132/92 on x86. The difference is expected (different engines), and the oracle compares within one engine.
- **Wall time:** ~80 min for the whole matrix with the controls.

**Per-frame core cost, PPSSPP, relative ranking only** (the cost arm: loopback, no per-frame hash; `core_prof` mean over the session, both consoles). PPSSPP is not a timing source; this only says which games will be hardest on hardware:

| game | ms (PPSSPP) | vs AW2 |
|---|---:|---:|
| Mario Tennis | 17.60 | **1.83×** |
| Mario Golf (waiting at the link screen) | 13.33 | 1.39× |
| Mario Kart: Super Circuit (VS race; 16.7, its own session: 10.90 vs AW2 9.56) | | 1.14× |
| AW2 | 9.62 | 1.00× |
| Sonic Advance (race) | 9.32 | 0.97× |
| Emerald battle | 9.17 | 0.95× |
| Emerald trade | 8.12 | 0.84× |

- Camelot's two games are the outliers: Tennis is the heaviest by far.
- Tennis transfers the most: 178,062 multi-player transfers in ~12,000 frames, about 15 a frame, against AW2's ~1.6. Every transfer is a parent BLOCK and a core switch.
- Golf costs 1.39× while only sitting on the link screen exchanging packets.
- Sonic, the real-time racer and the stand-in for Super Circuit, costs about what AW2 does, despite ~12 transfers a frame (116,890 in 10,000 frames).
- The hardware pair should be measured on Tennis next after AW2.

### 16.7 Mario Kart: Super Circuit (added 2026-10-03)

**The ROM.** The owner added `Mario Kart - Super Circuit (USA).gba` (AMKE, 4 MiB, md5 784a036f). The fixture `msc` runs a **two-player Multi-Pak VS race**, not Single-Pak:
- Multiplayer > MULTI-PAK LINK, the parent's OK?;
- VS: two players and no CPU karts, 50 cc, random characters;
- Mushroom Cup > Peach Circuit.

**A real race needs a driver.** A blind script cannot drive a lap of Peach Circuit, and a blind script tuned on the desktop's x86 dynarec would not survive the PSP's MIPS dynarec. So `gen_msc.py` writes a **closed-loop driver in plain autopilot steps**, with no new engine op:
- **The RAM**, found with the new `gl_host --ram-dump` (kart struct 0x1A0 bytes; kart 0 = the parent's, 1 = the child's, 2-7 CPU):
  - heading `0x03003BEE + k*0x1A0` (u16, 0 = north, +RIGHT);
  - position `0x03003B9A` / `+4` (track pixels);
  - progress byte `0x03003BE0` (0 → 4 after lap 1 → 12 → 0x34/0x3C finished);
  - speed `0x03003C56`.
- **The line** is one clean lap of a CPU kart in a Mushroom Cup GP race, recorded on the desktop mirror (`scripts/msc_line.json`).
- **The steering rule.**
  1. Every 128 px cell within 60 px of the line gets a pure-pursuit target heading (toward the line 250 px ahead), quantised to 16 sectors.
  2. Each loop pass finds the kart's cell with `ifram` on X/Y (mask 0xFF80; aligned row pairs with the same target share one test) and compares the heading sector against the target.
  3. Off target, it turns for 3 frames: with the throttle normally, coasting in the two hairpins.
  4. Every pass ends with `hold A 2`.
  5. Each kart stops driving once it completes lap 1.
- **The check.** Then BOTH consoles wait (RAM-gated) for BOTH karts' lap-1 bit; a kart that never makes it is an `ap_fail`.

**Tuned and checked on the desktop mirror.** 38 different start delays (0-53 frames, which changes every collision) all made lap 1 for both karts.
- The full 3-lap race finished for both in only 7 of 14 tries, so the fixture asserts lap 1.
- The brief allowed this: "at least one lap if a full race is too long".
- The tuning that mattered:
  - 16 sectors, not 8: an 8-sector "straight" is 45° wide, and a kart pushes into a barrier at a shallow angle forever;
  - no brake in the hairpins (B+turn barely turns);
  - the sector chosen by floor, not round (round biases "straight" half a sector clockwise).

**On the PSP's engine (PPSSPP)** both karts completed lap 1, seen on both consoles at f=12314/12315.

**SIO: multi-player 16-bit**, ~2 transfers a frame during the race (21,071 by f=14000). The port sits in JOY BUS from boot until Multiplayer, with no transfers.

**The matrix** (`run_matrix.sh ~/gbamulti/m4 msc`, same EBOOTs as 16.6; the AW2 control passed before and after):

| game | loopback | 15 ms ±5 ms jitter | 3 % loss |
|---|---|---|---|
| aw2 (control) | PASS f=14625 | PASS f=14625 | PASS f=14621 |
| msc Super Circuit VS race | PASS f=13830 | PASS f=13828 | PASS f=13802 |

- No desync.
- The golden at f=12000 is the same in all three conditions.
- Both karts' lap 1 was seen on both consoles in every arm (f=12314/12315 on the host's console).

**Cost** (cost arm, same session): Super Circuit 10.90 ms vs AW2 9.56 ms, so **1.14× AW2**. It ranks just under Mario Golf and well under Mario Tennis (1.83×). PPSSPP, a ranking only.

**Two notes for whoever drives a Super Circuit session by script:**
- The parent may finish its script well before the child: the child still has leftover loop passes and its own lap checks. The fixture therefore ends 1,500 frames after the host is DONE (`END_AFTER`); the default 120 cut the child's checks off.
- The menus are blind: VS, the cup and the track are taken by the parent's presses, timed in the dev loop and shown on both consoles' screen dumps.
## 16. Overnight 2026-10-02/03: profile and speed, one change at a time (branch `claude/gbalink-hw`)

Goal: the GBA mirror at full speed on a PSP-3000 + PSP-1000 pair. Not reached yet. Every number below is from hardware unless it says PPSSPP; PPSSPP was used only for determinism.

### 16.1 Running table

| round | change (kill switch) | where | 3000 fps / work / core ms | 1000 fps / work / core ms | determinism | kept |
|---|---|---|---|---|---|---|
| hw1 B | 3.1 session behaviour (all section-15 switches off) | pair, AW2 battle, 2 runs | 38.0 / 21.5 / 16.9 | 38.0 / 25.9 / 19.7 | PASS 2/2 (FINAL equal both consoles) | control |
| hw1 A | section 15 fixes on (absorb, NACK, audio cushion/follow/fade, hash every 240) | pair, 2 runs | 37.7 / 21.5 / 16.7 | 37.7 / 26.2 / 19.9 | PASS 2/2 | yes (no speed effect: CPU-bound; silence 36.4 -> 32.9 %) |
| solo S | the hw1 after build, both AW2 machines on ONE console (M-BATTLE140K bench) | 1000 alone, 2 runs | - | 59.0 / - / **16.31** | - | control |
| solo H | headless partner mixes no sound (`gba_link_headless_audio`) | 1000 alone | - | 59.1 / - / 16.47 (1 run) | PPSSPP bench = golden | yes (see N vs C: -0.33 ms) |
| solo D | `DISPATCH_CACHE` (claude/dynarec-profile prototype; build flag) | 1000 alone | - | 59.9 / - / **13.09** | HW: both machines every frame = golden (30,567 lines) | yes |
| solo N | D + save watch, headless audio OFF | 1000 alone, 2 runs | - | 59.9 / - / 13.26 | - | (A/B leg) |
| solo C | D + headless audio + save watch counting backup writes (`gba_link_sram_crc`) | 1000 alone, 2 runs | - | 59.9 / - / **12.93** (-3.38 ms, -21 % vs S) | HW per-frame hash = golden (V); shash X (C) = Y (S) over 15,318 frames | yes: the best build |
| hw2 P | C + the DRPROF_HW profiler (cycle 1; the scorer says INVALID only because hw_loop logged it as arm x: the card EBOOTs are arm P's) | pair, 1 run | 43.5 / 22.46 / 15.80 | 43.5 / 22.16 / 15.41 | PASS (score_session: 60/60, FINAL a2eb2b47 @ 14625 both) | profile only |
| hw2 A | merged control (772d287, no dispatch cache, partner audio on, 128 KiB SRAM hash) | pair, 2 runs | 38.4 / 25.71 / 19.36 | 38.4 / 21.46 / 15.22 | PASS 2/2 | control |
| hw2 C | best build (dispatch cache + headless partner audio + save watch) | pair, 2 runs | **42.0** / 23.48 / 17.10 | 42.0 / 20.75 / 14.47 | PASS 2/2 (FINAL a2eb2b47 @ 14625) | yes. Window-matched core vs A: host -2.26 ms, join -0.75 ms |
| hw2 F | AW2 link-cable fake (baseline) | pair | **the 3000 dropped off a few seconds into it**, as in hw1 cycle 5 (join: peer silent from frame ~126, `ap_fail` at 11029) | | not run | - |
| hw3 L | merged pinned tree (LAYOUT_PIN, JIT discipline) + profiler, shash on | pair, 1 run | 43.1 / 22.85 / 16.06 | 43.1 / 21.17 / 14.79 | PASS | profile |
| hw3 M | same, `shash = 0` (the harness oracle off) | pair, 1 run | 54.7 / 17.60 / 16.60 | 54.7 / 15.81 / 15.05 | PASS | yes: shash = ~5 ms of loop, harness-only |
| hw3 N | pinned best, shash 0 | pair, 2 runs | 56.8 / 16.31 / 15.68 | 56.8 / 16.61 / 15.66 | PASS 2/2 | - |
| hw3 U | UNPINNED best (`LAYOUT_PIN=0`), shash 0 | pair, 1 run | **59.3 / 13.76 / 13.21** | 59.3 / 15.58 / 14.62 | PASS | control; window-matched core N vs U: host +2.47 ms, join +1.04 |
| hw3 S | profiler, single-player (no session) | each console alone, 5,400 frames | 59.7 (paced) | 59.7 | not a session (the session scorer's FAIL is expected) | loop reference |
| hw4 U | unpinned (`LAYOUT_PIN=0`), shash 0, AW2 | pair, 2 runs | **59.2 / 14.11 / 13.49** | 59.2 / 15.73 / 14.81 | PASS 2/2 | **yes: the shipping layout for link builds** |
| hw4 P | pinned, B's cache at 0x0500 (`gba_link_jit_pin_b = 1280`) | pair, 2 runs | 53.6 / 14.09 / 14.23 | 53.6 / 18.15 / 17.16 | PASS 2/2 | no: core vs U host +0.74, join +2.35 ms |
| hw4 Q | pinned, B's cache at 0x1D00 (`7424`) | pair, 2 runs | 57.3 / 14.31 / 13.93 | 57.3 / 16.58 / 15.61 | PASS 2/2 | no: host +0.44, join +0.81 ms |
| hw4 V | unpinned, **Super Circuit** VS race | pair, 1 run | **58.9** / 14.86 / 14.36 | 58.9 / 15.34 / 14.50 | PASS 57/57; both karts lap 1 on both consoles | yes |
| hw4 R | pinned + B at 0x0500, Super Circuit | pair, 1 run | 57.6 / 15.71 / 15.18 | 57.6 / 15.58 / 14.83 | PASS 57/57; lap 1 both | no |

hw1 cycle 5 (the AW fake, arm F) never finished: the 3000 stopped about one second into it (its log ends at `ap_await_peer released frame=11`; the join saw it go silent and ended `ap_fail`). The mirror runs before it all ended `gblink_done` and handed off, so this is not the mirror's teardown. The owner found the 3000 off in the morning, and a battery death fits. Its last heartbeat file is from the previous (mirror) run, not this one.

### 16.2 Where a frame goes (DRPROF_HW, the 1000, both machines on one console)

`EVT linkprof` (section 15 profiler, `linkprof_report.py`). The sampler fires at ~420 Hz on the PSP, so the shares are what count. The core frame is 16.3 ms here:

| phase | A (drawn) | B (headless) |
|---|---|---|
| translated code | 17 % | 20 % |
| `update_gba` (event scheduler) | 14 % | 16 % |
| C block dispatch | 7 % | 8 % |
| serial (the cable) | 3 % | 4 % |
| sound | 2.5 % | 3 % |
| video (`update_scanline` + ME capture) | 2.6 % | 0 |
| DMA, memory slow paths, translation, flush | < 1 % each | < 1 % each |

- **The headless partner costs as much as the player's machine (52 % vs 48 %).** It already skips scanline rendering. It was still mixing sound, which is now off. Everything else it does is the game itself, and every bit of it is game-visible.
- **The ME renderer is active during sessions** (`me_rend on`; the ME renders A on the second core, about 10.5 ms there). So A's CPU-side video is only the scanline capture.
- **The dispatcher was the cheapest big win**, as the dynarec study predicted: DISPATCH_CACHE alone took the 1000 from 16.3 to 13.1 ms for two machines.
- **The session costs more than the bench**: core 19.9 vs 16.3 ms, and the session loop adds another ~6 ms outside the core (work 26 ms). hw2's profile arm P splits that.

### 16.3 What the best build does not fix (yet)

Projected from the solo results (an estimate, not a measurement): the 1000's session core drops ~3.4 ms, to ~16.5 ms, with work ~22.5 ms. That is still over a 16.7 ms frame, so the pair should land in the mid-40s fps, not 60. The remaining levers, in order:
1. The ~6 ms the session loop spends outside the machines on the 1000 (to be split by hw2 P).
2. The session-only core overhead: 3.6 ms over the bench.
3. `update_gba` (30 % of the core), the dynarec study's item 4.

The periodic hash (12 ms every 4 s) only costs a late frame each time; spreading it is small next to these.

### 16.4 Builds

From commit 772d287 (claude/gbalink-hw + claude/gbalink-multigame's 9e8819e):

| build | 1000 (harness, 32 MiB) | 3000 (harness64) |
|---|---|---|
| best (`GBA_LINK=1 CORE_EXTRA="DISPATCH_CACHE=1"`, title "GBAdhoc GBA LINK") | `builds/gbalink-hw-out/m-best-harness` EBOOT f0909ac5, ME 18158678 | `m-best-harness64` EBOOT b5b70d54, ME 19ae57b6 |
| control | `m-ctl-harness` 4aedcfcf | `m-ctl-harness64` e7b49c24 |
| profiler (+ `DRPROF_HW=1`) | `m-prof-harness` b485c095 | `m-prof-harness64` 92e3f721 |

- **There is no playable (release-profile) GBA link EBOOT.** `tools/build.sh` refuses `GBA_LINK=1` for a release by design: instance B is linked statically and costs every game ~5 MiB, and there is no menu entry yet (section 14.4). The pair runs from the harness builds.
- The 1000 needs its 32 MiB `harness` build. The boot gate passes on both layouts. The cable reserve (section 15.3) holds the 1000's session memory.

## 17. The loop outside the machines (2026-10-03, branch `claude/gbalink-hw`)

### 17.1 What hw2 says

- **The 3000 (host) is the console holding the pair back**, not the 1000. In lockstep both run at the slower one's rate: C's host does 23.5 ms of work a frame against the join's 20.8.
- **The 3000's speed depends on the binary.** The profiler build P (C plus the sampler, which can only add cost) ran the host's core at 15.8 ms; C's own binary ran it at 17.1 in both of its runs. The 1000 behaves as expected (P 15.4, C 14.5). hw1 had the opposite asymmetry (3000 core 16.9, 1000 19.7). This looks like code or data layout (cache-set conflicts between the two cores' hot paths or state), not anything either build does. It is unproven: hw3 runs the same C binary again (arm N) beside two profiler arms.
- **About 6.4 ms of every frame is loop on both consoles, in every arm** (work minus core: A 6.35/6.24, C 6.38/6.28, P 6.66/6.75).
- **The AW fake (F) took the 3000 down again.** As in hw1, the 3000 went silent a few seconds into F and never came back to the USB handoff. The join saw it and ended `ap_fail`. That is two out of two F starts on the 3000, so a dead battery is no longer the likeliest explanation. F is the old baseline EBOOT (baa63503, built clean from claude/gbalink-baseline 8aa913a) and is not under study here, so it is left out of the rigs from hw3 on. baa63503 does include the AW serial fake's frame-overflow fix (2a0229c is an ancestor of 8aa913a), so that 258-word overflow is not the cause. One difference worth recording: F is the only arm in which the 3000 runs the 32 MiB `harness` layout (one EBOOT for both roles) instead of `harness64`.

### 17.2 The finer split (commit 7720924, DRPROF_HW builds only)

- `EVT looprof f= lp=...` splits the sampler's `loop` share into the main loop's regions:
  - top (power ticks, pad);
  - `net_frame`, `gblink_service` (session audio follow, OSD chip);
  - misc (mgift/rfu drains, UI chords, FF);
  - the loop-top ME present and fps chip;
  - the autopilot frame;
  - `fe_host_run_frame` outside the session step;
  - post-core (health, dumps, rig events);
  - **shash**;
  - the pacing bookkeeping, the pre-wait OSD draw, the post-wait retire, the present, `vid_swap`, `rig_note`;
  - anything left.
- `EVT thrcpu` gives every user thread's own CPU per window (`runClocks`). The sampler counts a READY emulation thread, so a higher-priority thread that preempts it (audio 0x12, ad-hoc RX/TX 0x1E/0x1F, the sampler 0x0F) lands in whatever phase the emulator was in. This tells the two apart. PPSSPP reports 0 for every thread, so this needs hardware.
- `linkprof_report.py` prints both.

**PPSSPP (determinism only).**
- Rig3 arms L (profiler, defaults) and N (C binary, `shash = 0`): PASS, 60/60 hashes, FINAL `a2eb2b47a9241ad3` @ 14625, the same as every earlier build.
- Arm S (single-player) exits cleanly at its frame limit.
- Even in PPSSPP, the harness `shash` line (an FNV over ~390 KiB of guest memory every frame: IWRAM, EWRAM, VRAM, I/O, palette, OAM) is almost all of the loop: 1.85 of 1.92 ms.

### 17.3 hw3 (rig3): arms and why

Restaged after the merge (section 17.4): every arm comes from `claude/gbalink-hw` e5308c8. Each binary's build log is in `builds/gbalink-hw-out/p3-*.log`.

| arm | binaries (harness64 / harness EBOOT md5) | keys | question |
|---|---|---|---|
| L | `p3-prof`: pinned + DRPROF_HW (02b24003 / bc5dc12d) | defaults (`shash = 1`) | where the 6.4 ms of loop goes on hardware |
| M | same | `shash = 0` | the harness oracle's own cost (a player build never pays it) |
| S | same | `gblink_role = none`, `gba_dual = 0`, `autoexit_frames = 5400` | a single-player frame's loop on each console |
| N | `p3-best`: pinned (fbd126ea / 2de94ae7) | `shash = 0` | the merged best build's real speed without the oracle |
| U | `p3-unpin`: `LAYOUT_PIN=0` (19fc7cfe / 8f09c225) | `shash = 0` | the unpinned control: what pinning is worth on the pair |

- Order LMSNUNU, 7 runs, about 65 min.
- Determinism is scored on the session's own periodic and FINAL hashes, which do not depend on `shash`.

**PPSSPP, two consoles (determinism only):**
- L, N, U: PASS, 60/60 periodic hashes. The frame-14400 hash is `60aa80930892fee5` in all three, as in every build since section 15.
- L and N end at FINAL `a2eb2b47a9241ad3` @ 14625. U's end landed one frame earlier (`e14dbc2f7af26a4f` @ 14624). That is the radio-timed END_AT of section 15.2, not a divergence.
- S exits 0 at its frame limit on both consoles.
- **The multigame matrix on `p3-best`** (`~/gbalink-pp/mx-p3best`, Xvfb :140/:141, sandboxes under a private HOME so they cannot collide with the multigame agent's): the same verdicts as the pre-merge matrix (section 16.6).
  - aw2, em, emb, mt, sa1 PASS under loopback, 15 ± 5 ms jitter and 3 % loss.
  - mg GAME-FAIL in all three: the link never forms, the open item of section 16.4.
  - The end control passes.

### 17.4 Layout pinning merged (commit c51effc + e5308c8)

**Why.** The coordinator pointed out that `claude/gbalink-hw` had no layout pinning. On the 3000, the single-core candidate measured layout alone moving AW2 by about 1.2 ms a frame (unpinned 5.56 vs pinned 4.39), and padding moved an unpinned build by ±1 ms. That fits section 17.1's binary-sensitive host.

**The merge.** `claude/candidate-3.1-all` is merged in. It brings:
- `LAYOUT_PIN=1` by default and `psp/layout/hot.ord`;
- `JIT_CODE_DISCIPLINE=1`;
- `DISPATCH_CACHE=1` in the shared core flags;
- `SMC_RETIRE_WINDOW=1`, `SERIAL_IDLE_FAST=1`;
- the 3.1 fixes.

`SERIAL_IDLE_FAST` leaves `SERIAL_MODE_CABLE` alone: the default case takes the full path, so the cable stepper is unchanged.

**Instance B and the hot order.** B is the whole core archive, partially linked and renamed `gbab_*`.
- Its C functions keep their `.text.<name>` sections, so every per-function line of `hot.ord` already places `gbab_X` right beside `X` (checked: `gbab_update_gba` 0x08805440, `update_gba` 0x08805fa0).
- Only B's asm stubs, its plain `.text`, fell outside the pinned region (`gbab_mips_indirect_branch_arm` at 0x089d0f20).
- `hot_order.txt` / `gen_layout.py` now place `*gbalink_core_b.o(.text)` right after A's `mips_stub.o`. In a non-link build that line matches nothing.
- Not done yet: a two-core profile for `gen_layout.py`. B's translation cache is pinned at the same offset mod 8 KiB as A's, so both machines' hottest emitted stubs share I-cache sets. Moving B's base by half a way is the obvious next layout experiment, one change at a time.

**hw3 is restaged from the merged tree** (rig3 was built from the unpinned tree):
- `p3-prof` = pinned + profiler;
- `p3-best` = pinned, no profiler;
- `p3-unpin` = `LAYOUT_PIN=0`, the unpinned control.

### 17.5 hw3 results: the loop is the harness oracle; pinning both cores costs the host

**The loop, split on hardware** (`linkprof_report.py`, logs/hw3):

| ms/frame | 3000 L | 1000 L | 3000 M (shash 0) | 1000 M | 3000 S (single) | 1000 S |
|---|---|---|---|---|---|---|
| loop total | 6.55 | 7.02 | **1.24** | **1.26** | 10.38 (5.0 of it the one machine) | 10.35 |
| shash | 5.36 | 5.76 | 0 | 0 | 5.08 | 5.08 |
| `net_frame` (netdrv pump) | 0.52 | 0.50 | 0.52 | 0.49 | 0 | 0 |
| loop-top ME present | 0.28 | 0.30 | 0.25 | 0.25 | 0.05 | 0.05 |
| pre-wait OSD draw | 0.12 | 0.15 | 0.15 | 0.17 | 0.07 | 0.06 |
| `vid_swap` | 0.10 | 0.11 | 0.12 | 0.11 | 0.03 | 0.03 |
| post-core, session service, rest | 0.17 | 0.20 | 0.20 | 0.24 | 0.15 | 0.17 |

- **shash was the loop.** It is an FNV over ~390 KiB of guest memory every frame, written by harness builds only, and it is 5.1-5.8 ms on hardware. With it off, the session loop is 1.25 ms on both consoles, about 1 ms over a single-player frame's (0.3 ms). That ~1 ms is the netdrv pump (0.5), the loop-top ME present (+0.2), and the OSD session chip.
- **Other threads, per frame (`thrcpu`, M):** the WLAN driver (`SceWlanMac` 1.4, `SceWlanHal` 0.8, `SceNetNetintr` 0.3-0.4), our ad-hoc RX/TX 0.5, audio 0.25, the profiler's own sampler 0.5. The emulation thread itself is 13.2-13.9 ms of the 16.5 ms frame. The radio is about 3 ms a frame that the loop cannot win back.
- **Pinning both cores hurts the host.** N (pinned) vs U (unpinned): host core 15.68 vs 13.21 ms, window-matched +2.47 ms; join +1.04 ms. U was the only arm near 60 fps (59.3).
- The logs prove why. `EVT jit_cache` / `gbadual_jit` (added in 5df98d9) show A's and B's translation caches both at 0x1500 mod 8 KiB on both consoles (3000: A LARGE 0x...1500, B SMALL 0x...3500). The emitter's stubs are 11,248 B (3000) / 14,608 B (1000, swap stubs), so the two machines' hottest emitted code sits in the same I-cache sets.

**The fix under test (commit 5df98d9, kill switch `gba_link_jit_pin_b`).**
- In a LAYOUT_PIN link build, each instance's SMALL tier can start at its own offset mod 8 KiB.
- `mips_stub.S` gives the static arrays one way (8 KiB) of slack, so no capacity is lost.
- `jit_small_pin_at` (instance B's copy is `gbab_jit_small_pin_at`, set by `fe_gbadual` from the config/harness key) moves the pointers before anything is emitted.
- -1 (the default) is today's placement. Code addresses only; the guest cannot see them.
- PPSSPP pair: B at 0x0500 (P), at 0x1D00 (Q) and at the default (K) all PASS 60/60 with FINAL `a2eb2b47` @ 14625.

**Instance A's layout in a link build (commit cfb015e).** The hot order used to interleave B's renamed functions (`gbab_X` right before `X`), which moved instance A in every link build. `gen_layout.py` now places A's whole hot path first, with `EXCLUDE_FILE(*gbalink_core_b.o)` on A's lines, and B's stubs and hot functions after it, in the same order.
- Release with vs without GBA_LINK: A's dispatcher and stubs are at the same addresses (`mips_update_gba` 0x08804040, `mips_indirect_branch_arm` 0x088041c8).
- A's C hot functions still shift by 0x20-0x140 B. The link code's own branches make a few of them longer (`serial_next_event`, `update_gba`), which no ordering can undo.

### 17.6 The release with GBA_LINK: what the 5.4 MB is

`tools/build.sh release` with `GBA_LINK=1` builds (commit cfb015e) and passes the release audit. The boot gate passes on the PSP-1000 (heap 11.06 MB > 4 MiB floor), but against the release without the link code (same commit):

| section | without | with | growth | what it is |
|---|---|---|---|---|
| .text | 1,721,008 | 2,765,660 | **+1,044,652** | instance B's whole core (800 KB: `translate_block_arm` 367 KB, `_thumb` 150 KB, `execute_arm` 73 KB, ...), the frontend link code (~22 KB), A's functions grown by the cable branches (13 KB) |
| .rodata + .data | 378,032 | 534,040 | +156,008 | B's tables (`gba_cc_lut` 64 KB, option defs) and data |
| .bss | 5,519,616 | 9,722,112 | **+4,202,496** | B's SMALL translation cache 2.4 MiB (+ way alignment/slack), `rom_branch_hash` 256 KiB, `gamepak_backup` 128 KiB, sound buffer 256 KiB, sprite lists, guest RAM (EWRAM/IWRAM/VRAM/OAM/palette/IO), memory maps; the frontend's session buffers (`q` 64 KiB) |
| **image** | 7,624,960 | 13,031,680 | **+5,406,720** | heap at boot on the PSP-1000: 16.47 MB -> 11.06 MB |

All of it is static because instance B is the core archive linked into the EBOOT under renamed symbols (section 3). Making it session-owned means taking B out of the image:
- **bss (4.2 MB):** B's arrays and its translation cache would have to come from the heap at session start. On the 1000 the heap is empty by then (the ROM cache takes 1 MiB blocks until malloc fails), so session start must evict ROM-cache blocks. When the cart was resident, that is a resident-to-paged switch mid-game: the emitter's stubs are rebuilt for swapping (`init_emitter(gamepak_must_swap())`) and every translation is flushed. That is the path the resident-ROM I-cache derails came from (section 13; the kernel's >= 16 KiB I-cache range no-op).
- **text (1.0 MB) + data:** B's code itself would have to be loaded at session start: a relocating loader for the partially linked object (MIPS REL HI16/LO16/26/32 relocations, an import table for its libc and syscall-stub references, an indirection table for the frontend's ~40 `gbab_*` references), with a whole I-cache invalidate after loading.

### 17.7 hw4: unpinned wins for two cores

- hw4 complete (8 runs, chain ended 15:30; consoles parked). Every run PASSed: all periodic hashes, FINAL equal. The Super Circuit race reached `host_lap1` / `join_lap1` at the same frames on both consoles in both arms (f=12314/12315 host side, 12681/12682 join side, as in PPSSPP).
- **Unpinned (U, V) is the fastest arm on both games: 59.2 fps AW2, 58.9 fps Super Circuit.**
- Moving B's translation cache within the pinned layout recovers most of the host's loss (P: +0.74 ms; Q: +0.43 ms), but the 1000 gets worse (P +2.35 ms, Q +0.75 ms). Its SMALL tier carries 14.6 KB of swap stubs (3000: 11.2 KB), so no single offset clears both.
- The pinned layout's hot order and JIT offset were tuned on the single-core twin. With two cores the unpinned heap/.bss placement happens to land better on both consoles. A two-core lpsim (twin running the mirror) would be needed to pin well. Until then, **link builds should be built with `LAYOUT_PIN=0`**. The kill switch `gba_link_jit_pin_b` stays default -1 (no effect).
- AW2's host is still the tighter console: core 13.5 ms + loop ~1.3 ms + radio threads ~3 ms is at the 16.7 ms frame.

## 18. The player menus (branch `claude/gbalink-ui`, 2026-10-03)

The "Link cable" menu entry of 14.4 is built: Wireless > Host / Join with the mode chosen by the game (`frontend-common/link_games.h`), rooms named `GCnnXXXX` (GBA cable) / `GBnnXXXX` (GB) beside the unchanged `GPSPnn`, the cable protocol string `GBAdhoc cable v1` (harness role sessions too: both ends of a rig must be built from that branch or later), a sampled ROM identity, and a lost link that lets each player carry on. See docs/GBA-LINK-UI.md.

## 19. A cable session owns instance B's memory (branch `claude/gbalink-sessmem`, 2026-10-03)

The owner's rule: link support must not take memory from single player. Section 17.6 measured the cost of linking instance B in: 5.4 MB of image, 16.47 -> 11.06 MB of heap at boot on the PSP-1000.

### 19.1 Design

- **B's zero-initialised state is lent to the ROM cache.** `psp/gbalink_core_b.ld` (the partial link's script) marks B's `.data` and its `.bss`, and rounds the `.bss` up to a whole 32 KiB ROM page. While no session runs, that block is the ROM cache's second spare pool (`gba_memory.c`), so a single-player game gets it back: its whole MiB as blocks, the remainder as the cache's last, partial block (`gamepak_buffer_pages`). Until cand2 the `.bss` was padded to whole MiB; cand2's SMC_REUSE storage took B to 4,453,156 B, the pad to 5 MiB put 789,724 idle bytes in the image (idle during every session) and the PSP-1000 boot heap fell to 8.47 MB. The pad's only reason was the 1 MiB allocation unit; the cache was already managed in 32 KiB pages. Exact size: PSP-1000 boot heap 9,254,328 B (lab) / 9,737,656 B (release), B's address and bytes unchanged.
- **B's `.data` is restored at every session start.** It is copied once at boot (`bdata_pristine`, after the lent block). B's block is zeroed and its `.data` put back before every load of B, so each session's B starts from exactly the bytes a fresh boot gives it. Correct by construction; 19.3 proves it as well.
- **Session start is a controlled reload, not a mid-game switch** (`fe_host.c gba_session_mem`):
  1. Hold the battery writer.
  2. Serialise A and copy its battery to a scratch.
  3. Unload and deinit A's core and release its translation caches (`dynarec_release_translation_caches`, from `claude/inproc-return`).
  4. Withdraw the pool.
  5. Reload A exactly as a boot loads it, with the session's own reserve taken before the ROM cache.
  6. Restore A's state and battery.
  7. Load B fresh: it shares A's ROM cache.
- **Session end runs the reverse**, after the session has restored A (agreed end, carry on, or back to the start): B unloads completely (`fe_gbadual_unload`), A is reloaded with the block lent again, and A's state is restored.
- **The scratch:**
  - On the SMALL tier (the PSP-1000): the tail of A's own 2 MiB static code array. Its translations are discarded by the reload, and the emitter writes from the array's start.
  - On the LARGE tier: a heap block, freed after the restore.
  - No static scratch: the first version kept 576 KiB in the image.
- **The same game, reloaded:**
  - The reload replays the boot's translation-cache tier (`jit_cache_replay`). Without it, the 64 MiB model's LARGE probe failed against the first load's frontend allocations, and the game came back on the SMALL tier.
  - It skips the post-load reserve (`gamepak_reload`). That reserve keeps room for the frontend's per-game allocations, which still stand.
  - It caps the ROM cache at the boot's block count.
- **The battery writer is held across the reload.** It scans and writes the cartridge RAM on its own thread, and between A's unload and the battery restore that RAM is not the save. Without the hold, a scan there would write the reset RAM to the card. The 20-cycle census found this as a stray 1,136 B FILE: the writer had opened `aw2.sav` mid-cycle.
- **Failure path.** If any allocation fails, the single-player configuration is reloaded from the scratch and the player sees "Not enough memory for link play". If the game itself cannot be reloaded, the app exits (`gblink_reload_failed`). Never a half-session.
- **The screen.** During the reload the player sees the LINKING screen (`ui_linking_draw`, docs/GBA-LINK-UI.md) with the ring turning per stage, no frozen game frame, and the ME renderer suspended.
- **The boot-time 2.5 MiB cable reserve is gone in late mode.** The session's reserve (2,560 KiB, harness `cable_reserve_kb`) is taken inside the start's reload, between the tier and the ROM cache.
- **Where it is on.** Late mode is always on in a player's build. Harness: `gba_dual_late = 1`, otherwise the rigs' role sessions keep B loaded at boot as before. Kill switch for the lending: `gba_link_lend = 0`.

### 19.2 Memory

| | without link | link, B static (hw4) | link, B session-owned |
|---|---|---|---|
| release image (bytes) | 7,614,144 | 13,031,680 | 13,207,488, of which 4,194,304 is lent to the ROM cache |

Single-player ROM-cache blocks (AW2 and Heart & Soul, both memory models) and the 1000's session blocks: table 19.4.

A single-player game on a link build keeps everything but B's code and data: 1,399,040 B (13,207,488 - 7,614,144 - 4,194,304), i.e. one ROM block on the 1000.

**The 20-cycle census** (harness `sessmem_cycles = 20`; mallinfo plus the heap budget after every session start and end; both memory models; AW2):
- After every one of the 20 ends, live bytes, total free bytes and ROM blocks equal the pre-session values exactly:
  - 32 MiB: 9,522,664 live, 1,019,928 free, 12 blocks.
  - 64 MiB: 21,581,320 live, 22,515,704 free, 15 blocks.
- Every session start has the same values: 32 MiB 6 blocks; 64 MiB 15.
- The largest free block is larger after a cycle than before (1,015,808 vs 966,656 on the 32 MiB model): the reloaded layout leaves free space more contiguous than the boot's, never less.
- Reload time in PPSSPP: start 0.74-1.08 s, end 1.04 s. Hardware gives the real figure (hw5).

### 19.3 B's state between sessions: correct by construction, then proven

Before every load of B, `fe_gbadual_load` zeroes B's `.bss` block and copies B's `.data` back from `bdata_pristine`. That copy is taken at boot, before B has run, so every session's B starts from a fresh boot's bytes, whatever the previous session or the lending left in them.

The proof is harness `gblink_repeat = 1`: two complete sessions in one process, the second from the same saved state and the script's first line.

| run | where | session 1 FINAL | session 2 FINAL | frame-14400 hash, both sessions |
|---|---|---|---|---|
| rig5 T | PPSSPP pair | PASS 60/60 | PASS 60/60, `b9eedcc3` @ 14569 (end frame radio-timed, 15.2) | `60aa80930892fee5` both |
| hw5 L run 1 | PSP-3000 + PSP-1000 | `a2eb2b47a9241ad3` @ 14625 | `42540f3fce9f6768` @ 14622 (end request landed 3 frames earlier) | `60aa80930892fee5` both |
| hw5 L run 4 | PSP-3000 + PSP-1000 | `a2eb2b47a9241ad3` @ 14625 | **`a2eb2b47a9241ad3` @ 14625** | `60aa80930892fee5` both |

- The FINAL frame depends on when the host's end request lands, which is radio timing; the game does not.
- Every second session's periodic hashes all matched, and its frame-14400 hash is the fresh-boot golden.
- When the end landed on the same frame (hw5 run 4), the second session's FINAL is the fresh-boot FINAL, bit for bit, on both consoles.

### 19.4 Single-player ROM-cache blocks with B's block lent (PPSSPP, `s12` harness builds at be2ca8b)

| game | model | without link | link, B lent (late mode) | link, B static (hw4: B loaded at boot + 2.5 MiB boot reserve) |
|---|---|---|---|---|
| AW2 (8 MiB) | PSP-1000 (32 MiB) | 13, resident | **12, resident** | 5, paged (hw4 log) |
| AW2 | 64 MiB | 15, resident | **15, resident** | 15 |
| Heart & Soul (32 MiB) | PSP-1000 | 13, paged | **12, paged** | - |
| Heart & Soul | 64 MiB | 32, resident | **32, resident** | - |

- What single-player loses on a link build is B's code and data, 1,399,040 B (13,207,488 - 7,614,144 - 4,194,304 lent, release images): one ROM block on the 1000 and none on the 64 MiB model. Heap free after the load:
  - 1000: 1,019,928 vs 1,430,728 B without link;
  - 64 MiB: 22.5 MB vs 24.0 MB with AW2, 4.69 MB vs 6.15 MB with H&S.
- Heart & Soul stays fully resident (32 blocks) on the 64 MiB model with the pool lent.
  - On the real PSP-3000, the inproc soak found H&S gets 31 of 32 blocks (that 3000 has less user memory than PPSSPP's model, ROM-RESIDENCY.md).
  - With the lending, the 64 MiB build carves those blocks from B's 4 MiB first, so the 3000 should need the heap for 4 fewer blocks than a non-link build of the same tier. Not measured on hardware: the soak's arms were non-link builds.
- During a session on the 1000, A's ROM cache is 6 blocks (session census), against 5 with B static, because the boot reserve is gone.

### 19.5 hw5: session-owned memory on hardware (rig6, `s12` builds = be2ca8b, unpinned)

Arms on the same binaries:
- **L:** late mode (B's memory is the session's), AW2, two sessions per run (`gblink_repeat = 1`).
- **E:** B loaded at boot, the hw4 arrangement.
- **M / N:** Super Circuit race, late / early.

Order LEELMN, 6 runs, chain 18:56-19:46, **all PASS**.

| arm | runs | fps | 3000 work / core | 1000 work / core | core vs E (host / join, window-matched) |
|---|---|---|---|---|---|
| E early, AW2 | 2 | 59.3 / 59.4 | 14.75 / 14.01 | 15.43 / 14.56 | control |
| **L late, AW2** | 2 (4 sessions) | **58.9 / 59.3** | 14.69 / 14.14 | 15.51 / 14.63 | +0.19 / +0.14 ms |
| N early, Super Circuit | 1 | 57.6 | 15.76 / 15.15 | 15.12 / 14.67 | +1.16 / +0.13 |
| **M late, Super Circuit** | 1 | **57.8** | 15.85 / 15.19 | 15.19 / 14.54 | +1.21 / -0.00 |

- **Session speed is unchanged by late mode**, within noise: L vs E +0.1-0.2 ms core, M vs N within 0.05 ms. The 1000's session translation cache is A's static SMALL array as before, so it lands in the same place.
- **Reload time on hardware** (`EVT gba_sessmem ... ms=`, every session):

| | session start | session end |
|---|---|---|
| PSP-3000, AW2 (8 MiB, resident, LARGE tier) | 1.90-1.97 s | 1.81-1.83 s |
| PSP-1000, AW2 (12 blocks before, 6 during) | 1.36-1.38 s | 1.62-1.65 s |
| PSP-3000, Super Circuit (4 MiB) | 1.10 s | 1.01 s |
| PSP-1000, Super Circuit | 1.00 s | 0.89 s |

  It is mostly the ROM read from the Memory Stick (8 MiB in ~1.8 s on the 3000). The LINKING screen is up for all of it.
- **The 1000 pages less during an AW2 session:** `pg/min` 5.0 in L against 10.9 in E, because without the boot reserve the session's ROM cache is one block larger.
- **The second session of every L run hashes as a fresh boot's** (19.3). The 20-cycle heap census is PPSSPP (19.2).

### 19.6 PPSSPP gates (all on the `s11`/`s12` builds)

- **rig5 pairs (late mode):**
  - S: AW2, PASS 60/60.
  - T: AW2, two sessions, both PASS 60/60, golden 14400 hash.
  - R: Super Circuit, PASS 57/57, both karts lap 1 at f=12314/12315 and 12681/12682, as in every earlier build.
- **Multigame matrix with `gba_dual_late = 1`** on every fixture (`MG_EXTRA_INI`, `~/gbalink-pp/mx-s11`):
  - aw2, em, emb, msc, mt, sa1 PASS under loopback, jitter and loss.
  - mg GAME-FAIL (the link never forms, open since 16.4).
  - The end control passes.
- **Single-player identity:** the Emerald video oracle on the link harness64 (lent pool, 16 blocks) is 1,091 frames pixel-identical to the non-link build's capture.

### 19.7 Open

- The release build's single-player block count, frame identity and audio were measured on the harness flavours; the release flavour differs only in telemetry and harness keys (19.2 image numbers are release).
- The failure path ("Not enough memory for link play", single-player restored) is exercised only by construction. No run has made an allocation fail.
  - A harness knob that fails the session hold on purpose would prove it on both models.
  - Not done here: the coordinator asked for no further hardware round, and it is cheap in PPSSPP for whoever integrates the RC.
- B's 1.4 MB of code and data remain static (a relocating loader was out of scope).

### 19.8 Builds

All from `claude/gbalink-sessmem`, unpinned (`LAYOUT_PIN=0`):

| build | commit | EBOOT md5 |
|---|---|---|
| harness (1000), link | be2ca8b | 6e309b92 (`builds/gbalink-hw-out/s12-link-harness`) |
| harness64 (3000), link | be2ca8b | f4d87d7f (`s12-link-harness64`) |
| harness / harness64, no link (comparison) | be2ca8b | 037d85a1 / 4fa02304 |
| release, link | ce5b7d1 | 07ef13e5 (`r6-link`; image 13,207,488) |
| release, no link | ce5b7d1 | 8f214683 (`r6-nolink`; image 7,614,144) |

- be2ca8b differs from ce5b7d1 only in `tools/gbamirror/multigame/run_fixture_ppsspp.sh`.
- Commits after be2ca8b are docs only.
- A release build must be made with `LAYOUT_PIN=0 GBA_LINK=1`: `tools/build.sh` still defaults `LAYOUT_PIN` to 1, which section 17.7 measured as slower for two cores.

### 19.9 The 70,672 B per-session leak (branch `claude/linkleak`, 2026-10-30)

The 20-cycle census of 19.2 was flat at be2ca8b. XS_HEAP (cpu-levers, later) moved the SMC_SAME_SKIP / SMC_RETIRE_LOCAL tables into one heap block, 70,656 B (70,672 with the malloc header). `dynarec_heap_tables_init` kept the block's pointer in a function static, in `.bss`. For instance B, that `.bss` is zeroed by `b_fresh` and lent to A's ROM cache between sessions. So every session start saw a NULL pointer and took a new block, and the previous block was orphaned. The heap trace (`HEAP_TRACE=1`) named it: one new live 70,656 B block per cycle, from `gbab_dynarec_heap_tables_init`.

- **Leak, not fragmentation.** Live bytes rose 70,672 B a cycle, and the largest free block shrank with them: 1,675,264, then 1,597,440, then 1,527,808 B.
- **Effect on the PSP-1000 model.** The 4th session start failed: A's reload could not allocate its 77,280 B frame buffer, and the player saw "Not enough memory for link play".
- **Fix.** `dynarec_heap_tables_release()` frees the block. `fe_gbadual_unload` calls B's copy. A never releases its block, and every reload reuses it.
- **After the fix** (PSP-1000 model, `sessmem_cycles = 12`, Emerald and AW2): after every end, the census has 7,500,216 live bytes, 1,754,184 B free and 11 blocks, the same as before the first session. The largest free block is 1,748,992 B every time.
- **Real late-mode pair** (AW2, `gblink_repeat = 2`, host on the 64 MiB model, join on the 1000):
  - At `gblink_running`, free heap was 890,088, then 1,868,008, then 1,797,336 B before the fix, and 890,088, then 1,938,680, then 1,938,680 B after it.
  - The emulated-frame shash (30,000 frames on each console) and `gbadual_hash` are identical to the unfixed build.

## 20. Per-mode translation-cache offsets: single player pinned, a cable session at its own (commit bb20b69)

Pinning is worth -21 % AW2 core and -4 % H&S in single player on the 3000; two cores pinned at the same offset lose ~2 fps (17.7). A session start already reloads instance A, so the offset can be chosen per mode.

**Why it is a clean extension (owner's stability rule).**
- The offset is a parameter read where the caches are created: `dynarec_select_translation_caches`, after the reload's `dynarec_release_translation_caches`.
- SMALL: `jit_small_rebase` into the static arrays' existing slack, which B has used since 17.5. LARGE: the block is allocated one way larger and its base offset.
- Nothing live moves. The stubs are emitted fresh at the new base by the same load path a boot takes, and every load already runs the discipline's whole-I-cache ownership invalidates (`JOWN_INIT`, `JOWN_REBUILD`). No emitted code is patched; no new cache maintenance.
- **Kill switch:** keys `gba_link_pin_{a,b}_{small,large}` (config.ini; harness overrides). The default -1 everywhere is today's behaviour: pinned at `JIT_PIN_OFFSET` in both modes. The session's end reloads A with -1.

**PPSSPP:**
- 20-cycle census (pinned link build, A 0x0500 / B 0x1500 on SMALL, 0x0c00 / 0x1840 on LARGE): live bytes, free bytes and ROM blocks exact after all 20 on both models, offsets applied every time (`a_mod8k`, `gbadual_jit mod8k`).
- Pair PASS: rig7 A (offsets) and K (defaults) both FINAL `a2eb2b47` @ 14625, 60/60.
- Single player: the pinned link harness64 is 1,091 frames pixel-identical to the pinned no-link build (Emerald video oracle). The pinned no-link capture equals the unpinned one.

**Instance A's layout in the pinned link build vs pinned no-link** (`nm`, harness64):
- The dispatcher and asm stubs are at the same addresses.
- Hot C functions shift by 32-544 B: the cable branches in `serial_next_event`, `update_gba`, `sound_timer`, and `fe_host_run_frame` +480 B.
- The tail of the hot order (from `write` on) shifts ~3.9 KB.
- Hardware single-player core, pinned link vs pinned no-link, is the next round's solo arms.

### 20.1 hw6: the offset sweep (rig7, pinned link EBOOTs harness64 40aded2a / harness 284c98d3 = bb20b69; AW2, late mode, shash 0)

- One pinned EBOOT pair; the arms differ only in `gba_link_pin_*`, applied to both tiers alike.
- Each console's core depends only on its own layout, so the 3000 column measures the LARGE tier and the 1000 column the SMALL tier.
- All 9 runs PASS (60/60, FINAL equal).

| arm | A / B offset | fps | 3000 core (LARGE) | vs U | 1000 core (SMALL) | vs U |
|---|---|---|---|---|---|---|
| U | unpinned build (control, 2 runs) | 59.2 | 13.76 | - | 14.92 | - |
| K | pinned defaults 0x1500 / 0x1500 | 57.8 | **13.11** | **-0.65** | 15.54 | +0.62 |
| A | 0x0c00 / 0x1840 | 58.9 | 13.94 | +0.18 | 15.12 | +0.20 |
| B | 0x1300 / 0x00c0 | 58.0 | 14.54 | +0.79 | 15.47 | +0.55 |
| C | 0x1800 / 0x0800 | 57.0 | 14.96 | +1.20 | 15.86 | +0.94 |
| D | 0x0800 / 0x1800 | **59.7** | 13.74 | -0.02 | 14.34 | -0.58 |
| E | 0x1000 / 0x0000 | **59.7** | 14.77 | +1.02 | **13.79** | **-1.13** |
| G | 0x0400 / 0x1400 | 57.4 | 14.40 | +0.65 | 15.68 | +0.76 |

(core = core_prof ms/frame; vs U = window-matched difference, ms.)

**Per tier:**
- LARGE (3000/Go): the pinned defaults are best (-0.65 ms vs unpinned): keep -1.
- SMALL (1000): E (A 0x1000, B 0x0000) is best (-1.13 ms); D is second (-0.58).
- That combination, W, is checked in hw7 on AW2 and Super Circuit against unpinned.

### 20.2 hw7: the winner, Super Circuit, and single player on the pinned link build (rig8, same EBOOTs; chain 00:19-00:46, all PASS)

**W:** SMALL A 0x1000 / B 0x0000, LARGE at the pinned defaults. Logs confirm A/B at 0x1500/0x1500 on the 3000 and 0x1000/0x0000 on the 1000.

| arm | game | fps | 3000 core | 1000 core |
|---|---|---|---|---|
| W | AW2 | **59.8** | 13.38 | 13.91 |
| (hw6 U, unpinned) | AW2 | 59.2 | 13.76 | 14.92 |
| X = W's offsets | Super Circuit | **59.0** | 14.59 | 14.54 |
| V, unpinned | Super Circuit | 58.2 | 14.99 | 14.78 |

- X vs V window-matched core: 3000 -0.40, 1000 -0.24 ms.
- X: both karts lap 1 at the usual frames on both consoles.
- **Shipped as the defaults** (commit 76185bb, one `#define` block in `psp/main_psp.c`).

**Single player, pinned link build vs pinned no-link build** (same AW2 state and script, 3,600 frames, core_prof per 600-frame window after boot):

| | 3000 core | 1000 core |
|---|---|---|
| P: pinned link (B's block lent) | 4.54 ms | 4.52 ms |
| Q: pinned no-link | 5.07 ms | 5.05 ms |

- B's presence does not disturb A's pin; this layout is in fact 0.5 ms faster, window for window.

**ROM-cache blocks on the real consoles** (the harness `rom_cache` line):

| | PSP-3000 | PSP-1000 |
|---|---|---|
| AW2, pinned no-link | 15, resident | 13, resident |
| AW2, pinned link, B lent | 15, resident | **11**, resident |
| Heart & Soul, pinned link, B lent | **got 30 of 32** -> paged, 15 blocks | got 11 -> paged, 11 |

- **On real hardware the link build costs the 1000 two ROM blocks, not one** (PPSSPP's 1000 model: 13 -> 12):
  - The pinned link image is 13,588,736 B against 7,919,872 B (+5.67 MB), of which 4 MiB is lent back.
  - After the load the 1000 has 2.03 MB free but its largest block is 1,044,480 B, just under a 1 MiB block plus header. One of the two is fragmentation.
- **Heart & Soul on the 3000 gets 30 of its 32 blocks with B's pool lent.** The soak's non-link build got 31. Neither is resident on a real 3000; the link build is one block further. Both page at the 15-block cap.
- What would close it is not in the lending: B's 1.4 MB of code and data are still static (19.7).

## 21. "The session costs 2.5x the bench": it did not, the bench was one machine (2026-10-10, branch `claude/link-speed`)

**The measurement.** PSP-3000, build c8b0b36, Super Street Fighter II Turbo Revival, same scripts.
- One-console bench `r3_1_sf` (`gba_dual = 1`, `gba_dual_autopilot`, lookahead 140448): 59.7 fps, `core_prof` ~10.3 ms in the fight.
- Two-console role session `lk_sf_g1` (3000 + Go): ~40 fps, `core_prof` ~25.3 ms in the fight.

### 21.1 Cause

**The bench never started its pair.** The bench pair starts in `fe_gbadual_state_loaded`, i.e. when the player's machine loads a state. `r3_1_sf/harness.ini` has no `load_state`, so instance B was loaded and then never run: the log has `gbadual_load ok` and no `gbadual_start`, and (the detail that was noticed) no `gbadual frames=... switches=` line, because that line is only printed for a running pair. None of the 32 one-console results with `gba_dual = 1` under `builds/fix351-out/results` (`*_sf`, `sf_dual_*`) has a `gbadual_start`. The 10.3 ms is ONE machine, drawn and heard, with nothing on its port. The session's 25.3 ms is two machines in a linked fight. Nothing was sliced, blocked or spun differently: the two runs were not the same work.

**Off the console (PPSSPP, modelled time, the owner's ini byte for byte; `tools/gbamirror/sf_bench_ppsspp.sh`):**

| arm | `gbadual_start` | core_prof per 600-frame window (us) |
|---|---|---|
| the owner's ini (no state) | absent | 3475 3589 3805 6127 7409 7318 7349 7411 ... |
| + a power-on state (`load_state = 1`, `sf_p0.st0`) | present | 6679 7296 9087 10723 10464 10505 10470 ... |
| + `gba_dual_cold = 1` (new, no state file) | present | 6360 6655 7413 9106 10636 10428 10462 ... |
| two-PPSSPP session, host (`sf.fixture --nohash`) | (session) | 6281 6828 7467 9208 10814 10595 10556 ... |

- The pair really running costs what the session costs: 10.45 ms bench against 10.6 ms session in the fight. In modelled time the session layer adds ~0.15 ms.
- The running bench prints `switches=144355 blocked=62832/0 mp=62832/62831` at f=7770; the hardware session printed `switches=144299 blocked=62796/0 mp=62796/62795`. The same slicing, to 0.04 %.
- PPSSPP has no second core, so machine A is CPU-rendered there: the absolute numbers are not the PSP's, only the comparison between arms is.

**Why two machines are 2.5x one, not 2.0x.** Not the same scene. Desktop mirror census (`update_gba` instrumented in a scratch copy, x86 engine), CPU-active cycles per machine per frame out of 280,896:

| frames | pair, each machine | what it is |
|---|---|---|
| 0-1799 (logos, menus) | 70,000-89,000 | the same title sequence the solo run plays |
| 1800-8400 (VS menus, the fight) | 152,000-166,000 | linked, 8 multi-player transfers a frame |

- Each machine works 56 % of every frame in the fight.
- No wait loop: the hottest guest address after the BIOS interrupt path holds 7 % of the active cycles. The game takes an **H-blank interrupt on every scanline** (228 a frame per machine; IE = VBlank + HBlank + timer 3 on the parent / serial on the child); ~15 % of the active cycles end in the BIOS's interrupt return.
- The candidates, one by one:
  - **Slicing** is the same in bench and session, and cheap. AW2 on the 3000 (the note in `gbalink_pair.h`): 402 more switches a frame cost 7.1 ms, 17.7 us each. SF's 18.6 a frame are ~0.3 ms.
  - **`blocked`** is the parent waiting for the child's word at each transfer's end: two switches per transfer, by construction.
  - **Input delay** does not reach the machines; the pair stalls outside them.
  - **`SERIAL_IDLE_FAST`** leaves the cable mode alone in both runs.
  - **Partner audio** is off and the **save watch** counts writes in both.

**What the hardware numbers then say (estimate, not measured).** The session's 25.3 ms is wall time on the emulation thread, so it includes the radio threads that preempt it: section 17.5 measured ~3.4 ms of them per 16.7 ms frame in an AW2 session. At 40 fps that is 3.4-5 ms of the 25.3. The two machines are therefore ~20-22 ms, ~10-11 ms each, about what one drawn machine cost alone. The one-console pair bench on the 3000 (21.4) will say.

### 21.2 What changed (commit 679fb92; all off by default)

- **`gba_dual_cold = 1`** (harness): the bench starts its pair before the first frame, both machines from the player's power-on machine (serialized, loaded into both: what a state file does). No state file to stage, no 30-frame offset in the scripts.
- **`EVT gbadual_idle ... WARNING`** once at frame 120 and **`gbadual active=0`** on every heartbeat when a bench has `gba_dual = 1` and no pair running. A one-machine bench can no longer be read as a pair.
- **`gba_link_fast = 1`** (harness, or config.ini; `gbalink_core.c gbalink_fast`, one copy per instance, both set by `fe_gbadual_load`): the cable fast path. `update_gba` runs ~1,150 scheduler iterations per machine per frame, and in cable mode each one walked the bus twice (`lk_apply_due` in `gbalink_update_serial`, `gbalink_bus_peek` in `gbalink_next_event`). With the switch on, both answer from the port's own state when nothing can be due.
  - **Why it is the same machine.** The queue of events for a port changes only while the *other* core runs or is pumped, or when this port pops what it applied. So the time of the first waiting event (`lk.in_t`, taken at the end of every `lk_apply_due`) is exact for as long as this core runs, and every way into running goes through `lk_run` -> `lk_pump` -> `lk_apply_due`. When no event is due and no transfer ends, the full path only looks: `lk_take_replies` moves a queued reply into `lk.reply_*`, which nothing reads before the transfer's end or the entry check, and the entry check takes it itself.
  - `gbadual_start` / `gbadual_session_start` now end `fast=A/B`, and `gba_dual_cfg` prints `fast= cold=`.
  - Both consoles should run the same setting (that is what was tested). A mixed pair would still match, since the switch changes neither machine, but it has not been run.

### 21.3 Evidence

| check | result |
|---|---|
| Desktop mirror, per-frame hashes of both machines + every cable counter, `--fast` vs not (`multigame/fast_identity.sh`) | byte-identical on aw2, em, emb, mg, msc, mt, sa1, sf (14,999-39,999 lines each; multi-player, normal 32-bit, general-purpose and JOY BUS modes; bus depth up to 10) |
| PPSSPP one console, SF, `gba_dual_hash`, fast vs not | `gbadual_hash.txt` identical, 15,685 lines |
| Two PPSSPP consoles, per-frame hashes, fast on: `sf`, `aw2`, `msc` | PASS: every periodic HASH, FINAL equal, per-frame files identical, the golden frame equal to the fixture's (recorded with the switch off) |
| Two PPSSPP consoles, the unchanged path (fast off): `aw2`, `sf` | PASS (AW2 FINAL `a2eb2b47a9241ad3` @ 14625, as every build since section 15) |
| `tools/run_host_tests.py` | 30 passed, 0 failed, 0 skipped |

**Cost, PPSSPP modelled time (no cache model; compare arms only):**

| | off | fast | |
|---|---|---|---|
| SF one-console pair, fight windows | 10.45 ms | 10.02 ms | -4.1 % |
| SF one-console pair, menus (first window) | 6.36 | 5.97 | -6.1 % |
| SF session `--nohash`, host / join, whole run | 9.75 / 9.28 | 9.32 / 8.82 | -4.4 % / -5.0 % |
| AW2 session `--nohash`, host / join | 8.24 / 8.33 | 7.88 / 7.96 | -4.4 % / -4.4 % |

On hardware the saving should be about the same share (the skipped code is two calls into another object per iteration): ~1 ms of SF's 25. **Not measured on a PSP.** **Update: the one hardware run with the switch on froze the console; see section 22 before using it.**

### 21.4 Hardware: what to run

PRX: `builds/linkspeed-out/psp/ls-prx` (`gbadhoc.prx` md5 9d5374a9, `gbadhoc_me.prx` md5 2f252eeb = the c8b0b36 module, sha256 63d8c5a4; built clean from 9868cbf, `harness` + `GBA_LINK=1 BUILD_PRX=1`).

1. **The real one-console pair** (says how much of the 25.3 ms is radio). Stage the PRX pair as a `plrun.sh` arm and add one line to the existing row:
   `r <tag> <arm> sf.gba - - sf_host.inputs 8600 1 "gba_dual = 1" "gba_dual_autopilot = sf_join.txt" "gba_dual_lookahead = 140448" "gba_dual_cold = 1"`
   Read `EVT gbadual_cold size=425984 ok=1`, `EVT gbadual_start ... fast=0/0`, `EVT gbadual frames=...switches=` (~18.6 a frame) and `core_prof`. Estimate: 20-22 ms in the fight, 45-50 fps. Add `"gba_link_fast = 1"` for the fast arm.
2. **The session, control then fast.** `link_hw.py` audits keys against `../rc400-worktree` by default and refuses the new key, so `--src` is required (checked with `--dry`):
   `python link_hw.py ls_sf_c1 3000 go sf --nohash --prx ../linkspeed-out/psp/ls-prx --src ../linkspeed-worktree`
   `python link_hw.py ls_sf_f1 3000 go sf --nohash --prx ../linkspeed-out/psp/ls-prx --src ../linkspeed-worktree --ini "gba_link_fast = 1"`
   Read `gbadual_session_start ... fast=1/1` on both consoles, `core_prof core=`, `gblink_win work_us/core_us`, and the verdict's HASH n/n and FINAL. Then the same two lines with `aw2` and `msc`.

### 21.5 What still stands between this and 59.7 fps (estimates unless marked)

A session frame must fit 16.7 ms of emulation-thread wall time. SF's is 24.7-26.6 ms (measured, `gblink_win work_us`).

| item | size | basis |
|---|---|---|
| the two machines | ~20-22 ms | estimate (21.1); the cold bench measures it |
| radio and audio threads preempting | ~3.4-5 ms | measured for AW2 (17.5), scaled to 40 fps |
| session loop outside the machines | ~1.0-1.2 ms | measured (`work_us - core_us`) |
| cable fast path | -1 ms | PPSSPP -4 %, not on hardware |
| two-core layout (`gba_link_pin_{a,b}_large`) | +-1 to 2.5 ms | measured for AW2 (17.5, 20.1); SF never swept |

- **Sound.** The emulator's own mixing for one machine is 0.22 ms in PPSSPP (measured: `gba_link_headless_audio = 0` puts the partner's back, 10.45 -> 10.67 ms). The partner already skips it, and the player's has to be heard. The game's sound engine is guest code: DirectSound A and B on timer 0, fed by the game. No mixer shows in the profile: the IWRAM routines in the top 25 hold ~4.5 % of the active cycles, so the engine is a few percent of a machine at most, well under 1 ms (estimate from event-boundary sampling, not an exact profile).
- **No general fast path exists for the partner beyond what it has** (no rendering, no mixing). The partner on this console is the player's machine on the other one, and the contract is that both consoles compute it bit for bit, sound engine and its RAM included. Skipping or approximating any guest code on the partner diverges the hashes. An exact native sound engine is per-engine work (M4A was one; SF's is Capcom's own), not general.
- **What is general and left:** the per-machine machinery the dynarec study ranked (`update_gba` was 30 % of the AW2 pair's core in section 16.2; SF's 228 H-blank interrupts a frame per machine lean on that path and on the interrupt entry and return), and the layout sweep for SF on hardware. Neither is a switch.
- **SF at 59.7 fps in a session needs the pair near 11 ms; it is near 20. That is not reachable by tuning the link.** The only structural route is the second core: the partner machine on the Media Engine, which the renderer fully occupies today (section 16.2: ~10.5 ms a frame). That is a redesign, not an optimisation.

## 22. A PSP-3000 froze with `gba_link_fast = 1`: what is known, the checking mode, the guards (2026-10-10)

**`gba_link_fast` is NOT cleared for use. It stays 0 by default; do not run `= 1` on a console again until `= 2` (below) has run clean on hardware, with the owner present.**

### 22.1 What happened

The first two one-console SF pair runs ever made on hardware (section 21.4, PRX 9d5374a9, `gba_dual_cold = 1`):

| | control `ls_sfb_c` | fast `ls_sfb_f` (`gba_link_fast = 1`) |
|---|---|---|
| core_prof, first three windows | 14023 14932 16239 us | 12500 13493 14834 us (-9 to -11 %) |
| fight | 22.7 ms, 43.5 fps | not logged |
| `frontend.log` reaches | f=4200 contiguous, then 378 lines dropped, exit code 0 | f=2189, then nothing |
| `heartbeat.txt` (0x10 thread, raw sceIo) | to f=7651, the end | last line f=4452 (`pair=on run=1 step_runs=18 bbj=0`, healthy), then nothing |
| after | exit | PSPLink `reset` dead; needs a power cycle |

- So the pair's real one-console cost is **22.7 ms** in the fight (section 21.1 estimated 20-22), and the fast path's saving on hardware was **9-11 %** in the menus, about twice PPSSPP's figure.
- `stall_watch_s` was 0 in both runs, so no stall record was possible: there is no evidence of where the main thread was.

### 22.2 Mechanism: not found. What the code can and cannot do

**Not reproduced off the console, and no disagreement found** (22.4). The frame it froze at (4452) is inside the range where PPSSPP's MIPS build is hash-identical with and without the switch.

- **What the fast path and the pair can produce.** They are user-mode C with no system calls, no interrupt masking, and writes only to the instance's own `lk` struct. They cannot halt the CPU, spin with interrupts off, or enter the kernel. The worst a wrong cable answer can do is (a) change a machine, which the hashes would show, or (b) keep the emulation thread busy.
- **Could a busy emulation thread look like this freeze? Partly, and it cannot be excluded from the symptoms.** The heartbeat thread is 0x10 and the emulation thread 0x2B in these runs, so a spin does not stop the heartbeat thread from running. But its line is a `sceIoOpen`/write/close on `host0:`, and the log writer (0x2C, below the emulation thread) writes to `host0:` too. `stall_record` already documents the hazard for the Memory Stick: a starved writer parked inside the driver holding its lock makes the watchdog's own open block, "and read as no file = CPU halted". Whether usbhostfs serialises this way, and whether PSPLink's shell then dies with it, is not known here.
- **The log writer was starved in BOTH arms** from the moment the pair stopped holding 60 fps (control: lines dropped from ~f=4300 to the end; fast: nothing after f=2189). A CPU-bound pair never blocks, so nothing below 0x2B runs. The control ran in that state for 100 s and survived; the fast arm for 37 s and did not. One run each.
- **Candidates, none proved:**
  1. A hardware-only fault of the family this project has met before (stale I-cache / coherency, or the ME), exposed by different timing and a different instruction stream between translated blocks. The switch changes both.
  2. An I/O priority inversion under a never-blocking emulation thread, as above. This would be a hazard of the one-console pair bench as such, control included.
  3. A fast-path logic error that only hardware timing reaches. Every path found by inspection is covered by the invariant the checking mode tests (22.3); 3.7 billion checks off the console found none.
- **What will tell them apart on the next run:** `stall_watch_s` (thread states, loop phase and ME beat in `stall.txt`, written with the main thread dropped below the io thread, so it survives candidate 2), the checking mode's `fm=` in every heartbeat line, repeating the CONTROL arm as often as the test arm, and what the screen shows before the power cycle (a frozen picture, a moving one, or black).

### 22.3 What was built (commit 3e58ab2)

- **`gba_link_fast = 2`, the checking mode.** The full path decides every answer, so the machines are the `gba_link_fast = 0` machines by construction and the run cannot hang on the fast path's account. Beside it, on every scheduler call, the fast path's answer is computed and compared:
  - `where=1`: the cached first-event time (`lk.in_t`) is not the queue's;
  - `where=2`: the full path applied an event, raised the IRQ, posted, or changed the transfer where the fast path would have skipped;
  - `where=3`: `gbalink_next_event`'s two answers differ.
  The count per slot and the first disagreement (where, cached, true, now) go to `heartbeat.txt` on every line (`fast=2 fm=A/B w= c= t=`), because the log ring starves, and to `EVT gbalink_fast_check` / `EVT gbalink_fast_mismatch`. Cost in PPSSPP: +0.68 ms a frame (SF pair).
- **Progress guards, always on, no key.**
  - A core's `run()` that makes 2,000,000 scheduler calls yields `GBALINK_RUN_OVERRUN` (the measured maximum is 410 on all eight fixtures; a run covers at most ~421,000 cycles and every call moves the clock).
  - A pair step longer than `gba_link_step_max_ms` (harness; default 15,000, negative = off) fails `E_TIMEOUT`. The clock is read every 32 runs; a healthy step makes ~20.
  - With `STEP_RUNS_MAX`, `IDLE_MAX` and the deadlock rule of section 13, every loop in a step is now bounded.
  - A bench whose pair stopped on any error exits `code=7 reason=gbadual_failed`, with `EVT gbadual_failed err=N` and an immediate heartbeat line (`pair=failed err=N`). A session's pair error already ends it through `gblink_failed reason=start` and an ABORT to the peer.
- **`stall_exit_s = N`** (harness, default 0). When the main loop has not begun an iteration for N seconds, the 0x10 watchdog writes the stall record and a heartbeat line (`s=100000+N`), then sets `gbalink_abort` in both cores: a pair still reaching its scheduler fails `E_OVERRUN` and the run ends cleanly as above. With **`stall_exit_hard = 1`**, if the loop is still not turning 3 s later it calls `sceKernelExitGame` from the watchdog thread (heartbeat `s=200000+N` first). **The hard exit has only run in PPSSPP, never on a PSP or under PSPLink.** None of this helps if the watchdog thread itself no longer runs.
- **Stress harness.** `gl_host --stress SEED` gives every run a random horizon inside the stepping rule's limits and pumps a random machine before it (`SEED | 0x80000000`: pumps only); `--fast2` is the checking mode. `multigame/fast_stress.sh` runs off / fast / checking with the same seed and compares.

### 22.4 Evidence (off the console only)

| check | result |
|---|---|
| `fast_stress.sh`: aw2, em, emb, mg, msc, mt, sa1, sf, and SF for 40,000 frames; unstressed + 6 random-horizon seeds + 2 pump-only seeds each; off / fast / checking | 81/81 PASS: per-frame hashes of both machines byte-identical across the three, cable counters equal, `mismatch=0/0` (3.7 billion checks), no pair error; pump-only seeds hash like the unstressed run |
| PPSSPP one console, SF 7,843 frames, final EBOOT: off vs checking | `gbadual_hash.txt` identical (and identical to the pre-guard build's, cb79f80d); `checks=11548290/11499320 mismatch=0/0` |
| Guard, wall clock (`gba_link_step_max_ms = 1`) | `gbadual_step FAILED err=7`, `exit code=7 reason=gbadual_failed`, heartbeat `pair=failed err=7` |
| Guard, stall exit (`home_stall_frame = 1500`, `home_stall_ms = 9000`, `stall_exit_s = 4`) | `stall.txt` written, `err=6` (OVERRUN) when the loop resumed, `exit code=7` |
| Guard, hard exit (`home_stall_ms = 30000`, `stall_exit_hard = 1`) | heartbeat `s=100004`, `s=200004`, the process left |

### 22.5 The next hardware run (owner present)

PRX: `builds/linkspeed-out/psp/ls2-prx`. One console, the section 21.4 row, each arm with `"stall_watch_s = 10"`:

1. CONTROL x2: `... "gba_dual_cold = 1" "stall_watch_s = 10"`
2. CHECK x2: `... "gba_dual_cold = 1" "gba_link_fast = 2" "stall_watch_s = 10"`
3. Only if every heartbeat line of the CHECK runs says `fm=0/0` and all four ran to `exit code=0`: `"gba_link_fast = 1"`, with the owner at the console.

Read `heartbeat.txt` (`fast=2 fm=0/0`, `err=0`), `stall.txt` if it exists, and `EVT gbalink_fast_check ... mismatch=0/0`. If a run freezes: note what the screen shows before the power cycle.

## 23. Street Fighter's fight, exactly: the pair under the dynarec twin, the lean cable hooks, and what full speed needs (2026-10-10)

**Hardware since section 22 (PSP-3000, runs from the Memory Stick, `plrun_ms.sh`).** The section 22 freeze was candidate 2: the cable file system locked under a never-blocking emulation thread (the control froze too over `host0:`; HOME still answered). From the stick: control x2 clean (fight core 22.9-23.1 ms, 42.7-43.0 fps), checking mode x2 `mismatch=0/0`, `gba_link_fast = 1` x2 clean at 21.2 ms (-7.5 %).

### 23.1 The tool

`tools/drprof/Makefile` target `pair` builds `tools/gbamirror/gl_host.c` for the dynarec twin (the PSP's MIPS translator as a Linux/mipsel program under qemu, docs/DYNAREC-PROFILE.md) with both core instances; `drprof_plugin.so` counts every MIPS instruction. Machine A's emitted code is split by role and class as in the solo twin; machine B (the renamed copy) is counted per `gbab_*` symbol. `tools/drprof/dr_pair_report.py RUN SCENE FRAMES` prints the tables below. `--headless1` makes slot 1 the partner a console runs (no drawing, no mixing).

- **Calibration against hardware (measured both sides):** shipped pair 2,705,380 instr/frame on the twin and 22.9-23.1 ms on the 3000; `gba_link_fast = 1` 2,497,156 and 21.2 ms. That is -7.7 % against -7.5 %, and **8.5 ns per twin instruction** for this pair in both arms. Everything below converts at that rate; that conversion is the estimate, the counts are exact.
- Not in the twin: machine A's renderer runs in-process (listed, excluded: on a PSP it is on the Media Engine) and the main CPU's capture side of it (`ME_MIDFRAME_LOG`) is PSP-only code, so **the capture cost is not measured here**. SF writes one video register per scanline (BG1HOFS, below), so it is small next to the rest; `DRPROF_HW` on a console is the way to measure it.
- The twin's hashes (both machines, every frame) are the same with every switch of this section on or off.

### 23.2 The profile (SF fight, frames 3000-5200, as shipped; MIPS instructions per frame)

| | machine A (drawn) | machine B (headless) |
|---|---|---|
| emitted code, guest work | 126,119 | (414,246 for all four |
| emitted code, memory-call setup | 73,689 | emitted rows: B is one |
| emitter memory stubs | 199,187 | bucket) |
| emitted cycle update/check + dispatcher jumps | 16,523 | |
| event scheduler (`update_gba` 845 calls/frame, ~240 each; `mips_update_gba`) | 204,142 | 193,662 |
| cable hooks + pair stepper | 358,769 | 147,614 |
| interrupt entry/return in C (mode switch, CPSR, raise) | 124,840 | 124,769 |
| dispatch: indirect-branch stubs + block lookup | 89,748 | 78,440 |
| memory C slow paths (I/O writes) | 68,164 | 67,187 |
| sound | 233,367 | 95,051 |
| timers, DMA, libc, rest | 24,238 | 19,658 |
| **total** | **1,518,786** | **1,140,628** |

**Pair: 2,659,415** (this build; 2,705,380 before the direct hook calls of 23.3). Translation and flush: ~140 instructions a frame (nothing is retranslated in the fight). Renderer for A, not counted: 1,128,377.

**What Street Fighter does that Advance Wars 2 does not** (AW2 battle on the same twin: pair 1,925,832):

| per machine per frame | SF | AW2 |
|---|---|---|
| interrupts taken | 238 (228 H-blank) | ~21 |
| I/O register writes | 2,424 | ~108 |
| `set_cpu_mode` / `msr cpsr` (`execute_store_cpsr`) | 738 / 464 | 96 / 42 |
| indirect branches through the dispatcher stub | 2,153 | 1,744 |
| full block lookups (`block_lookup_address_*`) | 801 | 143 |
| interrupt C path | 124,840 | 22,857 |
| I/O writes (C + `st_*_io` stubs) | ~144,000 | ~6,000 |

- **The I/O writes are interrupt bookkeeping, not video.** Per frame (desktop census): IME 941, IE 425, IF 416, BG1HOFS 202, HALTCNT 126; everything else about once. The game's handler masks and unmasks around every one of its 238 interrupts (4 IME writes each).
- ~~Each IE/IME write returns through `write_io_epilogue` whenever an interrupt is pending, which ends the block and does a full `block_lookup_address` even when the interrupt cannot be taken; that is where the 801 lookups come from.~~ **Wrong; corrected in 24.4.**
- **Per H-blank interrupt, outside the guest's own instructions: ~1,400 MIPS instructions** (C mode switches and raise ~525, extra dispatch ~220, its I/O writes ~650). 238 x 2 machines: ~670,000 a frame, 5.7 ms on the 3000. **This is the Street Fighter tax.**
- **Memory stubs, machine A, calls per frame:** ld_u32_iwram 2,753; st_u16_io 1,514; st_u16_iwram 1,150; ld_u16_iwram 1,077; ld_a32_romB 822; st_a32_vram 702; st_u32_iwram 699; ld_s8_ewram 542; ld_u8_iwram 413; ld_u8_ewram 397; st_u8_io 387.
- **Sound.** A: `render_gbc_sound` 129,534 (the PSG channels, 5.7 calls a frame), `sound_timer` 73,425 (264 FIFO samples), read-out 28,556. B, already silent: 95,051, of which 28,556 was reading out a ring nobody hears. The guest's own sound code is inside "emitted code"; no mixer loop is visible (section 21.5).
- **Guest PCs** (desktop sampler, section 21.1): 15 % of active cycles end in the BIOS interrupt return, the largest ROM routine is 7 %, IWRAM routines ~4.5 %.
- **Halted:** 44 % of each machine's cycles. A halted machine still hops from event to event: the scheduler loop runs ~730 times a frame per machine, ~420 of them entered from running code.
- **Top 20 (pair, shipped):** B emitted code + stubs 414,246; A `update_gba` 189,199; B `update_gba` 180,045; A `render_gbc_sound` 129,534; A emitted guest work 126,119; A `lk_apply_due` 97,011; A memory-call setup 73,689; A `sound_timer` 73,425; A `st_u16_io` stub 62,086; A / B `write_io_register16` 58,362 / 57,301; A `gbalink_bus_peek` 50,535; A / B `gbalink_update_serial` 38,381 / 38,230; B / A `gbalink_next_event` 38,896 / 34,555; A `gbalink_entry_yield` 34,537; A / B `mips_indirect_branch_dual` 34,120 / 34,133; A `update_scanline` 30,723; B `sound_timer` 29,659.

### 23.3 What was built (commit efa9e90; every switch off by default)

| switch | what | twin, SF pair | class |
|---|---|---|---|
| (always) | `update_gba` calls the cable hooks directly in cable mode instead of through `serial_idle_fast` / `update_serial` / `serial_next_event` | 2,705,380 -> 2,659,415 | link only |
| `gba_link_fast = 1` | section 21 (hardware: -7.5 %) | 2,497,156 | link only |
| `gba_link_fast = 3` | 1 plus the **lean hooks**: inline in `update_gba` (serial.h), answered from two cached deadlines until the core's clock reaches them, `wake = min(horizon, first waiting event, end of the transfer in flight)` and `evt` (the same without the horizon). The six calls into `gbalink_core.c` per scheduler iteration (~200 instructions) become ~30. | ~2,255,000 (cable functions 506,383 -> 45,845; the inline checks add ~56,000 to `update_gba`) | link only |
| `gba_link_headless_audio = 2` | the headless machine's sound ring is not read out (indices move as before: they are in the save state) | -28,556 | link only |
| **all** | `gba_link_fast = 3` + `gba_link_headless_audio = 2` | **2,226,606: -16.3 % vs this build's off, -17.7 % vs the hardware control's build** | |

- AW2 battle with all: 1,925,832 -> 1,565,954 (-18.7 %).
- `gba_link_fast = 2` (checking) now also recomputes `wake` and `evt` at every hook (`where=4/5/6`).
- **Single player is untouched:** every change is inside `#ifdef GBA_LINK` and, there, inside the cable-mode branches. `main.c`, `serial.c` and `sound.c` compiled without `GBA_LINK` give byte-identical code before and after; the solo SF run's per-frame hashes equal the c8b0b36 build's. Nothing in the dynarec or the interrupt path changed, so the corpus oracle (a non-link twin) has nothing new to see; it was not run.

**Verified (off the console):**

| check | result |
|---|---|
| MIPS twin, SF and AW2 pair, off vs all | per-frame hashes identical |
| `fast_stress.sh` (8 fixtures + SF 40,000 frames; unstressed, 6 random-horizon seeds, 2 pump-only seeds; off / fast / checking / lean) | 81/81 PASS, 3.7 billion checks, `mismatch=0/0` |
| PPSSPP one console, SF: off, all, checking+headless 2 | `gbadual_hash.txt` identical, and identical to the first build of this branch (cb79f80d) |
| Two PPSSPP consoles, all on: `sf`, `aw2`, `msc`; checking: `sf` | PASS: every HASH, FINAL equal, per-frame files identical, the fixtures' golden frames (recorded with everything off) |
| `tools/run_host_tests.py` | 30 passed |

PPSSPP modelled time moves less (SF one console 10.72 -> 9.72 ms, session 10.15 -> 9.17) because machine A is CPU-rendered there; the twin's counts are the ones calibrated against the console.

### 23.4 Expected on hardware (estimates: twin counts x 8.5 ns)

| PSP-3000, SF fight | measured | expected with all |
|---|---|---|
| one console, pair core | 22.9-23.1 ms (fast 1: 21.2) | **~18.9 ms, ~52 fps** |
| session, core / fps | 23-25 ms / 38.5-39.6 | **~21 ms / ~45 fps** |
| AW2 session core | 13.4 ms | ~11 ms |

Harness builds also run the phase profiler at level 2 by default (`core_phase`, ~344 clock reads a frame on the emulation thread, "~500 us" in this repo's own notes); a release does not. `"core_phase = 0"` removes it from a measurement.

### 23.5 The budget, and whether full speed is reachable

- **Budget, from the measurements.** Session `work_us` must be <= 16.7 ms. Today the session's core is ~2.3 ms above the one-console pair (25.3 vs 22.9-23.1: the radio threads preempting, 9 % of wall time) and the loop adds ~1.1 ms. So the one-console pair must reach **~14.2 ms = ~1,670,000 twin instructions**. With everything of 23.3 it is at 2,226,606.
- **The remaining 556,000, ranked (gains are estimates from the counts above; none of this is built):**

| # | lever | gain (pair) | also single player | class | risk |
|---|---|---|---|---|---|
| 1 | IE/IF/IME writes: a dedicated path (inline in the I/O store stub or first in `write_io_register16`), and no block exit when the interrupt cannot be taken | ~-200,000 | yes, every game with a busy handler | general, dynarec | medium-high: changes where blocks are cut, so cycle accounting; needs the corpus oracle and probably new goldens |
| 2 | Scheduler: `update_gba` at ~240 instructions x 845 calls x 2. A timer-only and a halted-machine fast path, the cable deadline idea applied to the core's own events | ~-150,000 to -200,000 | yes (11-27 % of every game's core) | general | medium: no single hot line, it is a restructure; corpus-gated |
| 3 | Interrupt entry/return in C: `execute_store_cpsr` (88 per call), `set_cpu_mode`, `check_and_raise_interrupts` on every scheduler iteration | ~-100,000 | yes | general, interrupt path | medium-high |
| 4 | Machine A's sound synthesis on the Media Engine (the PSG render and the FIFO resample are output only; the FIFO pops, counters and status bits stay) | ~-180,000 (A only) | yes | general | medium; a second capture path like the renderer's; the ME has ~8 ms a frame free |
| 5 | Dispatch: a return-address cache for interrupt returns | ~-50,000 | yes | general, dynarec | medium |
| 6 | Two-core layout sweep for SF (`gba_link_pin_{a,b}_large`) | +-1 to 2.5 ms | no | link only | none (existing keys; hardware only) |

- **Answer.** 1 + 2 + 3 reach ~1,720,000-1,770,000 (14.6-15.0 ms: 57-59 fps in a session); adding 4 or 5 goes under 14.2. **So full speed for SF in a session looks reachable without a game-specific path and without moving the partner to the second core, but only through a dynarec / interrupt / scheduler campaign of three or four corpus-gated switches, not through anything safe to land in one sitting.** What is built here takes the session from ~39 to ~45 fps (estimate).
- **Not worth doing:** batching or skipping the partner's video writes (its whole video cost is 5,497 instructions; SF's per-scanline writes are interrupt registers, which the game reads back); an idle-loop skip (there is no wait loop); lookahead tuning (18.6 switches a frame are ~0.3 ms).
- **The partner's whole emulation on the Media Engine:** B is ~1,006,000 instructions a frame (~8.5 ms) and the renderer already takes ~8 ms of the ME's 16.7, so it does not fit beside the renderer even before the engineering (a dynarec with no OS under it: its own cache maintenance, no system calls, ROM paging through the main CPU, a deterministic hand-off of the bus at every blocked transfer). Not realistic for 4.0.0, and lever 4 buys the ME's idle time more cheaply.

## 24. Why the ls3 control was slower: the I-cache. A cost model that fits the console, and lever 2 (2026-10-10/11)

### 24.1 Hardware since section 23 (PSP-3000, from the Memory Stick; sessions 3000 + 1000)

| build / arm | one-console SF fight core | |
|---|---|---|
| ls2 control | 22.9-23.1 ms | |
| ls2 `gba_link_fast = 1` | 21.2-21.5 | |
| ls3 control | **25.3-25.6** | 2.5 ms slower than the ls2 control although the twin counts fewer instructions |
| ls3 checking mode | 28.5-28.8 | `mismatch=0/0` |
| ls3 all (`gba_link_fast = 3` + `gba_link_headless_audio = 2`) | **20.1-20.4**, 48.8 fps | section 23.4 expected 18.9 |

Sessions with all on, both PASS: **SF 44.3 fps** (work 22.05 ms; was 39.05), **AW2 59.57 fps** (was 57.3; the first full-speed session).

### 24.2 The cause: instruction-cache misses, which the instruction count cannot see

**Section 23's "8.5 ns per twin instruction" was two things added together, and only one of them is proportional to instructions.** The pair simulated through the Allegrex I-cache model (16 KiB, 2-way, 64 B lines; `tools/layout/lpsim_plugin.c`, extended to both core instances and three translation-cache ranges: `tools/layout/pair_sim.sh`, `pair_layouts.py`) with every fetch placed at its PSP address in the actual ls2 / ls3 / ls4 modules (symbol maps rebuilt from the three commits; the module loads at 0x08804000, which `gbadual_jit b_base` confirms):

| build, arm | twin instr/frame | I-cache misses/frame (sim) | x 215.3 ns | model: 5.42 ns x instr + misses | console |
|---|---|---|---|---|---|
| ls2 control | 2,705,380 | 40,632 | 8.75 ms | 23.4 | 22.9-23.1 |
| ls2 fast 1 | 2,497,156 | 37,678 | 8.11 | 21.6 | 21.2-21.5 |
| ls3 control | 2,659,415 | **49,035** | **10.56** | 25.0 | 25.3-25.6 |
| ls3 all | 2,226,606 | 36,434 | 7.84 | 19.9 | 20.1-20.4 |

- **The ls3 control runs 46,000 fewer instructions than ls2's and takes 8,400 more I-cache misses a frame: +1.8 ms modelled, +2.5 measured.** It is layout, not the guards and not the phase profiler.
- **Why the layout moved.** `update_gba` grew from 2,820 to 3,508 bytes (the inline hooks), so every function behind it in the pinned hot order (`check_and_raise_interrupts`, `set_cpu_mode`, `block_lookup_address_dual`, `update_scanline`'s capture head ...) moved by 800 bytes, and the hot core no longer fits the 6,144 bytes `psp/layout/hot.ord` budgets for it. The cable functions (`gbalink_*`, `lk_apply_due`, the bus) are not in the hot order at all: they sit ~2.4 MB into `.text` and moved by 3.9 KiB mod 8 KiB between the two builds. The control calls them six times per scheduler iteration; the lean path does not, which is why "all" lost less.
- **The model.** `time = 5.42 ns x instructions + 215.3 ns x I-cache misses` (215.3 ns is docs/CACHE-MAP.md's measured miss cost) reproduces all four console numbers within 0.5 ms. **Quote both terms from now on.** What it leaves out: the D-cache, the capture side of the renderer, other threads.
- **I-cache misses are 7-10 ms of the pair's frame.** That is the largest single item in the frame and was invisible in section 23. Two machines' hot code (each: ~11 KiB of emitter stubs, the fight's translated blocks, ~7 KiB of C) cannot stay in 16 KiB.
- **On the console** this is what `DRPROF_HW` cannot split either (it samples time by function, not misses). The model's check is the per-arm table above; to test a layout, run the same arm with different `gba_link_pin_*` (24.5).

### 24.3 Lever 2 as built: `SCHED_FAST=1` + `sched_fast = 1` (commit 58329e8)

`update_gba` no longer calls `check_and_raise_interrupts` when `IE & IF == 0` (both of its effects need a pending enabled interrupt) and ORs raised sources into IF in place. Exact: the machine is the same.

| twin, instr/frame (hashes identical in every row) | off | on | |
|---|---|---|---|
| SF pair (all section 23 switches on) | 2,226,606 | 2,200,332 | -1.2 % |
| AW2 pair | 1,565,954 | 1,542,375 | -1.5 % |
| solo AW2 / H&S heavy / Unbound rival / Unbound double | 642,403 / 1,174,016 / 1,802,941 / 1,266,158 | 630,555 / 1,159,888 / 1,788,027 / 1,249,594 | -0.8 to -1.8 % |
| solo Emerald battle / Driver 2 / NFSU2 / SF | 746,118 / 1,295,608 / 1,108,429 / 971,913 | 734,309 / 1,283,516 / 1,095,560 / 958,407 | -0.9 to -1.6 % |

- **Corpus oracle** (`DRCORPUS_SHIP=1 dr_corpus.py full base sched`: reference = today's shipped configuration; 35 ROMs x 6,000 frames from power-on, audio + registers + RAM + picture every frame): control 35/35, **`sched` 35/35 PASS, no golden moves** (Unbound included; Payback is not in the corpus's default pass).
- PPSSPP one console SF: hash file identical off / all+sched (cb79f80d). Two-PPSSPP sessions with everything on: `sf`, `aw2`, `msc` PASS (HASH n/n, FINAL, per-frame files, golden frames). Host tests 30/30.
- **This is far less than the 150,000-200,000 section 23.5 put on lever 2, and the rest is not available as an exact change.** `update_gba` has no fat line: ~240 instructions a call spread over the timer loop, the video edge, the cable hooks and the prologue. Its cost is the number of calls (845 a frame per machine: 456 scanline edges, 264 sound-timer overflows, the interrupts), and removing calls means not refreshing guest-readable registers (the timer counters, VCOUNT/DISPSTAT) at the times they are refreshed today. That changes what a game can read, so it is not hash-identical and needs an owner decision about goldens. Not done.

### 24.4 Corrections to section 23

- 23.2 said an IE/IME write "ends the block ... even when the interrupt cannot be taken". **Wrong.** `write_io_register16` returns `check_interrupt()`, which already requires IME, IE & IF and CPSR.I clear; a block is only left when the interrupt is taken. The 801 full lookups a frame are the 238 vector entries, the 126 HALTs, the dispatch-cache misses (355) and the SWIs.
- So **lever 1 is purely the cost of the 1,783 IE/IF/IME writes a frame per machine** (stub 41 + C 28 + call setup, ~75 instructions each: ~134,000 per machine), and it can be made exact: handle the three registers inline in the I/O store stub and fall back to today's path only when the write makes an interrupt deliverable. Estimated -100,000 per machine, -200,000 for the pair, plus the misses of the C path it no longer enters. It is emitter work in `mips/mips_emit.h`; not started.

### 24.5 Where the SF pair stands, and the next hardware lines

| | instr/frame | misses/frame | model |
|---|---|---|---|
| ls4 (58329e8), all + `sched_fast = 1` | 2,200,332 | 34,392 | **19.3 ms** |
| the target (session at 59.7 fps: one-console pair ~14.2 ms) | e.g. 1,670,000 | at 24,000 | 14.2 |

**The pair is 530,000 instructions or 24,000 misses a frame from the target, or a mix: 5.1 ms.** Section 23.5's table priced only instructions.

- **Lever 6 off the console (sim, ls4, all on):** the 64 combinations of the two machines' translation-cache offsets mod 8 KiB give 6.59-8.15 ms of miss time; today's (0x1500 / 0x1500) is 7.25, the best **A 0x1400 / B 0x0800 is 6.59 (-0.66 ms)**. Run-time keys, no rebuild.
- **The larger layout lever is the static code**, half of the misses: a hot order made for the pair (both instances' hot functions and the cable hooks placed, `update_gba` at its new size). `tools/layout/gen_layout.py` from a pair profile; needs a rebuild per candidate, and the simulator can now rank candidates before a console sees them. Next.
- **Lever 4 (machine A's sound synthesis on the Media Engine), measured gain:** the output-only part of A's sound is 167,000 instructions a frame (PSG render 129,534 minus the 34,985 a silent machine still spends on its counters; FIFO resample 73,425 - 29,659; read-out 28,556): 0.9 ms by the model plus its misses. Design, not built: the main CPU keeps every guest-visible step (FIFO pops and DMA requests, PSG length/envelope/sweep counters, SOUNDCNT_X) and logs, per frame, the popped FIFO bytes with their ring positions and a copy of the PSG channel state at each `render_gbc_sound` call; the ME runs the existing synthesis code on that log into the output ring, so the samples are the same integers by construction (to be proven on the desktop model as the renderer was, then on hardware by `audio_hash`). The audio job goes before the frame's render on the ME (it is ~1 ms there), the ring is written back from the ME's D-cache before the main CPU's audio thread reads it, the log lives in the volatile-pool work area beside the renderer's, and if the ME has not finished when the read-out comes the main CPU synthesises that frame itself from the same log.

PRX `builds/linkspeed-out/psp/ls4-prx` (`gbadhoc.prx` md5 7a625cb9, ME module 2f252eeb, built clean from 58329e8 with `CORE_EXTRA="SCHED_FAST=1"`):

```
VHASH=0 bash plrun_ms.sh <tag> <arm> sf.gba - - sf_host.inputs 8600 1 "gba_dual = 1" "gba_dual_autopilot = sf_join.txt" "gba_dual_lookahead = 140448" "gba_dual_cold = 1" "stall_watch_s = 10" <extra>
```

| arm | `<extra>` | model |
|---|---|---|
| all | `"gba_link_fast = 3" "gba_link_headless_audio = 2"` | ~19.3 ms (ls4's layout is by itself 0.6 ms better than ls3's) |
| all + sched | add `"sched_fast = 1"` | ~19.3: the model gives `sched_fast` nothing on this layout (-0.14 ms of instructions, +0.17 ms of misses, both inside its error) |
| all + sched + B's cache moved | add `"gba_link_jit_pin_b = 2048"` | ~18.7 (the bench cannot move A's) |

Sessions: `python link_hw.py <tag> 3000 1000 sf --nohash --prx ../linkspeed-out/psp/ls4-prx --src ../linkspeed-worktree --ini "gba_link_fast = 3" --ini "gba_link_headless_audio = 2" --ini "sched_fast = 1"`, then the same with `--ini "gba_link_pin_a_large = 5120" --ini "gba_link_pin_b_large = 2048"` (model: -0.7 ms on the 3000; the 1000's tier was not simulated). Read `sched_fast built=1 on=1`, `gbadual_jit ... mod8k=`, `jit_cache ... mod8k=`, `core_prof`, `gblink_win work_us`.

## 25. The pair's layout ranked in the simulator, lever 1 (IE/IF/IME inline), the budget guard (2026-10-11)

Every gain below is given as twin instructions, simulated I-cache misses, and the model of section 24 (`5.42 ns x instr + 215.3 ns x misses`). The one-console target is 14.2 ms.

### 25.1 Where the misses are, and what a layout can and cannot do

`tools/layout/pair_misses.py` (misses per PSP line of the first layout, `lpsim_plugin misses=`), SF pair, everything of sections 23-24 on, ls4 as linked: 34,391 misses a frame.

| | misses/frame |
|---|---|
| translated blocks, A / B (ROM caches) | 6,930 / 6,368 |
| translated blocks in the RAM caches | 952 |
| the emitter's stubs, A / B | 1,921 / 1,483 |
| `update_gba`, A / B | 1,204 / 2,359 |
| the rest of the C code (CPSR store, mode switch, I/O write, block lookup, cable, sound ...) | ~9,700 |
| not attributed (asm without sizes, the per-frame PSP path) | 3,423 |

- **With no static code in the cache at all, the translated code alone takes 9,528 misses a frame (2.05 ms).** That is the floor for any ordering of the C code. Today's 34,391 is 24,900 above it.
- **Reordering does not get near the floor.** Three orders were built as real PRXs and ranked (`tools/layout/pair_rank.sh`; misses per frame as linked):

| order | SF pair | AW2 pair | Emerald | Unbound rival | Unbound double | H&S heavy | Driver 2 | NFSU2 |
|---|---|---|---|---|---|---|---|---|
| current `hot.ord` (generated for older, smaller functions) | 34,392 | 10,155 | 3,952 | 20,933 | 7,433 | 10,427 | 13,269 | 7,549 |
| R1: the same solo weights, regenerated for today's sizes | 31,795 | 8,140 | 3,572 | 22,472 | 7,137 | 10,000 | 12,883 | 7,912 |
| P1: pair + solo weights (`psp/layout/hot_pair.ord`) | 33,041 | 8,945 | 3,898 | 23,970 | 7,353 | 10,509 | 12,392 | 8,022 |
| P2: P1's weights, 4.6 KiB hot core | 33,754 | 9,051 | 3,532 | 23,663 | 8,880 | 11,231 | 11,861 | 8,549 |

- The spread between orders is 2,600 misses (0.56 ms) on the SF pair, and **every regenerated order makes Unbound's rival battle worse in single player (+1,500 to +3,000 misses, +0.3 to +0.65 ms)** while helping most other fixtures a little. One static order does not serve both; the trade is about half a millisecond either way, which is also the simulator's own error against the console. **The default order is therefore unchanged**; P1 is in the tree as an opt-in (`PSP_EXTRA="HOT_ORD=layout/hot_pair.ord"`).
- **Placement of the two translation caches moves more than the order does, and needs no rebuild.** 64 (A, B) offsets mod 8 KiB per build, SF pair with both levers on (25.2), 2,201 fight frames:

| build | as linked (A 0x1500 / B 0x1500) | best offsets |
|---|---|---|
| ls5 (current order) | 34,621 = 7.45 ms | A 0x1C00 / B 0x0C00: 30,340 = 6.53 ms |
| ls5p (P1 order) | 32,207 = 6.93 ms | A 0x0C00 / B 0x1400: 30,473 = 6.56 ms |

- **Both tiers.** The SMALL tier (PSP-1000) differs in the simulator only by where A's cache sits (its static arrays; a session reloads A at 0x1000 and B at 0x0000): the sweep's numbers are the same function of the two offsets, and today's SMALL session pins (0x1000 / 0x0000) are a poor pair for SF (33,400-35,500). So the 1000 wants its own pins, the same table. **Not modelled for the 1000:** its swap stubs (14.6 KiB against 11.2) and the paged ROM's extra emitted code; its numbers are indicative only.
- Shorter windows gave P1 a larger lead (27,700 at 0x0C00 / 0x0400 over 800 frames); over the full fight it does not hold. Treat differences under ~2,000 misses as noise.

### 25.2 Lever 1: `IRQ_IO_FAST=1` + `irq_io_fast = 1` (commit 62b8f53)

The region-4 16-bit store stub handles IF (`IF &= ~value`) and IE / IME (store, then the test `check_interrupt` makes) inline; a write that makes an interrupt deliverable goes to the full path, which stores the same value again and raises the alert as before. Compiled in only with the build switch; the stub is emitted with the fast path only when `irq_io_fast` is set before the game loads (default 0: byte for byte the old stub).

| | off | on | |
|---|---|---|---|
| SF pair, twin instr/frame (lean hooks on) | 2,226,606 | 2,098,147 | -5.8 % |
| ... with `sched_fast` too | 2,200,332 | **2,071,873** | |
| AW2 pair | 1,565,954 | 1,565,493 | 0 |
| solo SF / Unbound rival | 971,913 / 1,802,941 | 910,868 / 1,733,245 | -6.3 % / -3.9 % |
| solo H&S heavy, Unbound double, Emerald, Driver 2, NFSU2, AW2 | | | -0.5 % to +0.003 % |
| SF pair misses/frame (sim, ls4 -> ls5 as linked) | 34,392 | 34,621 | none: the calls it removes were not the ones missing |

- Hashes identical on the twin in every row (SF and AW2 pairs, eight solo fixtures).
- **Corpus oracle:** `irqio` 35/35 PASS against the shipped configuration; no golden moves.
- PPSSPP: one-console SF hash file identical with it on (cb79f80d, the branch's first build); two-console `sf`, `aw2`, `msc` with all four switches PASS (every HASH, FINAL, per-frame files, golden frames). Host tests 30/30.
- It is 128,000 instructions, not the 200,000 estimated: 517 of the 1,783 writes a frame do make an interrupt deliverable and take the full path, and the inline path costs ~20 instructions itself.

### 25.3 The growth guard

`tools/layout/hot_budget.py` compares the pinned hot core's size in the ELF just linked with the sizes `hot_order.txt` was generated for; `tools/build.sh` prints it on every `LAYOUT_PIN=1` build and fails the build with `HOT_BUDGET_STRICT=1`. What it says about the builds of this campaign: ls2 (every GBA_LINK build since the cable branches went into `update_gba`) **over by 252 bytes**; ls4 / ls5 **over by 1,252** (`update_gba` 2,596 -> 3,596); the P1 order, generated for today's sizes, 6,136 of 6,144. It is a report by default because regenerating the default order is the solo trade of 25.1.

### 25.4 Where the SF pair stands

| PSP-3000, one console, fight | instr | misses | model |
|---|---|---|---|
| ls2 control (measured 22.9-23.1) | 2,705,380 | 40,632 | 23.4 ms |
| ls3 all (measured 20.1-20.4) | 2,226,606 | 36,434 | 19.9 |
| ls5, all four switches, as linked | 2,071,873 | 34,621 | **18.7** |
| ls5 with A 0x1C00 / B 0x0C00 (sessions only: the bench cannot move A) | 2,071,873 | 30,340 | **17.8** |
| ls5p (P1 order) as linked / with A 0x0C00 / B 0x1400 | 2,071,873 | 32,207 / 30,473 | 18.2 / 17.8 |
| target | | | 14.2 |

**3.6 ms to go** from the best configuration here: e.g. 660,000 instructions, or 16,700 misses, or a mix. In a session that is roughly work 20.4-21.2 ms, ~48 fps (from 44.3 measured), by the bench-to-session difference of section 23.5.

What is left that is exact and general, with what it could give (estimates from the tables above):

| lever | instr | misses | model |
|---|---|---|---|
| 3: `execute_store_cpsr` / `set_cpu_mode` / `execute_spsr_restore` fast paths (464 + 738 + 232 calls a frame per machine, 88 / 32 / 85 instructions each; ~1,400 misses) | -80,000 to -100,000 | -1,000 to -2,000 | -0.6 to -1.0 ms |
| 5: return-address cache for interrupt returns (801 full lookups a frame per machine; ~1,700 misses) | -50,000 | -1,000 to -1,500 | -0.5 to -0.6 |
| 4: A's sound output on the Media Engine | -167,000 | -500 to -1,000 | -1.0 to -1.1 |
| smaller hot C code (`update_gba` is 3.6 KiB and takes 3,560 misses; `-Os` or an out-of-line split of its cold halves) | 0 or slightly up | -2,000 to -3,000 | -0.4 to -0.6 |

Together about -2.5 to -3.3 ms: **14.5-15.3 ms, just short of the target.** Beyond that the remaining cost is the translated code itself (13,300 misses in blocks, 826,000 instructions in emitted code and stubs for the pair) and the scheduler's call count.

### 25.5 The case that needs the owner (not built): refreshing guest-readable registers less often

- **What it is.** `update_gba` runs 845 times a frame per machine in SF's fight. 264 of those are sound-timer overflows whose only guest-visible effect, 15 times out of 16, is the timer's counter register changing value; ~200 more are scanline edges inside a HALT. Running the scheduler only when something can be observed or raised (an enabled interrupt, a DMA, the CPU waking) would remove an estimated 300-400 calls a frame per machine: ~-150,000 to -190,000 instructions and ~-2,000 misses for the pair, **-1.2 to -1.5 ms**, and a similar share in single player.
- **What changes for a game.** A read of TMxCNT_L (and, for the halted case, nothing: a halted CPU reads nothing) would have to return a value computed at the read instead of the value left by the last scheduler call. That is closer to hardware than today (today the counter only moves at scheduler calls), but it is not what the current goldens record: any game that reads a running timer between events (random seeds, music drivers that poll) gets different values, so the corpus will not be hash-identical and every golden moves once.
- **Why it may be needed.** The exact levers above end at 14.5-15.3 ms against 14.2. This is the next largest general item and the only one that also shortens every single-player frame by ~3-4 %.
- **What I would ask for:** permission to prototype it behind a switch and report which corpus ROMs change and how (frame of first difference, whether pictures differ), before any decision to ship it.

### 25.6 The guest's one long stall per session is the rig's host script

In the menu-driven soak (`linkspecs/ui/*_host.ui`) the host's script, 150 loops after `linked`, opens the pause menu, walks to the Wireless page for two screenshots and closes it (`+10 menu ... +4 press CIRCLE +6 press CIRCLE`): ~48 loops with the host's game, and so the session, paused. The host's first `gblink_win` shows it (`loops=601 frames=547 stalls=6`: 48 loops that neither stepped nor stalled; `ui_script step=8 verb=1 ... frame=1339` through step 12 with `frame=` unchanged), and the guest waits those 46-48 frames for the host's buttons (`st_max=45-47`, once, in its first window, whichever console is the guest). The join script has no such step, and the role sessions and the PPSSPP sessions do not show it. Nothing in the emulator: a player opening the menu pauses the partner by design. To take it out of the numbers, run the soak with a host script that ends at `wait linked` (or score stalls from the second window on).

## 26. The timing prototype (`TIMER_LAZY`), the ls5 hardware round, and where the session's time really goes (2026-10-11)

### 26.1 Hardware, ls5 / ls5p (PSP-3000 from the stick; sessions 3000 host + 1000 guest)

| | measured | model (section 24) |
|---|---|---|
| ls5 one-console SF fight, all four switches | 18.7 ms, 52.4 fps | 18.7 |
| ls5p (pair order) one-console | 19.4 ms | 18.2 |
| Unbound rival, single player: ls5 keys off / `sched_fast` + `irq_io_fast` on | 13.3 / 12.7 ms (-5 %) | twin: -4.7 % instructions |
| Unbound rival: ls5p keys off | 13.7 ms (+3 % vs ls5) | sim: +0.3 to +0.65 ms |
| SF session, ls5 | 45.5 fps (ls3: 44.3) | section 25.4 said ~48 |

- **The pair order is withdrawn.** The simulator put ls5p 0.5 ms ahead of ls5; the console puts it 0.7 ms behind, and it costs single player 3 %. The simulator reproduces builds that share an order (ls2, ls3, ls5: within 0.5 ms each) but **cannot rank two orders against each other at the ~1 ms level**: what moves between orders includes code the twin never executes and the model only approximates (the capture head of `update_scanline`, 160 calls a frame, placed from a placeholder rate in `psp_frame_path.txt`; the asm with no symbol sizes; 3,400 misses a frame are unattributed), and data placement is not modelled at all. `psp/layout/hot_pair.ord` stays in the tree as a record; nothing should ship it.
- **The session pin arms tested nothing.** `gba_link_pin_{a,b}_*` are applied by the session start's reload, which only exists in late mode (`gba_dual_late = 1`, the player's flow); the role sessions run `late=0`. Both logs show `jit_cache ... mod8k=0x1500` and `gbadual_jit ... mod8k=0x1500` with the pins requested: unchanged. The fight windows of `ls5_sf_s1` and `ls5_sf_s2` are the same to 0.1 % (core 21.65 / 21.63 ms); s2's lower average is one 41-frame radio stall in its last window. To test the pins: add `--ini "gba_dual_late = 1"`, and read the two `mod8k=` lines.

### 26.2 The 2.5 ms between the one-console pair and the session

In the fight the session is **bound by the 3000 host's CPU, not by the 1000 and not by the radio's latency**: host `gblink_win` windows are `loops=433 frames=433 stalls=0 wait_ms=0`; the 1000 guest's are `frames=433 stalls=50-95` with its own core at 20.95 ms: it is the faster console here and waits. A 3000 + Go pair would run at the same 43.3 fps.

Per frame on the host, fight windows of `ls5_sf_s1/a`:

| | ms |
|---|---|
| frame period (433 frames / 10 s) | 23.09 |
| `work_us` | 22.74 |
| `core_us` (the pair's step, wall clock) | 21.65 |
| the same pair on the same console with no radio (`ls5_sfb`) | 18.7 |
| **inside the step, not the machines** | **2.95** |
| the loop around the step (`work - core`) | 1.09, of which `sess_cost pump` 0.70 (rx 0.65) |

- So the session costs **~4.0 ms a frame over the one-console pair**, and 2.95 of it is wall-clock time inside the pair's step that the emulation thread did not use. The only things that can take it are threads that outrank the emulation thread (0x2B in a session): the kernel's WLAN threads, our ad-hoc RX/TX threads, audio. hw3's `thrcpu` measured exactly that for AW2 (17.5: SceWlanMac 1.4 + SceWlanHal 0.8 + SceNetNetintr 0.35 + ad-hoc 0.5 + audio 0.25 = 3.3 ms per frame at 60 fps). **This is an inference from two measurements, not a direct one for SF.**
- The radio traffic in the fight: `sess_cost pdp=1267` (sceNetAdhocPdpSend calls per 600 frames: 2.1 a frame, 126 us each on the TX thread) and about as many received; the transport itself sends ~1 packet a frame (0.5 data + 0.46 bare ACKs).
- **This is a lever no profile of the machines could show, and it touches nothing guest-visible:** fewer radio packets per frame (ACKs riding on the next input packet instead of going bare; two frames of buttons per packet, which the 4-frame input delay allows), and a cheaper receive pump. If the WLAN threads' cost is per packet, halving the packets is worth 1-1.5 ms; if it is per second (beacons, polling), nothing. `thrcpu` and `looprof` on a console decide that: PRX `builds/linkspeed-out/psp/ls5prof-prx` (ls5 + `DRPROF_HW`), same session line, then `tools/gbamirror/linkprof_report.py`.

### 26.3 The timing prototype: `TIMER_LAZY=1` + `timer_lazy = 1` (commit 5a78241)

**What it does.** Overflows of a timer that raises no interrupt and feeds no cascade (in practice the direct-sound timers) stop being scheduler events; all pending overflows are processed at the next scheduler call, at most one H-blank edge (960 cycles) later.

**Is it more accurate? No.** On hardware the FIFO sample is popped and the refill DMA requested at the overflow. Today's core does that exactly; the prototype does it up to 960 cycles late. A timer counter read is as stale as before (both refresh it only at scheduler calls). It is a speed-for-timing trade, not a correction. (Not compared against mGBA: there is nothing to gain from the comparison when the change moves away from hardware by construction.)

**Gain** (twin instructions; simulated misses as linked; model):

| | off (ls5) | on (ls6) | model |
|---|---|---|---|
| SF pair | 2,071,873 / 34,621 | 2,021,540 / 37,892 | 18.7 -> **19.1 ms (worse)**: -0.27 ms of instructions, +0.70 ms of misses |
| AW2 pair | 1,541,914 / 8,574 | 1,348,871 / 12,261 | 10.2 -> 9.95 ms |
| solo SF / Unbound rival / Unbound double | 897,362 / 1,718,332 / 1,249,095 | 878,695 / 1,612,485 / 1,165,558 | -2.1 / -6.2 / -6.7 % instructions |
| solo Emerald battle / H&S heavy / Driver 2 / NFSU2 / AW2 | 732,148 / 1,154,072 / 1,283,364 / 1,090,696 / 630,574 | 661,514 / 1,111,532 / 1,236,726 / 1,039,787 / 544,501 | -9.6 / -3.7 / -3.6 / -4.7 / -13.6 % |

- `update_gba` calls per machine per frame: SF 845 -> 709, AW2 1,358 -> 922.
- The extra misses are the function growing again (`update_gba` +616 bytes; the guard: hot core over by 1,868). With a layout that absorbed it the SF pair would gain 0.27 ms, AW2 ~1 ms. Both tiers: the simulator has no tier-dependent term beyond the cache offsets (25.1).
- **For Street Fighter this lever is worth a quarter of a millisecond at best.** Section 25.5 estimated 1.2-1.5 ms; SF's sound timer overflows turn out to coincide with events that remain.

**What changes, on the whole library** (`dr_corpus.py full base tlazy` then `dr_corpus_fields.py`: the 35-ROM corpus, the owner's `roms` folder and the rig's ROMs, 42 GBA ROMs, 6,000 frames each from power-on with the corpus's input script; Payback 1,365 frames):

| | ROMs |
|---|---|
| identical in every field on every frame | 4: Iridion II, The Third Age, Shrek Super Slam, Payback (as far as it ran) |
| internal state only (same picture, same sound) | 1: Mario Golf |
| sound differs, picture identical on all frames | 8: Advance Wars 2, Minish Cap, Super Circuit, Mario Tennis, Battle Network 5 and 6, Mother 3, Heart & Soul |
| picture differs on some frames | 29, among them Unbound (1,026 of 6,000 frames), Emerald (299), FireRed (5), Mystery Dungeon (3,801), NFSU2 (1,902), Driver 2 (41), Street Fighter (71), both Castlevanias, both Golden Suns, both Fire Emblems, Sonic Advance 1 and 2 |

- Nothing hung, crashed or stopped drawing (every run completed its frames, `ctrl` 41/41).
- "Picture differs" under a fixed input script means the game reached the same screens at slightly different frames or took a different branch (a random number drawn at a different cycle); the table cannot tell those apart, and no save was compared. The audio hash differs on 37 of 42.
- This is the same class of change as the earlier timing changes that moved Unbound, only wider: it moves nearly every game.

**Lockstep.**
- With the switch on on both consoles, two-PPSSPP sessions stay in sync: `sf`, `aw2`, `msc`: every periodic HASH, FINAL equal, per-frame files identical (the fixtures' goldens differ, as they must).
- If the consoles disagree, the session is refused before it starts, both ways round: the host sends its core mask after CONFIG (`M_CORECFG`, only when not zero), the guest logs `gblink_core_mismatch host=... own=...` and fails `protocol`; the host ends `peer_aborted`. A build that does not know the message fails `protocol` on receiving it. Verified in PPSSPP (host on / guest off, and the reverse). With the switch off nothing new is sent, so default builds interoperate as before.
- Host tests 30/30.

**Recommendation: do not pursue it.** It gives SF nothing, it is less accurate than what ships, and it changes the picture or the sound in 37 of 42 games. It can be put to rest. The switch and the handshake stay in the tree, off.

### 26.4 The remaining safe levers, re-estimated

Each lever so far delivered about 60 % of its estimate (fast path 18 -> 12 %, scheduler 150-200 K -> 26 K, lever 1 200 K -> 128 K, timing change 1.2-1.5 ms -> 0.27). The same discount applied:

| lever (all exact, general) | first estimate | at 60 % |
|---|---|---|
| 3: CPSR store / mode switch fast paths | -0.6 to -1.0 ms | -0.4 to -0.6 |
| 5: return-address cache for interrupt returns | -0.5 to -0.6 | -0.3 to -0.35 |
| 4: A's sound output on the Media Engine | -1.0 to -1.1 | -0.6 to -0.65 |
| smaller `update_gba` (the hot core back inside its budget) | -0.4 to -0.6 | -0.25 to -0.35 |
| cache offsets for the 3000 (25.1; untested on hardware) | -0.9 | -0.5, or nothing: the simulator's ranking of placements is no better proven than its ranking of orders |
| **one-console SF pair, from 18.7 ms measured** | 15.0-15.5 | **16.3-17.2** |
| session-only: fewer radio packets, cheaper pump (26.2; needs `thrcpu`) | -1 to -1.5 | -0.6 to -0.9, or nothing |

**The honest range for a Street Fighter session on a PSP-3000** (work = one-console pair + ~4.0 ms of session, today):

| | work | fps |
|---|---|---|
| today (ls5, measured) | 22.7 ms in the fight | 43.3 in the fight, 45.5 over the session |
| all remaining machine levers at 60 % | 20.3-21.2 | 47-49 |
| ... and the radio lever at 60 % | 19.4-20.6 | 49-52 |
| everything at its full first estimate | 17.5-18.0 | 55-57 |
| with the timing change added | no different (-0.15 to -0.27 ms) | |

**Street Fighter at 59.7 fps in a session is not reachable with any lever identified so far**, with or without the timing change; the realistic ceiling is about 50 fps, the optimistic one 55-57. Advance Wars 2 and Super Circuit are already at full speed (59.3-59.7 and 59.0-59.3 over 18 soak sessions). What would close the rest is of a different kind: a translator that emits less code for the same guest work (the pair spends 826,000 instructions and ~17,000 misses a frame in emitted code and stubs), or half of the pair on the second core.

## 27. New target: Street Fighter steady at 50 fps in a session. The radio lever, evidence first (2026-10-11)

**Decisions.** The timing change (26.3) is dead: documented, default off, not to be proposed again. 59.7 fps for Street Fighter is no longer the target. The target is a **steady 50 fps** in a real two-console session, presented PAL-style by the `claude/slowdown-feel` work: a 20.0 ms frame, so the 3000 host's fight `work_us` must come from 22.74 ms to **18.5-19 ms** to hold it with margin.

### 27.1 The profiler run (before any radio code)

PRX `builds/linkspeed-out/psp/ls5prof-prx` (`gbadhoc.prx` md5 15dd9143: ls5 + `DRPROF_HW`; ME module unchanged). It has not run in PPSSPP or under PSPLink: owner present.

```
python link_hw.py <tag> 3000 1000 sf --ms --nohash --prx ../linkspeed-out/psp/ls5prof-prx --src ../linkspeed-worktree --ini "gba_link_fast = 3" --ini "gba_link_headless_audio = 2" --ini "sched_fast = 1" --ini "irq_io_fast = 1"
```

No profiler key is needed (`drprof_us` defaults to 1000: a 0x0F thread sampling every millisecond; the log says `drprof on us=1000 prio=0x0F`). The sampler costs ~0.5 ms a frame itself, so this run's fps is not a result.

What to read, per heartbeat window on the HOST (`a/frontend.log`), menus (59.7 fps) against fight (~43 fps):

- **`EVT thrcpu f= wall= name= prio= us=`**: one line per thread, `us` = that thread's CPU in the window of `wall` microseconds. Add the threads that outrank the emulation thread (0x2B): `SceWlanMac`, `SceWlanHal`, `SceNetNetintr`, our ad-hoc RX/TX (0x2A), audio, the sampler. Divided by the window's frames this should be the 2.95 ms of 26.2.
  - If their `us / wall` is the same in the menus and in the fight, the radio's cost is **per second** (beacons, polling): fewer packets will not help, and the lever is closed.
  - If their `us / frames` is the same, it is **per packet**: halving the packets is worth about half of it.
- **`EVT looprof ... names=top,netf,gbls,misc,pres0,ap,run,post,shash,acct,osd,retire,pres,swap,rign,other`**: sampler hits per loop region; `netf` is the network pump on the emulation thread (0.70 ms a frame by `sess_cost`), `gbls` the session service.
- **`EVT linkprof ... a=... b=... pair=`**: the split inside the step between machine A, machine B and the pair's own loop.
- `sess_cost pdp=<calls>/<mean us>/<max us>` and `adhoc_stats tx= rx=`: packets per window, to put the per-packet cost in numbers.

The log writer starves in the fight (it sits below the emulation thread), so some windows' lines will be missing (`evt_gap`); the windows that got through are enough.

### 27.2 Design (each its own switch, default off; nothing here changes what the machines compute)

(Corrected in 28.1: it is ~2 sent and ~2 received.) Today, per frame and per console in the fight: ~1 transport packet sent (0.5 with data, 0.46 bare ACKs) but 2.1 `sceNetAdhocPdpSend` calls and as many receives; the difference is still to be attributed (the profiler run's `sess_cost` and a packet census by type).

| switch | what | risk | test |
|---|---|---|---|
| `nd_ack_ride_us = N` (transport) | a bare ACK waits up to N us (one frame: 16,000) for the next outgoing data packet and rides on it; it goes bare only when the wait expires | the sender's RTT samples grow by the wait and the RTO floor is 50 ms, so no spurious retransmits; an idle direction still gets its ACK within N | two-PPSSPP `sf` / `aw2` / `msc`; then with `net_loss_pct = 3`, `net_jitter_ms = 5` + `net_latency_ms = 15`, and `net_blackout_ms` |
| `gblink_input_batch = 2` (session) | buttons for frames f and f+1 leave in one message every second frame | one frame less slack against the 4-frame input delay: more stalls on a bad radio; same buttons on the same frames, so hashes cannot change | as above; the stall counts are the measure |
| the receive pump | `sess_cost rx=645 us` a frame on the emulation thread for ~2 packets: find what it polls (the profiler's `netf`), then drain the RX thread's ring without system calls when it is empty | none to the protocol | as above |
| thread priorities | no code: `adhoc_net_prio` and `emu_prio` are harness keys already. Our ad-hoc threads sit one step above the emulation thread on purpose (a CPU-bound emulation thread never yields: below it they starve, as the log writer does). The kernel's WLAN threads are not ours to move. Not recommended; an A/B only if `thrcpu` puts real time in our two threads | starvation, stalls | hardware A/B |

- **Loss injection for the cable path exists:** `net_loss_pct`, `net_latency_ms`, `net_jitter_ms`, `net_blackout_ms` / `net_blackout_every_ms` act on the transport under both the adapter and the cable sessions (`run_fixture_ppsspp.sh --loss/--lat/--jit`; the multigame matrix passes at 3 % loss and 15 +- 5 ms). The NACK / resend path is exercised by them. No new key is needed.
- Nothing is built until the profiler run says the cost is per packet.

### 27.3 After the radio: the exact levers, in order of gain per risk

A's sound output on the Media Engine (-0.6 ms at the 60 % discount), CPSR / mode-switch fast paths (-0.4 to -0.6), the return-address cache (-0.3), a smaller `update_gba` (-0.25 to -0.35 and it brings the hot core back inside its budget, which every other lever must respect: `HOT_BUDGET_STRICT=1`). Together -1.5 to -1.9 ms: fight `work_us` 20.8-21.2 ms from them alone. **With the radio lever at its discounted -0.6 to -0.9: 19.9-20.6 ms. That is at the 20.0 ms frame, not under it with margin**; the 18.5-19 ms aim needs either the radio lever at its full estimate or more than is listed here. Stated now so it is not a surprise later.

## 28. The radio lever, built: ack riding and input batching (2026-10-11)

The profiler run (27.1) came back: the cost is **per packet**. On the 3000 host in the fight, per frame: `SceWlanMac` + `SceWlanHal` 2.15 ms, `SceNetNetintr` 0.34, our ad-hoc RX 0.25, TX 0.22: **2.96 ms** of threads that outrank the emulation thread, the 2.95 ms of 26.2.

### 28.1 Packet census (settles "2.1 sends for ~1 packet")

The earlier "~1 transport packet per frame" was wrong: it read one console's DATA count only. Counted from `net_stats` (`tx`, `tx_data`, `tx_ack`) in the two-PPSSPP `sf` session, per emulated frame and per console:

| | sent | received |
|---|---|---|
| DATA (reliable, carries `M_INPUT`: one frame of buttons) | 0.98-1.00 | 0.98-1.00 |
| bare ACK | 0.92-0.93 | 0.92-0.93 |
| PING, NACK, retransmits, bulk | ~0.05 | ~0.05 |
| **total** | **1.96** | **1.91-2.00** |

So each console handles **~3.9 packets per frame**, and `sess_cost pdp` (2.1 sends a frame on hardware) was right. Why one bare ACK per DATA: `arq_rx_data` sets `need_ack`, and the transport tick sends a bare ACK in the same tick unless one of our own DATA frames is due in that tick. The two consoles send their input at about the same moment, so the partner's packet arrives just after ours has left, and the ack never finds a ride. Half of everything on the air is 8-byte acknowledgements.

### 28.2 The two switches (both default off; neither changes what the machines compute)

| key | where | what |
|---|---|---|
| `nd_ack_ride_us = N` | transport (`netdrv`: `nd_config.ack_ride_us`, `ack_ride_over()`) | a pending ack waits up to N microseconds for an outgoing unicast to carry it; bare only when the wait runs out. A duplicate or out-of-order DATA frame is still acked at once (the sender is retransmitting). Each console may set it on its own: the wire format does not change. Recommended **30000**: longer than a 23 ms fight frame, shorter than the 50 ms RTO floor. |
| `gblink_input_batch = 2` | session (`fe_gblink`: `M_INPUTS` 0x15, core-mask bit 1) | buttons for two consecutive frames leave in one message. **Staggered**: the host's packet leaves on odd frames, the guest's on even ones, so each ack rides on the packet that leaves one frame later (without the stagger both sent together, the ride timed out and bare ACKs came back: 0.06 a frame instead of 0.01). A held frame is flushed alone on any stall and after two steps. Wire format: both consoles must set it; the `M_CORECFG` handshake refuses a mismatch before the first frame. |

What batching costs: the first frame of each pair leaves one frame late, so the input lead shrinks. Measured (`lead_mean_x10`, PPSSPP `sf`): 2.2 frames -> 1.9 (riding) -> 1.7 (riding + batching) of the 4-frame delay.

### 28.3 Two-PPSSPP results (EBOOTs ls8-eboot64 03984eff / ls8-eboot 13efa3cc, commit e64e21b; all four core switches on in every arm)

Packets per frame per console (tx + rx), three clean `sf` sessions run together under equal load:

| arm | packets/frame | bare ACK/frame | DATA/frame | host stalls | guest stalls | verdict |
|---|---|---|---|---|---|---|
| control | 3.87-3.96 | 0.93 | 0.98-1.00 | 186 | 27 | PASS |
| `nd_ack_ride_us = 30000` | 2.06-2.08 | 0.01 | 0.98 | 106 | 24 | PASS |
| + `gblink_input_batch = 2` | 1.09-1.11 | 0.01 | 0.50 | 119 | 37 | PASS |

PASS = both exit 0, every periodic HASH matched, equal FINALs, per-frame files identical over ~18,500 lines, the fixture's golden frame and markers. Stall counts in PPSSPP move with the PC's load (the same control arm gave 476 and 1894 under heavier load), so read them as "not worse", not as a gain.

Both switches together, other games and a bad radio:

| session | packets/frame | host / guest stalls | verdict |
|---|---|---|---|
| `aw2` | 1.22 | 4836 / 1092 (PC loaded: a PSP build ran beside it) | PASS, 60/60 hashes |
| `msc` (Mario Kart) | 1.10 | 28 / 37 | PASS |
| `sf`, 3 % loss | 1.26 | 559 / 275 | PASS |
| `sf`, 15 +- 5 ms latency | 1.14 | 439 / 149 | PASS |
| `sf`, 300 ms blackout every 5 s | 1.30 | 739 / 450 | PASS |
| `sf`, 3 % loss, riding only | 2.36 | 231 / 215 | PASS |
| `sf`, 3 % loss, control | 3.97 | 184 / 170 | PASS |
| `sf`, batch on the host only | - | - | refused at start: `gblink_core_mismatch host=00000002 own=00000000` |
| `sf`, batch on the guest only | - | - | refused at start: `gblink_core_mismatch host=00000000 own=00000002` |

The fault keys were live in those runs (`adhoc_stats faultdrop=182` at 3 % loss, `616` with blackouts, `faultdel=5235` with latency): they act at the ad-hoc transport, under the cable session's ARQ, NACK and resend paths. **At 3 % loss batching stalls more** (559 against 231 and 184 for the host): a lost packet now carries two frames. Riding alone costs nothing measurable under loss. Host tests 30/30 (the `netdrv` suite runs with riding off, the default: the default path is unchanged).

### 28.4 Expected on hardware (estimate; nothing here has run on a PSP)

Per-packet cost from the profiler run: 2.96 ms / 3.9 packets = **0.76 ms per packet per frame** of higher-priority thread time (a bare ACK is assumed to cost what a DATA packet costs: both are one tiny frame through the same threads; the with/without runs will say).

| arm | packets/frame | thread time removed, full estimate | at the 60 % discount | fight `work_us` (from 22.74) | margin to 20.0 ms |
|---|---|---|---|---|---|
| control | 3.9 | - | - | 22.74 (measured, ls5) | -2.74 |
| riding | 2.07 | -1.39 ms | -0.83 | 21.35-21.9 | -1.35 to -1.9 |
| riding + batching | 1.10 | -2.13 ms | -1.28 | 20.6-21.5 | -0.6 to -1.5 |

Not in the table: the emulation thread's own receive work (`sess_cost rx=` 0.65 ms a frame for ~2 packets received) should fall with the packets too, up to -0.45 ms; treat it as a bonus until measured. No code was changed there: with a quarter of the packets there is little left to save.

**So the radio lever alone does not reach 20.0 ms.** With the remaining exact levers of 27.3 (-1.5 to -1.9 ms at the discount): 19.1-19.6 ms if the radio delivers in full, 19.6-20.0 at 60 %. The 18.5-19 ms aim is reached only if everything delivers its full estimate.

### 28.5 Hardware runs (3000 host, 1000 guest, from the memory stick)

PRX `builds/linkspeed-out/psp/ls8-prx` (`gbadhoc.prx` md5 762f0150, commit e64e21b; ME module unchanged 2f252eeb) and the profiler build `ls8prof-prx` (md5 0893e773). `C` below stands for `--ini "gba_link_fast = 3" --ini "gba_link_headless_audio = 2" --ini "sched_fast = 1" --ini "irq_io_fast = 1"`. Checked with `--dry`: the key audit accepts both new keys.

```
python link_hw.py ls8_sf_c  3000 1000 sf --ms --nohash --prx ../linkspeed-out/psp/ls8-prx --src ../linkspeed-worktree C
python link_hw.py ls8_sf_r  3000 1000 sf --ms --nohash --prx ../linkspeed-out/psp/ls8-prx --src ../linkspeed-worktree C --ini "nd_ack_ride_us = 30000"
python link_hw.py ls8_sf_rb 3000 1000 sf --ms --nohash --prx ../linkspeed-out/psp/ls8-prx --src ../linkspeed-worktree C --ini "nd_ack_ride_us = 30000" --ini "gblink_input_batch = 2"
python link_hw.py ls8_sf_b  3000 1000 sf --ms --nohash --prx ../linkspeed-out/psp/ls8-prx --src ../linkspeed-worktree C --ini "gblink_input_batch = 2"
python link_hw.py ls8_sf_rbh 3000 1000 sf  --ms --prx ../linkspeed-out/psp/ls8-prx --src ../linkspeed-worktree C --ini "nd_ack_ride_us = 30000" --ini "gblink_input_batch = 2"
python link_hw.py ls8_aw_rbh 3000 1000 aw2 --ms --prx ../linkspeed-out/psp/ls8-prx --src ../linkspeed-worktree C --ini "nd_ack_ride_us = 30000" --ini "gblink_input_batch = 2"
python link_hw.py ls8_mk_rbh 3000 1000 msc --ms --prx ../linkspeed-out/psp/ls8-prx --src ../linkspeed-worktree C --ini "nd_ack_ride_us = 30000" --ini "gblink_input_batch = 2"
python link_hw.py ls8_sf_prof 3000 1000 sf --ms --nohash --prx ../linkspeed-out/psp/ls8prof-prx --src ../linkspeed-worktree C --ini "nd_ack_ride_us = 30000" --ini "gblink_input_batch = 2"
```

What to read on the HOST: `net_tuning ack_ride_us=30000`; `gblink_core_mask sent=00000002` (batching on); `gblink_win work_us=` and `stalls=` in the fight windows; `net_stats tx= tx_ack= tx_data=` (packets: tx + rx over frames should be ~3.9 / ~2.1 / ~1.1 a frame); `sess_cost pdp=` and `rx=`; in the profiler run `thrcpu` for `SceWlanMac`, `SceWlanHal`, `SceNetNetintr` per frame against 27.1's 2.15 + 0.34 ms. The `_rbh` runs keep the per-frame hash files: they are the determinism check on real radios (the `--nohash` arms are the cost arms).

Risks: (1) batching under real loss stalls more (28.3); if the fight windows show more `stalls` with `_rb` than with `_r`, riding alone is the lever. (2) With riding the sender's RTT samples include the ride, so `srtt_us` roughly doubles (17 -> 28-30 ms in PPSSPP) and the RTO sits above its 50 ms floor: a real loss is repaired a little later; the session's own NACK path is unaffected. (3) A console with riding on talking to one without is fine; batching must match (refused otherwise).
