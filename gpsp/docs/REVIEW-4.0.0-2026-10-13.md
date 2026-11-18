# GBAdhoc 4.0.0 review and local candidate

Review date: 13 October 2026. Public repository: https://github.com/ShoshinFauteux/GBAdhoc. Baseline: tag `v4.0.0`, commit `b5eeb238ae8ce1090cf78ae5f6253460897b4eb9`. Local branch: `review/4.0.0-safe-gba-cheats`. No upstream push or release publication was performed.

This review found ten actionable findings below. Five persistence findings are addressed in the local candidate. The remaining clock, network and GB compatibility findings are recorded with executable reproductions; they are not silently described as fixed. Passing existing tests preserved behavior that includes several older defects.

## Findings, ordered by impact

| ID | Priority | Trigger and observed result | Evidence | Candidate |
|---|---|---|---|---|
| F1 | P1 | A short write while replacing a save-state slot truncates the previously good slot. The operation returns failure after destroying it. | Actual `fe_host_state_save`, injected short stdio write; original slot no longer intact. | Fixed: write/close temporary snapshot before replacing current slot; rollback/recovery. |
| F2 | P1 | A deferred `fclose` error is ignored by state save, so the UI can report success for a failed write. | Actual production function; injected close error returns 0 on baseline, -1 on candidate. | Fixed; previous slot retained. |
| F6 | P1 | Rewind recording plus fast-forward and frequent RTC polling ratchets the cartridge clock ahead. Ten simulated wall seconds at 8x produces 80 RTC seconds even with snapshot sync every six frames. | Actual `gba_memory.c`, ASan/UBSan and PSP EBOOT in PPSSPP; ordinary mode gives ten seconds. | Open. Requires careful separation of forward wall time and deterministic replay. |
| F3 | P2 | Failed opening of a GB link save file leaks the allocated local merge buffer. Repeated failures consume memory. | Actual `gbl_commit`, tracked allocation remains outstanding after ordinary open failure. | Fixed: free buffer on this exit. |
| F5 | P2 | A state present only as `.old` cannot be opened through the normal state-load path after an interrupted replacement. | PPSSPP baseline: open failure; candidate: `state_recovered` followed by successful load. | Fixed: recover in load/save and slot/browser scans; deletion also removes backup. |
| F7 | P2 | GBA pair initialization installs the session clock before deserialization; a failed start and subsequent close leave `gpsp_rtc_frames_only=1` and the old wallclock callback unrestored. | Actual `fe_gbadual.c`, injected deserialize failure; host sanitizers and PSP EBOOT in PPSSPP. | Open. Restore clock ownership on every failed start and final close. |
| F8 | P2 | Joining accepts a WELCOME before validating its roster. A truncated or hostless roster leaves active session with zero peers and no timeout recovery. | Actual `netdrv.c`, CRC-valid framed input; after 30 simulated seconds `active=1 peers=0 started=1 stopped=0`, on host and PSP probe. | Open. Validate complete roster/host/self identity before changing state or firing callbacks. |
| F9 | P2 | SGB MASK_EN mode 3 requests palette colour 0 but `gb_fill_vframe` ignores its colour argument and clears black. | Actual SGB I/O packet and generated guest ROM under host sanitizers: white requested, black returned. | Open; inherited core issue. Convert fill colour using the normal output-format path. |
| F10 | P2 | GB NR52 square-channel status incorrectly depends on channel-3 wave volume; register-side length timing never advances. | Generated guest ROM in host and baseline PPSSPP: C000–C002 = 80,81,81. Expected channel-1 status bits = 1,1,0. | Open; inherited core behavior. Fix channel-active state and headless/audio timing together. |
| F4 | P3 | State loading reads only the maximum buffer and silently ignores trailing file bytes. An otherwise valid snapshot plus one extra byte is accepted. | Actual function plus baseline PPSSPP success; candidate rejects with `io_or_size`. | Fixed: check EOF, read error and close error before deserialization. |

Priority describes practical urgency, not exploitability. F6 is a controlled RTC workload rather than a demonstrated commercial-game save corruption. F7 deliberately injects core-load failure; real-world frequency is unknown. F8 uses an isolated transport probe, not a live attack or two-console radio test. F9's PSP visual control also showed black for mask-off, so the PSP screenshot cannot isolate that defect; the host packet/guest-ROM comparison does. F10 is guest-visible incorrect status; no commercial title was asserted to hang without a title-specific reproduction.

## Source paths and further evidence

- Persistence: `frontend-common/fe_host.c`, especially state save/load and `gbl_commit`; `psp/ui_psp.c` state slot scan and deletion. New regression: `tools/tests/test_review_persistence.c`.
- RTC: `gba_memory.c` `rtc_current_time` and `gpsp_rewind_clock_sync`; `frontend-common/fe_host.c` `rw_gather`; default rewind clock configuration in `psp/main_psp.c`.
- Link clock cleanup: `frontend-common/fe_gbadual.c` `m_start`, `m_stop`, `fe_gbadual_close`; `frontend-common/fe_gblink.c` `start_pair_m` and destruction ownership.
- Network: `netdrv/netdrv.c` `client_on_welcome` (baseline line 1097), `roster_apply` (939), and active-peer keepalive handling.
- SGB: `gbcore/tgbdual/gb.c:46–54`, `sgb.c:874–881`.
- APU: `gbcore/tgbdual/apu.c:63–99`, synthesis state restoration around 825–826, and `LOCAL-PATCHES.md` discussion of headless parity.

Workspace evidence paths below are relative to the parent of this repository:

- `audit/baseline/persistence-baseline.log` and `audit/reproduce_baseline_persistence.py`.
- `audit/candidate/persistence-cheats-tests.log`, `host-tests-complete.json`, `host-tests-complete.log`.
- `audit/rewind-review.txt`, `audit/candidate/review-rewind-rtc.log`, `review-link-clock.log`, and `audit/candidate/rewind-psp/` with actual PSP probe binaries, build logs, results and hashes.
- `GBAdhoc/audit/network/` with source, host result, PSP result and build log.
- `GBAdhoc/audit/core/REVIEW.md`, `results.txt`, reproducible original ROM generators and core probes.
- `audit/emulator/` contains the isolated PPSSPP runners; `results/` holds per-run configuration, inputs, logs, state hashes, binary hashes and results.

## Safe GameShark / Action Replay implementation

The local branch adds an explicit, bounded GBA RAM subset. It does not claim full compatibility with every command supported by the original devices. See [CHEATS.md](CHEATS.md) for the supported matrix and examples.

- `GS1:` / `GS1RAW:` select encrypted/decrypted GameShark v1/v2. `AR3:` / `AR3RAW:` select encrypted/decrypted Action Replay v3.
- Supports validated work-RAM writes, selected comparisons, AR3 bounded fills and per-entry DEADFACE reseeding. Ambiguous bare 8+8 codes require a family prefix.
- Unsupported hooks, ROM patches, pointer/block operations and other unsupported commands reject the entire entry. No partial application. Invalid address, width, alignment, record or work limits reject compilation.
- Shared worst-case budget is 4096 RAM accesses per frame across enabled GBA entries, including existing CodeBreaker operations. No per-frame decryption or unbounded interpretation.
- Existing default-off, `.on` identity, link suspension, translated-RAM invalidation and rewind barriers are preserved. Loading a save state does not itself execute a cheat.
- mGBA cipher/table provenance is pinned to `143adf5aa2a60c6036810b0cd72445e13d74b0e6`; full MPL-2.0 text is included in `licenses/`. New cipher header preserves attribution. The public source and local package contain the corresponding source.

Host verification includes frozen encrypted/decrypted vectors; every supported condition/width; reseed isolation; whole-operation skip behavior; pool/work limits; rejection/default-off/persistence; rewind generation and RAM flushes; 150,000 fuzz rounds. An independent probe additionally exhausts all 131,072 family/reseed combinations and tests cipher round trips, mirror boundaries and nested skips. ASan/UBSan reported no errors in these passing cases.

## Emulator and build results

The original release source was built separately from the candidate. PPSSPP was a local SDL build (revision `d90fdee8fcf7f1a1d9a154cf18441233eb65c8ca`) under Ubuntu WSL2/Xvfb. Each run used its own temporary memory stick and configuration. Existing rigs and the user's original project were left intact. The owner's local Sapphire Rev 2 copy was used privately and is not in deliverable packages.

| Check | Result |
|---|---|
| All registered host suites | **30 passed, 0 failed, 0 skipped** after recovering original historical fixtures and caching PSP headers. |
| Candidate official harness build, GBA_LINK=1 REWIND=1 | Passed build checks; SHA256 `9e35c55efbeffd94875462fa8e6cd53d0159fa2137d0d7d5060df49d48d720cb`. |
| Candidate official release build, same features | Passed build checks; SHA256 `2b2cd8d15fbaca2b38178a5b44d7586b64fdb7d51d0dcbe4b8f5f98461bee764`. |
| Encrypted GS1 + AR3 enabled in PSP frontend | Two usable/enabled entries; EWRAM test values read back as `12345678` and `89abcdef`. |
| Same file without `.on` | Two usable entries, zero enabled, RAM test words stay zero. |
| Valid write followed by unsupported command | Whole entry unusable/disabled; first write does not run. |
| Baseline vs candidate no-file/off/rejected/recovered-state | All **271 post-load frame hashes** match (registers, PC, work RAM, I/O, palette, OAM, VRAM, audio hash/count). Timing field `c=` excluded as documented. |
| Enabled-cheat negative control | Differs in all 271 frames beginning at frame 31, as expected. |
| State write/load and backup recovery | Candidate normal write/load succeeds; `.old` recovery succeeds; appended-byte state rejected. |
| Network and two clock probes | Their reported baseline defects reproduce as PSP EBOOTs in PPSSPP. |
| GB NR52 fixture | Baseline GBAdhoc runs the original test ROM and logs the same incorrect status bytes as the host. |

The 271-frame comparison starts after the harness's frame-30 autoload; pre-load startup is not a valid identity oracle. The `.on` test words are test writes in a real running game, not evidence that every published cheat works with every game revision.

Public-test reproducibility issue: two historical commits needed by the test harness are absent from this public snapshot. The first public-checkout run had 26 passes, one missing-baseline failure and three skips. Original historical fixtures were recovered read-only from the existing local development repository; `audit/run_complete_host.py` supplies them without changing suite commands/assertions. A fresh public clone alone cannot reproduce that full run until those baseline fixtures are distributed or the comparisons are redesigned.

Official build manifests are retained in `audit/candidate/{harness,release}` under this repository. Both baseline and candidate emit the pre-existing 7008-byte hot-core layout warning against a 6144-byte budget. It is a hardware performance question, not a new candidate-only build failure. Harness static image grows by 24,576 bytes; candidate release boot heap estimate is 10,474,936 bytes on PSP-1000 and 38,786,488 on PSP Go.

## Coverage and limits

Reviewed state persistence, cheat parse/compile/apply, file identity and UI integration, rewind clock/barrier/restore paths, GBA pair start/stop ownership, network handshake/rosters, GB RTC/controls/APU/SGB, and GE layout/filter/audio output boundaries. Ran the full registered suite spanning SMC, DMA, ROM loading, serialization, RTC, SRAM, controls, save sharing, net/RFU, GB parity and frontend buffers. Independent reviewers handled overlapping high-risk changes and separate core areas.

This is a broad executable review, not a proof that all C/assembly, games or timings are correct. Unresolved hypotheses are separated in `audit/rewind-review.txt`: the process-wide RTC monotonic floor across ROM changes, and the 64-entry rewind clock-anchor ring across timeline forks. Neither is counted as a confirmed extra defect.

Physical PSP validation remains necessary for ME execution, cache behavior, FAT power loss, sleep/resume, radio sessions, frame pacing and performance. PPSSPP did not execute the PSP Media Engine helper. Stdio close plus rename improves failure recovery but does not establish power-loss durability on a real Memory Stick. State thumbnails remain separate from the snapshot transaction. GE VRAM dumps require PPSSPP software rendering; Vulkan dumps can retain an old startup framebuffer, so those images are not used as gameplay-rendering proof.

## LCD filter and next releases

**An LCD-style filter is feasible.** Existing GBA/GBC colour correction and sharp bilinear scaling provide the base. A restrained repeating pixel-grid/subpixel mask can use a small fixed-function GU texture and blend pass after game scaling, before OSD. PSP has no programmable shaders; this should use the existing GE path, not a CPU postprocess.

The current triple-buffered layout reserves 1,712,128 of 2,097,152 VRAM bytes, leaving **385,024 bytes** before new alignment. Optional ghosting with a dedicated 256x256 RGB565 history texture costs 131,072 bytes. Do not treat an arbitrary old display buffer as temporal history. Fit scaling can cause moire; provide low intensity and integer-scale presets. Benchmark PSP-1000 and later models with sharp filtering, CPU/ME, fast-forward and pause transitions before promising a frame-time cost.

Recommended sequence of future work:

1. **Reliability/clock release:** land tested save recovery, resolve F6/F7, then validate RTC games through fast-forward, rewind, state load and restart. Highest immediate player value.
2. **GB accuracy release:** repair SGB fill and real APU channel-active timing, add conformance ROMs alongside parity tests; verify audible and headless linked cores together.
3. **LCD presets:** DMG grid, restrained GBC/GBA mask and optional subtle ghosting, measured against a PSP-1000 frame-time budget.
4. **ROM-identified cheat profiles:** show format and supported status, bind profiles to cartridge identity/revision, and explain a rejected entry before activation. Extend formats only when each new operation has semantic tests.
5. **GB/GBC rewind:** first measure state size/history depth on PSP-1000; reuse the existing input/history contract where appropriate, including RTC and sound timing.
6. **Useful diagnostics export:** one opt-in local bundle with build identity, game checksum, relevant settings and recent events for a bug report; omit saves/ROM contents by default. This directly improves reproducible compatibility reports.

Avoid combining speculative accuracy, renderer and networking rewrites into the same first candidate. These findings have separate determinism and hardware validation needs.
