# The hard freeze: root cause, fix, and what is still unproven

Branch `opus/performance-stability-fixes`, from `7283f13` (the accepted
candidate). Worktree
`builds/opus-performance-stability-worktree`; artifacts
`builds/opus-stability-candidate/`. The preserved candidate at
`builds/unbound-stability-release-candidate-7283f13` was not touched.

---

## CORRECTION AND OUTCOME — 2026-09-18

**Read this before the rest of the document.** What follows below is the original
write-up and is preserved deliberately, including the parts hardware later
disproved. It is not edited in place, because a record that silently becomes
correct teaches nothing about how it was wrong.

Current state of each claim:

| claim below | status after hardware A/B |
| --- | --- |
| the sentinel `(u8*)(~0)` patched as `j 0x3FFFFFF` reaches unmapped memory with no dispatcher in the path | **stands.** This is the demonstrated mechanism and the reason the containment exists |
| per-exit dispatch islands are a safe way to contain it | **DISPROVEN.** The islands *were* the measured performance regression |
| a cache-margin reservation (`UNMAPPABLE_ISLAND_BUDGET`) was needed | **DISPROVEN.** The `nobudget` arm landed on `dyn`; reserving or not reserving made no measurable difference. The reservation and the islands were both removed |
| clearing ranked SMC candidates at savestate load / reset / cheat install was needed | **WITHDRAWN.** The `fix` arm that added those boundaries measured within noise of the arm without them. ROM-load isolation is kept because that is all the evidence justified |
| `SMC_CAND_STALE_FRAMES` (candidate staleness) | **WITHDRAWN.** Not present in active C code. It must not be described as a requirement of `SMC_GATES_RANKED` |
| `ff_psp.c` was implicated in the slowdown | **CLEARED.** +0.1% to +0.2% over n=15 on two consoles |
| the PSP Go hard freeze is explained | **NOT EXPLAINED.** See below |

### The current invariant, as implemented

A translated block never contains a direct jump to an address the translator
could not resolve. An unresolvable exit is patched **directly to
`bios_swi_entrypoint`** — a permanent pre-generated block — and its
`branch_source` is cleared. This **emits no code**, so it consumes no
translation-cache bytes and cannot contribute to cache churn.

### What the bisect actually showed

Hardware, n=2 per arm, both consoles, versus the `7283f13` control:

| arm | `heart_soul_light` | `unbound_double_high` |
| --- | --- | --- |
| `dyn` (islands present) | +16.35% / +16.98% | +8.13% / +8.25% |
| `nobudget` (reservation removed) | +16.39% / +16.96% | +8.00% / +8.11% |
| `noisland` (islands removed) | +1.17% / +1.27% | +4.68% / +4.94% |
| `fix` (extra SMC resets) | +15.88% / +16.41% | +8.13% / +8.26% |
| `stub` (current implementation) | **+0.14% / +0.12%** | **+3.54% / +3.65%** |

The mechanism was translation-cache consumption: 16 bytes of *emitted* code per
unresolvable exit filled the cache sooner, which forced earlier full flushes and
more retranslation (`xlat` 705 → 831, SMC events per window 33 → 51).

### Still unresolved

* **The `unbound_double_high` residual**: `stub` keeps +3.5% to +3.65% on both
  consoles. `BADJUMP_REPORT` file-I/O gating changed inlining and code layout
  (`block_lookup_address_arm/thumb` 720 → 568 bytes, `badjump_recover` no longer
  a symbol), which makes layout a plausible common variable. **Static code-size
  inspection does not establish causality.** The clean experiment is 7283f13 plus
  only that gating change, measured on hardware.
* **The PSP Go hard freeze has no demonstrated cause.** The shared stub is useful
  *containment* — it prevents an unresolvable translated exit from jumping into
  invalid native memory — but containment is not a causal explanation, and it
  does not establish correct guest behaviour afterwards. Do not claim
  cache-budget overflow or dispatch islands were shown to cause the freeze. It
  stays open until it is reproduced, or until long play testing makes the release
  risk acceptable.

---

## Demonstrated root cause

**A translated block could contain a direct jump to `0x0FFFFFFC`, and nothing in
the path could report it.**

`block_lookup_translate_*` returns a sentinel, `(u8 *)(~0)`, for a guest pc in a
region it does not handle — regions `0x0`, `0x2`, `0x3` and `0x8..0xD` are
translated, so I/O, palette, VRAM, OAM and SRAM are not. Upstream chose a
non-NULL value deliberately, and says so at the return:

> Do not return NULL since it could indeed happen that some branch points to
> some random place (perhaps due to being garbage). This can happen when
> especulatively compiling code in RAM.

Both eager patch loops in `translate_block_arm` and `translate_block_thumb`
tested it only for NULL:

```c
translation_target = block_lookup_translate_arm(branch_target);
if (!translation_target)
   return false;
generate_branch_patch_unconditional(external_block_exits[i].branch_source,
                                    translation_target);
```

`(u8 *)(~0)` is not NULL, so it reached the patch macro:

```c
#define generate_branch_patch_unconditional(dest, offset)      \
  *((u32 *)(dest)) = (mips_opcode_j << 26) |                   \
   ((mips_absolute_offset(offset)) & 0x3FFFFFF)
```

with `mips_absolute_offset(x) == (u32)x / 4`. So `0xFFFFFFFF / 4 = 0x3FFFFFFF`,
truncated to 26 bits = `0x3FFFFFF`, emitted as `j 0x3FFFFFF`. MIPS `j` keeps the
delay slot's top four pc bits, and the RAM translation cache lives in the
`0x08…`/`0x09…` segment, so the target is **`0x0FFFFFFC`** — outside the PSP's
main RAM. The first execution of that branch is an instruction-fetch fault with
no handler.

Three things follow, and together they match the field report exactly:

* **It is a freeze, not a crash screen.** No dispatcher call is involved, so
  gpSP never sees the bad pc and never prints anything.
* **`BADJUMP_SAFE` could not catch it.** That guard wraps
  `block_lookup_address_*`, the *runtime* dispatcher. This path is inside the
  *translator*. That is why the previous fix reduced the crash rate without
  eliminating it.
* **It is rare because the branch must be taken.** Speculative scans produce
  these targets routinely — 223 in one Heart & Soul battle, every one from a
  block a translation gate started at a non-instruction boundary — and almost
  none are ever executed.

The reported trigger is the sequence that maximises how many such blocks exist:
Unbound then Heart & Soul (which, as below, promoted gates on a single
observation), a soundtrack change (which moves which IWRAM words the M4A engine
patches), and repeated fast-forward toggling.

### The two ways a gate landed on an address that was never code

The candidate's gate rule is `SMC_GATES_RANKED`: an address earns a gate after
64 writes of which at least half changed the word. Two lifetime defects let that
requirement be satisfied without the evidence.

**Cross-ROM.** `smc_gates_reset()` cleared the gate ages and rebuilt the bitmap
but never the ranked candidate table — addresses, write counts, and the last
*value* seen at each address. Unbound and Heart & Soul are both M4A engines and
copy a sound driver into nearly the same IWRAM addresses, so a row Unbound had
already driven past 64 was still saturated when H&S wrote that address for the
first time. The stale remembered value differs, so it counts as a change, the
ratio holds, and the address is promoted **on one observation**. A 64-sample
requirement had silently become a 1-sample one.

**Within one game.** `hits` and `chg` were lifetime totals; no row was ever
aged. An address hot in one role — a boot-time copy destination, the previous
soundtrack's mixer body — kept a saturated row for the whole session, and
promoted on one write when the game later used it in another role.

---

## The safety invariants added

1. **A translated block never contains a direct jump to an address the
   translator could not resolve.** An exit whose target region the translator
   does not handle is routed through `mips_indirect_branch_*` instead. That is
   lazy — an exit that is never taken costs nothing at all — and it puts the
   fault back inside `BADJUMP_SAFE`'s reach, where an unmappable guest pc resets
   the GBA rather than the console. The region test is a pure function of the
   target, so it runs before `translation_ptr` is committed and the dispatch
   island comes out of the block's own emit budget (at most `MAX_EXITS` × 16 B =
   512 B against a 2 KB threshold, and it can only claim exits the
   `SMC_PARTIAL_SAFE` island loop would not).
2. **Learned SMC state may not outlive the evidence that produced it.**
   `smc_gates_reset()` now clears the candidate table, and is called at ROM load,
   emulator reset, and cache flush (savestate load, cheat install, libretro
   load/reset).
3. **The islands are budgeted, not hoped for.** `translation_cache_limit` is
   only tested inside the per-instruction emit loop, so everything after it
   spends `TRANSLATION_CACHE_LIMIT_THRESHOLD`'s margin unchecked, and the ramtag
   table sits immediately after the RAM cache. The limit reserves
   `UNMAPPABLE_ISLAND_BUDGET` so invariant 1 costs no margin 7283f13 had.

**Invariants 4 and 5 were withdrawn on 2026-09-18 after a hardware session.**
A staleness rule on the candidate rows and a hardened writer fingerprint, both
described below under "what went wrong".

### What went wrong: the first candidate regressed on hardware

The build installed on 2026-09-18 ran **slower than the soak average and wedged**
on a PSP Go. The user's first hypothesis was that the flags had not been
recreated faithfully. They had been — diffing the actual compiler command lines
against `build.log` from the accepted candidate shows the core defines for
`cpu_threaded.c` identical except `GIT_VERSION` and `GPSP_PROFILE_RELEASE=1`
(which only drives `#error` checks and emits no code), the frontend identical
except the same define, and both at `-O3` core / `-O2` frontend. So it was the
code, not the build.

Three defects of mine, and a pattern common to all three: **each was an
unverified assumption on a hot path.**

1. **The islands were not budgeted.** `translation_cache_limit` is tested only
   inside the per-instruction emit loop; my dispatch islands are emitted after
   it, next to the gate epilogue and the `SMC_PARTIAL_SAFE` islands, all
   spending `TRANSLATION_CACHE_LIMIT_THRESHOLD`'s 2 KB with nothing checking. I
   reasoned "512 B against 2 KB, fine" without measuring the existing worst
   case — and the ramtag table sits immediately after the RAM cache, so an
   overrun corrupts block metadata rather than failing cleanly. That is a wedge
   mechanism, new in my build. Now reserved explicitly.
2. **The staleness rule could starve the gate that pays for the performance.**
   Earning a gate needs `SMC_GATE_MIN_SAMPLE` writes *at one address*; if that
   takes longer than the ~60-frame window, the row restarts forever and the gate
   is never earned — the documented 457 KB/s, 37 fps regime. I chose 60 from
   "the mixer writes thousands of times a second", which is true across all
   addresses and says nothing about one. Withdrawn.
3. **The hardened fingerprint gates selective invalidation.** Requiring writer
   and target in IWRAM, one latched pc and N repetitions means that if any of
   those does not hold for the ROM in hand, every mixer write takes a full
   flush. I never confirmed on hardware that Unbound's writer satisfies the
   IWRAM assumption. Withdrawn.

Both withdrawals are safe *because of* invariant 1: a wrongly promoted gate is
now a dispatcher lookup rather than a jump to `0x0FFFFFFC`. The containment fix
is what makes the promotion tightening optional, and the tightening is what cost
a hardware session — so the tightening goes.

**What none of my verification would have caught.** 461 host checks, 8 suites,
PPSSPP boot smoke and a 3000-frame Emerald run all passed on the regressed
build. Every one of these defects is load- and layout-dependent on real
hardware: a cache-margin overrun needs a block near the limit with several
untranslatable exits, and both performance defects need a specific game's mixer.
The lesson is not "test more on the host" — it is that a change touching the
dynarec's hot path or its emit budget is not validated until it has run on a
console, and that a tightening with no demonstrated defect behind it should not
be bundled with a fix that has one.

### What I deliberately did not do

The brief asked to ensure a promoted gate cannot force translation to begin on
data. **It still can, and that is now safe rather than prevented** — which is
the honest description, and the stronger property.

Every structural proof available from gpSP's own metadata was already tried and
rejected here, and I re-derived why: at the moment of the write, `0300168c` — the
address behind 97.8% of all flushes — is *interior* to its block, because a
block only starts there once the gate exists. So `VALID_TAG` at the write
address (`SMC_GATES_CODED`) rejects the one gate that matters, at 59.9 → 46-58
fps; and walking backwards to a proven head (`SMC_GATES_SNAP`) keeps validity
but loses isolation, at 1.43M flushes and 31-48 fps. gpSP's tag map also cannot
distinguish `03001404` from real code, because its linear scanner decoded that
word as an instruction too.

So the load-bearing fix is containment of the consequence, and the promotion
rules are tightened as defence in depth rather than as the primary guard.

I also corrected one of my own changes mid-way: I first added an absolute floor
on the changed count, then removed it on noticing it is mathematically redundant
with `chg * 2 >= hits` at `SMC_GATE_MIN_SAMPLE = 64`. The staleness rule
replaced it because that one adds protection the ratio does not.

---

## Performance

No systematic cost, and none was expected: the containment path only fires on
targets that were previously guaranteed fatal, and the staleness check is one
compare and one store per SMC candidate hit.

PPSSPP A/B, 7283f13 harness against the candidate harness, same rig, same
fixture (Emerald, `perf_from=600 perf_to=3000`), interleaved:

| window (n=300) | control `work_us` | candidate `work_us` | delta |
|---|---:|---:|---:|
| f=900  | 1925235 | 1925221 | −14 µs |
| f=1200 | 1573028 | 1573019 | −9 µs |
| f=1500 | 1664456 | 1664454 | −2 µs |
| f=1800 | 1707586 | 1707570 | −16 µs |

`wall_us`, `wall_max`, `over=0` and the frame-time histograms are identical;
`work_max` differs by 1 µs in one window. `core_prof` mean core time is
4428 µs (control) against 4429 (candidate).

**Each arm reproduced its own four numbers exactly across both rounds**, so these
deltas are not noise -- they are a consistent −0.0005% to −0.001%, in the
candidate's favour. Most likely the `load_gamepak_page` fallback that the writer
fingerprint no longer reaches.

**This is PPSSPP, not hardware, and the Emerald title screen is not a demanding
fixture.** It shows no overhead was added to the common path. It does *not*
substitute for the Unbound rival-battle and Heart & Soul soundtrack fixtures on
a real PSP-1000 and PSP-3000, which remain the acceptance measurement.

---

## Stability evidence

| check | result |
|---|---|
| `tools/test_smc_safety.c` | 461 host checks pass. It asserts the **pre-fix** behaviour too, so it documents the defect: `patch_loop_old` on an OAM target really does encode a jump to `0x0FFFFFFC`, and `earned_old` really does promote on one write after a carry-over and after a 300-frame gap. |
| all host suites | 8/8 pass, 0 skipped (`tools/run_host_tests.py`) |
| PPSSPP boot smoke, interleaved | control 3/3, candidate 3/3 booted and survived 12 s |
| PPSSPP ROM run | Emerald (`code=BPEE`, 16 MB, so the paged ROM cache is exercised) boots, runs 3000 frames through the dynarec, `exit code=0`, no bad jumps |
| all three build profiles | build warning-clean and pass their binary audits |

**Not done: hardware.** No PSP-1000, PSP-3000 or Go run, no soak, and in
particular no attempt at the reported sequence (Unbound → Heart & Soul →
soundtrack change → fast-forward toggling → Bellsprout encounter). The freeze was
rare enough that only a hardware soak can retire it.

One rig note, which is why the smoke runs are interleaved: the first control run
failed with SIGTRAP, and the log showed PPSSPP's own Vulkan backend asserting
`VulkanDescSetPool::Reset without valid pool` at 0.3 s *in the menu*, after
GBAdhoc had already booted. Three repeats were 3/3 for both arms. A single run on
this rig proves nothing in either direction.

---

## The sprite artifact is separate

Diagnosed statically, not reproduced, and it is a Media Engine capture-timing
issue with no path to executing wrong code.

`psp/me/me_render_glue.cc` captures I/O registers **per scanline**
(`cap->ioregs[ln]`, 160 rows) but takes **one snapshot of OAM, VRAM and palette
for the whole frame**, after the host has finished emulating it.
`reg[OAM_UPDATED]` is likewise a single value, so the sprite list is sorted once.

A game that moves a sprite *during* a frame — a mid-frame OAM write, or an
HBlank DMA — is therefore drawn by the ME with the sprite where it ended up, on
every line. The CPU renderer, which runs scanline by scanline as emulation
progresses, sees OAM as it was at each line. That is the shape of "one frame,
wrong position", and heavy audio makes it more likely because load changes *when*
in the frame the game writes OAM — which is exactly the correlation reported.

The A/B that settles it is `me_mode = 0` in `GBADHOC/CONFIG.INI` (CPU renderer)
against the default. I did not redesign the renderer, and nothing here links it
to the freeze.

---

## Also found while in here

* **The shipped 7283f13 candidate writes `ms0:/badjump.txt`** from inside the
  dynarec dispatch path, on the emulation thread, and `printf`s there too.
  `BADJUMP_SAFE` was wanted and its logging side was not; the release audit's
  forbidden-string list did not cover it. Both are now behind
  `BADJUMP_REPORT`, and `gpsp_profile.h` refuses that define in a release.
* **`cheats.c` scanned `%08x` into a `u32`**, which is `unsigned long` on this
  ABI. Same width on MIPS32, so it worked; on a 64-bit host build of the core
  `sscanf` writes four bytes into an eight-byte object.
* **The pixel format was decided in three independent places** — the top-level
  Makefile, `psp/Makefile`, and `psp/me/Makefile`, the last of which hardcoded
  it. A `PSP_PIXFMT=rgb565` build would have given a core and frontend on one
  layout and an ME renderer on the other, with R and B swapped in ME mode only.
  The core/frontend pair is now a link error; the PRX inherits the value.
* **The Makefile's flag documentation had drifted off its flags.** Five
  explanations sat above the wrong `ifeq`, and `SMC_GATES_RANKED`'s described a
  promotion threshold the code does not have.
* **Nine behaviour-changing dynarec switches have no recorded verdict anywhere.**
  `SMC_GATES_SNAPF` is the notable one: it is the only gate-address rule designed
  to keep validity *and* isolation, and it has never been measured. See
  `docs/BUILD-SWITCHES.md`.

---

## What to do next, in order

1. **Hardware.** Install the release profile from
   `builds/opus-stability-candidate/release/` and attempt the reported sequence
   on a PSP Go: Unbound, then Heart & Soul without leaving GBAdhoc, change the
   soundtrack, toggle fast-forward repeatedly, then battle.
2. **Confirm the perf claim on hardware** with the Unbound rival fixture and both
   Heart & Soul soundtracks, against 7283f13.
3. **Settle the sprite artifact** with the `me_mode = 0` A/B.
4. **Nothing to do on the fast-forward artifact.** It was fixed by `ecdd8bc`
   plus `add0f4d` and confirmed on a PSP-3000 before this branch existed; all
   three presets are covered, and `docs/FF-ARTIFACT-FIX.md` records the
   ownership rules. Earlier drafts of this document said it was still open on
   the unlimited presets and recommended a `gu_defer` A/B -- both wrong, and
   `gu_defer` had already been rejected from the SDK disassembly.
5. If the diagnostic profile is ever run in anger, read
   `unmappable block exits contained:` in `ms0:/smchisto.txt` — that counter is
   how many jumps to `0x0FFFFFFC` a pre-fix build would have written.
