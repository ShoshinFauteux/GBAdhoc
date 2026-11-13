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

/* .text.translate_block_arm (PCARG_SIDE, claude/pcarg-side; docs/PCARG-SIDE.md):
 * the per-site emitters went out of line, so A's translators shrank
 * (release: translate_block_thumb -2,304 B, translate_block_arm -28,096 B
 * 32 B-rounded) and B's hot path, linked right after A's translate_block_arm,
 * moved -30,400 B.  LAYOUT_PAD_XLAT_BYTES (psp/Makefile 5,824; the lab build's
 * translator is 64 B smaller again: tools/layout/lab-pads.env 5,888) in front
 * of A's translate_block_arm makes that -24,576 (three ways): B's hot path and
 * everything after it keep their sets.  B's own shrink is held the same way
 * from psp/gbalink_b_stubs.c, after B's copy.
 * Since the site emitters log their jal for SMC_REUSE (docs/PCARG-SIDE.md
 * section 9) the translators before B's hot path are 320 B larger in the
 * release (translate_block_thumb +352, translate_block_arm -32) and 384 B in
 * the lab build (+352, +32): the pad is 5,504 in both. */
	.section .text.translate_block_arm,"ax",@progbits
	.balign 4
layout_pad_warm_xlat:
	.space LAYOUT_PAD_XLAT_BYTES
