/* layout_pad_mid.S -- two cold, never-called pads inside the frontend's text,
 * part of the shipped layout like layout_pad_cold (docs/LAYOUT-PINNING.md,
 * "Keeping the pads").  psp/Makefile builds this file twice:
 *
 *   layout_pad_fe.o    linked after stb_impl.o, so frontend-common (fe_host's
 *                      per-frame rewind and input code, netdrv, the link
 *                      glue) keeps its cache sets when the browser and its
 *                      decoders before it change size;
 *   layout_pad_core.o  linked last of the objects, so instance A's cold core
 *                      text and its tables keep theirs when anything in the
 *                      frontend or instance B's cold text changes size.
 *
 * With layout_pad_cold (after main) these are three degrees of freedom for
 * three regions whose relative sizes change independently.  Zeros; nothing
 * calls them.  LAYOUT_PAD_MID_BYTES = 0 assembles to nothing. */
#ifndef LAYOUT_PAD_MID_BYTES
#define LAYOUT_PAD_MID_BYTES 0
#endif
#ifndef LAYOUT_PAD_MID_NAME
#define LAYOUT_PAD_MID_NAME layout_pad_mid
#endif
	.text
	.balign 4
	.globl LAYOUT_PAD_MID_NAME
	.type LAYOUT_PAD_MID_NAME, @function
LAYOUT_PAD_MID_NAME:
	.space LAYOUT_PAD_MID_BYTES
	.size LAYOUT_PAD_MID_NAME, . - LAYOUT_PAD_MID_NAME
