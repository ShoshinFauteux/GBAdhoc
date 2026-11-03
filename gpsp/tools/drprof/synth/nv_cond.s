@ Build: arm-none-eabi-as -mcpu=arm7tdmi nv_cond.s -o n.o && arm-none-eabi-objcopy -O binary n.o nv_cond.gba
@ Run on the twin (dynarec), 10 frames, DR_RAMDUMP=10, then
@   python3 nv_cond_check.py ramdump.bin
@ and once more with --option gpsp_drc=disabled (the interpreter: the
@ reference).  GREEN backdrop = every check passed, RED = at least one failed.
@ EWRAM 0x02000000.. holds each result word (order below), 0x02000100 the
@ fail count, 0x02000104 0x600D600D (pass) or 0x0BAD0BAD (fail).
@
@ ARM condition 0xF (NV) on the ARM7TDMI: the instruction never executes
@ (ARMv4 reserves the encoding; the interpreter's "treat as never").  The
@ translator opened no skip for it, so it ran NV words as AL and left
@ backpatch_address pointing at nothing (or at an older skip):
@
@   nv_stale  EQ (taken) then NV: the EQ skip was re-patched past the NV
@             word, which then ran.  0x33 expected; 4.5.0 gives 0xff.
@   nv_first  NV is the block's first conditional: the next instruction
@             patched a NULL backpatch_address.  0x11 expected; 4.5.0
@             faults (twin: SIGSEGV; PSP: DBE at translate_block_arm).
@   nv_last   the block ENDS on NV words (32 SWI exits = MAX_EXITS): the
@             patch after the emit loop, the exact 4.5.0 PSP crash
@             (issue #10, TMGC: a block scanned from mixer data in IWRAM,
@             words 0xFFxxFFxx).  0x22 expected (swinv 0x08 never runs).
@
@ Each runs from ROM, then from an IWRAM copy.  Twin: 4.5.0 faults in
@ nv_first (nv_stale alone reads 0xff, nv_last alone faults in the
@ end patch); the interpreter and the fix pass 6/6.  The mutant
@ tools/drprof/mutants/nv_never_taken.patch (a never-taken skip:
@ backpatch valid, NV still executed) fails 6/6 without faulting.
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

    @ ---------------------------------------------------- from ROM
    adr     r12, nv_stale
    mov     r7, #0x33
    bl      run_rom
    adr     r12, nv_first
    mov     r7, #0x11
    bl      run_rom
    adr     r12, nv_last
    mov     r7, #0x22
    bl      run_rom

    @ ---------------------------------------------------- from IWRAM
    adr     r0, nv_stale
    adr     r1, nv_stale_end
    mov     r7, #0x33
    bl      run_ram
    adr     r0, nv_first
    adr     r1, nv_first_end
    mov     r7, #0x11
    bl      run_ram
    adr     r0, nv_last
    adr     r1, nv_last_end
    mov     r7, #0x22
    bl      run_ram

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

@ r12 = template in ROM, r7 = expected
run_rom:
    mov     r9, lr
    bl      run_code
    bl      check
    bx      r9

@ r0..r1 = template, r7 = expected; runs at CODE
run_ram:
    mov     r9, lr
    bl      copy_code
    ldr     r12, =CODE
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

    .ltorg

@ ---------------------------------------------------- templates
@ (.word: the assembler refuses the NV condition)
nv_stale:
    mov     r0, #0
    cmp     r0, #0                  @ Z set: the EQ below runs
    moveq   r0, #0x33
    .word   0xF3A000FF              @ movnv r0, #0xff: never
    bx      lr
nv_stale_end:

nv_first:
    mov     r0, #0x11
    .word   0xF3A000FF              @ movnv r0, #0xff: never
    bx      lr
nv_first_end:

nv_last:
    mov     r0, #0x22
    .rept   32
    .word   0xFF080000              @ swinv 0x08 (Sqrt): never
    .endr
    bx      lr
nv_last_end:
