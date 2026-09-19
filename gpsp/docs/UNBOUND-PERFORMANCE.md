# Pokemon Unbound hardware performance decision

**Status:** accepted on PSP-1000 hardware, 2026-09-15. The validated candidate
is `sprt17b` (`EBOOT.PBP` SHA-256
`4ccba45ee82aabae94f2147c689e23e9d705dfc61d811911dcf6d33e39bcd7fc`).

## Decision

The release path combines four measured changes:

1. Keep exact block extents for translated RAM code and retire only blocks that
   overlap a proven self-modifying write range.
2. Give RAM blocks stable entry thunks, but let ordinary RAM-to-RAM branches
   link directly. A retired block redirects its thunk to the dispatcher; a
   rebuilt block reuses the same entry.
3. Feed the Media Engine renderer through its dirty-page VRAM mirror. Do not
   make the main CPU copy the full 98 KiB VRAM/OAM/palette shadow every frame.
   A successful savestate load dirties every VRAM page because it replaces VRAM
   without passing through the normal store and DMA markers.
4. Present completed Media Engine frames at loop top. This overlaps the GE
   texture read with the next emulated frame. Same-frame presentation remains
   a harness-only diagnostic because issuing its GE list immediately before
   swap costs a heavy title roughly 1.5 ms each frame.

The dynarec activation is based on the observed self-modifying write layout,
not a ROM title or game code. Titles that do not exhibit that layout remain on
the established full-cache invalidation path.

## Hardware result

Every score below is the same 3,600-frame scripted battle on the PSP-1000. The
reported rate includes the expensive opening transition into battle.

| Candidate | Unbound FPS | Mean work/frame | Heart & Soul FPS |
|---|---:|---:|---:|
| Safe partial runtime (`sprt09`) | 50.853 | 17.921 ms | — |
| Stable thunk + direct RAM links (`sprt12`) | 53.504 | 16.954 ms | 59.812 |
| Final renderer pipeline (`sprt17b`) | **59.366** | **16.276 ms** | **59.810** |

Unbound's steady combat windows run at approximately the GBA's full rate; the
aggregate is lower because it deliberately includes the initial battle
transition. Compared with `sprt09`, the final candidate improves the complete
fixture by 16.7%. Heart & Soul remains full speed.

The final hardware captures match the golden images exactly:

- Unbound: `d6331c6c...` at frame 91 and `ce1a073d...` at frame 3901.
- Heart & Soul: `b70cc26...` at frame 91 and `19248f4a...` at frame 3901.

The bounded PPSSPP oracle also matches every generated audio sample:

- Unbound: `25146ed5`, 2,156,388 samples.
- Heart & Soul: `0d32030f`, 2,156,387 samples.

## Rejected paths and traps

- **Selective thunks only inside the detected patch range:** exact, but Unbound
  fell to about 49.98 FPS. Stable thunks help Allegrex branch and instruction
  fetch behavior even for blocks that are not later retired. The rejected
  implementation is not retained in the source.
- **Full Media Engine input shadows:** copying 98 KiB on the main CPU costs more
  than the short input wait and bypasses the dirty-page path. Keep
  `me_shadow=0`.
- **Same-frame presentation:** exact, but Unbound falls to about 56.64 FPS
  because the late GE work cannot overlap the CPU. The loop-top pipeline adds
  one displayed frame of latency and is the measured performance winner in
  both fixtures.
- **Adaptive presentation:** rejected. Periodic probing scored 58.48 FPS;
  work-time selection scored 58.62 FPS and both games tripped during heavy
  startup frames. The additional hot-loop code also raised Unbound's mean work
  time. A fixed pipeline is simpler and faster.
- **Dirty pages after state load:** a state load replaces VRAM in bulk. Failing
  to invalidate the mirror produced a black background in the opening capture
  even though the final frame was correct.
- **PPSSPP timing:** use PPSSPP for exact frame/audio oracles, never to claim PSP
  speed. Performance decisions in this record come from the physical PSP-1000
  harness.

## Release rule

Build the PSP core with `SMC_GATES=1`, `SMC_GATES_SIMPLE=1`,
`SMC_GATES_RANKED=1`, `SMC_GATE_BITMAP=1`, `SMC_PARTIAL_SAFE=1`,
`SMC_PARTIAL_STABLE_THUNK=1`, and `SMC_PARTIAL_DIRECT_LINKS=1`, together with
the existing `GBA_PC_MASK=1` and `BADJUMP_SAFE=1` guards. A release candidate
must pass both frame/audio oracles and both physical-hardware fixtures before
these defaults change.
