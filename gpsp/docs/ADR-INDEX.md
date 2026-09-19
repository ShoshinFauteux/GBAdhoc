# Canonical ADR status index

**Generated** from the headings in `docs/DECISIONS.md` by
`gen_adr_index.py`. Regenerate it rather than editing it by hand, so the
identifiers and line numbers cannot drift from the file they describe.

`status` is deliberately conservative. **`unreviewed` means nobody has
checked the record against current behaviour — not that it is current.**
Only relationships stated in a heading, or established by evidence
recorded in this branch, are marked otherwise.

## How to read the collisions

Five identifiers carry two *primary* records each:

    ADR-0008  ADR-0033  ADR-0034  ADR-0035  ADR-0036

`ADR-0008` is a plain duplicate-numbering mistake: two unrelated mainline
records share it. `ADR-0033`..`ADR-0036` are an imported **phase6-coreopt**
series overlapping the **mainline** numbering.

They are disambiguated here by the aliases `(mainline)` and `(phase6)`,
and by line number. **They are not renumbered.** Active source comments
cite these numbers, and renumbering would silently break every reference
while leaving the citations looking correct.

A heading of the form `ADR-NNNN addendum` is **not** a collision — it is a
continuation of the same record, and often the place where that record's
original conclusion was corrected. An earlier count of "eight duplicated
identifiers" came from a naive grep that treated addenda as collisions;
the real figure is five.

## Coverage boundary

This file stops at **ADR-0053**. Active source and the outer HANDOVER cite
ADR-0054..ADR-0082, which are **not recorded here at all**. Do not assume a
number above 0053 is missing or invalid; look in the outer handover
document. Those records must be checked against current code and hardware
evidence before being imported, one at a time.

Numbers absent from this file below that ceiling: ADR-0043, ADR-0044, ADR-0045, ADR-0046, ADR-0047, ADR-0051, ADR-0052.

One known discrepancy, unresolved: the harness control-channel rename
(`autopilot.ini` -> `.gpsp-harness.ini`) is recorded here as **ADR-0036
(mainline)**, but active source and tooling cite it as **ADR-0067**. Both
names are in use. Do not "fix" either until the outer records are
reconciled.

## Records

| record | alias | line | status | replaced by | subsystem | title |
| --- | --- | --- | --- | --- | --- | --- |
| `ADR-0001` | - | 7 | unreviewed | - | - | Base repo and commit |
| `ADR-0002` | - | 15 | unreviewed | - | - | Local repo only until user picks hosting |
| `ADR-0003` | - | 23 | unreviewed | - | networking | Transport design corrections from the Phase-0 code read |
| `ADR-0004` | - | 31 | unreviewed | - | frontend | Clock policy: §9 governs over risk-register row 6 |
| `ADR-0005` | - | 37 | unreviewed | - | - | Desktop twin links the core as a static archive of unix objects |
| `ADR-0006` | - | 45 | unreviewed | - | storage | SRAM policy: persist the full 128 KiB, CRC32 dirty-compare |
| `ADR-0007` | - | 52 | unreviewed | - | video | PSP memory posture: PSP_LARGE_MEMORY=0, 1 MiB heap slack, greedy ROM buffer kept |
| `ADR-0008` | mainline | 59 | unreviewed | - | networking | netdrv deviations from §4.3 and Phase-3 smoke scope |
| `ADR-0008` | mainline | 112 | unreviewed | - | video | Autopilot: RAM predicates over frame timing; SRAM-CRC as the save oracle |
| `ADR-0009` | - | 177 | unreviewed | - | - | Canonical test ROMs and the patched Emerald |
| `ADR-0010` | - | 183 | superseded | ADR-0017 | networking | netdrv ARQ resize + timing retune from the Gate-3 trade runs |
| `ADR-0011` | - | 207 | unreviewed | - | networking | First core patch: rfu.c packet queues 4 → 16 (RFU_PKT_QUEUE) |
| `ADR-0012` | - | 262 | unreviewed | - | networking | PSP ad-hoc transport: static singleton, halved ARQ knobs, autopilot-only surface |
| `ADR-0013` | - | 336 | unreviewed | - | networking | Silent-wireless policy + second core patch (RFU activation hook) |
| `ADR-0014` | - | 359 | unreviewed | - | frontend | Phase-2 UI decisions (deviations & choices within plan §8) |
| `ADR-0015` | - | 401 | unreviewed | - | pacing | v0.1.0 scope calls: no clock setting, suspend/resume deferred, no FPS counter |
| `ADR-0016` | - | 424 | unreviewed | - | networking | RELIABLE payloads are never dropped: backpressure + spill, and a right-sized ad-hoc slot |
| `ADR-0017` | - | 465 | unreviewed | - | networking | Adaptive per-peer RTO with transport-level floors (supersedes ADR-0010's fixed 30 ms) |
| `ADR-0018` | - | 520 | superseded | ADR-0019 | pacing | Core auto-frameskip while `net=up` (the frontend now answers the audio-buffer-status env) |
| `ADR-0019` | - | 541 | unreviewed | - | video | A wireless session no longer blanket-enables frameskip (supersedes ADR-0018's policy) |
| `ADR-0020` | - | 593 | unreviewed | - | video | The emulation thread is never stalled by a save write (dirty blocks + held handle + budgeted drain) |
| `ADR-0021` | - | 654 | unreviewed | - | - | A live session's per-frame cost is measured, not guessed (and the sends come off the emulation thread) |
| `ADR-0022` | - | 801 | unreviewed | - | storage | FR/LG parked saves are supplied by hand, not played from a new game |
| `ADR-0023` | - | 831 | unreviewed | - | harness | Cross-edition harness: one edition table, and ROM revision checked before launch |
| `ADR-0024` | - | 855 | unreviewed | - | - | No memory-stick write happens on the emulation thread: the EVT/LOG writer moves to its own thread |
| `ADR-0025` | - | 913 | unreviewed | - | video | The .sav block writes leave the emulation thread too (ADR-0020's budget was necessary, not sufficient) |
| `ADR-0026` | - | 978 | unreviewed | - | - | The dirty scan follows the writes off the emulation thread (and what the user's memory stick actually costs) |
| `ADR-0027` | - | 1030 | unreviewed | - | networking | Peers match each other's emulated frame rate (equality beats absolute speed) |
| `ADR-0028` | - | 1156 | unreviewed | - | - | The core itself misses the frame deadline: attribute the spike, stabilise the matcher, stop paying for audio nobody can hear |
| `ADR-0029` | - | 1311 | unreviewed | - | dynarec | The core's spike is self-modifying code, not a small JIT cache: split the flush counter and stop planning around the wrong mechanism |
| `ADR-0030` | - | 1415 | unreviewed | - | video | WHERE the SMC writes land: 99.6 % of the flush storm is ONE halfword. Add address profiling; it is over-tagged data, not overlay swapping |
| `ADR-0031` | - | 1550 | unreviewed | - | video | The SMC storm is genuine self-modifying code: Emerald builds a two-instruction function on its stack. Block termination and tagging are both innocent, and the flush costs ~1 us, not ~70 |
| `ADR-0032` | - | 1708 | unreviewed | - | dynarec | Where the core's frame actually goes: bracket `retro_run` by phase. On the rig VIDEO is 59 % of it and the dynarec 36 %, and the phase maxima must never be summed |
| `ADR-0033` | mainline | 1852 | unreviewed | - | video | The requirement changed: a wireless session clamps both consoles to a FIXED frame rate. No negotiation, no control loop, no hunting |
| `ADR-0033` | phase6 | 3208 | unreviewed | - | video | Nothing in the renderer gets optimised until a frame-exact oracle exists, and the oracle has been made to fail on purpose |
| `ADR-0034` | mainline | 1981 | unreviewed | - | video | Put the blit's staging buffer in VRAM: 2513 → 1800 µs per frame, and most of the win is the GE, not the copy |
| `ADR-0034` | phase6 | 3264 | unreviewed | - | video | Where the renderer's time actually goes: it is ONE loop (4bpp tiled text BG, 4x overdrawn), and the VFPU targets in the brief are 0-4 % of it |
| `ADR-0035` | mainline | 2165 | unreviewed | - | video | The field answers ADR-0033/0034: 40.00 fps is unreachable by construction (snap to whole vblanks), the blit ratio INVERTS on hardware, and a missing Makefile dependency shipped a silent struct-layout corruption |
| `ADR-0035` | phase6 | 3389 | unreviewed | - | video | Phase 3 attempt 1 (byte-pair palette LUT) is REVERTED: pixel-exact but measurably slower, and its control run did not reconcile |
| `ADR-0036` | mainline | 2319 | unreviewed | - | video | The harness control channel gets an internal name: `autopilot.ini` → `.gpsp-harness.ini`, and a leftover legacy file is reported rather than obeyed |
| `ADR-0036` | phase6 | 3464 | unreviewed | - | video | The 4x overdraw is real and half of it is thrown away: 52 % of background stores are overwritten before the frame is shown, and the base layer is 93 % covered |
| `ADR-0037` | - | 2393 | unreviewed | - | video | The adaptive frameskip was measuring against a rate we stopped aiming at: compare with the APPLIED pace target |
| `ADR-0038` | - | 2444 | unreviewed | - | video | Split the blit's `gu` number into list-building and the GE wait, because only one of them is recoverable |
| `ADR-0039` | - | 2467 | unreviewed | - | video | Emit the GE's channel order from the core's palette conversion and delete the frontend's per-pixel swap |
| `ADR-0040` | - | 2553 | superseded | ADR-0049 | video | Let the GE finish during the vblank wait instead of blocking on it (`gu_defer`, default OFF) |
| `ADR-0041` | - | 2592 | unreviewed | - | networking | The exit-Union-Room screen is the game's FATAL RFU path, and we now know exactly what produces it (instrumentation, no fix) |
| `ADR-0042` | - | 2713 | corrected-by-addendum | ADR-0042 addendum | networking | Cap how many RFU packets a joining console hands the game per frame (`rfu_rx_cap`, default 0 = off) |
| `ADR-0042 addendum` | - | 2770 | addendum | - | harness | what the rig actually established, 2026-08-03 |
| `ADR-0048` | - | 2806 | unreviewed | - | - | Split `rx_dup`, and report the four numbers that make a retransmission legible |
| `ADR-0049` | - | 2877 | unreviewed | - | - | `gu_defer` defaults ON: a clean hardware A/B, and two hypotheses it killed |
| `ADR-0050` | - | 2931 | corrected-by-addendum | ADR-0050 addendum | networking | Split the transmit queue into hot metadata and cold payload |
| `ADR-0050 addendum` | - | 2984 | addendum | - | - | the hypothesis was wrong, and so was the number that motivated it |
| `ADR-0053` | - | 3034 | unreviewed | - | harness | Unattended hardware loop, milestone 1: the console hands over its own memory stick |
| `ADR-0053 addendum` | - | 3102 | addendum | - | harness | validated on the rig, and the bug it caught |
| `ADR-0053 addendum` | - | 3144 | addendum | - | harness | the harness, finished as far as the rig can take it |

## What still has to happen

1. Review each `unreviewed` row against current code, and set it to
   `current`, `superseded`, `withdrawn`, `rejected` or `experimental`.
2. Import ADR-0054..0082 from the outer HANDOVER one at a time, checking
   each claim before it is recorded as a decision.
3. Resolve the ADR-0036/ADR-0067 naming of the control-channel rename.
4. Only then consider a reviewed migration that gives the colliding
   records unique identifiers, updating every citing source comment in the
   same change.
