/* layout_pad.S -- static-layout padding for hardware A/B sweeps
 * (docs/JIT-CODE-DISCIPLINE.md, "Hardware validation").  Linked FIRST, and
 * only when psp/Makefile is given LAYOUT_PAD_TEXT and/or LAYOUT_PAD_BSS
 * (bytes), so every later object's .text / .bss moves by exactly that much:
 * the resident-ROM derail comes and goes with layout, so a fix is only
 * demonstrated across several layouts.  Never in a release (tools/build.sh
 * refuses PSP_EXTRA for the release profile). */
#ifndef LAYOUT_PAD_TEXT
#define LAYOUT_PAD_TEXT 0
#endif
#ifndef LAYOUT_PAD_BSS
#define LAYOUT_PAD_BSS 0
#endif
	.set noreorder
	.text
	.globl layout_pad_text
layout_pad_text:
	.space LAYOUT_PAD_TEXT
	.bss
	.globl layout_pad_bss
layout_pad_bss:
	.space LAYOUT_PAD_BSS
