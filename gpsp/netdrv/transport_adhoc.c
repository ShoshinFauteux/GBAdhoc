/* transport_adhoc.c — PSP sceNetAdhoc PDP backend for netdrv.
 * See transport_adhoc.h.  Every sce* semantic below is cited in
 * docs/ADHOC-NOTES.md (pspsdk header audit §1-§7, corrected crib sheet §8,
 * PPSSPP cross-check §11) — do not "fix" against memory of other docs.
 */
#include "transport_adhoc.h"

#include <string.h>

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_adhoc.h>
#include <pspnet_adhocctl.h>
#include <psputility.h>
#include <pspwlan.h>

/* Disconnect-crash diagnosis: the PSP frontend implements this to emit a
 * FLUSHED EVT before each blocking teardown step, so a mid-run Disconnect that
 * wedges leaves the offending call as the last line in the log.  (Only linked
 * into the PSP EBOOT; the desktop twin uses transport_udp.c, not this file.) */
void gpsp_adhoc_step(const char *step);

/* ---- constants the SDK headers do NOT define -----------------------------
 * Verified against PPSSPP master @ d90fdee8 (ADHOC-NOTES §11.1/§11.3):
 * adhocctl handler event codes and GetState states.  Named here because no
 * pspsdk header provides them (ADHOC-NOTES §9.1). */
#define ADHOCCTL_EVENT_ERROR       0
#define ADHOCCTL_EVENT_CONNECT     1
#define ADHOCCTL_EVENT_DISCONNECT  2
#define ADHOCCTL_EVENT_SCAN        3

#define ADHOCCTL_STATE_DISCONNECTED 0
#define ADHOCCTL_STATE_CONNECTED    1

/* SCE error codes (ADHOC-NOTES §11.1 / §11.7 / §11.8, ErrorCodes.h). */
#define SCE_NET_ADHOC_ERROR_WOULD_BLOCK       0x80410709
#define SCE_NET_ADHOC_ERROR_TIMEOUT           0x80410715
#define SCE_NET_ADHOC_ERROR_NOT_ENOUGH_SPACE  0x80400706  /* "not a typo" */

/* ---- static sizing (ADR-0007: no heap; all .bss) ------------------------- */

/* sceNetInit pool: 128 KiB (crib sheet value; hw observation "Vantage
 * Master passed 0x20000" — ADHOC-NOTES §11.11).  Priorities must be in
 * 0x08-0x77 (PPSSPP validates) — 0x2A matches the crib's 42. */
#define ADHOC_NET_POOL      (128 * 1024)
#define ADHOC_NET_PRIO      0x2A
#define ADHOC_NET_STACK     4096      /* forced to 4096 on non-1.5 fw anyway */

/* PdpCreate socket buffer: pending-RX queue depth on real hardware is
 * capped at bufsize (ADHOC-NOTES §11.10) — 0x2000 holds > 40 of our
 * <= 576 B frames. */
#define ADHOC_PDP_BUFSIZE   0x2000

/* RX thread: priority numerically just below main's 0x20 (= higher prio,
 * ADHOC-NOTES §7) so a runnable RX thread drains the socket promptly. */
#define ADHOC_RX_PRIO       0x1E
#define ADHOC_RX_STACK      0x4000
/* Blocking PdpRecv timeout slice: bounds teardown latency (thread checks
 * its run flag at least every 250 ms). */
#define ADHOC_RX_WAIT_US    250000

/* Recv scratch: one PDP datagram is at most MFS = 1444 B on hardware
 * (ADHOC-NOTES §11.8).  Receiving at full MFS capacity (not ND_MAX_FRAME)
 * means an oversize/foreign datagram is consumed and dropped here instead
 * of wedging the socket (NOT_ENOUGH_SPACE leaves the datagram queued —
 * §11.7). */
#define ADHOC_RX_SCRATCH    1472

/* SPSC ring: 64 datagrams ~= 37 KiB .bss; overflow drops (ARQ recovers,
 * counted in rx_ring_drop). */
#define ADHOC_RING_SLOTS    64

/* TX offload ring (ADR-0021).  32 slots x (ND_MAX_FRAME + 8) = ~5 KiB .bss
 * on the PSP profile.  Depth only has to cover one frame's worth of bursts
 * (an active RFU link runs ~2 payloads/frame); a full ring sends inline. */
#define ADHOC_TX_SLOTS      32
/* Two placements, because which one wins depends on something only real
 * hardware can tell us — how much of sceNetAdhocPdpSend is the WLAN driver
 * SLEEPING (which a thread hides completely) versus computing in kernel
 * mode (which it only moves).
 *   PROMPT (0x1F, between the RX thread's 0x1E and main's 0x20): signalling
 *     preempts us at once, so the datagram leaves with exactly today's
 *     latency and we win back only the part of the send that blocks.
 *   DEFERRED (0x21, just below main): the signal does not reschedule, so
 *     the whole send lands in the slack the emulation thread already
 *     donates at sceDisplayWaitVblankStart — a true offload — at the cost
 *     of up to one frame of TX latency.
 * Prompt is the default: it cannot regress link timing. */
#define ADHOC_TX_PRIO_PROMPT   0x1F
#define ADHOC_TX_PRIO_DEFERRED 0x21
#define ADHOC_TX_STACK      0x1000
#define ADHOC_TX_WAIT_US    250000    /* bounds teardown latency */

typedef struct
{
   uint8_t  mac[6];
   uint16_t len;
   uint64_t due_us;             /* debug shim: not readable before this */
   uint8_t  data[ND_MAX_FRAME];
} adhoc_ring_slot;

typedef struct
{
   uint8_t  mac[6];
   uint16_t len;
   uint8_t  data[ND_MAX_FRAME];
} adhoc_tx_slot;

/* ---- singleton state ----------------------------------------------------- */

/* Init progress ladder — term() unwinds exactly what init reached. */
enum
{
   ST_DOWN = 0,
   ST_MOD_COMMON,     /* PSP_NET_MODULE_COMMON loaded */
   ST_MOD_ADHOC,      /* PSP_NET_MODULE_ADHOC loaded */
   ST_NET,            /* sceNetInit done */
   ST_ADHOC,          /* sceNetAdhocInit done */
   ST_CTL,            /* sceNetAdhocctlInit done */
   ST_HANDLER,        /* handler registered */
   ST_CONNECTED,      /* adhocctl group CONNECTED */
   ST_PDP,            /* PDP socket created */
   ST_UP              /* RX thread running */
};

static struct
{
   int      progress;           /* ST_* ladder */
   int      handler_id;
   int      pdp_id;
   SceUID   rx_thid;
   uint8_t  mac[8];             /* sceWlanGetEtherAddr writes 6, wants 8
                                   (ADHOC-NOTES §5 gotcha) */
   char     group[9];

   /* set by the adhocctl handler (AdhocThread context — flag-writes only,
    * ADHOC-NOTES §11.2) */
   volatile int ctl_connected;
   volatile int ctl_scan_done;
   volatile int ctl_last_event;
   volatile unsigned ctl_last_error;

   volatile int rx_run;

   /* debug fault shim (harness only; all-zero = disabled, see header) */
   int      fault_lat_ms, fault_jit_ms, fault_loss_pct;
   uint32_t fault_prng;

   /* SPSC ring: rx thread produces (head), main thread consumes (tail).
    * Single-CPU MIPS: volatile index ordering is sufficient. */
   adhoc_ring_slot ring[ADHOC_RING_SLOTS];
   volatile uint32_t ring_head;   /* next write */
   volatile uint32_t ring_tail;   /* next read */

   /* TX offload (ADR-0021): main thread produces (tx_head), TX thread
    * consumes (tx_tail). */
   int      tx_want;              /* 0 off, 1 prompt, 2 deferred (pre-init) */
   /* ADR-0062: priority handed to sceNetInit for the WLAN stack's OWN
    * threads.  See adhoc_transport_set_net_prio(). */
   int      net_prio;
   SceUID   tx_thid;
   SceUID   tx_sema;
   volatile int tx_run;
   adhoc_tx_slot tx_ring[ADHOC_TX_SLOTS];
   volatile uint32_t tx_head, tx_tail;

   adhoc_stats st;
   uint32_t    last_sce;
   const char *stage;
} A = { .stage = "down", .pdp_id = -1, .rx_thid = -1, .handler_id = -1,
        .tx_want = 1, .net_prio = ADHOC_NET_PRIO, .tx_thid = -1,
        .tx_sema = -1 };

/* ---- adhocctl handler (runs on the library's AdhocThread) ---------------- */

static void ctl_handler(int flag, int error, void *unknown)
{
   (void)unknown;
   A.st.ctl_events++;
   A.ctl_last_event = flag;
   switch (flag)
   {
      case ADHOCCTL_EVENT_CONNECT:
         A.ctl_connected = 1;
         break;
      case ADHOCCTL_EVENT_DISCONNECT:
         A.ctl_connected = 0;
         A.st.ctl_disconnects++;
         break;
      case ADHOCCTL_EVENT_SCAN:
         A.ctl_scan_done = 1;
         break;
      case ADHOCCTL_EVENT_ERROR:
         A.ctl_last_error = (unsigned)error;
         A.st.ctl_errors++;
         break;
      default:
         break;
   }
}

/* ---- debug fault shim ----------------------------------------------------
 * See transport_adhoc.h.  Only touched on the RX path; `fault_on` is one
 * OR-test per datagram when disabled. */

static int fault_on(void)
{
   return (A.fault_lat_ms | A.fault_jit_ms | A.fault_loss_pct) != 0;
}

static uint32_t fault_rand(void)
{
   uint32_t x = A.fault_prng ? A.fault_prng : 0x9E3779B9u;
   x ^= x << 13; x ^= x >> 17; x ^= x << 5;
   A.fault_prng = x;
   return x;
}

/* Monotonic microseconds (ADHOC-NOTES §11.13: the "Wide" variant returns
 * microseconds as u64). */
static uint64_t adhoc_now_us(void)
{
   return (uint64_t)sceKernelGetSystemTimeWide();
}

void adhoc_transport_set_fault(int latency_ms, int jitter_ms, int loss_pct,
                               uint32_t seed)
{
   A.fault_lat_ms   = latency_ms   > 0 ? latency_ms   : 0;
   A.fault_jit_ms   = jitter_ms    > 0 ? jitter_ms    : 0;
   A.fault_loss_pct = loss_pct     > 0 ? loss_pct     : 0;
   if (seed)
      A.fault_prng = seed;
}

/* ---- RX thread ----------------------------------------------------------- */

static int rx_thread(SceSize args, void *argp)
{
   static uint8_t scratch[ADHOC_RX_SCRATCH];
   (void)args; (void)argp;

   while (A.rx_run)
   {
      uint8_t src[8];
      unsigned short sport = 0;
      /* dataLength pointee is s32, in/out: capacity in, datagram size out.
       * Success return is 0 (NOT byte count) — read the size from len.
       * (ADHOC-NOTES §11.7 — "the big one".) */
      int len = (int)sizeof(scratch);
      int rc = sceNetAdhocPdpRecv(A.pdp_id, src, &sport, scratch, &len,
                                  ADHOC_RX_WAIT_US, 0 /* block */);

      if (!A.rx_run)
         break;

      if (rc == 0)
      {
         if (len <= 0)
            continue;
         if (len > ND_MAX_FRAME)
         {
            /* Consumed at full scratch capacity, just not ours: netdrv's
             * validation wall would drop it anyway; save the copy. */
            A.st.rx_oversize++;
            continue;
         }
         {
            uint32_t head = A.ring_head;
            uint64_t due = 0;
            if (fault_on())
            {
               uint32_t delay_ms;
               if (A.fault_loss_pct &&
                   (int)(fault_rand() % 100) < A.fault_loss_pct)
               {
                  A.st.fault_dropped++;
                  continue;              /* injected loss */
               }
               delay_ms = (uint32_t)A.fault_lat_ms +
                  (A.fault_jit_ms
                     ? fault_rand() % (uint32_t)(A.fault_jit_ms + 1) : 0);
               due = adhoc_now_us() + (uint64_t)delay_ms * 1000ull;
               A.st.fault_delayed++;
            }
            if (head - A.ring_tail >= ADHOC_RING_SLOTS)
            {
               A.st.rx_ring_drop++;      /* full: drop, ARQ retransmits */
               continue;
            }
            {
               adhoc_ring_slot *s = &A.ring[head % ADHOC_RING_SLOTS];
               memcpy(s->mac, src, 6);
               s->len = (uint16_t)len;
               s->due_us = due;
               memcpy(s->data, scratch, (size_t)len);
               A.ring_head = head + 1;   /* publish after the copy */
               A.st.rx_pkts++;
            }
         }
      }
      else if ((unsigned)rc == SCE_NET_ADHOC_ERROR_TIMEOUT)
         continue;                        /* idle slice — re-check rx_run */
      else if ((unsigned)rc == SCE_NET_ADHOC_ERROR_NOT_ENOUGH_SPACE)
      {
         /* Datagram larger than MFS-sized scratch (should be impossible on
          * hardware, §11.8).  It stays queued (§11.7): back off instead of
          * spinning; ARQ traffic on this socket is lost until peer resets. */
         A.st.rx_err++;
         sceKernelDelayThread(100000);
      }
      else
      {
         A.st.rx_err++;
         A.last_sce = (uint32_t)rc;
         sceKernelDelayThread(5000);      /* avoid a hard-error spin */
      }
   }
   return 0;
}

/* ---- nd_transport vtable ------------------------------------------------- */

static const uint8_t BCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void stat_add(uint32_t *sum, uint32_t *max, uint64_t d)
{
   if (d > 0xFFFFFFFFull)
      d = 0xFFFFFFFFull;
   *sum += (uint32_t)d;
   if ((uint32_t)d > *max)
      *max = (uint32_t)d;
}

/* The actual syscall, wherever it runs (emu thread inline, or TX thread).
 * t0 is the clock read the caller already made, so one send costs exactly
 * two clock reads however it is routed.  `is_inline` says the emulation
 * thread is the one paying, in which case this cost IS the enqueue cost. */
static int adhoc_pdp_send(const uint8_t mac[6], const void *buf, size_t len,
                          uint64_t t0, int is_inline)
{
   /* Nonblocking send (cheap per plan Appendix B).  Success return is 0,
    * not bytes (ADHOC-NOTES §11.8).  Broadcast to FF:..:FF "never fails"
    * and fans out per known peer. */
   int rc = sceNetAdhocPdpSend(A.pdp_id, (unsigned char *)mac,
                               ADHOC_PDP_PORT, (void *)buf,
                               (unsigned int)len, 0, 1 /* nonblock */);
   uint64_t d = adhoc_now_us() - t0;
   A.st.tx_calls++;
   stat_add(&A.st.tx_us, &A.st.tx_max_us, d);
   if (is_inline)
      stat_add(&A.st.tx_enq_us, &A.st.tx_enq_max_us, d);
   if (rc == 0)
   {
      A.st.tx_pkts++;
      return 0;
   }
   if ((unsigned)rc == SCE_NET_ADHOC_ERROR_WOULD_BLOCK)
   {
      A.st.tx_would_block++;   /* dropped: loss is fine, ARQ owns reliability */
      return 0;
   }
   A.st.tx_fail++;
   A.last_sce = (uint32_t)rc;
   return -1;
}

/* TX thread: drains the offload ring.  Same shape as the RX thread, mirrored
 * (ADR-0021).  Wakes on a semaphore signalled by the producer; the 250 ms
 * timeout only exists so the run flag is rechecked for teardown. */
static int tx_thread(SceSize args, void *argp)
{
   (void)args; (void)argp;
   while (A.tx_run)
   {
      SceUInt tmo = ADHOC_TX_WAIT_US;
      sceKernelWaitSema(A.tx_sema, 1, &tmo);
      while (A.tx_tail != A.tx_head)
      {
         uint32_t tail = A.tx_tail;
         adhoc_tx_slot *s = &A.tx_ring[tail % ADHOC_TX_SLOTS];
         adhoc_pdp_send(s->mac, s->data, s->len, adhoc_now_us(), 0);
         A.tx_tail = tail + 1;      /* release after the send */
      }
   }
   return 0;
}

/* Producer side, always on the emulation thread. */
static int adhoc_send_raw(const uint8_t mac[6], const void *buf, size_t len)
{
   uint64_t t0 = adhoc_now_us();
   int rc;

   if (A.tx_thid >= 0)
   {
      uint32_t head = A.tx_head;
      if (head - A.tx_tail < ADHOC_TX_SLOTS && len <= ND_MAX_FRAME)
      {
         adhoc_tx_slot *s = &A.tx_ring[head % ADHOC_TX_SLOTS];
         memcpy(s->mac, mac, 6);
         s->len = (uint16_t)len;
         memcpy(s->data, buf, len);
         A.tx_head = head + 1;      /* publish after the copy */
         A.st.tx_queued++;
         sceKernelSignalSema(A.tx_sema, 1);
         stat_add(&A.st.tx_enq_us, &A.st.tx_enq_max_us, adhoc_now_us() - t0);
         return 0;
      }
      A.st.tx_ring_full++;          /* fall through: inline, never dropped */
   }

   A.st.tx_inline++;
   rc = adhoc_pdp_send(mac, buf, len, t0, 1);
   return rc;
}

static int adhoc_send_to(void *ctx, const uint8_t mac[6],
                         const void *buf, size_t len)
{
   (void)ctx;
   if (A.progress != ST_UP)
      return -1;
   return adhoc_send_raw(mac, buf, len);
}

static int adhoc_broadcast(void *ctx, const void *buf, size_t len)
{
   (void)ctx;
   if (A.progress != ST_UP)
      return -1;
   return adhoc_send_raw(BCAST_MAC, buf, len);
}

static int adhoc_recv(void *ctx, uint8_t src_mac[6], void *buf, size_t cap)
{
   uint32_t tail;
   adhoc_ring_slot *s;
   int len;

   (void)ctx;
   tail = A.ring_tail;
   if (tail == A.ring_head)
      return 0;                           /* nothing pending */
   s = &A.ring[tail % ADHOC_RING_SLOTS];
   if (s->due_us && adhoc_now_us() < s->due_us)
      return 0;                           /* debug shim: not due yet */
   len = s->len;
   if ((size_t)len > cap)
      len = (int)cap;                     /* cap >= ND_MAX_FRAME per contract */
   memcpy(src_mac, s->mac, 6);
   memcpy(buf, s->data, (size_t)len);
   A.ring_tail = tail + 1;                /* release after the copy */
   return len;
}

/* Cheap "is there a datagram waiting?" for netdrv_poll_needed (ADR-0021).
 * Two volatile loads and no syscall on the production path — the debug
 * shim's due_us is zero unless the harness turned latency injection on, and
 * only then does this read the clock.  This runs ~450 times per emulated
 * frame, so nothing else may ever be added to it. */
static int adhoc_pending(void *ctx)
{
   uint32_t tail = A.ring_tail;
   const adhoc_ring_slot *s;
   (void)ctx;
   if (tail == A.ring_head)
      return 0;
   s = &A.ring[tail % ADHOC_RING_SLOTS];
   if (s->due_us && adhoc_now_us() < s->due_us)
      return 0;
   return 1;
}

static void adhoc_local_addr(void *ctx, uint8_t mac[6])
{
   (void)ctx;
   memcpy(mac, A.mac, 6);
}

/* ---- init / term --------------------------------------------------------- */

static int group_valid(const char *g)
{
   size_t i, n = strlen(g);
   if (n < 1 || n > 8)
      return 0;
   for (i = 0; i < n; i++)
   {
      char c = g[i];
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z')))
         return 0;                        /* PPSSPP validNetworkName, §11.4 */
   }
   return 1;
}

/* Unwind a partial init but keep the failing stage + SCE code readable
 * for the caller's EVT log (term() itself resets them). */
static int init_fail(int err)
{
   const char *st = A.stage;
   uint32_t sce = A.last_sce;
   adhoc_transport_term();
   A.stage = st;
   A.last_sce = sce;
   return err;
}

/* Steps 1-5 of the bring-up ladder (WLAN switch → modules → net stack →
 * adhocctl init + handler).  Shared by init (which then connects) and
 * scan (which stops here, scans, and tears back down).  On failure the
 * partial state is unwound; returns ADHOC_ERR_*. */
static int init_to_handler(void)
{
   int rc;

   memset(&A.st, 0, sizeof(A.st));
   A.last_sce = 0;
   A.ctl_connected = 0;
   A.ctl_scan_done = 0;
   A.ctl_last_error = 0;
   A.ring_head = A.ring_tail = 0;
   A.tx_head = A.tx_tail = 0;
   A.handler_id = -1;
   A.pdp_id = -1;
   A.rx_thid = -1;
   A.tx_thid = -1;
   A.tx_sema = -1;

   /* 1. WLAN hardware switch (refuse early with a friendly error). */
   A.stage = "wlan_switch";
   if (sceWlanGetSwitchState() != 1)
      return ADHOC_ERR_WLAN_OFF;

   /* 2-3. Net modules (fake in PPSSPP, mandatory on real fw — §11.11). */
   A.stage = "module_common";
   rc = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
   if (rc < 0)
   {
      A.last_sce = (uint32_t)rc;
      return ADHOC_ERR_MODULES;
   }
   A.progress = ST_MOD_COMMON;

   A.stage = "module_adhoc";
   rc = sceUtilityLoadNetModule(PSP_NET_MODULE_ADHOC);
   if (rc < 0)
   {
      A.last_sce = (uint32_t)rc;
      return init_fail(ADHOC_ERR_MODULES);
   }
   A.progress = ST_MOD_ADHOC;

   /* 4. Stack init. */
   A.stage = "net_init";
   rc = sceNetInit(ADHOC_NET_POOL, A.net_prio, ADHOC_NET_STACK,
                   A.net_prio, ADHOC_NET_STACK);
   if (rc < 0)
   {
      A.last_sce = (uint32_t)rc;
      return init_fail(ADHOC_ERR_NET_INIT);
   }
   A.progress = ST_NET;

   A.stage = "adhoc_init";
   rc = sceNetAdhocInit();
   if (rc < 0)
   {
      A.last_sce = (uint32_t)rc;
      return init_fail(ADHOC_ERR_ADHOC_INIT);
   }
   A.progress = ST_ADHOC;

   /* 5. adhocctl: product = { type 0, "GPSPADHOC" } (9 chars exactly, no
    * NUL room; 4th field unk[3] zeroed — ADHOC-NOTES §1.2/§11.14). */
   A.stage = "ctl_init";
   {
      struct productStruct product;
      memset(&product, 0, sizeof(product));
      memcpy(product.product, "GPSPADHOC", 9);
      rc = sceNetAdhocctlInit(0x2000, 0x30, &product);
   }
   if (rc < 0)
   {
      A.last_sce = (uint32_t)rc;
      return init_fail(ADHOC_ERR_CTL_INIT);
   }
   A.progress = ST_CTL;

   A.stage = "add_handler";
   rc = sceNetAdhocctlAddHandler(ctl_handler, NULL);
   if (rc < 0)
   {
      A.last_sce = (uint32_t)rc;
      return init_fail(ADHOC_ERR_HANDLER);
   }
   A.handler_id = rc;                     /* id >= 0, keep for DelHandler */
   A.progress = ST_HANDLER;
   return ADHOC_OK;
}

int adhoc_transport_init(const char *group, uint32_t connect_timeout_us)
{
   int rc;

   if (A.progress != ST_DOWN)
      return ADHOC_ERR_ALREADY;

   if (!group || !group[0])
      group = ADHOC_GROUP_DEFAULT;
   if (!group_valid(group))
   {
      A.stage = "group";
      return ADHOC_ERR_BAD_GROUP;
   }
   memset(A.group, 0, sizeof(A.group));
   strncpy(A.group, group, 8);
   if (!connect_timeout_us)
      connect_timeout_us = 30 * 1000 * 1000;

   rc = init_to_handler();
   if (rc != ADHOC_OK)
      return rc;

   /* 6. Create-or-join the group by name (Connect == Create in PPSSPP,
    * §11.4), then wait for CONNECTED via handler flag OR GetState — the
    * only header-blessed readiness check (§1.3).  Events can take ~2 s
    * each in PPSSPP (§11.17). */
   A.stage = "ctl_connect";
   rc = sceNetAdhocctlConnect(A.group);
   if (rc < 0)
   {
      A.last_sce = (uint32_t)rc;
      return init_fail(ADHOC_ERR_CONNECT);
   }
   {
      uint32_t waited = 0;
      int connected = 0;
      while (waited < connect_timeout_us)
      {
         int st = 0;
         if (A.ctl_connected ||
             (sceNetAdhocctlGetState(&st) == 0 &&
              st == ADHOCCTL_STATE_CONNECTED))
         {
            connected = 1;
            break;
         }
         sceKernelDelayThread(50000);
         waited += 50000;
      }
      if (!connected)
      {
         A.stage = "ctl_connect_wait";
         A.last_sce = A.ctl_last_error;
         return init_fail(ADHOC_ERR_CONNECT_TIMEOUT);
      }
   }
   A.ctl_connected = 1;
   A.progress = ST_CONNECTED;

   /* 7. Own MAC (8-byte buffer — §5 gotcha) + PDP socket.  PdpCreate only
    * after CONNECTED (§11.10: MAC valid after a successful group join). */
   A.stage = "wlan_mac";
   memset(A.mac, 0, sizeof(A.mac));
   rc = sceWlanGetEtherAddr(A.mac);
   if (rc < 0)
   {
      A.last_sce = (uint32_t)rc;
      return init_fail(ADHOC_ERR_MAC);
   }

   A.stage = "pdp_create";
   rc = sceNetAdhocPdpCreate(A.mac, ADHOC_PDP_PORT, ADHOC_PDP_BUFSIZE, 0);
   if (rc < 0)
   {
      A.last_sce = (uint32_t)rc;
      return init_fail(ADHOC_ERR_PDP);
   }
   A.pdp_id = rc;
   A.progress = ST_PDP;

   /* 8. RX thread. */
   A.stage = "rx_thread";
   A.rx_run = 1;
   A.rx_thid = sceKernelCreateThread("gpsp_adhoc_rx", rx_thread,
                                     ADHOC_RX_PRIO, ADHOC_RX_STACK,
                                     PSP_THREAD_ATTR_USER, NULL);
   if (A.rx_thid < 0 || sceKernelStartThread(A.rx_thid, 0, NULL) < 0)
   {
      A.last_sce = (A.rx_thid < 0) ? (uint32_t)A.rx_thid : 0;
      A.rx_run = 0;
      if (A.rx_thid >= 0)
         sceKernelDeleteThread(A.rx_thid);
      A.rx_thid = -1;
      return init_fail(ADHOC_ERR_RX_THREAD);
   }

   /* 9. TX offload thread (ADR-0021).  Optional by design: if either the
    *    semaphore or the thread cannot be created we simply keep sending
    *    inline, which is exactly the pre-ADR-0021 behaviour — a perf
    *    optimisation must never be able to fail a wireless session. */
   if (A.tx_want)
   {
      A.stage = "tx_thread";
      A.tx_run = 1;
      A.tx_sema = sceKernelCreateSema("gpsp_adhoc_tx", 0, 0,
                                      ADHOC_TX_SLOTS, NULL);
      if (A.tx_sema >= 0)
      {
         A.tx_thid = sceKernelCreateThread("gpsp_adhoc_tx", tx_thread,
                                           A.tx_want == 2
                                              ? ADHOC_TX_PRIO_DEFERRED
                                              : ADHOC_TX_PRIO_PROMPT,
                                           ADHOC_TX_STACK,
                                           PSP_THREAD_ATTR_USER, NULL);
         if (A.tx_thid < 0 || sceKernelStartThread(A.tx_thid, 0, NULL) < 0)
         {
            if (A.tx_thid >= 0)
               sceKernelDeleteThread(A.tx_thid);
            A.tx_thid = -1;
            sceKernelDeleteSema(A.tx_sema);
            A.tx_sema = -1;
         }
      }
      if (A.tx_thid < 0)
         A.tx_run = 0;
   }

   A.progress = ST_UP;
   A.stage = "up";
   return ADHOC_OK;
}

int adhoc_transport_scan(char out[][9], int max, uint32_t timeout_us)
{
   static struct SceNetAdhocctlScanInfo scanbuf[16];
   int rc, count = 0;

   if (A.progress != ST_DOWN)
      return ADHOC_ERR_ALREADY;
   if (!timeout_us)
      timeout_us = 10 * 1000 * 1000;

   rc = init_to_handler();
   if (rc != ADHOC_OK)
      return rc;

   A.stage = "ctl_scan";
   A.ctl_scan_done = 0;
   rc = sceNetAdhocctlScan();
   if (rc < 0)
   {
      A.last_sce = (uint32_t)rc;
      return init_fail(ADHOC_ERR_CONNECT);
   }
   {
      uint32_t waited = 0;
      while (!A.ctl_scan_done && waited < timeout_us)
      {
         sceKernelDelayThread(50000);
         waited += 50000;
      }
   }
   if (A.ctl_scan_done)
   {
      int len = (int)sizeof(scanbuf);
      A.stage = "ctl_scan_info";
      memset(scanbuf, 0, sizeof(scanbuf));
      rc = sceNetAdhocctlGetScanInfo(&len, scanbuf);
      if (rc == 0 && len > 0)
      {
         const struct SceNetAdhocctlScanInfo *si = scanbuf;
         while (si && count < max)
         {
            int i, dup = 0;
            char name[9];
            memcpy(name, si->name, 8);
            name[8] = '\0';
            for (i = 0; i < count; i++)
               if (strcmp(out[i], name) == 0)
                  dup = 1;
            if (!dup && name[0])
            {
               memcpy(out[count], name, 9);
               count++;
            }
            /* next pointers point inside our own buffer (bounds-check). */
            if (si->next && (const void *)si->next >= (const void *)scanbuf &&
                (const void *)si->next <
                   (const void *)(scanbuf + sizeof(scanbuf) / sizeof(scanbuf[0])))
               si = si->next;
            else
               si = NULL;
         }
      }
      else if (rc < 0)
         A.last_sce = (uint32_t)rc;
   }
   else
      A.stage = "ctl_scan_timeout";

   adhoc_transport_term();
   return count;
}

/* CANDIDATE FIX for the mid-run-Disconnect black screen (default OFF so the
 * breadcrumb-only build shows the original wedge first): after Disconnect, wait
 * for the ctl stack to actually reach DISCONNECTED before terminating/unloading
 * the modules.  Disconnect completes asynchronously via the ctl handler, and
 * tearing the stack down before it settles is the suspected wedge — invisible
 * at app-exit (the only prior caller) but fatal when the emulator must resume. */
static int s_disc_await;
void adhoc_set_disc_await(int on) { s_disc_await = on ? 1 : 0; }

void adhoc_transport_term(void)
{
   /* Reverse of init (§1.5).  Term-without-Disconnect is tolerated by
    * PPSSPP but real fw is unverified — keep strict order.  Runs from the
    * main thread on clean exit, HOME exit and init-failure unwind. */
   if (A.progress == ST_DOWN)
      return;

   if (A.rx_thid >= 0)
   {
      SceUInt tmo = 1500000;              /* > 250 ms recv slice, with slack */
      gpsp_adhoc_step("term_rx_join");
      A.rx_run = 0;
      if (sceKernelWaitThreadEnd(A.rx_thid, &tmo) < 0)
         sceKernelTerminateDeleteThread(A.rx_thid);
      else
         sceKernelDeleteThread(A.rx_thid);
      A.rx_thid = -1;
   }

   if (A.tx_thid >= 0)
   {
      /* Let queued datagrams leave first: the last thing netdrv does before
       * teardown is BYE to every peer, and a peer that never hears it waits
       * out an 8 s death timer instead of disconnecting cleanly. */
      SceUInt tmo = 1500000;
      int spin;
      gpsp_adhoc_step("term_tx_join");
      for (spin = 0; spin < 100 && A.tx_tail != A.tx_head; spin++)
         sceKernelDelayThread(2000);
      A.tx_run = 0;
      sceKernelSignalSema(A.tx_sema, 1);  /* wake it out of the wait */
      if (sceKernelWaitThreadEnd(A.tx_thid, &tmo) < 0)
         sceKernelTerminateDeleteThread(A.tx_thid);
      else
         sceKernelDeleteThread(A.tx_thid);
      A.tx_thid = -1;
   }
   if (A.tx_sema >= 0)
   {
      sceKernelDeleteSema(A.tx_sema);
      A.tx_sema = -1;
   }

   if (A.progress >= ST_PDP && A.pdp_id >= 0)
   {
      gpsp_adhoc_step("term_pdp_delete");
      sceNetAdhocPdpDelete(A.pdp_id, 0);
      A.pdp_id = -1;
   }
   if (A.progress >= ST_CONNECTED)
   {
      gpsp_adhoc_step("term_ctl_disconnect");
      sceNetAdhocctlDisconnect();
      A.ctl_connected = 0;
      if (s_disc_await)
      {
         int st = 0, spin;
         gpsp_adhoc_step("term_await_disconnected");
         for (spin = 0; spin < 400; spin++)   /* bounded ~2 s at 5 ms */
         {
            if (sceNetAdhocctlGetState(&st) < 0 || st == 0)  /* 0 = DISCONNECTED */
               break;
            sceKernelDelayThread(5000);
         }
      }
   }
   if (A.progress >= ST_HANDLER && A.handler_id >= 0)
   {
      gpsp_adhoc_step("term_del_handler");
      sceNetAdhocctlDelHandler(A.handler_id);
      A.handler_id = -1;
   }
   if (A.progress >= ST_CTL)
   {
      gpsp_adhoc_step("term_ctl_term");
      sceNetAdhocctlTerm();
   }
   if (A.progress >= ST_ADHOC)
   {
      gpsp_adhoc_step("term_adhoc_term");
      sceNetAdhocTerm();
   }
   if (A.progress >= ST_NET)
   {
      gpsp_adhoc_step("term_net_term");
      sceNetTerm();
   }
   if (A.progress >= ST_MOD_ADHOC)
   {
      gpsp_adhoc_step("term_unload_adhoc");
      sceUtilityUnloadNetModule(PSP_NET_MODULE_ADHOC);
   }
   if (A.progress >= ST_MOD_COMMON)
   {
      gpsp_adhoc_step("term_unload_common");
      sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
   }
   gpsp_adhoc_step("term_done");

   A.progress = ST_DOWN;
   A.stage = "down";
}

void adhoc_transport_iface(nd_transport *out)
{
   memset(out, 0, sizeof(*out));          /* see udp_transport_iface */
   out->ctx        = NULL;                /* singleton */
   out->send_to    = adhoc_send_to;
   out->broadcast  = adhoc_broadcast;
   out->recv       = adhoc_recv;
   out->local_addr = adhoc_local_addr;
   out->pending    = adhoc_pending;
}

void adhoc_transport_set_tx_thread(int mode)
{
   if (A.progress == ST_DOWN)
      A.tx_want = (mode < 0 || mode > 2) ? 1 : mode;
}

/* ADR-0062: WHO GETS THE CPU THE EMULATOR IS NOT USING.
 *
 * sceNetInit's two priority arguments set the WLAN stack's own threads.
 * They have always been ADHOC_NET_PRIO = 0x2A, a value copied from a crib
 * sheet and never measured, and 0x2A is NUMERICALLY ABOVE the emulation
 * thread's 0x20 -- i.e. LOWER priority.  The driver that physically puts a
 * frame on the air therefore only runs while the emulation thread is blocked
 * in sceDisplayWaitVblankStart().
 *
 * Measured (FULLSPEED-FINDINGS §7): the emulated core costs 13.6-15.8 ms per
 * frame at EVERY session rate, so the idle time it donates is
 * 1/fps - 14.5 ms: 17.6 ms at 29.97 fps, 6.6 at 45.00, 3.6 at 53.00, and
 * 2.3 ms at 59.73.  Over the same range the mean wall-clock cost of one
 * sceNetAdhocPdpSend rises 60 us -> 1040 us and srtt rises 70 ms -> 680 ms.
 * The syscall did not get more expensive; the driver stopped being scheduled.
 *
 * Numerically BELOW 0x20 puts the stack above the emulator and it stops
 * waiting for a timeslice -- at the cost of preempting emulation, which
 * shows up directly in `EVT fps emu=` and `EVT sess_cost frame=`.  That is a
 * frame-rate-for-latency trade, which is the same trade net_session_fps has
 * been making all along, only made at the layer where the cost actually is.
 *
 * 0 = leave the historical 0x2A.  Must be called BEFORE
 * adhoc_transport_init(); ignored once up. */
void adhoc_transport_set_net_prio(int prio)
{
   if (A.progress != ST_DOWN)
      return;
   if (prio <= 0)
      A.net_prio = ADHOC_NET_PRIO;
   else if (prio < 0x08)
      A.net_prio = 0x08;          /* sceKernel/PPSSPP validate 0x08-0x77 */
   else if (prio > 0x77)
      A.net_prio = 0x77;
   else
      A.net_prio = prio;
}

int adhoc_transport_net_prio(void)
{
   return A.net_prio;
}

int adhoc_transport_tx_thread_active(void)
{
   return A.tx_thid >= 0 ? A.tx_want : 0;
}

int adhoc_transport_connected(void)
{
   return A.progress == ST_UP && A.ctl_connected;
}

uint32_t adhoc_transport_last_sce_error(void)
{
   return A.last_sce;
}

const char *adhoc_transport_stage(void)
{
   return A.stage;
}

void adhoc_transport_get_stats(adhoc_stats *out)
{
   *out = A.st;
}
