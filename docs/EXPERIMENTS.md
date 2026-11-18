# Experiments: retired branches (`attic/*` tags)

Long-shot experiment branches are welcome. The rule is to close them:

1. Record the outcome in one line: won/merged, lost (and why), or superseded by X.
2. Remove the experiment's worktree.
3. Keep its commits under an `attic/` name if it might be worth revisiting.

In the 2026-11 hygiene pass, each retired branch was replaced by an annotated tag
`attic/<branch-name>` at the same commit, and then the branch ref was deleted. No commit was lost.
Each tag's message records why it was retired and its original tip.

## Restore a branch

```sh
git branch <name> attic/<name>        # e.g. git branch claude/foo attic/claude/foo
git worktree add ../foo-worktree <name>   # only if you want to work on it again
git tag -n99 -l attic/<name>          # read why it was retired
```

## Outcome column

The outcome comes only from git facts, recomputed when this file was generated:
- **merged**: the tagged commit is part of a protected line (`claude/health`, `main`, a beta/rc branch).
- **patch-equivalent**: every commit has an equivalent already in `claude/health` (`git cherry`).
- **unknown**: the result is not recorded in the repo. Do not read it as won or lost.

## gpsp (this repo): 161 tags

| tag | commit | date | tip subject | outcome |
|---|---|---|---|---|
| `attic/claude/460-beta-public` | `9082e3bf99` | 2026-11-03 | 4.6.0-beta.2: README and changelog | merged: in `claude/health` |
| `attic/claude/460-beta4` | `c7a74a886d` | 2026-11-05 | layout: re-pin pads 304/0 -> 144/32 after the online PRX merge | merged: in `claude/health` |
| `attic/claude/460-beta4-fix1` | `d27dc90809` | 2026-11-07 | 4.6.0-beta.4.1: changelog and README for the online hotfix | merged: in `claude/health` |
| `attic/claude/460-cand3` | `c279997ef4` | 2026-11-03 | cand3 pin: pads 6256 / 200 (ff_flip_pace +576 B text, +16 B rodata on top of RAM_MIRROR_FI | merged: in `claude/health` |
| `attic/claude/b4-colour-ambient` | `8c3ffc2410` | 2026-11-05 | beta.4 fixes: colour modes on the ME renderer; ambient bars None / Art / Game | merged: in `claude/health` |
| `attic/claude/b4-direct` | `b656304b26` | 2026-11-05 | online PRX: 'connecting' during the first punch burst, no toast for a retry burst | merged: in `claude/health` |
| `attic/claude/b4-edge-glow` | `c9375f3a79` | 2026-11-06 | ambient bars: live edge glow for Game; Art switches back | merged: in `claude/health` |
| `attic/claude/b4-join-timeout` | `a4981bbbba` | 2026-11-07 | test_relay_nat: one-way direct path cases; beta.4's 20 s server explicit | merged: in `claude/health` |
| `attic/claude/b4-listlag` | `fce087bb0a` | 2026-11-05 | ui: list marks pump at zero image cost (PSP-1000 heap unchanged) | merged: in `claude/health` |
| `attic/claude/b4-online-desync` | `b90059002c` | 2026-11-05 | online: a lobby host survives a lost room; relay caps fit a link session | merged: in `claude/health` |
| `attic/claude/b4-wifi-picker` | `78c9e8d52d` | 2026-11-05 | docs: LAYOUT-PINNING, the 16/32 pin for the Wi-Fi picker | merged: in `claude/health` |
| `attic/claude/b5-online` | `e5c5085c4c` | 2026-11-07 | docs: ONLINE-TUNNEL home relay discovery -- PPSSPP Emerald trade by discovery only, sizes, | merged: in `claude/health` |
| `attic/claude/base-4.1.0` | `3e4752a1e1` | 2026-10-30 | base410 harness-only backport: harness: show_fps key turns the FPS chip on (the Lab sets i | patch-equivalent: all 7 commits already in `claude/health` |
| `attic/claude/bios-bound` | `869675b783` | 2026-11-03 | dr_corpus: DRCORPUS_BB set for BIOS_BOUND_FIX + XLAT_LAST_TRY_LAZY | merged: in `claude/health` |
| `attic/claude/bmt-mirror` | `974402dc9d` | 2026-10-28 | MIRROR_CENSUS (diagnostic, twin only): count RAM lookups answered through another mirror | unknown |
| `attic/claude/c3lat3` | `2510780dcf` | 2026-10-22 | Merge PR #15 (ChandlerHardy: sleep quiesce, gameplay power lock, menu/pause wake) into c3l | merged: in `claude/health` |
| `attic/claude/c3lat4` | `8d752b7f9e` | 2026-10-22 | Screenshots: 8-bit indexed PNG by default; sleep-menu test knows Reset | merged: in `claude/health` |
| `attic/claude/cand2` | `d4bbfb6bfc` | 2026-10-29 | Reset game: refuse in a live session, not in every GBA game | merged: in `claude/health` |
| `attic/claude/codegen-ilf` | `f1104d2b21` | 2026-10-30 | Makefile: INLINE_LOAD_FAST=1 hook (411e888 had none, so ilf1 was built without the lever) | unknown |
| `attic/claude/codegen-ima` | `f0760c5254` | 2026-10-31 | synth: pcarg_openbus.s, the PCARG_DROP open-bus oracle | unknown |
| `attic/claude/codegen-q3` | `29ae060bbc` | 2026-10-31 | psp: config.ini msr_fast defaults to the core's MSR_FAST_DEFAULT | merged: in `claude/health` |
| `attic/claude/codegen-quality` | `5bfba97c2b` | 2026-10-28 | codegen: ENTRY_DIET prototype + ARM_FLAG_LIVENESS v3 port (both OFF) | unknown |
| `attic/claude/codegen-shrink` | `9ddc5a9b66` | 2026-10-31 | Merge claude/rc-4.5.0 (6002e21) into claude/codegen-shrink | unknown |
| `attic/claude/combo3-lowlat` | `9c53961247` | 2026-10-21 | build.sh: COMBO3=1 adds the lab-combo3 core flags to any profile (owner playable build) | merged: in `claude/health` |
| `attic/claude/compat-cost` | `70a3e20305` | 2026-10-21 | tools/drprof: shiftreg_rom.py, a register-shift oracle ROM generator (twin) | unknown |
| `attic/claude/compat-fix` | `950c5f3025` | 2026-10-20 | cpu: FIQ_BANK_FIX -- bank FIQ's r8-r12 in set_cpu_mode (switch, off) | unknown |
| `attic/claude/coreb-mem` | `81aae4885f` | 2026-10-29 | tools: test_rom_loading stubs gamepak_buffer_pages / gamepak_block_bytes for the extracted | merged: in `claude/health` |
| `attic/claude/corpus-classify` | `ce64fc58e4` | 2026-10-28 | COND_ENTRY_FIX: mark conditional entry points at scan time (0x40), not per instruction | unknown |
| `attic/claude/corpus-classify-dbg` | `dbbcee53aa` | 2026-10-28 | dbg: report interpreter ARM fetch != memory | unknown |
| `attic/claude/corpus-classify-f1c2` | `e02e2a671e` | 2026-10-28 | COND_ENTRY_FIX: mark conditional entry points at scan time (0x40), not per instruction | unknown |
| `attic/claude/corpus-classify-ig` | `06a22302fe` | 2026-10-28 | COND_ENTRY_FIX: mark conditional entry points at scan time (0x40), not per instruction | unknown |
| `attic/claude/corpus-classify-oracle` | `658b4236dd` | 2026-10-28 | oracle: compare RAM addresses by their canonical mirror (LAB_LOCKSTEP) | unknown |
| `attic/claude/cpu-clock` | `683950bacd` | 2026-11-03 | Layout pads re-tuned for the CPU clock setting (7920 / 488) | merged: in `claude/health` |
| `attic/claude/cpu-levers` | `dc343509c9` | 2026-10-27 | SMC_STM_RANGE: short writeback STMIA retires its own span (FIFA 07) | unknown |
| `attic/claude/cpu-levers-arm` | `17bb418d5c` | 2026-10-27 | SMC_SAME_SKIP/ADAPT: layout-neutral hooks (cpl2c) | merged: in `claude/health` |
| `attic/claude/cpu-levers-chain` | `fddcb31dca` | 2026-10-27 | SMC_STM_CHAIN: close the STM block-store hole (cpu-levers) | merged: in `claude/health` |
| `attic/claude/cpu-levers-cpl2w` | `33e58faa2a` | 2026-10-27 | synth: xs_pair.s, XS_WALK_PAIR odd-slot mutant reproducer (catches XS_WALK_PAIR_MUTANT at  | merged: in `claude/health` |
| `attic/claude/cpu-levers-fiq` | `81c6333652` | 2026-10-27 | hot_data_guard.ld: one byte of content keeps .data.pin PROGBITS (a pad-only section is NOB | merged: in `claude/health` |
| `attic/claude/cpu-levers-stm` | `c36ce6e9ea` | 2026-10-27 | synth: stm_chain.s, FIFA-shape STM block-store hole reproducer | merged: in `claude/health` |
| `attic/claude/cpu-levers-vx` | `9bc4c2b50b` | 2026-10-27 | VRAM_EXEC: keep its growth out of the pinned DMA loops (cpu-levers vx) | merged: in `claude/health` |
| `attic/claude/cpu-levers-walk` | `fb5fee5ea1` | 2026-10-27 | SMC_WALK_FAST (twin-validated, not built): pairwise tag walk + start list | unknown |
| `attic/claude/crash1001` | `66cecd551d` | 2026-11-01 | 4.5.0: README, changelog, media and release packaging | merged: in `claude/health` |
| `attic/claude/data-pin-lab-compat-cost` | `4899f9801b` | 2026-10-21 | gitignore the generated DATA_PIN ordering file | unknown |
| `attic/claude/data-pin-lab-final` | `8fcc060246` | 2026-10-21 | gitignore the generated DATA_PIN ordering file | unknown |
| `attic/claude/direct-connect` | `446ba42512` | 2026-11-06 | online: direct path more often -- relay-LAN host candidate, symmetric port prediction, no  | merged: in `claude/health` |
| `attic/claude/dr2-nfsu` | `dbdc3d839b` | 2026-10-21 | dr2-nfsu: corpus mutant m_nfsu_word_t (judge against DRCORPUS_NFSUREF=nfsut) | unknown |
| `attic/claude/dr2-nfsu-cov` | `9314946bc9` | 2026-10-21 | dr2-nfsu: EXEC_STALE_CHECK, a per-executed-instruction stale-code checker (twin only) | merged: in `claude/health` |
| `attic/claude/engine-fixes` | `47199ce47a` | 2026-10-26 | dynarec: TEST_IMM_CARRY_FIX (TST/TEQ rotated imm sets C) | unknown |
| `attic/claude/engine-fixes-gate` | `e70b390a81` | 2026-10-26 | lab: gate lockstep -- step over condition-failed ARM instructions before a replayed IRQ (e | unknown |
| `attic/claude/epochc` | `e68a8b48c1` | 2026-11-14 | Epoch C C3-2e: the script/wireless bring-up and the hand-over to the loop are session_net_ | merged: in `claude/health` |
| `attic/claude/epochc-plan` | `814b6a68a9` | 2026-11-13 | tools/refactor: raw vs -E tells the copies of a helper's lines apart | merged: in `claude/health` |
| `attic/claude/exec-profile` | `a1b77ae40b` | 2026-10-21 | lab prx: 6.61 saved-context layout from the j484f8d raw dumps (EPC +0x38, SR +0x34, ra +0x | unknown |
| `attic/claude/ff-15x` | `e1a9016bbf` | 2026-11-13 | refactor: regenerate fe/README.md and the T14 headers for the 1.5x/2x tree | merged: in `claude/health` |
| `attic/claude/ff-pacing` | `a7d963c618` | 2026-11-03 | docs: FF presentation cadence, 4.1.0 vs beta.2, and ff_flip_pace measured | merged: in `claude/health` |
| `attic/claude/ffpace-4.1.0` | `5fc1cd9768` | 2026-11-03 | lab_ffpace clock_enforce: try the split cpu/bus setters too | unknown |
| `attic/claude/filters` | `eccc981308` | 2026-10-29 | video: grid filters follow GAME pixels at every scale | merged: in `claude/health` |
| `attic/claude/filters-v2` | `ea4395059a` | 2026-11-05 | tests: RGB stripe row-line check follows the soft profile | merged: in `claude/health` |
| `attic/claude/flag-bake` | `e2245ad0ee` | 2026-11-02 | docs: JIT_COST_KEYS and how Lab builds use it | merged: in `claude/health` |
| `attic/claude/gate-instr` | `b39d3df29f` | 2026-10-28 | oracle: interpreter steps over undefined Thumb E800-EFFF like the dynarec (LAB_LOCKSTEP) | unknown |
| `attic/claude/gb-video` | `df79fef2e5` | 2026-10-20 | gbcore: GB_PPU_FIX picture fixes (HDMA/VBK, 10-object limit, DMG X priority) + host tgb_re | unknown |
| `attic/claude/gbcore-perf` | `4eaac37c37` | 2026-10-28 | docs: Gen 2 armed spike profile, FAST_TILE exactness, gbp-c2 | merged: in `claude/health` |
| `attic/claude/go-slide-sleep` | `d3d3b39196` | 2026-11-02 | Check for a held-off suspend from the loop, not on a timer | merged: in `claude/health` |
| `attic/claude/hdg-linkfix-wip` | `81c6333652` | 2026-10-27 | hot_data_guard.ld: one byte of content keeps .data.pin PROGBITS (a pad-only section is NOB | merged: in `claude/health` |
| `attic/claude/hotfix-451` | `fbc48c7bb3` | 2026-11-02 | Quit to game list relaunches once the ME module was loaded (issue #28) | unknown |
| `attic/claude/hw1-sleep` | `fedb7048c0` | 2026-11-09 | docs: LAYOUT-PINNING records the HW-1 re-pin | merged: in `claude/health` |
| `attic/claude/hw1-sleep-r2` | `28405d3d9c` | 2026-11-13 | layout: re-pin for the ME-lost wake's hold (TEXT/CORE, lab RO; main grows at its exit) | merged: in `claude/health` |
| `attic/claude/hw2-exit` | `c9277cdc2f` | 2026-11-12 | layout: re-pin for the F4 hold (TEXT/CORE; main and the loop unmoved) | merged: in `claude/health` |
| `attic/claude/ibranch` | `d36ce6f6c2` | 2026-11-13 | layout: PCARG's cold-text pad 3584 -> 3104 takes DISPATCH_ASSOC's out-of-line fill and for | merged: in `claude/health` |
| `attic/claude/idle-aw` | `7a8c094978` | 2026-10-28 | FIXPOINT_SKIP: make the check cheap and only where it pays | unknown |
| `attic/claude/idle-general` | `05f0296bd8` | 2026-10-27 | layout: regenerate the pinned order for the idle-general build (tools/layout/regen.sh) | merged: in `claude/health` |
| `attic/claude/inproc-census-fix` | `cf91ebee45` | 2026-10-31 | inproc: give XS_HEAP's SMC tables back on the in-process return | merged: in `claude/health` |
| `attic/claude/inproc-me-sleep` | `53f12544d3` | 2026-11-02 | INPROC-RETURN: the kept module on the real ME, no sleep yet | merged: in `claude/health` |
| `attic/claude/jit-cost` | `6d3796c467` | 2026-10-20 | docs/JIT-COST.md: corpus and PPSSPP results | merged: in `claude/health` |
| `attic/claude/jit-cost-dr` | `0ab1e39a52` | 2026-10-20 | Merge branch 'claude/jit-cost' into claude/jit-cost-dr | unknown |
| `attic/claude/jit-thrash-fix` | `165839d3a2` | 2026-11-08 | dynarec: SMC_REUSE spare 384 KiB (owner decision 2026-11-08) | merged: in `claude/health` |
| `attic/claude/kirby-460` | `5b2872bad4` | 2026-11-03 | WIP kirby-460 status 2 (no code change) | unknown |
| `attic/claude/kirby-link` | `c9d362e38a` | 2026-10-30 | multigame: Kirby link fixtures (Amazing Mirror + Nightmare in Dream Land co-op) | unknown |
| `attic/claude/kirby-prof` | `263cc51556` | 2026-11-02 | gl_host: --headless0q (the joining console's view) and --sdiag; reproduces the HALT_HOP li | unknown |
| `attic/claude/lab-bootfix` | `c97e07556f` | 2026-10-27 | lab harness: the window USB note keeps the whole kind (after_task) | unknown |
| `attic/claude/lab-bootfix2` | `50fa6b42e0` | 2026-10-27 | lab harness: the window USB note keeps the whole kind (after_task) | unknown |
| `attic/claude/lab-c3lat4` | `a123f8980e` | 2026-10-26 | harness: show_fps key turns the FPS chip on (the Lab sets it on every task) | merged: in `claude/health` |
| `attic/claude/lab-cand-cA` | `a45070f002` | 2026-10-20 | lab-cand-cA: lab harness + claude/smc-coverage candidate A (bench-dr + SMC coverage classe | merged: in `claude/health` |
| `attic/claude/lab-cand-compat` | `85c7118e79` | 2026-10-20 | lab-cand-compat: lab harness (bd50d0c = lab-final) + claude/compat-fix | unknown |
| `attic/claude/lab-cand-compat-cost` | `c7b05582d4` | 2026-10-21 | lab-cand-compat-cost: lab-cand-compat (85c7118) + claude/compat-cost 265d13e | unknown |
| `attic/claude/lab-cand-dr2` | `f34d899448` | 2026-10-20 | lab-cand-dr2: lab harness + claude/bench-dr (SMC_PARTIAL_SINGLE) | merged: in `claude/health` |
| `attic/claude/lab-cand-j2` | `ea288832a2` | 2026-10-20 | lab-cand-j2: lab harness + claude/jit-cost (XLAT_FAST + JIT_COST) | unknown |
| `attic/claude/lab-cand-nfsu` | `daaaa1627f` | 2026-10-21 | lab-cand-nfsu: lab harness (bd50d0c = lab-final) + claude/dr2-nfsu (tag-hole fix + TIMER_R | unknown |
| `attic/claude/lab-cand-selfact` | `1708d3514f` | 2026-10-21 | lab-cand-selfact: lab-cand-compat-cost (c7b0558) + claude/self-activate 25cab1b | unknown |
| `attic/claude/lab-cand-x7` | `7498bfec5d` | 2026-10-20 | lab-cand-x7: lab harness + claude/bench-xlat (XLAT_FAST) | unknown |
| `attic/claude/lab-candidates` | `218bc8f12f` | 2026-10-20 | lab-candidates (combo): lab harness + jit-cost (x7 + JIT_COST) + bench-dr | unknown |
| `attic/claude/lab-combo2` | `c063b1c6c0` | 2026-10-21 | lab-combo2: lab harness + smc-coverage candidate A + jit-cost (bench-xlat XLAT_FAST + JIT_ | merged: in `claude/health` |
| `attic/claude/lab-combo3` | `a9f432c936` | 2026-10-21 | lab-combo3: lab-combo2 + claude/dr2-nfsu-cov (tag-hole fix for candidate A + TIMER_RELOAD_ | merged: in `claude/health` |
| `attic/claude/lab-divergence` | `c2386026c1` | 2026-10-20 | lab: LAB_COMPAT harness EVT lab_backup (emulated save chip) at core_health | unknown |
| `attic/claude/lab-harness` | `5ab9cd1f33` | 2026-10-20 | lab harness: per-frame hooks cost nothing unless used; blink without a clock syscall; one  | unknown |
| `attic/claude/lab-instruments` | `4211beced1` | 2026-10-20 | lab prx: accept only a kernel-text thread-info function (PPSSPP returns an error code) | unknown |
| `attic/claude/lab-sleep` | `e77c106025` | 2026-10-26 | lab-sleep: lab_sleep_dry (the oracle's base arm) and the RTC's view of each sleep | unknown |
| `attic/claude/latency-play` | `5978e9f378` | 2026-10-21 | Latency test builds: INPUT_FRESH=1 sets the input_fresh default | merged: in `claude/health` |
| `attic/claude/link-desync` | `119d3b9ce5` | 2026-11-13 | link: both copies of a machine start a session with the same dynarec | merged: in `claude/health` |
| `attic/claude/link-mismatch` | `825352c969` | 2026-11-02 | link: refuse a session between different builds, at admission in netdrv | merged: in `claude/health` |
| `attic/claude/link-ui-route-450` | `a8f516b30b` | 2026-11-02 | harness: ui_script reads the screen (the player's route) on v4.5.0 | patch-equivalent: all 1 commits already in `claude/health` |
| `attic/claude/linkleak` | `5b706c757d` | 2026-10-30 | docs: GBA-LINK-MIRROR 19.9, B's XS_HEAP block leaked 70,672 B per session | merged: in `claude/health` |
| `attic/claude/lowlat-t1` | `6f1bd1052a` | 2026-10-31 | docs: LOW-LATENCY 7.1, tier-1 hardware results (latency, exits, vhash, gate, A/B) | merged: in `claude/health` |
| `attic/claude/mainpsp-map` | `01067e7497` | 2026-11-05 | Merge claude/beta4-online-zero-7025085 94ab942: online PRX hardware fixes (16 KiB start/st | merged: in `claude/health` |
| `attic/claude/mainpsp-tools` | `8d44d4d741` | 2026-11-05 | refactor tools: run_all.sh, README, compensating-pad probe | merged: in `claude/health` |
| `attic/claude/mem-1000` | `9e41abc8c3` | 2026-11-02 | docs: the session numbers, measured | merged: in `claude/health` |
| `attic/claude/mgolf-fault` | `e70d60aa7c` | 2026-11-08 | layout: the beta.5 JIT fix's growth taken back in the hot order (bscan section) | merged: in `claude/health` |
| `attic/claude/mlog-range` | `4eb36cfc89` | 2026-11-13 | layout: re-pin the layout-neutral write maps (release and lab) | merged: in `claude/health` |
| `attic/claude/monomyth-compat` | `91c280079f` | 2026-10-31 | core: zero-byte I/O cycle position, refill in the exit delay slot | merged: in `claude/health` |
| `attic/claude/mt-desync` | `abbdbb372b` | 2026-10-30 | GBA link: cold start forgets the dynarec's learned policy (Mario Tennis desync) | unknown |
| `attic/claude/mxp-ui` | `f3e34fe9eb` | 2026-11-05 | browser: state/cheat marks on the hints, marquee names, position bar (#31) | merged: in `claude/health` |
| `attic/claude/nv-cond` | `94be48efa6` | 2026-11-02 | tests: Chandler's nv_cond.s and nv_cond_check.py, run by nv_cond_run.sh | merged: in `claude/health` |
| `attic/claude/nv-cond-450` | `79a6475fc9` | 2026-11-02 | dynarec: NV_COND_FIX on 4.5.0 (issue #10 test build) | unknown |
| `attic/claude/online-desktophost` | `4aa5a4e045` | 2026-11-05 | docs: desktop hosts -- PPSSPP room code + invite trade, natlab 14/14, fuzz, heap check | merged: in `claude/health` |
| `attic/claude/online-latency` | `65abc487ae` | 2026-11-05 | online: fix PSP warnings (sendto cast, build copy, rtt format) | merged: in `claude/health` |
| `attic/claude/opt-1000` | `a27c15fe1e` | 2026-11-07 | tools: flags.lock records build.sh MGIFT=1 | merged: in `claude/health` |
| `attic/claude/p-tidy` | `2e9f210d91` | 2026-11-09 | P-tidy: hot_order.txt's size column refreshed from the b7d0cafd release ELF | merged: in `claude/health` |
| `attic/claude/pcarg-side` | `f485cd0579` | 2026-11-09 | docs: PCARG-SIDE review fixes: pad oracle on the SMC titles, ROM cache sign | merged: in `claude/health` |
| `attic/claude/phase-p` | `1f8f7d2cd3` | 2026-11-09 | Merge claude/beta5-base e70d60aa into claude/phase-p (P7 merge re-proof base) | merged: in `claude/health` |
| `attic/claude/quickload-split` | `2d0f472a22` | 2026-11-12 | tools+docs: the quick load's route through the real menu (ui_model) | merged: in `claude/health` |
| `attic/claude/ram-mirror` | `6069a34f8e` | 2026-11-03 | Merge PR #30 (Chandler Hardy): RAM_MIRROR_FIX into claude/ram-mirror for validation | merged: in `claude/health` |
| `attic/claude/ram-mirror-pcfix` | `356d6d4bbd` | 2026-11-04 | pin: pads 5840 / 200 after RAM_MIRROR_FIX's alias-pc rework | merged: in `claude/health` |
| `attic/claude/ramtest` | `edf7a92e63` | 2026-11-02 | tools: standalone PSP RAM tester (ramtest) | unknown |
| `attic/claude/reuse-modes` | `38b92d7371` | 2026-10-28 | fix broken string literal in ESC trace (warning gate) | patch-equivalent: all 4 commits already in `claude/health` |
| `attic/claude/reuse-prelim` | `c5effe8468` | 2026-10-28 | SMC_REUSE step 4 (PRELIMINARY): spare region + exact always-on reuse | unknown |
| `attic/claude/rfu-audit` | `720d58393b` | 2026-10-30 | psp: release builds skip formatting the RFU flight dump | merged: in `claude/health` |
| `attic/claude/rfu-backlog` | `89645e3f96` | 2026-10-30 | rfu: census widened to 17 Pokemon runs -- worst standing run 49 frames, ARM 100 still in t | merged: in `claude/health` |
| `attic/claude/rfu-backlog-on-cand2` | `c40c63fd63` | 2026-10-30 | rfu: census widened to 17 Pokemon runs -- worst standing run 49 frames, ARM 100 still in t | patch-equivalent: all 6 commits already in `claude/health` |
| `attic/claude/rfu-em-fixtures` | `c7cbd2a744` | 2026-10-30 | e2e: Union Room battle, host-loss and cable-then-wireless fixtures | unknown |
| `attic/claude/rfu-flightrec` | `7f091a2888` | 2026-11-01 | rfu: per-packet flight recorder (RFUTL.BIN), lab builds only | unknown |
| `attic/claude/rfu-lobby` | `2599c27886` | 2026-11-02 | rfu: drop a closed room from peers' scan lists at once | unknown |
| `attic/claude/rfu-reference` | `488e0d2183` | 2026-11-02 | rfu_sched: a lost link ends the wait at once | unknown |
| `attic/claude/rfu-ticks` | `65d17a9872` | 2026-11-02 | rfu: no 57 fps session force with ticks; beta releases ship ticks | merged: in `claude/health` |
| `attic/claude/rom-mirror-reload` | `edb6450896` | 2026-11-02 | gamepak: a resident page faulted through a mirror is mapped, not reloaded | unknown |
| `attic/claude/sched-hot` | `123674e79b` | 2026-11-13 | docs: LAYOUT-PINNING -- SCHED_HOT's layout holds (no pad change, lock 0 differ, heap uncha | merged: in `claude/health` |
| `attic/claude/self-activate` | `25cab1b13d` | 2026-10-21 | VRAM_EXEC/FIQ_BANK_FIX: nothing on the common path until the game needs it | unknown |
| `attic/claude/shots-displayed` | `474bb0bd1b` | 2026-11-12 | layout: release RO pad 208 -> 236, off the edge of its 64 B step | merged: in `claude/health` |
| `attic/claude/sleep-stress` | `80addda372` | 2026-10-26 | sleep-stress: stress phases, power callback attribution, script_exit (harness only) | unknown |
| `attic/claude/slow-titles` | `151d92b77e` | 2026-10-27 | slow-titles: ENTRY_GATES_RANGE mutant drops the upper half (caught 4/35; the last-block fo | unknown |
| `attic/claude/smc-cost` | `8085753a9b` | 2026-11-13 | cpu_threaded: SMC bookkeeping costs (retired-tag index, one compare per same-value trap, q | merged: in `claude/health` |
| `attic/claude/smc-reuse-mv` | `457d2054fc` | 2026-10-28 | SMC_REUSE step 4 + cost rules: spare invariant, emission site log, epoch rule, SMC_REUSE_M | patch-equivalent: all 4 commits already in `claude/health` |
| `attic/claude/spd-combo` | `0bd4ac4fea` | 2026-11-13 | docs: LAYOUT-PINNING -- spd-combo (reg[60] fix, reg_slots.h, pads compose, heap unchanged) | merged: in `claude/health` |
| `attic/claude/stale-fifa` | `055f031c88` | 2026-10-21 | SMC_BLOCK_WORDS: test the mark in the u32 RAM store stub, emit nothing at store sites | unknown |
| `attic/claude/stick-bind-fix` | `8dfd086c96` | 2026-10-22 | Controls: the browser's Settings loop reads the stick (stick bind fix) | merged: in `claude/health` |
| `attic/claude/stm-fixed` | `445a7afefb` | 2026-11-03 | WIP: Chandler Hardy's PR #27 staged on the old claude/stm-fixed (edb6450, ROM_MIRROR_RELOA | unknown |
| `attic/claude/stm-fixed-460` | `019bd4bcd4` | 2026-11-03 | WIP SMC_STM_FIXED: fixed-base STMIA retires its own span, ON for the 4.6.0 beta | patch-equivalent: all 1 commits already in `claude/health` |
| `attic/claude/stm-fixed-beta` | `34d80727b5` | 2026-11-03 | WIP status: gate in progress (stale stage); twin diverges (timing) on Golf f303 (reconverg | patch-equivalent: all 7 commits already in `claude/health` |
| `attic/claude/stm-fixed-final` | `1d0e54fbb6` | 2026-11-03 | dynarec: SMC_STM_FIXED, a fixed-base STMIA retires its own span (PR #27) | merged: in `claude/health` |
| `attic/claude/t0b-guard-b4` | `52460f3b90` | 2026-11-05 | CATCH UP SOON: running a vblank late, take the audio nudge early (ring > HW/2 + a frame) s | merged: in `claude/health` |
| `attic/claude/tgr-regression` | `a24a64269f` | 2026-10-28 | UPDATE_TRACE: note every smc_trap_entry (diagnostic, LEGO f798 classification) | unknown |
| `attic/claude/tmgc-combat` | `1232139bf6` | 2026-11-03 | pads: LAYOUT_PAD_BYTES 176 -> 7120 after RAM_ALIAS_FIX (release A cold text back on beta.2 | unknown |
| `attic/claude/vidpreset-none` | `b25ec31091` | 2026-11-04 | controls: Video preset "none" is really none (#31) | merged: in `claude/health` |
| `attic/claude/xlat-cost` | `68635fbb19` | 2026-10-28 | xlat-cost: gate lockstep hook in-tree, opcode load through IMMP_ESC_OP | unknown |
| `attic/claude/xmb-launch-beta4` | `53bbfb9821` | 2026-11-05 | README: XMB launch is in testing; Exit -> XMB, Quit to game list -> list; the Windows make | merged: in `claude/health` |
| `attic/nv-cond-pre-rebase-6578e55` | `5d6b8221a9` | 2026-11-02 | tests: Chandler's nv_cond.s and nv_cond_check.py, run by nv_cond_run.sh | patch-equivalent: all 4 commits already in `claude/health` |
| `attic/pr15` | `d6a6f494aa` | 2026-10-19 | End a heap-backed rewind standing in an old save at the teardown | merged: in `claude/health` |
| `attic/pr16` | `95f73c8419` | 2026-10-19 | Swallow only what was held at the answer, and drop the whole stale frame | merged: in `claude/health` |
| `attic/pr20` | `a8f8196f5e` | 2026-10-23 | test_sleep_browser: a mutant gcc's -Wempty-body accepts | merged: in `claude/health` |
| `attic/pr26` | `681e7494f9` | 2026-11-02 | dynarec: ARM condition NV never executes (issue #10, TMGC data bus error) | unknown |
| `attic/pr30` | `75fc4f32ca` | 2026-11-03 | pin: pad 6832 after RAM_MIRROR_FIX | merged: in `claude/health` |
| `attic/pr32-load-confirm` | `6543911a21` | 2026-11-03 | pin: pads 7792 / 8120 after the load question | unknown |
| `attic/worktree-agent-a0a8881305bb32629` | `205b7cc911` | 2026-10-14 | Restore UTF-8 README and changelog characters | merged: in `claude/health` |

## Lab repo (GBAdhoc-Lab): 6 tags

These tags live in the Lab repository, not this one. The same restore commands apply there.

| tag | commit | date | tip subject | outcome |
|---|---|---|---|---|
| `attic/claude/gate-fix` | `23b2683846` | 2026-10-28 | gate: never cache incomplete PPSSPP runs; 0 frames / too few common frames = INFRA-FAIL | merged: in `claude/link-ui-route` |
| `attic/claude/kirby-link-fixtures` | `93b1066410` | 2026-10-30 | suites: Kirby co-op link cases in mp_speed + gameplay-window slice_fps | merged: in `claude/link-ui-route` |
| `attic/claude/lab-features-family` | `e39185a1da` | 2026-11-16 | lab: the features family is schedulable; a job whose case does not resolve for its family  | merged: in `claude/link-ui-route` |
| `attic/claude/lab-harness-tsv` | `b3d5c074b4` | 2026-11-15 | inikeys: read gpsp's harness_keys.tsv; grep psp/** with both call forms | merged: in `claude/link-ui-route` |
| `attic/claude/lab-sleep` | `e82a13b0b9` | 2026-10-26 | sleep oracle: lab-harness transparency check, rig schedule/driver, daemon glue | unknown |
| `attic/claude/rom-unplug` | `43323b1a42` | 2026-11-16 | suites: rom_unplug -- the ROM read failure path on a paged 32 MiB cart (GBAdhoc code-healt | merged: in `claude/link-ui-route` |
