/* gbalink_core_b.ld -- the partial link of GBA_LINK instance B
 * (docs/GBA-LINK-MIRROR.md section 19).  Only .data and .bss are placed;
 * every other input section keeps its own name (ld -r orphans), exactly as
 * without a script.  The markers are renamed gbab_* with every other global.
 *
 *   bdata_start .. bdata_end        B's initialised data (relocated pointers
 *                                   included): copied once at boot into
 *                                   bdata_pristine, restored at every session
 *   bregion_start .. bregion_lend   B's zero-initialised state and the
 *                                   bus queues (bregion_bus), rounded up
 *                                   to a whole 32 KiB ROM page: LENT to
 *                                   instance A's ROM cache while no session
 *                                   runs (its whole MiB as blocks, the rest
 *                                   as the cache's last, partial block),
 *                                   zeroed at session start
 *   bdata_pristine                  the pristine .data copy (never lent)
 * (Instance A's state waits across a reload in A's own static code array
 * or the heap: fe_host.c gba_scratch_get.)
 */
SECTIONS
{
  /* HOT_DATA_GUARD (mips/mips_stub.S): instance A's hot-block padding.  B
   * keeps its block where it always was.  Matches nothing in other builds. */
  /DISCARD/ : { *(.data.gbahot.a) *(.data.gbahot.c) }
  .data 0 : {
    bdata_start = .;
    *(.data .data.*)
    bdata_end = .;
  }
  .bss 0 : {
    bregion_start = .;
    *(.bss .bss.*)
    *(COMMON)
    /* The two cores' bus queues (gbalink/gbalink_bus.c, which checks the size
     * at every session): nothing but a session uses them, so they are lent
     * with B instead of sitting in the image. */
    . = ALIGN(16);
    bregion_bus = .;
    . += 0x10010;
    bregion_bus_end = .;
    /* NOT whole MiB.  Padding to 1 MiB (as until cand2) put up to 1 MiB of
     * idle bytes in the image, i.e. off every console's heap, used by
     * nothing while a session runs: cand2's B state, 4,453,156 B, padded to
     * 5 MiB = 789,724 B; PSP-1000 boot heap 8.47 MB.  The ROM cache takes
     * the remainder as pages (gba_memory.c gamepak_buffer_pages).  32 KiB =
     * one ROM page, and a multiple of the 8 KiB cache way, so everything
     * linked after B keeps its cache sets. */
    . = ALIGN(0x8000);
    bregion_lend = .;
    bdata_pristine = .;
    . += SIZEOF(.data);
    . = ALIGN(64);
  }
}
