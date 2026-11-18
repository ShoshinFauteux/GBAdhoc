# Architecture decision records, ADR-0043 and up

Records ADR-0001 to ADR-0053 live as headings in [`../DECISIONS.md`](../DECISIONS.md),
indexed by [`../ADR-INDEX.md`](../ADR-INDEX.md). That file has gaps
(0043-0047, 0051, 0052) and stops at 0053, while source code and docs cite
numbers up to 0082. The files in this folder cover every number that is
cited in the tracked tree but has no heading in DECISIONS.md: one file per
number, with letter suffixes (0062b-e, 0067b, 0075b, 0080a-e) folded into
their parent record.

**Every record here is reconstructed.** No original ADR text for these
numbers exists anywhere in the repository's history (a search of all
branches for ADR headings above 0053 finds only Makefile comments). The
"outer handover document" that `ADR-INDEX.md` points to only summarises a
few of them. Each record says what it was rebuilt from: the commit message
written with the change (the best source), the code comments that introduced
the number (most of 0051-0074 first appear in the overlay commit `276e4450`,
2026-08-09, with no separate commit message), and the handover's §4
summaries (`gpsp/docs/HANDOVER.md` at `dba5e900`, earlier `docs/HANDOVER.md`;
deleted from the tree in `6ab4221b`). Numbers quoted
in a record are quoted from the named source; none were re-measured. Status
was decided by reading the code at the time of writing.

Several cited commits (for example `276e4450`, `f43e89ee`) are on side
branches that reached the mainline through release squashes; use
`git log --all` or `git show <sha>` to find them.

| ADR | Title | Status | Reconstructed | Basis | Summary |
|---|---|---|---|---|---|
| [0043](ADR-0043.md) | Flight recorder for the exit-Union-Room fatal | current | yes | commit msg | 512-entry ring of every adapter command, dumped when a live session ends. |
| [0044](ADR-0044.md) | Stop dropping RFU payload on a burst | current | yes | commit msg | Receive queue 16 to 64 slots; report the deepest it gets. |
| [0045](ADR-0045.md) | Wall clock on the flight recorder | current | yes | commit msg | One clock read per frame, stamped on every recorder entry. |
| [0046](ADR-0046.md) | Log the input mask the core receives | current | yes | commit msg | One log line per change of the button mask the game is handed. |
| [0047](ADR-0047.md) | Pace against the vblank counter | current | yes | commit msg | Absolute vblank target; overruns consume their budget; arrears forgiven. |
| [0051](ADR-0051.md) | Count and time the adapter's SIO work | current | yes | code only | `rfux` calls/us per frame, previously hidden in the `cpu` residue. |
| [0052](ADR-0052.md) | Recorder dumps on any session-ending transition | current | yes | code only | Dump on reset or either disconnect from a live state, not reset only. |
| [0054](ADR-0054.md) | Every log carries its build identity | current | yes | code only | `EVT build stamp`; A/B arms group by build. |
| [0055](ADR-0055.md) | Measure the client's answer latency | current | yes | code only | How long a host packet waited before the game consumed it. |
| [0056](ADR-0056.md) | Test console parks instead of exiting | current (test builds) | yes | code only | USB handoff loop parks reachable; only `CMD=EXIT` leaves. |
| [0057](ADR-0057.md) | START+SELECT abort; keep the console awake | current | yes | code + handover | Clean-teardown abort gesture; power tick for the whole run. |
| [0058](ADR-0058.md) | Instruments report every window, zeros included | current | yes | code only | Periodic censuses so silence cannot mean a dead probe. |
| [0059](ADR-0059.md) | Adapter deadlines observable and stretchable | current | yes | code only | Timeout traces, timeout-scale test knobs, ACK frame counters. |
| [0060](ADR-0060.md) | Client liveness ACK unreliable, silence-driven | current | yes | code + handover | CLIENT_ACK unreliable and only after 1 s of client silence. |
| [0061](ADR-0061.md) | Measure the gap between transport pumps | current | yes | code only | Mean/max pump interval, to tell starvation from window blocking. |
| [0062](ADR-0062.md) | Give the Wi-Fi stack CPU time (b-e) | current; `adhoc_net_prio` removed | yes | code + handover | `emu_prio`, `io_prio`, poll throttle, ARQ timers, `core_phase` read. |
| [0063](ADR-0063.md) | Frame-time histogram (`frame_hist`) | current | yes | code only | Bucketed frame work time plus late/forgive/nudge counters. |
| [0064](ADR-0064.md) | Preemption census (`preempt_prof`) | retired (removed) | yes | code + handover | Answered: 2.0 preemptions/frame, all by our own TX thread. |
| [0065](ADR-0065.md) | Protected priority window | retired (falsified, removed) | yes | code + handover | Boost emulator above Wi-Fi early in each frame; premise disproved. |
| [0066](ADR-0066.md) | Screenshot marker-file poll as a knob | retired (removed) | yes | code only | Priced the poll at 12.4 ms; replaced by ADR-0069. |
| [0067](ADR-0067.md) | The playable build (and 0067b) | current, amended | yes | code + handover | Harness file unreachable in player builds; telemetry compiled out. Explains the ADR-0036/0067 mix-up. |
| [0068](ADR-0068.md) | Host reads the network mid-frame | current | yes | code + handover | Host polls every ~1/8 frame; halves srtt at a host fps cost. |
| [0069](ADR-0069.md) | Screenshots by button chord | current; yield removed | yes | code + handover | L+R+SELECT replaces a 12.4 ms Memory Stick lookup. |
| [0070](ADR-0070.md) | Wireless activates on session start, not login | current | yes | code + handover | Activation hook moved to search/host start commands. |
| [0071](ADR-0071.md) | Trading profiles, applied by relaunch | partly current | yes | code + handover | `profile` 57.00/29.97 latched at boot; settings row gone. |
| [0072](ADR-0072.md) | Instruments for lost input and lobby error | current | yes | code + handover | Gate-trip rate, `RESP_DISC` answers, arrival clumping, queue peak. |
| [0073](ADR-0073.md) | Speed-profile session rate 57.00 | superseded (scheduled ticks) | yes | code + handover | 57.00 fps clamp; ticks now let sessions run at 59.73. |
| [0074](ADR-0074.md) | Defer disconnect until queue drains | retired (removed) | yes | code + handover | Premise dead (`queued=1`); replaced by ADR-0076. |
| [0075](ADR-0075.md) | Per-frame delivery admission (and 0075b) | retired (removed) | yes | commit msg | Frame pace shipped on, then made redundant by ticks. |
| [0076](ADR-0076.md) | Grace window for the exit race | current | yes | commit msg | Stay CLIENT 120 frames after peer disconnect so the game exits cleanly. |
| 0077 | unknown | unknown | n/a | none | Never cited anywhere in tracked files or history; no record exists. |
| [0078](ADR-0078.md) | Backpressure session pacing | current (opt-in, not default) | yes | commit msg | Host rate floats on the live TX backlog. |
| [0079](ADR-0079.md) | Exit-echo state assist | current | yes | commit msg | Write the lost echo's state byte so the game runs its own exit. |
| [0080](ADR-0080.md) | Media Engine bring-up; video staging (a-e) | current | yes | commit msg + handover | Second-core module, mailbox, fallback; Stage-1 copy offload. |
| [0081](ADR-0081.md) | Make lost inputs visible and countable | current (tooling) | yes | commit msg | Arrival-gap timing and an identical-input drop test. |
| [0082](ADR-0082.md) | Present at loop top; cached ME read | current in code | yes | commit msg | GPU draws during the core run; latency stays one frame. |

## Known citation errors

- The control-file rename (`autopilot.ini` to `.gpsp-harness.ini`) is
  ADR-0036 (mainline), but several later texts call it "the ADR-0067
  rename". See [ADR-0067](ADR-0067.md) for the evidence.
- Some code comments describing these decisions are stale; each record's
  Notes section lists the ones found.
