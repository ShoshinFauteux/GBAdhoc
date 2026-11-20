@ Build: arm-none-eabi-as -mcpu=arm7tdmi stm_one.s -o s.o && arm-none-eabi-objcopy -O binary s.o stm_one.gba
@ Run on the twin (dynarec), 20 frames, DR_RAMDUMP=19, then
@   python3 stm_one_check.py ramdump.bin
@ EWRAM 0x02000000.. holds each result word (order below), 0x02000100 the
@ fail count, 0x02000104 0x600D600D (pass) or 0x0BAD0BAD (fail).
@
@ One-register STM span test: smc_trap_entry retires a ONE-register
@ STMIA Rn!,{Rx} / STMIA Rn,{Rx} from IWRAM into IWRAM exactly (tonc's
@ memcpy32 leftover loop, MonoMyth's stack-code copy).
@
@   start     STMIA r12!,{r1} writes the FIRST word of live block B
@             (mov r0,#7 -> mov r0,#8): 7, then 8.
@   interior  STMIA r12!,{r1} writes the LAST word of live block A, which
@             starts one word earlier (b TA1 -> b TA2): 1, then 2.
@   fixed     STMIA r12,{r1} (no writeback) rewrites B's first word again
@             (mov r0,#8 -> mov r0,#9): 9.
@
@ Expected: the full-flush core (one register not admitted), the one-register
@ span path, and the interpreter all pass.  With SMC_COVERAGE_MUTANT_SKIP_LAST
@ (smc_retire_span never retires a block that STARTS inside the range) the
@ span path must FAIL "start" and "fixed" (B's stale 7 / 8 runs), which also
@ proves that the span path is the one that ran: on a core that full-flushes
@ one-register stores the mutant changes nothing here.
    .arm
    .section .text
    .global _start
    .equ IW,   0x03000000
    .equ AA,   IW + 0x100       @ A: mov r0,#0 ; b TA1
    .equ AK,   IW + 0x104
    .equ BA,   IW + 0x108       @ B: mov r0,#7 ; bx lr
    .equ TA1,  IW + 0x200       @ mov r0,#1 ; bx lr
    .equ TA2,  IW + 0x300       @ mov r0,#2 ; bx lr
    .equ WRW,  IW + 0x400       @ writer: stmia r12!,{r1} ; bx lr
    .equ WRF,  IW + 0x480       @ writer: stmia r12,{r1} ; bx lr
    .equ FA,   IW + 0x600       @ churn function: mov r0,#n ; bx lr
    .equ RES,  0x02000000
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
    ldr     r0, =WRW
    ldr     r1, =0xE8AC0002     @ stmia r12!, {r1}   (W = 1)
    stmia   r0, {r1, r2}
    ldr     r0, =WRF
    ldr     r1, =0xE88C0002     @ stmia r12, {r1}    (W = 0)
    stmia   r0, {r1, r2}
    ldr     r0, =FA
    ldr     r1, =0xE3A00000
    stmia   r0, {r1, r2}
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

    @ ---------------------------------------------------- start
    ldr     r4, =AA
    mov     r7, #1
    bl      call_check          @ translate A
    ldr     r4, =BA
    mov     r7, #7
    bl      call_check          @ translate B
    ldr     r12, =BA            @ B's first word := mov r0,#8
    ldr     r1, =0xE3A00008
    ldr     r4, =WRW
    bl      write
    ldr     r4, =BA
    mov     r7, #8
    bl      call_check          @ 7 = stale B ran

    @ ---------------------------------------------------- interior
    ldr     r4, =AA
    mov     r7, #1
    bl      call_check          @ A still live and unchanged
    ldr     r12, =AK            @ A's last word := b TA2
    ldr     r1, =VB2
    ldr     r4, =WRW
    bl      write
    ldr     r4, =AA
    mov     r7, #2
    bl      call_check          @ 1 = stale A ran

    @ ---------------------------------------------------- fixed
    ldr     r12, =BA            @ B's first word := mov r0,#9
    ldr     r1, =0xE3A00009
    ldr     r4, =WRF
    bl      write
    ldr     r4, =BA
    mov     r7, #9
    bl      call_check          @ 8 = stale B ran

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

write:                          @ call the IWRAM writer in r4
    mov     r9, lr
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
