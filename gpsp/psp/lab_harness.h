/* lab_harness.h -- the GBAdhoc Lab task runner (GBAdhoc-Lab docs/PROTOCOL.md).
 *
 * Compiled only with LAB_HARNESS=1 (psp/Makefile) / `tools/build.sh lab`.
 * Without it not one line of this exists and main_psp.c's hooks expand to
 * nothing, so release and the existing harness are byte-for-byte unchanged.
 *
 * A lab build launched from ms0:/PSP/GAME/GBALAB/ (the launcher) or
 * ms0:/GBALAB/builds/<bid>/ reads the task queue the PC staged in
 * ms0:/GBALAB/, runs one task per process with the ordinary harness keys
 * (TASK.INI is the harness ini), keeps binary telemetry in RAM, writes the
 * result, then serves one USB window and relaunches itself.  Launched from
 * anywhere else it stays out of the way.  The ADR-0053 handoff
 * (usb_handoff.c) is untouched and is never used by a lab task.
 */
#ifndef LAB_HARNESS_H
#define LAB_HARNESS_H
#ifdef LAB_HARNESS
#include <stddef.h>

#define LAB_HARNESS_V 1

/* Call at the end of init_paths().  Returns 1 when a task is active (the
 * harness ini, rom, log dir and log path were pointed at the task), 0 when
 * this is not a lab launch.  May not return at all: a hop to another build,
 * an idle window, NOAUTO/EXIT all end the process here. */
int  lab_boot(const char *dir_base, char *harness, size_t harness_sz,
              char *rom_dir, size_t rom_sz, char *log_dir, size_t log_sz,
              char *log_path, size_t log_path_sz);
int  lab_active(void);
/* Right after fe_ini_audit(HARNESS_INI): read the lab_* keys again so the
 * key audit does not report them as unknown. */
void lab_audit_keys(void);
void lab_set_eboot_crc(unsigned crc);
/* After the save/state sidecar paths are computed: point them at
 * out/<tid>/work/ and copy the task's input state/save there. */
void lab_sidecars(char *save_path, size_t save_sz, char *state_path, size_t state_sz);
/* script / pad_script: resolve in tasks/<tid>/ instead of next to the EBOOT. */
void lab_task_file(char *out, size_t out_sz, const char *name);
/* Once per main-loop frame.  Returns nonzero when the task's wall-clock
 * limit is reached (the caller ends the loop with exit 6 lab_wall_limit). */
int  lab_frame(unsigned frame, unsigned wall_us, unsigned work_us,
               unsigned loop_iter, int loop_phase);
void lab_event(int code, unsigned arg, const char *text);
/* For the instruments agent: one pre-framed 64-byte TLM record (type 4..10). */
void lab_tlm_put(const void *rec64);
/* Replaces handoff_run() while a task is active: telemetry, RESULT.TXT,
 * then the window and the relaunch.  Returns only if every relaunch path
 * failed (the caller then exits to the XMB). */
void lab_finish(int exit_code, const char *reason);

#define LAB_EVT_STATE_LOAD 2
#endif

/* Hook macros for main_psp.c.  Each hook sits at the END of an existing line,
 * so a build without LAB_HARNESS has the same lines AND the same code: the
 * macros below expand to nothing / to exactly the original expression. */
#ifdef LAB_HARNESS
#define LAB_HOOK(x) x
#define LAB_ON lab_active()
#define LAB_HANDOFF(code, why, real) (lab_active() ? lab_finish((code), (why)) : real((code), (why)))
#else
#define LAB_HOOK(x)
#define LAB_ON 0
#define LAB_HANDOFF(code, why, real) real((code), (why))
#endif
#endif
