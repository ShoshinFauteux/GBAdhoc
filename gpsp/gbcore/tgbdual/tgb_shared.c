/* Copyright (c) 2026 GBAdhoc contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Read-only tables of the vendored core that every instance shares
 * (_mrand_table.h, _cgb_lcd_table.h, _cgb_dmg_pal.h).  Linked once, beside the
 * per-instance partial links, so the second Game Boy of a link session does
 * not duplicate them.
 */
#define TGB_SHARED_TABLES
#include "_mrand_table.h"
#include "_cgb_lcd_table.h"
#include "_cgb_dmg_pal.h"

/* gbcore_set_color_correction: whether CGB colours go through the Game Boy
 * Color LCD model (lcd.c).  A display preference, not machine state, so it
 * lives here, outside the per-instance image: gbcore_power_on does not
 * reset it and both machines of a link session show the same colours. */
int tgbshared_cgb_lcd = 1;

/* ------------------------------------------- GBC colours for DMG games --
 * The selection the Game Boy Color boot ROM makes (_cgb_dmg_pal.h). */
unsigned tgbshared_cgbpal_title_sum(const unsigned char *hdr)
{
	unsigned i, sum = 0;
	for (i = 0; i < 16; i++)
		sum += hdr[i];
	return sum & 0xff;
}

unsigned tgbshared_cgbpal_auto(const unsigned char *hdr)
{
	unsigned i, sum;
	/* 0x14B old licensee; 0x33 means the new code at 0x144-0x145. */
	unsigned char old = hdr[0x14B - 0x134];
	if (old == 0x33)
	{
		if (hdr[0x144 - 0x134] != '0' || hdr[0x145 - 0x134] != '1')
			return 0;
	}
	else if (old != 0x01)
		return 0;
	sum = tgbshared_cgbpal_title_sum(hdr);
	for (i = 0; i < CGBPAL_SUMS; i++)
	{
		if (cgbpal_sums[i] != sum)
			continue;
		if (i < CGBPAL_UNIQUE ||
		    hdr[3] == (unsigned char)cgbpal_letter[i - CGBPAL_UNIQUE])
			return cgbpal_pick[i];
	}
	return 0;
}

unsigned tgbshared_cgbpal_manual(unsigned n)
{
	return n < TGB_CGBPAL_MANUAL_COUNT ? cgbpal_manual[n] : 0;
}

void tgbshared_cgbpal_colors(unsigned combo, unsigned short out[12])
{
	unsigned i;
	const unsigned char *c = cgbpal_combos[combo < TGB_CGBPAL_COMBOS ? combo : 0];
	for (i = 0; i < 4; i++)
	{
		out[i]     = cgbpal_colors[c[2] + i];   /* BG */
		out[4 + i] = cgbpal_colors[c[0] + i];   /* OBJ0 */
		out[8 + i] = cgbpal_colors[c[1] + i];   /* OBJ1 */
	}
}
