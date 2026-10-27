@ Build: arm-none-eabi-as -mcpu=arm7tdmi xs_pair.s -o s.o && arm-none-eabi-objcopy -O binary s.o xs_pair.gba
@ Run on the twin built with the C3 flags + DISPATCH_PRECISE SMC_SAME_SKIP SMC_SAME_ADAPT
@ XS_WALK_PAIR EXEC_STALE_CHECK, 20 frames: expect EXEC_STALE hits=0.
@ Adding XS_WALK_PAIR_MUTANT must give a stale hit at 03000106.
@ XS_WALK_PAIR test (twin only).  Two live Thumb blocks share one IWRAM word:
@   C at 03000100: mov r0,#1 ; mov r1,#2 ; bx lr        (ends at 03000106)
@   B at 03000106: mov r0,#5 ; bx lr
@ A word store to 03000104 keeps C's `bx lr` and changes B's first halfword
@ (mov r0,#6).  B's start tag is the ODD slot of the pair (03000104,
@ 03000106) whose even slot is C's interior mark, so the mutant walk skips
@ it, sees only C (unchanged), lets SMC_SAME_SKIP skip the retirement and
@ leaves B's stale translation (mov r0,#5) live.
    .arm
    .section .text
    .global _start
    .equ IW,   0x03000000
    .equ CC,   IW + 0x100
    .equ BB,   IW + 0x106
    .equ FA,   IW + 0x600       @ churn function: mov r0,#n ; bx lr (ARM)
    .equ BX_LR, 0xE12FFF1E
_start:
    b       init
    .fill   0xBC, 1, 0
init:
    ldr     sp, =0x03007F00
    @ C and B (Thumb): 2001 2102 4770 | 2005 4770
    ldr     r0, =CC
    ldr     r1, =0x21022001
    str     r1, [r0]
    ldr     r1, =0x20054770     @ halfwords 03000104 = 4770, 03000106 = 2005
    str     r1, [r0, #4]
    ldr     r1, =0x00004770     @ 03000108 = 4770 (B's bx lr)
    strh    r1, [r0, #8]
    ldr     r0, =FA
    ldr     r1, =0xE3A00000
    ldr     r2, =BX_LR
    stmia   r0, {r1, r2}
    ldr     r10, =0x02000000    @ results in EWRAM
    @ activate selective retirement: translate F, then a single store to it
    bl      callF
    ldr     r0, =FA
    ldr     r1, =0xE3A000FF
    str     r1, [r0]
    bl      callF
    @ the first SMC_SINGLE_HOT (64) widened traps full-flush
    ldr     r6, =100
    bl      churn
    @ translate C and B
    ldr     r4, =CC + 1
    mov     lr, pc
    bx      r4
    str     r0, [r10], #4       @ expect 1
    ldr     r4, =BB + 1
    mov     lr, pc
    bx      r4
    str     r0, [r10], #4       @ expect 5
    @ the word store: C's bx lr kept, B's first halfword := mov r0,#6
    ldr     r0, =CC + 4
    ldr     r1, =0x20064770
    str     r1, [r0]
    ldr     r4, =CC + 1
    mov     lr, pc
    bx      r4
    str     r0, [r10], #4       @ expect 1
    ldr     r4, =BB + 1
    mov     lr, pc
    bx      r4
    str     r0, [r10], #4       @ expect 6 (5 = stale B ran)
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
