# A game's own XMB entry (issue #29)

Each game can have its own entry in the XMB's Game column, with its own icon and background,
that opens straight into the game. Entries are made on a PC with **GBAdhoc XMB Entry Maker**
(`tools/xmbwrap`); a maker that runs on the PSP itself is next.

## The design

- **A small launcher per game, not a copy of the emulator.** An entry is a folder
  `PSP/GAME/<title>/` holding an `EBOOT.PBP` of about 130 KB plus art, and a `launch.ini`. The
  EBOOT's code is the same launcher for every game (`psp/launcher/`); what makes it a game is
  its PARAM.SFO title, its ICON0 (144x80) and PIC1 (480x272), and `launch.ini`.
- **The launcher starts the installed GBAdhoc** with
  `argv = { <GBAdhoc>/EBOOT.PBP, "launch", <rom> }`, through the firmware's own launch call for
  the device it is on (`sctrlKernelLoadExecVSHMs2` on a Memory Stick, `...Ef2` on a PSP Go's
  internal storage). Updating GBAdhoc updates every entry. Saves, save states, settings, cheats
  and art are the game list's own: a game is the same game from its entry or from the list.
- **No ROM is copied into an entry.** `launch.ini` names the ROM relative to GBAdhoc's `roms`
  folder, so an entry is never a shareable copy of a game.
- **GBAdhoc's launch mode** (`main_psp.c`, XMB LAUNCH at `relaunch_eboot`): the first session
  boots the named ROM with the list's own save and state paths. *Exit* returns to the XMB;
  *Quit to game list* opens the list. A ROM that is not there opens the list instead.

## launch.ini

```
rom = Pokemon - Emerald Version (USA, Europe).gba
gbadhoc = PSP/GAME/GBAdhoc
```

- `rom`: relative to GBAdhoc's `roms` folder (subfolders allowed), or an absolute `dev:/` path.
  A name with non-ASCII characters is written in its FAT 8.3 form, which the firmware always
  opens (the Windows maker reads it off the card; GBAdhoc's list does the same since issue #19).
- `gbadhoc`: GBAdhoc's folder. A bare `PSP/GAME/...` is looked for on the entry's own device
  first, then on the other one (ms0: and ef0:), so an entry on a PSP Go's internal storage can
  start a GBAdhoc on its Memory Stick and the other way round.

The launcher checks both before launching and says on screen what is missing.

## Making entries on a PC

1. Connect the PSP in USB mode.
2. Run `GBAdhoc-XMB-Maker.exe`, press *Browse* and pick the game in GBAdhoc's `roms` folder on
   the PSP. The title and GBAdhoc's own art for that game (its `hero` background and `boxart`
   icon) are filled in.
3. Change the title, the icon or the background if you like (any PNG, JPEG or BMP; the icon is
   fitted into 144x80, the background fills 480x272), then *Create XMB entry*.

Command line: `xmbwrap.exe --rom-file G:\PSP\GAME\GBAdhoc\roms\Game.gba [--title T]
[--icon IMG] [--background IMG] [--no-art] [--folder NAME]`.

## Building and testing

- Launcher: `psp/launcher/Makefile` in the pspdev image.
- Maker: `tools/xmbwrap/build_win.sh` (mingw-w64 in docker; the launcher is built in).
- Host test: `tools/xmbwrap/test_xmbwrap.sh`: a fake card, three entries, each checked by
  `check_entry.py` (PBP layout, DATA.PSP is the launcher's, art sizes, launch.ini), and every
  PARAM.SFO byte-identical to the SDK's own `mksfoex` for the same title.
- On hardware: an entry on the Memory Stick and one on a PSP Go's internal storage; Exit, Quit
  to game list, sleep in the launched game, a save made from the entry seen from the list.
