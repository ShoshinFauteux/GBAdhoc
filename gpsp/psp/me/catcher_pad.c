/* catcher_pad.c -- hard-freeze A/B only (ME_PAD_RODATA / ME_PAD_BSS).
 *
 * Linked LAST, so every existing symbol keeps its address and only what
 * follows moves:
 *   ME_PAD_RODATA=N  N bytes appended to .rodata -> the whole .bss (all of the
 *                    Media Engine's working data) starts N bytes later;
 *   ME_PAD_BSS=N     N bytes appended to the END of .bss -> nothing the ME uses
 *                    moves, only _end (the module's last byte) does.
 * Used to reproduce the catch PRX's data layout without its code, or its code
 * without its layout.  See docs/BUILD-SWITCHES.md. */
#ifdef ME_PAD_RODATA
__attribute__((used)) const unsigned char me_layout_pad_ro[ME_PAD_RODATA] = { 1 };
#endif
#ifdef ME_PAD_BSS
__attribute__((used)) unsigned char me_layout_pad_bss[ME_PAD_BSS];
#endif
