/* gbalink_bus.c -- the wire between the two GBA link ports (one copy).
 *
 * Two queues of time-stamped events, one per direction.  A port posts while
 * it runs; only one core runs at a time, so a queue never changes under the
 * core reading it.  Events from one sender are in its time order (its clock
 * only moves forward).  See gbalink.h.
 */
#include <stdlib.h>
#include <string.h>

#include "../gbalink.h"

#define BUS_CAP 1024u   /* events in flight per direction; the stepper keeps
                           the two clocks within a frame of each other, and a
                           port posts only on a change, so a few dozen is the
                           realistic peak.  Full = the session stops. */

typedef struct
{
   gbalink_event ev[BUS_CAP];
   unsigned head, count;
} bus_queue;

/* q[s] = events FOR slot s.  On the PSP they live in instance B's block
 * (psp/gbalink_core_b.ld, which sizes them): session-only memory, lent to the
 * ROM cache with the rest of that block while no session runs.  Everywhere
 * else, plain static memory. */
#ifdef PSP
extern char gbab_bregion_bus[], gbab_bregion_bus_end[];
#define q ((bus_queue *)(void *)gbab_bregion_bus)
#else
static bus_queue q_mem[GBALINK_SLOTS];
#define q q_mem
#endif
static gbalink_bus_stats bst;

void gbalink_bus_reset(void)
{
#ifdef PSP
   /* The reservation in the link script must hold the queues. */
   if ((size_t)(gbab_bregion_bus_end - gbab_bregion_bus) <
       sizeof(bus_queue) * GBALINK_SLOTS)
      abort();
#endif
   memset(q, 0, sizeof(bus_queue) * GBALINK_SLOTS);
   memset(&bst, 0, sizeof(bst));
}

int gbalink_bus_post(unsigned from_slot, const gbalink_event *ev)
{
   bus_queue *d = &q[(from_slot ^ 1) & 1];
   if (d->count >= BUS_CAP)
   {
      bst.overflow++;
      return -1;
   }
   d->ev[(d->head + d->count) % BUS_CAP] = *ev;
   d->count++;
   bst.posted[from_slot & 1]++;
   if (d->count > bst.depth_max)
      bst.depth_max = d->count;
   return 0;
}

static int is_reply(const gbalink_event *ev)
{
   return ev->kind == GBALINK_EV_MP_DATA || ev->kind == GBALINK_EV_N_DATA;
}

/* Replies are taken out of order by gbalink_bus_take_reply; whatever is left
 * at the head that is a reply nobody asked for is dropped here (a reply to a
 * transfer this port abandoned). */
const gbalink_event *gbalink_bus_peek(unsigned to_slot)
{
   bus_queue *d = &q[to_slot & 1];
   unsigned i;
   for (i = 0; i < d->count; i++)
   {
      const gbalink_event *ev = &d->ev[(d->head + i) % BUS_CAP];
      if (!is_reply(ev))
         return ev;
   }
   return NULL;
}

void gbalink_bus_pop(unsigned to_slot)
{
   /* Remove the event gbalink_bus_peek returned: the first non-reply. */
   bus_queue *d = &q[to_slot & 1];
   unsigned i;
   for (i = 0; i < d->count; i++)
   {
      unsigned at = (d->head + i) % BUS_CAP;
      if (!is_reply(&d->ev[at]))
      {
         unsigned j;
         for (j = i; j > 0; j--)   /* shift the replies in front up by one */
            d->ev[(d->head + j) % BUS_CAP] = d->ev[(d->head + j - 1) % BUS_CAP];
         d->head = (d->head + 1) % BUS_CAP;
         d->count--;
         return;
      }
   }
}

int gbalink_bus_take_reply(unsigned to_slot, unsigned kind, gbalink_event *out)
{
   bus_queue *d = &q[to_slot & 1];
   unsigned i;
   for (i = 0; i < d->count; i++)
   {
      unsigned at = (d->head + i) % BUS_CAP;
      if (d->ev[at].kind == kind)
      {
         unsigned j;
         *out = d->ev[at];
         for (j = i; j > 0; j--)
            d->ev[(d->head + j) % BUS_CAP] = d->ev[(d->head + j - 1) % BUS_CAP];
         d->head = (d->head + 1) % BUS_CAP;
         d->count--;
         return 1;
      }
   }
   return 0;
}

void gbalink_bus_get_stats(gbalink_bus_stats *out)
{
   *out = bst;
}
