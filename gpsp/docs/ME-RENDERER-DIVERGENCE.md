# The ME renderer diverges from the CPU renderer, and it is deterministic

Measured 2026-09-18 on both consoles. This is the first time the Media Engine
renderer's output has actually been compared against the core's own renderer on
hardware, and it is the only place it *can* be compared: PPSSPP never runs the
PSP's second core (`me_init reason=handshake magic=0x00000000`), so the desktop
video oracle cannot reach this code at all. See the coverage table in
[SUBSYSTEMS.md](SUBSYSTEMS.md).

## Method

One EBOOT, one fixture, one input script, one save state. The arms differ by a
single ini key, `me_mode`. `vhash_frame()` hashes the **presented** frame in
both modes — the ME staging buffer when the engine is up, the core buffer
otherwise, FNV-1a over 240x160 halfwords either way — so the two `EVT vhash`
streams are directly comparable.

Fixture `heart_soul_heavy`, frames 300-3900, round 01, on the PSP-1000 and the
PSP-3000. The engine was up for the whole run on both (`EVT me_rend on`, and the
only `me_rend off` is `reason=exit`), so nothing here is a CPU fallback in
disguise.

Frames are compared only where **both** arms logged a hash. The EVT ring drops
lines under load and a dropped line is not a difference. On these runs the ME
arm logged 3480 of 3600 and the CPU arm 2159, leaving 2078 frames judgeable on
both consoles. **Every count below is therefore a lower bound.**

## Result

Over the 2078 frames both consoles could judge:

| | PSP-3000 | PSP-1000 |
| --- | --- | --- |
| stale by exactly one frame | 277 | 277 |
| images the CPU renderer never produced | **188** | **188** |
| frames where the two consoles disagree | \-\- | **0** |

The 188 agree completely between consoles, and all 188 carry the **identical ME
hash on both machines**. Two consoles with different clocks and different memory
timing produce bit-identical unmatched images at identical frame numbers.

**So neither behaviour is a race.** A deadline-miss mechanism would shift with
machine speed; these do not move at all. Both are deterministic in the emulated
content. That argues strongly against ordinary variable deadline slippage.

The lag search is equally clean: of the differing frames, the ones that *are*
explainable match the CPU's frame at exactly **-1 and nowhere else** — nothing at
-2..-8, nothing at +1..+8. The pipeline is one frame behind, always, by design.

## The two mechanisms

**Stale by one frame (277).** The documented pipeline latency: the engine renders
what was captured last frame. Invisible whenever the picture is not changing,
which is why these cluster in animated passages. Expected behaviour, not a
defect, and the `-1`-only distribution confirms the pipeline never slips further.

**Unmatched images (188).** The engine presented pictures the CPU renderer never
produced at any moment within +-8 frames.

The measurement is solid. **The mechanism is a hypothesis**, and it is stated that
way deliberately:

> The ME renderer shows deterministic, content-dependent divergence consistent
> with combining **per-scanline I/O capture** (`cap->ioregs[ln]`) with a
> **single graphics-memory snapshot** for the replayed frame.

If the game rewrites graphics memory partway down a frame, the core sees the
change from that scanline onward while the engine composes the whole frame from
one snapshot. That is consistent with every measurement here and with the
reported symptom — sprites briefly misplaced during battle targeting — but it is
an inference from output hashes, not a demonstration.

**What is NOT established:**

* **which** omitted input is responsible. OAM is the obvious suspect for a sprite
  symptom, but VRAM, palette and affine state are captured the same way and have
  not been separated. Do not claim OAM caused every divergent image.
* whether more than one input contributes.
* whether every unmatched frame is objectively *wrong* per GBA hardware. The CPU
  renderer is the reference here only because it is the shipped alternative, not
  because it has been validated against real hardware frame by frame.
* whether a deterministic ordering or synchronisation fault contributes
  independently of the snapshot granularity.

Isolating these means instrumenting which region was written mid-frame, or
capturing one input per scanline at a time and re-running the A/B. That is a
separate campaign on hardware, since PPSSPP cannot execute the ME at all.

Where they fall, and why the distribution is misleading at first glance:

| frames | compared | stale | unmatched |
| --- | --- | --- | --- |
| 300-599 | 300 | 161 | 45 |
| 600-899 | 300 | 19 | **122** |
| 900-1199 | 300 | 13 | 7 |
| 1200-1799 | 600 | 88 | 5 |
| 3300-3899 | 600 | 37 | 9 |

89% land in frames 300-899, which is also where the engine was 19-23 ms a frame
and over budget on 180-240 of every 300 frames. That correlation is **not**
causation: it is the battle intro, the most content-dynamic passage in the
fixture, and the cross-console determinism argues strongly against the load
explanation. The 21 unmatched images in the later, comfortably-in-budget passages
(11.5 ms a frame by f=3600) make the same point.

## Second fixture: it carries across content

`unbound_rival_medium` (a different game) on the PSP-3000, same method:

| | value |
| --- | --- |
| differing frames | 170 |
| stale by exactly -1 | 61 |
| unmatched within +-8, full neighbourhood | **106** |

Again the lag distribution is **-1 and nothing else**. So the effect is not
specific to Heart & Soul, and the one-frame-latency mechanism behaves identically
on both fixtures.

## Round 02: the count is stable

The r02 jobs completed (the campaign drained 99/99 on both consoles with no
halts). PSP-3000, same method:

| fixture | round | unmatched within +-8 | full-neighbourhood subset |
| --- | --- | --- | --- |
| `heart_soul_heavy` | r01 | 188 | 188 |
| `heart_soul_heavy` | r02 | 188 | 187 |
| `unbound_rival_medium` | r01 | 109 | 106 |
| `unbound_rival_medium` | r02 | 111 | 108 |

`heart_soul_heavy` returns **exactly 188 in both rounds**. The small movement on
`unbound_rival_medium` is which frames were *comparable*, not which diverged:
the CPU arm drops a different set of vhash lines each run, so the judgeable set
differs slightly between rounds.

Both rounds show the same `-1`-only lag distribution.

**This investigation is now deferred.** The results are preserved; no further ME
experiments are scheduled. Returning to it means starting from the unproven list
above — which input — not from re-measuring the divergence.

## What this does not establish
* **A lower bound only.** Roughly 1460 frames (about 1830-3290) went uncompared
  because the CPU arm dropped those vhash lines. Wrong images there would not
  have been seen.
* **Not the cause of the perf regression.** Unrelated; see the stub-fix results.
* **No fix is proposed here, and none should be attempted yet.** A fix cannot be
  designed before the responsible input is identified — see the unproven list
  above. Once it is, the shapes available are giving the engine finer-grained
  state for that input (memory and bandwidth it may not have), or detecting
  mid-frame writes to it and falling back to the CPU renderer for that frame.
  Both are designs, not patches, and neither can be validated anywhere but on
  hardware.

## Reproducing

    builds/opus-perf-harness/append_renderer.py      # queue the A/B
    builds/opus-perf-harness/compare_renderers.py    # summarise a collected pair

`compare_renderers.py` reports the stale/wrong split directly. Its "matches no
neighbour" figure is only meaningful for frames whose neighbours the CPU arm
actually logged — on these runs 188 of 189 qualified, but that check must be made
every time, because dropped lines otherwise masquerade as wrong images.
