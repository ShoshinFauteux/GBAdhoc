/* font_8x16.h -- the 8x16 character cell the OSD and UI lay text out on.
 *
 * The VGA 8x16 bitmap font (Linux kernel, GPL-2.0) that this header used to
 * declare, and the 64 KiB GE texture built from it at boot, are gone: the
 * Inter atlases (video_psp.c fu_ui_a / fu_hd_a) draw every glyph, and nothing
 * had sampled that texture since they replaced it.  Only the cell metrics
 * remain, for the layout code. */
#ifndef FONT_8X16_H
#define FONT_8X16_H

#define FE_FONT_W 8
#define FE_FONT_H 16

#endif /* FONT_8X16_H */
