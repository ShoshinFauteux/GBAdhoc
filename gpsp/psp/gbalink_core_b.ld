/* gbalink_core_b.ld -- the partial link of GBA_LINK instance B
 * (docs/GBA-LINK-MIRROR.md section 19).  Only .data and .bss are placed;
 * every other input section keeps its own name (ld -r orphans), exactly as
 * without a script.  The markers are renamed gbab_* with every other global.
 *
 *   bdata_start .. bdata_end        B's initialised data (relocated pointers
 *                                   included): copied once at boot into
 *                                   bdata_pristine, restored at every session
 *   bregion_start .. bregion_lend   B's zero-initialised state, padded to whole
 *                                   1 MiB blocks: LENT to instance A's ROM
 *                                   cache while no session runs, zeroed at
 *                                   session start
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
    . = ALIGN(0x100000);
    bregion_lend = .;
    bdata_pristine = .;
    . += SIZEOF(.data);
    . = ALIGN(64);
  }
}
