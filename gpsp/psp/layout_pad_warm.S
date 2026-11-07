/* layout_pad_warm.S -- a 32 B hole in the pinned warm tail, where a change
 * made a hot.ord function one 32 B step smaller (claude/opt-1000 round 7).
 *
 * psp/layout/hot.ord places sections by name from every object except
 * instance B's core, and this object is linked before the core archive, so
 * the section below lands just before the core's function of that name:
 *
 *   .text.block_lookup_translate_thumb  the lookup's constant hash shift
 *       (one static table on both tiers) made it 1,544 -> 1,536 B, round 3's
 *       size; these 32 B put it back at round 3's address (0x0880bb80) and
 *       everything after it where round 6 had it.  B's copy is held the same
 *       way from psp/gbalink_b_stubs.c.
 *
 * Zeros; nothing calls them.  Regenerating the layout (tools/layout/regen.sh)
 * retires this file.  Only linked with LAYOUT_PIN=1. */
	.section .text.block_lookup_translate_thumb,"ax",@progbits
	.balign 4
layout_pad_warm_lookup:
	.space 32
