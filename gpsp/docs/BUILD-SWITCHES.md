# BUILD SWITCHES — the inventory

Every dynarec and diagnostic switch in the tree, with a verdict. 27 in the
top-level Makefile plus 6 frontend defines.

**Why this exists.** Nine of these ship. Five are experiments that were tried
and rejected, with the measurement that rejected them. Several have no recorded
verdict anywhere — not in `DECISIONS.md`, not in the Makefile — so the only way
to know what they were for was to read `cpu_threaded.c`. A flag with no verdict
is worse than no flag: it looks like an option.

Two defects found while compiling this list and fixed in the same commit:

* Four comment blocks (`BADJUMP_REPORT`, `GBA_PC_MASK`, `SMC_GATES_CODED`,
  `SMC_GATES_SNAP`) had drifted away from their own `ifeq` guards and were
  stacked above `SMC_GATES_SNAPF`'s, so reading the Makefile top to bottom
  attributed each explanation to the wrong flag. `SMC_SCAN_SPCLAMP`'s comment
  sat above `SMC_WRITE_HISTO`'s guard the same way.
* `SMC_GATES_RANKED`'s comment described an `SMC_GATE_PROMOTE_HITS` threshold
  (default 256) **that the code does not have**. Frequency thresholds were tried
  and failed twice; the shipped rule is sample count *plus* a changed-word ratio
  *plus* a staleness bound. Anyone tuning "256" would have been tuning nothing.

---

## Shipped in every profile

These are the flags `tools/build.sh` passes for release, harness and diagnostic
alike. Changing one changes emulation.

| switch | what it does |
|---|---|
| `SMC_GATES` | put a translation gate at a self-modified address so later blocks split there. Deliberately independent of `SMC_PARTIAL*`: gates bought Unbound its speed and also broke its audio, so the two must be testable apart. |
| `SMC_GATES_SIMPLE` | the 2.0.1 add-only gate rule. Once placed, a gate never moves, so an already-translated block can never disagree with the table. 2.0.2's evicting rule is what crashed Heart & Soul. |
| `SMC_GATES_RANKED` | promote only an address that earned it: 64 writes, at least half of which changed the word, all from one continuous episode. See below. |
| `SMC_GATE_BITMAP` | same gate table and boundaries, tested through a bitmap instead of comparing every live gate per compiled instruction. |
| `SMC_PARTIAL_SAFE` | selective invalidation with exact per-ISA block extents. Advancing copy loops keep the full flush. |
| `SMC_PARTIAL_STABLE_THUNK` | a permanent per-block RAM entry thunk, so retiring a block redirects the thunk rather than orphaning inbound links. |
| `SMC_PARTIAL_DIRECT_LINKS` | keep RAM→RAM links direct; route only persistent ROM→RAM links through an island. Requires the stable thunk. |
| `GBA_PC_MASK` | mask the emulated pc to 28 bits before the region lookup, as the GBA does. **Not sufficient alone** — a masked garbage target lands in a region the translator *does* handle. |
| `BADJUMP_SAFE` | an unmappable guest pc soft-resets the GBA instead of `jr`-ing to it. Cannot regress: the path it replaces crashes 100% of the time. |
| `ROM_BUFFER_SIZE=15` | set unconditionally for PSP by the Makefile. A PSP-3000 can otherwise allocate all 32 one-megabyte blocks and Unbound diverges in battle. |

### What `SMC_GATES_RANKED` actually requires

All three, in `cpu_threaded.c`'s `smc_gate_earned`:

```
SMC_GATE_MIN_SAMPLE    64 writes of evidence
chg * 2 >= hits        at least half of them CHANGED the word
```

Two conditions, not three.  A staleness rule (`SMC_CAND_STALE_FRAMES`,
"the evidence is from one continuous episode") was proposed and
**withdrawn**; it is not present in `cpu_threaded.c` and is not a
requirement of `SMC_GATES_RANKED`.

Measured on the Heart & Soul rival battle, over one 8000-flush window:

| address | same | changed | what it is |
|---|---:|---:|---|
| `0300168c` | 120 | 62953 | real code being patched — the gate that matters, 97.8% of all flushes |
| `03001404` | 66 | 1 | a constant re-stored into a code block — the address that crashed us |

That gap is structural, not tuned. Frequency was the wrong discriminator and
failed twice: an absolute threshold is outgrown by any cold address, and a ratio
to the hottest candidate degenerates because a promoted gate stops counting and
the maximum freezes. Both crashed.

---

## Rejected, with the measurement

Kept in the tree because the measurement is the point; refused in a release
build where that applies.

| switch | verdict |
|---|---|
| `SMC_SKIP_SAME` | **White-screens Heart & Soul at boot** — the idempotent-store compare is wrong. `gpsp_profile.h` refuses it in a release. |
| `SMC_GATES_CODED` | Demands the write address itself already be a block start, which rejects `0300168c` because at the moment of the write that address is *interior* to its block. 59.9 → 46-58 fps. |
| `SMC_GATES_SNAP` | Snaps backwards to the block head: valid, but the block starting at that head still spans the SMC region, so isolation is lost. 1.43M flushes, 31-48 fps, against 116k and a locked 59.9 for add-only. |
| `SMC_PARTIAL` | Superseded by `SMC_PARTIAL_SAFE`. Its block-copy detector is the ancestor of the current writer fingerprint. |
| 2.0.2's evicting gate rule (no flag; the `#else` branch when neither `SIMPLE` nor `CLUSTER` is set) | Eviction reassigns a gate's address while blocks compiled against the old layout are live, and gpSP's tag map holds one slot per halfword — it cannot represent the resulting overlap. This is the Heart & Soul crash. |

---

## Built but with no recorded hardware verdict

**These are the ones to be careful with.** Each compiles, each changes
behaviour, and none has a result written down. Listed so that "there is a flag
for that" is never mistaken for "that was tried".

| switch | what it would do | status |
|---|---|---|
| `SMC_GATES_SNAPF` | Snap the gate *forward* to the first proven boundary at or after the write, which is the only variant designed to keep validity **and** isolation. | Never measured. The most interesting unexplored option here. |
| `SMC_GATES_CLUSTER` | Add-only plus 2.0.2's cluster grouping, without its eviction — keeps the half that cut dispatcher lookups (196k → 59k) and drops the half that crashed H&S. | Never measured on hardware. |
| `SMC_SCAN_SPCLAMP` | End `scan_block` at the stack pointer instead of the top of IWRAM, so scans stop tagging the live stack as code (10122 flushes measured in one battle from exactly that). | Never measured. Plausible performance win. |
| `SMC_SP_CHECKED` | Send SP-based block stores down the general path so their final word reaches the SMC check — the last known hole after the STM detector fix. | Never measured. Correctness-positive, costs speed on the hottest memory path. |
| `SMC_PARTIAL_SAFE_FRAMEFULL` | One full flush per frame regardless. | Never measured; no comment in the Makefile. |
| `SMC_PARTIAL_SAFE_NO_TRAMP` | Selective retirement without the trampoline. | Never measured; no comment. |
| `SMC_PARTIAL_DIRECT_GATE` | Patch the gate fall-through directly instead of through the dispatcher. | Never measured; no comment. |
| `GATE_SLOTS`, `GATE_RATIO`, `CLUSTER_BYTES` | Override `MAX_TRANSLATION_GATES`, `SMC_GATE_RATIO`, `SMC_GATE_CLUSTER`. | Tuning knobs, not experiments. `CLUSTER_BYTES` 128 is sized to H&S's M4A layout (two loop copies 152 bytes apart), not picked for roundness. |
| `OVERCLOCK_60FPS` | Upstream's 60 fps overclock build. | Untouched by this project. |

## Control arms

`SMC_PARTIAL_SAFE_CONTROL` disables the selective-invalidation fast path while
leaving everything else in place. It exists to make an A/B one variable. It is
refused in a release, because shipping it reads as an unexplained performance
regression with no apparent cause.

## Diagnostics

| switch | writes | profile |
|---|---|---|
| `BADJUMP_REPORT` | `ms0:/badjump.txt` — registers, gate table, previous block entry, and re-enables `badjump_recover`'s file append | `diagnostic` |
| `SMC_WRITE_HISTO` | `ms0:/smchisto.txt` every 4000 flushes — write addresses and storing pcs bucketed, plus the count of unmappable block exits contained | `diagnostic` |

Both write to the memory stick from inside the dynarec dispatch path, on the
emulation thread. Both are refused in a release by `gpsp_profile.h`.

## Frontend defines

| define | meaning |
|---|---|
| `GPSP_PLAYABLE` | build a binary for someone to play (ADR-0067). Among other things it points the harness ini at a filename that cannot exist, so a leftover `.gpsp-harness.ini` on a player's card is inert. |
| `VID_TRIPLE` | three framebuffers instead of two. Fixes tearing; brought a fast-forward black-bar artifact, since resolved for all three presets by `g_fb_filled` plus `vid_swap()`'s display-ownership read-back (`docs/FF-ARTIFACT-FIX.md`). |
| `GPSP_KEEP_TELEMETRY` | keep the EVT logger. Without it `fe_evt()` compiles to `((void)0)` and does **not** evaluate its arguments (ADR-0067b). |
| `GPSP_PERF_RIG` | the performance harness: autopilot plus perf windows. Requires `GPSP_KEEP_TELEMETRY`. |
| `GPSP_ROMLOAD_DIAGNOSTICS` | ROM-load instrumentation. Refused in a release. |
| `GPSP_STANDBY_NOTES` | standby/resume tracing. |

## Build plumbing, not experiments

`HAVE_DYNAREC`, `CROSS_COMPILE`, `FORCE_32BIT_ARCH`, `BIG_JIT`,
`SMALL_TRANSLATION_CACHE`, `PSP_PIXFMT`, `FRONTEND_SUPPORTS_RGB565`,
`GPSP_PROFILE`.

`BIG_JIT=1` drops `SMALL_TRANSLATION_CACHE` for 10MB/512KB caches on
large-memory hardware; the small caches fit the PSP-1000's budget, and
CFRU-class hacks overflow them and flush-storm.

---

## The rule for adding one

A switch needs three things or it becomes another entry in the "no recorded
verdict" table:

1. A comment **immediately above its own `ifeq`**, saying what it does and what
   it would prove.
2. An entry here once it has been measured, with the number.
3. A `gpsp_profile.h` check if it must never reach a release, or if it silently
   shadows another switch.
