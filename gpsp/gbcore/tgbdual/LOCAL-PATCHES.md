# Local changes to the vendored TGB Dual core

The files in this directory are MasterBoy 2.02's `gbcore/`
(PSP-Archive/MasterBoy commit `320bb5e1388df27f65f1044e366d77e7bd3cad4f`)
**plus the changes below**. The pristine import is its own commit
("gbcore: vendor the TGB Dual core from MasterBoy 2.02 (pristine)", 8f84303),
so the whole local diff is one command away:

```
git diff 8f84303 -- gbcore/tgbdual
```

Every changed line in the sources carries a `GBAdhoc:` comment. `tgb_port.h`,
`tgb_shared.c`, `README.md` and this file are new; `cheat.c` is unchanged and
not built.

## Import normalisation (no code change)

CRLF line endings became LF (`.gitattributes` requires it for `*.c`/`*.h`);
trailing whitespace, trailing blank lines and space-before-tab indentation
were removed so `tools/lint.sh` accepts the files. Comment bytes stay in
their original encodings (Shift-JIS, some CP1252). `diff -w -B
--strip-trailing-cr` of the pristine commit against upstream is empty.

## Removing MasterBoy (the frontend around the core)

| File | Change |
|---|---|
| `gb.h`, `gb.c`, `cpu.c`, `lcd.c`, `apu.c` | `../psp/pspcommon.h`, `../renderer.h`, `../syscall.h`, `../lib/zlib.h` and `<oslib/oslib.h>` replaced by `tgb_port.h` (via `gb.h`). The port header supplies `MAKE_COLOR`, `pad_state`, the DMG palette and the adapter callbacks (`gbe_*`, `set_gb_type`, `tgb_port_serial`), all implemented in `../gbcore_tgbdual.c`. |
| `gb.h` | `CHEAT_SUPPORT` only with `TGB_CHEATS` (off): it put a 64 KiB `cheat_map` lookup on every emulated write. `inline` dropped from ten prototypes (the definitions keep it; an inline declaration warns in every other file that includes the header). Declarations for the buffer-based state functions; externs of removed arrays deleted. |
| `gb.c` | `gb_save_state`/`gb_restore_state` work on memory only (MasterBoy's `VIRTUAL_FILE` path removed); `gb_save_state(NULL)` measures, `gb_restore_state` is bounds-checked. `vframe_mem` (128 KiB .bss) removed: the adapter allocates `vframe` so GBA sessions do not pay for it. `exiting_lcdc()` (ColorIt palette-file lookup) removed. |
| `cpu.c` | `gblCpuCycles` (MasterBoy's "Z80 clock" percentage) removed. GBC infrared bridge to the PSP's `irda0:` removed (`cpu_io.h`): the port reads "no light". |
| `lcd.c` | MasterBoy's ColorIt colourisation (tile palettes, VRAM CRC, `*_colored` renderers) compiled only with `TGB_COLORIT` (off). The DMG palette comes from `tgb_dmg_palette[]` (BGR555, set by the adapter) instead of OSLib colours from palette files. The SGB path's `oslGetCachedPtr`/`oslUncacheData` pair removed (the frame is ordinary cached memory). |
| `apu.c` | Output rate `snd_rate` (set by the adapter) replaces the literal 44100 in the four generators. `snd_render_orig`'s menu-volume scaling replaced by a fixed gain `TGB_SND_GAIN` with int16 saturation (see *Volume* below). The register-write queue is `SND_QUE_SIZE` (16384) entries allocated by the adapter instead of a 768 KiB .bss array. |
| `rom.c` | `sram_space` (128 KiB .bss) removed; the adapter passes its own cartridge RAM. |
| `sgb.c` | The SGB border image (112 KiB) and its renderer compiled only with `TGB_SGB_BORDER` (off): borders are not presented. Border packets are still parsed, since palettes ride along with them. |

## Portability (host tests and modern GCC)

| File | Change |
|---|---|
| `gb_types.h` | `dword` is `uint32_t`, not `unsigned long` (64-bit on LP64 hosts; the sound phase accumulators rely on 32-bit wrap). |
| `cpu.c` | `GenZF` keeps its MIPS `sltiu/sll` under `__psp__`, with a C fallback elsewhere. |
| `lcd.c` | `horizflip` keeps Allegrex `bitrev`/`wsbw` under `__psp__`, with a C fallback. The PSP asm no longer writes its input operand (`bitrev %1,%1`), which GCC's asm contract forbids; it reads `src` and writes only the output. |
| `cpu_io.h` | `_memcpy4x`/`_memcpy40` copy `dword`s, not `unsigned long`s (on LP64 every OAM DMA copied twice the span). |

## Bugs fixed (present in upstream TGB Dual / MasterBoy)

| File | Bug | Fix |
|---|---|---|
| `cpu.c` | Reads and writes of 0xFEA0-0xFEFF indexed `spare_oam[]` with `adr-0xFFA0`: a negative subscript that wrote before the array. Found by `-Warray-bounds`. | `adr-0xFEA0`. |
| `lcd.c` | `gbc_recreate_colors`, `gb_recreate_colors` and `gb_invalidate_all_colors` walked 2-D arrays as flat ones (`inval_col_pal[0][i]`, `i` up to 63; `m_pal16[0][0][i]`, `i` up to 11). That is undefined, and GCC warns (`-Waggressive-loop-optimizations`) that it may cut the loops to four iterations -- which would leave most CGB palettes and both DMG sprite palettes unconverted. | Flat pointers; `gbc_invalidate_color(i>>2, i&3)`. |
| `sgb.c` | `sgb_reset` cleared 2048 bytes of the 4096-byte `sgb_palette_memory`. | `sizeof`. |
| `rom.c` | A RAM size code above 5 indexed past `tbl_ram`/`ram_size_tbl`. | Treated as 0. |
| `rom.c` | Bank masks come from the header size code, so a file larger than its header claims lost its upper banks. | The code grows to cover the file (the adapter pads the buffer to match). |
| `apu.c` | `freq<<16` overflowed `int` for square frequencies above 32767 Hz. | Unsigned shift. |
| `op_def.h` | Header guard `OP_DEF_` defined `OP_DEF`. | Defines `OP_DEF_`. |
| `apu.c` | The wave channel's `sample * volume << 1` shifts a negative value (undefined; UBSan reports it on every real cartridge). | `* 2`: the same value on every compiler we use, and the single-player trace is bit-identical to 3.0.0 (`gbcore/tests/bitident.c`). |
| `gb.c` | HDMA from WRAM formed `ram-0xC000` from the array itself (`-Warray-bounds`). | Same pointer via `cpu_get_ram()`. |

## Behaviour changes

- **Serial port (the link cable).** MasterBoy had compiled out serial
  completion, so an internal-clock transfer never finished. `cpu_io_write_02`
  now offers SB to the adapter (`tgb_port_serial`) at every transfer start;
  GBAdhoc exchanges one whole byte with the peer over ad-hoc WiFi
  (`netdrv/gb_link.c`, `frontend-common/fe_host.c`). Internal clock: the
  received byte (0xFF with no cable) lands in SB with the serial interrupt
  after eight bit times (`seri_occer`, checked in `cpu_exec`). External
  clock: with a peer the byte lands at once; without one nothing happens, as
  on hardware. The DMG transfer time is corrected from 512 to 4096 clocks.
  New global `seri_rx`.
- **Game Boy Color colours.** `gbc_recreate_colors` (`lcd.c`) converts a
  CGB palette entry through the GBC LCD model (`cgb_lcd_color`: SameBoy's
  measured response curve and green/blue mix, tables generated into
  `_cgb_lcd_table.h` by `tools/gen_cgb_lcd_table.py`) instead of the plain
  5-to-8-bit expansion, unless the adapter's `gbcore_set_color_correction(0)`
  turns it off. The tables and the switch (`tgbshared_cgb_lcd`) are defined
  once in `tgb_shared.c`. DMG and SGB colours are unchanged. Why:
  `docs/GB-PALETTE-FIXES.md`.
- **`gb_run` frame boundary.** Unchanged in the core; the adapter ends a
  frame at VBlank (LY 144) rather than at LY 0, so a presented frame is
  lines 0-143 of one frame.

## Two machines in one program (GB link sessions)

A link session runs both players' Game Boys on each console
(`../gbcore_dual.h`). The core stays a set of globals: the build links the
adapter and core as one partial link through `../gbcore_instance.ld`, keeps
only the `gbcore_*` API global, and links a second copy with those names
renamed `gbcoreb_*` (`psp/Makefile`, `tools/run_gb_tests.py`). Changes for
that:

| File | Change |
|---|---|
| `cpu.c` | Serial completion (`seri_occer` expiring in `cpu_exec`) asks the adapter for the byte that lands in SB (`tgb_port_serial_complete`). With an in-memory cable the byte is exchanged THEN, with the peer machine's SB (`cpu_seri_send`), which is what upstream TGB Dual did between its two machines; otherwise the adapter returns `seri_rx`, the byte chosen at the SC write, exactly as before. |
| `gb.c` | `gb_fill_vframe` returns when there is no frame buffer: a headless machine (the partner's Game Boy) has none. It is reached from `gb_reset` and from SGB `MASK_EN` packets whatever `gbSkip` says. |
| `_mrand_table.h` | The noise tables are `const` and named `tgbshared_mrand7/15`, defined once by the new `tgb_shared.c` outside the per-instance link: 64 KiB the second machine does not duplicate. |
| `tgb_port.h` | Prototypes for `tgb_port_serial_complete` and `cpu_seri_send`. |

Headless (`gbcore_set_headless`) needs nothing else in the core: every
`lcd_render` call and the LCD-off fill are already under `gbSkip`, and the
synthesiser (`snd_render_orig`) works on its own copy of the APU state,
swapped in and out, so skipping it leaves every register the game can read
unchanged. `snd_update`'s shared counter (the comment by LCK) would have
coupled the two, but its register-side caller in `apu_write` never runs:
`clocks += clock - bef_clock` adds zero, because `bef_clock` is set from
`clock` on entry. `gbcore/tests/dual.c` checks the outcome on six real
cartridges: a headless machine's sync hash equals the drawn one's every frame.

`gb_reset` does not clear everything a game can read -- the sound registers
NR10-NR51 in `snd_mem`, `_ff6c`/`_ff72`-`_ff75`, `ext_mem` -- nor the
synthesiser's phases, so a second game in the same process started from the
first one's leftovers (measured: the sync hash differs at frame 0).
Single-player is left exactly as it was; a link session instead starts each
machine with `gbcore_power_on`, which restores the instance's whole `.data`
and clears its `.bss` (`../gbcore_instance.ld` marks them).

## Volume

`TGB_SND_GAIN` is 2. A synthetic ROM playing channel 1 (duty 50%, envelope
15, NR50 0x77) peaks at +/-8203; gpSP renders the GBA's identical PSG at
full volume as +/-8192 (`sound.c`: `8 * 2^28 >> 18`), so GB and GBA games
play at the same level. The SameBoy core this replaced peaked at +/-4540
for the same tone.

## Build flags

`-fgnu89-inline` (non-static `inline` definitions whose addresses the
dispatch tables take), `-fno-strict-aliasing` (VRAM/ROM bytes read through
`word`/`dword` pointers), `-O3` as MasterBoy built `gbcore/`
(`Makefile.psp`; about 1-2% faster than -O2 in PPSSPP), and
`-Wno-unused-parameter`: the I/O and opcode dispatch tables give every
handler one signature and 157 handlers ignore their argument. Every other
warning class stays on and the files build clean with `-Wall -Wextra`
(host gcc and psp-gcc 15.2).

On the PSP the core and its adapter are linked into one relocatable object
(`psp-ld -r`) whose only global symbols are `gbcore_*` (`psp-objcopy
--keep-global-symbol`): the core exports ~780 generic names (`vram`, `ram`,
`oam`, `halt`, `info`...) and `vram` collided with the GBA core's
`mips_stub.o` on the first link.
