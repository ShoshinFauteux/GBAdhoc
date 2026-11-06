.globl _open_gba_bios_rom
.globl open_gba_bios_rom

/* Read-only: libretro.c only memcpys it into bios_rom.  On the PSP both
 * GBA instances read this one copy (psp/Makefile B_SHARED), so a stray
 * write must fault rather than change the partner's BIOS too. */
#if defined(__ELF__)
.section .rodata
#else
.data
#endif
_open_gba_bios_rom:
open_gba_bios_rom:
  .incbin "bios/open_gba_bios.bin"

#if defined(__linux__) && defined(__ELF__)
  .section .note.GNU-stack,"",%progbits
#endif
