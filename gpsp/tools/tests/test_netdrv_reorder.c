/* test_netdrv_reorder.c -- netdrv's reliable receive path against a
 * reference model of the scan it replaced (critique-9 SF2).
 *
 * Round 9 made reorder[] direct-mapped (seq % ND_WINDOW): insert and drain
 * touch one slot each instead of scanning the window.  The claim is that the
 * game sees exactly what the old scan delivered.  This test keeps the old
 * algorithm (netdrv.c before 044ef9c6, arq_rx_data) as an independent
 * reference model on its own state and drives both with one random stream:
 *
 *   30 % in order, 55 % anywhere in the window or up to 8 beyond it, 15 % old
 *   or duplicate; random lengths, so a duplicate can carry a different length;
 *   payload bytes are a function of the seq.  Several 16-bit seq wraps a run.
 *
 * After EVERY packet the delivery count, an FNV-1a hash of every delivered
 * byte in order, rx_expect, reorder_hi, rx_dup and rx_beyond_win must agree.
 * Build it at the desktop window (32) and the PSP's (256, fe_features.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netdrv.c"

static unsigned long long nd_hash = 1469598103934665603ULL, nd_n;
static void nd_cb(void *u, const void *b, size_t len, uint8_t id)
{
   const uint8_t *q = b;
   size_t i;
   (void)u; (void)id;
   for (i = 0; i < len; i++) { nd_hash ^= q[i]; nd_hash *= 1099511628211ULL; }
   nd_n++;
}

/* ---- the reference: the pre-round-9 scan, on its own state ---------------- */
typedef struct { int used; uint16_t seq, len; uint8_t pl[ND_MAX_PAYLOAD]; } ref_slot;
static ref_slot ref_slots[ND_WINDOW];
static uint16_t ref_expect;
static unsigned ref_dup, ref_beyond, ref_hi;
static unsigned long long ref_hash = 1469598103934665603ULL, ref_n;

static void ref_deliver(const uint8_t *b, uint16_t len)
{
   uint16_t i;
   for (i = 0; i < len; i++) { ref_hash ^= b[i]; ref_hash *= 1099511628211ULL; }
   ref_n++;
}

static void ref_rx(uint16_t seq, const uint8_t *pl, uint16_t len)
{
   int16_t d = (int16_t)(uint16_t)(seq - ref_expect);
   int i;
   if (d < 0) { ref_dup++; return; }
   if (d == 0)
   {
      int progressed = 1;
      ref_expect++;
      ref_deliver(pl, len);
      while (progressed)
      {
         progressed = 0;
         for (i = 0; i < ND_WINDOW; i++)
         {
            ref_slot *r = &ref_slots[i];
            if (r->used && r->seq == ref_expect)
            {
               r->used = 0;
               ref_expect++;
               ref_deliver(r->pl, r->len);
               progressed = 1;
            }
            else if (r->used && (int16_t)(uint16_t)(r->seq - ref_expect) < 0)
               r->used = 0;
         }
      }
      return;
   }
   if (d < ND_WINDOW)
   {
      int free_i = -1, used = 0;
      for (i = 0; i < ND_WINDOW; i++)
      {
         if (ref_slots[i].used && ref_slots[i].seq == seq)
            return;
         if (ref_slots[i].used) used++;
         else if (free_i < 0) free_i = i;
      }
      if ((unsigned)used + 1u > ref_hi) ref_hi = (unsigned)used + 1u;
      if (free_i >= 0)
      {
         ref_slots[free_i].used = 1;
         ref_slots[free_i].seq = seq;
         ref_slots[free_i].len = len;
         memcpy(ref_slots[free_i].pl, pl, len);
      }
      return;
   }
   ref_beyond++;
}

static unsigned rs = 12345;
static unsigned rnd(void) { rs = rs * 1103515245u + 12345u; return rs >> 8; }

static int run(unsigned seed, int iters)
{
   netdrv *nd = calloc(1, sizeof(netdrv));
   nd_peer *p;
   uint8_t pay[ND_MAX_PAYLOAD];
   unsigned wraps = 0;
   uint16_t last = 0;
   int i;
   if (!nd) { printf("FAIL calloc\n"); return 1; }
   p = &nd->peers[1];
   nd->cb.deliver = nd_cb;
   memset(ref_slots, 0, sizeof(ref_slots));
   ref_expect = 0; ref_dup = ref_beyond = ref_hi = 0;
   ref_hash = nd_hash = 1469598103934665603ULL; ref_n = nd_n = 0;
   rs = seed;
   for (i = 0; i < iters; i++)
   {
      unsigned r = rnd() % 100;
      int off;
      uint16_t seq, len, k;
      if (r < 30) off = 0;
      else if (r < 85) off = (int)(rnd() % (ND_WINDOW + 8));
      else off = -(int)(rnd() % 300);
      seq = (uint16_t)(p->rx_expect + off);
      len = (uint16_t)(1 + rnd() % ND_MAX_PAYLOAD);
      for (k = 0; k < len; k++) pay[k] = (uint8_t)(seq * 31 + k);
      pay[0] = (uint8_t)seq;
      if (len > 1) pay[1] = (uint8_t)(seq >> 8);
      arq_rx_data(nd, p, seq, pay, len);
      ref_rx(seq, pay, len);
      if (p->rx_expect < last) wraps++;
      last = p->rx_expect;
      if (nd_n != ref_n || nd_hash != ref_hash || p->rx_expect != ref_expect ||
          nd->st.reorder_hi != ref_hi || nd->st.rx_dup != ref_dup ||
          nd->st.rx_beyond_win != ref_beyond)
      {
         printf("FAIL W=%d seed=%u packet %d (seq %u): deliv %llu/%llu expect %u/%u "
                "hi %u/%u dup %u/%u beyond %u/%u\n", ND_WINDOW, seed, i, seq,
                nd_n, ref_n, p->rx_expect, ref_expect, (unsigned)nd->st.reorder_hi,
                ref_hi, (unsigned)nd->st.rx_dup, ref_dup,
                (unsigned)nd->st.rx_beyond_win, ref_beyond);
         free(nd);
         return 1;
      }
   }
   printf("ok   W=%d seed=%u: %d packets, %llu delivered, %u wraps, reorder_hi %u, "
          "dup %u, beyond %u, hash %016llx\n", ND_WINDOW, seed, iters, nd_n, wraps,
          ref_hi, ref_dup, ref_beyond, nd_hash);
   /* the stream must have exercised what it claims to */
   if (wraps < 2 || ref_hi < 8 || ref_beyond == 0 || ref_dup == 0)
   {
      printf("FAIL W=%d seed=%u: the stream did not exercise wraps/stash/beyond/dup\n",
             ND_WINDOW, seed);
      free(nd);
      return 1;
   }
   free(nd);
   return 0;
}

int main(void)
{
   int fails = 0;
   unsigned seed;
   for (seed = 1; seed <= 3; seed++)
      fails += run(seed, 300000);
   printf("%d failure(s)\n", fails);
   return fails ? 1 : 0;
}
