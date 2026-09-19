<p align="center">
  <img src="assets/gbadhoc-logo.png" alt="GBAdhoc — PSP GBA emulator" width="480"><br>
  <sub><b>GBAdhoc 2.1.0</b></sub>
</p>

**A GBA emulator for the PSP that runs the renderer on the console's second CPU and
carries GBA Wireless Adapter multiplayer over the PSP's own ad-hoc WiFi.**

The first (and only) GBA Emulator with Wireless Adapter Support!
Trade your friends, Partake in Link battles, recieve Mystery Gifts and more!
It also just plays GBA games, and it is fast: Fully compatible with the PSP-1000,
The secret sauce is a NEW and IMPROVED dual-core Media Engine Frame Renderer.
Now 1.45ms Faster! (woohoo)

### [**⬇ Download GBAdhoc 2.1.0**](https://github.com/ShoshinFauteux/GBAdhoc/releases/latest)

Unzip to the **root of your memory stick** — it lands in `PSP/GAME/GBAdhoc`. Put your
`.gba` files in `roms/` and launch it from the XMB. No BIOS needed. Subfolders under
`roms/` are fine, two levels deep; the browser shows one alphabetical list regardless.

![Browsing the library](gpsp/docs/img/browser-marquee.gif)

## What you get

| | |
|---|---|
| **Mystery Gift** | Receive real Gen-3 Wonder Cards from an Android phone acting as the distribution station. **SELECT + DOWN** in game. See [Mystery Gift](#mystery-gift). |
| **Wireless multiplayer** | Full AGB-015 Wireless Adapter emulation carried over PSP ad-hoc. Trades, link battles, Union Room, Mystery Gift. **No Internet connection required** |
| **Wake from sleep** | Slide it shut, flick the switch, and come back whenever. Your game is exactly where you left it, under a translucent *Continue / Quit to game list* prompt. On by default. |
| **Two processors, both working** | The PSP's second CPU draws every frame while the first emulates the next one. Full speed on a PSP-1000, and **past 170 fps** with fast-forward uncapped. |
| **Three fast-forward presets** | **3x**, **Unlimited** and **Unlimited Smooth**. 3x holds a steady triple speed. Unlimited skips frames to go as fast as it can, **past 170 fps**. Unlimited Smooth draws every single frame instead, slower at roughly 100 fps, but far nicer to watch. Hold or toggle, your choice. |
| **A browser worth using** | Two layouts **Marquee** for one game and its art per screen, **Shelf** for more of the library at once, each in light or dark. Four looks from one setting pair. |
| **Save states on the shoulders** | Select + L saves, Select + R loads, one slot per game. Your battery-save SRAM is written out on exit, on HOME and periodically as you play, with a backup kept. |
| **Runs the hard stuff** | Pokémon Unbound and other CFRU hacks at full speed. Emerald, FireRed and LeafGreen all link. |
| **No BIOS needed** | An open-source replacement is bundled. Drop in a real `gba_bios.bin` if you prefer. |
| **All three PSPs** | 1000, 2000/3000 and Go, tested on hardware. |

---

| | |
|---|---|
| ![Marquee, dark](gpsp/docs/img/browser-marquee-dark.png) | ![Marquee, light](gpsp/docs/img/browser-marquee-light.png) |
| **Marquee**, one game per screen, its art behind it | The same, in the light theme |
| ![Shelf, dark](gpsp/docs/img/browser-shelf-dark.png) | ![Shelf, light](gpsp/docs/img/browser-shelf-light.png) |
| **Shelf**, more of the library at once, box art and saves at a glance | Light theme |
| ![Wake prompt](gpsp/docs/img/wake-dark.png) | ![Settings](gpsp/docs/img/settings-dark.png) |
| **Wake from sleep**, your game is still there, underneath | Settings |

---

## 2.1.0

Faster where it was slowest, and a hard freeze found and fixed.

### The sound mixer was costing a third of the frame

Pokemon games build their sound mixer at runtime and then rewrite its instructions as
they play. Every one of those writes forces the emulator to throw away compiled code and
recompile it, and a busy battle does that about twelve times a frame.

The emulator already knew how to handle this cheaply, retiring only the handful of
compiled blocks that the write actually touched. But it only recognised **one** game's
mixer by sight, and Heart & Soul's sits a few bytes further along, so it failed the check
and took the expensive path every single time: one write, thirty recompiles.

It now recognises the whole family of mixers rather than one specific address. On Heart &
Soul's heaviest battle that is **37% less work per frame, and 57 fps becomes a solid
59.8** — recompiles per measurement window fell from 107,416 to 10,778. Unbound and
everything already running at full speed are unaffected; they were passing the check
before.

**Known issue: a brief visual artifact on some attacks.** In Heart & Soul you may see a
sprite drawn in the wrong place for a few frames when certain attacks land. It is
deterministic and reproducible, and it is being worked on.

**Switching Heart & Soul to its classic soundtrack avoids it entirely**, if it bothers you
more than the music does. Running that game in **mono** also gives the sound engine less to
do, which is worth setting either way.

Testing has been extensive, on real hardware rather than in an emulator, but this is a
large change to how the emulator handles self-rewriting code and there may well be a bug
here and there. Please raise anything you find.

### The freeze

A compiled block could end up containing a direct jump to an address the translator had
failed to resolve. That is a jump into unmapped memory, and because nothing called the
dispatcher on the way there, the existing safety net never saw it, so the console locked
up hard instead of recovering. Those exits now go to a permanent known-good block and
emit no code at all.

Separately, the emulator's memory of which addresses rewrite themselves is now wiped when
a ROM loads, so one game's habits can no longer be applied to the next one.

**What backs it up:** 36 unattended runs across a PSP-1000 and a PSP-3000, both test
games, all three fast-forward presets, second-core renderer on. 1440 save-state reloads
with fast-forward toggling throughout, zero failures and zero lockups. Reloading a state
restores the game's memory underneath a translation cache that keeps its compiled code,
which is exactly where stale state would show itself if any survived.

The original freeze was rare and has never been reproduced on demand, so this prevents
the mechanism that was demonstrated rather than proving the symptom gone for good.

### Upgrading from any earlier version

Copy the new `EBOOT.PBP` and `gbadhoc_me.prx` over your existing install, then **delete
your old `config.ini`** — several defaults changed, and a stale file keeps the old ones.
Your `roms/`, battery saves and save states are untouched.

## The second processor

The PSP has two CPUs. The second one, Sony's Media Engine, sits idle in almost every
homebrew emulator; GBAdhoc gives it the entire GBA renderer.

The timing is what makes it pay. A GBA screen is 160 lines drawn top to bottom, and the
instant line 160 is done the picture is final — nothing the game does for the rest of the
frame can change it. But the console still has the whole vblank period before it has to
show anything. So the drawing is handed over at line 160 and both processors work at once
for the remainder.

Two details carry it. The Media Engine's first act is to take a private copy of what it
needs and say so, which releases the main CPU to start the next frame instead of waiting.
And it copies only the video memory the game actually wrote — typically 8 KB of 96 KB,
tracked by a per-page map — because handing over all of it was slower than not offloading
at all. The emulator gets roughly 750 microseconds of every frame back. About 85% of
frames finish inside their own frame; a miss simply presents one frame later, which is
what every earlier version did for every frame.

## Requirements

- A PSP running custom firmware (developed and tested on **ARK-4**). Works on the
  **PSP-1000, 2000/3000 and Go**, all three are tested.
- A GBA BIOS is **not** required: an open-source replacement is bundled. Drop a real
  `gba_bios.bin` in the app folder if you would rather use one.
- For wireless: **two** PSPs, both with the WLAN switch on, both on the **same fixed
  ad-hoc channel**.

---

## Mystery Gift

Gen-3 Mystery Gift works, from an Android phone. The phone stands in for Nintendo's
distribution station, and the game receives a real Wonder Card through its own Mystery
Gift menu — nothing is written into your save by the emulator.

**Get the app:**
[**Mystery Gift Station**](https://github.com/ShoshinFauteux/MysteryGiftStation)

1. On the phone, start a normal Wi-Fi hotspot named exactly **`Mystery Gift`**, with
   **security Open** and the band set to **2.4 GHz**. The PSP's radio is 2.4 GHz only and
   will not see a 5 GHz hotspot.
2. Open Mystery Gift Station and pick a gift.
3. On the PSP, in-game, press **SELECT + DOWN**. GBAdhoc joins that hotspot and starts
   listening — it creates the connection profile itself, so there is nothing to set up in
   the PSP's network settings.
4. In the game, go to **Mystery Gift** on the title screen and receive the card as you
   normally would.

**SELECT + DOWN** again stops it. Mystery Gift and wireless trading cannot run at the same
time; the emulator will tell you to stop one before starting the other.

Full protocol notes, the gift list and troubleshooting live in the app's repository.

## ⚠ Read this before you try wireless

### 1. The physical WLAN switch must be ON

It is in a different place on every model, and the PSP Go has no switch at all, it is
a setting. If it is off you now get a message that says so, rather than a puzzle.

### 2. Both consoles must be on the SAME FIXED ad-hoc channel, not "Automatic"

`Settings → Network Settings → Ad Hoc Channel` on both consoles. Pick **1**, **6** or
**11** and set the same one on both. "Automatic" lets the two consoles choose different
channels, and two radios on different channels cannot hear each other no matter how
correct everything else is.

---

## Install

1. Copy the `GBAdhoc` folder to `ms0:/PSP/GAME/`.
2. Put your `.gba` files in `GBAdhoc/roms/`, loose or in subfolders (two levels).
3. Optional: box art in `GBAdhoc/boxart/`, hero art in `GBAdhoc/hero/`.

---

## Artwork

Two optional kinds of picture: **box art**, the small cover in the Shelf layout, and
**hero art**, the full-screen 480x272 backdrop behind Marquee. The browser works fine
without either.

**The one rule that matters: a picture is matched to a game by filename and nothing
else.**

```
roms/Pokemon - LeafGreen Version (USA).gba
hero/Pokemon - LeafGreen Version (USA).png     <- found
hero/Pokemon LeafGreen.png                     <- silently ignored
```

No database, no fuzzy matching, and a mismatch is not an error and prints no warning,
the game just shows no art. **If a card is not showing up, the name is wrong.** Same
rule for `boxart/`.

Ready-made packs, a tool that renames a pack to match the ROMs you actually have, the
prompt used to generate the existing set, and a script that derives a backdrop from box
art are all in the art repo, along with the instructions for each:

### [**GBAdhoc-heroart**](https://github.com/ShoshinFauteux/GBAdhoc-heroart)

Drop PNGs in and you are done. The console bakes each one to the GPU's own `.565`
texture format on first view, one game at a time while the browser is idle, so a big
library never stalls at boot. Packs ship those pre-baked, which skips that first-view
cost and dithers better than the console can.

The packs are **generated images**, not scans and not official artwork. They are kept
out of this repository because hero art derives from copyrighted box art, so if a pack
ever has to come down, the emulator is untouched.

## Using it

### ROM browser

Two shells, switchable in Settings:

- **Marquee**, one game per screen, its hero art filling the background.
- **Shelf**, a denser list with box art alongside.

D-pad to move, **×** to start, **Start** for Settings. Art loads while you are idle and
never while you are scrolling, so holding a direction stays smooth.

### In-game controls

| PSP | Does |
|---|---|
| D-pad | GBA D-pad |
| **○** | GBA **A** |
| **×** | GBA **B** |
| L / R | GBA L / R |
| Start / Select | GBA Start / Select |
| **△** | Cycle video preset (saved) |
| **□** | Fast-forward (hold by default; preset and hold/toggle in Settings) |
| **Select + L** | Save state |
| **Select + R** | Load state |
| **Select + Start**, held ~¼ s | In-game menu |

One state slot per game. The shoulder chords require Select held, so L and R behave
normally in play.

### In-game menu

**Resume · Save state · Load state · Wireless · Settings · Exit.** Exit flushes your
save, as does quitting with HOME.

---

## Saves

- **SRAM** is written to `roms/<game>.sav`, flushed on exit, on HOME, and periodically
  during play. A `.sav.bak` is kept.
- **Save states**: one slot per game, `roms/<game>.st0`.
- Sleeping does not touch your save; the game is still in memory exactly as it was.

---

## What to expect from which games

| | |
|---|---|
| **Pokémon Emerald / FireRed / LeafGreen** | Full speed. Union Room trades and battles work between two consoles. |
| **Pokémon Unbound** and other CFRU hacks | Full speed (59.9). Was 29 before translation gates. Run it on the **medium music preset** (in the game's own options) for the best results. |
| **Pokémon Heart & Soul** | Full speed (59.8) as of 2.1.0, including its heaviest battles — was 57. Set its sound to **mono** for the best performance. Tested hard, but see the known issue above; if you hit something odd, please raise it. |
| Most commercial GBA titles | Full speed. |
| Heavy 3D / Mode 7 titles | Varies; fast-forward and frameskip are there if you need them. |

---

## Status and known limitations

**Works, tested on hardware, on all three PSP models.**

- Wireless link battles complete; earlier builds only managed quick trades.
- Sleep/wake with the Media Engine live.
- Single-core rendering is **deprecated** but kept for diagnostics (`me_mode = 0`).

**Rough edges, honestly:**

- The wake prompt appears a beat after the screen lights. The LCD shows video memory the
  instant it powers on, and we blank it at suspend, so you get black then the prompt,
  not a stale frame, but not instant either.
- Link sessions are paced to 59.73 fps on both consoles. Gen-3 games count link timeouts
  in *frames*, so two consoles that disagree about how long a frame is will drop the
  link. Both must run the same rate.
- Hero art is not included in this repository, it is derived from copyrighted box art
  and lives in a separate pack. See
  [GBAdhoc-heroart](https://github.com/ShoshinFauteux/GBAdhoc-heroart).
- FireRed/LeafGreen wireless is less tested than Emerald.

---

## Building from source

Docker supplies the toolchain, so the build is reproducible on any machine.

```bash
# one profile name, one artifact, one manifest
gpsp/tools/build.sh release      # what a player installs
gpsp/tools/build.sh harness      # the hardware test rig
gpsp/tools/build.sh diagnostic   # release plus instruments
# -> gpsp/psp/EBOOT.PBP
```

The reproducible release procedure and current flag set are documented in
[`gpsp/docs/RELEASE.md`](gpsp/docs/RELEASE.md).

The three profiles exist because these flavours used to be defined in four separate
places, each carrying its own copy of a long flag list, and they drifted. The lists now
live in that one script, and `gpsp_profile.h` refuses combinations that are silently
wrong. All three share the same dynarec configuration deliberately: a diagnostic build
must be the *same emulator* as the release, or it answers a different question.

What differs is the frontend.

- **release** — telemetry compiled out entirely (not stubbed; the call sites are gone),
  and the harness ini path points at a filename that cannot exist, so a leftover
  `.gpsp-harness.ini` on your memory stick is inert.
- **harness** — adds telemetry and the autopilot so a job file can drive a console
  unattended. Titled **GBAdhoc HARNESS** on the XMB, because a harness build must never
  be mistaken for a playable one.
- **diagnostic** — release plus instruments that write to the memory stick, which is
  exactly why the release profile refuses them.

The script then reads the linked binary and fails if a profile's expected strings are
missing, or a forbidden one is present, because a build that quietly did nothing looks
exactly like a build that worked. `make` does not track CFLAGS, so the default is a full
clean rebuild: a stale object silently drops a flag, and that reads as "the optimisation
didn't help" rather than as a build error.

---

## Documentation

- [`docs/ARCHITECTURE.md`](gpsp/docs/ARCHITECTURE.md), how the pieces fit together
- [`docs/ADHOC-NOTES.md`](gpsp/docs/ADHOC-NOTES.md), the RFU protocol as we found it
- [`docs/DECISIONS.md`](gpsp/docs/DECISIONS.md), every design decision and why, including the
  ones that were wrong
- [`docs/TESTING.md`](gpsp/docs/TESTING.md), the harness
- [`docs/RELEASE.md`](gpsp/docs/RELEASE.md), how a release is built and verified

## Credits, this project stands on other people's work

**Everything that makes wireless multiplayer possible was built by other people. We wrote
a PSP shell and a radio transport around it.**

- **David Guillen Fandos (davidgfnet)**, maintainer and principal author of the modern
  gpSP core. He **reverse-engineered the GBA Wireless Adapter and implemented its
  emulation** (`rfu.c`, `serial_proto.c`), which is the single hardest and most valuable
  piece of this entire stack. His write-up:
  <https://www.davidgf.net/2024/01/13/gba-wireless-adapter/>. Repo:
  <https://github.com/davidgfnet/gpsp>.

- **mcidclan (m-c/d)**, the reason sleep/wake works at all. The PSP cannot resume with
  custom code on the Media Engine, because the kernel's own ME driver owns a system
  event handler that runs during suspend and finds hardware it no longer understands.
  His libraries identified and solved this, and GBAdhoc uses his mechanism (MIT):
  [psp-media-engine-custom-core](https://github.com/mcidclan/psp-media-engine-custom-core)
  and [psp-media-engine-safe-task](https://github.com/mcidclan/psp-media-engine-safe-task).

- **ShoshinFauteux**, project lead.

  I set the direction and ran every hardware test. Sleep/wake was my idea and I pushed
  for it after being told it probably wasn't worth the risk. The wake prompt was my
  design; I turned down two versions before the one that shipped.

  When four rounds of elimination on hardware had ruled out everything we could think of,
  I remembered mcidclan's libraries from an unrelated project. That is where the answer
  was.

  I also guessed people would press HOME at the wake prompt, which turned out to be a
  softlock with no way out.

  The rest was running builds on three consoles and reporting what actually happened,
  including the times it contradicted what the build was meant to prove.

- **Rodrigo Alfonso (afska)** and **Corwin (kuiper.dev)**, the community protocol work on
  the Wireless Adapter that the emulation is built on and cross-checked against:
  [gba-link-connection's `wireless_adapter.md`](https://github.com/afska/gba-link-connection/blob/master/docs/wireless_adapter.md)
  and [blog.kuiper.dev/gba-wireless-adapter](https://blog.kuiper.dev/gba-wireless-adapter).
- **libretro/gpsp**, the upstream repository this is forked from
  (<https://github.com/libretro/gpsp>), including its MIPS32 dynarec (which is why a PSP
  can run this at all), the rewritten video renderer, and the bundled open-source BIOS
  replacement. gpSP itself descends from **Exophase**'s original gpSP and **notaz**'s fork.
- **The libretro project**, the `retro_netpacket_callback` interface (env call 78) that
  lets a frontend carry a core's link traffic over any transport it likes.
- **pret** (`pokeemerald` / `pokefirered` decompilations), used to understand game-side
  RFU behaviour while debugging the input-eating gates.
- **PSPSDK / pspdev**, the toolchain and the `sceNetAdhoc` documentation-by-source.
- **Inter** by Rasmus Andersson (SIL Open Font License), the UI typeface.

Licensed **GPL-2.0**, same as gpSP (see `COPYING`). Upstream copyright headers are intact.
Changes to the core itself are kept minimal and are intended to be offered upstream.

---

## License

GPL-2.0. See `COPYING`.
