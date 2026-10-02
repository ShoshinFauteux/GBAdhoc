/* home_host.h -- the EBOOT side of Settings > Controls > Menu = HOME
 * (docs/CONTROL-REMAP.md section 10).  The kernel side is psp/home/; the
 * contract between them is psp/home/home_link.h.
 *
 * Everything here is non-blocking and per frame, like me_host: the main loop
 * beats, says whether a game owns HOME this frame, and asks whether HOME was
 * pressed.  The module does the rest -- including giving HOME back to the
 * system by itself if the beat stops. */
#ifndef HOME_HOST_H
#define HOME_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Load <base_dir>/gbadhoc_home.prx and start it on our home_link.  Tried at
 * most once per process (a failure is remembered, never retried in a loop).
 * 0 = started; negative = unavailable (missing file, load refused, ...): the
 * menu then stays on START+SELECT, which always works. */
int  home_host_start(const char *base_dir);
int  home_host_tried(void);
/* Stop and unload.  The module switches the popup back on in module_stop. */
void home_host_stop(void);

/* Once per main-loop iteration: beat, and arm (a game owns HOME) or not. */
void home_host_frame(int armed);
/* HOME back to the system NOW (suspend, a modal screen, teardown). */
void home_host_disarm(void);

/* 1 while the module is polling and the popup switch was not refused: only
 * then may the menu move off START+SELECT. */
int  home_host_ok(void);
/* 1 once per HOME press the module counted while the popup was ours, and
 * only if it is fresh (pressed within the last ~1/4 s): a press that waited
 * out a blocked loop belongs to whatever the player was looking at then, not
 * to the menu now. */
int  home_host_take_press(void);
/* Diagnostics: times a stalled loop gave HOME back, last impose rc. */
unsigned home_host_fallbacks(void);
int  home_host_imp_rc(void);

#ifdef __cplusplus
}
#endif

#endif /* HOME_HOST_H */
