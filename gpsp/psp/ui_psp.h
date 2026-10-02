/* ui_psp.h — native UI v1 (plan §8, Phase 2): ROM browser, in-game menu
 * (Select+Start hold — chord detected by the main loop), wireless panel
 * (Host / Join via scan or room code), settings screens.
 *
 * The main loop owns the core; the UI returns actions for it to execute.
 * While ui_active(), the core is paused (no retro_run) but the wireless
 * pump keeps running.  All drawing goes through video_psp overlay
 * primitives; one ui_frame() call per displayed frame.
 */
#ifndef UI_PSP_H
#define UI_PSP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "fe_console.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
   UI_ACT_NONE = 0,
   UI_ACT_RESUME,          /* close the menu */
   UI_ACT_SAVESTATE,
   UI_ACT_LOADSTATE,
   UI_ACT_NET_HOST,        /* host on ui_group() */
   UI_ACT_NET_JOIN,        /* join ui_group() */
   UI_ACT_NET_DISCONNECT,
   /* Mystery Gift (2.0.5) is deliberately NOT a host/join variant.  It is a
    * different radio mode -- infrastructure Wi-Fi to the phone's hotspot, not
    * ad-hoc IBSS -- so it cannot share the session state machine, and the two
    * are interlocked as mutually exclusive.  See psp/mgift_net.h. */
   UI_ACT_NET_MGIFT,       /* start listening for a Mystery Gift Station */
   UI_ACT_NET_MGIFT_STOP,  /* stop listening and drop the association */
   UI_ACT_EXIT,            /* exit-with-flush */
   UI_ACT_GAMELIST,        /* relaunch back to the ROM browser */
   /* ADR-0071: the trading profile changed.  The main loop saves config and
    * RELAUNCHES the EBOOT — the session rate is latched at startup, so there
    * is no honest way to apply it live. */
   UI_ACT_RELAUNCH
} ui_action;

/* The in-game menu draws over the game (docs/UI-OVERLAY.md): the frame it
 * sits on -- a w x h PSP-5650 picture at the top-left of a 256x256 texture
 * (main_psp.c's wake snapshot), or NULL for a flat page.  Set before
 * ui_open(); the pointer is only read while the menu is up. */
void ui_set_backdrop(const uint16_t *frame, int w, int h);
/* Five 64x64 PSP-5650 textures the state-slot screen loads the previews
 * into (main_psp.c carves them from the netdrv arena's tail), or NULL. */
void ui_set_thumb_buffer(uint16_t *buf);
/* Harness `ui_backdrop = <file>`: a raw 480x272 PSP-5650 picture drawn 1:1
 * behind the menu instead of the game, for the pixel comparison with the
 * design model.  Allocates 512 KiB; call before the core loads a ROM. */
void ui_backdrop_inject(const char *rel);
void ui_open(void);
void ui_close(void);
int  ui_active(void);
/* Menu = HOME (docs/CONTROL-REMAP.md section 10): a HOME press while the
 * in-game menu is open.  On the menu's top page it closes the menu, exactly
 * as Resume does; on any other page it is ignored (a sub-page may be in the
 * middle of something -- a capture, a scan -- and O already walks back).
 * Returns 1 when the menu closed. */
int  ui_home_press(void);
/* 1 while Settings > Controls is waiting for the player to press the input
 * for a binding.  The main loop keeps its own always-on chords (the
 * screenshot) quiet meanwhile: the player is pressing them to TEACH them. */
int  ui_capturing(void);

/* Boot-only, main-thread presentation. No worker and no core calls.  The
 * only allocation is a launch without a browser pick (harness, variant):
 * ui_loading_begin then decodes the hero art by file name and FREES it
 * before returning -- i.e. before fe_host_boot (docs/DISPLAY-FEATURES.md). */
void ui_loading_begin(const char *path);
void ui_loading_update(const char *stage, unsigned done, unsigned total);
void ui_loading_finish(int success);
/* Harness `loading_dump = N`: GE-dump the first N loading-screen redraws to
 * log/ge_loading_<n>.bmp (docs/DISPLAY-FEATURES.md screenshots). */
void ui_loading_dump_shots(int n);
/* The console's darkest palette shade (its browser skin's first stripe
 * colour), libretro RGB565 -- the ambient bars' no-art fallback. */
uint16_t ui_console_shade(int console);

/* One frame of menu UI: input edges + draw (vid_overlay_begin(1)..end).
 * pad = raw SceCtrl button mask; session_active gates savestates + shows
 * the wireless status screen; session_info is the status line (or NULL). */
ui_action ui_frame(unsigned pad, int session_active, const char *session_info);
void ui_set_state_base(const char *slot1_path);
int ui_state_slot(void); /* selected slot, 1..5 */

/* Group selected by the last Host/Join action (scan pick or room code). */
const char *ui_group(void);

/* ---- Mystery Gift status, for the screen that shows it -------------------
 * Implemented in main_psp.c, where the rest of the wireless state lives (the
 * same arrangement as osd_session_chip_refresh below).  The frontend hands
 * the UI finished strings rather than the session struct, so ui_psp.c needs
 * no knowledge of the wire protocol. */
int         mgift_ui_active(void);    /* 1 while the listener is up */
/* Which stored network profile the player picked, 1-based (0 = first that
 * exists).  Read by the frontend when it associates. */
int         ui_mgift_conf(void);
const char *mgift_ui_line1(void);     /* what is happening */
const char *mgift_ui_line2(void);     /* the gift / progress, or "" */

/* Blocking pre-game ROM browser (generic build with no baked ROM and no
 * harness). Fills out with the full ROM path. Returns 0 on pick, -1 on
 * exit request / empty dir, and 1 when START->SETTINGS changed something
 * only read at boot (Media Engine mode) and the app must relaunch. */
int ui_browser(const char *rom_dir, char *out, size_t out_sz,
               int *out_state_slot, fe_console_t *console_out);

/* Pure extension predicate shared by the browser and its host test.  The
 * console selection is explicit: .gbc files are not folded into GB mode. */
static inline int ui_rom_matches_console(const char *name, fe_console_t console)
{
   const char *ext;
   size_t len, i;
   static const char *const suffix[] = { ".gba", ".gb", ".gbc" };
   if (!name || console < FE_CONSOLE_GBA || console > FE_CONSOLE_GBC)
      return 0;
   len = strlen(name);
   ext = suffix[console];
   if (len < strlen(ext))
      return 0;
   name += len - strlen(ext);
   for (i = 0; ext[i]; i++)
   {
      unsigned char a = (unsigned char)name[i];
      unsigned char b = (unsigned char)ext[i];
      if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
      if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
      if (a != b) return 0;
   }
   return 1;
}

static inline size_t ui_rom_stem_length(const char *name)
{
   const char *dot, *slash;
   if (!name) return 0;
   dot = strrchr(name, '.');
   slash = strrchr(name, '/');
   return dot && (!slash || dot > slash) ? (size_t)(dot - name) : strlen(name);
}

/* Wake-from-sleep overlay: the frozen frame with a plate over it.
 * `frame` may be NULL (nothing was captured), in which case the plate sits
 * on the theme background.  Returns 0 to resume, 1 for the game list. */
int ui_wake_menu(const uint16_t *frame, int frame_w, int frame_h,
                 const char *game);
/* Harness `wake_shot = N`: the next wake overlay dumps log/ge_wake.bmp on
 * its third frame and continues on its own. */
void ui_wake_shot_arm(void);

/* Harness self-drive (.gpsp-harness.ini ui_demo=1): scripted walk through
 * menu -> settings (cycle scale) -> wireless -> resume, with EVT markers
 * (ui_open/ui_screen/ui_demo_done) and a GE dump of the menu screen. */
void ui_demo_start(void);
/* .gpsp-harness.ini ui_controls_demo=1: the same self-drive, through
 * Settings > Controls -- unbind, capture, a steal, a chord, reset -- with GE
 * dumps log/ge_ctl_*.bmp (docs/CONTROL-REMAP.md). */
void ui_controls_demo_start(void);
/* ui_controls_demo=2 (with browser=1): the browser opens START settings,
 * walks to Controls (log/ge_ctlb_*.bmp), backs out and boots the game. */
void ui_controls_browser_demo(void);
/* ui_controls_demo=3: Settings > Controls > Menu, flipped once (ge_cth_*);
 * =4: save slot 2, then delete it from the Load page -- the question, a
 * "keep", the delete (ge_del_*).  docs/CONTROL-REMAP.md sections 10-11. */
void ui_home_demo_start(int which);
int  ui_demo_running(void);
void ui_demo_shots(void);          /* arm the gallery dump + auto-pick     */
/* ui_browser_demo=1 (with browser=1): the browser walks its 3.0 states --
 * star, favourites, empty favourites, console flare, empty console -- and
 * GE-dumps each to log/ge_gallery_*.bmp, then exits. */
void ui_browser_demo_shots(void);
/* ui_browser_script = <file under the app dir>: a text script of presses,
 * dumps and per-frame dump ranges for the README shoot (see ui_psp.c). */
void ui_browser_script_load(const char *rel);
void ui_set_theme_black(int b);    /* runtime page theme (-1 = leave)      */

#ifdef __cplusplus
}
#endif

#endif /* UI_PSP_H */
