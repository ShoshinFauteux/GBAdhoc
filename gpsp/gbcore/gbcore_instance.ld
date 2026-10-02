/* Partial-link script for one TGB Dual instance (the adapter plus the core,
 * `ld -r -T gbcore/gbcore_instance.ld`; psp/Makefile, tools/run_gb_tests.py).
 *
 * It gathers the instance's writable data into one .data and one .bss and
 * marks their bounds, so gbcore_power_on() can return every static byte of
 * the instance -- globals, file statics and function statics alike -- to
 * what a freshly loaded program holds.  A link session starts both
 * consoles' machines from that state; without it a console that had
 * already played a GB game would begin with different leftovers than one
 * that had not.
 *
 * The bounds symbols are ordinary symbols of the partial link; the objcopy
 * that follows makes them local like everything but the gbcore_* API.  The
 * adapter's own bookkeeping for the reset (.bss.gbcore_keep) is placed
 * before the marked range so the reset never clears it.  Small-data sections
 * are listed for completeness; the PSP build uses -G0 and has none.
 */
SECTIONS
{
  .data : {
    tgb_image_data_start = .;
    *(.data .data.* .sdata .sdata.*)
    tgb_image_data_end = .;
  }
  .bss : {
    *(.bss.gbcore_keep)
    tgb_image_bss_start = .;
    *(.bss .bss.* .sbss .sbss.* COMMON)
    tgb_image_bss_end = .;
  }
}
