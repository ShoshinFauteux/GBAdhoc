@ Build: arm-none-eabi-as -mcpu=arm7tdmi mirror_pc.s -o m.o && arm-none-eabi-objcopy -O binary m.o mirror_pc.gba
@   (or: clang --target=armv4t-none-eabi -c mirror_pc.s -o m.o && llvm-objcopy -O binary m.o mirror_pc.gba)
@ Run on the twin (dynarec), 40 frames, DR_RAMDUMP=30, then
@   python3 mirror_pc_check.py ramdump.bin
@ EWRAM 0x02000000.. holds each result word (order in mirror_pc_check.py),
@ 0x02000100 the fail count, 0x02000104 0x600D600D (pass) or 0x0BAD0BAD.
@
@ RAM_MIRROR_FIX test: RAM code reached through a mirror alias (IWRAM repeats
@ every 32 KiB in region 3, EWRAM every 256 KiB in region 2) is the code at
@ the canonical address.  Every function below that returns its own pc
@ ("mov r0, pc") is checked only when it is called at its CANONICAL address,
@ where the GBA, the interpreter and the fixed dynarec agree; what a call
@ through the alias returns is stored but not checked (the GBA says the
@ alias, the fixed dynarec the canonical address).
@
@   arm      F1 called at an IWRAM alias first, then at its canonical
@            address: the lookup shared F1's tag with the alias and handed the
@            canonical caller the alias's translation (alias + 8).
@   exit     H2's never-taken bne to an alias of F2 (a speculative scan's
@            garbage, as in TMGC's PCM buffer): the eager link translated F2 at
@            the alias, so F2 at its canonical address returned alias + 8.
@   thumb    F3 (Thumb, "mov r0, pc") as "arm".
@   ewram    F4 in EWRAM as "arm", through a 256 KiB alias.
@   ewexit   H5 in IWRAM, a never-taken bne to an EWRAM alias of F5, as "exit".
@   smc      G: translated at an alias, then patched by a store to its
@            canonical address, then by a store to an alias: every call, at
@            either address, sees the newest code (1, 2, 2, 3, 3).
@   stm      the mixer's paste: a fixed-base STMIA (SMC_STM_FIXED) writes two
@            words into a live block through an ALIAS of it (2, 10, 10).
@
@ Expected: the fixed dynarec and the interpreter pass; the 4.6.0-beta.2 core
@ (no RAM_MIRROR_FIX) fails arm, exit, thumb, ewram and ewexit; the
@ rm_no_canon mutant (lookup not canonical, lazy alias exits kept) fails arm,
@ thumb and ewram -- the call site in block_lookup_translate_* is the one the
@ test exercises.
    .arm
    .section .text
    .global _start
    .equ IW,    0x03000000
    .equ AL,    0x03100000      @ IWRAM alias (+32 x 32 KiB)
    .equ AL2,   0x03F08000      @ a far IWRAM alias
    .equ EW,    0x02000000
    .equ EAL,   0x02040000      @ EWRAM alias (+256 KiB)
    .equ EAL2,  0x02100000      @ EWRAM alias (+4 x 256 KiB)
    .equ F1,    IW + 0x400      @ mov r0, pc ; bx lr
    .equ F2,    IW + 0x480      @ mov r0, pc ; bx lr
    .equ H2,    IW + 0x500      @ cmp r0, r0 ; bne AL2+F2 ; mov r0, #5 ; bx lr
    .equ F3,    IW + 0x600      @ Thumb: mov r0, pc ; bx lr
    .equ H5,    IW + 0x700      @ cmp r0, r0 ; bne EAL2+F5 ; mov r0, #6 ; bx lr
    .equ G,     IW + 0x800      @ mov r0, #n ; bx lr
    .equ CB,    IW + 0xA00      @ mov r0,#0 ; add r0,#1 ; add r0,#1 ; bx lr
    .equ WR,    IW + 0xC00      @ stmia r12, {r1, r2} ; bx lr
    .equ FA,    IW + 0xD00      @ churn function: mov r0, #n ; bx lr
    .equ F4,    EW + 0x1000     @ mov r0, pc ; bx lr
    .equ F5,    EW + 0x1100     @ mov r0, pc ; bx lr
    .equ RES,   EW
    .equ MOVPC, 0xE1A0000F      @ mov r0, pc
    .equ BX_LR, 0xE12FFF1E
    .equ CMP00, 0xE1500000      @ cmp r0, r0
    .equ ADD1,  0xE2800001      @ add r0, r0, #1
    .equ ADD5,  0xE2800005      @ add r0, r0, #5
    .equ BNE2,  0x1A000000 | ((((AL2 + 0x480) - (H2 + 4 + 8)) >> 2) & 0xFFFFFF)
    .equ BNE5,  0x1A000000 | ((((EAL2 + 0x1100) - (H5 + 4 + 8)) >> 2) & 0xFFFFFF)
_start:
    b       init
    .fill   0xBC, 1, 0
init:
    ldr     sp, =0x03007F00
    adr     r0, code            @ copy the (address, word) table into RAM
1:
    ldmia   r0!, {r1, r2}
    cmp     r1, #0
    strne   r2, [r1]
    bne     1b
    ldr     r10, =RES           @ results in EWRAM
    mov     r11, #0             @ fail count
    @ activate selective retirement (as stm_fixed.s): translate FA, a single
    @ store to it, then the SMC_SINGLE_HOT widened traps of a ROM writer
    bl      callF
    ldr     r0, =FA
    ldr     r1, =0xE3A000FF
    str     r1, [r0]
    bl      callF
    ldr     r6, =100
    bl      churn

    @ ---------------------------------------------------- arm
    ldr     r4, =AL + 0x400
    bl      call_only
    ldr     r4, =F1
    ldr     r7, =F1 + 8
    bl      call_check
    @ ---------------------------------------------------- exit
    ldr     r4, =H2
    mov     r7, #5
    bl      call_check
    ldr     r4, =F2
    ldr     r7, =F2 + 8
    bl      call_check
    @ ---------------------------------------------------- thumb
    ldr     r4, =AL + 0x601
    bl      call_only
    ldr     r4, =F3 + 1
    ldr     r7, =F3 + 4
    bl      call_check
    @ ---------------------------------------------------- ewram
    ldr     r4, =EAL + 0x1000
    bl      call_only
    ldr     r4, =F4
    ldr     r7, =F4 + 8
    bl      call_check
    @ ---------------------------------------------------- ewexit
    ldr     r4, =H5
    mov     r7, #6
    bl      call_check
    ldr     r4, =F5
    ldr     r7, =F5 + 8
    bl      call_check
    @ ---------------------------------------------------- smc
    ldr     r4, =AL + 0x800
    mov     r7, #1
    bl      call_check          @ translate G at the alias
    ldr     r0, =G
    ldr     r1, =0xE3A00002     @ mov r0, #2, stored at the canonical address
    str     r1, [r0]
    ldr     r4, =AL + 0x800
    mov     r7, #2
    bl      call_check
    ldr     r4, =G
    mov     r7, #2
    bl      call_check
    ldr     r0, =AL + 0x800
    ldr     r1, =0xE3A00003     @ mov r0, #3, stored through the alias
    str     r1, [r0]
    ldr     r4, =G
    mov     r7, #3
    bl      call_check
    ldr     r4, =AL + 0x800
    mov     r7, #3
    bl      call_check
    @ ---------------------------------------------------- stm
    ldr     r4, =AL + 0xA00
    mov     r7, #2
    bl      call_check          @ translate CB at the alias
    ldr     r12, =AL + 0xA04    @ the paste lands on CB's two adds via the alias
    ldr     r1, =ADD5
    ldr     r2, =ADD5
    bl      write
    ldr     r4, =CB
    mov     r7, #10
    bl      call_check
    ldr     r4, =AL + 0xA00
    mov     r7, #10
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

call_only:                      @ call r4, store r0 (not checked)
    mov     r9, lr
    mov     lr, pc
    bx      r4
    str     r0, [r10], #4
    bx      r9

write:                          @ the IWRAM writer: stmia r12, {r1, r2}
    mov     r9, lr
    ldr     r4, =WR
    mov     lr, pc
    bx      r4
    bx      r9

callF:
    ldr     r4, =FA
    bx      r4                  @ FA returns to our caller via lr

churn:                          @ r6 iterations: change FA's immediate, call FA
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

code:                           @ (address, word), ended by address 0
    .word   F1,      MOVPC
    .word   F1 + 4,  BX_LR
    .word   F2,      MOVPC
    .word   F2 + 4,  BX_LR
    .word   H2,      CMP00
    .word   H2 + 4,  BNE2
    .word   H2 + 8,  0xE3A00005
    .word   H2 + 12, BX_LR
    .word   F3,      0x47704678 @ Thumb: mov r0, pc ; bx lr
    .word   H5,      CMP00
    .word   H5 + 4,  BNE5
    .word   H5 + 8,  0xE3A00006
    .word   H5 + 12, BX_LR
    .word   G,       0xE3A00001
    .word   G + 4,   BX_LR
    .word   CB,      0xE3A00000
    .word   CB + 4,  ADD1
    .word   CB + 8,  ADD1
    .word   CB + 12, BX_LR
    .word   WR,      0xE88C0006 @ stmia r12, {r1, r2}   (W = 0)
    .word   WR + 4,  BX_LR
    .word   FA,      0xE3A00000
    .word   FA + 4,  BX_LR
    .word   F4,      MOVPC
    .word   F4 + 4,  BX_LR
    .word   F5,      MOVPC
    .word   F5 + 4,  BX_LR
    .word   0, 0
    .fill   0x1000, 1, 0
