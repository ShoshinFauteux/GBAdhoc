/* hot_data_guard.ld -- HOT_DATA_GUARD's placement (mips/mips_stub.S), added to
 * psp-gcc's default script with INSERT (psp/Makefile, only when the ROOT make
 * has HOT_DATA_GUARD=1).
 *
 * WHY A SCRIPT.  The first guard asked for the placement with `.balign 8192`
 * on .data.gbahot.a.  An 8 KiB input alignment raises the alignment of the
 * whole read-write segment, and because this EBOOT's file offsets are
 * congruent to its addresses only mod 0x100, ld had to split that segment:
 * .ctors/.dtors stayed in a 16-byte PT_LOAD of their own and .data moved to
 * an 8 KiB-aligned one -- 4 loadable segments and 5 program headers.  PPSSPP
 * loads that; a real PSP never started it (2026-10-27: every guard build
 * hung black at the hop, the header-merged probe lab-xvg-4ph is the A/B).
 * tools/build.sh now refuses such an EBOOT.
 *
 * HERE the 8 KiB boundaries are location-counter assignments, which pad
 * without raising any section's alignment, so the read-write segment stays
 * one PT_LOAD:
 *   .data.pin     8 KiB-aligns the start of .data, so everything in .data
 *                 keeps its D-cache sets when text or rodata grows;
 *   .data.gbahot  after .data: an 8 KiB boundary, then the stub's pad (.a,
 *                 HOT_DATA_OFFSET bytes), the hot block (.b) and the
 *                 compensating pad (.c), so .sdata/.sbss/.bss keep their
 *                 sets whatever HOT_DATA_OFFSET is.
 * GBA_LINK's instance B carries its own .data.gbahot.b (its .a/.c are
 * discarded by gbalink_core_b.ld); it stays in .data where it always was. */
SECTIONS
{
  .data.pin :
  {
    gbahot_data_pin = .;
    . = ALIGN(8192);
  }
}
INSERT BEFORE .data;

SECTIONS
{
  .data.gbahot :
  {
    . = ALIGN(8192);
    gbahot_start = .;
    *(.data.gbahot.a)
    EXCLUDE_FILE(*gbalink_core_b.o) *(.data.gbahot.b)
    *(.data.gbahot.c)
    gbahot_end = .;
  }
}
INSERT AFTER .data;
