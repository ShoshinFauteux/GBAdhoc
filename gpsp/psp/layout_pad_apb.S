/* layout_pad_apb.S -- the partner autopilot's place in the pinned warm tail.
 *
 * psp/layout/hot.ord links `.text.fe_autopilot_{take_mark,status,frame}`
 * from every object except instance B's core, which in a rig build means
 * fe_autopilot.o AND its partner copy fe_autopilot_b.o, back to back.  A
 * player build no longer links fe_autopilot_b.o (psp/Makefile), so without
 * this the ~100 warm functions after fe_autopilot_frame would move 2,636 B
 * and change I-cache sets.  These sections have the same names and sizes as
 * the partner copy's (4.6.0-beta.4 release ELF: 0x10, 0xc, 0xa34), so the
 * same hot.ord patterns place them where it was.  Nothing calls them; they
 * are zeros.  Regenerating the layout (tools/layout/regen.sh) without the
 * partner copy retires this file.  Only linked with LAYOUT_PIN=1. */
	.section .text.fe_autopilot_take_mark,"ax",@progbits
	.balign 4
layout_pad_apb_take_mark:
	.space 0x10
	.section .text.fe_autopilot_status,"ax",@progbits
	.balign 4
layout_pad_apb_status:
	.space 0xc
	.section .text.fe_autopilot_frame,"ax",@progbits
	.balign 4
layout_pad_apb_frame:
	.space 0xa34
