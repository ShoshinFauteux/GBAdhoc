@ Build: arm-none-eabi-as -mcpu=arm7tdmi xs_norec.s -o n.o && arm-none-eabi-objcopy -O binary n.o xs_norec.gba
@ Run on the twin built with the C3 flags + DISPATCH_PRECISE SMC_SAME_SKIP SMC_SAME_ADAPT
@ EXEC_STALE_CHECK XS_WINDOW=2048u, 20 frames: expect EXEC_STALE hits=0, off_events=1
@ on_events=1.  Adding SMC_SAME_ADAPT_MUTANT_NOREC must give hits=1 at 03000104.
@ SMC_SAME_ADAPT record-bit test (twin only).  Block X is translated while
@ recording is OFF, from a word that an untrapped STM non-final store changed;
@ another live block Y holds the record of the old value.  After recording is
@ back ON and the word is restored (again untrapped), a same-value trap inside
@ X must NOT be skipped: X's translation (b T2) no longer matches memory (b T1).
    .arm
    .section .text
    .global _start
    .equ IW,   0x03000000
    .equ YA,   IW + 0xFC        @ Y: mov r3,#0 ; falls into W, K
    .equ WA,   IW + 0x100       @ X start, w: mov r2,#0
    .equ KA,   IW + 0x104       @ k: b T1 / b T2 (last word of X and Y)
    .equ T1A,  IW + 0x200
    .equ T2A,  IW + 0x300
    .equ FA,   IW + 0x400       @ churn function: mov r0,#n ; bx lr
    .equ BX_LR, 0xE12FFF1E
    .equ V1,   0xEA000000 | ((T1A - (KA + 8)) >> 2)
    .equ V2,   0xEA000000 | ((T2A - (KA + 8)) >> 2)
_start:
    b       init
    .fill   0xBC, 1, 0
init:
    ldr     sp, =0x03007F00
    ldr     r0, =YA
    ldr     r1, =0xE3A03000
    str     r1, [r0]
    ldr     r1, =0xE3A02000
    str     r1, [r0, #4]
    ldr     r1, =V1
    str     r1, [r0, #8]
    ldr     r0, =T1A
    ldr     r1, =0xE3A01001
    ldr     r2, =BX_LR
    stmia   r0, {r1, r2}
    ldr     r0, =T2A
    ldr     r1, =0xE3A01002
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
    @ Y translated while recording is ON: records k = V1
    ldr     r4, =YA
    mov     lr, pc
    bx      r4
    str     r1, [r10], #4
    @ churn until recording turns OFF (no skips in the window)
    ldr     r6, =600
    bl      churn
    @ untrapped change of k (non-final STM word; 0x108 is not code)
    ldr     r0, =KA
    ldr     r1, =V2
    ldr     r2, =0x12345678
    stmia   r0, {r1, r2}
    @ X translated while OFF: reads k = V2, unrecorded
    ldr     r4, =WA
    mov     lr, pc
    bx      r4
    str     r1, [r10], #4       @ expect 2
    @ churn until recording re-probes (256 IWRAM retires while OFF)
    ldr     r6, =400
    bl      churn
    @ restore k = V1, again untrapped
    ldr     r0, =KA
    ldr     r1, =V1
    ldr     r2, =0x12345678
    stmia   r0, {r1, r2}
    @ same-value single store to w, inside X: must retire X
    ldr     r0, =WA
    ldr     r1, =0xE3A02000
    str     r1, [r0]
    ldr     r4, =WA
    mov     lr, pc
    bx      r4
    str     r1, [r10], #4       @ expect 1 (2 = stale translation ran)
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
