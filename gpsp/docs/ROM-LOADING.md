# ROM selection delay and loading feedback

## Diagnosis

Selecting a ROM called `pcfg_save()` to remember `last_rom`. That function
performed 36 separate read/truncate/write/close cycles on CONFIG.INI, one per
setting, before opening the ROM. The browser showed its last frame throughout
settings persistence, core initialization, ROM reads, save loading and renderer
startup. This made a slow storage operation look like a crash.

This is a confirmed unnecessary I/O cost, not yet a measured explanation of the
entire reported hardware delay. The 36-write path already exists in `f43e89e`
(2.0); the older `37ff767` version had 22 writes. The core loader, shared host and
INI writer are identical in the FF Profiles, Gift Fix 1 and Gift Polish source
snapshots. There is no evidence that the latest Mystery Gift polish introduced
a new full-ROM scan or extra ROM read.

## Change

- Browser selection uses `pcfg_remember_rom()` to update only `last_rom`, once.
  Selecting the same ROM again performs no settings I/O. Settings-menu saves
  retain their validation and full persistence path. Unknown keys/comments are
  preserved; failed writes leave the remembered name unchanged for retry.
- A themed loading screen appears before that write. It shows the ROM name,
  startup stage, a corner activity indicator, and real read progress.
- `fe_host_config.boot_status` is an optional synchronous boot-stage callback.
  `gpsp_rom_load_progress` reports completed reads between the existing 1 MiB
  read calls, and `(0,0)` before cartridge setup. PSP installs the observer only
  around `fe_host_boot()` and removes it on success or failure.
- UI and GU calls remain on the main thread. Redraws are limited to 10 Hz except
  phase transitions and the final read. There is no background drawing thread,
  new ROM allocation, speculative read, or change to the buffer-swap algorithm.
  Activity updates occur between operations; a single slow read can still pause
  the indicator until that read returns.

The ROM cache size, read request sizes, mapping, small-ROM mirror/fallback,
backup detection and SRAM handling remain unchanged. Progress totals describe
the bytes initially loaded into the available cache; larger ROMs retain their
existing on-demand paging behavior.

## Validation

`tools/run_rom_load_tests.py` runs production config/INI code and compares the
production loader with checkpoint `ecdd8bc`:

- Observed baseline: 36 read opens and 36 write opens per selection. Updated:
  one of each for a different ROM, zero for a repeat.
- Settings/comments/unknown keys survive byte-for-byte; reload, invalid names
  and failed persistence are covered.
- Thirty before/after comparisons cover 32 KiB through 32 MiB ROMs, 1/6/32 MiB
  caches, partial final chunks, and 1 MiB mirror allocation success/fallback.
  Loaded-byte fingerprints, mapping records and read counts match. Address and
  undefined-behavior sanitizers pass. Progress is monotonic and reaches its
  expected byte count; empty/oversized files retain rejection.
- FF configuration/scheduling, Mystery Gift parcel/cart and display-buffer
  regression suites pass. A clean pinned-toolchain PSP build passes; the ME
  binary is identical to Gift Polish.
- The built EBOOT boots Heart & Soul in PPSSPP. Captured VRAM buffers show the
  browser, loading stages, advancing byte progress and the game's intro.
  PPSSPP cannot establish real PSP storage speed or ME/cache behavior; its ME
  initialization produces known unsupported-hardware errors and falls back.

## Hardware test build

**GBAdhoc Quick Load**, revision `ecdd8bc-romload1`, is a separate test install.
It includes `GPSP_ROMLOAD_DIAGNOSTICS`: one small `ROMLOAD.TXT` report per boot,
written after timing finishes, with startup-stage durations in microseconds.
Production builds without that flag do not write the report. Phase times include
their UI presentation cost; the total ends after renderer startup and excludes
writing the report and time to the first guest frame.

Compare the same ROM with Gift Polish on the same PSP/card. Select a different
ROM once, then reopen it to exercise the zero-write path. Check the loading
indicator, eventual game start, existing saves, fast-forward and Mystery Gift.
If loading remains slow, return the PSP to USB mode: ROMLOAD.TXT separates the
settings step from ROM reads, cartridge setup and renderer startup. Hardware
speedup and final stability acceptance remain pending that test.

Source manifest, build/test logs, captured buffers and deployment verification:
`builds/rom-load1/` in the containing workspace. Public release work remains
deferred until hardware validation.
