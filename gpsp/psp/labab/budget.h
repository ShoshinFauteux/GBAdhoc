/* budget.h -- the relaunch budget shared by the XMB autoboot plugin and the
 * lab harness (ms0:/GBALAB/sys/BUDGET.TXT, PROTOCOL.md 10.3, autoboot/DESIGN.md).
 *
 * Portable C, no libc calls, no allocation: the same file compiles into the
 * VSH plugin (no C runtime at all), the lab harness (newlib) and the host unit
 * tests (gcc).  Everything that touches a file lives in ab_io.c instead.
 *
 * FAIL-CLOSED CONTRACT.  ab_budget_parse() returns AB_OK only for a file that
 * is complete (crc line present and matching), version 1 and in range.  The
 * plugin launches nothing unless it returns AB_OK *and* left > 0. */
#ifndef GBALAB_BUDGET_H
#define GBALAB_BUDGET_H

typedef unsigned int       ab_u32;
typedef unsigned long long ab_u64;

#define AB_VERSION        1
#define AB_CAP_DEFAULT    3      /* left after a refill */
#define AB_CAP_MAX        9      /* a file claiming more is corrupt */
#define AB_HIST           6      /* launch timestamps kept (= hourly rate limit) */
#define AB_RATE_WINDOW_S  3600
#define AB_RATE_MAX       6      /* launches per AB_RATE_WINDOW_S */
#define AB_HEALTHY_S      60     /* harness: seconds since the last autoboot before it may refill */
#define AB_FUTURE_SLACK_S 300    /* a stamp further than this in the future = the clock went backwards */
#define AB_FILE_MAX       1024   /* BUDGET.TXT is never larger */
#define AB_STR            16

enum {
   AB_OK         =  0,
   AB_E_MISSING  = -1,   /* no file / unreadable */
   AB_E_SYNTAX   = -2,   /* malformed line, missing key, number overflow */
   AB_E_VERSION  = -3,   /* v != 1 */
   AB_E_CRC      = -4,   /* no crc line, or mismatch (torn or edited file) */
   AB_E_RANGE    = -5,   /* cap/left/launch count out of range */
   AB_E_TOOBIG   = -6
};

typedef struct {
   ab_u32 left;                  /* launches still allowed */
   ab_u32 cap;                   /* what a refill restores */
   ab_u32 seq;                   /* +1 on every write, by anyone */
   ab_u32 boot;                  /* harness BOOT.TXT boot counter at its last write (plugin copies it) */
   ab_u64 t;                     /* RTC seconds (sceRtcGetCurrentTick()/1e6) of this write */
   ab_u32 n_launch;              /* valid entries in launch[] */
   ab_u64 launch[AB_HIST];       /* plugin launch times, oldest first */
   char   by[AB_STR];            /* plugin | harness | abtest */
   char   reason[AB_STR];        /* launch | cancel | refill | init | exit | arm | disarm */
} ab_budget;

/* crc32 (IEEE 802.3, same as zlib.crc32), bitwise: small, no table. */
ab_u32 ab_crc32(const void *data, int len);

/* Parse a BUDGET.TXT image.  len < 0 or buf == 0 -> AB_E_MISSING. */
int  ab_budget_parse(const char *buf, int len, ab_budget *out);

/* Render b to buf (cap bytes); returns the length, or -1 if cap is too small.
 * The output always ends with the crc line, so a torn write fails to parse. */
int  ab_budget_format(const ab_budget *b, char *buf, int cap);

const char *ab_err_name(int rc);

/* A fresh file (no launch history), left = cap = AB_CAP_DEFAULT. */
void ab_budget_init(ab_budget *b, ab_u64 now, ab_u32 boot, const char *by, const char *reason);

/* ---- rate limit ---- */
/* Launches that count against the hourly limit at `now`: stamps inside
 * (now - window, now] plus any stamp in the future (clock went backwards:
 * counted, conservatively). */
int  ab_rate_count(const ab_budget *b, ab_u64 now);
int  ab_rate_ok(const ab_budget *b, ab_u64 now);

/* ---- plugin transitions ---- */
void ab_budget_take(ab_budget *b, ab_u64 now);     /* left -= 1, stamp, seq += 1, by=plugin reason=launch */
void ab_budget_cancel(ab_budget *b, ab_u64 now);   /* left = 0 (owner held a button), by=plugin reason=cancel */

/* ---- harness transitions ---- */
/* Should a harness write a refill now?  rc/b = the parse result of the file it
 * just read.  proof = this process consumed a token-valid CMD.TXT (the PC served
 * the previous window) or wrote a RESULT.TXT for a task.  Returns 1 = write
 * ab_budget_refill(); 0 = leave the file alone. */
int  ab_harness_should_refill(int rc, const ab_budget *b, ab_u64 now, int proof);
/* Refill in place (rc == AB_OK) or start a fresh file (rc != AB_OK).  Future
 * stamps (clock went backwards) are dropped; the rest of the history is kept. */
void ab_budget_refill(ab_budget *b, int rc, ab_u64 now, ab_u32 boot);
/* left = 0 (CMD=EXIT, or any deliberate "stop this console" path).  Returns 0
 * when there is nothing to write (no valid file = already fail-closed). */
int  ab_budget_disarm(ab_budget *b, int rc, ab_u64 now, ab_u32 boot, const char *reason);

/* ---- plugin decision ---- */
typedef struct {
   int    cold_boot;     /* VSH apitype == 0x200 (power-on), not 0x210/0x220 (return from a game / VSH reset) */
   int    lab_card;      /* ms0:/GBALAB/LAB.ID exists */
   int    noauto;        /* ms0:/GBALAB/NOAUTO exists */
   int    eboot_ok;      /* target exists and starts with "\0PBP" */
   int    usb_active;    /* USB mass storage is up (the PC may own the card) */
   int    clock_ok;      /* the RTC read succeeded */
   int    budget_rc;     /* ab_budget_parse() result */
   ab_u64 now;
} ab_inputs;

enum {
   AB_GO = 0,
   AB_SKIP_NOT_COLD,     /* silent: no file is touched */
   AB_SKIP_NOT_LAB,      /* silent: not a lab card, no file is touched */
   AB_SKIP_NOAUTO,
   AB_SKIP_USB,
   AB_SKIP_CLOCK,
   AB_SKIP_BUDGET_BAD,
   AB_SKIP_EXHAUSTED,
   AB_SKIP_RATE,
   AB_SKIP_NO_EBOOT
};
int         ab_decide(const ab_inputs *in, const ab_budget *b);
const char *ab_verdict_name(int v);
/* 1 = the plugin may write its log for this verdict (lab card on a cold boot). */
int         ab_verdict_logs(int v);

/* Tiny helpers the glue code uses too (no libc in the plugin). */
int  ab_strlen(const char *s);
void ab_strlcpy(char *dst, const char *src, int cap);
int  ab_put_u64(char *dst, int cap, ab_u64 v);      /* decimal, returns chars written (no NUL) or -1 */
int  ab_put_hex32(char *dst, int cap, ab_u32 v);    /* 8 lowercase hex digits, returns 8 or -1 */

#endif
