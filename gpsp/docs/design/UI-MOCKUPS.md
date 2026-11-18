# UI design mockups

The in-game menu, the link-mode menus, the Save States page, the boot splash and logo, and the
online room codes were each designed first as a set of mockups. **The mockup sources and their PNGs
are not kept in the repo**; this page replaces them. Code comments and docs cite it as
`docs/design/UI-MOCKUPS.md` (often with the mockup file they used to name: `mockups.py`,
`class Overlay`, `savestate.py`, `data.js`, ...). Each section below says what the set designed,
the numbers and rules the code took from it, and where that code lives.

Related: [UI-OVERLAY.md](../UI-OVERLAY.md) (how the in-game screens were built and checked),
[GBA-LINK-UI.md](../GBA-LINK-UI.md), [DISPLAY-FEATURES.md](../DISPLAY-FEATURES.md),
[ONLINE-TUNNEL.md](../ONLINE-TUNNEL.md).

**How the PSP mockups were drawn.** Every PSP screen was a 480x272 frame, dark and light theme,
drawn by `pspdraw.py`: a software model of the overlay renderer in `psp/video_psp.c`. It parsed the
real baked atlases (`psp/font_ui.h`: Inter Regular 15 px `ui`, Inter SemiBold 19 px `hd`, the
button glyphs; `psp/logo_ui.h`: the wordmark), used the RGB565 theme words of `psp/ui_psp.c`
(`THM_DARK`, `THM_LIGHT`, the console skins), and allowed only the primitives the UI code calls
(`vid_rect`, `vid_gradient_a`, `vid_text` / `vid_text_hd`, `vid_logo`, `vid_image`, `vid_clip`),
quantising the finished frame to RGB565. So a mockup PNG is what the PSP draws, and a PPSSPP GE
dump of the real screen can be compared with it pixel for pixel. Each set also counted its draw
calls per image; the budget was ~110 (what the browser draws at 60 fps with art).

**Tools that need a local copy.** The GE comparison rigs import the mockup code and read its
PNGs, so they only run where a copy of the mockups exists, pointed at by an environment
variable: `tools/ui_model/` (`UI_MOCKUPS` for the menu mockups, `SAVESTATE_MOCKUPS` for the Save
States set) and `tools/gbamirror/linkmenu/` (`LINKMODE_MOCKUPS`, `LINKMODE_PNG`). Since the
designs were made, `tools/ui_model/overlay_model.py` (the implemented screens, drawn the same way)
is the reference where code and design have diverged on purpose.

---

<a id="menu-mockups"></a>

## Menu mockups: the in-game menu, Ambient mode, the loading screen

**What it designed.** Five directions for the in-game menu (START+SELECT) and its Save State,
Settings, Wireless and new **Controls** (button remap) screens: A Overlay, B Shelf, C Marquee,
D Drawer, E Deck. Plus two studies: **Ambient mode** (the game's hero art in the side bars) and
the **ROM loading screen**. The owner picked **A, Overlay**; Ambient **"soft"**; loading **"L2"**.

**Shared vocabulary** (all directions): theme words `C_BG_TOP`, `C_CARD` (#181818 dark / white
light), `C_ACCENT` (white / black), `C_ACCENT_DK` (#8B8B8B / #6B6B6B), `C_SEL`, `C_ITEM`, `C_DIM`,
`C_VALUE`, `C_SHADOW`. The console colour `C_CON` appears only in the browser's three places
(badge, selection edge, star). The Controls page's one new idiom is the **chip**: a plate 18 px
tall, text inset 6 px, in four states: *bound* (`C_ACCENT` alpha 40, 28 light, `C_SEL` text),
*locked* (`C_CARD`, `C_DIM` text), *blank* (1 px `C_DIM` outline alpha 160 around `none`),
*capture* (1 px `C_CON` outline around `press...`, alpha-pulsed). `blank = disabled` is printed
on every Controls screen so an empty chip is never ambiguous.

**Direction A, Overlay** (the mockup's `class Overlay`; its numbers are the spec). The
wake-from-sleep screen's language for every in-game screen: the paused game frame stays up
behind a scrim, the wordmark sits in the top-left, the selection bleeds off the left edge, and a
ramp out of the bottom edge carries the footer. No panel is drawn.

| element | design numbers |
|---|---|
| backdrop | the frame 1:1, then `vid_rect(0,0,480,272,C_BG_BOT,170)`, then the ramp `vid_gradient_a(0,152,480,120,C_BG_BOT,0,165)` |
| scrim | 170 for menu, slots and wireless; 200 for the long lists (settings, controls) |
| head | `vid_logo(20,18,C_TITLE)`; hd title at x = 20 + logo width + 14, y = 16; game title at (22,48) in `C_DIM`; badge and room cluster top-right |
| menu rows | x = 18 from y = 88, 22 px pitch |
| selection band | `vid_rect(0,y-4,w,24,C_ACCENT,40)` + a 4 px solid edge `vid_rect(0,y-4,4,24,C_ACCENT,255)` |
| opening transition | scrim 0 to 170 and rows +8 px to 0 over 8 frames, text walked in from the background colour |
| settings | the list at x = 20..440, clipped to y 76..244, a 2 px scrollbar at x = 470 |
| controls | two columns, GAME BUTTONS (x = 20) and SHORTCUTS (x = 206), 19 px pitch, 16 px chips; capture and conflict say so on a plate `vid_rect(0,252,480,20,C_ACCENT,40)` in place of the footer |

Code: `psp/ui_psp.c`, "THE OVERLAY" (`OV_SCRIM 170`, `OV_SCRIM_DENSE 200`, `OV_OPEN_FRAMES 8`,
`OV_RAMP_Y`, `ov_backdrop`, `ov_head`, `ov_band`, `ov_plate`, the chips); the Controls table is
`psp/ctl_map.c`. Later changes moved some numbers on purpose (the 151x24 wordmark at y = 17, the
band centred on the text with `vid_band_y`, a longer Controls table): see
[UI-OVERLAY.md](../UI-OVERLAY.md) sections 4, 10 and 12.

**Ambient "soft".** The hero art box-averaged once at ROM load to **120x68 texels** in a 128x128
RGB565 texture (32 KiB), blurred once with radius 1, drawn per side bar with `GU_LINEAR` and a
scrim of **96** when the art came from `hero/` (pre-composed) or **150** when derived from box art.
Never re-baked per frame. "deep" is radius 2 with scrim 150. Fallback without art: the console's
darkest palette shade with a vertical ramp. The integer 2x preset crops 24 px top and bottom,
so it is opt-in. Code: `psp/ambient_look.h` (`AMB_W`, `AMB_H`, `AMB_BLUR_RADIUS`,
`AMB_SCRIM_HERO`, `AMB_SCRIM_BOXART`), `psp/video_psp.c` (`vid_ambient_bake_art`, the texture in
VRAM). The texture lives in VRAM, not the heap.

**Loading screen "L2".** The browser's marquee chrome over the ambient bake drawn full-screen
under a `C_BG_TOP` scrim of **110**: wordmark top-left, console badge and the busy ring top-right,
the game's title in the hd face at (20,74), the stage at (20,102) with the amount right-aligned
to 460, a **3 px progress bar at y = 126**, 440 wide, filled in the console colour over a
`C_ACCENT_DK` track, and the footer ramp `vid_gradient_a(0,232,480,40,C_BG_TOP,0,190)`. Without
art: the console's `pal[0]` ramp at alpha 170 dark / 70 light. Code: `psp/ui_psp.c`
(`ui_loading_begin` and the loading draw), `psp/ambient_look.h` (`LOAD_SCRIM`, `LOAD_FOOT_RAMP`,
`LOAD_PAL_RAMP_DARK`, `LOAD_PAL_RAMP_LIGHT`). Docs: [DISPLAY-FEATURES.md](../DISPLAY-FEATURES.md).

---

<a id="linkmode-mockups"></a>

## Link-mode mockups: Link Cable or Wireless Adapter

**What it designed.** Every screen and state of the Wireless menus once a GBA game can link by
Link Cable (the two-machine mirror) as well as by the Wireless Adapter, in the Overlay language,
over three backdrops (an Emerald frame, an Advance Wars 2 frame, a Game Boy frame). Notices were
width-checked against the atlas: plates at most 460 px, explainer lines at most 436 px.

**Rules the code implements.**

1. **The game decides.** A table keyed by the first three bytes of the GBA header code maps a game
   to cable, adapter or both; an unlisted game is cable. Code: `frontend-common/link_games.h`
   (`LINK_CABLE`, `LINK_ADAPTER`, `LINK_BOTH`, `link_game_mode`, `link_game_title`).
2. **Host asks only for a game that links both ways** (the HOST chooser). Otherwise the Host row's
   value names the mode: `Host  Link Cable`.
3. **Join never asks.** The room name carries mode, room and game, so the scan list can say
   `Room 7   Link Cable   <game>`; a dimmed row explains itself on a one-line plate.
4. **A per-game override** in Settings > WIRELESS > Link mode (Auto / Link Cable / Wireless
   Adapter), stored in `linkmode.txt` keyed by the ROM file name (a ROM hack keeps the base game's
   code, so a code key would hit the retail game).
5. GB/GBC uses the same Wireless screen; its rooms advertise as Link Cable rooms.

**Room advertisement** (the scan only returns the 8-byte ad hoc group name):

| group name | meaning |
|---|---|
| `GPSPnn` | Wireless Adapter room nn, unchanged since 1.0 |
| `GCnnXXXX` | Link Cable room nn, GBA, XXXX = the header game code (a non-alphanumeric byte becomes `0`) |
| `GBnnXXXX` | Link Cable room nn, GB/GBC, XXXX = the first four alphanumeric bytes of the cartridge title, else `GAME` |
| other | shown verbatim, joinable; the handshake decides |

The cable protocol string is `GBAdhoc cable v1`, so an older build is refused at JOIN rather than
half-joining. Code: `psp/room_label.h` (`room_parse`, `room_label`; it later gained `GSnnDATA`
for Share game data), the Wireless screens in `psp/ui_psp.c`, the GE walks and the comparison
against the design PNGs in `tools/gbamirror/linkmenu/` (`compare_mockups.py`,
`make_backdrops.py`, `ge_*.ui`). Docs: [GBA-LINK-UI.md](../GBA-LINK-UI.md), which also lists the
deliberate deviations from the design.

---

<a id="savestate-mockups"></a>

## Save States mockups: one page for save and load

**What it designed.** One **SAVE STATES** page replacing the Save State / Load State pair, with a
large preview and a time per slot, in four directions: **A Split**, B Stage, C Accordion,
D Compare. The owner picked **direction A, "Split"**, with the VRAM plan. The menu's two rows
became one, **Save states**, with "n of 5 used" beside it.

**Direction A** (the mockup's section 6, "implementer spec for A"):

- **Layout.** Five rows on the left at `y = 72 + 34 i`, two lines each (`Slot n`, then the time
  or `empty`), the 64x42 thumbnail drawn at 36x24 in front; selection band 212x32 bleeding off the
  left edge. The highlighted slot's picture on the right in a 240x160 panel at **(228, 62)**,
  **1:1 nearest**, with a 3 px shadow and a 1 px `C_ACCENT_DK` outline; the slot and its time
  under it.
- **Empty slot:** a `C_CARD` plate, "Nothing saved here". **No full preview** (an old state, or
  not read yet): the 64x42 thumbnail at 2x nearest on the plate. GB/GBC: 160x144 at 1:1, centred.
- **Times** come from the state file's modification time (`sceIoGetstat`, which the page already
  calls), formatted directly from the six date fields; nothing new is stored.
- **The full preview** is a companion file `<state>.shot` beside `<state>.thumb`: the same 16-byte
  header (`STH1`, width, height, stride **256**), then the rows at the texture's stride, so the
  body is one read (81,920 B for GBA, 73,728 B for GB/GBC). It is written after the state and
  removed on a failed write, so it can never affect the state itself.
- **Memory: zero heap, zero RAM.** One 256x256 texture (131,072 B) in VRAM above the sharp
  bilinear target. If VRAM has no room the page never reads a `.shot` and draws the 2x thumbnail.
- **Reading on dwell.** Only the highlighted slot's `.shot` is read, after the cursor has rested
  **6 frames** (longer than the 5-frame d-pad repeat, so a held d-pad never reads).
- **Questions on the plate** for the destructive actions; while a question is up, no other key
  acts.

Code: `psp/ui_psp.c` ("THE SAVE STATES PAGE": `SS_ROW_Y`, `SS_PX`, `SS_PY`, `SS_PW`, `SS_PH`,
`OV_SHOT_DWELL`, `g_slot_ask`), `psp/video_psp.c` (`vid_shot_tex`, `SHOT_BYTES`),
`psp/state_slots.h` (`psp_state_shot_path` and the delete names), `tools/tests/test_state_delete.c`,
`tools/ui_model/savestates/`. **Changed since the design:** the design's buttons were X load,
SQUARE save; since 4.6.0-beta.4 X saves and SQUARE loads, and a load asks first too
([UI-OVERLAY.md](../UI-OVERLAY.md) sections 13 to 15). The design PNGs still show the old footer;
`tools/ui_model/overlay_model.py` is the reference for it.

---

<a id="xmb-relay-mockup"></a>

## XMB relay mockup: hosting an online room from a PC, room codes and invite codes

**What it designed.** "Host online room": a desktop page (the GBAdhoc XMB app on a PC hosts a
room that friends join from anywhere) and the PSP side of joining it, drawn as the emulator's
480x272 overlay (wordmark at (20,17), hd title at y 16, explainer lines at y 72 / 90, rows from
y 120 on a 22 px pitch, a 300-wide band, a 20 px footer). Its sample data used documentation
addresses only. It proposed **two codes in one box**, told apart by length:

- **Room code: four letters**, looked up on the GBAdhoc server (the directory), e.g. `KQTR`.
  A to Z, 26^4 = 456,976 rooms. Code: `netdrv/relay_proto.h` (`RL_CODE_LEN 4`).
- **Invite code: 8 or 11 symbols**, the desktop host's public IPv4 address (and port) with a
  check, decoded on the PSP so no server is asked.

**The invite code** (the mockup's `data.js` encoder):

| | |
|---|---|
| alphabet | Crockford base32, `0123456789ABCDEFGHJKMNPQRSTVWXYZ` (no I L O U) |
| short form | 8 symbols = 40 bits: IPv4 (32 bits, first byte first) + check (8 bits) = CRC-8 (polynomial 0x07) of the four address bytes, XOR **0x5A**; implies the default port **27420** |
| long form | 11 symbols = 55 bits: IPv4 (32) + port (16) + a 7-bit check, for a host on another port |
| display | groups of four with dashes (`XXXX-XXXX`, `XXXX-XXXX-XXX`) |
| input | case, dashes and spaces ignored; look-alikes folded: O to 0, I and L to 1, U to V |

`0x5A` is the format's version tag: a code of another version fails the check instead of
decoding to a wrong address. **One deliberate change:** the mockup made the long form's check
the low 7 bits of the CRC-8, which misses about 1 in 128 single-symbol typos; the code uses a
real CRC-7 (polynomial 0x09), which catches every single wrong symbol. The short form is the
mockup's exactly. Code: `netdrv/invite_code.h` (`INV_ALPHABET`, `INV_DEFAULT_PORT`, `INV_TAG`,
`inv_encode`, `inv_decode`), used by `psp/online/main.c` and `tools/relay/`; the code box in
`psp/ui_psp.c` (ONLINE). `tools/relay/test_invite.c` checks vectors made by the mockup's own
encoder (short forms byte for byte; long forms in the address and port symbols). Spec:
[ONLINE-TUNNEL.md](../ONLINE-TUNNEL.md), "Invite codes".

---

<a id="splash-mockups"></a>

## Splash mockups: boot splash, refined logo, XMB icon

**What it designed.** Two jobs: (1) a boot splash in five directions (A Continuity, B Mark,
C Signal, **D Ambient**, E Zero) with the hand-off into the browser, plus the XMB presence (an
animated ICON1, a refreshed PIC1, an SND0 concept); (2) the wordmark refined, with every baked form
derived from one clean master.

**The logo.** The old in-app wordmark (130x22) was a LANCZOS resample of a raster crop at a
non-integer scale: soft edges, a half-grey cap line, and the rainbow arcs ending in "beads" that
were drawn into the master. The study redrew the rainbow and bar analytically (five arcs, butt
caps, no end markers) and **hinted** the letters (a 1:6 design: stems snapped to whole pixels,
cap / x-height / baseline on whole rows). The pick: **151x24, 4 px stems**, in the same 256x32 T8
atlas (8 KiB of `.rodata`, unchanged), drawn with **`GU_NEAREST`**. Code: `psp/logo_ui.h`,
`psp/video_psp.h` (`VID_LOGO_W 151`, `VID_LOGO_H 24`), `vid_logo` in `psp/video_psp.c`;
`tools/logo/logomark.py` (the study's mark generator, kept in the repo) and `tools/bake_logo.py`.

**ICON0, PIC1, ICON1.** ICON0 (144x80) uses the 3 px hinted letters with the rainbow re-spaced
for the icon (2 px arcs, 1 px gaps, outer radius 19) and a 2 px bar. ICON1 is a 144x80, 3 s loop
at 29.97 fps (90 frames): the letters always present, the five arcs sweeping in left to right,
innermost first, 0.1 s apart (0.2-1.0 s), the bar wiping left to right (1.0-1.4 s), then a hold
and a fade of arcs and bar over the last 0.4 s. Code: `psp/assets/gbadhoc_icon0.png`,
`gbadhoc_pic1.png`, `gbadhoc_icon1.pmf`; `tools/logo/make_icon1.py` (the frames, from the study's
`icon_frame`, and the PMF).

**The splash** (direction D: the launch screen is the loading screen, L2's chrome over an ambient
bake) was built and then **removed on 2026-10-03** by owner decision; nothing in the tree draws
or ships a splash. The logo, the header order and the icons stayed. Record:
[UI-OVERLAY.md](../UI-OVERLAY.md) section 12 (12.2 the splash as built, 12.9 its removal).
