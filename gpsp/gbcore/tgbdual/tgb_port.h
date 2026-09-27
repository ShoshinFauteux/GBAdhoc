/* Copyright (c) 2026 GBAdhoc contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * GBAdhoc port layer for the vendored TGB Dual core.  It stands in for the
 * MasterBoy headers the core used to include (../psp/pspcommon.h,
 * ../renderer.h, ../syscall.h, ../lib/zlib.h, <oslib/oslib.h>) and declares
 * the few adapter functions the core calls back into.  Everything declared
 * "adapter" here is defined in gbcore/gbcore_tgbdual.c.
 */
#ifndef TGB_PORT_H
#define TGB_PORT_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "gb_types.h"

/* Pixels leave the core already in the frontend's 16-bit layout, so the
 * adapter hands frames on without a conversion pass.  Libretro RGB565 is red
 * in the high bits; the PSP build (USE_PSP_RGB565_FORMAT, ADR-0039) wants the
 * GE's 5650 order, red in the low bits. */
#ifdef USE_PSP_RGB565_FORMAT
#define MAKE_COLOR(r, g, b) \
   ((word)((((b) >> 3) << 11) | (((g) >> 2) << 5) | ((r) >> 3)))
#else
#define MAKE_COLOR(r, g, b) \
   ((word)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#endif

/* Sound register-write queue length (apu.c).  Drained once per frame. */
#define SND_QUE_SIZE 0x4000
/* Output gain applied with the NR50 master volume in snd_render_orig.
 * Calibrated to the GBA path: a full-volume square channel with NR50=0x77
 * peaks at +/-8203 here, and gpSP renders the GBA's identical PSG at full
 * volume as +/-8192 (sound.c: 8 * 2^28 >> 18).  The SameBoy core this
 * replaced peaked at +/-4540 for the same tone, about 6 dB quieter than
 * GBA games.  Measured with a synthetic ROM (see LOCAL-PATCHES.md). */
#define TGB_SND_GAIN 2

/* --- state the core reads that MasterBoy's frontend used to own --------- */

/* Joypad, active high: 1 A, 2 B, 4 SELECT, 8 START, 16 DOWN, 32 UP,
 * 64 LEFT, 128 RIGHT (read by cpu_io_read_00).  Adapter. */
extern int pad_state;
/* DMG palette as BGR555: BG[4], OBJ0[4], OBJ1[4].  Adapter. */
extern word tgb_dmg_palette[12];

/* --- adapter callbacks --------------------------------------------------- */

void gbe_init(void);
void gbe_reset(void);
/* MBC3 clock registers 8..12 (seconds, minutes, hours, day low, day high
 * with halt bit 6 and carry bit 7), backed by the frontend's wall clock. */
byte gbe_getTime(int type);
void gbe_setTime(int type, byte dat);
word gbe_getSensor(char x_y);
void gbe_setBibrate(char bibrate);
/* Chooses DMG / SGB / CGB behaviour for the loaded cartridge (gb_reset). */
void set_gb_type(void);
/* A serial transfer started with `outgoing` in SB.  Returns 1 and fills
 * *received when a peer exchanged a byte, 0 when there is no cable. */
int tgb_port_serial(byte outgoing, int internal_clock, byte *received);

/* --- core functions without a prototype in gb.h -------------------------- */

void sgb_invalidate_color(int color);
void sgb_set_color(int color, word value);
void sgb_render_border(void);
void gb_invalidate_palette(int palNo);
size_t gb_save_state(byte *buf);

/* --- core internals the adapter reaches ---------------------------------- */

extern int total_clock, rest_clock, sys_clock, seri_occer, div_clock;
extern int gdma_rest, last_int;
extern volatile char int_disable_next, int_invoke_next;
extern byte seri_rx;
extern byte stack[], spare_oam[], ext_mem[];
extern byte _ff6c, _ff72, _ff73, _ff74, _ff75;
extern int now_win_line, re_render, gbSkip, snd_que_count, snd_bef_clock;
extern int snd_rate;
extern unsigned char snd_stat_upd;
extern struct apu_que *snd_write_que;
extern byte mbc3_latch, mbc3_sec, mbc3_min, mbc3_hour, mbc3_dayl, mbc3_dayh;
extern byte mbc3_timer;
extern int sgb_mask;

#endif
