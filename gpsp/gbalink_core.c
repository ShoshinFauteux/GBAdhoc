/* gbalink_core.c -- one GBA's end of the link cable, and the machinery that
 * lets two cores in one process take turns.  GBA_LINK builds only.
 *
 * Compiled into the core, so each core instance has its own copy (instance B
 * is the same object with every global renamed gbab_*).  The other port is
 * reached only through the bus (gbalink_bus.c, one copy).  See gbalink.h and
 * docs/GBA-LINK-MIRROR.md.
 *
 * THE CABLE.  An AGB-005 Game Link Cable between two GBAs, slot 0 at the
 * parent end.  Per GBATEK:
 *   - the parent's SI pin is grounded, the child's SI is the parent's SO;
 *     SC and SD are common wires;
 *   - multi-player mode: only the parent (SI low) can start; every port's
 *     SIOMULTI0-3 is reset to FFFF at the start and holds [parent word,
 *     child word, FFFF, FFFF] at the end; busy clears and the IRQ fires on
 *     both; the ID bits become valid (0 parent, 1 child);
 *   - normal mode on this cable is ONE-WAY: data shifts from the parent to the
 *     child, and the parent always receives zero (its SI is grounded),
 *     whichever end supplies the clock;
 *   - general-purpose mode: the four pins as wired above, SI interrupt on a
 *     falling edge.
 *   - UART and JOY BUS are not modelled (no two-GBA game is known to use
 *     them); the registers just hold what is written.
 *
 * TIMING.  Transfers take the times gpSP already used for its single-console
 * fakes (serial.c).  The start is stamped with the cycle it happened at (the
 * I/O store stub records the cycle counter, reg[GBALINK_REG_CYC]), the other
 * port applies it when its own clock reaches that time.  The stepper keeps
 * the child (slot 1) from ever running ahead of the parent, so everything the
 * parent starts reaches the child on time; a child-initiated effect can reach
 * the parent late by at most the stepper's lookahead (gbalink_pair.c).
 *
 * YIELDING.  A core is stopped only at the ENTRY of update_gba: the call is
 * postponed, not made, and resuming makes exactly that call with exactly the
 * same argument before re-entering translated code.  So stopping a core and
 * starting it again is invisible to the guest (tools/gbamirror/yieldtest.sh:
 * stopped every 300..5000 cycles, AW2 is hash-identical to never stopping
 * for 6000 frames).  A core stops at its first own event at or after the
 * time it was allowed to run to; see gbalink_next_event for why the horizon
 * is not made an event.
 */
#include "common.h"
#include "gbalink.h"

#ifdef GBA_LINK

/* serial.c */
extern int serial_mode;
/* libretro.c (GBA_LINK) */
void gbalink_lr_frame_end(void);
/* input.c (GBA_LINK) */
void gbalink_update_input(u32 new_key);

/* Kill switch for the headless machine's silent sound (sound.h
 * sound_synth_off): 1 = a headless machine mixes no samples.  Each instance
 * has its own copy (B's is gbab_gbalink_headless_audio); fe_gbadual sets
 * both from harness / config key gba_link_headless_audio. */
u32 gbalink_headless_audio = 1;

/* CABLE FAST PATH (docs/GBA-LINK-MIRROR.md section 21).  0 = off, the path
 * every hardware run so far used.  1: update_gba's two cable hooks
 * (gbalink_update_serial, gbalink_next_event) answer from this port's own
 * state when nothing on the cable can be due, instead of walking the bus on
 * every one of the ~1,150 scheduler iterations a machine makes per frame.
 *
 * Why it is the same machine.  The queue of events FOR this port changes
 * only (a) while the other core runs or is pumped -- never while this one
 * runs -- and (b) when this port pops what it applied.  So the time of the
 * first waiting event, lk.in_t, is exact from the end of one lk_apply_due
 * to the next, and every way into running this core passes through lk_pump
 * (lk_run's first act).  With no event due and no transfer due, the full
 * path does nothing but look: lk_take_replies only moves a queued reply into
 * lk.reply_*, which nothing reads before the transfer's end (lk_complete) or
 * the entry check (gbalink_entry_yield, which takes it itself), and no reply
 * can arrive while this core runs.  Each instance has its own copy (B's is
 * gbab_gbalink_fast); fe_gbadual sets both from harness / config key
 * gba_link_fast. */
u32 gbalink_fast = 0;
/* gbalink_fast = 2: THE CHECKING MODE.  Every answer is the full walk's (so
 * the machine is the gbalink_fast = 0 machine by construction); the fast
 * path's answer is computed beside it and any disagreement is counted and
 * the first one kept (gbalink_inst_stats fm_*).  For the hardware run that
 * follows the 2026-10-10 PSP-3000 freeze with gbalink_fast = 1 (section 22):
 * it cannot hang on the fast path's account and it names the first
 * disagreement, if there is one. */

/* PROGRESS GUARD (section 22).  One run() covers at most a lookahead plus a
 * frame of session time (~421,000 cycles) and every scheduler call moves the
 * clock, so a run makes a few thousand calls.  A run that reaches the limit
 * is a core that no longer makes progress: it yields GBALINK_RUN_OVERRUN and
 * the pair ends with an error instead of spinning.  gbalink_abort (set by a
 * watchdog thread through fe_gbadual_abort) asks for the same at the next
 * scheduler call. */
#define LK_RUN_CALLS_MAX GBALINK_RUN_CALLS_MAX
volatile u32 gbalink_abort = 0;
/* The lean hooks' state (serial.h); gbalink_fast = 3. */
gbalink_hot_t gbalink_hot;

/* SIO modes as in serial.c */
#define LKM_NORMAL 0
#define LKM_MULTI  1
#define LKM_UART   2
#define LKM_GPIO   3
#define LKM_JOYBUS 4

/* gpSP's transfer times (serial.c); multi-player is per player. */
#define LK_CYC_256K_8   524
#define LK_CYC_256K_32 2097
#define LK_CYC_2M_8      67
#define LK_CYC_2M_32    268
static const u32 lk_mp_cycles[4] = { 31457, 7864, 5242, 2621 };

/* Transfer in progress on this port */
enum { LKX_NONE = 0, LKX_MP_PARENT, LKX_MP_CHILD, LKX_N_MASTER, LKX_N_SLAVE };

/* Wire state word (posted in GBALINK_EV_WIRES.v0) */
#define LKW_SO        0x01u   /* SO level this port drives (1 = high)      */
#define LKW_SC_DRIVE  0x02u   /* GPIO: drives SC                           */
#define LKW_SC        0x04u
#define LKW_SD_DRIVE  0x08u   /* GPIO: drives SD                           */
#define LKW_SD        0x10u
#define LKW_MODE_SH   8       /* bits 8..10: LKM_*                         */

typedef struct
{
   int      active;
   unsigned slot;
   int      headless;
                             /* (the session clock is gbalink_hot.cycles)  */
   u64      horizon;
   int      yielded;         /* a postponed update_gba call is pending      */
   int      saved_remaining;
   int      yield_kind;      /* GBALINK_RUN_HORIZON / _BLOCKED              */
   u64      yield_now;       /* session time at the postponed call          */
   int      need_begin;      /* frame finished; waiting for frame_begin    */
   u64      in_t;            /* first waiting inbound event's time; ~0 none
                                (exact after lk_apply_due: gbalink_fast)    */
   /* this port */
   u32      wires_sent;      /* last wire word posted (~0 = none yet)      */
   u32      so_out;          /* normal mode: SO level latched              */
   /* the other port, as last heard */
   u32      p_wires;
   int      p_known;
   /* transfer */
   int      xfer;
   u64      t_start, t_done;
   u32      bits;            /* normal: 8 or 32                            */
   u32      word_rx;         /* what this port receives                    */
   u32      word_parent;     /* multi: parent's word                       */
   u32      word_own;        /* multi child: own word                      */
   int      reply_needed, reply_have, reply_active;
   u32      reply_word, reply_so;
   gbalink_inst_stats st;
} lk_state;

static lk_state lk;

/* serial.h: recompute the lean hooks' two deadlines.  Called wherever the
 * horizon, the first waiting event (lk.in_t) or the transfer in flight can
 * have changed: the end of lk_apply_due, of the full gbalink_update_serial,
 * of gbalink_write_siocnt, and lk_run / lk_begin. */
static void lk_rewake(void)
{
   u64 evt = lk.in_t;
   if (lk.xfer != 0 && lk.t_done < evt)
      evt = lk.t_done;
   gbalink_hot.evt = evt;
   gbalink_hot.wake = evt < lk.horizon ? evt : lk.horizon;
}

/* The cycle counter the I/O store stub saved (remaining cycles at the store),
 * see mips_emit.h / x86_stub.S.  update_gba re-seeds it with execute_cycles so
 * a C-side I/O write reads as "now". */
#define GBALINK_REG_CYC REG_IO_CYCLES   /* cpu.h; 62 */

static u32 lk_mode(u16 siocnt, u16 rcnt)
{
   if (rcnt & 0x8000)
      return (rcnt & 0x4000) ? LKM_JOYBUS : LKM_GPIO;
   if (siocnt & 0x2000)
      return (siocnt & 0x1000) ? LKM_UART : LKM_MULTI;
   return LKM_NORMAL;
}

static u32 lk_cur_mode(void)
{
   return lk_mode(read_ioreg(REG_SIOCNT), read_ioreg(REG_RCNT));
}

/* Session time of an I/O write made from translated code. */
static u64 lk_io_now(void)
{
   s32 rem = (s32)reg[GBALINK_REG_CYC];
   s32 done;
   if (rem < -64)
      rem = -64;
   done = (s32)execute_cycles - rem;
   if (done < 0)
      done = 0;
   return gbalink_hot.cycles + (u32)done;
}

static void lk_post(const gbalink_event *ev)
{
   /* A full queue is counted by the bus; the pair stops the session on it. */
   (void)gbalink_bus_post(lk.slot, ev);
}

/* ---- wires ------------------------------------------------------------- */

/* What this port drives on the cable now. */
static u32 lk_wires_now(void)
{
   u16 rcnt = read_ioreg(REG_RCNT);
   u32 mode = lk_cur_mode();
   u32 w = mode << LKW_MODE_SH;
   switch (mode)
   {
   case LKM_NORMAL:
      w |= lk.so_out ? LKW_SO : 0;
      break;
   case LKM_MULTI:
      w |= LKW_SO;                       /* idle high */
      break;
   case LKM_GPIO:
      /* an input pin is not driven: the other side sees the pull-up */
      w |= (rcnt & 0x80) ? ((rcnt & 0x08) ? LKW_SO : 0) : LKW_SO;
      if (rcnt & 0x10) w |= LKW_SC_DRIVE | ((rcnt & 0x01) ? LKW_SC : 0);
      if (rcnt & 0x20) w |= LKW_SD_DRIVE | ((rcnt & 0x02) ? LKW_SD : 0);
      break;
   default:
      w |= LKW_SO;
      break;
   }
   return w;
}

static u32 lk_p_mode(void)
{
   return lk.p_known ? ((lk.p_wires >> LKW_MODE_SH) & 7) : 0xFF;
}

/* My SI level: grounded at the parent end, the parent's SO at the child. */
static u32 lk_si_level(void)
{
   if (lk.slot == 0)
      return 0;
   return lk.p_known ? (lk.p_wires & LKW_SO) != 0 : 1;
}

/* Refresh the read-only bits this port sees (SIOCNT 2/3, RCNT 0-3).  Returns 1
 * if a general-purpose SI falling edge asks for an IRQ. */
static int lk_refresh_inputs(void)
{
   u16 siocnt = read_ioreg(REG_SIOCNT);
   u16 rcnt   = read_ioreg(REG_RCNT);
   u32 mode   = lk_mode(siocnt, rcnt);
   u32 si     = lk_si_level();
   int irq    = 0;

   switch (mode)
   {
   case LKM_NORMAL:
      siocnt = (siocnt & ~0x0004) | (si ? 0x0004 : 0);
      break;
   case LKM_MULTI:
   {
      u32 sd = (lk_p_mode() == LKM_MULTI);
      u32 busy = (siocnt & 0x80) != 0;
      siocnt = (siocnt & ~0x000C) | (si ? 0x0004 : 0) | (sd ? 0x0008 : 0);
      /* RCNT 0-3 mirror the pins (games test SC in bit 0) */
      rcnt = (rcnt & ~0x000F) | (busy ? 0 : 0x1) | (sd ? 0x2 : 0) |
             (si ? 0x4 : 0) | 0x8;
      write_ioreg(REG_RCNT, rcnt);
      break;
   }
   case LKM_GPIO:
   {
      u32 old_si = (rcnt >> 2) & 1;
      u32 v = rcnt & 0x0F;
      u32 pw = lk.p_known ? lk.p_wires : 0;
      if (!(rcnt & 0x10))   /* SC input */
         v = (v & ~1u) | ((pw & LKW_SC_DRIVE) ? ((pw & LKW_SC) ? 1 : 0) : 1);
      if (!(rcnt & 0x20))   /* SD input */
         v = (v & ~2u) | ((pw & LKW_SD_DRIVE) ? ((pw & LKW_SD) ? 2 : 0) : 2);
      if (!(rcnt & 0x40))   /* SI input */
         v = (v & ~4u) | (si ? 4 : 0);
      if (!(rcnt & 0x80))   /* SO input: nothing drives it on this cable */
         v = (v & ~8u) | 8;
      if (old_si && !(v & 4) && (rcnt & 0x100))
         irq = 1;
      rcnt = (rcnt & ~0x000F) | v;
      write_ioreg(REG_RCNT, rcnt);
      break;
   }
   default:
      break;
   }
   write_ioreg(REG_SIOCNT, siocnt);
   return irq;
}

static void lk_post_wires(u64 t)
{
   u32 w = lk_wires_now();
   if (w != lk.wires_sent)
   {
      gbalink_event ev;
      memset(&ev, 0, sizeof(ev));
      ev.kind = GBALINK_EV_WIRES;
      ev.t = t;
      ev.v0 = w;
      lk_post(&ev);
      lk.wires_sent = w;
   }
}

/* ---- transfers ------------------------------------------------------------ */

static void lk_mp_start_parent(u64 t)
{
   gbalink_event ev;
   u16 siocnt = read_ioreg(REG_SIOCNT);
   u32 d = lk_mp_cycles[siocnt & 3] * 2;
   lk.xfer = LKX_MP_PARENT;
   lk.t_start = t;
   lk.t_done = t + d;
   lk.word_parent = read_ioreg(REG_SIOMLT_SEND);
   lk.reply_needed = 1;
   lk.reply_have = 0;
   write_ioreg(REG_SIOMULTI0, 0xFFFF);
   write_ioreg(REG_SIOMULTI1, 0xFFFF);
   write_ioreg(REG_SIOMULTI2, 0xFFFF);
   write_ioreg(REG_SIOMULTI3, 0xFFFF);
   memset(&ev, 0, sizeof(ev));
   ev.kind = GBALINK_EV_MP_START;
   ev.t = t;
   ev.t_done = lk.t_done;
   ev.v0 = lk.word_parent;
   lk_post(&ev);
}

static void lk_n_start_master(u64 t)
{
   gbalink_event ev;
   u16 siocnt = read_ioreg(REG_SIOCNT);
   u32 is32 = (siocnt & 0x1000) != 0;
   u32 d = (siocnt & 0x2) ? (is32 ? LK_CYC_2M_32 : LK_CYC_2M_8)
                          : (is32 ? LK_CYC_256K_32 : LK_CYC_256K_8);
   lk.xfer = LKX_N_MASTER;
   lk.bits = is32 ? 32 : 8;
   lk.t_start = t;
   lk.t_done = t + d;
   /* The parent's SI is grounded: it never needs the other side. */
   lk.reply_needed = (lk.slot != 0);
   lk.reply_have = 0;
   memset(&ev, 0, sizeof(ev));
   ev.kind = GBALINK_EV_N_START;
   ev.t = t;
   ev.t_done = lk.t_done;
   ev.v0 = is32 ? (u32)read_ioreg32(REG_SIODATA32_L)
                : (u32)(read_ioreg(REG_SIOMLT_SEND) & 0xFF);
   ev.a = (u8)lk.bits;
   lk_post(&ev);
}

/* A start arrived from the other port. */
static void lk_on_start(const gbalink_event *ev)
{
   gbalink_event r;
   u16 siocnt = read_ioreg(REG_SIOCNT);
   u32 mode = lk_cur_mode();
   memset(&r, 0, sizeof(r));
   r.t = ev->t;

   if (ev->kind == GBALINK_EV_MP_START)
   {
      r.kind = GBALINK_EV_MP_DATA;
      if (mode == LKM_MULTI && lk.slot == 1 && lk.xfer == LKX_NONE)
      {
         lk.xfer = LKX_MP_CHILD;
         lk.t_start = ev->t;
         lk.t_done = ev->t_done;
         lk.word_parent = ev->v0 & 0xFFFF;
         lk.word_own = read_ioreg(REG_SIOMLT_SEND);
         write_ioreg(REG_SIOMULTI0, 0xFFFF);
         write_ioreg(REG_SIOMULTI1, 0xFFFF);
         write_ioreg(REG_SIOMULTI2, 0xFFFF);
         write_ioreg(REG_SIOMULTI3, 0xFFFF);
         write_ioreg(REG_SIOCNT, siocnt | 0x80);    /* busy */
         lk_refresh_inputs();
         r.a = 1;
         r.v0 = lk.word_own;
      }
      lk_post(&r);
      return;
   }

   /* normal mode */
   r.kind = GBALINK_EV_N_DATA;
   r.v1 = lk.so_out;
   if (mode == LKM_NORMAL && !(siocnt & 0x1) && (siocnt & 0x80) &&
       lk.xfer == LKX_NONE)
   {
      u32 bits = ev->a;
      lk.xfer = LKX_N_SLAVE;
      lk.bits = bits;
      lk.t_start = ev->t;
      lk.t_done = ev->t_done;
      /* AGB-005: the child receives the parent's bits, the parent zero */
      lk.word_rx = (lk.slot == 1) ? ev->v0 : 0;
      r.a = 1;
      r.v0 = (bits == 32) ? (u32)read_ioreg32(REG_SIODATA32_L)
                          : (u32)(read_ioreg(REG_SIOMLT_SEND) & 0xFF);
   }
   /* Only a child-clocked transfer needs the parent's bits (the parent's own
    * SI is grounded, so a parent master never waits for a reply). */
   if (lk.slot == 0)
      lk_post(&r);
}

/* Complete this port's transfer.  Returns 1 if the serial IRQ is wanted. */
static int lk_complete(void)
{
   u16 siocnt = read_ioreg(REG_SIOCNT);
   int want_irq = (siocnt & 0x4000) != 0;

   switch (lk.xfer)
   {
   case LKX_MP_PARENT:
   case LKX_MP_CHILD:
   {
      u32 child = (lk.xfer == LKX_MP_CHILD) ? lk.word_own :
                  (lk.reply_active ? (lk.reply_word & 0xFFFF) : 0xFFFF);
      write_ioreg(REG_SIOMULTI0, lk.word_parent & 0xFFFF);
      write_ioreg(REG_SIOMULTI1, child);
      write_ioreg(REG_SIOMULTI2, 0xFFFF);
      write_ioreg(REG_SIOMULTI3, 0xFFFF);
      siocnt &= ~(0x0080 | 0x0030 | 0x0040);
      siocnt |= (lk.slot ? 0x0010 : 0);           /* ID */
      write_ioreg(REG_SIOCNT, siocnt);
      lk.st.mp_xfers++;
      break;
   }
   case LKX_N_MASTER:
   case LKX_N_SLAVE:
   {
      u32 rx;
      if (lk.xfer == LKX_N_SLAVE)
         rx = lk.word_rx;
      else if (lk.slot == 0)
         rx = 0;                                   /* SI grounded */
      else if (lk.reply_active)
         rx = lk.reply_word;
      else                                         /* the parent's idle SO */
         rx = lk.reply_so ? 0xFFFFFFFFu : 0;
      if (lk.bits == 32)
      {
         write_ioreg(REG_SIODATA32_L, rx & 0xFFFF);
         write_ioreg(REG_SIODATA32_H, rx >> 16);
      }
      else
         write_ioreg(REG_SIOMLT_SEND,
                     (read_ioreg(REG_SIOMLT_SEND) & 0xFF00) | (rx & 0xFF));
      siocnt &= ~0x0080;
      write_ioreg(REG_SIOCNT, siocnt);
      lk.so_out = (siocnt & 0x0008) != 0;          /* SO back to bit 3 */
      lk.st.n_xfers++;
      break;
   }
   default:
      return 0;
   }
   lk.xfer = LKX_NONE;
   lk.reply_needed = lk.reply_have = 0;
   lk_refresh_inputs();
   lk_post_wires(lk.t_done);
   return want_irq;
}

static void lk_take_replies(void)
{
   gbalink_event ev;
   unsigned kind;
   if (!lk.reply_needed || lk.reply_have)
      return;
   kind = (lk.xfer == LKX_MP_PARENT) ? GBALINK_EV_MP_DATA : GBALINK_EV_N_DATA;
   if (gbalink_bus_take_reply(lk.slot, kind, &ev))
   {
      lk.reply_have = 1;
      lk.reply_active = ev.a;
      lk.reply_word = ev.v0;
      lk.reply_so = ev.v1;
   }
}

/* Apply every inbound event due at `now`.  Returns 1 for a serial IRQ. */
static int lk_apply_due(u64 now)
{
   const gbalink_event *ev;
   int irq = 0;
   lk_take_replies();
   while ((ev = gbalink_bus_peek(lk.slot)) && ev->t <= now)
   {
      gbalink_event e = *ev;
      gbalink_bus_pop(lk.slot);
      lk.st.ev_in++;
      if (e.t < now)
      {
         u64 late = now - e.t;
         lk.st.ev_late++;
         if (late > lk.st.late_max)
            lk.st.late_max = late > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)late;
      }
      switch (e.kind)
      {
      case GBALINK_EV_WIRES:
         lk.p_wires = e.v0;
         lk.p_known = 1;
         irq |= lk_refresh_inputs();
         break;
      case GBALINK_EV_MP_START:
      case GBALINK_EV_N_START:
         lk_on_start(&e);
         break;
      default:
         break;
      }
   }
   /* ev: the first event still waiting (not yet due), or none.  Read it
    * before lk_take_replies, which may shift the queue under the pointer. */
   lk.in_t = ev ? ev->t : ~(u64)0;
   lk_take_replies();
   lk_rewake();
   return irq;
}

/* gbalink_fast = 2: one disagreement. */
static void lk_fast_mismatch(u32 where, u64 cached, u64 truth)
{
   if (!lk.st.fast_mismatch++)
   {
      lk.st.fm_where = where;
      lk.st.fm_cached = cached;
      lk.st.fm_true = truth;
      lk.st.fm_now = gbalink_hot.cycles;
   }
}

/* The progress guard: 1 = this run must stop (lk.yielded is set). */
static int lk_guard(u64 now, int remaining_cycles)
{
   if (++gbalink_hot.run_calls <= LK_RUN_CALLS_MAX && !gbalink_abort)
      return 0;
   lk.yielded = 1;
   lk.yield_now = now;
   lk.yield_kind = GBALINK_RUN_OVERRUN;
   lk.saved_remaining = remaining_cycles;
   return 1;
}

/* ---- hooks called by the core (main.c / serial.c) -------------------------- */

int gbalink_active(void) { return lk.active; }

/* update_gba entry: 1 = postpone this call (the core returns to us). */
int gbalink_entry_yield(int remaining_cycles)
{
   s32 rem = remaining_cycles < -64 ? -64 : remaining_cycles;
   u64 now = gbalink_hot.cycles + (u32)((s32)execute_cycles - rem);
   if ((s32)execute_cycles - rem < 0)
      now = gbalink_hot.cycles;

   if (lk_guard(now, remaining_cycles))
      return 1;
   if (lk.reply_needed && !lk.reply_have)
   {
      lk_take_replies();
      if (!lk.reply_have && now >= lk.t_done)
      {
         lk.yielded = 1;
         lk.yield_now = now;
         lk.yield_kind = GBALINK_RUN_BLOCKED;
         lk.saved_remaining = remaining_cycles;
         lk.st.yields_blocked++;
         return 1;
      }
   }
   if (now >= lk.horizon)
   {
      lk.yielded = 1;
      lk.yield_now = now;
      lk.yield_kind = GBALINK_RUN_HORIZON;
      lk.saved_remaining = remaining_cycles;
      lk.st.yields_horizon++;
      return 1;
   }
   return 0;
}

/* update_gba's halt loop, after an iteration (remaining == 0 next).  Same
 * rule, applied at gbalink_hot.cycles. */
int gbalink_halt_yield(void)
{
   if (lk_guard(gbalink_hot.cycles, 0))
      return 1;
   if (lk.reply_needed && !lk.reply_have)
   {
      lk_take_replies();
      if (!lk.reply_have && gbalink_hot.cycles >= lk.t_done)
      {
         lk.yielded = 1;
         lk.yield_now = gbalink_hot.cycles;
         lk.yield_kind = GBALINK_RUN_BLOCKED;
         lk.saved_remaining = 0;
         lk.st.yields_blocked++;
         return 1;
      }
   }
   if (gbalink_hot.cycles >= lk.horizon)
   {
      lk.yielded = 1;
      lk.yield_now = gbalink_hot.cycles;
      lk.yield_kind = GBALINK_RUN_HORIZON;
      lk.saved_remaining = 0;
      lk.st.yields_horizon++;
      return 1;
   }
   return 0;
}

void gbalink_account(u32 completed_cycles)
{
   gbalink_hot.cycles += completed_cycles;
}

/* Cycles until the next thing this port must act on (transfer end, inbound
 * event, the run horizon); ~0 if none. */
/* The run horizon is deliberately NOT an event: an extra update_gba call
 * moves guest timing (update_gba clamps an overshoot past -64 cycles, so
 * where the calls fall decides how many cycles are dropped).  Measured: AW2,
 * slot 0 alone, stopped every 5000/1000/300 cycles -- identical to never
 * stopping for 6000 frames when stops wait for the core's own events; with
 * the horizon as an event the run diverged at frame 964.  A core therefore
 * stops at its first own event at or after the horizon (<= ~960 cycles
 * later); gbalink_pair.c keeps the child that far behind the parent.
 * Cable events ARE events: they are the hardware's. */
u32 gbalink_next_event(void)
{
   u64 next = ~(u64)0;
   const gbalink_event *ev;
   u32 fastv = 0;
   if (gbalink_fast)
   {
      /* the same two candidates, the event's time from lk.in_t */
      next = lk.in_t;
      if (lk.xfer != LKX_NONE && lk.t_done < next)
         next = lk.t_done;
      if (next <= gbalink_hot.cycles)
         fastv = 1;
      else
      {
         next -= gbalink_hot.cycles;
         fastv = next > 0x7FFF ? 0x7FFF : (u32)next;
      }
      if (gbalink_fast != 2)
         return fastv;
      next = ~(u64)0;          /* checking mode: the full walk decides */
   }
   ev = gbalink_bus_peek(lk.slot);
   if (gbalink_fast)
   {
      u64 t = ev ? ev->t : ~(u64)0;
      u64 slow = t;
      u32 slowv;
      if (lk.xfer != LKX_NONE && lk.t_done < slow)
         slow = lk.t_done;
      slowv = slow <= gbalink_hot.cycles ? 1 :
              (slow - gbalink_hot.cycles > 0x7FFF ? 0x7FFF : (u32)(slow - gbalink_hot.cycles));
      lk.st.fast_checks++;
      if (t != lk.in_t)
         lk_fast_mismatch(1, lk.in_t, t);
      else if (slowv != fastv)
         lk_fast_mismatch(3, fastv, slowv);
   }
   if (lk.xfer != LKX_NONE && lk.t_done < next)
      next = lk.t_done;
   if (ev && ev->t < next)
      next = ev->t;
   if (next <= gbalink_hot.cycles)
      return 1;
   next -= gbalink_hot.cycles;
   return next > 0x7FFF ? 0x7FFF : (u32)next;
}

/* After update_gba picked execute_cycles: re-seed the I/O cycle snapshot. */
void gbalink_reseed_snapshot(void)
{
   reg[GBALINK_REG_CYC] = execute_cycles;
}

/* update_serial in cable mode: apply due events, finish a due transfer. */
bool gbalink_update_serial(void)
{
   int irq;
   /* Nothing inbound is due and no transfer ends yet: the full path below
    * would change nothing (see gbalink_fast). */
   if (gbalink_fast)
   {
      int quiet = gbalink_hot.cycles < lk.in_t &&
                  (lk.xfer == LKX_NONE || gbalink_hot.cycles < lk.t_done);
      if (gbalink_fast != 2)
      {
         if (quiet)
            return false;
      }
      else
      {
         /* checking mode: is the cached time the queue's, and does the full
          * path really do nothing where the fast one would have skipped? */
         const gbalink_event *ev = gbalink_bus_peek(lk.slot);
         u64 t = ev ? ev->t : ~(u64)0;
         u32 ev_in0 = lk.st.ev_in, post0;
         int xfer0 = lk.xfer;
         gbalink_bus_stats bs;
         gbalink_bus_get_stats(&bs);
         post0 = bs.posted[lk.slot];
         lk.st.fast_checks++;
         if (t != lk.in_t)
            lk_fast_mismatch(1, lk.in_t, t);
         /* the lean hooks' deadlines (where 4: evt, 5: wake, 6: a hook
          * would have skipped although this call has work) */
         {
            u64 evt = t;
            if (lk.xfer != LKX_NONE && lk.t_done < evt)
               evt = lk.t_done;
            if (gbalink_hot.evt != evt)
               lk_fast_mismatch(4, gbalink_hot.evt, evt);
            else if (gbalink_hot.wake != (evt < lk.horizon ? evt : lk.horizon))
               lk_fast_mismatch(5, gbalink_hot.wake, lk.horizon);
            else if (gbalink_hot.cycles < gbalink_hot.wake && !quiet)
               lk_fast_mismatch(6, gbalink_hot.wake, gbalink_hot.cycles);
         }
         irq = lk_apply_due(gbalink_hot.cycles);
         if (lk.xfer != LKX_NONE && gbalink_hot.cycles >= lk.t_done &&
             (!lk.reply_needed || lk.reply_have))
            irq |= lk_complete();
         gbalink_bus_get_stats(&bs);
         if (quiet && (irq || lk.st.ev_in != ev_in0 || lk.xfer != xfer0 ||
                       bs.posted[lk.slot] != post0))
            lk_fast_mismatch(2, lk.in_t, t);
         lk_rewake();
         return irq != 0;
      }
   }
   irq = lk_apply_due(gbalink_hot.cycles);
   if (lk.xfer != LKX_NONE && gbalink_hot.cycles >= lk.t_done)
   {
      if (!lk.reply_needed || lk.reply_have)
         irq |= lk_complete();
      /* else: the entry check yields before we get here; reaching it means a
       * caller outside the stepper -- complete on the next call. */
   }
   lk_rewake();
   return irq != 0;
}

cpu_alert_type gbalink_write_siocnt(u16 value)
{
   u16 oldval = read_ioreg(REG_SIOCNT);
   u16 rcnt = read_ioreg(REG_RCNT);
   u32 nwmode = lk_mode(value, rcnt);
   u64 t = lk_io_now();
   u16 newval;

   switch (nwmode)
   {
   case LKM_MULTI:
      /* writable: baud, start (parent only), 8-11, mode, IRQ */
      newval = (value & 0x4F03) | (value & 0x3000) | (oldval & 0x007C);
      if (lk.slot != 0 || lk.xfer != LKX_NONE)
         newval = (newval & ~0x0080) | (oldval & 0x0080);
      else
         newval |= value & 0x0080;
      write_ioreg(REG_SIOCNT, newval);
      if ((newval & 0x80) && !(oldval & 0x80) && lk.slot == 0 &&
          lk.xfer == LKX_NONE)
         lk_mp_start_parent(t);
      break;
   case LKM_NORMAL:
      newval = (value & 0x7F8B) | (oldval & 0x0004);
      if (lk.xfer != LKX_NONE)               /* start stays set until done */
         newval |= 0x0080;
      write_ioreg(REG_SIOCNT, newval);
      if (!(newval & 0x0080))
         lk.so_out = (newval & 0x0008) != 0;
      /* A master transfer runs whenever start is set with the internal
       * clock.  Not only on start's rising edge: games arm the port with the
       * external clock first and then switch the clock source with start
       * still set -- the wireless-adapter probe of Nintendo's RFU library
       * writes 5080 then 5081 (Mario Golf: Advance Tour, Mario Tennis: Power
       * Tour, Pokemon Emerald) -- and the shifter starts clocking at the
       * switch.  Before this the probe never completed: start stuck, no
       * IRQ, the game timed out (docs/GBA-LINK-MIRROR.md 16.3).  An armed slave
       * (start, external clock) has no transfer of its own, so xfer is NONE
       * there too; a transfer already running is never restarted. */
      if ((newval & 0x80) && (newval & 0x1) && lk.xfer == LKX_NONE &&
          (!(oldval & 0x80) || !(oldval & 0x1)))
         lk_n_start_master(t);
      break;
   default:
      write_ioreg(REG_SIOCNT, value);       /* UART / GPIO / JOY: stored */
      break;
   }
   lk_refresh_inputs();
   lk_post_wires(t);
   lk_rewake();
   return CPU_ALERT_NONE;
}

cpu_alert_type gbalink_write_rcnt(u16 value)
{
   u64 t = lk_io_now();
   int irq;
   write_ioreg(REG_RCNT, value);
   irq = lk_refresh_inputs();
   lk_post_wires(t);
   if (irq)
   {
      flag_interrupt(IRQ_SERIAL);
      return CPU_ALERT_IRQ;
   }
   return CPU_ALERT_NONE;
}

/* ---- cold start ---------------------------------------------------------------
 *
 * Both consoles must compute the same two machines, but the copy of a machine
 * on its own console (instance A) may have been played for hours, while the
 * partner console's copy (instance B) is fresh.  A state load restores the
 * guest; it deliberately KEEPS the dynarec's learned SMC translation gates and
 * ranked candidates (savestate.c), and gates move block boundaries and charge
 * cycles (SMC_GATE_CHARGE) -- so the two copies could run the same code with
 * different timing.  docs/GBA-LINK-FEASIBILITY.md's cold-start rule: at session
 * start every machine returns to the cartridge's configured gate set (the one
 * load_gamepak saw, before any was learned), forgets its candidates, and
 * flushes its translations. */
static u32 lk_gate_cfg_n;
static u32 lk_gate_cfg_pc[MAX_TRANSLATION_GATES];

void gbalink_gates_snapshot(void)
{
   lk_gate_cfg_n = translation_gate_targets;
   memcpy(lk_gate_cfg_pc, translation_gate_target_pc, sizeof(lk_gate_cfg_pc));
}

static void lk_cold_start(void)
{
   translation_gate_targets = lk_gate_cfg_n;
   memcpy(translation_gate_target_pc, lk_gate_cfg_pc, sizeof(lk_gate_cfg_pc));
#ifdef SMC_GATES
   smc_gates_reset();
#endif
#ifdef HAVE_DYNAREC
   if (dynarec_enable)
      flush_dynarec_caches();
#endif
}

/* ---- the instance API (through gbalink_inst_ops) --------------------------- */

static void lk_begin(unsigned slot, int headless)
{
   lk_cold_start();
   memset(&lk, 0, sizeof(lk));
   lk.active = 1;
   lk.slot = slot;
   lk.headless = headless;
   lk.need_begin = 1;
   lk.wires_sent = ~0u;
   lk.in_t = 0;          /* unknown until the first lk_apply_due (lk_run) */
   gbalink_abort = 0;
   memset(&gbalink_hot, 0, sizeof(gbalink_hot));
   gbalink_hot.lean = gbalink_fast == 3;
   serial_mode = SERIAL_MODE_CABLE;
   /* A transfer that was in flight when the state was saved has no partner
    * now: drop it. */
   write_ioreg(REG_SIOCNT, read_ioreg(REG_SIOCNT) & ~0x0080);
   serial_set_irq_cycles(0);
   lk.so_out = (read_ioreg(REG_SIOCNT) & 0x0008) != 0;
   reg[GBALINK_REG_CYC] = execute_cycles;
   lk_refresh_inputs();
   lk_post_wires(0);
}

static void lk_end(void)
{
   lk.active = 0;
   lk.yielded = 0;
   serial_mode = SERIAL_MODE_DISABLED;
   sound_synth_off = 0;
}

/* The machine's clock: where it stopped.  At a postponed update_gba call
 * that includes the cycles the CPU ran since the last one (the call would
 * account them first). */
static uint64_t lk_now(void)
{
   return lk.yielded ? lk.yield_now : gbalink_hot.cycles;
}

static void lk_pump(void)
{
   if (lk_apply_due(lk_now()))
      flag_interrupt(IRQ_SERIAL);
}

static int lk_need_begin(void) { return lk.need_begin; }

static void lk_frame_begin(uint16_t keys)
{
   gbalink_update_input(keys & 0x3FF);
   skip_next_frame = lk.headless ? 1 : 0;
   sound_synth_off = (lk.headless && gbalink_headless_audio) ?
                     (gbalink_headless_audio >= 2 ? 2 : 1) : 0;
   lk.need_begin = 0;
}

static void lk_set_headless(int h) { lk.headless = h; }

static int lk_frame_done(void)
{
   lk.need_begin = 1;
   lk.st.frames++;
   gbalink_lr_frame_end();
   return GBALINK_RUN_FRAME;
}

static int lk_run(uint64_t horizon)
{
   if (!lk.active || lk.need_begin)
      return GBALINK_RUN_ERROR;
   lk.horizon = horizon;
   if (gbalink_hot.run_calls > lk.st.run_calls_max)
      lk.st.run_calls_max = gbalink_hot.run_calls;
   gbalink_hot.run_calls = 0;
   if (lk.yielded && lk.yield_kind == GBALINK_RUN_OVERRUN)
      return GBALINK_RUN_OVERRUN;      /* stopped for good (the guard) */
   lk_pump();
   lk_rewake();                        /* the new horizon */
   if (lk.yielded)
   {
      u32 r;
      lk.yielded = 0;
      r = update_gba(lk.saved_remaining);
      if (r & 0x80000000)
         return lk.yielded ? lk.yield_kind : lk_frame_done();
      execute_arm_translate(r & 0x7FFF);
   }
   else
      execute_arm_translate(execute_cycles);
   return lk.yielded ? lk.yield_kind : lk_frame_done();
}

static uint32_t lk_fnv(const void *p, size_t n)
{
   const uint32_t *w = (const uint32_t *)p;
   uint32_t h = 2166136261u;
   size_t i;
   for (i = 0; i < n / 4; i++)
      h = (h ^ w[i]) * 16777619u;
   return h;
}

static void lk_hash(uint32_t out[8])
{
   uint32_t all = 2166136261u;
   int i;
   out[0] = lk_fnv(reg, 17 * 4);
   out[1] = lk_fnv(iwram + 0x8000, 0x8000);
   out[2] = lk_fnv(ewram, 0x40000);
   out[3] = lk_fnv(io_registers, 0x400);
   out[4] = lk_fnv(palette_ram, 0x400);
   out[5] = lk_fnv(oam_ram, 0x400);
   out[6] = lk_fnv(vram, 1024 * 96);
   for (i = 0; i < 7; i++)
      all = (all ^ out[i]) * 16777619u;
   out[7] = all;
}

static void lk_get_stats(gbalink_inst_stats *out)
{
   *out = lk.st;
}

static int lk_peek(uint32_t addr, void *out, unsigned len)
{
   const u8 *base;
   u32 off, size;
   if ((addr >> 24) == 0x02)
   {
      base = ewram;
      off = addr & 0x3FFFF;
      size = 0x40000;
   }
   else if ((addr >> 24) == 0x03)
   {
      base = iwram + 0x8000;
      off = addr & 0x7FFF;
      size = 0x8000;
   }
   else
      return -1;
   if (off + len > size)
      return -1;
   memcpy(out, base + off, len);
   return 0;
}

static uint32_t lk_sram_crc(void)
{
   /* bitwise CRC-32 (the battery image is read rarely: waitsram only) */
   uint32_t c = 0xFFFFFFFFu;
   u32 i;
   int k;
   for (i = 0; i < sizeof(gamepak_backup); i++)
   {
      c ^= gamepak_backup[i];
      for (k = 0; k < 8; k++)
         c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
   }
   return ~c;
}

void gbalink_inst_get_ops(gbalink_inst_ops *ops)
{
   ops->name = "core";   /* the caller names the instance */
   ops->begin = lk_begin;
   ops->end = lk_end;
   ops->now = lk_now;
   ops->run = lk_run;
   ops->pump = lk_pump;
   ops->need_begin = lk_need_begin;
   ops->frame_begin = lk_frame_begin;
   ops->set_headless = lk_set_headless;
   ops->hash = lk_hash;
   ops->get_stats = lk_get_stats;
   ops->peek = lk_peek;
   ops->sram_crc = lk_sram_crc;
}

#endif /* GBA_LINK */
