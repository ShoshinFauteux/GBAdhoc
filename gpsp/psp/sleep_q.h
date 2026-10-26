/* sleep_q.h -- the "quiesce for sleep" sequence.  Pure C, no PSP headers, so
 * the host test runs the same ordering the EBOOT does.
 *
 * WHY.  A PSP-3001 (ARK-4 6.61) powered off instead of suspending when the
 * switch was slid IN GAME; from the ROM list it slept and woke fine, and
 * Snes9xTYLme sleeps fine on the same console.  The ME is not the cause
 * (me_boot = 0 fails identically).  What is live only in game is: the
 * emulation loop itself, the .sav writer thread, the held-open .sav and
 * paged-ROM handles, a sound stream carrying real samples, the HOME module
 * when Menu = HOME, and (in a session) the radio.  TYL's lifecycle is: pause
 * the game, stop AND JOIN its sound thread, kill the ME, let the deferred
 * suspend proceed while the main thread sleeps, and restart sound after the
 * wake.  This sequence brings GBAdhoc to the same state.
 *
 * Every step runs on the EMULATION thread at the loop's park point (between
 * frames: nothing is being emulated, nothing reads the ROM file, the core is
 * not mid-write into SRAM).  The power callback only waits for it, bounded.
 *
 * Down order mirrors TYL's before_pause: sound first, then the writer (which
 * lands the .sav), then the files (only once nobody else can own them), then
 * the HOME module and the radio.  Up is the reverse, sound last (TYL's
 * after_pause ends with InitSoundThread).  Only what a down step actually
 * stopped is restarted. */
#ifndef SLEEP_Q_H
#define SLEEP_Q_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Step return codes, shared by every op:
 *   0   the thing was live and is now stopped / restarted
 *   1   nothing to do (was not running / not loaded / not open)
 *  <0   failure (the kernel's rc where there is one) */
#define SQ_DONE  0
#define SQ_NONE  1

typedef struct sleep_q_ops
{
   void *ctx;
   int (*audio_stop)(void *ctx);
   int (*audio_start)(void *ctx);
   int (*writer_stop)(void *ctx);
   int (*writer_start)(void *ctx);
   /* Called after writer_stop, so no other thread can be using the .sav or
    * event-log handles.  Returns how many handles were closed; `detail`
    * gets e.g. "sav=1 rom=1 evt=0". */
   int (*files_close)(void *ctx, char *detail, size_t n);
   int (*files_reopen)(void *ctx, char *detail, size_t n);
   int (*home_stop)(void *ctx);
   int (*home_rearm)(void *ctx);
   int (*net_term)(void *ctx);
   /* One finished line, e.g. "q audio_stop rc=0".  The PSP side writes it
    * with standby_note. */
   void (*note)(void *ctx, const char *line);
} sleep_q_ops;

enum
{
   SQ_STEP_AUDIO  = 1u << 0,
   SQ_STEP_WRITER = 1u << 1,
   SQ_STEP_FILES  = 1u << 2,
   SQ_STEP_HOME   = 1u << 3,
   SQ_STEP_NET    = 1u << 4
};

typedef struct sleep_q
{
   int      down;       /* 1 between sleep_q_down and sleep_q_up */
   unsigned stopped;    /* SQ_STEP_* that a down step really stopped */
} sleep_q;

/* Idempotent: a second call while down does nothing and returns 0.
 * Returns the number of steps that stopped something. */
int sleep_q_down(sleep_q *q, const sleep_q_ops *ops);
/* Undoes exactly what sleep_q_down stopped, in reverse.  No-op unless down.
 * Returns the number of steps that restarted something. */
int sleep_q_up(sleep_q *q, const sleep_q_ops *ops);

#ifdef __cplusplus
}
#endif

#endif /* SLEEP_Q_H */
