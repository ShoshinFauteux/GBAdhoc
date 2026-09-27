/* fe_autopilot.c — autopilot input-script engine (see fe_autopilot.h). */
#include "fe_autopilot.h"
#include "fe_host.h"
#include "fe_evt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A fixture that plays a whole battle needs more steps than one that presses a
 * few buttons.  The rival battle runs ~3900 frames and has to be advanced with
 * a press every ~20, which is roughly 380 steps -- and the parser refuses a
 * script over the limit outright, so on hardware that reads as a console that
 * ran the job and did nothing.
 *
 * `mash` is not a way around it: it waits for a RAM predicate and ap_fail()s on
 * timeout, which scores the run invalid.
 *
 * Raised only for the rig.  steps[] is a static array, so this is BSS, and the
 * release build keeps the size it has always had rather than carrying 18 KB for
 * an automation feature a player cannot reach. */
#ifdef GPSP_PERF_RIG
#define AP_MAX_STEPS   1024
#else
#define AP_MAX_STEPS   256
#endif
#define AP_NAME_LEN    32
#define AP_MASH_ON     2     /* frames pressed per mash cycle */
#define AP_MASH_PERIOD 8     /* mash cycle length in frames */
#define AP_PRESS_GAP   2     /* release frames appended to press */
#define AP_MAX_CHAIN   64    /* zero-frame steps executed per frame */

enum ap_op
{
   OP_EVT, OP_FF, OP_DUMP, OP_WAIT, OP_PRESS, OP_HOLD,
   OP_WAITRAM, OP_MASH, OP_HOLDRAM, OP_WAITSRAM, OP_LOGRAM, OP_LOGPTR,
   OP_REPEAT, OP_ENDREPEAT, OP_LOGBYTES, OP_MASHIF
#ifdef GPSP_PERF_RIG
   , OP_STATE                 /* harness only -- see fe_autopilot_state_pending */
#endif
};

/* Only ever read by an fe_evt argument, so it disappears with telemetry. */
static const char *op_name[] __attribute__((unused)) =
{
   "evt", "ff", "dump", "wait", "press", "hold",
   "waitram", "mash", "holdram", "waitsram", "logram", "logptr",
   "repeat", "endrepeat", "logbytes", "mashif"
#ifdef GPSP_PERF_RIG
   , "state"
#endif
};

typedef struct
{
   uint8_t  op;
   uint8_t  size;              /* 1/2/4 for RAM ops */
   uint8_t  negate;            /* waitramne/waitptrne */
   uint8_t  deref;             /* *ptr variants: addr holds a u32 GBA ptr */
   uint8_t  flag;              /* ff on/off; mashsram (buttons != 0) */
   uint16_t line;              /* script line, for diagnostics */
   uint32_t buttons;
   uint32_t buttons2;          /* holdmash pulsed buttons */
   uint32_t addr;
   uint32_t mask;
   uint32_t val;
   uint32_t off;               /* logptr offset; repeat count */
   uint32_t frames;            /* wait/hold/press duration or timeout */
   uint8_t  csize;             /* mashif: the PRESS-ONLY-WHILE condition */
   uint32_t caddr, cmask, cval;
   uint32_t cstart;            /* mashif: frame the condition last rose */
   char     name[AP_NAME_LEN]; /* evt text / log label */
} ap_step;

static ap_step  steps[AP_MAX_STEPS];
static int      step_count;
static int      loaded;

static int      cur;            /* current step index */
static uint32_t step_frame;     /* frames spent in current step */
static int      step_inited;    /* per-step one-time init done */
static uint32_t sram_ref_crc;   /* waitsram reference */
static int      status = 2;     /* 0 run, 1 done, -1 fail, 2 none */
static int      ff_on;
static int      dump_req;
#ifdef GPSP_PERF_RIG
static int      state_req;     /* harness only: `state` asked for a reload */
#endif
static uint32_t frame_no;       /* engine frame counter (for EVT context) */

static int      rpt_start = -1; /* repeat block: index of step after REPEAT */
static uint32_t rpt_left;
/* 1-based iteration of the enclosing repeat block, 0 outside one.  Stamped on
 * every ap_mark/ap_sync/ap_val/ap_fail as `it=`, so a per-iteration metric
 * (a battle turn) is keyed by the engine itself, never by counting lines --
 * a count breaks the moment one line is lost. */
static uint32_t rpt_it;
/* The last value a predicate read, for ap_fail's `val=`: a timeout that says
 * WHAT it saw instead of only that it waited. */
static uint32_t last_val;
static int      last_err;
static uint32_t script_crc;     /* CRC32 of the script file, for ap_loaded */

#define AP_LOGBYTES_MAX 24

static uint32_t ap_crc32(uint32_t crc, const void *data, size_t len)
{
   const uint8_t *p = (const uint8_t *)data;
   size_t i;
   int b;
   crc = ~crc;
   for (i = 0; i < len; i++)
   {
      crc ^= p[i];
      for (b = 0; b < 8; b++)
         crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
   }
   return ~crc;
}

/* ------------------------------------------------------------- parsing -- */

static int parse_buttons(const char *s, uint32_t *out)
{
   static const struct { const char *n; int bit; } tab[] = {
      { "B", 0 }, { "SELECT", 2 }, { "START", 3 }, { "UP", 4 },
      { "DOWN", 5 }, { "LEFT", 6 }, { "RIGHT", 7 }, { "A", 8 },
      { "L", 10 }, { "R", 11 },
   };
   char buf[64], *tok, *save = NULL;
   uint32_t m = 0;
   size_t i;

   strncpy(buf, s, sizeof(buf) - 1);
   buf[sizeof(buf) - 1] = '\0';
   for (tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save))
   {
      int hit = 0;
      for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
      {
         if (strcmp(tok, tab[i].n) == 0)
         {
            m |= 1u << tab[i].bit;
            hit = 1;
            break;
         }
      }
      if (!hit)
         return -1;
   }
   *out = m;
   return m ? 0 : -1;
}

static uint32_t parse_num(const char *s)
{
   return (uint32_t)strtoul(s, NULL, 0);
}

int fe_autopilot_load(const char *path)
{
   FILE *f = fopen(path, "r");
   char line[256];
   int lineno = 0, in_repeat = 0;

   step_count = 0;
   loaded = 0;
   status = 2;
   cur = 0;
   step_frame = 0;
   step_inited = 0;
   ff_on = 0;
   dump_req = 0;
#ifdef GPSP_PERF_RIG
   state_req = 0;
#endif
   frame_no = 0;
   rpt_start = -1;
   rpt_left = 0;
   rpt_it = 0;
   last_val = 0;
   last_err = 0;
   script_crc = 0;

   if (!f)
   {
      fe_log("autopilot: cannot open %s", path);
      return -1;
   }

   while (fgets(line, sizeof(line), f))
   {
      char *tok[12];
      int ntok = 0;
      char *p, *save = NULL;
      ap_step *st;

      lineno++;
      script_crc = ap_crc32(script_crc, line, strlen(line));
      /* strip comments */
      p = strchr(line, '#');
      if (p) *p = '\0';
      p = strchr(line, ';');
      if (p) *p = '\0';

      for (p = strtok_r(line, " \t\r\n", &save);
           p && ntok < 12;
           p = strtok_r(NULL, " \t\r\n", &save))
         tok[ntok++] = p;
      if (ntok == 0)
         continue;

      if (step_count >= AP_MAX_STEPS)
      {
         fe_log("autopilot: %s:%d too many steps (max %d)", path, lineno,
                AP_MAX_STEPS);
         fclose(f);
         return -1;
      }
      st = &steps[step_count];
      memset(st, 0, sizeof(*st));
      st->line = (uint16_t)lineno;

      if (!strcmp(tok[0], "evt") && ntok >= 2)
      {
         st->op = OP_EVT;
         strncpy(st->name, tok[1], AP_NAME_LEN - 1);
      }
      else if (!strcmp(tok[0], "ff") && ntok == 2)
      {
         st->op = OP_FF;
         st->flag = (uint8_t)(strcmp(tok[1], "on") == 0);
      }
#ifdef GPSP_PERF_RIG
      else if (!strcmp(tok[0], "state") && ntok == 1)
      {
         st->op = OP_STATE;
      }
#endif
      else if (!strcmp(tok[0], "dump") && ntok == 1)
         st->op = OP_DUMP;
      else if (!strcmp(tok[0], "wait") && ntok == 2)
      {
         st->op = OP_WAIT;
         st->frames = parse_num(tok[1]);
      }
      else if (!strcmp(tok[0], "press") && (ntok == 2 || ntok == 3))
      {
         st->op = OP_PRESS;
         if (parse_buttons(tok[1], &st->buttons))
            goto bad;
         st->frames = (ntok == 3) ? parse_num(tok[2]) : 2;
      }
      else if (!strcmp(tok[0], "hold") && ntok == 3)
      {
         st->op = OP_HOLD;
         if (parse_buttons(tok[1], &st->buttons))
            goto bad;
         st->frames = parse_num(tok[2]);
      }
      else if ((!strcmp(tok[0], "waitram") || !strcmp(tok[0], "waitramne")) &&
               ntok == 6)
      {
         st->op = OP_WAITRAM;
         st->negate = (uint8_t)(tok[0][7] != '\0');
         st->size = (uint8_t)parse_num(tok[1]);
         st->addr = parse_num(tok[2]);
         st->mask = parse_num(tok[3]);
         st->val  = parse_num(tok[4]);
         st->frames = parse_num(tok[5]);
      }
      else if ((!strcmp(tok[0], "waitptr") || !strcmp(tok[0], "waitptrne")) &&
               ntok == 7)
      {
         st->op = OP_WAITRAM;
         st->deref = 1;
         st->negate = (uint8_t)(tok[0][7] != '\0');
         st->size = (uint8_t)parse_num(tok[1]);
         st->addr = parse_num(tok[2]);
         st->off  = parse_num(tok[3]);
         st->mask = parse_num(tok[4]);
         st->val  = parse_num(tok[5]);
         st->frames = parse_num(tok[6]);
      }
      else if ((!strcmp(tok[0], "mash") || !strcmp(tok[0], "holdram") ||
                !strcmp(tok[0], "mashne")) && ntok == 7)
      {
         st->op = tok[0][0] == 'm' ? OP_MASH : OP_HOLDRAM;
         /* mashne: mash until the value is NO LONGER VAL -- "keep pressing A
          * until the game has consumed it and left this menu". */
         st->negate = (uint8_t)(!strcmp(tok[0], "mashne"));
         if (parse_buttons(tok[1], &st->buttons))
            goto bad;
         st->size = (uint8_t)parse_num(tok[2]);
         st->addr = parse_num(tok[3]);
         st->mask = parse_num(tok[4]);
         st->val  = parse_num(tok[5]);
         st->frames = parse_num(tok[6]);
      }
      else if ((!strcmp(tok[0], "mashptr") || !strcmp(tok[0], "holdptr") ||
                !strcmp(tok[0], "mashptrne")) && ntok == 8)
      {
         st->op = tok[0][0] == 'm' ? OP_MASH : OP_HOLDRAM;
         st->deref = 1;
         st->negate = (uint8_t)(!strcmp(tok[0], "mashptrne"));
         if (parse_buttons(tok[1], &st->buttons))
            goto bad;
         st->size = (uint8_t)parse_num(tok[2]);
         st->addr = parse_num(tok[3]);
         st->off  = parse_num(tok[4]);
         st->mask = parse_num(tok[5]);
         st->val  = parse_num(tok[6]);
         st->frames = parse_num(tok[7]);
      }
      else if (!strcmp(tok[0], "holdmash") && ntok == 8)
      {
         st->op = OP_HOLDRAM;
         if (parse_buttons(tok[1], &st->buttons) ||
             parse_buttons(tok[2], &st->buttons2))
            goto bad;
         st->size = (uint8_t)parse_num(tok[3]);
         st->addr = parse_num(tok[4]);
         st->mask = parse_num(tok[5]);
         st->val  = parse_num(tok[6]);
         st->frames = parse_num(tok[7]);
      }
      else if (!strcmp(tok[0], "holdmashptr") && ntok == 9)
      {
         st->op = OP_HOLDRAM;
         st->deref = 1;
         if (parse_buttons(tok[1], &st->buttons) ||
             parse_buttons(tok[2], &st->buttons2))
            goto bad;
         st->size = (uint8_t)parse_num(tok[3]);
         st->addr = parse_num(tok[4]);
         st->off  = parse_num(tok[5]);
         st->mask = parse_num(tok[6]);
         st->val  = parse_num(tok[7]);
         st->frames = parse_num(tok[8]);
      }
      else if (!strcmp(tok[0], "mashif") && ntok == 11)
      {
         /* mashif BTNS CSZ CADDR CMASK CVAL  SZ ADDR MASK VAL TO
          * Mash BTNS only on frames where (mem[CADDR]&CMASK)==CVAL, until
          * (mem[ADDR]&MASK)==VAL.  Built for dialogue: press A only while a
          * text printer is active, until the menu that follows is up -- so a
          * press can advance text but can never land on the menu. */
         st->op = OP_MASHIF;
         if (parse_buttons(tok[1], &st->buttons))
            goto bad;
         st->csize = (uint8_t)parse_num(tok[2]);
         st->caddr = parse_num(tok[3]);
         st->cmask = parse_num(tok[4]);
         st->cval  = parse_num(tok[5]);
         st->size  = (uint8_t)parse_num(tok[6]);
         st->addr  = parse_num(tok[7]);
         st->mask  = parse_num(tok[8]);
         st->val   = parse_num(tok[9]);
         st->frames = parse_num(tok[10]);
         if (st->csize != 1 && st->csize != 2 && st->csize != 4)
            goto bad;
      }
      else if (!strcmp(tok[0], "waitsram") && ntok == 2)
      {
         st->op = OP_WAITSRAM;
         st->frames = parse_num(tok[1]);
      }
      else if (!strcmp(tok[0], "mashsram") && ntok == 3)
      {
         st->op = OP_WAITSRAM;
         st->flag = 1;
         if (parse_buttons(tok[1], &st->buttons))
            goto bad;
         st->frames = parse_num(tok[2]);
      }
      else if (!strcmp(tok[0], "logram") && ntok == 4)
      {
         st->op = OP_LOGRAM;
         strncpy(st->name, tok[1], AP_NAME_LEN - 1);
         st->size = (uint8_t)parse_num(tok[2]);
         st->addr = parse_num(tok[3]);
      }
      else if (!strcmp(tok[0], "logbytes") && ntok == 4)
      {
         /* logbytes NAME N ADDR -- N raw bytes as hex, one line. */
         st->op = OP_LOGBYTES;
         strncpy(st->name, tok[1], AP_NAME_LEN - 1);
         st->frames = parse_num(tok[2]);
         st->addr = parse_num(tok[3]);
         if (st->frames == 0 || st->frames > AP_LOGBYTES_MAX)
            goto bad;
      }
      else if (!strcmp(tok[0], "logptr") && ntok == 5)
      {
         st->op = OP_LOGPTR;
         strncpy(st->name, tok[1], AP_NAME_LEN - 1);
         st->size = (uint8_t)parse_num(tok[2]);
         st->addr = parse_num(tok[3]);
         st->off  = parse_num(tok[4]);
      }
      else if (!strcmp(tok[0], "repeat") && ntok == 2)
      {
         if (in_repeat)
            goto bad;   /* no nesting */
         in_repeat = 1;
         st->op = OP_REPEAT;
         st->off = parse_num(tok[1]);
      }
      else if (!strcmp(tok[0], "endrepeat") && ntok == 1)
      {
         if (!in_repeat)
            goto bad;
         in_repeat = 0;
         st->op = OP_ENDREPEAT;
      }
      else
         goto bad;

      if ((st->op == OP_WAITRAM || st->op == OP_MASH || st->op == OP_HOLDRAM ||
           st->op == OP_LOGRAM || st->op == OP_LOGPTR || st->op == OP_MASHIF) &&
          st->size != 1 && st->size != 2 && st->size != 4)
         goto bad;

      step_count++;
      continue;

   bad:
      fe_log("autopilot: %s:%d parse error", path, lineno);
      fclose(f);
      return -1;
   }
   fclose(f);

   if (in_repeat)
   {
      fe_log("autopilot: %s: repeat without endrepeat", path);
      return -1;
   }
   if (step_count == 0)
   {
      fe_log("autopilot: %s: empty script", path);
      return -1;
   }

   loaded = 1;
   status = 0;
   fe_evt("ap_loaded steps=%d file=%s crc=%08x", step_count, path,
          (unsigned)script_crc);
   return 0;
}

/* ------------------------------------------------------------- running -- */

static uint32_t read_mem(uint8_t size, uint32_t addr, int *err)
{
   uint8_t b[4] = { 0, 0, 0, 0 };
   if (fe_host_mem_read(addr, b, size) != 0)
   {
      *err = 1;
      return 0;
   }
   return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
          ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int predicate(const ap_step *st)
{
   int err = 0;
   uint32_t addr = st->addr;
   uint32_t v;
   int eq;

   if (st->deref)
   {
      addr = read_mem(4, st->addr, &err);
      last_err = err;
      if (err)
         return 0;
      addr += st->off;
   }
   v = read_mem(st->size, addr, &err);
   last_err = err;
   if (err)
      return 0;
   last_val = v;
   eq = ((v & st->mask) == st->val);
   return st->negate ? !eq : eq;
}

static void ap_fail(const ap_step *st)
{
   FE_EVT_ONLY(st);
   status = -1;
   if (last_err)
      fe_evt("ap_fail step=%d line=%d op=%s frame=%u it=%u val=ERR", cur,
             st->line, op_name[st->op], frame_no, (unsigned)rpt_it);
   else
      fe_evt("ap_fail step=%d line=%d op=%s frame=%u it=%u val=0x%08x", cur,
             st->line, op_name[st->op], frame_no, (unsigned)rpt_it,
             (unsigned)last_val);
}

static void log_val(const ap_step *st, int deref)
{
   int err = 0;
   uint32_t addr = st->addr;
   uint32_t v = 0;

   FE_EVT_ONLY(v);

   if (deref)
   {
      addr = read_mem(4, st->addr, &err);
      if (!err)
         addr += st->off;
   }
   v = err ? 0 : read_mem(st->size, addr, &err);
   if (err)
      fe_evt("ap_val name=%s val=ERR it=%u f=%u", st->name, (unsigned)rpt_it,
             frame_no);
   else
      fe_evt("ap_val name=%s val=0x%08x it=%u f=%u", st->name, (unsigned)v,
             (unsigned)rpt_it, frame_no);
}

static void log_bytes(const ap_step *st)
{
   uint8_t b[AP_LOGBYTES_MAX];
   char hex[AP_LOGBYTES_MAX * 2 + 1];
   unsigned i, n = st->frames;
   static const char dig[] = "0123456789abcdef";
   FE_EVT_ONLY(hex);
   if (fe_host_mem_read(st->addr, b, n) != 0)
   {
      fe_evt("ap_val name=%s val=ERR it=%u f=%u", st->name, (unsigned)rpt_it,
             frame_no);
      return;
   }
   for (i = 0; i < n; i++)
   {
      hex[i * 2]     = dig[b[i] >> 4];
      hex[i * 2 + 1] = dig[b[i] & 15];
   }
   hex[n * 2] = '\0';
   fe_evt("ap_val name=%s hex=%s it=%u f=%u", st->name, hex,
          (unsigned)rpt_it, frame_no);
}

/* Execute the current step for this frame.
 * Returns 1 if the step consumed the frame (input decided), 0 if it
 * completed instantly and the next step may run in the same frame. */
static int run_step(void)
{
   ap_step *st = &steps[cur];

   switch (st->op)
   {
   case OP_EVT:
      /* f= / t_ms= appended AFTER the text so every existing
       * `grep "ap_mark text=NAME"` still matches: per-step wall-clock is what
       * the link-lag oracle scores (summarize_log.py `lag`). */
      fe_evt("ap_mark text=%s f=%u t_ms=%u it=%u", st->name, frame_no,
             (unsigned)(fe_evt_now_us() / 1000ull), (unsigned)rpt_it);
      return 0;

   case OP_FF:
      ff_on = st->flag;
      return 0;

   case OP_DUMP:
      dump_req = 1;
      return 0;

#ifdef GPSP_PERF_RIG
   case OP_STATE:
      state_req = 1;
      return 0;
#endif

   case OP_LOGRAM:
      log_val(st, 0);
      return 0;

   case OP_LOGPTR:
      log_val(st, 1);
      return 0;

   case OP_LOGBYTES:
      log_bytes(st);
      return 0;

   case OP_REPEAT:
      rpt_start = cur + 1;
      rpt_left = st->off;
      rpt_it = 1;
      if (rpt_left == 0)
      {
         /* skip the whole block */
         while (cur < step_count && steps[cur].op != OP_ENDREPEAT)
            cur++;
      }
      return 0;

   case OP_ENDREPEAT:
      if (rpt_left > 1)
      {
         rpt_left--;
         rpt_it++;
         cur = rpt_start - 1;   /* advanced past REPEAT by caller */
      }
      else
         rpt_it = 0;            /* left the block */
      return 0;

   case OP_WAIT:
      fe_host_input_inject(0);
      if (++step_frame >= st->frames)
         return 2;
      return 1;

   case OP_HOLD:
      fe_host_input_inject(st->buttons);
      if (++step_frame >= st->frames)
         return 2;
      return 1;

   case OP_PRESS:
      fe_host_input_inject(step_frame < st->frames ? st->buttons : 0);
      if (++step_frame >= st->frames + AP_PRESS_GAP)
         return 2;
      return 1;

   case OP_WAITRAM:
   case OP_MASH:
   case OP_HOLDRAM:
      if (predicate(st))
      {
         fe_evt("ap_sync step=%d line=%d frame=%u t_ms=%u it=%u", cur,
                st->line, frame_no, (unsigned)(fe_evt_now_us() / 1000ull),
                (unsigned)rpt_it);
         fe_host_input_inject(0);
         return 2;
      }
      if (step_frame >= st->frames)
      {
         ap_fail(st);
         fe_host_input_inject(0);
         return 1;
      }
      if (st->op == OP_HOLDRAM)
         fe_host_input_inject(st->buttons |
                              (((step_frame % AP_MASH_PERIOD) < AP_MASH_ON)
                                  ? st->buttons2 : 0));
      else
         fe_host_input_inject((st->op == OP_MASH &&
                               (step_frame % AP_MASH_PERIOD) < AP_MASH_ON)
                                 ? st->buttons : 0);
      step_frame++;
      return 1;

   case OP_MASHIF:
   {
      int err = 0;
      uint32_t c;
      if (step_frame == 0)
         st->cstart = 0;
      if (predicate(st))
      {
         fe_evt("ap_sync step=%d line=%d frame=%u t_ms=%u it=%u", cur,
                st->line, frame_no, (unsigned)(fe_evt_now_us() / 1000ull),
                (unsigned)rpt_it);
         fe_host_input_inject(0);
         return 2;
      }
      if (step_frame >= st->frames)
      {
         ap_fail(st);
         fe_host_input_inject(0);
         return 1;
      }
      c = read_mem(st->csize, st->caddr, &err);
      if (err || (c & st->cmask) != st->cval)
      {
         st->cstart = step_frame + 1;     /* next eligible frame is a NEW press */
         fe_host_input_inject(0);
      }
      else
         fe_host_input_inject(((step_frame - st->cstart) % AP_MASH_PERIOD) <
                              AP_MASH_ON ? st->buttons : 0);
      step_frame++;
      return 1;
   }

   case OP_WAITSRAM:
      if (!step_inited)
      {
         sram_ref_crc = fe_host_sram_crc_now();
         step_inited = 1;
      }
      if (fe_host_sram_crc_now() != sram_ref_crc)
      {
         fe_evt("ap_sync step=%d line=%d frame=%u sram_crc=%08x", cur,
                st->line, frame_no, (unsigned)fe_host_sram_crc_now());
         fe_host_input_inject(0);
         return 2;
      }
      if (step_frame >= st->frames)
      {
         ap_fail(st);
         fe_host_input_inject(0);
         return 1;
      }
      fe_host_input_inject((st->flag &&
                            (step_frame % AP_MASH_PERIOD) < AP_MASH_ON)
                              ? st->buttons : 0);
      step_frame++;
      return 1;
   }
   return 1;   /* unreachable */
}

void fe_autopilot_frame(void)
{
   int chain;

   if (status != 0)
   {
      if (loaded)
         fe_host_input_inject(0);
      return;
   }

   frame_no++;

   for (chain = 0; chain < AP_MAX_CHAIN; chain++)
   {
      int r;
      if (cur >= step_count)
      {
         status = 1;
         fe_host_input_inject(0);
         fe_evt("ap_done steps=%d frame=%u", step_count, frame_no);
         return;
      }
      r = run_step();
      if (status == -1)
         return;
      if (r == 0 || r == 2)
      {
         /* step complete: advance */
         cur++;
         step_frame = 0;
         step_inited = 0;
         if (r == 2)
            return;      /* it consumed this frame */
         continue;       /* instant step: chain into the next one */
      }
      return;            /* r == 1: step in progress, frame consumed */
   }
   fe_log("autopilot: zero-frame step chain too long at step %d", cur);
   status = -1;
   fe_evt("ap_fail step=%d line=%d op=chain frame=%u", cur,
          steps[cur < step_count ? cur : step_count - 1].line, frame_no);
}

int fe_autopilot_active(void)
{
   return loaded && status == 0;
}

int fe_autopilot_status(void)
{
   return status;
}

int fe_autopilot_ff(void)
{
   return loaded && ff_on;
}

int fe_autopilot_dump_pending(void)
{
   int r = dump_req;
   dump_req = 0;
   return r;
}

#ifdef GPSP_PERF_RIG
int fe_autopilot_state_pending(void)
{
   int r = state_req;
   state_req = 0;
   return r;
}
#endif
