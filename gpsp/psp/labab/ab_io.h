/* ab_io.h -- the few sceIo operations the plugin, the test EBOOT and the lab
 * harness need around BUDGET.TXT.  PSP only (sceIo*, sceRtc*). */
#ifndef GBALAB_AB_IO_H
#define GBALAB_AB_IO_H
#include "budget.h"

#define AB_LOG_MAX 16384               /* AUTOBOOT.LOG rotates to AUTOBOOT.OLD above this */

int    ab_exists(const char *path);
/* Read up to cap bytes; returns the length, or -1 (missing / unreadable /
 * larger than cap -- a too-big BUDGET.TXT is corrupt by definition). */
int    ab_read_file(const char *path, char *buf, int cap);
/* Write the whole buffer (create/truncate), then read it back and compare.
 * Returns 0 only if the card now holds exactly these bytes. */
int    ab_write_verify(const char *path, const char *buf, int len);
/* 1 if the file starts with the PBP magic "\0PBP". */
int    ab_is_pbp(const char *path);
/* Append one line (a '\n' is added); rotates at AB_LOG_MAX.  Best effort. */
void   ab_log_line(const char *path, const char *old_path, const char *line);

/* RTC seconds (sceRtcGetCurrentTick / 1e6).  0 = the clock could not be read. */
ab_u64 ab_rtc_now(void);

/* Read + parse BUDGET.TXT in one go.  Returns the ab_budget_parse() code. */
int    ab_budget_load(const char *path, ab_budget *b);
/* Format + write + verify.  Returns 0 on success. */
int    ab_budget_store(const char *path, const ab_budget *b);

#endif
