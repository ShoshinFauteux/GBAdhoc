/* fe_autopilot.c — autopilot input-script engine (see fe_autopilot.h). */
#include "fe_autopilot.h"
#include "fe_host.h"
#include "fe_evt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AP_MAX_STEPS   256
#define AP_NAME_LEN    32
#define AP_MASH_ON     2     /* frames pressed per mash cycle */
#define AP_MASH_PERIOD 8     /* mash cycle length in frames */
#define AP_PRESS_GAP   2     /* release frames appended to press */
#define AP_MAX_CHAIN   64    /* zero-frame steps executed per frame */

enum ap_op
{
   OP_EVT, OP_FF, OP_DUMP, OP_WAIT, OP_PRESS, OP_HOLD,
   OP_WAITRAM, OP_MASH, OP_HOLDRAM, OP_WAITSRAM, OP_LOGRAM, OP_LOGPTR,
   OP_REPEAT, OP_ENDREPEAT
};

static const char *op_name[] =
{
   "evt", "ff", "dump", "wait", "press", "hold",
   "waitram", "mash", "holdram", "waitsram", "logram", "logptr",
   "repeat", "endrepeat"
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
static uint32_t frame_no;       /* engine frame counter (for EVT context) */

static int      rpt_start = -1; /* repeat block: index of step after REPEAT */
static uint32_t rpt_left;

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
   frame_no = 0;
   rpt_start = -1;
   rpt_left = 0;

   if (!f)
   {
      fe_log("autopilot: cannot open %s", path);
      return -1;
   }

   while (fgets(line, sizeof(line), f))
   {
      char *tok[10];
      int ntok = 0;
      char *p, *save = NULL;
      ap_step *st;

      lineno++;
      /* strip comments */
      p = strchr(line, '#');
      if (p) *p = '\0';
      p = strchr(line, ';');
      if (p) *p = '\0';

      for (p = strtok_r(line, " \t\r\n", &save);
           p && ntok < 10;
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
      else if ((!strcmp(tok[0], "mash") || !strcmp(tok[0], "holdram")) &&
               ntok == 7)
      {
         st->op = tok[0][0] == 'm' ? OP_MASH : OP_HOLDRAM;
         if (parse_buttons(tok[1], &st->buttons))
            goto bad;
         st->size = (uint8_t)parse_num(tok[2]);
         st->addr = parse_num(tok[3]);
         st->mask = parse_num(tok[4]);
         st->val  = parse_num(tok[5]);
         st->frames = parse_num(tok[6]);
      }
      else if ((!strcmp(tok[0], "mashptr") || !strcmp(tok[0], "holdptr")) &&
               ntok == 8)
      {
         st->op = tok[0][0] == 'm' ? OP_MASH : OP_HOLDRAM;
         st->deref = 1;
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
           st->op == OP_LOGRAM || st->op == OP_LOGPTR) &&
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
   fe_evt("ap_loaded steps=%d file=%s", step_count, path);
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
      if (err)
         return 0;
      addr += st->off;
   }
   v = read_mem(st->size, addr, &err);
   if (err)
      return 0;
   eq = ((v & st->mask) == st->val);
   return st->negate ? !eq : eq;
}

static void ap_fail(const ap_step *st)
{
   status = -1;
   fe_evt("ap_fail step=%d line=%d op=%s frame=%u", cur, st->line,
          op_name[st->op], frame_no);
}

static void log_val(const ap_step *st, int deref)
{
   int err = 0;
   uint32_t addr = st->addr;
   uint32_t v;

   if (deref)
   {
      addr = read_mem(4, st->addr, &err);
      if (!err)
         addr += st->off;
   }
   v = err ? 0 : read_mem(st->size, addr, &err);
   if (err)
      fe_evt("ap_val name=%s val=ERR", st->name);
   else
      fe_evt("ap_val name=%s val=0x%08x", st->name, (unsigned)v);
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
      fe_evt("ap_mark text=%s", st->name);
      return 0;

   case OP_FF:
      ff_on = st->flag;
      return 0;

   case OP_DUMP:
      dump_req = 1;
      return 0;

   case OP_LOGRAM:
      log_val(st, 0);
      return 0;

   case OP_LOGPTR:
      log_val(st, 1);
      return 0;

   case OP_REPEAT:
      rpt_start = cur + 1;
      rpt_left = st->off;
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
         cur = rpt_start - 1;   /* advanced past REPEAT by caller */
      }
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
         fe_evt("ap_sync step=%d line=%d frame=%u", cur, st->line, frame_no);
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
