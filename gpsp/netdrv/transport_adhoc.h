/* transport_adhoc.h — PSP sceNetAdhoc PDP backend for netdrv (plan §5
 * Phase 4, Appendix B; verified semantics: docs/ADHOC-NOTES.md §8/§11).
 *
 * Single static instance (PSP is single-session; no heap — ADR-0007
 * memory posture): all buffers live in .bss.  Bring-up performs the full
 * ADHOC-NOTES §1.5 verified sequence (module load → sceNetInit →
 * sceNetAdhocInit → adhocctl init/handler/connect → wait CONNECTED →
 * PdpCreate on port 0x4A4B → RX thread).  Teardown mirrors it in exact
 * reverse and is safe to call from the HOME-exit path and after a partial
 * init failure.
 *
 * Threading (docs/ARCHITECTURE.md netdrv section): the RX thread lives
 * entirely below the transport line — it blocks in sceNetAdhocPdpRecv
 * (250 ms timeout slices so teardown never hangs) and pushes raw datagrams
 * into an SPSC ring; the nd_transport recv() hook drains that ring on the
 * emu/main thread.  send_to/broadcast are nonblocking PdpSend calls made
 * inline from the pump thread; broadcast = FF:FF:FF:FF:FF:FF destination
 * (PPSSPP fans it out per known group peer — ADHOC-NOTES §11.8; a
 * broadcast before the peer list populates reaches nobody, which the
 * netdrv JOIN retry loop tolerates by design).
 */
#ifndef TRANSPORT_ADHOC_H
#define TRANSPORT_ADHOC_H

#include <stdint.h>

#include "netdrv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PDP port for all gpSP-AdHoc traffic (plan §4.3; == desktop 19019). */
#define ADHOC_PDP_PORT   0x4A4B
/* Default adhocctl group (<= 8 alphanumeric chars, plan Appendix B). */
#define ADHOC_GROUP_DEFAULT "GPSP07"

/* adhoc_transport_init() results.  On any failure the partial init is
 * already unwound; adhoc_transport_last_sce_error()/stage() carry the
 * offending SCE code + stage name for the EVT log. */
enum
{
   ADHOC_OK                  = 0,
   ADHOC_ERR_WLAN_OFF        = -1,   /* hardware WLAN switch is off */
   ADHOC_ERR_BAD_GROUP       = -2,   /* group name not <=8 alnum chars */
   ADHOC_ERR_MODULES         = -3,   /* sceUtilityLoadNetModule failed */
   ADHOC_ERR_NET_INIT        = -4,   /* sceNetInit failed */
   ADHOC_ERR_ADHOC_INIT      = -5,   /* sceNetAdhocInit failed */
   ADHOC_ERR_CTL_INIT        = -6,   /* sceNetAdhocctlInit failed */
   ADHOC_ERR_HANDLER         = -7,   /* sceNetAdhocctlAddHandler failed */
   ADHOC_ERR_CONNECT         = -8,   /* sceNetAdhocctlConnect failed */
   ADHOC_ERR_CONNECT_TIMEOUT = -9,   /* never reached CONNECTED state */
   ADHOC_ERR_MAC             = -10,  /* sceWlanGetEtherAddr failed */
   ADHOC_ERR_PDP             = -11,  /* sceNetAdhocPdpCreate failed */
   ADHOC_ERR_RX_THREAD       = -12,  /* RX thread create/start failed */
   ADHOC_ERR_ALREADY         = -13   /* init while already up */
};

typedef struct adhoc_stats
{
   uint32_t tx_pkts;        /* PdpSend successes */
   uint32_t tx_would_block; /* nonblock send WOULD_BLOCK (dropped; ARQ owns it) */
   uint32_t tx_fail;        /* other PdpSend errors */
   uint32_t rx_pkts;        /* datagrams pushed into the ring */
   uint32_t rx_ring_drop;   /* ring full — datagram dropped (ARQ recovers) */
   uint32_t rx_oversize;    /* datagram > ND_MAX_FRAME — dropped */
   uint32_t rx_err;         /* unexpected PdpRecv errors */
   uint32_t ctl_events;     /* adhocctl handler invocations */
   uint32_t ctl_disconnects;/* ADHOCCTL_EVENT_DISCONNECT seen */
   uint32_t ctl_errors;     /* ADHOCCTL_EVENT_ERROR seen */
   uint32_t fault_delayed;  /* debug shim: datagrams held for latency/jitter */
   uint32_t fault_dropped;  /* debug shim: datagrams dropped by loss_pct */
   /* TX cost accounting (ADR-0021).  sceNetAdhocPdpSend is the one call in
    * this driver whose real cost we cannot measure anywhere but on hardware:
    * PPSSPP implements it as a host UDP sendto, a PSP-1000 runs a whole
    * kernel/WLAN-driver round trip.  These make a field log say it outright.
    *   tx_calls/tx_us_*   — the PdpSend calls themselves, wherever they ran.
    *   tx_enq_us/tx_enq_max_us — what the EMULATION thread paid: the whole
    *                       send path when inline, or just the ring copy +
    *                       semaphore signal when the TX thread is on.
    *   tx_queued/tx_inline — how the split actually went.
    *   tx_ring_full       — TX ring was full, sent inline instead (ordering
    *                       is still fine: ARQ tolerates reorder). */
   uint32_t tx_calls;
   uint32_t tx_us, tx_max_us;
   uint32_t tx_enq_us, tx_enq_max_us;
   uint32_t tx_queued, tx_inline, tx_ring_full;
} adhoc_stats;

/* Full wireless bring-up: blocks the calling (main) thread until the
 * adhocctl group is CONNECTED and the PDP socket + RX thread are live, or
 * fails with an ADHOC_ERR_* (partial state unwound).  group NULL/"" =
 * ADHOC_GROUP_DEFAULT.  connect_timeout_us 0 = default 30 s (PPSSPP can
 * take ~2 s per adhocctl event — ADHOC-NOTES §11.17 — and real hardware
 * has radio+server latencies on top). */
int adhoc_transport_init(const char *group, uint32_t connect_timeout_us);

/* Room-discovery scan for the wireless Join panel (plan §8): performs the
 * module/net/adhocctl part of the bring-up, issues sceNetAdhocctlScan,
 * waits for the SCAN event (default 10 s), copies up to `max` unique group
 * names (NUL-terminated, 9 bytes each) into out[], then tears fully back
 * down.  Only callable while the transport is DOWN.  Returns the count
 * (>= 0) or ADHOC_ERR_* (< 0); a timeout returns 0, WLAN-off returns
 * ADHOC_ERR_WLAN_OFF.  Blocks the calling thread for the scan duration. */
int adhoc_transport_scan(char out[][9], int max, uint32_t timeout_us);

/* Reverse teardown (ADHOC-NOTES §1.5): RX thread join → PdpDelete →
 * Disconnect → DelHandler → AdhocctlTerm → AdhocTerm → NetTerm → module
 * unload.  Idempotent; also correct after a failed/partial init. */
void adhoc_transport_term(void);

/* Fill the nd_transport vtable for netdrv_create(). Valid only while up. */
void adhoc_transport_iface(nd_transport *out);

/* 1 while adhocctl reports CONNECTED (cleared by a DISCONNECT event from
 * the handler thread).  The frontend polls this to detect group loss. */
int adhoc_transport_connected(void);

/* Diagnostics for EVT logging. */
uint32_t    adhoc_transport_last_sce_error(void);   /* 0 if none */
const char *adhoc_transport_stage(void);            /* last init/fail stage */
void        adhoc_transport_get_stats(adhoc_stats *out);

/* ---- debug fault-injection shim (harness only) --------------------------
 * Mirrors udp_transport_set_fault() on the desktop backend so the PPSSPP
 * rig can emulate real ad-hoc radio, whose RTT is milliseconds-to-tens-of-ms
 * while PPSSPP's AdhocServer loopback is microseconds.  Without this the
 * harness cannot reproduce the field failure class at all (docs/HANDOFF.md
 * issue #2: an RTO tuned on loopback storms on real radio).
 *
 * The delay is applied on the RECEIVE side (PdpSend is fire-and-forget), so
 * each side contributes `latency_ms` to the one-way path: two peers both
 * configured at 20 ms give a ~40 ms RTT.  jitter_ms adds U[0,jitter] per
 * datagram; the ring stays FIFO (radio-like, no reorder).  loss_pct drops
 * datagrams before they enter the ring.
 *
 * Zero-cost when all knobs are 0 (the default): one integer test per
 * datagram.  Set before or after adhoc_transport_init(); survives it. */
void adhoc_transport_set_fault(int latency_ms, int jitter_ms, int loss_pct,
                               uint32_t seed);

/* ---- TX offload thread (ADR-0021) ---------------------------------------
 * send_to/broadcast copy the datagram into an SPSC ring and signal a
 * dedicated TX thread, so sceNetAdhocPdpSend does not run on the emulation
 * thread.  The emu thread pays a <=160 B memcpy and one semaphore signal
 * instead of a WLAN-driver round trip of unknown (and, on a PSP-1000,
 * unmeasured) cost.  Ordering is preserved — single producer, single
 * consumer, FIFO — and the ARQ above tolerates reorder anyway.  A full ring
 * falls back to sending inline rather than dropping.
 *
 * mode: 0 = off (inline, the pre-ADR-0021 behaviour)
 *       1 = prompt   (TX thread just ABOVE main: the datagram leaves with
 *                     today's latency; we win back whatever part of the
 *                     send blocks) — DEFAULT
 *       2 = deferred (TX thread just BELOW main: the send lands in the
 *                     slack the emu thread already donates at the vblank
 *                     wait — a full offload, at up to one frame of TX
 *                     latency).  Offered so the user can A/B all three on
 *                     hardware in one sitting; only hardware can rank them.
 *
 * Must be called BEFORE adhoc_transport_init(); ignored while up. */
void adhoc_transport_set_tx_thread(int mode);

/* The mode actually running (0 if the thread is not up). */
int  adhoc_transport_tx_thread_active(void);

/* ADR-0062: priority handed to sceNetInit for the WLAN stack's own threads.
 * Historically 0x2A, which is BELOW the emulation thread's 0x20, so the
 * driver only runs in the idle time the emulator donates at the vblank wait
 * -- 17.6 ms/frame at 29.97 fps but only 2.3 ms at 59.73.  0 keeps 0x2A.
 * Must be called BEFORE adhoc_transport_init(). */
void adhoc_transport_set_net_prio(int prio);
int  adhoc_transport_net_prio(void);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_ADHOC_H */
