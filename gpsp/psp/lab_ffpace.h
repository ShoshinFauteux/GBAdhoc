/* lab_ffpace.h -- fast-forward PRESENTATION CADENCE instrument (Lab builds only).
 *
 * Compiled only with LAB_HARNESS=1 (tools/build.sh lab).  Without it the hook
 * macro below expands to nothing and not one line of lab_ffpace.c exists, so
 * release and harness builds are unchanged.
 *
 * What it measures is what the LCD shows, not what the loop thinks it did: a
 * small thread at priority 0x11 wakes on every vblank and reads the displayed
 * framebuffer address (sceDisplayGetFrameBuf IMMEDIATE).  Every flip is tagged
 * BEFORE its sceDisplaySetFrameBuf with the game frame its picture holds (the
 * ME stage's frame for the ME renderer, the frame count at the flip for the
 * CPU renderer) and a sequence number.  So per vblank the observer knows
 * whether a new picture latched, how many vblanks the previous one stayed up,
 * how many game frames the picture advanced, and how many submitted flips were
 * never displayed (overwritten before their vblank).
 *
 * Harness keys (both already known to the Lab, harness-ini-key-trap):
 *   lab_pace        0 off; 1 on; >= 100 on AND pin the clock to that many MHz
 *                   (cpu = pll = N, bus = N/2 -- 222 = 222/222/111) at boot
 *   lab_pace_every  window length in vblanks (default 300 = ~5 s)
 * Output: EVT ffpace_* lines (frontend.log, which the Lab keeps per task).
 */
#ifndef LAB_FFPACE_H
#define LAB_FFPACE_H
#ifdef LAB_HARNESS
/* Boot: read the keys from `ini`, start the observer.  Returns the clock to
 * pin (0 = leave it).  apply = 1: set it here (a tree without its own clock
 * setting); 0: the caller routes it through its own clock code. */
int  lab_ffpace_boot(const char *ini, int apply);
/* Every flip, BEFORE sceDisplaySetFrameBuf: physical address it submits. */
void lab_ffpace_flip(unsigned phys_addr);
/* ME renderer: a render of game frame `frame` was posted into stage `stage`;
 * stage `stage` was drawn into the back buffer (its frame is the picture). */
void lab_ffpace_me_post(int stage, unsigned frame);
void lab_ffpace_me_present(int stage);
/* ME renderer: a render retired.  post_us = sceKernelGetSystemTimeLow() at its
 * post, spin_us = how long the main thread spun waiting for it (0 if none). */
void lab_ffpace_me_retire(unsigned post_us, unsigned spin_us);
/* Once per main-loop iteration, at its end. */
void lab_ffpace_loop(int ff_user, int gate, int mult, int uncapped, int me_rend);
/* After the main loop: cumulative summary. */
void lab_ffpace_finish(void);
#define LAB_FFP(x) x
#else
#define LAB_FFP(x)
#endif
#endif
