# Upstream report draft — full RAM translation-cache flushes dominate PSP frame time

Status: **draft, not yet sent.** Intended for `libretro/gpsp` / davidgfnet. Written
2026-08-02 from measurements taken on real PSP hardware and in PPSSPP. Everything here is
measured; nothing is inferred.

> **Status update, 2026-08-02 — read this first.** Everything below is still accurate as
> measurement, but the *conclusion* has changed and this draft is no longer a request for a fix.
> We identified the offending code (the game builds a two-instruction function on its stack) and
> then timed the flush directly: it costs **~1 us**, not the ~70 us we had inferred, and it
> discards a RAM cache holding **1.4 blocks**. Every flush is a true positive and the current
> full-cache behaviour is fine. The two sections marked *"What that halfword actually is"* and
> *"...and the flush turns out to be cheap"* carry the correction; the profiling patches are
> still offered, the invalidation request is withdrawn.

## Summary

On PSP, Pokémon Emerald triggers **~410 full RAM translation-cache flushes per second**,
every one of them caused by self-modifying-code detection (never by cache exhaustion).
Each flush discards *all* translated RAM code, so the game's IWRAM/EWRAM code is
continuously recompiled. Bursts reach **307 flushes in a single frame**, which on Allegrex
costs ~21 ms — against a 16.75 ms frame budget.

For ordinary single-player use this is invisible: gpSP still averages ~58.6/59.73 fps and
the game feels fine. It becomes fatal only when something depends on frame timing being
*consistent* — in our case two PSPs emulating the GBA Wireless Adapter to each other, where
one console stalling 21 ms while the other doesn't is enough to break the games' link
protocol.

## Measurements

Hardware: PSP-1000 and PSP-3000, ARK-4 CFW, 333 MHz, `platform=psp1` build
(`SMALL_TRANSLATION_CACHE`), real GBA BIOS, Pokémon Emerald (BPEE rev 0).

Per-frame emulation cost, measured around `retro_run` only, **with no netplay/serial
activity at all** (single-player, before any wireless session exists):

```
core mean 11.4 ms   core max 21.3 ms      (frame budget 16.75 ms)
```

Flush counters, split by cause (patch below), over a 600-frame window:

```
rom=0   ram_full=0   ram_smc=4109   ram_dma=1   gamepak_page_loads=0
worst single frame: 307 SMC flushes
```

- **`ram_full=0` in every window of every run** — the 384 KiB RAM translation cache never
  fills. Enlarging it cannot help.
- **`gamepak_page_loads=0`** — ROM paging is not involved.
- **`ram_dma=1` of 4110** — essentially all SMC alerts come from CPU stores, not DMA.

Derived cost: worst frame 8456 µs vs 5325 µs mean over 307 flushes ⇒ ~10.2 µs/flush in
PPSSPP; scaling to Allegrex gives ~70 µs/flush ⇒ ~21 ms for 307, matching the hardware
`core` max of 21,263 µs almost exactly.

## Mechanism

`flush_translation_cache_ram()` (`cpu_threaded.c:3342`) resets `ram_translation_ptr` to the
start of the cache and clears the SMC tag range, i.e. **every translated RAM block is
discarded** in response to a write to **one** halfword.

Detection itself is precise in the sense that the write paths test the per-halfword tag
before raising `CPU_ALERT_SMC` (`gba_memory.c:1833`, `:1856` for DMA; the CPU store path via
`write_io_epilogue`, `mips/mips_stub.S:329`) — every alert really is a write to a halfword
carrying a code tag. The disproportion is in the *response*, not the detection.

## Where the writes actually land

We added a second measurement patch that buckets the written address (256-byte pages, plus an
8-slot exact-address heavy-hitter table) and aggregates per 600-frame window, one bucket hit
per flush. The result is sharper than we expected:

```
host, storm window:  flushes=4110  distinct pages=2  iwram=4110  ewram=0
                     hottest exact address: 0x03007D90 = 4095   (no table evictions)
join, storm window:  flushes=4122  same address, 4096
Union Room entry:    flushes=1     the game's code DMA into 0x030046xx-0x03004Fxx
boot, solo:          flushes=70    one page, 0x03007D3C-0x03007D58
```

- **EWRAM is never involved** — `ewram=0` in every window of every run, on both consoles.
- **~99.6% of the steady-state storm is a single halfword, `0x03007D90`.** The exact-address
  table never had to evict, so 4095 of 4110 is a complete count, not an estimate.
- **A whole code DMA costs one flush.** The overlay copy this title does at link-session entry
  raises `CPU_ALERT_SMC` on every tagged halfword it crosses but flushes once, which is the
  correct and cheap behaviour. Genuine overlay swapping is not what is hurting.

## What that halfword actually is — and why we are no longer asking for a fix

We then recorded, for every RAM block translation, whether the range `scan_block` tags
(`[block_start_pc, block_end_pc)`) covers the hot address, together with the block's start PC,
its mode, why the scan stopped, the GBA PC of the storing instruction, and a byte copy of the
block. That settles it completely:

```
blk = 03007d90:03007d94:t:reason=1:4096      wr = 082e1a9c@03007d90:4107
```

- **The block starts AT `0x03007D90` and is four bytes long** — two Thumb instructions,
  `7800 4770` = `ldrb r0,[r0]` ; `bx lr`. `reason=1` means `scan_block` ended it on an
  unconditional branch. Every covering block in every window ended that way; none hit
  `MAX_BLOCK_SIZE` or the `0x3007FF0` clamp. **gpSP's block termination and tagging are both
  correct here** — the halfword is a genuine translated instruction, in fact the block's first.
- **It is the game's flash driver.** Those four bytes are `ReadFlash1` from the Pokémon gen-3
  flash code (`agb_flash.c`), and the writer at `0x082E1A9C` is `SetReadFlash1+0x2c`, the
  `while (i != 0) { *dest++ = *src++; i--; }` loop that copies `ReadFlash1` **onto the stack**
  and points the `PollFlashStatus` function pointer at it — so the driver never executes from
  the cartridge while the flash chip is in ID/poll mode. `0x03007D90` is stack (BIOS default
  `SP_usr` is `0x03007F00`). At boot the same mechanism appears one level up: `ReadFlash+0x64`
  copying the 0x22-byte `ReadFlash_Core` into a stack buffer at `0x03007D48`.
- So **every one of these flushes is a true positive.** The title really does write over
  translated code, ~410 times a second, because it really does build and run code on its stack.

## ...and the flush turns out to be cheap, so we withdraw the ask

We then timed `flush_translation_cache_ram()` directly rather than inferring its cost:

```
600-frame storm window:  4110 flushes   fus = 4229-5448 us total   =>  ~1.0-1.3 us per flush
                         tag memset = 267 bytes/flush (whole-region fallback never taken)
                         RAM blocks re-translated = 1.39 per flush
```

That is **0.13 % of the window's core time**, and the storm window's mean frame cost is *lower*
than a quiet window's. The reason is visible in the third number: this title keeps almost no code
in RAM (`SoundMainRAM_Buffer`, `IntrMain_Buffer`, and these stack thunks), so "discard the entire
RAM cache" discards about one and a half small blocks. Our earlier ~10 us/flush figure came from
dividing a whole worst-frame excess by the flush count — it assumed the answer. **The full-cache
flush on SMC is fine, and finer-grained invalidation is not worth building.** We are recording
that here so nobody repeats the work.

For completeness, if anyone ever does want the storm gone, the cheap lever is not invalidation
but **silent-store elision**: we measured that **4095 of ~4109 events per window (99.7 %) write
bytes that were already there** — the driver copies the same two halfwords from the same ROM
address to the same stack address every time, so the translation stays valid. Only ~14 events a
window are real code changes. The obstacle is that the store sits in the SMC branch's delay slot
(`mips_emit.h`, `emit_pmemst_stub`), so the old value is gone by the time `smc_write` runs; a
compare would have to live in the emitted stub. We have not attempted it, because on our
measurements it would buy 0.13 %.

One incidental observation while we were in there: `arm_block_memory` / `thumb_block_memory` with
`rn == REG_SP` emit a direct store with **no SMC check at all** (*"Assume IWRAM, the most common
path by far"*). A `push` over a stack-resident thunk therefore silently corrupts translated code
and raises nothing. It is benign for this title only because the game rewrites the thunk before
every call — but with games putting code on the stack, it is worth knowing the hole is there.

Pokémon gen-3 executes substantial code from IWRAM (including its RFU/wireless driver) and
writes near it constantly, which is why this title is such a heavy trigger.

## Why the obvious mitigations don't apply

Tested and ruled out by the counters above:

1. **Larger `RAM_TRANSLATION_CACHE_SIZE`** — the cache never fills (`ram_full=0`).
2. **Deferring the flush to an idle point** (e.g. on `CPU_ALERT_HALT` / SWI 0x02, which
   Pokémon hits constantly) — an SMC invalidation cannot be postponed without executing
   stale translated code in the interim.
3. **Freeing main RAM to spend on the cache** — nothing measured wants the memory.

4. **Selective invalidation** — discard only blocks covering the written address. We no longer
   propose this. Two obstacles, and a third reason not to bother: the `u16` per-halfword tag
   cannot record two overlapping blocks, so it needs an address→blocks index; **and translated
   blocks are directly linked** — every external block exit is patched with the raw code address
   returned by `block_lookup_translate_##type(branch_target)`, so neutering a dead block's tag
   does not stop an already-patched branch jumping into its stale code, and the linking would
   have to be reworked as well. And per the timings above it would save ~1 us on a path costing
   0.13 % of core time.

## Patch we are carrying (offered)

Counters only, no behaviour change — three `u32`s and increments at the existing call
sites, splitting the flush cause so the above can be measured:

- `flush_ram_full` at the two exhaustion sites (`cpu_threaded.c:3106`, `:3268`)
- `flush_ram_smc` / `flush_ram_dma` at the two alert entry points
- `flush_rom_total`, `gamepak_page_loads` for completeness

...and the address profile above: a `u16[1152]` of 256-byte page buckets, an 8-slot
heavy-hitter table, and two arguments passed to `smc_write` on MIPS (`reg_rv` and `reg_a0`
are both live at the SMC branch and dead afterwards, so it costs two `move`s on a path that
is already about to flush the whole cache). Measured overhead on a 333 MHz Allegrex: below
the resolution of a deterministic 3600-frame A/B; bounded analytically at ~0.2 µs per SMC
event, under 0.3% of the flush it accompanies.

Happy to submit as a PR if useful. We also carry two unrelated small patches from this
project (an RFU packet-queue depth increase, and an RFU activation hook for a frontend) —
see `docs/DECISIONS.md` ADR-0011 and ADR-0029.

## What we would find most useful

Confirmation of whether the full-cache flush on SMC is considered acceptable behaviour, or
whether a finer-grained invalidation has been considered before. If the latter is welcome
in principle, we are willing to prototype it against our measurement harness — we can
reproduce the flush storm on demand and count it precisely.
