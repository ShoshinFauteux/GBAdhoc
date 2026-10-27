@ Build: arm-none-eabi-as -mcpu=arm7tdmi vram_dma.s -o s.o && arm-none-eabi-objcopy -O binary s.o vram_dma.gba
@ Run on the twin built with the C3 flags + VRAM_EXEC EXEC_STALE_CHECK, 20 frames: expect hits=0
@ and vramx armed=1.
@ VRAM_EXEC DMA marking (twin only).  ARM function F (mov r0,#1 ; bx lr) is
@ stored into VRAM at 06010000 and called (VRAM_EXEC arms, F is translated).
@ DMA3 then copies G (mov r0,#2 ; bx lr) over F from EWRAM, and F is called
@ again: the DMA must have marked F's page so the lookup retranslates it.
@ VX_DMA_NOTE_MUTANT (no marking for DMA) must give a stale hit at 06010000.
    .arm
    .section .text
    .global _start
    .equ FV,    0x06010000
    .equ SRC,   0x02010000
    .equ BX_LR, 0xE12FFF1E
_start:
    b       init
    .fill   0xBC, 1, 0
init:
    ldr     sp, =0x03007F00
    ldr     r10, =0x02000000    @ results in EWRAM
    ldr     r0, =FV
    ldr     r1, =0xE3A00001     @ mov r0,#1
    ldr     r2, =BX_LR
    str     r1, [r0]
    str     r2, [r0, #4]
    ldr     r4, =FV
    mov     lr, pc
    bx      r4
    str     r0, [r10], #4       @ expect 1
    ldr     r4, =FV
    mov     lr, pc
    bx      r4
    str     r0, [r10], #4       @ expect 1 (translated)
    @ G in EWRAM
    ldr     r0, =SRC
    ldr     r1, =0xE3A00002     @ mov r0,#2
    ldr     r2, =BX_LR
    str     r1, [r0]
    str     r2, [r0, #4]
    @ DMA3: SRC -> FV, 2 words, 32-bit, immediate
    ldr     r3, =0x040000D4
    ldr     r1, =SRC
    str     r1, [r3]
    ldr     r1, =FV
    str     r1, [r3, #4]
    ldr     r1, =0x84000002
    str     r1, [r3, #8]
    nop
    nop
    ldr     r4, =FV
    mov     lr, pc
    bx      r4
    str     r0, [r10], #4       @ expect 2 (1 = stale F ran)
done:
    b       done
    .pool
    .fill   0x1000, 1, 0
