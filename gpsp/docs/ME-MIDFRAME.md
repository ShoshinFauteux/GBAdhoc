# Mid-frame state on the Media Engine renderer

Status 2026-09-29, branch `claude/me-midframe`: implemented behind two build
flags, validated on the desktop model, **hardware A/B pending**.

A player report (GitHub issue, GBAdhoc 3.0.0) listed broken graphics in Driver 2,
Tomb Raider, Penny Racers, OutRun and Mario Golf: Advance Tour, with an analysis
blaming the Media Engine renderer's single graphics-memory snapshot and its
line-0 affine seed. This file records which parts of that analysis the code
bears out, what was reproduced, what the branch changes, and what it costs.

## The report's claims, checked against the code

| # | claim | verdict | evidence |
|---|---|---|---|
| 1 | Not the BIOS | **consistent, not independently audited** | Every divergence reproduced below is explained by the ME contract and disappears with the fix, with no BIOS change. Mario Golf gives the same numbers (1558 wrong frames before, 2 after) with the official BIOS and with the built-in open BIOS (`--option gpsp_bios=builtin`); the synthetic ROMs use the open BIOS. The open BIOS's LZ77/Huffman were not audited. |
| 2 | HBlank IRQs fire correctly; scroll writes are in the per-line log | **true** | `main.c` raises `IRQ_HBLANK` on every line's HBlank transition (visible lines and VBlank alike); HBlank DMA runs on lines 0-159 only, as on hardware. `me_capture_frame.ioregs[160][64]` copies halfwords 0x00-0x3F of I/O (bytes 0x00-0x7F): DISPCNT, BGxCNT, BGxHOFS/VOFS, BG2/3 PA-PD and X/Y, windows, mosaic, blending. |
| 3 | Affine reference only on line 0 | **true** | `write_io_register16` reloads `affine_reference_x/y` when BG2X/BG2Y/BG3X/BG3Y are written (`gba_memory.c`). The capture recorded them only at `vcount == 0` (`affine_seed`), and capture mode 1 returned before the PB/PD step. The engine started from the seed and stepped on its own; the renderer reads the counters, not the registers, so a reload in HBlank never reached it. PA-PD are in the log. |
| 4 | One VRAM/OAM/palette snapshot per frame | **true** | `me_render_glue.cc` phase 1 copies VRAM (dirty pages), OAM and palette once per post, and with `me_vispost` (default) the post is `gpsp_visible_done_hook` at vcount 160, after line 159's HBlank DMA and IRQ. All 160 lines replay against that copy. |
| 4a | ...so mode-4 back-page drawing mid-frame flickers | **false as stated, true in a variant** | The page select is a DISPCNT bit and is in the line log; drawing the page that is *not* displayed is invisible at either snapshot. The synthetic `m4_back` case matches on every frame. What does break is drawing into the page that *is* (or was, earlier in the same frame) on screen: `m4_front` diverges on every frame. Separately, the fast-forward profiles still post at frame end, and there `m4_back` is wrong every frame (the VBlank flip and redraw land in the snapshot). |
| 5 | Proposed fix | **right direction, changed in the details** | See "Design" below. |

## Reproduced

`tools/e2e/run_me_midframe.sh` assembles 15 synthetic ROMs
(`tools/e2e/me_midframe/gen_roms.py`, GNU as in a throwaway container) and runs
each through the desktop twin built with `ME_TIMING_SIM`, which replays every
captured frame through `me_replay_lines()` -- the function the ME itself runs --
with the memory the ME would copy at the vcount-160 post, and compares it with
the CPU renderer's frame. Each ROM runs twice: capture mode 2 (the CPU renders
too; its image is the reference) and capture mode 1, which is production (the
core skips the render; its model images must equal the mode-2 CPU images).

Frames wrong out of 210 (after a 30-frame settle), before and after:

| case | what it does | 3.0.0 contract | this branch |
|---|---|---:|---:|
| `ctl_mode0`, `ctl_affine`, `aff_vblank`, `vram_offscreen`, `m4_flipmid` | controls | 0 | 0 |
| `aff_irq` | BG2/BG3 X/Y and PA rewritten in an HBlank IRQ (mode 2) | 210 | 0 |
| `aff_mos_irq` | the same with vertical BG mosaic | 210 | 0 |
| `aff_dma` | BG2X/Y streamed by HBlank DMA (mode 1 "road") | 210 | 0 |
| `pal_irq` | palette colour rewritten in an HBlank IRQ | 210 | 0 |
| `pal_dma` | palette gradient by HBlank DMA | 210 | 0 |
| `vram_irq` | tile rewritten per line (STM) in an HBlank IRQ | 210 | 0 |
| `vram_dma` | tile rewritten per line by HBlank DMA | 210 | 0 |
| `oam_irq` | sprite X rewritten per line | 210 | 0 |
| `m4_back` | mode 4, draw the back page, flip in VBlank | 0 | 0 |
| `m4_front` | mode 4, redraw the displayed page, unsynchronised | 210 | 0 |

The oracle bites: with both flags off every non-control case diverges on every
frame in both modes, and the controls read 0 in every build.

**Mario Golf: Advance Tour (USA)**, from power-on with the owner's save
(`tools/e2e/me_midframe/mgat_shot.inputs`: menus, hole-1 flyover, the aim view,
the swing and the ball flight), 3870 frames:

| engine | frames drawn differently from the CPU renderer |
|---|---:|
| 3.0.0 contract | **1558** -- the whole aim view and ball flight |
| per-line affine only | 23 |
| per-line affine + mid-frame log | **2** |

The aim view and the ball flight are a perspective ("mode 7") plane: the game
reloads BG2X/BG2Y every line. The old engine projected the whole screen from the
line-0 origin, which comes out mostly black -- the reported "graphics break when
switching to the overhead view". The 23 left after the affine fix are one-frame
transients (a palette fade written mid-frame, map tiles streamed during the
visible lines, a sprite). The 2 left after the log (frames 2027 and 2814) both
wrote VRAM during the visible lines while per-line VRAM tracking was off: the
first frame of a VRAM episode, which the hysteresis below does not cover by
design.

"Nice Shot" was not reached by the scripted inputs (a timing sweep of the swing
found none), so the reported colour fault there is **not reproduced**. Across
the whole run the game changed the palette mid-frame on 180 frames; after the fix
the only one still drawn differently is frame 2027, for its VRAM write.

## The Pokemon fixture library (clean-frame regression check)

`tools/e2e/run_me_midframe_fixtures.sh`, four arms per fixture (3.0.0 source;
this branch with the log off; with it on; production mode 1), 3870 frames each:

| fixture | CPU images identical in every arm | frames the old engine drew right, drawn identically now | wrong: 3.0.0 | affine only | both | mode 1 | replays that failed to restore memory |
|---|---|---:|---:|---:|---:|---:|---:|
| `mgat_shot` | yes | 2312/2312 | 1558 | 23 | 2 | 2 | 0 |
| `heart_soul_heavy` | yes | 3865/3865 | 5 | 5 | **0** | 0 | 0 |
| `heart_soul_light` | yes | 3870/3870 | 0 | 0 | 0 | 0 | 0 |
| `unbound_rival_medium` | yes | 3870/3870 | 0 | 0 | 0 | 0 | 0 |
| `unbound_double_medium` | yes | 3870/3870 | 0 | 0 | 0 | 0 | 0 |

The 5 `heart_soul_heavy` frames are the "known single-snapshot limitation" left
after `me_vispost` (ME-RENDERER-DIVERGENCE.md); they are now exact. The model's
replays run on the live arrays, so a log that failed to put memory back would
change emulation: the identical CPU images and the drift counter (0) both check
that.

## Design

### 1. Per-line affine reference (`ME_AFFINE_LINES`, default 1)

Capture mode 1 now runs the CPU renderer's own PB/PD step (moved verbatim into
`affine_advance()`) even though it skips the render, and records the four
reference counters for every line in `me_capture_frame.affine_line[160][4]`
(2.5 KiB, appended to the struct so older fields keep their offsets). The replay
installs them before each line. On a frame without a reload the recorded values
*are* what the engine would have stepped to, so the output is bit-identical to
the old contract (the fixture table's "drawn identically" column).

This also fixed two hard-coded capture sizes that would not have followed the
struct: the volatile-memory carve in `main_psp.c` (the literal 20544 would have
overlapped the two captures, on hardware only) and the SDL test buffer.

### 2. Mid-frame graphics-memory log (`ME_MIDFRAME_LOG`, default 1 on this branch)

The reporter proposed a line-0 baseline copy plus a forward write log. The
branch keeps the engine's line-160 snapshot instead (it is what the dirty-page
VRAM mirror is built on) and ships an **undo/redo** log: for each changed
palette/OAM halfword or VRAM word, the line it takes effect on, its old value
and its new value. The engine walks the log backwards over its snapshot to
reach the line-0 state, then forwards, applying each line's entries before
drawing it. After the last entry its copy is the line-160 state again, which
the persistent VRAM mirror needs for the next frame's patch. A log that
overflows (12288 entries, 144 KiB a capture) is dropped whole, because a partial
log would break that invariant; the frame then renders exactly as in 3.0.0.

Detection must not cost the store path. It is per scanline, from flags:

| region | per-line signal | store-path cost |
|---|---|---|
| OAM | `reg[OAM_UPDATED]` | none (the stubs always set it) |
| palette | `reg[PAL_UPDATED]` (new, `reg[63]`) | one `sw` in the MIPS palette stub, one `movl` in the x86 stub, one store in `write_palette*` |
| VRAM | `vram_clean[96]` (new "any page" byte after the dirty map) | one `sb` in the MIPS VRAM stub (5 instructions where there were 4), one in `vram_mark` |

Palette and OAM need a line-0 baseline to have old values; it is recopied (1 KiB
each) only when the flag says the region was written since the last check.
VRAM would need a 96 KiB baseline, so the core keeps a VRAM shadow
(`me_vram_shadow`, 96 KiB from the volatile partition) that is only trusted for
pages it has been tracking, and tracking is **hysteresis**: a frame whose
visible lines wrote a page that some line of that frame reads turns per-line
VRAM tracking on for 60 frames. "Reads" is computed from the captured
registers: all BG VRAM when any BG is on in modes 0-2, only the *displayed*
frame in modes 3-5, OBJ VRAM when OBJs are on. That is what keeps a mode-4 game
that draws its back page off this path entirely.

Where the reporter proposed a CPU fallback when the log overflows, the branch
drops the log for that frame instead. A fallback needs presentation plumbing
(CPU and ME frames interleaving in the loop-top pipeline) that has not been
built or validated; the overflow case was never reached in any fixture (maximum
10322 entries, in the synthetic `m4_front`).

The log is only valid over the line-160 state, so the FF profiles' end-of-frame
post zeroes it and renders as before (`main_psp.c` `g_mer_in_vis`).

### Not changed

* (2026-10-02: Unlimited and Unlimited Smooth now post their drawn frames at
  vcount 160 with the log, like 1x; only 3x still posts at frame end. See
  docs/FF-PRESETS.md.)
* Fast-forward still posts at frame end (docs/FF-ARTIFACT-FIX.md). In FF the
  engine draws with post-VBlank memory: `m4_back` and Mario Golf's aim view are
  wrong there on most frames (the "post" column of the model). Out of scope
  here; the FF profiles were validated with that post point.
* No CPU fallback, as above.

## Cost

Desktop twin, per emulated frame: the recorder's own time is about **0.5 us**
against **62-84 us** for the CPU renderer lines it lets the engine replace (the
recorder with the log off reads 3 us, all of it the timer calls; on minus off is
the work). The work counters (`me_log_stats`, also in the harness `EVT
me_midlog` census line) for the Pokemon fixtures, per frame on average: two
flag loads per line, about two 1 KiB baseline copies, 0.5-1.2 1 KiB page
passes (copies and diffs), and per-line VRAM tracking on 41-61% of frames (the games stream tiles
during the visible lines every few seconds, which re-arms the hysteresis).

**Not measured: the PSP.** An estimate from the counters is tens of
microseconds a frame on the Allegrex; the hardware census (`core` time,
`me_rend` `wait_mean_us` / `rend_mean_us`) is the number that decides the
default. On the ME side the log costs an undo and a redo pass over the entries
(at most 2089 in the Pokemon fixtures, 584 or fewer in three of the four) and a cache invalidate of the
used part.

The engine itself: `update_scanline` in the PRX shrank from 2752 to 2556 bytes
(the capture block is no longer compiled into the engine, `ME_PRX_BUILD`), and
the new replay loop (`me_replay_lines`, with the log) is 804 bytes.

## What this does not explain

* **Driver 2 "very slow", Mario Golf "slowdown during the shot"**: emulation
  cost, not the renderer. In the desktop twin Mario Golf flushes the RAM
  translation cache 2-4 times a frame in its menus, 6-11 in the aim view and
  **43-82 times a frame for the two seconds of the swing**, where the frame's
  core time triples. The flushes are CPU stores into translated code and
  CPU-started DMA into tagged RAM; the 3.0 `DMA_SMC_FLUSH` path (HBlank/VBlank
  DMA) contributed none. The ME cannot help there: it takes the render off the
  CPU, not the emulation. Driver 2 is not available to test.
* **Tomb Raider flicker, Penny Racers, OutRun's road**: not available to test.
  Each matches a class the synthetic ROMs cover (a mode-4/5 frame redrawn while
  on screen; per-line affine; per-line scroll plus palette), so they are
  plausible, not demonstrated.

## Reproducing

    # WSL; builds the twin with ME_TIMING_SIM, assembles the ROMs, runs both modes
    tools/e2e/run_me_midframe.sh
    # the fixture library (needs a pre-fix twin at $MMF_BASE_SDL for the base arm)
    tools/e2e/run_me_midframe_fixtures.sh heart_soul_heavy mgat_shot ...

`MTS_DUMP=1` / `MTS_DUMP_DIR` + `MTS_DUMP_FRAMES=A-B` write raw cpu/vis/post
images; `tools/e2e/me_midframe/raw2png.py` puts them side by side.

One-binary A/B on hardware (harness builds): `me_midlog = 0` in
`.gpsp-harness.ini` turns the log off (the per-line affine reference has no
runtime switch; the 3.0.0 build is its "before").
