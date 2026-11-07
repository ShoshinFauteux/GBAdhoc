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
| Video scale (`scale`) | Wireless: room (`group`, shown as "Room 7"), session overlay, every `net_*` / `rfu_*` / `nd_*` key |
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

### Grid filters: LCD grid, dot grid, scanlines (`filter_* = 3, 4, 5`)

Based on a community patch by u/Far_Froyo_6317 (GBAdhoc 4.1.0 Filters).
The picture is drawn nearest, and then one small
repeating alpha mask is blended over it (`grid_draw` in `psp/video_psp.c`).
The patch repeated its mask every 2 SCREEN pixels, which only lined up at 2x.
Here the mask's period is **one game pixel** on both axes. Its texture
coordinates are the picture's own source coordinates × 16, on the same
60-pixel strips the picture uses, so each period lands where the GE drew
that game pixel.

Each game pixel gets a darkened line at its right edge (columns) and bottom
edge (rows). The line is one screen pixel wide, but never more than half a
game pixel, so the picture is equally dark at every scale. LCD grid darkens the union of the two, dot
grid their crossing, and scanlines the rows only. The alphas are the patch's
(0x38, 0x68, 0x58).

| scale | GBA 240x160 | GB/GBC 160x144 | mask |
|---|---|---|---|
| 1x | 1:1 | 1:1 | **none**: the picture is plain nearest and Settings reads "(off at 1x)" |
| Fit | 1.7 x 1.7 | 1.89 x 1.89 | soft (LINEAR) both axes |
| Stretch | 2 x 1.7 | 3 x 1.89 | hard columns, soft rows |
| 2x | 2 x 2 | 2 x 2 | hard (NEAREST); same as the patch's 2x pattern |

* **Integer axis (k = 2, 3):** the last screen pixel of each game pixel is
  darkened, crisp. The profile's plateaus are several texels wide, so it
  stays exact when the other axis makes the sampling LINEAR.
* **Fractional axis:** a line half a game pixel wide, box-filtered over each
  screen pixel (darkness = the fraction of the pixel on the line). These
  fractions add up to the same amount per game pixel at any phase (0.85 px
  at 1.7x), so every line is equally dark overall; a line is only soft where
  it falls between two pixels. A crisp
  NEAREST mask cannot do this at 1.7x: a game pixel there is 1 or 2 screen
  pixels wide, so the narrow ones would go fully dark.
* **1x has no mask** because a game pixel is a single screen pixel. Any mask
  there darkens whole game pixels; the patch's mask darkened every other one.
  The same applies to 2x of a source too wide to double, which is drawn 1x
  (no GBA or GB picture is).
* **Every picture path** gets the mask: the CPU and ME blits (so all FF
  presets and rewind), the in-game menu backdrop and the wake overlay
  (`vid_image_screen`). The translucent backdrop (alpha < 255) does not.
* **GE state:** `grid_draw` leaves blend off, TexWrap CLAMP and TexFunc
  REPLACE/RGB, as `game_draw` did. Every later textured draw sets its own
  texture, mode and filter.
* **TRIANGLE:** since filters v2 (below) TRIANGLE cycles the scale and the
  smoothing only. The effect stays on and the toast names it: "fit, LCD grid".
* **CONFIG.INI:** appended values. An older build reads 3 to 5 as bilinear.
  The per-console profiles store them like any other filter.
* **Cost:** one textured, alpha-blended pass over the picture rect, using a
  16x16 texture that stays in the texture cache. That is 110,976 px at GBA
  Fit, 130,560 at GBA Stretch and 2x, 82,144 at GB Fit, and 87,040 at GB 2x.
  At the 4 to 6 ns/px calibration in SHARP-BILINEAR.md, that is about
  +0.45 to +0.8 ms of GE time per presented frame. Playable builds defer the
  sync (`gu_defer = 1`), so this lands on the otherwise idle GE. CPU list
  building is +7 to 8 µs per frame (PPSSPP, Emerald: `blit_prof gu` 12 →
  19 to 20 µs at Fit, Stretch and 2x). The one-off mask build on a mode
  change is about 200 µs. PPSSPP cannot show GE time, so
  this is an estimate until it is measured on hardware.
* **Tests:** `tools/test_video_geometry.c` simulates the GE's sampling per
  axis at every scale. It checks 1x has no mask, integer scales give exactly
  the last pixel, fit and stretch give the same line per game pixel, 2x is
  texel-for-texel the patch's pattern, and the presets.

### Filters v2: Smoothing, Screen effect, Colour (`claude/filters-v2`)

The single filter cycle became three rows per console profile. Ideas, but
no code, come from the community package "GBAdhoc LCD Filters Test 4", which
was built on 4.1.0.

| row | GBA | GB / GBC | where |
|---|---|---|---|
| Smoothing | nearest, bilinear, sharp bilinear | same | GE (`g_filter`) |
| Screen effect | none, LCD grid, dot grid, scanlines, vintage LCD | the same, plus RGB stripes and RGB stripes strong | GE (`g_fx`, `grid_draw`) |
| Colour | native, GBA LCD colour, black & white | the existing GB palette row | core (`video.h` PRESENTATION COLOUR) |

**CONFIG.INI.** New keys are written beside the old ones:
`smooth_<c>`, `effect_<c>` and `color_gba`. `filter_<c>` and the legacy
`filter` are always `vid_filter_join(smooth, effect)`. That join gives the
grid if one is on (vintage and RGB stripes join to LCD grid), else the
smoothing, so an older build reading the file shows the nearest equivalent.
Loading trusts the rows only while `filter_<c>` still equals their join. An
older build that changed the filter rewrote only `filter_<c>`, so then the
filter wins and is split. The split maps 0..5 exactly: 0..2 are the
smoothing with no effect, and 3..5 are nearest plus that grid. Code that sets
only `g_pcfg.filter` is obeyed the same way on commit.
`tools/test_display_profiles.c` 6d covers this.

**Effects, all on the game-pixel mask above.**

* **Vintage LCD:** the LCD grid's shape at alpha 0x15, plus the package's warm
  tone. Its modulate (F8,EE,E5) and veil (DC,D0,B8 at 0x10) fold into one
  untextured `GU_FIX` blend, `out = dst*(E8,DF,D7) + (0E,0D,0C)`. At 1x the
  grid draws nothing and the tone still applies ("tone only at 1x").
* **RGB stripes:** the game pixel is split into R, G and B columns (the
  package's x3 alphas: tint 20 / dark 48; strong 48 / 56), with the LCD
  grid's row line. A stripe only lines up where a game pixel is an exact 3+
  screen pixels wide, which is GB/GBC Stretch (480/160). No GBA scale
  reaches that, so GBA is not offered the effect. On GB/GBC at Fit, 1x and 2x
  nothing is drawn and the row says "(Stretch only)".
  `tools/test_video_geometry.c` checks that every screen column at GB
  Stretch samples exactly stripe X mod 3, in step with every game pixel.
* **Dropped:** "dense RGB" / "dense color+" differ from x3 only in the panel
  row period, and on a game-pixel mask the period is always one game pixel,
  so they are the same picture. "Micro RGB", "micro fine", "dots tight" and
  "dots tight RGB" are sub-game-pixel patterns that need 3+ screen pixels on
  both axes, which no scale has; at 2x their checkerboard is the dot grid's
  corner. The package's colour presets (GBA colour / B&W, with or without
  tight dots) are the Colour row combined with any effect.
* Effects draw over sharp bilinear too (`sharp_draw` ends in `grid_draw`).

**Colour.** It is transformed where a colour enters the renderer: the
renderer's copy of the converted palette, and the direct-colour pixel of
modes 3/5 (`video_color_direct`, out of line). Guest memory, including the
core's `palette_ram_converted`, keeps the game's values. The model is
`video_color.h`. GBA LCD colour is gpSP's `gba_cc_lut` (64 KiB `.rodata`
already linked, so nothing new). Black & white is BT.601 luma on the 5-bit
channels (77/151/28, neutral).

* *CPU renderer* (PPSSPP, ME fallback): `update_scanline` brings a
  transformed copy up to date with a 1 KiB diff per line (only changed
  entries are transformed), swaps it in for the render and swaps the core's
  back. The two 1 KiB copies are a frontend array (`g_vc_work`), so the
  core's `.bss` is unchanged. The new globals are `.sbss`.
* *Media Engine:* the render desc grew three appended fields guarded by
  `ME_COLOR_MAGIC`, with no version bump: the mode and the address of the
  EBOOT's own `gba_cc_lut`. The engine transforms its palette snapshot in
  place and the log's palette values as it replays them
  (`VIDEO_COLOR_IN_REPLAY`, engine only). It invalidates the LUT once per
  address. An older module ignores the fields (the picture stays native), and
  this module under an older EBOOT finds no magic (native).
* *Caveat:* because the transform happens before GBA blending and
  brightness, a blended or faded pixel can differ by a step from
  transforming the finished picture. Solid colours are exact.
* *Live behind the menu:* the frame on screen was drawn from inputs that
  still exist: its capture, the line-160 palette and OAM (`video_me_log.h`,
  or the input shadows) and the engine's VRAM mirror. `psp_color_preview`
  has the engine redraw it into the stage that is not shown. The first
  redraw is in the frame's own mode and must reproduce the engine's checksum
  for that frame, or nothing is used. Then a second redraw in the new mode
  becomes the backdrop. Without the engine, or if the check fails, a native
  snapshot is transformed on the CPU; otherwise the colour shows on resume.

**Rig keys** (harness builds): `fx_scale`, `fx_smooth`, `fx_effect` and
`fx_color` force the rows for every console. They reach the video layer
only and are never saved. `-DFX_SWEEP` (FE_EXTRA, rig builds) walks
display arms 300 frames each and prints `EVT fxcost` per arm.

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
   bars show the edge glow. A ROM load never fails because of it.
3. **Each frame:** inside the blit's own display list, **in place of the
   clear**, one textured sprite is drawn per bar: 2 at Fit, 4 at 1x, 0 at
   Stretch and at GBA 2x. They use float UVs and GU_LINEAR. The scrim is the
   MODULATE colour (255 − alpha): 96 for `hero/` art, 150 for box art. That
   is the same arithmetic as Fable's black rect, without a second blended
   fill. The bars cover fewer pixels than the clear did, and there is no
   per-pixel CPU work.
4. **The edge glow** (mode **"game"**, and mode **"art"** with no art):
   * Every 2nd presented frame (`AMB_GLOW_EVERY`), 12 rows (`AMB_GLOW_BANDS`)
     spread down each side edge of the drawn picture are read: 4 pixels per
     row (x 0,1,4,5 on the left, w-6,w-5,w-2,w-1 on the right) as two 32-bit
     reads, 48 reads in all, uncached for the ME stage. The bands are softened
     [1 2 1] and eased in by 1/4 per sample (`AMB_GLOW_EASE`), so a scene
     change crosses in about 8 frames without flicker.
   * The state (2 x 12 x RGB, 8.8 fixed point, 144 B) lives in a row of the
     ambient texture the bake never writes (`AMB_GLOW_ROW`): no BSS, no heap.
   * Each side bar is one Gouraud strip, a vertex pair (inner edge, outer
     edge) per band: no texture, no blend. The inner edge carries the colour
     under a 100 scrim (`AMB_GLOW_SCRIM`), the outer edge 72/255 of that
     (`AMB_GLOW_OUTER`). At 1x the top and bottom bars blend the two sides'
     end bands and fade to the outer brightness at the screen edge.
   * The in-game menu's backdrop seeds the glow from its own picture, so a
     mode picked there shows at once behind the menu.
5. **Setting:** Ambient bars `none` / `art` / `game`, per console
   (`bars_<c>`), default **none** (`PCFG_AMBIENT_DEF`). Art keeps the baked
   art in the texture whatever the mode, so art -> game -> art returns to it
   in the same session.

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
