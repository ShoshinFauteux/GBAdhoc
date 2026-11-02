# Memory budget per console

Branch `claude/go-memory-budget` (off `claude/gbalink-session`), 2026-10-02.

## The rule

**Every boot-time reservation must fit the PSP Go, and the PSP-1000.**

- A boot-time reservation is anything the binary holds before a game runs: the static image (text, data and every `.bss` array, the second GBA core included), the netdrv arena, the translation-cache tier, the ROM page cache.
- "Fits" means the console still has the boot floor (below) after it.
- The PSP Go is the smallest 64 MiB console. The PSP-1000 is the smallest console of all, because MEMSIZE does nothing on a 1000.
- PPSSPP is not the reference. Its 64 MiB model gives a process 5 MiB more than a real Go (`ROM-RESIDENCY.md`, 2026-09-27).

The rule is enforced in two places:

| where | what | on failure |
|---|---|---|
| `tools/build.sh` (every profile) | image size against this page's per-console numbers | the build fails |
| `psp/main_psp.c` (link builds; `GPSP_MEM_BUDGET` elsewhere) | the GBA core's minimum, measured just before `retro_init` | "NOT ENOUGH MEMORY" screen, `EVT exit code=8 reason=mem_budget`, back to the XMB |

## The model: one number per console

The heap is newlib's single partition block. It is taken before `main`, and it is all but 1 MiB of what the image leaves. Every GBAdhoc build, and `tools/memprobe`, uses the same `PSP_HEAP_SIZE_KB(-1024)`, the same 256 KiB main stack, and MEMSIZE=1 (playable and harness64). So:

    heap at boot = heap_plus_image(console) - image(build)

- `image` is `__executable_start .. _end`, the PT_LOAD span.
- `heap_plus_image` is a property of the console, not of the binary.
- **Checked in PPSSPP, three ways:**

| build | image | heap at boot (`EVT mem_boot`) | sum |
|---|---:|---:|---:|
| memprobe | 250,408 | 57,400,576 | **57,650,984** |
| link harness64 (dev2) | 13,199,232 | 44,451,840 | 57,651,072 |
| link harness64 (dev3) | 13,200,128 | 44,451,072 | 57,651,200 |

The sums agree to within 216 B (alignment).

### Per-console numbers

The cells marked *pending* are filled from the owner's `memprobe.txt` (OWNER-STEPS in `builds/memprobe-out`).

| console | heap_plus_image | source | release heap at boot (image 7,616,768) | link build heap at boot (image 13,200,128) |
|---|---:|---|---:|---:|
| PPSSPP, 64 MiB model | 57,650,984 | memprobe | 50,034,216 | 44,450,856 |
| PPSSPP, PSP-1000 model | 24,096,552 | memprobe (`PSPModel = 0`) | 16,479,784 | 10,896,424 (measured 10,896,640) |
| **PSP Go** (N1000) | **52,408,104** (derived) | PPSSPP minus 5,242,880 B (resrig, 2026-09-27); memprobe *pending* | 44,791,336 | 39,207,976 |
| PSP-3000 | *pending* | memprobe | | |
| PSP-1000 | *pending* | memprobe | | |

`tools/build.sh` takes the Go and 1000 numbers as `BUDGET_HPI_GO` / `BUDGET_HPI_1000`. When a probe result differs, update both this table and those defaults.

### The boot floor

Measured on the link build in PPSSPP: `boot_ballast_kb` holds N KiB of heap from the first statement of `main`. The AW2 wake arm (state load, 150 frames).

| free at boot | JIT tier | ROM blocks (8 MiB cart) | result |
|---:|---|---:|---|
| 44,449,672 (PPSSPP) | large | 15 (resident) | exit 0 |
| 39,206,776 (**the Go**) | large | 15 (resident) | exit 0; also with `gba_dual = 1` |
| 23,478,136 | small | 15 | exit 0 |
| 12,992,376 | small | 10 | exit 0 |
| 6,700,920 | small | 4 (paging) | exit 0 |
| 4,603,768 | small | 2 | exit 0 |
| 3,555,192 | small | 1 | exit 0 |
| 3,030,904 | - | **0** | **hang**: NULL ROM page, `translate_block_arm` reads address 0 forever |
| 933,752 | - | - | hang (netdrv arena also fails) |

- **Why zero blocks hangs.** `init_gamepak_buffer` holds a 1 MiB post-load reserve and then takes 1 MiB blocks until malloc fails. With no block it does not fail: `gamepak_buffers[0]` stays NULL.
- **The gate.** `MEM_GBA_CORE_MIN` = 1 MiB reserve + 1 block + 64 KiB, asked as three mallocs just before `rom_cache_select`. With the same 40,448 KiB ballast, dev3 logs `mem_budget FAIL need=2162688 largest=1974272`, shows the notice, and exits 8. With 39,936 KiB it passes and runs (one block).
- **Build floor** (`BUDGET_BOOT_MIN`): 4 MiB of heap at boot = netdrv's 1 MiB + the core minimum + margin over the measured 3.1-3.6 MB.

### Where the link build stands

- Its second core adds 5.4 MiB of image (13.20 vs 7.62 MiB).
- On the Go it boots with **39.2 MB** of heap: the large JIT tier and a resident 8 MiB cart.
- On a PSP-1000 it boots with **10.9 MB**: the small tier, and 8 blocks after the gamepak loop, `post_load` 1.37 MB free; `gba_dual = 1` runs.
- **A cable session on a PSP-1000 needs a boot reserve** (claude/gbalink-hw, GBA-LINK-MIRROR.md 15.3). Without one, AW2's 8 MiB cart went resident and left 1.25 MB, and the join's `fe_gblink_create` failed: the 1000 could not join at all. A session's peak is 2,284,912 B (AW2, `gblink_pre` -> `gblink_end`), so 2,560 KiB is now held from before the boot gate to the session's start (`cable_reserve_kb`). The 1000 model (PPSSPP, 32 MiB harness, image 13,203,968 B): heap at boot 10,892,800; after the reserve, the JIT's small tier and 5 ROM blocks (the cart pages) `post_load` is 1,778,936; the reserve handed back gives 4,400,296 (largest 2,621,440); `gblink_running` 2,673,352; lowest, `gblink_end`, 2,116,264.
- Both are far above the 4 MiB floor. **The link build does not exceed the Go's budget.**
- **The whole session on a Go-sized join.** This is the two-PPSSPP AW2 battle (section 14.2's r6 recipe, both consoles on Xvfb :131), using the rebuilt rig EBOOT `2539dedd` with `boot_ballast_kb = 5120` on the join:
  - **PASS**, with FINAL `a2eb2b47a9241ad3` at frame 14625. That is the same hash as r2/r3/r6.
  - The per-frame files are identical (29,249 lines), and 243/243 HASH messages matched on each side.

## The PSP-1000's ROM cache (4.6.0, branch `claude/mem-1000`)

The 1000 runs out of heap, so what the image and the boot take comes off the ROM cache. M = measured, D = derived from M.

| | 4.5.0 | now |
|---|---:|---:|
| release image | 14,367,116 | 13,891,980 |
| Lab build, solo (PPSSPP PSPModel 0) | 329 pages | 371 pages (M) |
| release, solo | 361 pages, 11.28 MiB | ~387 pages, 12.1 MiB (D) |
| release, late-mode session | 160 pages, 5 MiB | ~193 pages, 6.0 MiB (D) |

Multi-tail: `init_gamepak_buffer` no longer loses what is short of a MiB. Blocks hold their own page count: whole blocks, the lent pool's tail, then pieces of what the heap has left, taken while the post-load reserve is held. So every 32 KiB saved is a page and there are no thresholds. A reload holds no reserve: it keeps `GAMEPAK_RELOAD_FLOOR`, 256 KiB, and never exceeds the boot's pages. A console that reaches its target never runs the new code.

Bytes won (image, or heap for the arena):

| change | bytes | budget |
|---|---:|---|
| net arena to its exact size (895,309) | 153,283 | both |
| `backup_save` copies in 4 KiB pieces | 131,072 | both |
| RAM staging buffer on demand (VRAM is the default) | 82,432 | both |
| rewind picture copy on demand (the PSP draws directly) | 76,800 | both |
| test pattern on the heap | 76,800 | both |
| link bus queues in B's lent block | 65,552 | solo |
| `-Os` menus, pad map, config, Mystery Gift, cheats | ~33,000 | both |
| link script repeat lines, BIOS hash buffer | 32,768 | both |
| playable autopilot table (6,144 stays as a layout pad) | 32,592 | both |
| browser script table | 6,876 | both |

Rejected or parked:
- **`--gc-sections`** (~108 KiB): drops the import stubs ("no nid section found"). It needs a link script with KEEP for `.lib.stub`, `.rodata.sceNid` and friends.
- `state_buf` (416 KiB), the post-load reserve below 1 MiB, the browser list on the heap: each needs a low-water fixture first. Rewind's heap ring lives in that reserve.

### Lending the volatile pool (off)

Harness key `rom_vmem = 1`, Rewind off: the 3 MiB above the ME's megabyte joins the cache as its last blocks (Lab build 467 pages). A release cannot turn it on: it reads no harness ini.

- The pages never count as resident (`gamepak_must_swap`), so the paged load stubs stay.
- Into a sleep: `gamepak_vol_drop` unmaps them and the LRU skips them; after the wake they fault back from the card.
- A pool that does not come back stays out of the cache.
- A sleep that tore nothing down re-reads them (`g_rw_pool_suspect`).

PPSSPP (Unbound rival, 3,100 frames, vhash): identical to 4.5.0 with the lend, across a simulated sleep, and across a sleep with no teardown and the pool overwritten. The mutant `rewind_pool_restart = 0` differs from frame 1165, so the test can fail.

Evidence it needs before it can be on, none of it available in PPSSPP:
1. The sleep suite on a real 1000 with `rom_vmem = 1`: repeated sleeps in solo play, with a paged cart, and at the wake overlay. Sleep was never validated on 4.5.0, so run the same suite without the key as the control.
2. `sceIoRead` into the pool on hardware: the first boot answers it.
3. HOME and the utility dialogs open with the pool lent: does anything write it?
4. Reads after a wake: how long the faulted-back pages stall play.
5. A link session with the lend, including a sleep during one. PPSSPP has not run that.
6. Rewind on forfeits the lend, so the player trade needs the owner's decision.

## The 2026-10-02 Go black screen was not memory

**The report.** The session EBOOT `af98e599`, arm W of the pair rig: black screen at launch on the Go, with an empty `log/`. The same EBOOT ran on the 3000 and in PPSSPP.

**The cause: the Go's card held a different file.**
- md5 `abd13048`, the same size (3,321,559 B).
- Read back from E: (read-only) and compared cluster by cluster (32 KiB clusters, FAT32), it is arm W's file with:
  - **cluster 89 missing.** File clusters 89-99 are W's clusters 90-100, and 100 is W's last cluster (11,991 B).
  - **cluster 101** is cluster 52 of the `shash.txt` that the pair rig's takeover had collected and deleted from `log/` in the same window (it matches `logs/pair/pre-join-1790946729/log-shash.txt` at offset 1,703,936).
- That is a FAT chain rewritten under the file. It matches the documented Go M2 mechanism (memory: go-m2-handoff-fat-damage):
  - the parked app's firmware writes back a FAT cached from before the USB window, after the PC has freed and reallocated those clusters;
  - here the PC freed `shash.txt` and reused its clusters for the new EBOOT.
- The install was md5-verified at 09:12:17 and ejected at 09:12:18. Then the parked app (run 152) served the rest of its 300 s window, released USB, appended STATE.TXT ("usb released", "CMD=RUN -- relaunching"), and relaunched.

**What the damage hits.**
- In the ELF it starts at file offset 0x2C42CD: the tail of `.rodata`, `.ctors`, `.dtors`, all of `.data`, and the section headers.
- `.ctors` holds `0x08aa868c 0x08aabb5c` (rodata addresses) instead of `ffffffff 00000000`. `.data` begins with zeros.
- crt0 therefore jumps into data before `main`. That is why no log file was ever created.

**Reproduced in PPSSPP.** Xvfb :131, the same harness ini as the wake arm:
- The Go's file: PPSSPP's loader rejects every section name ("Bad name offset", "Bad section"). It "boots", but `log/` stays empty and it never exits: the Go's symptom.
- Arm W's file (`af98e599`): `EVT exit code=0` at frame 150.
- Both were run before and after ~20 other launches (control for the rig wear-out trap).

**What it means for the rigs.**
- The one-window rule ("repair, stage and CMD=RUN in ONE window, so the app relaunches on a fresh FAT") does not protect a staged EBOOT. The parked app still runs, and writes, between the window and the relaunch.
- An install must be verified **as the console reads it** before CMD=RUN:
  - a second window, after the app has released and re-exported the card;
  - or `memprobe`, which md5s every `GBADHOC*` EBOOT through the PSP's own file system.

## tools/memprobe

A 136 KB EBOOT, `GBAdhoc MEMPROBE`, shaped like GBAdhoc (same heap, stack and MEMSIZE). It writes `memprobe.txt` beside itself and returns to the XMB (after 10 s, or on X). It records:

1. Model (`kuKernelGetModel`), firmware (`sceKernelDevkitVersion`), CFW (`sctrlHENGetVersion`).
2. Memory outside the heap at entry: kernel free/max, and a bisected `sceKernelAllocPartitionMemory` for partition ids 1-12. These are user-mode calls; ids user code may not use are listed as refused.
3. The heap:
   - the block size (arena plus the un-sbrk'd tail);
   - the largest single malloc;
   - gpSP's greedy 1 MiB loop;
   - **`heap_plus_image`**.
4. md5 and size of every `PSP/GAME/GBADHOC*/EBOOT.PBP` and `gbadhoc_me.prx` on its own device.
5. The kernel's partition table. This is `SysMemForKernel` QueryMemoryPartitionInfo / Partition{Total,Max}FreeMemSize through ARK's `sctrlHENFindFunction` + `kuKernelCall`, trying the 6.60 NIDs first.
   - It runs last, after the file is saved once, because it is the only part that could misbehave on an unexpected CFW.
   - It reads "unavailable" in PPSSPP.

**PPSSPP results** (`PSPModel = 1`):
- heap 57,400,576; largest malloc 57,396,224; 54 greedy blocks; partition 2 has 524,288 outside the heap; partition 5 (volatile) 4 MiB.
- With `PSPModel = 0`: heap 23,846,144, 22 blocks.
- The md5 scan reported `abd13048` for the Go card's copy and `af98e599` for arm W.

Build:

    docker run --rm -v "$PWD":/build -w /build/tools/memprobe pspdev/pspdev make

## Harness keys added

- **`boot_ballast_kb = N`** (`GPSP_PERF_RIG` builds). It holds N KiB of heap from the first statement of `main`, before the netdrv arena, the renderer, the JIT tier and the ROM.
  - `5120` is the Go in PPSSPP.
  - `rom_ballast_kb` is later: it is taken just before the JIT decision.
- **`EVT mem_boot`** (telemetry builds), right after `boot_ok`. Fields: `image k_free k_max heap free largest ballast_kb`.

## Release builds are unchanged

The code is compiled only under `GPSP_PERF_RIG`, telemetry, or `GBA_LINK || GPSP_MEM_BUDGET`. The proof:
1. The frontend patch and the `build.sh` patch were applied onto `claude/candidate-3.1-all` (97a9dad).
2. Its release profile built to EBOOT md5 **`f239eb8e`**, identical to the unpatched build of the same commit. The ME PRX `19ae57b6` was identical too.
3. The patched `build.sh` printed `image 7616768 B; heap at boot PSP-1000 16479784 B, PSP Go 44791336 B`.
