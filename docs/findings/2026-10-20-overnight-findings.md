# Overnight findings, 2026-10-19/20: KarnFu fixtures + perf campaign

Durable record of what the 47-fixture bench suite (built around KarnFu's issue #9 save states) taught us on the first night, plus the morning report. Nothing here was pushed or merged. ROMs, saves and states are NOT in this file; they stay local in GBAdhoc-fixtures/bench/store.

Copies: GBAdhoc-fixtures/bench/reports/ (OneDrive), GBAdhoc-Lab docs/findings/ (git, with raw data in docs/findings/data/), gpsp branch claude/bench-dr docs/findings/ (git).

## 1. Headline findings
1. **The heavy KarnFu titles are slow because of self-modifying code.** Doom II, Duke Nukem Advance and Smashing Drive patch one fixed word of their own IWRAM render loop with a single ARM `STR` 12-22 times per frame. 4.1.1 answers every such write with a FULL RAM translation-cache flush (5-22 flushes/frame); Doom retranslates ~300 blocks/frame, about 32 ms of JIT per frame. 4.1.1's `smc_writer_safe_range` only admits the M4A mixer's STMIA-via-LR pattern; every other SMC trap falls back to a full flush.
2. **Fix: `SMC_PARTIAL_SINGLE` (claude/bench-dr 82f099a, default off).** A single ARM STR/STRB from IWRAM to IWRAM retires only the blocks covering the written word (`smc_retire_single_word`, via the STABLE_THUNK path), no gate needed. PPSSPP: translation work -75% Doom, -82% Duke, -70% Smashing, -78% NFS MW, -80% NFS Carbon, -84% Ford Racing 3, -32% SSX 3; Doom RAM flushes 13,754 -> 104. 21/26 bench-ready fixtures frame-identical to 4.1.1 (Doom and Ford included); 5 (Duke, Carbon, MW, Smashing, SSX) differ deterministically with 0 stale-code hits and look identical (timer 0.01 s apart, blink phase) = layout timing. **Not yet run on a PSP** (build GBAdhoc/builds/bench-dr/dr2-prx).
3. **Translation cost: XLAT_FAST (claude/bench-xlat 56c55e2, default off)**, six exact pieces (LIMITS, GATEMAP, EXITPF, RAMEND, DCLEAR, SCANFN; `XLAT_NO=` switches pieces off). PPSSPP vhash 26/26 identical, corpus oracle 35/35 with all 4 mutants caught, twin translation instructions -13..-18% on every heavy title. Hardware so far only LIMITS: Doom 18.50 -> 19.25 fps (+4.1%), Duke 17.18 -> 17.81 (+3.7%) on the PSP-1000.
   - Hardware split (diag build, Doom, PSP-1000): JIT 34.9 ms/frame, of which ARM scan_block 10.1 ms (29%). jit_code_publish costs 2.1 ms/frame OUTSIDE the jit phase (285 publishes, ~7.4 us each: kernel D-writeback + I-invalidate) = the next lever. Translator ~670 cycles per guest instruction (CPI ~3.2).
   - Compiling the translator at O2/Os gave no gain (Os worse: Doom 17.83 fps). Instruction count, not I-cache footprint, sets translator time, so twin counts are a good guide.
4. **Low latency (claude/low-latency, gpsp/docs/LOW-LATENCY.md).** The ME path shows a frame 4 vblanks after its pad poll; SetFrameBuf NEXTFRAME latches one vblank late. Tier 0 (flip_early + me_early_retire) = 2.0-2.5 vblanks with fps/work unchanged on 7 fixtures (PSP-1000, hardware-verified). `input_fresh` removes ~16 ms pad-sample age (true on real firmware too) for ~0.7 ms/frame. Tier 1 (me_lowlat, same-frame) unverified; the PSP-1000 dropped off USB at the exit of the one tier-1 nfsu2 run (cause not determined; battery plausible).
5. **PRs #15/#16 (claude/pr-review 56f35a2):** both recommended for the beta. One fix to #15 (a GB save write raced the sleep park ack); #16 rebased onto #15. Release build 0 warnings, host tests 30/30, PPSSPP smoke+gb vhash = 4.1.1. Sleep needs the owner's hardware test.
6. **Both PSPs fell off USB after hours on PSPLink** (3000 ~midnight, 1000 ~02:30). Flat batteries likely (the 1000 cannot charge over USB). This is the motivation for the servo rig and for the GBAdhoc Lab harness replacing PSPLink.
7. **Open question:** STALE_CHECK flags 13,853 stale-block reuses on Mario Golf in UNMODIFIED 4.1.1 (8 with dr2). Either checker false positives (data inside a block extent) or a pre-existing stale-code bug in the mixer path. Investigate.
8. **PSP-1000 and PSP-3000 perform within ~1%** on every fixture of the baseline (table below).


## 2. 4.1.1 hardware baseline (PSPLink harness PRX, both consoles)
Columns in ms except fps. Source: bench/runs/b411-phases.json (raw runs in bench/psplink-stage/runs/b411-3000 and b411-1000).

| Fixture | Console | fps | frame | work | cpu | jit | video | audio |
|---|---|---|---|---|---|---|---|---|
| kf_duke_nukem_advance | 3000 | 17.35 | 57.62 | 56.85 | 20.90 | 34.16 | 1.63 | 0.14 |
| kf_duke_nukem_advance | 1000 | 17.18 | 58.18 | 57.39 | 21.22 | 34.41 | 1.60 | 0.14 |
| kf_doom_ii | 3000 | 18.77 | 53.25 | 52.68 | 18.95 | 32.17 | 1.40 | 0.14 |
| kf_doom_ii | 1000 | 18.50 | 54.02 | 53.49 | 19.52 | 32.42 | 1.39 | 0.14 |
| kf_smashing_drive | 3000 | 30.42 | 32.38 | 33.08 | 15.72 | 15.84 | 1.36 | 0.14 |
| kf_smashing_drive | 1000 | 30.22 | 32.58 | 33.30 | 15.83 | 15.96 | 1.34 | 0.14 |
| kf_ssx_3 | 3000 | 36.61 | 27.23 | 27.99 | 12.42 | 13.89 | 1.52 | 0.13 |
| kf_ssx_3 | 1000 | 36.48 | 27.34 | 28.11 | 12.53 | 13.92 | 1.50 | 0.13 |
| kf_need_for_speed_most_wanted | 3000 | 38.11 | 26.21 | 25.92 | 13.35 | 10.55 | 1.87 | 0.13 |
| kf_need_for_speed_most_wanted | 1000 | 37.97 | 26.31 | 26.02 | 13.43 | 10.56 | 1.87 | 0.13 |
| kf_need_for_speed_carbon_own_the_city | 3000 | 43.30 | 23.06 | 22.80 | 12.42 | 8.18 | 2.04 | 0.13 |
| kf_need_for_speed_carbon_own_the_city | 1000 | 43.13 | 23.16 | 22.89 | 12.52 | 8.17 | 2.04 | 0.14 |
| kf_hot_wheels_stunt_track_challenge | 3000 | 43.82 | 22.79 | 22.56 | 12.11 | 7.32 | 2.97 | 0.13 |
| kf_hot_wheels_stunt_track_challenge | 1000 | 43.62 | 22.89 | 22.67 | 12.15 | 7.37 | 2.99 | 0.13 |
| kf_top_gear_rally | 3000 | 50.16 | 17.83 | 18.39 | 11.83 | 5.42 | 0.99 | 0.14 |
| kf_top_gear_rally | 1000 | 50.16 | 17.81 | 18.39 | 11.84 | 5.41 | 0.97 | 0.13 |
| kf_max_payne | 3000 | 51.90 | 19.24 | 18.96 | 10.63 | 7.30 | 0.87 | 0.14 |
| kf_max_payne | 1000 | 51.01 | 19.57 | 19.34 | 11.02 | 7.27 | 0.88 | 0.13 |
| kf_lego_drone_racers | 3000 | 51.11 | 17.87 | 16.29 | 8.11 | 5.71 | 2.32 | 0.13 |
| kf_lego_drone_racers | 1000 | 51.12 | 17.87 | 16.29 | 8.06 | 5.74 | 2.33 | 0.13 |
| kf_ford_racing_3 | 3000 | 56.42 | 17.67 | 17.51 | 9.79 | 6.27 | 1.30 | 0.13 |
| kf_ford_racing_3 | 1000 | 56.51 | 17.65 | 17.48 | 9.78 | 6.25 | 1.29 | 0.13 |
| kf_mario_tennis_power_tour | 3000 | 59.09 | 16.40 | 16.36 | 8.35 | 6.96 | 0.67 | 0.33 |
| kf_mario_tennis_power_tour | 1000 | 58.99 | 16.48 | 16.50 | 8.50 | 6.95 | 0.68 | 0.33 |
| ub_rival_high | 3000 | 59.79 | 14.69 | 14.59 | 12.22 | 1.08 | 0.94 | 0.32 |
| ub_rival_high | 1000 | 59.46 | 14.64 | 14.65 | 12.22 | 1.23 | 0.85 | 0.32 |
| hns_heavy | 3000 | 59.76 | 9.25 | 8.97 | 6.54 | 1.38 | 0.69 | 0.34 |
| hns_heavy | 1000 | 59.49 | 9.52 | 9.29 | 6.68 | 1.55 | 0.69 | 0.34 |
| hns_light | 3000 | 59.76 | 5.24 | 5.08 | 3.58 | 0.32 | 0.67 | 0.48 |
| hns_light | 1000 | 59.59 | 5.54 | 5.39 | 3.72 | 0.49 | 0.67 | 0.48 |
| em_battle | 3000 | 59.74 | 5.63 | 5.55 | 4.34 | 0.12 | 0.65 | 0.40 |
| em_battle | 1000 | 59.74 | 5.80 | 5.69 | 4.43 | 0.17 | 0.66 | 0.39 |
| aw2_battle | 3000 | 59.76 | 4.70 | 4.55 | 3.66 | 0.02 | 0.62 | 0.23 |
| aw2_battle | 1000 | 59.76 | 4.78 | 4.63 | 3.74 | 0.02 | 0.62 | 0.23 |
| d2_drive | 3000 | 59.76 | 12.38 | 8.76 | 6.79 | 0.03 | 1.77 | 0.14 |
| d2_drive | 1000 | 59.77 | 12.49 | 8.86 | 6.85 | 0.03 | 1.81 | 0.14 |
| own_aw2 | 3000 | 59.76 | 4.53 | 4.40 | 3.48 | 0.01 | 0.60 | 0.28 |
| own_aw2 | 1000 | 59.76 | 4.54 | 4.40 | 3.48 | 0.01 | 0.60 | 0.28 |
| own_fe8 | 3000 | 59.76 | 4.58 | 4.42 | 3.40 | 0.01 | 0.65 | 0.33 |
| own_fe8 | 1000 | 59.76 | 4.63 | 4.48 | 3.44 | 0.02 | 0.66 | 0.33 |
| ub_double_high | 3000 | 59.76 | 9.13 | 8.99 | 7.20 | 0.76 | 0.72 | 0.28 |
| ub_double_high | 1000 | 59.80 | 9.30 | 9.41 | 7.51 | 0.91 | 0.69 | 0.28 |
| ub_double_medium | 3000 | 59.76 | 7.89 | 7.76 | 5.80 | 0.95 | 0.68 | 0.30 |
| ub_double_medium | 1000 | 59.80 | 8.14 | 8.26 | 6.15 | 1.11 | 0.68 | 0.30 |
| ub_overworld | 3000 | 59.76 | 8.38 | 7.57 | 5.79 | 0.72 | 0.70 | 0.33 |
| ub_overworld | 1000 | 59.76 | 8.61 | 8.20 | 6.21 | 0.95 | 0.68 | 0.33 |
| ub_rival_medium | 3000 | 59.82 | 12.77 | 12.73 | 10.32 | 1.10 | 0.96 | 0.32 |
| ub_rival_medium | 1000 | 59.76 | 13.05 | 13.15 | 10.79 | 1.24 | 0.78 | 0.32 |
| nfsu2_race | 3000 | 59.77 | 9.48 | 6.76 | 5.34 | 0.05 | 1.21 | 0.14 |
| nfsu2_race | 1000 | 59.78 | 9.59 | 6.85 | 5.42 | 0.04 | 1.22 | 0.13 |
| kf_mario_golf_advance_tour | 3000 | 59.82 | 11.26 | 10.68 | 5.77 | 4.07 | 0.66 | 0.14 |
| kf_mario_golf_advance_tour | 1000 | 59.82 | 11.18 | 10.64 | 5.74 | 4.07 | 0.66 | 0.14 |

## 3. SMC fallback counters with dr2 (PPSSPP, 1200 frames)
`smcfb` index: [0] not-safe, [1] activation, [2] gate changed, [3] not gated, [4] range bounds, [5] final word untagged, [6] domain end, [7] no extent/single-path fail, [8] retired-tag table full, [9] partial done, [10] stale checks, [11] stale mismatches.

| Fixture | smcfb (dr2) |
|---|---|
| aw2_battle | 37/0/0/0/0/0/0/0/0/0/0/0 |
| d2_drive | 0/0/0/0/0/0/0/0/0/0/0/0 |
| em_battle | 0/0/0/0/0/0/0/0/0/0/0/0 |
| hns_heavy | 309/1/0/0/0/0/0/0/0/15312/2787928/0 |
| hns_light | 319/1/0/0/0/0/0/0/0/477/235181/0 |
| kf_doom_ii | 0/1/0/0/0/0/0/0/0/121822/2875198/0 |
| kf_duke_nukem_advance | 0/1/0/0/0/0/0/0/0/72542/3312219/0 |
| kf_ford_racing_3 | 0/1/0/0/0/0/0/0/0/6049/725447/0 |
| kf_hot_wheels_stunt_track_challenge | 774/0/0/0/0/0/0/0/0/0/0/0 |
| kf_lego_drone_racers | 335/0/0/0/0/0/0/0/0/0/0/0 |
| kf_mario_golf_advance_tour | 2179/2/1/0/0/0/0/0/0/2847/451228/8 |
| kf_mario_tennis_power_tour | 9139/1/1/0/0/0/0/0/0/2616/564014/0 |
| kf_max_payne | 1242/0/0/0/0/0/0/0/0/0/0/0 |
| kf_need_for_speed_carbon_own_the_city | 1/1/0/0/0/0/0/0/0/5677/871559/0 |
| kf_need_for_speed_most_wanted | 1/1/0/0/0/0/0/0/0/7935/1223073/0 |
| kf_smashing_drive | 0/1/0/0/0/0/0/0/0/21953/5063953/0 |
| kf_ssx_3 | 1343/1/0/0/0/0/0/0/0/7417/1078550/0 |
| kf_top_gear_rally | 2366/1/0/0/0/0/0/0/0/196/1221417/0 |
| nfsu2_race | 9/0/0/0/0/0/0/0/0/0/0/0 |
| own_aw2 | 18/0/0/0/0/0/0/0/0/0/0/0 |
| own_fe8 | 0/0/0/0/0/0/0/0/0/0/0/0 |
| ub_double_high | 1/1/0/0/0/0/0/0/0/8826/3296902/0 |
| ub_double_medium | 1/1/0/0/0/0/0/0/0/12590/1712968/0 |
| ub_overworld | 1/1/0/0/0/0/0/0/0/5252/600091/0 |
| ub_rival_high | 1/1/0/0/0/0/0/0/0/13632/5159228/0 |
| ub_rival_medium | 1/1/0/0/0/0/0/0/0/14222/3371546/0 |

## 4. KarnFu fixtures (issue #9, received 2026-10-16)
23 states. ROM revisions unconfirmed (no CRC32s yet). needs-script = saved on a title/menu screen, needs scripted input to reach gameplay; needs-input = needs live input to be meaningful.

| Fixture | Title | Status |
|---|---|---|
| kf_back_to_stone | Back To Stone | needs-script |
| kf_big_mutha_truckers | Big Mutha Truckers | needs-script |
| kf_doom_ii | Doom II | bench-ready |
| kf_duke_nukem_advance | Duke Nukem Advance | bench-ready |
| kf_fifa_07 | FIFA 07 | needs-script |
| kf_ford_racing_3 | Ford Racing 3 | bench-ready |
| kf_hot_wheels_stunt_track_challenge | Hot Wheels Stunt Track Challenge | bench-ready |
| kf_ice_nine | Ice Nine | needs-script |
| kf_kill_switch | Kill Switch | needs-script |
| kf_lego_drone_racers | Lego Drone Racers | bench-ready |
| kf_mario_golf_advance_tour | Mario Golf - Advance Tour | bench-ready |
| kf_mario_tennis_power_tour | Mario Tennis - Power Tour | bench-ready |
| kf_max_payne | Max Payne | bench-ready |
| kf_mortal_kombat_deadly_alliance | Mortal Kombat Deadly Alliance | needs-script |
| kf_need_for_speed_most_wanted | Need For Speed - Most Wanted | bench-ready |
| kf_need_for_speed_underground | Need For Speed - Underground | needs-script |
| kf_need_for_speed_carbon_own_the_city | Need For Speed Carbon - Own The City | bench-ready |
| kf_openlara_gba | Openlara GBA | needs-input |
| kf_payback | Payback | needs-script |
| kf_ssx_3 | SSX 3 | bench-ready |
| kf_smashing_drive | Smashing Drive | bench-ready |
| kf_street_racing_syndicate | Street Racing Syndicate | needs-script |
| kf_top_gear_rally | Top Gear Rally | bench-ready |

Next for these: script the 9 needs-script states into gameplay (Back To Stone, Big Mutha Truckers, FIFA 07, Ice Nine, Kill Switch, Mortal Kombat DA, NFS Underground, Payback, Street Racing Syndicate); ask KarnFu for CRC32s; credit KarnFu (Reported-by, fixture names, release notes).

## 5. What is verified where
| Item | PPSSPP | PSP hardware |
|---|---|---|
| SMC_PARTIAL_SINGLE (dr2) | 21/26 identical, 5 timing-only, 0 stale hits | not run |
| XLAT_FAST x7 | 26/26 identical, corpus 35/35 | LIMITS piece only (+4% Doom/Duke) |
| Low latency tier 0 | n/a | 4 -> 2.0-2.5 vblanks, fps unchanged (1000) |
| Low latency tier 1 | built | unverified; console dropped at exit once |
| PRs #15/#16 | smoke+gb identical | sleep test pending (owner) |

## 6. Next steps
1. Whole bench-ready suite: dr2 on the 3000, XLAT_FAST x7 on the 1000, compare vs b411 (queued in bench/tools/morning_watch.py, waiting for the consoles).
2. xlat per-piece A/B (x4, x5, x6) plus a b411 rerun as a noise control.
3. Tier-1 latency retest (nfsu2 x3), then tier 0 on GB/GBC, FF, rewind, link.
4. Combine dr2 + XLAT_FAST + latency tier 0 + PRs #15/#16 on one beta branch; owner hardware play on every core plus the sleep test plan. Multi-day beta.
5. Investigate the Mario Golf STALE_CHECK count; attack jit_code_publish (2.1 ms/frame).

## Appendix: campaign coordination log (copy of bench/COORD.md)

# Perf campaign coordination (2026-10-20)

Two workers, one PSP each. Append-only log below; newest at the bottom.
Claim an idea BEFORE building it; never test an idea the other has claimed.

| Worker | PSP | PSPLink port | Branch / worktree | Track |
|---|---|---|---|---|
| main (Claude) | PSP-3000 | 10000 | claude/bench-dr  builds/bench-dr-worktree | FREQUENCY: why SMC writes become full RAM flushes (fallbacks, gate churn/capacity) |
| agent "xlat" | PSP-1000 | 10100 | claude/bench-xlat builds/bench-xlat-worktree | COST: make each RAM-block (re)translation cheaper |

Baseline: 4.1.1 harness-prx (builds/bench-411/harness-prx), results psplink-stage/runs/b411-3000 and b411-1000.
Acceptance for any change: PPSSPP vhash identical to baseline on the full bench-ready set (bench.py validate),
then whole bench-ready suite on your PSP; a win must not regress any fixture beyond noise (~1%).

## Log
- main: baseline captured. Slow titles = full RAM flushes 5-22/frame (own field 6 of core_health jitc/own). Doom ~300 blocks retranslated/frame = 32 ms jit.
- main: two more workers (no console of their own): "prs" reviews PRs #15/#16 on claude/pr-review (builds/pr-review-worktree); "latency" designs a same-frame low-latency mode on claude/low-latency (builds/low-latency-worktree). They request console time here ("- latency: REQUEST psp-3000 slot ...") and wait for main to grant it.
- 2026-10-20 00:44 prs: START review of PRs #15/#16 on claude/pr-review (no console, no USB)
- xlat: started 2026-10-20. Measuring translation cost with the qemu twin (tools/drprof, ship config) on Doom II / Duke before any change. CLAIM: translation-side costs only (translate_block/scan/emit/publish/flush memsets/bookkeeping), not SMC policy.
- xlat: RESULT x1 (XLAT_FAST LIMITS: update_pc_limits once per block instead of per guest instr; exact). Twin Doom xlat instr -7.4%, twin hash IDENTICAL 600f. PSP-1000: Doom jit 32.68->30.40 ms, fps 18.50->19.25 (+4.1%); Duke jit 34.65->32.02, fps 17.18->17.81 (+3.7%). vhash PPSSPP pending. Next: CLAIM translator compiled at O2/Os (x2/x3, I-cache size experiment), then scan_block gate lookup hoist.
- latency: REQUEST psp-3000 (or 1000) ~60 min: PSPLink PRX harness builds/low-latency-out/lat-3-prx, ms0:/GBALAB only, 7 fixtures x 4 ini arms (flip_early/me_early_retire/me_lowlat/input_fresh), perf_rig + EVT lat. No handoff/sleep/quit. Waiting for GRANTED latency.
- main 2026-10-20 ~01:00: PSP-3000 DROPPED OFF USB (pspsh :10000 silent, device gone from Windows) sometime after 23:50; likely battery/power. Needs the owner in the morning. Nobody use port 10000.
- main: PSP-1000 is now SHARED, one job at a time, in this order: (1) xlat finishes its CURRENT run then pauses; (2) GRANTED latency on psp-1000 for up to 60 min starting when xlat logs "- xlat: PAUSED"; latency logs "- latency: RELEASED" when done; (3) xlat resumes. main's hardware runs queue after latency (main logs a REQUEST). Always check modlist/that no app runs before launching.
- xlat: RESULT x2/x3 (translator compiled at O2 / Os via optimize attribute; tba 361KB -> 298KB / 241KB): no gain. vs x1: O2 Doom fps 19.25->19.05, Duke 17.81->17.64; Os see next line. Dropped. x1 PPSSPP vhash: 26/26 bench-ready IDENTICAL to 4.1.1. Twin: XLAT_FAST + GATEMAP hoist + ARM exit-point prefilter = xlat instr -13% Doom, -13% Duke, -13% NFSMW, -12% SSX3, -11% Smashing, hashes identical.
- xlat: PAUSED (PSP-1000 port 10100 free for latency, up to 60 min)
- xlat: x3 (Os) result: WORSE, Doom fps 19.25->17.83, jit 30.4->34.6 ms; Duke 17.81->16.62. Instruction count, not I-cache footprint, sets translator time -> twin counts are a good guide.
- latency: START psp-1000 port 10100 (granted by main; xlat PAUSED). lathw.py arms A,B,C,D on 7 fixtures, ms0:/GBALAB/lat3-1000.
- 2026-10-20 01:39 prs: DONE. claude/pr-review @ 56f35a2: #15 (10 commits) + #16 (2, rebased onto #15) + 1 adjustment; release build 0 warnings (builds/pr-review-out/release), host 30/30, PPSSPP smoke+gb vhash = 4.1.1 baseline. Sleep needs owner HW test; no console used.
- xlat: x5 (XLAT_FAST LIMITS+GATEMAP+EXITPF+RAMEND) PPSSPP vhash 26/26 IDENTICAL to 4.1.1. Twin xlat -14.9% Doom. Added DCLEAR (dispatch cache clear only filled slots; twin flush zone -28% Doom), twin identical 12 fixtures. Waiting for console (latency).
- latency: RELEASED psp-1000 (port 10100 idle, no app running). Tier 0 HW-verified: latch-poll 4.0 -> 2.0-2.5 vblanks, fps/work unchanged on 7 fixtures. Pad sample is ~16 ms stale on real firmware too. Tier 1 has a 2nd bug (fixed, rebuilding); asking main for a 10-min follow-up slot.
- xlat: RESUMED psp-1000 (x6d diag split Doom/Duke, then x6 = full XLAT_FAST on the whole bench-ready set)
- latency: REQUEST psp-1000 follow-up ~15 min: lat-5-prx, arm C on em_battle/hns_heavy/nfsu2_race. Waiting.
- main: GRANTED latency one more 15-min slot on psp-1000 (lat-5-prx verify) NOW; then xlat resumes after '- latency: RELEASED'. main's own 1000 runs queue after xlat's next run.
- latency: grant noted, but gpsp-adhoc is RUNNING on 10100 (xlat resumed before the grant). Not launching. xlat: please log '- xlat: PAUSED' after your current job; I start then (15 min) and log RELEASED.
- main: PSP-1000 QUEUE (one job at a time; each logs PAUSED/RELEASED): xlat current job -> latency 15 min (tier-1 verify) -> main 25 min (dr2 perf) -> xlat resumes.
- xlat: PAUSED (x6d done; latency go, 15 min)
- latency: START psp-1000 tier-1 verify (lat-5-prx, arm C em/hns/nfsu2)
- xlat: HW split (x6d diag build, PSP-1000, per frame; diag adds ~1.4 ms of clock reads): Doom jit 34.9 ms of which RAM ARM scan_block 10.1 ms (29%); jit_code_publish 2.1 ms/frame OUTSIDE jit (285 publishes, ~7.4 us each: kernel D writeback + I invalidate). Duke: scan 10.4 of jit 35.7, publish 2.2 ms. Translator runs ~670 cycles per guest instr translated (twin ~210 instr -> CPI ~3.2).
- latency: RELEASED psp-1000 -- BUT PSPLink on 10100 is NOT ANSWERING. Last job (lat-5-prx tier-1 me_lowlat=1, nfsu2_race) ran at full speed to frame ~3296/3330 (heartbeat normal), then the console dropped around exit/teardown; frontend.log never came back. Cause unknown (tier-1 engaged for the first time in this build; could also be PSPLink/USB). Console needs a manual check/reboot by the owner or main; I touched nothing.
- main 02:40: PSP-1000 has ALSO left USB (device gone, like the 3000). Both consoles need the owner. Possibly flat batteries (both have run PSPLink on PC USB for hours; the 1000 cannot charge over USB) — but latency's tier-1 run (me_lowlat=1, nfsu2 at exit) is NOT ruled out. NO HARDWARE until the owner is back. Everyone: continue PPSSPP / twin work and write up.
- xlat: x7 (XLAT_FAST all: LIMITS+GATEMAP+EXITPF+RAMEND+DCLEAR+SCANFN, branch claude/bench-xlat 757b2c7+) PPSSPP vhash 26/26 bench-ready IDENTICAL to 4.1.1. Twin xlat instr vs ship: Doom -17.4, Duke -17.6, NFSMW -16.8, SSX3 -16.2, Smash -15.4, Ford -16.1, HotWheels -16.1, MaxPayne -16.9, NFSC -16.7, TopGear -17.3, MTennis -14.2, MGolf -13.5 (%); all twin hashes identical (+7 lab fixtures). No HW for x4-x7 yet (consoles off USB).
- xlat: corpus oracle (35 ROMs x 3000 f, power-on, ship config): xlat = fast2 + XLAT_FAST PASS 35/35 (state, audio, picture). 4 mutants (one per XLAT_FAST piece) all CAUGHT (DCLEAR 30/35, GATEMAP-noent 1/35, LIMITS 21/35, EXITPF 35/35). Commits on claude/bench-xlat.
- xlat: HW PLAN (consoles off USB; nothing below has run). PRX builds in builds/xlat-out/<tag>/ (all harness, GBA_LINK=1 REWIND=1 BUILD_PRX=1, 0 warnings):
    x1 = LIMITS (measured: Doom/Duke +4%) ; x4 = +GATEMAP+EXITPF ; x5 = +RAMEND ; x6 = +DCLEAR ; x7 = +SCANFN = all of XLAT_FAST at 56c55e2 (CANDIDATE)
  1) candidate, whole suite:  python psp_bench.py --console 1000 --port 10100 --build C:/Users/DrSto/OneDrive/Desktop/GBAdhoc/builds/xlat-out/x7 --tag x7 --tags bench-ready   (~2 h)
  2) per-piece A/B, key six:  for t in x4 x5 x6: same with --build .../xlat-out/$t --tag $t --only kf_doom_ii,kf_duke_nukem_advance,kf_smashing_drive,kf_ssx_3,kf_need_for_speed_most_wanted,hns_heavy   (~25 min each)
  3) noise control: --build C:/Users/DrSto/OneDrive/Desktop/GBAdhoc/builds/bench-411/harness-prx --tag b411r with the same --only
  compare: python builds/xlat-out/tools/cmp.py b411 x7   (fps/work/jit/cpu per fixture; cmp.py A B [console])
  any further piece: CORE_EXTRA="XLAT_FAST=1 XLAT_NO=SCANFN:DCLEAR" switches pieces off (LIMITS GATEMAP EXITPF RAMEND DCLEAR SCANFN).

