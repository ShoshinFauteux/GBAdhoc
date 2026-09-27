# Holding a 32 MiB cart whole on a 64 MiB PSP

2026-09-26, branch `claude/rom-resident` (from `fd672be`). Question: why did
letting a 2000/3000/Go keep a 32 MiB ROM fully resident break Pokemon Unbound,
and is full residency possible at all?

> **Status (2026-09-27, release 3.0.0):** residency is ON by default
> (`rom_resident = 1`). Decided on the PSP Go A/B rig (21/21 frame-identical to
> the paged golden). Shipped merged with `claude/jit-runtime-size`.
>
> **Correction (2026-09-27, `claude/rom-resident-budget`):** the 3.0.0
> candidate never engaged residency on a real PSP Go (loading bar total 15 MB).
> Its "4.6 MiB margin" was a PPSSPP number, and PPSSPP's 64 MiB layout gives
> the process **5.00 MiB more heap than a real Go**. The block-by-block attempt
> and the lent JIT array below fix it. See
> [The PSP Go never engaged it](#the-psp-go-never-engaged-it-2026-09-27). The
> 2026-09-16 PSP-3000 failure itself is still unexplained.

## Answers

1. **Full residency is possible in memory terms.** Measured in PPSSPP with the
   64 MiB layout (`PSPModel = 1`, `MEMSIZE = 1`), `EVT heap_budget` after the
   ROM loads:

   | build | ROM blocks | JIT tier | post-load heap free (largest) | ROM blocks span |
   |---|---:|---|---:|---|
   | `fd672be` (static JIT) | 15 | small, .bss | 33.43 MiB (32.46) | 0x0910A9C0 - 0x0A00AAA0 |
   | `fd672be` (static JIT) | 32 | small, .bss | **15.60 MiB** (14.63) | 0x0910A9C0 - 0x0B10ABB0 |
   | + `claude/jit-runtime-size` | 15 | large, heap | 22.42 MiB (21.45) | 0x09B898E0 - 0x0AA899C0 |
   | + `claude/jit-runtime-size` | 32 | large, heap | **4.60 MiB** (3.63) | 0x09B898E0 - 0x0BB89AD0 |

   Both fit. With the large translation caches the margin is 4.6 MiB. The
   frontend needs about 1 MiB of that (the post-load reserve), and 4 MiB was
   the slack the JIT tier was sized to keep. A PSP-1000's whole heap is about
   22 MiB, so it can never hold a 32 MiB cart. Nothing here changes it.

2. **The residency path is not the bug.** Paged and resident emulation are
   bit-identical in PPSSPP. That holds on the current tree, and on **the exact
   EBOOT that failed on the PSP-3000** (`4ccba45e`, sprt17b).

3. **The 2026-09-16 failure is hardware-only and deterministic**, and its
   mechanism is not yet known. The A/B below decides it. Until it passes, the
   default stays 15 blocks.

## Differential evidence (PPSSPP)

Oracle: `shash = 1` writes one line per frame to `log/shash.txt`. It records
the rolling audio hash, r0-r15/CPSR, and hashes of IWRAM, EWRAM, I/O, palette,
OAM and VRAM. It writes directly, not through the event ring, because the ring
drops lines: the first 32-block arm kept only 314 of 9,300 `vhash` lines.
Fixtures are `unbound_double_high` and `unbound_rival_high`: slot-0 restore,
then 1,500 x (A for 2 frames, wait 4), with no fast-forward and the ME off.

| comparison | frames compared | result |
|---|---:|---|
| `fd672be` 15 vs 32 blocks, double_high | 9,270 post-load | identical, audio `fa42ef1f` both |
| `fd672be` 15 vs 32 blocks, rival_high | 9,270 post-load | identical, audio `d5df7821` both |
| RC `4ccba45e` (32, resident) vs ROM15f `b82e958d` (15), the hardware job unchanged | 3,931, 28 frame dumps | all dumps identical, audio `ce3a1dde` both, SMC windows 1383/2183/2677/799/1538/1310 both |
| each A/B switch below vs default, double_high | 9,270 post-load | all identical |

**The only difference found is before the state load and is not a bug.** At
boot, IWRAM differs in two bytes at 0x03005E8E and 0x03005EA8: the RTC
seconds, 03 vs 05. The RTC base is read from the wall clock when the cart
loads. Reading 32 MiB takes about 2 s longer than 15 MiB, so the base is later.
The savestate restores the base, and `RTC_PIN` removes this for boot-time
comparisons.

## The 2026-09-16 hardware evidence, re-read

`builds/unbound-soak/`, PSP-3000, fixture `unbound_double_high`, job script
450 x A, ME on (`me_mode = 1`). `core_prof win=` is rom_flush / ram_full /
smc / dma / page_loads per 600-frame window:

| run | EBOOT | blocks | windows | outcome |
|---|---|---:|---|---|
| 1, 2, 7, 8, 9, 10 | RC sprt17b (`439c3d3` flags) | 32 | 3/7/1383/0/0, **1/9/1761/0/0**, 0/0/0/0/0 | corruption, then freeze. 6/6, identical counts |
| ROM15f 112, 118, 123, ... | same flags, 15-block cap | 15 | 3/7/1383/0/98, 1/12/2183/0/18, 0/14/2677/0/8, ... | pass |
| PPSSPP (this study) | the same RC EBOOT | 32 | 1383, 2183, 2677, 799, 1538, 1310 | pass |
| mem32 run 3 | `7a78499` (no SMC partial) | 32 | 2/0/2328/0/0, ... | pass (n=1) |

The failure needs all three of:

- the SMC-partial dynarec that `439c3d3` introduced;
- 32 resident blocks;
- real hardware.

It starts between frames 600 and 1200. The window-2 counts are identical in
all six runs, so it is **deterministic, not a race**. On the same hardware,
paging does 98 then 18 page loads per window. The resident run does none.
Resident also emits shorter ROM load stubs (`must_swap = false`), so every
later ROM translation sits at a different address.

Mechanisms that fit (hardware-only, deterministic, sensitive to code layout),
most likely first:

1. **A JIT cache-coherency hole.** PPSSPP does not model the caches. The page
   I/O's kernel cache maintenance, or the different code layout, would decide
   whether it shows. This is the same suspect as the long-running Heart & Soul
   freeze (`hns-crash-soft-in-3-0`).
2. **Code-layout sensitivity alone.** The shorter stubs move the ROM
   translation cache.
3. **Where the data lives.** Blocks above 0x0A000000 only exist in the 64 MiB
   layout.
4. **An interaction with the ME.** Every failing run had it on.

## The hardware A/B that decides it

Seven arms. Each differs from CTRL in harness ini keys only, and every key has
been checked emulation-neutral in PPSSPP.

| arm | extra ini keys | isolates |
|---|---|---|
| CTRL | none (15 blocks, paged) | control |
| RES | `rom_cap = 32` | does the current tree still fail resident? |
| RES-SWAP | `rom_cap = 32`, `swap_stubs = 1` | stub size / code layout vs residency |
| RES-PARANOID | `rom_cap = 32`, `cache_paranoid = 1` | cache coherency |
| BALLAST | `rom_ballast_kb = 17408` | ROM data above 0x0A000000, still paged |
| RES-MEOFF | `rom_cap = 32`, `me_mode = 0` | the Media Engine |
| RES-SCAN | `rom_cap = 32`, `jit_coherency_scan = 60` | names an unsynced code word (`EVT jit_coh n>0`) |

How to read the results:

- **RES passes 3/3:** the 09-16 failure does not reproduce on this tree. Run a
  long residency soak (battlecycle, Unbound and H&S) before changing the
  default.
- **RES fails:**
  - **RES-PARANOID passes:** coherency hole. RES-SCAN should name the word.
  - **RES-SWAP passes:** layout sensitivity. The bug is elsewhere and residency
    only exposes it; do not ship `swap_stubs` as a fix.
  - **BALLAST fails:** an address-range effect.
  - **RES-MEOFF passes:** the ME.

### The rig: `tools/rig/resrig.py`, unattended, on the PSP Go

**Console.** The PSP Go: 64 MiB, and the console that has frozen most. The
3000 and the 1000 are reserved for the wireless double-battle rig.

**App folder.** `PSP/GAME/GBADHOC-RESRIG`. resrig refuses any other appdir and
reads or writes nothing outside that folder. The owner's `GBADHOC` install is
never touched.

**Build.** `tools/build.sh harness64`: the harness, MEMSIZE=1, and the ME PRX
with `ME_CATCH=1` as in the release. XMB title **GBAdhoc HARNESS64**.

**Job.** The 2026-09-16 `unbound_double_high` job, unchanged except for three
things:

- `me_sameframe` is dropped from CONFIG.INI; this build does not read it, and
  the audit reported it `cfg_unknown`;
- the rig instruments are added: `shash = 1`, `heartbeat_s = 5`,
  `log_input = 0`;
- `run_id` and `arm` are added.

**Plan.** Seven arms x three rounds = 21 cycles. Each round rotates the arm
order by three places, so no arm always follows the same one
(`resrig.py plan` prints it).

**Loop.** resrig wraps `harness-kit/hw_loop.py`'s primitives (`parse_result`,
`flush`, `eject`, `copy_retry`, `card_root`) without changing them.

The wireless rig's `--arms/--order` fork (claude/wireless c752c0a) is not used
yet: it takes single-letter arms, guards its appdir to GBADHOC-RIG, and has
not been run solo. resrig follows that fork's conventions:

- `run_id = <cycle>-<unix>`;
- `arm =` in the staged ini;
- `meta.json` holding drive, role, arm, run_id, eboot_crc32/md5, prx_md5,
  cycle and console_run.

Each cycle it:

- collects **all** of `log/` (frontend.log, shash.txt, heartbeat.txt,
  stall.txt, the `.prev` files, frame dumps) and `handoff/`;
- restores the golden `.sav`/`.st0`;
- stages the next arm's ini;
- writes CMD.TXT, then flushes and ejects.

It checks the card's volume serial against `RIG.TXT` every cycle, so a
drifted drive letter is refused.

**Freeze.** If RESULT.TXT has not appeared after 10 minutes, the cycle is
logged as a freeze (`FREEZE-cNNN-<arm>.txt`), with the arm and run_id that
were running. The loop keeps waiting. After the owner power-cycles and
relaunches, the app keeps the frozen attempt's evidence:

- `frontend.prev.log` and `shash.prev.txt`, both preserved at boot;
- the appended `heartbeat.txt`.

resrig collects them with the retry, which re-runs the same arm.

### Telemetry standard (the wireless rig's, met item by item)

| requirement | how |
|---|---|
| every applied key echoed and checked | `EVT cfg src=harness key= raw=` for every ini key and `cfg_unknown` for a key nothing read (ported from claude/wireless de5e724). The APPLIED values are echoed too: `rom_diag rom_cap= swap_stubs= cache_paranoid= jit_coh_every= ballast_kb=`, `rom_cache_cap`, `rom_cache blocks= resident= lo= hi= plan=`, `me_mode on=`. The summarizer (I4) requires staged = echoed = applied. Every key was grepped against the source before staging (harness-ini-key-trap) |
| run_id + arm + EBOOT CRC everywhere; mismatches refused | the log (`rig`, `build eboot_crc`), the `shash.txt` header, every heartbeat line and `meta.json`. The summarizer (I2, I3) refuses any disagreement, and any CRC or PRX md5 that differs from the stage manifest |
| liveness independent of the log | `heartbeat.txt`: the 0x10 stall thread appends a line every 5 s with raw sceIo (frame, loop iteration, phase, drops, shash count, identity). RESULT.TXT carries `frames= evt_drop= t_ms=`, written after the log closes. A frozen run reads "frame STUCK" if the heartbeat kept beating, and ends at its last beat if it did not |
| dropped lines visible | in-band `EVT evt_gap dropped= total=` (de5e724), `evt_drop total=` at exit, `evt_drop=` in RESULT.TXT and in every heartbeat. The oracle does not go through the ring |
| missing reads "unknown" | every summarizer field. Missing SMC census windows are counted as unknown |
| outcome = shash oracle + SMC counters | per-frame `shash.txt` compared from frame 31 against the PPSSPP golden of the SAME stage and against this campaign's CTRL. Frames 14-30 carry the RTC's wall-clock seconds; frame 31 is the first after the savestate load. `core_prof win=` is compared window by window, as in the 09-16 analysis |
| the summarizer itself | `tools/rig/test_resrig_summary.py` (host suite `resrig_summary`). Each of these must give the right verdict FOR THE RIGHT REASON: wrong arm, foreign CRC, `cfg_unknown`, an un-echoed key, an applied value different from the staged one, diverged guest state, a missing oracle, and a stuck heartbeat |

### Dry run in PPSSPP

The dry run is the whole loop against a PPSSPP memory stick
(`resrig.py install/run --drive <dir>`). The console's USB handoff, CMD.TXT
and relaunch all work in PPSSPP, so the rig's mechanics were exercised end to
end: every cycle, collection, golden restore, ini staging and relaunch.

The dry run of the final stage is the Go campaign's golden
(`builds/resrig-go-2026-09-26/golden`; stage EBOOT crc32 `bbb5dc90`, built at
8208889; nothing console-side has changed since). Result: **21/21 cycles PASS**
against its first CTRL, 7 arms x 3. Every run is identical from frame 31
through 3931, the SMC census matches in all 6 windows, 0 invalid and 0 dropped
lines (`golden/summary.txt`).

**Freeze path, tested.** `freeze_test.sh` killed the emulator in the middle of
cycle 2 (RES), at about frame 1250, and relaunched it the way the owner would.
The loop then:

- logged the freeze after `--freeze-s`;
- collected the retry together with `frontend.prev.log`, `shash.prev.txt`
  (1,200 lines, flushed every 120) and both boots' heartbeats;
- went on to cycles 3 and 4.

The summarizer reported `FROZE (attempt 1: identical through f=1200); retry:
PASS`. Solo PPSSPP sandboxes
set `EnableAdhocServer = False` and `EnableWlan = False`; otherwise they take
the global port 27312 from the wireless rig.

### Owner steps (after the coordinator's handshake)

Commands are run from `builds\rom-resident-worktree`.

**0. Which Go volume.** The in-app USB export loads `usbstorms.prx`, the
Memory Stick driver, so the rig must live on the Go's **Memory Stick (M2)**,
not its internal storage. The handoff has never run on a Go.

- With the Go in XMB USB mode, identify the M2 volume by content; on
  2026-09-25, G: held GBADHOC.
- If the Go has no M2 card, stop: the rig then needs an ef0 export first.
- About 36 MB must be free on that volume.

**1. Backup.** Read-only, verified, before any write:

    python tools\rig\backup_sticks.py --out ..\go-stick-backup-2026-09-26 G:=PSP-Go

It must end with `MANIFEST.verified`, and `MANIFEST.txt` must record the
volume serial.

**2. Install.** The only step that writes to the Go. It refuses without step
1's manifest for that volume serial:

    python tools\rig\resrig.py install --stage ..\resrig-go-2026-09-26\stage --drive G: --backup ..\go-stick-backup-2026-09-26

Then eject G: from Windows and leave the Go's USB mode.

**3. Start the loop, detached:**

    Start-Process python -ArgumentList 'tools\rig\resrig.py','run','--stage','..\resrig-go-2026-09-26\stage','--drive','G:','--logs','..\resrig-go-2026-09-26\logs' -RedirectStandardOutput ..\resrig-go-2026-09-26\loop.log -WindowStyle Hidden

It resumes where it stopped (`logs\RIGSTATE.json`).

**4. Launch the app once.** On the Go, on AC power, USB cable in, slider open:
XMB → Game → **GBAdhoc HARNESS64**. From then on:

- it runs cycle 1 and hands the card over USB;
- the PC relaunches it for cycles 2-21;
- a cycle is about 75 s of game plus the handoff, so all 21 take about an
  hour;
- the owner does nothing unless it freezes.

**5. If it freezes** (picture stuck, or the corruption seen on 09-16):

- If the blue ME_CATCH screen is showing, photograph it.
- Hold POWER until the Go switches off, then power on.
- Relaunch GBAdhoc HARNESS64. The loop picks it up.

**6. Summarize:**

    python tools\rig\resrig_summary.py --logs ..\resrig-go-2026-09-26\logs --stage ..\resrig-go-2026-09-26\stage --golden ..\resrig-go-2026-09-26\golden --model PSP-Go

## Enabling residency, and the merge with `claude/jit-runtime-size`

### One startup memory budget

Both decisions are made once, in retro_init, in this order:

1. **JIT tier** (`dynarec_select_translation_caches`, jit-runtime-size,
   unchanged). It takes the large 10.5 MiB block only if the large tier plus
   `(ROM_BUFFER_SIZE + 1 + 4)` MiB fits. That is the paged 15-block cap plus
   the reserve and the frontend slack. When it does, the static SMALL arrays
   are idle for the life of the process, and the 2 MiB ROM array is **lent to
   the ROM cache** as two 1 MiB blocks (`gamepak_spare_pool`).
2. **ROM cap** (`init_gamepak_buffer`). This is the last allocation of
   startup. The frontend states the wish as the cart's size in 1 MiB blocks
   (CONFIG.INI `rom_resident = 1`: Emerald asks for 16, Unbound for 32). If
   the cart is larger than the cap, the core takes its blocks **one by one
   while holding the 1 MiB post-load reserve**: spare-pool blocks first, then
   the heap. It keeps them only if every block was granted. Then the load
   leaves the same >= 1 MiB floor a PSP-1000 always lives on. If any block is
   refused, the extra blocks go straight back, newest first, and the cart
   pages at the cap exactly as it would have without the wish.

Residency is still granted only from what the JIT tier leaves: the large JIT
tier is worth more to Unbound than residency (flush storms against at most
~100 page loads per 600 frames), so it keeps first claim. On a PSP-1000 the
attempt is the same sequence of successful mallocs as the paged path (the loop
fails at the same block either way). `EVT rom_cache_cap ... want=` states the
wish; `EVT rom_cache ... plan= want= got= spare=` reports `default`,
`resident` or `heap_short`, how many blocks the attempt reached, and how many
came from the spare pool.

**Superseded (3.0.0 candidate):** one contiguous probe malloc of 32 + 3 MiB
after the JIT tier. Measured on a scratch merge, in PPSSPP only:

| layout | JIT tier | `rom_resident` | plan | ROM blocks | heap free after load |
|---|---|---|---|---:|---:|
| 64 MiB | small (`jit_small = 1`) | 0 | default | 15 | 33.41 MiB |
| 64 MiB | small (`jit_small = 1`) | 1 | resident | 32 | 15.58 MiB |
| 64 MiB | large (auto) | 0 | default | 15 | 22.40 MiB |
| 64 MiB | large (auto) | 1 | resident | 32 | 4.57 MiB |
| 32 MiB (PSP-1000) | small (auto; large probe fails) | 0 | default | 13 | 1.95 MiB |
| 32 MiB (PSP-1000) | small (auto) | 1 | heap_short | 13 | 1.95 MiB (unchanged) |

The large-JIT resident row does not exist on a real Go; see the next section.

### The PSP Go never engaged it (2026-09-27)

Field report: the 3.0.0 candidate on the owner's PSP Go showed a loading bar
total of 15 MB for Unbound, so the cap stayed 15.

**Cause: PPSSPP's 64 MiB layout overstates the heap by 5.00 MiB.** The resrig
campaign ran the same EBOOT (`bbb5dc90`) through the same boot on both:

| arm | PPSSPP `heap_budget` after load | PSP Go | difference |
|---|---:|---:|---:|
| CTRL (15 blocks) | 33,403,768 B | 28,160,888 B | 5,242,880 B |
| RES (32 blocks) | 15,577,704 B | 10,334,824 B | 5,242,880 B |

The kernel's free memory outside the heap is identical on both (`mem_free`
765,952 B). The heap itself, which takes all but 1 MiB of the user partition
at the first malloc, is exactly 5 MiB smaller on the Go. PPSSPP maps the user
partition up to 0x0C000000; the Go's firmware (and whatever it loads first)
leaves less. Whether a 2000/3000 shows the same gap is not measured.

**Measured on the 3.0.0 candidate** (harness64, PPSSPP, Unbound;
`EVT heap_census`, whose `largest` is bisected with malloc itself):

| moment | free | largest |
|---|---:|---:|
| before the ROM browser (`pre_select`) | 49,171,336 | 49,164,288 |
| after the browser (`post_select`) | 49,171,336 | 49,164,288 |
| before the JIT decision (`pre_jit`) | 49,166,936 | 49,160,192 |
| at the residency probe (`pre_probe`) | 38,156,872 | 38,150,144 |

- **The browser is not it.** It leaves 0 bytes behind, in both shells, after
  36-44 box/hero art decodes, favourites, the state shelf and three console
  switches (`free`, `top` and `largest` are identical before and after).
- **Binary growth is not it.** The probe asks 36,700,160 B. PPSSPP still
  passes it with 1.38 MiB (the scratch merge had 1.6 MiB). A release binary
  is 238,296 B smaller than harness64, so it has that much more heap.
- **The Go is 5 MiB short.** 38,150,144 - 5,242,880 = ~31.4 MiB at the probe
  (~31.6 MiB in a release). That is less than 35 MiB, and less than even 32
  blocks plus the 1 MiB floor. No slack setting could have made it pass
  beside the large JIT tier.
- **Reproduced in PPSSPP.** `rom_ballast_kb = 5120` holds exactly the Go's
  missing 5 MiB before the JIT decision. The 3.0.0 candidate then logs
  `plan=heap_short`, 15 blocks: the field behaviour.

**The fix** is the budget above. Lending the idle 2 MiB static array means a
32 MiB cart needs 30 heap blocks plus the reserve. The 3 MiB slack becomes
the 1 MiB floor the PSP-1000 already runs every game on. The post-load
allocations the slack was meant for (frame buffer 77 KB, ME stages' heap
fallback ~250 KB, RFU hold ring 27 KB, viewport shadow 96 KB) all have to
fit in that floor on a 1000 anyway.

| build, PPSSPP 64 MiB | ballast | plan | blocks (spare) | heap free after load |
|---|---:|---|---:|---:|
| 3.0.0 candidate | 0 | resident | 32 (0) | 4,530,280 B |
| 3.0.0 candidate | 5120 KB (Go) | **heap_short** | 15 (0) | 17,113,448 B |
| fix | 0 | resident | 32 (2) | 6,626,696 B |
| fix | 5120 KB (Go) | **resident** | 32 (2) | 1,383,800 B |
| fix | 5376 KB | resident | 32 (2) | 1,121,656 B |
| fix | 5440 KB | heap_short (got 31) | 15 (2) | 18,882,184 B |
| fix, browser path | 5120 KB (Go) | resident | 32 (2) | 1,375,608 B |

The Go margin is therefore 256-320 KiB in a harness64 build, and about
0.5 MiB in a release (+238 KB). It is decided by the memory actually granted
at startup: a Go with less (a larger future binary, a plugin in user memory)
pages exactly as before; it does not starve.

PSP-1000 layout (harness, 32 MiB): 3.0.0 candidate vs fix, Unbound, both 13
blocks, `heap_short`. Post-load heap free is 1,909,160 vs 1,908,392 B: the
768 B is the fix's own code. The consumption from `pre_probe` to the
post-load census is 13,711,536 B in both, so the mallocs are identical.

A 16 MiB cart on the Go-modelled heap (Emerald, `rom_ballast_kb = 5120`):
the 3.0.0 candidate pages it, 15 blocks of 16, and leaves 17.1 MiB of heap
idle. The fix holds it whole (16 blocks, 2 lent), with 18.2 MiB left. On the
1000 layout it is 13 blocks, `heap_short`, unchanged.

**Emulation is unchanged.** The per-frame guest-state oracle (`shash`) ran on
Unbound with the resrig fixture (`unbound_double_high` state, `battle.inputs`,
4,000 frames, 0 `evt_gap`), fix build, PPSSPP 64 MiB:

| arm | blocks (spare) | plan | vs PAGED, frames 31-3931 |
|---|---:|---|---|
| PAGED (`rom_cap = 15`) | 15 (2) | default | - |
| RES | 32 (2) | resident | 3,901/3,901 identical |
| RES, Go heap (`rom_ballast_kb = 5120`) | 32 (2) | resident | 3,901/3,901 identical |
| 3.0.0 candidate RES (old probe) | 32 (0) | resident | 3,901/3,901 identical |

Frames 14-30 come before the state loads at frame 31. There the IWRAM hash
differs between any two resident runs, even two with the same layout (RES and
RES-Go-heap). It is boot-window timing: the documented RTC-seconds effect of
a longer ROM read. It is not layout.

### Merge instructions (for whoever merges; not merged here)

- Keep jit-runtime-size's `dynarec_select_translation_caches()` call before
  `init_gamepak_buffer()` in retro_init. That order IS the budget.
- Keep `JIT_LARGE_HEADROOM = (ROM_BUFFER_SIZE + 1 + 4) MiB` with
  `ROM_BUFFER_SIZE = 15`. **Never set ROM_BUFFER_SIZE=32.** Residency is the
  runtime wish, and ROM_BUFFER_SIZE=32 would make the JIT probe ask for
  47.5 MiB, which is about the whole heap.
- Conflicts:
  - root `Makefile`: keep `ROM_BUFFER_SIZE ?= 15` plus
    `CFLAGS += -DROM_BUFFER_SIZE=$(ROM_BUFFER_SIZE)`, then jit-runtime-size's
    block;
  - `psp/Makefile`: keep this branch's comment;
  - `psp/main_psp.c`: take both sides. jit-runtime-size's `heap_budget` hunk
    carries the `jit_cache` line.
  - The scratch merge resolved this way built and produced the table above.

### What changes for a player

`rom_resident = 1` is the default since 3.0.0; `rom_resident = 0` pages. A
1000 is unchanged. A 64 MiB console holds a cart whole when the heap it was
actually given allows it, with at least the 1 MiB post-load floor: on a PSP
Go running the large JIT tier that is about 0.5 MiB to spare for Unbound, and
plenty for a 16 MiB cart (which the 3.0.0 candidate also paged on the Go,
15 blocks of 16).

## A release build is unchanged without `rom_resident`

- **Switches.** All the new harness keys are read only under `GPSP_PERF_RIG`:
  `rom_cap`, `swap_stubs`, `cache_paranoid`, `rom_ballast_kb`,
  `jit_coherency_scan`, `shash`, `heartbeat_s`.
  - The core variables they set (`gamepak_force_swap_stubs`,
    `platform_cache_paranoid`) exist in a release but are never written, so
    they stay 0.
  - The release EBOOT.PBP contains none of the key strings except
    `rom_resident`.
- **Frames: identical.** The comparison runs on the **release-profile core
  archive** (`GPSP_PROFILE=release`, the exact `CORE_COMMON` flags), linked
  under the harness frontend so a fixture can drive it (a release frontend
  reads no harness ini). fd672be vs this branch at 8208889, `unbound_double_high`,
  9,300 frames, `io_prio = 42`, 0 `evt_gap`:

  | layout | ROM blocks (both) | vhash lines | distinct frames | frame dumps | audio |
  |---|---:|---|---:|---|---|
  | 64 MiB | 15 | 9,300/9,300 identical | 3,495 | 31/31 identical | `fa42ef1f` both |
  | 32 MiB | 13 | 9,300/9,300 identical | 3,495 | 31/31 identical | `fa42ef1f` both |

- **Static.** Release ELF sizes, text / data / bss:

  | build | text | data | bss |
  |---|---:|---:|---:|
  | fd672be | 1,672,456 | 160,880 | 5,342,524 |
  | this branch (8208889) | 1,676,144 | 160,880 | 5,342,716 |

  - The +3.7 KB of text is the runtime cap, the cache-sync branch, the unused
    coherency scanner, and handoff_set_extra.
  - bss is +192 bytes. The ported ini-audit table (13 KB) is compiled out of
    player builds for exactly this reason.
  - A 3.7 KB smaller heap costs a PSP-1000 a ROM block only if its post-load
    remainder was within 3.7 KB of a MiB boundary. The chance is about 0.4%,
    and it is not measurable without telemetry.
  - Symbol-level diff: only the functions above, plus inlining changes in
    `block_lookup_*` and `translate_icache_sync` that follow the larger
    `platform_cache_sync`.

Evidence: `builds/rom-resident-evidence-2026-09-26/`.
