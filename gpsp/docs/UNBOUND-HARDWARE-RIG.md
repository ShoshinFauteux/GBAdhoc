# Unbound / Heart & Soul hardware performance rig

The accepted performance result and the rejected paths are recorded in
[UNBOUND-PERFORMANCE.md](UNBOUND-PERFORMANCE.md).

## Baseline and scope (2026-09-15)

The hardware-accepted playable fallback is commit `add0f4d`, tag
`candidate/2026-09-15`. Its exact binaries are in
`releases/candidate-2026-09-15/`; see `RELEASE-CANDIDATE-2026-09-15.md`.
The three regular installs are named **GBAdhoc**. Performance work is isolated
on `codex/unbound-hardware-performance` and in **GBAdhoc Perf** on the 32 GB
PSP-1000. Never stage experiments over the regular install.

The goal is an autonomous hardware cycle: collect telemetry, diagnose it,
build a candidate, enqueue it, stage during USB handoff, relaunch, and compare
both games. The PC collector executes published jobs; the coding agent uses
their results to choose and implement the next candidate. An empty queue parks
the PSP while the agent analyses/builds. Only the first launch is manual.

## Existing harness contracts retained

Read the workspace's `harness-kit/HARNESS.md`, `docs/AUTOPILOT.md`, and the
comments in `psp/usb_handoff.c`. Some old prose is superseded by current code:

- `.gpsp-harness.ini` is the control channel. `autopilot.ini` is ignored.
- Stock `GPSP_PLAYABLE` builds deliberately ignore the harness. This build uses
  `GPSP_PLAYABLE`, `VID_TRIPLE`, `GPSP_KEEP_TELEMETRY`, and `GPSP_PERF_RIG` together:
  playable defaults, explicitly enabled automation, and telemetry.
- All core, ME, network and I/O activity is stopped and logs closed before USB
  export. No PSP filesystem access is allowed while the PC owns the volume.
- USB export uses a **fixed window**. Windows eject is not a reliable signal
  to shorten it. After the window the PSP releases USB, reads `CMD.TXT`, and
  invokes `sctrlKernelLoadExecVSHMs2`, retaining the existing fallback.
- `handoff_park_s=0` parks indefinitely; `handoff_max_runs=0` means the default
  cap, not infinity. This campaign uses 100000.
- Holding Start+Select during a run aborts cleanly; holding it about 1.5 s
  while parked leaves the loop. The normal menu is suppressed by the script.

### Fresh-window staging

Before exporting, the PSP now writes `handoff/WINDOW.TXT` with a token
`<run>-<window>` and window duration. The collector must observe a new token;
if started mid-window it waits for the next one. It reserves 15 seconds for
enumeration and another 20–40 seconds for staging/flush margins. This avoids
starting a normal copy near a known closing window; an indefinitely stalled
host/device I/O operation cannot be made safe by a wall-clock estimate alone.

`RESULT.TXT` is retained and journaled by run number plus job ID. A later
window for that result is not another test. The PSP owns `RUNS.TXT`.

## Fixtures and workload

Golden ROMs, BIOS, `.sav` files and slot-0 states are private local fixtures in
`../builds/unbound-harness/fixtures/`, with a hash manifest. They are not Git
assets. The states were copied before device consolidation:

| Fixture | Slot-0 SHA256 |
| --- | --- |
| Pokemon Unbound | `af83e9935e53af11acb2454a64a283e84880a2c1e379adf3a93f2cb992f3ecd9` |
| Pokemon Heart & Soul | `316906e438e13c626cb2316fe011c64f5e3709fa54b47eb3db6c90e59e022ad9` |

Both states start in the dialogue immediately before their battles. The script
advances into the fights. Each run restores the golden save/state, boots the
explicit ROM, loads slot 0 at frontend frame 30, and follows `battle.inputs`:

- Wait 90 frames, take a scene snapshot, wait another 210 frames.
- Repeat 450 times: press A for two frames, release for two, wait four.
- Take the ending snapshot and wait 30 frames; script finishes at frame 3931.

Measure frames **301–3900**: 3,600 frames, twelve 300-frame windows. Startup,
state loading and screenshots are outside that range. This is a reproducible
scripted battle workload, not an assertion that an entire battle was won.
No fast-forward, music suppression, or new frameskip policy is enabled.

Hard limits are 4500 frontend frames and 300 seconds from harness configuration.
The wall timeout is checked between frames; a hard CPU/firmware wedge can still
require human recovery. Missing/failed states exit with `state_load_failed`,
never silently benchmark the title screen.

## Measurement and promotion rules

`psp/perf_rig.h` records elapsed time through presentation/pacing, work time
before pacing, maxima, over-budget work frames, and eight wall-time bins with
edges 16740, 20000, 25000, 33400, 50000, 75000 and 100000 microseconds. Bins use
an inclusive lower edge and exclusive upper edge. Emulated FPS is
`sum(frames) * 1e6 / sum(wall_us)`, never an average of window averages.

Detailed `core_phase=2` runs diagnose guest CPU, rendering and sound work.
They are **not performance scores**: instrumentation has a known cost. Scoring
runs use `core_phase=0`. Existing core/SMC/ME telemetry is retained; the final
`audio_hash` remains disabled with normal `bench_mode=0` and is not an oracle.

The initial queue alternates Unbound/H&S three times, then profiles each once.
Compare each game against its own repeated baseline. Later comparisons must
interleave baseline/candidate, change one variable at a time, exceed observed
run variance, and preserve H&S performance and correctness. Inspect snapshots,
ME drops/fallbacks, timing tails and relevant diagnostic counters as well as FPS.
Check the eventual telemetry-free playable build on hardware before promotion.

Do not reintroduce gate eviction or arbitrary data addresses as translation
entry points. `cpu_threaded.c` documents crashes from both; savestate clock
rewind also broke the old eviction age calculation. Preserve ranked add-only
gates, PC masking and bad-jump safeguards while measuring the first baseline.
No core performance changes are part of this harness checkpoint.

## Host tools and campaign

Python 3.11+, Windows PowerShell/`Write-VolumeCache`, and the pinned PSP Docker
image are required. The active campaign is `../builds/unbound-harness/campaign/`:

- `campaign.json`: exact device path and random `RIG.ID` marker.
- `jobs/<id>/job.json`: immutable package hashes, fixture identity and window.
- `queue.json`: atomically published ordered job directories.
- `journal.json`, `runs/<run>-<id>/`: dispatch history, logs, screenshots,
  post-run saves/states, and summaries.
- `STOP`: creating this local file stops future dispatch; the current run can
  finish and park. Invalid runs create `HALTED.json` and stop the collector.

Run `python tools/perf_loop.py --campaign <campaign>` detached/hidden on Windows.
It waits for genuine handoff; it does not launch a PSP from the XMB. It refuses
other directories/devices, missing payloads, hash mismatches, incomplete samples,
failed autoloads, missing script completion/snapshots, and ME fallback. Packages
are verified before and after copying. Payloads are flushed before publishing
RUN; RUN is flushed before eject. Any staging error stops further dispatch.

To prepare subsequent candidates from this feature branch:

```text
python tools/build_perf.py --output <new-build-directory> --version <candidate-id> --core-option SMC_GATES_CLUSTER=1
python tools/make_perf_job.py --campaign <campaign> --template <baseline-unbound-job> --id <candidate-unbound-id> --build <new-build-directory> --enqueue
python tools/make_perf_job.py --campaign <campaign> --template <baseline-heart-soul-job> --id <candidate-heart-soul-id> --build <new-build-directory> --enqueue
```

Use fresh IDs for baseline repeats too. Include newly added source files in the
Git index before building; the build snapshots indexed paths and working bytes,
records hashes/diff, cleans both core/frontend, and pins the Docker digest.
`--core-option` is repeatable and accepts only `NAME=unsigned-integer`; the
result manifest records every option. Omit `--build` from `make_perf_job.py` to
repeat a template's exact executable for an interleaved baseline.
Use `--audio-oracle` for a diagnostic-only generated-audio checksum, and pass
`--smc-watch 0x03006200` (with the `0x` prefix) to move the detailed SMC probe
without manually editing a published job. Both options disable performance
scoring for that job.
The correct ME artifact is `psp/me/gbadhoc_me.prx`, not the stale top-level copy.
Use one queue writer. Do not edit a published job or a golden fixture.

## Validation before the first hardware run

- Pinned PSP build succeeded; ME PRX hash matches the accepted candidate.
- Host C checks cover measurement boundaries, totals, histogram edges and timeout.
- Python tests cover scoring rejection, failed copy/flush, wrong device/path/hash,
  late-window refusal, and a complete collect → stage → relaunch → park cycle.
- Both actual user states completed 3,600 frames and reached their battle scenes
  in PPSSPP with the CPU renderer. No emulator FPS is used as PSP evidence.
- PPSSPP lifecycle test covered detailed profiling, parking across windows,
  three relaunches, failed state loading and timeout returning to handoff. Its
  relaunch uses the existing plain-load fallback; native CFW and physical USB
  remount/cache behaviour still need the first PSP run.

Audit outputs live under `../builds/unbound-harness/`. The three cleanup archives
and per-file verification manifests are under `../builds/candidate-2026-09-15/`.

## Campaign result (2026-09-15)

The PSP-1000 campaign isolated Unbound's main measured cost to repeated
translation of its self-modifying M4A audio mixer in IWRAM. During the fixed
3,600-frame battle workload, Unbound translated 426,803 instructions while
repeatedly patching the `0x03006200` page. The hottest target was `0x03006220`;
the block-store writer at `0x030061e4` also touches the surrounding words.

The promoted change keeps the existing ranked, add-only gate table and the
same translated-block boundaries. It adds a one-bit-per-RAM-halfword lookup
map so `scan_block` can test a candidate PC directly instead of comparing it
with every live gate. The map consumes 18 KiB: 2 KiB for IWRAM and 16 KiB for
EWRAM. It is rebuilt after cartridge-specific gates are loaded and whenever
the optional legacy eviction path replaces a gate. Builds enable it explicitly
with `SMC_GATE_BITMAP=1`; builds without that option retain the linear lookup.

PSP-1000 scoring results:

| Build / fixture | Runs | Mean emulated FPS | Mean work/frame | Over-budget frames |
| --- | ---: | ---: | ---: | ---: |
| Accepted baseline / Unbound | 5 | 40.8685 | 22,798.85 us | 3,424.4 |
| Gate bitmap / Unbound | 2 | 41.7878 | 22,260.89 us | 3,386.5 |
| Accepted baseline / Heart & Soul | 1 paired guard | 59.7959 | 6,289.79 us | 61 |
| Gate bitmap / Heart & Soul | 1 | 59.7958 | 6,273.97 us | 60 |

The Unbound gain is 2.249% in emulated FPS and a 2.360% reduction in measured
work per frame. Its two candidate runs were 41.7856 and 41.7901 FPS, well
outside the baseline range of 40.8504-40.8953 FPS. Heart & Soul remained
locked at its 60 FPS ceiling.

The final source snapshot passed both 3,600-frame PPSSPP correctness oracles.
Heart & Soul retained audio hash `0d32030f` (2,156,387 samples) and frame hashes
`b70cc26a...` / `19248f4a...`; Unbound retained audio hash `25146ed5`
(2,156,388 samples) and frame hashes `d6331c6c...` / `ce1a073d...`. These are
correctness checks only; the performance numbers above come from the PSP-1000.

Rejected candidates remain useful negative evidence:

- Gate clustering preserved Heart & Soul but reduced Unbound to 39.9376 FPS.
- Partial invalidation variants changed Heart & Soul audio despite matching
  video, including versions that scanned every dirty byte.
- Skipping apparently unchanged self-modifying writes caused major Heart &
  Soul divergence.
- Expanding the gate table and comparing complete register lists preserved
  output but did not improve Unbound.
- Selecting a bitmap once per translated block regressed Unbound to 40.8353
  FPS and was removed.

Do not revive partial invalidation, gate movement, or new translation entry
points without new correctness evidence. The measured win comes from making
the existing gate-membership question cheaper, without changing its answer.
