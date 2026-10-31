@ Build: arm-none-eabi-as -mcpu=arm7tdmi pipe_timer.s -o s.o && arm-none-eabi-objcopy -O binary s.o pipe_timer.gba
@ Run (interpreter or dynarec, any build), 60 frames, dump a frame:
@   GREEN backdrop = every check passed, RED = at least one failed.
@   EWRAM 0x02000000.. holds each result word (order below), 0x02000100 the
@   fail count, 0x02000104 0x600D600D (pass) or 0x0BAD0BAD (fail).
@
@ Two hardware behaviours Pokemon MonoMyth's anti-emulator screen probes
@ (monomyth-compat, 2026-10-30; its "Timer Prescaler" and "Insn Prefetch"
@ tests, AGS-aging-cart style):
@
@ 1. LIVE TIMER COUNTERS.  For each timer 0..3 and prescaler 0..3, IWRAM
@    ARM code stops the timer, starts it, runs `subs r2,#1 / bne` 1024
@    times, two nops, and reads TMxCNT_L.  Hardware: 0x1000, 0x40, 0x10, 4
@    (4096 cycles).  gpSP 4.5.0 read the counter as of the start of the CPU
@    slice and charged the timer for the part of the slice before it was
@    started (0xAAC for prescaler 0), and charged a taken branch in RAM
@    1 cycle instead of 3.  16 results.
@
@ 2. PREFETCH WINDOW.  A store that rewrites one of the two instructions
@    after it does not change what runs next (the ARM7TDMI already fetched
@    them).  Results, ARM then Thumb, each test run twice (the second time
@    on a fresh copy of code that was already translated):
@      arm_slot2   overwrite str+8  (mov r0,#0xff -> mov r0,#0): 0xff
@      arm_slot1   overwrite str+4:                               0xff
@      arm_beyond  overwrite str+12 (outside the window):         0
@      thumb_slot2 overwrite strh+4:                              0xff
@      thumb_beyond overwrite strh+6 (outside):                   0
@    The "beyond" cases are the negative control: new code must run.
    .arm
    .section .text
    .global _start
    .equ IW,    0x03000000
    .equ CODE,  IW + 0x400
    .equ RES,   0x02000000
_start:
    b       init
    .fill   0xBC, 1, 0
init:
    ldr     sp, =0x03007F00
    ldr     r10, =RES
    mov     r11, #0                 @ fail count

    @ ---------------------------------------------------- timer prescaler
    mov     r5, #0                  @ timer index
t_timer:
    mov     r6, #0                  @ prescaler
t_pre:
    adr     r0, timer_tpl
    adr     r1, timer_tpl_end
    bl      copy_code
    ldr     r4, =0x04000100
    add     r4, r4, r5, lsl #2      @ &TMxCNT_L
    mov     r1, #0x80
    orr     r1, r1, r6
    mov     r1, r1, lsl #16         @ TMxCNT_H = enable | prescaler, reload 0
    mov     r2, #0x400
    mov     r3, #0
    ldr     r12, =CODE
    bl      run_code
    adr     r7, timer_expect
    ldr     r7, [r7, r6, lsl #2]
    bl      check
    add     r6, r6, #1
    cmp     r6, #4
    blt     t_pre
    add     r5, r5, #1
    cmp     r5, #4
    blt     t_timer

    @ ---------------------------------------------------- prefetch window
    mov     r8, #2                  @ each test twice
t_pipe:
    adr     r0, arm_slot2
    adr     r1, arm_slot2_end
    mov     r7, #0xff
    bl      run_arm
    adr     r0, arm_slot1
    adr     r1, arm_slot1_end
    mov     r7, #0xff
    bl      run_arm
    adr     r0, arm_beyond
    adr     r1, arm_beyond_end
    mov     r7, #0
    bl      run_arm
    adr     r0, thumb_slot2
    adr     r1, thumb_slot2_end
    ldr     r2, =CODE + 6           @ the overwritten halfword
    mov     r7, #0xff
    bl      run_thumb
    adr     r0, thumb_beyond
    adr     r1, thumb_beyond_end
    ldr     r2, =CODE + 8
    mov     r7, #0
    bl      run_thumb
    subs    r8, r8, #1
    bne     t_pipe

    @ ---------------------------------------------------- verdict
    ldr     r0, =RES + 0x100
    str     r11, [r0]
    cmp     r11, #0
    ldreq   r1, =0x600D600D
    ldrne   r1, =0x0BAD0BAD
    str     r1, [r0, #4]
    ldr     r0, =0x05000000
    ldreq   r1, =0x03E0             @ green
    ldrne   r1, =0x001F             @ red
    strh    r1, [r0]
    ldr     r0, =0x04000000
    mov     r1, #0
    strh    r1, [r0]                @ DISPCNT: mode 0, nothing on: backdrop
done:
    b       done

@ r0..r1 = template, r7 = expected; ARM entry
run_arm:
    mov     r9, lr
    bl      copy_code
    ldr     r12, =CODE
    bl      run_code
    bl      check
    bx      r9

@ r0..r1 = template, r2 = halfword to overwrite, r7 = expected; Thumb entry
run_thumb:
    mov     r9, lr
    mov     r4, r2
    bl      copy_code
    mov     r2, r4
    ldr     r3, =0x2000             @ movs r0, #0
    ldr     r12, =CODE + 1
    bl      run_code
    bl      check
    bx      r9

run_code:                           @ tail call: the code returns to our caller
    bx      r12

@ r0 = result, r7 = expected: record it, count a mismatch
check:
    str     r0, [r10], #4
    cmp     r0, r7
    addne   r11, r11, #1
    bx      lr

@ copy [r0, r1) to CODE (clobbers r0-r3)
copy_code:
    ldr     r2, =CODE
1:  ldr     r3, [r0], #4
    str     r3, [r2], #4
    cmp     r0, r1
    blt     1b
    bx      lr

timer_expect:
    .word   0x1000, 0x40, 0x10, 0x4

@ ---------------------------------------------------- templates (run at CODE)
timer_tpl:                          @ r4 = &TMxCNT_L, r1 = control<<16
    str     r3, [r4]                @ stop
    str     r1, [r4]                @ start
1:  subs    r2, r2, #1
    bne     1b
    nop
    nop
    ldrh    r0, [r4]
    bx      lr
timer_tpl_end:

arm_slot2:                          @ MonoMyth's own sequence
    mov     r1, lr
    mov     r0, #0
    add     lr, pc, #8              @ lr = str + 8
    ldr     r0, [pc, #-0x10]        @ r0 = the opcode `mov r0, #0` above
    str     r0, [lr]
    mov     r0, #0xff
    mov     r0, #0xff               @ overwritten: already fetched
    bx      r1
arm_slot2_end:

arm_slot1:
    mov     r1, lr
    mov     r0, #0
    add     lr, pc, #4              @ lr = str + 4
    ldr     r0, [pc, #-0x10]
    str     r0, [lr]
    mov     r0, #0xff               @ overwritten: already decoded
    bx      r1
arm_slot1_end:

arm_beyond:
    mov     r1, lr
    mov     r0, #0
    add     lr, pc, #12             @ lr = str + 12
    ldr     r0, [pc, #-0x10]
    str     r0, [lr]
    mov     r0, #0xff
    mov     r0, #0xff
    mov     r0, #0xff               @ overwritten, not yet fetched: mov r0,#0
    bx      r1
arm_beyond_end:

    .thumb
thumb_slot2:                        @ r2 = CODE + 6, r3 = 0x2000
    movs    r0, #0
    strh    r3, [r2]                @ CODE + 2
    movs    r0, #0xff
    movs    r0, #0xff               @ CODE + 6: overwritten, already fetched
    bx      lr
    .align  2
thumb_slot2_end:

thumb_beyond:                       @ r2 = CODE + 8
    movs    r0, #0
    strh    r3, [r2]                @ CODE + 2
    movs    r0, #0xff
    movs    r0, #0xff
    movs    r0, #0xff               @ CODE + 8: outside the window -> movs r0,#0
    bx      lr
    .align  2
thumb_beyond_end:

    .arm
    .ltorg
