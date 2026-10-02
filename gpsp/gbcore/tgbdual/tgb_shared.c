/* Copyright (c) 2026 GBAdhoc contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Read-only tables of the vendored core that every instance shares
 * (_mrand_table.h, _cgb_lcd_table.h).  Linked once, beside the
 * per-instance partial links, so the second Game Boy of a link session does
 * not duplicate them.
 */
#define TGB_SHARED_TABLES
#include "_mrand_table.h"
#include "_cgb_lcd_table.h"

/* gbcore_set_color_correction: whether CGB colours go through the Game Boy
 * Color LCD model (lcd.c).  A display preference, not machine state, so it
 * lives here, outside the per-instance image: gbcore_power_on does not
 * reset it and both machines of a link session show the same colours. */
int tgbshared_cgb_lcd = 1;
