/* relay_dirclient.c -- see relay_dirclient.h.
 *
 *   REGISTERING  REGISTER every 1 s (5 s after five tries), with the cookie
 *                once the server sent one; a COOKIE is answered at once.
 *   REGISTERED   a refresh (REGISTER with the entry's token) every
 *                RL_DIR_REFRESH_S; an unanswered refresh is retried every
 *                5 s.  A server that lost the entry (restart) answers the
 *                token with a COOKIE: register again, asking for the same code.
 *   FAILED       a refusal (directory full, rate, no directory, paused):
 *                try again in 60 s.  "code in use" for a code we asked for:
 *                ask the server to pick one (code_changed tells the host).
 */
#include "relay_dirclient.h"

#include <stdio.h>
#include <string.h>

static void dlog(rl_dirc *d, const char *fmt, const char *a, const char *b)
{
   char line[96];
   if (!d->cfg.log)
      return;
   snprintf(line, sizeof(line), fmt, a, b);
   d->cfg.log(d->cfg.user, line);
}

static void send_register(rl_dirc *d)
{
   uint8_t b[RL_DIR_PAD];
   uint8_t *p = b + RL_HDR;
   const char *want = d->code[0] ? d->code : d->cfg.want;
   memset(b, 0, sizeof(b));
   rl_hdr(b, RL_T_REGISTER, RL_SLOT_NONE, RL_SLOT_NONE,
          d->state == DC_REGISTERED ? d->token : 0);
   if (strlen(want) == RL_CODE_LEN)
      memcpy(p, want, RL_CODE_LEN);
   rl_put16(p + 4, d->cfg.adv_port);
   rl_put_ip(p + 6, d->cfg.priv_ip_be);
   rl_put16(p + 10, d->cfg.priv_port);
   p[12] = 0;                                  /* flags: none yet */
   rl_put64(p + 13, d->cookie);
   {
      size_t n = d->state == DC_REGISTERED ? RL_REFRESH_LEN : sizeof(b);
      d->tx_bytes += (uint32_t)n;
      d->cfg.send(d->cfg.user, d->cfg.dir_ip_be, d->cfg.dir_port, b, n);
   }
}

void rl_dirc_start(rl_dirc *d, const rl_dirc_cfg *cfg, uint64_t now_ms)
{
   memset(d, 0, sizeof(*d));
   d->cfg = *cfg;
   d->cfg.want[RL_CODE_LEN] = 0;
   d->state = DC_REGISTERING;
   d->next_ms = now_ms;
}

int rl_dirc_input(rl_dirc *d, uint32_t ip_be, uint16_t port, const void *buf,
                  size_t len, uint64_t now_ms)
{
   const uint8_t *b = (const uint8_t *)buf;
   const uint8_t *body = b + RL_HDR;
   size_t n;
   if (d->state == DC_OFF || ip_be != d->cfg.dir_ip_be ||
       (port != d->cfg.dir_port && port != d->cfg.dir_port + 1))
      return 0;
   if (rl_check(b, (int)len) != 0)
      return 1;                                 /* from the directory, but noise */
   n = len - RL_HDR;
   switch (b[5])
   {
   case RL_T_COOKIE:
      if (n >= RL_COOKIE_LEN)
      {
         d->rx_bytes += (uint32_t)len;
         d->cookie = rl_get64(body);
         if (d->state == DC_REGISTERED)
         {
            /* the server no longer knows our token: register again */
            d->state = DC_REGISTERING;
            dlog(d, "dir_lost code=%s%s", d->code, "");
         }
         send_register(d);
         d->next_ms = now_ms + 1000;
      }
      return 1;
   case RL_T_REGISTERED:
      if (n >= 8 && rl_get64(b + 8))
      {
         char c[RL_CODE_LEN + 1];
         d->rx_bytes += (uint32_t)len;
         memcpy(c, body, RL_CODE_LEN);
         c[RL_CODE_LEN] = 0;
         if (strcmp(c, d->code))
         {
            memcpy(d->code, c, sizeof(c));
            d->code_changed = 1;
         }
         if (d->state != DC_REGISTERED)
         {
            d->registers++;
            dlog(d, "dir_registered code=%s%s", d->code, "");
         }
         else
            d->refreshes++;
         d->token = rl_get64(b + 8);
         d->state = DC_REGISTERED;
         d->err = 0;
         d->errtext[0] = 0;
         d->tries = 0;
         d->last_ok_ms = now_ms;
         d->next_ms = now_ms + RL_DIR_REFRESH_S * 1000ull;
      }
      return 1;
   case RL_T_ERROR:
      if (n >= 1 && d->state == DC_REGISTERING)
      {
         size_t tl = n - 1 < sizeof(d->errtext) - 1 ? n - 1 : sizeof(d->errtext) - 1;
         d->rx_bytes += (uint32_t)len;
         d->err = body[0];
         memcpy(d->errtext, body + 1, tl);
         d->errtext[tl] = 0;
         if (d->err == RL_E_BUSY && (d->code[0] || d->cfg.want[0]))
         {
            /* our code is someone else's now: let the server pick */
            dlog(d, "dir_code_taken code=%s%s", d->code[0] ? d->code : d->cfg.want, "");
            d->code[0] = 0;
            d->cfg.want[0] = 0;
            d->next_ms = now_ms;
            return 1;
         }
         d->state = DC_FAILED;
         d->next_ms = now_ms + 60000;
         dlog(d, "dir_refused why=\"%s\"%s", d->errtext, "");
      }
      return 1;
   default:
      return 1;                                 /* the directory sends nothing else */
   }
}

void rl_dirc_tick(rl_dirc *d, uint64_t now_ms)
{
   if (d->state == DC_OFF || now_ms < d->next_ms)
      return;
   if (d->state == DC_FAILED)
      d->state = DC_REGISTERING;
   if (d->state == DC_REGISTERED && d->tries > 0 &&
       now_ms - d->last_ok_ms > RL_DIR_TTL_S * 1000ull)
   {
      /* no answer for a whole TTL: the entry is gone; start over */
      d->state = DC_REGISTERING;
      d->tries = 0;
   }
   send_register(d);
   d->tries++;
   if (d->state == DC_REGISTERED)
      d->next_ms = now_ms + 5000;               /* until the refresh is answered */
   else
      d->next_ms = now_ms + (d->tries < 5 ? 1000 : 5000);
}

void rl_dirc_stop(rl_dirc *d)
{
   if (d->state == DC_REGISTERED && d->token)
   {
      uint8_t b[RL_HDR + RL_CODE_LEN];
      rl_hdr(b, RL_T_UNREGISTER, RL_SLOT_NONE, RL_SLOT_NONE, d->token);
      memcpy(b + RL_HDR, d->code, RL_CODE_LEN);
      d->tx_bytes += sizeof(b);
      d->cfg.send(d->cfg.user, d->cfg.dir_ip_be, d->cfg.dir_port, b, sizeof(b));
   }
   d->state = DC_OFF;
}
