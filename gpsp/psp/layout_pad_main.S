/* layout_pad_main.S -- a cold, never-called pad directly in front of main(),
 * part of the shipped layout like the other LAYOUT_PAD_* pads
 * (docs/LAYOUT-PINNING.md, "Keeping the pads").
 *
 * main() is not in the hot order: it is the first `.text.startup.*` input
 * section, linked right after the ordered sections (the end of instance B's
 * warm tail).  LAYOUT_PAD_BSCAN_BYTES sits inside B's hot text, so it cannot
 * move main without moving B's hot text after B's update_scanline.  This pad
 * is the one degree of freedom between the two: it moves main (and with it
 * the frame loop's window) and everything after, while B's hot text stays.
 * It is a `.text.startup.*` section in an object linked before main_psp.o,
 * so it lands between the ordered text and main.  Zeros; nothing calls it.
 * LAYOUT_PAD_MAIN_BYTES = 0 assembles to an empty section. */
#ifndef LAYOUT_PAD_MAIN_BYTES
#define LAYOUT_PAD_MAIN_BYTES 0
#endif
	.section .text.startup.layout_pad_main,"ax",@progbits
	.balign 4
	.globl layout_pad_main
	.type layout_pad_main, @function
layout_pad_main:
	.space LAYOUT_PAD_MAIN_BYTES
	.size layout_pad_main, . - layout_pad_main
