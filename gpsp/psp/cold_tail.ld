/* cold_tail.ld -- the cold tail, added to psp-gcc's default script with
 * INSERT (psp/Makefile, every build).
 *
 * Code in .text.me_wmap_cold (gba_memory.h ME_WMAP_COLD) is linked after all
 * other text, in an output section of its own, so adding or growing it moves
 * no pinned function (docs/LAYOUT-PINNING.md): only what follows .text, which
 * `pin ro` places.  It holds the ME write maps' DMA note and row diff,
 * which run only for a game that writes the palette or OAM by DMA or while
 * lines draw.  GBA_LINK's instance B keeps its copies in its own text (the
 * CORE pin, in front of instance A's cold text, absorbs those bytes):
 * here they would push .rodata, and so .data, past its 8 KiB page. */
SECTIONS
{
  .text.cold_tail :
  {
    EXCLUDE_FILE(*gbalink_core_b.o) *(.text.me_wmap_cold)
  }
}
INSERT AFTER .text;
