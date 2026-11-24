# Changelog

All notable changes to **GBAdhoc**, the native PSP frontend for the modern gpSP core,
with GB/GBC support and link play between two PSPs.

Format loosely follows [Keep a Changelog](https://keepachangelog.com/). Versions are
semantic. The emulator core itself comes from
[libretro/gpsp](https://github.com/libretro/gpsp); core changes made here are listed
separately and are intended for upstream.

---

## [5.0.0-beta.7] - 2026-11-23

Pre-release: beta 6 plus new menus and the finishing work on multiplayer. Links only with
the same beta: update both PSPs, for the Link Cable, the Wireless Adapter, Game Boy link,
Share game data and online play alike. Copy all four files (`EBOOT.PBP`, `gbadhoc_me.prx`,
`gbadhoc_home.prx`, `gbadhoc_online.prx`) together.

> **Your saves and save states have their own folders** (`saves/`, `states/`), as in beta 6.
> GBAdhoc still moves them there for you the first time you open each game; **the next
> release will stop doing this.** The README shows where each file goes.

### Menus

- **Settings opens on one page of seven groups:** Display, Audio & speed, Saves & rewind,
  Controls, Screenshots, Wireless and Interface, each with a summary of what's set. ✕ (or →)
  opens a group, ○ goes back, L / R step between groups. Every group fits on one screen.
- **Greyed-out options** can be highlighted; the line above the button hints says why they
  can't be changed. The hints list only the buttons the highlighted row takes.
- **R on the game list opens *Wireless*:** Room, *Share game data*, *Join: scan for rooms*,
  Ad hoc channel and Fast link. In game, *Wireless* starts on the Room row, the only place
  the Room is set. *Turbo buttons* opens from *Settings → Controls*.
- **Renamed:** *Link mode* is *Default host link* (Settings → Wireless); *Screenshot picture*,
  *format* and *folder* are *Capture source*, *File format* and *Save folder*; *Quick load /
  save asks first* are *Confirm quick load / save*. Your `config.ini` carries over unchanged.
- **Wireless → Ad hoc channel** (Automatic, 1, 6 or 11) changes the PSP's own ad hoc channel
  setting, the one the XMB sets. An empty room list names the channel this PSP is on.

### Link Cable over ad hoc

- **The other player's buttons are used after 3 frames instead of 4** (50 ms instead of 67),
  and each packet also carries the two sent before it, so a lost packet is filled in by the
  next one. A button that arrives just in time is picked up when the game needs it, instead
  of at the start of the next frame. Mario Kart: Super Circuit, Advance Wars 2, Kirby:
  Nightmare in Dream Land and Emerald's link battles pause less often than in beta 6 (Mario
  Kart about every 5 to 6 seconds instead of every 2.5); Mario Tennis, Mario Golf and the
  Emerald Trade Center about as often. Frame rates are unchanged or a
  little higher. *Fast link* stays an option, off by default.

### Online play (experimental)

- **Link Cable and Game Boy link online:** input delay 6 frames (100 ms, was 67), and every
  packet carries the 6 inputs before it. Pokémon Emerald's Trade Center between a PSP on a
  phone hotspot and a PSP at home: about 52 fps, where beta 6 ran at 13-20; on a fast, steady
  connection 42 rose to 52.
- **Wireless Adapter online:** the other PSP's adapter data gets 7 frames (117 ms, was 67),
  and packets carry up to 6 copies. Your own buttons are not delayed. Emerald Trade Center:
  about 32 -> up to 55-57 fps over a phone hotspot, 53-55 -> 56-58 on a fast, steady connection.
- **Faster link start online.** The data exchanged when a link starts (saves, the game's
  state and, for Game Boy, a missing cartridge) has a lane of its own online, as it does
  over ad hoc. A Link Cable link over a phone hotspot starts in about a minute instead of two
  to three.
- **Game Boy link online needs the cartridge on one PSP only.** A missing cartridge is sent
  for the session, kept in memory, checked and never written to the Memory Stick. It crosses
  the online server in transit only; nothing is stored there. New and lightly tested so far.
- **Link setup survives a short drop:** while a link starts, the other PSP may be silent for
  up to 30 seconds (phone hotspots stall for several seconds now and then). In play, the
  session still ends after 8 seconds of silence, as before.

### Speed

- The second processor copies each picture faster, and one more kind of code that a game
  rewrites while it runs is handled cheaply. Hot Wheels: Stunt Track Challenge 57.0 -> 57.7 fps on a
  PSP-3000 and 57.4 -> 58.1 on a PSP-1000; a Pokémon MonoMyth trainer battle needs 3-4 %
  less work per frame. Every game we test runs at least as fast as in beta 6.

### Fixed

- Some Game Boy and Game Boy Color games, and GBA games of 256 KB or less, could be refused
  for link with the same file on both PSPs: the file check misread files whose size left
  56-63 bytes over a multiple of 64.
- A Game Boy link could send the whole cartridge across even when both PSPs had it, if the
  other PSP answered before this one had finished checking its own copy.
- The link error for a damaged or different game file now reads *A game file did not check
  out: damaged or different.* (it said the games did not match, which was wrong for Game Boy
  links, where the games may differ).

### Known issues

- Online Link Cable games carry about 100 ms of input delay; fast action games feel it.
- Mario Kart: Super Circuit online is playable but not smooth: about 50 fps over a phone
  hotspot (59.9 over ad hoc), frequent short pauses and the odd freeze of a few seconds.
- Phone hotspots can go silent for several seconds; 8 seconds of silence during play ends
  the session. On a very lossy connection the host can raise `gblink_delay` in `config.ini`
  (8 = 134 ms).
- As in beta 6: Mario Golf: Advance Tour's adapter lobby can occasionally stick when both
  players press at the same moment (leave and rejoin). Hot Wheels: Stunt Track Challenge (on
  the PSP-3000), Top Gear Rally and Payback still run below 58 fps, and so do FIFA 07's
  replays.

## [5.0.0-beta.6] - 2026-11-17

Pre-release: beta 5 plus the changes below, and the build we intend to become 5.0.0. Links
only with the same beta: update both PSPs, for the Link Cable, the Wireless Adapter, Game Boy
link, Share game data and online play alike. Copy all four files (`EBOOT.PBP`,
`gbadhoc_me.prx`, `gbadhoc_home.prx`, `gbadhoc_online.prx`) together.

> **Your saves and save states have their own folders.** Battery saves go in `saves/` and
> save states in `states/`, each in the same sub-folder its ROM has under `roms/`, no longer
> next to your ROMs. 5.0.0 still moves them there for you the first time you open each
> game. **The next release will stop doing this:** please move your files into these folders
> yourself. Until a save is in the right folder, its game starts as if it had none; nothing
> is deleted, so moving the file brings it back. The README shows where each file goes.

### Online play

- **Wireless Adapter games run faster online.** An online room now gives the other PSP's
  data a little more time to arrive before your game needs it (ad hoc play, where it arrives
  sooner, is unchanged). With the two changes below, a PSP-3000 in the Pokémon Emerald Trade
  Center over the internet ran at about 54 fps, where beta 5's setting ran at about 37 in
  the same test.
- **A lost packet costs a frame, not a stall.** Online, each packet also carries the two sent
  just before it, so a lost one is usually filled in by the next packet instead of being
  waited for and sent again.
- **A short network outage no longer breaks an online session.** When the connection
  stalls, both games now wait for it instead of running on without the other player's
  data; afterwards that data used to arrive all at once and some of it was lost, which
  could leave a Pokémon trade unable to finish saving. If the other PSP is gone for 8
  seconds, the session ends as before.
- **"Waiting for the other player..."** appears at the bottom left when a link session has
  held your game for a second, waiting for the other PSP (Link Cable, Wireless Adapter,
  Game Boy link, ad hoc or online), and goes away as soon as the game runs again. The
  picture used to freeze without a word.
- **Connecting online:** GBAdhoc stops trying the other PSP's home-network address once it
  has never answered for two rounds of attempts, instead of trying it for the whole session.
  Two PSPs on the same home network still connect directly as fast as before.

### Fixed

- **Games start at the same point in the frame as on a GBA.** GBAdhoc starts a game straight
  away, without the GBA's start-up logo, and began its first frame about half a frame
  earlier than a GBA does when the logo hands over to the cartridge. Now it starts where a
  GBA would. This fixes **Polarium Advance**, which hung at start-up on the PSP-3000 and
  kept restarting with the bundled BIOS: its first screen refresh came before the game had
  finished setting up its sound. The change is the same for every game, nothing in it is
  specific to Polarium, and speed is unchanged.
- **Rock 'N Roll Racing** shows its title screen and plays. It used to show stripes, then a
  blank screen. When a game saved the position of the instruction it was running, GBAdhoc
  saved it one instruction short of what a GBA saves; this game returns from every
  subroutine through that saved position, and landed on the wrong instruction.
- **Top Gun: Combat Zones** opens its menus again. The cursor moved but A and START did
  nothing. The cartridge has no save chip (the game uses passwords), and its copy check
  refuses to go on when it finds one; GBAdhoc now runs it with no save chip, as on a GBA.
  This setting applies to this game only.
- **X-Men: Reign of Apocalypse** has sound with the bundled BIOS. Some games leave the
  mixing of their music and sound effects to the GBA's BIOS, and the bundled BIOS did not
  do it, so they ran silent unless you had copied your own `gba_bios.bin`. The bundled BIOS
  now mixes their sound as a GBA's BIOS does. With your own `gba_bios.bin` nothing changes.
- **Mortal Kombat: Deadly Alliance** no longer freezes for about 50 seconds on the Midway
  logo with the bundled BIOS, and its title screen and menus are drawn correctly
  afterwards. The bundled BIOS read the size of some packed game data the wrong way and
  unpacked far too much, over the game's memory. It now reads it as a GBA's BIOS does. With
  your own `gba_bios.bin` nothing changes.
- **Activision Anthology** no longer asks you to "turn off the other Game Boy Advance" when
  you play alone. With no link cable connected, the link port now looks unplugged to the
  game, as on a GBA, instead of looking like another GBA had answered. Link play over the
  cable and the Wireless Adapter is unchanged.
- **Wireless Adapter: a player joining a group is no longer dropped when the leader takes a
  few seconds to accept.** While a joining game waited quietly for an answer, the leader's
  PSP gave up on it after about four seconds, and both players ended up back at the link
  screen (for example in the Pokémon Emerald Trade Center). A joined PSP now stays in the
  group for as long as it is connected, as with a real adapter.

**Linking with other versions** works as in beta 5: both PSPs need the same release. A PSP on
another version is turned away before any session starts, for every kind of link, and this
PSP says why: "The other PSP runs a different GBAdhoc version." (for 4.5.0, "an older GBAdhoc
version."). A PSP on 4.5.0 that hosts sees nothing, and one that joins over the Wireless
Adapter finds the room never answers. 5.0.0 will not link with 4.5.0 either, and keeps the
frame-by-frame Wireless Adapter link the betas use.

**Known issues:** online play is still experimental; only Pokémon Emerald has been tested over
the internet, and an online session can run below 58 fps. Mario Golf: Advance Tour's adapter
lobby can occasionally stick when both players press at the same moment (leave and rejoin).
Hot Wheels: Stunt Track Challenge, Top Gear Rally and Payback still run below 58 fps, and so
do FIFA 07's replays. **Downgrading:** 4.6.0-beta.3 and older look only in `roms/` for saves
and states; move them back from `saves/` and `states/` by hand before going back.

## [5.0.0-beta.5] - 2026-11-14

Pre-release, and the build we intend to become 5.0.0. Links only with the same beta: update
both PSPs, for the Link Cable, the Wireless Adapter, Game Boy link, Share game data and
online play alike. Copy all four files (`EBOOT.PBP`, `gbadhoc_me.prx`, `gbadhoc_home.prx`,
`gbadhoc_online.prx`) together; the online module only works with its own EBOOT.

### Faster

- **Heavy GBA games run faster.** The dynarec looks up translated code through a small
  two-way cache instead of a one-way table, the scheduler keeps its per-frame state where
  the CPU already has it, self-modifying code costs less bookkeeping, and the Media Engine is
  told exactly which rows of the palette and sprite table changed mid-frame. Measured on the
  PSP-1000 and PSP-3000 alike, against 4.6.0 beta 4 (a real GBA runs at about 59.7 fps):

  | Game | 4.6.0 beta 4 | 5.0.0 beta 5 |
  |---|--:|--:|
  | Hot Wheels: Stunt Track Challenge | 48.6 fps | 57 |
  | Duke Nukem Advance | 56.5 | 59.5 |
  | Ice Nine | 57.9 | 59.1 |

  Doom II, Need for Speed: Underground and OpenLara need 5-7 % less work per frame, which
  leaves more time to spare in each frame, and the Pokémon games 3-5 % less. Every game we
  tested runs at least as fast as on 4.5.0.
- **Games that wait a lot cost less.** While a GBA game is halted, waiting for its next
  interrupt, GBAdhoc skips straight over the stretch of the frame where nothing can happen,
  then puts every counter back exactly when the game wakes. The game sees no difference.
- **Beta 4's extra cost per frame is won back.** The new screen filters made each frame
  take about 1.5-3.5 % more CPU time, even with every filter off. The per-line code behind it
  has been rearranged so it gets in the way of the rest of the frame far less, and every game
  we tested now runs at least as fast as on 4.5.0.
- **Street Racing Syndicate and Big Mutha Truckers** no longer slow down from the code cache
  being rebuilt over and over (a beta 4 regression), and Top Gear Rally's code-cache resets
  are rarer.

### New

- **Screenshots "As displayed".** A screenshot is now the whole 480 x 272 screen as you see
  it: scale, smoothing, screen effect, colour, ambient bars and Game Boy palette, without
  the menu, the FPS counter or messages. To keep the old picture (the game alone, 240 x 160
  for GBA, 160 x 144 for GB and GBC), set *Settings > Screenshots > Screenshot picture* to
  *Game only* (`screenshot_source = game`). A screenshot pressed while the menu is open is no
  longer saved when the menu closes, or carried into the next game.
- **Quick load picks any slot.** When the quick load (Select + R) asks first, the d-pad moves
  the question to your other saved slots, with each one's picture and time. X loads the
  slot shown, Circle goes back to the game. Empty slots are skipped.
- **1.5x and 2x fast-forward** (#37, asked for by dragofintio), for games where 3x is too
  fast. *Settings > Gameplay > Fast-forward* now cycles 1.5x, 2x, 3x, Unlimited and
  Unlimited Smooth; 3x stays the default. 1.5x and 2x show a new picture on every screen
  refresh, and sound is muted as with the other presets. Older versions read 1.5x and 2x
  as 3x.
- **Game Boy Color colours for original Game Boy games** (#34, asked for by nunobfreire). The
  GB palette row gains *GBC (auto)*, the palette a Game Boy Color picks for that game when you
  switch it on, and the 12 palettes you could choose with its button combinations (*GBC Up*
  ... *GBC Right+B*). Games a Game Boy Color has no palette for get its default one. Only the
  colours change: the game runs exactly the same.

### Fixed

- **Link Cable sessions stay in step.** Mario Tennis: Power Tour and Mario Golf: Advance Tour
  could fall out of step within the first minute or so of a cable session. Each PSP runs both
  games in a session, and the copy of your own game started with what the code translator had
  learned in single player, while the copy of your friend's started fresh. Now both copies
  start every session alike, whatever each PSP did before and whichever PSP models are linked.
  All ten games we test over the cable stay in step, with either PSP hosting. Single player is
  unchanged.
- **Sleep is safer around loading, linking and quitting.** Sleeping while a game loads, while
  Online connects, or during *Quit to game list* now waits for that step to finish and then
  sleeps, instead of interrupting it.
- **If the Media Engine stops during sleep,** GBAdhoc now says so on waking ("The Media
  Engine stopped during sleep."), writes your save and returns to the game list. It used to
  end the session without a word.
- **Online connecting can be cancelled.** The LINKING screen shows Circle cancel while an
  online room is being set up.
- **HOME during *Quit to game list*** exits to the XMB. It could leave the PSP on "Please
  wait" for good.
- **Settings changes are kept** when you leave with a Settings, Controls or Online page still
  open, by HOME, by holding Start + Select, or by quitting from the wake screen.
- **Network errors say what went wrong.** A failed online or wireless start no longer reports
  "WLAN switch is OFF" when the switch is on.
- **PSP Go:** wireless and Mystery Gift no longer refuse to start because the WLAN switch read
  "off" for a moment just after the network came up. GBAdhoc now waits up to 5 seconds for it.
- **The save backup made before every wireless session** can no longer replace a good
  `.sav.bak` with a short one when the Memory Stick is full or a write fails.
- **Mystery Gift (Select + Down)** in a game that doesn't support it (anything but Pokémon
  FireRed, LeafGreen and Emerald) says so at once, instead of trying to connect for up to 30
  seconds. Its status messages now stay on screen long enough to read.
- **Online play:** the room-code box reads "random" until you type a code (it showed AAAA,
  which looked like the room you would get). GBAdhoc tries more ways to connect two PSPs
  directly before it falls back to the relay, and a PSP on the same home network as the relay
  it uses now finds it there. A refused online session (for example, "Link Cable needs a
  restart of the game") now disconnects cleanly.

### PSP-1000

- **About 1.4 MB (1,440 KB) more free memory at boot than 4.5.0,** from smaller sound and
  audio buffers, leaner Game Boy sound tables, unused buffers removed, and a smaller
  second-game image for link play. That memory goes to keeping more of a large cartridge in
  memory.

**Known issues:** online play is still experimental; only Pokémon Emerald has been tested over
the internet, and a relayed session on a high ping can run below 58 fps. Mario Golf: Advance
Tour's adapter lobby can still stick when both players press at the same moment (leave and
rejoin). Hot Wheels: Stunt Track Challenge, Top Gear Rally and Payback
still run below 58 fps. **Downgrading:** 4.6.0-beta.3 and older look only in `roms/` for saves
and states; move them back from `saves/` and `states/` by hand before going back.

## [4.6.0-beta.4.1] - 2026-11-07

Online hotfix. Links with 4.6.0-beta.4 (the link version is unchanged), but update both PSPs.

- Online sessions between two PSPs on the released beta.4 could end before they started: "The
  host left" on one side, or the joiner simply dropped. A cable session reloads the game first, and
  during that reload a PSP sent nothing at all; on a PSP with a slow Memory Stick the 16 MB Emerald
  reload took 18-25 s, so the partner gave up after 8-12 s and the relay dropped a silent player at
  20 s. A PSP that is starting now sends a keepalive for every megabyte it reads, a joiner waits up
  to 30 s for a host that is starting, and the relay (relay.rayquaza.ca, updated the same day) waits
  45 s before it drops a silent player.
- "The other PSP runs a different GBAdhoc version" no longer appears when the other PSP is only busy
  starting: that verdict now needs a host that is answering.
- The PSP-1000 has 8 KB less free memory than with beta.4 (one layout step).

## [4.6.0-beta.4] - 2026-11-07

Pre-release. Links only with the same beta.

- Screen filters are three rows per console under Settings > DISPLAY: **Smoothing** (nearest,
  bilinear, sharp bilinear), **Screen effect** (none, LCD grid, dot grid, scanlines, vintage
  LCD; GB/GBC also RGB stripes and RGB stripes strong) and **Colour** (GBA: native, GBA LCD
  colour, black & white; GB/GBC keep the GB palette row). The old single Video filter row could
  not combine smoothing with an effect, and its masks repeated every screen pixel, so at Fit
  (1.7x GBA, 1.89x GB) and Stretch they drifted and banded. Each mask now repeats once per game
  pixel at every scale. Effects are off at 1x, where a game pixel is one screen pixel, and RGB
  stripes need GB/GBC at Stretch (exactly 3 screen pixels per game pixel). GBA colour is applied
  inside the renderer, to the palette and to bitmap pixels; game memory is never changed. Old
  `filter_*` values carry over, and TRIANGLE's presets keep your effect and colour.
- Battery saves live in `saves/` and save states (with their pictures) in `states/`, instead of
  beside the ROMs (#31, asked for by MXPHunter). Both folders mirror `roms/`, so
  `roms/GBA/hacks/Game.gba` saves to `saves/GBA/hacks/Game.sav`. Empty `roms/GBA`, `roms/GB`
  and `roms/GBC` folders are made on first run for sorting by console; ROMs anywhere in `roms/`
  keep working and are never moved, so XMB entries and old folder layouts still work.
- Existing saves and states move the first time you open each game: copied, read back and
  checked (size and CRC32), then the original is removed. If anything fails, the old files stay
  and are used, and the move is tried again next time. Share game data moves a game's files the
  same way before replacing them.
- **The Save States page's buttons changed** (#31, MXPHunter): in game, X saves, Square loads
  and Triangle deletes (X used to load and Square to save); on the game list's state shelf X
  loads and Triangle deletes (Square used to delete). Every question is the same as before and
  X answers it.
- Loading a state asks first, on the Save States page and from the Select + R quick load:
  "Load Slot n? Unsaved progress is lost." X loads; any other button keeps the game you are
  playing. A load replaces everything since the last save, and the quick load sits one shoulder
  away from the quick save. The quick load's question can be turned off under Settings > Quick
  load asks first (`quick_load_ask = 0`); it follows the quick load to whatever buttons it is
  mapped to (PR #32 by Chandler Hardy).
- More of MXPHunter's requests (#31): Settings > Quick save asks first (off by default,
  `quick_save_ask`) before the quick save overwrites slot 1; Settings > Volume, 0 to 100 in
  steps of 10, applied as the PSP's own channel volume (100 is the output of every earlier
  release, bit for bit); the build you are running under the SETTINGS title, read from
  `VERSION.txt`; d-pad arrows instead of the words Left/Right/Up/Down in the hints; on the game
  list, `◀ states` / `▶ cheats` light up when the highlighted game has states or a cheats file,
  long names scroll on the selected row, and a thin bar shows your place in a long list; and the
  "app" screenshot folder is now `GBAdhoc/Screenshots/` instead of `log/`.
- The in-game Video preset bound to **none** (Square on the Controls page) is really none (#31,
  reported by MXPHunter). In a rewind build an unbound preset lets the rewind button cycle the
  preset while rewind is off, and an explicit none was the same setting, so it was overridden.
  Unbound is now *auto* (the old behaviour, a grey "auto" chip) or *none* (`bind_video = OFF`).
  Older builds read OFF as their default.
- **XMB launch mode** (#29, asked for by lipowskm): GBAdhoc can be started with a ROM, so a game
  can have its own entry in the XMB's Game column that opens straight into it. An entry is a
  small launcher, its art and a `launch.ini` naming the ROM inside GBAdhoc's `roms` folder; no
  ROM is copied. *Exit* returns to the XMB, *Quit to game list* opens the list, and a ROM that is
  not there opens the list. Entries are made with the GBAdhoc XMB apps (Windows and PSP), which
  ship separately. A game is the same game from its entry and from the list: the same saves,
  states, settings and cheats.
- **Online play (experimental, off by default).** Two PSPs can link over the internet instead of
  ad hoc Wi-Fi: turn on Settings > Online play, then in game open Wireless > Online. The host
  creates a room and gets a 4-letter code; the other player enters it and joins. Both PSPs use an
  infrastructure Wi-Fi connection saved in the XMB (2.4 GHz only; WPA2 needs ARK-4's WPA2 plugin)
  and meet through the GBAdhoc relay at relay.rayquaza.ca, which forwards the same link traffic two
  PSPs exchange over ad hoc; a direct path is tried when both routers allow it, with the relay as
  the fallback. The link sessions are the ad hoc ones, but the GB cartridge stream and Share game
  data are not offered online, so both players need the game. Recent partners are not built yet.
  Only Pokemon Emerald has been tested online: a Cable Club trade ran start to finish between a
  PSP Go on a phone's mobile data and a PSP-3000 on home Wi-Fi, through the relay (36 ms ping,
  about 42 fps: a relayed session waits for each frame). The Network row lists Automatic and every
  connection saved in the XMB by name, also on the Mystery Gift screen; a room code the host types
  is the room it gets; Join takes a 4-letter room code or an 8/11-character invite code from
  GBAdhoc XMB 0.2.0-beta.1 or later; a host waiting alone keeps its room; a chip shows the path
  and ping (Direct or Via relay); and a failed connection says what happened. A link between a PSP
  with gba_bios.bin and one without falls out of step within seconds, so it is refused before it
  starts. The session code lives in its own module, gbadhoc_online.prx (28 KB), loaded only when an
  online session starts, so online play costs no memory and nothing per frame until it is used.
- Ambient bars are None, Art or Game. **Game is a live edge glow**: the bars take their colours
  from the edges of the picture and follow the game smoothly (48 samples every other frame, eased
  over 8 frames), at no measurable speed cost on the PSP-1000 or PSP-3000. Art shows the game's art,
  or the edge glow for a game without art, and switching back to Art mid-session works.
- 222 MHz plays smoothly again at normal speed (reported by ingo3643). The lower-latency
  presentation added in 4.5.0 shows each frame the moment it is ready; at 222 MHz the Media
  Engine often finished a frame just after the screen refreshed, so that frame showed late and
  the next one was skipped (about 140 hitches a minute in FireRed on beta.3). Below 333 MHz a
  frame that misses its refresh is now presented on the next one, and the early flip waits for
  it instead of replacing it: 0 hitches a minute in FireRed and Fire Emblem on both PSP-1000 and
  PSP-3000, with input latency about 33 ms below 333 MHz (4.1.0 was about 67 ms). When the picture
  does fall a refresh behind, the audio nudge now comes early, so it catches up within frames
  instead of up to about 5 seconds. At 333 MHz GBAdhoc presents exactly as in beta.3.
- The game list scrolls without lag again: the new state/cheat hints and the SAVE badge looked
  files up on the Memory Stick for every step (a regression in beta.4 test builds only); each
  game's marks are now cached and filled in on idle frames.
- Layout re-pinned for the new code (pads 304 / 0).

**Known issues:** on hardware each frame takes about 1-3 % more CPU time than in beta.3 (Lab,
both consoles): frame rates are unchanged in the 8 single-player scenes and CPU-bound link
sessions are within 1 %; the cause is not yet attributed. The PSP-1000 has 32 KiB less free
memory than with beta.3 (filters v2 24 KiB, folders 8 KiB). Mario Golf's adapter lobby can
still stick when both players press at the same moment (leave and rejoin). **Downgrading:**
4.6.0-beta.3 and older look only in `roms/` and will not see moved saves or states; move them
back from `saves/` and `states/` by hand before going back.

## [4.6.0-beta.3] - 2026-11-05

Pre-release. Links only with the same beta.

- The Morrow's Golden Country no longer slows to ~2 s a frame and freezes after a few combat
  rounds (#10; PR #30 by Chandler Hardy). Its sound mixer ran code through RAM mirror addresses
  (IWRAM repeats every 32 KiB, EWRAM every 256 KiB), each mirror got its own translation, and
  RAM code was flushed about once a frame. Code reached through a mirror now shares one
  translation.
- That fix first broke Pokémon MonoMyth's first trainer battle in testing: MonoMyth runs battle
  code from an EWRAM mirror on purpose, and its branches depend on the mirrored address. Code
  reached through a mirror now keeps its mirrored address while sharing the translation. Fixed
  before release; it never shipped.
- Fast-forward shows its pictures evenly. 3x flipped its pictures unsynchronised with the LCD
  and an early flip could replace one that had not yet been shown, and Smooth and Unlimited
  drifted against the LCD's refresh. On the PSP-3000 at full speed, 3x now shows ~60 new
  pictures a second with none skipped (was ~56, with 7-9 % skipped).

**Known issues:** at 222 MHz, normal-speed play hitches (about 100 per minute in FireRed),
because the lower-latency presentation added in 4.5.0 misses the screen refresh when the PSP
is this slow. 333 MHz is unaffected. A fix is planned for the next beta. Mario Golf's adapter
lobby can still stick when both players press at the same moment (leave and rejoin).

## [4.6.0-beta.2] - 2026-11-03

Pre-release. Links only with the same beta (the Wireless Adapter protocol changed).

- Wireless Adapter play is scheduled frame by frame: each PSP sends its frame's adapter traffic
  with a marker and waits only when that marker is really missing. Lag no longer builds up,
  resends fall from 40-60 % to under 5 % in Mario Tennis, and the 57 fps cap on wireless
  sessions is gone.
- Settings -> CPU clock: 333 MHz (default), 222 MHz, or System (leave the CFW's clock alone).
- More ROM cache on the PSP-1000.
- Two PSPs on different GBAdhoc builds no longer start a link session: both are told, for the
  Wireless Adapter, Link Cable and GB/GBC alike.
- Sleep after *Quit to game list* no longer powers the PSP off: the Media Engine module stays
  loaded across the return to the list (#28; diagnosis and tests by Chandler Hardy).
- PSP Go: sliding it shut sleeps again during play (4.5.0 regression).
- Fire Emblem: The Morrow's Golden Country boots and plays (#10): an ARM word with condition
  NV is never executed (PR #26 by Chandler Hardy, his test ROM included), and a bad jump at the
  edge of the BIOS region no longer resets the game.
- The Morrow's Golden Country no longer stutters at ~46 fps: a fixed-base STMIA that rewrites
  code retires only its own span instead of flushing all RAM code (PR #27 by Chandler Hardy).
  47.5 -> 58.8 fps on the PSP-1000 and PSP-3000. MonoMyth and other games using ipatix's
  sound mixer benefit too.
- Release builds come from one command (`tools/release.sh`) with the build flags fixed in the
  source, so a release can't be built with the wrong set.

**Known issues:** Mario Golf: Advance Tour's adapter lobby can still stick when both players
press at the same moment (leave and rejoin). The Morrow's Golden Country has not had long play
testing. 222 MHz speed has not been measured.

## [4.5.0] — 2026-11-01

- Pokémon MonoMyth is compatible: live timer counters and the ARM7 instruction prefetch
  window, with no game-specific code.
- Self-modifying GBA code costs far less: rewritten code is patched or reused instead of
  retranslated. 46 test scenes average 26.7% higher frame rates than 4.1.0, and 68% on the
  scenes under 58 fps on 4.1.0, on both the PSP-1000 and PSP-3000.
- Lower input latency, and buttons are read right before each frame starts.
- Faster GB/GBC core: the timer catches up only when read, and linked PSPs run more of each
  frame in one go.
- Wireless Adapter: works after a cable session; the other PSP learns when a host leaves; a
  closed room leaves the list; no phantom players; less lag builds up on the joining PSP.
- New screen filters (LCD grid, dot grid, scanlines) and PNG screenshots in PICTURE/GBAdhoc.
- *Reset game* in the in-game menu, for GBA, GB and GBC.
- Need for Speed Underground, Big Mutha Truckers and Street Racing Syndicate start, and ROMs
  with accented names show up and open.
- Sleep is steadier around the menus; *Thumbstick → Custom* binds from the game list.
- GB/GBC battery saves are written by the writer thread, once cartridge RAM has
  held still for one check (at most ~30 s while it keeps changing); exit, sleep and
  quit still write at once. Fixes a hitch every ~5 s in GBC games that use battery
  RAM as scratch (Pokémon Gold/Silver/Crystal).
- Install folder back to `PSP/GAME/GBAdhoc`; plain "GBAdhoc" XMB title.
- A boot refused over an unreadable save shows the reason (unreadable file, or no
  memory for it) instead of returning silently to the XMB.
- Rewind: an unlogged save read during replay is reported again, not a game reset.
- netdrv: the unreliable broadcast path sends each peer's nonce (was uninitialised).

**Known issues:** Mario Tennis: Power Tour and Mario Golf: Advance Tour struggle over the
Wireless Adapter. The adapter path is the focus of the next updates.

Both PSPs must be updated for linking and sharing. See the [README](../README.md).

## [4.1.0] — 2026-10-14

- Protect previous battery images on failed full writes and failed loads; preserve
  exact written generations and drain pending saves before GBA cable RAM handoff.
- Correct GB/GBC timer phase, DIV/TAC edges, delayed TIMA reload and HALT clocking;
  fix audio, SGB masking and state validation.
- Restore rewind cartridge clock context and recover safely from restore failures.
- Reject malformed GBA state components, EEPROM bounds and invalid sound/timer state.
- Correct PSP worker, Media Engine, standby and USB ownership cleanup.
- Upgrade wireless and save-share admission to protocol version 2, reject stale traffic,
  and create PSP adhoc host groups with the SDK's explicit host API.
- Add bounded, disabled-by-default GBA GS1/AR3 RAM cheats.

Both PSPs must be updated for linking and sharing. See
[release notes](docs/RELEASE-4.1.0.md) for validation scope.

## [0.1.0] — unreleased

First release. Everything below exists and has been exercised; the "Known issues" section
is not a disclaimer boilerplate, it is the actual list.

### Added — the frontend

- **Native PSP frontend** hosting the gpSP libretro core in-process (statically linked, no
  RetroArch). Boots to a ROM browser, runs GBA games, exits cleanly with the save flushed.
- **Video:** GU-accelerated blit with scaling presets `1x` / `fit` / `stretch`, each with
  nearest or bilinear filtering — Triangle cycles the five presets in-game, and the choice
  persists in `config.ini`.
- **Audio:** core mixes at 65536 Hz, resampled to the PSP's 44100 Hz output on its own
  thread through a lock-free ring; the ring's fill level is the pacing master.
- **Fast-forward** on Square (hold by default, toggle optional), multipliers 1.5× / 2× / 3×
  / uncapped, core frameskip engaged while active, on-screen chip. Forced to 1× while a
  wireless session is live.
- **In-game menu** on `Select`+`Start` held ~¼ second: Resume / Save state / Load state /
  Wireless / Settings / Exit. No HOME-button hooking, so it is CFW-safe.
- **Settings** (persisted): video scale, video filter, FF multiplier, FF hold-vs-toggle,
  room code.
- **Savestates**, one slot per ROM (`<rom>.st0`), staged through a static buffer so they
  never compete with the core's ROM allocations.
- **CPU clock** set to 333/333/166 MHz at init and logged, per the project's
  overclock-first policy for CFW hardware.
- **Structured `EVT` log** at `log/frontend.log` — a product feature, not scaffolding: it
  is what makes a field failure diagnosable without the user in the debug loop.
- **XMB branding:** title "PSP AGB" (after the AGB-015 adapter), custom ICON0/PIC1.

### Added — wireless

- **`netdrv`**, a full-mesh reliable-datagram driver implementing the libretro
  `retro_netpacket` contract: sliding-window ARQ with retransmission and duplicate
  detection, CRC + exact-length framing, host-broadcast roster with generation counters,
  asymmetric death authority, and per-peer channels. All core traffic is
  `RELIABLE|FLUSH_HINT`, which is what the emulated adapter demands.
- **PSP ad-hoc transport** over `sceNetAdhoc` PDP datagrams: adhocctl group create/join,
  a dedicated RX thread, blocking receive in timeout slices so teardown never hangs. Static
  singleton allocation — a session can't fail to start because the heap fragmented.
- **UDP transport** for the desktop development twin, with injectable packet loss and
  jitter for impairment testing.
- **Wireless UI panel:** Host session, Join by scan (~10 s ad-hoc scan with a results
  list), Join by room code (`GPSP00`–`GPSP99`), live session status row, Disconnect, plus
  a persistent session chip during play.
- **Save auto-backup:** `<rom>.sav` → `<rom>.sav.bak` before every session start.
- **Savestates blocked** (save *and* load) while a session is active, with a toast — the
  core does not serialise adapter state, so a mid-session load would desync both consoles
  permanently.
- **Silent-wireless mode** for per-game "invisible" builds: the session negotiates itself
  (join-first, promote-to-host on a jittered timeout) at the moment the game activates the
  adapter, with zero emulator UI. See `docs/VARIANTS.md`.
- **Per-game variant builder** (`tools/make_variant.sh`): an EBOOT with its own XMB title
  and icon that boots straight into one baked ROM, keeps saves in a private namespace, and
  brings wireless up silently.

### Added — validation

- An autonomous PPSSPP-based end-to-end harness (`tools/e2e/`) that drives the real EBOOT:
  boot, 30-minute soak, in-game save persistence across restarts, GU colour-order
  regression check against real GE readback, UI smoke, transport echo, and a **fully
  autonomous Emerald ↔ Emerald Union Room trade with a party-swap oracle checked in both
  RAM and the post-run `.sav`** — on the desktop twin under 5 % packet loss + 30 ms jitter,
  and on two PPSSPP instances over the PSP ad-hoc transport. Plus peer-disconnect runs that
  assert the survivor recovers with the game's own error dialog and exits cleanly.
- `tools/make_release.sh` (this release's packaging) and `docs/HARDWARE-ACCEPTANCE.md`
  (the two-console hardware gate).

### Changed — the emulator core (minimal, intended for upstream)

- **`rfu.c`: link-frame queues 4 → 16** (`RFU_PKT_QUEUE`). The core buffered
  netpacket-delivered frames in fixed 4-deep queues and silently discarded overflow — a
  depth that assumes a zero-latency transport. Any real transport delivers legitimate
  clumps deeper than 4, gen-3 games never retransmit their one-shot commands, and a dropped
  clump wedges both games with no diagnostics. Two array sizes and the matching
  enqueue/dequeue loops; no logic change; behaviour-identical at zero latency. Relevant to
  upstream RetroArch netplay over a WAN, not just to us.
- **`rfu.c`: weak `gpsp_rfu_activated_hook()`** fired when a game completes the adapter
  login handshake — 8 lines, a no-op for any frontend that doesn't override it. This is
  what makes silent wireless possible without per-ROM RAM tables.

- **`rfu.c`: weak `gpsp_rfu_link_down_hook(reason, slot)`** — the symmetric partner of the
  activated-hook, fired when the emulated RFU link ends: locally (`RFU_CMD_DISCONNECT`, i.e.
  the player left the room), on a peer `NET_RFU_DISCONNECT`, on adapter reset, or on a
  host-side client timeout. Also a no-op for any frontend that doesn't override it. Nothing
  in the libretro netpacket contract tells a frontend that the *core's* wireless session
  ended — `netpacket_stop()` is frontend-driven only — so a log could not distinguish "the
  game ended the session" from "the frontend tore the transport down". Now it can.
- **`rfu.c`: two receive-path bounds checks.** `NET_RFU_CLIENT_SEND` took its payload length
  from a header byte (0–255) straight into a 16-byte buffer, with no `len >= blen + 12`
  guard — unlike `NET_RFU_HOST_SEND` immediately above it, which has both — so a malformed
  or foreign packet could read past the received datagram and overwrite neighbouring client
  slots. `NET_RFU_BROADCAST` read 6 payload words behind a `len >= 12` gate. Legitimate
  senders cap both, so no working behaviour changes; this is hardening, not a diagnosed
  fault.

All four are offered upstream with this release (see `docs/RELEASE.md`).

### Known issues

- **Wireless session stability on real radio is being actively fixed.** In a field session
  with two real consoles the games discovered each other and began entering the Union Room,
  then dropped to "communication error" a few seconds later. The cause is understood and
  quantified from both consoles' logs: our ARQ retransmit timeout is a fixed 30 ms floor
  tuned on loopback, which is far shorter than real ad-hoc round-trip time, so we
  retransmit before an acknowledgement can possibly arrive. That produces a duplicate storm
  (68 % of received traffic was duplicates; ~3.8 transmissions per delivered payload
  against 1.1 in the emulated harness), the send window stalls, the transmit queue fills,
  and **two RELIABLE payloads were silently dropped** — which destroys the emulated
  adapter's state machine even though the radio and our session stay nominally healthy.
  Nothing was actually lost on the air: `txfail=0`, `rxerr=0`. The fix is a correctness
  change (never silently drop RELIABLE payload — apply backpressure or fail the session
  visibly) plus an adaptive RTT-based retransmit timeout with a PSP-ad-hoc floor. The
  emulated harness cannot reproduce this class at all, which is exactly why it survived to
  the field.
- **Peer discovery requires a fixed ad-hoc channel.** Two consoles on `Automatic` each
  create their own identically-named group and never see each other, silently. Documented
  as a prerequisite rather than fixed in code; a scan-then-join-with-retry path that cannot
  fall through to creating a rival group is the intended code-side improvement.
- **Cross-edition trade support is incomplete.** The automated trade oracle covers Emerald ↔
  Emerald. FireRed ↔ LeafGreen (and cross-edition combinations generally) need their own
  autopilot address tables built from pret rev-1 symbols before they can be claimed as
  tested. The games themselves are expected to work; we simply haven't proven it.
- **PSP-1000 memory headroom is tight.** The core allocates the ROM greedily and a wireless
  session adds roughly 300 KiB of driver plus the system's network pool and thread stacks
  on top. It fits — with a 16 MiB ROM fully resident there was headroom to spare in
  measurement — but the 1000-series is the binding constraint, and the ARQ window and queue
  depth are already halved there relative to the desktop build. The field run also suggests
  the 1000 pumps the driver more slowly than a 3000 and generates more traffic; engaging
  frameskip while linked is on the list.
- **No suspend/resume handling.** The frontend keeps the console awake during a session but
  does not tear the session down on suspend or rebuild it on resume. Behaviour across a
  suspend is currently unknown and is an explicit observation step in the hardware gate.
- **Re-linking after a disconnect** requires going back through the game's own wireless
  counter; the emulator does not attempt to re-establish a dropped session by itself.
- **Two-console hardware acceptance has not been signed off.** Single-player on real
  hardware is verified (Pokémon Emerald at ~59 fps of 59.73 at 333 MHz on a CFW PSP); the
  full wireless trade is verified in emulation only. `docs/HARDWARE-ACCEPTANCE.md` is the
  gate that closes this, and it is gated behind the wireless fix above.

### Not in scope

Link **cable** emulation, internet play, infrastructure-mode WiFi, lobby servers, GB/GBC
games, e-Reader, more than 5 players, RetroArch feature parity, and non-PSP platforms. The
desktop build exists as a development twin and test rig, not as a product.
