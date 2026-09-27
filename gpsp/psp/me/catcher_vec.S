# catcher_vec.S -- ME_CATCH=1 only.  The default exception handler registered
# by catcher.c.  Entry protocol copied from the PSPSDK's own
# _pspDebugExceptionHandler (libpspdebug exception_asm.o, disassembled):
#   * the handler must begin with NOPs;
#   * the original $v0 / $v1 are in COP0 control registers $4 / $5;
#   * $at, $a0-$ra are still the faulting thread's.
# Unlike the SDK we never eret: we stay in exception context (kernel mode,
# interrupts off), take a private stack and call catch_main(), which draws the
# register dump into VRAM and spins.
	.set	noreorder
	.set	noat

	.text
	.global	me_catch_vector
	.ent	me_catch_vector
me_catch_vector:
	nop
	nop
	lui	$v0, %hi(g_catch_regs)
	addiu	$v0, $v0, %lo(g_catch_regs)
	sw	$0,   0($v0)
	sw	$1,   4($v0)
	cfc0	$1, $4
	sw	$1,   8($v0)
	cfc0	$1, $5
	sw	$1,  12($v0)
	sw	$4,  16($v0)
	sw	$5,  20($v0)
	sw	$6,  24($v0)
	sw	$7,  28($v0)
	sw	$8,  32($v0)
	sw	$9,  36($v0)
	sw	$10, 40($v0)
	sw	$11, 44($v0)
	sw	$12, 48($v0)
	sw	$13, 52($v0)
	sw	$14, 56($v0)
	sw	$15, 60($v0)
	sw	$16, 64($v0)
	sw	$17, 68($v0)
	sw	$18, 72($v0)
	sw	$19, 76($v0)
	sw	$20, 80($v0)
	sw	$21, 84($v0)
	sw	$22, 88($v0)
	sw	$23, 92($v0)
	sw	$24, 96($v0)
	sw	$25, 100($v0)
	sw	$26, 104($v0)
	sw	$27, 108($v0)
	sw	$28, 112($v0)
	sw	$29, 116($v0)
	sw	$30, 120($v0)
	sw	$31, 124($v0)
	mfc0	$1, $8
	sw	$1, 128($v0)
	mfc0	$1, $13
	sw	$1, 132($v0)
	mfc0	$1, $14
	sw	$1, 136($v0)
	mfc0	$1, $12
	sw	$1, 140($v0)
	lui	$sp, %hi(g_catch_stack + 4096 - 16)
	addiu	$sp, $sp, %lo(g_catch_stack + 4096 - 16)
	jal	catch_main
	nop
1:	b	1b
	nop
	.end	me_catch_vector
