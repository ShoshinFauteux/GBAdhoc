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
`README.md` and this file are new; `cheat.c` is unchanged and not built.

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
- **`gb_run` frame boundary.** Unchanged in the core; the adapter ends a
  frame at VBlank (LY 144) rather than at LY 0, so a presented frame is
  lines 0-143 of one frame.

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
