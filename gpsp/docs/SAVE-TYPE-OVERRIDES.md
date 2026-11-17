# Save-type overrides

How the core decides which backup chip a cartridge has, the "no chip" type, and
when a game gets a row in `gba_over.h` instead.

| | |
|---|---|
| Detection | `gba_memory.c` `detect_backup_subcircuit()` (ROM strings) |
| Per-game table | `gba_over.h`, read by `load_game_config_over()` |
| Chip access | `gba_memory.c` `read_backup()`, `write_backup()`, `write_eeprom()` |

## Detection

A GBA cartridge carries at most one backup chip: SRAM, flash (64 or 128 KiB)
or EEPROM.  Nothing in the header says which one.  The core scans the ROM for
the library ID strings Nintendo's save libraries leave behind (`EEPROM_V`,
`SRAM_V`, `FLASH1M_V`, `FLASH512_V`, `FLASH_V`) and picks in that order:
EEPROM, then SRAM, then 128 KiB flash, then 64 KiB flash.  A ROM with none of
them starts as "unknown" and becomes SRAM on its first access.  `gba_over.h`
rows run first and can force a type (`FLAGS_EEPROM`, `FLAGS_FLASH_128KB`,
`FLAGS_NO_BACKUP`).

## The "no chip" type (`BACKUP_NONE`, `FLAGS_NO_BACKUP`)

For a cartridge with no backup chip on the board.  The SRAM/flash region
(0x0E000000) reads 0xFF and ignores writes, flash commands do nothing, and
EEPROM writes are ignored, so the game sees the empty bus a real cartridge
without a chip gives it.  The game cannot change backup memory, so its own
writes never trigger a save-file write.  Savestates store the type like any
other (a state saved under an older build keeps the type it was saved with).

It exists for games that test for a chip and refuse to run properly when one
answers.  A flash cart or reproduction always provides one, so a chip that
answers is how these copy checks spot a copy; to them, an emulator that always
emulates a chip looks like one.

### Top Gun - Combat Zones (USA), A2YE

The cartridge has no chip (the game uses passwords).  At boot the game writes
"TiTus Interactive Studio" to 0x0E000000 and reads it back, then runs a flash ID
probe; on reaching the main menu it runs a second flash ID probe.  Each probe
that finds no chip adds 17 to the halfword at 0x03003740, and the menu's
confirm handler returns without acting unless the sum is 51.  With a chip
answering, the menu cursor moves but A and START open nothing (KarnFu, issue
#9).  The ROM links the SRAM_V113, FLASH512_V131 and FLASH1M_V102 libraries
only to run these probes, so string detection picks SRAM, and the probe's
0xAA write to 0x5555 then turns it into flash.  Both fail the check.

Evidence: on beta.6 RC (Lab bc82f581a240), KarnFu's own state with only that
halfword changed (0x1122 to 51) opens every menu; with the A2YE row
(claude/kf-topgun, Lab 6652a8881acc) the cold boot reaches difficulty select,
plane select and the first mission in PPSSPP.  Emerald, Minish Cap, Mario Kart,
Golden Sun, Kirby & The Amazing Mirror, FE Sacred Stones, Aria of Sorrow and
Doom II are frame-for-frame identical to beta.6 RC over 2000 frames.

Other emulators reach the same verdict:

- mGBA `src/gba/overrides.c` L186-187 at 190a68d6:
  `{ "A2YE", GBA_SAVEDATA_FORCE_NONE, HW_NONE, GBA_IDLE_LOOP_NONE }`
  (https://github.com/mgba-emu/mgba/blob/190a68d6095d6d9ce71f0bf56a359d5db5589cfc/src/gba/overrides.c#L186-L187)
- VBA-M `src/vba-over.ini` L38-40 at 48504fe5: `[A2YE] saveType=5` (None);
  also `src/libretro/gba-over.inc` L438
  (https://github.com/visualboyadvance-m/visualboyadvance-m/blob/48504fe50fd5f315cb8647e2e7fea3425b090627/src/vba-over.ini#L38-L40)
- NanoBoyAdvance `src/platform/core/src/game_db.cc` L23 at 55b5cf0a:
  `BackupType::None`
  (https://github.com/nba-emu/NanoBoyAdvance/blob/55b5cf0ae3d929582ac5bfd486558173502b8354/src/platform/core/src/game_db.cc#L23)
- MAME `src/devices/bus/gba/gba_slot.cpp` L23 at a7230f9f: "multiple NVRAM
  chips detected, but none present (protection against emu?)"; MAME's
  software list `hash/gba.xml` gives the cartridge no save chip
  (https://github.com/mamedev/mame/blob/a7230f9f3b6b29b491fb2d4a8e44f1452c5ecc3a/src/devices/bus/gba/gba_slot.cpp#L23)
- https://zork.net/~st/jottings/GBA_saves.html: the only game with three
  save codes; if any matching chip is found, the main menu locks.

**Old savestates do not recover.**  The check runs at boot and its result
lives in the game's own RAM, so a state saved while the menu was dead (KarnFu's
`kf_top_gun_combat_zones` fixture holds 0x1122 at 0x03003740) stays dead on any
build.  Power the game on again (no progress is lost: it uses passwords); the
Lab fixture that proves the fix is `kf_top_gun_combat_zones_cold`.

## Why a table row and not a general rule

A general rule would be "a ROM that carries save strings for more than one
kind of chip has no chip".  Two emulators use one, and both need data to
survive it:

- SkyEmu (`src/gba.h` `gba_search_rom_for_backup_string`, commit bc5ec395,
  added for this game, SkyEmu issue #34) matches the prefixes `EEPROM_`,
  `SRAM_`, `FLASH_`, `FLASH512_`, `FLASH1M_`.  Over the 73 unique ROMs in the
  owner's library (2026-11-16) it flags Top Gun and also Pokemon Mystery
  Dungeon - Red Rescue Team (B24E), whose texture names (`FLASH_01`,
  `FLASH_02_L`, ...) sit beside its real `FLASH1M_V102`: that rule would drop
  PMD's saves.
- MAME (`gba_slot.cpp` `gba_chip_has_conflict`) uses the strict `_V` strings and
  disables the chip on a conflict, except for a list of 33 retail games that
  carry several save strings and do have a chip (Medabots AX, Onimusha
  Tactics, Kim Possible 2, Super Monkey Ball Jr., Rocky, Breath of Fire (J),
  One Piece (J), Rockman EXE 4.5, ...).  A general rule that is right needs 33
  per-game exceptions to save one game.

The strict rule flags only Top Gun in the owner's library, but none of MAME's
33 exceptions are in it, so the library cannot show the rule is safe.  One row
for the one game is smaller and cannot lose anyone's save.

## Candidates (not added)

| Game | Codes | Source | Status |
|---|---|---|---|
| Iridion II (USA, Europe) | AI2E, AI2P | mGBA `FORCE_NONE` (issue #196: freezes at the first boss when a chip answers); VBA-M libretro `gba-over.inc` L183-184 | Pending.  AI2E is in the library; 12,000 frames of boot, menus and the tutorial are identical with and without a row, but the test never reached the first boss.  Needs a state before the stage 1 boss or play on hardware. |
| Stuart Little 2 (USA/Europe, France) | ASLE, ASLF | mGBA `FORCE_NONE` (issue #2843) | Not added: mGBA's reason is cosmetic (an empty 32 KiB save file); no failure is known. |

## Adding an entry

A `FLAGS_NO_BACKUP` row (or any forced save type) needs both:

1. A reproducible failure on our core: a Lab fixture or a PPSSPP run that shows
   it, and the same run passing with the row.
2. The same verdict from a reputable emulator's table (mGBA, VBA-M,
   NanoBoyAdvance, MAME), cited in the row's comment with file, line and commit.

The row's comment says what the game does and names the sources.  A list of
game codes and save types is factual data; credit the table it came from.
