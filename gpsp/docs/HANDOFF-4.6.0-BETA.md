# Handoff: 4.6.0 beta (2026-11-03)

For an agent picking this up cold. Read this file, then the memory index at
`C:\Users\DrSto\.claude\projects\C--Users-DrSto-OneDrive-Desktop-GBAdhoc\memory\MEMORY.md`.
Claude Code loads that index automatically when started in `C:\Users\DrSto\OneDrive\Desktop\GBAdhoc`.
Its key files for this work are `housekeeping-460-plan.md`, `rfu-rework-post-450.md`,
`post-450-priorities.md`, `autonomy-460-orchestrator.md`, `validate-the-player-path.md` and
`kirby-link-priority.md`.

## Ground rules (owner's)

- Build, test and use the rig freely. Ask before any push, merge to `main`, tag or release.
- Rig: never change a console's cmd, never kill processes by name, never open a COM port except
  through the Lab (COM6 is the servo rig, COM5 is a 3D printer).
- Only one build awaits hardware at a time. PPSSPP is the pre-gate; hardware decides.
- Fixtures must take the player's path (see `validate-the-player-path`).
- General fixes beat game-specific ones; never lock frame rates; drafts for people unless asked to post.

## Where things are

- Repo: git common dir `builds/github-main-inspect/.git`; worktrees under `builds/*-worktree`.
  Don't use isolation worktrees.
- **Beta branch:** `claude/460-dev` in `builds/dev460-worktree`, tip `3adb1ce` (beta.2 pin; beta.1 was `3683b73`).
  `beta.1` is built in `builds/beta-460`, with `GBAdhoc-4.6.0-beta.1.zip` and `BETA-NOTES.md`.
  The beta agent writes `builds/beta-460/STATUS.md`. Release command: `tools/release.sh`
  (see `docs/RELEASE.md`); check flags with `python gpsp/tools/check_flags.py`.
- **Lab:** labd on :8765, run from `builds/lab-linkui-worktree`:
  `python -u backend/labd.py --hw --no-github --rig-port COM6 --rig-enable --data C:/Users/DrSto/OneDrive/Desktop/GBAdhoc-Lab/data`.
  The portal password gate is `python -u backend/labgate.py`, run from `GBAdhoc-Lab` (port 8766). The
  Cloudflare tunnel runs on its own. New harness keys need the source tree listed in
  `backend/lab/inikeys.py` EXTRA_SRC, then a labd restart.
- **Rig recovery:** `POST localhost:8765/api/rig/<ch>/power-cycle {"confirm":"<console id>","force":true,"reason":"..."}`.
  ch 0 = 3000 `1d9f2997642a486a`, ch 1 = 1000 `4a535a84c04ac466`. Cancel the frozen job first.
  Lab builds have no crash screen, so a crash shows as a silent freeze.
- **Queue order:** Kirby jobs sit at positions over 6000 on purpose. Beta work goes first
  (see `builds/rfuref-out/tmgc_front.py`).

## In the beta (`claude/460-dev`)

Flag bake and the one-command release; link build-mismatch refusal; PSP-1000 memory cuts; the issue #28 sleep fix
(ME module kept across Quit to list) and the Go slider fix (both verified on the owner's Go);
scheduled ticks for the Wireless Adapter (`RFU_SCHED`, on for beta/nightly, 57 fps lock removed);
the CPU clock setting; `NV_COND_FIX` (issue #10); and `BIOS_BOUND_FIX` with `XLAT_LAST_TRY_LAZY`
(TMGC bad jumps, fixed on both consoles).

## Still to do, in order

1. **DONE 2026-11-03: PR #27 + route verbs merged, beta.2 built.** `SMC_STM_FIXED` validated on
   `claude/stm-fixed-beta` (gate 47 PASS / 0 FAIL, stale 0, corpus 30/35 identical + 5 timing; Golf
   INFRA-FAIL = interpreter replay dies on the base too, corpus lockstep says timing; HW ABBA TMGC
   47.5 -> 58.8 fps both consoles, others +-0.1), squashed as `1d0e54f` (Chandler co-author), merged
   `40e47d1`; `claude/link-ui-route` merged `3c373a5`; pin `3adb1ce` = pads 176/216 (layout vs beta.1:
   only frontend text/.bss moved, `builds/beta460-out/tools/nmdelta.py`). `tools/release.sh 4.6.0-beta.2`
   (run it WITHOUT MSYS_NO_PATHCONV) -> `builds/beta-460b2` (EBOOT 0f1adb8d, zip sha256 65ba736f...,
   BETA-NOTES.md). Release boot 90 s in PPSSPP clean. Installed on the owner's Go (G:, old build kept
   as *-c38f). NOT yet: Lab build of 3adb1ce, hardware smoke incl. `link_msc-connect` on the beta.
2. **Gate trust:** PPSSPP gate runs from 2026-11-02 until the BIOS was restored had no BIOS. Re-run
   the gate on the final beta (see `lab-correctness-gate`).
3. **Before pushing,** check the diff since `v4.5.0`: no ROM data, rig paths or lab-only code
   switched on.
4. **Owner's tests on `beta.2`:** sleep after Quit to list, the slider, normal sleep on a 1000 or 3000,
   222 MHz with ARK-4 set to Auto, a beta-to-beta wireless session, and `BETA-NOTES.md`.
5. **Push,** only on the owner's OK: a public `beta` branch plus a GitHub pre-release with the zip.
   `main` stays on 4.5.0.

## Not in the beta

Kirby link speed (`claude/kirby-460` tip `5b2872b`: HALT_HOP + HOP_DRAWN + end-of-frame link hash
are merge-ready, corpus 35/35, KAM 50.9 -> 51.9; DS_BATCH reaches 53.3 but fails 26/35 corpus ROMs and
needs a timing-exact redesign; KAM is 50.9 on 4.6.0-dev vs 51.6 on 4.5.0, unexplained);
`ROM_MIRROR_RELOAD_FIX` (`claude/rom-mirror-reload`; its Golf "regression" was the missing BIOS,
so re-check it); the full player-route fixture conversion; GBA-link pause handling (about 10 s in
the in-game menu loses a linked session); the Golf lobby race (narrowed, not gone).

## Open threads with people

- Issue #28 (Chandler): fixed and verified; he'll hear when the beta drops.
- Issue #10 / PR #26 (Chandler, MXPHunter): our NV fix was chosen. Post a build once `beta.2` is out.
- Issue #23: waiting on the reporter. KarnFu's #9 list: Mortal Kombat, FIFA replays, Payback, Falcon.
