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
   UI_ACT_EXIT,            /* exit-with-flush */
   UI_ACT_GAMELIST,        /* relaunch back to the ROM browser */
   /* ADR-0071: the trading profile changed.  The main loop saves config and
    * RELAUNCHES the EBOOT — the session rate is latched at startup, so there
    * is no honest way to apply it live. */
   UI_ACT_RELAUNCH
} ui_action;

void ui_open(void);
void ui_close(void);
int  ui_active(void);

/* One frame of menu UI: input edges + draw (vid_overlay_begin(1)..end).
 * pad = raw SceCtrl button mask; session_active gates savestates + shows
 * the wireless status screen; session_info is the status line (or NULL). */
ui_action ui_frame(unsigned pad, int session_active, const char *session_info);

/* Group selected by the last Host/Join action (scan pick or room code). */
const char *ui_group(void);

/* Blocking pre-game ROM browser (generic build with no baked ROM and no
 * harness). Fills out with the full ROM path. Returns 0 on pick, -1 on
 * exit request / empty dir. */
int ui_browser(const char *rom_dir, char *out, size_t out_sz);

/* Harness self-drive (.gpsp-harness.ini ui_demo=1): scripted walk through
 * menu -> settings (cycle scale) -> wireless -> resume, with EVT markers
 * (ui_open/ui_screen/ui_demo_done) and a GE dump of the menu screen. */
void ui_demo_start(void);
int  ui_demo_running(void);
void ui_demo_shots(void);          /* arm the gallery dump + auto-pick     */
void ui_set_theme_black(int b);    /* runtime page theme (-1 = leave)      */

#ifdef __cplusplus
}
#endif

#endif /* UI_PSP_H */
