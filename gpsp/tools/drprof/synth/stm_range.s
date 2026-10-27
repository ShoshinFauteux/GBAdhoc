@ Build: arm-none-eabi-as -mcpu=arm7tdmi stm_range.s -o s.o && arm-none-eabi-objcopy -O binary s.o stm_range.gba
@ Run on the twin built with the C3 flags + DISPATCH_PRECISE SMC_SAME_SKIP SMC_SAME_ADAPT
@ SMC_STM_RANGE EXEC_STALE_CHECK, 20 frames: expect EXEC_STALE hits=0.
@ Adding SMC_STM_RANGE_MUTANT_LASTWORD must give a stale hit at 03000104.
@ SMC_STM_RANGE span test (twin only).  An IWRAM STMIA r12!,{r1,r2} writes
@ two words that belong to DIFFERENT live blocks: its first word is the last
@ word of block A (b TA1 -> b TA2), its final (trapping) word is the first
@ word of block B.  Retiring only the final word's blocks leaves A's stale
@ translation (b TA1) live.
    .arm
    .section .text
    .global _start
    .equ IW,   0x03000000
    .equ AA,   IW + 0x100       @ A: mov r0,#0 ; b TA1
    .equ AK,   IW + 0x104
    .equ BA,   IW + 0x108       @ B: mov r0,#7 ; bx lr
    .equ TA1,  IW + 0x200       @ mov r0,#1 ; bx lr
    .equ TA2,  IW + 0x300       @ mov r0,#2 ; bx lr
    .equ WR,   IW + 0x400       @ writer: stmia r12!,{r1,r2} ; bx lr
    .equ FA,   IW + 0x600       @ churn function: mov r0,#n ; bx lr
    .equ BX_LR, 0xE12FFF1E
    .equ VB1,  0xEA000000 | ((TA1 - (AK + 8)) >> 2)
    .equ VB2,  0xEA000000 | ((TA2 - (AK + 8)) >> 2)
_start:
    b       init
    .fill   0xBC, 1, 0
init:
    ldr     sp, =0x03007F00
    ldr     r2, =BX_LR
    ldr     r0, =AA
    ldr     r1, =0xE3A00000
    str     r1, [r0]
    ldr     r1, =VB1
    str     r1, [r0, #4]
    ldr     r1, =0xE3A00007
    str     r1, [r0, #8]
    str     r2, [r0, #12]
    ldr     r0, =TA1
    ldr     r1, =0xE3A00001
    stmia   r0, {r1, r2}
    ldr     r0, =TA2
    ldr     r1, =0xE3A00002
    stmia   r0, {r1, r2}
    ldr     r0, =WR
    ldr     r1, =0xE8AC0006     @ stmia r12!, {r1, r2}
    stmia   r0, {r1, r2}
    ldr     r0, =FA
    ldr     r1, =0xE3A00000
    stmia   r0, {r1, r2}
    ldr     r10, =0x02000000    @ results in EWRAM
    @ activate selective retirement: translate F, then a single store to it
    bl      callF
    ldr     r0, =FA
    ldr     r1, =0xE3A000FF
    str     r1, [r0]
    bl      callF
    @ the first SMC_SINGLE_HOT (64) widened traps (ROM writer) full-flush
    ldr     r6, =100
    bl      churn
    @ translate A and B
    ldr     r4, =AA
    mov     lr, pc
    bx      r4
    str     r0, [r10], #4       @ expect 1
    ldr     r4, =BA
    mov     lr, pc
    bx      r4
    str     r0, [r10], #4       @ expect 7
    @ IWRAM writer: A's last word := b TA2, B's first word := mov r0,#8
    ldr     r12, =AK
    ldr     r1, =VB2
    ldr     r2, =0xE3A00008
    ldr     r4, =WR
    mov     lr, pc
    bx      r4
    ldr     r4, =AA
    mov     lr, pc
    bx      r4
    str     r0, [r10], #4       @ expect 2 (1 = stale A ran)
    ldr     r4, =BA
    mov     lr, pc
    bx      r4
    str     r0, [r10], #4       @ expect 8
done:
    b       done

callF:
    ldr     r4, =FA
    bx      r4                  @ F returns to our caller via lr

churn:                          @ r6 iterations: change F's immediate, call F
    mov     r9, lr
    mov     r5, #0
1:
    and     r1, r5, #0x7F
    orr     r1, r1, #0xE3000000
    orr     r1, r1, #0x00A00000
    ldr     r0, =FA
    str     r1, [r0]
    bl      callF
    add     r5, r5, #1
    cmp     r5, r6
    blo     1b
    bx      r9
    .pool
    .fill   0x1000, 1, 0
