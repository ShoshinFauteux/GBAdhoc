/* usb_handoff.h — ADR-0053: hands the memory stick to the PC between runs.
 *
 * Milestone 1 of the unattended hardware loop.  The console finishes a run,
 * publishes its result, exposes ms0 over USB for a fixed window so the PC can
 * collect the log and stage the next build, then relaunches itself.
 *
 * DEFAULT OFF.  Enabled only by `handoff = 1` in `.gpsp-harness.ini`, never by
 * CONFIG.INI — same reasoning as ADR-0036: a stale harness file that silently
 * toggles USB on a console someone is actually playing is exactly the class of
 * surprise that rename existed to prevent.
 */
#ifndef USB_HANDOFF_H
#define USB_HANDOFF_H

/* Read the harness keys.  Call once at startup, before the run. */
void handoff_config(int enabled, int window_s, int max_runs, int total_s,
                    int park_s);

/* Call as the VERY LAST thing before sceKernelExitGame(), after evt_shutdown()
 * — i.e. once every thread that can touch ms0 has been stopped and the log is
 * closed.  Returns normally if the handoff is disabled, times out, or the PC
 * asks us to stop; does not return at all if the PC asks for another run
 * (sceKernelLoadExec replaces this process).
 *
 * `exit_code` / `exit_reason` are published to the PC so the loop can tell a
 * clean finish from ap_fail without parsing the log. */
void handoff_run(int exit_code, const char *exit_reason);

#endif
