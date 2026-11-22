# UI overhaul: structure and navigation

The menus keep their look (the overlay's wordmark, scrims, bands, fonts, both
themes, the Shelf and Marquee lists). What changed is how they are organised
and how they say what a key does. The layout follows the 5.1 UI mockup's
structure; `psp/ui_psp.c` is the code.

## Settings

- A top page of seven categories, each row a summary of its values:
  Display, Audio & speed, Saves & rewind, Controls, Screenshots, Wireless,
  Interface. X (or RIGHT) opens one; O goes back.
- Each category is one page of at most five rows. Nothing scrolls. L / R step
  to the previous / next category without going back; the pips on line 2 say
  which one is open.
- Display edits the console in context, as before; its title names it.
- Rows: Display (Video scale, Smoothing, Screen effect, Colour or GB palette,
  Ambient bars); Audio & speed (Volume, Fast-forward speed, Fast-forward
  button, CPU clock, FPS counter); Saves & rewind (Confirm quick load, Confirm
  quick save, Rewind, Rewind length); Controls (Thumbstick, Button mapping,
  Turbo buttons); Screenshots (Capture source, File format, Save folder);
  Wireless (Default host link, Session overlay, Online play); Interface
  (Theme, Menu style).
- Room and Fast link are the Wireless screen's. Share game data and the room
  list are the game list's Wireless screen.
- No config.ini key changed.

## Wireless

- In game: Room (LEFT / RIGHT changes it; the only place it is edited), Host,
  Join: scan for rooms, Join room, Ad hoc channel, Fast link, Mystery Gift,
  Online. No Back row: O goes back everywhere.
- In the game list, R opens it: Room, Share game data, Join: scan for rooms,
  Ad hoc channel, Fast link. L still pages the list up.
- Settings' per-game link override is "Default host link"; the Online page's
  own choice is "Link type".

## One rule for a greyed row

On every screen: the cursor stops on it, nothing changes it, the info line
says why, and the footer drops the keys it cannot use. Examples: Rewind length
with Rewind off, Default host link for a GB game, Fast link and Mystery Gift
for a GB game, Online with Online play off, Save states and Reset game during
link play, Online's Link type for a one-way game.

## The info line and the footer

- The info line, just above the footer, says what the highlighted row does or
  why it is greyed (two lines on the Wireless screen, where Host explains how
  the game links).
- The footer holds keys only, and only the keys the highlighted row takes:
  "change room" only on Room, "change" only on rows LEFT / RIGHT change,
  "select" or "open" only on rows X acts on.
- Values start with a capital and booleans read On / Off.
- Rewind's value says "(next game)" while it differs from what the running
  game started with.
- The pause screen's footer no longer offers LEFT / RIGHT; Mystery Gift names
  its shortcut as Controls has it now.

## Removed

The dead relaunch path behind Settings (the Media Engine row's restart,
`g_profile_changed`) in the menu code. The main loop's `UI_ACT_RELAUNCH` case
stays: it is frame-loop code, outside this change.

## Layout cost

The menu code grew about 2.2 KB of text and 1.5 KB of strings. Release's
`fe_common` gap had 276 B, so `pin fe_common` took one page
(`layout_lock.py regen release --spend-page fe_common`): 8 KiB of PSP-1000
boot heap. The file's `.rodata` tables (after `pin ro`, in front of
`netpacket_iface`) and its `.data` are held at their old sizes, the two new
label tables living in its text, so every lock value holds and release and
lab touch the same data layout (frame parity).
