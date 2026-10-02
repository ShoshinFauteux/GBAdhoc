# Display features: per-console profiles, integer 2x, ambient bars, loading art

2026-10-01, branch `claude/display-features` (from `claude/candidate-3.1`,
`d7df3bb`). Four player requests from the 3.0.0 Reddit thread. None of them
touches emulation. Validated in PPSSPP and the host suites; the hardware checks
are listed at the end and have **not** been run.

Look: Fable's chosen designs in `builds/menu-mockups/README.md`. The ambient
bars are **"soft"** and the loading screen is **"L2"**. Every look parameter
lives in `psp/ambient_look.h`.

## 1. Per-console display profiles

### Settings inventory

| per console (GBA / GB / GBC) | global |
|---|---|
| Video scale (`scale`) | Wireless: room code, session overlay, every `net_*` / `rfu_*` / `nd_*` key |
| Video filter (`filter`) | Theme, Menu style (`ui_shell`) |
| Ambient bars (`ambient`, new) | Fast-forward mode and button (`ff_*`), A/B swap, FPS counter |
| GB palette (`gb_palette`; GB and GBC only) | Sparse frameskip, ME / standby / blit / gu_defer, `rom_resident`, `loading_art` |

FPS counter and sparse frameskip stay global: the first is an overlay, the
second is a performance setting. There is no audio setting besides `ff_audio`.
The GB palette applies under GBC too, because a DMG cartridge running on CGB
hardware uses it. Note that in GB mode **Auto** also selects SGB hardware
(`set_gb_type`). That behaviour predates this branch; the profile only decides
which console's value is used.

### Format and migration (`psp/config_psp.c`, end of file)

```
scale_gba  filter_gba  ambient_gba
scale_gb   filter_gb   ambient_gb   gb_palette_gb
scale_gbc  filter_gbc  ambient_gbc  gb_palette_gbc
```

* **Migration:** each key that is absent falls back independently to the
  legacy `scale` / `filter` / `gb_palette` value. The first boot after the
  upgrade therefore looks identical, and nothing is written while loading. The
  first save writes the new keys.
* **Legacy keys are still written:** `scale` and `filter` come from the GBA
  profile and `gb_palette` from the GB profile, so an older build keeps its
  look. Harness `config.ini` files that set `scale = N` still work.
* **Runtime:** `g_pcfg.scale/filter/ambient/gb_palette` stay the live mirror of
  one profile, so every existing reader is unchanged. Writers call
  `pcfg_display_commit()`.
* **Live-console guard:** commits go to the console last passed to
  `pcfg_display_select()`, not to `g_pcfg.console`. Moving `g_pcfg.console`
  without a select cannot write one console's values into another's profile.
  `tools/test_display_profiles.c` tests this.
* **Context:** the browser's TRIANGLE switch selects the profile (inside
  `pcfg_remember_console`). In game, `main_psp.c` selects the running ROM's
  console, then applies `vid_set_mode`, `vid_ambient_mode` and the palette.
* The in-game TRIANGLE preset now saves through `pcfg_display_save()`. That is
  4 INI rewrites instead of `pcfg_save()`'s ~50, on the emulation thread.

**UI:** the Settings section is titled **DISPLAY (GBA)**, **DISPLAY (GB)** or
**DISPLAY (GBC)**, with the tag in that console's badge colour. Its rows are
scale, filter, ambient bars and GB palette. The palette row is greyed out
("GB / GBC only") and skipped under GBA. Theme and Menu style moved under a new
**INTERFACE** header. Under 2x the filter row reads "sharp (2x)".

The filter row has a third value, **sharp bilinear** (`filter_* = 2`), which
was added later on `claude/sharp-bilinear`. See
[SHARP-BILINEAR.md](SHARP-BILINEAR.md).

## 2. Integer 2x (`VID_SCALE_INT2 = 3`)

| source | drawn | rows shown | bars |
|---|---|---|---|
| GBA 240x160 | 480x272 (from 480x320) | 12..147 (12 cropped top and bottom) | none |
| GB/GBC 160x144 | 320x272 (from 320x288) | 4..139 (4 cropped top and bottom) | 80 px each side |

* **GB decision:** 2x with a 4-row crop. The largest uncropped integer scale
  is 1x (160x144), which is too small to be useful. Losing 4 rows top and
  bottom (2.8 %) gives sharp, even pixels. The bottom text box's lower border
  is clipped; Fit (302x272) is still there for anyone who minds.
* **Crop method:** the crop is done in the texture's v range (`vid_geo.sy0/sh`
  in `dest_geo`), not with off-screen vertices. Every output row is therefore
  exactly two copies of one source row, and no vertex goes negative.
* **Filter:** always GU_NEAREST, whatever the filter setting says.
* **Paths:** the same geometry applies to the CPU blit (`vid_draw_frame`), the
  ME blit (`vid_draw_prestaged`, used for all FF presets) and the wake overlay
  (`vid_image_screen`).
* **Not in the TRIANGLE cycle** (Fable's advice): it crops the GBA picture, so
  it is opt-in from Settings. From 2x, TRIANGLE goes to fit.
* **CONFIG.INI value:** 3. It was appended to the enum, not inserted. An older
  build clamps 3 to 1x.

## 3. Ambient bars

### Pipeline

1. **At ROM pick, before `art_free_all()`:**
   * Source: the browser's decoded hero when it belongs to the picked ROM (the
     Marquee usually has it). Otherwise the Marquee's own decode (`hero/` then
     `boxart/`, by file name) into the same buffer. A launch without the
     browser (harness, variant) decodes by file name inside
     `ui_loading_begin` and frees it before returning.
   * Bake (`amb_bake`): cover-crop to 480:272, then a 4x4 box average down to
     120x68, then one 3x3 box blur.
2. **Texture:** 120x68 inside a 128x128 RGB565 texture, **32 KiB in VRAM**
   directly above the staging buffer (offset 1,196,544 of 2,097,152). It is not
   BSS. On a PSP the BSS is taken from the user partition the heap comes from
   (`PSP_HEAP_SIZE_KB(-1024)`), so 32 KiB of BSS would be 32 KiB less for the
   PSP-1000's ROM-cache loop. VRAM costs the heap nothing, and the offset does
   not depend on `blit_mode`. If VRAM has no room, the bake returns -1 and the
   bars fall back to the palette shade. A ROM load never fails because of it.
3. **Each frame:** inside the blit's own display list, **in place of the
   clear**, one textured sprite is drawn per bar: 2 at Fit, 4 at 1x, 0 at
   Stretch and at GBA 2x. They use float UVs and GU_LINEAR. The scrim is the
   MODULATE colour (255 − alpha): 96 for `hero/` art, 150 for box art, 130 for
   a game frame. That is the same arithmetic as Fable's black rect, without a
   second blended fill. The bars cover fewer pixels than the clear did, and
   there is no per-pixel CPU work.
4. **Fallback with no art:**
   * Mode **"art"**: the console's `pal[0]` under a black vertical ramp from
     60 to 200 (Fable's palette fallback), drawn as one Gouraud strip per bar.
   * Mode **"art, else game"**: a game snapshot replaces the palette. It is the
     first frame at or after frame 180 with mean luma ≥ 24, retried every 60
     frames, giving up at 3600. It reads one tap per texel (about 8k pixels;
     uncached reads for the ME stage), and is retaken when the in-game menu
     closes (covering state loads and new areas). It is never taken per frame.
5. **Setting:** Ambient bars `off` / `art` / `art, else game`, per console,
   default **off** (`PCFG_AMBIENT_DEF`).

### Measured (PPSSPP; CPU-side only, so hardware numbers are needed)

| | value |
|---|---|
| bake, 480x272 hero | 19.1 ms, once, at ROM pick |
| bake, 240x160 snapshot | 7.9 ms, once (one frame) |
| launch art source: Marquee reuse / Shelf decode (.565) | 19 ms / 34 ms, before the first loading frame |
| `core_prof` core (AW2, five 600-frame windows) | base 5092–5208 µs; off 5092–5209; art (fit) 5095–5212; palette (1x) 5095–5214; snapshot (1x) 5109–5216; 2x 5092–5209 |
| `blit_prof` gu (list build) | off 12 µs; fit art +3 µs; 1x palette +6; 1x art/snapshot +7; 2x +0 |
| `blit_prof` wait | 4 µs in every arm (PPSSPP's GE is instant, so this number means nothing for hardware) |

## 4. ROM loading screen ("L2")

The background is the same 32 KiB ambient bake (`vid_ambient_image`), drawn
full-screen under a `C_BG_TOP` scrim of alpha 110, with the browser's marquee
chrome on top: wordmark, console badge, busy ring, hd title, and stage and
amount at y=102. The progress track is 440x3 and fills in the console's
colour. With no art, the background is the console's `pal[0]` ramp
(170→0 dark, 70→0 light) with the browser's cartridge. **The sharp hero is
never held**: the browser frees its art pool exactly where it did before
(L1 would hold 512 KiB through the load). `loading_art = 0` in CONFIG.INI is
read but never written; it skips the art decode and the bake, which is the A/B
switch for the heap proof. Harness: `loading_dump = N` GE-dumps the first N
loading frames.

### Memory proof (`heap_census`, PPSSPP, Emerald with hero art)

| layout | build / launch | pre_select | post_select | post_load free (largest) | ROM blocks / JIT |
|---|---|---:|---:|---:|---|
| 32 MiB (`harness`) | base, by path | 15,253,896 | 15,253,896 | 1,537,992 (966,656) | 13 / small |
| | this, by path, art on | 15,242,632 | 15,242,632 | 1,526,728 (966,656) | 13 / small |
| | this, by path, `loading_art = 0` | 15,242,632 | 15,242,632 | 1,526,728 (966,656) | 13 / small |
| | base, browser (Shelf and Marquee) | 15,253,896 | 15,253,896 | 1,537,928 (966,656) | 13 / small |
| | this, browser Shelf (decodes) and Marquee (reuses) | 15,242,632 | 15,242,632 | 1,526,664 (966,656) | 13 / small |
| 64 MiB (`harness64`) | base, by path | 48,808,328 | 48,808,328 | 23,033,768 (22,056,960) | 16 resident / large |
| | this, by path, art on and off | 48,797,064 | 48,797,064 | 23,022,504 (22,048,768) | 16 resident / large |
| | base, browser | 48,808,328 | 48,808,328 | 23,033,704 (22,056,960) | 16 resident / large |
| | this, browser Shelf and Marquee | 48,797,064 | 48,797,064 | 23,022,440 (22,048,768) | 16 resident / large |

* **The launch path costs 0 bytes.** Within this build, `post_select` equals
  `pre_select` byte for byte on every launch path (the by-path decode, the
  Shelf decode and the Marquee reuse). `post_load` is identical with
  `loading_art` on and off. ROM blocks and the JIT tier are unchanged.
* **The branch as a whole costs 11,264 B at both layouts.** The difference
  already exists at `pre_select`, before any launch code has run, so it is the
  frontend image's code and rodata growth (the same effect any feature commit
  has), not anything allocated at run time. The largest block after load is
  unchanged on the 32 MiB layout (966,656).

## Emulation unchanged (`shash`, per-frame guest state)

* **AW2 fixture** (`rig-stage-hnsdiag`, `aw2.st0` plus the `aw2.txt` tour,
  3000 frames): features off, art on (Fit), snapshot (1x), palette and 2x are
  all identical (`fe624cba9551`). The base build matches on every frame line;
  only the header's EBOOT CRC differs.
* **H&S fixture** (`hns.st0` plus `hns.txt`, 900 frames): frames 31–900 are
  identical in base, base again, off and on. Frames 1–30 run before the state
  load at frame 30, and the RTC is seeded from the wall clock, so they vary
  from run to run: the two base runs differ from each other there in exactly
  the same way.

## Tests

* `python tools/run_host_tests.py`: 30/30 suites. `rig_pc` failed once on a
  timing assertion in the PC rig tooling, which this branch does not touch,
  then passed 2/2 on rerun.
* New: `tools/test_display_profiles.c` (in the `ff` runner, because the suite
  count is capped at 30) and `tools/test_video_geometry.c` (in
  `video_buffers`). They cover migration, isolation, the live-console guard,
  clamping, the 4-rewrite save, every scale's geometry for GBA and GB, 2x
  forcing nearest, and the TRIANGLE cycle.
* PPSSPP rig: `tools/display-rig/` (`run.sh` plus setups; `shash_mine.sh`,
  `heap.sh`, `heap2.sh`, `shots.sh`). Set `DISPLAY_RIG_DIR`. Every run is
  `unshare`-isolated.
* Screenshots: `docs/img/display/`.

## Hardware checks (not run; the main session owns the consoles)

Use the `harness` build on the **PSP-1000** with the ME on (the default) and
Emerald plus its `hero/` art.

1. **Frame cost, ambient off vs on.** AW2 fixture plus the `aw2.txt` tour,
   same EBOOT, Fit then 1x, with config `ambient_gba = 0` against
   `ambient_gba = 1`. Compare `EVT blit_prof` `gu=` and `wait=`, and
   `EVT core_prof`, over five 600-frame windows. Expected: `wait` is the same
   or lower with ambient on (fewer bar pixels than the clear they replace),
   and `core` is unchanged. Repeat at 1x with `ambient_gba = 2` and no art,
   and record the one `ambient_bake src=game us=` line (the snapshot hitch).
2. **Bake time:** the `EVT ambient_bake src=art us=` and
   `launch_art_source us=` lines on the 1000, from a Shelf pick (decodes) and
   a Marquee pick (reuses).
3. **Heap:** `heap_census at=post_load` and `rom_cache blocks=` match a
   3237335 build minus 11,264 B, and are identical with `loading_art = 0`.
   Repeat on a 3000 or Go with `harness64`: Emerald stays resident.
4. **Picture:** 2x on GBA (no bars, rows 12..147, crisp) and on GB/GBC (80 px
   bars). Ambient bars at Fit and 1x. The loading screen on art and no-art
   ROMs. Check for no tearing or flashing at bar edges during FF (all three
   presets) and on wake from standby (the wake frame uses the 2x crop).
5. **VRAM:** confirm `ambient_bake ... rc=0 vram_off=1196544` on every model.
   An `rc=-1` would mean less eDRAM than assumed; the bars then show the
   palette shade.

## Notes

* **Merge with `claude/control-remap`.** The overlaps are: the settings row
  enum and table in `ui_psp.c`; the TRIANGLE handler in `main_psp.c` (2 lines);
  the profile code appended at the end of `config_psp.c` and `config_psp.h`;
  and two fields added after `gb_palette` in `psp_config`. The harness keys
  `ui_browser_script` and `gedump_every` are ported from `claude/readme-3.0`
  (commit 3237335) and will conflict trivially with that branch.
* **Pre-existing hazard, not fixed here.** In `hero_get`, if the 512 KiB
  allocation fails and the 256x256 fallback is used, box art is fitted to a
  256x358 box and writes 358 rows into a 256-row texture. The ambient bake
  clamps the rows it reads, but the overflow itself is in the browser's own
  path.
* **A sharp L1 loading screen** would also cost no heap if its copy went into
  VRAM, since there is about 800 KiB free above the ambient texture. That is
  an option if the owner ever prefers L1.
