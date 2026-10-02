# The resident-ROM crash: stale instruction-cache lines (2026-09-30 / 10-01)

## Symptom

With the whole cart resident (`rom_resident = 1`, 64 MiB consoles), Heart & Soul
derails on the first frame after a state load. Translated code jumps to an
unmappable guest PC. BADJUMP_SAFE catches it and soft-resets the GBA, so the run
ends spinning at BIOS `0x186C` with EWRAM frozen.

- **PSP Go:** every time (about 40 runs).
- **PSP-3000:** some of the time.
- **PPSSPP:** never, because it models no caches.

It has the same shape as the crashes that once made residency unusable on the Go
and the 3000. It is layout-sensitive. A rebuild that only adds about 3.7 KiB of
diagnostic code hides it, while moving the heap, the ROM blocks or the JIT block
does not.

## Ruled out, on the failing binary

| Hypothesis | Test | Result |
|---|---|---|
| Media Engine race | `me_mode = 0` | still dies, on the Go and the 3000 |
| CPU slowness hides it | ME off runs at 17 ms/frame | still dies |
| Stray write into ROM | `rom_canary`: FNV of all 8192 resident pages, cached and uncached views | 0 pages ever change |
| Lent small-JIT array | `jit_small = 1` | inconclusive on the 3000 (its base is a coin flip there) |
| Residency probe | forced `rom_cap = 32` | dies |
| Partial cache lines | site B's range rounded out to whole 64-byte lines | dies |

## The bisection

Every `platform_cache_sync` copy in the original EBOOT is inlined, and each one
is gated by `bnez <cache_paranoid>`. Making one gate unconditional turns that
single site into a whole D+I sync, a one-instruction patch that moves nothing.
The patched EBOOTs, the analysis and the scripts are in `builds/hns-patch/`:
`PATCHES.json`, `base.dis` and `helper.S`.

Go results:

| Patch | Go result |
|---|---|
| none (control) | dead, every run |
| all nine sites | alive |
| **B: `translate_icache_sync`, ROM range (out-of-line copy: Thumb/dual lookups, `init_bios_hooks`, `badjump_recover`)** | **alive** |
| A, C, D, E (same ROM sync inlined in the ARM lookup), F, plus E with only the I-half or only the D-half whole | dead |
| B with **ranged D + whole I** | **alive** |
| B with ranged I + whole D | dead |

## Fix (as shipped: d7df3bb, the NARROW form)

The first version of the fix (8b48b48) put the whole I-cache invalidate in
`translate_icache_sync()` and left every caller on it, so the ARM lookup's
normal success path paid it too. **What candidate 3.1 ships is narrower
(d7df3bb):** the whole invalidate stays in `translate_icache_sync()`, and the
ARM lookup's success path is moved to a ranged copy,
`translate_icache_sync_ranged()`.

| path | sync on a new ROM translation, in source |
|---|---|
| `translate_icache_sync()`: the Thumb lookup (also the dual lookup's Thumb branch) and `badjump_recover` | ranged D writeback (`sceKernelDcacheWritebackRange` over the new bytes) + **whole I invalidate** (`sceKernelIcacheInvalidateAll`) |
| `block_lookup_address_arm`'s success path, and so everything that reaches it: the dual lookup's ARM branch and `init_bios_hooks` | the old ranged D+I sync, via `translate_icache_sync_ranged()` |
| RAM-zone publishes (both) | unchanged: ranged D+I |

Why the ARM path stays ranged: in the bisection, making site E (the ARM
lookup's inlined copy) whole -- D+I, I only or D only -- never cured the
derail, while site B alone did.

**How close this is to the tested patch.** Patch B changed the out-of-line
copy in the FAILING binary, so it covered whatever called that copy there.
In a symbol-carrying rebuild of the same source (`builds/hns-patch/canary.elf`)
those callers are: `block_lookup_address_arm` (one call, not the success
path, whose copy is the inlined site E), `init_bios_hooks` (its inlined ARM
lookup), `block_lookup_address_thumb` and `block_lookup_address_dual`. The
shipped source keeps the whole invalidate for the Thumb lookup and
`badjump_recover`, but `init_bios_hooks` and the dual lookup's ARM branch now
call the ARM lookup and get the ranged sync. Disassembly of the 3.1 RC1 and
RC-ALL release ELFs (2026-10-01) confirms it: the unconditional
`sceKernelIcacheInvalidateAll` appears in `translate_icache_sync`, the Thumb
lookup, the dual lookup (its inlined Thumb branch) and `badjump_recover`;
`init_bios_hooks` and the ARM lookup's success path have only the
`cache_paranoid`-gated one (RC1's ARM lookup does hold one unconditional
whole invalidate: `badjump_recover`, inlined on its failure path). `init_bios_hooks` runs only when the emitter is
built (boot, ROM load), not on a state load (`gba_load_state` calls
`flush_dynarec_caches`), so the difference does not touch the state-load
path the reproducer exercises -- but it is a difference, not an identity.

When new ROM translations are published through `translate_icache_sync()`,
the data cache writes back their range as before, and the whole instruction
cache is invalidated. The stale lines lie outside the range just written,
which is why the ranged invalidate cannot reach them. That is 256 line
operations per such publish; ROM publishes are rare once a game is warm. The
tested patch measured 9.77 ms/frame against about 9.7 for paged runs.

The exact code that leaves those lines stale is not yet pinned down. The fix is
verified on the failing binary (the one-instruction patch at B), and the
mechanism class is demonstrated: instruction-side only, outside the published
range, and cured at the out-of-line ROM-publish point. Finding the precise
writer is a follow-up, not a blocker.

**Verified vs not.** The patched-binary results above are hardware (PSP Go).
A source build of d7df3bb moves code, and this derail is layout-sensitive, so
the built binary is a different layout from the one bisected, and (above) its
whole-I coverage is slightly narrower than patch B's. As of 2026-10-01 the
rig logs in `builds/candidate-out/` hold the narrow build (`CBONLY`) only on
the AW2 bench on the Go, not on the Heart & Soul resident-state-load
reproducer; that run is still owed.

## Cost: the "+18% on AW2" was layout, not the invalidate

The first candidate (8b48b48, whole I on every ROM publish) measured about
+18% frame time on the AW2 tour on the Go, which is what motivated the narrow
form. The JIT code discipline work (`claude/jit-code-discipline`,
`docs/JIT-CODE-DISCIPLINE.md` section 6.5) priced it and found the
invalidate cannot explain it:

- In steady state AW2 does about **0.21 ROM publishes per frame**. At most
  256 lines x 215 ns (the measured I-cache miss, icprobe) ~= 55 us of refill
  per whole invalidate, that is **at most ~12 us/frame** -- not ~1.3 ms.
- H&S heavy does four times as many whole invalidates per frame (0.85) and
  measured only +0.33 ms (PUB 9.69 -> CAND 10.02 ms, Go).
- Go AW2, `builds/candidate-out/logs-go-iso*`: no fix 6.98, narrow fix
  (CBONLY) 7.16, whole-on-every-publish (CAND) 8.27 ms/frame -- three
  binaries whose only difference is where one sync site's code sits.

So most of the +18% is a **code-layout effect** (hot code landing in the same
I-cache sets), the same sensitivity that makes the derail come and go. Two
different binaries on hardware price layout as much as policy; a fair policy
comparison runs ONE binary at several paddings (the JIT discipline's
`JIT_CODE_AB` build, section 8 of that doc). The narrow fix is kept because
it is the exact tested patch, not because the whole-I form was shown to be
expensive.
