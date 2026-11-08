/* layout_pad_late.S -- the pad that places the ME capture's mid-frame-log
 * helpers (docs/LAYOUT-PINNING.md, "The capture line", beta.5 integration).
 *
 * The helpers are hot_order.txt's `# ---- last` rows (ml_diff_page,
 * ml_relevance, ml_line_body, ml_update_relevance): they link after B's late
 * copies, so where they land mod 8 KiB depends on B's hot text and the late
 * rows, not on what they need.  psp/layout/hot.ord places sections by name
 * from every object except instance B's core, and this object is linked
 * before the core archive, so the section below lands just before instance
 * A's ml_diff_page, the first of them: LAYOUT_PAD_LATE_BYTES of zeros put
 * ml_line_body at 0x01c0 mod 8 KiB, the middle of the 384 B plateau the I+D
 * model found (tools/layout/cap_map.py + capsim.sh).  Release and lab need
 * different values (B's hot text differs); the MAIN pad after it takes the
 * bytes back, so the image does not grow.  Ordering the helpers after B's
 * late copies (not before them) keeps this pad small in both flavours: one
 * that placed them before B's copies needed 4,096 B in the lab and cost its
 * image 8 KiB more.  Zeros; nothing calls them.  Only linked with
 * LAYOUT_PIN=1.  LAYOUT_PAD_LATE_BYTES = 0 assembles to an empty section. */
#ifndef LAYOUT_PAD_LATE_BYTES
#define LAYOUT_PAD_LATE_BYTES 0
#endif
	.section .text._ZL12ml_diff_pagemm,"ax",@progbits
	.balign 4
	.globl layout_pad_late
	.type layout_pad_late, @function
layout_pad_late:
	.space LAYOUT_PAD_LATE_BYTES
	.size layout_pad_late, . - layout_pad_late
