# DYNAREC PROFILE — where the main CPU's time goes, and what is worth fixing

Branch `claude/dynarec-profile` (off the published 3.0.0 source plus
`claude/gba-link-bench`), 2026-09-29. Measurement study; nothing here changes
a release default.

**Labels used throughout.** *Twin* = exact dynamic MIPS-instruction counts of
the PSP's own translator running under qemu (not time). *PPSSPP* = emulator
numbers, no cache model (used for smoke and hash identity only). *Hardware* =
PSP. The one hardware time split in this study is **staged, not yet run**
(section 8); hardware numbers quoted below are the owner's earlier AW2/H&S
runs, used only to calibrate the twin.

---

## 0. Answer in one screen

* **The translated code itself is not where the time goes.** Emitted MIPS doing
  guest work is 12–22% of main-CPU instructions in every scene measured. The
  rest is the machinery around it: the dispatcher (6–26%), the event scheduler
  `update_gba` (11–27%), the SMC machinery on self-modifying games (translate
  + flush 13–27%), the memory stubs (10–16%) and sound (7–24%).
* **The dispatcher is the biggest single dynarec waste.** Every indirect branch
  (`bx lr`, `pop {pc}`, and every exit of a block that ends at an SMC gate)
  costs ~140 MIPS instructions: a 15-register save, a C call, a hash walk and a
  15-register restore. AW2 does ~1,700 a frame; Unbound's rival battle ~3,000.
* **Two targeted prototypes, both speed-only and oracle-identical, cut 17–23%
  of core instructions on every heavy fixture** (AW2 −22.9%, H&S heavy −19.9%,
  Unbound rival −20.2%, double −17.1%, overworld −17.5%; Emerald −4.2%):
  `DISPATCH_CACHE` (a pc→entry probe in asm before the C lookup) and
  `SMC_RETIRE_WINDOW` (a 4 KiB tag walk per SMC event shortened to the longest
  live block). Twin, bit-identical per-frame guest state on 7 fixtures /
  22,000 frames with audio; PPSSPP bit-identical on the staged EBOOTs.
* **Twin → hardware calibration holds to a few percent**: 6.5 ns per twin core
  instruction reproduces both the owner's Go AW2 median (5.67 ms) and H&S heavy
  (~9.5 ms). If that holds for the prototypes, AW2 drops ~1.3 ms/frame and H&S
  heavy ~1.9 ms. The hardware A/B is staged to confirm or refute it.
* **Recommendation: targeted surgery, not a new translator.** The register
  allocation is already complete (all 15 ARM registers and 4 flags live in MIPS
  registers; zero reg[] spills in emitted code), Thumb has flag liveness, and a
  new translator would attack the ~20% that is already reasonable while
  re-opening SMC and timing correctness — which the oracle cannot validate for
  a translator that moves block boundaries. Ranked list in section 6.
* **Staged for the owner**: `builds/dynarec-profile-out/` — ONE rig app
  `GBADHOC-DRPROF` driven by the USB handoff loop: AW2 tour and H&S battle ×
  base / prototypes / time-split sampler, twice (ABBA), one launch, ~40
  hands-off minutes per console, one analysis command.

---

## 1. Method

### 1.1 The twin (tools/drprof)

The PSP's translator (`mips/mips_emit.h`, `mips/mips_stub.S`,
`cpu_threaded.c`) is built unchanged except for platform glue as a static
Linux mipsel program (`platform=drprof-mipsel`, the release `CORE_COMMON`
dynarec flags, the PSP pixel format) and run under qemu-mipsel 9.2 with a TCG
plugin (`tools/drprof/drprof_plugin.c`). The plugin counts **every guest
MIPS instruction executed**, with no sampling error, and attributes it:

* translated code — by the **role** of each MIPS instruction (guest ALU work,
  flag-register write, cycle update/check, reg[] access, memory-stub call,
  dispatcher jump, block link, branch, nop) and by the **guest instruction
  class** that emitted it, from a translation-time class map
  (`DRPROF_TWIN`, `drprof.h`) kept beside the caches;
* the emitter's stub area — per word, named from the stub tables
  (`--stubmap`);
* C and asm — per symbol, except inside the *translation* and *flush* zones
  (marker calls around `translate_block_*` and the flush entry points), which
  are charged to the zone and also broken down by symbol.

What the twin changes, and why it does not matter:

| difference from the PSP build | effect |
|---|---|
| patch handler uses `sltiu`+branch where the PSP uses Allegrex `min`; `synci` where the PSP uses `cache` | a few instructions on region repatching (43–293 instructions/frame measured in total) |
| C compiled by mipsel gcc 12 `-march=mips32r2` instead of psp-gcc `-march=allegrex` | C instruction counts differ slightly; emitted code does not |
| `DRPROF_TWIN` bookkeeping (class map, markers) | not counted by the plugin; **emitted code unchanged**: `dr_oracle.py base base-notwin` IDENTICAL on all fixtures |
| renderer runs on the CPU (no ME) | excluded from "core" (the PSP renders on the ME); reported separately |
| RTC pinned (`gpsp_wallclock`) | Unbound/Emerald deterministic |

**Fidelity, measured.** The twin's per-frame guest state is **bit-identical
to the PSP build** (the harness EBOOT in PPSSPP, which the link study proved
equal to PSP Go hardware) on every field — registers, IWRAM, EWRAM, I/O,
palette, OAM, VRAM — for **all 6,020 frames of the AW2 tour** and **all 3,070
frames of the H&S heavy battle** (the SMC-heaviest fixture). Audio is compared
within one platform only (the PSP harness hashes its output path).

### 1.2 Calibration against hardware (owner's earlier runs)

| fixture | twin core instr/frame | hardware ms/frame (Go) | ns per twin instruction |
|---|---:|---:|---:|
| AW2 tour (ME on) | 869k median | 5.67 median (`gba-link-bench-out/hw-go`) | 6.52 |
| H&S heavy battle | 1,467k mean | ~9.5 (owner's figure) | ~6.5 |

Two very different workloads (dispatch-heavy AW2, translation/flush-heavy H&S)
land on the same ~6.5 ns (≈2.2 cycles at 333 MHz) per instruction. That is
evidence that instruction counts are a usable proxy for time **on average**;
it is not proof per category, which is what the hardware sampler (8) is for.
One known exception: AW2's menu, minimap and CO-screen frames cost ~4.1 ms
more than typical frames on hardware at p95 but only ~1.8 ms more in twin
instructions — about 60% of that spike is not in the instruction stream (ME
capture/fallback or cache effects). It sets the AW2 p95 and it is not dynarec.

### 1.3 Scenes (owner's own states; `tools/drprof/dr_suite.sh`)

| fixture | state | script | measured window |
|---|---|---|---|
| AW2 scene tour | owner's AW2 `.st0` (link bench) | `aw2_psp_script.txt` (link bench tour, identical to the PSP bench) | all 12 scenes, 6,021 frames |
| H&S battle, heavy music | `heart_soul_heavy` | `battle.txt` (soak fixture: mash A through the battle) | 2,800 frames |
| H&S battle, light music | `heart_soul_light` | same | 2,800 |
| Unbound rival battle | `unbound_rival_high` | same | 2,800 |
| Unbound double battle | `unbound_double_high` | same | 2,800 |
| Unbound overworld walk | owner's Go `.st0` (Pokémon Center) | `unbound_ow.txt`: leave, walk the town | 1,220 |
| Emerald wild battle | owner's 3000 `.st0` (Petalburg) | `emerald_battle.txt`: Route 102 grass until a wild battle, mash A | 1,394 |

Screenshots confirmed each scene reaches what it claims (battle, town, tour).

---

## 2. Where the main CPU's instructions go (twin)

"Core" = everything the emulation thread executes inside `retro_run` except
the renderer (on the ME on a PSP) and the host harness. Per frame.

| fixture | core instr/frame mean | p95 | 1 translated | 2 memory | 3 dispatch | 4 cycles + update_gba | 5 translate + flush | 6 other core |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| AW2 scene tour | 888,478 | 1,134,869 | 18.1% | 9.8% | **26.3%** | **24.4%** | 0.1% | 21.3% |
| H&S battle, heavy music | 1,466,610 | 2,082,335 | 17.2% | 11.9% | 12.2% | 12.7% | **26.5%** | 19.5% |
| H&S battle, light music | 700,042 | 1,351,525 | 11.7% | 9.9% | 8.0% | 22.0% | 7.1% | **41.4%** |
| Unbound rival battle | 2,260,116 | 2,607,071 | 18.5% | 16.3% | 18.9% | 12.3% | 14.0% | 20.0% |
| Unbound double battle | 1,540,097 | 1,913,319 | 22.0% | 14.6% | 14.4% | 16.6% | 13.2% | 19.2% |
| Unbound overworld walk | 1,247,128 | 1,609,695 | 18.4% | 13.5% | 14.2% | 14.7% | 19.8% | 19.3% |
| Emerald wild battle | 740,417 | 862,803 | 20.9% | 12.2% | 5.7% | **29.0%** | 0.9% | 31.4% |

Category definitions (`dr_analyze.py`): **1** emitted code doing guest work;
**2** emitted memory-call jal + the emitter's region stubs + C slow paths (I/O,
backup, EEPROM, GPIO, gamepak paging); **3** emitted jumps into the dispatcher
+ `mips_indirect_branch_*`/`lookup_pc` + `block_lookup_*` (not translation);
**4** emitted cycle updates/budget checks + `mips_update_gba` + `update_gba`
itself; **5** translation zone + flush/SMC zone; **6** sound, serial/link,
DMA, IRQ, mode switches, BIOS, libc.

### 2.1 Inside the categories

| fixture | mem: jal | mem: stubs | mem: C | dispatches /frame | instr per dispatch | cycle code in blocks | `update_gba` + stub | translate | flush/SMC | SMC events /frame |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| AW2 | 0.8% | 8.6% | 0.4% | 1,682 | 139 | 1.8% | 22.6% | 0.1% | 0.0% | 0 |
| H&S heavy | 1.1% | 10.2% | 0.7% | 1,225 | 146 | 1.4% | 11.3% | 11.4% | 15.1% | 5.3 |
| H&S light | 0.8% | 8.1% | 1.0% | 397 | 141 | 0.9% | 21.1% | 5.9% | 1.2% | 0.3 |
| Unbound rival | 1.2% | 12.8% | 2.3% | 2,980 | 143 | 1.4% | 10.9% | 5.5% | 8.6% | 4.6 |
| Unbound double | 1.4% | 12.7% | 0.5% | 1,534 | 144 | 1.9% | 14.8% | 5.2% | 8.0% | 2.9 |
| Unbound overworld | 1.2% | 11.8% | 0.5% | 1,249 | 142 | 1.5% | 13.2% | 7.4% | 12.4% | 3.7 |
| Emerald battle | 1.1% | 10.9% | 0.2% | 303 | 140 | 2.0% | 27.0% | 0.9% | 0.0% | 0 |

Findings:

* **Dispatch costs ~140 instructions per indirect branch, in every game.** On
  AW2: ~35 in `mips_indirect_branch_dual` (15 `sw` + 15 `lw` of the mapped
  guest registers + glue), ~49 in `block_lookup_address_dual` (prologue,
  CPSR T-bit, the `_thumb` wrapper, `translate_icache_sync`), ~54 in
  `block_lookup_translate_thumb` (ROM hash and chain walk inside a function
  whose frame is sized for the translator it inlines). *Who* dispatches:
  AW2 96% Thumb `bx` (function returns); H&S heavy **69% block tails ending at
  an SMC translation gate** (`generate_translation_gate` exits through the
  dispatcher) + 27% `bx`; Unbound rival 40% gate tails, 38% `bx`, 18% ARM
  `mov/ldr pc`.
* **`update_gba` (the event scheduler) is 11–27% on its own.** ~680 calls a
  frame from translated code (plus the halt loop), ~250 instructions each:
  timers, sound timer queue, DMA triggers, serial, IRQ checks. It is the
  largest cost on the non-SMC games (AW2, Emerald), and it is not dynarec.
* **The SMC machinery on H&S/Unbound**: each SMC event cost ~42,000
  instructions in `flush_translation_cache_ram_range` (two passes over every
  tag halfword from `lo − 4 KiB`) — found with the plugin's `hot=` mode and
  addr2line; and each RAM block retranslation ~19,000 instructions (H&S 8.8
  blocks/frame).
* **Memory**: ~10 instructions per stub call plus ~2 of argument setup (address
  and a PC constant) in the block. Details in section 4.

### 2.2 "Other core" by subsystem (% of core instructions)

| fixture | sound | serial/link | DMA | IRQ | libc | rest (mode switch, PSR, BIOS) |
|---|---:|---:|---:|---:|---:|---:|
| AW2 | 11.0% | 5.9% | 1.2% | 1.6% | 0.8% | 0.8% |
| H&S heavy | 10.1% | 3.3% | 3.8% | 1.2% | 0.5% | 0.7% |
| H&S light | 24.1% | 7.0% | 5.4% | 2.7% | 0.9% | 1.2% |
| Unbound rival | 7.1% | 2.7% | 2.8% | 1.7% | 0.3% | 5.4% |
| Unbound double | 9.7% | 3.8% | 3.7% | 1.2% | 0.4% | 0.4% |
| Unbound overworld | 10.7% | 3.6% | 2.9% | 1.2% | 0.5% | 0.5% |
| Emerald battle | 19.1% | 5.6% | 3.6% | 1.9% | 0.9% | 0.3% |

**Serial/link emulation costs 3–7% of every frame in solo play**
(`serialpoke_update` / `serialaw_update` / `rfu_update`, `update_serial`,
`serial_next_event`, called from every `update_gba`) with no partner attached.
Sound (`sound_timer`, `render_gbc_sound`) is 7–24%.

---

## 3. The emitted code

| fixture | guest instr/frame | MIPS per guest instr | ALU | flag writes | arg/temp setup | cycle upd+chk | mem calls | SP fast path ld/st | branches | block links | dispatcher jumps | nops | reg[] ld/st |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| AW2 | 42,643 | 4.36 | 23.7% | 17.9% | 31.9% | 8.7% | 5.6% | 2.7% | 3.6% | 2.5% | 0.9% | 2.6% | **0.0%** |
| H&S heavy | 59,289 | 4.89 | 23.3% | 27.3% | 27.1% | 7.1% | 5.7% | 0.6% | 5.6% | 1.3% | 0.4% | 1.7% | 0.0% |
| H&S light | 25,023 | 3.75 | 23.8% | 20.9% | 31.9% | 6.8% | 6.5% | 1.8% | 3.9% | 1.9% | 0.4% | 2.1% | 0.0% |
| Unbound rival | 101,891 | 4.70 | 22.2% | 25.2% | 28.8% | 6.7% | 6.6% | 1.8% | 4.9% | 1.3% | 0.6% | 1.8% | 0.0% |
| Unbound double | 79,855 | 4.89 | 22.5% | 28.1% | 26.7% | 7.3% | 5.7% | 0.5% | 5.7% | 1.3% | 0.4% | 1.7% | 0.0% |
| Unbound overworld | 61,913 | 4.28 | 23.7% | 24.8% | 28.7% | 6.9% | 6.5% | 1.2% | 4.3% | 1.6% | 0.5% | 1.9% | 0.0% |
| Emerald battle | 49,309 | 3.61 | 25.3% | 23.3% | 28.2% | 8.4% | 4.9% | 0.8% | 4.6% | 2.1% | 0.2% | 2.2% | 0.0% |

(Shares of MIPS instructions executed inside translated blocks. "arg/temp"
= writes to `$a0-$a2`/`$at`/`$v0`: memory addresses, PC constants for the
stubs, shift/operand temporaries.)

* **3.6–4.9 MIPS instructions per guest instruction**, and **no register
  spills**: all 15 ARM registers and the four flags live in MIPS registers
  (`mips_emit.h`); the only reg[] traffic is the save/restore inside the asm
  stubs around C calls (30 memory ops per C call — that is where the dispatcher
  pays).
* **Flags are 18–28% of emitted instructions.** Thumb has a per-block liveness
  pass and it works: only 22–36% of flag-setting Thumb instructions generate
  flag code. **ARM has none** — `arm_dead_flag_eliminate()` sets
  `flag_status = 0xF`, so 100% of S-bit ARM instructions compute all four
  flags. ARM flag writes are 2–6% of all core instructions (highest on the
  ARM-heavy SMC games: H&S heavy runs 79% ARM, Unbound rival 75%).
  A `cmp` costs 6.8–7.6 MIPS instructions, 5.5–5.8 of them flag writes.
* **Per guest class** (AW2 / Unbound rival): ALU 3.1 / 4.0, load 4.2 / 4.4
  (+ the stub), store 4.3 / 4.1, `ldm` 9.9 / 11.9, `stm` 10.7 / 12.5, `b` 4.9 /
  5.2 (cycle update + budget check at every block entry point), `bl` 6.5 / 5.7,
  `bx` 3.0 / 2.9 plus ~140 in the dispatcher, literal-pool loads 1.8 / 2.9
  (already resolved at translation time).
* **Branch/link pattern**: direct block links are 1.3–2.5% of emitted
  instructions (cheap, patched `j`); indirect branches are 0.2–0.9% of emitted
  instructions but ~140 instructions each downstream. Unfilled delay slots
  (nops) are 1.7–2.6%.

---

## 4. Memory accesses

| fixture | stub calls/frame | IWRAM | EWRAM | ROM | I/O | VRAM/OAM/PAL | stub instr per call | region-repatch instr/frame |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| AW2 | 7,417 | 64% | 6% | 17% | 8% | 5% | 10.3 | 43 |
| H&S heavy | 15,843 | 88% | 7% | 2% | 2% | 2% | 9.4 | 107 |
| H&S light | 5,342 | 67% | 20% | 3% | 4% | 5% | 10.6 | 78 |
| Unbound rival | 26,835 | 73% | 17% | 2% | 7% | 1% | 10.8 | 260 |
| Unbound double | 21,195 | 73% | 23% | 2% | 1% | 1% | 9.2 | 293 |
| Unbound overworld | 15,533 | 59% | 32% | 6% | 1% | 3% | 9.5 | 124 |
| Emerald battle | 8,098 | 57% | 23% | 16% | 1% | 3% | 9.9 | 169 |

* **Call sites are effectively monomorphic.** Each `jal` is patched to the
  region it last saw; repatching (the patch handlers) costs 43–293 instructions
  a frame in total — negligible. So an inline fast path per site, guarded by
  the same region check the stub already does, would hit almost always.
* **Statically resolvable accesses are already resolved or rare.** PC-relative
  literal loads are constants at translation time (1.8–2.9 MIPS each, no stub).
  SP-relative single loads/stores are 0.2–0.5% of guest instructions; SP block
  transfers use a raw fast path already (the "SP fast path" column). The rest
  are dynamic addresses, 57–88% of them IWRAM, 6–32% EWRAM.

---

## 5. The prototypes (OFF by default; oracle-validated)

### 5.1 `DISPATCH_CACHE` (root make `DISPATCH_CACHE=1`)

A direct-mapped {guest pc → host entry} table, 512 entries per ISA (4 KiB
each), probed by `mips_indirect_branch_{arm,thumb,dual}` in asm **before**
`save_registers` and the C call: ~12 instructions on a hit. Filled only by
the C lookup on its success path (so it only holds pointers the C path itself
returned), keyed on exactly the bits the C path keys on (GBA_PC-masked,
aligned, shifted), and emptied by all three flush entry points (full RAM, ROM,
partial retire — the last because a retired block's stable thunk jumps back
into the dispatcher, and a surviving entry would loop). The `dual` path writes
CPSR.T exactly as `block_lookup_address_dual` does. `DISPATCH_CYCLE_CHECK`
still runs first, so guest timing is unchanged by construction.

### 5.2 `SMC_RETIRE_WINDOW` (root make `SMC_RETIRE_WINDOW=1`)

`flush_translation_cache_ram_range` validates and retires every block that can
cover a write by walking tags from `lo − MAX_BLOCK_SIZE*4` (4 KiB) — twice,
halfword by halfword. The prototype records the longest source extent of any
block since the last full RAM flush (`ramtag_note_extent`) and starts the walk
there. Caveat for productisation: base also full-flushes when a live block
*without* a recorded extent sits anywhere in the 4 KiB window; the prototype
only sees such blocks inside its shorter window. The oracle shows no
difference on any fixture, but the invariant ("every live RAM block has an
extent once partial retirement is active") should be asserted, not assumed,
before it ships.

### 5.3 Results

| fixture | base core instr/frame | DISPATCH_CACHE | SMC_RETIRE_WINDOW | both | both, p95 |
|---|---:|---:|---:|---:|---:|
| AW2 scene tour | 888,478 | −22.9% | 0.0% | **−22.9%** | −22.2% |
| H&S battle, heavy | 1,466,610 | −8.0% | −11.9% | **−19.9%** | −21.2% |
| H&S battle, light | 700,042 | −5.7% | −0.8% | −6.5% | −16.9% |
| Unbound rival | 2,260,116 | −13.4% | −6.8% | **−20.2%** | −21.8% |
| Unbound double | 1,540,097 | −10.7% | −6.3% | **−17.1%** | −19.8% |
| Unbound overworld | 1,247,128 | −7.6% | −9.9% | **−17.5%** | −19.8% |
| Emerald battle | 740,417 | −4.2% | 0.0% | −4.2% | −5.2% |

Instructions per dispatch after the cache: 18 on AW2 (hits), 31–57 on the SMC
games (the table is cleared on every SMC event).

**Oracle** (`tools/drprof/dr_oracle.py`, all fields, audio included):
`base vs dcache`, `base vs rwin`, `base vs both` — **IDENTICAL on all 7
fixtures** (6,061 + 3,100 + 3,100 + 3,100 + 3,100 + 1,705 + 2,100 frames).
**PPSSPP**, the staged harness64 EBOOTs: base vs prototypes IDENTICAL for all
6,020 AW2 frames and 2,899 H&S frames, audio included; PPSSPP's own cost
counter moved −9.1% (AW2) and −9.5% (H&S) — emulator numbers, with the
renderer on the CPU in PPSSPP, so smaller relative effect is expected.

**At 6.5 ns per instruction** (section 1.2; an estimate until the hardware A/B
reports): AW2 −1.3 ms/frame (5.9 → ~4.6), H&S heavy −1.9 ms (9.5 → ~7.6),
Unbound rival −3.0 ms. For AW2 link play (two cores, owner's projection 12.4 ms
average, 19–22 ms p95): average ~9.8 ms and p95 ~16–19 ms. The p95 is set by
menu/minimap frames whose excess over a typical frame is only ~40% instructions
(1.2); the rest is not dynarec and is what sampler run 3 should explain.

---

## 6. Ranked opportunities

Gain = twin core instructions (ms at 6.5 ns on the Go). Every item that keeps
guest timing identical is validated by `dr_oracle.py` (bit-exact per frame);
items marked **timing** move cycle accounting and cannot pass it — they need an
argued A/B plus ears, like SMC gates did.

| # | opportunity | gain | risk | effort | how the oracle validates it |
|---|---|---|---|---|---|
| 1 | **Dispatch cache** (prototyped, 5.1) | 4–23%; AW2 −23% (−1.3 ms), Unbound rival −13% | low: speed-only; invalidation covers every flush entry point | done; hardware A/B + review | IDENTICAL, 7 fixtures + PPSSPP |
| 2 | **SMC retire window** (prototyped, 5.2) | 6–12% on SMC games (H&S heavy −1.1 ms), 0 elsewhere | low–medium: assert the extent invariant | done; hardware A/B + invariant check | IDENTICAL, 7 fixtures + PPSSPP |
| 3 | **Serial/link idle fast path**: skip the per-event serial update when no link is active | 3–7% on every game | medium: absent-partner timeouts must stay cycle-exact | low | must be IDENTICAL (if a game observes the idle serial clock, the oracle says so) |
| 4 | **Event scheduler diet** (`update_gba`: per-event work, batching timer/sound updates, fewer calls in the halt loop) | up to 10–20% on AW2/Emerald (it is 22–27% there) | medium–high: timing core | medium–high | IDENTICAL if cycle-exact; any reorder of IRQ delivery is **timing** |
| 5 | **Inline memory fast paths** for IWRAM/EWRAM (same region guard inline, stub as slow path; drop the PC constant from the fast path) | ~4–7% (stubs are 9–16% + ~2 setup instr per access) | medium: SMC check on inline stores; code growth → I-cache pressure (hardware must price it) | medium–high | IDENTICAL (same accesses, same cycles) |
| 6 | **Retranslation memo for SMC blocks**: reuse a previous translation when the patched block's source words match one seen before (H&S's mixer toggles a few values) | up to ~5–10% on H&S/Unbound (translation is 5–11%, ~19k instr/block) | medium: must key on exact source + gate layout | medium | IDENTICAL |
| 7 | **ARM flag liveness** (port the Thumb pass) | 1.5–4% (ARM flag writes are 2–6%; Thumb's pass removes 64–78%) | low–medium: conservative at exits | low–medium | IDENTICAL |
| 8 | **Gate exits as direct links** (69% of H&S dispatches are gate tails; `SMC_PARTIAL_DIRECT_GATE` exists, untested) | most of it is already recovered by #1; small on top | medium | low | IDENTICAL if it only changes how the exit is reached |
| 9 | `cmp`+branch fusion | ~1% | medium | medium | IDENTICAL |
| 10 | In-block cycle accounting | ≤2% total — not worth it | — | — | — |
| — | Sound (7–24%) | large on some games, not dynarec | — | — | IDENTICAL for pure-speed rewrites |

---

## 7. Recommendation: targeted surgery, not a new translator

1. **The prize a new translator could claim is the smallest slice.** Emitted
   guest work is 12–22% of core instructions; with memory-call setup, ~25%.
   The translator already maps every guest register (no spills) and has Thumb
   flag liveness. A better code generator might halve that slice: ~10%.
2. **The big costs are around the translator and are local**: dispatcher,
   SMC retirement, event scheduler, serial, stubs. Two afternoon-sized changes
   already remove 17–23% on every heavy fixture, provably without changing
   what the guest computes.
3. **A new translator would re-open what took months to close** — SMC
   correctness (the gate/retire saga, the Heart & Soul crash) and the timing
   model. Its block boundaries would differ, so its cycle accounting would
   differ, so **it could not pass the bit-exact oracle**; it would need a new
   audio-by-ear validation campaign across the library.
4. Order: land #1 and #2 after the hardware A/B; then #3 (cheap, broad);
   then #5 and #4 with hardware pricing of I-cache effects; #6/#7 as needed
   for H&S/Unbound.

---

## 8. The hardware bench (staged, not run)

**One rig app, one launch** (standing rule: hardware benches are a single
app driven by the USB handoff loop). `builds/dynarec-profile-out/` —
`OWNER-STEPS.txt`, `rig-stage/` (`MANIFEST.json`, `base/`, `eboots/`),
`rig-dryrun/`. Driver: `tools/drprof/drrig.py` (stage / plan / install /
run), built on the `harness-kit/hw_loop.py` primitives the way
`tools/rig/resrig.py` is (21 cycles on the Go, 2026-09-26).

App `GBADHOC-DRPROF`, XMB title `DRPROF RIG`. Twelve cycles, ABBA:

| cycles | arm | EBOOT staged | fixture |
|---|---|---|---|
| 1, 12 | A-BASE | build-A (harness64, shipped dynarec) | AW2 tour, ~1:45 |
| 2, 11 | A-PROTO | build-B (+ DISPATCH_CACHE + SMC_RETIRE_WINDOW) | AW2 tour |
| 3, 10 | A-SAMP | build-S (+ DRPROF_HW, `drprof_us = 1000`) | AW2 tour |
| 4, 9 | H-BASE | build-A | H&S heavy battle, ~1:00 |
| 5, 8 | H-PROTO | build-B | H&S heavy battle |
| 6, 7 | H-SAMP | build-S | H&S heavy battle |

Between cycles, while the console is parked in its handoff window, the PC
collects `log/` (`shash.txt`, `frontend.log`, `heartbeat.txt`) and
`handoff/` into `autoNNN-<arm>-solo/` with a `meta.json`, restores both
fixtures' golden `.sav`/`.st0`, copies the next arm's EBOOT (md5-verified)
and ini, writes `CMD.TXT = RUN` and ejects; the console relaunches itself.
ME renderer on, ME_CATCH PRX. PSP Go first, then the PSP-1000.

**The 2026-09-29 Go attempt, and the guard it produced.** The first staging
(six separate apps, handoff keys added by the coordinator) was installed on
the Go's **internal storage** (log: `base_dir=ef0:/...`). The run completed
(`ap_done frame=6061`, `exit code=0`, a usable AW2-base hardware run), then
`handoff_run` exported the volume with `usbstorms.prx` — the **Memory Stick**
driver — so the PC saw the Go's empty M2 card (E:) while `hw_loop` watched
the internal volume (G:) and timed out; the Go sat parked on the run's last
frame (the AW2 title), which reads like a relaunch. This is the documented
limit in `docs/ROM-RESIDENCY.md` ("Which Go volume": the resrig ran from the
M2 card, E:). The EBOOTs now refuse the handoff when their base directory is
`ef0:` — `EVT handoff_refused ...` plus an on-screen notice — and run once
to the XMB instead of parking invisibly.

**PPSSPP dry run of the whole loop** (`tools/drprof/drrig_dry.sh`: install,
one launch, then every handoff, collection, golden restore, EBOOT/ini swap
and relaunch): see `rig-dryrun/` and section 8.1.

**What it answers.** (a) The real speed of the prototypes (per-scene ms,
base vs proto) and a **hardware oracle** (base vs proto must be identical on
every frame, audio included). (b) The **time split on real hardware**
(`DRPROF_HW`): the core publishes a phase word (`reg[40]`, written by the
`cfncall` macro around every C call from translated code and by a scope at
the top of `update_gba`, the lookups, translation, flushes, memory slow paths,
sound, DMA, serial, IRQ and `update_scanline`); a 0x0F-priority thread
samples it at 1 kHz and counts a sample only when the emulation thread was
running (`sceKernelReferThreadRunStatus` READY). Dividing each phase's
hardware time share by its twin instruction share (table below) gives its
relative cycles-per-instruction on the Allegrex — the direct answer to "does
the instruction split translate to time". (c) What AW2's menu/minimap spike
is (`video` phase = `update_scanline` + ME capture/fallback).

Twin instruction shares in the sampler's phases (renderer excluded):

| fixture | jit (incl. stubs, asm glue) | disp | upd | xlat | flush | memc | sound | dma | serial | irq | other |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| AW2 | 40.3% | 19.5% | 18.8% | 0.1% | 0.0% | 0.4% | 11.0% | 1.2% | 5.9% | 1.6% | 1.1% |
| H&S heavy | 34.3% | 9.1% | 10.4% | 11.4% | 15.0% | 0.6% | 10.1% | 3.8% | 3.3% | 1.1% | 0.9% |

(`jit` here includes the dispatcher's register save/restore, which run before
`cfncall` sets the phase.) Sampler cost in PPSSPP: +4% frame time; the report
prints it on hardware (sampler vs base). The sampler's phases do not split
translated code from the inline memory stubs (they write nothing); the twin
does.

**Analysis**: `python tools/drprof/hw_report.py --rig builds/dynarec-profile-out/logs-<model>`
(per fixture and round: base / proto / sampler ms per scene, the hardware
oracle, the time split, the sampler's cost).

### 8.1 PPSSPP dry run of the loop

`tools/drprof/drrig_dry.sh` against a PPSSPP memory stick, from the exact
`rig-stage/` files: install, **one launch**, then **all 12 cycles** with **11
handoff relaunches** (`CMD=RUN` ×11 in `STATE.TXT`), no freeze, 0 dropped
log lines. Every cycle's log reports the EBOOT CRC of *its* arm (base
`94ffb5e4`, proto `8ca97ff9`, sampler `a51b4194`) and its own `run_id`/`arm`,
which proves the relaunch picked up the EBOOT the PC staged during the
handoff, across both fixture switches (AW2→H&S, H&S→AW2). `hw_report.py
--rig` on the collected logs: both rounds of both fixtures report base vs
proto and base vs sampler IDENTICAL on every frame, audio included (6,020 AW2
frames, 2,899 H&S frames); emulator costs proto −9.2% (AW2) / −9.5% (H&S),
sampler +4.3% / +3.7%. Evidence: `builds/dynarec-profile-out/rig-dryrun/`
(`loop.log`, `REPORT.txt`, one `autoNNN-<arm>-solo/` per cycle).

Not covered by PPSSPP, and not claimed: physical USB enumeration and ejection
(proven on hardware by resrig), and the ef0 refusal (a four-line check on the
logged base directory).

---

## 9. The differential oracle (`tools/drprof/dr_oracle.py`) — the gate for dynarec work

```
tools/drprof/dr_build.sh base                          # the shipped dynarec
tools/drprof/dr_build.sh mychange "-DMY_CHANGE=1"      # a translator variant
python3 tools/drprof/dr_oracle.py base mychange        # PASS / FAIL, exit 0/1
```

Runs every fixture in `dr_suite.sh` on both variants from the same state,
save and script, and requires identical per-frame hash lines: audio (rolling
FNV of every sample + count), r0–r15/CPSR, pc, IWRAM, EWRAM, I/O, palette,
OAM, VRAM. ~2 minutes for the whole library on this PC. Controls, all run:

| control | expected | result |
|---|---|---|
| base vs base | PASS (determinism) | PASS, 7/7 |
| base vs base without `DRPROF_TWIN` | PASS (bookkeeping perturbs nothing) | PASS, 7/7 |
| base vs `DRPROF_NEGCTL` (+1 cycle per Thumb instruction) | FAIL everywhere | FAIL, 7/7, from frame 1–2 |

Rules: a speed-only change must PASS; a timing change cannot, and must be
argued and A/B'd by ear (SMC gates). The fixture library is the owner's
states; add one by adding a line to `SUITE` in `dr_suite.sh` (state, save,
script, frames). The same hash lines come from a PSP harness EBOOT with
`shash = 1`, so the identical comparison runs in PPSSPP
(`drrig_dry.sh` + `hw_report.py --rig`, or `tools/linkbench/cmp_hash.py`)
and on hardware (the bench above). PPSSPP and hardware compare audio only
within one platform.

---

## 10. Reproduce

```
# once: the image (qemu 9.2.2 with plugins + mipsel gcc); dr_build.sh does it if missing
docker build -t drprof-qemu tools/drprof/docker
# fixtures into $DRPROF_HOME/fx (default ~/drprof/fx): see dr_suite.sh SUITE
tools/drprof/dr_build.sh base
tools/drprof/dr_suite.sh base base              # profile all fixtures (~45 s, parallel)
python3 tools/drprof/dr_tables.py ~/drprof/runs/base [--cmp ~/drprof/runs/<variant>]
HOT=<symbol> SAV=x.sav tools/drprof/dr_profile.sh out rom st script frames base   # per-address
```

PSP EBOOTs: `tools/build.sh harness64` with `CORE_EXTRA="DISPATCH_CACHE=1
SMC_RETIRE_WINDOW=1"` (B) or `CORE_EXTRA="DRPROF_HW=1" FE_EXTRA="-DDRPROF_HW"`
(S); stage with `tools/drprof/drrig.py stage` (one rig app). Raw twin profiles and the
tables: `builds/dynarec-profile-out/twin-results/`.

## 11. Standards kept

* Everything new is behind a switch that is OFF by default; `gpsp_profile.h`
  refuses `DRPROF_*` in a release; `tools/build.sh` refuses `CORE_EXTRA` /
  `FE_EXTRA` with the release profile.
* `tools/build.sh release` passes its gates, and **every release core object
  disassembles identically** to the pre-study core (6fcc46c): 18 objects
  compared with `psp-objdump -d`. No `drprof`/`dispatch_cache` string in the
  release ELF.
* Host tests: `python tools/run_host_tests.py` 30 passed, 0 failed.
* `psp/me` binaries restored after each build.
