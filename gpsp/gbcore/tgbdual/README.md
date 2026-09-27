# TGB Dual core (from MasterBoy)

This directory is the Game Boy / Game Boy Color emulation core that GBAdhoc
runs for `.gb` and `.gbc` games.

## Provenance and credits

- **TGB Dual** — Game Boy emulator by **Hii**, Copyright (C) 2001.
  GPL version 2 or (at your option) any later version.
- **MasterBoy** — PSP port by **Brunni** (Florian), with earlier PSP work
  credited in the sources (LCK, ruka, RIN). MasterBoy turned TGB Dual's C++
  into C, moved the CPU registers out of a struct for `$gp`-relative access,
  added MIPS helpers, the `snd_render` sound path and the per-line renderer
  this port keeps. GPL v2 or later.
- `sgb.c` carries Super Game Boy code derived from **GEST** (Copyright (C)
  2003-2004 TM) and **VisualBoyAdvance** (Copyright (C) 1999-2004 Forgotten),
  both GPL v2 or later.

Source: <https://github.com/PSP-Archive/MasterBoy>, commit
`320bb5e1388df27f65f1044e366d77e7bd3cad4f` ("import v 2.02"), directory
`gbcore/`. `LICENSE` is that repository's `docs/license` (GPL v2).

Only the emulation core was taken. MasterBoy's menus, file browser, skins,
configuration system, ColorIt colourisation, zlib/OSLib glue and Master
System emulator are not part of GBAdhoc.

## Layout

The files here are the upstream `gbcore/` files **plus the local patches
listed in `LOCAL-PATCHES.md`**. `tgb_port.h` replaces the MasterBoy headers
the core included (`../psp/pspcommon.h`, `../renderer.h`, `../syscall.h`,
`../lib/zlib.h`, `<oslib/oslib.h>`), and `../gbcore_tgbdual.c` is the adapter
behind GBAdhoc's `gbcore.h` API.

`cheat.c` is vendored but not built: GBAdhoc does not offer GB cheats, and
compiling it out removes a 64 KiB lookup from every emulated memory write
(see `LOCAL-PATCHES.md`).
