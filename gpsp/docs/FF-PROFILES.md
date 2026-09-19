# Three fast-forward profiles (2026-09-14)

The preceding **GBAdhoc FF Fix 1** build was reported artifact-free by the
PSP-3000 owner. This change retains its LCD-buffer ownership selection and
GE-before-ME-reuse synchronization. See [the fix record](FF-ARTIFACT-FIX.md).

## User-facing profiles

| Setting and OSD | Policy |
| --- | --- |
| 3x (default) | Preserve former 3x Smooth: three emulated frames per paced host iteration, submit only the final capture. Target roughly 180 emulated / 60 rendered where hardware permits. |
| Unlimited | No vblank pacing or fixed render cadence. Emulation continues while the ME is busy; submit the next capture when it is available. Present only newly completed stages. |
| Unlimited Smooth | No vblank pacing. Capture and render every emulated frame; wait for the preceding ME render before posting another. Rendering capacity limits sustained speed. |

"Every frame" describes rendering, not the physical LCD: the panel cannot
display more unique frames than its refresh rate. The async pipeline can have
one render outstanding; the OSD's completed-render counter can therefore lag
emulation by one at a sampling boundary.

## Implementation and boundaries

- `config_psp.c/.h`: one shared mode/name mapping. Keep legacy INI keys and
  struct layout: `30/1`, `0/0`, `0/1` for `ff_mult_x10/ff_smooth`. Former capped
  selections (including 1.5x, plain 3x and retired 2x) map to 3x. Max/Max Smooth
  map to the two unlimited modes. A legacy Max file without `ff_smooth` remains
  Unlimited; an empty configuration defaults to 3x.
- `ui_psp.c`: exactly three choices, cycling in either direction; the OSD uses
  the same names. The built-in font displays the multiplier as `3x`.
- `main_psp.c`: remove the separate synchronous 33 ms cadence and its counters.
  All ME modes use the existing capture/retire pipeline. Unlimited does not
  wait for a busy render; Smooth has a 50 ms hard ceiling, matching the existing
  input-handshake ceiling, then falls back to the CPU on a stuck ME. The usual
  9 ms late-frame policy remains unchanged for 3x and normal play.
- The input-consumed handshake remains mandatory before emulation can modify
  the live VRAM/OAM/palette arrays. "Unlimited" does not bypass memory safety.
- Unlimited checks the heartbeat watchdog at most once per 16.667 ms, so many
  tiny guest frames during a valid render do not create a false failure.
- Switching profiles while FF is toggled on reapplies core frameskip and the
  OSD; ME failure also reapplies the CPU fallback policy on the next iteration.
- With the ME unavailable, Unlimited retains alternate-frame CPU rendering
  for emulation headroom and avoids blitting duplicate frames. Smooth disables
  core frameskip and blits every rendered frame. The old 1-in-32 CPU display
  sampling is restricted to harness FF; it does not apply to user profiles.
- Wireless-session FF interlock, hold/toggle behavior, FF audio option, normal
  frameskip policy, and harness benchmark pacing remain in place.

## Validation

Host config/pipeline tests and the 64,000-iteration display-buffer regression
pass. The clean pinned-toolchain PSP build exits successfully, with the same
73 warnings as FF Fix 1 (normalized for source line numbers), no new warnings,
and no `stubs out of order` warning. The PSP-3000 owner subsequently reported
that the three modes worked correctly and the artifact was completely gone.
The second PSP has not yet been validated for this change.

Installed as **GBAdhoc FF Profiles** at `F:/PSP/GAME/GBADHOC-FFPROFILES` on
the 128 GB PSP-3000. All 163 destination files passed hash verification; all
355 files checked across the original and FF Fix 1 folders were preserved.
Settings and saves were copied from FF Fix 1. Its last selected Max profile
migrates to Unlimited; the default for a fresh configuration is 3x.

Run `python3 tools/run_ff_tests.py` with a native GCC. The config test uses the
real INI reader/writer to cover defaults, all six previous presets, old/invalid
values and persistence for each new profile. The pipeline test compiles the
actual scheduling/presentation functions from `main_psp.c` with a fake ME;
there is no duplicate scheduling implementation. Its 12 ms render exceeds
the old 9 ms budget and checks:

- Unlimited returns promptly while busy, without reusing the pending capture.
- Ready renders are retired/replaced and duplicate GPU blits are suppressed.
- Switching into Smooth with a render outstanding still posts every frame.
- Smooth submits 100 consecutive frames without a render drop.
- 3x retains its prior late-render behavior.
- A stuck ME exits Smooth through the bounded fallback.

Also rerun `tools/test_video_buffers.c` using the SDK headers as described in
the artifact-fix record. Physical speed, visual smoothness and continued lack
of black artifacts require PSP testing of this new build.

## Build and hardware comparison

Use the pinned toolchain and flags in the artifact-fix record, changing only:

```sh
# Core build identifier (checkpoint plus uncommitted profile patch):
GIT_VERSION=ecdd8bc-ffprofiles1
# Frontend package title:
PSP_EBOOT_TITLE='GBAdhoc FF Profiles'
```

Install in a new `GBADHOC-FFPROFILES` folder with independent saves copied from
the user's artifact-fix test. Compare the same Heart & Soul town in all three
modes, including FF toggles, changing the selection while toggled on, settings
persistence after restart, normal play, and scene transitions. Keep the FF Fix 1
copy as the control, especially for 3x's approximately 180/60 target.

Package SHA-256:

- EBOOT.PBP: `fdb8d18441996ac6f7ce7110abd3d7a404e56a7aee03f0409aefa54a558a2307`
- gbadhoc_me.prx: `7612a2e1425150ad32006025a43c4252dff6eb1355164de18a9abb19243ef098`
  (unchanged from FF Fix 1).
