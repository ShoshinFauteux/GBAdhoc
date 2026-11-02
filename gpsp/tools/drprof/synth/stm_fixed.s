@ Build: arm-none-eabi-as -mcpu=arm7tdmi stm_fixed.s -o s.o && arm-none-eabi-objcopy -O binary s.o stm_fixed.gba
@   (or: clang --target=armv4t-none-eabi -c stm_fixed.s -o s.o && llvm-objcopy -O binary s.o stm_fixed.gba)
@ Run on the twin (dynarec), 20 frames, DR_RAMDUMP=19, then
@   python3 stm_fixed_check.py ramdump.bin
@ EWRAM 0x02000000.. holds each result word (order below), 0x02000100 the
@ fail count, 0x02000104 0x600D600D (pass) or 0x0BAD0BAD (fail).
@
@ SMC_STM_FIXED span test: SMC_STM_RANGE's exact retirement for an IWRAM
@ STMIA Rn,{..} WITHOUT writeback (ipatix's mixer, The Morrow's Golden
@ Country, pastes its resampling loop with STMIA LR,{R0,R1}).
@
@   cross    STMIA r12,{r1,r2}: its first word is the last word of block A
@            (b TA1 -> b TA2), its final (trapping) word the first word of
@            block B.  Retiring only the final word's blocks leaves A's stale
@            translation (b TA1) live: 1 instead of 2.
@   mixer    the mixer's shape: the same writer patches two interior slots of
@            a live block (add r0,#1 -> add r0,#5 -> add r0,#1), called after
@            each patch: 2, 10, 2.
@
@ Expected: the 4.5.0 core (full flush), SMC_STM_FIXED, and the interpreter
@ all pass.  SMC_STM_FIXED + SMC_STM_RANGE_MUTANT_LASTWORD (the retired span
@ is the final word only) must FAIL "cross" -- which also proves the new
@ path is the one that ran: without SMC_STM_FIXED the mutant changes nothing.
    .arm
    .section .text
    .global _start
    .equ IW,   0x03000000
    .equ AA,   IW + 0x100       @ A: mov r0,#0 ; b TA1
    .equ AK,   IW + 0x104
    .equ BA,   IW + 0x108       @ B: mov r0,#7 ; bx lr
    .equ TA1,  IW + 0x200       @ mov r0,#1 ; bx lr
    .equ TA2,  IW + 0x300       @ mov r0,#2 ; bx lr
    .equ WR,   IW + 0x400       @ writer: stmia r12,{r1,r2} ; bx lr
    .equ FA,   IW + 0x600       @ churn function: mov r0,#n ; bx lr
    .equ CB,   IW + 0x700       @ mixer block: mov r0,#0; add #1; add #1; bx lr
    .equ RES,  0x02000000
    .equ BX_LR, 0xE12FFF1E
    .equ ADD1, 0xE2800001       @ add r0, r0, #1
    .equ ADD5, 0xE2800005       @ add r0, r0, #5
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
    ldr     r1, =0xE88C0006     @ stmia r12, {r1, r2}   (W = 0)
    stmia   r0, {r1, r2}
    ldr     r0, =FA
    ldr     r1, =0xE3A00000
    stmia   r0, {r1, r2}
    ldr     r0, =CB
    ldr     r1, =0xE3A00000     @ mov r0, #0
    str     r1, [r0]
    ldr     r1, =ADD1
    str     r1, [r0, #4]
    str     r1, [r0, #8]
    str     r2, [r0, #12]
    ldr     r10, =RES           @ results in EWRAM
    mov     r11, #0             @ fail count
    @ activate selective retirement: translate F, then a single store to it
    bl      callF
    ldr     r0, =FA
    ldr     r1, =0xE3A000FF
    str     r1, [r0]
    bl      callF
    @ the first SMC_SINGLE_HOT (64) widened traps (ROM writer) full-flush
    ldr     r6, =100
    bl      churn

    @ ---------------------------------------------------- cross
    ldr     r4, =AA
    mov     r7, #1
    bl      call_check          @ translate A
    ldr     r4, =BA
    mov     r7, #7
    bl      call_check          @ translate B
    ldr     r12, =AK            @ A's last word := b TA2, B's first := mov r0,#8
    ldr     r1, =VB2
    ldr     r2, =0xE3A00008
    bl      write
    ldr     r4, =AA
    mov     r7, #2
    bl      call_check          @ 1 = stale A ran
    ldr     r4, =BA
    mov     r7, #8
    bl      call_check

    @ ---------------------------------------------------- mixer
    ldr     r4, =CB
    mov     r7, #2
    bl      call_check          @ translate the block
    ldr     r12, =CB + 4
    ldr     r1, =ADD5
    ldr     r2, =ADD5
    bl      write
    ldr     r4, =CB
    mov     r7, #10
    bl      call_check
    ldr     r12, =CB + 4
    ldr     r1, =ADD1
    ldr     r2, =ADD1
    bl      write
    ldr     r4, =CB
    mov     r7, #2
    bl      call_check

    ldr     r0, =RES + 0x100
    str     r11, [r0]
    cmp     r11, #0
    ldreq   r1, =0x600D600D
    ldrne   r1, =0x0BAD0BAD
    str     r1, [r0, #4]
done:
    b       done

call_check:                     @ call r4, store r0, count r0 != r7
    mov     r9, lr
    mov     lr, pc
    bx      r4
    str     r0, [r10], #4
    cmp     r0, r7
    addne   r11, r11, #1
    bx      r9

write:                          @ the IWRAM writer: stmia r12, {r1, r2}
    mov     r9, lr
    ldr     r4, =WR
    mov     lr, pc
    bx      r4
    bx      r9

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
