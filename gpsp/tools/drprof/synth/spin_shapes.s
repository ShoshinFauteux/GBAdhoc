@ Build: arm-none-eabi-as -mcpu=arm7tdmi spin_shapes.s -o s.o && arm-none-eabi-objcopy -O binary s.o spin_shapes.gba
@ SPIN_SKIP loop shapes beyond the single-block back-edge (twin only).  Run
@ with --script spin_shapes.txt (A held for 30 frames every 120) for ~900
@ frames on a SPIN_SKIP twin; with SPIN_SKIP_VERIFY expect loops >= 3,
@ claims > 0, violations=0, vetoes=1 (the timer poll below).
@
@ A VBlank IRQ handler sets FLAG.  The main loop waits for it three ways,
@ then once for KEYINPUT, then once on a lazy timer counter:
@   1. threaded back-edge (Thumb): the poll branches FORWARD to an
@      unconditional `b` back to its start -- Final Fantasy Tactics Advance's
@      shape (beq skip / b out / skip: b loop);
@   2. exit in the middle (ARM): a conditional branch out of the loop body,
@      the loop closed by a second test;
@   3. the plain shape, for reference (Thumb ldr/cmp/beq);
@   4. KEYINPUT poll (changes only at the frame boundary in this core);
@   5. TM0CNT_L poll: a lazy counter, must still be vetoed;
@   6. (run before 5) the plain shape with `ldr r0, =FLAG` inside the
@      loop: its address register is rewritten every pass, with a constant.
@ Each wait bumps a counter in IWRAM afterwards (a store OUTSIDE the loop).
    .arm
    .section .text
    .global _start
    .equ FLAG,  0x03000100
    .equ CNT,   0x03000104
    .equ KCNT,  0x03000108
    .equ IRQV,  0x03007FFC
_start:
    b       init
    .fill   0xBC, 1, 0
init:
    ldr     sp, =0x03007F00
    ldr     r0, =IRQV
    adr     r1, irq
    str     r1, [r0]
    ldr     r0, =0x04000004     @ DISPSTAT: VBlank IRQ on
    mov     r1, #8
    strh    r1, [r0]
    ldr     r0, =0x04000200     @ IE = VBlank
    mov     r1, #1
    strh    r1, [r0]
    ldr     r0, =0x04000208     @ IME = 1
    strh    r1, [r0]
    ldr     r0, =0x04000102     @ TM0CNT_H: on, prescale 1
    mov     r1, #0x80
    strh    r1, [r0]
    adr     r0, tmain + 1
    bx      r0

irq:                            @ ARM, called by the BIOS dispatcher
    ldr     r0, =0x04000200
    ldrh    r1, [r0, #2]        @ IF
    strh    r1, [r0, #2]        @ acknowledge
    ldr     r0, =FLAG
    mov     r1, #1
    str     r1, [r0]
    bx      lr

@ 2. exit in the middle (ARM)
amid:
    ldr     r0, =FLAG
    ldr     r3, =0x04000006     @ VCOUNT
    mov     r2, #0
    str     r2, [r0]
amid_loop:
    ldr     r1, [r0]
    tst     r1, #1
    bne     amid_out            @ exit in the middle of the body
    ldrh    r2, [r3]
    cmp     r2, #255            @ VCOUNT never reaches 255
    bne     amid_loop
amid_out:
    adr     r0, tmain_back + 1
    bx      r0
    .ltorg

    .thumb
    .align 2
tmain:
@ 1. threaded back-edge (Thumb)
    ldr     r0, =FLAG
    mov     r1, #0
    str     r1, [r0]
thr_loop:
    ldr     r1, [r0]
    cmp     r1, #0
    beq     thr_skip            @ the loop's real back-edge: forward ...
    b       thr_out
thr_skip:
    b       thr_loop            @ ... to an unconditional branch back
thr_out:
    bl      bump
    ldr     r0, =amid + 0x08000000  @ 2. (ARM; objcopy'd at 0, so rebase)
    bx      r0
tmain_back:
    bl      bump
@ 3. plain shape
    ldr     r0, =FLAG
    mov     r1, #0
    str     r1, [r0]
pl_loop:
    ldr     r1, [r0]
    cmp     r1, #0
    beq     pl_loop
    bl      bump
@ 4. KEYINPUT: wait for A down, then for A up
    ldr     r0, =0x04000130
    mov     r2, #1
k_down:
    ldrh    r1, [r0]
    tst     r1, r2
    bne     k_down
k_up:
    ldrh    r1, [r0]
    tst     r1, r2
    beq     k_up
    ldr     r0, =KCNT
    ldr     r1, [r0]
    add     r1, #1
    str     r1, [r0]
@ 6. the flag's address reloaded from the literal pool inside the loop
    ldr     r0, =FLAG
    mov     r1, #0
    str     r1, [r0]
lit_loop:
    ldr     r0, =FLAG
    ldr     r1, [r0]
    cmp     r1, #0
    beq     lit_loop
    bl      bump
@ 5. lazy timer counter: must be vetoed (runs iteration by iteration)
    ldr     r0, =0x04000100
t_loop:
    ldrh    r1, [r0]
    lsr     r1, r1, #12
    cmp     r1, #7
    bne     t_loop
    b       tmain

bump:
    ldr     r0, =CNT
    ldr     r1, [r0]
    add     r1, #1
    str     r1, [r0]
    bx      lr
    .pool
    .fill   0x1000, 1, 0
