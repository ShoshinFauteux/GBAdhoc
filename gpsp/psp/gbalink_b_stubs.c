/* gbalink_b_stubs.c -- what instance B (the headless link partner) links
 * in place of the core's renderer and cheat engine (psp/Makefile,
 * gbalink_core_b.o; docs/GBA-LINK-MIRROR.md section 19).
 *
 * B never draws: every frame it runs, lk_frame_begin (gbalink_core.c) sets
 * skip_next_frame = 1, so the real update_scanline returns before its first
 * side effect, B's ME capture is never armed (me_capture_mode stays 0, so
 * me_capture_visible_end/resume return at their first test), and its
 * picture goes to b_video (fe_gbadual.c), which drops it.  The affine
 * reference latches are renderer state the guest cannot read back.  So
 * these empty bodies are what B already executed, without B's own copy of
 * video.o (~193 KB of code, ~106 KB of .bss) and gba_cc_lut.o (64 KiB).
 *
 * B never has a cheat: nothing calls gbab_retro_cheat_set, so B's
 * cheats_active stays 0 and cheats_apply_frame is never reached; the
 * translator compares PCs with cheat_master_hook, which must keep the value
 * cheats.c starts it at (~0: no hook).
 *
 * Everything here is renamed gbab_* with the rest of B.  The psp/Makefile
 * rule checks that B is left with no unresolved reference to any symbol of
 * the members it drops, except open_gba_bios_rom: the built-in BIOS image
 * is read-only (libretro.c only memcpys it), so B reads instance A's copy.
 *
 * Types: the core's own headers declare every name defined here, so a
 * signature or type change there breaks this file's build instead of
 * linking a mismatched stub (critique-4 SF3).  psptypes.h gives the u16 /
 * u32 / s32 they assume (as main_psp.c includes video.h); gba_cc_lut.h is
 * not included because it pulls the whole core through common.h, so its
 * one declaration is repeated below, word for word. */
#include <psptypes.h>
#include "video.h"
#include "cheats.h"

/* ---- video.o ------------------------------------------------------------ */
s32  affine_reference_x[2];
s32  affine_reference_y[2];
u16 *gba_screen_pixels;                /* libretro.c mallocs B's own frame */

/* Read by libretro.c's colour-correction post-process only, which B never
 * runs: fe_host's gpsp_color_correction is "disabled" and refused on PSP
 * (USE_PSP_RGB565_FORMAT, fe_host_option_set_live), and b_env forwards that
 * answer.  One entry, so a stray reach shows as a wrong colour on a picture
 * nobody sees rather than a 64 KiB table. */
extern const u16 gba_cc_lut[];         /* gba_cc_lut.h, verbatim */
const u16 gba_cc_lut[1];

void update_scanline(void) { }
/* Layout: B's renderer was 7,712 B of text in the pinned hot order
 * (psp/layout/hot.ord: update_scanline sits between B's update_gba group and
 * its block lookups).  This stub is 8 B, so everything after it -- the rest
 * of B's hot text, then main (the first frontend function) and its frame
 * loop -- moved 7,712 B in claude/opt-1000 round 4, and solo Doom II lost
 * 1.2-1.7 % on hardware (hw-batch-3).  The section keeps the renderer's size
 * (zeros plus the 8 B stub, never executed; same trick as layout_pad_apb.S)
 * so they stay where round 3 and beta.4 had them mod 8 KiB.
 * -ffunction-sections puts the stub in this section. */
__asm__(".pushsection .text.update_scanline,\"ax\",@progbits\n"
        "  .fill 7704 / 4, 4, 0\n"
        ".popsection\n");
void video_reload_counters(void) { }
void me_capture_visible_end(void) { }
void me_capture_visible_resume(void) { }

/* ---- cheats.o ----------------------------------------------------------- */
u32 cheat_master_hook = 0xffffffffu;
u32 cheats_active;
u32 cheats_suspended;

void process_cheats(void) { }
void cheats_apply_frame(void) { }
void cheat_clear(void) { }
void cheat_disable(unsigned index) { (void)index; }
cheat_error cheat_parse(unsigned index, const char *code)
{
   (void)index; (void)code;
   return CheatErrorNotSupported;
}

/* ---- layout ------------------------------------------------------------- */
/* B's block_lookup_translate_thumb became one 32 B step smaller in
 * claude/opt-1000 round 7 (the constant hash shift).  The partial link
 * merges same-named sections in member order and this file is the last
 * member, so these zeros sit right after B's copy and keep B's hot text
 * after it where round 6 had it (A's copy is held by psp/layout_pad_warm.S,
 * in front of it). */
__asm__(".pushsection .text.block_lookup_translate_thumb,\"ax\",@progbits\n"
        "  .fill 32 / 4, 4, 0\n"
        ".popsection\n");
