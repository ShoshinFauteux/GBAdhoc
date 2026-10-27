@ Build: arm-none-eabi-as -mcpu=arm7tdmi spin_inskip.s -o s.o && arm-none-eabi-objcopy -O binary s.o spin_inskip.gba
@ SPIN_SKIP negative case (twin only): a forward branch INSIDE the loop body.
@ The poll always takes `beq 1f`, skipping the `mov r4, #0` that the
@ fall-through path relies on, so on the path actually run r4 counts up and
@ the loop leaves after 3000 iterations: not a spin loop.  spin_analyze
@ rejects a branch whose target lies inside the loop (loops=0 here); the
@ SPIN_SKIP_MUTANT_INSKIP build accepts it as an exit and SPIN_SKIP_VERIFY
@ must then report violations (regs differ at a repeat / left the loop).
    .arm
    .section .text
    .global _start
    .equ ZERO,  0x03000100      @ always 0
    .equ CNT,   0x03000104
_start:
    b       init
    .fill   0xBC, 1, 0
init:
    ldr     sp, =0x03007F00
    adr     r0, tcode + 1
    bx      r0
    .thumb
    .align 2
tcode:
    ldr     r0, =ZERO
    ldr     r5, =3000
    mov     r1, #0
    str     r1, [r0]
again:
    mov     r4, #0
loop:
    ldr     r1, [r0]
    cmp     r1, #0
    beq     1f                  @ always taken: skips the write of r4
    mov     r4, #0
1:  add     r4, #1
    cmp     r4, r5
    bne     loop
    ldr     r2, =CNT
    ldr     r3, [r2]
    add     r3, #1
    str     r3, [r2]
    b       again
    .pool
    .fill   0x1000, 1, 0
