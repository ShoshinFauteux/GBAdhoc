/* budget.c -- see budget.h.  No libc, no allocation, no I/O. */
#include "budget.h"

/* ------------------------------------------------------------ helpers */
int ab_strlen(const char *s)
{
   int n = 0;
   if (!s) return 0;
   while (s[n]) n++;
   return n;
}

void ab_strlcpy(char *dst, const char *src, int cap)
{
   int i = 0;
   if (cap <= 0) return;
   if (src)
      for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
   dst[i] = 0;
}

static int streq(const char *a, int alen, const char *b)
{
   int i;
   for (i = 0; i < alen; i++)
      if (!b[i] || a[i] != b[i]) return 0;
   return b[alen] == 0;
}

int ab_put_u64(char *dst, int cap, ab_u64 v)
{
   char tmp[24];
   int n = 0, i;
   do { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && n < 22);
   if (n > cap) return -1;
   for (i = 0; i < n; i++) dst[i] = tmp[n - 1 - i];
   return n;
}

int ab_put_hex32(char *dst, int cap, ab_u32 v)
{
   static const char hx[] = "0123456789abcdef";
   int i;
   if (cap < 8) return -1;
   for (i = 0; i < 8; i++) dst[i] = hx[(v >> (28 - 4 * i)) & 15];
   return 8;
}

ab_u32 ab_crc32(const void *data, int len)
{
   const unsigned char *p = (const unsigned char *)data;
   ab_u32 c = 0xFFFFFFFFu;
   int i, k;
   for (i = 0; i < len; i++)
   {
      c ^= p[i];
      for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
   }
   return c ^ 0xFFFFFFFFu;
}

const char *ab_err_name(int rc)
{
   switch (rc)
   {
      case AB_OK:        return "ok";
      case AB_E_MISSING: return "missing";
      case AB_E_SYNTAX:  return "syntax";
      case AB_E_VERSION: return "version";
      case AB_E_CRC:     return "crc";
      case AB_E_RANGE:   return "range";
      case AB_E_TOOBIG:  return "toobig";
   }
   return "unknown";
}

/* ------------------------------------------------------------ parse */
static int parse_u64(const char *s, int n, ab_u64 *out)
{
   ab_u64 v = 0;
   int i;
   if (n <= 0 || n > 20) return -1;
   for (i = 0; i < n; i++)
   {
      ab_u64 d;
      if (s[i] < '0' || s[i] > '9') return -1;
      d = (ab_u64)(s[i] - '0');
      if (v > (0xFFFFFFFFFFFFFFFFull - d) / 10) return -1;
      v = v * 10 + d;
   }
   *out = v;
   return 0;
}

static int parse_u32(const char *s, int n, ab_u32 *out)
{
   ab_u64 v;
   if (parse_u64(s, n, &v) || v > 0xFFFFFFFFull) return -1;
   *out = (ab_u32)v;
   return 0;
}

static int parse_hex32(const char *s, int n, ab_u32 *out)
{
   ab_u32 v = 0;
   int i;
   if (n != 8) return -1;
   for (i = 0; i < 8; i++)
   {
      char c = s[i];
      int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else return -1;
      v = (v << 4) | (ab_u32)d;
   }
   *out = v;
   return 0;
}

static int parse_word(const char *s, int n, char *out)
{
   int i;
   if (n <= 0 || n >= AB_STR) return -1;
   for (i = 0; i < n; i++)
      if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
         return -1;
   for (i = 0; i < n; i++) out[i] = s[i];
   out[n] = 0;
   return 0;
}

enum { K_V = 1, K_LEFT = 2, K_CAP = 4, K_SEQ = 8, K_T = 16, K_BOOT = 32, K_LAUNCH = 64,
       K_BY = 128, K_REASON = 256, K_CRC = 512 };
#define K_REQUIRED (K_V | K_LEFT | K_CAP | K_SEQ | K_T | K_CRC)

int ab_budget_parse(const char *buf, int len, ab_budget *out)
{
   ab_budget b;
   int pos = 0, seen = 0, crc_at = -1, i;
   ab_u32 v = 0, crc_file = 0;

   if (!buf || len < 0) return AB_E_MISSING;
   if (len == 0) return AB_E_SYNTAX;
   if (len > AB_FILE_MAX) return AB_E_TOOBIG;

   for (i = 0; i < (int)sizeof(b); i++) ((char *)&b)[i] = 0;

   while (pos < len)
   {
      int start = pos, end, eq, klen, vlen, bit = 0;
      const char *key, *val;
      while (pos < len && buf[pos] != '\n') pos++;
      end = pos;
      if (pos < len) pos++;                      /* skip '\n' */
      if (end > start && buf[end - 1] == '\r') end--;
      if (end == start) continue;                /* blank line */
      if (seen & K_CRC) return AB_E_SYNTAX;      /* nothing may follow the crc line */

      for (eq = start; eq < end && buf[eq] != '='; eq++) ;
      if (eq == end || eq == start) return AB_E_SYNTAX;
      key = buf + start; klen = eq - start;
      val = buf + eq + 1; vlen = end - eq - 1;

      if      (streq(key, klen, "v"))        { bit = K_V;      if (parse_u32(val, vlen, &v)) return AB_E_SYNTAX; }
      else if (streq(key, klen, "left"))     { bit = K_LEFT;   if (parse_u32(val, vlen, &b.left)) return AB_E_SYNTAX; }
      else if (streq(key, klen, "cap"))      { bit = K_CAP;    if (parse_u32(val, vlen, &b.cap)) return AB_E_SYNTAX; }
      else if (streq(key, klen, "seq"))      { bit = K_SEQ;    if (parse_u32(val, vlen, &b.seq)) return AB_E_SYNTAX; }
      else if (streq(key, klen, "boot"))     { bit = K_BOOT;   if (parse_u32(val, vlen, &b.boot)) return AB_E_SYNTAX; }
      else if (streq(key, klen, "t"))        { bit = K_T;      if (parse_u64(val, vlen, &b.t)) return AB_E_SYNTAX; }
      else if (streq(key, klen, "by"))       { bit = K_BY;     if (parse_word(val, vlen, b.by)) return AB_E_SYNTAX; }
      else if (streq(key, klen, "reason"))   { bit = K_REASON; if (parse_word(val, vlen, b.reason)) return AB_E_SYNTAX; }
      else if (streq(key, klen, "launches"))
      {
         int p = 0;
         bit = K_LAUNCH;
         while (p < vlen)
         {
            int q = p;
            while (q < vlen && val[q] != ',') q++;
            if (b.n_launch >= AB_HIST) return AB_E_RANGE;
            if (parse_u64(val + p, q - p, &b.launch[b.n_launch])) return AB_E_SYNTAX;
            b.n_launch++;
            if (q == vlen) break;
            p = q + 1;
            if (p == vlen) return AB_E_SYNTAX;   /* trailing comma */
         }
      }
      else if (streq(key, klen, "crc"))
      {
         bit = K_CRC;
         crc_at = start;
         if (parse_hex32(val, vlen, &crc_file)) return AB_E_SYNTAX;
      }
      else
         continue;                               /* unknown key: covered by the crc, ignored */

      if (seen & bit) return AB_E_SYNTAX;        /* duplicate key */
      seen |= bit;
   }

   if (!(seen & K_CRC)) return AB_E_CRC;         /* torn write: the crc line never made it */
   if (ab_crc32(buf, crc_at) != crc_file) return AB_E_CRC;
   if ((seen & K_REQUIRED) != K_REQUIRED) return AB_E_SYNTAX;
   if (v != AB_VERSION) return AB_E_VERSION;
   if (b.cap < 1 || b.cap > AB_CAP_MAX || b.left > b.cap) return AB_E_RANGE;

   *out = b;
   return AB_OK;
}

/* ------------------------------------------------------------ format */
typedef struct { char *p; int n, cap, bad; } wbuf;

static void w_str(wbuf *w, const char *s)
{
   while (*s)
   {
      if (w->n >= w->cap) { w->bad = 1; return; }
      w->p[w->n++] = *s++;
   }
}

static void w_u64(wbuf *w, ab_u64 v)
{
   int k = ab_put_u64(w->p + w->n, w->cap - w->n, v);
   if (k < 0) w->bad = 1; else w->n += k;
}

static void w_kv(wbuf *w, const char *k, ab_u64 v)
{
   w_str(w, k); w_str(w, "="); w_u64(w, v); w_str(w, "\n");
}

int ab_budget_format(const ab_budget *b, char *buf, int cap)
{
   wbuf w;
   ab_u32 i, crc;
   w.p = buf; w.n = 0; w.cap = cap; w.bad = 0;
   w_kv(&w, "v", AB_VERSION);
   w_kv(&w, "left", b->left);
   w_kv(&w, "cap", b->cap);
   w_kv(&w, "seq", b->seq);
   w_str(&w, "by="); w_str(&w, b->by[0] ? b->by : "unknown"); w_str(&w, "\n");
   w_str(&w, "reason="); w_str(&w, b->reason[0] ? b->reason : "unknown"); w_str(&w, "\n");
   w_kv(&w, "boot", b->boot);
   w_kv(&w, "t", b->t);
   w_str(&w, "launches=");
   for (i = 0; i < b->n_launch && i < AB_HIST; i++)
   {
      if (i) w_str(&w, ",");
      w_u64(&w, b->launch[i]);
   }
   w_str(&w, "\n");
   if (w.bad) return -1;
   crc = ab_crc32(buf, w.n);
   w_str(&w, "crc=");
   if (w.cap - w.n < 9) return -1;
   w.n += ab_put_hex32(w.p + w.n, w.cap - w.n, crc);
   w_str(&w, "\n");
   return w.bad ? -1 : w.n;
}

/* ------------------------------------------------------------ transitions */
static void zero(ab_budget *b)
{
   int i;
   for (i = 0; i < (int)sizeof(*b); i++) ((char *)b)[i] = 0;
}

void ab_budget_init(ab_budget *b, ab_u64 now, ab_u32 boot, const char *by, const char *reason)
{
   zero(b);
   b->cap = b->left = AB_CAP_DEFAULT;
   b->seq = 1;
   b->boot = boot;
   b->t = now;
   ab_strlcpy(b->by, by, AB_STR);
   ab_strlcpy(b->reason, reason, AB_STR);
}

int ab_rate_count(const ab_budget *b, ab_u64 now)
{
   ab_u32 i;
   int n = 0;
   for (i = 0; i < b->n_launch && i < AB_HIST; i++)
   {
      ab_u64 t = b->launch[i];
      if (t > now) n++;                                  /* future: clock went back -- count it */
      else if (now - t < AB_RATE_WINDOW_S) n++;
   }
   return n;
}

int ab_rate_ok(const ab_budget *b, ab_u64 now)
{
   return ab_rate_count(b, now) < AB_RATE_MAX;
}

static void push_launch(ab_budget *b, ab_u64 now)
{
   ab_u32 i;
   if (b->n_launch >= AB_HIST)
   {
      for (i = 1; i < AB_HIST; i++) b->launch[i - 1] = b->launch[i];
      b->n_launch = AB_HIST - 1;
   }
   b->launch[b->n_launch++] = now;
}

void ab_budget_take(ab_budget *b, ab_u64 now)
{
   if (b->left) b->left--;
   push_launch(b, now);
   b->seq++;
   b->t = now;
   ab_strlcpy(b->by, "plugin", AB_STR);
   ab_strlcpy(b->reason, "launch", AB_STR);
}

void ab_budget_cancel(ab_budget *b, ab_u64 now)
{
   b->left = 0;
   b->seq++;
   b->t = now;
   ab_strlcpy(b->by, "plugin", AB_STR);
   ab_strlcpy(b->reason, "cancel", AB_STR);
}

int ab_harness_should_refill(int rc, const ab_budget *b, ab_u64 now, int proof)
{
   ab_u32 i;
   if (!proof) return 0;
   if (rc != AB_OK) return 1;                            /* (re)create: a healthy harness arms the console */
   /* N seconds of good running since the most recent autoboot (future stamps ignored). */
   for (i = 0; i < b->n_launch && i < AB_HIST; i++)
   {
      ab_u64 t = b->launch[i];
      if (t > now + AB_FUTURE_SLACK_S) continue;
      if (t + AB_HEALTHY_S > now) return 0;
   }
   if (b->left == b->cap && b->cap == AB_CAP_DEFAULT) return 0;   /* already full: no write */
   return 1;
}

void ab_budget_refill(ab_budget *b, int rc, ab_u64 now, ab_u32 boot)
{
   ab_u32 i, k = 0;
   if (rc != AB_OK)
   {
      ab_budget_init(b, now, boot, "harness", "init");
      return;
   }
   for (i = 0; i < b->n_launch && i < AB_HIST; i++)
      if (b->launch[i] <= now + AB_FUTURE_SLACK_S) b->launch[k++] = b->launch[i];
   b->n_launch = k;
   b->cap = AB_CAP_DEFAULT;
   b->left = b->cap;
   b->seq++;
   b->boot = boot;
   b->t = now;
   ab_strlcpy(b->by, "harness", AB_STR);
   ab_strlcpy(b->reason, "refill", AB_STR);
}

int ab_budget_disarm(ab_budget *b, int rc, ab_u64 now, ab_u32 boot, const char *reason)
{
   if (rc != AB_OK || b->left == 0) return 0;
   b->left = 0;
   b->seq++;
   b->boot = boot;
   b->t = now;
   ab_strlcpy(b->by, "harness", AB_STR);
   ab_strlcpy(b->reason, reason ? reason : "disarm", AB_STR);
   return 1;
}

/* ------------------------------------------------------------ decision */
int ab_decide(const ab_inputs *in, const ab_budget *b)
{
   /* Order matters: the first two are silent (no file is touched at all), so a
    * return to the XMB from a game, or a card that is not a lab card, sees a
    * plugin that does nothing. */
   if (!in->cold_boot)            return AB_SKIP_NOT_COLD;
   if (!in->lab_card)             return AB_SKIP_NOT_LAB;
   if (in->noauto)                return AB_SKIP_NOAUTO;
   if (in->usb_active)            return AB_SKIP_USB;
   if (!in->clock_ok)             return AB_SKIP_CLOCK;
   if (in->budget_rc != AB_OK || !b) return AB_SKIP_BUDGET_BAD;
   if (b->left == 0)              return AB_SKIP_EXHAUSTED;
   if (!ab_rate_ok(b, in->now))   return AB_SKIP_RATE;
   if (!in->eboot_ok)             return AB_SKIP_NO_EBOOT;
   return AB_GO;
}

const char *ab_verdict_name(int v)
{
   switch (v)
   {
      case AB_GO:              return "go";
      case AB_SKIP_NOT_COLD:   return "not_cold";
      case AB_SKIP_NOT_LAB:    return "not_lab";
      case AB_SKIP_NOAUTO:     return "noauto";
      case AB_SKIP_USB:        return "usb_active";
      case AB_SKIP_CLOCK:      return "clock";
      case AB_SKIP_BUDGET_BAD: return "budget_bad";
      case AB_SKIP_EXHAUSTED:  return "exhausted";
      case AB_SKIP_RATE:       return "rate";
      case AB_SKIP_NO_EBOOT:   return "no_eboot";
   }
   return "unknown";
}

int ab_verdict_logs(int v)
{
   return v != AB_SKIP_NOT_COLD && v != AB_SKIP_NOT_LAB;
}
