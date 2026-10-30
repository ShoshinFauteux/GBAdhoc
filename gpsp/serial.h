/* gameplaySP
 *
 * Copyright (C) 2023 David Guillen Fandos <david@davidgf.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define SERIAL_MODE_DISABLED      0
#define SERIAL_MODE_GBP           1  // Connected to the GB Player
#define SERIAL_MODE_RFU           2  // Wifi Adapter
#define SERIAL_MODE_SERIAL_POKE   3  // Connected to serial cable (Pokemon emulation mode)
#define SERIAL_MODE_SERIAL_AW1    4  // Connected to serial cable (AdvWars emulation mode)
#define SERIAL_MODE_SERIAL_AW2    5  // Connected to serial cable (AdvWars emulation mode)
#define SERIAL_MODE_AUTO          6  // Choose best fit automatically
/* GBA_LINK builds: a link cable to a second core in the same process
 * (gbalink_core.c).  Never chosen automatically; only gbalink's begin() sets
 * it, and nothing else in the core reacts to it. */
#define SERIAL_MODE_CABLE         7

extern int serial_mode;

// Register writes
cpu_alert_type write_siocnt(u16 value);
cpu_alert_type write_rcnt(u16 value);

// Serial IRQ interface
u32 serial_next_event();
bool update_serial(unsigned cycles);

#ifdef GBA_LINK
// Link cable (gbalink_core.c)
cpu_alert_type gbalink_write_siocnt(u16 value);
cpu_alert_type gbalink_write_rcnt(u16 value);
bool gbalink_update_serial(void);
u32 gbalink_next_event(void);
int gbalink_active(void);
int gbalink_entry_yield(int remaining_cycles);
int gbalink_halt_yield(void);
void gbalink_account(u32 completed_cycles);
void gbalink_reseed_snapshot(void);

/* THE LEAN CABLE HOOKS (gba_link_fast = 3; docs/GBA-LINK-MIRROR.md 23).
 * update_gba runs ~850 times per machine per frame and, in cable mode, made
 * six calls into gbalink_core.c each time: ~200 MIPS instructions with the
 * fast path on (the twin's count), 12 % of a Street Fighter pair.  Nothing on
 * the cable can concern this core before
 *     gbalink_hot.wake = min(run horizon, first waiting inbound event,
 *                            end of the transfer in flight)
 * so until its clock reaches that, every hook's answer is known here: no
 * yield, no serial work, and the next event is gbalink_hot.evt (the same
 * minimum without the horizon, which is deliberately not an event).  At or
 * past it the hook calls the full function, as before.  gbalink_core.c
 * recomputes both (lk_rewake) wherever one of the three inputs changes; the
 * checking mode (gba_link_fast = 2) recomputes them at every hook and counts
 * any difference.  lean is 0 unless gbalink_fast is 3. */
typedef struct
{
   u64 cycles;      /* session cycles accounted by update_gba              */
   u64 wake;        /* first session time a hook must take the full path   */
   u64 evt;         /* first cable event (no horizon); ~0 = none           */
   u32 run_calls;   /* scheduler calls in this run (the progress guard)    */
   u32 lean;
} gbalink_hot_t;
extern gbalink_hot_t gbalink_hot;
extern volatile u32 gbalink_abort;
#define GBALINK_RUN_CALLS_MAX 2000000u

/* GBALINK_LEAN_OUTLINE=1 (build; default off): the four hooks below are
 * real functions instead of inline code in update_gba.  Inline they add
 * ~700 bytes to update_gba, which is in the pinned hot core
 * (psp/layout/hot_order.txt): everything behind it moves and single-player
 * games pay in I-cache misses although they never run this code (4.0.0
 * beta integration: Driver 2 +2,000 simulated misses a frame).  Out of line
 * a link session pays four calls per scheduler iteration instead.  Same
 * machines either way. */
#ifdef GBALINK_LEAN_OUTLINE
#define GBALINK_Q static __attribute__((noinline, unused))
#else
#define GBALINK_Q static inline
#endif

GBALINK_Q int gbalink_entry_yield_q(int remaining_cycles)
{
   if (gbalink_hot.lean)
   {
      s32 rem = remaining_cycles < -64 ? -64 : remaining_cycles;
      s32 done = (s32)execute_cycles - rem;
      u64 now = gbalink_hot.cycles + (done > 0 ? (u32)done : 0u);
      if (now < gbalink_hot.wake && !gbalink_abort &&
          ++gbalink_hot.run_calls <= GBALINK_RUN_CALLS_MAX)
         return 0;
   }
   return gbalink_entry_yield(remaining_cycles);
}

GBALINK_Q int gbalink_halt_yield_q(void)
{
   if (gbalink_hot.lean && gbalink_hot.cycles < gbalink_hot.wake &&
       !gbalink_abort && ++gbalink_hot.run_calls <= GBALINK_RUN_CALLS_MAX)
      return 0;
   return gbalink_halt_yield();
}

GBALINK_Q bool gbalink_update_serial_q(void)
{
   if (gbalink_hot.lean && gbalink_hot.cycles < gbalink_hot.wake)
      return false;
   return gbalink_update_serial();
}

GBALINK_Q u32 gbalink_next_event_q(void)
{
   if (gbalink_hot.lean && gbalink_hot.cycles < gbalink_hot.evt)
   {
      u64 d = gbalink_hot.evt - gbalink_hot.cycles;
      return d > 0x7FFF ? 0x7FFF : (u32)d;
   }
   return gbalink_next_event();
}
#endif

// Savestate accessors
u32 serial_get_irq_cycles(void);
void serial_set_irq_cycles(u32 v);

// RFU interface
void rfu_reset(void);
bool rfu_update(unsigned cycles);
u32 rfu_transfer(u32 value);
void rfu_frame_update(void);
void rfu_net_receive(const void* buf, size_t len, uint16_t client_id);
void rfu_net_peer_gone(uint16_t client_id);
void rfu_net_session_end(void);

// Serial implementations
void serialproto_reset(void);

void serialpoke_frame_update(void);
void serialpoke_master_send(void);
bool serialpoke_update(unsigned cycles);
void serialpoke_net_receive(const void* buf, size_t len, uint16_t client_id);

void serialaw_frame_update(void);
void serialaw_master_send(void);
bool serialaw_update(unsigned cycles);
void serialaw_net_receive(const void* buf, size_t len, uint16_t client_id);

// GBP interface
u32 gbp_transfer(u32 value);
void gbp_reset(void);
u32 gbp_get_state(void);
void gbp_set_state(u32 v);

