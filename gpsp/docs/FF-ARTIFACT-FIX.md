# FF display-buffer ownership (2026-09-14)

## Status and scope

Candidate hardware fix on `codex/mystery-gift-discovery`, based on `ecdd8bc`.
The host regression reproduces a display-buffer ownership violation in the
checkpoint and passes with the fix. The user reports **no artifacts** on the
PSP-3000 test build. This validates the combined fix in their play session;
separate attribution to each race and validation on the second PSP remain open.
The clean pinned-toolchain build passed with the same 73 warnings as the
reproduced checkpoint (normalized for line numbers), and no import-order warning.
Installed on the 128 GB PSP-3000 at `F:/PSP/GAME/GBADHOC-FFFIX1` as
**GBAdhoc FF Fix 1**. All 163 destination files were hash-verified; all 355
existing files across the original and Rebuild folders remained unchanged.
The copied configuration selects 3x Smooth, with independent ROM/save copies.
All six existing fast-forward presets and their pacing/render policies remain
unchanged for this test. The agreed three-profile UI is a subsequent change.

## Findings and rules

1. **Three display buffers require ownership tracking.** The old modulo-three
   rotation can clear/draw into the current LCD buffer when several swaps occur
   before vblank (uncapped FF or paced catch-up). `g_fb_filled` prevents showing
   an unfilled buffer, but does not prevent writing the displayed buffer.
   After successfully submitting a NEXTFRAME flip, query the current display
   address with IMMEDIATE and exclude both it and the submitted buffer. Query
   after submission so an older pending flip cannot invalidate the choice.
   Normalize cached/uncached VRAM aliases. A failed submission retains the frame;
   a failed query waits for the successful submission to latch before reuse.
   The normal path adds no vblank wait and does not cap emulation to LCD refresh.
2. **ME output completion and GE texture-read completion are separate.** At loop
   top, presentation queues a read of ready stage A. Retiring stage B then makes
   A the next ME output. Before posting that render, flush queued GE reads in
   `me_rend_fill_desc`. Otherwise the ME can overwrite pixels still being sampled.
   This may cost a remaining GPU wait, which needs measuring on hardware.
3. **Do not replace the existing queue wait based on the old hypothesis.** The
   pinned SDK's disassembly shows `sceGuSync(0, 0)` calls `sceGeDrawSync(0)`;
   it is not a wait for just one of the two GU lists. Keep the pre-swap flush.

Both races are consistent with artifacts, but a host simulation cannot establish
which one caused every black line reported on a physical PSP.

## Regression

`tools/test_video_buffers.c` includes the actual `video_psp.c`; it does not copy
the buffer-selection algorithm. A simulated LCD independently retains current
and next-vblank buffers. Assertions reject draw targets owned by either one.
It exercises 64,000 iterations across 64 timing/alias combinations, skipped
frames, zero/one/two pending GE lists, failed submission, and failed query.
The original code fails `g_fb_cur != lcd`; the fixed code passes. This test
covers display-buffer ownership, not ME execution timing or physical scanout.

With a native GCC and PSP SDK headers available:

```sh
gcc -std=gnu99 -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
  -I"$PSPSDK/include" -Ifrontend-common tools/test_video_buffers.c \
  -Wl,--gc-sections -o /tmp/test_video_buffers
/tmp/test_video_buffers
```

On a 64-bit host, the SDK and existing VRAM readback code produce expected
32-bit-address-to-pointer warnings; this test never dereferences PSP addresses.

## Reproducible PSP build

Toolchain image (also used to reproduce the installed checkpoint):
`pspdev/pspdev@sha256:b22811072ea0721d7f6c666a5a279b6def2b0cd95892b819d9570f8ffc4452c0`.
Build in a fresh source export to preserve the installed checkpoint artifacts.

```sh
make -C psp clean
make platform=psp1 clean
make platform=psp1 GIT_VERSION=ecdd8bc-fffix1 \
  SMC_GATES=1 SMC_GATES_SIMPLE=1 SMC_GATES_RANKED=1 \
  GBA_PC_MASK=1 BADJUMP_SAFE=1 -j4
make -C psp EXTRA_DEFS='-DGPSP_PLAYABLE -DVID_TRIPLE' \
  PSP_EBOOT_TITLE='GBAdhoc FF Fix 1'
```

`ecdd8bc-fffix1` identifies the checkpoint plus this uncommitted test patch;
it is not a new commit hash. Check both make's exit status and the log for
`stubs out of order` before deploying. Ship `psp/EBOOT.PBP` with
`psp/me/gbadhoc_me.prx` (not the stale untracked `psp/gbadhoc_me.prx`).

## Hardware acceptance

Use a separate `GBADHOC-FFFIX1` game folder, titled **GBAdhoc FF Fix 1**, with
its own copied settings, ROMs and saves. Keep the original and Rebuild folders.

- Heart & Soul: same town/save, 3x Smooth, sustained vertical scrolling and
  indoor/outdoor transitions. Compare artifacts and emulated/drawn FPS against
  the Rebuild control; the desired performance remains roughly 180/60 where
  the control achieves it.
- Repeat with Max, Max Smooth, and the other existing presets. Toggle FF on/off
  repeatedly and check for black bands, stale frames, or freezes.
- Check normal-speed rendering, settings/game menus, and suspend/resume.
- Test the 32 GB unit separately before treating this as verified across both
  devices; include a PSP-1000 when validating the smaller-memory target.

Do not attribute an FPS gain or complete artifact resolution to this patch
until the hardware comparison is recorded here.

Test package SHA-256:

- EBOOT.PBP: `2ea26a4ed976b7f8ed59e1ec0173c7c8012f69187fce07311b7a4ea6414714d1`
- gbadhoc_me.prx: `7612a2e1425150ad32006025a43c4252dff6eb1355164de18a9abb19243ef098`
  (unchanged from the reproduced checkpoint).
