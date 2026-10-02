#!/usr/bin/env python3
"""Generate the synthetic mid-frame test ROMs (GNU as source, ARM7TDMI).

Each ROM exercises one way a game can change what the PPU draws while
lines 0-159 are being drawn.  run_me_midframe.sh assembles them and runs them
through the desktop twin with ME_TIMING_SIM, which replays every frame the way
the Media Engine does and compares it with the CPU renderer's frame.

    gen_roms.py OUTDIR

Cases (see docs/ME-MIDFRAME.md for what each is expected to show):

  ctl_mode0     static mode-0 BG, no IRQ                      control
  ctl_affine    static mode-2 rotation, PB/PD != 0, no IRQ    control
  aff_irq       mode 2, BG2+BG3 X/Y (and PA) rewritten in an HBlank IRQ  (a)
  aff_dma       mode 1, BG2X/BG2Y streamed by HBlank DMA0              (a)
  aff_mos_irq   aff_irq with vertical BG mosaic                        (a)
  aff_vblank    affine reference written only in VBlank       control
  pal_irq       palette colour 1 rewritten in an HBlank IRQ            (b)
  pal_dma       palette colour 1 streamed by HBlank DMA0               (b)
  vram_irq      BG tile data rewritten (STM) in an HBlank IRQ          (c)
  vram_dma      BG tile data streamed by HBlank DMA0                   (c)
  oam_irq       sprite X rewritten in an HBlank IRQ           (OAM, extra)
  m4_back       mode 4, draw back page + flip in VBlank                (d)
  m4_front      mode 4, draw the DISPLAYED page, unsynchronised        (d)
  m4_flipmid    mode 4, page flip at line 80 (VCOUNT IRQ)              (d)
  vram_offscreen  mode 0, main loop writes VRAM no BG/OBJ reads  control
  sa2_waves     Sonic Advance 2 title sea: per-line BG2 affine by HBlank
                DMA (PC=PD=0) + CpuFastSet palette gradient in HBlank IRQ
"""
import os
import sys

HDR = r"""
    .arm
    .section .text
    .global _start
_start:
    b start
    .fill 0xBC, 1, 0          @ cartridge header area (unused by the twin)
start:
    msr cpsr_c, #0x1F         @ System mode, IRQ unmasked
    ldr sp, =0x03007F00
    mov r4, #0x04000000
    mov r0, #0x80             @ forced blank during setup
    strh r0, [r4]
    ldr r0, =irq_handler
    ldr r1, =0x03007FFC
    str r0, [r1]
"""

COPY = r"""
@ r0 = src, r1 = dst, r2 = words
copy_words:
    ldr r3, [r0], #4
    str r3, [r1], #4
    subs r2, r2, #1
    bne copy_words
    bx lr
@ r0 = value, r1 = dst, r2 = words
fill_words:
    str r0, [r1], #4
    subs r2, r2, #1
    bne fill_words
    bx lr
"""

WAIT_VBL = r"""
1:  ldrh r0, [r4, #6]
    cmp r0, #160
    bne 1b
"""
WAIT_VBL_END = r"""
2:  ldrh r0, [r4, #6]
    cmp r0, #160
    beq 2b
"""

# IRQ: BIOS saved r0-r3,r12,lr.  r0 = 0x04000000, r1 = VCOUNT+1 (the line the
# write is meant for), then the case body, then acknowledge everything.
IRQ_HEAD = r"""
irq_handler:
    mov r0, #0x04000000
    ldrh r1, [r0, #6]
    add r1, r1, #1
"""
IRQ_TAIL = r"""
    add r3, r0, #0x200
    ldrh r2, [r3, #2]
    strh r2, [r3, #2]         @ IF: acknowledge what fired
    bx lr
"""


def words(vals):
    out = []
    for i in range(0, len(vals), 8):
        out.append("    .word " + ", ".join("0x%08x" % (v & 0xFFFFFFFF) for v in vals[i:i + 8]))
    return "\n".join(out)


def halfs(vals):
    out = []
    for i in range(0, len(vals), 16):
        out.append("    .hword " + ", ".join("0x%04x" % (v & 0xFFFF) for v in vals[i:i + 16]))
    return "\n".join(out)


def pal256():
    return [((i * 0x1234) ^ (i << 7)) & 0x7FFF if i else 0x0000 for i in range(256)]


def affine_tiles():
    """256 8bpp tiles, 64 bytes each, pixel = (t + (x ^ y) * 3) & 0xFF."""
    data = bytearray()
    for t in range(256):
        for y in range(8):
            for x in range(8):
                data.append((t + (x ^ y) * 3) & 0xFF)
    return [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]


def affine_map(n=32 * 32):
    data = bytes((k * 7) & 0xFF for k in range(n))
    return [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]


AFFINE_SETUP = r"""
    ldr r0, =pal_data
    ldr r1, =0x05000000
    mov r2, #128
    bl copy_words
    ldr r0, =tile_data
    ldr r1, =0x06000000
    ldr r2, =4096
    bl copy_words
    ldr r0, =map_data
    ldr r1, =0x06004000
    mov r2, #256
    bl copy_words
"""

AFFINE_DATA = lambda: ("pal_data:\n" + halfs(pal256()) + "\n    .align 2\ntile_data:\n" +
                       words(affine_tiles()) + "\nmap_data:\n" + words(affine_map()) + "\n")

# BG2CNT/BG3CNT: char block 0, screen block 8 (0x4000), wrap, 256x256.
BGCNT_AFF = 0x6000 | (8 << 8)


def rom(init, irq_body="", vblank="", data="", loop=None):
    src = HDR + init
    src += "main_loop:\n"
    if loop is not None:
        src += loop
    else:
        src += WAIT_VBL + vblank + WAIT_VBL_END + "    b main_loop\n"
    src += IRQ_HEAD + irq_body + IRQ_TAIL + COPY + "    .ltorg\n    .align 2\n" + data
    return src


def enable_hblank_irq():
    return r"""
    ldr r1, =0x04000200
    mov r0, #2                @ IE = HBlank
    strh r0, [r1]
    mov r0, #1
    strh r0, [r1, #8]         @ IME
    mov r0, #0x10             @ DISPSTAT: HBlank IRQ enable
    strh r0, [r4, #4]
"""


def affine_regs(pb=0, pd=0x100, pa=0x100, pc=0, bg3=True):
    s = "    ldr r0, =0x%x\n    strh r0, [r4, #0x20]\n" % pa
    s += "    ldr r0, =0x%x\n    strh r0, [r4, #0x22]\n" % pb
    s += "    ldr r0, =0x%x\n    strh r0, [r4, #0x24]\n" % pc
    s += "    ldr r0, =0x%x\n    strh r0, [r4, #0x26]\n" % pd
    if bg3:
        s += "    ldr r0, =0x%x\n    strh r0, [r4, #0x30]\n" % 0xF0
        s += "    ldr r0, =0x%x\n    strh r0, [r4, #0x32]\n" % (pb + 0x20)
        s += "    ldr r0, =0x%x\n    strh r0, [r4, #0x34]\n" % 0x10
        s += "    ldr r0, =0x%x\n    strh r0, [r4, #0x36]\n" % (pd + 0x08)
    return s


def set_reg(off, val):
    return "    ldr r0, =0x%x\n    strh r0, [r4, #0x%x]\n" % (val, off)


def dma0_arm(src_label, dst, count, cnt_h):
    return (r"""
    ldr r1, =0x040000B0
    mov r0, #0
    strh r0, [r1, #10]        @ DMA0CNT_H = 0 (stop)
    ldr r0, =%s
    str r0, [r1]              @ SAD
    ldr r0, =0x%08x
    str r0, [r1, #4]          @ DAD
    ldr r0, =%d
    strh r0, [r1, #8]         @ CNT_L
    ldr r0, =0x%04x
    strh r0, [r1, #10]        @ CNT_H
""" % (src_label, dst, count, cnt_h))


# DMA0CNT_H bits: enable 0x8000, HBlank start 0x2000, 32-bit 0x0400,
# repeat 0x0200, dest reload 0x0060, dest fixed 0x0040.
DMA_HB32_RELOAD = 0xA660
DMA_HB16_FIXED = 0xA240

CASES = {}

# ---------------------------------------------------------------- controls
CASES["ctl_mode0"] = rom(
    init=r"""
    ldr r0, =0x7C1F
    ldr r1, =0x05000002
    strh r0, [r1]
    ldr r0, =0x11111111
    ldr r1, =0x06000020
    mov r2, #8
    bl fill_words
    ldr r0, =0x00010001
    ldr r1, =0x0600F800
    mov r2, #512
    bl fill_words
""" + set_reg(0x08, 0x1F00) + set_reg(0x00, 0x0100))

CASES["ctl_affine"] = rom(
    init=AFFINE_SETUP + affine_regs(pb=0x40, pd=0xF0, pa=0xE0, pc=0x30) +
    set_reg(0x0C, BGCNT_AFF) + set_reg(0x0E, BGCNT_AFF | 1) + set_reg(0x00, 0x0C02),
    data=AFFINE_DATA())

# ---------------------------------------------------------------- (a) affine
AFF_IRQ = r"""
    mov r2, r1, lsl #8
    add r2, r2, r1, lsl #10   @ BG2X = (L+1) * 5.0
    str r2, [r0, #0x28]
    mov r2, r1, lsl #9        @ BG2Y = (L+1) * 2.0
    str r2, [r0, #0x2C]
    rsb r2, r1, #0
    mov r2, r2, lsl #9        @ BG3X = -(L+1) * 2.0
    str r2, [r0, #0x38]
    mov r2, r1, lsl #10       @ BG3Y = (L+1) * 4.0
    str r2, [r0, #0x3C]
    add r2, r1, #0x80         @ BG2PA = 0x80 + L+1 (already in the line log)
    strh r2, [r0, #0x20]
"""
CASES["aff_irq"] = rom(
    init=AFFINE_SETUP + affine_regs(pb=0x10, pd=0x100) + set_reg(0x0C, BGCNT_AFF) +
    set_reg(0x0E, BGCNT_AFF | 1) + enable_hblank_irq() + set_reg(0x00, 0x0C02),
    irq_body=AFF_IRQ, data=AFFINE_DATA())

CASES["aff_mos_irq"] = rom(
    init=AFFINE_SETUP + affine_regs(pb=0x10, pd=0x100) + set_reg(0x0C, BGCNT_AFF | 0x40) +
    set_reg(0x0E, BGCNT_AFF | 1) + set_reg(0x4C, 0x0032) + enable_hblank_irq() +
    set_reg(0x00, 0x0C02),
    irq_body=AFF_IRQ, data=AFFINE_DATA())

# BG2X/BG2Y for line L+1 at table entry L: a perspective-ish road.
road = []
for L in range(160):
    n = L + 1
    road += [(n * n * 3) & 0x0FFFFFFF, (n * 0x280) & 0x0FFFFFFF]
CASES["aff_dma"] = rom(
    init=AFFINE_SETUP + affine_regs(pb=0, pd=0x100, bg3=False) + set_reg(0x0C, BGCNT_AFF) +
    set_reg(0x00, 0x0401),
    vblank=dma0_arm("road_tab", 0x04000028, 2, DMA_HB32_RELOAD),
    data=AFFINE_DATA() + "road_tab:\n" + words(road) + "\n")

CASES["aff_vblank"] = rom(
    init=AFFINE_SETUP + affine_regs(pb=0x20, pd=0x100, bg3=False) + set_reg(0x0C, BGCNT_AFF) +
    set_reg(0x00, 0x0402) + "    mov r5, #0\n",
    vblank=r"""
    add r5, r5, #0x300
    str r5, [r4, #0x28]
    str r5, [r4, #0x2C]
""", data=AFFINE_DATA())

# ---------------------------------------------------------------- (b) palette
MODE0_SOLID = r"""
    ldr r0, =0x11111111
    ldr r1, =0x06000020
    mov r2, #8
    bl fill_words
    ldr r0, =0x00010001
    ldr r1, =0x0600F800
    mov r2, #512
    bl fill_words
""" + set_reg(0x08, 0x1F00)

CASES["pal_irq"] = rom(
    init=MODE0_SOLID + enable_hblank_irq() + set_reg(0x00, 0x0100),
    irq_body=r"""
    and r2, r1, #31
    orr r3, r2, r2, lsl #5
    orr r2, r3, r2, lsl #10   @ grey level (L+1) & 31
    mov r3, #0x05000000
    strh r2, [r3, #2]
""")

grad = [((L + 1) & 31) << 10 | ((L + 1) * 3 & 31) for L in range(160)]
CASES["pal_dma"] = rom(
    init=MODE0_SOLID + set_reg(0x00, 0x0100),
    vblank=dma0_arm("grad_tab", 0x05000002, 1, DMA_HB16_FIXED),
    data="grad_tab:\n" + halfs(grad) + "\n")

# ---------------------------------------------------------------- (c) VRAM
PAL16 = "pal16:\n" + halfs([0] + [((i * 0x0842) ^ (i << 11)) & 0x7FFF for i in range(1, 16)]) + "\n"
MODE0_TILE1 = r"""
    ldr r0, =pal16
    ldr r1, =0x05000000
    mov r2, #8
    bl copy_words
    ldr r0, =0x00010001
    ldr r1, =0x0600F800
    mov r2, #512
    bl fill_words
""" + set_reg(0x08, 0x1F00)

CASES["vram_irq"] = rom(
    init=MODE0_TILE1 + enable_hblank_irq() + set_reg(0x00, 0x0100),
    irq_body=r"""
    stmfd sp!, {r4-r8}
    and r2, r1, #15
    orr r2, r2, r2, lsl #4
    orr r2, r2, r2, lsl #8
    orr r2, r2, r2, lsl #16
    mov r3, r2
    mov r4, r2
    mov r5, r2
    mov r6, r2
    mov r7, r2
    mov r8, r2
    mov r12, r2
    ldr r0, =0x06000020
    stmia r0, {r2-r8, r12}    @ tile 1 = solid colour (L+1) & 15
    ldmfd sp!, {r4-r8}
    mov r0, #0x04000000
""", data=PAL16)

tiles = []
for L in range(160):
    c = (L + 1) & 15
    v = c * 0x11111111
    tiles += [v ^ (0x0F000000 if r & 1 else 0) for r in range(8)]
CASES["vram_dma"] = rom(
    init=MODE0_TILE1 + set_reg(0x00, 0x0100),
    vblank=dma0_arm("tile_tab", 0x06000020, 8, DMA_HB32_RELOAD),
    data=PAL16 + "tile_tab:\n" + words(tiles) + "\n")

# Writes VRAM continuously from the main loop, but only at 0x08000+ which no
# enabled layer reads (BG0 uses char block 0 and screen block 31).
CASES["vram_offscreen"] = rom(
    init=MODE0_TILE1 + set_reg(0x00, 0x0100) + "    mov r5, #0\n",
    loop=r"""
    add r5, r5, #1
    mov r0, r5
    ldr r1, =0x06008000
    ldr r2, =2048
    bl fill_words
    b main_loop
""", data=PAL16)

# ---------------------------------------------------------------- OAM
CASES["oam_irq"] = rom(
    init=r"""
    ldr r0, =0x7C00
    ldr r1, =0x05000202
    strh r0, [r1]             @ OBJ palette colour 1
    ldr r0, =0x11111111
    ldr r1, =0x06010000
    ldr r2, =512
    bl fill_words             @ 64x64 4bpp sprite = 64 tiles
    ldr r0, =0x02000200       @ attr0 = disabled, for every sprite
    ldr r1, =0x07000000
    mov r2, #256
    bl fill_words
    ldr r1, =0x07000000
    ldr r0, =0x0010
    strh r0, [r1]             @ sprite 0: Y=16, square, 4bpp
    ldr r0, =0xC000
    strh r0, [r1, #2]         @ 64x64, X=0
    mov r0, #0
    strh r0, [r1, #4]
""" + enable_hblank_irq() + set_reg(0x00, 0x1040),
    irq_body=r"""
    mov r2, r1, lsl #24
    mov r2, r2, lsr #23       @ X = ((L+1) & 0xFF) * 2
    orr r2, r2, #0xC000
    mov r3, #0x07000000
    strh r2, [r3, #2]
""")

# ---------------------------------------------------------------- (d) mode 4
M4_SETUP = r"""
    ldr r0, =pal_data
    ldr r1, =0x05000000
    mov r2, #128
    bl copy_words
""" + set_reg(0x0C, 0x0000)
M4_DATA = lambda: "pal_data:\n" + halfs(pal256()) + "\n"

# Page flip in VBlank, then fill the page that is NOT displayed.
CASES["m4_back"] = rom(
    init=M4_SETUP + set_reg(0x00, 0x0404) + "    mov r5, #1\n",
    vblank=r"""
    ldrh r0, [r4]
    eor r0, r0, #0x10
    strh r0, [r4]             @ flip: show the page drawn last frame
    tst r0, #0x10
    ldreq r1, =0x0600A000     @ showing page 0 -> draw page 1
    ldrne r1, =0x06000000
    add r5, r5, #1
    and r5, r5, #0xFF
    orr r0, r5, r5, lsl #8
    orr r0, r0, r0, lsl #16
    ldr r2, =9600
    bl fill_words
""", data=M4_DATA())

# Single-buffered: redraw the DISPLAYED page with no VBlank sync at all.
CASES["m4_front"] = rom(
    init=M4_SETUP + set_reg(0x00, 0x0404) + "    mov r5, #1\n",
    loop=r"""
    add r5, r5, #7
    and r5, r5, #0xFF
    orr r0, r5, r5, lsl #8
    orr r0, r0, r0, lsl #16
    ldr r1, =0x06000000
    ldr r2, =9600
    bl fill_words
    b main_loop
""", data=M4_DATA())

# Page 1 shown from line 80 on (VCOUNT-match IRQ), page 0 restored in VBlank.
CASES["m4_flipmid"] = rom(
    init=M4_SETUP + r"""
    ldr r0, =0x21212121
    ldr r1, =0x06000000
    ldr r2, =9600
    bl fill_words
    ldr r0, =0x77777777
    ldr r1, =0x0600A000
    ldr r2, =9600
    bl fill_words
    ldr r1, =0x04000200
    mov r0, #4                @ IE = VCOUNT match
    strh r0, [r1]
    mov r0, #1
    strh r0, [r1, #8]
    ldr r0, =0x5020           @ DISPSTAT: VCOUNT IRQ, LYC = 80
    strh r0, [r4, #4]
""" + set_reg(0x00, 0x0404),
    vblank=r"""
    ldrh r0, [r4]
    bic r0, r0, #0x10
    strh r0, [r4]
""",
    irq_body=r"""
    ldrh r2, [r0]
    orr r2, r2, #0x10
    strh r2, [r0]
""", data=M4_DATA())


# ---------------------------------------------------------------- SA2 title
# Sonic Advance 2's title-screen sea, as the SAT-R/sa2 decomp writes it
# (src/game/sa2/title_screen.c WavesBackgroundAnim, src/core.c VBlank):
#  * mode 1, BG2 = 256x256 affine, wrap OFF, inside WIN1 (WINOUT = 0x13);
#  * the whole geometry is per line: in VBlank DMA3 copies table entry 0
#    (PA, PB, PC, PD, X, Y = 8 halfwords) to BG2PA, then DMA0 is armed
#    16-bit / HBlank / repeat / dest-reload, count 8, from entry 1;
#  * PC = PD = 0 and PB = 0, so NOTHING steps the reference between lines:
#    every line's position comes from its reload;
#  * lines above the horizon point BG2 off the map (Y = line + 512 - top);
#  * a palette gradient (bank 14) is written by the BIOS CpuFastSet from the
#    HBlank IRQ, and the base bank is restored in VBlank (ResetWavesPalette).
# The backdrop is white here so the failure is visible; SA2's own backdrop
# colour was not checked (no ROM was available).
SA2_TOP = 64
sa2 = []
for i in range(160):
    if i >= SA2_TOP:
        k = i - SA2_TOP
        pa = 0x180 - k * 2                  # far rows squeezed, near rows 1:1
        x = -(((240 << 8) - 240 * pa) >> 1)
        y = k * pa + (0x20 << 8)
        sa2 += [pa, 0, 0, 0, x & 0xFFFF, (x >> 16) & 0x0FFF, y & 0xFFFF, (y >> 16) & 0x0FFF]
    else:
        y = (i + 512 - SA2_TOP) << 8
        sa2 += [0, 0, 0, 0, 0, 0, y & 0xFFFF, (y >> 16) & 0x0FFF]
sa2_grad = []
for b in range(16):
    sa2_grad += [(((b + c) & 15) << 11) | ((c * 2) << 5) | 4 for c in range(16)]
sa2_base = [0x7C00 | (c << 5) for c in range(16)]
CASES["sa2_waves"] = rom(
    init=AFFINE_SETUP + r"""
    ldr r0, =0x7FFF
    ldr r1, =0x05000000
    strh r0, [r1]             @ backdrop: white
""" + set_reg(0x0C, 0x4000 | (8 << 8)) +          # BG2CNT: 256x256, no wrap
    set_reg(0x42, 0x00F0) + set_reg(0x46, ((SA2_TOP - 2) << 8) | 160) +
    set_reg(0x48, 0x3F00) + set_reg(0x4A, 0x0013) +
    enable_hblank_irq() + set_reg(0x00, 0x4401),   # mode 1, BG2, WIN1
    vblank=r"""
    ldr r0, =sa2_base
    ldr r1, =0x050001C0
    mov r2, #8
    bl copy_words             @ ResetWavesPalette
    ldr r1, =0x040000D4
    ldr r0, =sa2_tab
    str r0, [r1]
    ldr r0, =0x04000020
    str r0, [r1, #4]
    mov r0, #8
    strh r0, [r1, #8]
    ldr r0, =0x8000
    strh r0, [r1, #10]        @ DMA3 now: entry 0 -> BG2PA..BG2Y_H
""" + dma0_arm("sa2_tab+16", 0x04000020, 8, 0xA260),
    irq_body=r"""
    cmp r1, #%d
    blt 9f
    sub r2, r1, #%d
    and r2, r2, #15
    ldr r0, =sa2_grad
    add r0, r0, r2, lsl #5
    ldr r1, =0x050001C0
    mov r2, #8
    swi 0x0C0000              @ CpuFastSet, as SA2's CpuFastCopy
9:  mov r0, #0x04000000
""" % (SA2_TOP, SA2_TOP),
    data=AFFINE_DATA() + "sa2_tab:\n" + halfs(sa2) + "\n    .align 2\nsa2_grad:\n" +
    halfs(sa2_grad) + "\n    .align 2\nsa2_base:\n" + halfs(sa2_base) + "\n")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    for name, src in CASES.items():
        with open(os.path.join(out, name + ".s"), "w") as fh:
            fh.write(src)
    print(" ".join(CASES))


if __name__ == "__main__":
    main()
