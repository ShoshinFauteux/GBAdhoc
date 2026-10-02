/* ambient_look.h -- THE LOOK of the ambient bars and the loading screen, in
 * one place (docs/DISPLAY-FEATURES.md).
 *
 * Everything a designer would tune is a number here; the mechanism in
 * video_psp.c (vid_ambient_*) and ui_psp.c (the loading screen) reads only
 * these.  Changing the look is an edit to this file and a rebuild.
 *
 * Defaults: Fable's chosen "soft" ambient and "L2" loading screen
 * (builds/menu-mockups/README.md, ambient/soft-*.png, loading/L2-*.png).
 * The alternatives it drew are one or two numbers away -- "deep" is
 * AMB_BLUR_RADIUS 2 with AMB_SCRIM_HERO 150. */
#ifndef AMBIENT_LOOK_H
#define AMBIENT_LOOK_H

/* ---- ambient texture ----------------------------------------------------
 * The art (or the snapshot) is cover-cropped to the screen's 480:272, box-
 * averaged down to AMB_W x AMB_H (a 480x272 hero is exactly 4x4 per texel),
 * blurred, and stored ONCE in VRAM.  The GE magnifies it 4x with bilinear
 * filtering when it draws the bars.  AMB_W must be a multiple of 4. */
#define AMB_W              120
#define AMB_H              68

/* Box blur after the downscale: radius in texels (1 texel = 4 screen px)
 * and passes.  "soft" = one 3x3 pass. */
#define AMB_BLUR_RADIUS    1
#define AMB_BLUR_PASSES    1

/* Optional bake-time grade, 1/256ths; the defaults change nothing.
 *   saturation  256 = as is, 0 = grey
 *   tint        a colour mixed in by AMB_TINT_AMOUNT                      */
#define AMB_SATURATION     256
#define AMB_TINT_R         0
#define AMB_TINT_G         0
#define AMB_TINT_B         0
#define AMB_TINT_AMOUNT    0

/* ---- the scrim over the bars ---------------------------------------------
 * A black scrim of this alpha (0-255), by where the picture came from: art
 * from hero/ is composed dark already; a cover from boxart/ is not; a game
 * frame sits in between.  Applied as the GE's texture MODULATE colour
 * (255 - alpha), which is the same arithmetic as a black alpha rect drawn
 * on top -- without a second, blended fill of every bar pixel. */
#define AMB_SCRIM_HERO     96
#define AMB_SCRIM_BOXART   150
#define AMB_SCRIM_FRAME    130

/* ---- no art: the palette fallback -----------------------------------------
 * The console's darkest palette shade (its browser skin's pal[0]) under a
 * vertical black ramp, top alpha -> bottom alpha.  No texture.  Interpolated
 * by the GE as vertex colours (one strip per bar, no blending). */
#define AMB_PAL_RAMP_TOP   60
#define AMB_PAL_RAMP_BOT   200

/* ---- snapshot fallback (ambient = "art, else game") ----------------------
 * With no art, the bars come from a game frame instead: the first frame at
 * or after AMB_SNAP_AFTER whose mean luma reaches AMB_SNAP_MIN_LUMA (0-255)
 * -- the BIOS logo and black title cards are skipped -- retried every
 * AMB_SNAP_RETRY frames, giving up (palette) at AMB_SNAP_GIVE_UP.  Counted
 * in presented frames.  Re-taken when the in-game menu closes (a state load
 * or a new area), never per frame. */
#define AMB_SNAP_AFTER     180
#define AMB_SNAP_RETRY     60
#define AMB_SNAP_GIVE_UP   3600
#define AMB_SNAP_MIN_LUMA  24

/* ---- loading screen ("L2") ------------------------------------------------
 * The same ambient bake drawn full-screen under a C_BG_TOP scrim, the
 * browser's marquee chrome on top. */
#define LOAD_SCRIM         110
#define LOAD_FOOT_RAMP     190     /* vid_gradient_a(0,232,480,40, .., 0, x) */
/* No art: the console's pal[0] as a vertical ramp, alpha top -> 0. */
#define LOAD_PAL_RAMP_DARK  170
#define LOAD_PAL_RAMP_LIGHT 70

/* ---- Settings from the ROM browser (docs/UI-OVERLAY.md) -----------------
 * The selected game's bake under the Overlay's own scrim (OV_SCRIM_DENSE);
 * with no art, the LOAD_PAL_RAMP_* ramp above.  A Shelf cover comes from the
 * 128-px box-art cache, nearly 1:1 with the texture, so it gets these extra
 * box passes on top of the "soft" one. */
#define AMB_COVER_BLUR_RADIUS 2
#define AMB_COVER_BLUR_PASSES 2

#endif /* AMBIENT_LOOK_H */
