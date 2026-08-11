/* me_render_data.c — ME-local storage for the render inputs video.cc reads as
 * externs.  The renderer object (video.o) references these by name with C
 * linkage; the linker matches by name+size, so plain types suffice.  These are
 * the ME's working copy of the frame snapshot (host DMAs into them each frame).
 * Sizes mirror the core: cpu.cc / gba_memory.h. */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

u16 io_registers[512];
u16 oam_ram[512];
u16 palette_ram_converted[512];
u8  vram[1024 * 96];
u32 reg[64];
int sprite_limit;
u32 skip_next_frame;
