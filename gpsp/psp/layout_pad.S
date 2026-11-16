/* layout_pad.S -- static-layout padding for hardware A/B sweeps
 * (docs/JIT-CODE-DISCIPLINE.md, "Hardware validation").  Linked FIRST, and
 * only when psp/Makefile is given LAYOUT_PAD_BSS (bytes), so every later
 * object's .bss moves by exactly that much: the resident-ROM derail comes and
 * goes with layout, so a fix is only demonstrated across several layouts.
 * The text half of the sweep, LAYOUT_PAD_TEXT, is no longer zeros here: it
 * moves the linker pins (psp/Makefile, tools/layout/pins.awk sweep=N), which
 * would have absorbed zeros linked in front of them (code-health C3-3).
 * Never in a release (tools/build.sh refuses PSP_EXTRA for the release
 * profile). */
#ifndef LAYOUT_PAD_BSS
#define LAYOUT_PAD_BSS 0
#endif
	.bss
	.globl layout_pad_bss
layout_pad_bss:
	.space LAYOUT_PAD_BSS
