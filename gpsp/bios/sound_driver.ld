/* Links bios/source/sound_driver.c as one position-dependent blob at
 * SD_BASE inside the 16 KiB BIOS image (bios/splice_sound_driver.sh).
 * Everything it has is code or constant data: a .data or .bss section would
 * be RAM the BIOS does not own, so the splice refuses one. */
SECTIONS
{
  . = 0x2800;
  .text : { *(.text*) *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) *(.ARM.attributes) }
}
