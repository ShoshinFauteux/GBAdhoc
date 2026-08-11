/* font_8x16.h — classic VGA 8x16 bitmap font (Linux kernel, GPL-2.0).
 * 256 glyphs, 16 bytes each; one byte per row, MSB = leftmost pixel. */
#ifndef FONT_8X16_H
#define FONT_8X16_H

#define FE_FONT_W 8
#define FE_FONT_H 16

extern const unsigned char fe_font8x16[256 * 16];

#endif /* FONT_8X16_H */
